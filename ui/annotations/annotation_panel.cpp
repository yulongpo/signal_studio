#include "ui/annotations/annotation_panel.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "infrastructure/annotation_json.h"
#include "infrastructure/label_import.h"
#include "ui/label_mapping_dialog.h"
#include "ui/background_operation.h"
#include "ui/annotations/annotation_evidence_widget.h"
#include "ui/controls/adaptive_value_edit.h"
#include "ui/controls/unsigned_value_edit.h"

namespace signalstudio {
namespace {
QString q(const std::string& s) { return QString::fromUtf8(s); }
QString sourceName(AnnotationSource s) {
  return s == AnnotationSource::ImportedLabel ? "原始标签"
         : s == AnnotationSource::Manual      ? "手动"
                                              : "算法";
}
QString reviewName(ReviewStatus s) {
  return s == ReviewStatus::Accepted   ? "已采纳"
         : s == ReviewStatus::Rejected ? "已忽略"
                                       : "待判断";
}
QString timeQualityName(TimeQuality quality) {
  switch (quality) {
    case TimeQuality::SampleLabel:
      return "样本标签";
    case TimeQuality::RoundedLabel:
      return "量化标签";
    case TimeQuality::AlgorithmEstimated:
      return "算法估计";
    case TimeQuality::Manual:
      return "人工";
    case TimeQuality::WindowObservation:
      return "窗口观察；时间未定位";
    case TimeQuality::Unknown:
      return "时间未定位";
  }
  return "时间未定位";
}
QString physicalRangeText(const ViewRange& range, double sampleRate) {
  return QString("%1–%2 ms · %3–%4 MHz")
      .arg(static_cast<double>(range.time.begin) / sampleRate * 1000, 0, 'g',
           10)
      .arg(static_cast<double>(range.time.end) / sampleRate * 1000, 0, 'g', 10)
      .arg(range.frequency.lowerHz / 1e6, 0, 'g', 12)
      .arg(range.frequency.upperHz / 1e6, 0, 'g', 12);
}
std::optional<FrequencyRange> rowFrequency(const Annotation& value) {
  if (const auto* segment = activeSegment(value))
    return segment->range.frequency;
  return value.observation
             ? std::optional<FrequencyRange>{value.observation->frequency}
             : std::nullopt;
}
AnnotationSelectionMode selectionMode(Qt::KeyboardModifiers modifiers) {
  if (modifiers & Qt::ShiftModifier)
    return modifiers & Qt::ControlModifier
               ? AnnotationSelectionMode::ExtendRange
               : AnnotationSelectionMode::Range;
  return modifiers & Qt::ControlModifier ? AnnotationSelectionMode::Toggle
                                         : AnnotationSelectionMode::Replace;
}
QPushButton* button(QHBoxLayout* l, QString title, QString id,
                    std::function<void()> fn) {
  auto* b = new QPushButton(title);
  b->setObjectName(id);
  l->addWidget(b);
  QObject::connect(b, &QPushButton::clicked, b, std::move(fn));
  return b;
}
}  // namespace
class AnnotationTableModel final : public QAbstractTableModel {
 public:
  Session& session;
  std::vector<const Annotation*> rows;
  std::vector<Annotation> snapshots;
  std::vector<std::string> filteredIds;
  std::unordered_map<std::string, int> channelCounts;
  std::unordered_set<std::string> selectedIds;
  std::function<void()> selectionChanged;
  std::size_t total = 0;
  int order = 1;
  Qt::SortOrder direction = Qt::AscendingOrder;
  explicit AnnotationTableModel(Session& s, QObject* parent)
      : QAbstractTableModel(parent), session(s) {}
  int rowCount(const QModelIndex& p = {}) const override {
    return p.isValid() ? 0 : static_cast<int>(rows.size());
  }
  int columnCount(const QModelIndex& p = {}) const override {
    return p.isValid() ? 0 : 11;
  }
  QVariant headerData(int column, Qt::Orientation orientation,
                      int role) const override {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return QStringList{
        "",          "名称 / annotationId", "来源",       "当前段", "起始 / ms",
        "结束 / ms", "中心 / MHz",          "带宽 / kHz", "mean",   "复核",
        "通道"}
        .value(column);
  }
  QVariant data(const QModelIndex& i, int role) const override {
    if (!i.isValid() || i.row() >= rowCount()) return {};
    const auto& a = *rows[static_cast<std::size_t>(i.row())];
    const auto* file = session.activeFile();
    if (!file) return {};
    const bool selected = selectedIds.contains(a.id);
    const bool current = file->activeMarkId == a.id;
    const bool hovered = file->hoveredMarkId == a.id;
    if (role == Qt::UserRole) return q(a.id);
    if (role == Qt::UserRole + 1)
      return (selected ? 1 : 0) | (current ? 2 : 0) | (hovered ? 4 : 0);
    if (role == Qt::CheckStateRole && i.column() == 0)
      return selected ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::ToolTipRole)
      return q(a.id) + "\n" + q(a.sourceSetId) + "\n" +
             (a.classLabel ? q(*a.classLabel) : "类别未知") + "\n" +
             (activeSegment(a) ? "几何列仅显示当前活动段；其它段不计入范围"
                               : "时间未定位；观察窗口不作为活动段");
    if (role == Qt::ForegroundRole)
      return QColor(a.sourceKind == AnnotationSource::ImportedLabel ? "#65d2bd"
                    : a.sourceKind == AnnotationSource::Manual      ? "#edc56d"
                                                               : "#51c4e9");
    if (role == Qt::BackgroundRole) {
      if (current) return QColor("#205d7c");
      if (selected) return QColor("#183f59");
      if (hovered) return QColor("#22425a");
      return {};
    }
    if (role == Qt::FontRole && current) {
      QFont font;
      font.setBold(true);
      return font;
    }
    if (role != Qt::DisplayRole) return {};
    const auto* segment = activeSegment(a);
    const auto frequency = rowFrequency(a);
    const auto channel = channelCounts.find(a.id);
    switch (i.column()) {
      case 1:
        return q(a.name) + "\n" + q(a.id).left(24) + " · r" +
               QString::number(a.revisions.size());
      case 2:
        return sourceName(a.sourceKind);
      case 3:
        return segment ? QString("第 %1/%2 段")
                             .arg(segment - a.segments.data() + 1)
                             .arg(a.segments.size())
                       : QString("时间未定位");
      case 4:
      case 5:
        return segment
                   ? QString::number(
                         static_cast<double>(i.column() == 4
                                                 ? segment->range.time.begin
                                                 : segment->range.time.end) /
                             file->metadata.sampleRateHz * 1000,
                         'g', 10)
                   : QString("—");
      case 6:
        return frequency ? QString::number(
                               (frequency->lowerHz + frequency->upperHz) / 2e6,
                               'g', 12)
                         : QString("—");
      case 7:
        return frequency ? QString::number(
                               (frequency->upperHz - frequency->lowerHz) / 1000,
                               'g', 9)
                         : QString("—");
      case 8:
        return a.originEvidence && a.originEvidence->scoreMean
                   ? QString::number(*a.originEvidence->scoreMean, 'g', 4)
                   : QString("—");
      case 9:
        return reviewName(a.reviewStatus);
      case 10:
        return channel != channelCounts.end()
                   ? QString("%1 个快照").arg(channel->second)
                   : QString("—");
      default:
        return {};
    }
  }
  Qt::ItemFlags flags(const QModelIndex& index) const override {
    auto flags = QAbstractTableModel::flags(index);
    if (index.column() == 0) flags |= Qt::ItemIsUserCheckable;
    const auto* file = session.activeFile();
    if (!file || !index.isValid() || index.row() >= rowCount()) return flags;
    const auto& annotation = *rows[static_cast<std::size_t>(index.row())];
    if (session.annotations().canEdit(*file, annotation.id) &&
        (index.column() == 1 ||
         ((index.column() == 4 || index.column() == 5) &&
          activeSegment(annotation)) ||
         ((index.column() == 6 || index.column() == 7) &&
          rowFrequency(annotation))))
      flags |= Qt::ItemIsEditable;
    return flags;
  }
  bool setData(const QModelIndex& i, const QVariant& value, int role) override {
    auto* file = session.activeFile();
    if (!file || !i.isValid() || i.row() >= rowCount() || i.column() != 0 ||
        role != Qt::CheckStateRole)
      return false;
    if (data(i, Qt::CheckStateRole).toInt() == value.toInt()) return true;
    const auto result = session.annotations().chooseSelection(
        *file, rows[static_cast<std::size_t>(i.row())]->id,
        AnnotationSelectionMode::Toggle, filteredIds);
    if (!result.ok) return false;
    if (selectionChanged) selectionChanged();
    return true;
  }
  void update(std::size_t& page, bool revealActive) {
    beginResetModel();
    rows.clear();
    channelCounts.clear();
    selectedIds.clear();
    if (const auto* file = session.activeFile()) {
      selectedIds.insert(file->selectedMarkIds.begin(),
                         file->selectedMarkIds.end());
      for (const auto& a : file->marks)
        if (annotationVisible(*file, a)) rows.push_back(&a);
      for (const auto& channel : file->channels)
        ++channelCounts[channel.annotationId.empty() ? channel.sourceMarkId
                                                     : channel.annotationId];
    }
    std::stable_sort(rows.begin(), rows.end(), [&](auto* a, auto* b) {
      const auto number =
          [&](const Annotation& value) -> std::optional<double> {
        const auto* segment = activeSegment(value);
        const auto frequency = rowFrequency(value);
        switch (order) {
          case 2:
            return static_cast<int>(value.sourceKind);
          case 3:
            return static_cast<double>(value.segments.size());
          case 4:
          case 5:
            return segment ? std::optional<double>{static_cast<double>(
                                 order == 4 ? segment->range.time.begin
                                            : segment->range.time.end)}
                           : std::nullopt;
          case 6:
          case 7:
            if (!frequency) return std::nullopt;
            return order == 6 ? (frequency->lowerHz + frequency->upperHz) / 2
                              : frequency->upperHz - frequency->lowerHz;
          case 8:
            return value.originEvidence ? value.originEvidence->scoreMean
                                        : std::nullopt;
          case 9:
            return static_cast<int>(value.reviewStatus);
          case 10:
            return channelCounts.contains(value.id) ? channelCounts.at(value.id)
                                                    : 0;
          default:
            return std::nullopt;
        }
      };
      int comparison = 0;
      if (order == 4 || order == 5) {
        const auto* left = activeSegment(*a);
        const auto* right = activeSegment(*b);
        if (static_cast<bool>(left) != static_cast<bool>(right))
          return static_cast<bool>(left);
        if (left) {
          const auto first =
              order == 4 ? left->range.time.begin : left->range.time.end;
          const auto second =
              order == 4 ? right->range.time.begin : right->range.time.end;
          comparison = first < second ? -1 : first > second ? 1 : 0;
        }
      } else if (order == 1) {
        comparison = a->name.compare(b->name);
      } else {
        const auto left = number(*a), right = number(*b);
        if (left.has_value() != right.has_value()) return left.has_value();
        if (left) comparison = *left < *right ? -1 : *left > *right ? 1 : 0;
      }
      if (comparison == 0) return a->id < b->id;
      return direction == Qt::AscendingOrder ? comparison < 0 : comparison > 0;
    });
    total = rows.size();
    filteredIds.clear();
    filteredIds.reserve(total);
    for (const auto* annotation : rows) filteredIds.push_back(annotation->id);
    if (revealActive)
      if (const auto* file = session.activeFile()) {
        const auto active = std::find(filteredIds.begin(), filteredIds.end(),
                                      file->activeMarkId);
        if (active != filteredIds.end())
          page = static_cast<std::size_t>(active - filteredIds.begin()) / 200;
      }
    page = std::min(page, total == 0 ? std::size_t{0} : (total - 1) / 200);
    const auto offset = std::min(page * 200, total);
    const auto finish = std::min(offset + 200, total);
    rows = std::vector<const Annotation*>(
        rows.begin() + static_cast<std::ptrdiff_t>(offset),
        rows.begin() + static_cast<std::ptrdiff_t>(finish));
    // A worker can reallocate file.marks while an editor keeps this page
    // visible. Keep only the displayed page as stable snapshots until refresh.
    snapshots.clear();
    snapshots.reserve(rows.size());
    for (const auto* annotation : rows) snapshots.push_back(*annotation);
    rows.clear();
    for (const auto& annotation : snapshots) rows.push_back(&annotation);
    endResetModel();
  }
};

class AnnotationSelectionDelegate final : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    auto style = option;
    style.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus |
                     QStyle::State_MouseOver);
    QStyledItemDelegate::paint(painter, style, index);
    const auto state = index.data(Qt::UserRole + 1).toInt();
    if (!(state & 7)) return;
    painter->save();
    QPen pen(state & 2   ? QColor("#a7ecff")
             : state & 1 ? QColor("#6ab1d5")
                         : QColor("#c0ecff"),
             state & 2 ? 2 : 1);
    if (!(state & 3)) pen.setStyle(Qt::DotLine);
    painter->setPen(pen);
    const auto box = option.rect.adjusted(0, 1, -1, -2);
    painter->drawLine(box.topLeft(), box.topRight());
    painter->drawLine(box.bottomLeft(), box.bottomRight());
    if (index.column() == 0) painter->drawLine(box.topLeft(), box.bottomLeft());
    if (index.column() == index.model()->columnCount() - 1)
      painter->drawLine(box.topRight(), box.bottomRight());
    if ((state & 4) && (state & 3)) {
      painter->setPen(QPen(QColor("#d4f2ff"), 1, Qt::DotLine));
      painter->drawLine(box.topLeft() + QPoint(0, 2),
                        box.topRight() + QPoint(0, 2));
    }
    painter->restore();
  }
};

class AnnotationTableView final : public QTableView {
 public:
  using QTableView::QTableView;
  std::function<void(const QModelIndex&, Qt::KeyboardModifiers, bool)> choose;
  std::function<void(const QModelIndex&)> locate;
  std::function<void(const QModelIndex&)> editCell;

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid()) {
      setFocus(Qt::MouseFocusReason);
      setCurrentIndex(index);
      setProperty("annotationCurrentColumn", index.column());
      if (choose) choose(index, event->modifiers(), index.column() == 0);
      event->accept();
      return;
    }
    QTableView::mousePressEvent(event);
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      event->accept();
      return;
    }
    QTableView::mouseReleaseEvent(event);
  }
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid()) {
      if (index.flags() & Qt::ItemIsEditable) {
        if (editCell) editCell(index);
      } else if (locate) {
        locate(index);
      }
      event->accept();
      return;
    }
    QTableView::mouseDoubleClickEvent(event);
  }
};

class AnnotationCellEditor final : public QLineEdit, public ParameterDraft {
 public:
  using QLineEdit::QLineEdit;
  std::function<void(bool, bool)> finish;
  void commit() override {
    if (finish) finish(true, false);
  }
  void discard() override {
    if (finish) finish(false, false);
  }

 protected:
  void focusOutEvent(QFocusEvent* event) override {
    QLineEdit::focusOutEvent(event);
    if (event->reason() == Qt::ActiveWindowFocusReason ||
        event->reason() == Qt::PopupFocusReason ||
        event->reason() == Qt::MenuBarFocusReason)
      return;
    QTimer::singleShot(0, this, [this] {
      auto* target = QApplication::focusWidget();
      if (!isVisible() || !target || target->window() != window()) return;
      for (auto* owner = target; owner; owner = owner->parentWidget())
        if (owner->property("preserveParameterDrafts").toBool()) return;
      if (finish) finish(true, false);
    });
  }
  bool event(QEvent* event) override {
    if (event->type() == QEvent::KeyPress) {
      const auto key = static_cast<QKeyEvent*>(event)->key();
      if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab ||
          key == Qt::Key_Backtab || key == Qt::Key_Escape) {
        if (finish) finish(key != Qt::Key_Escape, true);
        event->accept();
        return true;
      }
    }
    return QLineEdit::event(event);
  }
};

AnnotationPanel::AnnotationPanel(Session& s, QWidget* parent)
    : QWidget(parent), session_(s) {
  setObjectName("annotationPanel");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(6, 4, 6, 4);
  outer->setSpacing(4);
  buildFilters(outer);
  buildReviewBar(outer);
  buildTable(outer);
  buildInspector();
  qApp->installEventFilter(this);
}
AnnotationPanel::~AnnotationPanel() { qApp->removeEventFilter(this); }
const std::vector<std::string>& AnnotationPanel::sortedVisibleIds() const {
  return model_->filteredIds;
}
void AnnotationPanel::buildFilters(QVBoxLayout* outer) {
  auto* bar = new QHBoxLayout;
  source_ = new QComboBox;
  source_->setObjectName("annotationSourceFilter");
  source_->addItems({"全部来源", "原始标签", "手动", "算法"});
  bar->addWidget(source_);
  status_ = new QComboBox;
  status_->setObjectName("annotationReviewFilter");
  status_->addItems({"全部质量", "待判断", "已采纳", "已忽略"});
  bar->addWidget(status_);
  run_ = new QComboBox;
  run_->setObjectName("annotationRunFilter");
  run_->setAccessibleName("按检测运行筛选");
  // Use list-item sizing for dynamic IDs. The menu-style combo delegate can
  // add its reserved menu width again after the popup is constrained by QSS.
  run_->setItemDelegate(new QStyledItemDelegate(run_));
  run_->addItem("全部运行", QString{});
  bar->addWidget(run_);
  minimumScore_ = new ParameterDoubleSpinBox;
  minimumScore_->setObjectName("annotationMinimumMeanScore");
  minimumScore_->setRange(0, 1);
  minimumScore_->setDecimals(2);
  minimumScore_->setSingleStep(.05);
  minimumScore_->setPrefix("mean ≥ ");
  minimumScore_->setValue(0);
  bar->addWidget(minimumScore_);
  connect(run_, &QComboBox::currentIndexChanged, this,
          [this] { filterChanged(); });
  connect(minimumScore_, &QDoubleSpinBox::valueChanged, this,
          [this] { filterChanged(); });
  layerBar_ = new QWidget;
  auto* layerRow = new QHBoxLayout(layerBar_);
  layerRow->setContentsMargins(0, 0, 0, 0);
  layerRow->setSpacing(15);
  for (int i = 0; i < 3; ++i) {
    layers_[i] = new QCheckBox(sourceName(static_cast<AnnotationSource>(i)));
    layers_[i]->setChecked(true);
    layers_[i]->setObjectName(QString("annotationLayer%1").arg(i));
    layerRow->addWidget(layers_[i]);
    connect(layers_[i], &QCheckBox::toggled, this, [this, i](bool on) {
      if (refreshing_) return;
      if (auto* f = session_.activeFile()) {
        f->annotationLayers[i] = on;
        session_.annotations().reconcileSelection(*f);
        emit changed();
      }
    });
  }
  count_ = new QLabel;
  count_->setObjectName("annotationCounts");
  bar->addWidget(count_, 1);
  button(bar, "导入标签…", "importLabels", [this] { showImportLabelsDialog(); });
  button(bar, "JSON", "exportAnnotationJson", [this] { exportFile(false); });
  button(bar, "CSV", "exportAnnotationCsv", [this] { exportFile(true); });
  outer->addLayout(bar);
}

void AnnotationPanel::buildReviewBar(QVBoxLayout* outer) {
  auto* review = new QHBoxLayout;
  button(review, "定位当前标记", "locateAnnotation", [this] {
    if (auto* file = session_.activeFile()) {
      session_.focusMark(file->activeMarkId);
      emit changed();
    }
  });
  button(review, "删除所选标记", "deleteSelectedAnnotations", [this] {
    deleteSelected();
  })->setProperty("preserveParameterDrafts", true);
  auto* quality = button(review, "质量标记", "annotationQualityMenu", [] {});
  quality->setProperty("preserveParameterDrafts", true);
  quality->setToolTip("可选；仅用于筛选、导出和复查，不限制编辑或创建通道");
  auto* qualityMenu = new QMenu(quality);
  qualityMenu->setObjectName("annotationQualityActions");
  quality->setMenu(qualityMenu);
  const auto addQuality = [&](const QString& text, const char* name,
                              ReviewStatus status) {
    auto* choice = qualityMenu->addAction(text, this, [this, status] {
      if (auto* file = session_.activeFile())
        action(session_.annotations().reviewBatch(*file, file->selectedMarkIds,
                                                  status));
    });
    choice->setObjectName(QString::fromLatin1(name));
    choice->setProperty("preserveParameterDrafts", true);
  };
  addQuality("已采纳", "acceptAnnotations", ReviewStatus::Accepted);
  addQuality("已忽略", "rejectAnnotations", ReviewStatus::Rejected);
  addQuality("待判断", "clearAnnotationReview", ReviewStatus::Pending);
  button(review, "撤销标记", "undoAnnotations", [this] {
    if (auto* f = session_.activeFile())
      action(session_.annotations().undo(*f));
  });
  button(review, "重做标记", "redoAnnotations", [this] {
    if (auto* f = session_.activeFile())
      action(session_.annotations().redo(*f));
  });
  button(review, "清选", "clearAnnotationSelection", [this] {
    session_.selectMarks({});
    emit changed();
  });
  review->addStretch();
  button(review, "上一页", "annotationsPreviousPage", [this] {
    if (page_) --page_;
    refresh();
  });
  button(review, "下一页", "annotationsNextPage", [this] {
    if ((page_ + 1) * 200 < model_->total) ++page_;
    refresh();
  });
  outer->addLayout(review);
}

void AnnotationPanel::buildTable(QVBoxLayout* outer) {
  auto* view = new AnnotationTableView;
  table_ = view;
  view->choose = [this](const QModelIndex& index,
                        Qt::KeyboardModifiers modifiers,
                        bool toggle) { chooseRow(index, modifiers, toggle); };
  view->locate = [this](const QModelIndex& index) {
    session_.focusMark(
        model_->data(index, Qt::UserRole).toString().toStdString());
    revealActive();
    emit changed();
  };
  view->editCell = [this](const QModelIndex& index) { editCell(index); };
  table_->setObjectName("annotationTable");
  table_->installEventFilter(this);
  table_->viewport()->installEventFilter(this);
  model_ = new AnnotationTableModel(session_, table_);
  table_->setModel(model_);
  table_->setItemDelegate(new AnnotationSelectionDelegate(table_));
  table_->setMouseTracking(true);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(table_, &QTableView::customContextMenuRequested, this,
          [this](QPoint point) {
            const auto index = table_->indexAt(point);
            if (!index.isValid()) return;
            auto* file = session_.activeFile();
            const auto id = index.data(Qt::UserRole).toString().toStdString();
            if (!file) return;
            if (std::find(file->selectedMarkIds.begin(),
                          file->selectedMarkIds.end(),
                          id) == file->selectedMarkIds.end())
              chooseRow(index, Qt::NoModifier, false);
            QMenu menu(this);
            menu.addAction("定位当前标记", this, [this] {
              if (auto* file = session_.activeFile()) {
                session_.focusMark(file->activeMarkId);
                emit changed();
              }
            });
            menu.addAction("编辑当前范围…", this, &AnnotationPanel::editRange);
            auto* channel =
                menu.addAction("创建窄带通道…", this,
                               &AnnotationPanel::createChannelRequested);
            channel->setObjectName("annotationTableCreateChannelAction");
            auto* remove = menu.addAction("删除所选标记…", this,
                                          [this] { deleteSelected(); });
            remove->setObjectName("annotationTableDeleteAction");
            menu.exec(table_->viewport()->mapToGlobal(point));
          });
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->verticalHeader()->hide();
  table_->verticalHeader()->setDefaultSectionSize(38);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setColumnWidth(0, 30);
  table_->setColumnWidth(1, 205);
  table_->setColumnWidth(2, 85);
  table_->setColumnWidth(3, 110);
  for (int column : {4, 5, 6, 7}) table_->setColumnWidth(column, 110);
  table_->setColumnWidth(8, 65);
  table_->setColumnWidth(9, 85);
  selectAll_ = new QCheckBox(table_->horizontalHeader()->viewport());
  selectAll_->setObjectName("selectAllVisibleAnnotations");
  selectAll_->setToolTip("选择全部符合筛选条件的可见标记");
  selectAll_->setTristate(true);
  selectAll_->move(7, 6);
  model_->selectionChanged = [this] {
    revealActive();
    emit changed();
  };
  connect(selectAll_, &QCheckBox::clicked, this, [this](bool selected) {
    auto* file = session_.activeFile();
    if (!file) return;
    const auto ids =
        selected ? model_->filteredIds : std::vector<std::string>{};
    const auto active =
        std::find(ids.begin(), ids.end(), file->activeMarkId) != ids.end()
            ? file->activeMarkId
        : ids.empty() ? std::string{}
                      : ids.front();
    session_.selectMarks(ids, active);
    emit changed();
  });
  table_->setSortingEnabled(false);
  table_->setAlternatingRowColors(true);
  outer->addWidget(table_, 1);
  connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this,
          [this](int c) {
            if (c == 0) return;
            model_->direction =
                model_->order == c && model_->direction == Qt::AscendingOrder
                    ? Qt::DescendingOrder
                    : Qt::AscendingOrder;
            model_->order = c;
            revealActive();
            emit changed();
          });
  connect(source_, &QComboBox::currentIndexChanged, this,
          [this] { filterChanged(); });
  connect(status_, &QComboBox::currentIndexChanged, this,
          [this] { filterChanged(); });
}

void AnnotationPanel::buildInspector() {
  inspector_ = new QWidget;
  inspector_->setObjectName("annotationInspector");
  auto* details = new QVBoxLayout(inspector_);
  details->setContentsMargins(10, 4, 10, 8);
  detail_ = new QLabel;
  detail_->setTextFormat(Qt::PlainText);
  detail_->setWordWrap(true);
  detail_->setObjectName("annotationIdentity");
  details->addWidget(detail_);
  segments_ = new QComboBox;
  segments_->setObjectName("annotationSegment");
  details->addWidget(segments_);
  connect(segments_, &QComboBox::currentIndexChanged, this, [this](int i) {
    if (refreshing_ || i < 0) return;
    if (auto* f = session_.activeFile())
      action(session_.annotations().select(
          *f, f->selectedMarkIds, f->activeMarkId,
          segments_->itemData(i).toString().toStdString()));
  });
  auto* segmentNavigation = new QHBoxLayout;
  button(segmentNavigation, "上一段", "annotationPreviousSegment",
         [this] { selectAdjacentSegment(-1); });
  button(segmentNavigation, "下一段", "annotationNextSegment",
         [this] { selectAdjacentSegment(1); });
  details->addLayout(segmentNavigation);
  auto* form = new QGridLayout;
  begin_ = new AdaptiveValueEdit(UnitKind::Scalar, 0);
  end_ = new AdaptiveValueEdit(UnitKind::Scalar, 1);
  low_ = new AdaptiveValueEdit(UnitKind::Scalar, 0);
  high_ = new AdaptiveValueEdit(UnitKind::Scalar, 1);
  low_->setAllowNegative(true);
  high_->setAllowNegative(true);
  begin_->setObjectName("annotationStartMilliseconds");
  end_->setObjectName("annotationEndMilliseconds");
  low_->setObjectName("annotationLowerMHz");
  high_->setObjectName("annotationUpperMHz");
  form->addWidget(new QLabel("起始 / ms"), 0, 0);
  form->addWidget(new QLabel("结束 / ms"), 0, 1);
  form->addWidget(begin_, 1, 0);
  form->addWidget(end_, 1, 1);
  form->addWidget(new QLabel("下边界 / MHz"), 2, 0);
  form->addWidget(new QLabel("上边界 / MHz"), 2, 1);
  form->addWidget(low_, 3, 0);
  form->addWidget(high_, 3, 1);
  details->addLayout(form);
  auto* draftActions = new QHBoxLayout;
  button(draftActions, "完整范围编辑…", "editAnnotationRange", [this] {
    editRange();
  })->setProperty("preserveParameterDrafts", true);
  button(draftActions, "撤销草稿 · Esc", "cancelAnnotationGeometry", [this] {
    discardDrafts();
    if (auto* file = session_.activeFile())
      session_.annotations().endRevision(*file);
    emit changed();
  })->setProperty("preserveParameterDrafts", true);
  details->addLayout(draftActions);
  const std::array fields{begin_, end_, low_, high_};
  for (int field = 0; field < 4; ++field) {
    connect(fields[field], &QLineEdit::textEdited, this, [this, field] {
      if (!inlineContexts_[field])
        if (const auto* file = session_.activeFile())
          inlineContexts_[field] = annotationEditContext(
              *file, file->activeMarkId, session_.projectGeneration());
    });
    connect(fields[field], &AdaptiveValueEdit::valueCommitted, this,
            [this, field] { geometry(field); });
  }
  auto* edit = new QHBoxLayout;
  button(edit, "恢复原始", "restoreAnnotationOrigin", [this] {
    if (auto* f = session_.activeFile())
      action(session_.annotations().restoreOrigin(*f, f->activeMarkId));
  });
  details->addLayout(edit);
  auto* add = new QHBoxLayout;
  button(add, "将当前视区添加为段", "addAnnotationSegment", [this] {
    if (auto* file = session_.activeFile())
      action(session_.annotations().addSegment(*file, file->activeMarkId,
                                               file->view));
  });
  details->addLayout(add);
  auto* parts = new QHBoxLayout;
  button(parts, "拆分段", "splitAnnotationSegment", [this] {
    if (auto* f = session_.activeFile())
      if (auto* a = findMark(*f, f->activeMarkId))
        if (auto* seg = activeSegment(*a))
          action(session_.annotations().splitSegment(
              *f, a->id, seg->id,
              seg->range.time.begin +
                  (seg->range.time.end - seg->range.time.begin) / 2));
  });
  button(parts, "合并相邻", "mergeAnnotationSegments", [this] {
    if (auto* f = session_.activeFile())
      if (auto* a = findMark(*f, f->activeMarkId);
          a && a->segments.size() > 1) {
        auto first = std::find_if(
            a->segments.begin(), a->segments.end(),
            [&](const auto& x) { return x.id == a->activeSegmentId; });
        auto second = first + 1;
        if (second == a->segments.end()) {
          second = first;
          --first;
        }
        action(session_.annotations().mergeSegments(*f, a->id,
                                                    {first->id, second->id}));
      }
  });
  button(parts, "删除当前段", "deleteAnnotationSegment", [this] {
    deleteSelected(true);
  })->setProperty("preserveParameterDrafts", true);
  details->addLayout(parts);
  auto* unknown = new QHBoxLayout;
  button(unknown, "采用观察范围（人工）", "adoptAnnotationObservation", [this] {
    if (auto* f = session_.activeFile())
      action(session_.annotations().adoptObservation(*f, f->activeMarkId));
  });
  details->addLayout(unknown);
  button(unknown, "编辑备注", "editAnnotationNote", [this] {
    auto* file = session_.activeFile();
    auto* annotation = file ? findMark(*file, file->activeMarkId) : nullptr;
    if (!annotation || !session_.annotations().canEdit(*file, annotation->id))
      return;
    const auto context = annotationEditContext(*file, annotation->id,
                                               session_.projectGeneration());
    const auto name = annotation->name;
    bool accepted = false;
    const auto text = QInputDialog::getMultiLineText(
        this, "标记备注", "备注随修订记录保存", q(annotation->note), &accepted);
    file = session_.activeFile();
    if (accepted && file &&
        annotationEditContextMatches(*file, context,
                                     session_.projectGeneration()))
      action(session_.annotations().rename(*file, context.annotationId, name,
                                           text.toStdString()));
  });
  error_ = new QLabel;
  error_->setTextFormat(Qt::PlainText);
  error_->setWordWrap(true);
  error_->setStyleSheet("color:#f3bd76");
  details->addWidget(error_);
  evidencePanel_ = new AnnotationEvidenceWidget;
  details->addWidget(evidencePanel_);
}

void AnnotationPanel::action(const AnnotationResult& r) {
  if (!r.ok) {
    error_->setText(q(r.error));
    emit message(q(r.error));
    return;
  }
  error_->clear();
  emit changed();
}
void AnnotationPanel::revealActive() { revealActive_ = true; }
void AnnotationPanel::refreshHover() {
  const auto* file = session_.activeFile();
  table_->setProperty("hoveredAnnotationId",
                      file ? q(file->hoveredMarkId) : QString{});
  if (model_->rowCount())
    emit model_->dataChanged(model_->index(0, 0),
                             model_->index(model_->rowCount() - 1, 10),
                             {Qt::BackgroundRole, Qt::UserRole + 1});
  table_->viewport()->update();
}
void AnnotationPanel::chooseRow(const QModelIndex& index,
                                Qt::KeyboardModifiers modifiers, bool toggle) {
  if (refreshing_ || !index.isValid()) return;
  auto* file = session_.activeFile();
  if (!file) return;
  const auto result = session_.annotations().chooseSelection(
      *file, model_->data(index, Qt::UserRole).toString().toStdString(),
      selectionMode(toggle ? modifiers | Qt::ControlModifier : modifiers),
      model_->filteredIds);
  revealActive();
  action(result);
}
bool AnnotationPanel::eventFilter(QObject* watched, QEvent* event) {
  if (cellDraft_ && event->type() == QEvent::MouseButtonPress) {
    auto* mouse = static_cast<QMouseEvent*>(event);
    auto* target = qobject_cast<QWidget*>(watched);
    if (target && target->window() == window() && target != cellEditor_ &&
        !preservesParameterDrafts(target, mouse))
      finishCell(true);
  }
  if (watched == table_->viewport()) {
    if (event->type() == QEvent::MouseMove) {
      const auto index = table_->indexAt(
          static_cast<QMouseEvent*>(event)->position().toPoint());
      const auto id = model_->data(index, Qt::UserRole).toString();
      const auto* file = session_.activeFile();
      const auto* annotation =
          file ? findAnnotation(*file, id.toStdString()) : nullptr;
      emit hoverChanged(
          id, annotation ? q(annotation->activeSegmentId) : QString{});
    } else if (event->type() == QEvent::Leave) {
      emit hoverChanged({}, {});
    }
  }
  if (watched == table_ && event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    auto* file = session_.activeFile();
    if (!file) return QWidget::eventFilter(watched, event);
    if (key->key() == Qt::Key_F2) {
      auto index = table_->currentIndex();
      if (!index.isValid() || !(index.flags() & Qt::ItemIsEditable)) {
        for (int row = 0; row < model_->rowCount(); ++row)
          if (model_->index(row, 1)
                  .data(Qt::UserRole)
                  .toString()
                  .toStdString() == file->activeMarkId) {
            index = model_->index(row, 1);
            break;
          }
      }
      editCell(index);
      return true;
    }
    if (key->key() == Qt::Key_Delete) {
      deleteSelected();
      return true;
    }
    if (key->matches(QKeySequence::SelectAll)) {
      session_.selectMarks(
          model_->filteredIds,
          std::find(model_->filteredIds.begin(), model_->filteredIds.end(),
                    file->activeMarkId) != model_->filteredIds.end()
              ? file->activeMarkId
          : model_->filteredIds.empty() ? std::string{}
                                        : model_->filteredIds.front());
      emit changed();
      return true;
    }
    const auto current =
        std::find(model_->filteredIds.begin(), model_->filteredIds.end(),
                  file->activeMarkId);
    auto row = current == model_->filteredIds.end()
                   ? std::ptrdiff_t{0}
                   : current - model_->filteredIds.begin();
    bool navigate = true;
    switch (key->key()) {
      case Qt::Key_Up:
        --row;
        break;
      case Qt::Key_Down:
        ++row;
        break;
      case Qt::Key_PageUp:
        row -= 200;
        break;
      case Qt::Key_PageDown:
        row += 200;
        break;
      case Qt::Key_Home:
        row = 0;
        break;
      case Qt::Key_End:
        row = static_cast<std::ptrdiff_t>(model_->filteredIds.size()) - 1;
        break;
      case Qt::Key_Space:
        break;
      default:
        navigate = false;
        break;
    }
    if (navigate && !model_->filteredIds.empty()) {
      row = std::clamp(
          row, std::ptrdiff_t{0},
          static_cast<std::ptrdiff_t>(model_->filteredIds.size()) - 1);
      const auto result = session_.annotations().chooseSelection(
          *file, model_->filteredIds[static_cast<std::size_t>(row)],
          key->key() == Qt::Key_Space ? AnnotationSelectionMode::Toggle
                                      : selectionMode(key->modifiers()),
          model_->filteredIds);
      revealActive();
      action(result);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}
void AnnotationPanel::discardDrafts() {
  finishCell(false);
  begin_->discard();
  end_->discard();
  low_->discard();
  high_->discard();
  inlineContexts_ = {};
}
void AnnotationPanel::queueRefresh() {
  if (refreshQueued_) return;
  refreshQueued_ = true;
  QTimer::singleShot(0, this, [this] {
    refreshQueued_ = false;
    emit changed();
  });
}

void AnnotationPanel::editCell(const QModelIndex& index) {
  if (!index.isValid() || !(index.flags() & Qt::ItemIsEditable)) return;
  const auto id = index.data(Qt::UserRole).toString().toStdString();
  const int column = index.column();
  if (cellDraft_ && !finishCell(true)) return;
  auto* file = session_.activeFile();
  const auto* annotation = file ? findAnnotation(*file, id) : nullptr;
  if (!annotation || !session_.annotations().canEdit(*file, id)) return;
  if (file->activeMarkId != id) {
    session_.annotations().select(*file, {id}, id);
    queueRefresh();
  }
  const auto* segment = activeSegment(*annotation);
  const auto range =
      segment ? segment->range : annotation->observation.value_or(ViewRange{});
  QString text;
  if (column == 1)
    text = q(annotation->name);
  else if (column == 4 || column == 5)
    text = formatAdaptiveValue(
        static_cast<double>(column == 4 ? range.time.begin : range.time.end) /
            file->metadata.sampleRateHz * 1000,
        UnitKind::Scalar);
  else if (column == 6)
    text = formatAdaptiveValue(
        (range.frequency.lowerHz + range.frequency.upperHz) / 2e6,
        UnitKind::Scalar);
  else if (column == 7)
    text = formatAdaptiveValue(
        (range.frequency.upperHz - range.frequency.lowerHz) / 1000,
        UnitKind::Scalar);
  else
    return;
  cellDraft_ =
      CellDraft{annotationEditContext(*file, id, session_.projectGeneration()),
                column, range, text};
  auto* editor = new AnnotationCellEditor(table_->viewport());
  cellEditor_ = editor;
  editor->setObjectName("annotationTableEditor");
  editor->setProperty("annotationId", q(id));
  editor->setProperty("baseRevisionId", q(annotation->revisionId));
  editor->setProperty("segmentId", q(annotation->activeSegmentId));
  editor->setText(text);
  editor->setGeometry(table_->visualRect(index).adjusted(1, 1, -1, -1));
  editor->finish = [this](bool commit, bool focus) {
    finishCell(commit, focus);
  };
  connect(editor, &QLineEdit::textEdited, editor,
          [editor] { editor->setProperty("parameterDraft", true); });
  editor->show();
  editor->setFocus(Qt::OtherFocusReason);
  editor->selectAll();
}

bool AnnotationPanel::finishCell(bool commit, bool focusTable) {
  if (!cellDraft_ || !cellEditor_) return true;
  const auto pending = *cellDraft_;
  const auto text = cellEditor_->text();
  auto* file = session_.activeFile();
  AnnotationResult result{true};
  const bool current =
      file && annotationEditContextMatches(*file, pending.context,
                                           session_.projectGeneration());
  if (commit && !current)
    result = {false, false, {}, "编辑目标或来源修订已改变，草稿已取消"};
  if (commit && current && text != pending.initialText) {
    const auto* annotation =
        findAnnotation(*file, pending.context.annotationId);
    if (pending.column == 1) {
      const auto name = text.trimmed();
      if (name.isEmpty() || name.size() > 80)
        result = {false, false, {}, "名称须为 1–80 个字符"};
      else
        result = session_.annotations().rename(
            *file, annotation->id, name.toStdString(), annotation->note);
    } else {
      const auto value =
          parseAdaptiveValue(text, UnitKind::Scalar, pending.column == 6);
      auto range = pending.range;
      if (!value) {
        result = {false, false, {}, "请输入有限的数值"};
      } else if (pending.column == 4 || pending.column == 5) {
        const long double samples =
            std::round(static_cast<long double>(*value) / 1000 *
                       file->metadata.sampleRateHz);
        if (samples < 0 || samples > std::numeric_limits<SampleIndex>::max())
          result = {false, false, {}, "时间超出可表示样本范围"};
        else if (pending.column == 4)
          range.time.begin = static_cast<SampleIndex>(samples);
        else
          range.time.end = static_cast<SampleIndex>(samples);
      } else if (pending.column == 6) {
        const auto oldCenter =
            (range.frequency.lowerHz + range.frequency.upperHz) / 2e6;
        if (*value != oldCenter) {
          const auto offset = *value * 1e6 - oldCenter * 1e6;
          range.frequency.lowerHz += offset;
          range.frequency.upperHz += offset;
        }
      } else if (pending.column == 7) {
        const auto oldBandwidth =
            (range.frequency.upperHz - range.frequency.lowerHz) / 1000;
        if (*value != oldBandwidth) {
          const auto center =
              (range.frequency.lowerHz + range.frequency.upperHz) / 2;
          range.frequency = {center - *value * 500, center + *value * 500};
        }
      }
      if (result.ok)
        result = pending.context.segmentId.empty()
                     ? session_.annotations().commitObservationFrequency(
                           *file, pending.context.annotationId, range.frequency)
                     : session_.annotations().commitGeometry(
                           *file, pending.context.annotationId,
                           pending.context.segmentId, range);
    }
  }
  if (!result.ok && current) {
    error_->setText(q(result.error));
    cellEditor_->setProperty("invalid", true);
    emit message(q(result.error));
    return false;
  }
  auto* editor = cellEditor_.data();
  cellEditor_.clear();
  cellDraft_.reset();
  editor->setProperty("parameterDraft", false);
  editor->setObjectName({});
  editor->hide();
  editor->deleteLater();
  if (focusTable) table_->setFocus(Qt::OtherFocusReason);
  if (!result.ok) {
    error_->setText(q(result.error));
    emit message(q(result.error));
  } else {
    error_->clear();
  }
  // Wait until the original mouse target has received its click. A reset here
  // can otherwise destroy a tree/button target before its release event.
  if (commit || deferredRefresh_) queueRefresh();
  deferredRefresh_ = false;
  return result.ok;
}

void AnnotationPanel::editRange() {
  auto* file = session_.activeFile();
  const auto* annotation =
      file ? findAnnotation(*file, file->activeMarkId) : nullptr;
  if (!annotation || !session_.annotations().canEdit(*file, annotation->id))
    return;
  const auto context = annotationEditContext(*file, annotation->id,
                                             session_.projectGeneration());
  const auto* segment = activeSegment(*annotation);
  if (!segment && !annotation->observation) return;
  const auto base = segment ? segment->range : *annotation->observation;
  const double sampleRate = file->metadata.sampleRateHz;
  const std::array original{
      static_cast<double>(base.time.begin) / sampleRate * 1000,
      static_cast<double>(base.time.end) / sampleRate * 1000,
      base.frequency.lowerHz / 1e6, base.frequency.upperHz / 1e6};
  QDialog dialog(this);
  dialog.setObjectName("annotationRangeDialog");
  dialog.setProperty("preserveParameterDrafts", true);
  dialog.setWindowTitle(segment ? "完整范围编辑 · 当前活动段"
                                : "频率范围编辑 · 时间未定位");
  dialog.setMinimumWidth(450);
  auto* layout = new QVBoxLayout(&dialog);
  auto* notice = new QLabel(
      segment ? "只修改当前活动段；全部字段一次校验、一次提交，可一次撤销。"
              : "仅修改频率；观察支持和未知活动时间保留，不生成活动段。");
  notice->setWordWrap(true);
  layout->addWidget(notice);
  auto* form = new QFormLayout;
  const std::array names{"annotationRangeStartMilliseconds",
                         "annotationRangeEndMilliseconds",
                         "annotationRangeLowerMHz", "annotationRangeUpperMHz"};
  const std::array labels{"起始 / ms", "结束 / ms", "下边界 / MHz",
                          "上边界 / MHz"};
  const std::array inlineFields{begin_, end_, low_, high_};
  std::array<QLineEdit*, 4> fields{};
  for (int i = 0; i < 4; ++i) {
    fields[i] =
        new QLineEdit(formatAdaptiveValue(original[i], UnitKind::Scalar));
    fields[i]->setObjectName(names[i]);
    fields[i]->setEnabled(segment || i >= 2);
    if (inlineFields[i]->property("parameterDraft").toBool() &&
        inlineContexts_[i] && *inlineContexts_[i] == context)
      fields[i]->setText(inlineFields[i]->text());
    form->addRow(labels[i], fields[i]);
  }
  layout->addLayout(form);
  auto* error = new QLabel;
  error->setObjectName("annotationRangeError");
  error->setWordWrap(true);
  error->setStyleSheet("color:#f3bd76");
  layout->addWidget(error);
  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
  auto* apply = buttons->button(QDialogButtonBox::Apply);
  apply->setObjectName("applyAnnotationRange");
  apply->setText("应用范围");
  buttons->button(QDialogButtonBox::Cancel)
      ->setObjectName("cancelAnnotationRange");
  buttons->button(QDialogButtonBox::Cancel)->setText("取消");
  layout->addWidget(buttons);
  AnnotationResult result{true};
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(apply, &QPushButton::clicked, &dialog, [&] {
    auto* current = session_.activeFile();
    if (!current || !annotationEditContextMatches(
                        *current, context, session_.projectGeneration())) {
      error->setText("工程、文件、当前段或来源修订已改变，请取消后重新编辑。");
      return;
    }
    auto next = base;
    for (int i = 0; i < 4; ++i) {
      if (!fields[i]->isEnabled()) continue;
      const auto value =
          parseAdaptiveValue(fields[i]->text(), UnitKind::Scalar, i >= 2);
      if (!value) {
        error->setText("全部字段必须是有限数值；时间不能为负。");
        return;
      }
      // Preserve original samples/Hz for untouched or numerically unchanged
      // fields. Formatting is never a reason to rebuild the other boundaries.
      if (*value == original[i]) continue;
      if (i < 2) {
        const auto samples =
            std::round(static_cast<long double>(*value) / 1000 * sampleRate);
        if (samples < 0 || samples > std::numeric_limits<SampleIndex>::max()) {
          error->setText("时间超出可表示样本范围。");
          return;
        }
        if (i == 0)
          next.time.begin = static_cast<SampleIndex>(samples);
        else
          next.time.end = static_cast<SampleIndex>(samples);
      } else if (i == 2) {
        next.frequency.lowerHz = *value * 1e6;
      } else {
        next.frequency.upperHz = *value * 1e6;
      }
    }
    result = context.segmentId.empty()
                 ? session_.annotations().commitObservationFrequency(
                       *current, context.annotationId, next.frequency)
                 : session_.annotations().commitGeometry(
                       *current, context.annotationId, context.segmentId, next);
    if (!result.ok) {
      error->setText(q(result.error));
      return;
    }
    dialog.accept();
  });
  if (dialog.exec() == QDialog::Accepted) {
    discardDrafts();
    action(result);
  }
}

void AnnotationPanel::deleteSelected(bool currentSegmentOnly) {
  auto* file = session_.activeFile();
  if (!file) return;
  session_.annotations().reconcileSelection(*file);
  const auto generation = session_.projectGeneration();
  const auto fileId = file->metadata.id;
  const auto sampleChannel = file->metadata.sampleFormat.selectedChannel;
  const auto* active = findAnnotation(*file, file->activeMarkId);
  const auto* segment = active ? activeSegment(*active) : nullptr;
  if (currentSegmentOnly && !segment) return;
  const bool lastSegment = currentSegmentOnly && active->segments.size() == 1;
  const auto segmentId = segment ? segment->id : std::string{};
  std::vector<std::pair<std::string, std::string>> targets;
  std::size_t segmentCount = 0, channelCount = 0;
  for (const auto& id : currentSegmentOnly
                            ? std::vector<std::string>{active->id}
                            : file->selectedMarkIds) {
    const auto* annotation = findAnnotation(*file, id);
    if (!annotation || !annotationVisible(*file, *annotation)) continue;
    targets.emplace_back(id, annotation->revisionId);
    segmentCount +=
        currentSegmentOnly && !lastSegment ? 1 : annotation->segments.size();
  }
  if (targets.empty()) return;
  for (const auto& channel : file->channels)
    for (const auto& [id, revision] : targets) {
      Q_UNUSED(revision);
      if ((channel.annotationId == id || channel.sourceMarkId == id) &&
          (!currentSegmentOnly || lastSegment ||
           channel.segmentId == segmentId)) {
        ++channelCount;
        break;
      }
    }
  QDialog dialog(this);
  dialog.setObjectName("annotationDeleteDialog");
  dialog.setProperty("preserveParameterDrafts", true);
  dialog.setWindowTitle(lastSegment ? "删除最后一段将删除标记对象"
                        : currentSegmentOnly ? "删除当前活动段"
                                             : "删除所选标记");
  dialog.setMinimumWidth(440);
  auto* layout = new QVBoxLayout(&dialog);
  const auto objects = currentSegmentOnly && !lastSegment ? 0 : targets.size();
  auto* summary =
      new QLabel(QString("%1 个标记对象 · %2 个活动段 · %3 个关联通道")
                     .arg(objects)
                     .arg(segmentCount)
                     .arg(channelCount));
  summary->setObjectName("annotationDeleteSummary");
  summary->setProperty("objectCount", static_cast<qulonglong>(objects));
  summary->setProperty("segmentCount", static_cast<qulonglong>(segmentCount));
  summary->setProperty("channelCount", static_cast<qulonglong>(channelCount));
  layout->addWidget(summary);
  auto* explanation = new QLabel(
      lastSegment
          ? "最后一段按删除对象处理。关联通道保留创建时快照并标示来源已删除。"
          : "仅处理当前筛选内的所选标记。关联通道保留创建时快照并标示来源已删除"
            "。");
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  layout->addWidget(new QLabel("确认后一次撤销可恢复。取消不会提交范围草稿。"));
  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)
      ->setObjectName("deleteAnnotationsConfirm");
  buttons->button(QDialogButtonBox::Ok)
      ->setText(currentSegmentOnly && !lastSegment ? "删除当前段" : "删除标记");
  buttons->button(QDialogButtonBox::Cancel)
      ->setObjectName("deleteAnnotationsCancel");
  buttons->button(QDialogButtonBox::Cancel)->setText("取消");
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  file = session_.activeFile();
  if (!file || generation != session_.projectGeneration() ||
      file->metadata.id != fileId ||
      file->metadata.sampleFormat.selectedChannel != sampleChannel) {
    emit message("删除目标上下文已改变，请重新选择。");
    return;
  }
  std::vector<std::string> ids;
  for (const auto& [id, revision] : targets) {
    const auto* annotation = findAnnotation(*file, id);
    if (!annotation || !annotationVisible(*file, *annotation) ||
        annotation->revisionId != revision) {
      emit message("删除目标或修订已改变，请重新预览。");
      return;
    }
    ids.push_back(id);
  }
  discardDrafts();
  action(
      currentSegmentOnly && !lastSegment
          ? session_.annotations().deleteSegment(*file, ids.front(), segmentId)
          : session_.annotations().deleteBatch(*file, ids));
}

void AnnotationPanel::geometry(int field) {
  auto* f = session_.activeFile();
  if (!f) return;
  const auto context = inlineContexts_[field];
  inlineContexts_[field].reset();
  if (!context || !annotationEditContextMatches(*f, *context,
                                                session_.projectGeneration())) {
    error_->setText("编辑目标或来源修订已改变，草稿已取消");
    queueRefresh();
    return;
  }
  auto* a = findMark(*f, context->annotationId);
  if (!a) return;
  const auto* seg = activeSegment(*a);
  if (!seg && !a->observation) return;
  auto range = seg ? seg->range : *a->observation;
  const std::array fields{begin_, end_, low_, high_};
  const std::array displayed{
      static_cast<double>(range.time.begin) / f->metadata.sampleRateHz * 1000,
      static_cast<double>(range.time.end) / f->metadata.sampleRateHz * 1000,
      range.frequency.lowerHz / 1e6, range.frequency.upperHz / 1e6};
  if (fields[field]->value() == displayed[field]) {
    queueRefresh();
    return;
  }
  if (field == 0 || field == 1) {
    const auto seconds = (field == 0 ? begin_->value() : end_->value()) / 1000;
    const long double samples = std::round(static_cast<long double>(seconds) *
                                           f->metadata.sampleRateHz);
    if (samples < 0 || samples > std::numeric_limits<SampleIndex>::max()) {
      error_->setText("时间超出可表示样本范围");
      queueRefresh();
      return;
    }
    if (field == 0)
      range.time.begin = static_cast<SampleIndex>(samples);
    else
      range.time.end = static_cast<SampleIndex>(samples);
  }
  if (field == 2) range.frequency.lowerHz = low_->value() * 1e6;
  if (field == 3) range.frequency.upperHz = high_->value() * 1e6;
  const auto r = session_.commitAnnotationGeometry(a->id, range);
  if (!r.ok) {
    error_->setText(q(r.error));
    emit message(q(r.error));
  } else {
    error_->clear();
  }
  queueRefresh();
}
void AnnotationPanel::refresh() {
  refreshing_ = true;
  auto* f = session_.activeFile();
  if (f) session_.annotations().reconcileSelection(*f);
  if (cellDraft_) {
    if (f && annotationEditContextMatches(*f, cellDraft_->context,
                                          session_.projectGeneration())) {
      deferredRefresh_ = true;
      refreshing_ = false;
      return;
    }
    finishCell(false);
  }
  const auto context =
      std::to_string(session_.projectGeneration()) + ":" +
      (f ? f->metadata.id + ":" +
               std::to_string(f->metadata.sampleFormat.selectedChannel)
         : "");
  if (filterContext_ != context) {
    const QSignalBlocker sourceBlock(source_), statusBlock(status_),
        scoreBlock(minimumScore_);
    source_->setCurrentIndex(f ? f->annotationFilter.source + 1 : 0);
    status_->setCurrentIndex(f ? f->annotationFilter.review + 1 : 0);
    minimumScore_->setValue(f ? std::max(0.0, f->annotationFilter.minimumMean)
                              : 0);
    filterContext_ = context;
    page_ = 0;
  }
  const auto runId = f ? q(f->annotationFilter.runId) : QString{};
  {
    const QSignalBlocker block(run_);
    QStringList ids{QString{}, "__no_run__"};
    if (f)
      for (const auto& sourceSet : f->sourceSets)
        if (!sourceSet.runId.empty() && !ids.contains(q(sourceSet.runId)))
          ids.push_back(q(sourceSet.runId));
    bool samePrefix = run_->count() <= ids.size();
    for (int index = 0; samePrefix && index < run_->count(); ++index)
      samePrefix = run_->itemData(index).toString() == ids[index];
    // Ordinary refresh and appended publications keep the same popup model
    // and persistent rows, including the user's current hover/keyboard row.
    if (!samePrefix) run_->clear();
    for (int index = run_->count(); index < ids.size(); ++index) {
      const auto text = index == 0 ? QString("全部运行")
                        : index == 1 ? QString("非算法来源") : ids[index];
      run_->addItem(text, ids[index]);
      run_->setItemData(index, text, Qt::ToolTipRole);
    }
    const int selected = std::max(0, run_->findData(runId));
    if (run_->currentIndex() != selected) run_->setCurrentIndex(selected);
    run_->view()->ensurePolished();
    // Include the actual delegate's check/icon/style margins, not just the
    // text advance. The collapsed editor can retain its compact width.
    int popupWidth = run_->width();
    QStyleOptionViewItem option;
    option.initFrom(run_->view());
    option.font = run_->font();
    option.fontMetrics = QFontMetrics(option.font);
    for (int index = 0; index < run_->count(); ++index) {
      const auto item = run_->model()->index(index, run_->modelColumn(), run_->rootModelIndex());
      popupWidth = std::max(popupWidth,
          run_->view()->itemDelegate()->sizeHint(option, item).width() + 24);
      popupWidth = std::max(popupWidth,
          run_->fontMetrics().horizontalAdvance(run_->itemText(index)) + 40);
    }
    if (auto* screen = run_->screen())
      popupWidth = std::min(popupWidth, std::max(1, screen->availableGeometry().width() - 16));
    if (run_->view()->minimumWidth() != popupWidth) run_->view()->setMinimumWidth(popupWidth);
    run_->setToolTip(runId.isEmpty() ? QString("按检测运行筛选") : run_->currentText());
  }
  model_->update(
      page_, revealActive_ ||
                 (f ? f->activeMarkId : std::string{}) != displayedActiveId_);
  displayedActiveId_ = f ? f->activeMarkId : std::string{};
  revealActive_ = false;
  table_->setProperty("annotationPage", static_cast<qulonglong>(page_ + 1));
  table_->setProperty("matchingAnnotationCount",
                      static_cast<qulonglong>(model_->total));
  refreshTableSelection(f);
  refreshInspector(f);
  refreshing_ = false;
}
void AnnotationPanel::filterChanged() {
  if (refreshing_) return;
  if (auto* file = session_.activeFile()) {
    file->annotationFilter.source = source_->currentIndex() - 1;
    file->annotationFilter.review = status_->currentIndex() - 1;
    file->annotationFilter.runId = run_->currentData().toString().toStdString();
    file->annotationFilter.minimumMean = minimumScore_->value();
    session_.annotations().reconcileSelection(*file);
  }
  page_ = 0;
  emit changed();
}
void AnnotationPanel::refreshTableSelection(const FileState* file) {
  const QSignalBlocker selectionBlock(table_->selectionModel());
  table_->selectionModel()->clearSelection();
  table_->selectionModel()->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
  std::unordered_set<std::string> selected;
  if (file) {
    selected.insert(file->selectedMarkIds.begin(), file->selectedMarkIds.end());
    for (int i = 0; i < 3; ++i)
      layers_[i]->setChecked(file->annotationLayers[i]);
    for (int row = 0; row < model_->rowCount(); ++row) {
      const auto& id = model_->rows[static_cast<std::size_t>(row)]->id;
      if (selected.contains(id))
        table_->selectionModel()->select(
            model_->index(row, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
      if (file->activeMarkId == id) {
        const int column =
            std::clamp(table_->property("annotationCurrentColumn").isValid()
                           ? table_->property("annotationCurrentColumn").toInt()
                           : 1,
                       0, 10);
        table_->selectionModel()->setCurrentIndex(
            model_->index(row, column), QItemSelectionModel::NoUpdate);
        table_->scrollTo(model_->index(row, column),
                         QAbstractItemView::EnsureVisible);
      }
    }
  }
  selectAll_->setEnabled(!model_->filteredIds.empty());
  selectAll_->setCheckState(selected.empty() ? Qt::Unchecked
                            : selected.size() == model_->filteredIds.size()
                                ? Qt::Checked
                                : Qt::PartiallyChecked);
  const auto counts = file ? annotationCounts(*file) : AnnotationCounts{};
  findChild<QPushButton*>("annotationQualityMenu")
      ->setEnabled(counts.selected != 0);
  for (const auto* name :
       {"acceptAnnotations", "rejectAnnotations", "clearAnnotationReview"})
    findChild<QAction*>(name)->setEnabled(counts.selected != 0);
  count_->setText(
      QString("匹配 %1 / 总数 %2 / 已选 %3 · 页 %4/%5")
          .arg(counts.matching)
          .arg(counts.total)
          .arg(counts.selected)
          .arg(page_ + 1)
          .arg(std::max<std::size_t>(1, (model_->total + 199) / 200)));
  findChild<QPushButton*>("annotationsPreviousPage")->setEnabled(page_ != 0);
  findChild<QPushButton*>("annotationsNextPage")
      ->setEnabled((page_ + 1) * 200 < model_->total);
  refreshHover();
}
void AnnotationPanel::refreshInspector(const FileState* f) {
  const auto* a = f ? findMark(*f, f->activeMarkId) : nullptr;
  const auto next =
      std::to_string(session_.projectGeneration()) + ":" +
      (f ? f->metadata.id + ":" +
               std::to_string(f->metadata.sampleFormat.selectedChannel)
         : "") +
      ":" + (a ? a->id + ":" + a->activeSegmentId + ":" + a->revisionId : "");
  if (context_ != next) {
    discardDrafts();
    context_ = next;
  }
  const auto* seg = a ? activeSegment(*a) : nullptr;
  segments_->clear();
  if (a) {
    const auto active = std::find_if(a->segments.begin(), a->segments.end(),
                                     [&](const Segment& segment) {
                                       return segment.id == a->activeSegmentId;
                                     });
    const auto offset =
        active == a->segments.end()
            ? 0
            : static_cast<std::size_t>(active - a->segments.begin()) / 200 *
                  200;
    for (std::size_t i = offset; i < std::min(offset + 200, a->segments.size());
         ++i)
      segments_->addItem(QString("段 %1 · %2–%3 samples")
                             .arg(i + 1)
                             .arg(a->segments[i].range.time.begin)
                             .arg(a->segments[i].range.time.end),
                         q(a->segments[i].id));
    segments_->setCurrentIndex(segments_->findData(q(a->activeSegmentId)));
  }
  const bool editable = a && session_.annotations().canEdit(*f, a->id);
  begin_->setEnabled(editable && seg);
  end_->setEnabled(editable && seg);
  low_->setEnabled(editable && (seg || a->observation));
  high_->setEnabled(editable && (seg || a->observation));
  inspector_->findChild<QPushButton*>("editAnnotationRange")
      ->setEnabled(editable && (seg || a->observation));
  if (seg) {
    if (!begin_->property("parameterDraft").toBool())
      begin_->setValue(seg->range.time.begin / f->metadata.sampleRateHz * 1000);
    if (!end_->property("parameterDraft").toBool())
      end_->setValue(seg->range.time.end / f->metadata.sampleRateHz * 1000);
    if (!low_->property("parameterDraft").toBool())
      low_->setValue(seg->range.frequency.lowerHz / 1e6);
    if (!high_->property("parameterDraft").toBool())
      high_->setValue(seg->range.frequency.upperHz / 1e6);
    begin_->setToolTip(
        QString("包含样本 %1；提交按最近样本量化").arg(seg->range.time.begin));
    end_->setToolTip(
        QString("不包含样本 %1；提交按最近样本量化").arg(seg->range.time.end));
    low_->setToolTip("当前段频率下边界（MHz），坐标语义见来源证据");
    high_->setToolTip("当前段频率上边界（MHz），坐标语义见来源证据");
  } else {
    // Observation support is not an activity-time editor. Its frequency
    // boundaries remain directly editable without manufacturing a segment.
    for (auto* edit : {begin_, end_}) {
      const QSignalBlocker blocker(edit);
      edit->setProperty("parameterDraft", false);
      edit->setProperty("invalid", false);
      edit->setValue(0);
      edit->clear();
      edit->setPlaceholderText("—");
      edit->setToolTip("没有已定位活动段；窗口观测支持请见检测证据");
    }
    if (a && a->observation) {
      if (!low_->property("parameterDraft").toBool())
        low_->setValue(a->observation->frequency.lowerHz / 1e6);
      if (!high_->property("parameterDraft").toBool())
        high_->setValue(a->observation->frequency.upperHz / 1e6);
      low_->setToolTip("只修改频率下边界；活动时间保持未知");
      high_->setToolTip("只修改频率上边界；活动时间保持未知");
    } else {
      for (auto* edit : {low_, high_}) {
        edit->setValue(0);
        edit->clear();
        edit->setPlaceholderText("—");
      }
    }
  }
  detail_->setText(
      a ? q(a->name) + "\n" + sourceName(a->sourceKind) + " · " +
              reviewName(a->reviewStatus) + "\n" + q(a->id) + " / " +
              q(a->revisionId) + "\n" +
              (a->classLabel ? "原始类别 " + q(*a->classLabel) : "类别未知") +
              (seg ? QString("\n当前第 %1/%2 段")
                         .arg(seg - a->segments.data() + 1)
                         .arg(a->segments.size())
                   : QString("\n时间未定位；没有活动段"))
        : "选择标记查看来源 / 段 / 修订");
  refreshEvidence(a);
}
void AnnotationPanel::refreshEvidence(const Annotation* annotation) {
  evidencePanel_->setAnnotation(annotation, session_.activeFile());
}
void AnnotationPanel::selectAdjacentSegment(int offset) {
  auto* file = session_.activeFile();
  auto* annotation = file ? findAnnotation(*file, file->activeMarkId) : nullptr;
  if (!annotation || annotation->segments.empty()) return;
  auto current =
      std::find_if(annotation->segments.begin(), annotation->segments.end(),
                   [&](const Segment& segment) {
                     return segment.id == annotation->activeSegmentId;
                   });
  if (current == annotation->segments.end()) return;
  const auto next = std::clamp<std::ptrdiff_t>(
      current - annotation->segments.begin() + offset, 0,
      static_cast<std::ptrdiff_t>(annotation->segments.size() - 1));
  discardDrafts();
  action(session_.annotations().select(*file, file->selectedMarkIds,
                                       annotation->id,
                                       annotation->segments[next].id));
}
void AnnotationPanel::exportFile(bool csv) {
  auto* f = session_.activeFile();
  if (!f) return;
  const auto path = QFileDialog::getSaveFileName(
      this,
      csv ? "CSV每活动段一行；不保存完整原始字段/修订证据" : "无损标记JSON导出",
      csv ? "annotations.csv" : "annotations.json");
  if (path.isEmpty()) return;
  const auto bytes = csv ? exportAnnotationsCsv(*f) : exportAnnotationsJson(*f);
  QSaveFile out(path);
  if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() ||
      !out.commit())
    emit message("导出失败：" + out.errorString());
  else
    emit message(csv ? "已导出CSV；原始字段/完整修订仅JSON保留"
                     : "已导出标记JSON；这是工程交换格式，不是第三类源标签");
}
void AnnotationPanel::showImportLabelsDialog() {
  const auto* target = session_.activeFile();
  if (!target) { emit message("请先添加或选择目标信号，再导入标签"); return; }
  const auto fileId = target->metadata.id;
  const auto generation = session_.projectGeneration();
  QFileDialog dialog(this, QString("导入标签 — 目标信号：%1").arg(q(target->metadata.name)),
                     QFileInfo(q(target->metadata.path)).absolutePath(),
                     "标签 (*.txt *.sigmf-meta *.sigmf-data *.json)");
  dialog.setObjectName("importLabelFileDialog");
  dialog.setFileMode(QFileDialog::ExistingFile);
  if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
  if (!session_.activeFile() || session_.activeFile()->metadata.id != fileId ||
      session_.projectGeneration() != generation) {
    emit message("目标信号已变化，标签导入已取消"); return;
  }
  importLabels(dialog.selectedFiles().front());
}

bool AnnotationPanel::importLabels(const QString& path, QString* error,
                                   bool preview) {
  auto* f = session_.activeFile();
  if (!f) {
    if (error) *error = "请先导入信号";
    return false;
  }
  const auto metadata = f->metadata;
  const auto generation = session_.projectGeneration();
  LabelImportResult result;
  runBackgroundOperation(window(), "校验标签来源…", [&](auto cancelled) {
    LabelImportOptions options; options.cancelled = cancelled;
    result = importLabelSource(path, metadata, generation, false, options); return result.ok();
  });
  if (result.state == LabelImportState::NeedsMapping) {
    QString boundedPreview, why;
    if (!runBackgroundOperation(window(), "读取标签预览…", [&](auto cancelled) {
          LabelImportOptions options; options.cancelled = cancelled;
          return previewLabelSource(path, boundedPreview, why, options);
        })) { if (error) *error = why; emit message(why); return false; }
    LabelMappingDialog mapping(path, boundedPreview, this);
    if (mapping.exec() != QDialog::Accepted || !mapping.mapping()) return false;
    const auto fields = *mapping.mapping();
    runBackgroundOperation(window(), "校验字段映射与物理范围…", [&](auto cancelled) {
      LabelImportOptions options; options.cancelled = cancelled;
      result = importMappedTextLabels(path, metadata, fields, generation, options); return result.ok();
    });
  }
  f = session_.activeFile();
  if (!f || f->metadata.id != metadata.id || generation != session_.projectGeneration()) { if (error) *error = "标签导入上下文已变化"; return false; }
  if (result.state == LabelImportState::Incomplete && result.incompleteRecords) {
    if (QMessageBox::question(this, "仅保存标签来源证据", result.error + "\n只保留原文、类别、映射和诊断，生成 0 个定位标记。继续？", QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes) return false;
    f = session_.activeFile();
    if (!f || f->metadata.id != metadata.id || generation != session_.projectGeneration()) {
      if (error) *error = "标签导入上下文已变化"; return false;
    }
    const auto archived = session_.annotations().publishSourceSet(*f, result.sourceSet, {});
    if (error) *error = q(archived.error);
    emit message(archived.ok ? "已保存原始标签证据；0 个可定位标记" : q(archived.error));
    if (archived.changed) emit changed();
    return archived.ok;
  }
  if (preview) {
    QDialog dialog(this);
    dialog.setWindowTitle("标签导入预览");
    dialog.resize(780, 570);
    auto* l = new QVBoxLayout(&dialog);
    auto* target = new QLabel(QString("目标信号：%1\n%2").arg(q(metadata.name), q(metadata.path)));
    target->setObjectName("labelImportTarget");
    target->setTextFormat(Qt::PlainText);
    target->setWordWrap(true);
    l->addWidget(target);
    auto* report = new QPlainTextEdit;
    report->setReadOnly(true);
    report->setMaximumHeight(150);
    l->addWidget(report);
    auto* records = new QTableWidget(0, 5);
    records->setObjectName("labelImportPreviewRecords");
    records->setHorizontalHeaderLabels(
        {"对象 / 类别", "段数", "物理几何", "时间依据", "可用范围"});
    records->setEditTriggers(QAbstractItemView::NoEditTriggers);
    records->horizontalHeader()->setStretchLastSection(true);
    records->setColumnWidth(0, 140);
    records->setColumnWidth(2, 280);
    l->addWidget(records, 1);
    auto* metaOnly = new QCheckBox("仅导入SigMF meta；保留配套JSON冲突诊断");
    metaOnly->setVisible(!path.endsWith(".txt", Qt::CaseInsensitive));
    l->addWidget(metaOnly);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("导入此来源集合");
    l->addWidget(buttons);
    auto show = [&] {
      QString text =
          result.ok() ? QString("%1个对象 · %2\n坐标：%3\n来源hash %4\n")
                            .arg(result.annotations.size())
                            .arg(q(result.sourceSet.formatId),
                                 q(result.sourceSet.coordinateInterpretation),
                                 q(result.sourceSet.sourceHash))
                      : "拒绝：" + result.error;
      for (const auto& d : result.diagnostics)
        text += QString("行%1 · %2\n").arg(d.line).arg(q(d.message));
      text +=
          QString(
              "绑定来源：%1\nFs %2 Hz · Fc %3 Hz · 物理 %4 / 可用 %5 samples\n")
              .arg(q(metadata.path))
              .arg(metadata.sampleRateHz, 0, 'g', 12)
              .arg(metadata.centerFrequencyHz, 0, 'g', 12)
              .arg(metadata.sampleCount)
              .arg(availableSamples(metadata));
      text += "保留全部原始字段；下表最多展示首200个对象，不截断实际导入。\n";
      report->setPlainText(text);
      const auto count = std::min<std::size_t>(200, result.annotations.size());
      records->setRowCount(static_cast<int>(count));
      for (std::size_t row = 0; row < count; ++row) {
        const auto& annotation = result.annotations[row];
        const auto* segment = activeSegment(annotation);
        const QString values[]{
            q(annotation.name), QString::number(annotation.segments.size()),
            segment
                ? physicalRangeText(segment->range, metadata.sampleRateHz)
                : "时间未定位",
            timeQualityName(segment ? segment->timeQuality
                                    : annotation.observationTimeQuality),
            segment && segment->range.time.end > availableSamples(metadata)
                ? "包含尚未读入范围；保留原标签"
                : "可用范围内"};
        for (int column = 0; column < 5; ++column)
          records->setItem(static_cast<int>(row), column,
                           new QTableWidgetItem(values[column]));
        records->item(static_cast<int>(row), 2)
            ->setToolTip(QString("对象 %1；完整 %2 段，导入保持源几何")
                             .arg(q(annotation.id))
                             .arg(annotation.segments.size()));
      }
      buttons->button(QDialogButtonBox::Ok)->setEnabled(result.ok());
    };
    show();
    connect(metaOnly, &QCheckBox::toggled, &dialog, [&](bool checked) {
      result = importLabelSource(path, metadata, generation, checked);
      show();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
  }
  if (!result.ok()) {
    if (error) *error = result.error;
    emit message(result.error);
    return false;
  }
  if (!session_.activeFile() ||
      session_.activeFile()->metadata.id != result.sourceSet.fileId ||
      session_.projectGeneration() != generation) {
    if (error) *error = "目标文件已变化";
    return false;
  }
  const auto published = session_.annotations().publishSourceSet(
      *session_.activeFile(), std::move(result.sourceSet),
      std::move(result.annotations));
  if (!published.ok) {
    if (error) *error = q(published.error);
    action(published);
    return false;
  }
  emit message(published.changed ? "标签原子导入完成；保留原始几何与尾部标签"
                                 : "相同来源已存在，未新增重复标记");
  emit changed();
  return true;
}
}  // namespace signalstudio
