#include "infrastructure/narrowband_analysis/analysis_source.h"

#include <QFileInfo>
#include <QDateTime>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>

namespace signalstudio {
std::string analysisSourceKey(const FileMetadata& source,const Channel& channel) {
  const QFileInfo info(QString::fromStdString(source.path));
  QStringList parts{QString::fromStdString(source.id),QString::fromStdString(source.path),
    QString::fromStdString(sampleFormatKey(source.sampleFormat)),QString::fromStdString(source.availability.fingerprint),
    QString::number(source.sampleCount),QString::number(availableSamples(source)),
    QString::number(source.availability.generation),QString::number(static_cast<int>(source.availability.status)),
    QString::number(source.sampleRateHz,'g',17),QString::number(source.centerFrequencyHz,'g',17),
    QString::number(source.effectiveBandwidthHz,'g',17),QString::number(info.size()),
    QString::number(info.lastModified().toMSecsSinceEpoch()),QString::fromStdString(channel.id),
    QString::number(channel.configVersion),QString::number(channel.sourceTime.begin),QString::number(channel.sourceTime.end),
    QString::number(channel.outputSampleRateHz,'g',17),QString::number(channel.centerFrequencyHz,'g',17),
    QString::number(channel.bandwidthHz,'g',17),QString::number(static_cast<int>(channel.filter))};
  return parts.join('\n').toStdString();
}
bool mapAnalysisPosition(const AnalysisSourceData& data,SamplePosition output,SamplePosition& source) {
  if(!data.outputRateHz || !data.sourceRateHz || !std::isfinite(output.fraction) || output.fraction<0 || output.fraction>=1)return false;
  const auto quotient=output.integer/data.outputRateHz,remainder=output.integer%data.outputRateHz;
  const auto maximum=std::numeric_limits<SampleIndex>::max();
  if(quotient>maximum/data.sourceRateHz || remainder>maximum/data.sourceRateHz)return false;
  const auto product=remainder*data.sourceRateHz;
  const auto whole=quotient*data.sourceRateHz;
  const auto integerFraction=product/data.outputRateHz;
  if(integerFraction>maximum-whole || data.sourceOrigin>maximum-whole-integerFraction)return false;
  double fraction=static_cast<double>(product%data.outputRateHz)/data.outputRateHz+
      output.fraction*static_cast<double>(data.sourceRateHz)/data.outputRateHz;
  const auto carry=static_cast<SampleIndex>(std::floor(fraction));
  const auto integer=data.sourceOrigin+whole+integerFraction;
  if(carry>maximum-integer)return false;
  fraction-=static_cast<double>(carry);
  source={integer+carry,fraction};return true;
}
bool analysisIdentityCurrent(const AnalysisIdentity& identity,const FileMetadata& source,const Channel& channel,
                             std::uint64_t project,std::uint64_t request) {
  return identity.projectGeneration==project && identity.requestGeneration==request &&
      identity.channelId==channel.id && identity.channelConfigVersion==channel.configVersion &&
      identity.sourceKey==analysisSourceKey(source,channel);
}
AnalysisSourceData readAnalysisSource(ChannelSampleCache& cache,const FileMetadata& source,const Channel& channel,
                                      const AnalysisRequest& request,const AnalysisCancel& cancelled) {
  AnalysisSourceData result;result.identity=request.identity;result.requestedOutputSamples=request.identity.outputSamples;
  const auto fail=[&](AnalysisStatus status,std::string message) {
    result.samples.clear();result.status=status;result.message=std::move(message);return std::move(result);
  };
  if(cancelled && cancelled())return fail(AnalysisStatus::Cancelled,"分析已取消。");
  const auto key=analysisSourceKey(source,channel);
  if(request.identity.channelId!=channel.id || request.identity.channelConfigVersion!=channel.configVersion ||
     (!request.identity.sourceKey.empty() && request.identity.sourceKey!=key))
    return fail(AnalysisStatus::Cancelled,"通道配置或来源身份已变化。");
  result.identity.sourceKey=key;
  if(source.demo || source.path.empty())return fail(AnalysisStatus::NotApplicable,"来源没有真实 IQ 样本。");
  if(source.availability.status==LoadStatus::Failed || !availableSamples(source) ||
     channel.sourceTime.end>availableSamples(source))
    return fail(AnalysisStatus::Failed,"通道时段超出真实可用前缀，或来源读取失败。");
  const auto range=request.identity.outputSamples;
  if(range.end<=range.begin)return fail(AnalysisStatus::InsufficientSamples,"所选区间没有输出样本。");
  if(!request.sampleBudget || request.sampleBudget>AnalysisRequest::maximumSamples || range.end-range.begin>request.sampleBudget)
    return fail(AnalysisStatus::Failed,"分析样本预算超限，请缩小所选时间区间。");
  ChannelDspPlan plan;QString error;
  if(!makeChannelDspPlan(source,channel,plan,error,nullptr,cancelled))
    return fail(cancelled && cancelled()?AnalysisStatus::Cancelled:AnalysisStatus::Failed,error.toStdString());
  const auto valid=channelValidOutputSamples(source,channel,plan);
  const auto begin=std::max(range.begin,valid.begin),end=std::min(range.end,valid.end);
  if(end<=begin)return fail(AnalysisStatus::InsufficientSamples,"排除 FIR 边界补零后没有真实有效样本。");
  result.outputSamples={begin,end};result.sourceOrigin=channel.sourceTime.begin;
  result.sourceRateHz=plan.sourceRateHz;result.outputRateHz=plan.outputRateHz;
  result.discardedLeft=begin-range.begin;result.discardedRight=range.end-end;
  result.samples.reserve(static_cast<std::size_t>(end-begin));
  for(auto position=begin;position<end;) {
    if(cancelled && cancelled())return fail(AnalysisStatus::Cancelled,"分析已取消。");
    const auto last=std::min(end,position+ChannelSampleCache::blockSamples);
    ChannelSampleData block;
    if(!cache.process(source,channel,plan,{position,last},block,cancelled))
      return fail(cancelled && cancelled()?AnalysisStatus::Cancelled:AnalysisStatus::Failed,"连续通道 IQ 读取失败；未产生替代数据。");
    result.amplitudeScale=block.waveformScale;
    result.samples.insert(result.samples.end(),block.samples.begin(),block.samples.end());
    position=last;
  }
  if(key!=analysisSourceKey(source,channel))return fail(AnalysisStatus::Cancelled,"来源在分析读取期间发生变化。");
  SamplePosition first,last;
  if(!mapAnalysisPosition(result,{begin,0},first) || !mapAnalysisPosition(result,{end,0},last))
    return fail(AnalysisStatus::Failed,"来源样本映射超出 uint64 范围。");
  result.sourceSamples={first.integer,last.integer+(last.fraction>0?1:0)};
  result.status=AnalysisStatus::Ready;result.message="真实连续窄带 IQ；已排除 FIR 补零边界。";
  return result;
}
}  // namespace signalstudio
