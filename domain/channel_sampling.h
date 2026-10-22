#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace signalstudio {
struct FileMetadata;
struct Channel;

enum class ChannelSamplingMode { Manual, RecommendedFactor };
struct ChannelSamplingPolicy {
  ChannelSamplingMode mode = ChannelSamplingMode::Manual;
  double bandwidthFactor = 2.0;
  bool operator==(const ChannelSamplingPolicy&) const = default;
};
struct ChannelFirSpecification {
  double sampleRateHz = 0, passEdgeHz = 0, stopEdgeHz = 0;
  double attenuationDb = 0;
  std::size_t taps = 0;
};
struct ChannelDspSpecification {
  std::uint64_t sourceRateHz = 0, outputRateHz = 0;
  std::uint32_t interpolation = 0, decimation = 0;
  int sourceDecimationStages = 0;
  double transitionHz = 0, cutoffHz = 0, attenuationDb = 0;
  std::size_t estimatedWorkingBytes = 0;
  double estimatedBlockMacs = 0;
  std::vector<ChannelFirSpecification> stages;
};
struct OutputRatePlan {
  bool valid = false, adjusted = false;
  double requestedFactor = 2, targetRateHz = 0;
  double realizedRateHz = 0, realizedFactor = 0;
  std::string reason, field;
  ChannelDspSpecification dsp;
};

// Arithmetic only. Coefficients and physical file validation stay in the
// infrastructure processor. Both default requests and the processor use this.
bool describeChannelDsp(const FileMetadata&, const Channel&,
                        ChannelDspSpecification&, std::string& error,
                        std::string* field = nullptr);
OutputRatePlan planChannelOutputRate(const FileMetadata&, const Channel&);
}  // namespace signalstudio
