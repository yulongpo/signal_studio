#include "infrastructure/narrowband_analysis/constellation_analysis.h"
#include "infrastructure/narrowband_analysis/eye_analysis.h"
#include "infrastructure/narrowband_analysis/symbol_rate_estimator.h"
#include "infrastructure/narrowband_analysis/single_carrier_recovery.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace signalstudio {
NarrowbandAnalysisResult analyzeNarrowband(std::shared_ptr<const AnalysisSourceData> source,
    const NarrowbandAnalysisSettings& settings,const AnalysisCancel& cancelled) {
  NarrowbandAnalysisResult result;result.source=source;
  if(!source)return result;
  result.identity=source->identity;result.status=source->status;result.message=source->message;
  if(source->status!=AnalysisStatus::Ready || source->samples.empty())return result;
  if(settings.pointCount<256 || settings.pointCount>20000) {result.status=AnalysisStatus::Failed;result.message="星座点数必须为 256–20000。";return result;}
  std::complex<double> mean{};
  for(std::size_t i=0;i<source->samples.size();++i) {
    if((i&4095U)==0 && cancelled && cancelled()) {result.status=AnalysisStatus::Cancelled;return result;}
    const auto sample=source->samples[i];
    if(!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) {result.status=AnalysisStatus::Failed;result.message="来源 IQ 包含非有限值。";return result;}
    mean+=std::complex<double>(sample);
  }
  mean/=static_cast<double>(source->samples.size());
  result.removedDc=settings.removeDc?mean:std::complex<double>{};
  double power=0;
  for(const auto sample:source->samples)power+=std::norm(std::complex<double>(sample)-result.removedDc);
  power/=static_cast<double>(source->samples.size());
  result.gain=settings.normalizePower && power>1e-24?1/std::sqrt(power):1;
  double minimum=std::numeric_limits<double>::infinity(),maximum=-minimum;
  for(const auto sample:source->samples) {
    const auto point=(std::complex<double>(sample)-result.removedDc)*result.gain;
    minimum=std::min({minimum,point.real(),point.imag()});maximum=std::max({maximum,point.real(),point.imag()});
  }
  const double margin=std::max((maximum-minimum)*.05,1e-9);
  result.axisMinimum=minimum-margin;result.axisMaximum=maximum+margin;
  const auto count=std::min<std::size_t>(static_cast<std::size_t>(settings.pointCount),source->samples.size());
  result.constellation.reserve(count);
  for(std::size_t i=0;i<count;++i) {
    const auto index=count>1?i*(source->samples.size()-1)/(count-1):0;
    TaggedSymbol point;point.value=static_cast<std::complex<float>>((std::complex<double>(source->samples[index])-result.removedDc)*result.gain);
    point.output={source->outputSamples.begin+index,0};
    if(!mapAnalysisPosition(*source,point.output,point.source)) {result.constellation.clear();result.status=AnalysisStatus::Failed;result.message="星座来源映射失败。";return result;}
    result.constellation.push_back(point);
  }
  result.status=AnalysisStatus::Ready;
  result.message="真实连续 IQ 散点 · 未执行符号／载波同步";
  auto effective=settings;
  if(settings.hypothesis==ModulationHypothesis::Fsk || settings.hypothesis==ModulationHypothesis::Ofdm || settings.hypothesis==ModulationHypothesis::Gmsk) {
    result.eyeStatus=AnalysisStatus::NotApplicable;result.eyeMessage="所选体制不适用本期单载波 PSK/QAM 眼图和同步流程；保留真实原始 IQ。";
    return result;
  }
  bool unreliable=false;std::string rateMessage;
  if(settings.symbolRateMode==SymbolRateMode::Estimated) {
    const auto estimate=estimateSymbolRate(source->samples,static_cast<double>(source->outputRateHz),cancelled);
    result.rateCandidates=estimate.candidates;rateMessage=estimate.message;
    if(estimate.reliable)effective.symbolRateHz=estimate.candidates.front().rateHz;
    else {effective.symbolRateMode=SymbolRateMode::Unknown;unreliable=true;}
  }
  recoverSingleCarrier(result,effective,cancelled);
  buildEye(result,effective,cancelled);
  if(unreliable){result.eyeStatus=AnalysisStatus::LowConfidence;result.eyeMessage=rateMessage;}
  if(!rateMessage.empty())result.message+=" · "+rateMessage;
  return result;
}
}  // namespace signalstudio
