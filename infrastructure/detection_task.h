#pragma once

#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "domain/detection.h"
#include "domain/project.h"

namespace signalstudio {

struct DetectionPublication {
  AnnotationSourceSet sourceSet;
  std::vector<Annotation> annotations;
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
  bool enqueue(const QString& projectDirectory, const FileMetadata&,
               DetectionRequest, QString& error);
  bool cancel(const std::string& runId);
  void invalidateSource(const std::string& runId, DetectionSourceValidity);
  void cancelGeneration(std::uint64_t generation);
  void pump();
  std::vector<DetectionRun> snapshots() const;
  bool takePublication(const std::string& runId, DetectionPublication&);

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
  };
  void execute(const std::shared_ptr<Job>&);
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
