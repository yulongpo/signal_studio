#include "ui/annotations/annotation_panel.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "infrastructure/annotation_json.h"
#include "infrastructure/label_import.h"
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
  return s == ReviewStatus::Accepted   ? "已接受"
         : s == ReviewStatus::Rejected ? "已拒绝"
                                       : "待复核";
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
    return QAbstractTableModel::flags(index) |
           (index.column() == 0 ? Qt::ItemIsUserCheckable : Qt::NoItemFlags);
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

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid()) {
      setFocus(Qt::MouseFocusReason);
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
      if (locate) locate(index);
      event->accept();
      return;
    }
    QTableView::mouseDoubleClickEvent(event);
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
}
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
  status_->addItems({"全部复核", "待复核", "已接受", "已拒绝"});
  bar->addWidget(status_);
  run_ = new QComboBox;
  run_->setObjectName("annotationRunFilter");
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
  button(bar, "标签预览…", "importLabels", [this] {
    auto path = QFileDialog::getOpenFileName(
        this, "导入指定两类原始标签", {},
        "标签 (*.txt *.sigmf-meta *.sigmf-data *.json)");
    if (!path.isEmpty()) importLabels(path);
  });
  button(bar, "JSON", "exportAnnotationJson", [this] { exportFile(false); });
  button(bar, "CSV", "exportAnnotationCsv", [this] { exportFile(true); });
  outer->addLayout(bar);
}

void AnnotationPanel::buildReviewBar(QVBoxLayout* outer) {
  auto* review = new QHBoxLayout;
  button(review, "接受所选", "acceptAnnotations", [this] {
    if (auto* f = session_.activeFile())
      action(session_.annotations().reviewBatch(*f, f->selectedMarkIds,
                                                ReviewStatus::Accepted));
  });
  button(review, "拒绝所选", "rejectAnnotations", [this] {
    if (auto* f = session_.activeFile())
      action(session_.annotations().reviewBatch(*f, f->selectedMarkIds,
                                                ReviewStatus::Rejected));
  });
  button(review, "撤回复核", "clearAnnotationReview", [this] {
    if (auto* file = session_.activeFile())
      action(session_.annotations().reviewBatch(*file, file->selectedMarkIds,
                                                ReviewStatus::Pending));
  });
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
  table_->setObjectName("annotationTable");
  table_->installEventFilter(this);
  table_->viewport()->installEventFilter(this);
  model_ = new AnnotationTableModel(session_, table_);
  table_->setModel(model_);
  table_->setItemDelegate(new AnnotationSelectionDelegate(table_));
  table_->setMouseTracking(true);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
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
  auto* confirmation = new QHBoxLayout;
  button(confirmation, "确认此段有效", "confirmAnnotationSegment", [this] {
    if (auto* file = session_.activeFile())
      action(session_.annotations().confirmSegment(*file, file->activeMarkId,
                                                   file->activeSegmentId));
  });
  details->addLayout(confirmation);
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
  button(draftActions, "提交修订", "commitAnnotationGeometry", [this] {
    for (auto* edit : {begin_, end_, low_, high_}) edit->commit();
    if (auto* file = session_.activeFile())
      session_.annotations().endRevision(*file);
    emit changed();
  });
  button(draftActions, "撤销草稿 · Esc", "cancelAnnotationGeometry", [this] {
    discardDrafts();
    if (auto* file = session_.activeFile())
      session_.annotations().endRevision(*file);
    emit changed();
  });
  details->addLayout(draftActions);
  connect(begin_, &AdaptiveValueEdit::valueCommitted, this,
          [this] { geometry(0); });
  connect(end_, &AdaptiveValueEdit::valueCommitted, this,
          [this] { geometry(1); });
  connect(low_, &AdaptiveValueEdit::valueCommitted, this,
          [this] { geometry(2); });
  connect(high_, &AdaptiveValueEdit::valueCommitted, this,
          [this] { geometry(3); });
  auto* edit = new QHBoxLayout;
  button(edit, "创建修订", "beginAnnotationRevision", [this] {
    if (auto* f = session_.activeFile())
      action(session_.annotations().beginRevision(*f, f->activeMarkId));
  });
  button(edit, "恢复原始", "restoreAnnotationOrigin", [this] {
    if (auto* f = session_.activeFile())
      action(session_.annotations().restoreOrigin(*f, f->activeMarkId));
  });
  button(edit, "结束修订", "endAnnotationRevision", [this] {
    if (auto* file = session_.activeFile()) {
      session_.annotations().endRevision(*file);
      discardDrafts();
      emit changed();
    }
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
  button(parts, "删段", "deleteAnnotationSegment", [this] {
    if (auto* f = session_.activeFile())
      if (auto* a = findMark(*f, f->activeMarkId))
        action(session_.annotations().deleteSegment(*f, a->id,
                                                    a->activeSegmentId));
  });
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
    bool accepted = false;
    const auto text = QInputDialog::getMultiLineText(
        this, "标记备注", "备注随修订记录保存", q(annotation->note), &accepted);
    if (accepted)
      action(session_.annotations().rename(
          *file, annotation->id, annotation->name, text.toStdString()));
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
  begin_->discard();
  end_->discard();
  low_->discard();
  high_->discard();
}
void AnnotationPanel::geometry(int field) {
  auto* f = session_.activeFile();
  if (!f) return;
  auto* a = findMark(*f, f->activeMarkId);
  if (!a) return;
  const auto* seg = activeSegment(*a);
  if (!seg) return;
  auto range = seg->range;
  if (field == 0 || field == 1) {
    const auto seconds = (field == 0 ? begin_->value() : end_->value()) / 1000;
    const long double samples = std::round(static_cast<long double>(seconds) *
                                           f->metadata.sampleRateHz);
    if (samples < 0 || samples > std::numeric_limits<SampleIndex>::max()) {
      error_->setText("时间超出可表示样本范围");
      refresh();
      return;
    }
    if (field == 0)
      range.time.begin = static_cast<SampleIndex>(samples);
    else
      range.time.end = static_cast<SampleIndex>(samples);
  }
  if (field == 2) range.frequency.lowerHz = low_->value() * 1e6;
  if (field == 3) range.frequency.upperHz = high_->value() * 1e6;
  const auto sourceKind = a->sourceKind;
  const auto r = session_.commitAnnotationGeometry(a->id, range);
  if (r.ok && sourceKind != AnnotationSource::Manual)
    session_.annotations().endRevision(*f);
  action(r);
  if (!r.ok) refresh();
}
void AnnotationPanel::refresh() {
  refreshing_ = true;
  auto* f = session_.activeFile();
  if (f) session_.annotations().reconcileSelection(*f);
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
    run_->clear();
    run_->addItem("全部运行", QString{});
    run_->addItem("非算法来源", "__no_run__");
    if (f)
      for (const auto& sourceSet : f->sourceSets)
        if (!sourceSet.runId.empty() && run_->findData(q(sourceSet.runId)) < 0)
          run_->addItem(q(sourceSet.runId), q(sourceSet.runId));
    run_->setCurrentIndex(std::max(0, run_->findData(runId)));
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
        table_->selectionModel()->setCurrentIndex(
            model_->index(row, 1), QItemSelectionModel::NoUpdate);
        table_->scrollTo(model_->index(row, 1),
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
  if (auto* accept = findChild<QPushButton*>("acceptAnnotations")) {
    accept->setText(QString("✓ 接受所选 %1").arg(counts.selected));
    accept->setEnabled(counts.selected != 0);
  }
  if (auto* reject = findChild<QPushButton*>("rejectAnnotations")) {
    reject->setText(QString("× 拒绝所选 %1").arg(counts.selected));
    reject->setEnabled(counts.selected != 0);
  }
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
      segments_->addItem(
          QString("段 %1 · %2–%3 samples · %4")
              .arg(i + 1)
              .arg(a->segments[i].range.time.begin)
              .arg(a->segments[i].range.time.end)
              .arg(a->segments[i].confirmed ? "已确认" : "未确认"),
          q(a->segments[i].id));
    segments_->setCurrentIndex(segments_->findData(q(a->activeSegmentId)));
  }
  const bool editable = a && session_.annotations().canEdit(*f, a->id) && seg;
  inspector_->findChild<QPushButton*>("confirmAnnotationSegment")
      ->setEnabled(
          a && seg && !a->deleted &&
          a->reviewStatus != ReviewStatus::Rejected && !seg->confirmed &&
          validateSegment(*seg, annotationBounds(f->metadata), true).ok);
  for (auto* w : {static_cast<QWidget*>(begin_), static_cast<QWidget*>(end_),
                  static_cast<QWidget*>(low_), static_cast<QWidget*>(high_)})
    w->setEnabled(editable);
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
    // A window observation has no editable activity geometry. Do not leave
    // the previously selected segment's coordinates visible in its controls.
    for (auto* edit : {begin_, end_, low_, high_}) {
      const QSignalBlocker blocker(edit);
      edit->setProperty("parameterDraft", false);
      edit->setProperty("invalid", false);
      edit->setValue(0);
      edit->clear();
      edit->setPlaceholderText("—");
      edit->setToolTip("没有已定位活动段；窗口观测支持请见检测证据");
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
bool AnnotationPanel::importLabels(const QString& path, QString* error,
                                   bool preview) {
  auto* f = session_.activeFile();
  if (!f) {
    if (error) *error = "请先导入信号";
    return false;
  }
  auto result =
      importLabelSource(path, f->metadata, session_.projectGeneration());
  if (preview) {
    QDialog dialog(this);
    dialog.setWindowTitle("两类标签导入预览");
    dialog.resize(780, 570);
    auto* l = new QVBoxLayout(&dialog);
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
              .arg(q(f->metadata.path))
              .arg(f->metadata.sampleRateHz, 0, 'g', 12)
              .arg(f->metadata.centerFrequencyHz, 0, 'g', 12)
              .arg(f->metadata.sampleCount)
              .arg(availableSamples(f->metadata));
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
                ? physicalRangeText(segment->range, f->metadata.sampleRateHz)
                : "时间未定位",
            timeQualityName(segment ? segment->timeQuality
                                    : annotation.observationTimeQuality),
            segment && segment->range.time.end > availableSamples(f->metadata)
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
      result = importLabelSource(path, f->metadata,
                                 session_.projectGeneration(), checked);
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
      session_.activeFile()->metadata.id != result.sourceSet.fileId) {
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
