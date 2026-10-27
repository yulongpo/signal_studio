#pragma once
#include <QModelIndex>
#include <QPointer>
#include <QWidget>
#include <array>
#include <optional>

#include "application/session.h"
class QTableView;
class QComboBox;
class QLabel;
class QCheckBox;
class QVBoxLayout;
class QDoubleSpinBox;
class QLineEdit;
class QStackedWidget;
namespace signalstudio {
class UnsignedValueEdit;
class AdaptiveValueEdit;
class AnnotationTableModel;
class AnnotationEvidenceWidget;
class AnnotationPanel final : public QWidget {
  Q_OBJECT
 public:
  explicit AnnotationPanel(Session&, QWidget* parent = nullptr);
  ~AnnotationPanel() override;
  QWidget* inspectorWidget() const { return inspector_; }
  const std::vector<std::string>& sortedVisibleIds() const;
  void refresh();
  void revealActive();
  void refreshHover();
  void discardDrafts();
  void editRange();
  void deleteSelected(bool currentSegmentOnly = false);
  void showImportLabelsDialog();
  bool importLabels(const QString&, QString* error = nullptr,
                    bool preview = true);
  void setSourceTab(AnnotationSource, bool reconcile = true);
  void setDetectionPreviewNote(const QString& note);
  AnnotationSource sourceTab() const { return sourceTab_; }
  void convertSelected(bool currentSegmentOnly = false,
                       bool allowExplicitDuplicate = false);
 signals:
  void changed();
  void hoverChanged(const QString& id, const QString& segmentId);
  void message(const QString&);
  void createChannelRequested();
  void sourceTabChanged(int source);

 protected:
  bool eventFilter(QObject*, QEvent*) override;

 private:
  void buildFilters(QVBoxLayout*);
  void buildReviewBar(QVBoxLayout*);
  void buildTable(QVBoxLayout*);
  void buildInspector();
  void refreshTableSelection(const FileState*);
  void refreshInspector(const FileState*);
  void refreshEvidence(const Annotation*);
  void selectAdjacentSegment(int offset);
  void action(const AnnotationResult&);
  void chooseRow(const QModelIndex&, Qt::KeyboardModifiers, bool toggle);
  void filterChanged();
  void applyOverlaySource(FileState&, AnnotationSource);
  void geometry(int);
  void editCell(const QModelIndex&);
  bool finishCell(bool commit, bool focusTable = false);
  void queueRefresh();
  void exportFile(bool csv);
  Session& session_;
  AnnotationTableModel* model_ = nullptr;
  QTableView* table_ = nullptr;
  AnnotationSource sourceTab_ = AnnotationSource::ImportedLabel;
  QStackedWidget* tablePages_ = nullptr;
  struct TabState {
    QTableView* table = nullptr;
    AnnotationTableModel* model = nullptr;
    QCheckBox* selectAll = nullptr;
    std::size_t page = 0;
    AnnotationViewFilter filter;
    QString query;
  };
  std::array<TabState, 3> tabs_{};
  QLineEdit* query_ = nullptr;
  QComboBox* status_ = nullptr;
  QComboBox* segments_ = nullptr;
  QDoubleSpinBox* minimumScore_ = nullptr;
  QLabel* count_ = nullptr;
  QLabel* detectionStatus_ = nullptr;
  QString detectionPreviewNote_;
  QCheckBox* selectAll_ = nullptr;
  QLabel* detail_ = nullptr;
  QLabel* rangeSummary_ = nullptr;
  QLabel* error_ = nullptr;
  QWidget* inspector_ = nullptr;
  QWidget* geometryEditor_ = nullptr;
  QLabel* readOnlyRange_ = nullptr;
  AnnotationEvidenceWidget* evidencePanel_ = nullptr;
  AdaptiveValueEdit* begin_ = nullptr;
  AdaptiveValueEdit* end_ = nullptr;
  AdaptiveValueEdit* low_ = nullptr;
  AdaptiveValueEdit* high_ = nullptr;
  std::size_t page_ = 0;
  bool refreshing_ = false, revealActive_ = false;
  std::string displayedActiveId_;
  std::string context_;
  std::string filterContext_;
  struct CellDraft {
    AnnotationEditContext context;
    int column = 1;
    ViewRange range;
    QString initialText;
  };
  std::optional<CellDraft> cellDraft_;
  QPointer<QLineEdit> cellEditor_;
  std::array<std::optional<AnnotationEditContext>, 4> inlineContexts_;
  bool deferredRefresh_ = false, refreshQueued_ = false;
};
}  // namespace signalstudio
