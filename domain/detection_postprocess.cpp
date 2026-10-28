#include "domain/detection_postprocess.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {
bool fail(std::string& error, const char* reason) {
  error = reason;
  return false;
}
bool stop(const PostprocessCancellation& cancelled, std::string& error) {
  if (!cancelled || !cancelled()) return false;
  error = "postprocess-cancelled";
  return true;
}
bool unit(double value) {
  return std::isfinite(value) && value >= 0 && value <= 1;
}
bool validFrequency(FrequencyRange range) {
  return std::isfinite(range.lowerHz) && std::isfinite(range.upperHz) &&
         range.lowerHz >= -1e12 && range.upperHz <= 1e12 &&
         range.upperHz > range.lowerHz;
}
bool validTime(TimeRange range) { return range.end > range.begin; }
TimeRange envelope(TimeRange a, TimeRange b) {
  return {std::min(a.begin, b.begin), std::max(a.end, b.end)};
}
FrequencyRange envelope(FrequencyRange a, FrequencyRange b) {
  return {std::min(a.lowerHz, b.lowerHz), std::max(a.upperHz, b.upperHz)};
}
double intervalIou(double a0, double a1, double b0, double b1) {
  const double intersection =
      std::max(0.0, std::min(a1, b1) - std::max(a0, b0));
  const double united = a1 - a0 + b1 - b0 - intersection;
  return united > 0 ? intersection / united : 0;
}
std::string seedId(const WindowObservation& window, std::size_t peakIndex) {
  // Length prefixes avoid ambiguous identities without a Qt/hash dependency.
  const auto part = [](const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
  };
  return "frequency-track:" + part(window.runId) + part(window.fileId) +
         part(window.windowId) + ":" + std::to_string(peakIndex);
}
void summarize(FrequencyTrack& track) {
  long double lower = 0, upper = 0, scores = 0;
  track.scoreMax = 0;
  bool first = true;
  for (const auto& window : track.windows) {
    long double windowLower = 0, windowUpper = 0;
    for (const auto& band : window.bands) {
      windowLower += band.frequency.lowerHz;
      windowUpper += band.frequency.upperHz;
      track.originalFrequencyEnvelope =
          first ? band.frequency
                : envelope(track.originalFrequencyEnvelope, band.frequency);
      first = false;
    }
    lower += windowLower / window.bands.size();
    upper += windowUpper / window.bands.size();
    scores += window.score;
    track.scoreMax = std::max(track.scoreMax, window.score);
  }
  track.hits = track.windows.size();
  track.scoreMean = static_cast<double>(scores / track.hits);
  track.originalFrequency = {static_cast<double>(lower / track.hits),
                             static_cast<double>(upper / track.hits)};
  track.windowCoverage =
      track.windowDenominator
          ? static_cast<double>(track.hits) / track.windowDenominator
          : 0;
}
void mergeTracks(FrequencyTrack& target, const FrequencyTrack& source) {
  if (std::tuple{source.windows.front().ordinal,
                 source.windows.front().bands.front().peakIndex,
                 source.seedId} <
      std::tuple{target.windows.front().ordinal,
                 target.windows.front().bands.front().peakIndex, target.seedId})
    target.seedId = source.seedId;
  target.frequency = envelope(target.frequency, source.frequency);
  target.nominalSupport =
      envelope(target.nominalSupport, source.nominalSupport);
  target.actualSupport = envelope(target.actualSupport, source.actualSupport);
  for (const auto& window : source.windows) {
    const auto next = std::lower_bound(
        target.windows.begin(), target.windows.end(), window.ordinal,
        [](const auto& existing, std::uint64_t ordinal) {
          return existing.ordinal < ordinal;
        });
    if (next != target.windows.end() && next->ordinal == window.ordinal) {
      next->score = std::max(next->score, window.score);
      next->bands.insert(next->bands.end(), window.bands.begin(),
                         window.bands.end());
      std::sort(next->bands.begin(), next->bands.end(),
                [](const auto& a, const auto& b) {
                  return a.peakIndex < b.peakIndex;
                });
    } else {
      target.windows.insert(next, window);
    }
  }
  summarize(target);
}
struct CarrierSimilarity {
  bool matched = false;
  double similarity = 0, centerDistance = 0;
};
CarrierSimilarity sameCarrier(FrequencyRange a, FrequencyRange b,
                              const FrequencyTrackerConfig& config) {
  const double intersection =
      std::min(a.upperHz, b.upperHz) - std::max(a.lowerHz, b.lowerHz);
  if (intersection <= 0) return {};
  const double narrow =
      std::max(1e-9, std::min(a.upperHz - a.lowerHz, b.upperHz - b.lowerHz));
  const double containment = intersection / narrow;
  const double iou = frequencyIntersectionOverUnion(a, b);
  const double distance =
      std::abs((a.lowerHz + a.upperHz - b.lowerHz - b.upperHz) * .5);
  const double centerLimit = narrow * .5 + config.containmentCenterToleranceHz;
  return {iou >= config.matchIou ||
              (containment >= config.matchIou && distance <= centerLimit),
          std::max(iou, containment), distance};
}
struct OnlineTrack {
  FrequencyTrack result;
  std::uint32_t misses = 0;
  bool previewStable = false;
  long double lowerSum = 0, upperSum = 0, scoreSum = 0;
};
struct AssociationPair {
  double iou = 0, score = 0;
  std::size_t track = 0, candidate = 0;
};
// Upper bounds include vector growth, strings, allocator/node overhead, and
// the full transactional copy. Runtime/FFT/head memory belongs to the caller.
std::uint64_t stateBytes(std::size_t tracks, std::size_t bands,
                         std::size_t windows, std::size_t ids,
                         std::size_t idBytes) {
  return 4096 + std::uint64_t{tracks} * 4096 + std::uint64_t{bands} * 512 +
         std::uint64_t{windows} * 1024 + std::uint64_t{ids} * 256 +
         std::uint64_t{idBytes} * 8;
}
bool validateConfig(const FrequencyTrackerConfig& config, std::string& error) {
  const auto& limits = config.limits;
  if (limits.maximumIdBytes == 0 || limits.maximumIdBytes > 4096 ||
      config.runId.empty() || config.fileId.empty() ||
      config.runId.size() > limits.maximumIdBytes ||
      config.fileId.size() > limits.maximumIdBytes)
    return fail(error, "invalid-tracker-identity");
  if (config.inputBins < 4 || config.inputBins > 262144 ||
      !std::isfinite(config.binWidthHz) || config.binWidthHz <= 0 ||
      !validFrequency(
          {config.firstBinHz,
           config.firstBinHz + (config.inputBins - 1) * config.binWidthHz}) ||
      !unit(config.matchIou) || config.matchIou == 0 || config.minHits == 0 ||
      config.maxMiss > 1000000 || !std::isfinite(config.mergeToleranceHz) ||
      config.mergeToleranceHz < 0 ||
      !std::isfinite(config.containmentCenterToleranceHz) ||
      config.containmentCenterToleranceHz < 0)
    return fail(error, "invalid-tracker-frequency-or-parameters");
  if (limits.maximumWindows == 0 || limits.maximumWindows > 1000000 ||
      config.totalAnalysisWindows > limits.maximumWindows ||
      limits.maximumCandidatesPerWindow == 0 ||
      limits.maximumCandidatesPerWindow > 512 ||
      limits.maximumActiveTracks == 0 || limits.maximumTracks == 0 ||
      limits.maximumTracks > 1000000 ||
      limits.maximumActiveTracks > limits.maximumTracks ||
      limits.maximumEvidenceBands == 0 ||
      limits.maximumEvidenceBands > 1000000 ||
      limits.maximumAssociationPairs == 0 ||
      limits.maximumAssociationPairs > 4000000 ||
      limits.maximumAssociationComparisons == 0 ||
      limits.maximumFusionComparisons == 0 || limits.residentByteBudget < 4096 ||
      !limits.maximumPreviewEvents || limits.maximumPreviewEvents > 65536)
    return fail(error, "invalid-tracker-resource-limits");
  return true;
}
struct BinInterval {
  std::size_t begin = 0, end = 0;
};
BinInterval binsFor(FrequencyRange range, double first, double step,
                    std::size_t count) {
  const double begin = std::floor((range.lowerHz - first) / step + 1e-10);
  const double end = std::ceil((range.upperHz - first) / step - 1e-10);
  return {static_cast<std::size_t>(std::clamp(begin, 0.0, double(count))),
          static_cast<std::size_t>(std::clamp(end, 0.0, double(count)))};
}
bool hitThisFrame(const FrequencyTrack& track, std::uint64_t ordinal) {
  return std::any_of(track.windows.begin(), track.windows.end(),
                     [&](const auto& window) { return window.ordinal == ordinal; });
}
void releaseWindowEvidence(std::size_t& bandCount, std::size_t& windowEvidenceCount,
                           std::size_t& idBytes, FrequencyTrack& track) {
  for (const auto& window : track.windows) {
    const auto count = window.bands.size();
    bandCount = bandCount >= count ? bandCount - count : 0;
    windowEvidenceCount =
        windowEvidenceCount >= count ? windowEvidenceCount - count : 0;
    const auto bytes = window.windowId.size() * (count + 1);
    idBytes = idBytes >= bytes ? idBytes - bytes : 0;
  }
  track.windows.clear();
}
template <typename PreviewFn>
bool fuseOpenTracks(std::vector<OnlineTrack>& tracks,
                    const FrequencyTrackerConfig& config, std::uint64_t ordinal,
                    std::size_t sealedCount, std::uint64_t& comparisons,
                    bool& mergedAny, std::string& error,
                    const PostprocessCancellation& cancelled, PreviewFn&& preview) {
  const auto refreshSums = [&](OnlineTrack& track) {
    track.lowerSum = track.upperSum = track.scoreSum = 0;
    for (const auto& window : track.result.windows) {
      if (window.bands.empty()) continue;
      long double lower = 0, upper = 0;
      for (const auto& band : window.bands) {
        lower += band.frequency.lowerHz;
        upper += band.frequency.upperHz;
      }
      const auto count = static_cast<long double>(window.bands.size());
      track.lowerSum += lower / count;
      track.upperSum += upper / count;
      track.scoreSum += window.score;
    }
    if (hitThisFrame(track.result, ordinal)) track.misses = 0;
    track.previewStable = track.previewStable || track.result.hits >= config.minHits;
  };
  const auto mergePass = [&](bool fragments) {
    while (tracks.size() > 1) {
      std::optional<std::tuple<double, SampleIndex, SampleIndex, double, double,
                               SampleIndex, std::size_t, std::size_t>>
          best;
      for (std::size_t i = 0; i + 1 < tracks.size(); ++i) {
        for (std::size_t j = i + 1; j < tracks.size(); ++j) {
          if (stop(cancelled, error)) return false;
          if (++comparisons > config.limits.maximumFusionComparisons) {
            error = "frequency-fusion-comparison-limit-exceeded open=" +
                    std::to_string(tracks.size()) +
                    " sealed=" + std::to_string(sealedCount) +
                    " comparisons=" + std::to_string(comparisons);
            return false;
          }
          const auto& a = tracks[i].result;
          const auto& b = tracks[j].result;
          const auto begin =
              std::max(a.nominalSupport.begin, b.nominalSupport.begin);
          const auto end = std::min(a.nominalSupport.end, b.nominalSupport.end);
          double frequencyGap = 0, similarity = 0, centerDistance = 0;
          SampleIndex sampleGap = 0, overlapRank = 0, earliest = 0;
          if (fragments) {
            const auto gap = begin > end ? begin - end : 0;
            if (gap > config.fragmentGapSamples) continue;
            const auto match = sameCarrier(a.frequency, b.frequency, config);
            if (!match.matched) continue;
            sampleGap = gap;
            similarity = -match.similarity;
            centerDistance = match.centerDistance;
            earliest = std::min(a.nominalSupport.begin, b.nominalSupport.begin);
          } else {
            const double gap =
                std::max(a.frequency.lowerHz, b.frequency.lowerHz) -
                std::min(a.frequency.upperHz, b.frequency.upperHz);
            if (begin >= end || gap > config.mergeToleranceHz) continue;
            frequencyGap = gap;
            overlapRank =
                std::numeric_limits<SampleIndex>::max() - (end - begin);
          }
          const auto key =
              std::tuple{frequencyGap,   sampleGap, overlapRank, similarity,
                         centerDistance, earliest,  i,           j};
          if (!best || key < *best) best = key;
        }
      }
      if (!best) break;
      const auto i = std::get<6>(*best), j = std::get<7>(*best);
      const auto absorbed = tracks[j].result.previewId.empty()
                                ? tracks[j].result.seedId
                                : tracks[j].result.previewId;
      const auto survivorMisses = std::min(tracks[i].misses, tracks[j].misses);
      const bool survivorStable =
          tracks[i].previewStable || tracks[j].previewStable;
      mergeTracks(tracks[i].result, tracks[j].result);
      tracks[i].misses = survivorMisses;
      tracks[i].previewStable = survivorStable;
      refreshSums(tracks[i]);
      if (tracks[i].result.previewId.empty())
        tracks[i].result.previewId = tracks[i].result.seedId;
      ++tracks[i].result.previewRevision;
      preview(tracks[i].result, absorbed);
      mergedAny = true;
      tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(j));
    }
    return true;
  };
  if (config.mergeToleranceHz > 0 && !mergePass(false)) return false;
  return mergePass(true);
}
}  // namespace

double frequencyIntersectionOverUnion(FrequencyRange a, FrequencyRange b) {
  if (!validFrequency(a) || !validFrequency(b)) return 0;
  const double intersection = std::max(
      0.0, std::min(a.upperHz, b.upperHz) - std::max(a.lowerHz, b.lowerHz));
  const double united =
      std::max(a.upperHz, b.upperHz) - std::min(a.lowerHz, b.lowerHz);
  return intersection / std::max(1e-9, united);
}

bool decodeDetectionHeads(const HeadView& heads,
                          const DetectionDecoderConfig& config,
                          std::vector<DecodedCandidate>& output,
                          DetectionDecodeSummary& summary, std::string& error,
                          const PostprocessCancellation& cancelled) {
  if (config.inputBins < 4 || config.inputBins % 4 != 0 ||
      config.maximumInputBins > 262144 ||
      config.inputBins > config.maximumInputBins ||
      !unit(config.scoreThreshold) || !unit(config.nmsIou) ||
      config.topK == 0 || config.topK > 512 ||
      (config.kind != DetectionDecoderKind::FfscnSevenBin &&
       config.kind != DetectionDecoderKind::ScnThreeBin))
    return fail(error, "invalid-decoder-configuration");
  const bool scn = config.kind == DetectionDecoderKind::ScnThreeBin;
  const auto width = config.inputBins / 4;
  const auto validBins =
      config.validInputBins ? config.validInputBins : config.inputBins;
  if (validBins < 4 || validBins > config.inputBins || validBins % 4)
    return fail(error, "invalid-decoder-physical-bin-range");
  const auto validWidth = validBins / 4;
  const auto expected = scn ? std::array<std::size_t, 3>{width, 1, 1}
                            : std::array<std::size_t, 3>{1, 1, width};
  if (heads.sampleShape != expected || heads.heatmap.size() != width ||
      heads.bandwidth.size() != width || heads.offset.size() != width)
    return fail(error, "invalid-stride-four-head-shape");
  std::vector<std::size_t> peaks;
  peaks.reserve(width);
  const auto radius = scn ? std::size_t{1} : std::size_t{3};
  const float threshold = static_cast<float>(config.scoreThreshold);
  for (std::size_t i = 0; i < width; ++i) {
    if (stop(cancelled, error)) return false;
    if (!unit(heads.heatmap[i]) || !unit(heads.offset[i]) ||
        !std::isfinite(heads.bandwidth[i]) || heads.bandwidth[i] < 0)
      return fail(error, "invalid-activated-head-values");
  }
  for (std::size_t i = 0; i < validWidth; ++i) {
    if (stop(cancelled, error)) return false;
    const float score = heads.heatmap[i];
    if (scn ? score < threshold : score <= threshold) continue;
    const auto begin = i - std::min(i, radius);
    const auto end = i + std::min(validWidth - i - 1, radius) + 1;
    const float local = *std::max_element(heads.heatmap.begin() + begin,
                                          heads.heatmap.begin() + end);
    if (scn ? std::abs(score - local) >= 1e-6f : score != local) continue;
    peaks.push_back(i);
  }
  std::sort(peaks.begin(), peaks.end(), [&](auto a, auto b) {
    return heads.heatmap[a] != heads.heatmap[b]
               ? heads.heatmap[a] > heads.heatmap[b]
               : a < b;
  });
  DetectionDecodeSummary nextSummary;
  nextSummary.qualifyingPeaks = peaks.size();
  nextSummary.topKTruncated = peaks.size() > config.topK;
  if (peaks.size() > config.topK) peaks.resize(config.topK);
  nextSummary.rankedPeaks = peaks.size();
  std::vector<DecodedCandidate> kept;
  kept.reserve(peaks.size());
  for (const auto index : peaks) {
    if (stop(cancelled, error)) return false;
    // FFSCN float32 arithmetic matches the frozen Tensor/NumPy oracle. The
    // SCN reference decoder explicitly promotes its arithmetic to double.
    const double center =
        scn ? (double(index) + heads.offset[index]) * 4
            : double((float(index) + heads.offset[index]) * 4);
    const double bandwidth = scn ? double(heads.bandwidth[index]) * 4
                                 : double(heads.bandwidth[index] * 4);
    const double lower = scn ? center - bandwidth * .5
                             : double(float(center) - float(bandwidth) * .5f);
    const double upper = scn ? center + bandwidth * .5
                             : double(float(center) + float(bandwidth) * .5f);
    const double clippedLower = std::clamp(lower, 0.0, double(validBins - 1));
    const double clippedUpper = std::clamp(upper, 0.0, double(validBins - 1));
    if (!std::isfinite(lower) || !std::isfinite(upper))
      return fail(error, "decoded-bandwidth-arithmetic-overflow");
    if (clippedUpper <= clippedLower) {
      ++nextSummary.invalidWidthCount;
      continue;
    }
    const bool suppressed =
        std::any_of(kept.begin(), kept.end(), [&](const auto& p) {
          const auto iou =
              intervalIou(clippedLower, clippedUpper, p.lowerBin, p.upperBin);
          return scn ? iou >= config.nmsIou : iou > config.nmsIou;
        });
    if (suppressed) {
      ++nextSummary.suppressedCount;
      continue;
    }
    kept.push_back({clippedLower, clippedUpper, heads.heatmap[index], index});
  }
  if (stop(cancelled, error)) return false;
  output = std::move(kept);
  summary = nextSummary;
  error.clear();
  return true;
}

struct FrequencyTracker::State {
  FrequencyTrackerConfig config;
  std::vector<OnlineTrack> active;
  std::vector<FrequencyTrack> sealed;
  std::unordered_set<std::string> windowIds;
  std::vector<PreviewTrackEvent> preview;
  // Latest geometry for tracks that are still on screen. Log eviction does not
  // erase this map; Merged removes the absorbed trackId.
  std::unordered_map<std::string, PreviewTrackEvent> visiblePreview;
  std::uint64_t processed = 0;
  std::uint64_t previewSequence = 0;
  std::uint64_t droppedPreview = 0;
  std::size_t bandCount = 0, windowEvidenceCount = 0, idBytes = 0;
  TimeRange previousNominal;
  bool previewSealed = false;
  std::size_t lastOpen = 0, lastSealed = 0;
  std::uint64_t lastComparisons = 0;
  bool lastSealedFrame = false, lastFusedFrame = false;
};
void FrequencyTracker::rememberPreview(State& state, PreviewTrackEvent event) {
  if (state.previewSealed) return;
  if (state.preview.size() >= state.config.limits.maximumPreviewEvents) {
    state.preview.erase(state.preview.begin());
    ++state.droppedPreview;
  }
  event.sequence = ++state.previewSequence;
  event.timeLocalized = false;
  if (event.phase == PreviewTrackPhase::Merged && !event.absorbedId.empty() &&
      event.absorbedId != event.trackId)
    state.visiblePreview.erase(event.absorbedId);
  state.visiblePreview.insert_or_assign(event.trackId, event);
  state.preview.push_back(std::move(event));
}
FrequencyTracker::FrequencyTracker() = default;
FrequencyTracker::~FrequencyTracker() = default;
FrequencyTracker::FrequencyTracker(FrequencyTracker&&) noexcept = default;
FrequencyTracker& FrequencyTracker::operator=(FrequencyTracker&&) noexcept =
    default;

bool FrequencyTracker::reset(const FrequencyTrackerConfig& config,
                             std::string& error) {
  if (!validateConfig(config, error)) return false;
  auto next = std::make_unique<State>();
  next->config = config;
  state_ = std::move(next);
  error.clear();
  return true;
}
std::uint64_t FrequencyTracker::processedWindows() const {
  return state_ ? state_->processed : 0;
}
std::size_t FrequencyTracker::retainedTracks() const {
  return state_ ? state_->active.size() + state_->sealed.size() : 0;
}
FrequencyTrackerFrameStats FrequencyTracker::frameStats() const {
  FrequencyTrackerFrameStats stats;
  if (!state_) return stats;
  stats.openTracks = state_->lastOpen;
  stats.sealedTracks = state_->lastSealed;
  stats.comparisons = state_->lastComparisons;
  stats.sealed = state_->lastSealedFrame;
  stats.fused = state_->lastFusedFrame;
  return stats;
}
const std::vector<PreviewTrackEvent>& FrequencyTracker::previewEvents() const {
  static const std::vector<PreviewTrackEvent> empty;
  return state_ ? state_->preview : empty;
}
std::vector<PreviewTrackEvent> FrequencyTracker::visiblePreviewEvents() const {
  std::vector<PreviewTrackEvent> events;
  if (!state_) return events;
  events.reserve(state_->visiblePreview.size());
  for (const auto& item : state_->visiblePreview) events.push_back(item.second);
  std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
    return std::tuple{a.sequence, a.trackId} < std::tuple{b.sequence, b.trackId};
  });
  return events;
}
std::uint64_t FrequencyTracker::droppedPreviewEvents() const {
  return state_ ? state_->droppedPreview : 0;
}
bool FrequencyTracker::sealPreview(bool finalized, std::string& error) {
  if (!state_) return fail(error, "tracker-not-initialized");
  if (state_->previewSealed) return fail(error, "preview-already-sealed");
  const auto emit = [&](FrequencyTrack& track, PreviewTrackPhase phase) {
    if (track.previewId.empty()) track.previewId = track.seedId;
    ++track.previewRevision;
    rememberPreview(*state_, {track.previewId, {}, track.previewRevision, 0, phase,
                              track.runId, track.fileId, track.frequency, false,
                              track.hits, track.nominalSupport});
  };
  if (finalized) {
    for (auto& track : state_->active) emit(track.result, PreviewTrackPhase::Finalized);
    for (auto& track : state_->sealed)
      if (track.hits >= state_->config.minHits)
        emit(track, PreviewTrackPhase::Finalized);
  } else {
    for (auto& track : state_->active) emit(track.result, PreviewTrackPhase::Closed);
  }
  state_->previewSealed = true;
  error.clear();
  return true;
}

bool FrequencyTracker::consume(const WindowObservation& window,
                               std::string& error,
                               const PostprocessCancellation& cancelled) {
  if (!state_) return fail(error, "tracker-not-initialized");
  if (state_->previewSealed) return fail(error, "preview-sealed");
  const auto& config = state_->config;
  const auto& limits = config.limits;
  if (window.runId != config.runId || window.fileId != config.fileId ||
      window.windowId.empty() || window.windowId.size() > limits.maximumIdBytes)
    return fail(error, "foreign-or-invalid-window-identity");
  if (window.ordinal != state_->processed ||
      state_->windowIds.contains(window.windowId) ||
      state_->processed >= config.totalAnalysisWindows)
    return fail(error, "duplicate-or-out-of-order-window");
  if (!validTime(window.nominalSupport) || !validTime(window.actualSupport) ||
      std::max(window.nominalSupport.begin, window.actualSupport.begin) >=
          std::min(window.nominalSupport.end, window.actualSupport.end) ||
      (state_->processed &&
       (window.nominalSupport.begin < state_->previousNominal.begin ||
        window.nominalSupport.end < state_->previousNominal.end)))
    return fail(error, "invalid-window-observation-support");
  if (window.candidates.size() > limits.maximumCandidatesPerWindow)
    return fail(error, "window-candidate-limit-exceeded");
  std::vector<FrequencyBandEvidence> bands;
  bands.reserve(window.candidates.size());
  std::unordered_set<std::size_t> peakIndices;
  for (const auto& candidate : window.candidates) {
    if (stop(cancelled, error)) return false;
    const FrequencyRange frequency{
        config.firstBinHz + candidate.lowerBin * config.binWidthHz,
        config.firstBinHz + candidate.upperBin * config.binWidthHz};
    if (!std::isfinite(candidate.lowerBin) ||
        !std::isfinite(candidate.upperBin) || candidate.lowerBin < 0 ||
        candidate.upperBin > config.inputBins - 1 ||
        candidate.upperBin <= candidate.lowerBin || !unit(candidate.score) ||
        candidate.peakIndex >= (config.peakIndexLimit ? config.peakIndexLimit
                                                      : config.inputBins / 4) ||
        !validFrequency(frequency) ||
        !peakIndices.insert(candidate.peakIndex).second)
      return fail(error, "invalid-window-frequency-candidate");
    bands.push_back({frequency, candidate.lowerBin, candidate.upperBin,
                     candidate.score, candidate.peakIndex});
  }
  if (state_->bandCount + bands.size() > limits.maximumEvidenceBands)
    return fail(error, "track-evidence-limit-exceeded");
  const auto upperBytes = stateBytes(
      retainedTracks() + bands.size(), state_->bandCount + bands.size(),
      state_->windowEvidenceCount + bands.size(), state_->processed + 1,
      state_->idBytes + window.windowId.size() * (bands.size() + 1));
  if (upperBytes > limits.residentByteBudget)
    return fail(error, "tracker-resident-byte-budget-exceeded");
  std::vector<char> burstOpen(state_->active.size(), 1);
  if (config.fragmentGapSamples > 0) {
    for (std::size_t index = 0; index < state_->active.size(); ++index) {
      const auto end = state_->active[index].result.nominalSupport.end;
      const auto begin = window.nominalSupport.begin;
      const auto gap = begin > end ? begin - end : SampleIndex{0};
      if (begin > end && gap > config.fragmentGapSamples) burstOpen[index] = 0;
    }
  }
  std::vector<AssociationPair> pairs;
  std::uint64_t associationComparisons = 0;
  for (std::size_t ti = 0; ti < state_->active.size(); ++ti) {
    if (!burstOpen[ti]) continue;
    for (std::size_t di = 0; di < bands.size(); ++di) {
      if (stop(cancelled, error)) return false;
      if (++associationComparisons > limits.maximumAssociationComparisons)
        return fail(error, "association-comparison-limit-exceeded");
      const double iou = frequencyIntersectionOverUnion(
          state_->active[ti].result.frequency, bands[di].frequency);
      if (iou < config.matchIou) continue;
      if (pairs.size() == limits.maximumAssociationPairs)
        return fail(error, "association-pair-limit-exceeded");
      if (upperBytes + (pairs.size() + 1) * sizeof(AssociationPair) * 2 >
          limits.residentByteBudget)
        return fail(error, "association-resident-byte-budget-exceeded");
      pairs.push_back({iou, bands[di].score, ti, di});
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
    return std::tuple{-a.iou, -a.score, a.track, a.candidate} <
           std::tuple{-b.iou, -b.score, b.track, b.candidate};
  });
  auto next = std::make_unique<State>(*state_);
  std::vector<bool> matchedTracks(next->active.size());
  std::vector<bool> matchedBands(bands.size());
  const auto evidence = [&](std::size_t index) {
    return TrackWindowEvidence{window.windowId,      window.ordinal,
                               bands[index].score,   window.nominalSupport,
                               window.actualSupport, {bands[index]}};
  };
  for (const auto& pair : pairs) {
    if (stop(cancelled, error)) return false;
    if (matchedTracks[pair.track] || matchedBands[pair.candidate]) continue;
    auto& track = next->active[pair.track];
    const auto& band = bands[pair.candidate];
    track.result.windows.push_back(evidence(pair.candidate));
    track.result.nominalSupport =
        envelope(track.result.nominalSupport, window.nominalSupport);
    track.result.actualSupport =
        envelope(track.result.actualSupport, window.actualSupport);
    track.lowerSum += band.frequency.lowerHz;
    track.upperSum += band.frequency.upperHz;
    track.scoreSum += band.score;
    auto& result = track.result;
    result.hits = result.windows.size();
    result.originalFrequency = {
        static_cast<double>(track.lowerSum / result.hits),
        static_cast<double>(track.upperSum / result.hits)};
    result.originalFrequencyEnvelope =
        envelope(result.originalFrequencyEnvelope, band.frequency);
    result.scoreMean = static_cast<double>(track.scoreSum / result.hits);
    result.scoreMax = std::max(result.scoreMax, band.score);
    result.windowCoverage =
        static_cast<double>(result.hits) / result.windowDenominator;
    // Multiple bands in one window are a fused span. A later hit extends that
    // span. A single-band track still publishes the mean of its window edges.
    const bool fusedSpan = std::any_of(
        result.windows.begin(), result.windows.end(),
        [](const auto& evidence) { return evidence.bands.size() > 1; });
    result.frequency = fusedSpan ? envelope(result.frequency, band.frequency)
                                 : result.originalFrequency;
    track.misses = 0;
    ++result.previewRevision;
    rememberPreview(*next, {result.previewId, {}, result.previewRevision, 0,
                            PreviewTrackPhase::Updated, result.runId,
                            result.fileId, result.frequency, false, result.hits,
                            result.nominalSupport});
    if (!track.previewStable && result.hits >= config.minHits) {
      track.previewStable = true;
      ++result.previewRevision;
      rememberPreview(*next, {result.previewId, {}, result.previewRevision, 0,
                              PreviewTrackPhase::Stable, result.runId,
                              result.fileId, result.frequency, false, result.hits,
                              result.nominalSupport});
    }
    matchedTracks[pair.track] = true;
    matchedBands[pair.candidate] = true;
  }
  bool sealedNow = false;
  const auto seal = [&](OnlineTrack track) {
    auto& result = track.result;
    if (result.previewId.empty()) result.previewId = result.seedId;
    ++result.previewRevision;
    rememberPreview(*next, {result.previewId, {}, result.previewRevision, 0,
                            PreviewTrackPhase::Finalized, result.runId, result.fileId,
                            result.frequency, false, result.hits, result.nominalSupport});
    releaseWindowEvidence(next->bandCount, next->windowEvidenceCount, next->idBytes,
                          result);
    next->sealed.push_back(std::move(result));
    sealedNow = true;
  };
  for (std::size_t index = next->active.size(); index-- > 0;) {
    if (index >= burstOpen.size() || burstOpen[index]) continue;
    auto track = std::move(next->active[index]);
    next->active.erase(next->active.begin() + static_cast<std::ptrdiff_t>(index));
    seal(std::move(track));
  }
  for (std::size_t i = 0; i < bands.size(); ++i) {
    if (stop(cancelled, error)) return false;
    if (matchedBands[i]) continue;
    FrequencyTrack track;
    track.seedId = seedId(window, bands[i].peakIndex);
    track.previewId = track.seedId;
    track.previewRevision = 1;
    track.runId = config.runId;
    track.fileId = config.fileId;
    track.frequency = bands[i].frequency;
    track.nominalSupport = window.nominalSupport;
    track.actualSupport = window.actualSupport;
    track.windowDenominator = config.totalAnalysisWindows;
    track.windows.push_back(evidence(i));
    summarize(track);
    const auto stableNow = track.hits >= config.minHits;
    if (stableNow) track.previewRevision = 2;
    OnlineTrack created;
    created.result = std::move(track);
    created.previewStable = stableNow;
    created.lowerSum = bands[i].frequency.lowerHz;
    created.upperSum = bands[i].frequency.upperHz;
    created.scoreSum = bands[i].score;
    rememberPreview(*next, {created.result.previewId, {}, 1, 0,
                            PreviewTrackPhase::Created, created.result.runId,
                            created.result.fileId, created.result.frequency, false,
                            created.result.hits, created.result.nominalSupport});
    if (stableNow)
      rememberPreview(*next, {created.result.previewId, {}, 2, 0,
                              PreviewTrackPhase::Stable, created.result.runId,
                              created.result.fileId, created.result.frequency, false,
                              created.result.hits, created.result.nominalSupport});
    next->active.push_back(std::move(created));
  }
  std::uint64_t fusionComparisons = 0;
  bool mergedAny = false;
  if (!fuseOpenTracks(
          next->active, config, window.ordinal, next->sealed.size(),
          fusionComparisons, mergedAny, error, cancelled,
          [&](const FrequencyTrack& survivor, const std::string& absorbed) {
            rememberPreview(*next, {survivor.previewId, absorbed, survivor.previewRevision,
                                    0, PreviewTrackPhase::Merged, survivor.runId,
                                    survivor.fileId, survivor.frequency, false,
                                    survivor.hits, survivor.nominalSupport});
          }))
    return false;
  if (config.fragmentGapSamples == 0) {
    for (std::size_t index = next->active.size(); index-- > 0;) {
      if (hitThisFrame(next->active[index].result, window.ordinal)) continue;
      if (++next->active[index].misses <= config.maxMiss) continue;
      auto track = std::move(next->active[index]);
      next->active.erase(next->active.begin() + static_cast<std::ptrdiff_t>(index));
      seal(std::move(track));
    }
  } else {
    for (auto& track : next->active)
      if (!hitThisFrame(track.result, window.ordinal)) ++track.misses;
  }
  if (next->active.size() > limits.maximumActiveTracks ||
      next->active.size() + next->sealed.size() > limits.maximumTracks)
    return fail(error, "retained-track-limit-exceeded");
  next->bandCount += bands.size();
  next->windowEvidenceCount += bands.size();
  next->idBytes += window.windowId.size() * (bands.size() + 1);
  next->windowIds.insert(window.windowId);
  ++next->processed;
  next->previousNominal = window.nominalSupport;
  next->lastOpen = next->active.size();
  next->lastSealed = next->sealed.size();
  next->lastComparisons = fusionComparisons;
  next->lastSealedFrame = sealedNow;
  next->lastFusedFrame = mergedAny;
  if (stop(cancelled, error)) return false;
  state_ = std::move(next);
  error.clear();
  return true;
}

bool FrequencyTracker::finish(std::vector<FrequencyTrack>& output,
                              std::string& error,
                              const PostprocessCancellation& cancelled) {
  if (!state_) return fail(error, "tracker-not-initialized");
  if (stop(cancelled, error)) return false;
  const auto& config = state_->config;
  std::vector<FrequencyTrack> tracks;
  tracks.reserve(retainedTracks());
  const auto minimumHits = std::min<std::uint64_t>(
      config.minHits, std::max<std::uint64_t>(1, config.totalAnalysisWindows));
  const auto collect = [&](const FrequencyTrack& track) {
    if (stop(cancelled, error)) return false;
    if (track.hits >= minimumHits) tracks.push_back(track);
    return true;
  };
  for (const auto& track : state_->sealed)
    if (!collect(track)) return false;
  for (const auto& track : state_->active)
    if (!collect(track.result)) return false;
  std::sort(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) {
    return std::tuple{a.frequency.lowerHz, a.nominalSupport.begin,
                      a.frequency.upperHz, a.seedId} <
           std::tuple{b.frequency.lowerHz, b.nominalSupport.begin,
                      b.frequency.upperHz, b.seedId};
  });
  if (stop(cancelled, error)) return false;
  output = std::move(tracks);
  error.clear();
  return true;
}

bool refineScnFrequencyBands(
    std::span<const FrequencyRange> bands,
    std::span<const FrequencyRange> occupancy, std::span<const float> averageDb,
    std::span<const float> maximumDb, double firstBinHz, double binWidthHz,
    double marginDb, std::vector<ScnFrequencyRefinement>& output,
    std::string& error, const PostprocessCancellation& cancelled) {
  if (averageDb.empty() || averageDb.size() != maximumDb.size() ||
      averageDb.size() > 262144 || bands.size() > 100000 ||
      occupancy.size() > 100000 || !std::isfinite(firstBinHz) ||
      !std::isfinite(binWidthHz) || binWidthHz <= 0 ||
      !std::isfinite(marginDb) || marginDb < 0 ||
      !validFrequency({firstBinHz, firstBinHz + averageDb.size() * binWidthHz}))
    return fail(error, "invalid-scn-refinement-configuration");
  const auto count = averageDb.size();
  std::vector<BinInterval> occupied;
  occupied.reserve(occupancy.size());
  for (const auto& band : occupancy) {
    if (stop(cancelled, error)) return false;
    if (!validFrequency(band))
      return fail(error, "invalid-occupancy-frequency-band");
    const auto interval = binsFor(band, firstBinHz, binWidthHz, count);
    if (interval.end > interval.begin) occupied.push_back(interval);
  }
  std::sort(occupied.begin(), occupied.end(), [](const auto& a, const auto& b) {
    return std::tie(a.begin, a.end) < std::tie(b.begin, b.end);
  });
  std::vector<BinInterval> merged;
  for (const auto& range : occupied) {
    if (merged.empty() || range.begin > merged.back().end)
      merged.push_back(range);
    else
      merged.back().end = std::max(merged.back().end, range.end);
  }
  const auto isOccupied = [&](std::size_t bin) {
    const auto next = std::upper_bound(merged.begin(), merged.end(), bin,
                                       [](auto value, const auto& interval) {
                                         return value < interval.begin;
                                       });
    return next != merged.begin() && std::prev(next)->end > bin;
  };
  std::vector<ScnFrequencyRefinement> results;
  results.reserve(bands.size());
  std::uint64_t visitedBins = 0;
  const auto visit = [&] {
    if (stop(cancelled, error)) return false;
    if (++visitedBins > 64 * 1024 * 1024)
      return fail(error, "scn-refinement-work-limit-exceeded");
    return true;
  };
  for (const auto& band : bands) {
    if (stop(cancelled, error)) return false;
    if (!validFrequency(band))
      return fail(error, "invalid-refinement-frequency-band");
    ScnFrequencyRefinement result;
    result.originalFrequency = result.frequency = band;
    const auto range = binsFor(band, firstBinHz, binWidthHz, count);
    if (range.end <= range.begin) {
      result.skippedReason = "empty-original-band";
      results.push_back(result);
      continue;
    }
    const auto sampleWidth =
        std::max<std::size_t>(1, (range.end - range.begin) / 10);
    const auto left = range.begin - std::min(range.begin, sampleWidth);
    const auto right = range.end + std::min(count - range.end, sampleWidth);
    double noise = std::numeric_limits<double>::infinity();
    const auto sample = [&](std::size_t begin, std::size_t end) {
      for (auto bin = begin; bin < end; ++bin) {
        if (!visit()) return false;
        if (isOccupied(bin) || !std::isfinite(averageDb[bin])) continue;
        noise = std::min(noise, double(averageDb[bin]));
        ++result.noiseSampleCount;
      }
      return true;
    };
    if (!sample(left, range.begin) || !sample(range.end, right)) return false;
    if (!result.noiseSampleCount) {
      result.skippedReason = "no-unoccupied-out-of-band-samples";
      results.push_back(result);
      continue;
    }
    result.noiseFloorDb = noise;
    result.thresholdDb = noise + marginDb;
    if (!std::isfinite(result.thresholdDb))
      return fail(error, "scn-noise-threshold-overflow");
    auto first = range.end, last = range.begin;
    for (auto bin = range.begin; bin < range.end; ++bin) {
      if (!visit()) return false;
      if (!std::isfinite(maximumDb[bin]) || maximumDb[bin] < result.thresholdDb)
        continue;
      first = std::min(first, bin);
      last = bin;
    }
    if (first == range.end) {
      result.skippedReason = "no-in-band-bin-meets-threshold";
    } else {
      result.frequency = {
          std::max(band.lowerHz, firstBinHz + first * binWidthHz),
          std::min(band.upperHz, firstBinHz + (last + 1) * binWidthHz)};
      if (result.frequency.upperHz <= result.frequency.lowerHz) {
        result.frequency = band;
        result.skippedReason = "empty-refined-band";
      } else {
        result.applied = result.frequency != band;
      }
    }
    results.push_back(std::move(result));
  }
  if (stop(cancelled, error)) return false;
  output = std::move(results);
  error.clear();
  return true;
}

}  // namespace signalstudio
