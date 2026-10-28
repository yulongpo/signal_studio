#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "domain/detection_postprocess.h"

using namespace signalstudio;

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
std::uint64_t mirroredStateBytes(std::size_t tracks, std::size_t bands,
                                 std::size_t windows, std::size_t ids,
                                 std::size_t idBytes) {
  return 4096 + std::uint64_t{tracks} * 4096 + std::uint64_t{bands} * 512 +
         std::uint64_t{windows} * 1024 + std::uint64_t{ids} * 256 +
         std::uint64_t{idBytes} * 8;
}
std::uint64_t mirroredIndexBytes(std::size_t count) {
  return 256ull + static_cast<std::uint64_t>(count) * 96ull;
}
FrequencyTrackerConfig trackerConfig(std::uint64_t windows = 4) {
  FrequencyTrackerConfig config;
  config.runId = "run-A";
  config.fileId = "file-A";
  config.inputBins = 256;
  config.firstBinHz = 0;
  config.binWidthHz = 1;
  config.totalAnalysisWindows = windows;
  config.minHits = 1;
  config.maxMiss = 1;
  config.matchIou = .3;
  config.mergeToleranceHz = 0;
  config.containmentCenterToleranceHz = 0;
  config.fragmentGapSamples = 0;
  return config;
}
DecodedCandidate band(double lower, double upper, double score = .8,
                      std::size_t peak = 1, double firstBinHz = 0) {
  return {lower - firstBinHz, upper - firstBinHz, score, peak};
}
WindowObservation observation(std::uint64_t ordinal,
                              std::vector<DecodedCandidate> candidates,
                              TimeRange nominal = {}, bool customTime = false) {
  if (!customTime) nominal = {ordinal * 10, ordinal * 10 + 10};
  const TimeRange actual{nominal.begin, nominal.end + 1};
  return {"run-A", "file-A", "window-" + std::to_string(ordinal), ordinal,
          nominal, actual, std::move(candidates)};
}
FrequencyTracker openTracker(const FrequencyTrackerConfig& config) {
  FrequencyTracker tracker;
  std::string error;
  require(tracker.reset(config, error), error);
  return tracker;
}
void consume(FrequencyTracker& tracker, const WindowObservation& window) {
  std::string error;
  require(tracker.consume(window, error), error.empty() ? "consume failed" : error);
}
std::vector<FrequencyTrack> finish(FrequencyTracker& tracker) {
  std::vector<FrequencyTrack> result;
  std::string error;
  require(tracker.finish(result, error), error.empty() ? "finish failed" : error);
  return result;
}
std::string seedFor(const std::string& windowId, std::size_t peak) {
  const auto part = [](const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
  };
  return "frequency-track:" + part("run-A") + part("file-A") + part(windowId) +
         ":" + std::to_string(peak);
}
struct ReferenceTrack {
  FrequencyRange frequency;
  std::string seed;
  std::size_t peak = 0;
  int bands = 1;
};
bool sameCarrierReference(FrequencyRange a, FrequencyRange b, double matchIou,
                          double centerTolerance) {
  const double intersection =
      std::min(a.upperHz, b.upperHz) - std::max(a.lowerHz, b.lowerHz);
  if (intersection <= 0) return false;
  const double narrow =
      std::max(1e-9, std::min(a.upperHz - a.lowerHz, b.upperHz - b.lowerHz));
  const double containment = intersection / narrow;
  const double iou = frequencyIntersectionOverUnion(a, b);
  const double distance =
      std::abs((a.lowerHz + a.upperHz - b.lowerHz - b.upperHz) * .5);
  const double centerLimit = narrow * .5 + centerTolerance;
  return iou >= matchIou || (containment >= matchIou && distance <= centerLimit);
}
std::vector<ReferenceTrack> cartesianFuse(
    const std::vector<DecodedCandidate>& candidates, double matchIou,
    double mergeTolerance, double centerTolerance, TimeRange nominal) {
  std::vector<ReferenceTrack> tracks;
  for (const auto& candidate : candidates) {
    tracks.push_back({{candidate.lowerBin, candidate.upperBin},
                      seedFor("window-0", candidate.peakIndex),
                      candidate.peakIndex, 1});
  }
  const auto pass = [&](bool fragments) {
    while (tracks.size() > 1) {
      std::optional<std::tuple<double, std::uint64_t, std::uint64_t, double, double,
                               std::uint64_t, std::size_t, std::size_t>>
          best;
      for (std::size_t i = 0; i + 1 < tracks.size(); ++i) {
        for (std::size_t j = i + 1; j < tracks.size(); ++j) {
          const auto begin = nominal.begin;
          const auto end = nominal.end;
          double frequencyGap = 0, similarity = 0, centerDistance = 0;
          std::uint64_t sampleGap = 0, overlapRank = 0, earliest = begin;
          if (fragments) {
            const auto matchA = tracks[i].frequency;
            const auto matchB = tracks[j].frequency;
            if (!sameCarrierReference(matchA, matchB, matchIou, centerTolerance))
              continue;
            const double intersection =
                std::min(matchA.upperHz, matchB.upperHz) -
                std::max(matchA.lowerHz, matchB.lowerHz);
            const double narrow = std::max(
                1e-9, std::min(matchA.upperHz - matchA.lowerHz,
                               matchB.upperHz - matchB.lowerHz));
            similarity = -std::max(frequencyIntersectionOverUnion(matchA, matchB),
                                   intersection / narrow);
            centerDistance =
                std::abs((matchA.lowerHz + matchA.upperHz - matchB.lowerHz -
                          matchB.upperHz) *
                         .5);
          } else {
            const double gap =
                std::max(tracks[i].frequency.lowerHz, tracks[j].frequency.lowerHz) -
                std::min(tracks[i].frequency.upperHz, tracks[j].frequency.upperHz);
            if (begin >= end || gap > mergeTolerance) continue;
            frequencyGap = gap;
            overlapRank = std::numeric_limits<std::uint64_t>::max() - (end - begin);
          }
          const auto key = std::tuple{frequencyGap, sampleGap, overlapRank, similarity,
                                      centerDistance, earliest, i, j};
          if (!best || key < *best) best = key;
        }
      }
      if (!best) break;
      const auto i = std::get<6>(*best), j = std::get<7>(*best);
      if (std::tuple<std::uint64_t, std::size_t, std::string>{
              0, tracks[j].peak, tracks[j].seed} <
          std::tuple<std::uint64_t, std::size_t, std::string>{
              0, tracks[i].peak, tracks[i].seed})
        tracks[i].seed = tracks[j].seed;
      tracks[i].peak = std::min(tracks[i].peak, tracks[j].peak);
      tracks[i].frequency = {std::min(tracks[i].frequency.lowerHz,
                                      tracks[j].frequency.lowerHz),
                             std::max(tracks[i].frequency.upperHz,
                                      tracks[j].frequency.upperHz)};
      tracks[i].bands += tracks[j].bands;
      tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(j));
    }
  };
  if (mergeTolerance > 0) pass(false);
  pass(true);
  std::sort(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) {
    return std::tuple{a.frequency.lowerHz, a.frequency.upperHz, a.seed} <
           std::tuple{b.frequency.lowerHz, b.frequency.upperHz, b.seed};
  });
  return tracks;
}
void expectPartition(const FrequencyTrackerConfig& config,
                     const std::vector<DecodedCandidate>& candidates,
                     const std::string& label) {
  auto tracker = openTracker(config);
  consume(tracker, observation(0, candidates));
  auto result = finish(tracker);
  const auto expected = cartesianFuse(candidates, config.matchIou,
                                      config.mergeToleranceHz,
                                      config.containmentCenterToleranceHz,
                                      {0, 10});
  require(result.size() == expected.size(), label + " partition size");
  for (std::size_t i = 0; i < result.size(); ++i) {
    require(result[i].frequency == expected[i].frequency, label + " frequency");
    require(result[i].seedId == expected[i].seed, label + " seed " + result[i].seedId);
    require(result[i].hits == 1, label + " hits");
    require(result[i].windows.size() == 1 &&
                result[i].windows[0].bands.size() ==
                    static_cast<std::size_t>(expected[i].bands),
            label + " band count");
  }
}
void exhaustiveFusionMatchesCartesianOracle() {
  const std::vector<std::vector<DecodedCandidate>> scenes{
      {band(0, 10, .8, 1), band(10, 20, .8, 2), band(20, 30, .8, 3),
       band(40, 50, .8, 4)},
      {band(0, 10, .8, 1), band(1, 11, .8, 2), band(2, 12, .8, 3),
       band(3, 13, .8, 4)},
      {band(0, 10, .8, 4), band(10, 20, .8, 3), band(20, 30, .8, 2),
       band(30, 40, .8, 1)},
      {band(0, 100, .8, 1), band(10, 20, .8, 8), band(40, 48, .8, 3),
       band(70, 90, .8, 5)},
      {band(0, 10, .8, 2), band(12, 22, .8, 6), band(24, 34, .8, 1)}};
  for (double tolerance : {0.0, 2.0, 5.0, 50.0}) {
    for (double matchIou : {0.0, 0.3, 0.9}) {
      auto config = trackerConfig(1);
      config.matchIou = matchIou;
      config.mergeToleranceHz = tolerance;
      config.containmentCenterToleranceHz = tolerance == 0 ? 0 : 2;
      for (std::size_t scene = 0; scene < scenes.size(); ++scene)
        expectPartition(config, scenes[scene],
                        "scene " + std::to_string(scene) + " tol " +
                            std::to_string(tolerance));
    }
  }
}
void fullOverlapCountsEveryPairOnce() {
  auto config = trackerConfig(1);
  config.mergeToleranceHz = 5;
  auto tracker = openTracker(config);
  consume(tracker, observation(0, {band(0, 10, .8, 1), band(1, 11, .8, 2),
                                   band(2, 12, .8, 3), band(3, 13, .8, 4)}));
  require(tracker.frameStats().comparisons == 10,
          "four mutual overlaps merge in 6+3+1 comparisons, got " +
              std::to_string(tracker.frameStats().comparisons));
  const auto result = finish(tracker);
  require(result.size() == 1 && result[0].frequency == FrequencyRange{0, 13} &&
              result[0].windows[0].bands.size() == 4,
          "mutual overlap still collapses to one envelope");
}
void touchingChainRecomputesIntervalsAfterEachMerge() {
  auto config = trackerConfig(1);
  config.mergeToleranceHz = 2;
  auto tracker = openTracker(config);
  consume(tracker, observation(0, {band(0, 10, .8, 5), band(12, 22, .8, 4),
                                   band(24, 34, .8, 1)}));
  const auto result = finish(tracker);
  require(result.size() == 1 && result[0].frequency == FrequencyRange{0, 34} &&
              result[0].seedId == seedFor("window-0", 1),
          "a gap chain must merge again after the survivor interval grows");
  require(tracker.frameStats().comparisons == 3,
          "the far pair is irrelevant until the survivor grows, got " +
              std::to_string(tracker.frameStats().comparisons));
}
void toleranceAndTimeBoundariesKeepTheOldPredicate() {
  auto touching = trackerConfig(1);
  touching.mergeToleranceHz = 5;
  auto merged = openTracker(touching);
  consume(merged, observation(0, {band(0, 10, .8, 1), band(15, 25, .8, 2)}));
  require(finish(merged).size() == 1 &&
              finish(merged)[0].frequency == FrequencyRange{0, 25},
          "a frequency gap equal to the tolerance still merges");
  auto separated = openTracker(touching);
  const double lower = std::nextafter(15.0, 100.0);
  consume(separated, observation(0, {band(0, 10, .8, 1), band(lower, lower + 10, .8, 2)}));
  require(finish(separated).size() == 2,
          "the next representable gap above the tolerance stays split");

  auto timed = trackerConfig(2);
  timed.mergeToleranceHz = 5;
  timed.matchIou = .9;
  auto overlapped = openTracker(timed);
  consume(overlapped, observation(0, {band(0, 10, .8, 1)}, {0, 10}, true));
  consume(overlapped, observation(1, {band(12, 22, .8, 2)}, {9, 19}, true));
  require(finish(overlapped).size() == 1 &&
              finish(overlapped)[0].frequency == FrequencyRange{0, 22},
          "overlapping nominal time within tolerance merges");
  auto endpoint = openTracker(timed);
  consume(endpoint, observation(0, {band(0, 10, .8, 1)}, {0, 10}, true));
  consume(endpoint, observation(1, {band(12, 22, .8, 2)}, {10, 20}, true));
  require(finish(endpoint).size() == 2,
          "nominal supports that only touch do not satisfy the overlap pass");
}
void iouThresholdContainmentAndTieOrder() {
  auto exact = trackerConfig(2);
  exact.matchIou = .5;
  exact.maxMiss = 0;
  auto associated = openTracker(exact);
  consume(associated, observation(0, {band(0, 6, .8, 1)}));
  consume(associated, observation(1, {band(2, 8, .8, 2)}));
  const auto mean = finish(associated);
  require(mean.size() == 1 && mean[0].hits == 2 &&
              mean[0].frequency == FrequencyRange{1, 7} &&
              mean[0].windows[1].bands[0].peakIndex == 2,
          "IoU exactly on matchIou associates and publishes the mean");

  auto below = trackerConfig(2);
  below.matchIou = std::nextafter(0.1, 1.0);
  below.maxMiss = 0;
  auto rejected = openTracker(below);
  consume(rejected, observation(0, {band(0, 100, .8, 1)}));
  consume(rejected, observation(1, {band(60, 70, .8, 2)}));
  require(finish(rejected).size() == 2,
          "the next matchIou above an exact IoU neither associates nor merges");
  below.matchIou = 0.1;
  auto inclusive = openTracker(below);
  consume(inclusive, observation(0, {band(0, 100, .8, 1)}));
  consume(inclusive, observation(1, {band(60, 70, .8, 2)}));
  const auto included = finish(inclusive);
  require(included.size() == 1 && included[0].frequency == FrequencyRange{30, 85},
          "IoU equality is inclusive for a contained off-center candidate");

  auto contained = trackerConfig(2);
  contained.firstBinHz = -128;
  contained.maxMiss = 0;
  contained.containmentCenterToleranceHz = 2;
  auto aligned = openTracker(contained);
  consume(aligned, observation(0, {band(-40, 40, .8, 1, -128)}));
  consume(aligned, observation(1, {band(-5, 5, .8, 2, -128)}));
  require(finish(aligned).size() == 1 &&
              finish(aligned)[0].frequency == FrequencyRange{-40, 40},
          "centered containment still reconnects after strict IoU misses");
  auto offset = openTracker(contained);
  consume(offset, observation(0, {band(-40, 40, .8, 1, -128)}));
  consume(offset, observation(1, {band(25, 35, .8, 2, -128)}));
  require(finish(offset).size() == 2,
          "off-center containment stays two carriers");

  auto ties = trackerConfig(2);
  ties.matchIou = 0;
  ties.maxMiss = 1;
  auto lowerIndex = openTracker(ties);
  consume(lowerIndex, observation(0, {band(0, 10, .8, 1), band(100, 110, .8, 2)}));
  consume(lowerIndex, observation(1, {band(50, 60, .4, 3)}));
  const auto tied = finish(lowerIndex);
  require(tied.size() == 2, "zero-threshold tie must not collapse disjoint carriers");
  const auto winner = std::find_if(tied.begin(), tied.end(), [](const auto& track) {
    return track.hits == 2;
  });
  require(winner != tied.end() && winner->seedId == seedFor("window-0", 1) &&
              winner->frequency == FrequencyRange{25, 35} &&
              winner->windows[1].bands[0].peakIndex == 3,
          "equal IoU and score keep the lower track index");

  auto scores = trackerConfig(2);
  scores.matchIou = 0.1;
  scores.maxMiss = 1;
  auto byScore = openTracker(scores);
  consume(byScore, observation(0, {band(0, 100, .8, 1)}));
  consume(byScore, observation(1, {band(0, 10, .4, 2), band(90, 100, .9, 8)}));
  const auto scored = finish(byScore);
  require(scored.size() == 2, "the losing equal-IoU candidate stays its own track");
  const auto scoredWinner =
      std::find_if(scored.begin(), scored.end(), [](const auto& track) {
        return track.hits == 2;
      });
  require(scoredWinner != scored.end() &&
              scoredWinner->windows[1].bands[0].peakIndex == 8,
          "higher candidate score beats a lower candidate index");
  auto byIndex = openTracker(scores);
  consume(byIndex, observation(0, {band(0, 100, .8, 1)}));
  consume(byIndex, observation(1, {band(0, 10, .8, 2), band(90, 100, .8, 8)}));
  const auto indexed = finish(byIndex);
  const auto indexWinner =
      std::find_if(indexed.begin(), indexed.end(), [](const auto& track) {
        return track.hits == 2;
      });
  require(indexWinner != indexed.end() &&
              indexWinner->windows[1].bands[0].peakIndex == 2,
          "equal score keeps the earlier candidate index");
}
void zeroThresholdKeepsDisjointPairsUntilTheCap() {
  auto config = trackerConfig(2);
  config.matchIou = 0;
  config.maxMiss = 1;
  auto kept = openTracker(config);
  consume(kept, observation(0, {band(0, 10, .8, 1)}));
  consume(kept, observation(1, {band(80, 90, .8, 4)}));
  const auto glued = finish(kept);
  require(glued.size() == 1 && glued[0].hits == 2,
          "matchIou zero associates a disjoint candidate instead of spawning one");

  config.limits.maximumAssociationComparisons = 1;
  auto capped = openTracker(config);
  consume(capped, observation(0, {band(0, 10, .8, 1), band(40, 50, .8, 2)}));
  std::string error;
  require(!capped.consume(observation(1, {band(80, 90, .8, 4)}), error) &&
              error == "association-comparison-limit-exceeded" &&
              capped.processedWindows() == 1 && finish(capped).size() == 2,
          "matchIou zero still fails when every disjoint pair exceeds the cap: " +
              error);
}
void relevantOverlapStillFailsAtomically() {
  auto config = trackerConfig(2);
  config.limits.maximumFusionComparisons = 1;
  auto first = openTracker(config);
  std::string error;
  require(!first.consume(observation(0, {band(0, 10, .8, 1), band(9, 19, .8, 2),
                                         band(18, 28, .8, 3)}),
                         error) &&
              error.rfind("frequency-fusion-comparison-limit-exceeded", 0) == 0 &&
              error.find("open=") != std::string::npos &&
              error.find("sealed=") != std::string::npos &&
              error.find("comparisons=") != std::string::npos &&
              first.processedWindows() == 0 && first.retainedTracks() == 0 &&
              finish(first).empty(),
          "overlapping candidates that exceed the fusion cap roll back: " + error);

  auto prior = openTracker(config);
  consume(prior, observation(0, {band(0, 10, .8, 1)}));
  require(!prior.consume(observation(1, {band(100, 130, .8, 2), band(110, 140, .8, 3),
                                         band(120, 150, .8, 4)}),
                         error) &&
              error.rfind("frequency-fusion-comparison-limit-exceeded", 0) == 0 &&
              prior.processedWindows() == 1,
          "a later overlapping frame must not replace the committed frame: " + error);
  const auto kept = finish(prior);
  require(kept.size() == 1 && kept[0].hits == 1 &&
              kept[0].frequency == FrequencyRange{0, 10},
          "rollback restores the formal track from the previous window");

  config.limits.maximumFusionComparisons = 4000000;
  config.limits.maximumAssociationComparisons = 1;
  auto association = openTracker(config);
  consume(association, observation(0, {band(0, 100, .8, 1)}));
  require(!association.consume(observation(1, {band(0, 20, .8, 2), band(40, 60, .8, 3)}),
                               error) &&
              error == "association-comparison-limit-exceeded" &&
              association.processedWindows() == 1,
          "two strict overlaps still exhaust the association cap: " + error);
}
void indexBytesUseTheResidentBudgetAndDoNotPublishPartialTracks() {
  const std::string firstId = "window-0";
  const auto firstBase = mirroredStateBytes(2, 2, 2, 1, firstId.size() * 3);
  auto failing = trackerConfig(1);
  failing.limits.residentByteBudget = firstBase;
  auto rejected = openTracker(failing);
  std::string error;
  require(!rejected.consume(observation(0, {band(0, 10, .8, 1), band(30, 40, .8, 2)}),
                            error) &&
              error == "tracker-resident-byte-budget-exceeded" &&
              rejected.processedWindows() == 0 && finish(rejected).empty(),
          "the fusion index is charged before a two-track frame commits: " + error);
  auto passing = failing;
  passing.limits.residentByteBudget = firstBase + mirroredIndexBytes(2);
  auto accepted = openTracker(passing);
  consume(accepted, observation(0, {band(0, 10, .8, 1), band(30, 40, .8, 2)}));
  require(finish(accepted).size() == 2,
          "a budget that includes the two-track index still publishes both");

  const auto secondBase = mirroredStateBytes(2, 2, 2, 2, firstId.size() * 2 + 8 * 2);
  auto narrow = trackerConfig(2);
  narrow.limits.residentByteBudget = secondBase;
  auto rollback = openTracker(narrow);
  consume(rollback, observation(0, {band(0, 10, .8, 1)}));
  require(!rollback.consume(observation(1, {band(100, 110, .8, 2)}), error) &&
              error == "tracker-resident-byte-budget-exceeded" &&
              rollback.processedWindows() == 1 && finish(rollback).size() == 1 &&
              finish(rollback)[0].hits == 1,
          "an association index that does not fit leaves the formal track: " + error);
}
void previewCapDoesNotRemoveFormalTracks() {
  auto config = trackerConfig(4);
  config.limits.maximumPreviewEvents = 2;
  auto tracker = openTracker(config);
  for (std::uint64_t ordinal = 0; ordinal < 4; ++ordinal)
    consume(tracker, observation(ordinal, {band(0, 10, .8, 1)}));
  require(tracker.previewEvents().size() == 2 && tracker.droppedPreviewEvents() >= 2,
          "preview log eviction still drops the oldest events");
  const auto result = finish(tracker);
  require(result.size() == 1 && result[0].hits == 4,
          "preview eviction must not remove the formal track");
  require(!tracker.visiblePreviewEvents().empty(),
          "visible preview geometry remains after log eviction");
}
void sparseOpenSetStaysUnderTheRawComparisonCap() {
  constexpr std::size_t perWindow = 512;
  constexpr std::uint64_t windows = 6;
  constexpr std::uint64_t openBeforeMerge = perWindow * windows;
  const auto rawPairs = openBeforeMerge * (openBeforeMerge - 1) / 2;
  require(rawPairs > 4000000, "this layout used to exceed the raw fusion cap");
  auto config = trackerConfig(windows);
  config.inputBins = 16384;
  config.maxMiss = 1000000;
  config.mergeToleranceHz = 0;
  config.matchIou = .3;
  auto tracker = openTracker(config);
  for (std::uint64_t ordinal = 0; ordinal < windows; ++ordinal) {
    std::vector<DecodedCandidate> candidates;
    candidates.reserve(perWindow);
    const double base = static_cast<double>(ordinal * perWindow * 4);
    for (std::size_t index = 0; index < perWindow; ++index) {
      double lower = base + static_cast<double>(index) * 4;
      double upper = lower + 2;
      if (ordinal + 1 == windows && index == 11) {
        lower = base + 41;
        upper = base + 43;
      }
      candidates.push_back(band(lower, upper, .8, index));
    }
    consume(tracker, observation(ordinal, std::move(candidates)));
  }
  const auto stats = tracker.frameStats();
  require(stats.comparisons == 1 && stats.openTracks == openBeforeMerge - 1,
          "only the one overlapping pair is compared, got comparisons " +
              std::to_string(stats.comparisons) + " open " +
              std::to_string(stats.openTracks));
  const auto result = finish(tracker);
  require(result.size() == openBeforeMerge - 1, "sparse carriers stay formal tracks");
  const auto merged = std::find_if(result.begin(), result.end(), [](const auto& track) {
    return track.windows.size() == 1 && track.windows[0].bands.size() == 2;
  });
  require(merged != result.end() && merged->hits == 1,
          "the single overlapping pair still merges");
  std::size_t singles = 0;
  for (const auto& track : result)
    if (track.hits == 1 && track.windows.size() == 1 && track.windows[0].bands.size() == 1)
      ++singles;
  require(singles == openBeforeMerge - 2, "non-overlapping carriers are not dropped");
}
}  // namespace

int main() {
  try {
    exhaustiveFusionMatchesCartesianOracle();
    fullOverlapCountsEveryPairOnce();
    touchingChainRecomputesIntervalsAfterEachMerge();
    toleranceAndTimeBoundariesKeepTheOldPredicate();
    iouThresholdContainmentAndTieOrder();
    zeroThresholdKeepsDisjointPairsUntilTheCap();
    relevantOverlapStillFailsAtomically();
    indexBytesUseTheResidentBudgetAndDoNotPublishPartialTracks();
    previewCapDoesNotRemoveFormalTracks();
    sparseOpenSetStaysUnderTheRawComparisonCap();
    std::cout << "SS-DE-RPO tracking broad-phase tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
