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
  // Explicit detect/redetect only. Removes the committed formal result for
  // this file and sample channel before the new run is saved. Manual marks
  // and imported labels stay. Failure does not restore the removed result.
  static bool clearCommittedDetection(Project&, const std::string& fileId,
                                      std::uint32_t sampleChannel,
                                      bool& clearedPrevious, QString& error);

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

// Terminal runs and a disabled intermediate-result option hide dashed
// previews. The snapshot is not written into file marks.
inline DetectionPreviewPresentation presentDetectionPreview(
    const DetectionPreviewSnapshot& raw, bool terminal,
    bool showIntermediate) {
  DetectionPreviewPresentation result;
  result.visible = raw;
  if (terminal || !showIntermediate) {
    result.hiddenTrackIds = detectionPreviewTrackIds(raw);
    result.visible.events.clear();
    result.visible.dropped = 0;
    if (terminal) result.visible.sealed = true;
  }
  return result;
}
}  // namespace signalstudio
