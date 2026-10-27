#pragma once

#include <QStringList>
#include <QWidget>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "application/session.h"
#include "application/detection_result_service.h"
#include "infrastructure/detection_task.h"

class QLabel;
class QListView;
class QTimer;
class QPushButton;
class QAction;

namespace signalstudio {
class DetectionRunListModel;
class DetectionPanel : public QWidget {
  Q_OBJECT
 public:
  DetectionPanel(Session&, std::function<QString()> projectDirectory,
                 QWidget* parent = nullptr);
  ~DetectionPanel() override;
  bool startRequest(DetectionRequest, QString& error);
  void configure(const DetectionRun* parentRun = nullptr);
  void openRunSource(const QString& runId);
  void deriveRun(const QString& runId);
  void refresh();
  void presentLivePreview(DetectionPreviewSnapshot raw, bool terminal);
  void setShowIntermediateResults(bool show);
  bool showIntermediateResults() const { return showIntermediate_; }
  QString clearanceNoteForActive() const;
  void setProjectFilePathProvider(std::function<QString()> provider) {
    projectFilePath_ = std::move(provider);
  }
  void setStartSaveFault(bool fail) { startSaveFault_ = fail; }
  void setPublicationFault(DetectionTransactionOptions options) {
    publicationFault_ = std::move(options);
  }
  void setPublicationGate(std::shared_ptr<std::atomic<bool>> gate) {
    publicationGate_ = std::move(gate);
  }
  bool projectCommitInProgress() const { return commitInFlight_; }
  bool publicationCommitInProgress() const {
    return commitInFlight_ && publicationCommit_;
  }
  // Connect only committed algorithm/parameter changes, not draft refreshes.
  void markConfigurationStale();
  DetectionTaskManager& tasks() { return *tasks_; }

 signals:
  void changed();
  void presentationRequested();
  void commitSettled(bool publication, bool installed);
  void message(const QString&);
  void configurationRequested(const DetectionRun* parentRun);
  void runProgressChanged();
  void runFocused(const QString& runId);
  void previewChanged();
  void previewSelectionDropped(const QStringList& trackIds);

 private:
  struct StartCommit;
  struct PublishCommit;
  void poll();
  void finishStartCommit(const std::shared_ptr<StartCommit>&);
  void finishPublicationCommit(const std::shared_ptr<PublishCommit>&);
  void launchCommitWorker(std::function<void()> body);
  void beginPublicationCommit(DetectionResultTicket ticket, DetectionRun run,
                              DetectionPublication publication);
  void releaseCommitLock();
  void cancelSelected();
  void cancelRun(const QString& runId);
  void showSelectedDetails();
  void showRunDetails(const QString& runId);
  void discoverRecoverable();
  void scheduleGc(const QString& path = {});
  const DetectionRun* selectedRun() const;
  const DetectionRun* findRun(const QString& runId) const;
  bool refreshRunList(bool refreshPublicationCounts);
  void updateSelectionActions();
  std::uint64_t publishedCount(const FileState&, const DetectionRun&);
  struct PublishedCounts {
    bool initialized = false;
    std::size_t annotationCount = 0, sourceSetCount = 0;
    std::unordered_map<std::string, std::uint64_t> runs;
  };
  Session& session_;
  std::function<QString()> projectDirectory_;
  std::function<QString()> projectFilePath_;
  std::unordered_map<std::string, DetectionResultTicket> tickets_;
  std::future<QString> gc_;
  QString pendingGcPath_;
  std::unique_ptr<DetectionTaskManager> tasks_;
  std::uint64_t generation_ = 0;
  QLabel* summary_ = nullptr;
  QListView* list_ = nullptr;
  DetectionRunListModel* model_ = nullptr;
  QPushButton* details_ = nullptr;
  QPushButton* derive_ = nullptr;
  QPushButton* recover_ = nullptr;
  QAction* taskViewSourceAction_ = nullptr;
  QAction* taskDetailsAction_ = nullptr;
  QAction* taskDeriveAction_ = nullptr;
  QAction* taskCancelAction_ = nullptr;
  std::optional<DetectionRun> configurationParent_;
  std::unordered_map<std::string, PublishedCounts> publishedCounts_;
  std::uint64_t listGeneration_ = 0;
  QTimer* timer_ = nullptr;
  DetectionPreviewSnapshot livePreview_;
  DetectionPreviewSnapshot rawPreview_;
  bool rawPreviewTerminal_ = false;
  bool showIntermediate_ = true;
  std::unordered_set<std::string> clearedScopes_;
  std::uint64_t previewToken_ = 0;
  std::uint64_t previewDropped_ = 0;
  std::size_t previewCount_ = 0;
  bool previewSealed_ = false;
  std::string previewFile_;
  bool commitInFlight_ = false;
  bool publicationCommit_ = false;
  bool startSaveFault_ = false;
  std::atomic<bool> shuttingDown_{false};
  std::string commitScope_;
  std::string committingRunId_;
  DetectionTransactionOptions publicationFault_;
  std::shared_ptr<std::atomic<bool>> publicationGate_;
  std::mutex commitThreadMutex_;
  std::vector<std::thread> commitThreads_;
  qint64 lastRejectMessageMs_ = 0;
 public:
  const DetectionPreviewSnapshot& livePreview() const { return livePreview_; }
};
}  // namespace signalstudio
