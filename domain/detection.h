#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "domain/annotation.h"

namespace signalstudio {

enum class DetectionLifecycle {
  Queued,
  Validating,
  Running,
  Cancelling,
  Completed,
  Cancelled,
  Failed,
  Interrupted
};
enum class DetectionSourceValidity { Current, Missing, Changed, Unsupported };
enum class DetectionTimeStrategy { Sample, Roi, Coarse, None };
enum class DetectionSampleEnergyMode { LegacyExact, Auto, StreamingFir };
enum class DetectionCoverageKind {
  Read,
  Eligible,
  Completed,
  Excluded,
  Unread
};
enum class DetectionBackendKind { TensorRt, Scn, Experimental17, Fixture };
enum class DetectionSpectrumMode {
  ModelDefault,
  Current,
  Mean,
  Maximum,
  Minimum
};

struct DetectionParameters {
  double scoreThreshold = .7;
  double nmsIou = .3;
  std::uint32_t topK = 512;
  std::uint32_t batchSize = 2;
  std::uint32_t minHits = 2;
  std::uint32_t maxMiss = 1;
  double mergeFrequencyHz = 100000;
  double bandwidthMarginDb = 6;
  DetectionTimeStrategy timeStrategy = DetectionTimeStrategy::Sample;
  bool splitBursts = true;
  double sampleEdgeRatio = .6;
  double minOnSeconds = .005;
  double minOffSeconds = .010;
  double smoothSeconds = .001;
  double coarseSplitGapSeconds = .020;
  double coarseMinBurstSeconds = .010;
  bool operator==(const DetectionParameters&) const = default;
};

// Physical execution candidates. Values outside this set are rejected. An
// installed engine may support only a prefix; the runtime records that.
inline bool isDetectionExecutionBatchTier(std::uint32_t batch) {
  switch (batch) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
      return true;
    default:
      return false;
  }
}

struct DetectionProfile {
  std::string id;
  std::uint32_t version = 1;
  std::string name;
  std::string status;
  DetectionBackendKind backend = DetectionBackendKind::TensorRt;
  std::uint32_t fftSize = 65536;
  std::uint32_t modelFrames = 10;
  std::uint32_t modelHopFrames = 5;
  double hopSeconds = .001;
  bool maximumPsdOnly = false;
  bool supportsReal = false;
};

inline constexpr std::uint64_t kDetectionWorkingSetMinBytes = 16ULL * 1024 * 1024;
// Nominal defaults. The live default is the smaller of this target and the
// machine limit: physical memory for the working set, free space on the budget
// volume for disk budgets.
inline constexpr std::uint64_t kDetectionWorkingSetTargetBytes =
    8ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kDetectionDiskBudgetTargetBytes =
    64ULL * 1024 * 1024 * 1024;
// Omitted historical manifests keep this scratch size. It is not the new default.
inline constexpr std::uint64_t kDetectionLegacySampleScratchBytes =
    16ULL * 1024 * 1024 * 1024;
// Explicit raw-head quotas on requests that are not using retention. Legacy
// combined manifests and this 2 GiB explicit cap stay in force.
inline constexpr std::uint64_t kDetectionExplicitRawHeadMaxBytes =
    2ULL * 1024 * 1024 * 1024;

// Empty path uses the current directory. A missing volume leaves the disk
// target in place so a default can still be formed.
std::uint64_t detectionPhysicalMemoryBytes();
std::optional<std::uint64_t> detectionVolumeFreeBytes(const std::string& path);
std::uint64_t detectionWorkingSetLimitBytes();
std::uint64_t detectionWorkingSetDefaultBytes();
std::uint64_t detectionDiskBudgetLimitBytes(const std::string& path);
std::uint64_t detectionDiskBudgetDefaultBytes(const std::string& path);

struct DetectionRequest {
  // Version 1 preserves historical fixed-grid runs. Version 2 separates the
  // user analysis grid from the backend tensor grid.
  std::uint32_t inputAdaptationVersion = 2;
  DetectionSpectrumMode spectrumMode = DetectionSpectrumMode::ModelDefault;
  DetectionSampleEnergyMode sampleEnergyMode = DetectionSampleEnergyMode::Auto;
  bool gpuDspPreferred = true;
  // Independent bounded temporary DSP storage, reclaimed before publication.
  std::uint64_t sampleScratchBudgetBytes = detectionDiskBudgetDefaultBytes({});
  std::string runId;
  std::optional<std::string> parentRunId;
  SourceIdentity sourceIdentity;
  DetectionProfile profile;
  DetectionParameters parameters;
  ViewRange requestedRange;
  ViewRange contextRange;
  std::string rangeOrigin;
  std::string sourceAnnotationId;
  std::string sourceRevisionId;
  std::string modelManifestPath;
  std::string modelSha256;
  std::uint64_t workingSetBudgetBytes = detectionWorkingSetDefaultBytes();
  std::uint64_t evidenceBudgetBytes = detectionDiskBudgetDefaultBytes({});
  // New requests leave this false and do not write per-window .f32 heads.
  // Options may turn it on; preflight then sizes the quota from the window count.
  bool retainRawModelOutput = false;
  // True only after decoding a request that omitted both retainRawModelOutput
  // and rawHeadBudgetBytes. Those runs keep the historical combined ledger.
  bool legacyCombinedEvidence = false;
  // Zero with legacyCombinedEvidence is the old combined quota. Zero without
  // it means the raw heads are not stored. A positive value is an explicit or
  // already accepted disk quota.
  std::uint64_t rawHeadBudgetBytes = 0;
};

std::uint32_t detectionModelMinimumBins(const DetectionRequest&);
std::uint32_t detectionModelInputBins(const DetectionRequest&);
double detectionModelBinScale(const DetectionRequest&);
std::uint32_t detectionModelTensorFrames(const DetectionRequest&,
                                         std::uint32_t actualFrames);
// ModelDefault selects the historical maximum for a single-spectrum backend.
// FFSCN's default temporal tensor bypasses this reduction.
bool selectDetectionSpectrum(std::span<const float> frames, std::size_t bins,
                             DetectionSpectrumMode, std::vector<float>&,
                             std::string& error);

struct DetectionCoverage {
  DetectionCoverageKind kind = DetectionCoverageKind::Unread;
  ViewRange range;
  std::string reason;
};
struct DetectionChunkReference {
  std::uint64_t sequence = 0;
  std::string relativePath;
  std::string sha256;
  std::uint32_t resultCount = 0;
};
struct DetectionRun {
  DetectionRequest request;
  DetectionLifecycle lifecycle = DetectionLifecycle::Queued;
  DetectionSourceValidity sourceValidity = DetectionSourceValidity::Current;
  std::vector<DetectionCoverage> coverage;
  std::vector<DetectionChunkReference> chunks;
  std::uint64_t readFrames = 0;
  std::uint64_t eligibleWindows = 0;
  std::uint64_t completedWindows = 0;
  std::uint64_t resultCount = 0;
  // Published shard ledger. Append adds the new chunk and new head only;
  // terminal publication and load rehash every shard and raw head.
  std::uint64_t accountedChunkBytes = 0;
  std::uint64_t accountedEvidenceBytes = 0;
  std::uint64_t accountedRawHeadBytes = 0;
  std::string computationPhase = "none";
  std::uint64_t refinedCandidates = 0, totalRefinementCandidates = 0;
  std::uint64_t startedAtMilliseconds = 0;
  std::uint64_t finishedAtMilliseconds = 0;
  std::string error;
  std::string device;
  std::string runtime;
  std::string manifestRelativePath;
  bool recoverable = false;
  // Optional bounded diagnostic JSON, not part of source/result authority.
  std::string performanceJson;
};

struct DetectionValidation {
  bool ok = true;
  std::string error;
  explicit operator bool() const { return ok; }
};

const std::vector<DetectionProfile>& detectionProfiles();
const DetectionProfile* findDetectionProfile(const std::string& id);
const char* detectionLifecycleName(DetectionLifecycle);
const char* detectionSourceValidityName(DetectionSourceValidity);
const char* detectionCoverageKindName(DetectionCoverageKind);
const char* detectionTimeStrategyName(DetectionTimeStrategy);
bool detectionIsTerminal(DetectionLifecycle);
bool detectionTransitionAllowed(DetectionLifecycle from, DetectionLifecycle to);
// storagePath is the volume that will hold the disk budgets (the project
// directory). Empty skips the live free-space cap; physical memory and the
// explicit 2 GiB raw-head cap still apply. Per-shard JSON stays 8 MiB elsewhere.
DetectionValidation validateDetectionRequest(const DetectionRequest&,
                                            const std::string& storagePath = {});
DetectionValidation validateDetectionRun(const DetectionRun&,
                                        const std::string& storagePath = {});

}  // namespace signalstudio
