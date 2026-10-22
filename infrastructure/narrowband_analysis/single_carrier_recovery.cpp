#include "infrastructure/narrowband_analysis/single_carrier_recovery.h"
#include "infrastructure/narrowband_analysis/fft.h"
#include "infrastructure/narrowband_analysis/pulse_shaping.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>

namespace signalstudio {
namespace {
constexpr double pi=std::numbers::pi;
constexpr std::size_t acquisitionSymbols=512;
struct CfoEstimate {double hz=0,prominence=0;bool valid=false;};
struct Clock {std::vector<double> positions;std::vector<std::complex<double>> values;double jitter=0,period=0,coherence=0;bool locked=false;};
std::pair<double,double> loopGains(double bandwidth) {
  const double damping=std::sqrt(.5),theta=bandwidth/(damping+1/(4*damping));
  const double denominator=1+2*damping*theta+theta*theta;
  return {4*damping*theta/denominator,4*theta*theta/denominator};
}
CfoEstimate momentCfo(std::span<const std::complex<float>> signal,double fs,double rs,
                      ModulationHypothesis hypothesis,const AnalysisCancel& cancelled) {
  CfoEstimate result;
  const int order=hypothesis==ModulationHypothesis::Bpsk?2:4;
  const auto observed=std::min<std::size_t>(signal.size(),131072);
  if(observed<1024)return result;
  double energy=0;for(std::size_t i=0;i<observed;++i)energy+=std::norm(signal[i]);
  if(!(energy>1e-20))return result;
  const double scale=std::sqrt(energy/observed);
  std::size_t n=1;while(n<observed)n*=2;
  std::vector<std::complex<double>> moment(n);
  for(std::size_t i=0;i<observed;++i) {
    if((i&4095U)==0 && cancelled && cancelled())return result;
    const auto z=std::complex<double>(signal[i])/scale;
    const auto squared=z*z;
    moment[i]=(order==2?squared:squared*squared)*(.5-.5*std::cos(2*pi*i/(observed-1)));
  }
  narrowband_dsp::fft(moment);
  const double maximum=std::min(rs>0?rs*.10:fs/(order*2.5),fs/(2*order)*.9);
  std::vector<double> powers;double largest=-1;std::size_t peak=0;
  for(std::size_t i=0;i<n;++i) {
    const double frequency=(i<=n/2?static_cast<double>(i):static_cast<double>(i)-n)*fs/n;
    if(std::abs(frequency)>maximum*order)continue;
    const double power=std::norm(moment[i]);powers.push_back(power);
    if(power>largest){largest=power;peak=i;}
  }
  if(powers.empty())return result;
  auto middle=powers.begin()+static_cast<std::ptrdiff_t>(powers.size()/2);std::nth_element(powers.begin(),middle,powers.end());
  result.prominence=10*std::log10(std::max(largest,1e-30)/std::max(*middle,1e-30));
  const double a=std::log(std::max(std::norm(moment[(peak+n-1)%n]),1e-30)),b=std::log(std::max(largest,1e-30)),c=std::log(std::max(std::norm(moment[(peak+1)%n]),1e-30));
  const double correction=std::abs(a-2*b+c)>1e-12?std::clamp(.5*(a-c)/(a-2*b+c),-.5,.5):0;
  result.hz=((peak<=n/2?static_cast<double>(peak):static_cast<double>(peak)-n)+correction)*fs/n/order;
  result.valid=result.prominence>=12 && std::abs(result.hz)<=maximum;
  return result;
}
void rotateCfo(std::vector<std::complex<float>>& signal,SampleIndex origin,double fs,double hz,double phase,
                const AnalysisCancel& cancelled) {
  double turns=std::fmod(-hz/fs,1.0);if(turns<0)turns+=1;
  const double highScaled=std::ldexp(turns,32);
  const auto high=static_cast<std::uint64_t>(std::floor(highScaled));
  const auto low=static_cast<std::uint64_t>(std::floor(std::ldexp(highScaled-high,32)));
  const auto increment=(high<<32)|(low&0xffffffffULL);
  const auto oscillatorAt=[&](SampleIndex position) {
    const auto word=increment*position;
    const double angle=2*pi*(std::ldexp(static_cast<double>(word>>32),-32)+std::ldexp(static_cast<double>(static_cast<std::uint32_t>(word)),-64))-phase;
    return std::polar(1.0,angle);
  };
  auto oscillator=oscillatorAt(origin);const auto step=std::polar(1.0,-2*pi*hz/fs);
  for(std::size_t i=0;i<signal.size();++i) {
    if((i&1023U)==0){if(cancelled && cancelled())return;oscillator=oscillatorAt(origin+i);}
    signal[i]=static_cast<std::complex<float>>(std::complex<double>(signal[i])*oscillator);oscillator*=step;
  }
}
Clock synchronizeClock(std::span<const std::complex<float>> samples,double sps,double bandwidth,const AnalysisCancel& cancelled) {
  Clock clock;clock.period=sps;
  if(samples.size()<sps*(acquisitionSymbols+256)+64 || sps<2)return clock;
  double power=0;for(const auto z:samples)power+=std::norm(z);power/=samples.size();
  if(power<1e-20)return clock;
  std::complex<double> cyclic{},phasor=1;const auto step=std::polar(1.0,-2*pi/sps);
  const auto acquisitionLength=std::min(samples.size(),static_cast<std::size_t>(std::ceil(2048*sps)));
  for(std::size_t i=0;i<acquisitionLength;++i) {
    cyclic+=(static_cast<double>(std::norm(samples[i]))-power)*phasor;phasor*=step;
    if((i&4095U)==0){if(cancelled && cancelled())return clock;phasor=std::polar(1.0,-2*pi*(i+1)/sps);}
  }
  clock.coherence=std::abs(cyclic)/(power*acquisitionLength);
  double phase=std::fmod(-std::arg(cyclic)*sps/(2*pi)+sps,sps);
  const double guard=17+sps;
  double position=phase+std::ceil((guard-phase)/sps)*sps;
  const auto errorAt=[&](double t,double period) {
    std::complex<double> early,mid,late;
    if(!interpolateBandlimited(samples,t-period/2,early) || !interpolateBandlimited(samples,t,mid) || !interpolateBandlimited(samples,t+period/2,late))return 0.0;
    return std::real((early-late)*std::conj(mid))/power;
  };
  double slope=0;int probes=0;
  for(int i=0;i<1024 && position+(i+1)*sps+17+sps<samples.size();++i) {
    const double t=position+i*sps;slope+=(errorAt(t+.05*sps,sps)-errorAt(t-.05*sps,sps))/.10;++probes;
  }
  if(!probes)return clock;slope/=probes;
  if(std::abs(slope)<.01)return clock;
  const auto [alpha,beta]=loopGains(bandwidth);double period=sps;std::size_t clipped=0;
  while(position+17+period/2<samples.size()) {
    if(cancelled && cancelled())return {};
    std::complex<double> value;if(!interpolateBandlimited(samples,position,value))break;
    clock.positions.push_back(position);clock.values.push_back(value);
    const double error=std::clamp(errorAt(position,period)/slope,-1.0,1.0);
    const double proposed=period-beta*error*sps;
    period=std::clamp(proposed,sps*.98,sps*1.02);if(period!=proposed)++clipped;
    position+=period-alpha*error*sps;
  }
  if(clock.positions.size()<=acquisitionSymbols+256)return clock;
  const auto count=std::min<std::size_t>(2048,clock.positions.size()-acquisitionSymbols);
  const auto first=clock.positions.size()-count;
  const double meanIndex=(count-1)*.5;
  double meanTime=0;for(std::size_t i=first;i<clock.positions.size();++i)meanTime+=clock.positions[i];meanTime/=count;
  double covariance=0,variance=0;
  for(std::size_t i=0;i<count;++i){const double k=i-meanIndex;covariance+=k*(clock.positions[first+i]-meanTime);variance+=k*k;}
  const double fitted=covariance/variance;double error=0;
  for(std::size_t i=0;i<count;++i){const double residual=clock.positions[first+i]-meanTime-fitted*(i-meanIndex);error+=residual*residual;}
  clock.jitter=std::sqrt(error/count)/sps;clock.period=fitted;
  clock.locked=clock.coherence>.003 && clock.jitter<.15 && std::abs(fitted/sps-1)<.02 && clipped<clock.positions.size()/20;
  return clock;
}
std::complex<double> slice(std::complex<double> z,ModulationHypothesis hypothesis) {
  if(hypothesis==ModulationHypothesis::Bpsk)return {z.real()>=0?1.0:-1.0,0};
  if(hypothesis==ModulationHypothesis::Qpsk)return {z.real()>=0?std::sqrt(.5):-std::sqrt(.5),z.imag()>=0?std::sqrt(.5):-std::sqrt(.5)};
  const auto level=[](double x){return (x< -2/std::sqrt(10.0)?-3:x<0?-1:x<2/std::sqrt(10.0)?1:3)/std::sqrt(10.0);};
  return {level(z.real()),level(z.imag())};
}
double initialCarrierPhase(const std::vector<std::complex<double>>& symbols,double scale,ModulationHypothesis hypothesis) {
  const auto count=std::min<std::size_t>(2048,symbols.size());
  if(hypothesis!=ModulationHypothesis::Qam16) {
    std::complex<double> moment{};
    for(std::size_t i=0;i<count;++i){const auto z=symbols[i]/scale;moment+=hypothesis==ModulationHypothesis::Bpsk?z*z:z*z*z*z;}
    return hypothesis==ModulationHypothesis::Bpsk?std::arg(moment)/2:(std::arg(moment)-pi)/4;
  }
  double best=std::numeric_limits<double>::infinity(),phase=0;
  for(int i=0;i<64;++i) {
    const double angle=pi/2*i/64;const auto rotation=std::polar(1.0,-angle);double cost=0;
    for(std::size_t j=0;j<count;++j){const auto z=symbols[j]/scale*rotation;cost+=std::norm(z-slice(z,hypothesis));}
    if(cost<best){best=cost;phase=angle;}
  }
  return phase;
}
bool restoreCarrier(const Clock& clock,ModulationHypothesis hypothesis,double bandwidth,
                    std::vector<std::complex<double>>& restored,std::vector<double>& phases,
                    double& finalFrequency,double& evm,const AnalysisCancel& cancelled) {
  if(clock.values.size()<=acquisitionSymbols+256)return false;
  double power=0;for(std::size_t i=acquisitionSymbols;i<clock.values.size();++i)power+=std::norm(clock.values[i]);
  const double scale=std::sqrt(power/(clock.values.size()-acquisitionSymbols));if(!(scale>1e-10))return false;
  double phase=initialCarrierPhase(clock.values,scale,hypothesis),frequency=0;
  const auto [alpha,beta]=loopGains(bandwidth);std::array<std::size_t,16> populations{};
  double errorPower=0,referencePower=0;restored.reserve(clock.values.size());phases.reserve(clock.values.size());
  for(std::size_t i=0;i<clock.values.size();++i) {
    if((i&255U)==0 && cancelled && cancelled())return false;
    const auto z=clock.values[i]/scale*std::polar(1.0,-phase),decision=slice(z,hypothesis);
    phases.push_back(phase);restored.push_back(z*scale);
    double error=0;
    if(hypothesis==ModulationHypothesis::Bpsk)error=(z.real()>=0?1:-1)*z.imag();
    else if(hypothesis==ModulationHypothesis::Qpsk)error=((z.real()>=0?1:-1)*z.imag()-(z.imag()>=0?1:-1)*z.real())/std::sqrt(2.0);
    else error=std::imag(z*std::conj(decision))/std::max(.1,std::norm(decision));
    error=std::clamp(error,-.5,.5);frequency=std::clamp(frequency+beta*error,-.1,.1);phase+=frequency+alpha*error;
    if(i>=acquisitionSymbols) {
      errorPower+=std::norm(z-decision);referencePower+=std::norm(decision);
      int cell=0;
      if(hypothesis==ModulationHypothesis::Bpsk)cell=decision.real()>0?1:0;
      else if(hypothesis==ModulationHypothesis::Qpsk)cell=(decision.real()>0?2:0)+(decision.imag()>0?1:0);
      else cell=static_cast<int>(std::lround((decision.real()*std::sqrt(10.0)+3)/2))*4+static_cast<int>(std::lround((decision.imag()*std::sqrt(10.0)+3)/2));
      if(cell>=0 && cell<16)++populations[static_cast<std::size_t>(cell)];
    }
  }
  finalFrequency=frequency;evm=std::sqrt(errorPower/std::max(1e-30,referencePower));
  std::size_t occupied=0;for(const auto count:populations)if(count>clock.values.size()/200)++occupied;
  const auto required=hypothesis==ModulationHypothesis::Bpsk?2U:hypothesis==ModulationHypothesis::Qpsk?4U:12U;
  return occupied>=required && evm<(hypothesis==ModulationHypothesis::Qam16?.18:.20) && std::abs(frequency)<.099;
}
void selectConstellation(NarrowbandAnalysisResult& result,const NarrowbandAnalysisSettings& settings) {
  if(settings.constellationStage==AnalysisStage::RawIq)return;
  const std::vector<TaggedSymbol>* symbols=nullptr;
  if(settings.constellationStage==AnalysisStage::TimingAligned && result.timingLocked)symbols=&result.clockSymbols;
  if(settings.constellationStage==AnalysisStage::CarrierRecovered && result.carrierLocked)symbols=&result.synchronizedSymbols;
  const auto* continuous=findAnalysisStage(result,settings.constellationStage);
  if(!symbols && !continuous)return;
  const auto size=symbols?symbols->size():continuous->samples.size();if(!size)return;
  result.constellation.clear();const auto count=std::min<std::size_t>(settings.pointCount,size);
  double minimum=std::numeric_limits<double>::infinity(),maximum=-minimum;
  for(std::size_t i=0;i<count;++i) {
    const auto n=count>1?i*(size-1)/(count-1):0;TaggedSymbol tagged;
    if(symbols)tagged=(*symbols)[n];else {tagged.value=continuous->samples[n];tagged.output={continuous->outputBegin+n,0};mapAnalysisPosition(*result.source,tagged.output,tagged.source);}
    minimum=std::min({minimum,static_cast<double>(tagged.value.real()),static_cast<double>(tagged.value.imag())});maximum=std::max({maximum,static_cast<double>(tagged.value.real()),static_cast<double>(tagged.value.imag())});
    result.constellation.push_back(tagged);
  }
  const double margin=std::max((maximum-minimum)*.05,1e-9);result.axisMinimum=minimum-margin;result.axisMaximum=maximum+margin;
  result.constellationStage=settings.constellationStage;
  result.message="真实单载波处理阶段 · 绝对相位／比特极性未知";
}
}
const ContinuousAnalysisStage* findAnalysisStage(const NarrowbandAnalysisResult& result,AnalysisStage stage) {
  for(const auto& value:result.stages)if(value.stage==stage)return &value;return nullptr;
}
void recoverSingleCarrier(NarrowbandAnalysisResult& result,const NarrowbandAnalysisSettings& settings,const AnalysisCancel& cancelled) {
  const bool timing=settings.timingRecovery || settings.carrierRecovery || settings.constellationStage>=AnalysisStage::TimingAligned || settings.eyeStage>=AnalysisStage::TimingAligned;
  const bool carrier=settings.carrierRecovery || settings.constellationStage==AnalysisStage::CarrierRecovered || settings.eyeStage==AnalysisStage::CarrierRecovered;
  const bool matched=settings.matchedFilter || settings.constellationStage==AnalysisStage::MatchedFiltered || settings.eyeStage==AnalysisStage::MatchedFiltered;
  if(!timing && !matched && settings.cfoMode==CfoMode::Off)return;
  const auto fail=[&](const std::string& reason){result.syncMessage=reason;result.message+=" · "+reason;selectConstellation(result,settings);};
  if(!result.source || result.source->samples.empty())return;
  const auto& source=*result.source;const double fs=static_cast<double>(source.outputRateHz),rs=settings.symbolRateHz;
  if(source.samples.size()>256U*1024U*1024U/112U){fail("同步工作区预算超限，请缩小分析时窗。");return;}
  const bool known=settings.hypothesis==ModulationHypothesis::Bpsk || settings.hypothesis==ModulationHypothesis::Qpsk || settings.hypothesis==ModulationHypothesis::Qam16;
  std::vector<std::complex<float>> signal;signal.reserve(source.samples.size());
  for(const auto z:source.samples)signal.push_back(static_cast<std::complex<float>>((std::complex<double>(z)-result.removedDc)*result.gain));
  double cfo=0;
  if(settings.cfoMode==CfoMode::Automatic) {
    if(!known){fail("自动 CFO 需要明确 BPSK/QPSK/16QAM 分析假设；未知信号可手工输入。");return;}
    const auto estimate=momentCfo(signal,fs,rs,settings.hypothesis,cancelled);
    result.cfoConfidenceDb=estimate.prominence;
    if(!estimate.valid){fail("粗 CFO 候选置信度不足或观测过短，保留原始 IQ。");return;}
    cfo=estimate.hz;
  } else if(settings.cfoMode==CfoMode::Manual) {
    cfo=settings.manualCfoHz;if(!std::isfinite(cfo) || std::abs(cfo)>=fs/2){fail("手工 CFO 必须位于实际 Fs 的 Nyquist 区间内。");return;}
  }
  if(settings.cfoMode!=CfoMode::Off) {
    rotateCfo(signal,source.outputSamples.begin,fs,cfo,settings.manualPhaseRadians,cancelled);
    result.estimatedCfoHz=cfo;result.stages.push_back({AnalysisStage::CfoCorrected,source.outputSamples.begin,signal});
  }
  auto outputBegin=source.outputSamples.begin;
  if(matched) {
    if(!(rs>0)){fail("匹配滤波／定时恢复需要可靠候选或手工 Rs。");return;}
    std::vector<std::complex<float>> filtered;std::size_t discarded=0;std::string error;
    if(!matchedRrc(signal,fs/rs,settings.rolloff,settings.filterSpanSymbols,filtered,discarded,error,cancelled)){fail(error);return;}
    outputBegin+=discarded;result.filterDiscardedSamples=discarded*2;signal=std::move(filtered);
    result.stages.push_back({AnalysisStage::MatchedFiltered,outputBegin,signal});
  }
  if(!timing){selectConstellation(result,settings);return;}
  if(!(rs>0) || fs/rs<2){fail("Gardner 需要实际 SPS ≥ 2；请提高通道输出 Fs，K_B 不等于 SPS。");return;}
  const auto clock=synchronizeClock(signal,fs/rs,settings.timingLoopBandwidth,cancelled);
  result.timingLocked=clock.locked;result.timingErrorUi=clock.jitter;result.timingPeriodSamples=clock.period;
  if(!clock.locked){fail("定时未锁定：样本、循环特征或时钟稳定性不足。");return;}
  result.acquisitionSymbolsDiscarded=acquisitionSymbols;
  for(std::size_t i=acquisitionSymbols;i<clock.values.size();++i) {
    const auto integer=static_cast<SampleIndex>(std::floor(clock.positions[i]));TaggedSymbol tagged;
    tagged.value=static_cast<std::complex<float>>(clock.values[i]);tagged.output={outputBegin+integer,clock.positions[i]-integer};
    mapAnalysisPosition(source,tagged.output,tagged.source);result.clockSymbols.push_back(tagged);
  }
  if(carrier) {
    if(!known){fail("载波相位恢复需要明确的单载波 PSK/QAM 假设。");return;}
    std::vector<std::complex<double>> symbols;std::vector<double> phases;double residualFrequency=0,evm=0;
    const bool locked=restoreCarrier(clock,settings.hypothesis,settings.carrierLoopBandwidth,symbols,phases,residualFrequency,evm,cancelled);
    result.carrierLocked=locked;
    if(!locked){fail("载波未锁定：模板残差、符号多样性或环路状态不满足条件。");return;}
    result.conditionalEvm=evm;result.estimatedCfoHz=cfo+residualFrequency*rs/(2*pi);
    for(std::size_t i=acquisitionSymbols;i<symbols.size();++i){auto tagged=result.clockSymbols[i-acquisitionSymbols];tagged.value=static_cast<std::complex<float>>(symbols[i]);result.synchronizedSymbols.push_back(tagged);}
    std::vector<std::complex<float>> waveform(signal.size());std::size_t right=1;
    for(std::size_t i=0;i<signal.size();++i) {
      if((i&1023U)==0 && cancelled && cancelled())return;
      while(right+1<clock.positions.size() && clock.positions[right]<i)++right;
      const auto left=right-1;const double fraction=(static_cast<double>(i)-clock.positions[left])/(clock.positions[right]-clock.positions[left]);
      const double phase=phases[left]+std::clamp(fraction,0.0,1.0)*(phases[right]-phases[left]);
      waveform[i]=static_cast<std::complex<float>>(std::complex<double>(signal[i])*std::polar(1.0,-phase));
    }
    result.stages.push_back({AnalysisStage::CarrierRecovered,outputBegin,std::move(waveform)});
  }
  result.syncMessage=carrier?"定时与载波已锁定（单载波假设）；窗口独立捕获，绝对相位未知。":"定时已锁定；尚未执行载波相位恢复。";
  selectConstellation(result,settings);result.message+=" · "+result.syncMessage;
}
}  // namespace signalstudio
