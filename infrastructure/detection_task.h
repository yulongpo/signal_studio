#pragma once

#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "domain/detection.h"
#include "domain/detection_postprocess.h"
#include "domain/project.h"
#include "infrastructure/detection_profiling.h"

namespace signalstudio {
class DetectionWorkerClient;

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
};
bool recoverDetectionPublication(const QString& projectDirectory,
                                 const DetectionRun&, DetectionPublication&,
                                 QString& error);

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
  // startRequest saves that run, then enqueue() persists and queues the same
  // request only after the project file commit succeeds.
  bool prepareQueuedRun(const QString& projectDirectory, const FileMetadata&,
                        DetectionRequest, QueuedDetectionRun&,
                        QString& error) const;
  bool canEnqueue(const std::string& runId, QString& error) const;
  // Writes the run manifest without queueing it. Project save reads it back.
  bool persistQueuedRun(const QString& projectDirectory, const DetectionRun&,
                        QString& error) const;
  bool enqueue(const QString& projectDirectory, const FileMetadata&,
               DetectionRequest, QString& error);
  bool cancel(const std::string& runId);
  void invalidateSource(const std::string& runId, DetectionSourceValidity);
  void cancelGeneration(std::uint64_t generation);
  void pump();
  std::vector<DetectionRun> snapshots() const;
  bool takePublication(const std::string& runId, DetectionPublication&);
  // Live preview only. It is not written into file marks or the project file.
  DetectionPreviewSnapshot previewFor(const std::string& fileId,
                                      std::uint64_t projectGeneration) const;

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
};

}  // namespace signalstudio
