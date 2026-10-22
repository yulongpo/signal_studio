#pragma once
#include <cstddef>
namespace signalstudio {
struct NarrowbandAnalysisSettings {
  int pointCount=4096;
  bool removeDc=false, normalizePower=false, constellationAutoScale=true;
  std::size_t sampleBudget=1U*1024U*1024U;
  bool operator==(const NarrowbandAnalysisSettings&) const = default;
};
}  // namespace signalstudio
