#pragma once
#include "domain/detection.h"

#include <QString>

#include <cstdint>
#include <string>

namespace signalstudio {
struct DetectionSourcePlan;

// Optional, bounded profile diagnostics. JSON stays in the Qt infrastructure
// layer; domain resource arithmetic and probes have no Qt dependency.
bool mergeRuntimeResourceDiagnostics(const std::string& performanceJson,
                                     const RuntimeResourceSnapshot& snapshot,
                                     const DiskLedgerEvaluation* ledger,
                                     const ResourceDecision* sampleDecision,
                                     std::string& merged, std::string& error);

// Bits of DetectionDiskUsage that the caller has actually measured or planned.
// A missing bit is deferred. Scratch omitted at publishing is not-live.
// Neither case is stored as zero bytes.
inline constexpr std::uint32_t kDiskFactRawHead = 1u << 0;
inline constexpr std::uint32_t kDiskFactResultShards = 1u << 1;
inline constexpr std::uint32_t kDiskFactPublicationShards = 1u << 2;
inline constexpr std::uint32_t kDiskFactScratch = 1u << 3;
inline constexpr std::uint32_t kDiskFactCheckpoint = 1u << 4;
inline constexpr std::uint32_t kDiskFactProject = 1u << 5;

// Call after applyDetectionEvidencePreflight. Captures RAM and the asset
// directory's volume. VRAM stays NotRequested. No asset walk, hash, or delete.
// plan.estimatedPeakBytes > 0 is checked against probed available RAM.
// workingSetBudgetBytes is only an authorization ceiling.
// estimatedPeakBytes == 0 stays deferred and is not treated as a fit.
// On a resource decision the profile is updated and timeline keys are kept.
// A rejected document leaves performanceJson unchanged.
// true includes an Adapted sample ceiling. It does not mean a deferred
// estimate was measured as zero.
bool assessDetectionResourcePreflight(const QString& assetDirectory,
                                      const DetectionSourcePlan& plan,
                                      const DetectionRequest& request,
                                      std::string& performanceJson,
                                      QString& error);
bool assessDetectionResourcePreflight(const RuntimeResourceSnapshot& snapshot,
                                      const DetectionSourcePlan& plan,
                                      const DetectionRequest& request,
                                      std::string& performanceJson,
                                      QString& error);

// Bytes already stored on a volume and included in this call's live ledger.
// Zero leaves the historical comparison against probed free space.
// Pass raw heads, result shards, publication shards, the committed checkpoint,
// the old checkpoint file, and the old project file that already occupy that
// volume. Scratch bytes already on the scratch volume use the scratch field.
// Working-set, evidence, sample-scratch, and raw-head authorization budgets
// stay out of these fields; diskPeakBytes remains the live ledger peak.
struct DetectionStageResidentCredit {
  std::uint64_t alreadyResidentOnDurableVolumeBytes = 0;
  std::uint64_t alreadyResidentOnScratchVolumeBytes = 0;
};

// stage is "checkpoint", "refine", or "publishing".
// Only kDiskFact bits are entered in the phase ledger. Known zeros are real
// zeros. scratchSnapshot supplies the free-space probe for a known scratch
// payload whose volume key differs from snapshot.volume.volumeKey.
// reserveBytes is used as given. Return true when every known requirement
// fits; knownPeakComplete in the profile says whether any fact is still
// deferred. The profile bound remains 64 KiB.
// The eight-argument entry passes a zero credit and keeps its existing symbol.
bool assessDetectionResourceStage(
    const char* stage, const DetectionDiskUsage& knownUsage,
    std::uint32_t knownFacts, const RuntimeResourceSnapshot& snapshot,
    std::string& performanceJson, QString& error,
    const RuntimeResourceSnapshot* scratchSnapshot = nullptr,
    std::uint64_t reserveBytes = 0);

// Credit is added to the probed free bytes of the matching volume with
// checked arithmetic. Durable credit applies only to snapshot.volume.
// Scratch credit applies to the separate scratch probe when the volume keys
// differ, and to that same durable budget when scratch is live there.
// Credit never moves free space onto the other volume. A sum that does not
// fit in uint64 is resource.arithmetic-overflow.
// The volume probe's available bytes stay factual. When either credit is
// non-zero the profile records resources.residentCredit.
bool assessDetectionResourceStage(
    const char* stage, const DetectionDiskUsage& knownUsage,
    std::uint32_t knownFacts, const RuntimeResourceSnapshot& snapshot,
    std::string& performanceJson, QString& error,
    const RuntimeResourceSnapshot* scratchSnapshot, std::uint64_t reserveBytes,
    DetectionStageResidentCredit residentCredit);
}  // namespace signalstudio
