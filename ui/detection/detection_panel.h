#pragma once

#include <QWidget>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include "application/session.h"
#include "infrastructure/detection_task.h"

class QLabel;
class QListView;
class QTimer;
class QPushButton;

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
  DetectionTaskManager& tasks() { return *tasks_; }

 signals:
  void changed();
  void message(const QString&);
  void configurationRequested(const DetectionRun* parentRun);
  void runProgressChanged();
  void runFocused(const QString& runId);

 private:
  void poll();
  void cancelSelected();
  void showSelectedDetails();
  void discoverRecoverable();
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
  std::unique_ptr<DetectionTaskManager> tasks_;
  std::uint64_t generation_ = 0;
  QLabel* summary_ = nullptr;
  QListView* list_ = nullptr;
  DetectionRunListModel* model_ = nullptr;
  QPushButton* details_ = nullptr;
  QPushButton* derive_ = nullptr;
  QPushButton* recover_ = nullptr;
  std::optional<DetectionRun> configurationParent_;
  std::unordered_map<std::string, PublishedCounts> publishedCounts_;
  std::uint64_t listGeneration_ = 0;
  QTimer* timer_ = nullptr;
};
}  // namespace signalstudio
