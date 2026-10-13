#include "ui/detection/detection_panel.h"

#include <QAbstractListModel>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QLabel>
#include <QListView>
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
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <deque>

#include "infrastructure/detection_store.h"
#include "infrastructure/source_loader.h"

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
    QString sourceName, error;
    DetectionLifecycle lifecycle = DetectionLifecycle::Queued;
    DetectionSourceValidity validity = DetectionSourceValidity::Current;
    std::uint64_t read = 0, completed = 0, eligible = 0, results = 0,
                  published = 0, started = 0, finished = 0;
    std::size_t coverageCount = 0, chunkCount = 0;
    bool recoverable = false;
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
  return {q(file.metadata.name),
          q(run.error),
          run.lifecycle,
          run.sourceValidity,
          run.readFrames,
          run.completedWindows,
          run.eligibleWindows,
          run.resultCount,
          published,
          run.startedAtMilliseconds,
          run.finishedAtMilliseconds,
          run.coverage.size(),
          run.chunks.size(),
          run.recoverable};
}
RunCard makeCard(const FileState& file, const DetectionRun& run,
                 std::uint64_t published) {
  const auto& request = run.request;
  RunCard card;
  card.signature = cardSignature(file, run, published);
  card.id = q(request.runId);
  card.title = stateName(run.lifecycle) + (run.recoverable ? " / 可恢复" : "") +
               " · " + shortRunId(request.runId);
  card.sourcePath = q(request.sourceIdentity.path);
  card.cancellable = !detectionIsTerminal(run.lifecycle);
  card.facts = {
      QString("源文件：%1 · Fs %2 MS/s")
          .arg(q(file.metadata.name),
               number(request.sourceIdentity.sampleRateHz / 1e6)),
      QString("冻结 %1 · %2 / %3")
          .arg(rangeText(request.requestedRange,
                         request.sourceIdentity.sampleRateHz),
               q(request.profile.name),
               QString::fromLatin1(
                   detectionTimeStrategyName(request.parameters.timeStrategy))),
      QString("已读谱帧 %1 · unique windows %2 / %3 · 工程已发布 %4 个对象")
          .arg(run.readFrames)
          .arg(run.completedWindows)
          .arg(run.eligibleWindows)
          .arg(published),
      "已覆盖：" + coverageSummary(run, DetectionCoverageKind::Completed),
      "未覆盖：未读 " + coverageSummary(run, DetectionCoverageKind::Unread) +
          "；排除 " + coverageSummary(run, DetectionCoverageKind::Excluded) +
          "。已覆盖范围以外没有完整结论。"};
  if (run.sourceValidity != DetectionSourceValidity::Current)
    card.facts.push_back(sourceValidityText(run.sourceValidity));
  if (!run.error.empty()) {
    const auto error = q(run.error).simplified();
    card.error = "错误：" + error.left(180) + (error.size() > 180 ? "…" : "");
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
      QString(
          "工程已发布 %1 个来源对象 · 持久化 %2 个窗口 DTO · %3 个不可变分片")
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
    if (role == Qt::ToolTipRole) return value->id + "\n" + value->sourcePath;
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
  RunCardDelegate(QListView* view, std::function<void(const QString&)> open,
                  std::function<void(const QString&)> cancel)
      : QStyledItemDelegate(view),
        view_(view),
        open_(std::move(open)),
        cancel_(std::move(cancel)) {}
  QSize sizeHint(const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override {
    const auto* card = value(index);
    if (!card) return {};
    QTextDocument document;
    prepareDocument(document, *card, option.font, view_->viewport()->width());
    return {std::max(1, view_->viewport()->width()),
            static_cast<int>(std::ceil(document.size().height())) + 66};
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
    const auto actions = actionRects(option.rect, card->cancellable);
    paintButton(painter, actions.first, "打开源文件任务");
    if (card->cancellable) paintButton(painter, actions.second, "取消此任务");
    painter->restore();
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
    const auto actions = actionRects(option.rect, card->cancellable);
    const int action = actions.first.contains(mouse->position().toPoint()) ? 0
                       : card->cancellable && actions.second.contains(
                                                  mouse->position().toPoint())
                           ? 1
                           : -1;
    if (event->type() == QEvent::MouseButtonPress) {
      pressed_ = index;
      pressedAction_ = action;
      return action >= 0;
    }
    const bool activate =
        pressed_ == index && action >= 0 && action == pressedAction_;
    pressed_ = QPersistentModelIndex{};
    pressedAction_ = -1;
    if (!activate) return false;
    const auto id = card->id;
    if (action == 0)
      open_(id);
    else
      cancel_(id);
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
  static std::pair<QRect, QRect> actionRects(const QRect& rect, bool cancel) {
    const int available = std::max(1, rect.width() - 24);
    const int openWidth = std::min(160, cancel ? available / 2 : available);
    const QRect open(rect.left() + 12, rect.bottom() - 41, openWidth, 29);
    const QRect close(open.right() + 9, open.top(),
                      std::min(140, std::max(1, available - openWidth - 8)),
                      29);
    return {open, close};
  }
  void paintButton(QPainter* painter, const QRect& rect,
                   const QString& text) const {
    QStyleOptionButton button;
    button.initFrom(view_);
    button.rect = rect;
    button.text = text;
    button.state |= QStyle::State_Enabled;
    view_->style()->drawControl(QStyle::CE_PushButton, &button, painter, view_);
  }
  QListView* view_;
  std::function<void(const QString&)> open_, cancel_;
  QPersistentModelIndex pressed_;
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
  commands->addWidget(details_);
  connect(details_, &QPushButton::clicked, this,
          &DetectionPanel::showSelectedDetails);
  derive_ = new QPushButton("派生新运行");
  derive_->setObjectName("deriveWidebandDetection");
  commands->addWidget(derive_);
  connect(derive_, &QPushButton::clicked, this, [this] {
    if (const auto* run = selectedRun()) openRunSource(q(run->request.runId));
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
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setResizeMode(QListView::Adjust);
  list_->setLayoutMode(QListView::Batched);
  list_->setBatchSize(32);
  list_->setMouseTracking(true);
  model_ = new DetectionRunListModel(list_);
  list_->setModel(model_);
  list_->setItemDelegate(new RunCardDelegate(
      list_, [this](const QString& id) { openRunSource(id); },
      [this](const QString& id) {
        const int row = model_->rowForId(id);
        if (row < 0) return;
        list_->setCurrentIndex(model_->index(row));
        cancelSelected();
      }));
  connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this] { updateSelectionActions(); });
  connect(list_, &QListView::activated, this, [this](const QModelIndex& index) {
    openRunSource(index.data(Qt::UserRole).toString());
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
  if (!tasks_->enqueue(projectDirectory_(), file->metadata, std::move(request),
                       error))
    return false;
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
  }
  tasks_->pump();
  bool changedState = false;
  for (auto& run : tasks_->snapshots()) {
    if (run.request.sourceIdentity.projectGeneration != generation_) continue;
    auto file = std::find_if(
        session_.project().files.begin(), session_.project().files.end(),
        [&](const auto& value) {
          return value.metadata.id == run.request.sourceIdentity.fileId;
        });
    if (file == session_.project().files.end()) {
      tasks_->invalidateSource(run.request.runId,
                               DetectionSourceValidity::Missing);
      continue;
    }
    const auto& source = run.request.sourceIdentity;
    if (!QFileInfo::exists(q(source.path))) {
      run.sourceValidity = DetectionSourceValidity::Missing;
    } else if (file->metadata.path != source.path ||
               file->metadata.sampleFormat != source.sampleFormat ||
               file->metadata.sampleRateHz != source.sampleRateHz ||
               file->metadata.centerFrequencyHz != source.centerFrequencyHz ||
               iqSourceFingerprint(q(source.path)).toStdString() !=
                   source.quickFingerprint) {
      run.sourceValidity = DetectionSourceValidity::Changed;
    }
    if (run.sourceValidity != DetectionSourceValidity::Current)
      tasks_->invalidateSource(run.request.runId, run.sourceValidity);
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
      if (run.sourceValidity != DetectionSourceValidity::Current) {
        emit message("来源已变化：已落盘证据保留，重新核验来源后再恢复标记");
        changedState = true;
        continue;
      }
      const auto result = session_.annotations().publishSourceSet(
          *file, std::move(publication.sourceSet),
          std::move(publication.annotations));
      emit message(result.ok ? "检测已落盘，结果来源集合已发布；请复核活动段"
                             : q(result.error));
      changedState = true;
    }
  }
  if (refreshRunList(changedState)) emit runProgressChanged();
  if (changedState) emit changed();
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
  details_->setEnabled(run);
  derive_->setEnabled(run && !projectDirectory_().isEmpty());
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
  configure();
  emit runFocused(runId);
}
void DetectionPanel::cancelSelected() {
  if (const auto* run = selectedRun();
      run && !detectionIsTerminal(run->lifecycle)) {
    tasks_->cancel(run->request.runId);
    poll();
  }
}
void DetectionPanel::showSelectedDetails() {
  const auto* run = selectedRun();
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
  const auto runs = discoverDetectionRuns(projectDirectory_(), error);
  if (!error.isEmpty()) emit message(error);
  std::vector<DetectionRun> missing;
  for (const auto& run : runs)
    if (run.request.sourceIdentity.fileId == file->metadata.id &&
        std::none_of(file->detectionRuns.begin(), file->detectionRuns.end(),
                     [&](const auto& value) {
                       return value.request.runId == run.request.runId;
                     }))
      missing.push_back(run);
  if (missing.empty()) {
    emit message("没有当前文件可恢复的未引用运行");
    return;
  }
  if (QMessageBox::question(
          this, "恢复运行索引",
          QString(
              "发现 %1 "
              "个已校验的未引用运行。恢复运行和已完成窗口的来源集合；不续算、"
              "不改变已有修订。是否恢复？")
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
    if (!recoverDetectionPublication(projectDirectory_(), run, publication,
                                     error)) {
      emit message(error);
      continue;
    }
    const auto published = session_.annotations().publishSourceSet(
        *file, std::move(publication.sourceSet),
        std::move(publication.annotations));
    if (!published.ok) {
      emit message(q(published.error));
      continue;
    }
    file->detectionRuns.push_back(run);
  }
  refresh();
  emit changed();
}
}  // namespace signalstudio
