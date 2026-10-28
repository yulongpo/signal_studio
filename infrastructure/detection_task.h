#pragma once

#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "domain/detection.h"
#include "domain/detection_postprocess.h"
#include "domain/project.h"
#include "infrastructure/detection_profiling.h"

namespace signalstudio {
class DetectionWorkerClient;
struct DetectionSourcePlan;

// Rejects before inference when retained raw heads exceed the configured quota
// or the free space on the project volume. The quota ceiling is that free
// space. An explicit raw-head quota, the legacy combined ledger, and the JSON
// observation budget are still checked. Automatic retention writes the accepted
// byte count into request.rawHeadBudgetBytes.
QString applyDetectionEvidencePreflight(const QString& projectDirectory,
                                        const DetectionSourcePlan& plan,
                                        DetectionRequest& request);
void setDetectionVolumeFreeBytesProbeForTest(
    std::function<std::optional<std::uint64_t>(const QString& path)> probe);

struct QueuedDetectionRun {
  DetectionRun run;
  std::uint32_t executionBatchSize = 1;
  std::uint64_t publicationBudgetBytes = 0;
};
struct DetectionPublication {
  AnnotationSourceSet sourceSet;
  std::vector<Annotation> annotations;
};
struct DetectionPreviewSnapshot {
  std::string runId;
  std::string fileId;
  std::uint64_t projectGeneration = 0;
  std::vector<PreviewTrackEvent> events;
  std::uint64_t dropped = 0;
  bool sealed = false;
  // Failure, cancellation, and interruption keep this preview visible.
  bool unpublished = false;
};
inline bool detectionPrefixRecoveryWouldRepeatFusion(const std::string& error) {
  return error.find("frequency-fusion-comparison-limit-exceeded") != std::string::npos;
}
bool recoverDetectionPublication(const QString& projectDirectory,
                                 const DetectionRun&, DetectionPublication&,
                                 QString& error);

// Writes TensorRT CUDA-event timings as tensorRtInference. QRhi display and
// CUDA DSP are not stored in that object. The legacy "gpu" key is not written.
QJsonObject bindDetectionPerformanceTimings(QJsonObject performance,
                                           const QJsonObject& tensorRtCudaEvents);

// One producer and one worker request in flight. The UI polls lightweight run
// snapshots; spectrum tensors never accumulate in the GUI event queue.
class DetectionTaskManager {
 public:
  using PersistRun =
      std::function<bool(const QString&, const DetectionRun&, QString&)>;
  explicit DetectionTaskManager(QString workerExecutable,
                                PersistRun persistRun = {});
  ~DetectionTaskManager();
  // Validates and fills the queued run without persisting it or starting it.
  // The detection panel runs this, the project save, and the disk budget
  // check off the UI thread. enqueue() persists and queues only after that
  // project commit succeeds.
  bool prepareQueuedRun(const QString& projectDirectory, const FileMetadata&,
                        DetectionRequest, QueuedDetectionRun&,
                        QString& error) const;
  bool canEnqueue(const std::string& runId, QString& error) const;
  // Writes the run manifest without queueing it. Project save reads it back.
  bool persistQueuedRun(const QString& projectDirectory, const DetectionRun&,
                        QString& error) const;
  bool enqueue(const QString& projectDirectory, const FileMetadata&,
               DetectionRequest, QString& error);
  // Queues a run already accepted by prepareQueuedRun. Does not plan again.
  bool queuePreparedRun(const QString& projectDirectory, const FileMetadata&,
                        QueuedDetectionRun, QString& error);
  bool cancel(const std::string& runId);
  void invalidateSource(const std::string& runId, DetectionSourceValidity);
  void cancelGeneration(std::uint64_t generation);
  void pump();
  // Both callbacks run without mutex_ and must not call back into this
  // manager. Production leaves them empty. beforePumpJoin runs only after the
  // task mutex is released and before joining a producer that already
  // finished. beforeProducerTerminalLock runs after finish() and before the
  // terminal preview lock.
  void bindSchedulingProbeForTest(
      std::function<void()> beforeProducerTerminalLock,
      std::function<void()> beforePumpJoin);
  std::vector<DetectionRun> snapshots() const;
  bool takePublication(const std::string& runId, DetectionPublication&);
  // Session preview only. It does not start a worker, persist the run, or
  // write file marks. Later jobs stay later in previewFor's scan.
  void retainPreview(DetectionRun run, DetectionPreviewSnapshot preview);
  // Latest live preview for this file, IQ channel, and project generation.
  // A newer job on another channel is ignored. The snapshot has no channel;
  // callers still match runId against snapshots() before drawing it.
  DetectionPreviewSnapshot previewFor(const std::string& fileId,
                                      std::uint32_t sampleChannel,
                                      std::uint64_t projectGeneration) const;
  // Drops a retained unpublished preview for this file, channel, and generation.
  void discardUnpublishedPreview(const std::string& fileId,
                                 std::uint32_t sampleChannel,
                                 std::uint64_t projectGeneration);

 private:
  struct Job {
    QString directory;
    FileMetadata metadata;
    DetectionRun run;
    std::atomic_bool cancelled = false;
    std::atomic_bool finished = false;
    bool started = false;
    bool finishing = false;
    bool terminalPersisted = false;
    bool publicationTaken = false;
    DetectionPublication publication;
    std::uint32_t executionBatchSize = 1;
    std::uint64_t publicationBytes = 65536, publicationBudgetBytes = 0;
    std::unique_ptr<DetectionWorkerClient> worker;
    DetectionProfiler profiling;
    QJsonObject sourceProfile, workerDiagnostic, gpuProfile, modelProfile;
    DetectionPreviewSnapshot preview;
    std::array<std::uint64_t,17> batches{};
    std::size_t openTracks = 0;
    std::size_t sealedTracks = 0;
    std::uint64_t frameComparisons = 0;
    bool frameSealed = false;
    bool frameFused = false;
    std::uint32_t checkpointBatch = 0;
    qint64 firstDecodedCandidateMs=-1, frequencyReadyMs=-1;
    QElapsedTimer clock;
  };
  void execute(const std::shared_ptr<Job>&);
  std::string performanceSnapshot(const std::shared_ptr<Job>&) const;
  bool checkpoint(const std::shared_ptr<Job>&, QString& error);
  void finish(const std::shared_ptr<Job>&, DetectionLifecycle,
              const QString& error = {});
  QString workerExecutable_;
  PersistRun persistRun_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Job>> jobs_;
  std::shared_ptr<Job> active_;
  std::jthread thread_;
  std::function<void()> beforeProducerTerminalLock_;
  std::function<void()> beforePumpJoin_;
};

}  // namespace signalstudio
