#include "infrastructure/gpu_dsp.h"
#include <QFileInfo>
#include <QLibrary>
#include <thread>
#include <limits>
#include <atomic>

namespace signalstudio::gpu_dsp {
namespace { std::atomic<std::uint64_t> sessionSequence{1}; }
struct Session::Impl {
  Config config;
  Capabilities caps;
  mutable Metrics measured;
  const std::thread::id owner = std::this_thread::get_id();
  QLibrary jit, fft, module;
  const module_abi::Api* api = nullptr;
  void* session = nullptr;
  const std::uint64_t sessionIdentity = sessionSequence.fetch_add(1);
  FirFilter moduleFilter;
  static int cancel(void* state) noexcept {
    auto* self=static_cast<Impl*>(state);
    try { return self->config.cancelled && self->config.cancelled() ? 1 : 0; }
    catch (...) { return 1; }
  }
  static bool load(QLibrary& lib,const std::string& path,Error& error) {
    const auto name=QString::fromStdString(path);
    if(path.empty() || !QFileInfo(name).isAbsolute()) {
      error={ErrorCode::Unavailable,"runtime-library","An explicit absolute runtime DLL path is required"};return false;
    }
    lib.setFileName(name);
    if(!lib.load()) {error={ErrorCode::Unavailable,"runtime-library",lib.errorString().toStdString()};return false;}
    return true;
  }
  void update() const {
    if(!session || !api || owner!=std::this_thread::get_id()) return;
    module_abi::Telemetry t;api->telemetry(session,&t);
    measured.setupMs=t.setup;measured.hostAllocationMs=t.hostAlloc;measured.planMs=t.plan;
    measured.h2dMs=t.h2d;measured.fftMs=t.fft;measured.kernelsMs=t.kernels;measured.d2hMs=t.d2h;measured.totalHostMs=t.host;
    measured.h2dBytes=t.inBytes;measured.d2hBytes=t.outBytes;measured.fftExecutions=t.execs;measured.kernelLaunches=t.launches;
    measured.deviceBytes=t.device;measured.devicePeakBytes=t.peak;measured.pinnedBytes=t.pinned;
    measured.workspaceBytes=t.workspace;measured.hostStagingBytes=t.hostStaging;measured.planCount=t.plans;
    measured.libraryAlgorithmCalls=t.sdkCalls;measured.sdkTemporaryBytes=t.sdkTemporaryBytes;
  }
  bool allowed(Error& error) const {
    if(owner!=std::this_thread::get_id()) {error={ErrorCode::WrongThread,"thread","GPU session belongs to another thread"};return false;}
    if(!session) {error={ErrorCode::Unavailable,"module","Optional GPU module unavailable; use CPU fallback"};return false;}
    return true;
  }
  bool done(int status,const module_abi::Failure& f,Error& error) {
    update();
    if(status) {error={};return true;}
    error={static_cast<ErrorCode>(f.code),f.stage,f.detail};return false;
  }
  ~Impl() {
    if(session) {
      if(owner!=std::this_thread::get_id()) std::terminate();
      api->destroy(session);
    }
    module.unload();fft.unload();jit.unload();
  }
};
Session::Session():impl_(std::make_unique<Impl>()){}
Session::~Session()=default;
std::unique_ptr<Session> Session::create(const Config& config,Capabilities& caps,Error& error) {
  auto result=std::unique_ptr<Session>(new Session);auto& i=*result->impl_;i.config=config;
  try {
    if(!config.nvJitLinkLibrary.empty() && !Impl::load(i.jit,config.nvJitLinkLibrary,error)) {caps.fallbackReason=error.detail;return {};}
    if(!Impl::load(i.fft,config.cufftLibrary,error)) {caps.fallbackReason=error.detail;return {};}
    i.caps.cufftLoaded=true;i.caps.cufftLibrary=config.cufftLibrary;
    if(!Impl::load(i.module,config.moduleLibrary,error)) {caps=i.caps;caps.fallbackReason=error.detail;return {};}
    auto get=reinterpret_cast<module_abi::GetApi>(i.module.resolve("ssGpuDspGetApi"));
    if(!get || !(i.api=get(module_abi::version)) || i.api->abiVersion!=module_abi::version || i.api->bytes!=sizeof(module_abi::Api)) {
      error={ErrorCode::Unsupported,"module-abi","GPU module ABI mismatch"};caps=i.caps;caps.fallbackReason=error.detail;return {};
    }
    module_abi::Options options;options.limits=config.limits;options.device=config.deviceOrdinal;
    options.cancel=&Impl::cancel;options.cancelState=&i;
    module_abi::Info info;module_abi::Failure failure;
    i.session=i.api->create(&options,&info,&failure);
    i.caps.driverVersion=info.driver;i.caps.driverLoaded=info.driver>0;i.caps.cufftVersion=info.cufft;
    i.caps.deviceOrdinal=config.deviceOrdinal;i.caps.computeMajor=info.major;i.caps.computeMinor=info.minor;
    i.caps.deviceName=info.name;i.caps.deviceTotalBytes=info.total;i.caps.deviceFreeBytesAtSetup=info.free;
    i.caps.moduleLibrary=config.moduleLibrary;i.caps.driverLibrary="CUDA Runtime + system CUDA driver";
    if(!i.session) {error={static_cast<ErrorCode>(failure.code),failure.stage,failure.detail};caps=i.caps;caps.fallbackReason=error.detail;return {};}
    i.caps.sessionReady=true;i.update();caps=i.caps;error={};return result;
  } catch(const std::exception& e) {error={ErrorCode::Unavailable,"module",e.what()};caps=i.caps;caps.fallbackReason=e.what();return {};}
}
const Capabilities& Session::capabilities()const{return impl_->caps;}
const Metrics& Session::metrics()const{impl_->update();return impl_->measured;}
bool Session::prepareFir(std::span<const float> h,std::uint32_t radius,FirFilter& token,Error& error) {
  auto& i=*impl_;if(!i.allowed(error))return false;
  if(h.size()>std::numeric_limits<unsigned>::max()){error={ErrorCode::ResourceLimit,"FIR","Coefficient count overflow"};return false;}
  module_abi::Failure f;FirFilter next;
  const int ok=i.api->prepare(i.session,h.data(),unsigned(h.size()),radius,&next,&f);
  if(ok){i.moduleFilter=next;token=next;token.owner=i.sessionIdentity;}
  else i.moduleFilter={};
  return i.done(ok,f,error);
}
bool Session::periodicFirBlock(const FirFilter& token,const PeriodicFirInput& input,std::span<float> out,Error& error,std::span<std::complex<float>> tap) {
  auto& i=*impl_;if(!i.allowed(error))return false;
  if(input.raw.size()>std::numeric_limits<unsigned>::max() || out.size()!=input.count || (!tap.empty()&&tap.size()!=input.count)) {
    error={ErrorCode::InvalidInput,"FIR","Output/raw shape mismatch"};return false;
  }
  if(token.owner!=i.sessionIdentity || !token.generation || token.generation!=i.moduleFilter.generation) {
    error={ErrorCode::InvalidInput,"FIR","Stale/cross-session FIR token"};return false;
  }
  auto moduleToken=token;moduleToken.owner=i.moduleFilter.owner;
  module_abi::Failure f;const int ok=i.api->fir(i.session,&moduleToken,input.raw.data(),unsigned(input.raw.size()),input.localStart,input.contextN,
    input.sampleRateHz,input.relativeCenterHz,input.count,out.data(),tap.empty()?nullptr:tap.data(),&f);return i.done(ok,f,error);
}
bool Session::stft(const StftInput& input,std::span<float> out,StftSummary& summary,Error& error) {
  auto& i=*impl_;if(!i.allowed(error))return false;
  if(out.size()!=input.outputBins || input.records.size()!=std::uint64_t(input.transformBins)*input.recordCount || input.hann.size()!=input.transformBins ||
     (!input.recordWeights.empty()&&input.recordWeights.size()!=input.recordCount)) {error={ErrorCode::InvalidInput,"STFT","Packed shape mismatch"};return false;}
  module_abi::Stft request{input.records.data(),input.hann.data(),input.recordWeights.empty()?nullptr:input.recordWeights.data(),
    input.transformBins,input.recordCount,input.outputBins,input.sampleRateHz,input.hannEnergy,input.recordDivisor};
  module_abi::Failure f;const int ok=i.api->stft(i.session,&request,out.data(),&summary,&f);return i.done(ok,f,error);
}
bool Session::normalize(std::span<const float> input,std::span<float> out,NormalizationSummary& summary,Error& error) {
  auto& i=*impl_;if(!i.allowed(error))return false;
  if(out.size()!=input.size() || input.size()>std::numeric_limits<unsigned>::max()){error={ErrorCode::InvalidInput,"normalize","Shape mismatch"};return false;}
  NormalizationSummary candidate;candidate.method=kNormalizeMethod;
  module_abi::Failure f;double values[4]{};const int ok=i.api->normalize(i.session,input.data(),unsigned(input.size()),out.data(),values,&f);
  if(ok){candidate.mean=values[0];candidate.sampleStd=values[1];candidate.normalizationStd=values[2];candidate.epsilonGuardApplied=values[3]!=0;summary=std::move(candidate);}
  return i.done(ok,f,error);
}
bool Session::radixHistogram(std::span<const float> input,std::span<const RadixPrefix> prefixes,std::span<std::uint64_t> out,Error& error) {
  auto& i=*impl_;if(!i.allowed(error))return false;
  if(input.size()>std::numeric_limits<unsigned>::max() || prefixes.size()>4 || out.size()!=prefixes.size()*256){error={ErrorCode::InvalidInput,"radix","Shape mismatch"};return false;}
  module_abi::Failure f;const int ok=i.api->histogram(i.session,input.data(),unsigned(input.size()),prefixes.data(),unsigned(prefixes.size()),out.data(),&f);return i.done(ok,f,error);
}
bool Session::selectOrderStatistics(std::span<const float> values,std::span<const std::uint64_t> ranks,
                                    std::span<float> orderStatistics,Error& error) {
  auto& i=*impl_;if(!i.allowed(error))return false;
  if(!i.api->select){error={ErrorCode::Unsupported,"select","GPU module has no order-statistic entry"};return false;}
  if(values.size()>std::numeric_limits<unsigned>::max() || ranks.empty() || ranks.size()>4 || orderStatistics.size()!=ranks.size()){
    error={ErrorCode::InvalidInput,"select","Shape mismatch"};return false;
  }
  module_abi::Failure f;const int ok=i.api->select(i.session,values.data(),unsigned(values.size()),ranks.data(),unsigned(ranks.size()),orderStatistics.data(),&f);
  return i.done(ok,f,error);
}
bool Session::drain(Error& error) {auto& i=*impl_;if(!i.allowed(error))return false;module_abi::Failure f;return i.done(i.api->drain(i.session,&f),f,error);}
} // namespace signalstudio::gpu_dsp
