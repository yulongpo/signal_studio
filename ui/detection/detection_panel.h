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
  void presentLivePreview(DetectionPreviewSnapshot raw, bool terminal,
                         bool retainUnpublished = false);
  // Draws this file, IQ channel, and project generation. A newer snapshot from
  // another channel does not replace this channel's live tracks. A preview
  // whose run has left the snapshot list is cleared.
  void projectScopedPreview(DetectionPreviewSnapshot preview,
                            const std::vector<DetectionRun>& runs);
  // File, sample channel, or project generation changed: publish the current
  // scope now, instead of waiting for the next poll.
  void syncPreviewScope();
  QString previewAbsenceNote() const { return previewAbsenceNote_; }
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
  // Last startRequest that armed a start transaction. Read these only after
  // that call returns true; a later rejection does not clear them.
  const std::string& acceptedRunId() const { return acceptedRunId_; }
  std::uint64_t acceptedSteadyNs() const { return acceptedSteadyNs_; }
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
  // GUI thread. Start was hidden, then rejected before publication
  // (preflight, save, or a generation change). The card latch lives on
  // DetectionCard; main_window connects this to noteStartRejected.
  void startRejected(QString runId, QString message);

 private:
  struct StartCommit;
  struct PublishCommit;
  void poll();
  void publishPolledPreview();
  void rememberViewScope();
  bool viewScopeChanged() const;
  void runStartCommit(const std::shared_ptr<StartCommit>&);
  void finishStartCommit(const std::shared_ptr<StartCommit>&);
  void finishPublicationCommit(const std::shared_ptr<PublishCommit>&);
  void launchCommitWorker(std::function<void()> body);
  void beginPublicationCommit(DetectionResultTicket ticket, DetectionRun run,
                              DetectionPublication publication);
  bool publicationGenerationCurrent(std::uint64_t epoch, std::uint64_t generation,
                                    const std::string& fileId) const;
  void releaseCommitLock();
  QString commitBusyMessage() const;
  void clearUnpublishedPreview(const std::string& fileId, std::uint32_t channel);
  void suppressFormalDisplay(const std::string& fileId, std::uint32_t channel);
  void restoreFormalDisplay(const std::string& fileId);
  bool showingPreview() const;
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
  std::uint64_t previewToken_ = 0;
  std::uint64_t previewDropped_ = 0;
  std::size_t previewCount_ = 0;
  bool previewSealed_ = false;
  std::string previewFile_;
  bool previewRunActive_ = false;
  std::string previewPhase_;
  QString previewAbsenceNote_;
  std::string viewedFile_;
  std::uint32_t viewedChannel_ = 0;
  std::uint64_t viewedGeneration_ = 0;
  bool viewScopeReady_ = false;
  bool commitInFlight_ = false;
  bool publicationCommit_ = false;
  std::string acceptedRunId_;
  std::uint64_t acceptedSteadyNs_ = 0;
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
