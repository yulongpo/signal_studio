#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "domain/detection_refined_fusion.h"

using namespace signalstudio;

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

DetectionRefinedEvent event(std::string id, std::size_t parent,
                            SampleIndex begin, SampleIndex end,
                            double lowerHz = 100, double upperHz = 110,
                            SampleIndex uncertainty = 2) {
  Segment segment;
  segment.id = id;
  segment.range = {{begin, end}, {lowerHz, upperHz}};
  segment.timeQuality = TimeQuality::AlgorithmEstimated;
  segment.frequencyQuality = FrequencyQuality::Refined;
  segment.uncertaintySamples = uncertainty;
  return {std::move(segment), parent, std::move(id)};
}

DetectionRefinedFusionOptions options() {
  DetectionRefinedFusionOptions result;
  result.binWidthHz = 1;
  return result;
}

std::vector<DetectionRefinedFusion> fuse(
    const std::vector<DetectionRefinedEvent>& input,
    const DetectionRefinedFusionOptions& config = options(),
    DetectionRefinedFusionSummary* returnedSummary = nullptr) {
  std::vector<DetectionRefinedFusion> output;
  DetectionRefinedFusionSummary summary;
  std::string error;
  check(fuseRefinedDetectionEvents(input, config, output, summary, error),
        error.c_str());
  if (returnedSummary) *returnedSummary = summary;
  return output;
}

void expandedDifferentParentsKeepBothLineages() {
  // Nominal parents [10,20) and [24,34) were distinct before their +/-8 ms
  // sample searches measured overlapping physical events [9,27) and [22,40).
  auto a = event("parent-7/event-0", 7, 9, 27, 100, 110, 2);
  auto b = event("parent-19/event-0", 19, 22, 40, 101, 112, 5);
  a.segment.confirmed = b.segment.confirmed = true;
  b.segment.clippedAtRoi = true;
  DetectionRefinedFusionSummary summary;
  const std::vector<DetectionRefinedEvent> input{a, b};
  const auto output = fuse(input, options(), &summary);
  check(output.size() == 1 &&
            output[0].segment.range == ViewRange{{9, 40}, {100, 112}} &&
            output[0].memberEventIndices == std::vector<std::size_t>{0, 1} &&
            input[output[0].memberEventIndices[0]].parentIndex == 7 &&
            input[output[0].memberEventIndices[1]].parentIndex == 19 &&
            output[0].segment.id == "parent-7/event-0" &&
            output[0].segment.timeQuality == TimeQuality::AlgorithmEstimated &&
            output[0].segment.uncertaintySamples == 5 &&
            output[0].segment.clippedAtRoi && !output[0].segment.confirmed &&
            summary.inputEvents == 2 && summary.outputEvents == 1 &&
            summary.mergedPairs == 1,
        "Expanded fine events must fuse geometry while retaining both original "
        "parent lineages");
  check(input[0].segment.range.time == TimeRange{9, 27} &&
            input[1].segment.range.time == TimeRange{22, 40},
        "Fusion must not mutate the original fine event evidence");
}

void positiveSampleGapsNeverBridgeButTouchMerges() {
  constexpr SampleIndex origin = (SampleIndex{1} << 53) + 7;
  auto output = fuse({event("A", 0, origin, origin + 10),
                      event("B", 1, origin + 11, origin + 20)});
  check(output.size() == 2 && output[0].segment.range.time.end == origin + 10 &&
            output[1].segment.range.time.begin == origin + 11,
        "A positive one-sample gap must stay split without floating-time "
        "epsilon");
  output = fuse({event("A", 0, origin, origin + 10),
                 event("B", 1, origin + 10, origin + 20)});
  check(output.size() == 1 &&
            output[0].segment.range.time == TimeRange{origin, origin + 20},
        "Touching half-open physical time ranges may merge");
}

void frequencyContactAndDisjointBandsStaySeparate() {
  const auto output =
      fuse({event("A", 0, 0, 20, 0, 10), event("B", 1, 0, 20, 10, 20),
            event("C", 2, 0, 20, 30, 40)});
  check(output.size() == 3,
        "Frequency contact or disjoint bands must not imply the same carrier");
}

void containmentRequiresThePhysicalCenterLimit() {
  auto output = fuse(
      {event("wide", 0, 0, 20, 0, 100), event("narrow", 1, 5, 25, 52, 62)});
  check(output.size() == 1,
        "Low IoU containment passes at narrowWidth/2 + 2*binWidth center "
        "distance");
  output = fuse({event("wide", 0, 0, 20, 0, 100),
                 event("narrow", 1, 5, 25, 52.0001, 62.0001)});
  check(output.size() == 2,
        "Containment beyond the center limit must not merge even with full "
        "overlap");
  auto widerBins = options();
  widerBins.binWidthHz = 2;
  output =
      fuse({event("wide", 0, 0, 20, 0, 100), event("narrow", 1, 5, 25, 54, 64)},
           widerBins);
  check(output.size() == 1,
        "The two-bin tolerance must be converted to physical Hz");
}

void deterministicBestPairChangesAChainOutcome() {
  auto config = options();
  config.matchIou = .5;
  const std::vector<DetectionRefinedEvent> input{event("A", 0, 0, 10, 0, 10),
                                                 event("B", 1, 0, 10, 5, 15),
                                                 event("C", 2, 0, 10, 10, 20)};
  const auto output = fuse(input, config);
  const auto repeated = fuse(input, config);
  check(output.size() == 2 &&
            output[0].memberEventIndices == std::vector<std::size_t>{0, 1} &&
            output[0].segment.range.frequency == FrequencyRange{0, 15} &&
            output[1].memberEventIndices == std::vector<std::size_t>{2} &&
            output[0].segment == repeated[0].segment &&
            output[0].memberEventIndices == repeated[0].memberEventIndices,
        "Equal pair scores must pick lower input indices before a union "
        "changes later eligibility");
}

void rankingUsesSimilarityThenCenterThenTime() {
  auto config = options();
  config.matchIou = .5;
  // Equal similarity and centre distances let minStart choose B-C first.
  const auto output =
      fuse({event("A", 0, 5, 20, 0, 10), event("B", 1, 5, 20, 5, 15),
            event("C", 2, 0, 20, 10, 20)},
           config);
  check(output.size() == 2 &&
            output[0].memberEventIndices == std::vector<std::size_t>{0} &&
            output[1].memberEventIndices == std::vector<std::size_t>{1, 2},
        "The earliest physical start must resolve equal similarity and "
        "center-distance pairs");
  config.matchIou = .3;
  const auto stable =
      fuse({event("A", 0, 0, 20, 0, 10), event("B", 1, 10, 30, 0, 10),
            event("C", 2, 25, 40, 0, 10)},
           config);
  check(stable.size() == 1 &&
            stable[0].segment.range.time == TimeRange{0, 40} &&
            stable[0].memberEventIndices == std::vector<std::size_t>{0, 1, 2},
        "Fusion must repeat until the expanded geometry has no remaining "
        "admissible pair");
}

void qualitiesAndOutputOrderRemainPhysical() {
  auto first = event("first", 0, 10, 20, 100, 110, 1);
  auto second = event("second", 1, 15, 25, 100, 110, 8);
  second.segment.frequencyQuality = FrequencyQuality::ModelEstimated;
  const auto output = fuse({event("higher-band", 2, 0, 10, 200, 210), second,
                            first, event("earlier", 3, 0, 5, 100, 110)});
  check(output.size() == 3 && output[0].segment.id == "earlier" &&
            output[1].segment.id == "second" &&
            output[2].segment.id == "higher-band" &&
            output[1].segment.frequencyQuality ==
                FrequencyQuality::ModelEstimated &&
            output[1].segment.timeQuality == TimeQuality::AlgorithmEstimated &&
            output[1].segment.uncertaintySamples == 8,
        "Output ordering and merged quality must retain physical coordinates "
        "and conservative uncertainty");
}

void budgetsAndCancellationAreAtomic() {
  const std::vector<DetectionRefinedEvent> input{
      event("A", 0, 0, 10), event("B", 1, 0, 10), event("C", 2, 0, 10)};
  std::vector<DetectionRefinedFusion> output{
      {event("sentinel", 99, 90, 100).segment, {99}}};
  const auto originalOutput = output;
  DetectionRefinedFusionSummary summary;
  summary.numericalVersion = "sentinel";
  summary.inputEvents = 99;
  std::string error;
  const auto unchanged = [&] {
    return output.size() == originalOutput.size() &&
           output[0].segment == originalOutput[0].segment &&
           output[0].memberEventIndices ==
               originalOutput[0].memberEventIndices &&
           summary.numericalVersion == "sentinel" && summary.inputEvents == 99;
  };
  auto config = options();
  config.maximumPairComparisons = 3;
  check(!fuseRefinedDetectionEvents(input, config, output, summary, error) &&
            unchanged() && error.find("comparison budget") != std::string::npos,
        "Cumulative comparison exhaustion after an internal merge must not "
        "publish partial output");
  config = options();
  config.workingByteBudget = 1;
  check(!fuseRefinedDetectionEvents(input, config, output, summary, error) &&
            unchanged(),
        "Working-byte exhaustion must preserve output and summary");
  config = options();
  config.maximumInputEvents = 2;
  check(!fuseRefinedDetectionEvents(input, config, output, summary, error) &&
            unchanged(),
        "Input-event bound must be enforced before tentative allocation");
  config = options();
  unsigned checks = 0;
  config.cancelled = [&] { return ++checks == 5; };
  check(!fuseRefinedDetectionEvents(input, config, output, summary, error) &&
            checks == 5 && unchanged() && error == "Refined fusion cancelled",
        "Cancellation after an internal merge must leave durable output "
        "unchanged");
}

void validationIsStrictAndEmptyInputIsExplicit() {
  auto a = event("A", 0, 0, 10);
  auto b = event("B", 1, 0, 10);
  std::vector<DetectionRefinedFusion> output;
  DetectionRefinedFusionSummary summary;
  std::string error;
  b.stableId = a.stableId;
  check(fuseRefinedDetectionEvents({&a, 1}, options(), output, summary, error),
        "A valid singleton must be accepted");
  check(!fuseRefinedDetectionEvents(std::vector<DetectionRefinedEvent>{a, b},
                                    options(), output, summary, error),
        "Duplicate stable identity must fail instead of dropping provenance");
  b = event("B", 1, 0, 10);
  b.segment.frequencyFrame = FrequencyFrame::Baseband;
  check(!fuseRefinedDetectionEvents(std::vector<DetectionRefinedEvent>{a, b},
                                    options(), output, summary, error),
        "Mixed frequency coordinate frames must not fuse");
  b = event("B", 1, 0, 10);
  b.segment.range.frequency.upperHz = std::numeric_limits<double>::infinity();
  check(!fuseRefinedDetectionEvents(std::vector<DetectionRefinedEvent>{a, b},
                                    options(), output, summary, error),
        "Nonfinite refined geometry must fail");
  b = event("B", 1, 0, 10);
  b.segment.timeQuality = TimeQuality::WindowObservation;
  check(!fuseRefinedDetectionEvents(std::vector<DetectionRefinedEvent>{a, b},
                                    options(), output, summary, error),
        "An uncertain observation without measured activity must not enter "
        "event fusion");
  auto config = options();
  config.binWidthHz = 0;
  check(!fuseRefinedDetectionEvents({&a, 1}, config, output, summary, error),
        "Two-bin frequency tolerance requires an explicit physical bin width");
  check(fuseRefinedDetectionEvents({}, options(), output, summary, error) &&
            output.empty() && summary.inputEvents == 0 &&
            summary.outputEvents == 0 && summary.pairComparisons == 0,
        "An empty scope must explicitly produce no events");
}

void sparseFrequencyIndexAvoidsCartesianLimit() {
  std::vector<DetectionRefinedEvent> input;
  for (std::size_t i = 0; i < 3072; ++i)
    input.push_back(event("sparse-" + std::to_string(i), i, 0, 10,
                          static_cast<double>(i * 10), static_cast<double>(i * 10 + 1)));
  auto config = options();
  config.maximumPairComparisons = 1;
  std::vector<DetectionRefinedFusion> output;
  DetectionRefinedFusionSummary summary;
  std::string error;
  check(fuseRefinedDetectionEvents(input, config, output, summary, error) &&
            output.size() == input.size() && summary.pairComparisons == 0 &&
            summary.mergedPairs == 0,
        "Disjoint frequency intervals must not spend the 4.7M Cartesian comparison budget");
}
}  // namespace

int main() {
  try {
    expandedDifferentParentsKeepBothLineages();
    positiveSampleGapsNeverBridgeButTouchMerges();
    frequencyContactAndDisjointBandsStaySeparate();
    containmentRequiresThePhysicalCenterLimit();
    deterministicBestPairChangesAChainOutcome();
    rankingUsesSimilarityThenCenterThenTime();
    qualitiesAndOutputOrderRemainPhysical();
    budgetsAndCancellationAreAtomic();
    validationIsStrictAndEmptyInputIsExplicit();
    sparseFrequencyIndexAvoidsCartesianLimit();
    std::cout << "Detection refined fusion tests: 10 groups passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
