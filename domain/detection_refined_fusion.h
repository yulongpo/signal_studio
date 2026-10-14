#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "domain/annotation.h"

namespace signalstudio {

// One publication scope only: all inputs belong to the same source/channel/run.
// The caller retains this immutable array to resolve every member's parent and
// original stable identifier when it builds annotation/window origin evidence.
struct DetectionRefinedEvent {
  Segment segment;
  std::size_t parentIndex = 0;
  std::string stableId;
};

struct DetectionRefinedFusion {
  Segment segment;
  // Sorted indices into the original DetectionRefinedEvent input array.
  std::vector<std::size_t> memberEventIndices;
};

struct DetectionRefinedFusionOptions {
  double matchIou = .3;
  double binWidthHz = 0;
  std::size_t maximumInputEvents = 100000;
  std::size_t maximumIdBytes = 256;
  std::uint64_t maximumPairComparisons = 4000000;
  std::uint64_t workingByteBudget = 32 * 1024 * 1024;
  std::function<bool()> cancelled;
};

struct DetectionRefinedFusionSummary {
  std::string numericalVersion = "signalstudio-refined-fusion-v1";
  std::size_t inputEvents = 0;
  std::size_t outputEvents = 0;
  std::size_t mergedPairs = 0;
  std::uint64_t pairComparisons = 0;
  std::uint64_t estimatedWorkingBytes = 0;
};

// Only positive-overlap/touching time ranges can merge. Positive source-sample
// gaps never merge, including at sample rates where one sample is below 1 ps.
// Frequency uses IoU or containment with centre limit min(widths)/2 + 2 bins.
// Each iteration chooses (-max(IoU,containment), centreDistance, minStart, i,
// j) and unions physical geometry; this repeats until no admissible pair
// remains. Errors/cancellation leave both output arguments unchanged.
bool fuseRefinedDetectionEvents(std::span<const DetectionRefinedEvent> input,
                                const DetectionRefinedFusionOptions& options,
                                std::vector<DetectionRefinedFusion>& output,
                                DetectionRefinedFusionSummary& summary,
                                std::string& error);

}  // namespace signalstudio
