#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "domain/project.h"

namespace signalstudio {
enum class AnalysisStage { RawIq, CfoCorrected, MatchedFiltered, TimingAligned, CarrierRecovered };
enum class AnalysisStatus { Ready, NeedSymbolRate, Estimating, Processing, InsufficientSamples, LowConfidence, NotApplicable, Cancelled, Failed };
enum class SymbolRateMode { Unknown, Manual, Estimated };
enum class ModulationHypothesis { Unknown, Bpsk, Qpsk, Qam16, Fsk, Ofdm, Gmsk };
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
};
}  // namespace signalstudio
