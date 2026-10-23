#include "domain/channel_sampling.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <set>
#include "domain/project.h"

namespace signalstudio {
namespace {
std::uint64_t integralRate(double rate, double tolerance) {
  if (!std::isfinite(rate) || rate <= 0 || rate > 4e9) return 0;
  const auto rounded = static_cast<std::uint64_t>(std::llround(rate));
  return std::abs(rate - static_cast<double>(rounded)) <= tolerance ? rounded : 0;
}
std::size_t tapCount(double fs, double pass, double stop, double db) {
  if (!(pass > 0 && stop > pass && stop < fs / 2)) return 0;
  const double n = std::ceil((db - 8) / (2.285 * 2 * std::numbers::pi * (stop-pass)/fs)) + 1;
  if (!std::isfinite(n) || n < 3 || n > 131071) return 0;
  return static_cast<std::size_t>(n) | std::size_t{1};
}
bool fail(std::string message, const char* key, std::string& error,
          std::string* field) {
  error = std::move(message);
  if (field) *field = key;
  return false;
}
}

bool describeChannelDsp(const FileMetadata& source, const Channel& channel,
                        ChannelDspSpecification& plan, std::string& error,
                        std::string* field) {
  plan = {};
  error.clear();
  if (field) field->clear();
  const auto sourceRate = integralRate(source.sampleRateHz, .01);
  const auto outputRate = integralRate(channel.outputSampleRateHz, 1e-7);
  if (!sourceRate) return fail("来源 Fs 必须为 0–4 GHz 内的整数 Hz。", "source", error, field);
  if (!outputRate) return fail("当前 DSP 输出 Fs 必须为 0–4 GHz 内的整数 Hz；手工值未被改写。", "outputSampleRateHz", error, field);
  const double pass = channel.bandwidthHz / 2, stop = channel.bandwidthHz * .65;
  if (!std::isfinite(pass) || !(pass > 0) || !std::isfinite(channel.centerFrequencyHz))
    return fail("提取中心和带宽必须有效。", "frequency", error, field);
  const auto band = fullRange(source).frequency;
  if (channel.centerFrequencyHz-stop < band.lowerHz || channel.centerFrequencyHz+stop > band.upperHz)
    return fail("通道滤波过渡带超出源文件有效频带", "frequency", error, field);
  if (outputRate < stop*2 || static_cast<double>(outputRate) > sourceRate*4096.0)
    return fail("输出采样率必须覆盖通带与抗混叠过渡带", "outputSampleRateHz", error, field);
  const auto filter = static_cast<int>(channel.filter);
  if (filter < 0 || filter > 2) return fail("FIR 滤波配置无效。", "filter", error, field);
  const double db = filter == 0 ? 40 : filter == 2 ? 80 : 60;
  auto rate = sourceRate;
  const auto minimum = static_cast<std::uint64_t>(std::ceil(stop*4));
  while ((rate % 2) == 0 && rate/2 >= minimum && rate/2 >= 2) {
    const double attenuation = db + 6 + 20*std::log10(static_cast<double>(plan.stages.size()+2));
    const double aliasStop = static_cast<double>(rate/2)-stop;
    const auto taps = tapCount(static_cast<double>(rate), pass, aliasStop, attenuation);
    if (!taps) break;
    plan.stages.push_back({static_cast<double>(rate), pass, aliasStop, attenuation, taps});
    rate /= 2;
  }
  const auto common = std::gcd(outputRate, rate);
  const auto l = outputRate/common, m = rate/common;
  if (l > 4096 || m > 4096)
    return fail("有理重采样比例超出 L、M ≤ 4096 的支持范围", "outputSampleRateHz", error, field);
  const double expanded = static_cast<double>(rate)*l;
  const double attenuation = db + 6 + 20*std::log10(static_cast<double>(plan.stages.size()+1));
  const auto taps = tapCount(expanded, pass, std::min(expanded/2, stop), attenuation);
  if (!taps) return fail("FIR 阶数超出当前资源预算；请增加过渡带或更换输出采样率", "filter", error, field);
  plan.sourceDecimationStages = static_cast<int>(plan.stages.size());
  plan.stages.push_back({expanded, pass, std::min(expanded/2, stop), attenuation, taps});
  plan.sourceRateHz=sourceRate; plan.outputRateHz=outputRate;
  plan.interpolation=static_cast<std::uint32_t>(l); plan.decimation=static_cast<std::uint32_t>(m);
  plan.transitionHz=stop-pass; plan.cutoffHz=(pass+stop)/2; plan.attenuationDb=db;
  double support = static_cast<double>(taps)/(2*l)+2;
  for (int i=plan.sourceDecimationStages-1; i>=0; --i)
    support = support*2 + plan.stages[static_cast<std::size_t>(i)].taps/2;
  double input = std::ceil(16384.0*sourceRate/outputRate)+2*support+64;
  std::size_t coefficientBytes=0;
  for (const auto& stage:plan.stages) coefficientBytes += stage.taps*sizeof(double);
  plan.estimatedWorkingBytes = static_cast<std::size_t>(std::ceil(input*1.5*sizeof(float)*2))+coefficientBytes;
  for (int i=0;i<plan.sourceDecimationStages;++i) {
    input = std::ceil(input/2);
    plan.estimatedBlockMacs += input*plan.stages[static_cast<std::size_t>(i)].taps;
  }
  plan.estimatedBlockMacs += 16384.0*(static_cast<double>(taps)/l+2);
  if (plan.estimatedWorkingBytes > 256U*1024U*1024U || plan.estimatedBlockMacs > 256e6)
    return fail("每个 IQ 块的 DSP 工作内存或运算预算超限；请调整带宽、滤波或输出 Fs。", "filter", error, field);
  return true;
}

OutputRatePlan planChannelOutputRate(const FileMetadata& source, const Channel& channel) {
  OutputRatePlan result;
  result.requestedFactor=channel.sampling.bandwidthFactor;
  const bool automatic=channel.sampling.mode==ChannelSamplingMode::RecommendedFactor;
  result.targetRateHz=automatic ? channel.bandwidthHz*result.requestedFactor : channel.outputSampleRateHz;
  if (automatic && (!std::isfinite(result.requestedFactor) ||
      (result.requestedFactor!=1.5 && result.requestedFactor!=2 && result.requestedFactor!=4 && result.requestedFactor!=8))) {
    result.reason="请选择 1.5×、2×、4×或 8×。"; result.field="sampling"; return result;
  }
  if (!automatic) {
    result.valid=describeChannelDsp(source,channel,result.dsp,result.reason,&result.field);
    if (result.valid) { result.realizedRateHz=static_cast<double>(result.dsp.outputRateHz); result.realizedFactor=result.realizedRateHz/channel.bandwidthHz; }
    return result;
  }
  if (!std::isfinite(result.targetRateHz) || result.targetRateHz<=0) {
    result.reason="带宽必须为有限正值。"; result.field="frequency"; return result;
  }
  const auto sourceRate=integralRate(source.sampleRateHz,.01);
  const double minimum=std::ceil(channel.bandwidthHz*1.3);
  if (!sourceRate || minimum>sourceRate) { result.reason="源 Fs 不足以覆盖通道与 FIR 过渡带；请收窄带宽。";result.field="frequency";return result; }
  const auto low=static_cast<std::uint64_t>(std::max(1.0,minimum));
  const double target=std::clamp(result.targetRateHz,static_cast<double>(low),static_cast<double>(sourceRate));
  std::set<std::uint64_t> candidates{sourceRate, low, static_cast<std::uint64_t>(std::llround(target))};
  // Enumerate bounded rational denominators; no coefficient construction here.
  // Trying all dyadic intermediate rates also covers a stopped decimator.
  for (auto rate=sourceRate;;rate/=2) {
    for (std::uint64_t m=1;m<=4096;++m) {
      if (rate%m) continue;
      const auto unit=rate/m;
      const auto nearest=static_cast<std::int64_t>(std::llround(target/unit));
      for (int offset=-1;offset<=1;++offset) {
        const auto l=nearest+offset;
        if (l<1 || l>4096) continue;
        const auto candidate=unit*static_cast<std::uint64_t>(l);
        if (candidate>=low && candidate<=sourceRate) candidates.insert(candidate);
      }
    }
    if (rate%2 || rate/2<2 || static_cast<double>(rate/2)<channel.bandwidthHz*2.6) break;
  }
  double best=std::numeric_limits<double>::infinity(), cost=best;
  for (const auto rate:candidates) {
    auto candidate=channel;candidate.outputSampleRateHz=static_cast<double>(rate);
    ChannelDspSpecification dsp;std::string reason,field;
    if (!describeChannelDsp(source,candidate,dsp,reason,&field)) {
      if (result.reason.empty()) {result.reason=reason;result.field=field;}
      continue;
    }
    const double distance=std::abs(static_cast<double>(rate)-result.targetRateHz);
    if (distance<best || (distance==best && dsp.estimatedBlockMacs<cost)) {
      best=distance;cost=dsp.estimatedBlockMacs;result.dsp=std::move(dsp);result.valid=true;
    }
  }
  if (!result.valid) {result.reason="无可实现默认输出采样率："+result.reason;return result;}
  result.realizedRateHz=static_cast<double>(result.dsp.outputRateHz);
  result.realizedFactor=result.realizedRateHz/channel.bandwidthHz;
  result.adjusted=std::abs(result.realizedRateHz-result.targetRateHz)>1e-7;
  result.field.clear();
  result.reason=!result.adjusted ? "目标 Fs 可直接实现。" : result.targetRateHz>sourceRate ?
      "目标超过源 Fs；采用不超过源 Fs 的实际速率，不增加原始频谱信息。" :
      "目标不满足有理比例、FIR 或工作区约束；采用最接近的可行速率。";
  return result;
}
}  // namespace signalstudio
