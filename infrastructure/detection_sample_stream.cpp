#include "infrastructure/detection_sample_stream.h"
#include "infrastructure/detection_sample_stream_scratch.h"
#include "infrastructure/detection_sample_energy.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <utility>

namespace signalstudio::sample_stream {
namespace {
using detail::fail;
using detail::cancelled; // returns true when execution may continue
constexpr std::uint64_t MiB = 1024*1024;
constexpr auto maxIndex = std::numeric_limits<SampleIndex>::max();
bool finite(double x, double lo, double hi) { return std::isfinite(x) && x>=lo && x<=hi; }
bool viewValid(ViewRange v) {
  return v.time.begin<v.time.end && std::isfinite(v.frequency.lowerHz) &&
      std::isfinite(v.frequency.upperHz) && v.frequency.lowerHz<v.frequency.upperHz &&
      std::isfinite(v.frequency.upperHz-v.frequency.lowerHz);
}
ViewRange clipped(ViewRange a, ViewRange b) {
  return {{std::max(a.time.begin,b.time.begin),std::min(a.time.end,b.time.end)},
          {std::max(a.frequency.lowerHz,b.frequency.lowerHz),std::min(a.frequency.upperHz,b.frequency.upperHz)}};
}
SampleIndex saturatedAdd(SampleIndex a, SampleIndex b) { return b>maxIndex-a ? maxIndex : a+b; }
bool duration(double seconds, double fs, SampleIndex minimum, SampleIndex& v) {
  const double count = seconds*fs;
  if (!std::isfinite(count) || count<0 || count>=static_cast<double>(maxIndex)) return false;
  v=std::max(minimum,static_cast<SampleIndex>(std::floor(count))); return true;
}
std::uint64_t largestLeaf(std::uint64_t n) {
  while (n>1) {
    std::uint64_t r=n;
    for (const auto p : {2U,3U,5U,7U,11U,13U}) if (n%p==0) { r=p; break; }
    if (r==n) return n;
    n/=r;
  }
  return 1;
}
bool contextValid(const Context& c, const Config& cfg, const Runtime& rt, Failure& f) {
  const auto& i=c.input; const auto& o=c.options; const auto& p=c.parameters;
  if (cfg.version!=1 || (cfg.mode!=Mode::LegacyExact && cfg.mode!=Mode::StreamingDiagnostic && cfg.mode!=Mode::AutoConservative) ||
      !viewValid(i.observation) || !viewValid(i.publishBounds) || !viewValid(clipped(i.observation,i.publishBounds)) ||
      i.candidateId.empty() || i.candidateId.size()>4096 || i.energy || i.spectrum || !i.fallbackActivity.empty() ||
      !std::isfinite(i.sampleRateHz) || i.sampleRateHz<=0 || !finite(i.windowCoverage,0,1) ||
      !std::isfinite(c.relativeCenterHz) || !std::isfinite(c.bandwidthHz) || c.bandwidthHz<=0 || c.bandwidthHz>i.sampleRateHz ||
      c.searchSamples.begin>=c.searchSamples.end || c.searchSamples.end>c.availableSamples ||
      c.searchSamples.end-c.searchSamples.begin>(std::uint64_t{1}<<34) ||
      p.timeStrategy!=DetectionTimeStrategy::Sample || !finite(p.smoothSeconds,0,60) ||
      !finite(p.minOnSeconds,0,60) || !finite(p.minOffSeconds,0,60) || !finite(p.sampleEdgeRatio,0,1) ||
      !finite(o.sampleNoiseQuantile,0,1) || !finite(o.sampleBurstQuantile,0,1) ||
      !finite(o.sampleMinimumOnFraction,0,1) || !finite(o.sampleMinimumRunSeconds,0,60) ||
      !std::isfinite(o.sampleMinimumBurstNoiseRatio) || o.sampleMinimumBurstNoiseRatio<1 ||
      !o.workingCellBudget || o.workingCellBudget>256*1024*1024ULL || o.maximumActivitySegments>annotationSegmentBudget ||
      !cfg.outputBlockSamples || cfg.outputBlockSamples>131072 ||
      cfg.maximumFftSamples<2 || cfg.maximumFftSamples>(1U<<20) ||
      (cfg.maximumFftSamples&(cfg.maximumFftSamples-1)) || cfg.maximumFirRadius>262143 ||
      !std::isfinite(cfg.firTailAmplitudeBudget) || cfg.firTailAmplitudeBudget<=0 || cfg.firTailAmplitudeBudget>8e-5 ||
      rt.memoryGrantBytes>1024*MiB || !rt.memoryGrantBytes || rt.resultByteGrant>rt.memoryGrantBytes ||
      rt.readerResidentReserveBytes>rt.memoryGrantBytes ||
      !rt.sourceReadBlockSamples || rt.sourceReadBlockSamples>131072)
    return fail(f,ErrorCode::InvalidInput,"plan","Invalid sample context/configuration/budgets");
  return true;
}
bool charge(const Runtime& rt, std::uint64_t count, Failure& f) {
  if (!rt.work.consumedCells || rt.work.limit>(std::uint64_t{1}<<34))
    return fail(f,ErrorCode::Integration,"work","Existing cumulative work ledger required");
  auto& used=*rt.work.consumedCells;
  if (used>rt.work.limit || count>rt.work.limit-used)
    return fail(f,ErrorCode::WorkLimit,"work","Time-refinement cumulative cell work limit exceeded");
  used+=count; return true;
}
bool readSource(const Context& c,const Runtime& rt,SampleIndex local,std::span<Complex> dst,
                Statistics& stats,Failure& f) {
  if (!cancelled(c,f,"source")) return false;
  if (local>c.searchSamples.end-c.searchSamples.begin || dst.size()>c.searchSamples.end-c.searchSamples.begin-local)
    return fail(f,ErrorCode::InvalidInput,"source","Physical sample callback outside frozen context");
  if (!charge(rt,dst.size(),f)) return false;
  QString why;
  const auto begin=c.searchSamples.begin+local;
  if (!rt.read({begin,begin+dst.size()},dst,why)) {
    if (!cancelled(c,f,"source")) return false;
    return fail(f,ErrorCode::SourceRead,"source",why);
  }
  stats.sourceSamplesRead+=dst.size();
  for (std::size_t j=0;j<dst.size();++j) {
    if (!(j%4096) && !cancelled(c,f,"source")) return false;
    if (!std::isfinite(dst[j].real()) || !std::isfinite(dst[j].imag()))
      return fail(f,ErrorCode::Numerical,"source","Nonfinite complex IQ");
    stats.maximumInputAmplitude=std::max(stats.maximumInputAmplitude,std::hypot(static_cast<double>(dst[j].real()),static_cast<double>(dst[j].imag())));
  }
  return true;
}
bool verify(const Context& c,const Runtime& rt,Failure& f) {
  if (!cancelled(c,f,"identity")) return false;
  QString why;
  if (!rt.verifySource || !rt.verifySource(why))
    return fail(f,ErrorCode::SourceChanged,"identity",why.isEmpty()?"Frozen source identity unavailable/changed":why);
  return true;
}
bool tap(const Context& c,const std::function<bool(SampleIndex,std::span<const float>,QString&)>& sink,
         SampleIndex local,std::span<const float> values,Failure& f) {
  if (!sink) return true;
  QString why;
  return sink(c.searchSamples.begin+local,values,why) || fail(f,ErrorCode::ScratchIo,"golden-tap",why);
}
void fallback(Result& out,const Context& c,const char* reason) {
  out.refinement={}; out.refinement.observation=clipped(c.input.observation,c.input.publishBounds);
  out.refinement.method=RefineMethod::Sample; out.refinement.status=RefineStatus::Fallback;
  out.refinement.fallbackReason=reason;
  out.refinement.evidence.numericalVersion="signalstudio-sample-stream-v1";
  out.refinement.evidence.energyMethod="cpu-periodic-fir-complex64-radix2-v2";
  out.refinement.evidence.sourceOriginSample=c.searchSamples.begin;
  out.refinement.evidence.sourceStepSamples=1;
  out.refinement.evidence.sourceSupportEndSample=c.searchSamples.end;
  out.route=Route::UnknownNeedsMeasuredCoarse; out.needsMeasuredCoarse=true;
  out.publicationEligible=false;
}
void evidence(const Context& c,const Plan& p,SampleIndex support,const std::string& method,Result& out) {
  auto& r=out.refinement; auto& e=r.evidence;
  r.observation=clipped(c.input.observation,c.input.publishBounds); r.method=RefineMethod::Sample;
  e.numericalVersion="signalstudio-sample-stream-v1"; e.energyMethod=method;
  e.sourceOriginSample=c.searchSamples.begin; e.sourceStepSamples=1; e.sourceSupportEndSample=c.searchSamples.end;
  e.filterSupportSamples=support; e.quantizationUncertaintySamples=1;
  e.continuousTrack=p.continuous; e.splittingApplied=p.split;
  e.smoothingSamples=p.smoothingSamples; e.minOnSamples=p.minOnSamples; e.minOffSamples=p.minOffSamples;
  e.fineParametersApplied=true; e.durationParametersApplied=c.parameters.splitBursts;
  e.effectiveEdgeRatio=c.parameters.sampleEdgeRatio*(p.continuous?.5/.6:1.);
  e.effectiveSmoothSeconds=c.parameters.smoothSeconds;
  e.effectiveMinOnSeconds=c.parameters.splitBursts?std::max(c.options.sampleMinimumRunSeconds,c.parameters.minOnSeconds):c.options.sampleMinimumRunSeconds;
  e.effectiveMinOffSeconds=c.parameters.splitBursts?c.parameters.minOffSeconds:0;
}
struct Trace {
  SampleIndex width=1,count=0,start=0;
  double sum=0;
  Trace(SampleIndex n):width(n/512+(n%512!=0)) {}
  void add(SampleIndex j,double value,const Context& c,Statistics& s) {
    if (!count) start=j;
    sum+=value; ++count;
    if (count==width || j+1==s.plan.count) {
      s.trace.push_back({c.searchSamples.begin+start,sum/static_cast<double>(count)});
      count=0; sum=0;
    }
  }
};
bool smoothEnergy(const Context& c,const Runtime& rt,detail::Scratch& files,Result& out,Failure& f) {
  const auto& p=out.statistics.plan;
  std::vector<float> current(p.outputBlockSamples),delayed(p.outputBlockSamples),values(p.outputBlockSamples);
  double running=0; Trace trace(p.count);
  for (SampleIndex first=0;first<p.count;) {
    const auto count=static_cast<std::size_t>(std::min<SampleIndex>(p.outputBlockSamples,p.count-first));
    if (!files.read(files.energy,first,std::span<float>(current).first(count),f)) return false;
    const auto oldFirst=std::max(first,p.smoothingSamples);
    const auto delayedCount=first+count>oldFirst?static_cast<std::size_t>(first+count-oldFirst):0;
    if (delayedCount && !files.read(files.energy,oldFirst-p.smoothingSamples,std::span<float>(delayed).first(delayedCount),f)) return false;
    std::size_t old=0;
    for (std::size_t j=0;j<count;++j) {
      if (!(j%4096) && !cancelled(c,f,"smooth")) return false;
      running+=current[j];
      if (first+j>=p.smoothingSamples) running-=delayed[old++];
      const double mean=running/static_cast<double>(p.smoothingSamples);
      values[j]=static_cast<float>(mean);
      if (!std::isfinite(values[j])) return fail(f,ErrorCode::Numerical,"smooth","Sample smoothing overflow");
      out.statistics.maximumSmoothedMagnitude=std::max(out.statistics.maximumSmoothedMagnitude,std::abs(static_cast<double>(values[j])));
      trace.add(first+j,mean,c,out.statistics);
    }
    auto span=std::span<const float>(values).first(count);
    if (!files.append(files.smooth,span,f) || !tap(c,rt.smoothedTap,first,span,f)) return false;
    first+=count;
  }
  return files.seal(files.smooth,f) && files.removeEnergy(f);
}
bool scanRuns(const Context& c,const Runtime& rt,detail::Scratch& files,SampleIndex support,
              const std::string& method,Result& out,Failure& f) {
  const auto& p=out.statistics.plan;
  evidence(c,p,support,method,out);
  auto& e=out.refinement.evidence;
  double noise=0,burst=0;
  if (!detail::exactQuantiles(files,c,p,noise,burst,f,rt.gpu)) return false;
  e.noise=noise; e.burst=burst;
  const auto markFallback=[&](const char* why) {
    out.refinement.activity.clear(); out.refinement.status=RefineStatus::Fallback;
    out.refinement.fallbackReason=why; out.needsMeasuredCoarse=true; return true;
  };
  if (burst<=noise*c.options.sampleMinimumBurstNoiseRatio) return markFallback("sample-burst-noise-ratio-too-low");
  e.threshold=noise+e.effectiveEdgeRatio*(burst-noise);
  e.comparisonThreshold=static_cast<float>(*e.threshold);
  if (!std::isfinite(*e.comparisonThreshold)) return fail(f,ErrorCode::Numerical,"runs","Threshold overflow");
  // The ORIGINAL on-fraction gate precedes run counting/result allocation.
  // Preserve that order even when it requires one more bounded S-file scan.
  std::vector<float> block(p.outputBlockSamples);
  for (SampleIndex first=0;first<p.count;) {
    const auto count=static_cast<std::size_t>(std::min<SampleIndex>(block.size(),p.count-first));
    if (!files.read(files.smooth,first,std::span<float>(block).first(count),f)) return false;
    for (std::size_t j=0;j<count;++j) {
      if (!(j%4096) && !cancelled(c,f,"on-fraction")) return false;
      out.statistics.onCount+=block[j]>*e.comparisonThreshold;
    }
    first+=count;
  }
  e.onFraction=static_cast<double>(out.statistics.onCount)/static_cast<double>(p.count);
  out.statistics.onCountMeasured=true;
  if (*e.onFraction<c.options.sampleMinimumOnFraction) return markFallback("sample-on-fraction-too-low");
  const auto perSegment=2*sizeof(Segment)+c.input.candidateId.size()+64;
  const auto capacity=std::min<std::uint64_t>({annotationSegmentBudget,c.options.maximumActivitySegments,rt.resultByteGrant/perSegment});
  out.refinement.activity.reserve(static_cast<std::size_t>(capacity));
  bool inOn=false,hasPending=false;
  out.statistics.rawRunCountMeasured=true;
  SampleIndex onBegin=0,pendingBegin=0,pendingEnd=0;
  const auto publishPending=[&]() {
    const ViewRange physical{{c.searchSamples.begin+pendingBegin,c.searchSamples.begin+pendingEnd},c.input.observation.frequency};
    const auto range=clipped(physical,c.input.publishBounds);
    if (!viewValid(range)) return true;
    if (out.refinement.activity.size()==capacity)
      return fail(f,ErrorCode::ResourceLimit,"runs","Activity result byte budget exceeded");
    Segment segment;
    segment.id=c.input.candidateId+"/activity/"+std::to_string(out.refinement.activity.size());
    segment.range=range; segment.timeQuality=TimeQuality::AlgorithmEstimated;
    segment.frequencyQuality=FrequencyQuality::ModelEstimated;
    segment.uncertaintySamples=saturatedAdd(support,p.smoothingSamples);
    segment.clippedAtRoi=range!=physical;
    out.refinement.activity.push_back(std::move(segment)); return true;
  };
  const auto closeRaw=[&](SampleIndex end) {
    if (end-onBegin<p.minOnSamples) return true;
    if (out.statistics.rawQualifiedRuns==annotationSegmentBudget)
      return fail(f,ErrorCode::Numerical,"runs","Activity-run budget exceeded");
    ++out.statistics.rawQualifiedRuns; // BEFORE gap merge/hull/ROI clipping
    if (!hasPending) {pendingBegin=onBegin;pendingEnd=end;hasPending=true;return true;}
    if (!p.split || onBegin-pendingEnd<p.minOffSamples) {pendingEnd=end;return true;}
    if (!publishPending()) return false;
    pendingBegin=onBegin;pendingEnd=end;return true;
  };
  for (SampleIndex first=0;first<p.count;) {
    const auto count=static_cast<std::size_t>(std::min<SampleIndex>(block.size(),p.count-first));
    if (!files.read(files.smooth,first,std::span<float>(block).first(count),f)) return false;
    for (std::size_t j=0;j<count;++j) {
      if (!(j%4096) && !cancelled(c,f,"runs")) return false;
      const bool on=block[j]>*e.comparisonThreshold;
      if (on && !inOn) {onBegin=first+j;inOn=true;}
      if (!on && inOn) {if (!closeRaw(first+j)) return false;inOn=false;}
    }
    first+=count;
  }
  if (inOn && !closeRaw(p.count)) return false;
  if (hasPending && !publishPending()) return false;
  if (out.refinement.activity.empty())
    return markFallback(out.statistics.rawQualifiedRuns?"sample-runs-outside-publish-bounds":"sample-no-sustained-on-run");
  out.refinement.status=RefineStatus::Applied; return true;
}
long double sinc(long double x) {
  if (std::abs(x)<1e-5L) {const auto t=x*x;return 1-t/6+t*t/120;}
  return std::sin(x)/x;
}
float coefficient(std::int64_t k,const Context& c) {
  const long double fs=c.input.sampleRateHz,cut=c.bandwidthHz/2,taper=std::max(c.bandwidthHz/4,1.);
  if (cut>=fs/2) return k==0?1.f:0.f;
  const long double d=std::min(taper,fs/2-cut),u=cut+d,a=std::numbers::pi_v<long double>/taper;
  const long double w=2*std::numbers::pi_v<long double>*k/fs;
  const auto j=[&](long double beta) {return d*sinc(beta*d/2)*std::cos(beta*(cut+d/2)-a*cut);};
  return static_cast<float>((cut*sinc(w*cut)+u*sinc(w*u)+.5L*(j(a-w)+j(a+w)))/fs);
}
bool periodicMixed(const Context& c,const Runtime& rt,std::int64_t local,
                   std::span<Complex> dst,Statistics& stats,Failure& f,bool mix=true) {
  const auto n=c.searchSamples.end-c.searchSamples.begin;
  std::size_t offset=0;
  while (offset<dst.size()) {
    auto mod=local%static_cast<std::int64_t>(n); if (mod<0) mod+=static_cast<std::int64_t>(n);
    const auto first=static_cast<SampleIndex>(mod);
    const auto count=static_cast<std::size_t>(std::min<SampleIndex>({n-first,dst.size()-offset,rt.sourceReadBlockSamples}));
    auto part=dst.subspan(offset,count);
    if (!readSource(c,rt,first,part,stats,f)) return false;
    for (std::size_t j=0;mix && j<count;++j) {
      if (!(j%4096) && !cancelled(c,f,"mix")) return false;
      double phase=std::fmod(-2*std::numbers::pi*c.relativeCenterHz*static_cast<double>(first+j)/c.input.sampleRateHz,2*std::numbers::pi);
      if (phase<0) phase+=2*std::numbers::pi;
      const double rounded=static_cast<float>(phase);
      part[j]*=Complex{static_cast<float>(std::cos(rounded)),static_cast<float>(std::sin(rounded))};
    }
    local+=count;offset+=count;
  }
  return true;
}
bool produceFir(const Context& c,const Runtime& rt,detail::Scratch& files,Result& out,Failure& f) {
  const auto& p=out.statistics.plan;
  std::vector<Complex> h(p.fftSamples),work(p.fftSamples),input(2*p.radius+p.outputBlockSamples);
  std::vector<float> power(p.outputBlockSamples);
  for (std::uint32_t i=0;i<p.taps;++i) {
    if (!(i%4096) && !cancelled(c,f,"kernel")) return false;
    h[i]=coefficient(static_cast<std::int64_t>(i)-p.radius,c);
    if (!std::isfinite(h[i].real())) return fail(f,ErrorCode::Numerical,"kernel","Nonfinite FIR coefficient");
  }
  QString why;
  gpu_dsp::FirFilter gpuFilter;
  gpu_dsp::Error gpuError;
  if (rt.gpu) {
    std::vector<float> coefficients(p.fftSamples);
    for (std::size_t i=0;i<h.size();++i) coefficients[i]=h[i].real();
    if (!rt.gpu->prepareFir(coefficients,p.radius,gpuFilter,gpuError))
      return cancelled(c,f,"gpu-fir-prepare") && fail(f,ErrorCode::Integration,"gpu-fir-prepare",
          QString::fromStdString(gpuError.stage+": "+gpuError.detail));
  } else if (!detectionBoundedRadixTwoTransform(h,false,rt.memoryGrantBytes,why,c.options.cancelled))
    return cancelled(c,f,"kernel-fft") && fail(f,ErrorCode::Numerical,"kernel-fft",why);
  SampleIndex first=0;std::size_t previousCount=0;
  while (first<p.count) {
    const auto count=static_cast<std::size_t>(std::min<SampleIndex>(p.outputBlockSamples,p.count-first));
    if (!first) {
      if (!periodicMixed(c,rt,-static_cast<std::int64_t>(p.radius),std::span<Complex>(input).first(count+2*p.radius),out.statistics,f,!rt.gpu)) return false;
    } else {
      for (std::size_t j=0;j<2*p.radius;++j) input[j]=input[previousCount+j];
      if (!periodicMixed(c,rt,static_cast<std::int64_t>(first+p.radius),
                         std::span<Complex>(input).subspan(2*p.radius,count),out.statistics,f,!rt.gpu)) return false;
    }
    if (rt.gpu) {
      gpu_dsp::PeriodicFirInput blockInput;
      blockInput.raw=std::span<const Complex>(input).first(count+2*p.radius);
      blockInput.localStart=static_cast<std::int64_t>(first)-p.radius;
      blockInput.contextN=p.count;blockInput.sampleRateHz=c.input.sampleRateHz;
      blockInput.relativeCenterHz=c.relativeCenterHz;
      blockInput.count=static_cast<std::uint32_t>(count);
      auto iqTap=rt.filteredIqTap?std::span<Complex>(work).first(count):std::span<Complex>{};
      if (!rt.gpu->periodicFirBlock(gpuFilter,blockInput,std::span<float>(power).first(count),gpuError,iqTap))
        return cancelled(c,f,"gpu-fir-block") && fail(f,ErrorCode::Integration,"gpu-fir-block",
            QString::fromStdString(gpuError.stage+": "+gpuError.detail));
      if (rt.filteredIqTap && !rt.filteredIqTap(c.searchSamples.begin+first,iqTap,why))
        return fail(f,ErrorCode::ScratchIo,"filtered-iq-tap",why);
      ++out.statistics.gpuFirBlocks;
    } else {
      std::fill(work.begin(),work.end(),Complex{});
      std::copy_n(input.begin(),count+2*p.radius,work.begin());
      if (!detectionBoundedRadixTwoTransform(work,false,rt.memoryGrantBytes,why,c.options.cancelled))
        return cancelled(c,f,"fir-fft") && fail(f,ErrorCode::Numerical,"fir-fft",why);
      for (std::size_t j=0;j<work.size();++j) {
        if (!(j%4096) && !cancelled(c,f,"fir-multiply")) return false;
        work[j]*=h[j];
      }
      if (!detectionBoundedRadixTwoTransform(work,true,rt.memoryGrantBytes,why,c.options.cancelled))
        return cancelled(c,f,"fir-ifft") && fail(f,ErrorCode::Numerical,"fir-ifft",why);
      if (rt.filteredIqTap && !rt.filteredIqTap(c.searchSamples.begin+first,
          std::span<const Complex>(work).subspan(2*p.radius,count),why))
        return fail(f,ErrorCode::ScratchIo,"filtered-iq-tap",why);
      for (std::size_t j=0;j<count;++j) {
        if (!(j%4096) && !cancelled(c,f,"energy")) return false;
        power[j]=std::norm(work[2*p.radius+j]);
      }
    }
    for (std::size_t j=0;j<count;++j) {
      if (!std::isfinite(power[j])) return fail(f,ErrorCode::Numerical,"energy","Nonfinite FIR power");
      out.statistics.maximumEnergy=std::max(out.statistics.maximumEnergy,static_cast<double>(power[j]));
    }
    const auto block=std::span<const float>(power).first(count);
    if (!files.append(files.energy,block,f) || !tap(c,rt.energyTap,first,block,f)) return false;
    previousCount=count;first+=count;
  }
  return files.seal(files.energy,f);
}
bool precisionGuard(const Context& c,detail::Scratch& files,Result& out,Failure& f) {
  const auto& p=out.statistics.plan;auto& s=out.statistics;const auto& e=out.refinement.evidence;
  out.precisionGuardEvaluated=true;
  const auto reject=[&](const char* why) {out.precisionReason=why;out.precisionGuardPassed=false;return true;};
  const auto accept=[&](const char* why) {out.precisionReason=why;out.precisionGuardPassed=true;return true;};
  const auto ratio=c.bandwidthHz/c.input.sampleRateHz;
  if (ratio<.002 || c.bandwidthHz<4 || c.input.sampleRateHz>1e9)
    return reject("Outside explicit empirical FIR scope: B/Fs>=.002, B>=4Hz, Fs<=1GHz");
  if (!e.noise || !e.burst) return reject("Missing exact global quantiles");
  if (s.maximumInputAmplitude==0) return accept("Complete IQ context is exactly zero; original ratio fallback");
  // Engineering sensitivity envelope fixed BEFORE Golden observations. This is
  // NOT a proved native FFT bound, and does not alter any original threshold.
  constexpr double u32=0x1p-24,u64=0x1p-53;
  constexpr double g3=3*u32/(1-3*u32);
  const double delta=2e-4*s.maximumInputAmplitude;
  const double y=std::sqrt(s.maximumEnergy/(1-g3));
  const double powerError=2*y*delta+delta*delta+g3*(y*y+(y+delta)*(y+delta));
  const double gamma=2*static_cast<double>(p.count)*u64/(1-2*static_cast<double>(p.count)*u64);
  const double rolling=gamma*(static_cast<double>(p.smoothingSamples)+2)/p.smoothingSamples*
      (s.maximumEnergy+(y+delta)*(y+delta)*(1+g3));
  const double err=powerError+rolling+8*u32*(s.maximumSmoothedMagnitude+powerError+rolling);
  s.empiricalSmoothAllowance=err;
  if (!std::isfinite(err)) return reject("Empirical sensitivity allowance overflow");
  const double qerr=err+8*u32*(s.maximumSmoothedMagnitude+err);
  const double n=*e.noise,b=*e.burst,r=c.options.sampleMinimumBurstNoiseRatio;
  const double lowGate=b-qerr-r*(n+qerr),highGate=b+qerr-r*(n-qerr);
  if (highGate<=0) return accept("Original ratio fallback stable under empirical sensitivity envelope");
  if (lowGate<=0) return reject("Global burst/noise ratio is precision-sensitive");
  if (!e.threshold || !e.comparisonThreshold) return reject("Threshold not available after stable ratio gate");
  const float lo=std::nextafter(static_cast<float>(*e.threshold-qerr),-std::numeric_limits<float>::infinity());
  const float hi=std::nextafter(static_cast<float>(*e.threshold+qerr),std::numeric_limits<float>::infinity());
  if (std::abs(*e.comparisonThreshold)<=2*qerr) return reject("Threshold comparable to FIR/float energy allowance");
  struct BoundsRuns {
    SampleIndex begin=0,pendingBegin=0,pendingEnd=0;bool on=false,pending=false,overflow=false;
    std::uint64_t count=0;std::vector<TimeRange> runs;
    void appendPending() {runs.push_back({pendingBegin,pendingEnd});}
    void close(SampleIndex end,const Plan& p) {
      if (end-begin<p.minOnSamples) return;
      if (++count>annotationSegmentBudget) {overflow=true;return;}
      if (!pending) {pending=true;pendingBegin=begin;pendingEnd=end;return;}
      if (!p.split || begin-pendingEnd<p.minOffSamples) {pendingEnd=end;return;}
      appendPending();pendingBegin=begin;pendingEnd=end;
    }
    void push(bool flag,SampleIndex i,const Plan& p) {
      if (flag&&!on) {on=true;begin=i;}
      if (!flag&&on) {close(i,p);on=false;}
    }
    void finish(const Plan& p) {if(on)close(p.count,p);if(pending&&!overflow)appendPending();}
  } lower,upper;
  const auto runCap=static_cast<std::size_t>(std::min<SampleIndex>(annotationSegmentBudget,p.count/p.minOnSamples+1));
  lower.runs.reserve(runCap);upper.runs.reserve(runCap);
  std::uint64_t mustCount=0,mayCount=0;
  std::vector<float> block(p.outputBlockSamples);
  for (SampleIndex first=0;first<p.count;) {
    const auto count=static_cast<std::size_t>(std::min<SampleIndex>(block.size(),p.count-first));
    if (!files.read(files.smooth,first,std::span<float>(block).first(count),f)) return false;
    for (std::size_t j=0;j<count;++j) {
      if (!(j%4096) && !cancelled(c,f,"precision-guard")) return false;
      const bool must=static_cast<double>(block[j])-err>hi,may=static_cast<double>(block[j])+err>lo;
      mustCount+=must;mayCount+=may;s.uncertainComparisonCells+=may&&!must;
      lower.push(must,first+j,p);upper.push(may,first+j,p);
      if (lower.overflow||upper.overflow) return reject("Pre-merge run-budget sensitivity");
    }
    first+=count;
  }
  lower.finish(p);upper.finish(p);
  if (lower.overflow||upper.overflow || mayCount/p.minOnSamples>annotationSegmentBudget)
    return reject("Pre-merge run budget not established for uncertain masks");
  const double minimum=c.options.sampleMinimumOnFraction;
  if (static_cast<double>(mayCount)/p.count<minimum) return accept("Original on-fraction fallback stable");
  if (static_cast<double>(mustCount)/p.count<minimum) return reject("ON-fraction gate is precision-sensitive");
  if (lower.runs.size()!=upper.runs.size()) return reject("Possible activity component count differs");
  const auto tolerance=std::max<SampleIndex>(2,static_cast<SampleIndex>(std::floor(c.input.sampleRateHz*std::min(.00005,.05*c.parameters.smoothSeconds))));
  for (std::size_t k=0;k<lower.runs.size();++k) {
    const auto l=lower.runs[k],u=upper.runs[k];
    if (!(u.begin<=l.begin && l.end<=u.end) || l.begin-u.begin>tolerance || u.end-l.end>tolerance ||
        (p.split && (l.begin-u.begin>=p.minOnSamples || u.end-l.end>=p.minOnSamples)))
      return reject("Activity boundary/topology sensitivity exceeds unchanged tolerance");
    const auto lclip=clipped({{c.searchSamples.begin+l.begin,c.searchSamples.begin+l.end},c.input.observation.frequency},c.input.publishBounds);
    const auto uclip=clipped({{c.searchSamples.begin+u.begin,c.searchSamples.begin+u.end},c.input.observation.frequency},c.input.publishBounds);
    if (viewValid(lclip)!=viewValid(uclip)) return reject("ROI visibility is precision-sensitive");
  }
  return accept("Empirical scoped gate/topology checks passed; no universal native error theorem");
}
bool stream(const Context& c,const Config& cfg,const Runtime& rt,const Plan& p,
            const EnergySource* external,Result& out,Failure& f) {
  out.statistics.plan=p; detail::Scratch files(rt,out.statistics);
  const auto body=[&]() {
    if (!files.open(c,cfg,p,f)) return false;
    if (external) {
      std::vector<float> values(p.outputBlockSamples);
      for (SampleIndex first=0;first<p.count;) {
        const auto count=static_cast<std::size_t>(std::min<SampleIndex>(values.size(),p.count-first));
        auto block=std::span<float>(values).first(count);QString why;
        if (!cancelled(c,f,"external-energy") || !charge(rt,count,f)) return false;
        if (!external->read(c.searchSamples.begin+first,block,why))
          return cancelled(c,f,"external-energy") && fail(f,ErrorCode::SourceRead,"external-energy",why);
        for (std::size_t j=0;j<block.size();++j) {
          if (!(j%4096) && !cancelled(c,f,"external-energy")) return false;
          if (!std::isfinite(block[j]) || block[j]<0)
            return fail(f,ErrorCode::Numerical,"external-energy","Energy must be finite nonnegative linear power");
        }
        out.statistics.externalEnergyCellsRead+=count;
        if (!files.append(files.energy,block,f) || !tap(c,rt.energyTap,first,block,f)) return false;
        first+=count;
      }
      if (!files.seal(files.energy,f)) return false;
    } else if (!produceFir(c,rt,files,out,f)) return false;
    if (!smoothEnergy(c,rt,files,out,f)) return false;
    if (!scanRuns(c,rt,files,external?external->filterSupportSamples:p.taps,
                    external?external->method:rt.gpu?gpu_dsp::kFirMethod:"cpu-periodic-fir-complex64-radix2-v2",out,f)) return false;
    return external || precisionGuard(c,files,out,f);
  };
  bool ok=false;
  try {ok=body();} catch (const std::bad_alloc&) {fail(f,ErrorCode::ResourceLimit,"allocation","Bounded sample allocation failed");}
  const bool clean=files.close(f);
  if (!ok || !clean) return false;
  out.route=out.precisionGuardPassed?Route::EmpiricalFir:Route::UnverifiedFirDiagnostic;
  out.publicationEligible=!external && out.precisionGuardPassed && out.refinement.status==RefineStatus::Applied;
  out.needsMeasuredCoarse=out.needsMeasuredCoarse || (!external && !out.precisionGuardPassed);
  out.autoReason=out.publicationEligible?"Scoped empirical FIR accepted; native theorem not claimed":"FIR requires precision fallback or measured Coarse/Unknown";
  return true;
}
bool legacy(const Context& c,const Runtime& rt,const Plan& p,Result& out,Failure& f) {
  if (!p.legacyEligible) return fail(f,ErrorCode::ResourceLimit,"legacy-plan","LegacyExact working budget unavailable");
  out.statistics.plan=p;
  std::vector<Complex> iq(static_cast<std::size_t>(p.count));
  for (SampleIndex first=0;first<p.count;) {
    const auto count=static_cast<std::size_t>(std::min<SampleIndex>(rt.sourceReadBlockSamples,p.count-first));
    if (!readSource(c,rt,first,std::span<Complex>(iq).subspan(static_cast<std::size_t>(first),count),out.statistics,f)) return false;
    first+=count;
  }
  std::vector<float> power;QString why;
  DetectionFilteredSampleObserver observe;
  if (rt.filteredIqTap) observe=[&](std::uint64_t first,std::span<const Complex> values,QString& error) {
    for (std::size_t offset=0;offset<values.size();) {
      const auto count=std::min<std::size_t>(p.outputBlockSamples,values.size()-offset);
      if (!rt.filteredIqTap(c.searchSamples.begin+first+offset,values.subspan(offset,count),error)) return false;
      offset+=count;
    }
    return true;
  };
  if (!detectionBandEnergy(std::move(iq),c.input.sampleRateHz,c.relativeCenterHz,c.bandwidthHz,
                           power,rt.memoryGrantBytes,why,c.options.cancelled,observe))
    return cancelled(c,f,"legacy-energy") && fail(f,ErrorCode::Numerical,"legacy-energy",why);
  for (const auto value:power) out.statistics.maximumEnergy=std::max(out.statistics.maximumEnergy,static_cast<double>(value));
  // Trace/taps reproduce the ORIGINAL rolling order; no FIR statistics reused.
  std::vector<float> smoothed(p.outputBlockSamples);Trace trace(p.count);double running=0;
  for (SampleIndex first=0;first<p.count;) {
    const auto count=static_cast<std::size_t>(std::min<SampleIndex>(smoothed.size(),p.count-first));
    if (!tap(c,rt.energyTap,first,std::span<const float>(power).subspan(static_cast<std::size_t>(first),count),f)) return false;
    for (std::size_t j=0;j<count;++j) {
      if (!(j%4096) && !cancelled(c,f,"legacy-smooth")) return false;
      const auto i=first+j; running+=power[static_cast<std::size_t>(i)];
      if (i>=p.smoothingSamples) running-=power[static_cast<std::size_t>(i-p.smoothingSamples)];
      const double mean=running/static_cast<double>(p.smoothingSamples);
      smoothed[j]=static_cast<float>(mean);trace.add(i,mean,c,out.statistics);
      out.statistics.maximumSmoothedMagnitude=std::max(out.statistics.maximumSmoothedMagnitude,std::abs(static_cast<double>(smoothed[j])));
    }
    if (!tap(c,rt.smoothedTap,first,std::span<const float>(smoothed).first(count),f)) return false;
    first+=count;
  }
  auto input=c.input;
  input.energy=DetectionEnergySeries{c.searchSamples.begin,1,c.searchSamples.end,p.count,
                                    std::move(power),"cpu-exact-length-complex64-raised-cosine-fft-v1"};
  auto legacyOptions=c.options;
  const auto perSegment=2*sizeof(Segment)+c.input.candidateId.size()+64;
  legacyOptions.maximumActivitySegments=static_cast<std::size_t>(std::min<std::uint64_t>(
      legacyOptions.maximumActivitySegments,rt.resultByteGrant/perSegment));
  auto result=refineDetection(input,c.parameters,legacyOptions);
  if (!result.ok) {
    if (!cancelled(c,f,"legacy-domain")) return false;
    return fail(f,result.error=="Activity output exceeds working budget"?ErrorCode::ResourceLimit:ErrorCode::Numerical,
        "legacy-domain",QString::fromStdString(result.error));
  }
  if (result.activity.size()>rt.resultByteGrant/perSegment)
    return fail(f,ErrorCode::ResourceLimit,"legacy-output","Legacy result exceeds reserved byte grant");
  out.refinement=std::move(result);out.route=Route::LegacyExact;
  out.needsMeasuredCoarse=out.refinement.status==RefineStatus::Fallback;
  out.publicationEligible=out.refinement.status==RefineStatus::Applied;
  return true;
}
} // namespace

bool makePlan(const Context& c,const Config& cfg,const Runtime& rt,Plan& output,Failure& f) {
  f={}; if (!contextValid(c,cfg,rt,f)) return false;
  Plan p;p.count=c.searchSamples.end-c.searchSamples.begin;
  p.continuous=c.input.windowCoverage>=.7;p.split=c.parameters.splitBursts&&!p.continuous;
  const double minOn=c.parameters.splitBursts?std::max(c.options.sampleMinimumRunSeconds,c.parameters.minOnSeconds):c.options.sampleMinimumRunSeconds;
  if (!duration(c.parameters.smoothSeconds,c.input.sampleRateHz,1,p.smoothingSamples) ||
      !duration(minOn,c.input.sampleRateHz,2,p.minOnSamples) ||
      !duration(c.parameters.splitBursts?c.parameters.minOffSeconds:0,c.input.sampleRateHz,2,p.minOffSamples))
    return fail(f,ErrorCode::InvalidInput,"plan","Sample duration conversion overflow");
  if (!c.parameters.splitBursts) p.minOffSamples=0;
  p.outputBlockSamples=static_cast<std::uint32_t>(std::min<std::uint64_t>({cfg.outputBlockSamples,p.count,c.options.workingCellBudget}));
  const auto leaf=largestLeaf(p.count);
  std::uint64_t convolution=0;
  if (leaf>13) {convolution=1;while(convolution<2*leaf-1) convolution*=2;}
  const long double fixed=static_cast<long double>(rt.readerResidentReserveBytes)+rt.resultByteGrant+4*MiB;
  const long double fft=std::ceil(p.count*8.125L)+16.L*convolution+65536;
  // Decode buffers are gone before FFT/refinement; percentile scratch is gone
  // before activity vectors are built. Charge the largest LIVE phase instead
  // of summing these mutually exclusive lifetimes into an artificial refusal.
  const long double traceBytes=4.L*p.outputBlockSamples+512*sizeof(TracePoint);
  const long double exact=std::max({8.L*p.count+rt.readerResidentReserveBytes,fft,
      12.L*p.count+traceBytes,
      8.125L*p.count+traceBytes+2.L*rt.resultByteGrant+32.L*annotationSegmentBudget})+2*MiB;
  p.legacyResidentEstimateBytes=static_cast<std::uint64_t>(exact);
  const auto worstSegments=p.split?std::min<SampleIndex>(annotationSegmentBudget,p.count/(p.minOnSamples+1)+1):1;
  const auto segmentBytes=2*sizeof(Segment)+c.input.candidateId.size()+64;
  p.legacyEligible=exact<=rt.memoryGrantBytes && p.count<=c.options.workingCellBudget &&
                    p.count<=std::numeric_limits<std::size_t>::max() && worstSegments<=rt.resultByteGrant/segmentBytes;
  if (cfg.mode==Mode::LegacyExact) {p.estimatedResidentBytes=p.legacyResidentEstimateBytes;output=p;return true;}
  const long double fs=c.input.sampleRateHz,taper=std::max(c.bandwidthHz/4,1.);
  if (c.bandwidthHz<c.input.sampleRateHz) {
    const bool clipped=c.bandwidthHz/2+taper>fs/2;
    const long double raw=clipped?fs/(std::numbers::pi_v<long double>*taper*cfg.firTailAmplitudeBudget):
        std::max(fs/taper,fs/taper/std::sqrt(3*std::numbers::pi_v<long double>*cfg.firTailAmplitudeBudget));
    if (!std::isfinite(raw) || std::ceil(raw)>cfg.maximumFirRadius)
      return fail(f,ErrorCode::ResourceLimit,"fir-plan","FIR radius required by tail budget exceeds cap");
    p.radius=static_cast<std::uint32_t>(std::ceil(raw));
    p.tailBound=static_cast<double>(clipped?fs/(std::numbers::pi_v<long double>*taper*p.radius):
        fs*fs/(3*std::numbers::pi_v<long double>*taper*taper*p.radius*p.radius));
  }
  p.taps=2*p.radius+1;
  if (p.taps>cfg.maximumFftSamples)
    return fail(f,ErrorCode::ResourceLimit,"fir-plan","FIR kernel exceeds fixed FFT cap");
  p.outputBlockSamples=std::min(p.outputBlockSamples,cfg.maximumFftSamples-p.taps+1);
  p.fftSamples=1;while(p.fftSamples<p.outputBlockSamples+2*p.radius) p.fftSamples*=2;
  p.scratchReservationBytes=8*p.count+8192;
  const long double resident=32.L*p.fftSamples+16.L*(2*p.radius+p.outputBlockSamples)+
      16.L*p.outputBlockSamples+fixed;
  p.estimatedResidentBytes=static_cast<std::uint64_t>(resident);
  if (resident>rt.memoryGrantBytes || p.scratchReservationBytes>cfg.scratchBudgetBytes)
    return fail(f,ErrorCode::ResourceLimit,"fir-plan","Fixed Sample workspace or scratch quota exceeded");
  output=p;return true;
}

bool run(const Context& c,const Config& cfg,const Runtime& rt,Result& output,Failure& f) {
  f={};
  if (!rt.read || !rt.verifySource || !rt.work.consumedCells || !rt.work.limit || rt.work.limit>(std::uint64_t{1}<<34))
    return fail(f,ErrorCode::Integration,"entry","IQ/identity/work callbacks required");
  try {
    Plan p;bool planned=makePlan(c,cfg,rt,p,f);
    if (!planned && (cfg.mode!=Mode::AutoConservative || f.code!=ErrorCode::ResourceLimit)) return false;
    if (!verify(c,rt,f)) return false;
    Result next;
    if (cfg.mode==Mode::LegacyExact) {
      if (!legacy(c,rt,p,next,f)) return false;
    } else {
      bool streamed=false;
      if (planned) {
        streamed=stream(c,cfg,rt,p,nullptr,next,f);
        if (!streamed && (cfg.mode!=Mode::AutoConservative || f.code!=ErrorCode::ResourceLimit || !f.cleanupFailure.isEmpty())) return false;
      }
      if (cfg.mode==Mode::AutoConservative && (!streamed || !next.precisionGuardPassed)) {
        const auto precisionReason=next.precisionReason.isEmpty()?f.detail:next.precisionReason;
        const bool assessed=next.precisionGuardEvaluated;
        Statistics history=std::move(next.statistics);
        const auto proposals=next.refinement.activity.size();
        next=Result{}; // release ALL FIR proposals before resident Legacy retry
        Config exactCfg=cfg;exactCfg.mode=Mode::LegacyExact;
        Plan exactPlan;f={};if (!makePlan(c,exactCfg,rt,exactPlan,f)) return false;
        if (exactPlan.legacyEligible) {
          if (!legacy(c,rt,exactPlan,next,f)) return false;
          next.autoReason="Native FIR error envelope unestablished: complete LegacyExact precision fallback";
        } else {
          next.statistics.plan=exactPlan;
          fallback(next,c,"sample-fir-precision-unestablished; legacy-working-budget-unavailable; measured-coarse-required");
          next.autoReason="Complete LegacyExact unavailable; retain observation and measure Coarse or record Unknown";
        }
        if (streamed) {
          next.statistics.diagnosticFirPlan=history.plan;next.statistics.diagnosticFirSegments=proposals;
          next.statistics.diagnosticFirTrace=std::move(history.trace);
        }
        next.precisionGuardEvaluated=assessed;next.precisionReason=precisionReason;
        next.statistics.sourceSamplesRead+=history.sourceSamplesRead;
        next.statistics.gpuFirBlocks+=history.gpuFirBlocks;
        next.statistics.scratchBytesRead+=history.scratchBytesRead;
        next.statistics.scratchBytesWritten+=history.scratchBytesWritten;
        next.statistics.scratchPeakBytes=std::max(next.statistics.scratchPeakBytes,history.scratchPeakBytes);
      }
    }
    if (!verify(c,rt,f)) return false;
    output=std::move(next);return true;
  } catch (const std::bad_alloc&) {return fail(f,ErrorCode::ResourceLimit,"allocation","Bounded Sample allocation failed");}
}

bool refineEnergyBlocks(const Context& c,const Config& cfg,const Runtime& rt,const EnergySource& source,
                        Result& output,Failure& f) {
  f={};
  if (!contextValid(c,cfg,rt,f)) return false;
  if (source.count!=c.searchSamples.end-c.searchSamples.begin || !source.count || !source.filterSupportSamples ||
      source.method.empty() || source.method.size()>256 || !source.read || !rt.verifySource || !rt.work.consumedCells)
    return fail(f,ErrorCode::InvalidInput,"external-energy","Invalid exact EnergySource");
  Config base=cfg;base.mode=Mode::LegacyExact;
  Plan p;if (!makePlan(c,base,rt,p,f)) return false;
  p.scratchReservationBytes=8*p.count+8192;
  p.estimatedResidentBytes=rt.readerResidentReserveBytes+rt.resultByteGrant+4*MiB+32ULL*p.outputBlockSamples;
  if (p.estimatedResidentBytes>rt.memoryGrantBytes || p.scratchReservationBytes>cfg.scratchBudgetBytes)
    return fail(f,ErrorCode::ResourceLimit,"external-energy","External Energy workspace/scratch budget exceeded");
  if (!verify(c,rt,f)) return false;
  Result next;
  if (!stream(c,cfg,rt,p,&source,next,f) || !verify(c,rt,f)) return false;
  output=std::move(next);return true;
}
} // namespace signalstudio::sample_stream
