// Build as forward-compatible PTX. No fast math; --fmad=false --ftz=false.
#include <cuda_runtime.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <cuda.h>
#include <cufft.h>
#include <cub/cub.cuh>
#define SS_GPU_DSP_MODULE_ABI_ONLY
#include "infrastructure/gpu_dsp.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
__device__ bool finite2(float2 x) { return isfinite(x.x) && isfinite(x.y); }
__device__ void invalid(unsigned* bad) { atomicExch(bad, 1U); }
}
extern "C" __global__ void ss_fir_coeff(const float* h, float2* out, unsigned k,
                                        unsigned taps, unsigned* bad) {
  unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= k) return;
  float value = h[i];
  if (!isfinite(value) || (i >= taps && value != 0.f)) invalid(bad);
  out[i] = make_float2(value, 0.f);
}
extern "C" __global__ void ss_fir_mix(const float2* raw, float2* work,
    unsigned rawCount, unsigned k, unsigned long long startModulo,
    unsigned long long contextN, double fs, double fc, unsigned* bad) {
  unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= k) return;
  if (i >= rawCount) { work[i] = make_float2(0.f, 0.f); return; }
  float2 x = raw[i];
  if (!finite2(x)) invalid(bad);
  const unsigned long long local = (startModulo + i) % contextN;
  constexpr double tau = 6.283185307179586476925286766559;
  double phase = fmod(-tau * fc * static_cast<double>(local) / fs, tau);
  if (phase < 0) phase += tau;
  // CPU periodicMixed rounds phase to float but evaluates sin/cos as double.
  double rounded = static_cast<double>(__double2float_rn(phase));
  float c = __double2float_rn(cos(rounded)), s = __double2float_rn(sin(rounded));
  float2 value = make_float2(x.x * c - x.y * s, x.x * s + x.y * c);
  if (!finite2(value)) invalid(bad);
  work[i] = value;
}
extern "C" __global__ void ss_fir_multiply(float2* work, const float2* filter,
                                           unsigned k, unsigned* bad) {
  unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= k) return;
  float2 a = work[i], b = filter[i];
  float2 v = make_float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
  if (!finite2(v)) invalid(bad);
  work[i] = v;
}
extern "C" __global__ void ss_fir_energy(float2* work, float* energy,
    unsigned k, unsigned radius, unsigned count, unsigned* bad) {
  unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= count) return;
  unsigned at = 2U * radius + i;
  float2 x = work[at];
  x.x /= static_cast<float>(k); x.y /= static_cast<float>(k);
  float power = x.x * x.x + x.y * x.y;
  if (!finite2(x) || !isfinite(power)) invalid(bad);
  work[at] = x; energy[i] = power;
}
extern "C" __global__ void ss_stft_window(const float2* raw, const double* hann,
    double2* transformed, unsigned k, unsigned elements, unsigned* bad) {
  unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= elements) return;
  float2 x = raw[i]; double h = hann[i % k];
  if (!finite2(x) || !isfinite(h)) invalid(bad);
  transformed[i] = make_double2(static_cast<double>(x.x) * h, static_cast<double>(x.y) * h);
}
extern "C" __global__ void ss_stft_power(const double2* transformed,
    const double* weights, double* power, unsigned k, unsigned records, unsigned* bad) {
  unsigned bin = blockIdx.x * blockDim.x + threadIdx.x;
  if (bin >= k) return;
  const unsigned source = (bin + k - k / 2) % k;
  double sum = 0;
  for (unsigned record = 0; record < records; ++record) {
    double2 x = transformed[static_cast<unsigned long long>(record) * k + source];
    double weight = weights[record];
    if (!isfinite(weight) || weight < 0) invalid(bad);
    double value = x.x * x.x + x.y * x.y;
    sum += value * weight;
  }
  if (!isfinite(sum) || sum < 0) invalid(bad);
  power[bin] = sum;
}
extern "C" __global__ void ss_stft_density(const double* power, float* out,
    unsigned sourceBins, unsigned bins, double divisor, double densityScale,
    unsigned long long* guards, unsigned* bad) {
  unsigned bin = blockIdx.x * blockDim.x + threadIdx.x;
  if (bin >= bins) return;
  double sum;
  if (sourceBins == bins) sum = power[bin];
  else {
    sum = 0;
    double scale = static_cast<double>(sourceBins) / bins;
    double left = bin * scale, right = (bin + 1.0) * scale;
    unsigned end = min(sourceBins, static_cast<unsigned>(ceil(right)));
    for (unsigned at = static_cast<unsigned>(left); at < end; ++at)
      sum += power[at] * fmax(0.0, fmin(right, at + 1.0) - fmax(left, static_cast<double>(at))) / scale;
  }
  float average = __double2float_rn(sum / divisor);
  if (!isfinite(average)) invalid(bad);
  if (average < FLT_MIN) atomicAdd(guards, 1ULL);
  float clamped = fmaxf(average, FLT_MIN);
  if (clamped / __double2float_rn(densityScale) == 0.f) atomicAdd(guards + 1, 1ULL);
  float value = __double2float_rn(10.0 * log10(static_cast<double>(clamped) / densityScale));
  if (!isfinite(value)) invalid(bad);
  out[bin] = value;
}

// Normalization uses SDK CUB reductions; these are only pointwise adapters.
struct Promote { __device__ double operator()(float x) const { return double(x); } };
struct CenteredSquare {
  const double* sum; unsigned count;
  __device__ double operator()(float x) const { double d=double(x)-sum[0]/count;return d*d; }
};
__global__ void ss_cub_summary(double* values,unsigned n,unsigned* bad) {
  if(threadIdx.x||blockIdx.x)return;
  double mean=values[0]/n,sd=sqrt(fmax(0.0,values[1]/double(n-1)));
  if(!isfinite(mean)||!isfinite(values[1])||!isfinite(sd))invalid(bad);
  values[0]=mean;values[1]=sd;values[2]=fmax(sd,double(FLT_EPSILON));values[3]=sd<double(FLT_EPSILON)?1:0;
}
__global__ void ss_cub_normalized(const float* x,float* out,unsigned n,const double* stats,unsigned* bad) {
  unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;
  float v=__double2float_rn((double(x[i])-stats[0])/stats[2]);if(!isfinite(v))invalid(bad);out[i]=v;
}
__global__ void ss_radix_prefix_samples(const float* x,unsigned* keys,unsigned n,unsigned prefix,unsigned bytes,unsigned* bad) {
  unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;
  if(!isfinite(x[i])){invalid(bad);keys[i]=256;return;}
  unsigned b=__float_as_uint(x[i]),key=b&0x80000000U?~b:b^0x80000000U;
  unsigned mask=bytes?0xffffffffU<<(32-bytes*8):0;
  keys[i]=(key&mask)==prefix?((key>>(24-bytes*8))&255):256;
}

namespace {
using namespace signalstudio::gpu_dsp;
namespace abi=signalstudio::gpu_dsp::module_abi;
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
struct Problem:std::runtime_error{ErrorCode code;const char* stage;Problem(ErrorCode c,const char* s,const std::string& d):std::runtime_error(d),code(c),stage(s){}};
void check(bool b,const char* stage,const char* detail,ErrorCode code=ErrorCode::InvalidInput){if(!b)throw Problem(code,stage,detail);}
void cudaCheck(cudaError_t result,const char* stage){if(result!=cudaSuccess)throw Problem(ErrorCode::Driver,stage,cudaGetErrorString(result));}
void fftCheck(cufftResult result,const char* stage){if(result!=CUFFT_SUCCESS)throw Problem(ErrorCode::Fft,stage,"cuFFT status="+std::to_string(int(result)));}
void failure(abi::Failure* f,ErrorCode code,const char* stage,const char* detail)noexcept{
  if(!f)return;f->code=int(code);std::snprintf(f->stage,sizeof(f->stage),"%s",stage);std::snprintf(f->detail,sizeof(f->detail),"%s",detail);
}
std::atomic<unsigned long long> ownerSequence{1};
struct Context {
  bool pushed=false;
  explicit Context(CUcontext ctx){if(cuCtxPushCurrent(ctx)!=CUDA_SUCCESS)throw Problem(ErrorCode::Driver,"context","cuCtxPushCurrent failed");pushed=true;}
  ~Context(){if(pushed){CUcontext prior=nullptr;cuCtxPopCurrent(&prior);}}
};
struct State {
  abi::Options options;abi::Telemetry t;
  std::thread::id owner=std::this_thread::get_id();CUcontext context=nullptr;int device=-1;
  cudaStream_t stream=nullptr;cudaEvent_t first=nullptr,last=nullptr;
  void* pinned=nullptr;size_t pinnedBytes=0;
  struct Buffer {void* ptr=nullptr;size_t bytes=0;};
  Buffer raw,work,coeff,filter,hann,weights,power,out,stats,bad,guards,temp,keys,hist;
  struct Plan{cufftHandle handle=0;unsigned n=0,batch=0; cufftType type=CUFFT_C2C;Buffer workspace;};
  std::vector<Plan> plans;FirFilter token;unsigned long long identity=ownerSequence++,generation=0;
  void cancel(){if(options.cancel&&options.cancel(options.cancelState))throw Problem(ErrorCode::Cancelled,"cancel","GPU DSP cancelled");}
  void release(Buffer& b)noexcept{if(b.ptr){cudaFree(b.ptr);t.device-=b.bytes;}b={};}
  void reserve(Buffer& b,size_t bytes){
    if(b.bytes>=bytes)return;cudaCheck(cudaStreamSynchronize(stream),"resize-drain");release(b);
    check(bytes<=options.limits.deviceBytes-t.device,"allocate","GPU owned-memory cap exceeded",ErrorCode::ResourceLimit);
    cudaCheck(cudaMalloc(&b.ptr,bytes),"cudaMalloc");b.bytes=bytes;t.device+=bytes;t.peak=std::max(t.peak,t.device);
  }
  void staging(size_t bytes){
    check(bytes<=options.limits.pinnedBytes,"staging","Pinned cap exceeded",ErrorCode::ResourceLimit);if(pinnedBytes>=bytes)return;
    auto at=Clock::now();cudaCheck(cudaStreamSynchronize(stream),"pinned-drain");if(pinned){cudaFreeHost(pinned);pinned=nullptr;pinnedBytes=0;}
    cudaCheck(cudaMallocHost(&pinned,bytes),"cudaMallocHost");pinnedBytes=bytes;t.pinned=bytes;t.hostAlloc+=ms(at);
  }
  template<class F>void timed(double& time,F fn){cudaCheck(cudaEventRecord(first,stream),"event-start");fn();cudaCheck(cudaEventRecord(last,stream),"event-end");cudaCheck(cudaEventSynchronize(last),"event-wait");float value=0;cudaCheck(cudaEventElapsedTime(&value,first,last),"event-time");time+=value;}
  void upload(Buffer& to,const void* from,size_t bytes){staging(bytes);std::memcpy(pinned,from,bytes);timed(t.h2d,[&]{cudaCheck(cudaMemcpyAsync(to.ptr,pinned,bytes,cudaMemcpyHostToDevice,stream),"H2D");});t.inBytes+=bytes;}
  void download(void* to,const void* from,size_t bytes){staging(bytes);timed(t.d2h,[&]{cudaCheck(cudaMemcpyAsync(pinned,from,bytes,cudaMemcpyDeviceToHost,stream),"D2H");});std::memcpy(to,pinned,bytes);t.outBytes+=bytes;}
  void zero(Buffer& b,size_t bytes){cudaCheck(cudaMemsetAsync(b.ptr,0,bytes,stream),"memset");}
  template<class F>void kernel(F fn){timed(t.kernels,[&]{fn();cudaCheck(cudaGetLastError(),"kernel-launch");});++t.launches;cancel();}
  void shape(std::uint64_t count){check(count&&count<=options.limits.maximumElements&&count<=(1U<<28),"shape","Element cap exceeded",ErrorCode::ResourceLimit);}
  void host(size_t bytes){check(bytes<=options.limits.hostBytes,"host","Atomic host output cap exceeded",ErrorCode::ResourceLimit);t.hostStaging=std::max(t.hostStaging,std::uint64_t(bytes));}
  void validate(){unsigned flag=0;download(&flag,bad.ptr,4);check(!flag,"numerical","Nonfinite GPU data or invalid padding/weights",ErrorCode::Numerical);cancel();}
  void destroyPlan(Plan& p)noexcept{if(p.handle)cufftDestroy(p.handle);t.workspace-=p.workspace.bytes;release(p.workspace);p.handle=0;}
  Plan& getPlan(unsigned n,unsigned batch,cufftType type){
    for(auto& p:plans)if(p.n==n&&p.batch==batch&&p.type==type)return p;
    auto at=Clock::now();if(plans.size()==options.limits.plans){destroyPlan(plans.front());plans.erase(plans.begin());}
    Plan p;p.n=n;p.batch=batch;p.type=type;
    try{fftCheck(cufftCreate(&p.handle),"cufftCreate");fftCheck(cufftSetAutoAllocation(p.handle,0),"cufft-autoalloc-off");size_t size=0;int dims=int(n);
      fftCheck(cufftMakePlanMany(p.handle,1,&dims,nullptr,1,int(n),nullptr,1,int(n),type,int(batch),&size),"cufftMakePlanMany");
      reserve(p.workspace,size);t.workspace+=p.workspace.bytes;if(size)fftCheck(cufftSetWorkArea(p.handle,p.workspace.ptr),"cufftSetWorkArea");
      fftCheck(cufftSetStream(p.handle,stream),"cufftSetStream");plans.push_back(p);p.handle=0;p.workspace={};t.plans=unsigned(plans.size());t.plan+=ms(at);return plans.back();
    }catch(...){destroyPlan(p);throw;}
  }
  void transform(Plan& p,Buffer& b,int direction){timed(t.fft,[&]{if(p.type==CUFFT_C2C)fftCheck(cufftExecC2C(p.handle,(cufftComplex*)b.ptr,(cufftComplex*)b.ptr,direction),"cufftExecC2C");else fftCheck(cufftExecZ2Z(p.handle,(cufftDoubleComplex*)b.ptr,(cufftDoubleComplex*)b.ptr,direction),"cufftExecZ2Z");});++t.execs;cancel();}
  ~State(){
    if(owner!=std::this_thread::get_id())std::terminate();if(!context)return;
    if(cuCtxPushCurrent(context)==CUDA_SUCCESS){if(stream)cudaStreamSynchronize(stream);for(auto& p:plans)destroyPlan(p);
      for(auto* b:{&raw,&work,&coeff,&filter,&hann,&weights,&power,&out,&stats,&bad,&guards,&temp,&keys,&hist})release(*b);
      if(pinned)cudaFreeHost(pinned);if(first)cudaEventDestroy(first);if(last)cudaEventDestroy(last);if(stream)cudaStreamDestroy(stream);CUcontext old=nullptr;cuCtxPopCurrent(&old);}
    cuDevicePrimaryCtxRelease(device);
  }
};
// All exceptions and allocations remain inside this DLL. API returns POD errors.
template<class F>int invoke(void* opaque,abi::Failure* error,F fn)noexcept{
  auto* s=static_cast<State*>(opaque);auto at=Clock::now();
  try{check(s&&s->owner==std::this_thread::get_id(),"thread","Wrong GPU owner thread",ErrorCode::WrongThread);Context current(s->context);
    try{s->cancel();s->reserve(s->bad,4);s->zero(s->bad,4);fn(*s);}catch(...){cudaStreamSynchronize(s->stream);throw;}
    s->t.host+=ms(at);if(error)*error={};return 1;
  }catch(const Problem& e){failure(error,e.code,e.stage,e.what());}catch(const std::exception& e){failure(error,ErrorCode::ResourceLimit,"host",e.what());}catch(...){failure(error,ErrorCode::Driver,"module","Unknown CUDA module error");}
  if(s&&s->owner==std::this_thread::get_id())s->t.host+=ms(at);return 0;
}
void* create(const abi::Options* options,abi::Info* info,abi::Failure* error)noexcept{
  State* s=nullptr;auto at=Clock::now();
  try{check(options&&info,"setup","Missing options/info");s=new State;s->options=*options;
    check(options->limits.plans&&options->limits.plans<=16&&options->limits.maximumBatch&&options->limits.maximumBatch<=1024&&
          options->limits.maximumTransform&&options->limits.maximumTransform<=(1U<<24)&&options->limits.deviceBytes>=65536&&options->limits.pinnedBytes>=65536,"limits","Invalid GPU caps");
    cudaCheck(cudaDriverGetVersion(&info->driver),"driver-version");fftCheck(cufftGetVersion(&info->cufft),"cufft-version");
    check(cuInit(0)==CUDA_SUCCESS,"driver","cuInit failed",ErrorCode::Driver);CUdevice device;
    check(cuDeviceGet(&device,options->device)==CUDA_SUCCESS,"device","Invalid CUDA device",ErrorCode::Driver);s->device=device;
    check(cuDevicePrimaryCtxRetain(&s->context,device)==CUDA_SUCCESS,"context","Primary context retain failed",ErrorCode::Driver);
    Context current(s->context);cudaCheck(cudaSetDevice(options->device),"cudaSetDevice");cudaDeviceProp prop{};cudaCheck(cudaGetDeviceProperties(&prop,options->device),"device-info");
    info->major=prop.major;info->minor=prop.minor;info->total=prop.totalGlobalMem;std::snprintf(info->name,sizeof(info->name),"%s",prop.name);
    size_t free=0,total=0;cudaCheck(cudaMemGetInfo(&free,&total),"memory-info");info->free=free;
    cudaCheck(cudaStreamCreateWithFlags(&s->stream,cudaStreamNonBlocking),"stream");cudaCheck(cudaEventCreate(&s->first),"event");cudaCheck(cudaEventCreate(&s->last),"event");
    s->t.setup=ms(at);if(error)*error={};return s;
  }catch(const Problem& e){failure(error,e.code,e.stage,e.what());}catch(const std::exception& e){failure(error,ErrorCode::ResourceLimit,"setup",e.what());}catch(...){failure(error,ErrorCode::Driver,"setup","Unknown setup failure");}
  delete s;return nullptr;
}
void destroy(void* s)noexcept{delete static_cast<State*>(s);}
int prepare(void* state,const float* coeff,unsigned k,unsigned radius,FirFilter* output,abi::Failure* error)noexcept{
  return invoke(state,error,[&](State& s){s.shape(k);check(coeff&&output&&k<=s.options.limits.maximumTransform&&!(k&(k-1))&&2ULL*radius+1<=k,"FIR","Invalid FIR coefficients/radius");s.token={};
    s.reserve(s.coeff,k*4ULL);s.reserve(s.filter,k*8ULL);s.upload(s.coeff,coeff,k*4ULL);
    s.kernel([&]{ss_fir_coeff<<<(k+255)/256,256,0,s.stream>>>((float*)s.coeff.ptr,(float2*)s.filter.ptr,k,2*radius+1,(unsigned*)s.bad.ptr);});
    auto& plan=s.getPlan(k,1,CUFFT_C2C);s.transform(plan,s.filter,CUFFT_FORWARD);s.validate();s.token={s.identity,++s.generation,k,radius};*output=s.token;});
}
int fir(void* state,const FirFilter* token,const void* raw,unsigned rawCount,std::int64_t start,std::uint64_t contextN,double fs,double fc,unsigned count,float* energy,void* tap,abi::Failure* error)noexcept{
  return invoke(state,error,[&](State& s){check(token&&raw&&energy&&token->owner==s.identity&&token->generation&&token->generation==s.token.generation&&token->fftSamples==s.token.fftSamples&&token->radius==s.token.radius,"FIR","Invalid/stale FIR token");
    unsigned k=token->fftSamples,radius=token->radius;s.shape(rawCount);
    check(count&&rawCount==count+2ULL*radius&&rawCount<=k&&contextN&&contextN<=std::uint64_t(INT64_MAX)&&std::isfinite(fs)&&fs>0&&std::isfinite(fc)&&std::abs(fc)<=fs,"FIR","Invalid FIR block/context");
    s.host(count*(tap?12ULL:4ULL));s.reserve(s.raw,rawCount*8ULL);s.reserve(s.work,k*8ULL);s.reserve(s.out,count*4ULL);s.upload(s.raw,raw,rawCount*8ULL);
    auto mod=start%std::int64_t(contextN);if(mod<0)mod+=std::int64_t(contextN);
    s.kernel([&]{ss_fir_mix<<<(k+255)/256,256,0,s.stream>>>((float2*)s.raw.ptr,(float2*)s.work.ptr,rawCount,k,mod,contextN,fs,fc,(unsigned*)s.bad.ptr);});
    auto& plan=s.getPlan(k,1,CUFFT_C2C);s.transform(plan,s.work,CUFFT_FORWARD);
    s.kernel([&]{ss_fir_multiply<<<(k+255)/256,256,0,s.stream>>>((float2*)s.work.ptr,(float2*)s.filter.ptr,k,(unsigned*)s.bad.ptr);});s.transform(plan,s.work,CUFFT_INVERSE);
    s.kernel([&]{ss_fir_energy<<<(count+255)/256,256,0,s.stream>>>((float2*)s.work.ptr,(float*)s.out.ptr,k,radius,count,(unsigned*)s.bad.ptr);});s.validate();
    std::vector<float> values(count);std::vector<float2> iq(tap?count:0);s.download(values.data(),s.out.ptr,count*4ULL);
    if(tap)s.download(iq.data(),(float2*)s.work.ptr+2*radius,count*8ULL);s.cancel();std::memcpy(energy,values.data(),count*4ULL);if(tap)std::memcpy(tap,iq.data(),count*8ULL);});
}
int stft(void* state,const abi::Stft* request,float* out,StftSummary* summary,abi::Failure* error)noexcept{
  return invoke(state,error,[&](State& s){check(request&&out&&summary,"STFT","Missing STFT parameters");const auto& r=*request;s.shape(std::uint64_t(r.k)*r.batch);
    check(r.records&&r.hann&&r.k<=s.options.limits.maximumTransform&&r.batch<=s.options.limits.maximumBatch&&r.bins&&r.bins<=s.options.limits.maximumTransform&&std::isfinite(r.fs)&&r.fs>0&&std::isfinite(r.windowEnergy)&&r.windowEnergy>0&&std::isfinite(r.divisor)&&r.divisor>0,"STFT","Invalid STFT shape/scale");double scale=r.fs*r.windowEnergy;check(std::isfinite(scale),"STFT","Density scale overflow");
    s.host(r.bins*4ULL+r.batch*8ULL);s.reserve(s.raw,std::uint64_t(r.k)*r.batch*8);s.reserve(s.work,std::uint64_t(r.k)*r.batch*16);s.reserve(s.hann,r.k*8ULL);s.reserve(s.weights,r.batch*8ULL);s.reserve(s.power,r.k*8ULL);s.reserve(s.out,r.bins*4ULL);s.reserve(s.guards,16);
    s.upload(s.raw,r.records,std::uint64_t(r.k)*r.batch*8);s.upload(s.hann,r.hann,r.k*8ULL);std::vector<double> weights(r.weights?0:r.batch,1);s.upload(s.weights,r.weights?r.weights:weights.data(),r.batch*8ULL);s.zero(s.guards,16);
    s.kernel([&]{ss_stft_window<<<(r.k*r.batch+255)/256,256,0,s.stream>>>((float2*)s.raw.ptr,(double*)s.hann.ptr,(double2*)s.work.ptr,r.k,r.k*r.batch,(unsigned*)s.bad.ptr);});
    auto& plan=s.getPlan(r.k,r.batch,CUFFT_Z2Z);s.transform(plan,s.work,CUFFT_FORWARD);
    s.kernel([&]{ss_stft_power<<<(r.k+255)/256,256,0,s.stream>>>((double2*)s.work.ptr,(double*)s.weights.ptr,(double*)s.power.ptr,r.k,r.batch,(unsigned*)s.bad.ptr);});
    s.kernel([&]{ss_stft_density<<<(r.bins+255)/256,256,0,s.stream>>>((double*)s.power.ptr,(float*)s.out.ptr,r.k,r.bins,r.divisor,scale,(unsigned long long*)s.guards.ptr,(unsigned*)s.bad.ptr);});s.validate();
    std::vector<float> values(r.bins);unsigned long long guards[2]{};s.download(values.data(),s.out.ptr,r.bins*4ULL);s.download(guards,s.guards.ptr,16);s.cancel();std::memcpy(out,values.data(),r.bins*4ULL);*summary={guards[0],guards[1]};});
}
int normalize(void* state,const float* input,unsigned n,float* output,double* summary,abi::Failure* error)noexcept{
  return invoke(state,error,[&](State& s){s.shape(n);check(n>=2&&input&&output&&summary,"normalize","Invalid normalization input");s.host(n*4ULL);s.reserve(s.raw,n*4ULL);s.reserve(s.out,n*4ULL);s.reserve(s.stats,32);s.upload(s.raw,input,n*4ULL);
    cub::TransformInputIterator<double,Promote,const float*> values((float*)s.raw.ptr,Promote{});
    size_t bytes=0;cudaCheck(cub::DeviceReduce::Sum(nullptr,bytes,values,(double*)s.stats.ptr,int(n),s.stream),"CUB reduce query");s.reserve(s.temp,bytes);
    s.timed(s.t.kernels,[&]{cudaCheck(cub::DeviceReduce::Sum(s.temp.ptr,bytes,values,(double*)s.stats.ptr,int(n),s.stream),"CUB FP64 sum");});++s.t.sdkCalls;
    cub::TransformInputIterator<double,CenteredSquare,const float*> centered((float*)s.raw.ptr,CenteredSquare{(double*)s.stats.ptr,n});
    size_t centeredBytes=0;cudaCheck(cub::DeviceReduce::Sum(nullptr,centeredBytes,centered,(double*)s.stats.ptr+1,int(n),s.stream),"CUB variance query");s.reserve(s.temp,centeredBytes);
    s.timed(s.t.kernels,[&]{cudaCheck(cub::DeviceReduce::Sum(s.temp.ptr,centeredBytes,centered,(double*)s.stats.ptr+1,int(n),s.stream),"CUB FP64 centered sum");});++s.t.sdkCalls;
    s.kernel([&]{ss_cub_summary<<<1,1,0,s.stream>>>((double*)s.stats.ptr,n,(unsigned*)s.bad.ptr);});
    s.kernel([&]{ss_cub_normalized<<<(n+255)/256,256,0,s.stream>>>((float*)s.raw.ptr,(float*)s.out.ptr,n,(double*)s.stats.ptr,(unsigned*)s.bad.ptr);});s.validate();
    double meta[4];std::vector<float> valuesOut(n);s.download(meta,s.stats.ptr,32);s.download(valuesOut.data(),s.out.ptr,n*4ULL);s.cancel();std::memcpy(output,valuesOut.data(),n*4ULL);std::memcpy(summary,meta,32);});
}
int histogram(void* state,const float* input,unsigned n,const RadixPrefix* prefixes,unsigned lanes,std::uint64_t* output,abi::Failure* error)noexcept{
  return invoke(state,error,[&](State& s){s.shape(n);check(input&&prefixes&&output&&lanes&&lanes<=4,"radix","Invalid radix lanes");
    for(unsigned lane=0;lane<lanes;++lane){auto p=prefixes[lane];check(p.significantBytes<=3,"radix","Invalid prefix width");unsigned mask=p.significantBytes?0xffffffffU<<(32-p.significantBytes*8):0;check(!(p.prefix&~mask),"radix","Noncanonical prefix");}
    s.host(lanes*257*8ULL);s.reserve(s.raw,n*4ULL);s.reserve(s.keys,n*4ULL);s.reserve(s.hist,lanes*257*8ULL);s.upload(s.raw,input,n*4ULL);
    size_t bytes=0;cudaCheck(cub::DeviceHistogram::HistogramEven(nullptr,bytes,(unsigned*)s.keys.ptr,(unsigned long long*)s.hist.ptr,258,0U,257U,int(n),s.stream),"CUB histogram query");s.reserve(s.temp,bytes);
    for(unsigned lane=0;lane<lanes;++lane){auto p=prefixes[lane];s.kernel([&]{ss_radix_prefix_samples<<<(n+255)/256,256,0,s.stream>>>((float*)s.raw.ptr,(unsigned*)s.keys.ptr,n,p.prefix,p.significantBytes,(unsigned*)s.bad.ptr);});
      s.timed(s.t.kernels,[&]{cudaCheck(cub::DeviceHistogram::HistogramEven(s.temp.ptr,bytes,(unsigned*)s.keys.ptr,(unsigned long long*)s.hist.ptr+lane*257,258,0U,257U,int(n),s.stream),"CUB integer histogram");});++s.t.sdkCalls;}
    s.validate();std::vector<std::uint64_t> counts(lanes*257);s.download(counts.data(),s.hist.ptr,counts.size()*8);s.cancel();for(unsigned lane=0;lane<lanes;++lane)std::memcpy(output+lane*256,counts.data()+lane*257,256*8);});
}
int drain(void* opaque,abi::Failure* error)noexcept{
  auto* s=static_cast<State*>(opaque);
  try{check(s&&s->owner==std::this_thread::get_id(),"drain","Wrong owner thread",ErrorCode::WrongThread);Context current(s->context);cudaCheck(cudaStreamSynchronize(s->stream),"drain");if(error)*error={};return 1;}
  catch(const Problem& e){failure(error,e.code,e.stage,e.what());}catch(...){failure(error,ErrorCode::Driver,"drain","Drain failure");}return 0;
}
void telemetry(void* state,abi::Telemetry* out)noexcept{auto* s=static_cast<State*>(state);if(s&&out&&s->owner==std::this_thread::get_id()){*out=s->t;out->sdkTemporaryBytes=s->temp.bytes;}}
const abi::Api api{abi::version,sizeof(abi::Api),create,destroy,prepare,fir,stft,normalize,histogram,drain,telemetry};
}
extern "C" __declspec(dllexport) const signalstudio::gpu_dsp::module_abi::Api* ssGpuDspGetApi(unsigned version)noexcept{
  return version==signalstudio::gpu_dsp::module_abi::version?&api:nullptr;
}
