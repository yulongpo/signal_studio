#pragma once

#include <QString>
#include <functional>
#include "application/annotation_service.h"
#include "infrastructure/detection_task.h"

namespace signalstudio {

enum class DetectionTransactionPoint { Stage, Persist, Commit, Gc };
struct DetectionTransactionOptions {
  // Return true to inject a failure at this boundary. Gc failure is deferred;
  // it never rolls back a successfully committed project.
  std::function<bool(DetectionTransactionPoint)> failAt;
};
struct DetectionResultTicket {
  std::string fileId, runId;
  std::uint32_t sampleChannel = 0;
  std::uint64_t projectGeneration = 0, slotGeneration = 0;
  SourceIdentity sourceIdentity;
};
struct DetectionCommitResult {
  bool committed = false, gcDeferred = false;
  QString error;
  explicit operator bool() const { return committed; }
};

const CurrentDetectionSlot* currentDetectionSlot(const FileState&,
                                                std::uint32_t sampleChannel);
// This validates only the authority/reference graph, also during a live save.
AnnotationValidation validateCurrentDetectionSlots(const FileState&);
// Call after legacy non-Manual user revisions have been preserved as Manual.
// Missing fields are migrated on the temporary decoded FileState only.
AnnotationValidation migrateLegacyDetectionSlots(
    FileState&, const std::vector<std::string>& liveRunIds = {});

class DetectionResultService {
 public:
  // Mutates a candidate (including pending ID/stale/generation), not disk.
  // The panel saves this candidate before starting to publish task progress.
  static bool beginDetection(Project&, const DetectionRequest&,
                             std::uint64_t projectGeneration,
                             DetectionResultTicket&, QString& error);
  static bool prepareRecovery(Project&, const DetectionRun&,
                              std::uint64_t projectGeneration,
                              DetectionResultTicket&, QString& error);
  static void abandonDetection(Project&, const DetectionResultTicket&);
  static bool markStale(Project&, const std::string& fileId,
                        std::uint32_t sampleChannel);

  // Stage on a fresh Project -> verify durable run/shards -> atomic .ssproj
  // save -> noexcept move into the caller. Never uses Session::replaceProject
  // (that would advance projectGeneration/cancel unrelated running tasks).
  static DetectionCommitResult replaceCurrentDetection(
      Project&, AnnotationService&, const DetectionResultTicket&,
      std::uint64_t projectGeneration, const DetectionRun&,
      DetectionPublication, const QString& ssprojPath,
      const DetectionTransactionOptions& = {});
};
}  // namespace signalstudio
