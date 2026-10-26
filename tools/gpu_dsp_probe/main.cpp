#include "infrastructure/gpu_dsp.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace signalstudio::gpu_dsp;
namespace {
void check(bool good, const std::string& why) { if (!good) throw std::runtime_error(why); }
QString q(const std::string& s) { return QString::fromStdString(s); }
QJsonObject capability(const Capabilities& c) {
  return {{"driverLoaded",c.driverLoaded},{"cufftLoaded",c.cufftLoaded},{"sessionReady",c.sessionReady},
    {"driverVersion",c.driverVersion},{"cufftVersion",c.cufftVersion},
    {"computeMajor",c.computeMajor},{"computeMinor",c.computeMinor},{"device",q(c.deviceName)},
    {"cufftLibrary",q(c.cufftLibrary)},{"moduleLibrary",q(c.moduleLibrary)},{"fallbackReason",q(c.fallbackReason)},
    {"deviceTotalBytes",QString::number(c.deviceTotalBytes)},{"deviceFreeBytesAtSetup",QString::number(c.deviceFreeBytesAtSetup)}};
}
QJsonObject metrics(const Metrics& m) {
  return {{"setupMs",m.setupMs},{"hostAllocationMs",m.hostAllocationMs},{"planMs",m.planMs},
    {"h2dMs",m.h2dMs},{"fftMs",m.fftMs},{"kernelsMs",m.kernelsMs},{"d2hMs",m.d2hMs},{"totalHostMs",m.totalHostMs},
    {"h2dBytes",QString::number(m.h2dBytes)},{"d2hBytes",QString::number(m.d2hBytes)},
    {"fftExecutions",QString::number(m.fftExecutions)},{"kernelLaunches",QString::number(m.kernelLaunches)},
    {"ownedDeviceBytes",QString::number(m.deviceBytes)},{"ownedDevicePeakBytes",QString::number(m.devicePeakBytes)},
    {"workspaceBytes",QString::number(m.workspaceBytes)},{"pinnedBytes",QString::number(m.pinnedBytes)},
    {"hostStagingBytes",QString::number(m.hostStagingBytes)},{"planCount",int(m.planCount)},
    {"cubAlgorithmCalls",QString::number(m.libraryAlgorithmCalls)},{"cubTemporaryBytes",QString::number(m.sdkTemporaryBytes)},
    {"meaning",q(m.timingMeaning)}};
}
QString goldenPython;
std::vector<float> psdGolden(const StftInput& input) {
  QTemporaryDir temp;check(temp.isValid(),"Cannot create NumPy reference directory");
  auto write=[&](const char* name,const void* data,std::size_t size) {QFile f(temp.filePath(name));check(f.open(QIODevice::WriteOnly)&&f.write(static_cast<const char*>(data),qint64(size))==qint64(size),"Write reference input");};
  write("raw.f32",input.records.data(),input.records.size_bytes());write("hann.f64",input.hann.data(),input.hann.size_bytes());
  std::vector<double> weights(input.recordCount,1);if(!input.recordWeights.empty())std::copy(input.recordWeights.begin(),input.recordWeights.end(),weights.begin());
  write("weights.f64",weights.data(),weights.size()*8);
  QProcess process;process.start(goldenPython,{QCoreApplication::applicationDirPath()+"/gpu-dsp-golden.py",
    "--input",temp.filePath("raw.f32"),"--hann",temp.filePath("hann.f64"),"--weights",temp.filePath("weights.f64"),
    "--output",temp.filePath("psd.f32"),"--k",QString::number(input.transformBins),"--records",QString::number(input.recordCount),
    "--bins",QString::number(input.outputBins),"--density-scale",QString::number(input.sampleRateHz*input.hannEnergy,'g',17),
    "--divisor",QString::number(input.recordDivisor,'g',17)});
  check(process.waitForFinished(60000)&&process.exitCode()==0,process.readAllStandardError().toStdString());
  QFile result(temp.filePath("psd.f32"));check(result.open(QIODevice::ReadOnly),"Read NumPy reference");auto data=result.readAll();
  check(data.size()==qint64(input.outputBins)*4,"NumPy reference shape mismatch");std::vector<float> out(input.outputBins);std::memcpy(out.data(),data.data(),out.size()*4);return out;
}
std::vector<float> normalizeGolden(std::span<const float> input, NormalizationSummary& stats) {
  double mean=0,m2=0;std::uint64_t count=0;
  for (float value:input) {double d=value-mean;mean+=d/++count;m2+=d*(value-mean);}
  stats.mean=mean;stats.sampleStd=std::sqrt(std::max(0.0,m2/(count-1)));
  stats.normalizationStd=std::max(stats.sampleStd,double(std::numeric_limits<float>::epsilon()));
  std::vector<float> out;out.reserve(input.size());
  for(float value:input)out.push_back(float((value-mean)/stats.normalizationStd));
  return out;
}
double maximumDifference(std::span<const float> a,std::span<const float> b) {
  check(a.size()==b.size(),"Different result sizes");double maximum=0;
  for(std::size_t i=0;i<a.size();++i) {check(std::isfinite(a[i])&&std::isfinite(b[i]),"Nonfinite comparison");maximum=std::max(maximum,std::abs(double(a[i])-b[i]));}
  return maximum;
}
void run(Session& session, Config config, bool precision, QJsonObject& report, std::atomic_bool& cancel, unsigned requestedBins) {
  Error error;
  const auto requireGpu=[&](bool ok) {check(ok,error.stage+": "+error.detail);};
  unsigned k=requestedBins?requestedBins:(precision?65536:1024),records=precision?10:3;
  check(k<=config.limits.maximumTransform&&std::uint64_t(k)*records<=config.limits.maximumElements&&
        std::uint64_t(k)*records*8<=config.limits.hostBytes,"Probe fixture exceeds bounded input budget");
  std::vector<std::complex<float>> raw(std::size_t(k)*records);
  std::uint32_t rng=0x1234abcd;
  auto random=[&] {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return float(double(rng)/4294967296.0-.5);};
  for(auto& x:raw) x={random(),random()};
  std::vector<double> hann(k);double hannEnergy=0;
  for(unsigned i=0;i<k;++i) {hann[i]=.5-.5*std::cos(2*std::numbers::pi*i/k);hannEnergy+=hann[i]*hann[i];}
  StftInput input{raw,hann,k,records,k,20e6,hannEnergy,{},double(records)};
  auto golden=psdGolden(input);
  std::vector<float> gpu(k,-999);
  StftSummary summary;
  requireGpu(session.stft(input,gpu,summary,error));
  const auto psdError=maximumDifference(gpu,golden);
  check(psdError<=3.8e-5,"PSD error exceeds unchanged 3.8e-5 gate");
  report["stft"]=QJsonObject{{"method",kStftMethod},{"records",int(records)},{"bins",int(k)},
      {"maximumAbsDb",psdError},{"tolerance",3.8e-5},{"powerFloorBins",QString::number(summary.powerFloorBins)}};
  NormalizationSummary cpuStats,gpuStats;
  auto cpuNormalized=normalizeGolden(golden,cpuStats);
  std::vector<float> normalized(k);
  requireGpu(session.normalize(gpu,normalized,gpuStats,error));
  double tensorError=maximumDifference(normalized,cpuNormalized);
  check(tensorError<=5e-6,"Tensor error exceeds unchanged 5e-6 gate");
  report["normalization"]=QJsonObject{{"method",q(gpuStats.method)},{"maximumTensorAbs",tensorError},{"tolerance",5e-6},
      {"meanError",std::abs(gpuStats.mean-cpuStats.mean)},{"stdError",std::abs(gpuStats.sampleStd-cpuStats.sampleStd)}};
  // Separately test a full model-sized normalization tensor, not just one frame.
  std::vector<float> model(std::size_t(k)*records);
  for(auto& value:model)value=-80.f+random()*40.f;
  auto modelGolden=normalizeGolden(model,cpuStats);normalized.resize(model.size());
  requireGpu(session.normalize(model,normalized,gpuStats,error));
  double modelError=maximumDifference(normalized,modelGolden);
  check(modelError<=5e-6,"Model-shaped normalization exceeds 5e-6 gate");
  report["modelNormalization"]=QJsonObject{{"values",QString::number(model.size())},{"maximumTensorAbs",modelError},{"method",q(gpuStats.method)}};
  // Exercise physical-bin area overlap and explicit per-record weights.
  std::vector<double> recordWeights(records);for(unsigned j=0;j<records;++j)recordWeights[j]=1.+.125*j;
  auto resampled=input;resampled.outputBins=k/2+13;resampled.recordWeights=recordWeights;
  auto resampleGolden=psdGolden(resampled);std::vector<float> resampleValues(resampled.outputBins);
  requireGpu(session.stft(resampled,resampleValues,summary,error));
  auto resampleError=maximumDifference(resampleValues,resampleGolden);check(resampleError<=3.8e-5,"Resampled PSD exceeds 3.8e-5");
  report["resampledWeightedStft"]=QJsonObject{{"bins",int(resampled.outputBins)},{"maximumAbsDb",resampleError}};
  report["normalizationReference"]="CPU source ordered Welford, GPU CUB FP64 two-pass";
  // Periodic block seam, including negative localStart and multiple wraparounds.
  const unsigned firK=precision?65536:1024,radius=7,count=precision?32768:512;
  std::vector<float> coefficients(firK,0);double sum=0;
  for(unsigned j=0;j<=2*radius;++j) {coefficients[j]=float(std::exp(-double(int(j)-int(radius))*double(int(j)-int(radius))/8));sum+=coefficients[j];}
  for(unsigned j=0;j<=2*radius;++j)coefficients[j]=float(coefficients[j]/sum);
  FirFilter filter;check(session.prepareFir(coefficients,radius,filter,error),error.detail);
  PeriodicFirInput block;block.localStart=-7;block.contextN=137;block.sampleRateHz=20e6;block.relativeCenterHz=1234567;block.count=count;
  std::vector<std::complex<float>> unmixed(count+2*radius),mixed(unmixed.size());
  for(unsigned j=0;j<unmixed.size();++j) {
    auto index=(block.localStart+int(j))%137;if(index<0)index+=137;
    unmixed[j]={float(std::cos(double(index)*.13)),float(std::sin(double(index)*.17))};
    double phase=std::fmod(-2*std::numbers::pi*block.relativeCenterHz*double(index)/block.sampleRateHz,2*std::numbers::pi);
    if(phase<0)phase+=2*std::numbers::pi;double rounded=float(phase);
    mixed[j]=unmixed[j]*std::complex<float>(float(std::cos(rounded)),float(std::sin(rounded)));
  }
  block.raw=unmixed;std::vector<float> energy(count),energyGolden(count);std::vector<std::complex<float>> tap(count);
  check(session.periodicFirBlock(filter,block,energy,error,tap),error.detail);
  double iqError=0;
  for(unsigned j=0;j<count;++j) {
    std::complex<double> value{};
    for(unsigned t=0;t<=2*radius;++t)value+=std::complex<double>(mixed[2*radius+j-t])*double(coefficients[t]);
    energyGolden[j]=float(std::norm(value));iqError=std::max(iqError,std::abs(value-std::complex<double>(tap[j])));
  }
  report["fir"]=QJsonObject{{"method",kFirMethod},{"fftSamples",int(firK)},{"count",int(count)},{"maximumEnergyAbs",maximumDifference(energy,energyGolden)},
      {"maximumIqAbs",iqError},{"reference","independent direct convolution; not LegacyExact Sample/topology acceptance"}};
  check(iqError<1e-4,"FIR arithmetic smoke error exceeds 1e-4; not a Sample publication tolerance");
  auto wrongToken=filter;wrongToken.owner^=1;std::fill(energy.begin(),energy.end(),123.f);
  check(!session.periodicFirBlock(wrongToken,block,energy,error)&&error.code==ErrorCode::InvalidInput,"Cross-session FIR token accepted");
  check(std::all_of(energy.begin(),energy.end(),[](float x){return x==123.f;}),"Invalid token overwrote output");
  report["firTokenRejectedAtomic"]=true;
  // Explicit zero/padding floor and nonfinite rejection, using one small record.
  std::vector<std::complex<float>> zeros(1024);std::vector<double> zeroHann(1024,1.);std::vector<float> floor(1024);
  StftInput zeroInput{zeros,zeroHann,1024,1,1024,20e6,1024.,{},1.};
  requireGpu(session.stft(zeroInput,floor,summary,error));
  check(summary.powerFloorBins==1024&&summary.densityUnderflowGuardBins==1024,"Zero-power guard counts differ");
  auto zeroGolden=psdGolden(zeroInput);check(maximumDifference(floor,zeroGolden)<=3.8e-5,"Zero-power density differs");
  zeros[0]={std::numeric_limits<float>::quiet_NaN(),0};std::fill(floor.begin(),floor.end(),123.f);
  check(!session.stft(zeroInput,floor,summary,error)&&error.code==ErrorCode::Numerical,"Nonfinite GPU input not rejected");
  check(std::all_of(floor.begin(),floor.end(),[](float v){return v==123.f;}),"Numerical failure overwrote output");report["nonfiniteAtomicOutput"]=true;
  Config limited=config;limited.limits.deviceBytes=65536;
  Capabilities limitedCaps;auto bounded=Session::create(limited,limitedCaps,error);check(bool(bounded),error.detail);
  std::fill(normalized.begin(),normalized.end(),123.f);
  // At least 65536 floats so device input alone exceeds the 64 KiB owned cap.
  std::vector<float> quotaInput(std::max<std::size_t>(model.size(),65536),1),quotaOutput(quotaInput.size(),123.f);
  check(!bounded->normalize(quotaInput,quotaOutput,gpuStats,error)&&error.code==ErrorCode::ResourceLimit,"Owned VRAM cap was not enforced");
  check(std::all_of(quotaOutput.begin(),quotaOutput.end(),[](float v){return v==123.f;}),"Quota failure overwrote output");
  report["deviceQuotaAtomicOutput"]=true;bounded.reset();
  // Both integer counts and prefix selection must be exact, including signed zero.
  std::vector<float> histInput=model;histInput.push_back(0.f);histInput.push_back(-0.f);
  unsigned bits=std::bit_cast<unsigned>(histInput[0]);unsigned key=bits&0x80000000U?~bits:bits^0x80000000U;
  std::array<RadixPrefix,4> prefixes{{{0,0},{key&0xff000000U,1},{key&0xffff0000U,2},{key&0xffffff00U,3}}};
  std::vector<std::uint64_t> counts(1024),expected(1024,0);
  for(float value:histInput) {
    unsigned b=std::bit_cast<unsigned>(value),v=b&0x80000000U?~b:b^0x80000000U;
    for(unsigned lane=0;lane<4;++lane) {auto p=prefixes[lane];unsigned mask=p.significantBytes?0xffffffffU<<(32-p.significantBytes*8):0;
      if((v&mask)==p.prefix)++expected[lane*256+((v>>(24-p.significantBytes*8))&255)];}
  }
  check(session.radixHistogram(histInput,prefixes,counts,error),error.detail);
  check(counts==expected,"Integer radix histogram mismatch");report["radixExact"]=true;
  auto wrongThread=std::async(std::launch::async,[&] {Error issue;NormalizationSummary meta;return !session.normalize(model,normalized,meta,issue)&&issue.code==ErrorCode::WrongThread;});
  check(wrongThread.get(),"Wrong-thread operation was not rejected");report["wrongThreadRejected"]=true;
  std::fill(normalized.begin(),normalized.end(),123.f);cancel=true;
  check(!session.normalize(model,normalized,gpuStats,error)&&error.code==ErrorCode::Cancelled,"Cancellation was not rejected");
  check(std::all_of(normalized.begin(),normalized.end(),[](float v){return v==123.f;}),"Cancelled GPU operation overwrote output");
  check(session.drain(error),error.detail);cancel=false;report["cancelAtomicOutput"]=true;
  report["metrics"]=metrics(session.metrics());
  check(session.metrics().devicePeakBytes<=config.limits.deviceBytes&&session.metrics().pinnedBytes<=config.limits.pinnedBytes&&
        session.metrics().planCount<=config.limits.plans,"Resource cap exceeded");
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);auto args=app.arguments();
  auto value=[&](const char* flag){int at=args.indexOf(flag);return at>=0&&at+1<args.size()?args[at+1]:QString{};};
  goldenPython=value("--python");if(goldenPython.isEmpty())goldenPython="python";
  Config config;config.moduleLibrary=value("--module").toStdString();config.cufftLibrary=value("--cufft").toStdString();config.nvJitLinkLibrary=value("--nvjitlink").toStdString();
  std::atomic_bool cancel=false;config.cancelled=[&]{return cancel.load();};
  Capabilities caps;Error error;QJsonObject report{{"schema","signal-studio-gpu-dsp-probe-v1"},
    {"scope","synthetic isolated numerical/driver probe; not model inference or real-label/Sample topology acceptance"}};
  int exit=0;auto session=Session::create(config,caps,error);report["capabilities"]=capability(caps);
  if(!session){report["status"]="UNSUPPORTED";report["error"]=q(error.stage+": "+error.detail);exit=2;}
  else {
    try{bool valid=false;unsigned requested=0;auto bins=value("--transform-bins");if(!bins.isEmpty()){requested=bins.toUInt(&valid);check(valid&&requested,"Invalid --transform-bins");}
      run(*session,config,args.contains("--precision"),report,cancel,requested);report["status"]="PASS_SYNTHETIC_GPU_PROBE";}
    catch(const std::exception& e){report["status"]="FAIL";report["error"]=QString::fromUtf8(e.what());report["metrics"]=metrics(session->metrics());exit=1;}
  }
#ifdef _WIN32
  QJsonArray runtime;
  for(const auto* name:{L"nvcuda.dll",L"cufft64_11.dll",L"cudart64_12.dll",L"nvrtc64_120_0.dll",L"nvJitLink_120_0.dll"}) {
    if(auto module=GetModuleHandleW(name)) {wchar_t path[32768]{};GetModuleFileNameW(module,path,32768);runtime.append(QString::fromWCharArray(path));}
  }
  report["loadedRuntimeDlls"]=runtime;
#endif
  session.reset();auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);
  const auto output=value("--output");
  if(!output.isEmpty()) {QDir().mkpath(QFileInfo(output).absolutePath());QSaveFile file(output);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())return 3;}
  std::cout<<bytes.toStdString();return exit;
}
