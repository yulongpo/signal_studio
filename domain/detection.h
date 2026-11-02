#pragma once

#include <cstddef>
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

// Runtime-only. These types are not DetectionRequest fields and are not
// serialized into project JSON.
enum class ResourceDecisionKind { Accepted, Adapted, Rejected };

// requested, available, reserve, and granted are bytes when resource is
// "ram", "vram", or "disk". They are cell counts when resource is
// "work-unit". reserve stays 0 when the caller did not pass a reserve.
struct ResourceDecision {
  ResourceDecisionKind kind = ResourceDecisionKind::Rejected;
  std::string errorCode;
  std::string phase;
  std::string resource;
  std::uint64_t requested = 0;
  std::uint64_t available = 0;
  std::uint64_t reserve = 0;
  std::uint64_t granted = 0;
  std::string message;
};

// One contiguous allocation safety cap. This is not a RAM authorization ceiling.
inline constexpr std::uint64_t kSampleContiguousAllocationCapBytes =
    1024ull * 1024 * 1024;
// Cell-count ceiling for one algorithm work unit. This is not a byte budget.
inline constexpr std::size_t kSampleWorkUnitCellCeiling = 256u * 1024u * 1024u;

// On success writes a * b to out and returns true. On overflow writes 0 and
// returns false. The product is never wrapped.
bool checkedMulU64(std::uint64_t a, std::uint64_t b, std::uint64_t& out);
// On success writes a + b to out and returns true. On overflow writes 0 and
// returns false. The sum is never wrapped.
bool checkedAddU64(std::uint64_t a, std::uint64_t b, std::uint64_t& out);

// Sample-phase budget. phase is "time-refine". An authorization above the
// contiguous cap is Accepted or Adapted, never Rejected only because it is large.
// authorizedRamBytes == 0 rejects with sample.invalid-context.
// plannedCells > kSampleWorkUnitCellCeiling rejects with sample.work-unit-ceiling.
// estimatedChunkResidentBytes > authorizedRamBytes, when authorized is
// non-zero, rejects with sample.chunk-exceeds-grant.
// estimated resident above the contiguous cap but within the authorization
// returns Adapted, granted = the cap, and tells the planner to chunk. That
// adaptation is not a hard failure of the whole job. Callers must branch on
// kind; sample.grant-exceeds-contiguous-cap is returned only for Adapted.
// This signature and its decision order are the shared Sample decision.
// Cell counts stay on this function. Do not pass them to the byte helpers.
ResourceDecision decideSamplePhaseBudget(std::uint64_t authorizedRamBytes,
                                         std::uint64_t estimatedChunkResidentBytes,
                                         std::size_t plannedCells);

// "cells" only when resource == "work-unit". Every other resource is "bytes".
const char* resourceDecisionUnit(const ResourceDecision& decision);

// One detection result shard file. The sum of shards may be larger.
// Equal to this cap is legal. One byte over is a format rejection.
inline constexpr std::uint64_t kDetectionShardFileCapBytes = 8ULL * 1024 * 1024;

// Runtime probes. They are not DetectionRequest fields and are not project JSON.
// A failed or skipped probe stores zeros. It does not substitute the 8 GiB
// working-set target or a guessed VRAM size.
enum class ResourceProbeStatus { Probed, Unavailable, NotRequested };

struct ResourceByteProbe {
  ResourceProbeStatus status = ResourceProbeStatus::NotRequested;
  std::uint64_t totalBytes = 0;
  std::uint64_t availableBytes = 0;
  std::string detail;
};

struct VolumeResourceProbe {
  ResourceProbeStatus status = ResourceProbeStatus::Unavailable;
  std::string path;
  std::string volumeKey;
  std::uint64_t availableBytes = 0;
  std::uint64_t totalBytes = 0;
  std::string detail;
};

// F or D fills this from cudaMemGetInfo. This component does not call CUDA.
struct VramProbeInput {
  bool probed = false;
  std::uint64_t totalBytes = 0;
  std::uint64_t availableBytes = 0;
  std::string deviceIdentity;
  std::string detail;
};

struct RuntimeResourceSnapshot {
  ResourceByteProbe ram;
  ResourceByteProbe vram;
  VolumeResourceProbe volume;
  // Parent-process steady_clock nanoseconds at capture. Not a worker clock.
  std::uint64_t capturedAtSteadyNs = 0;
};

// volumePath empty uses the current directory. vram null leaves VRAM
// NotRequested with both byte fields at 0.
RuntimeResourceSnapshot captureRuntimeResourceSnapshot(
    const std::string& volumePath, const VramProbeInput* vram = nullptr);

// Byte resident check for RAM or VRAM. Sample geometry stays on
// decideSamplePhaseBudget. resource must be "ram" or "vram".
// Accepted grants the request. A shortfall grants 0 and does not shrink it.
ResourceDecision decideResidentAgainstProbe(const char* phase,
                                            const char* resource,
                                            std::uint64_t requestedBytes,
                                            const ResourceByteProbe& probe,
                                            std::uint64_t reserveBytes);

// Raw-head preflight arithmetic. Does not modify DetectionRequest and does not
// replace applyDetectionEvidencePreflight's stored sentences.
// legacyCombined: applies is false and the existing combined branch stays in D.
// retain off and quota 0: Accepted, requested 0, granted 0, even if the
// untaken window product would be 2044 * 196608 or would overflow.
// retain off and quota > 0: the explicit quota is the limit. The 2 GiB cap
// matches validateDetectionRequest. Volume is not a gate on this branch.
// retain on: resource.raw-head-shortfall for quota, unknown volume, or volume.
// Overflow is resource.arithmetic-overflow and is never reported as a shortfall.
struct RawHeadDiskInput {
  bool legacyCombined = false;
  bool retainRawModelOutput = false;
  std::uint64_t windows = 0;
  std::uint64_t bytesPerWindow = 0;
  std::uint64_t explicitQuotaBytes = 0;
  std::uint64_t volumeAvailableBytes = 0;
  bool volumeKnown = false;
};

struct RawHeadDiskAssessment {
  bool applies = false;
  bool overflow = false;
  std::uint64_t requiredBytes = 0;
  ResourceDecision decision;
};

RawHeadDiskAssessment assessRawHeadDisk(const RawHeadDiskInput& input);

// Planned peaks for one run. Scratch is live only in phase "refine" and is
// reclaimed before publication. Checkpoint's atomic pair (new payload plus
// the file still on disk) is live only in phase "checkpoint"; later phases
// keep one committed file of checkpointBytes. Project QSaveFile peak
// (payload + existing, or payload alone when the project is new) is live
// only in phase "publishing". Retained raw heads and result shards stay
// across checkpoint, refine, and publishing. Publication shards are added
// only in publishing. Same-volume lines in one phase are summed. A different
// scratchVolumeKey is a separate peak.
struct DetectionDiskUsage {
  std::string durableVolumeKey;
  std::string scratchVolumeKey;
  bool storeRawHead = false;
  std::uint64_t rawHeadBytes = 0;
  std::uint64_t scratchBytes = 0;
  std::uint64_t resultShardBytes = 0;
  std::uint64_t largestResultShardBytes = 0;
  std::uint64_t publicationShardBytes = 0;
  std::uint64_t largestPublicationShardBytes = 0;
  std::uint64_t checkpointBytes = 0;
  std::uint64_t checkpointExistingBytes = 0;
  std::uint64_t projectBytes = 0;
  std::uint64_t projectExistingBytes = 0;
};

enum class DiskLedgerKind : std::uint8_t {
  RawHead,
  Scratch,
  Shards,
  Checkpoint,
  ProjectAtomic
};

struct DiskLedgerLine {
  DiskLedgerKind kind = DiskLedgerKind::Shards;
  std::string phase;
  std::string volumeKey;
  std::uint64_t payloadBytes = 0;
  // Added to the payload only while atomicReplace is set. That is the
  // predecessor QSaveFile keeps until commit. Zero means there is no old file.
  std::uint64_t existingBytes = 0;
  bool atomicReplace = false;
  // For Shards, the largest single file. Zero means the caller did not split
  // the payload; a payload above the 8 MiB cap is then a format rejection.
  std::uint64_t largestUnitBytes = 0;
};

struct DiskVolumeBudget {
  std::string volumeKey;
  bool known = false;
  std::uint64_t availableBytes = 0;
};

struct DiskLedgerEvaluation {
  ResourceDecision decision;
  std::vector<ResourceDecision> perVolume;
  std::string peakPhase;
  std::string peakVolumeKey;
  std::uint64_t peakBytes = 0;
};

// Copies required bytes only after an applicable Accepted assessment.
// A rejection leaves usage unchanged and returns false.
bool applyAcceptedRawHeadToUsage(const RawHeadDiskAssessment& assessment,
                                 DetectionDiskUsage& usage);

// Appends the checkpoint, refine, and publishing lines. Does not clear lines.
void appendPhaseOverlapDiskLines(const DetectionDiskUsage& usage,
                                 std::vector<DiskLedgerLine>& lines);

// reserveBytes is held back on every volume. Equal to usable space is
// Accepted. requested is the simultaneous peak. granted is that peak, or 0
// when rejected. A raw head that itself exceeds usable space is
// resource.raw-head-shortfall. Any other overage is resource.disk-shortfall.
// Shard files above 8 MiB are resource.shard-boundary and resource "format".
// Bytes and cells are never compared here.
DiskLedgerEvaluation evaluatePhaseOverlapDiskLedger(
    std::span<const DiskLedgerLine> lines,
    std::span<const DiskVolumeBudget> volumes, std::uint64_t reserveBytes);

DiskVolumeBudget diskBudgetFromVolumeProbe(const VolumeResourceProbe& probe);

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
