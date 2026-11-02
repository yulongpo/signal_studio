#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "domain/detection_postprocess.h"

using namespace signalstudio;

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
struct Heads {
  std::vector<float> hm, bw, off;
  explicit Heads(std::size_t inputBins)
      : hm(inputBins / 4), bw(inputBins / 4, 1), off(inputBins / 4) {}
  HeadView view(DetectionDecoderKind kind) const {
    return {hm, bw, off,
            kind == DetectionDecoderKind::ScnThreeBin
                ? std::array<std::size_t, 3>{hm.size(), 1, 1}
                : std::array<std::size_t, 3>{1, 1, hm.size()}};
  }
};
std::vector<DecodedCandidate> decode(
    const Heads& heads, DetectionDecoderConfig config,
    DetectionDecodeSummary* summary = nullptr) {
  std::string error;
  std::vector<DecodedCandidate> result;
  DetectionDecodeSummary stats;
  check(decodeDetectionHeads(heads.view(config.kind), config, result, stats,
                             error),
        error.c_str());
  if (summary) *summary = stats;
  return result;
}
DetectionDecoderConfig decoder(
    DetectionDecoderKind kind = DetectionDecoderKind::FfscnSevenBin) {
  DetectionDecoderConfig config;
  config.inputBins = 256;
  config.kind = kind;
  return config;
}
FrequencyTrackerConfig trackerConfig(std::uint64_t windows = 5) {
  FrequencyTrackerConfig config;
  config.runId = "run-A";
  config.fileId = "file-A";
  config.inputBins = 256;
  config.firstBinHz = -128;
  config.binWidthHz = 1;
  config.totalAnalysisWindows = windows;
  config.minHits = 1;
  config.mergeToleranceHz = 0;
  config.containmentCenterToleranceHz = 2;
  return config;
}
DecodedCandidate candidate(double lowerHz, double upperHz, double score = .8,
                           std::size_t peak = 32) {
  return {lowerHz + 128, upperHz + 128, score, peak};
}
WindowObservation window(std::uint64_t ordinal,
                         std::vector<DecodedCandidate> candidates = {}) {
  return {"run-A",
          "file-A",
          "window-" + std::to_string(ordinal),
          ordinal,
          {ordinal * 5, ordinal * 5 + 10},
          {ordinal * 5, ordinal * 5 + 13},
          std::move(candidates)};
}
void consume(FrequencyTracker& tracker, const WindowObservation& observation) {
  std::string error;
  check(tracker.consume(observation, error), error.c_str());
}
std::vector<FrequencyTrack> finish(FrequencyTracker& tracker) {
  std::string error;
  std::vector<FrequencyTrack> result;
  check(tracker.finish(result, error), error.c_str());
  return result;
}
FrequencyTracker tracker(FrequencyTrackerConfig config) {
  FrequencyTracker result;
  std::string error;
  check(result.reset(config, error), error.c_str());
  return result;
}

void scoreAndLocalPeakBoundaries() {
  Heads heads(256);
  heads.hm[8] = .7f;
  check(decode(heads, decoder()).empty(), "FFSCN threshold must be strict");
  auto scn = decoder(DetectionDecoderKind::ScnThreeBin);
  check(decode(heads, scn).size() == 1, "SCN threshold must be inclusive");
  heads.hm[8] = std::nextafter(.7f, 1.f);
  check(decode(heads, decoder()).size() == 1,
        "One ULP over threshold must pass");
  heads.hm[8] = .8f;
  heads.hm[10] = .9f;
  check(decode(heads, decoder()).size() == 1 && decode(heads, scn).size() == 2,
        "Seven-bin and three-bin local peaks must remain distinct");
  heads = Heads(256);
  heads.hm[8] = .8f;
  heads.hm[9] = std::nextafter(.8f, 1.f);
  check(decode(heads, scn).size() == 2,
        "SCN local maximum epsilon accepts sub-1e-6 differences");
}
void nmsChainAndBoundary() {
  Heads heads(256);
  for (std::size_t i : {8, 12, 16}) heads.bw[i] = 8;
  heads.hm[8] = .95f;
  heads.hm[12] = .9f;
  heads.hm[16] = .85f;
  const auto kept = decode(heads, decoder());
  check(kept.size() == 2 && kept[0].peakIndex == 8 && kept[1].peakIndex == 16,
        "Suppressed B must not suppress C in the A-B-C overlap chain");
  heads = Heads(256);
  heads.hm[8] = .9f;
  heads.hm[12] = .8f;
  heads.bw[8] = heads.bw[12] = 12;
  auto main = decoder();
  main.nmsIou = .5;
  auto scn = main;
  scn.kind = DetectionDecoderKind::ScnThreeBin;
  check(decode(heads, main).size() == 2 && decode(heads, scn).size() == 1,
        "IoU exactly on NMS threshold must preserve profile comparisons");
}
void stableDenseTopKAndGeometry() {
  Heads heads(256);
  std::fill(heads.hm.begin(), heads.hm.end(), .8f);
  std::fill(heads.bw.begin(), heads.bw.end(), .2f);
  std::fill(heads.off.begin(), heads.off.end(), .5f);
  auto config = decoder();
  config.topK = 8;
  DetectionDecodeSummary summary;
  const auto dense = decode(heads, config, &summary);
  check(dense.size() == 8 && summary.qualifyingPeaks == 64 &&
            summary.rankedPeaks == 8 && summary.topKTruncated,
        "Dense plateau truncation must be explicit");
  for (std::size_t i = 0; i < dense.size(); ++i)
    check(dense[i].peakIndex == i,
          "Equal TopK scores must retain ascending indices");
  check(near(dense[0].lowerBin, 1.6) && near(dense[0].upperBin, 2.4),
        "Offsets and widths must be interpreted at stride four");
  heads = Heads(256);
  heads.hm[0] = .99f;
  heads.bw[0] = 20;
  heads.hm[63] = .9f;
  heads.bw[63] = 20;
  const auto edge = decode(heads, decoder());
  check(edge.size() == 2 && edge[0].lowerBin == 0 && edge[1].upperBin == 255,
        "FFT input endpoints must clip to zero through N-1");
  heads = Heads(256);
  heads.hm[8] = .95f;
  heads.hm[16] = .9f;
  heads.bw[8] = 0;
  config.topK = 1;
  check(
      decode(heads, config, &summary).empty() && summary.invalidWidthCount == 1,
      "Invalid width consumes its TopK rank before candidate validation");
}
void invalidHeadsAndCancellationAreAtomic() {
  Heads heads(256);
  auto config = decoder();
  std::string error;
  const std::vector<DecodedCandidate> sentinel{{1, 2, .8, 0}};
  auto result = sentinel;
  DetectionDecodeSummary summary;
  summary.qualifyingPeaks = 987;
  auto view = heads.view(config.kind);
  view.sampleShape = {64, 1, 1};
  check(!decodeDetectionHeads(view, config, result, summary, error) &&
            result == sentinel && summary.qualifyingPeaks == 987,
        "Wrong output tensor layout cannot publish candidates");
  for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(), -1.f, 1.1f}) {
    heads.hm[0] = invalid;
    check(!decodeDetectionHeads(heads.view(config.kind), config, result,
                                summary, error) &&
              result == sentinel,
          "Invalid activated head must reject the entire batch member");
  }
  heads = Heads(256);
  heads.bw[2] = -1;
  check(!decodeDetectionHeads(heads.view(config.kind), config, result, summary,
                              error),
        "Negative bandwidth must not be ignored");
  heads.bw[2] = 1;
  heads.off[0] = 2;
  check(!decodeDetectionHeads(heads.view(config.kind), config, result, summary,
                              error),
        "Offsets are already activated");
  heads.off[0] = 0;
  heads.hm[0] = .8f;
  heads.bw[0] = std::numeric_limits<float>::max();
  check(!decodeDetectionHeads(heads.view(config.kind), config, result, summary,
                              error) &&
            result == sentinel,
        "Finite heads whose float32 decoded widths overflow must reject "
        "publication");
  heads.bw[0] = 1;
  heads.bw.pop_back();
  check(!decodeDetectionHeads(heads.view(config.kind), config, result, summary,
                              error),
        "Every head must have the declared output length");
  heads.bw.push_back(1);
  check(!decodeDetectionHeads(heads.view(config.kind), config, result, summary,
                              error, [] { return true; }) &&
            result == sentinel,
        "Cancellation must keep output unchanged");
  config.maximumInputBins = 128;
  check(!decodeDetectionHeads(heads.view(config.kind), config, result, summary,
                              error),
        "Decoder input resource bound must be enforced");
}
void uniqueWindowStatisticsAndStableScope() {
  {
    Heads padded(2048);
    padded.hm[7] = .9f;
    padded.hm[8] = .95f;
    auto configuration = decoder();
    configuration.inputBins = 2048;
    configuration.validInputBins = 32;
    configuration.topK = 1;
    const auto found = decode(padded, configuration);
    check(found.size() == 1 && found.front().peakIndex == 7,
          "Padding must not suppress a physical peak before TopK/local maxima");
  }
  {
    auto mapped = trackerConfig(1);
    mapped.inputBins = 3001;
    mapped.peakIndexLimit = 8192;
    mapped.firstBinHz = 0;
    auto subject = tracker(mapped);
    auto observation = window(0);
    observation.candidates = {{1.25, 1.5, .8, 100}, {1.51, 1.75, .9, 101}};
    consume(subject, observation);
    check(!finish(subject).empty(),
          "Distinct model peaks survive an irregular analysis grid and shared "
          "integer bins");
  }
  auto config = trackerConfig(3);
  auto first = tracker(config);
  consume(first, window(0, {candidate(0, 20, .75)}));
  consume(first, window(1, {candidate(2, 22, .95)}));
  consume(first, window(2));
  const auto result = finish(first);
  check(
      result.size() == 1 && result[0].hits == 2 &&
          result[0].windowDenominator == 3 &&
          near(result[0].windowCoverage, 2. / 3) &&
          near(result[0].scoreMean, .85) && near(result[0].scoreMax, .95),
      "Mean, maximum, hit count and coverage must use the same unique windows");
  check(result[0].frequency == FrequencyRange{1, 21} &&
            result[0].originalFrequencyEnvelope == FrequencyRange{0, 22} &&
            result[0].nominalSupport == TimeRange{0, 15} &&
            result[0].actualSupport == TimeRange{0, 18},
        "Original frequency edges and actual observation support must survive");
  auto second = tracker(config);
  consume(second, window(0, {candidate(0, 20, .75)}));
  consume(second, window(1, {candidate(2, 22, .95)}));
  consume(second, window(2));
  check(finish(second)[0].seedId == result[0].seedId,
        "Replay must reproduce the stable track seed");
  std::string error;
  auto scoped = tracker(config);
  consume(scoped, window(0, {candidate(0, 20)}));
  auto retry = window(0, {candidate(0, 20)});
  check(!scoped.consume(retry, error) && scoped.processedWindows() == 1,
        "Duplicate windows cannot inflate scores or hits");
  auto foreign = window(1, {candidate(0, 20)});
  foreign.runId = "run-B";
  check(!scoped.consume(foreign, error), "Tracks cannot cross run boundaries");
  foreign.runId = config.runId;
  foreign.fileId = "file-B";
  check(!scoped.consume(foreign, error), "Tracks cannot cross file boundaries");
  foreign.fileId = config.fileId;
  foreign.windowId = retry.windowId;
  check(!scoped.consume(foreign, error),
        "Window IDs must remain globally unique");
}
void maxMissMinHitsAndReconnection() {
  auto config = trackerConfig(5);
  config.minHits = 2;
  auto kept = tracker(config);
  consume(kept, window(0, {candidate(0, 20)}));
  consume(kept, window(1));
  consume(kept, window(2, {candidate(0, 20)}));
  check(finish(kept).size() == 1 && finish(kept)[0].hits == 2,
        "A single missed window must reconnect before maxMiss is exceeded");
  auto dropped = tracker(config);
  consume(dropped, window(0, {candidate(0, 20)}));
  consume(dropped, window(1));
  consume(dropped, window(2));
  consume(dropped, window(3, {candidate(0, 20)}));
  check(finish(dropped).empty(),
        "Sub-minHits tracks are filtered before fusion");
  auto shortConfig = trackerConfig(1);
  shortConfig.minHits = 2;
  auto shortFile = tracker(shortConfig);
  consume(shortFile, window(0, {candidate(0, 20)}));
  check(finish(shortFile).size() == 1,
        "One eligible-window files use the frozen effective minHits rule");
  config.minHits = 1;
  config.maxMiss = 0;
  auto split = tracker(config);
  consume(split, window(0, {candidate(0, 20)}));
  consume(split, window(1));
  consume(split, window(2));
  consume(split, window(3, {candidate(0, 20)}));
  check(finish(split).size() == 2,
        "Separate observation contexts cannot fill a silent gap implicitly");
  config.fragmentGapSamples = 5;
  auto stitched = tracker(config);
  consume(stitched, window(0, {candidate(0, 20)}));
  consume(stitched, window(1));
  consume(stitched, window(2));
  consume(stitched, window(3, {candidate(0, 20)}));
  check(finish(stitched).size() == 1 && finish(stitched)[0].windows.size() == 2,
        "Explicit fragment gap joins context while retaining only two hit "
        "windows");
}
void globalOneToOneAndContainment() {
  auto config = trackerConfig(2);
  config.containmentCenterToleranceHz = 0;
  auto online = tracker(config);
  consume(online,
          window(0, {candidate(0, 20, .8, 30), candidate(14, 34, .8, 40)}));
  consume(online,
          window(1, {candidate(8, 28, .99, 35), candidate(0, 20, .8, 30)}));
  const auto result = finish(online);
  check(result.size() == 2 && result[0].hits == 2 && result[1].hits == 2 &&
            result[0].frequency == FrequencyRange{0, 20} &&
            result[1].frequency == FrequencyRange{11, 31},
        "A weaker overlap cannot steal the best admissible one-to-one match");
  config.maxMiss = 0;
  config.fragmentGapSamples = 0;
  config.containmentCenterToleranceHz = 2;
  auto contained = tracker(config);
  consume(contained, window(0, {candidate(-40, 40)}));
  consume(contained, window(1, {candidate(-5, 5)}));
  const auto merged = finish(contained);
  check(merged.size() == 1 && merged[0].frequency == FrequencyRange{-40, 40},
        "Aligned narrow/wide containment reconnects fragments after online "
        "strict IoU");
  auto offset = tracker(config);
  consume(offset, window(0, {candidate(-40, 40)}));
  consume(offset, window(1, {candidate(25, 35)}));
  check(finish(offset).size() == 2,
        "Off-center contained signal must not be silently identified as the "
        "same carrier");
}
void frequencyFusionKeepsAllRawEvidence() {
  auto config = trackerConfig(2);
  config.mergeToleranceHz = 3;
  auto fused = tracker(config);
  consume(fused,
          window(0, {candidate(0, 10, .75, 32), candidate(12, 22, .95, 36)}));
  consume(fused,
          window(1, {candidate(0, 10, .8, 32), candidate(12, 22, .9, 36)}));
  const auto result = finish(fused);
  check(
      result.size() == 1 && result[0].frequency == FrequencyRange{0, 22} &&
          result[0].hits == 2 && result[0].windows[0].bands.size() == 2 &&
          result[0].windows[1].bands.size() == 2 &&
          near(result[0].scoreMean, .925) && near(result[0].windowCoverage, 1),
      "Fusion must retain all original bands but count each window only once");
  config.maxMiss = 0;
  config.totalAnalysisWindows = 4;
  auto disjoint = tracker(config);
  consume(disjoint, window(0, {candidate(0, 10)}));
  consume(disjoint, window(1));
  consume(disjoint, window(2));
  consume(disjoint, window(3, {candidate(12, 22)}));
  check(finish(disjoint).size() == 2,
        "Frequency-near but time-disjoint contexts must not merge as multitone "
        "evidence");
}
void trackingLimitsAndCancellationAreAtomic() {
  auto config = trackerConfig(2);
  config.limits.maximumCandidatesPerWindow = 1;
  auto bounded = tracker(config);
  std::string error;
  check(!bounded.consume(
            window(0, {candidate(0, 10, .8, 32), candidate(20, 30, .8, 40)}),
            error) &&
            bounded.processedWindows() == 0,
        "Candidate overflow must fail before mutating tracker");
  check(!bounded.consume(window(0, {candidate(0, 10)}), error,
                         [] { return true; }) &&
            bounded.processedWindows() == 0,
        "Cancelled consume must be retryable");
  unsigned cancellationChecks = 0;
  check(!bounded.consume(window(0, {candidate(0, 10)}), error,
                         [&] { return ++cancellationChecks == 3; }) &&
            bounded.processedWindows() == 0 && bounded.retainedTracks() == 0,
        "Cancellation after constructing tentative tracks must remain atomic");
  consume(bounded, window(0, {candidate(0, 10)}));
  std::vector<FrequencyTrack> sentinel(1);
  sentinel[0].seedId = "unchanged";
  check(!bounded.finish(sentinel, error, [] { return true; }) &&
            sentinel[0].seedId == "unchanged",
        "Cancelled finish cannot replace existing output");
  config = trackerConfig(2);
  config.limits.maximumEvidenceBands = 1;
  auto evidenceBound = tracker(config);
  consume(evidenceBound, window(0, {candidate(0, 10)}));
  check(!evidenceBound.consume(window(1, {candidate(0, 10)}), error) &&
            evidenceBound.processedWindows() == 1 &&
            finish(evidenceBound)[0].hits == 1,
        "Cumulative evidence limit must preserve the durable preceding state");
  config = trackerConfig(2);
  config.limits.residentByteBudget = 4096;
  auto byteBound = tracker(config);
  check(!byteBound.consume(window(0, {candidate(0, 10)}), error),
        "Resident byte budget includes transactional copies");
  config = trackerConfig(2);
  config.limits.maximumActiveTracks = 1;
  auto trackBound = tracker(config);
  check(!trackBound.consume(
            window(0, {candidate(0, 10, .8, 32), candidate(40, 50, .8, 44)}),
            error) &&
            trackBound.processedWindows() == 0,
        "Active track overflow cannot publish partial new tracks");
  config = trackerConfig(2);
  config.limits.maximumAssociationPairs = 1;
  auto pairBound = tracker(config);
  // The carriers do not overlap, so this frame's open-set fusion leaves both.
  // The next candidate matches both and must stop at the explicit pair cap.
  consume(pairBound,
          window(0, {candidate(0, 10, .8, 32), candidate(12, 22, .8, 40)}));
  check(!pairBound.consume(window(1, {candidate(2, 20)}), error) &&
            pairBound.processedWindows() == 1,
        "Dense association pairs must be explicitly bounded");
  config = trackerConfig(1);
  config.limits.maximumFusionComparisons = 1;
  auto fusionBound = tracker(config);
  check(!fusionBound.consume(
            window(0, {candidate(0, 10, .8, 16), candidate(9, 19, .8, 32),
                       candidate(18, 28, .8, 48)}),
            error) &&
            fusionBound.processedWindows() == 0 &&
            fusionBound.retainedTracks() == 0 &&
            error.find("frequency-fusion-comparison-limit-exceeded") == 0 &&
            error.find("open=") != std::string::npos &&
            error.find("sealed=") != std::string::npos &&
            error.find("comparisons=") != std::string::npos,
        "An open set whose comparisons exceed the explicit limit of 1 fails "
        "the frame");
  std::vector<FrequencyTrack> published;
  check(fusionBound.finish(published, error) && published.empty(),
        "A rejected open-set frame must not publish partial tracks");
}
void scnMinimumNoiseAndMaximumOnlyEdges() {
  std::vector<float> average(64, 20), maximum(64, 0);
  average[9] = 1;
  average[20] = 9;
  maximum[12] = maximum[17] = 5;
  const std::vector<FrequencyRange> bands{{10, 20}};
  std::string error;
  std::vector<ScnFrequencyRefinement> result;
  check(refineScnFrequencyBands(bands, bands, average, maximum, 0, 1, 3, result,
                                error),
        error.c_str());
  check(result.size() == 1 && result[0].frequency == FrequencyRange{12, 18} &&
            result[0].noiseFloorDb == 1 && result[0].thresholdDb == 4 &&
            result[0].noiseSampleCount == 2 && result[0].applied,
        "SCN must use neighboring minimum, not mean, and only shrink edges");
  average[12] = average[17] = 100;
  maximum[12] = maximum[17] = 0;
  check(refineScnFrequencyBands(bands, bands, average, maximum, 0, 1, 3, result,
                                error) &&
            result[0].frequency == bands[0] &&
            result[0].skippedReason == "no-in-band-bin-meets-threshold",
        "Average in-band peaks must never replace maximum-spectrum evidence");
  maximum[12] = maximum[17] = 4;
  check(refineScnFrequencyBands(bands, bands, average, maximum, 0, 1, 3, result,
                                error) &&
            result[0].frequency == FrequencyRange{12, 18},
        "SCN bandwidth threshold equality must pass");
  const std::vector<FrequencyRange> occupied{{9, 10}, {10, 20}, {20, 21}};
  check(refineScnFrequencyBands(bands, occupied, average, maximum, 0, 1, 3,
                                result, error) &&
            result[0].frequency == bands[0] &&
            result[0].noiseSampleCount == 0 &&
            result[0].skippedReason == "no-unoccupied-out-of-band-samples",
        "All fused occupancy must mask noise points, independently of UI "
        "filtering");
}
void sameSignalKeepsOnePreviewIdentity() {
  auto config = trackerConfig(4);
  config.minHits = 2;
  auto model = tracker(config);
  consume(model, window(0, {candidate(-20, -10)}));
  consume(model, window(1, {candidate(-20, -10)}));
  const auto& events = model.previewEvents();
  check(events.size() >= 3, "Creation, update and stable must be visible");
  check(events[0].phase == PreviewTrackPhase::Created &&
            events[1].phase == PreviewTrackPhase::Updated &&
            events[2].phase == PreviewTrackPhase::Stable,
        "One physical signal must move Created, Updated, then Stable");
  check(events[0].trackId == events[1].trackId &&
            events[1].trackId == events[2].trackId &&
            events[0].revision == 1 && events[1].revision == 2 &&
            events[2].revision == 3 && events[2].sequence > events[1].sequence,
        "Cross-batch updates must keep one track id and increase revision");
  check(!events[0].timeLocalized && !events[2].timeLocalized &&
            model.droppedPreviewEvents() == 0,
        "Unrefined preview must stay time-unlocalized and bounded");
  auto tracks = finish(model);
  check(tracks.size() == 1 && tracks[0].previewId == events[0].trackId,
        "Formal finish must keep the same signal identity");
  std::string error;
  check(model.sealPreview(true, error), error.c_str());
  check(model.previewEvents().back().phase == PreviewTrackPhase::Finalized,
        "Successful seal must finalize the preview");
  check(!model.sealPreview(false, error), "A sealed preview cannot be sealed again");
}
void distinctSignalsDoNotMergeWithoutAnExistingRule() {
  auto config = trackerConfig(3);
  config.minHits = 1;
  config.mergeToleranceHz = 0;
  config.fragmentGapSamples = 0;
  auto model = tracker(config);
  consume(model, window(0, {candidate(-40, -30, .8, 8), candidate(30, 40, .8, 40)}));
  consume(model, window(1, {candidate(-36, -26, .8, 8), candidate(26, 36, .8, 40)}));
  auto tracks = finish(model);
  check(tracks.size() == 2, "Approaching signals must stay distinct");
  std::size_t merged = 0;
  for (const auto& event : model.previewEvents())
    if (event.phase == PreviewTrackPhase::Merged) ++merged;
  check(merged == 0, "No existing association may invent a merge");
  std::string error;
  check(model.sealPreview(false, error), error.c_str());
  check(model.previewEvents().back().phase == PreviewTrackPhase::Closed,
        "Cancellation must seal the preview instead of finalizing it");
  for (const auto& event : model.previewEvents())
    check(event.phase != PreviewTrackPhase::Finalized,
          "A sealed failure must not finalize preview tracks");
}
void previewQueueDropsOldestWithoutTruncatingFormalTracks() {
  auto config = trackerConfig(6);
  config.minHits = 1;
  config.limits.maximumPreviewEvents = 2;
  auto model = tracker(config);
  for (std::uint64_t ordinal = 0; ordinal < 4; ++ordinal)
    consume(model, window(ordinal, {candidate(-5, 5)}));
  check(model.previewEvents().size() == 2 && model.droppedPreviewEvents() >= 2,
        "The preview queue must drop the oldest events and count them");
  auto tracks = finish(model);
  check(tracks.size() == 1 && tracks[0].hits == 4,
        "Dropping preview events must not truncate the formal track");
}
void closedPreviewSurvivesLogEviction() {
  auto config = trackerConfig(6);
  config.minHits = 1;
  config.maxMiss = 0;
  config.mergeToleranceHz = 0;
  config.fragmentGapSamples = 0;
  config.limits.maximumPreviewEvents = 2;
  auto model = tracker(config);
  consume(model, window(0, {candidate(-40, -30, .8, 8)}));
  consume(model, window(1, {candidate(-38, -28, .8, 8)}));
  const auto established = model.visiblePreviewEvents();
  check(established.size() == 1, "One signal must expose one visible preview");
  const auto closedId = established.front().trackId;
  const auto droppedBefore = model.droppedPreviewEvents();
  consume(model, window(2, {candidate(30, 40, .8, 40)}));
  consume(model, window(3, {candidate(70, 80, .8, 52)}));
  consume(model, window(4, {candidate(100, 110, .8, 60)}));
  check(model.previewEvents().size() == 2 &&
            model.droppedPreviewEvents() > droppedBefore,
        "The preview log must stay capped at 2 and count dropped old events");
  for (const auto& event : model.previewEvents())
    check(event.trackId != closedId,
          "Filling the log must evict the closed track's events");
  const auto visible = model.visiblePreviewEvents();
  const auto kept = std::find_if(
      visible.begin(), visible.end(),
      [&](const auto& event) { return event.trackId == closedId; });
  check(kept != visible.end() && kept->phase == PreviewTrackPhase::Finalized &&
            near(kept->frequency.lowerHz, -39) &&
            near(kept->frequency.upperHz, -29) &&
            kept->observedSupport == TimeRange{0, 15},
        "Evicting old log events must keep the closed track's last geometry");
  const auto tracks = finish(model);
  const auto formal = std::find_if(
      tracks.begin(), tracks.end(),
      [&](const auto& track) { return track.previewId == closedId; });
  check(formal != tracks.end() && formal->hits == 2 && tracks.size() == 4,
        "Dropping preview events must not truncate the formal track");
}
void fusedPreviewKeepsOnlySurvivorEnvelope() {
  auto config = trackerConfig(4);
  config.minHits = 1;
  config.maxMiss = 1;
  config.mergeToleranceHz = 0;
  config.fragmentGapSamples = 5;
  auto model = tracker(config);
  consume(model, window(0, {candidate(0, 10, .8, 32)}));
  consume(model, window(1));
  consume(model, window(2));
  consume(model, window(3, {candidate(2, 12, .8, 36)}));
  const auto before = model.visiblePreviewEvents();
  check(before.size() == 1 && before.front().observedSupport == TimeRange{0, 25},
        "A hit inside the fragment gap must reconnect to the same open track");
  const auto survivor = before.front().trackId;
  const auto result = finish(model);
  check(result.size() == 1 && result[0].previewId == survivor && result[0].hits == 2 &&
            result[0].windows.size() == 2 &&
            result[0].originalFrequencyEnvelope == FrequencyRange{0, 12} &&
            result[0].nominalSupport == TimeRange{0, 25},
        "Online reconnection keeps both hits and the observed envelope");
}
void sealedBurstsDoNotReenterFusion() {
  constexpr std::size_t framesFor60Seconds = 60 * 4000;
  FrequencyTrackerLimits defaults;
  check(defaults.maximumWindows >= framesFor60Seconds &&
            defaults.maximumWindows < 1000000 &&
            defaults.maximumEvidenceBands >= framesFor60Seconds &&
            defaults.maximumEvidenceBands <= 1000000,
        "Default window and evidence caps must cover 60 s at 0.25 ms");
  auto config = trackerConfig(8);
  config.minHits = 1;
  config.maxMiss = 0;
  config.mergeToleranceHz = 0;
  config.fragmentGapSamples = 5;
  config.limits.maximumFusionComparisons = 100000;
  auto model = tracker(config);
  std::vector<DecodedCandidate> burst;
  burst.reserve(30);
  for (std::size_t index = 0; index < 30; ++index) {
    const double lower = -120 + static_cast<double>(index) * 4;
    burst.push_back(candidate(lower, lower + 2, .8, index));
  }
  consume(model, window(0, burst));
  consume(model, window(1, burst));
  consume(model, window(2));
  consume(model, window(3));
  consume(model, window(4));
  const auto beforeGap = model.frameStats();
  check(beforeGap.sealedTracks == 0 && beforeGap.openTracks == 30,
        "Tracks inside the fragment gap stay open");
  consume(model, window(5, {candidate(40, 42, .8, 60), candidate(80, 82, .8, 61)}));
  const auto afterGap = model.frameStats();
  check(afterGap.sealed && afterGap.sealedTracks == 30 && afterGap.openTracks == 2 &&
            afterGap.comparisons == 0,
        "The second burst must not compare the sealed first burst");
  const auto result = finish(model);
  check(result.size() == 32, "Each burst remains its own tracks");
  const auto first = std::find_if(result.begin(), result.end(), [](const auto& track) {
    return track.nominalSupport.begin == 0;
  });
  check(first != result.end() && first->hits == 2 && first->windows.empty() &&
            first->nominalSupport == TimeRange{0, 15} &&
            first->actualSupport == TimeRange{0, 18},
        "Sealing drops per-window evidence without dropping hits or observed time");
  auto separated = tracker(config);
  consume(separated, window(0, {candidate(0, 10, .8, 32)}));
  consume(separated, window(1));
  consume(separated, window(2));
  consume(separated, window(3));
  consume(separated, window(4));
  consume(separated, window(5, {candidate(0, 10, .8, 32)}));
  check(finish(separated).size() == 2,
        "Energy after the fragment gap is a new track");
}
void scnUnavailableAndInvalidSamples() {
  std::vector<float> average(32, std::numeric_limits<float>::quiet_NaN());
  std::vector<float> maximum(32, 100);
  const std::vector<FrequencyRange> bands{{8, 20}};
  std::vector<ScnFrequencyRefinement> result(1);
  result[0].skippedReason = "sentinel";
  std::string error;
  check(refineScnFrequencyBands(bands, bands, average, maximum, 0, 1, 3, result,
                                error) &&
            result[0].frequency == bands[0] && result[0].noiseSampleCount == 0,
        "Nonfinite neighboring samples are unavailable rather than fake noise "
        "floors");
  average[7] = 1;
  maximum[10] = std::numeric_limits<float>::infinity();
  check(refineScnFrequencyBands(bands, bands, average, maximum, 0, 1, 3, result,
                                error) &&
            result[0].frequency == bands[0],
        "Unavailable in-band maximum cannot extend the original band");
  result[0].skippedReason = "sentinel";
  check(!refineScnFrequencyBands(bands, bands, average, maximum, 0, 0, 3,
                                 result, error) &&
            result[0].skippedReason == "sentinel",
        "Invalid physical mapping must leave refinement output untouched");
  check(!refineScnFrequencyBands(bands, bands, average, maximum, 0, 1, 3,
                                 result, error, [] { return true; }) &&
            result[0].skippedReason == "sentinel",
        "Cancelled SCN refinement cannot publish partial edges");
}
}  // namespace

int main() {
  try {
    scoreAndLocalPeakBoundaries();
    nmsChainAndBoundary();
    stableDenseTopKAndGeometry();
    invalidHeadsAndCancellationAreAtomic();
    uniqueWindowStatisticsAndStableScope();
    maxMissMinHitsAndReconnection();
    globalOneToOneAndContainment();
    frequencyFusionKeepsAllRawEvidence();
    trackingLimitsAndCancellationAreAtomic();
    scnMinimumNoiseAndMaximumOnlyEdges();
    sameSignalKeepsOnePreviewIdentity();
    distinctSignalsDoNotMergeWithoutAnExistingRule();
    previewQueueDropsOldestWithoutTruncatingFormalTracks();
    closedPreviewSurvivesLogEviction();
    fusedPreviewKeepsOnlySurvivorEnvelope();
    sealedBurstsDoNotReenterFusion();
    scnUnavailableAndInvalidSamples();
    std::cout << "Detection postprocess tests: 17 groups passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
