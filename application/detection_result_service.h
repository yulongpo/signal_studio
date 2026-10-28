#pragma once

#include <QString>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include "application/annotation_service.h"
#include "infrastructure/detection_task.h"

namespace signalstudio {

enum class DetectionTransactionPoint { Stage, Persist, Commit, Gc };
enum class DetectionPublicationStage { Recovered, Compared };
struct DetectionTransactionOptions {
  // Return true to inject a failure at this boundary. Gc failure is deferred;
  // it never rolls back a successfully committed project.
  std::function<bool(DetectionTransactionPoint)> failAt;
  // Same generation comparison as a pending start. False at the atomic commit
  // leaves the previous project file and the caller's Project unchanged.
  std::function<bool()> stillCurrent;
  // Observes completed boundaries in the parent's monotonic clock. Telemetry
  // must never change the publication transaction's outcome.
  std::function<void(DetectionPublicationStage)> publicationStage;
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
// Live session check used when a background publication save finishes.
bool detectionTicketMatches(const Project&, const FileState&,
                            const DetectionResultTicket&,
                            std::uint64_t projectGeneration, const DetectionRun&);
// This validates only the authority/reference graph, also during a live save.
AnnotationValidation validateCurrentDetectionSlots(const FileState&);
// Call after legacy non-Manual user revisions have been preserved as Manual.
// Missing fields are migrated on the temporary decoded FileState only.
AnnotationValidation migrateLegacyDetectionSlots(
    FileState&, const std::vector<std::string>& liveRunIds = {});

class DetectionResultService {
 public:
  // Mutates a candidate (including pending ID/stale/generation), not disk.
  // The committed run and its marks stay. The panel publishes this candidate
  // only through publishPendingStart, after the task can be queued.
  static bool beginDetection(Project&, const DetectionRequest&,
                             std::uint64_t projectGeneration,
                             DetectionResultTicket&, QString& error);
  // Writes `candidate` only while `stillCurrent` remains true through the
  // atomic commit, then calls `commitQueued`. A generation change, a formal
  // result mismatch, or a queue failure leaves `live` and the previous file
  // bytes in place.
  static bool publishPendingStart(Project& live, const Project& candidate,
                                  const std::function<bool()>& stillCurrent,
                                  const std::function<bool(QString&)>& commitQueued,
                                  const QString& path, QString& error);
  static bool prepareRecovery(Project&, const DetectionRun&,
                              std::uint64_t projectGeneration,
                              DetectionResultTicket&, QString& error);
  static void abandonDetection(Project&, const DetectionResultTicket&);
  // Cancel, failure, and interruption do not publish. The task row stays in
  // detectionRuns and is not added to retiredRunIds; those two sets are
  // exclusive. The committed result is left in place.
  static void retainUnpublishedDetection(Project&, const DetectionResultTicket&,
                                         const DetectionRun&);
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

struct DetectionPreviewPresentation {
  DetectionPreviewSnapshot visible;
  std::vector<std::string> hiddenTrackIds;
};

inline std::vector<std::string> detectionPreviewTrackIds(
    const DetectionPreviewSnapshot& preview) {
  std::vector<std::string> ids;
  ids.reserve(preview.events.size());
  for (const auto& event : preview.events)
    if (std::find(ids.begin(), ids.end(), event.trackId) == ids.end())
      ids.push_back(event.trackId);
  return ids;
}

// One rectangle per trackId: the event with the greatest sequence. Folding
// only adjacent duplicates leaves an older box when another track sits between
// two updates of the same id.
inline std::vector<PreviewTrackEvent> latestPreviewEvents(
    const std::vector<PreviewTrackEvent>& events) {
  std::vector<PreviewTrackEvent> latest;
  latest.reserve(events.size());
  for (const auto& event : events) {
    const auto existing =
        std::find_if(latest.begin(), latest.end(), [&](const auto& item) {
          return item.trackId == event.trackId;
        });
    if (existing == latest.end())
      latest.push_back(event);
    else if (event.sequence >= existing->sequence)
      *existing = event;
  }
  return latest;
}

// A disabled intermediate-result option hides a live dashed preview. Successful
// completion clears it. Failure, cancellation, and interruption keep the
// preview and mark it unpublished. The snapshot is not written into file marks.
inline DetectionPreviewPresentation presentDetectionPreview(
    const DetectionPreviewSnapshot& raw, bool terminal, bool showIntermediate,
    bool retainUnpublished = false) {
  DetectionPreviewPresentation result;
  result.visible = raw;
  if (terminal && retainUnpublished) {
    result.visible.sealed = true;
    result.visible.unpublished = !result.visible.events.empty();
    return result;
  }
  if (terminal || !showIntermediate) {
    result.hiddenTrackIds = detectionPreviewTrackIds(raw);
    result.visible.events.clear();
    result.visible.dropped = 0;
    result.visible.unpublished = false;
    if (terminal) result.visible.sealed = true;
  }
  return result;
}
}  // namespace signalstudio
