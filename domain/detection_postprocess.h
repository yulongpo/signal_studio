#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "domain/annotation.h"

namespace signalstudio {

using PostprocessCancellation = std::function<bool()>;
enum class DetectionDecoderKind { FfscnSevenBin, ScnThreeBin };

// One batch member. All three tensors have the declared sample shape and
// already contain sigmoid hm/off and nonnegative bandwidth predictions.
struct HeadView {
  std::span<const float> heatmap;
  std::span<const float> bandwidth;
  std::span<const float> offset;
  std::array<std::size_t, 3> sampleShape{};
};
struct DetectionDecoderConfig {
  DetectionDecoderKind kind = DetectionDecoderKind::FfscnSevenBin;
  std::size_t inputBins = 65536;
  double scoreThreshold = .7;
  double nmsIou = .3;
  std::size_t topK = 512;
  std::size_t maximumInputBins = 262144;
};
struct DecodedCandidate {
  double lowerBin = 0, upperBin = 0;
  double score = 0;
  std::size_t peakIndex = 0;
  bool operator==(const DecodedCandidate&) const = default;
};
struct DetectionDecodeSummary {
  std::size_t qualifyingPeaks = 0;
  std::size_t rankedPeaks = 0;
  std::size_t invalidWidthCount = 0;
  std::size_t suppressedCount = 0;
  bool topKTruncated = false;
};

// Errors/cancellation leave both output arguments unchanged. Positive widths
// consumed by TopK may be clipped away; zero widths also consume their rank.
bool decodeDetectionHeads(const HeadView&, const DetectionDecoderConfig&,
                          std::vector<DecodedCandidate>&,
                          DetectionDecodeSummary&, std::string& error,
                          const PostprocessCancellation& cancelled = {});
double frequencyIntersectionOverUnion(FrequencyRange, FrequencyRange);

struct FrequencyBandEvidence {
  FrequencyRange frequency;
  double lowerBin = 0, upperBin = 0;
  double score = 0;
  std::size_t peakIndex = 0;
};
struct TrackWindowEvidence {
  std::string windowId;
  std::uint64_t ordinal = 0;
  double score = 0;
  TimeRange nominalSupport;
  TimeRange actualSupport;
  std::vector<FrequencyBandEvidence> bands;
};
struct FrequencyTrack {
  std::string seedId;
  std::string runId, fileId;
  FrequencyRange frequency;
  FrequencyRange originalFrequency;
  FrequencyRange originalFrequencyEnvelope;
  // These are observation contexts, never estimated transmission intervals.
  TimeRange nominalSupport;
  TimeRange actualSupport;
  std::vector<TrackWindowEvidence> windows;
  double scoreMean = 0, scoreMax = 0;
  std::uint64_t hits = 0;
  std::uint64_t windowDenominator = 0;
  double windowCoverage = 0;
};
struct WindowObservation {
  std::string runId, fileId, windowId;
  // Contiguous ordinal in this run's eligible-window sequence, starting at 0.
  std::uint64_t ordinal = 0;
  TimeRange nominalSupport;
  TimeRange actualSupport;
  std::vector<DecodedCandidate> candidates;
};
struct FrequencyTrackerLimits {
  std::size_t maximumWindows = 100000;
  std::size_t maximumCandidatesPerWindow = 512;
  std::size_t maximumActiveTracks = 4096;
  std::size_t maximumTracks = 32768;
  std::size_t maximumEvidenceBands = 100000;
  std::size_t maximumAssociationPairs = 262144;
  std::uint64_t maximumAssociationComparisons = 4000000;
  std::uint64_t maximumFusionComparisons = 4000000;
  std::uint64_t residentByteBudget = 32 * 1024 * 1024;
  std::size_t maximumIdBytes = 256;
};
struct FrequencyTrackerConfig {
  std::string runId, fileId;
  std::size_t inputBins = 65536;
  double firstBinHz = 0, binWidthHz = 0;
  double matchIou = .3;
  std::uint32_t minHits = 2, maxMiss = 1;
  double mergeToleranceHz = 100000;
  double containmentCenterToleranceHz = 0;
  SampleIndex fragmentGapSamples = 0;
  // Immutable eligible-window denominator, including incomplete windows.
  std::uint64_t totalAnalysisWindows = 0;
  FrequencyTrackerLimits limits;
};

// A bounded deterministic single-run tracker. Each successful consume is
// atomic; retries/foreign scope/out-of-order windows fail without mutation.
// finish may also be called after cancellation for explicit partial recovery.
class FrequencyTracker {
 public:
  FrequencyTracker();
  ~FrequencyTracker();
  FrequencyTracker(FrequencyTracker&&) noexcept;
  FrequencyTracker& operator=(FrequencyTracker&&) noexcept;
  FrequencyTracker(const FrequencyTracker&) = delete;
  FrequencyTracker& operator=(const FrequencyTracker&) = delete;

  bool reset(const FrequencyTrackerConfig&, std::string& error);
  bool consume(const WindowObservation&, std::string& error,
               const PostprocessCancellation& cancelled = {});
  bool finish(std::vector<FrequencyTrack>&, std::string& error,
              const PostprocessCancellation& cancelled = {}) const;
  std::uint64_t processedWindows() const;
  std::size_t retainedTracks() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

struct ScnFrequencyRefinement {
  FrequencyRange originalFrequency;
  FrequencyRange frequency;
  bool applied = false;
  std::size_t noiseSampleCount = 0;
  double noiseFloorDb = 0, thresholdDb = 0;
  std::string skippedReason;
};

// Both spectra are physical dB values. Average is used only for the local
// minimum noise estimate; only maximum is compared against the threshold.
// Occupancy contains ALL fused candidates before any presentation filtering.
bool refineScnFrequencyBands(
    std::span<const FrequencyRange> bands,
    std::span<const FrequencyRange> occupancy, std::span<const float> averageDb,
    std::span<const float> maximumDb, double firstBinHz, double binWidthHz,
    double marginDb, std::vector<ScnFrequencyRefinement>&, std::string& error,
    const PostprocessCancellation& cancelled = {});

}  // namespace signalstudio
