#pragma once

#include <QString>
#include <QWidget>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/session.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace signalstudio {
class AdaptiveValueEdit;
class ParameterInputPolicy;
class UnsignedValueEdit;

// The containing file section owns scrolling and collapse. This card only
// edits the next request; refresh never loads parameters from a running task.
class DetectionCard : public QWidget {
  Q_OBJECT

 public:
  DetectionCard(Session&, std::function<QString()> projectDirectory,
                QWidget* parent = nullptr);
  void refresh();
  void configure(const DetectionRun* parentRun = nullptr);
  void setRequestedRoi(ViewRange);
  void useVisibleRange();
  void focusRun(const QString& runId);
  DetectionRequest request() const;
  bool showIntermediateResults() const;
  QString validationError() const;
  QWidget* actionsWidget() const { return actions_; }

 signals:
  void requestSubmitted(const DetectionRequest&);
  void intermediateResultsToggled(bool shown);
  void roiSelectionRequested();
  void selectionChanged();
  void configurationCommitted();
  void cancelRunRequested(const QString& runId);
  void focusRunningRunRequested(const QString& runId);
  void message(const QString&);

 private:
  void setupProfile(QVBoxLayout*);
  void setupRange(QVBoxLayout*);
  void setupTiming(QVBoxLayout*);
  void setupThresholds(QVBoxLayout*);
  void setupStrategy(QVBoxLayout*);
  void setupBranches(QVBoxLayout*);
  void setupAdvanced(QVBoxLayout*);
  void setupStatus(QVBoxLayout*);
  QWidget* pairedFields(const QString&, QWidget*, const QString&, QWidget*);
  AdaptiveValueEdit* addParameter(const char* name, const QString& label,
                                  double DetectionParameters::* member,
                                  double maximum, double scale = 1);
  UnsignedValueEdit* addUnsigned(
      const char* name, const QString& label, std::uint64_t maximum,
      std::uint64_t minimum,
      std::function<std::uint64_t(const DetectionRequest&)> getter,
      std::function<void(DetectionRequest&, std::uint64_t)> setter);
  void switchProfile();
  void saveConfiguration(DetectionRequest);
  void rejectControl(QLineEdit*, const QString& error,
                     const std::function<void()>& restore);
  void loadControls();
  void refreshSource();
  void refreshSegments();
  void refreshTiming();
  void refreshBranchState();
  void refreshStatus();
  void submit();
  bool hasDraft() const;
  QString configurationError(const DetectionRequest&) const;
  QString rangeError(const DetectionRequest&) const;
  QString sourceValidationError() const;
  QString profileValidationError(const DetectionRequest&) const;
  QString contextValidationError(const DetectionRequest&) const;
  const Annotation* selectedAnnotation() const;
  const Segment* selectedSegment() const;
  const DetectionRun* latestRun() const;
  const DetectionRun* unfinishedRun() const;
  const DetectionRun* focusedUnfinished() const;
  ViewRange availableRange() const;

  struct ParameterControl {
    AdaptiveValueEdit* edit;
    double DetectionParameters::* member;
    double scale;
    double maximum;
    QString label;
  };
  struct UnsignedControl {
    UnsignedValueEdit* edit;
    std::function<std::uint64_t(const DetectionRequest&)> getter;
  };

  Session& session_;
  std::function<QString()> projectDirectory_;
  ParameterInputPolicy* inputPolicy_ = nullptr;
  DetectionRequest configuration_;
  std::map<std::string, DetectionRequest> profileConfigurations_;
  std::optional<FileMetadata> metadata_;
  SourceIdentity source_;
  QString contextKey_;
  std::optional<ViewRange> requestedRoi_;
  std::string segmentAnnotationId_, segmentRevisionId_;
  bool updating_ = false;
  QComboBox* model_ = nullptr;
  QComboBox* spectrumMode_ = nullptr;
  QComboBox* rangeKind_ = nullptr;
  QComboBox* activeSegment_ = nullptr;
  QLabel* rangeSummary_ = nullptr;
  QLabel* physicalTimes_ = nullptr;
  QLabel* calculationDetails_ = nullptr;
  QLabel* diagnostics_ = nullptr;
  QLabel* branchHelp_ = nullptr;
  QLabel* draftNote_ = nullptr;
  QLabel* preflight_ = nullptr;
  QLabel* runSummary_ = nullptr;
  std::array<QLabel*, 5> phaseSteps_{};
  QWidget* phaseBar_ = nullptr;
  std::string displayRunId_;
  AdaptiveValueEdit* hopMilliseconds_ = nullptr;
  AdaptiveValueEdit* noiseMargin_ = nullptr;
  AdaptiveValueEdit* edgeFraction_ = nullptr;
  AdaptiveValueEdit* smoothMilliseconds_ = nullptr;
  AdaptiveValueEdit* minOnMilliseconds_ = nullptr;
  AdaptiveValueEdit* minOffMilliseconds_ = nullptr;
  AdaptiveValueEdit* splitGapMilliseconds_ = nullptr;
  AdaptiveValueEdit* minBurstMilliseconds_ = nullptr;
  QWidget* fineFields_ = nullptr;
  QWidget* fineSplitFields_ = nullptr;
  QWidget* coarseSplitFields_ = nullptr;
  QWidget* marginFields_ = nullptr;
  QButtonGroup* strategies_ = nullptr;
  QCheckBox* splitBursts_ = nullptr;
  QCheckBox* showIntermediate_ = nullptr;
  QPushButton* selectRoi_ = nullptr;
  QWidget* actions_ = nullptr;
  QPushButton* run_ = nullptr;
  QPushButton* cancel_ = nullptr;
  QPushButton* viewRunning_ = nullptr;
  QLabel* otherRun_ = nullptr;
  std::vector<ParameterControl> parameters_;
  std::vector<UnsignedControl> unsignedControls_;
};
}  // namespace signalstudio
