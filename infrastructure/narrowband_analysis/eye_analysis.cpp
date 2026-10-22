#include "infrastructure/narrowband_analysis/eye_analysis.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace signalstudio {
void buildEye(NarrowbandAnalysisResult& result,const NarrowbandAnalysisSettings& settings,const AnalysisCancel& cancelled) {
  result.eyes.clear();result.eyePeriods=settings.eyePeriods;result.eyePointsPerUi=settings.eyePointsPerUi;
  if(!result.source || result.source->status!=AnalysisStatus::Ready)return;
  if(settings.symbolRateMode==SymbolRateMode::Unknown || !(settings.symbolRateHz>0)) {
    result.eyeStatus=AnalysisStatus::NeedSymbolRate;result.eyeMessage="需要输入或估计符号率；旧数值尚未确认。";return;
  }
  const auto& source=*result.source;
  const double sps=source.outputRateHz/settings.symbolRateHz;
  result.symbolRateHz=settings.symbolRateHz;result.samplesPerSymbol=sps;
  if(!std::isfinite(sps) || sps<1 || settings.eyePeriods<1 || settings.eyePeriods>8 ||
     settings.eyeTraces<8 || settings.eyeTraces>256 || settings.eyeComponent<0 || settings.eyeComponent>2 ||
     (settings.eyePointsPerUi!=4 && settings.eyePointsPerUi!=8 && settings.eyePointsPerUi!=16) ||
     !std::isfinite(settings.eyeTimingOffsetUi) || std::abs(settings.eyeTimingOffsetUi)>2) {
    result.eyeStatus=AnalysisStatus::NotApplicable;result.eyeMessage="眼图配置无效或实际 Fs 不足以表示符号率。";return;
  }
  std::span<const std::complex<float>> signal=source.samples;
  std::vector<std::complex<float>> filtered,preprocessed;std::size_t discarded=0;
  bool preprocessingApplied=false;
  if(settings.eyeStage==AnalysisStage::MatchedFiltered) {
    if(settings.removeDc || settings.normalizePower) {
      preprocessed.reserve(signal.size());
      for(const auto sample:signal)preprocessed.push_back(static_cast<std::complex<float>>((std::complex<double>(sample)-result.removedDc)*result.gain));
      signal=preprocessed;preprocessingApplied=true;
    }
    std::string error;
    if(!matchedRrc(signal,sps,settings.rolloff,settings.filterSpanSymbols,filtered,discarded,error,cancelled)) {
      result.eyeStatus=cancelled && cancelled()?AnalysisStatus::Cancelled:AnalysisStatus::InsufficientSamples;result.eyeMessage=error;return;
    }
    signal=filtered;result.filterDiscardedSamples=discarded*2;
  }
  const double start=16+std::fmod(settings.eyeTimingOffsetUi+2,1.0)*sps;
  const double available=static_cast<double>(signal.size())-17-start-settings.eyePeriods*sps;
  if(available<0) {result.eyeStatus=AnalysisStatus::InsufficientSamples;result.eyeMessage="真实有效波形不足以覆盖眼图周期和插值支持。";return;}
  const auto lastSymbol=static_cast<std::size_t>(std::floor(available/sps));
  const auto count=std::min<std::size_t>(static_cast<std::size_t>(settings.eyeTraces),lastSymbol+1);
  const int points=settings.eyePeriods*settings.eyePointsPerUi+1;
  double minimum=std::numeric_limits<double>::infinity(),maximum=-minimum;
  for(std::size_t j=0;j<count;++j) {
    if(cancelled && cancelled()) {result.eyes.clear();result.eyeStatus=AnalysisStatus::Cancelled;return;}
    const auto symbol=count>1?j*lastSymbol/(count-1):0;
    const double first=start+symbol*sps;
    EyeTrace trace;trace.i.reserve(points);trace.q.reserve(points);trace.outputPositions.reserve(points);
    for(int p=0;p<points;++p) {
      const double position=first+static_cast<double>(p)/settings.eyePointsPerUi*sps;
      std::complex<double> value;
      if(!interpolateBandlimited(signal,position,value)) {result.eyes.clear();result.eyeStatus=AnalysisStatus::Failed;result.eyeMessage="眼图插值越过有效数据支持。";return;}
      if(!preprocessingApplied)value=(value-result.removedDc)*result.gain;
      trace.i.push_back(static_cast<float>(value.real()));trace.q.push_back(static_cast<float>(value.imag()));
      const auto integer=static_cast<SampleIndex>(std::floor(position));
      trace.outputPositions.push_back({source.outputSamples.begin+discarded+integer,position-static_cast<double>(integer)});
      if(settings.eyeComponent!=2){minimum=std::min(minimum,value.real());maximum=std::max(maximum,value.real());}
      if(settings.eyeComponent!=1){minimum=std::min(minimum,value.imag());maximum=std::max(maximum,value.imag());}
    }
    trace.outputStart=trace.outputPositions.front();mapAnalysisPosition(source,trace.outputStart,trace.sourceStart);
    result.eyes.push_back(std::move(trace));
  }
  const double margin=std::max((maximum-minimum)*.05,1e-9);
  result.eyeAxisMinimum=minimum-margin;result.eyeAxisMaximum=maximum+margin;
  result.eyeStatus=AnalysisStatus::Ready;
  result.eyeMessage=settings.eyeStage==AnalysisStage::MatchedFiltered?"真实 RRC 假设滤波眼图 · 周期假设，未锁定":"真实 DDC IQ 眼图 · 周期假设，未锁定";
}
}  // namespace signalstudio
