#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "domain/project.h"

namespace signalstudio {
enum class AnalysisStatus { Ready, NeedSymbolRate, Estimating, Processing, InsufficientSamples, LowConfidence, NotApplicable, Cancelled, Failed };
inline NarrowbandAnalysisSettings channelAnalysisSettings(const Channel& channel) {
  auto settings=channel.analysis;
  settings.eyePeriods=channel.eyePeriods;settings.eyeTraces=channel.eyeTraces;settings.eyeComponent=channel.eyeComponent;
  return settings;
}
struct SamplePosition {
  SampleIndex integer = 0;
  double fraction = 0;
  bool operator==(const SamplePosition&) const = default;
};
struct AnalysisIdentity {
  std::uint64_t projectGeneration=0, requestGeneration=0, channelConfigVersion=0;
  std::string channelId, sourceKey;
  TimeRange outputSamples;
  bool operator==(const AnalysisIdentity&) const = default;
};
struct AnalysisRequest {
  AnalysisIdentity identity;
  std::size_t sampleBudget=1U*1024U*1024U;
  static constexpr std::size_t maximumSamples=8U*1024U*1024U;
};
struct AnalysisSourceData {
  AnalysisStatus status=AnalysisStatus::Failed;
  AnalysisIdentity identity;
  std::string message;
  std::vector<std::complex<float>> samples;
  TimeRange outputSamples, sourceSamples;
  TimeRange requestedOutputSamples;
  SampleIndex sourceOrigin=0;
  std::uint64_t sourceRateHz=0, outputRateHz=0;
  SampleIndex discardedLeft=0, discardedRight=0;
  double amplitudeScale=32768;
};
struct TaggedSymbol {
  std::complex<float> value;
  SamplePosition output, source;
};
struct EyeTrace {
  std::vector<float> i, q;
  SamplePosition outputStart, sourceStart;
  std::vector<SamplePosition> outputPositions;
};
struct SymbolRateCandidate {
  double rateHz=0,score=0,prominenceDb=0;
  std::string method;
};
struct NarrowbandAnalysisResult {
  AnalysisIdentity identity;
  AnalysisStatus status=AnalysisStatus::Failed, eyeStatus=AnalysisStatus::NeedSymbolRate;
  AnalysisStage constellationStage=AnalysisStage::RawIq;
  std::string message, eyeMessage="需要输入或估计符号率。";
  std::shared_ptr<const AnalysisSourceData> source;
  std::vector<TaggedSymbol> constellation;
  std::vector<EyeTrace> eyes;
  double axisMinimum=-1,axisMaximum=1;
  std::complex<double> removedDc{};
  double gain=1;
  double symbolRateHz=0,samplesPerSymbol=0,estimatedCfoHz=0;
  double eyeAxisMinimum=-1,eyeAxisMaximum=1;
  double conditionalEvm=-1,timingErrorUi=-1;
  int eyePeriods=2,eyePointsPerUi=16;
  std::size_t filterDiscardedSamples=0;
  bool timingLocked=false,carrierLocked=false;
  std::vector<SymbolRateCandidate> rateCandidates;
  std::vector<TaggedSymbol> synchronizedSymbols;
};
}  // namespace signalstudio
