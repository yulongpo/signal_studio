#pragma once
#include <QModelIndex>
#include <QWidget>

#include "application/session.h"
class QTableView;
class QComboBox;
class QLabel;
class QCheckBox;
class QVBoxLayout;
class QDoubleSpinBox;
namespace signalstudio {
class UnsignedValueEdit;
class AdaptiveValueEdit;
class AnnotationTableModel;
class AnnotationEvidenceWidget;
class AnnotationPanel final : public QWidget {
  Q_OBJECT
 public:
  explicit AnnotationPanel(Session&, QWidget* parent = nullptr);
  QWidget* inspectorWidget() const { return inspector_; }
  QWidget* layerWidget() const { return layerBar_; }
  const std::vector<std::string>& sortedVisibleIds() const;
  void refresh();
  void revealActive();
  void refreshHover();
  void discardDrafts();
  bool importLabels(const QString&, QString* error = nullptr,
                    bool preview = true);
 signals:
  void changed();
  void hoverChanged(const QString& id, const QString& segmentId);
  void message(const QString&);

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
  void geometry(int);
  void exportFile(bool csv);
  Session& session_;
  AnnotationTableModel* model_ = nullptr;
  QTableView* table_ = nullptr;
  QComboBox* source_ = nullptr;
  QComboBox* status_ = nullptr;
  QComboBox* segments_ = nullptr;
  QComboBox* run_ = nullptr;
  QDoubleSpinBox* minimumScore_ = nullptr;
  QCheckBox* layers_[3]{};
  QLabel* count_ = nullptr;
  QCheckBox* selectAll_ = nullptr;
  QLabel* detail_ = nullptr;
  QLabel* error_ = nullptr;
  QWidget* inspector_ = nullptr;
  QWidget* layerBar_ = nullptr;
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
};
}  // namespace signalstudio
