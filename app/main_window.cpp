#include "app/main_window.h"
#include "ui/chart_control_strip.h"
#include "ui/context_stack.h"
#include "ui/settings_popover.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "infrastructure/channel_processor.h"
#include "infrastructure/int16_iq_file.h"
#include "infrastructure/project_store.h"
#include "infrastructure/source_loader.h"
#include "infrastructure/spectral_settings_json.h"
#include "infrastructure/wav_metadata.h"
#include "ui/annotations/annotation_evidence_widget.h"
#include "ui/annotations/annotation_panel.h"
#include "ui/brand/brand_assets.h"
#include "ui/charts/plot_widget.h"
#include "ui/checkbox_style.h"
#include "ui/controls/panel_resize_handle.h"
#include "ui/controls/sample_time_edit.h"
#include "ui/custom_combo.h"
#include "ui/detection/detection_card.h"
#include "ui/detection/detection_panel.h"
#include "ui/import/signal_import_dialog.h"
#include "ui/narrowband_workspace.h"
#include "ui/parameter_input.h"
#include "ui/source_units.h"
#include "ui/spectral_settings_widget.h"

namespace signalstudio {
namespace {
constexpr std::array<const char*, 5> kDetectionFolds{
    "detectionAnalysisParametersToggle", "detectionPostprocessParametersToggle",
    "detectionResourceParametersToggle", "detectionCalculationDetailsToggle",
    "detectionDiagnosticsToggle"};
QString q(const std::string& text) { return QString::fromStdString(text); }
QString annotationCountText(const FileState& file) {
  const auto counts = annotationCounts(file);
  return QString("匹配 %1 / 总数 %2 / 已选 %3")
      .arg(counts.matching)
      .arg(counts.total)
      .arg(counts.selected);
}
QColor annotationSourceColor(AnnotationSource source) {
  return source == AnnotationSource::ImportedLabel ? QColor("#65d2bd")
         : source == AnnotationSource::Manual      ? QColor("#edc56d")
                                                   : QColor("#51c4e9");
}
class AnnotationTreeDelegate final : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    if (index.data(Qt::UserRole).toString() != "mark") {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }
    auto style = option;
    initStyleOption(&style, index);
    style.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus |
                     QStyle::State_MouseOver);
    const auto* widgetStyle =
        style.widget ? style.widget->style() : QApplication::style();
    const auto textRect = widgetStyle->subElementRect(
        QStyle::SE_ItemViewItemText, &style, style.widget);
    const auto text = style.fontMetrics.elidedText(
        style.text, style.textElideMode, textRect.width());
    style.text.clear();
    widgetStyle->drawControl(QStyle::CE_ItemViewItem, &style, painter,
                             style.widget);
    // The application item QSS overrides ForegroundRole during styled text
    // painting. Draw mark text explicitly so every state keeps its source
    // color.
    painter->save();
    painter->setFont(style.font);
    painter->setPen(index.data(Qt::ForegroundRole).value<QBrush>().color());
    painter->drawText(
        textRect, static_cast<int>(style.displayAlignment) | Qt::TextSingleLine,
        text);
    painter->restore();
    const auto state = index.data(Qt::UserRole + 3).toInt();
    if (!(state & 7)) return;
    painter->save();
    QPen pen(state & 2   ? QColor("#84daf5")
             : state & 1 ? QColor("#37647d")
                         : QColor("#c0ecff"),
             state & 2 ? 2 : 1);
    if (!(state & 3)) pen.setStyle(Qt::DotLine);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
    if ((state & 4) && (state & 3)) {
      painter->setPen(QPen(QColor("#d4f2ff"), 1, Qt::DotLine));
      painter->drawRect(option.rect.adjusted(3, 3, -4, -4));
    }
    painter->restore();
  }
};
bool hasActiveSegment(const FileState& file) {
  const auto* annotation = findMark(file, file.activeMarkId);
  return annotation && !annotation->deleted && activeSegment(*annotation);
}
QString number(double value, int precision = 9) {
  return QString::number(value, 'g', precision);
}
QString coordinate(double value, double span, bool time) {
  const double width = std::abs(span);
  const double chosen = width > 0 && std::abs(value) < width * 1000
                            ? width
                            : std::max(std::abs(value), width);
  const double scale = time ? (chosen >= 1      ? 1
                               : chosen >= .001 ? 1000
                                                : 1e6)
                            : (chosen >= 1e6    ? 1e-6
                               : chosen >= 1000 ? .001
                                                : 1);
  const auto unit = time ? (chosen >= 1      ? "s"
                            : chosen >= .001 ? "ms"
                                             : "us")
                         : (chosen >= 1e6    ? "MHz"
                            : chosen >= 1000 ? "kHz"
                                             : "Hz");
  return number(value * scale, 11) + " " + unit;
}
QString timeRange(const TimeRange& range, double fs) {
  const double span = static_cast<double>(range.end - range.begin) / fs;
  return coordinate(static_cast<double>(range.begin) / fs, span, true)
             .section(' ', 0, 0) +
         "–" + coordinate(static_cast<double>(range.end) / fs, span, true);
}
QString frequencyRange(const FrequencyRange& range) {
  return coordinate(range.lowerHz, range.upperHz - range.lowerHz, false)
             .section(' ', 0, 0) +
         "–" + coordinate(range.upperHz, range.upperHz - range.lowerHz, false);
}
QString sidebarSettingsKey(const FileMetadata& metadata) {
  if (metadata.demo || metadata.path.empty()) return {};
  const auto sourcePath = QString::fromUtf8(
      metadata.path.data(), static_cast<qsizetype>(metadata.path.size()));
  const auto normalizedPath =
      QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath()).toCaseFolded();
  return QString::fromLatin1(
      QCryptographicHash::hash(normalizedPath.toUtf8(),
                               QCryptographicHash::Sha256)
          .toHex());
}
QString channelRelationLabel(ChannelSourceRelation relation) {
  switch (relation) {
    case ChannelSourceRelation::Current:
      return "来源当前";
    case ChannelSourceRelation::Changed:
      return "来源已修改；保留创建快照";
    case ChannelSourceRelation::Missing:
      return "来源已删除或缺失";
    case ChannelSourceRelation::Rejected:
      return "来源质量：已忽略";
    case ChannelSourceRelation::Unconfirmed:
      return "旧版段确认记录；保留创建快照";
    case ChannelSourceRelation::LegacyNeedsReview:
      return "旧版来源待复核";
  }
  return "来源待复核";
}
QString sidebarSourcePath(const FileMetadata& metadata) {
  if (metadata.demo || metadata.path.empty()) return {};
  const auto sourcePath = QString::fromUtf8(
      metadata.path.data(), static_cast<qsizetype>(metadata.path.size()));
  return QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath())
      .toCaseFolded();
}
QPushButton* push(const QString& text, const QString& id, QLayout* layout) {
  auto* widget = new QPushButton(text);
  widget->setObjectName(id);
  widget->setCursor(Qt::PointingHandCursor);
  layout->addWidget(widget);
  return widget;
}
QLabel* label(const QString& text, const QString& id = {},
              const QString& role = {}) {
  auto* widget = new QLabel(text);
  widget->setTextFormat(Qt::PlainText);
  if (!id.isEmpty()) widget->setObjectName(id);
  if (!role.isEmpty()) widget->setProperty("uiRole", role);
  return widget;
}
QLabel* output(const QString& id) {
  auto* widget = label("—", id, "output");
  widget->setWordWrap(true);
  widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
  widget->setMinimumWidth(0);
  return widget;
}
QComboBox* combo(const QString& id, const QStringList& items) {
  auto* widget = new QComboBox;
  widget->setObjectName(id);
  widget->addItems(items);
  widget->setMinimumWidth(0);
  widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  widget->setFixedHeight(28);
  widget->setCursor(Qt::PointingHandCursor);
  return widget;
}
void row(QFormLayout* form, const QString& title, QWidget* field) {
  auto* name = label(title, {}, "settingLabel");
  name->setMinimumWidth(80);
  form->addRow(name, field);
}
QTreeWidgetItem* childOfKind(QTreeWidgetItem* item, const QString& kind) {
  if (!item) return nullptr;
  for (int i = 0; i < item->childCount(); ++i)
    if (item->child(i)->data(0, Qt::UserRole).toString() == kind)
      return item->child(i);
  return nullptr;
}
class PrototypeSplitter;
class PrototypeHandle : public QSplitterHandle {
 public:
  PrototypeHandle(Qt::Orientation orientation, QSplitter* parent)
      : QSplitterHandle(orientation, parent) {
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::SplitVCursor);
  }
  std::function<void()> restoreDefaults;
  bool isDragging() const { return !base_.isEmpty(); }
  void cancel() {
    if (!base_.isEmpty()) {
      splitter()->setSizes(base_);
      base_.clear();
      releaseMouse();
    }
  }

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    base_ = splitter()->sizes();
    QSplitterHandle::mousePressEvent(event);
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    QSplitterHandle::mouseReleaseEvent(event);
    base_.clear();
  }
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    cancel();
    if (restoreDefaults) restoreDefaults();
    event->accept();
  }
  void keyPressEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_Escape) {
      cancel();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
      auto sizes = splitter()->sizes();
      int index = 1;
      while (index < splitter()->count() && splitter()->handle(index) != this)
        ++index;
      if (index < sizes.size()) {
        const int delta = event->key() == Qt::Key_Up ? -10 : 10;
        sizes[index - 1] += delta;
        sizes[index] -= delta;
        splitter()->setSizes(sizes);
      }
      event->accept();
      return;
    }
    QSplitterHandle::keyPressEvent(event);
  }
  bool event(QEvent* event) override {
    if (event->type() == QEvent::WindowDeactivate) cancel();
    return QSplitterHandle::event(event);
  }

 private:
  QList<int> base_;
};
class PrototypeSplitter : public QSplitter {
 public:
  explicit PrototypeSplitter(QWidget* parent = nullptr)
      : QSplitter(Qt::Vertical, parent) {}
  std::function<void()> restoreDefaults;
  void cancel() {
    for (int i = 1; i < count(); ++i)
      static_cast<PrototypeHandle*>(handle(i))->cancel();
  }
  bool isDragging() const {
    for (int i = 1; i < count(); ++i)
      if (static_cast<PrototypeHandle*>(handle(i))->isDragging()) return true;
    return false;
  }

 protected:
  QSplitterHandle* createHandle() override {
    auto* handle = new PrototypeHandle(orientation(), this);
    handle->restoreDefaults = [this] {
      if (restoreDefaults) restoreDefaults();
    };
    return handle;
  }
};
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  qApp->setWindowIcon(BrandAssets::windowIcon());
  setWindowIcon(BrandAssets::windowIcon());
  setWindowFlag(Qt::FramelessWindowHint);
  setObjectName("SignalStudioWindow");
  setWindowTitle("Signal Studio · A1.4.3");
  resize(1600, 960);
  setMinimumSize(720, 520);
  setStyleSheet(R"(
        QWidget { background:#101d2f; color:#dbe8f8; font-family:'Segoe UI','Microsoft YaHei UI'; font-size:12px; }
        QMainWindow { background:#0b1422; }
        QMenuBar { background:#0b1729; border-bottom:1px solid #28384b; color:#b4c8dc; }
        QMenuBar::item { padding:5px 11px; background:transparent; }
        QMenuBar::item:selected { background:#263d56; }
        QMenu { background:#1b2b41; border:1px solid #56718b; padding:5px; }
        QMenu::item { padding:9px 12px; min-width:200px; } QMenu::item:selected { background:#275475; }
        QMenu::item:disabled { color:#627a91; } QMenu::separator { height:1px; background:#375168; margin:4px; }
        QPushButton,QToolButton { border:1px solid transparent; border-radius:4px; padding:5px 8px; background:transparent; color:#bdd5e9; }
        QPushButton:hover,QToolButton:hover { background:#263d56; }
        QPushButton:disabled,QToolButton:disabled { color:#5b7186; }
        QPushButton:checked { background:#1b5883; color:#cdefff; }
        QPushButton[uiRole="outline"] { border:1px solid #37566c; }
        QPushButton[uiRole="primary"] { background:#1a4e76; border:1px solid #2b4055; color:#bdebff; }
        QPushButton[uiRole="segment"] { font-size:11px; padding:4px 9px; border-radius:2px; }
        QPushButton[uiRole="icon"] { color:#a4c1d8; font-size:15px; padding:0; }
        QComboBox,QDoubleSpinBox,QLineEdit { border:1px solid #314963; background:#0e1a2a; border-radius:4px; padding:5px 7px; font-size:11px; }
        QComboBox:disabled,QDoubleSpinBox:disabled { color:#627a91; }
        QLineEdit[invalid="true"],QComboBox[invalid="true"],QAbstractSpinBox[invalid="true"] { border-color:#e2a05b; }
        QComboBox QAbstractItemView { background:#17263a; selection-background-color:#235476; }
        QTreeWidget { border:0; background:#101d2f; outline:0; }
        QTreeWidget::item { border-radius:4px; padding:0 3px; color:#b2cbe4; }
        QTreeWidget::item:hover { background:#285473; color:#f0fcff; }
        QTreeWidget::item:selected { background:#644f23; color:#fff0c7; border-left:2px solid #ebc966; }
        QSplitter::handle { background:#0d1725; } QSplitter::handle:hover,QSplitter::handle:focus { background:#46a7d0; }
        QScrollArea { border:0; background:#111e2e; } QScrollBar:vertical { background:#111e2e; width:8px; }
        QScrollBar::handle:vertical { background:#35506b; border-radius:4px; min-height:24px; }
        QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
        QLabel { background:transparent; }
        QLabel[uiRole="output"] { color:#d5e7f5; font:11px 'Consolas','Microsoft YaHei UI'; padding:3px 0; }
        QLabel[uiRole="settingLabel"] { font-size:11px; color:#a8bdd3; }
        QLabel[uiRole="hint"] { font-size:10px; color:#819ab2; }
        QLabel[uiRole="help"] { font-size:11px; color:#7895b0; }
        QLabel[uiRole="tag"] { font:10px 'Consolas'; color:#a0bbd3; padding:4px 6px; border:1px solid #324b67; border-radius:3px; background:#0e1d30; }
        QWidget[uiRole="panelHead"] { background:#172740; border-bottom:1px solid #28415d; }
        QWidget[uiRole="chartPanel"] { background:#121f32; border:1px solid #293f56; border-radius:3px; }
        QWidget[uiRole="chartHead"] { background:#16283e; border-bottom:1px solid #273c53; }
        QWidget[uiRole="segmentGroup"] { background:#0c192a; border:1px solid #2b445f; border-radius:4px; }
        QToolButton[uiRole="sectionToggle"] { background:#1b2b40; color:#c5dcef; border:0; border-radius:0; padding:8px 10px; text-align:left; font-weight:600; }
        QToolButton[uiRole="sectionToggle"]:hover { background:#243b55; }
        QToolButton[uiRole="sectionToggle"]:!checked { color:#7f9cba; }
        QWidget[uiRole="section"] { background:#111e2e; border-bottom:1px solid #263b52; }
        #resources { border-right:1px solid #294058; } #properties { background:#111e2e; border-left:1px solid #30455e; }
        #workspace,#graphSplitter { background:#0d1725; }
        #scopeBar { background:#122034; border-bottom:1px solid #223752; }
        #specMeta { background:#121f32; border-top:1px solid #243b54; }
        #bottom { background:#111c2e; border-top:1px solid #30455e; }
        #bottomTabs { background:#18283c; border-bottom:1px solid #2c4056; }
        #bottomTabs QPushButton { padding:4px 15px; color:#9cbbd2; } #bottomTabs QPushButton:checked { color:#d6ebfc; background:#174668; }
        QPlainTextEdit { border:0; background:#111c2e; color:#a8bcd3; font:11px Consolas; }
        QStatusBar { background:#0b1726; border-top:1px solid #203851; color:#91a9c0; font:11px Consolas; }
        QStatusBar::item { border:0; }
        QProgressBar { background:#0b1625; border:0; height:8px; } QProgressBar::chunk { background:#36b7dc; }
    )" + signalstudio::checkboxStyleSheet());
  rightSidebarSaveTimer_ = new QTimer(this);
  rightSidebarSaveTimer_->setSingleShot(true);
  rightSidebarSaveTimer_->setInterval(400);
  connect(rightSidebarSaveTimer_, &QTimer::timeout, this,
          &MainWindow::saveRightSidebarSettings);
  buildMenus();
  buildWorkspace();
  parameterInputPolicy_ = new ParameterInputPolicy(this);
  parameterInputPolicy_->changesContext = [this](QWidget* target,
                                                 QMouseEvent* event) {
    if (!tree_ || !(target == tree_ || tree_->isAncestorOf(target)))
      return false;
    const auto point =
        tree_->viewport()->mapFrom(target, event->position().toPoint());
    const auto index = tree_->indexAt(point);
    const auto type = index.data(Qt::UserRole).toString();
    const auto id = index.data(Qt::UserRole + 1).toString().toStdString();
    if (type == "file")
      return id != session_.project().activeFileId ||
             session_.project().narrowbandWorkspaceOpen;
    if (type == "channel")
      return id != session_.project().activeChannelId ||
             !session_.project().narrowbandWorkspaceOpen;
    if (type == "detectionRun") {
      for (const auto& file : session_.project().files)
        for (const auto& run : file.detectionRuns)
          if (run.request.runId == id)
            return file.metadata.id != session_.project().activeFileId ||
                   session_.project().narrowbandWorkspaceOpen;
    }
    return false;
  };
  restoreUiState();
  sourceLoadTimer_ = new QTimer(this);
  sourceLoadTimer_->setInterval(80);
  connect(sourceLoadTimer_, &QTimer::timeout, this,
          &MainWindow::pollSourceLoads);
  sourceLoadTimer_->start();
  connect(qApp, &QApplication::applicationStateChanged, this,
          [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) cancelInteractions();
          });
  refresh();
  QTimer::singleShot(0, this,
                     [this] { enforceLayout(!splitterStateRestored_); });
  log("A1.4.3 工作区已就绪；可导入交替 int16 IQ 文件并生成真实图谱");
}
MainWindow::~MainWindow() {
  delete parameterInputPolicy_;
  parameterInputPolicy_ = nullptr;
  if (sourceLoadTimer_) sourceLoadTimer_->stop();
  sourceLoads_.clear();
  saveUiState();
  if (rightSidebarSaveTimer_) rightSidebarSaveTimer_->stop();
  saveRightSidebarSettings();
  destroying_ = true;
  qApp->removeEventFilter(this);
  disconnect(qApp, nullptr, this, nullptr);
  for (auto* plot : {navigation_, auxiliary_, main_})
    disconnect(plot, nullptr, this, nullptr);
  cancelInteractions();
  // Charts must end while the session member they reference is still alive.
  delete maxHost_;
  maxHost_ = nullptr;
  delete maxVeil_;
  maxVeil_ = nullptr;
  delete takeCentralWidget();
}

void MainWindow::buildMenus() {
  menuBar()->setFixedHeight(34);
  menuBar()->setNativeMenuBar(false);
  menuBar()->setMouseTracking(true);
  auto* controls = new QWidget(menuBar());
  controls->setObjectName("windowControls");
  auto* controlsRow = new QHBoxLayout(controls);
  controlsRow->setContentsMargins(0, 0, 0, 0);
  controlsRow->setSpacing(0);
  const auto button = [&](const char* id, const QString& text,
                          const QString& tip) {
    auto* control = new QToolButton(controls);
    control->setObjectName(id);
    control->setText(text);
    control->setToolTip(tip);
    control->setFixedSize(44, 33);
    control->setFocusPolicy(Qt::NoFocus);
    control->setStyleSheet(
        "QToolButton { border:0; border-radius:0; padding:0; font-size:18px; } "
        "QToolButton:hover { background:#29445f; }");
    controlsRow->addWidget(control);
    return control;
  };
  connect(button("windowMinimize", "−", "最小化"), &QToolButton::clicked, this,
          [this] {
            cancelInteractions(false);
            showMinimized();
          });
  windowMaximize_ = button("windowMaximize", "□", "最大化");
  connect(windowMaximize_, &QToolButton::clicked, this,
          &MainWindow::toggleWindowState);
  auto* close = button("windowClose", "×", "关闭");
  close->setStyleSheet(
      "QToolButton { border:0; border-radius:0; padding:0; font-size:20px; } "
      "QToolButton:hover { background:#bd3547; color:white; }");
  connect(close, &QToolButton::clicked, this, &QWidget::close);
  menuBar()->setCornerWidget(controls, Qt::TopRightCorner);
  auto* brand = new QWidget;
  auto* brandRow = new QHBoxLayout(brand);
  brandRow->setContentsMargins(11, 0, 22, 0);
  brandRow->setSpacing(8);
  brandRow->addWidget(BrandAssets::label("icon", QSize(24, 24), brand,
                                         "titleBrandIcon", "primary"));
  brandRow->addWidget(BrandAssets::label("wordmark", QSize(132, 24), brand,
                                         "titleBrandWordmark"));
  auto* version = label("A1.4.3 · 交互可靠性优化");
  version->setStyleSheet(
      "font-size:10px;color:#edc586;border:1px solid #836d44;padding:3px "
      "6px;border-radius:3px;");
  brandRow->addWidget(version);
  menuBar()->setCornerWidget(brand, Qt::TopLeftCorner);
  auto* file = menuBar()->addMenu("文件(&F)");
  file->menuAction()->setProperty("preserveParameterDrafts", true);
  auto* action =
      file->addAction("▧ 新建工程…", this, &MainWindow::showNewProjectDialog);
  action->setObjectName("newProjectAction");
  action->setShortcut(QKeySequence::New);
  action = file->addAction("⇧ 打开工程文件夹…", this, [this] {
    const auto path = QFileDialog::getExistingDirectory(
        this, "打开工程文件夹",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (!path.isEmpty()) openProject(path);
  });
  action->setObjectName("openProjectAction");
  action->setShortcut(QKeySequence::Open);
  action = file->addAction("打开旧版工程 JSON…", this, [this] {
    const auto path = QFileDialog::getOpenFileName(
        this, "打开旧版工程 JSON", {}, "Signal Studio 工程 (*.json)");
    if (!path.isEmpty()) openProject(path);
  });
  action->setObjectName("openProjectJsonAction");
  action =
      file->addAction("打开宽带演示工程", this, &MainWindow::openDemoProject);
  action->setObjectName("openDemoProjectAction");
  action = file->addAction("打开窄带演示工程", this,
                           &MainWindow::openNarrowbandDemoProject);
  action->setObjectName("openNarrowbandDemoProjectAction");
  file->addSeparator();
  addSignalAction_ = file->addAction("▣ 添加 / 打开信号…", this,
                                     &MainWindow::showAddFileDialog);
  addSignalAction_->setObjectName("openIqAction");
  saveAction_ = file->addAction("⇩ 保存工程", this, [this] {
    if (!projectPath_.isEmpty()) {
      saveProject(projectPath_);
      return;
    }
    const auto path = QFileDialog::getSaveFileName(
        this, "保存工程 JSON", "signal-studio-project.json",
        "Signal Studio 工程 (*.json)");
    if (!path.isEmpty()) saveProject(path);
  });
  saveAction_->setObjectName("saveProjectAction");
  saveAction_->setShortcut(QKeySequence::Save);
  recentProjectsMenu_ = file->addMenu("最近打开的工程");
  recentProjectsMenu_->setObjectName("recentProjectsMenu");
  updateRecentProjectsMenu();
  removeAction_ = file->addAction("⊖ 从工程移除当前文件", this, [this] {
    cancelInteractions();
    const auto* f = session_.activeFile();
    if (!f) return;
    QString prompt =
        "从工程移除文件“" + q(f->metadata.name) + "”？源文件本身不会删除。";
    if (!f->channels.empty()) {
      QStringList names;
      for (const auto& channel : f->channels) names << q(channel.name);
      prompt += "\n\n以下窄带通道也会移除：\n" + names.join("\n");
    }
    if (QMessageBox::question(this, "移除文件及关联通道", prompt) !=
        QMessageBox::Yes)
      return;
    session_.removeActiveFile();
    if (!session_.activeFile() && maximizedPanel_ >= 0)
      toggleMaximized(maximizedPanel_);
    refresh();
    log("已从工程移除 IQ 文件");
  });
  removeAction_->setObjectName("removeFileAction");
  // Stable automation entry point; the user-facing file action opens the
  // metadata dialog.
  auto* addDemo = new QAction(this);
  addDemo->setObjectName("addDemoAction");
  connect(addDemo, &QAction::triggered, this, [this] {
    cancelInteractions();
    const auto* active = session_.activeFile();
    const DisplaySettings fallback =
        active ? active->display : DisplaySettings{};
    session_.addDemoFile();
    applyGlobalRightSidebarSettings(fallback);
    refresh();
    scheduleRightSidebarSettingsSave();
  });
  auto* edit = menuBar()->addMenu("编辑(&E)");
  edit->menuAction()->setProperty("preserveParameterDrafts", true);
  for (bool redo : {false, true}) {
    auto* history = edit->addAction(redo ? "重做标记命令" : "撤销标记命令");
    history->setObjectName(redo ? "redoAnnotationAction"
                                : "undoAnnotationAction");
    history->setShortcut(redo ? QKeySequence::Redo : QKeySequence::Undo);
    connect(history, &QAction::triggered, this, [this, redo] {
      if (auto* input = qobject_cast<QLineEdit*>(QApplication::focusWidget());
          input && !input->isReadOnly()) {
        if (redo)
          input->redo();
        else
          input->undo();
        return;
      }
      if (auto* file = session_.activeFile()) {
        const auto result = redo ? session_.annotations().redo(*file)
                                 : session_.annotations().undo(*file);
        if (!result.ok) log(q(result.error));
        refresh();
      }
    });
  }
  deleteAction_ =
      edit->addAction("删除所选标记", this, &MainWindow::deleteMarks);
  deleteAction_->setObjectName("deleteMarksAction");
  deleteAction_->setShortcut(QKeySequence::Delete);
  edit->addAction("重命名当前标记", this, &MainWindow::renameMark);
  auto* view = menuBar()->addMenu("视图(&V)");
  backAction_ = view->addAction("视图后退", this, [this] {
    cancelInteractions();
    session_.back();
    propagateGlobalRightSidebarSettings();
    refresh();
    scheduleRightSidebarSettingsSave();
  });
  backAction_->setObjectName("backAction");
  backAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
  forwardAction_ = view->addAction("视图前进", this, [this] {
    cancelInteractions();
    session_.forward();
    propagateGlobalRightSidebarSettings();
    refresh();
    scheduleRightSidebarSettingsSave();
  });
  forwardAction_->setObjectName("forwardAction");
  forwardAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
  view->addAction("适应全部数据", this, [this] {
    cancelInteractions();
    if (const auto* f = session_.activeFile())
      session_.setView(fullRange(f->metadata));
    refresh();
  });
  view->addAction("恢复默认视图", this, [this] {
    cancelInteractions();
    session_.resetView();
    propagateGlobalRightSidebarSettings();
    refresh();
    scheduleRightSidebarSettingsSave();
  });
  auto* analysis = menuBar()->addMenu("分析(&A)");
  detectionAction_ = analysis->addAction("宽带信号检测…", this,
                                         [this] { openWidebandDetection(); });
  detectionAction_->setObjectName("widebandDetectionAction");
  auto* roiAction = analysis->addAction("选择检测 ROI…", this,
                                        &MainWindow::beginDetectionRoi);
  roiAction->setObjectName("widebandDetectionRoiAction");
  auto* about = menuBar()
                    ->addMenu("工具(&T)")
                    ->addAction("关于 Signal Studio", this, [this] {
                      auto* dialog = BrandAssets::aboutDialog(this);
                      dialog->setAttribute(Qt::WA_DeleteOnClose);
                      dialog->open();
                    });
  about->setObjectName("aboutSignalStudioAction");
  auto* help = menuBar()->addMenu("帮助(&H)");
  auto* interactions = help->addAction("鼠标交互帮助");
  interactions->setObjectName("mouseInteractionHelpAction");
  connect(interactions, &QAction::triggered, this, [this] {
    QMessageBox::information(
        this, "鼠标交互帮助",
        "时频图：X 轴为时间，Y 轴为频率；瀑布图：X 轴为频率，Y "
        "轴为时间（向下递增）。\n\n"
        "图内及底部滚轮缩放 X 轴，左侧滚轮缩放 Y "
        "轴；拖动坐标轴平移。右键菜单开启持续选择信号后，拖动创建标记；"
        "已选框可移动并调整边角，Esc 回滚并退出创建模式。\n\n"
        "辅助图可拖动区间放大；波形时间、PSD "
        "频率与主图联动。全局导航支持单击跳转、拖动时间窗和滚轮缩放。"
        "Ctrl / Shift 多选，Delete "
        "删除所选标记。分隔条双击恢复默认高度，方向键每次调整 10 px。");
  });
}

void MainWindow::buildWorkspace() {
  auto* body = new QWidget;
  auto* bodyLayout = new QHBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(0);
  setCentralWidget(body);
  resources_ = new QWidget;
  resources_->setObjectName("resources");
  auto* left = new QVBoxLayout(resources_);
  left->setContentsMargins(0, 0, 0, 0);
  left->setSpacing(0);
  auto* resourceHead = new QWidget;
  resourceHead->setProperty("uiRole", "panelHead");
  resourceHead->setFixedHeight(35);
  auto* resourceHeadRow = new QHBoxLayout(resourceHead);
  resourceHeadRow->setContentsMargins(10, 0, 8, 0);
  resourceTitle_ = label("工程管理");
  resourceTitle_->setStyleSheet("font-weight:600;");
  resourceHeadRow->addWidget(resourceTitle_);
  resourceHeadRow->addStretch();
  auto* resourceToggle = push("☰", "resourceToggle", resourceHeadRow);
  resourceToggle->setProperty("uiRole", "icon");
  resourceToggle->setFixedSize(25, 25);
  resourceToggle->setToolTip("收折 / 展开工程管理");
  left->addWidget(resourceHead);
  connect(resourceToggle, &QPushButton::clicked, this, [this] {
    cancelInteractions(false);
    resourcesCollapsed_ = !resourcesCollapsed_;
    enforceLayout();
    saveUiState();
  });
  resourceTools_ = new QWidget;
  auto* tools = new QHBoxLayout(resourceTools_);
  tools->setContentsMargins(8, 7, 8, 7);
  tools->setSpacing(5);
  auto* add = push("＋ 添加 / 打开信号", "projectAddSignal", tools);
  add->setProperty("uiRole", "primary");
  add->setFixedHeight(28);
  auto* save = push("⇩ 保存工程", "projectSave", tools);
  save->setProperty("uiRole", "outline");
  save->setFixedHeight(28);
  connect(add, &QPushButton::clicked, this, [this] {
    if (addSignalAction_->isEnabled()) addSignalAction_->trigger();
  });
  connect(save, &QPushButton::clicked, saveAction_, &QAction::trigger);
  left->addWidget(resourceTools_);
  auto* resourceScroll = new QScrollArea;
  resourceBody_ = resourceScroll;
  resourceScroll->setObjectName("resourceScroll");
  resourceScroll->setWidgetResizable(true);
  resourceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* resourceInner = new QWidget;
  auto* resourceContent = new QVBoxLayout(resourceInner);
  resourceContent->setContentsMargins(7, 8, 7, 24);
  resourceContent->setSpacing(18);
  resourceScroll->setWidget(resourceInner);
  tree_ = new QTreeWidget;
  tree_->setObjectName("projectTree");
  tree_->setHeaderHidden(true);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->setIndentation(12);
  tree_->setRootIsDecorated(false);
  tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  tree_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  tree_->setMouseTracking(true);
  tree_->setItemDelegate(new AnnotationTreeDelegate(tree_));
  connect(tree_, &QTreeWidget::itemExpanded, this,
          [this] { resizeTreeToContents(); });
  connect(tree_, &QTreeWidget::itemCollapsed, this,
          [this] { resizeTreeToContents(); });
  resourceContent->addWidget(tree_);
  resourceContent->addStretch();
  left->addWidget(resourceBody_, 1);
  bodyLayout->addWidget(resources_);
  leftResize_ = new PanelResizeHandle(Qt::Horizontal);
  leftResize_->setObjectName("leftPanelResize");
  bodyLayout->addWidget(leftResize_);

  auto* workspace = new QWidget;
  workspace->setObjectName("workspace");
  workspace->setMinimumWidth(0);
  auto* center = new QVBoxLayout(workspace);
  center->setContentsMargins(0, 0, 0, 0);
  center->setSpacing(0);
  auto* scopeBar = new QWidget;
  scopeBar->setObjectName("scopeBar");
  scopeBar->setFixedHeight(35);
  auto* scopeRow = new QHBoxLayout(scopeBar);
  scopeRow->setContentsMargins(12, 0, 12, 0);
  scopeRow->setSpacing(13);
  auto* workMode = push("宽带时频研判", "workspaceMode", scopeRow);
  workspaceModeButton_ = workMode;
  workMode->setCheckable(true);
  workMode->setChecked(true);
  workMode->setProperty("uiRole", "primary");
  workMode->setFixedHeight(28);
  projectLabel_ = label({}, "projectLabel", "hint");
  scopeRow->addWidget(projectLabel_);
  scopeRow->addStretch();
  scope_ = label({}, "scopeText", "hint");
  scope_->setStyleSheet("font:11px Consolas;color:#8db5d3;");
  scopeRow->addWidget(scope_);
  widebandDetect_ = push("◇ 检测当前区域…", "widebandDetect", scopeRow);
  widebandDetect_->setProperty("uiRole", "primary");
  connect(widebandDetect_, &QPushButton::clicked, this,
          [this] {
            detectionCard_->useVisibleRange();
            openWidebandDetection();
          });
  center->addWidget(scopeBar);
  auto* graphArea = new QWidget;
  auto* graphLayout = new QVBoxLayout(graphArea);
  graphLayout->setContentsMargins(5, 5, 5, 5);
  graphLayout->setSpacing(0);
  auto* splitter = new PrototypeSplitter;
  graphs_ = splitter;
  graphs_->setObjectName("graphSplitter");
  graphs_->setChildrenCollapsible(false);
  graphs_->setHandleWidth(10);
  splitter->restoreDefaults = [this] {
    cancelInteractions(false);
    enforceLayout(true);
  };
  graphLayout->addWidget(graphs_);
  workspaceStack_ = new QStackedWidget;
  workspaceStack_->setObjectName("workspaceStack");
  workspaceStack_->addWidget(graphArea);
  narrowband_ = new NarrowbandWorkspace(session_);
  workspaceStack_->addWidget(narrowband_);
  connect(narrowband_, &NarrowbandWorkspace::displayParametersChanged, this,
          &MainWindow::refresh);
  narrowband_->setCallbacks([this] { locateActiveChannelSource(); },
                            [this] {
                              const auto* channel = session_.activeChannel();
                              if (channel) showChannelDialog(q(channel->id));
                            },
                            [this] {
                              session_.project().narrowbandWorkspaceOpen =
                                  false;
                              workspaceStack_->setCurrentIndex(0);
                              workspaceModeButton_->setChecked(true);
                              refresh();
                            },
                            [this](const QString& message) { log(message); });
  center->addWidget(workspaceStack_, 1);
  connect(workMode, &QPushButton::clicked, this, [this, workMode] {
    if (workMode->isChecked()) {
      session_.project().narrowbandWorkspaceOpen = false;
      workspaceStack_->setCurrentIndex(0);
    } else if (session_.activeChannel()) {
      session_.project().narrowbandWorkspaceOpen = true;
      workspaceStack_->setCurrentIndex(1);
      narrowband_->refreshFromSession();
    } else {
      workMode->setChecked(true);
      QMessageBox::information(
          this, "窄带工作区",
          "请先从工程树打开一个窄带通道，或从信号标记创建通道。");
    }
    refresh();
  });
  connect(graphs_, &QSplitter::splitterMoved, this, [this] {
    enforceLayout();
    saveUiState();
  });
  navigation_ = new PlotWidget(session_, PlotWidget::Kind::Navigation);
  navigation_->setObjectName("navigationPlot");
  auxiliary_ = new PlotWidget(session_, PlotWidget::Kind::Auxiliary);
  auxiliary_->setObjectName("auxPlot");
  main_ = new PlotWidget(session_, PlotWidget::Kind::Main);
  main_->setObjectName("mainPlot");
  // Segment buttons are the visible controls. Hidden combos preserve a single
  // mode state and automation API.
  mainMode_ = combo("modeMain", {"时频图", "瀑布图"});
  mainMode_->setParent(this);
  mainMode_->hide();
  auxMode_ = combo("modeAux", {"时域波形", "功率谱 (PSD)"});
  auxMode_->setParent(this);
  auxMode_->hide();
  propertyPalette_ = combo("colormap", {"Turbo", "Viridis", "Gray", "Plasma", "Inferno",
                               "Magma", "Cividis", "CoolEdit Classic"});
  propertyPalette_->setFixedSize(155, 24);
  propertyPalette_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  const std::array<QString, 3> panelIds{"navPanel", "auxPanel", "specPanel"},
      names{"全局时间导航", "辅助分析", "宽带图谱"},
      maxIds{"navMaximize", "auxMaximize", "specMaximize"};
  const std::array<PlotWidget*, 3> plots{navigation_, auxiliary_, main_};
  for (int index = 0; index < 3; ++index) {
    auto* panel = new QWidget;
    panel->setObjectName(panelIds[index]);
    panel->setProperty("uiRole", "chartPanel");
    panel->setMinimumWidth(0);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    auto* head = new QWidget;
    head->setProperty("uiRole", "chartHead");
    head->setFixedHeight(32);
    head->installEventFilter(this);
    head->setProperty("chartIndex", index);
    head->setObjectName(index == 0 ? "navChartHead" : index == 1 ? "auxChartHead" : "specChartHead");
    auto* header = new QHBoxLayout(head);
    header->setContentsMargins(9, 0, 9, 0);
    header->setSpacing(7);
    auto* title = label(names[index]);
    title->setStyleSheet("font-weight:600;color:#d5e6f5;");
    header->addWidget(title);
    if (index == 0) {
      auto* hint =
          label("滚轮缩放时间窗 · 拖动蓝色范围 · 单击跳转", "navHint", "hint");
      hint->setMinimumWidth(0);
      header->addWidget(hint);
    } else {
      auto* group = new QWidget;
      group->setProperty("uiRole", "segmentGroup");
      auto* segments = new QHBoxLayout(group);
      segments->setContentsMargins(2, 2, 2, 2);
      segments->setSpacing(2);
      auto& buttons = index == 1 ? auxiliaryButtons_ : mainButtons_;
      const QStringList texts = index == 1
                                    ? QStringList{"时域波形", "功率谱 (PSD)"}
                                    : QStringList{"时频图", "瀑布图"};
      const QStringList ids =
          index == 1 ? QStringList{"waveMode", "psdMode"}
                     : QStringList{"timeFrequencyMode", "waterfallMode"};
      for (int mode = 0; mode < 2; ++mode) {
        buttons[mode] = push(texts[mode], ids[mode], segments);
        buttons[mode]->setProperty("uiRole", "segment");
        buttons[mode]->setCheckable(true);
        buttons[mode]->setFixedHeight(24);
        connect(buttons[mode], &QPushButton::clicked, this,
                [this, index, mode] {
                  if (settingsPopovers_)
                    settingsPopovers_->close(PopoverCloseReason::AnchorHidden);
                  if (index == 1) auxiliaryEvidenceVisible_ = false;
                  (index == 1 ? auxMode_ : mainMode_)->setCurrentIndex(mode);
                  refresh();
                  saveUiState();
                });
      }
      if (index == 1) {
        evidenceMode_ = push("检测证据", "detectionEvidenceMode", segments);
        evidenceMode_->setCheckable(true);
        evidenceMode_->setProperty("uiRole", "segment");
        connect(evidenceMode_, &QPushButton::clicked, this, [this] {
          auxiliaryEvidenceVisible_ = true;
          refresh();
          saveUiState();
        });
      }
      header->addWidget(group);
    }
    if (index == 2) {
      specMode_ = label({}, "specMode");
      specMode_->setStyleSheet(
          "font-size:11px;color:#76c6e8;border:1px solid #2b556a;padding:3px "
          "8px;border-radius:3px;");
      header->addWidget(specMode_);
    }
    header->addStretch();
    if (index == 0) {
      navStatus_ = label({}, "navStatus", "tag");
      header->addWidget(navStatus_);
      stopSourceLoad_ = push("停止", "stopSourceLoad", header);
      stopSourceLoad_->hide();
      connect(stopSourceLoad_, &QPushButton::clicked, this, [this] {
        if (const auto* file = session_.activeFile()) {
          const auto job = sourceLoads_.find(file->metadata.id);
          if (job != sourceLoads_.end()) job->second.loader->stop();
        }
      });
    }
    if (index == 1) {
      auxStatus_ = label({}, "auxStatus", "tag");
      header->addWidget(auxStatus_);
    }
    if (index == 2) {
      specAxis_ = label({}, "specAxisLabel", "hint");
      specAxis_->setMinimumWidth(0);
      specAxis_->setMaximumWidth(150);
      header->addWidget(specAxis_);
      rangeTag_ = label({}, "rangeTag", "tag");
      rangeTag_->setMinimumWidth(0);
      header->addWidget(rangeTag_);
      manualTool_ = push("▱ 持续标记", "manualTool", header);
      manualTool_->setCheckable(true);
      connect(manualTool_, &QPushButton::clicked, this, [this](bool enabled) {
        main_->setCreating(enabled);
        refresh();
      });
      detectionRoiTool_ = push("▨ 检测 ROI", "detectionRoiTool", header);
      connect(detectionRoiTool_, &QPushButton::clicked, this,
              &MainWindow::beginDetectionRoi);
    }
    auto* maximize = push("⤢", maxIds[index], header);
    maximize->setProperty("uiRole", "icon");
    maximize->setFixedSize(25, 25);
    maximize->setToolTip("最大化 / 还原");
    connect(maximize, &QPushButton::clicked, this,
            [this, index] { toggleMaximized(index); });
    layout->addWidget(head);
    if (index == 1) {
      auxiliaryDisplay_ = new QStackedWidget;
      auxiliaryDisplay_->addWidget(plots[index]);
      auto* evidenceScroll = new QScrollArea;
      evidenceScroll->setWidgetResizable(true);
      auxiliaryEvidence_ = new AnnotationEvidenceWidget(
          AnnotationEvidenceWidget::Mode::Auxiliary);
      auxiliaryEvidence_->setObjectName("auxiliaryDetectionEvidence");
      auxiliaryEvidence_->findChild<QPlainTextEdit*>("annotationEvidence")
          ->setObjectName("auxiliaryAnnotationEvidence");
      auxiliaryEvidence_
          ->findChild<QToolButton*>("annotationOriginalFieldsToggle")
          ->setObjectName("auxiliaryOriginalFieldsToggle");
      evidenceScroll->setWidget(auxiliaryEvidence_);
      auxiliaryDisplay_->addWidget(evidenceScroll);
      layout->addWidget(auxiliaryDisplay_, 1);
    } else {
      layout->addWidget(plots[index], 1);
    }
    panels_[index] = panel;
    graphs_->addWidget(panel);
    graphs_->setStretchFactor(index, index == 2 ? 1 : 0);
  }
  for (int i = 1; i < graphs_->count(); ++i) {
    graphs_->handle(i)->setObjectName(i == 1 ? "navSplit" : "auxSplit");
    graphs_->handle(i)->installEventFilter(this);
  }
  auto* meta = new QWidget;
  meta->setObjectName("specMeta");
  meta->setFixedHeight(32);
  auto* metaRow = new QHBoxLayout(meta);
  metaRow->setContentsMargins(12, 0, 12, 0);
  metaRow->setSpacing(7);
  cursorData_ = label({}, "cursorData");
  viewData_ = label({}, "viewData");
  selectionData_ = label({}, "selectionData");
  for (auto* value : {cursorData_, viewData_, selectionData_}) {
    value->setStyleSheet("font:10px Consolas;color:#a7bbd0;");
    metaRow->addWidget(value);
  }
  selectionData_->setStyleSheet("font:10px Consolas;color:#edc676;");
  metaRow->addStretch();
  extract_ = push("基于所选标记创建窄带通道 →", "extract", metaRow);
  extract_->setProperty("uiRole", "primary");
  extract_->setProperty("preserveParameterDrafts", true);
  extract_->setFixedHeight(24);
  connect(extract_, &QPushButton::clicked, this, [this] {
    showChannelDialog();
  });
  static_cast<QVBoxLayout*>(panels_[2]->layout())->addWidget(meta);
  detectionRoiConfirmation_ = new QWidget(main_);
  detectionRoiConfirmation_->setObjectName("detectionRoiConfirmation");
  detectionRoiConfirmation_->setStyleSheet(
      "#detectionRoiConfirmation {background:#2a3029;border:1px solid "
      "#e3bf70;}");
  auto* confirmation = new QHBoxLayout(detectionRoiConfirmation_);
  confirmation->setContentsMargins(12, 5, 12, 5);
  auto* roiRange = label({}, "detectionRoiRange");
  roiRange->setStyleSheet("font:11px Consolas;color:#f1d590;");
  confirmation->addWidget(roiRange);
  confirmation->addStretch();
  auto* confirm = push("确认 · Enter", "confirmDetectionRoi", confirmation);
  confirm->setProperty("uiRole", "primary");
  confirm->setFocusPolicy(Qt::NoFocus);
  connect(confirm, &QPushButton::clicked, main_,
          &PlotWidget::confirmDetectionRoi);
  auto* cancel = push("取消 · Esc", "cancelDetectionRoi", confirmation);
  cancel->setFocusPolicy(Qt::NoFocus);
  connect(cancel, &QPushButton::clicked, main_,
          &PlotWidget::cancelDetectionRoi);
  detectionRoiConfirmation_->resize(650, 40);
  detectionRoiConfirmation_->hide();
  auto* layerBar = new QWidget;
  layerBar->setObjectName("annotationLayerBar");
  layerBar->setFixedHeight(31);
  auto* layerRow = new QHBoxLayout(layerBar);
  layerRow->setContentsMargins(8, 0, 8, 0);
  layerRow->setSpacing(14);
  graphLayout->addWidget(layerBar);

  bottomResize_ = new PanelResizeHandle(Qt::Vertical);
  bottomResize_->setObjectName("bottomPanelResize");
  center->addWidget(bottomResize_);
  bottom_ = new QWidget;
  bottom_->setObjectName("bottom");
  auto* bottomLayout = new QVBoxLayout(bottom_);
  bottomLayout->setContentsMargins(0, 0, 0, 0);
  bottomLayout->setSpacing(0);
  auto* tabs = new QWidget;
  tabs->setObjectName("bottomTabs");
  tabs->setFixedHeight(29);
  auto* tabRow = new QHBoxLayout(tabs);
  tabRow->setContentsMargins(8, 0, 8, 0);
  tabRow->setSpacing(3);
  const std::array<QString, 3> tabNames{"统一标记记录", "后台任务", "日志"},
      tabIds{"resultTab", "taskTab", "logTab"};
  for (int i = 0; i < 3; ++i) {
    bottomButtons_[i] = push(tabNames[i], tabIds[i], tabRow);
    bottomButtons_[i]->setCheckable(true);
    connect(bottomButtons_[i], &QPushButton::clicked, this, [this, i] {
      cancelInteractions(false);
      bottomTab_ = i;
      resultToggle_->setChecked(true);
      updateBottom();
    });
  }
  tabRow->addStretch();
  resultToggle_ = new QToolButton;
  resultToggle_->setObjectName("resultsToggle");
  resultToggle_->setCheckable(true);
  resultToggle_->setText("⌃");
  resultToggle_->setToolTip("展开 / 收起结果");
  resultToggle_->setCursor(Qt::PointingHandCursor);
  tabRow->addWidget(resultToggle_);
  bottomLayout->addWidget(tabs);
  bottomContent_ = new QStackedWidget;
  bottomContent_->setObjectName("bottomContent");
  resultSummary_ = label({}, "resultSummary");
  resultSummary_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  resultSummary_->setMargin(8);
  taskSummary_ = label("无后台任务；全部是演示数据。", "taskSummary");
  taskSummary_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  taskSummary_->setMargin(8);
  results_ = new QPlainTextEdit;
  results_->setObjectName("resultsPanel");
  results_->setReadOnly(true);
  results_->setMaximumBlockCount(300);
  annotations_ = new AnnotationPanel(session_);
  layerRow->addWidget(annotations_->layerWidget());
  connect(annotations_, &AnnotationPanel::changed, this, [this] { refresh(); });
  connect(annotations_, &AnnotationPanel::message, this, &MainWindow::log);
  connect(annotations_, &AnnotationPanel::createChannelRequested, this,
          [this] { showChannelDialog(); });
  connect(annotations_, &AnnotationPanel::hoverChanged, this,
          [this](const QString& id, const QString& segment) {
            if (auto* file = session_.activeFile())
              if (session_.annotations().hover(*file, id.toStdString(),
                                               segment.toStdString()))
                refreshAnnotationHover();
          });
  resultSummary_->setParent(annotations_);
  resultSummary_->hide();
  bottomContent_->addWidget(annotations_);
  detection_ = new DetectionPanel(session_, [this] {
    return projectPath_.isEmpty() ? QString{}
                                  : QFileInfo(projectPath_).absolutePath();
  });
  connect(detection_, &DetectionPanel::changed, this, [this] { refresh(); });
  connect(detection_, &DetectionPanel::message, this, &MainWindow::log);
  connect(detection_, &DetectionPanel::configurationRequested, this,
          &MainWindow::openWidebandDetection);
  connect(detection_, &DetectionPanel::runProgressChanged, this, [this] {
    if (detectionCard_) detectionCard_->refresh();
  });
  connect(detection_, &DetectionPanel::runFocused, this,
          [this](const QString& id) {
            if (detectionCard_) {
              detectionCard_->focusRun(id);
              propertiesCollapsed_ = false;
              showInspectorPage(1, true);
              enforceLayout();
            }
          });
  taskSummary_->setParent(detection_);
  taskSummary_->hide();
  bottomContent_->addWidget(detection_);
  bottomContent_->addWidget(results_);
  bottomLayout->addWidget(bottomContent_);
  center->addWidget(bottom_);
  connect(resultToggle_, &QToolButton::toggled, this, [this] {
    cancelInteractions(false);
    updateBottom();
    enforceLayout();
    saveUiState();
  });
  bodyLayout->addWidget(workspace, 1);
  rightResize_ = new PanelResizeHandle(Qt::Horizontal);
  rightResize_->setObjectName("rightPanelResize");
  bodyLayout->addWidget(rightResize_);
  properties_ = new QWidget;
  properties_->setObjectName("properties");
  auto* right = new QVBoxLayout(properties_);
  right->setContentsMargins(0, 0, 0, 0);
  right->setSpacing(0);
  auto* propHead = new QWidget;
  propHead->setProperty("uiRole", "panelHead");
  propHead->setFixedHeight(35);
  auto* propHeadRow = new QHBoxLayout(propHead);
  propHeadRow->setContentsMargins(10, 0, 8, 0);
  propertiesTitle_ = label("分析与属性");
  propertiesTitle_->setStyleSheet("font-weight:600;");
  propHeadRow->addWidget(propertiesTitle_);
  propHeadRow->addStretch();
  auto* closeProps = push("☷", "propClose", propHeadRow);
  closeProps->setProperty("uiRole", "icon");
  closeProps->setFixedSize(25, 25);
  right->addWidget(propHead);
  connect(closeProps, &QPushButton::clicked, this, [this] {
    cancelInteractions(false);
    propertiesCollapsed_ = true;
    enforceLayout();
    saveUiState();
  });
  propRail_ = new QWidget;
  auto* railLayout = new QVBoxLayout(propRail_);
  railLayout->setContentsMargins(3, 12, 3, 0);
  auto* openProps = push("⚙", "propRailOpen", railLayout);
  openProps->setProperty("uiRole", "icon");
  openProps->setFixedSize(30, 30);
  openProps->setToolTip("展开参数");
  railLayout->addStretch();
  right->addWidget(propRail_, 1);
  connect(openProps, &QPushButton::clicked, this, [this] {
    cancelInteractions(false);
    propertiesCollapsed_ = false;
    enforceLayout();
    saveUiState();
  });
  inspectorTabs_ = new QWidget;
  inspectorTabs_->setObjectName("inspectorTabs");
  auto* inspectorTabsRow = new QHBoxLayout(inspectorTabs_);
  inspectorTabsRow->setContentsMargins(6, 4, 6, 4);
  inspectorTabsRow->setSpacing(3);
  const QStringList inspectorNames{"显示", "检测", "标记", "通道"};
  const QStringList inspectorIds{"contextDisplay", "contextDetection", "contextAnnotation", "contextChannel"};
  for (int index = 0; index < 4; ++index) {
    auto* tab = push(inspectorNames[index], inspectorIds[index], inspectorTabsRow);
    inspectorButtons_[index] = tab;
    tab->setCheckable(true);
    tab->setFixedHeight(28);
    tab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(tab, &QPushButton::clicked, this, [this, index] {
      showInspectorPage(index, true);
      saveUiState();
    });
  }
  right->addWidget(inspectorTabs_);
  propScroll_ = new QScrollArea;
  propScroll_->setObjectName("propScroll");
  propScroll_->setWidgetResizable(true);
  propScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* content = new QWidget;
  content->setObjectName("propContent");
  content->setMinimumWidth(0);
  content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* propertyLayout = new QVBoxLayout(content);
  propertyLayout->setContentsMargins(0, 0, 0, 0);
  inspectorPages_ = new ContextStack;
  inspectorPages_->setObjectName("inspectorPages");
  propertyLayout->addWidget(inspectorPages_);
  const auto makePage = [this](const char* name) {
    auto* page = new QWidget;
    page->setObjectName(name);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 16);
    layout->setSpacing(9);
    inspectorPages_->addWidget(page);
    return layout;
  };
  auto* displayPage = makePage("displayPage");
  auto* detectionPage = makePage("detectionPage");
  auto* annotationPage = makePage("annotationPage");
  auto* channelPage = makePage("channelPage");
  sourceSummary_ = output("inspectorSourceSummary");
  sourceSummary_->setWordWrap(true);
  displayPage->addWidget(sourceSummary_);
  filePropertiesButton_ = new QPushButton("文件属性 / 调整有效带宽…");
  filePropertiesButton_->setObjectName("fileProperties");
  filePropertiesButton_->setProperty("uiRole", "outline");
  filePropertiesButton_->setCursor(Qt::PointingHandCursor);
  filePropertiesButton_->setToolTip("有效带宽是文件级物理分析范围，不随图谱显示缩放改变。");
  displayPage->addWidget(filePropertiesButton_);
  displaySummary_ = output("inspectorDisplaySummary");
  displaySummary_->setWordWrap(true);
  displayPage->addWidget(displaySummary_);
  paletteHost_ = new QWidget;
  paletteHost_->setObjectName("paletteHost");
  auto* paletteRow = new QHBoxLayout(paletteHost_);
  paletteRow->setContentsMargins(0, 0, 0, 0);
  paletteRow->addWidget(new QLabel("色表"));
  displayPage->addWidget(paletteHost_);
  narrowbandSettingsLinks_ = new QWidget;
  narrowbandSettingsLinks_->setObjectName("chartSettingsLinks");
  displayPage->addWidget(narrowbandSettingsLinks_);
  displayPage->addStretch();
  filePropertiesContent_ = new QWidget;
  auto* fileForm = new QFormLayout(filePropertiesContent_);
  fileForm->setContentsMargins(5, 5, 5, 5);
  fileForm->setSpacing(12);
  const std::array<QString, 4> fileNames{"文件", "采样率", "中心频率",
                                         "完整时间范围"},
      fileIds{"fileName", "sampleRate", "centerFreq", "fullRange"};
  for (int i = 0; i < 4; ++i) {
    fileValues_[i] = output(fileIds[i]);
    row(fileForm, fileNames[i], fileValues_[i]);
  }
  effectiveBandwidth_ = new ParameterDoubleSpinBox;
  effectiveBandwidth_->setObjectName("effectiveBandwidthMHz");
  effectiveBandwidth_->setMinimumWidth(0);
  effectiveBandwidth_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  effectiveBandwidth_->setRange(0.001, 1'000'000.0);
  effectiveBandwidth_->setDecimals(6);
  effectiveBandwidth_->setSingleStep(1.0);
  effectiveBandwidth_->setSuffix(" MHz");
  effectiveBandwidth_->setToolTip(
      "限定时频图和功率谱使用的中心有效带宽；最大值受采样率限制");
  row(fileForm, "有效带宽", effectiveBandwidth_);
  auto* bandwidthHelp = label("中心有效带宽用于实际 PSD / STFT 分析，按文件保存。", "effectiveBandwidthHelp", "help");
  bandwidthHelp->setWordWrap(true);
  fileForm->addRow(bandwidthHelp);
  detectionCard_ = new DetectionCard(session_, [this] {
    return projectPath_.isEmpty() ? QString{}
                                  : QFileInfo(projectPath_).absolutePath();
  });
  detectionPage->setContentsMargins(0, 4, 0, 12);
  detectionPage->addWidget(detectionCard_);
  detectionPage->addStretch();
  connect(detectionCard_, &DetectionCard::message, this, &MainWindow::log);
  connect(detectionCard_, &DetectionCard::selectionChanged, this, [this] {
    if (auto* file = session_.activeFile())
      file->display.psdFromSelection =
          sharedPsdFromSelectionPreference_ && hasActiveSegment(*file);
    refresh();
  });
  connect(detectionCard_, &DetectionCard::requestSubmitted, this,
          [this](const DetectionRequest& request) {
            QString error;
            if (!detection_->startRequest(request, error)) log(error);
            refresh();
          });
  connect(detectionCard_, &DetectionCard::roiSelectionRequested, this,
          &MainWindow::beginDetectionRoi);
  connect(detectionCard_, &DetectionCard::cancelRunRequested, this,
          [this](const QString& runId) {
            detection_->tasks().cancel(runId.toStdString());
            detection_->refresh();
            detectionCard_->refresh();
          });
  connect(detectionCard_, &DetectionCard::focusRunningRunRequested, this,
          [this](const QString& runId) {
            detection_->openRunSource(runId);
            bottomTab_ = 1;
            resultToggle_->setChecked(true);
            updateBottom();
          });
  annotationEmpty_ = label("从图谱、结果列表或工程树选择标记，查看并编辑来源、活动段与修订。", "annotationEmptyGuide", "help");
  annotationEmpty_->setWordWrap(true);
  annotationPage->addWidget(annotationEmpty_);
  annotationPage->addWidget(annotations_->inspectorWidget());
  selectedCount_ = label({}, "selectedCount", "help");
  selectedCount_->setWordWrap(true);
  annotationPage->addWidget(selectedCount_);
  auto* markActions = new QWidget;
  auto* markRow = new QHBoxLayout(markActions);
  markRow->setContentsMargins(0, 0, 0, 0);
  markRow->setSpacing(5);
  rename_ = push("重命名", "renameMark", markRow);
  locate_ = push("定位", "locateMark", markRow);
  delete_ = push("删除所选标记", "deleteMarks", markRow);
  delete_->setProperty("preserveParameterDrafts", true);
  for (auto* action : {rename_, locate_, delete_})
    action->setProperty("uiRole", "outline");
  markRow->addStretch();
  annotationActions_ = markActions;
  annotationPage->addWidget(markActions);
  annotationPage->addStretch();
  connect(rename_, &QPushButton::clicked, this, &MainWindow::renameMark);
  connect(locate_, &QPushButton::clicked, this, &MainWindow::locateMark);
  connect(delete_, &QPushButton::clicked, this, &MainWindow::deleteMarks);
  auto* channelFields = new QWidget;
  auto* channelForm = new QFormLayout(channelFields);
  channelForm->setContentsMargins(0, 0, 0, 0);
  channelForm->setSpacing(12);
  channelPage->addWidget(channelFields);
  channelPage->addStretch();
  const std::array<QString, 6> channelNames{"来源文件",   "来源标记",
                                            "射频中心",   "有效带宽",
                                            "输出采样率", "源样本时段"};
  const std::array<QString, 6> channelIds{
      "narrowbandSourceFile", "narrowbandSourceMark", "narrowbandCenter",
      "narrowbandBandwidth",  "narrowbandOutputRate", "narrowbandSourceTime"};
  for (std::size_t index = 0; index < channelValues_.size(); ++index) {
    channelValues_[index] = output(channelIds[index]);
    row(channelForm, channelNames[index], channelValues_[index]);
  }
  channelRevision_ = output("channelSourceRevision");
  channelRelation_ = output("channelSourceRelation");
  channelRevision_->setWordWrap(true);
  channelRelation_->setWordWrap(true);
  row(channelForm, "创建修订", channelRevision_);
  row(channelForm, "来源关系", channelRelation_);
  auto* channelActions = new QWidget;
  auto* channelRow = new QHBoxLayout(channelActions);
  channelRow->setContentsMargins(0, 5, 0, 0);
  channelRow->setSpacing(5);
  auto* locateChannel =
      push("⌖ 来源定位", "propertyLocateChannelSource", channelRow);
  auto* editChannel = push("修改参数…", "propertyEditChannel", channelRow);
  locateChannel->setProperty("uiRole", "outline");
  editChannel->setProperty("uiRole", "outline");
  channelForm->addRow(channelActions);
  connect(locateChannel, &QPushButton::clicked, this,
          &MainWindow::locateActiveChannelSource);
  connect(editChannel, &QPushButton::clicked, this, [this] {
    if (const auto* active = session_.activeChannel())
      showChannelDialog(q(active->id));
  });
  waveformMode_ =
      combo("waveformMode", {"I 分量（ADC 计数）", "Q 分量（ADC 计数）",
                             "幅度（RMS，ADC 计数）", "幅度包络（ADC 计数）"});
  QStringList fftSizes;
  for (int order = 5; order <= 16; ++order)
    fftSizes << QString::number(1 << order);
  psd_ = combo("psdFft", fftSizes);
  psdScope_ = combo("psdScope",
                    {"当前可见时间窗", "当前活动信号标记", "整个文件 / 通道"});
  psdParameters_ = new SpectralSettingsWidget(true);
  QStringList stftSizes;
  for (int order = 5; order <= 16; ++order)
    stftSizes << QString::number(1 << order);
  stft_ = combo("stftFft", stftSizes);
  dynamic_ = combo("dynamic",
                   {"20 dB", "40 dB", "60 dB", "80 dB", "100 dB", "120 dB"});
  reference_ =
      combo("reference", {"0 dBFS/Hz", "-20 dBFS/Hz", "-40 dBFS/Hz",
                          "-60 dBFS/Hz", "-80 dBFS/Hz", "-100 dBFS/Hz"});
  stftParameters_ = new SpectralSettingsWidget(false);
  dynamic_->setEditable(true);
  reference_->setEditable(true);
  dynamic_->setInsertPolicy(QComboBox::NoInsert);
  reference_->setInsertPolicy(QComboBox::NoInsert);
  for (auto* control : {dynamic_, reference_}) {
    control->setCompleter(nullptr);
    control->lineEdit()->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral(
            R"([-+]?(?:\d+(?:\.\d{0,3})?|\.\d{1,3})(?:\s*dB(?:FS(?:/Hz)?)?)?)")),
        control));
  }
  freqMode_ = combo("freqMode", {"射频绝对频率", "基带相对频率"});
  layerRow->addStretch();
  for (int i = 0; i < dynamic_->count(); ++i)
    dynamic_->setItemData(i, 20 + 20 * i);
  for (int i = 0; i < reference_->count(); ++i)
    reference_->setItemData(i, -20 * i);
  autoPowerFit_ = new QPushButton("自动适配电平");
  autoPowerFit_->setObjectName("autoPowerFit");
  connect(autoPowerFit_, &QPushButton::clicked, this,
          &MainWindow::autoFitPower);
  colorScale_ = new QCheckBox;
  colorScale_->setObjectName("colorbarToggle");
  grid_ = new QCheckBox;
  grid_->setObjectName("gridToggle");
  propScroll_->setWidget(content);
  right->addWidget(propScroll_, 1);
  right->addWidget(detectionCard_->actionsWidget());
  showInspectorPage(0);
  bodyLayout->addWidget(properties_);
  const auto bindPanelResize = [this](PanelResizeHandle* handle, int* value,
                                      bool reverse, int minimum, int maximum) {
    auto before = std::make_shared<int>(*value);
    handle->beginResize = [this, value, before] {
      for (auto* plot : {main_, auxiliary_, navigation_})
        plot->cancelGesture(false);
      *before = *value;
    };
    const auto apply = [this, value](int next) {
      *value = next;
      if (bottom_ && resultToggle_->isChecked())
        bottom_->setFixedHeight(bottomHeight_);
      enforceLayout();
    };
    handle->resizeDelta = [before, reverse, minimum, maximum,
                           apply](int delta) {
      apply(std::clamp(*before + (reverse ? -delta : delta), minimum, maximum));
    };
    handle->endResize = [this] { saveUiState(); };
    handle->cancelResize = [before, apply] { apply(*before); };
  };
  bindPanelResize(leftResize_, &resourceWidth_, false, 190, 450);
  bindPanelResize(rightResize_, &propertyWidth_, true, 320, 650);
  bindPanelResize(bottomResize_, &bottomHeight_, true, 100, 500);

  graphArea->setObjectName("graphArea");
  graphArea->installEventFilter(this);
  empty_ = new QWidget(graphArea);
  empty_->setObjectName("emptyWorkspace");
  empty_->setStyleSheet("background:#0d1725;");
  auto* emptyLayout = new QVBoxLayout(empty_);
  emptyLayout->setSpacing(12);
  emptyLayout->addStretch();
  emptyLayout->addWidget(
      BrandAssets::label("lockup", QSize(360, 90), empty_, "welcomeBrand"), 0,
      Qt::AlignCenter);
  auto* emptyTitle = label("开始宽带数据分析", "emptyWorkflowTitle");
  emptyTitle->setStyleSheet("font-size:22px;font-weight:600;color:#dce9f6;");
  emptyTitle->setAlignment(Qt::AlignCenter);
  emptyLayout->addWidget(emptyTitle);
  auto* emptyHelp =
      label("按顺序完成工程、信号和宽带数据操作", "emptyWorkflowHelp", "help");
  emptyHelp->setAlignment(Qt::AlignCenter);
  emptyLayout->addWidget(emptyHelp);
  auto* workflow = label("01  工程     →     02  信号     →     03  宽带数据",
                         "emptyWorkflowSteps");
  workflow->setAlignment(Qt::AlignCenter);
  workflow->setStyleSheet("font-size:14px;color:#79b8de;padding:14px;");
  emptyLayout->addWidget(workflow);
  auto* projectButtons = new QWidget(empty_);
  projectButtons->setObjectName("emptyProjectActions");
  auto* projectButtonRow = new QHBoxLayout(projectButtons);
  projectButtonRow->setContentsMargins(0, 0, 0, 0);
  projectButtonRow->setSpacing(8);
  auto* newProject = push("▧ 新建工程…", "emptyNewProject", projectButtonRow);
  newProject->setProperty("uiRole", "primary");
  auto* openProject = push("⇧ 打开工程…", "emptyOpenProject", projectButtonRow);
  openProject->setProperty("uiRole", "outline");
  auto* demoProject =
      push("打开演示工程", "emptyDemoProject", projectButtonRow);
  demoProject->setProperty("uiRole", "outline");
  emptyLayout->addWidget(projectButtons, 0, Qt::AlignCenter);
  auto* emptyAdd = push("＋ 添加 / 打开信号…", "emptyAddSignal", emptyLayout);
  emptyAdd->setProperty("uiRole", "primary");
  emptyLayout->addStretch();
  connect(newProject, &QPushButton::clicked, this,
          [this] { findChild<QAction*>("newProjectAction")->trigger(); });
  connect(openProject, &QPushButton::clicked, this,
          [this] { findChild<QAction*>("openProjectAction")->trigger(); });
  connect(demoProject, &QPushButton::clicked, this,
          [this] { findChild<QAction*>("openDemoProjectAction")->trigger(); });
  connect(emptyAdd, &QPushButton::clicked, this, [this] {
    if (addSignalAction_->isEnabled()) addSignalAction_->trigger();
  });
  maxVeil_ = new QWidget(this);
  maxVeil_->setObjectName("maximizeVeil");
  maxVeil_->setStyleSheet("background:rgba(3,9,21,217);");
  maxVeil_->hide();
  maxVeil_->installEventFilter(this);
  maxHost_ = new QWidget(this);
  maxHost_->setObjectName("maximizeHost");
  maxHost_->setStyleSheet("background:#121f32;");
  auto* maxLayout = new QVBoxLayout(maxHost_);
  maxLayout->setContentsMargins(0, 0, 0, 0);
  maxHost_->hide();

  connect(tree_, &QTreeWidget::itemClicked, this,
          [this](QTreeWidgetItem* item, int) {
            if (refreshing_) return;
            if (item->data(0, Qt::UserRole).toString() == "detectionRun") {
              const auto id = item->data(0, Qt::UserRole + 1).toString();
              QTimer::singleShot(0, this, [this, id] {
                cancelInteractions();
                detection_->openRunSource(id);
                bottomTab_ = 1;
                resultToggle_->setChecked(true);
                updateBottom();
              });
              return;
            }
            if (item->data(0, Qt::UserRole).toString() == "channel") {
              activateTreeChannel(item->data(0, Qt::UserRole + 1).toString());
              return;
            }
            if (item->data(0, Qt::UserRole).toString() == "annotationMore") {
              const auto kind = item->data(0, Qt::UserRole + 1).toInt();
              QTimer::singleShot(0, this, [this, kind] {
                treeAnnotationLimits_[kind] += kTreeAnnotationBatchSize;
                updateTree();
              });
              return;
            }
            if (item->data(0, Qt::UserRole).toString() == "mark") return;
            if (item->data(0, Qt::UserRole).toString() != "file") return;
            const auto id =
                item->data(0, Qt::UserRole + 1).toString().toStdString();
            QTimer::singleShot(0, this, [this, id] {
              cancelInteractions();
              if (session_.activateFile(id)) {
                refresh();
              }
            });
          });
  connect(tree_, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem* item, int) {
            if (item->data(0, Qt::UserRole).toString() == "mark") {
              cancelInteractions();
              session_.focusMark(
                  item->data(0, Qt::UserRole + 1).toString().toStdString());
              session_.project().narrowbandWorkspaceOpen = false;
              workspaceStack_->setCurrentIndex(0);
              refresh();
            } else if (item->data(0, Qt::UserRole).toString() == "channel")
              activateTreeChannel(item->data(0, Qt::UserRole + 1).toString());
          });
  connect(tree_, &QTreeWidget::itemEntered, this,
          [this](QTreeWidgetItem* item, int) {
            if (auto* file = session_.activeFile()) {
              const auto id = item->data(0, Qt::UserRole).toString() == "mark"
                                  ? item->data(0, Qt::UserRole + 1).toString()
                                  : QString{};
              const auto* annotation = findAnnotation(*file, id.toStdString());
              if (session_.annotations().hover(
                      *file, id.toStdString(),
                      annotation ? annotation->activeSegmentId : std::string{}))
                refreshAnnotationHover();
            }
          });
  tree_->viewport()->installEventFilter(this);
  connect(
      tree_, &QTreeWidget::customContextMenuRequested, this,
      [this](QPoint point) {
        auto* item = tree_->itemAt(point);
        if (!item) return;
        const auto kind = item->data(0, Qt::UserRole).toString();
        if (kind == "channel") {
          const auto channelId = item->data(0, Qt::UserRole + 1).toString();
          QMenu menu(this);
          menu.addAction("打开窄带工作区", this,
                         [this, channelId] { activateTreeChannel(channelId); });
          menu.addAction("修改通道参数…", this,
                         [this, channelId] { showChannelDialog(channelId); });
          menu.addAction("定位来源标记", this, [this, channelId] {
            session_.activateChannel(channelId.toStdString());
            locateActiveChannelSource();
          });
          menu.addAction("重命名通道…", this, [this, channelId] {
            auto* file = session_.fileForChannel(channelId.toStdString());
            if (!file) return;
            auto it = std::find_if(file->channels.begin(), file->channels.end(),
                                   [&](const Channel& value) {
                                     return value.id == channelId.toStdString();
                                   });
            if (it == file->channels.end()) return;
            bool ok = false;
            const auto name =
                QInputDialog::getText(this, "重命名窄带通道", "通道名称",
                                      QLineEdit::Normal, q(it->name), &ok)
                    .trimmed();
            if (ok && !name.isEmpty() && name.size() <= 80) {
              auto updated = *it;
              updated.name = name.toStdString();
              session_.updateChannel(updated.id, updated);
              refresh();
            }
          });
          menu.addAction("删除通道…", this, [this, channelId] {
            auto* file = session_.fileForChannel(channelId.toStdString());
            if (!file) return;
            const auto* channel = [&]() -> const Channel* {
              for (const auto& value : file->channels)
                if (value.id == channelId.toStdString()) return &value;
              return nullptr;
            }();
            if (!channel ||
                QMessageBox::question(this, "删除窄带通道",
                                      "删除通道“" + q(channel->name) + "”？") !=
                    QMessageBox::Yes)
              return;
            session_.removeChannel(channelId.toStdString());
            refresh();
            log("已删除窄带通道");
          });
          menu.exec(tree_->viewport()->mapToGlobal(point));
          return;
        }
        if (kind == "file") {
          const auto fileId = item->data(0, Qt::UserRole + 1).toString();
          QMenu menu(this);
          menu.addAction("打开宽带工作区", this, [this, fileId] {
            session_.activateFile(fileId.toStdString());
            session_.project().narrowbandWorkspaceOpen = false;
            refresh();
          });
          menu.addAction("添加 / 打开 IQ…", this,
                         &MainWindow::showAddFileDialog);
          menu.exec(tree_->viewport()->mapToGlobal(point));
          return;
        }
        if (kind != "mark") return;
        const auto id = item->data(0, Qt::UserRole + 1).toString();
        const auto* f = session_.activeFile();
        if (!f) return;
        if (std::find(f->selectedMarkIds.begin(), f->selectedMarkIds.end(),
                      id.toStdString()) == f->selectedMarkIds.end())
          selectMark(id, Qt::NoModifier);
        QMenu menu(this);
        auto* create = menu.addAction("创建窄带通道…", this,
                                      [this] { showChannelDialog(); });
        create->setObjectName("annotationTreeCreateChannelAction");
        menu.addAction("重命名当前标记", this, &MainWindow::renameMark);
        menu.addAction("定位当前标记", this, &MainWindow::locateMark);
        menu.addAction(QString("删除所选标记 (%1)")
                           .arg(session_.activeFile()->selectedMarkIds.size()),
                       this, &MainWindow::deleteMarks);
        menu.addSeparator();
        menu.addAction(backAction_);
        menu.addAction(forwardAction_);
        menu.addSeparator();
        menu.addAction("适应全部数据", this, [this] {
          if (const auto* active = session_.activeFile())
            session_.setView(fullRange(active->metadata));
          refresh();
        });
        menu.addAction("恢复默认视图", this, [this] {
          session_.resetView();
          propagateGlobalRightSidebarSettings();
          refresh();
          scheduleRightSidebarSettingsSave();
        });
        menu.exec(tree_->viewport()->mapToGlobal(point));
      });
  for (auto* plot : {navigation_, auxiliary_, main_}) {
    connect(plot, &PlotWidget::stateChanged, this, [this] {
      propagateGlobalRightSidebarSettings();
      refresh();
      scheduleRightSidebarSettingsSave();
    });
    connect(plot, &PlotWidget::statusMessage, this, &MainWindow::log);
    connect(plot, &PlotWidget::markSelectionRequested, this,
            [this](const QString& id, Qt::KeyboardModifiers modifiers) {
              selectMark(id, modifiers);
            });
    connect(plot, &PlotWidget::markSegmentSelectionRequested, this,
            [this](const QString& id, const QString& segment,
                   Qt::KeyboardModifiers modifiers) {
              selectMark(id, modifiers, segment);
            });
    connect(plot, &PlotWidget::annotationHoverChanged, this,
            &MainWindow::refreshAnnotationHover);
    connect(plot, &PlotWidget::cursorChanged, this, &MainWindow::updateCursor);
    connect(
        plot, &PlotWidget::interactionHint, this,
        [this](const QString& text) { statusBar()->showMessage(text, 2500); });
    connect(plot, &PlotWidget::markRenameRequested, this,
            &MainWindow::renameMark);
    connect(plot, &PlotWidget::annotationChannelRequested, this,
            [this] { showChannelDialog(); });
    connect(plot, &PlotWidget::markDeleteRequested, this,
            &MainWindow::deleteMarks);
    connect(plot, &PlotWidget::widebandDetectionRequested, this,
            [this] {
              detectionCard_->useVisibleRange();
              openWidebandDetection();
            });
    connect(plot, &PlotWidget::detectionRoiRequested, this,
            &MainWindow::beginDetectionRoi);
  }
  connect(main_, &PlotWidget::detectionRoiPreviewChanged, this,
          &MainWindow::updateDetectionRoiConfirmation);
  connect(main_, &PlotWidget::detectionRoiConfirmed, this,
          [this](const ViewRange& range) {
            detectionCard_->setRequestedRoi(range);
            openWidebandDetection();
          });
  for (auto* control : {dynamic_, reference_}) {
    restoreCustomValue(control, control == dynamic_ ? " dB" : " dBFS/Hz");
  }
  const auto spectralEdit = [this](bool psd) {
    if (refreshing_) return;
    auto* file = session_.activeFile();
    if (!file) return;
    auto* control = psd ? psdParameters_ : stftParameters_;
    if (session_.project().narrowbandWorkspaceOpen &&
        session_.activeChannel()) {
      auto* channel = session_.activeChannel();
      if (psd) {
        channel->psd.parameters = control->parameters();
        channel->psd.statistic = control->statistic();
      } else
        channel->spectrogram.parameters = control->parameters();
    } else {
      if (psd) {
        file->display.psd.parameters = control->parameters();
        file->display.psd.statistic = control->statistic();
      } else
        file->display.spectrogram.parameters = control->parameters();
      propagateGlobalRightSidebarSettings();
      scheduleRightSidebarSettingsSave();
    }
    refresh();
  };
  connect(psdParameters_, &SpectralSettingsWidget::edited, this,
          [spectralEdit] { spectralEdit(true); });
  connect(stftParameters_, &SpectralSettingsWidget::edited, this,
          [spectralEdit] { spectralEdit(false); });
  auto change = [this] {
    if (refreshing_) return;
    auto* f = session_.activeFile();
    if (!f || f->metadata.availability.status == LoadStatus::Loading) return;
    const bool narrow =
        session_.project().narrowbandWorkspaceOpen && session_.activeChannel();
    if (narrow && sender() == freqMode_) {
      if (auto* channel = session_.activeChannel())
        channel->absoluteFrequencyLabels = freqMode_->currentIndex() == 0;
      refresh();
      return;
    }
    if (narrow && sender() == grid_) {
      if (auto* grid = narrowband_->findChild<QCheckBox*>("narrowbandGrid"))
        grid->setChecked(grid_->isChecked());
      refresh();
      return;
    }
    if (narrow && sender() == psdScope_) {
      session_.activeChannel()->psd.scope =
          static_cast<PsdScope>(psdScope_->currentIndex());
      refresh();
      return;
    }
    if (narrow && (sender() == stft_ || sender() == psd_)) {
      const auto points =
          (sender() == stft_ ? stft_ : psd_)->currentText().toInt();
      const bool stftChanged = sender() == stft_;
      cancelInteractions(false);
      if (auto* channel = session_.activeChannel()) {
        if (stftChanged)
          channel->stftFftSize = points;
        else
          channel->psdFftSize = points;
      }
      refresh();
      return;
    }
    const auto fileId = f->metadata.id;
    auto display = f->display;
    const auto nextAux = static_cast<AuxiliaryMode>(auxMode_->currentIndex());
    display.auxiliaryMode = nextAux;
    display.mainMode = static_cast<MainMode>(mainMode_->currentIndex());
    display.palette = static_cast<Palette>(propertyPalette_->currentIndex());
    display.waveformMode =
        static_cast<WaveformMode>(waveformMode_->currentIndex());
    if (!narrow) {
      display.absoluteFrequency = freqMode_->currentIndex() == 0;
      display.grid = grid_->isChecked();
    }
    display.colorScale = colorScale_->isChecked();
    if (!narrow) {
      if (!session_.linkedCursor(fileId).framePsd)
        display.psdSize = psd_->currentText().toInt();
      display.stftSize = stft_->currentText().toInt();
    }
    if (sender() == psdScope_) {
      sharedPsdFromSelectionPreference_ = psdScope_->currentIndex() == 1;
      display.psd.scope = static_cast<PsdScope>(psdScope_->currentIndex());
    }
    display.psdFromSelection =
        sharedPsdFromSelectionPreference_ && hasActiveSegment(*f);
    if (sharedPsdFromSelectionPreference_ && !display.psdFromSelection)
      display.psd.scope = PsdScope::Visible;
    if (sharedPsdFromSelectionPreference_ && !display.psdFromSelection)
      log("请先选择当前文件的信号标记；PSD 统计来源仍为当前可见时间窗");
    cancelInteractions(false);
    f = session_.activeFile();
    if (!f || f->metadata.id != fileId) return;
    // Use restored ranges after cancellation; controls above may have
    // synchronously refreshed.
    display.waveformMin = f->display.waveformMin;
    display.waveformMax = f->display.waveformMax;
    display.psdMin = f->display.psdMin;
    display.psdMax = f->display.psdMax;
    if (f->display.auxiliaryMode == AuxiliaryMode::Waveform) {
      display.waveformMin = f->display.auxiliaryMin;
      display.waveformMax = f->display.auxiliaryMax;
    } else {
      display.psdMin = f->display.auxiliaryMin;
      display.psdMax = f->display.auxiliaryMax;
    }
    display.auxiliaryMin = nextAux == AuxiliaryMode::Waveform
                               ? display.waveformMin
                               : display.psdMin;
    display.auxiliaryMax = nextAux == AuxiliaryMode::Waveform
                               ? display.waveformMax
                               : display.psdMax;
    const double effectiveBandwidthHz = effectiveBandwidth_->value() * 1e6;
    const bool waveformChanged =
        display.waveformMode != f->display.waveformMode;
    if (waveformChanged) display.waveformAutoFit = true;
    f->display = display;
    if (sender() == effectiveBandwidth_ &&
        std::abs(f->metadata.effectiveBandwidthHz - effectiveBandwidthHz) > 0.5)
      session_.setEffectiveBandwidthHz(effectiveBandwidthHz);
    f = session_.activeFile();
    if (!f || f->metadata.id != fileId) return;
    session_.setView(f->view, false);
    propagateGlobalRightSidebarSettings();
    refresh();
    scheduleRightSidebarSettingsSave();
  };
  for (auto* control : {mainMode_, auxMode_, waveformMode_, propertyPalette_, psd_,
                        stft_, psdScope_, freqMode_})
    connect(control, &QComboBox::currentIndexChanged, this, change);
  for (auto* control : {dynamic_, reference_}) {
    bindParameterCombo(control, [this, control] {
      applyPowerInput(control);
      normalizePowerInput(control);
    });
  }
  for (auto* chart : {main_, auxiliary_})
    connect(chart, &PlotWidget::autoPowerFitRequested, this,
            &MainWindow::autoFitPower);
  connect(narrowband_, &NarrowbandWorkspace::autoPowerFitRequested, this,
          &MainWindow::autoFitPower);
  connect(effectiveBandwidth_, &QDoubleSpinBox::valueChanged, this, change);
  for (auto* control : {grid_, colorScale_})
    connect(control, &QCheckBox::toggled, this, change);
  statusBar()->setFixedHeight(25);
  statusBar()->setSizeGripEnabled(false);
  auto* ready = label("● 就绪", "renderingBackend");
  ready->setStyleSheet("font:11px Consolas;color:#91a9c0;");
  statusBar()->addWidget(ready);
  statusTime_ = label({}, "statusTime");
  statusFrequency_ = label({}, "statusFreq");
  statusFile_ = label({}, "statusSignal");
  for (auto* value : {statusTime_, statusFrequency_, statusFile_}) {
    value->setStyleSheet(
        "font:11px Consolas;color:#91a9c0;border-left:1px solid "
        "#294058;padding:0 12px;");
    statusBar()->addWidget(value);
  }
  dataSourceStatus_ = label("演示数据 · 未运行 DSP");
  dataSourceStatus_->setObjectName("dataSourceStatus");
  dataSourceStatus_->setStyleSheet(
      "font:11px Consolas;color:#91a9c0;padding:0 12px;");
  statusBar()->addPermanentWidget(dataSourceStatus_);
  connect(
      main_, &PlotWidget::backendChanged, ready,
      [ready](const QString& description) { ready->setToolTip(description); });
  for (auto* toggle : propScroll_->findChildren<QToolButton*>())
    if (std::any_of(kDetectionFolds.begin(), kDetectionFolds.end(), [toggle](const char* name) {
          return toggle->objectName() == name;
        }))
      connect(toggle, &QToolButton::toggled, this, [this] { saveUiState(); });
  ready->setToolTip(main_->renderingBackend());
  updateBottom();
  qApp->installEventFilter(this);
  // Settings must arbitrate Escape before the general workspace shortcuts.
  buildChartControls();
}

void MainWindow::buildChartControls() {
  settingsPopovers_ = new SettingsPopoverHost(this);
  settingsPopovers_->add(filePropertiesButton_, filePropertiesContent_, "filePropertiesPopover");
  const auto replaceStretch = [](QHBoxLayout* layout, QWidget* controls) {
    for (int index = 0; index < layout->count(); ++index)
      if (layout->itemAt(index)->spacerItem()) {
        delete layout->takeAt(index);
        layout->insertWidget(index, controls, 1);
        return;
      }
    layout->addWidget(controls, 1);
  };
  auto* auxHead = findChild<QWidget*>("auxChartHead");
  auto* specHead = findChild<QWidget*>("specChartHead");
  auxiliaryControls_ = new ChartControlStrip(settingsPopovers_, "auxChartControls", auxHead);
  spectrumControls_ = new ChartControlStrip(settingsPopovers_, "specChartControls", specHead);
  replaceStretch(qobject_cast<QHBoxLayout*>(auxHead->layout()), auxiliaryControls_);
  replaceStretch(qobject_cast<QHBoxLayout*>(specHead->layout()), spectrumControls_);
  const auto button = [this](const QString& id, const QString& text, const QString& help) {
    auto* result = new QToolButton(this);
    result->setObjectName(id);
    result->setText(text);
    result->setToolTip(help);
    result->setAccessibleName(help);
    result->setFixedHeight(24);
    return result;
  };
  psdSettingsButton_ = button("psdSettings", "设置", "PSD 估计方法、窗、重叠与分段时长");
  stftSettingsButton_ = button("stftSettings", "设置", "STFT 方法、窗、重叠与分段时长");
  powerSettingsButton_ = button("powerSettings", "电平", "参考电平、动态范围与自动适配");
  displaySettingsButton_ = button("displaySettings", "显示", "频率坐标、网格与色阶条");
  settingsPopovers_->add(psdSettingsButton_, psdParameters_, "psdSettingsPopover");
  settingsPopovers_->add(stftSettingsButton_, stftParameters_, "stftSettingsPopover");
  auto* power = new QWidget;
  auto* powerForm = new QFormLayout(power);
  powerForm->setContentsMargins(0, 0, 0, 0);
  powerForm->setSpacing(9);
  powerForm->addRow("参考电平", reference_);
  powerForm->addRow("动态范围", dynamic_);
  powerForm->addRow(autoPowerFit_);
  settingsPopovers_->add(powerSettingsButton_, power, "powerSettingsPopover");
  auto* display = new QWidget;
  auto* displayForm = new QFormLayout(display);
  displayForm->setContentsMargins(0, 0, 0, 0);
  displayForm->setSpacing(9);
  displayForm->addRow("频率坐标", freqMode_);
  displayForm->addRow("网格", grid_);
  displayForm->addRow("色阶条", colorScale_);
  settingsPopovers_->add(displaySettingsButton_, display, "displaySettingsPopover");
  const auto addSetting = [this](ChartControlStrip* strip, QToolButton* setting,
                                  const QString& title, int priority) {
    strip->addControl(setting, title, priority, [this, strip, setting] {
      settingsPopovers_->open(setting, strip->overflowButton());
    });
  };
  waveformMode_->setFixedWidth(182);
  waveformMode_->setToolTip("波形分量，单位 ADC 计数");
  psd_->setFixedWidth(86);
  psd_->setAccessibleName("PSD N");
  stft_->setFixedWidth(86);
  stft_->setAccessibleName("STFT N");
  psdScope_->setFixedWidth(150);
  psdScope_->setToolTip("PSD 统计时间范围；显示分析，不改变检测任务");
  psdStatisticControl_ = psdParameters_->takeStatisticControl();
  psdStatisticControl_->setFixedWidth(84);
  psdStatisticControl_->setAccessibleName("PSD 统计类型");
  psdSourceControl_ = combo("psdSource", {"统计谱", "驻留帧谱"});
  psdSourceControl_->setFixedWidth(100);
  psdSourceControl_->setToolTip("驻留帧谱直接复用实际 STFT 帧，N 随 STFT");
  connect(psdSourceControl_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (refreshing_) return;
    const auto* file = session_.activeFile();
    if (!file) return;
    const auto* channel = session_.project().narrowbandWorkspaceOpen ? session_.activeChannel() : nullptr;
    session_.setFramePsd(channel ? channel->id : file->metadata.id, index == 1);
    refresh();
  });
  auxiliaryControls_->addControl(waveformMode_, "波形分量", 100);
  auxiliaryControls_->addControl(psd_, "PSD N", 100);
  auxiliaryControls_->addControl(psdScope_, "统计时间范围", 20);
  auxiliaryControls_->addControl(psdStatisticControl_, "PSD 统计类型", 30);
  auxiliaryControls_->addControl(psdSourceControl_, "统计谱 / 驻留帧谱", 10);
  addSetting(auxiliaryControls_, psdSettingsButton_, "PSD 设置", 5);
  spectrumControls_->addControl(stft_, "STFT N", 100);
  spectrumControls_->addControl(propertyPalette_, "色表", 90);
  addSetting(spectrumControls_, powerSettingsButton_, "电平", 80);
  addSetting(spectrumControls_, displaySettingsButton_, "显示选项", 20);
  addSetting(spectrumControls_, stftSettingsButton_, "STFT 设置", 10);
  specMode_->setMaximumWidth(160);
  specAxis_->setMaximumWidth(110);
  auto* links = new QVBoxLayout(narrowbandSettingsLinks_);
  links->setContentsMargins(12, 8, 12, 8);
  auto* caption = new QLabel("图谱设置", narrowbandSettingsLinks_);
  links->addWidget(caption);
  for (auto* setting : {psdSettingsButton_, stftSettingsButton_, powerSettingsButton_, displaySettingsButton_}) {
    const auto linkCaption = setting == psdSettingsButton_ ? "PSD 方法与窗…"
        : setting == stftSettingsButton_ ? "STFT 方法与窗…"
        : setting == powerSettingsButton_ ? "电平与自动适配…" : "显示选项…";
    auto* link = new QPushButton(linkCaption, narrowbandSettingsLinks_);
    link->setProperty("uiRole", "outline");
    link->setToolTip(setting->toolTip());
    link->setCursor(Qt::PointingHandCursor);
    link->setObjectName("narrowband" + setting->objectName());
    link->setAccessibleName(setting->toolTip());
    links->addWidget(link);
    connect(link, &QPushButton::clicked, this, [this, link, setting] {
      settingsPopovers_->open(setting, link);
    });
  }
}

void MainWindow::updateChartControls(bool narrow) {
  if (!auxiliaryControls_) return;
  const auto* file = session_.activeFile();
  const bool wave = !file || file->display.auxiliaryMode == AuxiliaryMode::Waveform;
  const bool evidence = auxiliaryEvidenceVisible_;
  auxiliaryControls_->requestVisible(waveformMode_, wave && !evidence);
  for (auto* control : {static_cast<QWidget*>(psd_), static_cast<QWidget*>(psdScope_),
                         static_cast<QWidget*>(psdStatisticControl_), static_cast<QWidget*>(psdSourceControl_),
                         static_cast<QWidget*>(psdSettingsButton_)})
    auxiliaryControls_->requestVisible(control, !wave && !evidence);
  const bool ready = file && file->metadata.availability.status != LoadStatus::Loading;
  filePropertiesButton_->setEnabled(file != nullptr);
  spectrumControls_->setExternalHost(propertyPalette_, narrow ? paletteHost_ : nullptr);
  paletteHost_->setVisible(narrow);
  waveformMode_->setEnabled(ready);
  stft_->setEnabled(ready);
  for (auto* setting : {psdSettingsButton_, stftSettingsButton_, powerSettingsButton_, displaySettingsButton_})
    setting->setEnabled(ready);
  stftParameters_->setEnabled(ready);
  psdSourceControl_->setEnabled(ready);
  if (file) {
    const auto* channel = narrow ? session_.activeChannel() : nullptr;
    const auto& cursor = session_.linkedCursor(channel ? channel->id : file->metadata.id);
    psd_->setEnabled(ready && !cursor.framePsd);
    psdScope_->setEnabled(ready && !cursor.framePsd);
    psdParameters_->setEnabled(ready && !cursor.framePsd);
    psdSourceControl_->setCurrentIndex(cursor.framePsd ? 1 : 0);
    if (auto* model = qobject_cast<QStandardItemModel*>(psdSourceControl_->model()))
      model->item(1)->setEnabled(cursor.pinned);
    psdStatisticControl_->setEnabled(ready && !cursor.framePsd &&
                                    psdParameters_->parameters().method != SpectralMethod::Periodogram);
    psdSettingsButton_->setEnabled(ready && !cursor.framePsd);
  } else {
    psdStatisticControl_->setEnabled(false);
  }
  for (auto* setting : {psdSettingsButton_, stftSettingsButton_, powerSettingsButton_, displaySettingsButton_})
    if (auto* link = narrowbandSettingsLinks_->findChild<QPushButton*>("narrowband" + setting->objectName()))
      link->setEnabled(setting->isEnabled());
  if (file) {
    powerSettingsButton_->setText(QString("电平 %1/%2")
        .arg(QString::number(file->display.referenceLevelDb, 'f', 1),
             QString::number(file->display.dynamicRangeDb, 'f', 1)));
    powerSettingsButton_->setToolTip(QString("参考电平 %1 %2 · 动态范围 %3 dB\n打开电平与自动适配设置")
        .arg(number(file->display.referenceLevelDb), powerUnit(file->metadata),
             number(file->display.dynamicRangeDb)));
  }
  auxiliaryControls_->reflow();
  spectrumControls_->reflow();
}

void MainWindow::rebuildTree() {
  if (treePointerPressed_) {
    treeRefreshAfterRelease_ = true;
    return;
  }
  QSignalBlocker block(tree_);
  std::map<QString, bool> expanded;
  const auto itemKey = [](QTreeWidgetItem* item) {
    QString key;
    for (auto* part = item; part; part = part->parent())
      key.prepend("/" + part->data(0, Qt::UserRole).toString() + ":" +
                  part->data(0, Qt::UserRole + 1).toString());
    return key;
  };
  const auto focusedKey =
      tree_->currentItem() ? itemKey(tree_->currentItem()) : QString{};
  for (QTreeWidgetItemIterator it(tree_); *it; ++it)
    expanded.emplace(itemKey(*it), (*it)->isExpanded());
  tree_->clear();
  auto* root =
      new QTreeWidgetItem(tree_, {"▾ ◧ " + q(session_.project().name)});
  root->setExpanded(true);
  root->setFlags(Qt::ItemIsEnabled);
  root->setData(0, Qt::UserRole, "project");
  root->setSizeHint(0, QSize(0, 35));
  root->setBackground(0, QColor("#1b2f45"));
  QFont bold = tree_->font();
  bold.setBold(true);
  root->setFont(0, bold);
  for (const auto& file : session_.project().files) {
    auto* item = new QTreeWidgetItem(root, {"◈ " + q(file.metadata.name)});
    item->setData(0, Qt::UserRole, "file");
    item->setData(0, Qt::UserRole + 1, q(file.metadata.id));
    item->setFlags(Qt::ItemIsEnabled);
    item->setSizeHint(0, QSize(0, 32));
    item->setExpanded(true);
    item->setToolTip(0, q(file.metadata.path));
    const bool active = file.metadata.id == session_.project().activeFileId;
    if (active) {
      item->setBackground(0, QColor("#214c70"));
      item->setForeground(0, QColor("#e0f5ff"));
    }
    const double duration = static_cast<double>(file.metadata.sampleCount) /
                            file.metadata.sampleRateHz;
    const QString bandwidth =
        file.metadata.declaredBandwidthHz > 0
            ? " · BW " + number(file.metadata.declaredBandwidthHz / 1e6) +
                  " MHz"
            : QString{};
    auto* info = new QTreeWidgetItem(
        item, {number(file.metadata.sampleRateHz / 1e6) + " MS/s · " +
               number(file.metadata.centerFrequencyHz / 1e6) + " MHz" +
               bandwidth + " · " + number(duration) + " s"});
    info->setData(0, Qt::UserRole, "metadata");
    info->setFlags(Qt::ItemIsEnabled);
    info->setForeground(0, QColor("#8da8c2"));
    info->setFont(0, QFont("Consolas", 8));
    info->setSizeHint(0, QSize(0, 25));
    if (!active) continue;
    auto* marks = new QTreeWidgetItem(item, {"信号标记"});
    marks->setData(0, Qt::UserRole, "marks");
    marks->setFlags(Qt::ItemIsEnabled);
    marks->setExpanded(true);
    marks->setSizeHint(0, QSize(0, 30));
    marks->setForeground(0, QColor("#93b9d6"));
    auto* actions = new QTreeWidgetItem(marks);
    actions->setFlags(Qt::ItemIsEnabled);
    actions->setData(0, Qt::UserRole, "treeActions");
    actions->setSizeHint(0, QSize(0, 61));
    auto* controls = new QWidget;
    auto* controlColumn = new QVBoxLayout(controls);
    controlColumn->setContentsMargins(0, 1, 0, 1);
    controlColumn->setSpacing(3);
    auto* counts = new QLabel(annotationCountText(file));
    counts->setObjectName("treeAnnotationCounts");
    counts->setWordWrap(true);
    controlColumn->addWidget(counts);
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(4);
    controlColumn->addLayout(actionRow);
    auto* all = push("全选匹配", "treeSelectAll", actionRow);
    auto* remove = push("删除所选标记", "treeDeleteMarks", actionRow);
    remove->setProperty("preserveParameterDrafts", true);
    all->setProperty("uiRole", "outline");
    remove->setProperty("uiRole", "outline");
    all->setEnabled(annotationCounts(file).matching != 0);
    remove->setEnabled(!file.selectedMarkIds.empty());
    actionRow->addStretch();
    tree_->setItemWidget(actions, 0, controls);
    connect(all, &QPushButton::clicked, this, &MainWindow::selectAllMarks);
    connect(remove, &QPushButton::clicked, this, &MainWindow::deleteMarks);
    const auto order = treeSelectionOrder();
    std::unordered_map<std::string, const Annotation*> byId;
    for (const auto& annotation : file.marks)
      byId.emplace(annotation.id, &annotation);
    for (int kind = 0; kind < 3; ++kind) {
      const auto source = static_cast<AnnotationSource>(kind);
      const QString title = kind == 0   ? "原始标签"
                            : kind == 1 ? "手动标记"
                                        : "算法结果";
      std::size_t total = 0, matching = 0, loaded = 0;
      for (const auto& annotation : file.marks)
        if (annotation.sourceKind == source &&
            annotationInContext(file, annotation))
          ++total;
      for (const auto& id : order)
        if (byId.at(id)->sourceKind == source) ++matching;
      auto* group = new QTreeWidgetItem(
          marks, {QString("%1 (%2/%3)").arg(title).arg(matching).arg(total)});
      group->setData(0, Qt::UserRole, "annotationSource");
      group->setData(0, Qt::UserRole + 1, kind);
      group->setFlags(Qt::ItemIsEnabled);
      group->setExpanded(true);
      std::size_t ordinal = 0;
      for (const auto& id : order) {
        const auto& mark = *byId.at(id);
        if (mark.sourceKind != source) continue;
        const bool include = ordinal++ < treeAnnotationLimits_[kind] ||
                             treeRevealedAnnotationIds_.contains(mark.id);
        if (!include) continue;
        ++loaded;
        auto* node = new QTreeWidgetItem(group, {"▱ " + q(mark.name)});
        node->setData(0, Qt::UserRole, "mark");
        node->setData(0, Qt::UserRole + 1, q(mark.id));
        node->setSizeHint(0, QSize(0, 31));
        node->setToolTip(
            0, "单击选择 · Ctrl 多选 · Shift 连选 · 双击定位\n" + q(mark.id));
      }
      if (loaded < matching) {
        auto* more =
            new QTreeWidgetItem(group, {QString("加载后 %1 条 · 已显示 %2/%3")
                                            .arg(kTreeAnnotationBatchSize)
                                            .arg(loaded)
                                            .arg(matching)});
        more->setData(0, Qt::UserRole, "annotationMore");
        more->setData(0, Qt::UserRole + 1, kind);
        more->setFlags(Qt::ItemIsEnabled);
        more->setForeground(0, QColor("#91cbe8"));
        more->setSizeHint(0, QSize(0, 30));
      }
    }
    auto* runs = new QTreeWidgetItem(
        item, {QString("检测运行 (%1)").arg(file.detectionRuns.size())});
    runs->setData(0, Qt::UserRole, "detectionRuns");
    runs->setExpanded(true);
    for (std::size_t index = file.detectionRuns.size() > 100
                                 ? file.detectionRuns.size() - 100
                                 : 0;
         index < file.detectionRuns.size(); ++index) {
      const auto& run = file.detectionRuns[index];
      auto* node =
          new QTreeWidgetItem(runs, {q(run.request.profile.name) + " · " +
                                     QString("%1/%2 窗口")
                                         .arg(run.completedWindows)
                                         .arg(run.eligibleWindows)});
      node->setData(0, Qt::UserRole, "detectionRun");
      node->setData(0, Qt::UserRole + 1, q(run.request.runId));
      node->setToolTip(0, q(run.request.runId) + "\n" + q(run.error));
      node->setSizeHint(0, QSize(0, 28));
    }
    auto* channels = new QTreeWidgetItem(
        item, {QString("窄带通道 (%1)").arg(file.channels.size())});
    channels->setData(0, Qt::UserRole, "channels");
    channels->setFlags(Qt::ItemIsEnabled);
    channels->setExpanded(true);
    channels->setSizeHint(0, QSize(0, 30));
    channels->setForeground(0, QColor("#93b9d6"));
    for (const auto& channel : file.channels) {
      auto* node = new QTreeWidgetItem(
          channels,
          {"◇ " + q(channel.name) + " · " +
           coordinate(channel.centerFrequencyHz, channel.bandwidthHz, false) +
           (channel.sourceTime.end > availableSamples(file.metadata)
                ? " · 来源未读入"
                : "")});
      node->setData(0, Qt::UserRole, "channel");
      node->setData(0, Qt::UserRole + 1, q(channel.id));
      node->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      node->setSizeHint(0, QSize(0, 31));
      node->setToolTip(
          0, channelRelationLabel(channel.sourceRelation) +
                 (channel.sourceSnapshot
                      ? "\n创建修订 " + q(channel.sourceSnapshot->revisionId)
                      : "\n没有来源修订快照"));
      if (channel.sourceRelation != ChannelSourceRelation::Current)
        node->setForeground(0, QColor("#e6ba65"));
      node->setToolTip(
          0, node->toolTip(0) + "\n" +
                 (channel.processingState == ChannelProcessingState::Ready
                      ? "打开真实窄带分析工作区"
                      : "通道配置待确认"));
      if (channel.id == session_.project().activeChannelId) {
        node->setBackground(0, QColor("#3d442f"));
        node->setForeground(0, QColor("#fff0bf"));
      }
    }
    if (file.activeMarkId.empty())
      tree_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
  }
  for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
    const auto key = itemKey(*it);
    const auto found = expanded.find(key);
    if (found != expanded.end()) (*it)->setExpanded(found->second);
    if (!focusedKey.isEmpty() && key == focusedKey)
      tree_->setCurrentItem(*it, 0, QItemSelectionModel::NoUpdate);
  }
  treeStructure_ = treeStructure();
  syncTreeState();
  resizeTreeToContents();
}

void MainWindow::resizeTreeToContents() {
  if (!tree_) return;
  int height = 0;
  for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
    bool shown = !(*it)->isHidden();
    for (auto* ancestor = (*it)->parent(); ancestor;
         ancestor = ancestor->parent())
      shown = shown && ancestor->isExpanded() && !ancestor->isHidden();
    if (shown)
      height += std::max(tree_->fontMetrics().height() + 6,
                         (*it)->sizeHint(0).height());
  }
  tree_->setFixedHeight(height + 2);
}

std::vector<std::string> MainWindow::treeSelectionOrder() const {
  std::vector<std::string> result;
  const auto* file = session_.activeFile();
  if (!file || !annotations_) return result;
  std::unordered_map<std::string, const Annotation*> byId;
  for (const auto& annotation : file->marks)
    byId.emplace(annotation.id, &annotation);
  for (int kind = 0; kind < 3; ++kind)
    for (const auto& id : annotations_->sortedVisibleIds()) {
      const auto found = byId.find(id);
      if (found != byId.end() &&
          static_cast<int>(found->second->sourceKind) == kind &&
          annotationVisible(*file, *found->second))
        result.push_back(id);
    }
  return result;
}

QStringList MainWindow::treeStructure() const {
  QStringList structure{q(session_.project().activeFileId)};
  for (const auto& file : session_.project().files) {
    structure.append("file:" + q(file.metadata.id));
    if (file.metadata.id != session_.project().activeFileId) continue;
    std::array<std::size_t, 3> sourceCounts{};
    std::unordered_map<std::string, const Annotation*> byId;
    for (const auto& mark : file.marks) byId.emplace(mark.id, &mark);
    for (const auto& id : treeSelectionOrder()) {
      const auto kind = static_cast<int>(byId.at(id)->sourceKind);
      if (sourceCounts[kind]++ < treeAnnotationLimits_[kind] ||
          treeRevealedAnnotationIds_.contains(id))
        structure.append("mark:" + QString::number(kind) + ":" + q(id));
    }
    for (auto count : sourceCounts) structure.append(QString::number(count));
    for (const auto& channel : file.channels)
      structure.append("channel:" + q(channel.id));
    for (const auto& run : file.detectionRuns)
      structure.append("run:" + q(run.request.runId) + ":" +
                       QString::number(static_cast<int>(run.lifecycle)));
  }
  return structure;
}
void MainWindow::restoreUiState() {
  if (!qApp->property("uiStatePersistenceEnabled").toBool()) return;
  restoringUiState_ = true;
  QSettings settings;
  settings.beginGroup(QStringLiteral("ui"));
  const auto geometry =
      settings.value(QStringLiteral("geometry")).toByteArray();
  if (!geometry.isEmpty()) restoreGeometry(geometry);
  resourcesCollapsed_ =
      settings.value(QStringLiteral("resourcesCollapsed"), false).toBool();
  propertiesCollapsed_ =
      settings.value(QStringLiteral("propertiesCollapsed"), false).toBool();
  resultToggle_->setChecked(
      settings.value(QStringLiteral("resultsExpanded"), false).toBool());
  bottomTab_ =
      std::clamp(settings.value(QStringLiteral("bottomTab"), 0).toInt(), 0, 2);
  auxiliaryEvidenceVisible_ =
      settings.value("auxiliaryEvidenceVisible", false).toBool();
  resourceWidth_ =
      std::clamp(settings.value("resourceWidth", 278).toInt(), 190, 450);
  propertyWidth_ =
      std::clamp(settings.value("propertyWidth", 390).toInt(), 320, 650);
  bottomHeight_ =
      std::clamp(settings.value("bottomHeight", 260).toInt(), 100, 500);
  inspectorManualOverride_ = false;
  showInspectorPage(std::clamp(settings.value("inspectorPage", 0).toInt(), 0, 3));
  for (const auto* name : kDetectionFolds)
    if (auto* toggle = findChild<QToolButton*>(name))
      toggle->setChecked(settings.value(QString("detectionFold/") + name, false).toBool());
  const auto splitterState =
      settings.value(QStringLiteral("graphSplitter")).toByteArray();
  splitterStateRestored_ =
      !splitterState.isEmpty() && graphs_->restoreState(splitterState);
  settings.endGroup();
  restoringUiState_ = false;
  updateBottom();
}

void MainWindow::saveUiState() {
  if (restoringUiState_ || !graphs_ ||
      !qApp->property("uiStatePersistenceEnabled").toBool())
    return;
  QSettings settings;
  settings.beginGroup(QStringLiteral("ui"));
  settings.setValue(QStringLiteral("geometry"), saveGeometry());
  settings.setValue(QStringLiteral("resourcesCollapsed"), resourcesCollapsed_);
  settings.setValue(QStringLiteral("propertiesCollapsed"),
                    propertiesCollapsed_);
  settings.setValue(QStringLiteral("resultsExpanded"),
                    resultToggle_->isChecked());
  settings.setValue(QStringLiteral("bottomTab"), bottomTab_);
  settings.setValue("auxiliaryEvidenceVisible", auxiliaryEvidenceVisible_);
  settings.setValue("resourceWidth", resourceWidth_);
  settings.setValue("propertyWidth", propertyWidth_);
  settings.setValue("bottomHeight", bottomHeight_);
  settings.setValue(QStringLiteral("graphSplitter"), graphs_->saveState());
  if (inspectorPages_) settings.setValue("inspectorPage", inspectorPages_->currentIndex());
  for (const auto* name : kDetectionFolds)
    if (auto* toggle = findChild<QToolButton*>(name))
      settings.setValue(QString("detectionFold/") + name, toggle->isChecked());
  settings.endGroup();
  settings.sync();
}

void MainWindow::restoreRightSidebarSettings(FileState& file) {
  if (!qApp->property("uiStatePersistenceEnabled").toBool()) return;
  const auto key = sidebarSettingsKey(file.metadata);
  if (key.isEmpty()) return;
  QSettings settings;
  settings.beginGroup(QStringLiteral("rightSidebar"));
  const auto values = settings.value(key).toMap();
  settings.endGroup();
  if (values.isEmpty() ||
      values.value(QStringLiteral("sourcePath")).toString() !=
          sidebarSourcePath(file.metadata))
    return;

  bool ok = false;
  const double bandwidth = values
                               .value(QStringLiteral("effectiveBandwidthHz"),
                                      file.metadata.effectiveBandwidthHz)
                               .toDouble(&ok);
  if (ok && std::isfinite(bandwidth) &&
      bandwidth >= file.metadata.sampleRateHz / 65536.0 &&
      bandwidth <= file.metadata.sampleRateHz)
    file.metadata.effectiveBandwidthHz = bandwidth;
}

void MainWindow::applyGlobalRightSidebarSettings(
    const DisplaySettings& fallback) {
  DisplaySettings shared = fallback;
  QVariantMap values;
  if (qApp->property("uiStatePersistenceEnabled").toBool()) {
    QSettings settings;
    settings.beginGroup(QStringLiteral("rightSidebar"));
    values = settings.value(QStringLiteral("globalSettings")).toMap();
    if (values.isEmpty()) {
      if (const auto* active = session_.activeFile()) {
        const auto legacy =
            settings.value(sidebarSettingsKey(active->metadata)).toMap();
        if (legacy.value(QStringLiteral("sourcePath")).toString() ==
            sidebarSourcePath(active->metadata))
          values = legacy;
      }
    }
    settings.endGroup();
  }
  const auto integer = [&values](const QString& name, int fallbackValue) {
    bool ok = false;
    const int value = values.value(name).toInt(&ok);
    return ok ? value : fallbackValue;
  };
  const auto real = [&values](const QString& name, double fallbackValue) {
    bool ok = false;
    const double value = values.value(name).toDouble(&ok);
    return ok && std::isfinite(value) ? value : fallbackValue;
  };
  const auto supportedSize = [](int size, int minimum, int maximum) {
    return size >= minimum && size <= maximum && (size & (size - 1)) == 0;
  };
  const int mainMode =
      integer(QStringLiteral("mainMode"), static_cast<int>(shared.mainMode));
  if (mainMode >= 0 && mainMode <= 1)
    shared.mainMode = static_cast<MainMode>(mainMode);
  const int auxiliaryMode = integer(QStringLiteral("auxiliaryMode"),
                                    static_cast<int>(shared.auxiliaryMode));
  if (auxiliaryMode >= 0 && auxiliaryMode <= 1)
    shared.auxiliaryMode = static_cast<AuxiliaryMode>(auxiliaryMode);
  const int waveformMode = integer(QStringLiteral("waveformMode"),
                                   static_cast<int>(shared.waveformMode));
  if (waveformMode >= 0 && waveformMode <= 3)
    shared.waveformMode = static_cast<WaveformMode>(waveformMode);
  const int palette =
      integer(QStringLiteral("palette"), static_cast<int>(shared.palette));
  if (palette >= 0 && palette <= static_cast<int>(Palette::CoolEditClassic))
    shared.palette = static_cast<Palette>(palette);
  try {
    if (values.contains("psdSettings"))
      shared.psd = decodePsdSettings(
          QJsonDocument::fromJson(values.value("psdSettings").toByteArray())
              .object());
    if (values.contains("spectrogramSettings"))
      shared.spectrogram.parameters = decodeSpectralParameters(
          QJsonDocument::fromJson(
              values.value("spectrogramSettings").toByteArray())
              .object());
  } catch (const std::exception&) {
  }
  const int stftSize = integer(QStringLiteral("stftSize"), shared.stftSize);
  if (supportedSize(stftSize, 32, 65536)) shared.stftSize = stftSize;
  const int psdSize = integer(QStringLiteral("psdSize"), shared.psdSize);
  if (supportedSize(psdSize, 32, 65536)) shared.psdSize = psdSize;
  const double dynamicRange =
      real(QStringLiteral("dynamicRangeDb"), shared.dynamicRangeDb);
  if (dynamicRange > 0 && dynamicRange <= 10000)
    shared.dynamicRangeDb = dynamicRange;
  const double referenceLevel =
      real(QStringLiteral("referenceLevelDb"), shared.referenceLevelDb);
  if (referenceLevel >= -200 && referenceLevel <= 100)
    shared.referenceLevelDb = referenceLevel;
  shared.absoluteFrequency =
      values
          .value(QStringLiteral("absoluteFrequency"), shared.absoluteFrequency)
          .toBool();
  shared.grid = values.value(QStringLiteral("grid"), shared.grid).toBool();
  shared.waveformAutoFit =
      values.value(QStringLiteral("waveformAutoFit"), true).toBool();
  shared.colorScale =
      values.value(QStringLiteral("colorScale"), shared.colorScale).toBool();
  sharedPsdFromSelectionPreference_ =
      values.value(QStringLiteral("psdFromSelection"), shared.psdFromSelection)
          .toBool();
  shared.waveformMin = real(QStringLiteral("waveformMin"), shared.waveformMin);
  shared.waveformMax = real(QStringLiteral("waveformMax"), shared.waveformMax);
  shared.psdMin = real(QStringLiteral("psdMin"), shared.psdMin);
  shared.psdMax = real(QStringLiteral("psdMax"), shared.psdMax);
  if (!std::isfinite(shared.waveformMin) ||
      !std::isfinite(shared.waveformMax) ||
      shared.waveformMax - shared.waveformMin < 2.0) {
    shared.waveformMin = -32768;
    shared.waveformMax = 32768;
  }
  shared.waveformMin = std::clamp(shared.waveformMin, -65536.0, 65534.0);
  shared.waveformMax =
      std::clamp(shared.waveformMax, shared.waveformMin + 2.0, 65536.0);
  if (!validPsdRange(shared.psdMin, shared.psdMax)) {
    shared.psdMin = shared.referenceLevelDb - shared.dynamicRangeDb;
    shared.psdMax = shared.referenceLevelDb;
  }
  shared.auxiliaryMin = shared.auxiliaryMode == AuxiliaryMode::Waveform
                            ? shared.waveformMin
                            : shared.psdMin;
  shared.auxiliaryMax = shared.auxiliaryMode == AuxiliaryMode::Waveform
                            ? shared.waveformMax
                            : shared.psdMax;
  for (auto& file : session_.project().files) {
    auto& display = file.display;
    display.mainMode = shared.mainMode;
    display.auxiliaryMode = shared.auxiliaryMode;
    display.waveformMode = shared.waveformMode;
    display.palette = shared.palette;
    display.psd = shared.psd;
    display.spectrogram = shared.spectrogram;
    display.stftSize = shared.stftSize;
    display.psdSize = shared.psdSize;
    display.dynamicRangeDb = shared.dynamicRangeDb;
    display.referenceLevelDb = shared.referenceLevelDb;
    display.absoluteFrequency = shared.absoluteFrequency;
    display.grid = shared.grid;
    display.waveformAutoFit = shared.waveformAutoFit;
    display.colorScale = shared.colorScale;
    display.waveformMin = shared.waveformMin;
    display.waveformMax = shared.waveformMax;
    display.psdMin = shared.psdMin;
    display.psdMax = shared.psdMax;
    display.auxiliaryMin = shared.auxiliaryMin;
    display.auxiliaryMax = shared.auxiliaryMax;
    display.psdFromSelection =
        sharedPsdFromSelectionPreference_ && hasActiveSegment(file);
    if (file.metadata.availability.status != LoadStatus::Loading)
      file.view = clampRange(file.view, file.metadata, display.stftSize,
                             display.psdSize);
  }
}

void MainWindow::propagateGlobalRightSidebarSettings() {
  auto* active = session_.activeFile();
  if (!active) return;
  active->display.psdFromSelection =
      sharedPsdFromSelectionPreference_ && hasActiveSegment(*active);
  const auto shared = active->display;
  for (auto& file : session_.project().files) {
    if (file.metadata.id == active->metadata.id) continue;
    auto& display = file.display;
    display.mainMode = shared.mainMode;
    display.auxiliaryMode = shared.auxiliaryMode;
    display.waveformMode = shared.waveformMode;
    display.palette = shared.palette;
    display.psd = shared.psd;
    display.spectrogram = shared.spectrogram;
    display.stftSize = shared.stftSize;
    display.psdSize = shared.psdSize;
    display.dynamicRangeDb = shared.dynamicRangeDb;
    display.referenceLevelDb = shared.referenceLevelDb;
    display.absoluteFrequency = shared.absoluteFrequency;
    display.grid = shared.grid;
    display.waveformAutoFit = shared.waveformAutoFit;
    display.colorScale = shared.colorScale;
    display.waveformMin = shared.waveformMin;
    display.waveformMax = shared.waveformMax;
    display.psdMin = shared.psdMin;
    display.psdMax = shared.psdMax;
    display.auxiliaryMin = shared.auxiliaryMin;
    display.auxiliaryMax = shared.auxiliaryMax;
    display.psdFromSelection =
        sharedPsdFromSelectionPreference_ && hasActiveSegment(file);
    if (file.metadata.availability.status != LoadStatus::Loading)
      file.view = clampRange(file.view, file.metadata, display.stftSize,
                             display.psdSize);
  }
}

void MainWindow::scheduleRightSidebarSettingsSave() {
  if (rightSidebarSaveTimer_ &&
      qApp->property("uiStatePersistenceEnabled").toBool())
    rightSidebarSaveTimer_->start();
}

void MainWindow::saveRightSidebarSettings() {
  if (!qApp->property("uiStatePersistenceEnabled").toBool()) return;
  QSettings settings;
  settings.beginGroup(QStringLiteral("rightSidebar"));
  for (const auto& file : session_.project().files) {
    const auto key = sidebarSettingsKey(file.metadata);
    if (key.isEmpty()) continue;
    QVariantMap values{
        {QStringLiteral("sourcePath"), sidebarSourcePath(file.metadata)},
        {QStringLiteral("effectiveBandwidthHz"),
         file.metadata.effectiveBandwidthHz}};
    settings.setValue(key, values);
  }
  if (const auto* file = session_.activeFile()) {
    const auto& display = file->display;
    settings.setValue(
        QStringLiteral("globalSettings"),
        QVariantMap{
            {QStringLiteral("mainMode"), static_cast<int>(display.mainMode)},
            {QStringLiteral("auxiliaryMode"),
             static_cast<int>(display.auxiliaryMode)},
            {QStringLiteral("waveformMode"),
             static_cast<int>(display.waveformMode)},
            {QStringLiteral("palette"), static_cast<int>(display.palette)},
            {QStringLiteral("stftSize"), display.stftSize},
            {QStringLiteral("psdSize"), display.psdSize},
            {QStringLiteral("psdSettings"),
             QJsonDocument(encodePsdSettings(display.psd))
                 .toJson(QJsonDocument::Compact)},
            {QStringLiteral("spectrogramSettings"),
             QJsonDocument(
                 encodeSpectralParameters(display.spectrogram.parameters))
                 .toJson(QJsonDocument::Compact)},
            {QStringLiteral("dynamicRangeDb"), display.dynamicRangeDb},
            {QStringLiteral("referenceLevelDb"), display.referenceLevelDb},
            {QStringLiteral("absoluteFrequency"), display.absoluteFrequency},
            {QStringLiteral("grid"), display.grid},
            {QStringLiteral("colorScale"), display.colorScale},
            {QStringLiteral("psdFromSelection"),
             sharedPsdFromSelectionPreference_},
            {QStringLiteral("waveformMin"), display.waveformMin},
            {QStringLiteral("waveformMax"), display.waveformMax},
            {QStringLiteral("waveformAutoFit"), display.waveformAutoFit},
            {QStringLiteral("psdMin"), display.psdMin},
            {QStringLiteral("psdMax"), display.psdMax}});
  }
  settings.endGroup();
  settings.sync();
}

void MainWindow::updateRecentProjectsMenu() {
  if (!recentProjectsMenu_) return;
  recentProjectsMenu_->clear();
  const auto paths =
      QSettings().value(QStringLiteral("recentProjects")).toStringList();
  for (const auto& path : paths) {
    const QFileInfo info(path);
    const QString displayName =
        info.fileName().compare("project.json", Qt::CaseInsensitive) == 0
            ? QFileInfo(info.absolutePath()).fileName()
            : info.completeBaseName();
    auto* action = recentProjectsMenu_->addAction(
        displayName.isEmpty() ? path : displayName);
    action->setObjectName(QStringLiteral("recentProjectAction"));
    action->setToolTip(path);
    action->setStatusTip(path);
    action->setEnabled(QFileInfo::exists(path));
    connect(action, &QAction::triggered, this,
            [this, path] { openProject(path); });
  }
  if (paths.isEmpty()) {
    auto* empty = recentProjectsMenu_->addAction("暂无历史工程");
    empty->setEnabled(false);
  } else {
    recentProjectsMenu_->addSeparator();
    auto* clear = recentProjectsMenu_->addAction("清除历史工程");
    connect(clear, &QAction::triggered, this, [this] {
      QSettings().remove(QStringLiteral("recentProjects"));
      updateRecentProjectsMenu();
    });
  }
}

void MainWindow::rememberProject(const QString& path) {
  if (!qApp->property("uiStatePersistenceEnabled").toBool()) return;
  const auto absolutePath = QFileInfo(path).absoluteFilePath();
  auto paths =
      QSettings().value(QStringLiteral("recentProjects")).toStringList();
  paths.removeAll(absolutePath);
  paths.prepend(absolutePath);
  while (paths.size() > 10) paths.removeLast();
  QSettings().setValue(QStringLiteral("recentProjects"), paths);
  updateRecentProjectsMenu();
}

void MainWindow::updateTree() {
  // A committed editor or worker publication can request a rebuild between a
  // native press and release. Keep the original item widget until its click is
  // delivered; a zero-delay refresh alone does not protect that interval.
  if (treePointerPressed_) {
    treeRefreshAfterRelease_ = true;
    return;
  }
  const auto* file = session_.activeFile();
  const auto context =
      QString::number(session_.projectGeneration()) + ":" +
      (file ? q(file->metadata.id) + ":" +
                  QString::number(file->metadata.sampleFormat.selectedChannel)
            : QString{});
  if (treeAnnotationContext_ != context) {
    treeAnnotationContext_ = context;
    treeAnnotationLimits_.fill(kTreeAnnotationBatchSize);
    treeRevealedAnnotationIds_.clear();
    treeActiveAnnotationId_.clear();
    treeStructure_.clear();
    treeRevealPending_ = true;
  }
  const auto active = file ? file->activeMarkId : std::string{};
  if (treeActiveAnnotationId_ != active) {
    treeActiveAnnotationId_ = active;
    treeRevealPending_ = true;
  }
  if (!active.empty()) treeRevealedAnnotationIds_.insert(active);
  if (tree_->topLevelItemCount() == 1 && treeStructure_ == treeStructure()) {
    syncTreeState();
    return;
  }
  if (treeRebuildPending_) return;
  treeRebuildPending_ = true;
  QTimer::singleShot(0, this, [this] {
    treeRebuildPending_ = false;
    rebuildTree();
  });
}

void MainWindow::syncTreeState() {
  if (!tree_->topLevelItemCount()) return;
  const QSignalBlocker block(tree_);
  auto* root = tree_->topLevelItem(0);
  root->setText(0, "▾ ◧ " + q(session_.project().name));
  for (std::size_t index = 0; index < session_.project().files.size();
       ++index) {
    const auto& file = session_.project().files[index];
    auto* item = root->child(static_cast<int>(index));
    if (!item) continue;
    item->setText(0, "◈ " + q(file.metadata.name));
    if (file.metadata.id != session_.project().activeFileId) continue;
    auto* marks = childOfKind(item, "marks");
    auto* channels = childOfKind(item, "channels");
    if (!marks || !channels) continue;
    const auto counts = annotationCounts(file);
    if (auto* controls = tree_->itemWidget(marks->child(0), 0)) {
      controls->findChild<QLabel*>("treeAnnotationCounts")
          ->setText(annotationCountText(file));
      controls->findChild<QPushButton*>("treeSelectAll")
          ->setEnabled(counts.matching != 0);
      controls->findChild<QPushButton*>("treeDeleteMarks")
          ->setEnabled(counts.selected != 0);
    }
    std::array<std::size_t, 3> matching{}, total{};
    for (const auto& annotation : file.marks) {
      const auto kind = static_cast<int>(annotation.sourceKind);
      if (annotationInContext(file, annotation)) ++total[kind];
      if (annotationVisible(file, annotation)) ++matching[kind];
    }
    for (int group = 1; group < marks->childCount(); ++group) {
      auto* node = marks->child(group);
      if (node->data(0, Qt::UserRole).toString() != "annotationSource")
        continue;
      const auto kind = node->data(0, Qt::UserRole + 1).toInt();
      const auto title = kind == 0   ? "原始标签"
                         : kind == 1 ? "手动标记"
                                     : "算法结果";
      node->setText(0, QString("%1 (%2/%3)")
                           .arg(title)
                           .arg(matching[kind])
                           .arg(total[kind]));
    }
    channels->setText(0, QString("窄带通道 (%1)").arg(file.channels.size()));
    for (std::size_t i = 0; i < file.channels.size(); ++i) {
      const auto& channel = file.channels[i];
      if (auto* node = channels->child(static_cast<int>(i)))
        node->setText(
            0, "◇ " + q(channel.name) + " · " +
                   coordinate(channel.centerFrequencyHz, channel.bandwidthHz,
                              false) +
                   (channel.sourceTime.end > availableSamples(file.metadata)
                        ? " · 来源未读入"
                        : ""));
    }
  }
  syncTreeAnnotationStyles();
  revealTreeAnnotation();
}

void MainWindow::syncTreeAnnotationStyles() {
  const auto* file = session_.activeFile();
  if (!file) return;
  const QSignalBlocker blocker(tree_);
  std::unordered_map<std::string, const Annotation*> byId;
  for (const auto& annotation : file->marks)
    byId.emplace(annotation.id, &annotation);
  const std::unordered_set<std::string> selected(file->selectedMarkIds.begin(),
                                                 file->selectedMarkIds.end());
  for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
    auto* node = *it;
    if (node->data(0, Qt::UserRole).toString() != "mark") continue;
    const auto id = node->data(0, Qt::UserRole + 1).toString().toStdString();
    const auto found = byId.find(id);
    if (found == byId.end()) continue;
    const auto& annotation = *found->second;
    const bool isSelected = selected.contains(id),
               current = file->activeMarkId == id,
               hovered = file->hoveredMarkId == id;
    const auto* segment = activeSegment(annotation);
    const auto segmentText =
        segment ? current ? QString(" · 第 %1/%2 段")
                                .arg(segment - annotation.segments.data() + 1)
                                .arg(annotation.segments.size())
                          : QString(" · %1段").arg(annotation.segments.size())
                : QString(" · 时间未定位");
    node->setText(0,
                  (current ? "▣ " : "▱ ") + q(annotation.name) + segmentText);
    node->setToolTip(0, "单击选择 · Ctrl 多选 · Shift 连选 · 双击定位\n" +
                            q(annotation.id) + "\n" + segmentText.trimmed());
    node->setForeground(0, annotationSourceColor(annotation.sourceKind));
    node->setBackground(0, current      ? QBrush(QColor("#1a3f56"))
                           : isSelected ? QBrush(QColor("#1a3f56"))
                           : hovered    ? QBrush(QColor("#22425a"))
                                        : QBrush{});
    auto font = tree_->font();
    font.setBold(current);
    node->setFont(0, font);
    node->setData(0, Qt::UserRole + 3,
                  (isSelected ? 1 : 0) | (current ? 2 : 0) | (hovered ? 4 : 0));
    node->setSelected(isSelected);
  }
  tree_->setProperty("hoveredAnnotationId", q(file->hoveredMarkId));
  tree_->viewport()->update();
}

void MainWindow::revealTreeAnnotation() {
  if (!treeRevealPending_) return;
  const auto* file = session_.activeFile();
  if (!file || file->activeMarkId.empty()) {
    treeRevealPending_ = false;
    return;
  }
  for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
    auto* node = *it;
    if (node->data(0, Qt::UserRole).toString() != "mark" ||
        node->data(0, Qt::UserRole + 1).toString() != q(file->activeMarkId))
      continue;
    for (auto* parent = node->parent(); parent; parent = parent->parent())
      parent->setExpanded(true);
    tree_->setCurrentItem(node, 0, QItemSelectionModel::NoUpdate);
    resizeTreeToContents();
    tree_->scrollToItem(node, QAbstractItemView::EnsureVisible);
    if (auto* scroll = qobject_cast<QScrollArea*>(resourceBody_)) {
      const auto point = tree_->viewport()->mapTo(
          scroll->widget(), tree_->visualItemRect(node).center());
      scroll->ensureVisible(point.x(), point.y(), 0, 40);
    }
    treeRevealPending_ = false;
    return;
  }
}

void MainWindow::refreshAnnotationHover() {
  if (destroying_) return;
  if (main_) main_->refreshAnnotationOverlay();
  if (annotations_) annotations_->refreshHover();
  syncTreeAnnotationStyles();
}

void MainWindow::refresh() {
  const auto* detectionFile = session_.activeFile();
  const bool detectionReady =
      detectionFile && !detectionFile->metadata.demo &&
      detectionFile->metadata.availability.status != LoadStatus::Loading &&
      detectionFile->metadata.availability.status != LoadStatus::Failed &&
      availableSamples(detectionFile->metadata) > 0 && !projectPath_.isEmpty();
  if (widebandDetect_) widebandDetect_->setEnabled(detectionReady);
  if (detectionAction_) detectionAction_->setEnabled(detectionReady);
  if (refreshing_) return;
  refreshing_ = true;
  auto* file = session_.activeFile();
  if (file) session_.annotations().reconcileSelection(*file);
  projectLabel_->setText(q(session_.project().name));
  const bool narrowOpen = session_.project().narrowbandWorkspaceOpen &&
                          session_.activeChannel() != nullptr;
  const auto settingsContext = QString::number(session_.projectGeneration()) + "/" +
      q(narrowOpen ? session_.activeChannel()->id : file ? file->metadata.id : std::string{});
  if (settingsContext != settingsSourceContext_) {
    if (settingsPopovers_) settingsPopovers_->closeForContextChange();
    settingsSourceContext_ = settingsContext;
  }
  if (parameterInputPolicy_)
    parameterInputPolicy_->setContext(
        QString::number(session_.projectGeneration()) + "/" +
        q(narrowOpen ? session_.activeChannel()->id
          : file     ? file->metadata.id
                     : std::string{}));
  workspaceStack_->setCurrentIndex(narrowOpen ? 1 : 0);
  workspaceModeButton_->setChecked(!narrowOpen);
  if (narrowband_ && narrowOpen) narrowband_->refreshFromSession();
  const bool projectReady =
      !projectPath_.isEmpty() || !session_.project().files.empty();
  for (auto* control : std::array<QWidget*, 14>{
           mainMode_, auxMode_, waveformMode_, propertyPalette_,
           dynamic_, reference_, freqMode_, grid_, colorScale_, psd_, stft_,
           psdScope_, extract_, effectiveBandwidth_})
    control->setEnabled(file != nullptr);
  waveformMode_->setEnabled(file && !file->metadata.demo &&
                            file->display.auxiliaryMode ==
                                AuxiliaryMode::Waveform);
  for (auto* button : auxiliaryButtons_) button->setEnabled(file != nullptr);
  for (auto* button : mainButtons_) button->setEnabled(file != nullptr);
  for (const auto& id : {"navMaximize", "auxMaximize", "specMaximize"})
    findChild<QPushButton*>(id)->setEnabled(file != nullptr);
  addSignalAction_->setEnabled(projectReady);
  saveAction_->setEnabled(!projectPath_.isEmpty() ||
                          !session_.project().files.empty());
  removeAction_->setEnabled(file != nullptr);
  deleteAction_->setEnabled(file && !file->selectedMarkIds.empty());
  backAction_->setEnabled(session_.canBack());
  forwardAction_->setEnabled(session_.canForward());
  empty_->setVisible(!file);
  if (!file) empty_->raise();
  if (auto* title = findChild<QLabel*>("emptyWorkflowTitle"))
    title->setText(projectReady ? "工程已就绪 · 等待添加信号"
                                : "开始宽带数据分析");
  if (auto* help = findChild<QLabel*>("emptyWorkflowHelp"))
    help->setText(
        projectReady
            ? "当前工程：" + q(session_.project().name) +
                  "。添加信号后即可进行宽带数据操作。"
            : "第一步：新建工程（自动创建工程文件夹）或打开已有工程。");
  if (auto* actions = findChild<QWidget*>("emptyProjectActions"))
    actions->setVisible(!projectReady);
  if (auto* addButton = findChild<QPushButton*>("emptyAddSignal"))
    addButton->setVisible(projectReady);
  if (auto* addButton = findChild<QPushButton*>("projectAddSignal")) {
    addButton->setVisible(projectReady);
    addButton->setEnabled(projectReady);
  }
  if (auto* saveButton = findChild<QPushButton*>("projectSave"))
    saveButton->setEnabled(saveAction_->isEnabled());
  if (file) {
    const bool real =
        file->metadata.sampleFormat.structure == SampleStructure::Real;
    if (real && file->display.waveformMode == WaveformMode::Q)
      file->display.waveformMode = WaveformMode::I;
    if (auto* model = qobject_cast<QStandardItemModel*>(waveformMode_->model()))
      model->item(1)->setEnabled(!real);
    const QStringList waveformTitles{"I 分量（ADC 计数）", "Q 分量（ADC 计数）",
                                     "幅度（RMS，ADC 计数）",
                                     "幅度包络（ADC 计数）"};
    for (int i = 0; i < 4; ++i)
      waveformMode_->setItemText(
          i, sourceUnits(waveformTitles[i], file->metadata));
    const auto& display = file->display;
    mainMode_->setCurrentIndex(static_cast<int>(display.mainMode));
    auxMode_->setCurrentIndex(static_cast<int>(display.auxiliaryMode));
    propertyPalette_->setCurrentIndex(static_cast<int>(display.palette));
    dataSourceStatus_->setText(
        file->metadata.demo ? "演示数据 · 未运行 DSP"
        : file->metadata.availability.status == LoadStatus::Loading
            ? real ? "实际 ADC · 正在读入" : "实际 IQ · 正在读入"
        : file->metadata.availability.status == LoadStatus::Failed
            ? "来源异常 · 分析不可用"
        : real ? "实际 ADC · 单边 FFT 已启用"
               : "实际 IQ · FFT 已启用");
    dataSourceStatus_->setToolTip(
        file->metadata.demo
            ? "当前文件使用原型演示数据"
            : QString::fromStdString(formatId(file->metadata.sampleFormat)) +
                  " 数据来自磁盘；波形、PSD、时频图均使用实际选择通道");
    {
      const QSignalBlocker blocker(reference_);
      for (int i = 0; i < reference_->count(); ++i)
        reference_->setItemText(i, reference_->itemText(i).replace(
                                       QRegularExpression("\\s*dB.*$"),
                                       " " + powerUnit(file->metadata)));
    }
    displayComboValue(dynamic_, display.dynamicRangeDb, " dB");
    displayComboValue(reference_, display.referenceLevelDb,
                      " " + powerUnit(file->metadata));
    waveformMode_->setCurrentIndex(static_cast<int>(display.waveformMode));
    const double minimumBandwidth = file->metadata.sampleRateHz / 65536.0 / 1e6;
    const double maximumBandwidth =
        file->metadata.sampleRateHz / (real ? 2e6 : 1e6);
    if (effectiveBandwidth_->minimum() != minimumBandwidth ||
        effectiveBandwidth_->maximum() != maximumBandwidth)
      effectiveBandwidth_->setRange(minimumBandwidth, maximumBandwidth);
    displayParameterValue(effectiveBandwidth_,
                          file->metadata.effectiveBandwidthHz / 1e6);
    const auto* channel = narrowOpen ? session_.activeChannel() : nullptr;
    freqMode_->setCurrentIndex(
        (channel ? channel->absoluteFrequencyLabels : display.absoluteFrequency)
            ? 0
            : 1);
    const auto* narrowGrid =
        narrowOpen ? narrowband_->findChild<QCheckBox*>("narrowbandGrid")
                   : nullptr;
    grid_->setChecked(narrowGrid ? narrowGrid->isChecked() : display.grid);
    colorScale_->setChecked(display.colorScale);
    psdScope_->setCurrentIndex(
        static_cast<int>(channel ? channel->psd.scope : display.psd.scope));
    psdParameters_->setParameters(
        channel ? channel->psd.parameters : display.psd.parameters,
        channel ? channel->psd.statistic : display.psd.statistic);
    stftParameters_->setParameters(channel ? channel->spectrogram.parameters
                                           : display.spectrogram.parameters);
    const auto showFft = [](QComboBox* combo, int n) {
      if (combo->property("legacyFft").toBool()) {
        combo->removeItem(0);
        combo->setProperty("legacyFft", false);
      }
      const auto text = QString::number(n);
      if (combo->findText(text) < 0) {
        combo->insertItem(0, text, n);
        combo->setProperty("legacyFft", true);
        combo->setItemData(0,
                           "旧 FFT 点数保留；请选择 32–65536 中的新档位后分析",
                           Qt::ToolTipRole);
      }
      combo->setCurrentText(text);
    };
    showFft(psd_, channel ? channel->psdFftSize : display.psdSize);
    showFft(stft_, channel ? channel->stftFftSize : display.stftSize);
    const bool framePsd =
        session_.linkedCursor(channel ? channel->id : file->metadata.id)
            .framePsd;
    if (framePsd)
      showFft(psd_, channel ? channel->stftFftSize : display.stftSize);
    psd_->setEnabled(!framePsd);
    psdScope_->setEnabled(!framePsd);
    psdParameters_->setEnabled(!framePsd);
    psd_->setToolTip(framePsd ? "驻留帧谱有效点数随 STFT；平均谱设置保留"
                              : "当前可见频段内的分析点数");
    for (int i = 0; i < 2; ++i) {
      auxiliaryButtons_[i]->setChecked(i == auxMode_->currentIndex());
      mainButtons_[i]->setChecked(i == mainMode_->currentIndex());
    }
    const double duration = static_cast<double>(file->metadata.sampleCount) /
                            file->metadata.sampleRateHz;
    scope_->setText(QString("%1 · Fₛ %2 MS/s · fc %3 MHz")
                        .arg(q(file->metadata.name),
                             number(file->metadata.sampleRateHz / 1e6),
                             number(file->metadata.centerFrequencyHz / 1e6)));
    const bool loading =
        file->metadata.availability.status == LoadStatus::Loading;
    stopSourceLoad_->setVisible(loading);
    effectiveBandwidth_->setEnabled(!loading);
    navStatus_->setText(
        loading
            ? QString("已读 %1 / %2 样本")
                  .arg(file->metadata.availability.availableSamples)
                  .arg(file->metadata.sampleCount)
            : "0–" +
                  number(static_cast<double>(availableSamples(file->metadata)) /
                         file->metadata.sampleRateHz) +
                  " s");
    auxStatus_->setText(auxiliary_->statusText());
    rangeTag_->setText("动态 " + number(display.dynamicRangeDb) + " dB");
    const bool waterfall = display.mainMode == MainMode::Waterfall;
    specMode_->setText("正在框选信号 · 右键或 Esc 退出");
    specMode_->setVisible(main_->isCreating() && width() > 990);
    specAxis_->setText(waterfall ? "X 频率 · Y 时间↓" : "X 时间 · Y 频率");
    specAxis_->setToolTip(waterfall ? "X 轴：频率；Y 轴：时间；颜色：功率"
                                    : "X 轴：时间；Y 轴：频率；颜色：功率");
    fileValues_[0]->setText(q(file->metadata.name));
    fileValues_[1]->setText(number(file->metadata.sampleRateHz / 1e6) +
                            " MS/s");
    fileValues_[2]->setText(number(file->metadata.centerFrequencyHz / 1e6) +
                            " MHz");
    fileValues_[3]->setText("0–" + number(duration) + " s");
    viewData_->setText(
        "| 视图 ΔT " +
        coordinate(
            static_cast<double>(file->view.time.end - file->view.time.begin) /
                file->metadata.sampleRateHz,
            0, true) +
        " · ΔF " +
        coordinate(file->view.frequency.upperHz - file->view.frequency.lowerHz,
                   0, false));
    const auto* mark = findMark(*file, file->activeMarkId);
    const auto* segment = mark ? activeSegment(*mark) : nullptr;
    extract_->setEnabled(!file->selectedMarkIds.empty());
    extract_->setToolTip(
        file->selectedMarkIds.size() > 1
            ? "打开配置后明确选择一个来源对象；不会合并所选对象。"
            : "打开通道配置；质量标记为可选，按实际来源与 DSP 条件校验。");
    if (mark) {
      const auto geometry =
          segment ? segment->range : mark->observation.value_or(file->view);
      selectionData_->setText(
          "| 标记 " + q(mark->name) + " · " +
          coordinate(
              (geometry.frequency.lowerHz + geometry.frequency.upperHz) / 2, 0,
              false));
    } else {
      selectionData_->setText("| 无信号标记");
    }
    selectedCount_->setText(annotationCountText(*file) +
                            (segment
                                 ? QString(" · 当前第 %1/%2 段")
                                       .arg(segment - mark->segments.data() + 1)
                                       .arg(mark->segments.size())
                             : mark ? QString(" · 时间未定位 / 0 段")
                                    : QString{}));
    rename_->setEnabled(mark &&
                        session_.annotations().canEdit(*file, mark->id));
    locate_->setEnabled(mark != nullptr);
    delete_->setEnabled(!file->selectedMarkIds.empty());
    if (cursorFileId_ != q(file->metadata.id)) {
      cursorFileId_ = q(file->metadata.id);
      cursorSample_ = file->view.time.begin +
                      (file->view.time.end - file->view.time.begin) / 2;
      cursorFrequency_ =
          real ? (file->view.frequency.lowerHz + file->view.frequency.upperHz) /
                     2
               : file->metadata.centerFrequencyHz;
    }
    updateCursor(cursorSample_, cursorFrequency_);
  } else {
    stopSourceLoad_->hide();
    scope_->setText("未添加 IQ 文件");
    navStatus_->setText("—");
    auxStatus_->setText("—");
    for (auto* value : fileValues_) value->setText("—");
    viewData_->clear();
    cursorData_->clear();
    selectionData_->setText("无信号标记");
    statusTime_->clear();
    statusFrequency_->clear();
    statusFile_->clear();
    cursorFileId_.clear();
  }
  updatePropertyContext();
  updateBottom();
  updateTree();
  for (auto* plot : {navigation_, auxiliary_, main_}) plot->syncState();
  updateChartControls(narrowOpen);
  refreshing_ = false;
  updatePowerFit();
}

void MainWindow::activateTreeChannel(const QString& channelId) {
  cancelInteractions(false);
  if (!session_.activateChannel(channelId.toStdString())) return;
  session_.project().narrowbandWorkspaceOpen = true;
  workspaceStack_->setCurrentIndex(1);
  refresh();
  log("已打开窄带通道 · 真实 DDC 按可见时段按需计算");
}

void MainWindow::locateActiveChannelSource() {
  const auto* channel = session_.activeChannel();
  if (!channel) return;
  const auto channelId = channel->id;
  const auto* file = session_.fileForChannel(channelId);
  if (!file) return;
  const auto fileId = file->metadata.id;
  const auto markId = channel->sourceMarkId;
  cancelInteractions(false);
  if (!session_.activateFile(fileId) || !session_.focusMark(markId)) return;
  session_.project().narrowbandWorkspaceOpen = false;
  workspaceStack_->setCurrentIndex(0);
  refresh();
  showInspectorPage(2, true);
  log("已返回来源文件并定位源标记；再次打开通道可恢复窄带视图");
}

void MainWindow::showChannelDialog(const QString& channelId) {
  // Opening a modal operation must leave a parent-window annotation draft
  // intact. Only chart gestures need to end here.
  if (main_) main_->cancelGesture(false);
  auto* file = session_.activeFile();
  const bool editing = !channelId.isEmpty();
  Channel original;
  if (editing) {
    file = session_.fileForChannel(channelId.toStdString());
    if (!file) return;
    const auto found =
        std::find_if(file->channels.begin(), file->channels.end(),
                     [&](const Channel& value) {
                       return value.id == channelId.toStdString();
                     });
    if (found == file->channels.end()) return;
    original = *found;
  }
  if (!file) return;
  const auto fileId = file->metadata.id;
  const auto generation = session_.projectGeneration();
  const auto sampleChannel = file->metadata.sampleFormat.selectedChannel;
  const auto resolveFile = [this, fileId, generation,
                            sampleChannel]() -> FileState* {
    if (generation != session_.projectGeneration()) return nullptr;
    for (auto& current : session_.project().files)
      if (current.metadata.id == fileId &&
          current.metadata.sampleFormat.selectedChannel == sampleChannel)
        return &current;
    return nullptr;
  };

  auto* dialog = new QDialog(this);
  dialog->setObjectName("channelConfigDialog");
  dialog->setWindowTitle(editing ? "修改窄带通道" : "创建窄带通道");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setModal(true);
  dialog->setMinimumWidth(620);
  auto* outer = new QVBoxLayout(dialog);
  outer->setContentsMargins(20, 17, 20, 16);
  outer->setSpacing(9);
  auto* intro = label(
      editing ? "保留创建时不可变来源快照；参数作为一次配置事务应用。"
              : "三类来源使用同一通道配置；质量标记可选，不作为创建前置条件。",
      {}, "help");
  intro->setWordWrap(true);
  outer->addWidget(intro);
  auto* form = new QFormLayout;
  form->setSpacing(7);
  form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  auto* name = new QLineEdit(
      editing ? q(original.name)
              : QString("窄带通道 %1").arg(file->channels.size() + 1));
  name->setObjectName("channelName");
  auto* sourceMark = new QComboBox;
  sourceMark->setObjectName("channelSourceMark");
  sourceMark->setProperty("preserveParameterDrafts", true);
  sourceMark->setMinimumContentsLength(22);
  sourceMark->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  const bool multiple = !editing && file->selectedMarkIds.size() > 1;
  sourceMark->addItem(
      multiple ? "当前多选；请明确选择一个来源对象" : "请选择一个来源对象",
      QString{});
  const auto addSource = [&](const Annotation& annotation) {
    if (!annotationInContext(*file, annotation) ||
        sourceMark->findData(q(annotation.id)) >= 0)
      return;
    const auto kind = annotation.sourceKind == AnnotationSource::Manual ? "手动"
                      : annotation.sourceKind == AnnotationSource::Algorithm
                          ? "算法"
                          : "原始标签";
    sourceMark->addItem(
        QString("%1 · %2 · %3").arg(q(annotation.name), kind, q(annotation.id)),
        q(annotation.id));
  };
  if (editing) {
    if (const auto* annotation = findAnnotation(*file, original.sourceMarkId))
      addSource(*annotation);
    if (sourceMark->findData(q(original.sourceMarkId)) < 0)
      sourceMark->addItem(original.sourceSnapshot
                              ? q(original.sourceSnapshot->name)
                              : q(original.sourceMarkId),
                          q(original.sourceMarkId));
  } else {
    if (!multiple)
      if (const auto* preferred = findAnnotation(*file, file->activeMarkId))
        addSource(*preferred);
    for (const auto& id : file->selectedMarkIds) {
      if (sourceMark->count() > 200) break;
      if (const auto* annotation = findAnnotation(*file, id))
        addSource(*annotation);
    }
    if (!multiple)
      for (const auto& annotation : file->marks) {
        if (sourceMark->count() > 200) break;
        addSource(annotation);
      }
  }
  sourceMark->setEnabled(!editing);
  sourceMark->setToolTip(
      "一次明确选择一个来源；多选不会自动合并或取最后一行。来源列表最多显示 "
      "200 个对象，可从标记表重新选择。");
  sourceMark->setCurrentIndex(
      editing    ? sourceMark->findData(q(original.sourceMarkId))
      : multiple ? 0
                 : std::max(0, sourceMark->findData(q(file->activeMarkId))));
  auto* sourceSegment = new QComboBox;
  sourceSegment->setObjectName("channelSourceSegment");
  sourceSegment->setProperty("preserveParameterDrafts", true);
  auto* bindingKind = label({}, "channelBindingKind", "help");
  bindingKind->setWordWrap(true);
  auto* sourceSummary = label({}, "channelSourceSummary", "help");
  sourceSummary->setWordWrap(true);
  auto* begin = new SampleTimeEdit(0, file->metadata.sampleRateHz);
  auto* end = new SampleTimeEdit(0, file->metadata.sampleRateHz);
  begin->setObjectName("channelRangeStartMilliseconds");
  end->setObjectName("channelRangeEndMilliseconds");
  begin->setEnabled(!editing);
  end->setEnabled(!editing);
  auto* actualRange = label({}, "channelActualSourceRange", "help");
  actualRange->setWordWrap(true);
  auto* center = new ParameterDoubleSpinBox;
  center->setObjectName("channelCenterMHz");
  center->setRange(-1e6, 1e6);
  center->setDecimals(9);
  center->setSingleStep(.1);
  center->setSuffix(" MHz");
  auto* bandwidth = new ParameterDoubleSpinBox;
  bandwidth->setObjectName("channelBandwidthMHz");
  bandwidth->setRange(0, 1e6);
  bandwidth->setDecimals(9);
  bandwidth->setSingleStep(.1);
  bandwidth->setSuffix(" MHz");
  auto* outputRate = new ParameterDoubleSpinBox;
  outputRate->setObjectName("channelOutputRateMSps");
  outputRate->setRange(0, 4e9 / 1e6);
  outputRate->setDecimals(9);
  outputRate->setSingleStep(1);
  outputRate->setSuffix(" MS/s");
  auto* timeMode = new QComboBox;
  timeMode->setObjectName("channelTimeScope");
  timeMode->setEnabled(false);
  auto* usePrefix = new QPushButton("显式使用可用前缀");
  usePrefix->setObjectName("channelUseAvailablePrefix");
  usePrefix->setProperty("preserveParameterDrafts", true);
  usePrefix->setEnabled(!editing);
  auto* filter = new QComboBox;
  filter->setObjectName("channelFilter");
  filter->addItems({"快速预览 · 40 dB", "标准 · 60 dB", "高抑制 · 80 dB"});
  filter->setCurrentIndex(editing ? static_cast<int>(original.filter)
                                  : static_cast<int>(ChannelFilter::Standard));
  auto* preserveTime = new QCheckBox("保持源文件绝对时间显示");
  preserveTime->setObjectName("channelPreserveSourceTime");
  preserveTime->setChecked(!editing || original.preserveSourceTime);
  auto* autoOpen = new QCheckBox("创建后打开窄带工作区");
  autoOpen->setObjectName("channelAutoOpen");
  autoOpen->setChecked(true);
  form->addRow("通道名称", name);
  form->addRow("来源对象（单选）", sourceMark);
  form->addRow("来源当前段", sourceSegment);
  form->addRow("绑定方式", bindingKind);
  form->addRow("来源说明", sourceSummary);
  form->addRow("提取起始 / ms", begin);
  form->addRow("提取结束 / ms", end);
  form->addRow("实际提取范围", actualRange);
  form->addRow("射频中心", center);
  form->addRow("有效带宽", bandwidth);
  form->addRow("输出采样率", outputRate);
  form->addRow("提取时段", timeMode);
  form->addRow(usePrefix);
  form->addRow("FIR 滤波", filter);
  form->addRow("时间坐标", preserveTime);
  outer->addLayout(form);
  auto* preview = label({}, "channelDspPlanPreview", "help");
  preview->setWordWrap(true);
  preview->setMinimumHeight(52);
  preview->setStyleSheet(
      "background:#0c1a2a;border:1px solid #29445e;padding:8px;color:#95c9dc;");
  outer->addWidget(preview);
  auto* validation = label({}, "channelValidation");
  validation->setWordWrap(true);
  validation->setStyleSheet("color:#ff9b8d;");
  outer->addWidget(validation);
  outer->addWidget(autoOpen);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  auto* confirm = buttons->button(QDialogButtonBox::Ok);
  confirm->setText(editing ? "应用通道参数" : "创建通道");
  confirm->setObjectName("confirmChannelConfig");
  confirm->setAutoDefault(false);
  buttons->button(QDialogButtonBox::Cancel)->setText("取消");
  buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
  outer->addWidget(buttons);
  auto request = std::make_shared<AnnotationChannelRequest>();
  // MHz controls are a rounded presentation. Keep the exact physical edges
  // until a committed control change explicitly replaces them.
  auto frequency = std::make_shared<FrequencyRange>();
  const auto readRequest = [request, frequency, name, begin, end, outputRate,
                            filter, preserveTime] {
    auto current = *request;
    current.name = name->text().trimmed().toStdString();
    current.range = {{begin->sampleValue(), end->sampleValue()}, *frequency};
    current.outputSampleRateHz = outputRate->value() * 1e6;
    current.filter = static_cast<ChannelFilter>(filter->currentIndex());
    current.preserveSourceTime = preserveTime->isChecked();
    return current;
  };
  const auto validate = [resolveFile, editing, original, begin, end,
                         readRequest, actualRange, preview, validation,
                         confirm]() -> bool {
    QString reason, field;
    auto* currentFile = resolveFile();
    const auto current = readRequest();
    Channel candidate = editing ? original : Channel{};
    if (!currentFile) {
      field = "source";
      reason = "工程、文件或样本通道已切换，请重新打开配置。";
    } else if (!begin->error().isEmpty() || !end->error().isEmpty()) {
      field = !begin->error().isEmpty() ? "sourceTime.begin" : "sourceTime.end";
      reason = !begin->error().isEmpty() ? begin->error() : end->error();
    } else if (editing) {
      if (const auto checked = validateChannelName(current.name); !checked) {
        field = "name";
        reason = q(checked.error);
      }
      candidate.name = current.name;
      candidate.centerFrequencyHz =
          (current.range.frequency.lowerHz + current.range.frequency.upperHz) /
          2;
      candidate.bandwidthHz =
          current.range.frequency.upperHz - current.range.frequency.lowerHz;
      candidate.outputSampleRateHz = current.outputSampleRateHz;
      candidate.filter = current.filter;
      candidate.preserveSourceTime = current.preserveSourceTime;
    } else {
      const auto checked =
          prepareAnnotationChannel(*currentFile, current, candidate);
      if (!checked) {
        field = q(checked.field);
        reason = q(checked.error);
      }
    }
    actualRange->setProperty("lowerHz", current.range.frequency.lowerHz);
    actualRange->setProperty("upperHz", current.range.frequency.upperHz);
    actualRange->setText(
        QString("[%1, %2) samples · RF %3–%4 MHz")
            .arg(current.range.time.begin)
            .arg(current.range.time.end)
            .arg(current.range.frequency.lowerHz / 1e6, 0, 'g', 12)
            .arg(current.range.frequency.upperHz / 1e6, 0, 'g', 12));
    ChannelDspPlan plan;
    const bool planned =
        reason.isEmpty() && makeChannelDspPlan(currentFile->metadata, candidate,
                                               plan, reason, &field);
    if (planned) {
      std::size_t taps = 0;
      for (const auto& coefficients : plan.stageCoefficients)
        taps += coefficients.size();
      preview->setText(
          QString("来源 IQ → CPU DDC / FIR → 有理重采样\nFs %1 → %2 MS/s · "
                  "过渡带 %3 kHz · FIR %4 taps · L/M=%5/%6 · %7 dB 目标")
              .arg(plan.sourceRateHz / 1e6, 0, 'g', 12)
              .arg(plan.outputRateHz / 1e6, 0, 'g', 12)
              .arg(plan.transitionHz / 1e3, 0, 'g', 8)
              .arg(taps)
              .arg(plan.interpolation)
              .arg(plan.decimation)
              .arg(plan.stopbandAttenuationDb, 0, 'f', 0));
    } else {
      preview->setText(
          "DSP 计划尚不可执行；请根据字段原因调整配置。FIR 保护带 "
          "15%，有理重采样 L、M ≤ 4096。");
    }
    validation->setProperty("field", field);
    validation->setText(reason.isEmpty() ? QString{} : field + "：" + reason);
    confirm->setEnabled(planned && reason.isEmpty());
    return planned && reason.isEmpty();
  };
  const auto loadSource = [resolveFile, generation, editing, original, request,
                           frequency, sourceMark, sourceSegment, bindingKind,
                           sourceSummary, begin, end, center, bandwidth,
                           outputRate, timeMode,
                           validate](const std::string& segmentId = {}) {
    auto* currentFile = resolveFile();
    if (!currentFile) {
      validate();
      return;
    }
    const auto id = sourceMark->currentData().toString().toStdString();
    const auto* annotation = findAnnotation(*currentFile, id);
    *request = annotationChannelRequest(
        *currentFile, id, currentFile->view.time, generation, segmentId);
    if (editing) {
      request->annotationId = original.annotationId;
      request->segmentId = original.segmentId;
      request->sourceIdentity = original.sourceIdentity;
      request->range = {
          original.sourceTime,
          {original.centerFrequencyHz - original.bandwidthHz / 2,
           original.centerFrequencyHz + original.bandwidthHz / 2}};
      request->outputSampleRateHz = original.outputSampleRateHz;
      if (original.sourceSnapshot) {
        request->bindingKind = original.sourceSnapshot->bindingKind;
        request->boundRange = original.sourceSnapshot->boundRange.value_or(
            original.sourceSnapshot->range);
      }
    }
    {
      const QSignalBlocker block(sourceSegment);
      sourceSegment->clear();
      if (annotation && !annotation->segments.empty()) {
        const auto addSegment = [&](const Segment& segment) {
          if (sourceSegment->findData(q(segment.id)) < 0)
            sourceSegment->addItem(QString("%1 · %2–%3 samples")
                                       .arg(q(segment.id))
                                       .arg(segment.range.time.begin)
                                       .arg(segment.range.time.end),
                                   q(segment.id));
        };
        if (const auto* selected = findSegment(*annotation, request->segmentId))
          addSegment(*selected);
        for (const auto& segment : annotation->segments) {
          if (sourceSegment->count() >= 200) break;
          addSegment(segment);
        }
        sourceSegment->setCurrentIndex(
            sourceSegment->findData(q(request->segmentId)));
      } else {
        sourceSegment->addItem("时间未定位；无算法活动段", QString{});
      }
      sourceSegment->setEnabled(!editing && annotation &&
                                annotation->segments.size() > 1);
    }
    const bool userRange =
        request->bindingKind == ChannelBindingKind::UserRange;
    bindingKind->setText(
        userRange ? "UserRange · 人工提取范围；不修改算法活动段或检测证据。"
                  : "LocatedSegment · 明确当前段；可裁剪提取时段。");
    QString sourceText =
        annotation
            ? QString("%1 · %2 · Fs %3 MS/s · 可用前缀 [0, %4) samples")
                  .arg(q(annotation->name),
                       annotation->sourceKind == AnnotationSource::Manual
                           ? "手动"
                       : annotation->sourceKind == AnnotationSource::Algorithm
                           ? "算法"
                           : "原始标签")
                  .arg(currentFile->metadata.sampleRateHz / 1e6, 0, 'g', 12)
                  .arg(availableSamples(currentFile->metadata))
            : "请明确选择一个来源对象。";
    if (annotation)
      sourceText +=
          QString(" · 质量：%1（可选）")
              .arg(annotation->reviewStatus == ReviewStatus::Accepted ? "已采纳"
                   : annotation->reviewStatus == ReviewStatus::Rejected
                       ? "已忽略"
                       : "待判断");
    sourceSummary->setText(sourceText);
    *frequency = request->range.frequency;
    begin->setSampleValue(request->range.time.begin);
    end->setSampleValue(request->range.time.end);
    for (auto* control : {center, bandwidth, outputRate})
      control->setProperty("parameterDraft", false);
    {
      const QSignalBlocker blockCenter(center), blockBandwidth(bandwidth),
          blockRate(outputRate);
      center->setValue((request->range.frequency.lowerHz +
                        request->range.frequency.upperHz) /
                       2e6);
      bandwidth->setValue((request->range.frequency.upperHz -
                           request->range.frequency.lowerHz) /
                          1e6);
      outputRate->setValue(request->outputSampleRateHz / 1e6);
    }
    timeMode->clear();
    timeMode->addItem(userRange ? "人工指定时间；默认当前视区 ∩ 可用前缀"
                                : "当前段内的实际裁剪时段");
    validate();
  };
  // Keep the name separate from loadSource so switching a source never replaces
  // a user-entered channel name.
  loadSource();
  begin->applied = validate;
  end->applied = validate;
  connect(sourceMark, &QComboBox::currentIndexChanged, dialog,
          [loadSource] { loadSource(); });
  connect(sourceSegment, &QComboBox::currentIndexChanged, dialog,
          [sourceSegment, loadSource] {
            loadSource(sourceSegment->currentData().toString().toStdString());
          });
  connect(
      usePrefix, &QPushButton::clicked, dialog,
      [resolveFile, request, begin, end, validate] {
        auto* currentFile = resolveFile();
        if (!currentFile) {
          validate();
          return;
        }
        const auto prefix = availableSamples(currentFile->metadata);
        const auto range =
            request->bindingKind == ChannelBindingKind::LocatedSegment
                ? TimeRange{std::min(request->boundRange.time.begin, prefix),
                            std::min(request->boundRange.time.end, prefix)}
                : TimeRange{0, prefix};
        begin->setSampleValue(range.begin);
        end->setSampleValue(range.end);
        validate();
      });
  connect(name, &QLineEdit::textChanged, dialog, [validate] { validate(); });
  connect(center, &QDoubleSpinBox::valueChanged, dialog,
          [frequency, validate](double value) {
            const auto halfBandwidth =
                (frequency->upperHz - frequency->lowerHz) / 2;
            *frequency = {value * 1e6 - halfBandwidth,
                          value * 1e6 + halfBandwidth};
            validate();
          });
  connect(bandwidth, &QDoubleSpinBox::valueChanged, dialog,
          [frequency, validate](double value) {
            const auto centerHz = frequency->lowerHz +
                                  (frequency->upperHz - frequency->lowerHz) / 2;
            *frequency = {centerHz - value * 5e5, centerHz + value * 5e5};
            validate();
          });
  connect(outputRate, &QDoubleSpinBox::valueChanged, dialog,
          [validate] { validate(); });
  connect(filter, &QComboBox::currentIndexChanged, dialog,
          [validate] { validate(); });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(
      buttons, &QDialogButtonBox::accepted, dialog,
      [this, dialog, editing, channelId, original, resolveFile, readRequest,
       validate, validation, autoOpen] {
        if (!validate()) return;
        auto* currentFile = resolveFile();
        if (!currentFile) return;
        const auto current = readRequest();
        if (editing) {
          auto updated = original;
          updated.name = current.name;
          updated.centerFrequencyHz = (current.range.frequency.lowerHz +
                                       current.range.frequency.upperHz) /
                                      2;
          updated.bandwidthHz =
              current.range.frequency.upperHz - current.range.frequency.lowerHz;
          updated.outputSampleRateHz = current.outputSampleRateHz;
          updated.filter = current.filter;
          updated.preserveSourceTime = current.preserveSourceTime;
          updated.visibleBasebandFrequency = {-updated.outputSampleRateHz / 2,
                                              updated.outputSampleRateHz / 2};
          if (!session_.updateChannel(channelId.toStdString(), updated)) {
            validation->setProperty("field", "channel");
            validation->setText(
                "channel：通道身份、创建时段或快照已变化，请重新打开配置。");
            return;
          }
        } else {
          const auto created = session_.createAnnotationChannel(current);
          if (!created) {
            validation->setProperty("field", q(created.field));
            validation->setText(q(created.field) + "：" + q(created.error));
            return;
          }
        }
        session_.project().narrowbandWorkspaceOpen = autoOpen->isChecked();
        workspaceStack_->setCurrentIndex(autoOpen->isChecked() ? 1 : 0);
        refresh();
        dialog->accept();
        log(editing ? "窄带通道参数已更新；创建来源快照保持不变。"
                    : "窄带通道已创建；真实 CPU DDC 按可见时段按需计算。");
      });
  dialog->open();
}

void MainWindow::updatePropertyContext() {
  const auto* file = session_.activeFile();
  const auto* channel = session_.activeChannel();
  const bool narrow =
      session_.project().narrowbandWorkspaceOpen && channel && file;
  const bool marked = !narrow && file && findMark(*file, file->activeMarkId);
  inspectorButtons_[1]->setEnabled(!narrow);
  inspectorButtons_[3]->setEnabled(file && channel);
  annotations_->inspectorWidget()->setVisible(marked);
  annotationEmpty_->setVisible(!marked);
  annotationActions_->setVisible(marked);
  selectedCount_->setVisible(marked && file->selectedMarkIds.size() > 1);
  const auto selection = file ? q(file->activeMarkId) + ":" + q(file->activeSegmentId) : QString{};
  const auto context = QString::number(session_.projectGeneration()) + ":" +
      (file ? q(file->metadata.id) : QString{}) + ":" +
      (narrow ? q(channel->id) : QStringLiteral("wide"));
  if (context != inspectorContext_) {
    inspectorContext_ = context;
    inspectorSelection_ = selection;
    inspectorManualOverride_ = false;
    if (narrow) showInspectorPage(3);
  } else if (selection != inspectorSelection_) {
    inspectorSelection_ = selection;
    const auto widgets = findChildren<QWidget*>();
    const bool draft = std::any_of(widgets.begin(), widgets.end(), [](QWidget* widget) {
      return widget->property("parameterDraft").toBool();
    });
    if (marked && !draft && !inspectorManualOverride_) showInspectorPage(2);
  }
  if ((!channel && inspectorPages_->currentIndex() == 3) ||
      (narrow && inspectorPages_->currentIndex() == 1)) showInspectorPage(narrow ? 3 : 0);
  if (file) {
    const auto state = file->metadata.demo ? QStringLiteral("演示数据")
        : file->metadata.availability.status == LoadStatus::Loading ? QStringLiteral("正在读取")
        : file->metadata.availability.status == LoadStatus::Partial ? QStringLiteral("可用前缀")
        : file->metadata.availability.status == LoadStatus::Failed ? QStringLiteral("读取异常")
        : QStringLiteral("实际数据");
    sourceSummary_->setText(q(file->metadata.name) + "\n" + state + " · " +
        number(file->metadata.sampleRateHz / 1e6) + " MS/s · " +
        number(file->metadata.centerFrequencyHz / 1e6) + " MHz");
    const bool frame = session_.linkedCursor(narrow ? channel->id : file->metadata.id).framePsd;
    const int stftN = narrow ? channel->stftFftSize : file->display.stftSize;
    const int psdN = frame ? stftN : narrow ? channel->psdFftSize : file->display.psdSize;
    displaySummary_->setText(QString("PSD N %1 · %2\nSTFT N %3 · %4\n电平 %5 / %6 dB")
        .arg(psdN).arg(frame ? "驻留帧谱" : "统计谱").arg(stftN)
        .arg(propertyPalette_->currentText()).arg(QString::number(file->display.referenceLevelDb, 'f', 1))
        .arg(QString::number(file->display.dynamicRangeDb, 'f', 1)));
  } else {
    sourceSummary_->setText("请先新建或打开工程，再添加信号文件。");
    displaySummary_->clear();
  }
  if (file && channel) {
    channelValues_[0]->setText(q(file->metadata.name));
    const auto* sourceMark = findMark(*file, channel->sourceMarkId);
    channelValues_[1]->setText(sourceMark ? q(sourceMark->name)
                                          : "来源标记丢失");
    channelValues_[2]->setText(number(channel->centerFrequencyHz / 1e6) +
                               " MHz");
    channelValues_[3]->setText(number(channel->bandwidthHz / 1e6) + " MHz");
    channelValues_[4]->setText(number(channel->outputSampleRateHz / 1e6) +
                               " MS/s");
    channelValues_[5]->setText(
        timeRange(channel->sourceTime, file->metadata.sampleRateHz));
    channelRevision_->setText(channel->sourceSnapshot
                                  ? q(channel->sourceSnapshot->revisionId)
                                  : "旧版来源；没有创建修订快照");
    channelRelation_->setText(channelRelationLabel(channel->sourceRelation));
  }
  showInspectorPage(inspectorPages_->currentIndex());
}
void MainWindow::showInspectorPage(int page, bool intentional) {
  if (!inspectorPages_) return;
  page = std::clamp(page, 0, 3);
  if (page == 3 && !session_.activeChannel()) page = 0;
  if (intentional) inspectorManualOverride_ = true;
  auto* previousPage = inspectorPages_->currentWidget();
  if (previousPage) previousPage->setProperty("parameterLayerHiding", true);
  inspectorPages_->setCurrentIndex(page);
  if (previousPage) previousPage->setProperty("parameterLayerHiding", false);
  for (int index = 0; index < 4; ++index) {
    const QSignalBlocker blocker(inspectorButtons_[index]);
    inspectorButtons_[index]->setChecked(index == page);
  }
  detectionCard_->actionsWidget()->setVisible(page == 1 &&
      !session_.project().narrowbandWorkspaceOpen && !propertiesCollapsed_ && width() > 1150);
}
void MainWindow::updateBottom() {
  if (annotations_) annotations_->refresh();
  if (auxiliaryEvidence_) {
    if (auxiliaryEvidenceVisible_) {
      const auto* file = session_.activeFile();
      auxiliaryEvidence_->setAnnotation(
          file ? findMark(*file, file->activeMarkId) : nullptr, file);
    }
    auxiliaryDisplay_->setCurrentIndex(auxiliaryEvidenceVisible_ ? 1 : 0);
    evidenceMode_->setChecked(auxiliaryEvidenceVisible_);
    if (auxiliaryEvidenceVisible_)
      for (auto* button : auxiliaryButtons_) button->setChecked(false);
  }
  if (detectionCard_) detectionCard_->refresh();
  if (manualTool_) manualTool_->setChecked(main_->isCreating());
  const bool open = resultToggle_->isChecked();
  bottom_->setFixedHeight(open ? bottomHeight_ : 29);
  if (bottomResize_) bottomResize_->setVisible(open);
  bottomContent_->setVisible(open);
  bottomContent_->setCurrentIndex(bottomTab_);
  resultToggle_->setText(open ? "⌄" : "⌃");
  for (int i = 0; i < 3; ++i) bottomButtons_[i]->setChecked(i == bottomTab_);
  const auto* file = session_.activeFile();
  resultSummary_->setText(file ? QString("%1    标记 %2 个，通道 %3 个。")
                                     .arg(q(file->metadata.name))
                                     .arg(file->marks.size())
                                     .arg(file->channels.size())
                               : "空工程 · 请添加 IQ 文件");
  if (session_.project().narrowbandWorkspaceOpen && session_.activeChannel())
    taskSummary_->setText("窄带通道 · " +
                          (narrowband_ ? narrowband_->dataStatusText()
                                       : QStringLiteral("等待后台处理")));
  else
    taskSummary_->setText(
        !file                 ? "空工程 · 无后台任务。"
        : file->metadata.demo ? "演示图谱 · 未运行真实 DSP。"
        : file->metadata.sampleFormat.structure == SampleStructure::Real
            ? "真实 ADC · 单边 PSD/STFT 在后台线程计算；Fc 仅作参考。"
            : "真实 IQ · 图谱抽取与 PSD/STFT 计算在后台线程执行。");
}
void MainWindow::updateCursor(SampleIndex sample, double frequencyHz) {
  const auto* file = session_.activeFile();
  if (!file) return;
  cursorSample_ = sample;
  cursorFrequency_ = frequencyHz;
  const auto t = coordinate(
      static_cast<double>(sample) / file->metadata.sampleRateHz,
      static_cast<double>(file->view.time.end - file->view.time.begin) /
          file->metadata.sampleRateHz,
      true);
  const auto f = coordinate(
      frequencyHz, file->view.frequency.upperHz - file->view.frequency.lowerHz,
      false);
  cursorData_->setText("游标 T " + t + " · F " + f);
  statusTime_->setText("T " + t);
  statusFrequency_->setText("F " + f);
  statusFile_->setText(q(file->metadata.name));
  main_->setCursorCoordinates(sample, frequencyHz);
}

void MainWindow::enforceLayout(bool reset) {
  if (!graphs_ || layoutBusy_) return;
  layoutBusy_ = true;
  const int w = width();
  const bool hideResources = resourcesCollapsed_ || w <= 990,
             hideProperties = propertiesCollapsed_ || w <= 1150;
  resources_->setFixedWidth(hideResources ? 38
                            : w <= 1490   ? (w <= 1150 ? 194 : 218)
                                          : resourceWidth_);
  properties_->setFixedWidth(hideProperties ? 38 : propertyWidth_);
  if (leftResize_) leftResize_->setVisible(!hideResources);
  if (rightResize_) rightResize_->setVisible(!hideProperties);
  resourceTitle_->setVisible(!hideResources);
  resourceTools_->setVisible(!hideResources);
  resourceBody_->setVisible(!hideResources);
  propertiesTitle_->setVisible(!hideProperties);
  propScroll_->setVisible(!hideProperties);
  if (inspectorTabs_) inspectorTabs_->setVisible(!hideProperties);
  if (detectionCard_ && inspectorPages_)
    detectionCard_->actionsWidget()->setVisible(!hideProperties && inspectorPages_->currentIndex() == 1 &&
        !session_.project().narrowbandWorkspaceOpen);
  propRail_->setVisible(hideProperties);
  findChild<QPushButton*>("propClose")->setVisible(!hideProperties);
  scope_->setVisible(w > 1330);
  projectLabel_->setVisible(w > 990);
  specMode_->setVisible(main_ && main_->isCreating() && w > 990);
  specAxis_->setVisible(w > 1330);
  rangeTag_->setVisible(w > 990);
  cursorData_->setVisible(w > 990);
  viewData_->setVisible(w > 1150);
  findChild<QLabel*>("navHint")->setVisible(w > 1150);
  if (maximizedPanel_ < 0) {
    for (int i = 1; i < graphs_->count(); ++i) {
      graphs_->handle(i)->setObjectName(i == 1 ? "navSplit" : "auxSplit");
      graphs_->handle(i)->installEventFilter(this);
    }
    const int available = std::max(0, graphs_->height() - 14);
    const int budget = std::min(366, std::max(80, available - 62 - 116));
    const int others = std::max(0, available - budget);
    const auto sizes = graphs_->sizes();
    int nav = reset || sizes.size() < 3 ? 82 : sizes[0];
    int aux =
        reset || sizes.size() < 3 ? (height() < 850 ? 176 : 205) : sizes[1];
    const int navMinimum = std::min(62, others),
              navMaximum = std::min(128, std::max(0, others - 116));
    nav = std::clamp(nav, std::min(navMinimum, navMaximum), navMaximum);
    const int auxMinimum = std::min(116, std::max(0, others - nav)),
              auxMaximum = std::max(0, others - nav);
    aux = std::clamp(aux, auxMinimum, auxMaximum);
    panels_[0]->setMinimumHeight(navMinimum);
    panels_[0]->setMaximumHeight(std::max(navMinimum, navMaximum));
    panels_[1]->setMinimumHeight(auxMinimum);
    panels_[1]->setMaximumHeight(std::max(auxMinimum, auxMaximum));
    panels_[2]->setMinimumHeight(80);
    graphs_->setSizes(
        {nav, aux, std::max(80, graphs_->height() - nav - aux - 20)});
  } else {
    maxVeil_->setGeometry(rect());
    maxHost_->setGeometry(12, 86, std::max(1, width() - 24),
                          std::max(1, height() - 120));
    maxVeil_->raise();
    maxHost_->raise();
  }
  layoutBusy_ = false;
}
void MainWindow::toggleMaximized(int index) {
  const bool creating = main_->isCreating();
  cancelInteractions(false);
  const int previous = maximizedPanel_;
  if (previous >= 0) {
    maxHost_->layout()->removeWidget(panels_[previous]);
    panels_[previous]->setParent(graphs_);
    graphs_->insertWidget(previous, panels_[previous]);
    panels_[previous]->show();
    maximizedPanel_ = -1;
    maxHost_->hide();
    maxVeil_->hide();
    graphs_->setSizes(panelSizes_);
    enforceLayout();
  }
  if (previous == index) {
    if (creating && !main_->isCreating()) main_->setCreating(true);
    return;
  }
  panelSizes_ = graphs_->sizes();
  maximizedPanel_ = index;
  panels_[index]->setMaximumHeight(QWIDGETSIZE_MAX);
  panels_[index]->setMinimumHeight(0);
  panels_[index]->setParent(maxHost_);
  maxHost_->layout()->addWidget(panels_[index]);
  panels_[index]->show();
  maxVeil_->show();
  maxHost_->show();
  enforceLayout();
  if (creating && !main_->isCreating()) main_->setCreating(true);
}
void MainWindow::resizeEvent(QResizeEvent* event) {
  rememberNormalGeometry();
  QMainWindow::resizeEvent(event);
  if (!graphs_) return;
  cancelInteractions(false);
  enforceLayout();
  QTimer::singleShot(0, this, [this] { enforceLayout(); });
}
void MainWindow::moveEvent(QMoveEvent* event) {
  QMainWindow::moveEvent(event);
  rememberNormalGeometry();
  if (graphs_) cancelInteractions(false);
}
void MainWindow::rememberNormalGeometry() {
  if (isVisible() && !restoringWindowGeometry_ &&
      !property("enteringTargetFullscreen").toBool() &&
      !(windowState() &
        (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized)))
    normalWindowGeometry_ = geometry();
}
void MainWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange) {
    windowDragPending_ = windowDragFallback_ = false;
    if (resizeCursorWidget_) {
      resizeCursorWidget_->unsetCursor();
      resizeCursorWidget_.clear();
    }
    if (graphs_) cancelInteractions(false);
    updateWindowControls();
  }
}
void MainWindow::updateWindowControls() {
  if (!windowMaximize_) return;
  const bool restore = isMaximized() || isFullScreen();
  windowMaximize_->setText(restore ? "❐" : "□");
  windowMaximize_->setToolTip(restore ? "窗口化" : "最大化");
  windowMaximize_->setProperty("restoresWindow", restore);
}
void MainWindow::toggleWindowState() {
  cancelInteractions(false);
  rememberNormalGeometry();
  if (isMaximized() || isFullScreen()) {
    QRect restore =
        normalWindowGeometry_.isValid()
            ? normalWindowGeometry_
            : QRect(screen()->availableGeometry().topLeft() + QPoint(80, 60),
                    QSize(1600, 960));
    const auto available = screen()->availableGeometry();
    restore.setSize(
        restore.size().boundedTo(available.size()).expandedTo(minimumSize()));
    restore.moveLeft(std::clamp(
        restore.left(), available.left(),
        std::max(available.left(), available.right() - restore.width() + 1)));
    restore.moveTop(std::clamp(
        restore.top(), available.top(),
        std::max(available.top(), available.bottom() - restore.height() + 1)));
    restoringWindowGeometry_ = true;
    showNormal();
    setGeometry(restore);
    restoringWindowGeometry_ = false;
    normalWindowGeometry_ = geometry();
  } else
    showMaximized();
  updateWindowControls();
}
Qt::Edges MainWindow::resizeEdges(QPoint point) const {
  if (isMaximized() || isFullScreen() || isMinimized()) return {};
  Qt::Edges edges;
  if (point.x() < 6)
    edges |= Qt::LeftEdge;
  else if (point.x() >= width() - 6)
    edges |= Qt::RightEdge;
  if (point.y() < 6)
    edges |= Qt::TopEdge;
  else if (point.y() >= height() - 6)
    edges |= Qt::BottomEdge;
  return edges;
}
bool MainWindow::handleWindowChrome(QObject* watched, QEvent* event) {
  auto* widget = qobject_cast<QWidget*>(watched);
  if (!widget || widget->window() != this) return false;
  const bool mouse = event->type() == QEvent::MouseMove ||
                     event->type() == QEvent::MouseButtonPress ||
                     event->type() == QEvent::MouseButtonRelease ||
                     event->type() == QEvent::MouseButtonDblClick;
  if (!mouse) return false;
  auto* input = static_cast<QMouseEvent*>(event);
  const auto point = widget->mapTo(this, input->position().toPoint());
  const auto edges = resizeEdges(point);
  if (event->type() == QEvent::MouseMove &&
      !(input->buttons() & Qt::LeftButton)) {
    if (resizeCursorWidget_ && (resizeCursorWidget_ != widget || !edges)) {
      resizeCursorWidget_->unsetCursor();
      resizeCursorWidget_.clear();
    }
    if (edges) {
      const bool horizontal =
          edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
      const bool vertical =
          edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
      widget->setCursor(
          horizontal && vertical
              ? ((edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge))
                     ? Qt::SizeFDiagCursor
                     : Qt::SizeBDiagCursor)
          : horizontal ? Qt::SizeHorCursor
                       : Qt::SizeVerCursor);
      resizeCursorWidget_ = widget;
    }
  }
  if (event->type() == QEvent::MouseButtonPress &&
      input->button() == Qt::LeftButton && edges) {
    cancelInteractions(false);
    setProperty("windowResizeEdges", static_cast<int>(edges));
    setProperty("windowSystemResizeStarted",
                windowHandle() && windowHandle()->startSystemResize(edges));
    return true;
  }
  const bool blank = widget == menuBar() &&
                     menuBar()->rect().contains(input->position().toPoint()) &&
                     !menuBar()->actionAt(input->position().toPoint()) &&
                     !menuBar()->childAt(input->position().toPoint());
  if (blank && event->type() == QEvent::MouseButtonDblClick &&
      input->button() == Qt::LeftButton) {
    windowDragPending_ = windowDragFallback_ = false;
    toggleWindowState();
    return true;
  }
  if (blank && event->type() == QEvent::MouseButtonPress &&
      input->button() == Qt::LeftButton) {
    cancelInteractions(false);
    windowDragPending_ = true;
    windowDragAnchor_ = input->globalPosition().toPoint();
    windowDragOrigin_ = pos();
    return true;
  }
  if (windowDragPending_ && event->type() == QEvent::MouseMove &&
      (input->buttons() & Qt::LeftButton)) {
    const auto distance = input->globalPosition().toPoint() - windowDragAnchor_;
    if (distance.manhattanLength() >= QApplication::startDragDistance()) {
      cancelInteractions(false);
      if (!windowDragFallback_ && windowHandle() &&
          windowHandle()->startSystemMove()) {
        windowDragPending_ = false;
        return true;
      }
      windowDragFallback_ = true;
      if (!isMaximized() && !isFullScreen()) move(windowDragOrigin_ + distance);
    }
    return true;
  }
  if (event->type() == QEvent::MouseButtonRelease &&
      input->button() == Qt::LeftButton) {
    windowDragPending_ = windowDragFallback_ = false;
  }
  return false;
}

void MainWindow::applyPowerInput(QComboBox* control) {
  if (refreshing_ || !session_.activeFile()) return;
  QString text = control->currentText().trimmed();
  text.remove(QRegularExpression(
      QStringLiteral("\\s*dB(?:FS(?:/Hz)?|\\((?:unit|ADC)\\^2/Hz\\))?\\s*$")));
  bool ok = false;
  const double value = text.toDouble(&ok);
  const auto& display = session_.activeFile()->display;
  PowerDisplayRange range{display.referenceLevelDb, display.dynamicRangeDb};
  if (control == dynamic_)
    range.dynamicRangeDb = value;
  else
    range.referenceLevelDb = value;
  const bool valid = ok && range.valid();
  control->setProperty("powerInputValid", valid);
  setParameterError(control, valid ? QString{}
                      : control == dynamic_
                          ? "请输入大于 0 且不超过 10000 的动态范围"
                          : "请输入 -200 至 100 的参考电平（dBFS/Hz）");
  if (!valid) {
    control->setProperty("parameterDraft", true);
    return;
  }
  if (control->property("parameterCustomCommit").toBool())
    rememberCustomValue(control, value,
                        control == dynamic_
                            ? " dB"
                            : " " + powerUnit(session_.activeFile()->metadata));
  cancelInteractions(false);
  session_.setPowerDisplayRange(range);
  scheduleRightSidebarSettingsSave();
  refresh();
}
void MainWindow::normalizePowerInput(QComboBox* control) {
  if (refreshing_ || !session_.activeFile()) return;
  if (!control->property("powerInputValid").toBool()) return;
  const auto& display = session_.activeFile()->display;
  const QSignalBlocker blocker(control);
  const QSignalBlocker editBlocker(control->lineEdit());
  control->setProperty("parameterDraft", false);
  const auto text =
      number(control == dynamic_ ? display.dynamicRangeDb
                                 : display.referenceLevelDb) +
      (control == dynamic_ ? " dB"
                           : " " + powerUnit(session_.activeFile()->metadata));
  control->setProperty("parameterCommittedText", text);
  control->setEditText(text);
  control->lineEdit()->setStyleSheet({});
  control->setProperty("powerInputValid", true);
  control->setToolTip({});
}
void MainWindow::updatePowerFit() {
  if (!autoPowerFit_) return;
  PowerAnalysisSnapshot snapshot;
  if (session_.project().narrowbandWorkspaceOpen && session_.activeChannel())
    snapshot = narrowband_->powerSnapshot();
  else if (session_.activeFile()) {
    snapshot.heatmap = main_->currentSpectrogram();
    snapshot.psd = auxiliary_->currentPowerFrame();
    snapshot.ready =
        snapshot.heatmap &&
        (session_.activeFile()->display.auxiliaryMode != AuxiliaryMode::Psd ||
         snapshot.psd);
  }
  if (snapshot.ready != powerFitReady_ ||
      snapshot.heatmap != powerFitHeatmap_ || snapshot.psd != powerFitPsd_) {
    powerFitReady_ = snapshot.ready;
    powerFitHeatmap_ = snapshot.heatmap;
    powerFitPsd_ = snapshot.psd;
    cachedPowerFit_ =
        snapshot.ready && snapshot.heatmap
            ? fitPowerDisplayRange(*snapshot.heatmap, snapshot.psd.get())
            : PowerFitResult{};
  }
  const bool enabled = snapshot.ready && cachedPowerFit_.valid;
  autoPowerFit_->setEnabled(enabled);
  autoPowerFit_->setToolTip(
      enabled ? "按当前可见热图及 PSD 的有效功率上下各留 3 dB，一次应用"
              : "等待当前分析数据，或没有有效功率");
  for (auto* chart : {main_, auxiliary_})
    chart->setProperty("autoPowerFitAvailable", enabled);
  narrowband_->setProperty("autoPowerFitAvailable", enabled);
}
void MainWindow::autoFitPower() {
  updatePowerFit();
  if (!autoPowerFit_->isEnabled()) return;
  cancelInteractions(false);
  session_.setPowerDisplayRange(cachedPowerFit_.range);
  rememberCustomValue(dynamic_, cachedPowerFit_.range.dynamicRangeDb, " dB");
  rememberCustomValue(reference_, cachedPowerFit_.range.referenceLevelDb,
                      " " + powerUnit(session_.activeFile()->metadata));
  normalizePowerInput(dynamic_);
  normalizePowerInput(reference_);
  scheduleRightSidebarSettingsSave();
  refresh();
  log(cachedPowerFit_.limited
          ? "自动适配已应用，达到参数合法边界"
          : "已按当前可见热图及 PSD 自动适配电平（上下各 3 dB 余量）");
}

bool MainWindow::clearActiveLinkedCursor() {
  const auto* file = session_.activeFile();
  if (!file) return false;
  const auto* channel = session_.project().narrowbandWorkspaceOpen
                            ? session_.activeChannel()
                            : nullptr;
  const auto context = channel ? channel->id : file->metadata.id;
  if (!session_.linkedCursor(context).pinned) return false;
  session_.clearCursor(context);
  refresh();
  return true;
}
void MainWindow::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    if (main_ && main_->isSelectingDetectionRoi()) {
      main_->cancelDetectionRoi();
      refresh();
      event->accept();
      return;
    }
    cancelInteractions();
    if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
    event->accept();
    return;
  }
  QMainWindow::keyPressEvent(event);
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (destroying_) return QMainWindow::eventFilter(watched, event);
  if (event->type() == QEvent::MouseButtonPress ||
      event->type() == QEvent::MouseButtonDblClick) {
    auto* target = qobject_cast<QWidget*>(watched);
    if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton &&
        target && (target == tree_ || tree_->isAncestorOf(target))) {
      treePointerPressed_ = true;
      ++treePointerSequence_;
    }
  }
  if (treePointerPressed_ &&
      ((event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) ||
       (event->type() == QEvent::WindowDeactivate && watched == this))) {
    const auto sequence = treePointerSequence_;
    QTimer::singleShot(0, this, [this, sequence] {
      if (sequence != treePointerSequence_) return;
      treePointerPressed_ = false;
      if (treeRefreshAfterRelease_) {
        treeRefreshAfterRelease_ = false;
        updateTree();
      }
    });
  }
  if (event->type() == QEvent::ShortcutOverride &&
      static_cast<QKeyEvent*>(event)->key() == Qt::Key_Delete) {
    auto* focus = QApplication::focusWidget();
    const auto* line = qobject_cast<QLineEdit*>(focus);
    const auto* plain = qobject_cast<QPlainTextEdit*>(focus);
    const auto* rich = qobject_cast<QTextEdit*>(focus);
    if ((line && !line->isReadOnly()) || (plain && !plain->isReadOnly()) ||
        (rich && !rich->isReadOnly())) {
      event->accept();
      return true;
    }
  }
  if (handleWindowChrome(watched, event)) return true;
  if (watched == tree_->viewport()) {
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseButtonDblClick) {
      auto* mouse = static_cast<QMouseEvent*>(event);
      auto* item = tree_->itemAt(mouse->position().toPoint());
      if (item && mouse->button() == Qt::LeftButton &&
          item->data(0, Qt::UserRole).toString() == "mark") {
        const auto id = item->data(0, Qt::UserRole + 1).toString();
        if (event->type() == QEvent::MouseButtonPress) {
          tree_->setFocus(Qt::MouseFocusReason);
          selectMark(id, mouse->modifiers(), {}, true);
        } else if (event->type() == QEvent::MouseButtonDblClick) {
          cancelInteractions(false);
          session_.focusMark(id.toStdString());
          session_.project().narrowbandWorkspaceOpen = false;
          workspaceStack_->setCurrentIndex(0);
          annotations_->revealActive();
          treeRevealPending_ = true;
          refresh();
          showInspectorPage(2, true);
        }
        return true;
      }
    } else if (event->type() == QEvent::Leave) {
      if (auto* file = session_.activeFile())
        if (session_.annotations().hover(*file)) refreshAnnotationHover();
    } else if (event->type() == QEvent::MouseMove) {
      auto* item =
          tree_->itemAt(static_cast<QMouseEvent*>(event)->position().toPoint());
      if (!item || item->data(0, Qt::UserRole).toString() != "mark")
        if (auto* file = session_.activeFile())
          if (session_.annotations().hover(*file)) refreshAnnotationHover();
    }
  }
  if (watched == tree_ && event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->matches(QKeySequence::SelectAll)) {
      selectAllMarks();
      return true;
    }
    const auto* file = session_.activeFile();
    const auto order = treeSelectionOrder();
    const auto* currentItem = tree_->currentItem();
    if (file && !order.empty() && currentItem &&
        currentItem->data(0, Qt::UserRole).toString() == "mark") {
      const auto focusedId =
          currentItem->data(0, Qt::UserRole + 1).toString().toStdString();
      const auto current = std::find(order.begin(), order.end(), focusedId);
      auto row =
          current == order.end() ? std::ptrdiff_t{0} : current - order.begin();
      bool navigate = true;
      switch (key->key()) {
        case Qt::Key_Up:
          --row;
          break;
        case Qt::Key_Down:
          ++row;
          break;
        case Qt::Key_Home:
          row = 0;
          break;
        case Qt::Key_End:
          row = static_cast<std::ptrdiff_t>(order.size()) - 1;
          break;
        case Qt::Key_PageUp:
          row -= static_cast<std::ptrdiff_t>(kTreeAnnotationBatchSize);
          break;
        case Qt::Key_PageDown:
          row += static_cast<std::ptrdiff_t>(kTreeAnnotationBatchSize);
          break;
        case Qt::Key_Space:
          break;
        default:
          navigate = false;
          break;
      }
      if (navigate) {
        row = std::clamp(row, std::ptrdiff_t{0},
                         static_cast<std::ptrdiff_t>(order.size()) - 1);
        selectMark(q(order[static_cast<std::size_t>(row)]),
                   key->key() == Qt::Key_Space ? Qt::ControlModifier
                                               : key->modifiers(),
                   {}, true);
        return true;
      }
    }
  }
  if (watched->objectName() == "graphArea" && event->type() == QEvent::Resize &&
      empty_)
    empty_->setGeometry(
        static_cast<QWidget*>(watched)->rect().adjusted(5, 5, -5, -5));
  if (auto* widget = qobject_cast<QWidget*>(watched);
      widget && (widget == this || isAncestorOf(widget))) {
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape &&
        !QApplication::activeModalWidget()) {
      if (auto* focus = QApplication::focusWidget();
          focus && (focus->property("parameterDraft").toBool() ||
                    focus->objectName() == "annotationTableEditor")) {
        if (auto* draft = dynamic_cast<ParameterDraft*>(focus)) {
          draft->discard();
          event->accept();
          return true;
        }
      }
      if (main_ && main_->isSelectingDetectionRoi()) {
        main_->cancelDetectionRoi();
        refresh();
        event->accept();
        return true;
      }
      const bool active =
          (main_ && (main_->isCreating() || main_->hasPendingInteraction())) ||
          (auxiliary_ && auxiliary_->hasPendingInteraction()) ||
          (navigation_ && navigation_->hasPendingInteraction()) ||
          (narrowband_ && narrowband_->hasPendingInteraction()) ||
          static_cast<PrototypeSplitter*>(graphs_)->isDragging();
      cancelInteractions();
      if (!active && !clearActiveLinkedCursor() && maximizedPanel_ >= 0)
        toggleMaximized(maximizedPanel_);
      event->accept();
      return true;
    }
    if (event->type() == QEvent::ContextMenu &&
        widget->property("chartIndex").isValid()) {
      const auto* context = static_cast<QContextMenuEvent*>(event);
      const int index = widget->property("chartIndex").toInt();
      auto* plot =
          std::array<PlotWidget*, 3>{navigation_, auxiliary_, main_}[index];
      auto* menu =
          plot->createContextMenu(plot->mapFromGlobal(context->globalPos()));
      menu->exec(context->globalPos());
      delete menu;
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::selectMark(const QString& id, Qt::KeyboardModifiers modifiers,
                            const QString& segmentId, bool fromTree) {
  auto* file = session_.activeFile();
  if (!file) return;
  AnnotationResult result{true};
  if (id.isEmpty()) {
    session_.selectMarks({});
  } else {
    const auto mode = modifiers & Qt::ShiftModifier
                          ? modifiers & Qt::ControlModifier
                                ? AnnotationSelectionMode::ExtendRange
                                : AnnotationSelectionMode::Range
                      : modifiers & Qt::ControlModifier
                          ? AnnotationSelectionMode::Toggle
                          : AnnotationSelectionMode::Replace;
    result = session_.annotations().chooseSelection(
        *file, id.toStdString(), mode,
        fromTree ? treeSelectionOrder() : annotations_->sortedVisibleIds(),
        segmentId.toStdString());
  }
  if (!result.ok) log(q(result.error));
  annotations_->revealActive();
  treeRevealPending_ = true;
  if (auto* active = session_.activeFile())
    active->display.psdFromSelection =
        sharedPsdFromSelectionPreference_ && hasActiveSegment(*active);
  propagateGlobalRightSidebarSettings();
  refresh();
  scheduleRightSidebarSettingsSave();
}
void MainWindow::selectAllMarks() {
  cancelInteractions(false);
  if (const auto* file = session_.activeFile()) {
    const auto& ids = annotations_->sortedVisibleIds();
    const auto active =
        std::find(ids.begin(), ids.end(), file->activeMarkId) != ids.end()
            ? file->activeMarkId
        : ids.empty() ? std::string{}
                      : ids.front();
    session_.selectMarks(ids, active);
    if (auto* current = session_.activeFile())
      current->display.psdFromSelection =
          sharedPsdFromSelectionPreference_ && hasActiveSegment(*current);
    propagateGlobalRightSidebarSettings();
    refresh();
    scheduleRightSidebarSettingsSave();
  }
}
void MainWindow::locateMark() {
  cancelInteractions(false);
  if (const auto* file = session_.activeFile())
    session_.focusMark(file->activeMarkId);
  refresh();
  showInspectorPage(2, true);
}
void MainWindow::openWidebandDetection(const DetectionRun* parentRun) {
  if (!detectionCard_) return;
  propertiesCollapsed_ = false;
  enforceLayout();
  detectionCard_->configure(parentRun);
  showInspectorPage(1, true);
  propScroll_->verticalScrollBar()->setValue(0);
}

void MainWindow::beginDetectionRoi() {
  const auto* file = session_.activeFile();
  if (!file || availableSamples(file->metadata) == 0) return;
  openWidebandDetection();
  main_->beginDetectionRoi();
  main_->setFocus(Qt::OtherFocusReason);
  statusBar()->showMessage("拖出单个检测 ROI；Enter 确认，Esc 恢复前一工具");
}

void MainWindow::updateDetectionRoiConfirmation() {
  const auto preview = main_->detectionRoiPreview();
  const auto* file = session_.activeFile();
  const bool visible = preview && file && file->metadata.sampleRateHz > 0;
  detectionRoiConfirmation_->setVisible(visible);
  if (!visible) return;
  const auto rate = file->metadata.sampleRateHz;
  auto* text =
      detectionRoiConfirmation_->findChild<QLabel*>("detectionRoiRange");
  text->setText(QString("ROI 待确认  %1–%2 ms · RF %3–%4 MHz")
                    .arg(preview->time.begin / rate * 1000, 0, 'f', 4)
                    .arg(preview->time.end / rate * 1000, 0, 'f', 4)
                    .arg(preview->frequency.lowerHz / 1e6, 0, 'f', 6)
                    .arg(preview->frequency.upperHz / 1e6, 0, 'f', 6));
  text->setToolTip(
      QString("精确样本 [%1, %2) · %3 samples\n"
              "持续 %4 ms · Fs %5 Hz\n"
              "正文为舍入显示；确认使用精确半开样本边界。")
          .arg(QString::number(preview->time.begin),
               QString::number(preview->time.end),
               QString::number(preview->time.end - preview->time.begin),
               QString::number(
                   (preview->time.end - preview->time.begin) / rate * 1000, 'g',
                   17),
               QString::number(rate, 'g', 17)));
  const auto plot = main_->plotRect();
  const int available = std::max(1, static_cast<int>(plot.width()) - 100);
  const int width = std::min(
      available, std::max(650, detectionRoiConfirmation_->sizeHint().width()));
  detectionRoiConfirmation_->resize(width, 44);
  detectionRoiConfirmation_->move(qRound(plot.center().x() - width / 2.0),
                                  qRound(plot.top()) + 16);
  detectionRoiConfirmation_->raise();
}

void MainWindow::cancelInteractions(bool exitCreating) {
  for (auto* handle : {leftResize_, rightResize_, bottomResize_})
    if (handle) handle->cancel();
  if (annotations_) annotations_->discardDrafts();
  if (main_) main_->cancelGesture(exitCreating);
  if (auxiliary_) auxiliary_->cancelGesture();
  if (navigation_) navigation_->cancelGesture();
  if (narrowband_) narrowband_->cancelInteractions();
  if (graphs_) static_cast<PrototypeSplitter*>(graphs_)->cancel();
}
void MainWindow::deleteMarks() {
  if (annotations_) annotations_->deleteSelected();
}
void MainWindow::renameMark() {
  cancelInteractions(false);
  auto* file = session_.activeFile();
  if (!file) return;
  auto* mark = findMark(*file, file->activeMarkId);
  if (!mark) return;
  const auto context =
      annotationEditContext(*file, mark->id, session_.projectGeneration());
  bool ok = false;
  const auto name =
      QInputDialog::getText(this, "重命名标记", "标记名称（最多 80 个字符）",
                            QLineEdit::Normal, q(mark->name), &ok)
          .trimmed()
          .left(80);
  file = session_.activeFile();
  if (!ok || name.isEmpty() || !file ||
      !annotationEditContextMatches(*file, context,
                                    session_.projectGeneration()))
    return;
  mark = findMark(*file, context.annotationId);
  if (mark) {
    const auto result = session_.annotations().rename(
        *file, context.annotationId, name.toStdString(), mark->note);
    refresh();
    log(result.ok ? "已创建名称修订" : q(result.error));
  }
}
void MainWindow::showNewProjectDialog() {
  if ((!projectPath_.isEmpty() || !session_.project().files.empty()) &&
      QMessageBox::question(this, "新建工程",
                            "新建工程会切换当前工程，继续吗？") !=
          QMessageBox::Yes)
    return;

  auto* dialog = new QDialog(this);
  dialog->setObjectName("newProjectDialog");
  dialog->setWindowTitle("新建 Signal Studio 工程");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setModal(true);
  dialog->setMinimumWidth(520);
  auto* layout = new QVBoxLayout(dialog);
  layout->setContentsMargins(20, 18, 20, 18);
  layout->setSpacing(12);
  auto* intro =
      label("新工程将创建独立文件夹，并在其中保存 project.json。", {}, "help");
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto* form = new QFormLayout;
  form->setSpacing(10);
  auto* name = new QLineEdit("Signal Studio Project");
  name->setObjectName("newProjectName");
  auto* location = new QLineEdit(
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
  location->setObjectName("newProjectLocation");
  auto* locationRow = new QWidget(dialog);
  auto* rowLayout = new QHBoxLayout(locationRow);
  rowLayout->setContentsMargins(0, 0, 0, 0);
  rowLayout->setSpacing(6);
  rowLayout->addWidget(location, 1);
  auto* browse = push("浏览…", "browseProjectLocation", rowLayout);
  browse->setProperty("uiRole", "outline");
  form->addRow("工程名称", name);
  form->addRow("保存位置", locationRow);
  layout->addLayout(form);
  auto* target = label({}, "newProjectTarget", "help");
  target->setWordWrap(true);
  layout->addWidget(target);
  auto updateTarget = [name, location, target] {
    target->setText("将创建：" +
                    QDir(location->text()).filePath(name->text().trimmed()));
  };
  connect(name, &QLineEdit::textChanged, dialog,
          [updateTarget] { updateTarget(); });
  connect(location, &QLineEdit::textChanged, dialog,
          [updateTarget] { updateTarget(); });
  updateTarget();
  connect(browse, &QPushButton::clicked, dialog, [dialog, location] {
    const auto path = QFileDialog::getExistingDirectory(
        dialog, "选择工程保存位置", location->text());
    if (!path.isEmpty()) location->setText(path);
  });
  auto* error = label({}, "newProjectError");
  error->setStyleSheet("color:#ff9b8d;");
  error->setWordWrap(true);
  layout->addWidget(error);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  buttons->button(QDialogButtonBox::Ok)->setText("创建工程文件夹");
  buttons->button(QDialogButtonBox::Ok)->setObjectName("createProjectButton");
  buttons->button(QDialogButtonBox::Cancel)->setText("取消");
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, dialog,
          [this, dialog, name, location, error] {
            QString message;
            if (!createProject(location->text(), name->text(), &message)) {
              error->setText(message);
              return;
            }
            dialog->accept();
          });
  dialog->open();
}

bool MainWindow::createProject(const QString& parentDirectory,
                               const QString& projectName, QString* error) {
  auto setError = [error](const QString& message) {
    if (error) *error = message;
  };
  const QString name = projectName.trimmed();
  if (name.isEmpty() || name == "." || name == ".." || name.contains('/') ||
      name.contains('\\')) {
    setError("工程名称不能为空，也不能包含路径分隔符。");
    return false;
  }
  QDir parent(parentDirectory);
  if (!parent.exists()) {
    setError("工程保存位置不存在，请重新选择。");
    return false;
  }
  if (!parent.mkdir(name)) {
    setError(
        "无法创建工程文件夹；请检查名称、权限，或选择一个尚不存在的文件夹。");
    return false;
  }
  const QString folder = parent.absoluteFilePath(name);
  const QString projectFile = QDir(folder).filePath("project.json");
  Project candidate;
  candidate.name = name.toUtf8().toStdString();
  QString message;
  if (!ProjectStore::save(projectFile, candidate, message)) {
    QFile::remove(projectFile);
    QDir().rmdir(folder);
    setError("工程文件初始化失败：" + message);
    return false;
  }
  cancelInteractions();
  if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
  session_.replaceProject(std::move(candidate));
  projectPath_ = QFileInfo(projectFile).absoluteFilePath();
  rememberProject(projectPath_);
  refresh();
  log("已创建工程文件夹：" + folder);
  if (error) error->clear();
  return true;
}

void MainWindow::openDemoProject() {
  if ((!projectPath_.isEmpty() || !session_.project().files.empty()) &&
      QMessageBox::question(this, "打开演示工程",
                            "打开演示工程会切换当前工程，继续吗？") !=
          QMessageBox::Yes)
    return;
  cancelInteractions();
  if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
  session_.newProject();
  session_.project().name = "演示工程";
  const auto first = session_.addDemoFile();
  session_.addDemoFile();
  session_.addDemoFile();
  session_.activateFile(first);
  projectPath_.clear();
  refresh();
  log("已打开演示工程");
}

void MainWindow::openNarrowbandDemoProject() {
  if ((!projectPath_.isEmpty() || !session_.project().files.empty()) &&
      QMessageBox::question(this, "打开窄带演示工程",
                            "打开窄带演示工程会切换当前工程，继续吗？") !=
          QMessageBox::Yes)
    return;
  const QString fixturePath =
      QStringLiteral(":/signalstudio/demo/narrowband_demo.iq");
  QFile resource(fixturePath);
  if (!resource.open(QIODevice::ReadOnly) || resource.size() != 1'048'576) {
    QMessageBox::critical(this, "窄带演示工程不可用",
                          "内置确定性 IQ 资源缺失或长度错误。");
    return;
  }
  Int16IqFile fixture;
  QString error;
  if (!fixture.open(fixturePath, error) || fixture.sampleCount() != 262'144) {
    QMessageBox::critical(this, "窄带演示工程不可用",
                          "内置 IQ 样例校验失败：" + error);
    return;
  }

  cancelInteractions();
  if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
  session_.newProject();
  session_.project().name = "窄带通道演示工程 · NB-A1.1";
  FileMetadata metadata;
  metadata.name = "IQ0 · 阿拉善信号样例";
  metadata.path = fixturePath.toStdString();
  metadata.sampleRateHz = 102.4e6;
  metadata.centerFrequencyHz = 2500e6;
  metadata.sampleCount = fixture.sampleCount();
  metadata.demo = false;
  metadata.demoSeed = 1;
  metadata.declaredBandwidthHz = 102.4e6;
  metadata.effectiveBandwidthHz = 102.4e6;
  const auto fileId = session_.addDemoFile(std::move(metadata));
  auto* source = session_.activeFile();
  if (fileId.empty() || !source) {
    QMessageBox::critical(this, "窄带演示工程不可用",
                          "无法创建样例工程文件状态。");
    return;
  }
  const auto time1 = TimeRange{0, 196'608}, time2 = TimeRange{65'536, 262'144};
  source->marks.push_back(
      {"demo-mark-signal-01", "Signal-01", {time1, {2510.6e6, 2511.8e6}}});
  source->marks.push_back(
      {"demo-mark-signal-02", "Signal-02", {time2, {2493.0e6, 2494.2e6}}});
  session_.selectMarks({"demo-mark-signal-01"}, "demo-mark-signal-01");
  const auto channelId = session_.createChannel(
      "Signal-01", "demo-mark-signal-01", 2511.2e6, 1.2e6, 4e6, time1,
      ChannelFilter::Standard, false, true);
  if (channelId.empty()) {
    QMessageBox::critical(this, "窄带演示工程不可用",
                          "样例通道参数未能通过校验。");
    return;
  }
  session_.project().narrowbandWorkspaceOpen = true;
  projectPath_.clear();
  workspaceStack_->setCurrentIndex(1);
  refresh();
  log("已载入内置确定性 IQ 样例；此演示工程会运行真实 DDC、FIR、重采样、PSD 与 "
      "STFT，识别/解调仍为合成示例");
}

void MainWindow::showAddFileDialog() {
  cancelInteractions(false);
  auto* dialog = new SignalImportDialog(q(session_.project().name), this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->commitSource = [this](FileState file, QString& error) {
    return addImportedSource(std::move(file), error);
  };
  dialog->open();
}

bool MainWindow::addImportedSource(FileState file, QString& error) {
  if (!availableSamples(file.metadata) ||
      file.metadata.availability.status == LoadStatus::Failed) {
    error = "没有可用的完整样本帧";
    return false;
  }
  const auto path = q(file.metadata.path);
  if (path.endsWith(".wav", Qt::CaseInsensitive)) {
    WavDescriptor descriptor;
    if (!readWavDescriptor(path, descriptor, error)) return false;
    if (file.metadata.sampleRateHz != descriptor.sampleRateHz ||
        file.metadata.sampleFormat != descriptor.format ||
        file.metadata.sampleCount != descriptor.sampleCount) {
      error = QString(
                  "WAV 导入元数据与容器冲突：Fs %1 / %2 Hz，物理样本数 %3 / "
                  "%4；格式和数据边界须来自容器，请重新导入。")
                  .arg(file.metadata.sampleRateHz, 0, 'g', 17)
                  .arg(descriptor.sampleRateHz)
                  .arg(file.metadata.sampleCount)
                  .arg(descriptor.sampleCount);
      return false;
    }
  }
  const auto metadata = file.metadata;
  const auto id = session_.addDemoFile(metadata);
  if (id.empty()) {
    error = "文件元数据无效";
    return false;
  }
  auto* added = session_.activeFile();
  added->navigationEnvelope = std::move(file.navigationEnvelope);
  restoreRightSidebarSettings(*added);
  if (metadata.sampleFormat.structure == SampleStructure::Real)
    added->display.waveformMode = WaveformMode::I;
  cancelInteractions(false);
  refresh();
  log(QString("已导入 %1 · %2 · %3 个可用样本")
          .arg(q(metadata.name),
               QString::fromStdString(formatId(metadata.sampleFormat)))
          .arg(availableSamples(metadata)));
  error.clear();
  return true;
}

bool MainWindow::addIqFile(const QString& path, QString* error) {
  if (path.endsWith(".wav", Qt::CaseInsensitive)) {
    if (error)
      *error =
          "WAV 请使用信号导入配置：Fs、格式与数据边界来自容器，RF "
          "中心频率需人工确认。";
    return false;
  }
  QString message;
  const auto descriptor = describeInt16IqFile(path, message);
  if (!descriptor) {
    if (error) *error = message;
    return false;
  }
  auto metadata = descriptor->metadata;
  metadata.declaredBandwidthHz = descriptor->declaredBandwidthHz;
  metadata.effectiveBandwidthHz = descriptor->declaredBandwidthHz > 0
                                      ? descriptor->declaredBandwidthHz
                                      : metadata.sampleRateHz;
  const auto* activeBeforeImport = session_.activeFile();
  const bool hasActiveBeforeImport = activeBeforeImport != nullptr;
  const DisplaySettings priorSettings =
      activeBeforeImport ? activeBeforeImport->display : DisplaySettings{};
  const auto name = QString::fromUtf8(
      metadata.name.data(), static_cast<qsizetype>(metadata.name.size()));
  const auto id = session_.addDemoFile(std::move(metadata));
  if (id.empty()) {
    if (error) *error = QStringLiteral("文件元数据无效，未能加入工程");
    return false;
  }
  auto* imported = session_.activeFile();
  if (imported && imported->metadata.id == id)
    restoreRightSidebarSettings(*imported);
  applyGlobalRightSidebarSettings(hasActiveBeforeImport
                                      ? priorSettings
                                      : (imported && imported->metadata.id == id
                                             ? imported->display
                                             : DisplaySettings{}));
  cancelInteractions(false);
  if (imported) startSourceLoad(*imported, imported->metadata.sampleCount);
  refresh();
  log("正在读入真实 int16 IQ 文件：" + name);
  scheduleRightSidebarSettingsSave();
  if (error) error->clear();
  return true;
}

bool MainWindow::openProject(const QString& path) {
  cancelInteractions();
  Project candidate;
  QString error;
  const QFileInfo requested(path);
  const QString projectFile =
      requested.isDir() ? QDir(path).filePath("project.json") : path;
  if (!ProjectStore::load(projectFile, candidate, error)) {
    QMessageBox::critical(this, "打开失败", error);
    return false;
  }
  const auto* current = session_.activeFile();
  const DisplaySettings fallbackSettings =
      current ? current->display
              : (candidate.files.empty() ? DisplaySettings{}
                                         : candidate.files.front().display);
  if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
  session_.replaceProject(std::move(candidate));
  for (auto& file : session_.project().files)
    if (!file.metadata.demo)
      startSourceLoad(file, availableSamples(file.metadata));
  applyGlobalRightSidebarSettings(fallbackSettings);
  projectPath_ = QFileInfo(projectFile).absoluteFilePath();
  rememberProject(projectPath_);
  refresh();
  scheduleRightSidebarSettingsSave();
  log("已打开工程：" + projectPath_);
  return true;
}
bool MainWindow::saveProject(const QString& path) {
  const QFileInfo requested(path);
  const QString projectFile =
      requested.isDir() ? QDir(path).filePath("project.json") : path;
  cancelInteractions(false);
  QString error;
  if (!ProjectStore::save(projectFile, session_.project(), error)) {
    QMessageBox::critical(this, "保存失败", error);
    return false;
  }
  projectPath_ = QFileInfo(projectFile).absoluteFilePath();
  rememberProject(projectPath_);
  refresh();
  log("工程已保存：" + projectPath_);
  return true;
}
void MainWindow::startSourceLoad(FileState& file, SampleIndex target) {
  const auto path = QString::fromStdString(file.metadata.path);
  const auto fingerprint = iqSourceFingerprint(path);
  if (!file.metadata.availability.fingerprint.empty() &&
      file.metadata.availability.fingerprint != fingerprint.toStdString()) {
    file.metadata.availability.status = LoadStatus::Failed;
    file.metadata.availability.availableSamples = 0;
    file.metadata.availability.error = "源文件指纹已改变，未恢复原前缀";
    return;
  }
  file.metadata.availability.status = LoadStatus::Loading;
  file.metadata.availability.availableSamples = 0;
  ++file.metadata.availability.generation;
  file.navigationEnvelope.clear();
  sourceLoads_[file.metadata.id] = {
      std::make_shared<SourceLoader>(
          path, std::min(target, file.metadata.sampleCount),
          file.metadata.sampleFormat),
      session_.projectGeneration(), 0};
}
void MainWindow::pollSourceLoads() {
  bool changed = false;
  for (auto it = sourceLoads_.begin(); it != sourceLoads_.end();) {
    const auto found = std::find_if(
        session_.project().files.begin(), session_.project().files.end(),
        [&](const auto& f) { return f.metadata.id == it->first; });
    if (it->second.project != session_.projectGeneration() ||
        found == session_.project().files.end()) {
      it = sourceLoads_.erase(it);
      continue;
    }
    auto& file = *found;
    const auto snapshot = it->second.loader->snapshot();
    if (snapshot.loaded != it->second.previous || snapshot.finished) {
      file.metadata.availability.availableSamples = snapshot.loaded;
      file.navigationEnvelope = snapshot.envelope;
      it->second.previous = snapshot.loaded;
      changed = true;
    }
    if (!snapshot.finished) {
      ++it;
      continue;
    }
    file.metadata.availability.fingerprint = snapshot.fingerprint.toStdString();
    file.metadata.availability.error = snapshot.error.toStdString();
    file.metadata.availability.status =
        !snapshot.error.isEmpty()                      ? LoadStatus::Failed
        : snapshot.loaded == file.metadata.sampleCount ? LoadStatus::Ready
                                                       : LoadStatus::Partial;
    if (snapshot.loaded)
      file.view = clampRange(file.view, file.metadata, file.display.stftSize,
                             file.display.psdSize);
    log(snapshot.error.isEmpty() ? QString("读入结束：%1 · %2 / %3 样本")
                                       .arg(q(file.metadata.name))
                                       .arg(snapshot.loaded)
                                       .arg(file.metadata.sampleCount)
                                 : "读入失败：" + snapshot.error);
    if (!snapshot.loaded && file.marks.empty() && file.channels.empty()) {
      const auto id = file.metadata.id;
      session_.activateFile(id);
      session_.removeActiveFile();
    }
    it = sourceLoads_.erase(it);
  }
  if (changed) refresh();
}
void MainWindow::log(const QString& message) {
  if (!results_) return;
  results_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") +
                            "  信息  " + message);
  statusBar()->showMessage(message, 2500);
}
}  // namespace signalstudio

namespace signalstudio {
bool MainWindow::importLabels(const QString& path, QString* error,
                              bool preview) {
  return annotations_ && annotations_->importLabels(path, error, preview);
}
}  // namespace signalstudio
