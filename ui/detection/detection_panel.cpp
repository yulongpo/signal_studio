#include "ui/detection/detection_panel.h"

#include <QAbstractListModel>
#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <utility>

#include "infrastructure/detection_model_snapshot.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/project_file.h"
#include "infrastructure/source_loader.h"
#include "ui/detection/detection_phase.h"

namespace signalstudio {
namespace {
QString q(const std::string& value) { return QString::fromUtf8(value); }
QString stateName(DetectionLifecycle state) {
  switch (state) {
    case DetectionLifecycle::Queued:
      return "排队";
    case DetectionLifecycle::Validating:
      return "预检";
    case DetectionLifecycle::Running:
      return "运行";
    case DetectionLifecycle::Cancelling:
      return "取消中";
    case DetectionLifecycle::Completed:
      return "完成";
    case DetectionLifecycle::Cancelled:
      return "已取消";
    case DetectionLifecycle::Failed:
      return "失败";
    case DetectionLifecycle::Interrupted:
      return "中断";
  }
  return "未知";
}
QString workerPath() {
#ifdef Q_OS_WIN
  return QCoreApplication::applicationDirPath() +
         "/SignalStudioDetectionWorker.exe";
#else
  return QCoreApplication::applicationDirPath() +
         "/SignalStudioDetectionWorker";
#endif
}
QString number(double value) {
  return std::isfinite(value) ? QString::number(value, 'g', 9) : QString("—");
}
QString rangeText(const ViewRange& range, double sampleRate) {
  const auto samples =
      QString("[%1, %2) samples").arg(range.time.begin).arg(range.time.end);
  const auto time = std::isfinite(sampleRate) && sampleRate > 0
                        ? QString("%1–%2 ms")
                              .arg(number(range.time.begin / sampleRate * 1000))
                              .arg(number(range.time.end / sampleRate * 1000))
                        : QString("时间换算不可用");
  return QString("%1 · %2 · RF [%3, %4) MHz")
      .arg(time, samples, number(range.frequency.lowerHz / 1e6),
           number(range.frequency.upperHz / 1e6));
}
QString sourceValidityText(DetectionSourceValidity validity) {
  switch (validity) {
    case DetectionSourceValidity::Current:
      return "来源当前有效";
    case DetectionSourceValidity::Missing:
      return "来源缺失；已有证据保留";
    case DetectionSourceValidity::Changed:
      return "来源已变化；已有证据保留";
    case DetectionSourceValidity::Unsupported:
      return "来源不支持当前检测";
  }
  return "来源状态未知";
}
QString coverageSummary(const DetectionRun& run, DetectionCoverageKind kind,
                        std::size_t limit = 1) {
  QStringList ranges;
  std::size_t count = 0;
  for (const auto& coverage : run.coverage) {
    if (coverage.kind != kind) continue;
    if (count++ < limit)
      ranges.push_back(
          rangeText(coverage.range, run.request.sourceIdentity.sampleRateHz));
  }
  if (count == 0) return "—（无对应记录）";
  return QString("%1（%2 条记录%3）")
      .arg(ranges.join(" | "))
      .arg(count)
      .arg(count > limit ? "；其余见详情" : "");
}
QString shortRunId(const std::string& runId) {
  const auto id = q(runId);
  return id.size() > 24 ? id.left(12) + "…" + id.right(8) : id;
}
struct RunCard {
  struct Signature {
    QString sourceName, error, phase;
    DetectionLifecycle lifecycle = DetectionLifecycle::Queued;
    DetectionSourceValidity validity = DetectionSourceValidity::Current;
    std::uint64_t read = 0, completed = 0, eligible = 0, results = 0,
                  published = 0, started = 0, finished = 0, refined = 0,
                  refinementTotal = 0;
    std::size_t coverageCount = 0, chunkCount = 0;
    bool recoverable = false, stale = false;
    bool operator==(const Signature&) const = default;
  };
  QString id, title, sourcePath, error;
  QStringList facts;
  bool cancellable = false;
  Signature signature;
  bool operator==(const RunCard&) const = default;
};
RunCard::Signature cardSignature(const FileState& file, const DetectionRun& run,
                                 std::uint64_t published) {
  const auto* slot = currentDetectionSlot(file, run.request.sourceIdentity.selectedSampleChannel);
  return {q(file.metadata.name),
          q(run.error),
          q(run.computationPhase),
          run.lifecycle,
          run.sourceValidity,
          run.readFrames,
          run.completedWindows,
          run.eligibleWindows,
          run.resultCount,
          published,
          run.startedAtMilliseconds,
          run.finishedAtMilliseconds,
          run.refinedCandidates,
          run.totalRefinementCandidates,
          run.coverage.size(),
          run.chunks.size(),
          run.recoverable,
          slot && slot->committedRunId == run.request.runId && slot->stale};
}
RunCard makeCard(const FileState& file, const DetectionRun& run,
                 std::uint64_t published) {
  const auto& request = run.request;
  RunCard card;
  card.signature = cardSignature(file, run, published);
  card.id = q(request.runId);
  card.title = stateName(run.lifecycle) + (run.recoverable ? " / 可恢复" : "") +
               " · " + q(file.metadata.name);
  card.sourcePath = q(request.sourceIdentity.path);
  card.cancellable = !detectionIsTerminal(run.lifecycle);
  card.facts = {
      QString("模型：%1 · 阶段：%2 · %3")
          .arg(q(request.profile.name),
               detectionPhaseText(run.computationPhase), shortRunId(request.runId)),
      QString("已读谱帧 %1 · 窗口 %2 / %3 · 精修候选 %4 / %5")
          .arg(run.readFrames)
          .arg(run.completedWindows)
          .arg(run.eligibleWindows)
          .arg(run.refinedCandidates)
          .arg(run.totalRefinementCandidates),
      QString("结果记录 %1 · 已发布 %2 个对象")
          .arg(run.resultCount)
          .arg(published)};
  if (card.signature.stale)
    card.facts.back() += " · 待更新（上次冻结配置）";
  else if (run.lifecycle == DetectionLifecycle::Completed && !published) {
    const auto* slot = currentDetectionSlot(file, request.sourceIdentity.selectedSampleChannel);
    card.facts.back() += run.resultCount == 0 ? " · 完成但未检出"
        : slot && slot->committedRunId == request.runId ? " · 当前无标记"
                                                      : " · 待提交";
  }
  QString reason;
  if (!run.error.empty()) {
    reason = q(run.error);
  } else if (run.sourceValidity != DetectionSourceValidity::Current) {
    reason = sourceValidityText(run.sourceValidity);
  } else {
    for (const auto& coverage : run.coverage) {
      if ((coverage.kind == DetectionCoverageKind::Unread ||
           coverage.kind == DetectionCoverageKind::Excluded) &&
          !coverage.reason.empty()) {
        reason = q(coverage.reason);
        break;
      }
    }
  }
  if (!reason.isEmpty()) {
    reason = reason.trimmed().section('\n', 0, 0).simplified();
    card.error = "提示：" + reason.left(160) +
                 (reason.size() > 160 ? "…（详情）" : "");
  }
  return card;
}
QString cardHtml(const RunCard& card) {
  QString html =
      "<div style='color:#e5f1fb'><b>" + card.title.toHtmlEscaped() + "</b>";
  for (const auto& fact : card.facts)
    html += "<p style='margin-top:5px;margin-bottom:0'>" +
            fact.toHtmlEscaped() + "</p>";
  if (!card.error.isEmpty())
    html += "<p style='color:#ffc0a0;margin-top:5px'>" +
            card.error.toHtmlEscaped() + "</p>";
  return html + "</div>";
}
QString runFacts(const DetectionRun& run, std::uint64_t published) {
  const auto& request = run.request;
  const auto& source = request.sourceIdentity;
  QStringList facts{
      "运行：" + q(request.runId) + " · " + stateName(run.lifecycle),
      "来源：" + q(source.path),
      QString("Fs %1 MS/s · Fc %2 MHz · %3")
          .arg(number(source.sampleRateHz / 1e6),
               number(source.centerFrequencyHz / 1e6),
               sourceValidityText(run.sourceValidity)),
      "冻结请求：" + rangeText(request.requestedRange, source.sampleRateHz),
      "冻结上下文：" + rangeText(request.contextRange, source.sampleRateHz),
      QString("方案：%1（%2 v%3） · 策略：%4 · 范围来源：%5")
          .arg(q(request.profile.name), q(request.profile.id))
          .arg(request.profile.version)
          .arg(QString::fromLatin1(
                   detectionTimeStrategyName(request.parameters.timeStrategy)),
               q(request.rangeOrigin)),
      QString("FFT %1 · 谱步长 %2 ms · 模型 %3 帧 · 窗口滑动 %4 帧")
          .arg(request.profile.fftSize)
          .arg(number(request.profile.hopSeconds * 1000))
          .arg(request.profile.modelFrames)
          .arg(request.profile.modelHopFrames),
      QString(
          "阈值 %1 · NMS IoU %2 · topK %3 · batch %4 · minHits %5 · maxMiss %6")
          .arg(number(request.parameters.scoreThreshold),
               number(request.parameters.nmsIou))
          .arg(request.parameters.topK)
          .arg(request.parameters.batchSize)
          .arg(request.parameters.minHits)
          .arg(request.parameters.maxMiss),
      QString("合并频差 %1 Hz · 带宽噪底余量 %2 dB · 多突发拆分 %3")
          .arg(number(request.parameters.mergeFrequencyHz),
               number(request.parameters.bandwidthMarginDb),
               request.parameters.splitBursts ? "开启" : "关闭"),
      QString("实际已读谱帧 %1 · unique completed windows %2 / eligible %3")
          .arg(run.readFrames)
          .arg(run.completedWindows)
          .arg(run.eligibleWindows),
      QString("计算阶段 %1 · 精修候选处理 %2 / %3（包含回退，非成功数）")
          .arg(detectionPhaseText(run.computationPhase))
          .arg(run.refinedCandidates)
          .arg(run.totalRefinementCandidates),
      QString("工程已发布 %1 个来源对象 · 持久化 %2 条记录 · %3 个不可变分片")
          .arg(published)
          .arg(run.resultCount)
          .arg(run.chunks.size()),
      "已覆盖：" + coverageSummary(run, DetectionCoverageKind::Completed, 8),
      "已读：" + coverageSummary(run, DetectionCoverageKind::Read, 8),
      "可处理：" + coverageSummary(run, DetectionCoverageKind::Eligible, 8),
      "未读：" + coverageSummary(run, DetectionCoverageKind::Unread, 8),
      "排除：" + coverageSummary(run, DetectionCoverageKind::Excluded, 8),
      "结论仅限已覆盖范围；未读或排除区域没有检测结论。",
      QString("运行环境：%1 · 设备：%2").arg(q(run.runtime), q(run.device)),
      "Manifest：" + q(run.manifestRelativePath),
      QString("工作集预算 %1 bytes · 证据预算 %2 bytes")
          .arg(request.workingSetBudgetBytes)
          .arg(request.evidenceBudgetBytes)};
  std::size_t reasons = 0;
  for (const auto& coverage : run.coverage)
    if (!coverage.reason.empty() && reasons++ < 16)
      facts.push_back(QString("覆盖说明（%1）：%2 · %3")
                          .arg(QString::fromLatin1(
                                   detectionCoverageKindName(coverage.kind)),
                               rangeText(coverage.range, source.sampleRateHz),
                               q(coverage.reason)));
  if (reasons > 16) facts.push_back("其余覆盖说明见折叠的原始技术 JSON。");
  if (request.parentRunId)
    facts.push_back("派生自：" + q(*request.parentRunId));
  if (!run.error.empty()) facts.push_back("错误：" + q(run.error));
  return facts.join("\n\n");
}
}  // namespace

class DetectionRunListModel : public QAbstractListModel {
 public:
  explicit DetectionRunListModel(QObject* parent)
      : QAbstractListModel(parent) {}
  int rowCount(const QModelIndex& parent = {}) const override {
    return parent.isValid() ? 0 : static_cast<int>(cards_.size());
  }
  QVariant data(const QModelIndex& index, int role) const override {
    const auto* value = card(index.row());
    if (!index.isValid() || !value) return {};
    if (role == Qt::UserRole) return value->id;
    if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole)
      return value->title + "\n" + value->facts.join("\n") +
             (value->error.isEmpty() ? "" : "\n" + value->error);
    if (role == Qt::ToolTipRole)
      return value->id + "\n" + value->sourcePath +
             "\nEnter 查看来源 · Alt+Return 详情 · Ctrl+Return 派生 · "
             "Ctrl+Shift+X 取消";
    return {};
  }
  const RunCard* card(int row) const {
    return row >= 0 && row < cards_.size() ? &cards_[row] : nullptr;
  }
  int rowForId(const QString& id) const {
    for (int row = 0; row < cards_.size(); ++row)
      if (cards_[row].id == id) return row;
    return -1;
  }
  bool updateCards(const QList<RunCard>& next) {
    bool changed = false;
    for (int row = static_cast<int>(cards_.size()) - 1; row >= 0; --row) {
      if (std::none_of(next.begin(), next.end(), [&](const auto& value) {
            return value.id == cards_[row].id;
          })) {
        beginRemoveRows({}, row, row);
        cards_.removeAt(row);
        endRemoveRows();
        changed = true;
      }
    }
    for (int row = 0; row < next.size(); ++row) {
      if (row >= cards_.size() || cards_[row].id != next[row].id) {
        const int existing = rowForId(next[row].id);
        if (existing >= 0) {
          beginMoveRows({}, existing, existing, {}, row);
          cards_.move(existing, row);
          endMoveRows();
        } else {
          beginInsertRows({}, row, row);
          cards_.insert(row, next[row]);
          endInsertRows();
        }
        changed = true;
      }
      if (cards_[row] != next[row]) {
        cards_[row] = next[row];
        emit dataChanged(index(row), index(row));
        changed = true;
      }
    }
    return changed;
  }

 private:
  QList<RunCard> cards_;
};

namespace {
class RunCardDelegate : public QStyledItemDelegate {
 public:
  using Callbacks = std::array<std::function<void(const QString&)>, 4>;
  RunCardDelegate(QListView* view, std::array<QAction*, 4> actions,
                  Callbacks callbacks, std::function<bool()> canDerive)
      : QStyledItemDelegate(view),
        view_(view),
        actions_(actions),
        callbacks_(std::move(callbacks)),
        canDerive_(std::move(canDerive)) {}
  QSize sizeHint(const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override {
    const auto* card = value(index);
    if (!card) return {};
    QTextDocument document;
    prepareDocument(document, *card, option.font, view_->viewport()->width());
    return {std::max(1, view_->viewport()->width()),
            static_cast<int>(std::ceil(document.size().height())) +
                actionHeight(view_->viewport()->width()) + 37};
  }
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    const auto* card = value(index);
    if (!card) return;
    const auto box = option.rect.adjusted(3, 3, -3, -3);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    painter->save();
    painter->setClipRect(option.rect);
    painter->setPen(QColor(selected ? "#55c3e0" : "#3c526c"));
    painter->setBrush(QColor(selected ? "#163c51" : "#102137"));
    painter->drawRoundedRect(box, 5, 5);
    QTextDocument document;
    prepareDocument(document, *card, option.font, option.rect.width());
    painter->save();
    painter->translate(option.rect.left() + 12, option.rect.top() + 12);
    document.drawContents(painter);
    painter->restore();
    const auto rects = actionRects(option.rect);
    for (int action = 0; action < 4; ++action)
      paintButton(painter, rects[action], actions_[action]->text(),
                  actionEnabled(action, *card));
    painter->restore();
  }
  bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                 const QStyleOptionViewItem& option,
                 const QModelIndex& index) override {
    const auto* card = value(index);
    if (card && event->type() == QEvent::ToolTip) {
      const auto rects = actionRects(option.rect);
      for (int action = 0; action < 4; ++action) {
        if (!rects[action].contains(event->pos())) continue;
        auto text = actions_[action]->toolTip();
        if (action == 3 && !card->cancellable) text += "\n任务已结束，不能取消。";
        QToolTip::showText(event->globalPos(), text, view_->viewport(),
                           rects[action]);
        return true;
      }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
  }
  bool editorEvent(QEvent* event, QAbstractItemModel*,
                   const QStyleOptionViewItem& option,
                   const QModelIndex& index) override {
    const auto* card = value(index);
    if (!card || (event->type() != QEvent::MouseButtonPress &&
                  event->type() != QEvent::MouseButtonRelease))
      return false;
    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton) return false;
    const auto rects = actionRects(option.rect);
    int action = -1;
    for (int candidate = 0; candidate < 4; ++candidate)
      if (rects[candidate].contains(mouse->position().toPoint()) &&
          actionEnabled(candidate, *card)) {
        action = candidate;
        break;
      }
    if (event->type() == QEvent::MouseButtonPress) {
      pressed_ = index;
      pressedRunId_ = card->id;
      pressedAction_ = action;
      return action >= 0;
    }
    const bool activate =
        pressed_ == index && pressedRunId_ == card->id && action >= 0 &&
        action == pressedAction_;
    pressed_ = QPersistentModelIndex{};
    pressedRunId_.clear();
    pressedAction_ = -1;
    if (!activate) return false;
    const auto id = card->id;
    callbacks_[action](id);
    return true;
  }

 private:
  static const RunCard* value(const QModelIndex& index) {
    if (!index.isValid() || !index.model()) return nullptr;
    return static_cast<const DetectionRunListModel*>(index.model())
        ->card(index.row());
  }
  static void prepareDocument(QTextDocument& document, const RunCard& card,
                              const QFont& font, int width) {
    document.setDefaultFont(font);
    document.setDocumentMargin(0);
    document.setHtml(cardHtml(card));
    document.setTextWidth(std::max(1, width - 24));
  }
  int actionColumns(int width) const {
    int minimum = 1;
    for (const auto* action : actions_)
      minimum = std::max(
          minimum, view_->fontMetrics().horizontalAdvance(action->text()) + 24);
    return std::clamp((std::max(1, width - 24) + 8) / (minimum + 8), 1, 4);
  }
  int actionHeight(int width) const {
    const int rows = (4 + actionColumns(width) - 1) / actionColumns(width);
    return rows * 29 + (rows - 1) * 6;
  }
  std::array<QRect, 4> actionRects(const QRect& rect) const {
    const int available = std::max(1, rect.width() - 24);
    const int columns = actionColumns(rect.width());
    const int width = std::min(160, std::max(1, (available - (columns - 1) * 8) /
                                                 columns));
    const int top = rect.bottom() - 11 - actionHeight(rect.width());
    std::array<QRect, 4> rects;
    for (int action = 0; action < 4; ++action)
      rects[action] = {rect.left() + 12 + (action % columns) * (width + 8),
                       top + (action / columns) * 35, width, 29};
    return rects;
  }
  bool actionEnabled(int action, const RunCard& card) const {
    if (action == 3) return card.cancellable;
    if (action == 2) return canDerive_();
    return true;
  }
  void paintButton(QPainter* painter, const QRect& rect,
                   const QString& text, bool enabled) const {
    QStyleOptionButton button;
    button.initFrom(view_);
    button.rect = rect;
    button.text = text;
    button.state.setFlag(QStyle::State_Enabled, enabled && view_->isEnabled());
    view_->style()->drawControl(QStyle::CE_PushButton, &button, painter, view_);
  }
  QListView* view_;
  std::array<QAction*, 4> actions_;
  Callbacks callbacks_;
  std::function<bool()> canDerive_;
  QPersistentModelIndex pressed_;
  QString pressedRunId_;
  int pressedAction_ = -1;
};
}  // namespace

DetectionPanel::DetectionPanel(Session& session,
                               std::function<QString()> directory,
                               QWidget* parent)
    : QWidget(parent),
      session_(session),
      projectDirectory_(std::move(directory)),
      tasks_(std::make_unique<DetectionTaskManager>(workerPath())),
      generation_(session.projectGeneration()),
      listGeneration_(session.projectGeneration()) {
  setObjectName("detectionPanel");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 4, 8, 4);
  auto* commands = new QHBoxLayout;
  details_ = new QPushButton("任务详情");
  details_->setObjectName("detectionRunDetails");
  details_->setProperty("preserveParameterDrafts", true);
  commands->addWidget(details_);
  connect(details_, &QPushButton::clicked, this,
          &DetectionPanel::showSelectedDetails);
  derive_ = new QPushButton("派生新运行");
  derive_->setObjectName("deriveWidebandDetection");
  derive_->setProperty("preserveParameterDrafts", true);
  commands->addWidget(derive_);
  connect(derive_, &QPushButton::clicked, this, [this] {
    if (const auto* run = selectedRun()) deriveRun(q(run->request.runId));
  });
  recover_ = new QPushButton("查找未引用结果…");
  recover_->setObjectName("recoverWidebandDetection");
  commands->addWidget(recover_);
  connect(recover_, &QPushButton::clicked, this,
          &DetectionPanel::discoverRecoverable);
  commands->addStretch();
  layout->addLayout(commands);
  summary_ = new QLabel("本工程尚无任务。检测配置位于右侧宽带检测卡。");
  summary_->setObjectName("detectionStatus");
  summary_->setTextFormat(Qt::PlainText);
  summary_->setWordWrap(true);
  layout->addWidget(summary_);
  list_ = new QListView;
  list_->setObjectName("detectionRunList");
  list_->setProperty("preserveParameterDrafts", true);
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setResizeMode(QListView::Adjust);
  list_->setLayoutMode(QListView::Batched);
  list_->setBatchSize(32);
  list_->setMouseTracking(true);
  auto action = [this](const QString& name, const QString& text,
                       const QList<QKeySequence>& shortcuts,
                       const QString& tooltip) {
    auto* result = new QAction(text, list_);
    result->setObjectName(name);
    result->setShortcuts(shortcuts);
    result->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    result->setAutoRepeat(false);
    result->setToolTip(tooltip);
    result->setStatusTip(tooltip);
    result->setProperty("preserveParameterDrafts", true);
    result->setShortcutVisibleInContextMenu(true);
    list_->addAction(result);
    return result;
  };
  taskViewSourceAction_ = action(
      "taskViewSourceAction", "查看来源",
      {QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)},
      "查看来源（Return / Enter）：定位源文件和此任务；同文件保留配置草稿。");
  taskDetailsAction_ = action(
      "taskDetailsAction", "详情",
      {QKeySequence(Qt::ALT | Qt::Key_Return),
       QKeySequence(Qt::ALT | Qt::Key_Enter)},
      "详情（Alt+Return）：查看冻结范围、覆盖、请求与技术 JSON；保留配置草稿。");
  taskDeriveAction_ = action(
      "taskDeriveAction", "派生",
      {QKeySequence(Qt::CTRL | Qt::Key_Return),
       QKeySequence(Qt::CTRL | Qt::Key_Enter)},
      "派生（Ctrl+Return）：复制选中任务的参数与冻结范围，不自动运行。");
  taskCancelAction_ = action(
      "taskCancelAction", "取消",
      {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X)},
      "取消（Ctrl+Shift+X）：只取消选中任务的 runId，保留已完成前缀。");
  connect(taskViewSourceAction_, &QAction::triggered, this, [this] {
    if (const auto* run = selectedRun()) openRunSource(q(run->request.runId));
  });
  connect(taskDetailsAction_, &QAction::triggered, this,
          &DetectionPanel::showSelectedDetails);
  connect(taskDeriveAction_, &QAction::triggered, this, [this] {
    if (const auto* run = selectedRun()) deriveRun(q(run->request.runId));
  });
  connect(taskCancelAction_, &QAction::triggered, this,
          &DetectionPanel::cancelSelected);
  details_->setToolTip(taskDetailsAction_->toolTip() + "\n快捷键在任务列表内生效。");
  derive_->setToolTip(taskDeriveAction_->toolTip() + "\n快捷键在任务列表内生效。");
  list_->setToolTip("选择任务后：Enter 查看来源 · Alt+Return 详情 · "
                    "Ctrl+Return 派生 · Ctrl+Shift+X 取消。右键打开同名操作。");
  model_ = new DetectionRunListModel(list_);
  list_->setModel(model_);
  list_->setItemDelegate(new RunCardDelegate(
      list_, {taskViewSourceAction_, taskDetailsAction_, taskDeriveAction_,
              taskCancelAction_},
      {[this](const QString& id) { openRunSource(id); },
       [this](const QString& id) { showRunDetails(id); },
       [this](const QString& id) { deriveRun(id); },
       [this](const QString& id) { cancelRun(id); }},
      [this] { return !projectDirectory_().isEmpty(); }));
  connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this] { updateSelectionActions(); });
  // QAction owns Return/Enter so modifier shortcuts never also activate a row.
  connect(list_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
    openRunSource(index.data(Qt::UserRole).toString());
  });
  list_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(list_, &QWidget::customContextMenuRequested, this,
          [this](const QPoint& position) {
            const auto index = list_->indexAt(position);
            if (index.isValid()) list_->setCurrentIndex(index);
            updateSelectionActions();
            QMenu menu(list_);
            menu.setToolTipsVisible(true);
            menu.addActions({taskViewSourceAction_, taskDetailsAction_,
                             taskDeriveAction_, taskCancelAction_});
            const auto anchor = position.x() >= 0 && position.y() >= 0
                                    ? position
                                    : list_->visualRect(list_->currentIndex())
                                          .center();
            menu.exec(list_->viewport()->mapToGlobal(anchor));
          });
  layout->addWidget(list_, 1);
  updateSelectionActions();
  timer_ = new QTimer(this);
  timer_->setInterval(100);
  connect(timer_, &QTimer::timeout, this, &DetectionPanel::poll);
  timer_->start();
}
DetectionPanel::~DetectionPanel() = default;

bool DetectionPanel::startRequest(DetectionRequest request, QString& error) {
  const auto* file = session_.activeFile();
  if (!file || file->metadata.demo ||
      request.sourceIdentity.projectGeneration !=
          session_.projectGeneration() ||
      request.sourceIdentity.fileId != file->metadata.id) {
    error = "检测仅接受当前工程中真实文件的来源快照";
    return false;
  }
  const auto path = projectFilePath_ ? projectFilePath_() : QString{};
  if (path.isEmpty() || !QFileInfo::exists(path)) {
    error = "检测事务需要当前已保存 .ssproj 的实际文件路径";
    return false;
  }
  if (!freezeDetectionModelSnapshot(request, error)) return false;
  const auto id = request.runId;
  if (!tasks_->enqueue(projectDirectory_(), file->metadata, std::move(request),
                       error))
    return false;
  const auto snapshots = tasks_->snapshots();
  const auto queued = std::find_if(snapshots.begin(), snapshots.end(),
      [&](const auto& run) { return run.request.runId == id; });
  Project candidate = session_.project();
  DetectionResultTicket ticket;
  if (queued == snapshots.end() ||
      !DetectionResultService::beginDetection(candidate, queued->request,
          session_.projectGeneration(), ticket, error)) {
    tasks_->cancel(id);
    return false;
  }
  for (auto& source : candidate.files)
    if (source.metadata.id == ticket.fileId) source.detectionRuns.push_back(*queued);
  if (!ProjectFileCodec::save(path, candidate, error)) {
    tasks_->cancel(id);
    return false;
  }
  if (generation_ != session_.projectGeneration()) {
    tasks_->cancelGeneration(generation_);
    generation_ = session_.projectGeneration();
    tickets_.clear();
  }
  session_.project() = std::move(candidate);
  // Superseded requests may finish, but only the newest ticket can publish.
  for (const auto& [oldId, oldTicket] : tickets_)
    if (oldTicket.fileId == ticket.fileId && oldTicket.sampleChannel == ticket.sampleChannel)
      tasks_->cancel(oldId);
  tickets_[id] = std::move(ticket);
  tasks_->pump();
  poll();
  return true;
}

void DetectionPanel::configure(const DetectionRun* parentRun) {
  const auto* file = session_.activeFile();
  if (!file || file->metadata.demo || projectDirectory_().isEmpty()) {
    emit message("请先创建/保存工程并打开真实宽带数据");
    return;
  }
  if (parentRun)
    configurationParent_ = *parentRun;
  else
    configurationParent_.reset();
  emit configurationRequested(configurationParent_ ? &*configurationParent_
                                                   : nullptr);
}
void DetectionPanel::poll() {
  if (generation_ != session_.projectGeneration()) {
    tasks_->cancelGeneration(generation_);
    generation_ = session_.projectGeneration();
    tickets_.clear();
  }
  scheduleGc();
  tasks_->pump();
  bool changedState = false;
  for (auto& run : tasks_->snapshots()) {
    if (run.request.sourceIdentity.projectGeneration != generation_) continue;
    const auto ticketEntry = tickets_.find(run.request.runId);
    if (ticketEntry == tickets_.end()) continue;
    const auto ticket = ticketEntry->second;
    auto file = std::find_if(
        session_.project().files.begin(), session_.project().files.end(),
        [&](const auto& value) {
          return value.metadata.id == run.request.sourceIdentity.fileId;
        });
    if (file == session_.project().files.end()) {
      tasks_->invalidateSource(run.request.runId,
                               DetectionSourceValidity::Missing);
      tickets_.erase(ticketEntry);
      continue;
    }
    const auto& source = run.request.sourceIdentity;
    if (!QFileInfo::exists(q(source.path))) {
      run.sourceValidity = DetectionSourceValidity::Missing;
    } else {
      auto format = file->metadata.sampleFormat;
      format.selectedChannel = source.selectedSampleChannel;
      if (file->metadata.path != source.path ||
               format != source.sampleFormat ||
               file->metadata.sampleRateHz != source.sampleRateHz ||
               file->metadata.centerFrequencyHz != source.centerFrequencyHz ||
               iqSourceFingerprint(q(source.path)).toStdString() !=
                   source.quickFingerprint) {
        run.sourceValidity = DetectionSourceValidity::Changed;
      }
    }
    const auto* slot = currentDetectionSlot(*file, ticket.sampleChannel);
    if (!slot || slot->generation != ticket.slotGeneration ||
        slot->pendingRunId != ticket.runId) {
      tasks_->cancel(run.request.runId);
      tickets_.erase(ticketEntry);
      continue;
    }
    if (run.sourceValidity != DetectionSourceValidity::Current ||
        (detectionIsTerminal(run.lifecycle) && run.lifecycle != DetectionLifecycle::Completed)) {
      tasks_->invalidateSource(run.request.runId, run.sourceValidity);
      DetectionResultService::abandonDetection(session_.project(), ticket);
      emit message(run.sourceValidity != DetectionSourceValidity::Current
          ? "来源已变化：未发布本次检测，上次正式结果待更新"
          : stateName(run.lifecycle) + "：未发布部分结果，上次正式结果待更新。" + q(run.error));
      tickets_.erase(ticketEntry);
      changedState = true;
      continue;
    }
    auto current =
        std::find_if(file->detectionRuns.begin(), file->detectionRuns.end(),
                     [&](const auto& value) {
                       return value.request.runId == run.request.runId;
                     });
    if (current == file->detectionRuns.end())
      file->detectionRuns.push_back(run);
    else
      *current = run;
    DetectionPublication publication;
    if (tasks_->takePublication(run.request.runId, publication)) {
      const auto path = projectFilePath_ ? projectFilePath_() : QString{};
      const auto result = DetectionResultService::replaceCurrentDetection(
          session_.project(), session_.annotations(), ticket,
          session_.projectGeneration(), run, std::move(publication), path);
      emit message(result ? "检测事务已保存并整体替换；ROI 外未检测，0 检出同样清空旧结果"
                          : "检测未提交，保留上次正式集合：" + result.error);
      tickets_.erase(run.request.runId);
      if (result) scheduleGc(path);
      changedState = true;
    }
  }
  if (refreshRunList(changedState)) emit runProgressChanged();
  if (changedState) emit changed();
}

void DetectionPanel::markConfigurationStale() {
  const auto* file = session_.activeFile();
  if (!file) return;
  const auto id = file->metadata.id;
  const auto channel = file->metadata.sampleFormat.selectedChannel;
  for (const auto& [runId, ticket] : tickets_)
    if (ticket.fileId == id && ticket.sampleChannel == channel) tasks_->cancel(runId);
  if (DetectionResultService::markStale(session_.project(), id, channel)) {
    refresh();
    emit changed();
  }
}

void DetectionPanel::scheduleGc(const QString& path) {
  if (!path.isEmpty()) pendingGcPath_ = path;
  if (gc_.valid()) {
    if (gc_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    const auto error = gc_.get();
    if (!error.isEmpty()) emit message("旧检测资产清理已延后：" + error);
  }
  if (pendingGcPath_.isEmpty()) return;
  const auto target = std::exchange(pendingGcPath_, QString{});
  gc_ = std::async(std::launch::async, [target] {
    QString error;
    DetectionGcReport report;
    collectRetiredDetectionAssets(target, report, error);
    return error;
  });
}

void DetectionPanel::refresh() { refreshRunList(true); }
bool DetectionPanel::refreshRunList(bool refreshPublicationCounts) {
  if (listGeneration_ != session_.projectGeneration()) {
    listGeneration_ = session_.projectGeneration();
    list_->clearSelection();
    list_->setCurrentIndex({});
    publishedCounts_.clear();
    configurationParent_.reset();
  } else if (refreshPublicationCounts)
    publishedCounts_.clear();
  const auto selected = list_->currentIndex().data(Qt::UserRole).toString();
  const int scroll = list_->verticalScrollBar()->value();
  std::deque<std::pair<const FileState*, const DetectionRun*>> runs;
  std::size_t totalRuns = 0;
  for (const auto& sourceFile : session_.project().files)
    for (const auto& run : sourceFile.detectionRuns) {
      ++totalRuns;
      runs.emplace_back(&sourceFile, &run);
      if (runs.size() > 200) runs.pop_front();
    }
  QList<RunCard> cards;
  cards.reserve(static_cast<qsizetype>(runs.size()));
  for (const auto& [file, run] : runs) {
    const auto published = publishedCount(*file, *run);
    const auto* previous =
        model_->card(model_->rowForId(q(run->request.runId)));
    // Requests are immutable. Live coverage advances with frame/window
    // counters; explicit refresh also rebuilds facts after loading or editing
    // the project.
    if (!refreshPublicationCounts && previous &&
        previous->signature == cardSignature(*file, *run, published))
      cards.push_back(*previous);
    else
      cards.push_back(makeCard(*file, *run, published));
  }
  const bool changed = model_->updateCards(cards);
  if (changed) {
    const int row = model_->rowForId(selected);
    if (row >= 0) list_->setCurrentIndex(model_->index(row));
    list_->verticalScrollBar()->setValue(scroll);
  }
  const auto summary =
      totalRuns ? QString(
                      "本工程 %1 个运行 · 显示 %2 个（上限 200） · "
                      "任务使用冻结范围，未读或排除区域没有检测结论")
                      .arg(totalRuns)
                      .arg(runs.size())
                : QString("本工程尚无任务。检测配置位于右侧宽带检测卡。");
  if (summary_->text() != summary) summary_->setText(summary);
  updateSelectionActions();
  return changed;
}
const DetectionRun* DetectionPanel::selectedRun() const {
  return findRun(list_->currentIndex().data(Qt::UserRole).toString());
}
const DetectionRun* DetectionPanel::findRun(const QString& runId) const {
  if (runId.isEmpty()) return nullptr;
  const auto id = runId.toStdString();
  for (const auto& file : session_.project().files) {
    const auto found =
        std::find_if(file.detectionRuns.begin(), file.detectionRuns.end(),
                     [&](const auto& run) { return run.request.runId == id; });
    if (found != file.detectionRuns.end()) return &*found;
  }
  return nullptr;
}
void DetectionPanel::updateSelectionActions() {
  const auto* run = selectedRun();
  const bool present = run != nullptr;
  const bool canDerive = present && !projectDirectory_().isEmpty();
  details_->setEnabled(present);
  derive_->setEnabled(canDerive);
  taskViewSourceAction_->setEnabled(present);
  taskDetailsAction_->setEnabled(present);
  taskDeriveAction_->setEnabled(canDerive);
  taskCancelAction_->setEnabled(present && !detectionIsTerminal(run->lifecycle));
  recover_->setEnabled(session_.activeFile() && !projectDirectory_().isEmpty());
}
std::uint64_t DetectionPanel::publishedCount(const FileState& file,
                                             const DetectionRun& run) {
  auto& counts = publishedCounts_[file.metadata.id];
  if (!counts.initialized || counts.annotationCount != file.marks.size() ||
      counts.sourceSetCount != file.sourceSets.size()) {
    counts.initialized = true;
    counts.annotationCount = file.marks.size();
    counts.sourceSetCount = file.sourceSets.size();
    counts.runs.clear();
    std::unordered_map<std::string, std::string> sourceRuns;
    for (const auto& source : file.sourceSets)
      if (!source.runId.empty()) sourceRuns[source.id] = source.runId;
    for (const auto& annotation : file.marks) {
      if (annotation.sourceKind != AnnotationSource::Algorithm) continue;
      const auto source = sourceRuns.find(annotation.sourceSetId);
      const auto id = source != sourceRuns.end() ? source->second
                      : annotation.originEvidence
                          ? annotation.originEvidence->runId
                          : std::string{};
      if (!id.empty()) ++counts.runs[id];
    }
  }
  const auto found = counts.runs.find(run.request.runId);
  return found == counts.runs.end() ? 0 : found->second;
}
void DetectionPanel::openRunSource(const QString& runId) {
  const auto* run = findRun(runId);
  if (!run) return;
  const auto original = *run;
  if (!session_.activateFile(original.request.sourceIdentity.fileId)) {
    emit message("任务来源文件不在当前工程中");
    return;
  }
  const int row = model_->rowForId(runId);
  if (row >= 0) list_->setCurrentIndex(model_->index(row));
  emit changed();
  emit runFocused(runId);
}
void DetectionPanel::deriveRun(const QString& runId) {
  const auto* run = findRun(runId);
  if (!run) return;
  const DetectionRun snapshot = *run;
  if (!session_.activateFile(snapshot.request.sourceIdentity.fileId)) {
    emit message("任务来源文件不在当前工程中");
    return;
  }
  const auto* file = session_.activeFile();
  if (!file || file->metadata.id != snapshot.request.sourceIdentity.fileId) {
    emit message("派生运行需要先打开其源文件；当前配置草稿保持");
    return;
  }
  const bool present = std::any_of(
      file->detectionRuns.begin(), file->detectionRuns.end(),
      [&](const DetectionRun& existing) {
        return existing.request.runId == snapshot.request.runId;
      });
  if (!present) {
    emit message("派生运行的来源任务已不在该文件中；当前配置草稿保持");
    return;
  }
  const int row = model_->rowForId(runId);
  if (row >= 0) list_->setCurrentIndex(model_->index(row));
  emit changed();
  configure(&snapshot);
  emit runFocused(runId);
}
void DetectionPanel::cancelSelected() {
  if (const auto* run = selectedRun()) cancelRun(q(run->request.runId));
}
void DetectionPanel::cancelRun(const QString& runId) {
  const auto* run = findRun(runId);
  if (!run || detectionIsTerminal(run->lifecycle)) return;
  // Keep the target independent of current selection and active-file changes.
  const auto id = run->request.runId;
  tasks_->cancel(id);
  poll();
}
void DetectionPanel::showSelectedDetails() {
  if (const auto* run = selectedRun()) showRunDetails(q(run->request.runId));
}
void DetectionPanel::showRunDetails(const QString& runId) {
  const auto* run = findRun(runId);
  if (!run) return;
  QDialog dialog(this);
  dialog.setWindowTitle("检测运行快照 / 覆盖 / 分片");
  dialog.resize(860, 620);
  auto* layout = new QVBoxLayout(&dialog);
  auto* facts = new QTextBrowser;
  facts->setObjectName("detectionRunFacts");
  std::uint64_t published = 0;
  for (const auto& file : session_.project().files)
    if (file.metadata.id == run->request.sourceIdentity.fileId)
      published = publishedCount(file, *run);
  facts->setPlainText(runFacts(*run, published));
  layout->addWidget(facts, 1);
  auto* technical = new QToolButton;
  technical->setObjectName("detectionRunTechnicalToggle");
  technical->setText("原始技术 JSON");
  technical->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  technical->setArrowType(Qt::RightArrow);
  technical->setCheckable(true);
  layout->addWidget(technical);
  auto* text = new QPlainTextEdit;
  text->setObjectName("detectionRunTechnicalJson");
  text->setReadOnly(true);
  text->setPlainText(QString::fromUtf8(
      QJsonDocument(encodeDetectionRun(*run)).toJson(QJsonDocument::Indented)));
  text->hide();
  layout->addWidget(text, 1);
  connect(technical, &QToolButton::toggled, &dialog,
          [technical, text](bool expanded) {
            technical->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            text->setVisible(expanded);
          });
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.exec();
}
void DetectionPanel::discoverRecoverable() {
  auto* file = session_.activeFile();
  if (!file || projectDirectory_().isEmpty()) return;
  QString error;
  const auto runs = discoverDetectionRuns(projectDirectory_(), session_.project(), error);
  if (!error.isEmpty()) emit message(error);
  std::vector<DetectionRun> missing;
  for (const auto& run : runs) {
    const auto* slot = currentDetectionSlot(*file, run.request.sourceIdentity.selectedSampleChannel);
    if (run.request.sourceIdentity.fileId == file->metadata.id && slot &&
        slot->pendingRunId == run.request.runId && run.lifecycle == DetectionLifecycle::Completed)
      missing.push_back(run);
  }
  if (missing.empty()) {
    emit message("没有当前文件可恢复的未引用运行");
    return;
  }
  if (QMessageBox::question(
          this, "恢复运行索引",
          QString(
              "发现 %1 "
              "个权威 pending slot 中已完成的运行。原子保存并整体替换对应样本通道的"
              "正式结果；未完成、取消和退役运行不能恢复。是否恢复？")
              .arg(missing.size())) != QMessageBox::Yes)
    return;
  for (const auto& run : missing) {
    const auto sourcePath =
        QString::fromStdString(run.request.sourceIdentity.path);
    if (!QFileInfo::exists(sourcePath) ||
        iqSourceFingerprint(sourcePath).toStdString() !=
            run.request.sourceIdentity.quickFingerprint) {
      emit message("恢复前来源指纹不一致，请重新核验原始数据");
      continue;
    }
    DetectionPublication publication;
    DetectionResultTicket ticket;
    if (!DetectionResultService::prepareRecovery(session_.project(), run,
        session_.projectGeneration(), ticket, error)) { emit message(error); continue; }
    if (!recoverDetectionPublication(projectDirectory_(), run, publication,
                                     error)) {
      emit message(error);
      continue;
    }
    const auto path = projectFilePath_ ? projectFilePath_() : QString{};
    const auto published = DetectionResultService::replaceCurrentDetection(
        session_.project(), session_.annotations(), ticket,
        session_.projectGeneration(), run, std::move(publication), path);
    if (!published) {
      emit message(published.error);
      continue;
    }
    scheduleGc(path);
  }
  refresh();
  emit changed();
}
}  // namespace signalstudio
