#include "app/main_window.h"
#include "infrastructure/project_store.h"
#include "infrastructure/int16_iq_file.h"
#include "infrastructure/channel_processor.h"
#include "ui/narrowband_workspace.h"
#include "ui/charts/plot_widget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStandardPaths>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>

namespace signalstudio {
namespace {
QString q(const std::string& text) { return QString::fromStdString(text); }
QString number(double value, int precision = 9) { return QString::number(value, 'g', precision); }
QString coordinate(double value, double span, bool time) {
    const double width = std::abs(span);
    const double chosen = width > 0 && std::abs(value) < width * 1000 ? width : std::max(std::abs(value), width);
    const double scale = time ? (chosen >= 1 ? 1 : chosen >= .001 ? 1000 : 1e6)
                              : (chosen >= 1e6 ? 1e-6 : chosen >= 1000 ? .001 : 1);
    const auto unit = time ? (chosen >= 1 ? "s" : chosen >= .001 ? "ms" : "us")
                          : (chosen >= 1e6 ? "MHz" : chosen >= 1000 ? "kHz" : "Hz");
    return number(value * scale, 11) + " " + unit;
}
QString timeRange(const TimeRange& range, double fs) {
    const double span = static_cast<double>(range.end - range.begin) / fs;
    return coordinate(static_cast<double>(range.begin) / fs, span, true).section(' ', 0, 0) + "–" + coordinate(static_cast<double>(range.end) / fs, span, true);
}
QString frequencyRange(const FrequencyRange& range) {
    return coordinate(range.lowerHz, range.upperHz - range.lowerHz, false).section(' ', 0, 0) + "–" + coordinate(range.upperHz, range.upperHz - range.lowerHz, false);
}
QString sidebarSettingsKey(const FileMetadata& metadata) {
    if (metadata.demo || metadata.path.empty()) return {};
    const auto sourcePath = QString::fromUtf8(metadata.path.data(), static_cast<qsizetype>(metadata.path.size()));
    const auto normalizedPath = QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath()).toCaseFolded();
    return QString::fromLatin1(QCryptographicHash::hash(normalizedPath.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString sidebarSourcePath(const FileMetadata& metadata) {
    if (metadata.demo || metadata.path.empty()) return {};
    const auto sourcePath = QString::fromUtf8(metadata.path.data(), static_cast<qsizetype>(metadata.path.size()));
    return QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath()).toCaseFolded();
}
QPushButton* push(const QString& text, const QString& id, QLayout* layout) {
    auto* widget = new QPushButton(text);
    widget->setObjectName(id); widget->setCursor(Qt::PointingHandCursor);
    layout->addWidget(widget); return widget;
}
QLabel* label(const QString& text, const QString& id = {}, const QString& role = {}) {
    auto* widget = new QLabel(text); widget->setTextFormat(Qt::PlainText);
    if (!id.isEmpty()) widget->setObjectName(id);
    if (!role.isEmpty()) widget->setProperty("uiRole", role);
    return widget;
}
QLabel* output(const QString& id) {
    auto* widget = label("—", id, "output"); widget->setWordWrap(true);
    widget->setTextInteractionFlags(Qt::TextSelectableByMouse); widget->setMinimumWidth(0);
    return widget;
}
QComboBox* combo(const QString& id, const QStringList& items) {
    auto* widget = new QComboBox; widget->setObjectName(id); widget->addItems(items);
    widget->setMinimumWidth(0); widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    widget->setFixedHeight(28); widget->setCursor(Qt::PointingHandCursor);
    return widget;
}
struct PropertySection { QWidget* widget; QFormLayout* form; };
PropertySection section(QVBoxLayout* target, const QString& id, const QString& title, bool expanded) {
    auto* widget = new QWidget; widget->setObjectName(id); widget->setProperty("uiRole", "section");
    auto* layout = new QVBoxLayout(widget); layout->setContentsMargins(0, 0, 0, 1); layout->setSpacing(0);
    auto* toggle = new QToolButton; toggle->setObjectName(id + "Toggle"); toggle->setProperty("uiRole", "sectionToggle");
    toggle->setText(title); toggle->setCheckable(true); toggle->setChecked(expanded); toggle->setFixedHeight(34);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); toggle->setCursor(Qt::PointingHandCursor);
    auto* content = new QWidget; content->setObjectName(id + "Content");
    auto* form = new QFormLayout(content); form->setContentsMargins(10, 5, 10, 9);
    form->setVerticalSpacing(12); form->setHorizontalSpacing(7); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(toggle); layout->addWidget(content); target->addWidget(widget); content->setVisible(expanded);
    QObject::connect(toggle, &QToolButton::toggled, content, [content, toggle](bool open) {
        content->setVisible(open); toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    return {widget, form};
}
void row(QFormLayout* form, const QString& title, QWidget* field) {
    auto* name = label(title, {}, "settingLabel"); name->setMinimumWidth(80); form->addRow(name, field);
}
QTreeWidgetItem* childOfKind(QTreeWidgetItem* item, const QString& kind) {
    if (!item) return nullptr;
    for (int i = 0; i < item->childCount(); ++i)
        if (item->child(i)->data(0, Qt::UserRole).toString() == kind) return item->child(i);
    return nullptr;
}
class PrototypeSplitter;
class PrototypeHandle : public QSplitterHandle {
public:
    PrototypeHandle(Qt::Orientation orientation, QSplitter* parent) : QSplitterHandle(orientation, parent) {
        setFocusPolicy(Qt::StrongFocus); setCursor(Qt::SplitVCursor);
    }
    std::function<void()> restoreDefaults;
    bool isDragging() const { return !base_.isEmpty(); }
    void cancel() { if (!base_.isEmpty()) { splitter()->setSizes(base_); base_.clear(); releaseMouse(); } }
protected:
    void mousePressEvent(QMouseEvent* event) override { base_ = splitter()->sizes(); QSplitterHandle::mousePressEvent(event); }
    void mouseReleaseEvent(QMouseEvent* event) override { QSplitterHandle::mouseReleaseEvent(event); base_.clear(); }
    void mouseDoubleClickEvent(QMouseEvent* event) override { cancel(); if (restoreDefaults) restoreDefaults(); event->accept(); }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) { cancel(); event->accept(); return; }
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
            auto sizes = splitter()->sizes(); int index = 1;
            while (index < splitter()->count() && splitter()->handle(index) != this) ++index;
            if (index < sizes.size()) { const int delta = event->key() == Qt::Key_Up ? -10 : 10; sizes[index - 1] += delta; sizes[index] -= delta; splitter()->setSizes(sizes); }
            event->accept(); return;
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
    explicit PrototypeSplitter(QWidget* parent = nullptr) : QSplitter(Qt::Vertical, parent) {}
    std::function<void()> restoreDefaults;
    void cancel() { for (int i = 1; i < count(); ++i) static_cast<PrototypeHandle*>(handle(i))->cancel(); }
    bool isDragging() const { for (int i = 1; i < count(); ++i) if (static_cast<PrototypeHandle*>(handle(i))->isDragging()) return true; return false; }
protected:
    QSplitterHandle* createHandle() override {
        auto* handle = new PrototypeHandle(orientation(), this);
        handle->restoreDefaults = [this] { if (restoreDefaults) restoreDefaults(); }; return handle;
    }
};
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setObjectName("SignalStudioWindow"); setWindowTitle("Signal Studio · A1.4.3"); resize(1600, 960); setMinimumSize(720, 520);
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
        QComboBox QAbstractItemView { background:#17263a; selection-background-color:#235476; }
        QCheckBox::indicator { width:14px; height:14px; }
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
    )");
    rightSidebarSaveTimer_ = new QTimer(this); rightSidebarSaveTimer_->setSingleShot(true); rightSidebarSaveTimer_->setInterval(400);
    connect(rightSidebarSaveTimer_, &QTimer::timeout, this, &MainWindow::saveRightSidebarSettings);
    buildMenus(); buildWorkspace(); restoreUiState();
    connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) cancelInteractions();
    });
    refresh(); QTimer::singleShot(0, this, [this] { enforceLayout(!splitterStateRestored_); });
    log("A1.4.3 工作区已就绪；可导入交替 int16 IQ 文件并生成真实图谱");
}
MainWindow::~MainWindow() {
    saveUiState();
    if (rightSidebarSaveTimer_) rightSidebarSaveTimer_->stop();
    saveRightSidebarSettings();
    destroying_ = true; qApp->removeEventFilter(this); disconnect(qApp, nullptr, this, nullptr);
    for (auto* plot : {navigation_, auxiliary_, main_}) disconnect(plot, nullptr, this, nullptr);
    cancelInteractions();
    // Charts must end while the session member they reference is still alive.
    delete maxHost_; maxHost_ = nullptr; delete maxVeil_; maxVeil_ = nullptr;
    delete takeCentralWidget();
}

void MainWindow::buildMenus() {
    menuBar()->setFixedHeight(34); menuBar()->setNativeMenuBar(false);
    auto* brand = new QWidget; auto* brandRow = new QHBoxLayout(brand); brandRow->setContentsMargins(11, 0, 22, 0); brandRow->setSpacing(8);
    auto* mark = label({});
    const auto ratio = devicePixelRatioF(); QPixmap icon(qCeil(22 * ratio), qCeil(22 * ratio)); icon.setDevicePixelRatio(ratio); icon.fill(Qt::transparent);
    QPainter painter(&icon); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(QPen(QColor("#59c6f0"), 1)); painter.setBrush(QColor("#16324a")); painter.drawEllipse(QPointF(11, 11), 9, 9);
    painter.setPen(QPen(QColor("#308ec0"), 1)); painter.setBrush(QColor("#39baf3")); painter.drawEllipse(QPointF(11, 11), 6, 6); painter.end(); mark->setPixmap(icon); brandRow->addWidget(mark);
    auto* name = label("Signal Studio"); name->setStyleSheet("font-size:15px;font-weight:700;"); brandRow->addWidget(name);
    auto* version = label("A1.4.3 · 交互可靠性优化"); version->setStyleSheet("font-size:10px;color:#edc586;border:1px solid #836d44;padding:3px 6px;border-radius:3px;");
    brandRow->addWidget(version); menuBar()->setCornerWidget(brand, Qt::TopLeftCorner);
    auto* file = menuBar()->addMenu("文件(&F)");
    auto* action = file->addAction("▧ 新建工程…", this, &MainWindow::showNewProjectDialog);
    action->setObjectName("newProjectAction"); action->setShortcut(QKeySequence::New);
    action = file->addAction("⇧ 打开工程文件夹…", this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, "打开工程文件夹", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
        if (!path.isEmpty()) openProject(path);
    }); action->setObjectName("openProjectAction"); action->setShortcut(QKeySequence::Open);
    action = file->addAction("打开旧版工程 JSON…", this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "打开旧版工程 JSON", {}, "Signal Studio 工程 (*.json)");
        if (!path.isEmpty()) openProject(path);
    }); action->setObjectName("openProjectJsonAction");
    action = file->addAction("打开宽带演示工程", this, &MainWindow::openDemoProject); action->setObjectName("openDemoProjectAction");
    action = file->addAction("打开窄带演示工程", this, &MainWindow::openNarrowbandDemoProject); action->setObjectName("openNarrowbandDemoProjectAction");
    file->addSeparator();
    addSignalAction_ = file->addAction("▣ 添加 / 打开信号…", this, &MainWindow::showAddFileDialog); addSignalAction_->setObjectName("openIqAction");
    saveAction_ = file->addAction("⇩ 保存工程", this, [this] {
        if (!projectPath_.isEmpty()) { saveProject(projectPath_); return; }
        const auto path = QFileDialog::getSaveFileName(this, "保存工程 JSON", "signal-studio-project.json", "Signal Studio 工程 (*.json)");
        if (!path.isEmpty()) saveProject(path);
    }); saveAction_->setObjectName("saveProjectAction"); saveAction_->setShortcut(QKeySequence::Save);
    recentProjectsMenu_ = file->addMenu("最近打开的工程"); recentProjectsMenu_->setObjectName("recentProjectsMenu");
    updateRecentProjectsMenu();
    removeAction_ = file->addAction("⊖ 从工程移除当前文件", this, [this] {
        cancelInteractions(); const auto* f = session_.activeFile(); if (!f) return;
        QString prompt = "从工程移除文件“" + q(f->metadata.name) + "”？源文件本身不会删除。";
        if (!f->channels.empty()) {
            QStringList names; for (const auto& channel : f->channels) names << q(channel.name);
            prompt += "\n\n以下窄带通道也会移除：\n" + names.join("\n");
        }
        if (QMessageBox::question(this, "移除文件及关联通道", prompt) != QMessageBox::Yes) return;
        session_.removeActiveFile(); if (!session_.activeFile() && maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
        selectionAnchor_.clear(); refresh(); log("已从工程移除 IQ 文件");
    }); removeAction_->setObjectName("removeFileAction");
    // Stable automation entry point; the user-facing file action opens the metadata dialog.
    auto* addDemo = new QAction(this); addDemo->setObjectName("addDemoAction");
    connect(addDemo, &QAction::triggered, this, [this] {
        cancelInteractions();
        const auto* active = session_.activeFile();
        const DisplaySettings fallback = active ? active->display : DisplaySettings{};
        session_.addDemoFile(); applyGlobalRightSidebarSettings(fallback);
        selectionAnchor_.clear(); refresh(); scheduleRightSidebarSettingsSave();
    });
    auto* edit = menuBar()->addMenu("编辑(&E)");
    deleteAction_ = edit->addAction("删除所选标记", this, &MainWindow::deleteMarks); deleteAction_->setObjectName("deleteMarksAction"); deleteAction_->setShortcut(QKeySequence::Delete);
    edit->addAction("重命名当前标记", this, &MainWindow::renameMark);
    auto* view = menuBar()->addMenu("视图(&V)");
    backAction_ = view->addAction("视图后退", this, [this] { cancelInteractions(); session_.back(); propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave(); }); backAction_->setObjectName("backAction"); backAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    forwardAction_ = view->addAction("视图前进", this, [this] { cancelInteractions(); session_.forward(); propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave(); }); forwardAction_->setObjectName("forwardAction"); forwardAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    view->addAction("适应全部数据", this, [this] { cancelInteractions(); if (const auto* f = session_.activeFile()) session_.setView(fullRange(f->metadata)); refresh(); });
    view->addAction("恢复默认视图", this, [this] { cancelInteractions(); session_.resetView(); propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave(); });
    menuBar()->addMenu("分析(&A)")->addAction("算法仅为 UI 演示", this, [this] { log("算法仅为 UI 演示，尚未处理真实 IQ"); });
    menuBar()->addMenu("工具(&T)")->addAction("关于 Signal Studio", this, [this] { log("工程保存文件路径、有效带宽和分析状态；显示参数按全局偏好保存，IQ 样本仍保留在原始文件"); });
    auto* help = menuBar()->addMenu("帮助(&H)");
    auto* interactions = help->addAction("鼠标交互帮助"); interactions->setObjectName("mouseInteractionHelpAction");
    connect(interactions, &QAction::triggered, this, [this] {
        QMessageBox::information(this, "鼠标交互帮助",
            "时频图：X 轴为时间，Y 轴为频率；瀑布图：X 轴为频率，Y 轴为时间（向下递增）。\n\n"
            "图内及底部滚轮缩放 X 轴，左侧滚轮缩放 Y 轴；拖动坐标轴平移。右键菜单开启持续选择信号后，拖动创建标记；"
            "已选框可移动并调整边角，Esc 回滚并退出创建模式。\n\n"
            "辅助图可拖动区间放大；波形时间、PSD 频率与主图联动。全局导航支持单击跳转、拖动时间窗和滚轮缩放。"
            "Ctrl / Shift 多选，Delete 删除所选标记。分隔条双击恢复默认高度，方向键每次调整 10 px。");
    });
}

void MainWindow::buildWorkspace() {
    auto* body = new QWidget; auto* bodyLayout = new QHBoxLayout(body); bodyLayout->setContentsMargins(0, 0, 0, 0); bodyLayout->setSpacing(0); setCentralWidget(body);
    resources_ = new QWidget; resources_->setObjectName("resources"); auto* left = new QVBoxLayout(resources_); left->setContentsMargins(0, 0, 0, 0); left->setSpacing(0);
    auto* resourceHead = new QWidget; resourceHead->setProperty("uiRole", "panelHead"); resourceHead->setFixedHeight(35);
    auto* resourceHeadRow = new QHBoxLayout(resourceHead); resourceHeadRow->setContentsMargins(10, 0, 8, 0);
    resourceTitle_ = label("工程管理"); resourceTitle_->setStyleSheet("font-weight:600;"); resourceHeadRow->addWidget(resourceTitle_); resourceHeadRow->addStretch();
    auto* resourceToggle = push("☰", "resourceToggle", resourceHeadRow); resourceToggle->setProperty("uiRole", "icon"); resourceToggle->setFixedSize(25, 25); resourceToggle->setToolTip("收折 / 展开工程管理"); left->addWidget(resourceHead);
    connect(resourceToggle, &QPushButton::clicked, this, [this] { cancelInteractions(false); resourcesCollapsed_ = !resourcesCollapsed_; enforceLayout(); saveUiState(); });
    resourceTools_ = new QWidget; auto* tools = new QHBoxLayout(resourceTools_); tools->setContentsMargins(8, 7, 8, 7); tools->setSpacing(5);
    auto* add = push("＋ 添加 / 打开信号", "projectAddSignal", tools); add->setProperty("uiRole", "primary"); add->setFixedHeight(28);
    auto* save = push("⇩ 保存工程", "projectSave", tools); save->setProperty("uiRole", "outline"); save->setFixedHeight(28);
    connect(add, &QPushButton::clicked, this, [this] { if (addSignalAction_->isEnabled()) addSignalAction_->trigger(); }); connect(save, &QPushButton::clicked, saveAction_, &QAction::trigger); left->addWidget(resourceTools_);
    auto* resourceScroll = new QScrollArea; resourceBody_ = resourceScroll; resourceScroll->setObjectName("resourceScroll"); resourceScroll->setWidgetResizable(true); resourceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* resourceInner = new QWidget; auto* resourceContent = new QVBoxLayout(resourceInner); resourceContent->setContentsMargins(7, 8, 7, 24); resourceContent->setSpacing(18); resourceScroll->setWidget(resourceInner);
    tree_ = new QTreeWidget; tree_->setObjectName("projectTree"); tree_->setHeaderHidden(true); tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setIndentation(12); tree_->setRootIsDecorated(false); tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); tree_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); tree_->setContextMenuPolicy(Qt::CustomContextMenu); tree_->setMouseTracking(true);
    resourceContent->addWidget(tree_);
    resourceContent->addStretch(); left->addWidget(resourceBody_, 1); bodyLayout->addWidget(resources_);

    auto* workspace = new QWidget; workspace->setObjectName("workspace"); workspace->setMinimumWidth(0);
    auto* center = new QVBoxLayout(workspace); center->setContentsMargins(0, 0, 0, 0); center->setSpacing(0);
    auto* scopeBar = new QWidget; scopeBar->setObjectName("scopeBar"); scopeBar->setFixedHeight(35); auto* scopeRow = new QHBoxLayout(scopeBar); scopeRow->setContentsMargins(12, 0, 12, 0); scopeRow->setSpacing(13);
    auto* workMode = push("宽带时频研判", "workspaceMode", scopeRow); workspaceModeButton_ = workMode;
    workMode->setCheckable(true); workMode->setChecked(true); workMode->setProperty("uiRole", "primary"); workMode->setFixedHeight(28);
    projectLabel_ = label({}, "projectLabel", "hint"); scopeRow->addWidget(projectLabel_); scopeRow->addStretch(); scope_ = label({}, "scopeText", "hint"); scope_->setStyleSheet("font:11px Consolas;color:#8db5d3;"); scopeRow->addWidget(scope_); center->addWidget(scopeBar);
    auto* graphArea = new QWidget; auto* graphLayout = new QVBoxLayout(graphArea); graphLayout->setContentsMargins(5, 5, 5, 5); graphLayout->setSpacing(0);
    auto* splitter = new PrototypeSplitter; graphs_ = splitter; graphs_->setObjectName("graphSplitter"); graphs_->setChildrenCollapsible(false); graphs_->setHandleWidth(10);
    splitter->restoreDefaults = [this] { cancelInteractions(false); enforceLayout(true); };
    graphLayout->addWidget(graphs_);
    workspaceStack_ = new QStackedWidget; workspaceStack_->setObjectName("workspaceStack");
    workspaceStack_->addWidget(graphArea);
    narrowband_ = new NarrowbandWorkspace(session_); workspaceStack_->addWidget(narrowband_);
    narrowband_->setCallbacks([this] { locateActiveChannelSource(); }, [this] {
        const auto* channel = session_.activeChannel(); if (channel) showChannelDialog(q(channel->id));
    }, [this] {
        session_.project().narrowbandWorkspaceOpen = false; workspaceStack_->setCurrentIndex(0);
        workspaceModeButton_->setChecked(true); refresh();
    }, [this](const QString& message) { log(message); });
    center->addWidget(workspaceStack_, 1);
    connect(workMode, &QPushButton::clicked, this, [this, workMode] {
        if (workMode->isChecked()) { session_.project().narrowbandWorkspaceOpen = false; workspaceStack_->setCurrentIndex(0); }
        else if (session_.activeChannel()) { session_.project().narrowbandWorkspaceOpen = true; workspaceStack_->setCurrentIndex(1); narrowband_->refreshFromSession(); }
        else {
            workMode->setChecked(true);
            QMessageBox::information(this, "窄带工作区", "请先从工程树打开一个窄带通道，或从信号标记创建通道。");
        }
        refresh();
    });
    connect(graphs_, &QSplitter::splitterMoved, this, [this] { enforceLayout(); saveUiState(); });
    navigation_ = new PlotWidget(session_, PlotWidget::Kind::Navigation); navigation_->setObjectName("navigationPlot");
    auxiliary_ = new PlotWidget(session_, PlotWidget::Kind::Auxiliary); auxiliary_->setObjectName("auxPlot");
    main_ = new PlotWidget(session_, PlotWidget::Kind::Main); main_->setObjectName("mainPlot");
    // Segment buttons are the visible controls. Hidden combos preserve a single mode state and automation API.
    mainMode_ = combo("modeMain", {"时频图", "瀑布图"}); mainMode_->setParent(this); mainMode_->hide();
    auxMode_ = combo("modeAux", {"时域波形", "功率谱 (PSD)"}); auxMode_->setParent(this); auxMode_->hide();
    palette_ = combo("palette", {"Turbo", "Viridis", "Gray", "Plasma", "Inferno", "Magma", "Cividis", "CoolEdit Classic"}); palette_->setFixedSize(155, 26); palette_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    const std::array<QString, 3> panelIds{"navPanel", "auxPanel", "specPanel"}, names{"全局时间导航", "辅助分析", "宽带图谱"}, maxIds{"navMaximize", "auxMaximize", "specMaximize"};
    const std::array<PlotWidget*, 3> plots{navigation_, auxiliary_, main_};
    for (int index = 0; index < 3; ++index) {
        auto* panel = new QWidget; panel->setObjectName(panelIds[index]); panel->setProperty("uiRole", "chartPanel"); panel->setMinimumWidth(0);
        auto* layout = new QVBoxLayout(panel); layout->setContentsMargins(1, 1, 1, 1); layout->setSpacing(0);
        auto* head = new QWidget; head->setProperty("uiRole", "chartHead"); head->setFixedHeight(32); head->installEventFilter(this); head->setProperty("chartIndex", index);
        auto* header = new QHBoxLayout(head); header->setContentsMargins(9, 0, 9, 0); header->setSpacing(7);
        auto* title = label(names[index]); title->setStyleSheet("font-weight:600;color:#d5e6f5;"); header->addWidget(title);
        if (index == 0) { auto* hint = label("滚轮缩放时间窗 · 拖动蓝色范围 · 单击跳转", "navHint", "hint"); hint->setMinimumWidth(0); header->addWidget(hint); }
        else {
            auto* group = new QWidget; group->setProperty("uiRole", "segmentGroup"); auto* segments = new QHBoxLayout(group); segments->setContentsMargins(2, 2, 2, 2); segments->setSpacing(2);
            auto& buttons = index == 1 ? auxiliaryButtons_ : mainButtons_;
            const QStringList texts = index == 1 ? QStringList{"时域波形", "功率谱 (PSD)"} : QStringList{"时频图", "瀑布图"};
            const QStringList ids = index == 1 ? QStringList{"waveMode", "psdMode"} : QStringList{"timeFrequencyMode", "waterfallMode"};
            for (int mode = 0; mode < 2; ++mode) {
                buttons[mode] = push(texts[mode], ids[mode], segments); buttons[mode]->setProperty("uiRole", "segment"); buttons[mode]->setCheckable(true); buttons[mode]->setFixedHeight(24);
                connect(buttons[mode], &QPushButton::clicked, this, [this, index, mode] { (index == 1 ? auxMode_ : mainMode_)->setCurrentIndex(mode); refresh(); });
            }
            header->addWidget(group);
        }
            if (index == 2) { specMode_ = label({}, "specMode"); specMode_->setStyleSheet("font-size:11px;color:#76c6e8;border:1px solid #2b556a;padding:3px 8px;border-radius:3px;"); header->addWidget(specMode_); }
        header->addStretch();
        if (index == 0) { navStatus_ = label({}, "navStatus", "tag"); header->addWidget(navStatus_); }
        if (index == 1) { auxStatus_ = label({}, "auxStatus", "tag"); header->addWidget(auxStatus_); }
        if (index == 2) {
            specAxis_ = label({}, "specAxisLabel", "hint"); specAxis_->setMinimumWidth(0); specAxis_->setMaximumWidth(150); header->addWidget(specAxis_); header->addWidget(palette_); rangeTag_ = label({}, "rangeTag", "tag"); rangeTag_->setMinimumWidth(0); header->addWidget(rangeTag_);
        }
        auto* maximize = push("⤢", maxIds[index], header); maximize->setProperty("uiRole", "icon"); maximize->setFixedSize(25, 25); maximize->setToolTip("最大化 / 还原");
        connect(maximize, &QPushButton::clicked, this, [this, index] { toggleMaximized(index); }); layout->addWidget(head); layout->addWidget(plots[index], 1);
        panels_[index] = panel; graphs_->addWidget(panel); graphs_->setStretchFactor(index, index == 2 ? 1 : 0);
    }
    for (int i = 1; i < graphs_->count(); ++i) {
        graphs_->handle(i)->setObjectName(i == 1 ? "navSplit" : "auxSplit"); graphs_->handle(i)->installEventFilter(this);
    }
    auto* meta = new QWidget; meta->setObjectName("specMeta"); meta->setFixedHeight(32); auto* metaRow = new QHBoxLayout(meta); metaRow->setContentsMargins(12, 0, 12, 0); metaRow->setSpacing(7);
    cursorData_ = label({}, "cursorData"); viewData_ = label({}, "viewData"); selectionData_ = label({}, "selectionData");
    for (auto* value : {cursorData_, viewData_, selectionData_}) { value->setStyleSheet("font:10px Consolas;color:#a7bbd0;"); metaRow->addWidget(value); }
    selectionData_->setStyleSheet("font:10px Consolas;color:#edc676;"); metaRow->addStretch();
    extract_ = push("基于所选标记创建窄带通道 →", "extract", metaRow); extract_->setProperty("uiRole", "primary"); extract_->setFixedHeight(24);
    connect(extract_, &QPushButton::clicked, this, [this] { cancelInteractions(false); showChannelDialog(); });
    static_cast<QVBoxLayout*>(panels_[2]->layout())->addWidget(meta);

    bottom_ = new QWidget; bottom_->setObjectName("bottom"); auto* bottomLayout = new QVBoxLayout(bottom_); bottomLayout->setContentsMargins(0, 0, 0, 0); bottomLayout->setSpacing(0);
    auto* tabs = new QWidget; tabs->setObjectName("bottomTabs"); tabs->setFixedHeight(29); auto* tabRow = new QHBoxLayout(tabs); tabRow->setContentsMargins(8, 0, 8, 0); tabRow->setSpacing(3);
    const std::array<QString, 3> tabNames{"当前文件结果", "后台任务", "日志"}, tabIds{"resultTab", "taskTab", "logTab"};
    for (int i = 0; i < 3; ++i) { bottomButtons_[i] = push(tabNames[i], tabIds[i], tabRow); bottomButtons_[i]->setCheckable(true); connect(bottomButtons_[i], &QPushButton::clicked, this, [this, i] { cancelInteractions(false); bottomTab_ = i; resultToggle_->setChecked(true); updateBottom(); }); }
    tabRow->addStretch(); resultToggle_ = new QToolButton; resultToggle_->setObjectName("resultsToggle"); resultToggle_->setCheckable(true); resultToggle_->setText("⌃"); resultToggle_->setToolTip("展开 / 收起结果"); resultToggle_->setCursor(Qt::PointingHandCursor); tabRow->addWidget(resultToggle_);
    bottomLayout->addWidget(tabs); bottomContent_ = new QStackedWidget; bottomContent_->setObjectName("bottomContent");
    resultSummary_ = label({}, "resultSummary"); resultSummary_->setAlignment(Qt::AlignTop | Qt::AlignLeft); resultSummary_->setMargin(8);
    taskSummary_ = label("无后台任务；全部是演示数据。", "taskSummary"); taskSummary_->setAlignment(Qt::AlignTop | Qt::AlignLeft); taskSummary_->setMargin(8);
    results_ = new QPlainTextEdit; results_->setObjectName("resultsPanel"); results_->setReadOnly(true); results_->setMaximumBlockCount(300);
    bottomContent_->addWidget(resultSummary_); bottomContent_->addWidget(taskSummary_); bottomContent_->addWidget(results_); bottomLayout->addWidget(bottomContent_); center->addWidget(bottom_);
    connect(resultToggle_, &QToolButton::toggled, this, [this] { cancelInteractions(false); updateBottom(); enforceLayout(); saveUiState(); }); bodyLayout->addWidget(workspace, 1);
    properties_ = new QWidget; properties_->setObjectName("properties"); auto* right = new QVBoxLayout(properties_); right->setContentsMargins(0, 0, 0, 0); right->setSpacing(0);
    auto* propHead = new QWidget; propHead->setProperty("uiRole", "panelHead"); propHead->setFixedHeight(35); auto* propHeadRow = new QHBoxLayout(propHead); propHeadRow->setContentsMargins(10, 0, 8, 0);
    propertiesTitle_ = label("当前文件 / 分析参数"); propertiesTitle_->setStyleSheet("font-weight:600;"); propHeadRow->addWidget(propertiesTitle_); propHeadRow->addStretch();
    auto* closeProps = push("☷", "propClose", propHeadRow); closeProps->setProperty("uiRole", "icon"); closeProps->setFixedSize(25, 25); right->addWidget(propHead);
    connect(closeProps, &QPushButton::clicked, this, [this] { cancelInteractions(false); propertiesCollapsed_ = true; enforceLayout(); saveUiState(); });
    propRail_ = new QWidget; auto* railLayout = new QVBoxLayout(propRail_); railLayout->setContentsMargins(3, 12, 3, 0);
    auto* openProps = push("⚙", "propRailOpen", railLayout); openProps->setProperty("uiRole", "icon"); openProps->setFixedSize(30, 30); openProps->setToolTip("展开参数"); railLayout->addStretch(); right->addWidget(propRail_, 1);
    connect(openProps, &QPushButton::clicked, this, [this] { cancelInteractions(false); propertiesCollapsed_ = false; enforceLayout(); saveUiState(); });
    propScroll_ = new QScrollArea; propScroll_->setObjectName("propScroll"); propScroll_->setWidgetResizable(true); propScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget; content->setObjectName("propContent");
    content->setMinimumWidth(0); content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    propertyLayout_ = new QVBoxLayout(content); propertyLayout_->setContentsMargins(0, 0, 0, 20); propertyLayout_->setSpacing(0);
    auto file = section(propertyLayout_, "fileSection", "活动信号文件", true); fileSection_ = file.widget;
    const std::array<QString, 4> fileNames{"文件", "采样率", "中心频率", "完整时间范围"}, fileIds{"fileName", "sampleRate", "centerFreq", "fullRange"};
    for (int i = 0; i < 4; ++i) { fileValues_[i] = output(fileIds[i]); row(file.form, fileNames[i], fileValues_[i]); }
    effectiveBandwidth_ = new QDoubleSpinBox; effectiveBandwidth_->setObjectName("effectiveBandwidthMHz");
    effectiveBandwidth_->setMinimumWidth(0); effectiveBandwidth_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    effectiveBandwidth_->setRange(0.001, 1'000'000.0); effectiveBandwidth_->setDecimals(6); effectiveBandwidth_->setSingleStep(1.0); effectiveBandwidth_->setSuffix(" MHz");
    effectiveBandwidth_->setToolTip("限定时频图和功率谱使用的中心有效带宽；最大值受采样率限制"); row(file.form, "有效带宽", effectiveBandwidth_);
    auto view = section(propertyLayout_, "viewSection", "当前视图（仅显示状态）", true); viewSection_ = view.widget;
    viewValues_[0] = output("visibleTime"); viewValues_[1] = output("visibleFreq"); row(view.form, "时间范围", viewValues_[0]); row(view.form, "频率范围", viewValues_[1]);
    auto mark = section(propertyLayout_, "markSection", "当前信号标记（非放缩区域）", true); markSection_ = mark.widget;
    const std::array<QString, 3> markNames{"开始时间", "结束时间", "频率范围"}, markIds{"selStart", "selEnd", "selFreq"};
    for (int i = 0; i < 3; ++i) { markValues_[i] = output(markIds[i]); row(mark.form, markNames[i], markValues_[i]); }
    selectedCount_ = label({}, "selectedCount", "help"); selectedCount_->setWordWrap(true); mark.form->addRow(selectedCount_);
    auto* markActions = new QWidget; auto* markRow = new QHBoxLayout(markActions); markRow->setContentsMargins(0, 0, 0, 0); markRow->setSpacing(5);
    rename_ = push("重命名", "renameMark", markRow); locate_ = push("定位", "locateMark", markRow); delete_ = push("删除所选", "deleteMarks", markRow);
    for (auto* action : {rename_, locate_, delete_}) action->setProperty("uiRole", "outline"); markRow->addStretch(); mark.form->addRow(markActions);
    connect(rename_, &QPushButton::clicked, this, &MainWindow::renameMark); connect(locate_, &QPushButton::clicked, this, &MainWindow::locateMark); connect(delete_, &QPushButton::clicked, this, &MainWindow::deleteMarks);
    auto channel = section(propertyLayout_, "narrowbandChannelSection", "窄带通道参数", true);
    channelSection_ = channel.widget;
    const std::array<QString, 6> channelNames{"来源文件", "来源标记", "射频中心", "有效带宽", "输出采样率", "源样本时段"};
    const std::array<QString, 6> channelIds{"narrowbandSourceFile", "narrowbandSourceMark", "narrowbandCenter", "narrowbandBandwidth", "narrowbandOutputRate", "narrowbandSourceTime"};
    for (std::size_t index = 0; index < channelValues_.size(); ++index) {
        channelValues_[index] = output(channelIds[index]);
        row(channel.form, channelNames[index], channelValues_[index]);
    }
    auto* channelActions = new QWidget; auto* channelRow = new QHBoxLayout(channelActions);
    channelRow->setContentsMargins(0, 5, 0, 0); channelRow->setSpacing(5);
    auto* locateChannel = push("⌖ 来源定位", "propertyLocateChannelSource", channelRow);
    auto* editChannel = push("修改参数…", "propertyEditChannel", channelRow);
    locateChannel->setProperty("uiRole", "outline"); editChannel->setProperty("uiRole", "outline");
    channel.form->addRow(channelActions);
    connect(locateChannel, &QPushButton::clicked, this, &MainWindow::locateActiveChannelSource);
    connect(editChannel, &QPushButton::clicked, this, [this] {
        if (const auto* active = session_.activeChannel()) showChannelDialog(q(active->id));
    });
    auto psd = section(propertyLayout_, "psdSection", "辅助图设置", true); psdSection_ = psd.widget;
    waveformMode_ = combo("waveformMode", {"I 分量（ADC 计数）", "Q 分量（ADC 计数）", "幅度（RMS，ADC 计数）", "幅度包络（ADC 计数）"}); row(psd.form, "时域波形", waveformMode_);
    psd_ = combo("psdFft", {"1024", "2048", "4096", "8192"}); psdScope_ = combo("psdScope", {"当前可见时间窗", "当前活动信号标记"}); row(psd.form, "PSD FFT 点数", psd_); row(psd.form, "统计时间范围", psdScope_);
    auto spec = section(propertyLayout_, "specSection", "时频图设置", true);
    QStringList stftSizes; for (int order = 8; order <= 16; ++order) stftSizes << QString::number(1 << order);
    stft_ = combo("stftFft", stftSizes); propertyPalette_ = combo("colormap", {"Turbo", "Viridis", "Gray", "Plasma", "Inferno", "Magma", "Cividis", "CoolEdit Classic"});
    dynamic_ = combo("dynamic", {"20 dB", "40 dB", "60 dB", "80 dB", "100 dB", "120 dB"});
    reference_ = combo("reference", {"0 dBFS", "-20 dBFS", "-40 dBFS", "-60 dBFS", "-80 dBFS", "-100 dBFS"});
    dynamic_->setEditable(true); reference_->setEditable(true);
    dynamic_->setInsertPolicy(QComboBox::NoInsert); reference_->setInsertPolicy(QComboBox::NoInsert);
    dynamic_->lineEdit()->setValidator(new QDoubleValidator(1, 10000, 3, dynamic_->lineEdit()));
    reference_->lineEdit()->setValidator(new QDoubleValidator(-200, 100, 3, reference_->lineEdit()));
    freqMode_ = combo("freqMode", {"射频绝对频率", "基带相对频率"});
    for (int i = 0; i < dynamic_->count(); ++i) dynamic_->setItemData(i, 20 + 20 * i);
    for (int i = 0; i < reference_->count(); ++i) reference_->setItemData(i, -20 * i);
    row(spec.form, "STFT FFT 点数", stft_); row(spec.form, "颜色映射", propertyPalette_); row(spec.form, "动态范围", dynamic_); row(spec.form, "参考电平", reference_); row(spec.form, "频率坐标", freqMode_);
    colorScale_ = new QCheckBox; colorScale_->setObjectName("colorbarToggle"); grid_ = new QCheckBox; grid_->setObjectName("gridToggle"); row(spec.form, "显示色阶条", colorScale_); row(spec.form, "网格显示", grid_);
    propertyLayout_->addStretch(); propScroll_->setWidget(content); right->addWidget(propScroll_, 1); bodyLayout->addWidget(properties_);

    graphArea->setObjectName("graphArea"); graphArea->installEventFilter(this);
    empty_ = new QWidget(graphArea); empty_->setObjectName("emptyWorkspace"); empty_->setStyleSheet("background:#0d1725;");
    auto* emptyLayout = new QVBoxLayout(empty_); emptyLayout->setSpacing(12); emptyLayout->addStretch();
    auto* emptyTitle = label("开始宽带数据分析", "emptyWorkflowTitle"); emptyTitle->setStyleSheet("font-size:22px;font-weight:600;color:#dce9f6;"); emptyTitle->setAlignment(Qt::AlignCenter); emptyLayout->addWidget(emptyTitle);
    auto* emptyHelp = label("按顺序完成工程、信号和宽带数据操作", "emptyWorkflowHelp", "help"); emptyHelp->setAlignment(Qt::AlignCenter); emptyLayout->addWidget(emptyHelp);
    auto* workflow = label("01  工程     →     02  信号     →     03  宽带数据", "emptyWorkflowSteps"); workflow->setAlignment(Qt::AlignCenter); workflow->setStyleSheet("font-size:14px;color:#79b8de;padding:14px;"); emptyLayout->addWidget(workflow);
    auto* projectButtons = new QWidget(empty_); projectButtons->setObjectName("emptyProjectActions"); auto* projectButtonRow = new QHBoxLayout(projectButtons); projectButtonRow->setContentsMargins(0, 0, 0, 0); projectButtonRow->setSpacing(8);
    auto* newProject = push("▧ 新建工程…", "emptyNewProject", projectButtonRow); newProject->setProperty("uiRole", "primary");
    auto* openProject = push("⇧ 打开工程…", "emptyOpenProject", projectButtonRow); openProject->setProperty("uiRole", "outline");
    auto* demoProject = push("打开演示工程", "emptyDemoProject", projectButtonRow); demoProject->setProperty("uiRole", "outline");
    emptyLayout->addWidget(projectButtons, 0, Qt::AlignCenter);
    auto* emptyAdd = push("＋ 添加 / 打开信号…", "emptyAddSignal", emptyLayout); emptyAdd->setProperty("uiRole", "primary");
    emptyLayout->addStretch();
    connect(newProject, &QPushButton::clicked, this, [this] { findChild<QAction*>("newProjectAction")->trigger(); });
    connect(openProject, &QPushButton::clicked, this, [this] { findChild<QAction*>("openProjectAction")->trigger(); });
    connect(demoProject, &QPushButton::clicked, this, [this] { findChild<QAction*>("openDemoProjectAction")->trigger(); });
    connect(emptyAdd, &QPushButton::clicked, this, [this] { if (addSignalAction_->isEnabled()) addSignalAction_->trigger(); });
    maxVeil_ = new QWidget(this); maxVeil_->setObjectName("maximizeVeil"); maxVeil_->setStyleSheet("background:rgba(3,9,21,217);"); maxVeil_->hide(); maxVeil_->installEventFilter(this);
    maxHost_ = new QWidget(this); maxHost_->setObjectName("maximizeHost"); maxHost_->setStyleSheet("background:#121f32;"); auto* maxLayout = new QVBoxLayout(maxHost_); maxLayout->setContentsMargins(0, 0, 0, 0); maxHost_->hide();

    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (refreshing_) return;
        if (item->data(0, Qt::UserRole).toString() == "channel") {
            activateTreeChannel(item->data(0, Qt::UserRole + 1).toString()); return;
        }
        if (item->data(0, Qt::UserRole).toString() == "mark") {
            if (!(treeSelectionModifiers_ & Qt::ShiftModifier)) selectionAnchor_ = item->data(0, Qt::UserRole + 1).toString().toStdString();
            return;
        }
        if (item->data(0, Qt::UserRole).toString() != "file") return;
        const auto id = item->data(0, Qt::UserRole + 1).toString().toStdString();
        QTimer::singleShot(0, this, [this, id] { cancelInteractions(); if (session_.activateFile(id)) { selectionAnchor_.clear(); refresh(); } });
    });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (refreshing_) return; const auto* current = tree_->currentItem(); if (!current || current->data(0, Qt::UserRole).toString() != "mark") return;
        std::vector<std::string> ids; for (auto* item : tree_->selectedItems()) if (item->data(0, Qt::UserRole).toString() == "mark") ids.push_back(item->data(0, Qt::UserRole + 1).toString().toStdString());
        session_.selectMarks(std::move(ids), current->data(0, Qt::UserRole + 1).toString().toStdString());
        if (auto* active = session_.activeFile()) active->display.psdFromSelection = sharedPsdFromSelectionPreference_ && findMark(*active, active->activeMarkId);
        propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave();
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item->data(0, Qt::UserRole).toString() == "mark") { cancelInteractions(); session_.focusMark(item->data(0, Qt::UserRole + 1).toString().toStdString()); session_.project().narrowbandWorkspaceOpen = false; workspaceStack_->setCurrentIndex(0); refresh(); }
        else if (item->data(0, Qt::UserRole).toString() == "channel") activateTreeChannel(item->data(0, Qt::UserRole + 1).toString());
    });
    connect(tree_, &QTreeWidget::itemEntered, this, [this](QTreeWidgetItem* item, int) { main_->setHoveredMark(item->data(0, Qt::UserRole).toString() == "mark" ? item->data(0, Qt::UserRole + 1).toString() : QString{}); });
    tree_->viewport()->installEventFilter(this);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](QPoint point) {
        auto* item = tree_->itemAt(point); if (!item) return;
        const auto kind = item->data(0, Qt::UserRole).toString();
        if (kind == "channel") {
            const auto channelId = item->data(0, Qt::UserRole + 1).toString();
            QMenu menu(this);
            menu.addAction("打开窄带工作区", this, [this, channelId] { activateTreeChannel(channelId); });
            menu.addAction("修改通道参数…", this, [this, channelId] { showChannelDialog(channelId); });
            menu.addAction("定位来源标记", this, [this, channelId] { session_.activateChannel(channelId.toStdString()); locateActiveChannelSource(); });
            menu.addAction("重命名通道…", this, [this, channelId] {
                auto* file = session_.fileForChannel(channelId.toStdString()); if (!file) return;
                auto it = std::find_if(file->channels.begin(), file->channels.end(), [&](const Channel& value) { return value.id == channelId.toStdString(); });
                if (it == file->channels.end()) return; bool ok = false;
                const auto name = QInputDialog::getText(this, "重命名窄带通道", "通道名称", QLineEdit::Normal, q(it->name), &ok).trimmed();
                if (ok && !name.isEmpty() && name.size() <= 80) { auto updated = *it; updated.name = name.toStdString(); session_.updateChannel(updated.id, updated); refresh(); }
            });
            menu.addAction("删除通道…", this, [this, channelId] {
                auto* file = session_.fileForChannel(channelId.toStdString()); if (!file) return;
                const auto* channel = [&]() -> const Channel* { for (const auto& value : file->channels) if (value.id == channelId.toStdString()) return &value; return nullptr; }();
                if (!channel || QMessageBox::question(this, "删除窄带通道", "删除通道“" + q(channel->name) + "”？") != QMessageBox::Yes) return;
                auto& channels = file->channels;
                channels.erase(std::remove_if(channels.begin(), channels.end(), [&](const Channel& value) { return value.id == channelId.toStdString(); }), channels.end());
                if (session_.project().activeChannelId == channelId.toStdString()) { session_.project().activeChannelId.clear(); session_.project().narrowbandWorkspaceOpen = false; }
                refresh(); log("已删除窄带通道");
            });
            menu.exec(tree_->viewport()->mapToGlobal(point)); return;
        }
        if (kind == "file") {
            const auto fileId = item->data(0, Qt::UserRole + 1).toString();
            QMenu menu(this); menu.addAction("打开宽带工作区", this, [this, fileId] { session_.activateFile(fileId.toStdString()); session_.project().narrowbandWorkspaceOpen = false; refresh(); });
            menu.addAction("添加 / 打开 IQ…", this, &MainWindow::showAddFileDialog); menu.exec(tree_->viewport()->mapToGlobal(point)); return;
        }
        if (kind != "mark") return;
        const auto id = item->data(0, Qt::UserRole + 1).toString(); const auto* f = session_.activeFile(); if (!f) return;
        if (std::find(f->selectedMarkIds.begin(), f->selectedMarkIds.end(), id.toStdString()) == f->selectedMarkIds.end()) selectMark(id, Qt::NoModifier);
        cancelInteractions(false); QMenu menu(this); menu.addAction("重命名当前标记", this, &MainWindow::renameMark); menu.addAction("定位当前标记", this, &MainWindow::locateMark);
        menu.addAction(QString("删除所选标记 (%1)").arg(session_.activeFile()->selectedMarkIds.size()), this, &MainWindow::deleteMarks); menu.addSeparator(); menu.addAction(backAction_); menu.addAction(forwardAction_);
        menu.addSeparator(); menu.addAction("适应全部数据", this, [this] { if (const auto* active = session_.activeFile()) session_.setView(fullRange(active->metadata)); refresh(); }); menu.addAction("恢复默认视图", this, [this] { session_.resetView(); propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave(); }); menu.exec(tree_->viewport()->mapToGlobal(point));
    });
    for (auto* plot : {navigation_, auxiliary_, main_}) {
        connect(plot, &PlotWidget::stateChanged, this, [this] { propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave(); });
        connect(plot, &PlotWidget::statusMessage, this, &MainWindow::log);
        connect(plot, &PlotWidget::markSelectionRequested, this, &MainWindow::selectMark);
        connect(plot, &PlotWidget::cursorChanged, this, &MainWindow::updateCursor);
        connect(plot, &PlotWidget::interactionHint, this, [this](const QString& text) { statusBar()->showMessage(text, 2500); });
        connect(plot, &PlotWidget::markRenameRequested, this, &MainWindow::renameMark);
        connect(plot, &PlotWidget::markDeleteRequested, this, &MainWindow::deleteMarks);
    }
    auto change = [this] {
        if (refreshing_) return; auto* f = session_.activeFile(); if (!f) return;
        const auto fileId = f->metadata.id; auto display = f->display;
        const auto nextAux = static_cast<AuxiliaryMode>(auxMode_->currentIndex());
        display.auxiliaryMode = nextAux;
        display.mainMode = static_cast<MainMode>(mainMode_->currentIndex()); display.palette = static_cast<Palette>(palette_->currentIndex());
        display.waveformMode = static_cast<WaveformMode>(waveformMode_->currentIndex());
        const auto comboNumber = [](QComboBox* control, const QString& suffix, double fallback) {
            bool ok = false;
            double value = control->currentText().trimmed().remove(suffix).trimmed().toDouble(&ok);
            if (!ok) value = control->currentData().toDouble(&ok);
            return ok ? value : fallback;
        };
        display.dynamicRangeDb = comboNumber(dynamic_, "dB", display.dynamicRangeDb);
        display.referenceLevelDb = comboNumber(reference_, "dBFS", display.referenceLevelDb);
        display.absoluteFrequency = freqMode_->currentIndex() == 0; display.grid = grid_->isChecked(); display.colorScale = colorScale_->isChecked();
        display.psdSize = psd_->currentText().toInt(); display.stftSize = stft_->currentText().toInt();
        if (sender() == psdScope_) sharedPsdFromSelectionPreference_ = psdScope_->currentIndex() == 1;
        display.psdFromSelection = sharedPsdFromSelectionPreference_ && findMark(*f, f->activeMarkId);
        if (sharedPsdFromSelectionPreference_ && !display.psdFromSelection) log("请先选择当前文件的信号标记；PSD 统计来源仍为当前可见时间窗");
        cancelInteractions(false); f = session_.activeFile(); if (!f || f->metadata.id != fileId) return;
        // Use restored ranges after cancellation; controls above may have synchronously refreshed.
        display.waveformMin = f->display.waveformMin; display.waveformMax = f->display.waveformMax;
        display.psdMin = f->display.psdMin; display.psdMax = f->display.psdMax;
        if (f->display.auxiliaryMode == AuxiliaryMode::Waveform) { display.waveformMin = f->display.auxiliaryMin; display.waveformMax = f->display.auxiliaryMax; }
        else { display.psdMin = f->display.auxiliaryMin; display.psdMax = f->display.auxiliaryMax; }
        display.auxiliaryMin = nextAux == AuxiliaryMode::Waveform ? display.waveformMin : display.psdMin;
        display.auxiliaryMax = nextAux == AuxiliaryMode::Waveform ? display.waveformMax : display.psdMax;
        const double effectiveBandwidthHz = effectiveBandwidth_->value() * 1e6;
        const bool waveformChanged = display.waveformMode != f->display.waveformMode;
        if (waveformChanged) display.waveformAutoFit = true;
        f->display = display;
        if (std::abs(f->metadata.effectiveBandwidthHz - effectiveBandwidthHz) > 0.5)
            session_.setEffectiveBandwidthHz(effectiveBandwidthHz);
        f = session_.activeFile(); if (!f || f->metadata.id != fileId) return;
        session_.setView(f->view, false); propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave();
    };
    for (auto* control : {mainMode_, auxMode_, waveformMode_, palette_, psd_, stft_, psdScope_, freqMode_})
        connect(control, &QComboBox::currentIndexChanged, this, change);
    for (auto* control : {dynamic_, reference_})
        connect(control, &QComboBox::currentIndexChanged, this, [change](int index) { if (index >= 0) change(); });
    for (auto* control : {dynamic_, reference_})
        connect(control->lineEdit(), &QLineEdit::editingFinished, this, change);
    connect(effectiveBandwidth_, &QDoubleSpinBox::valueChanged, this, change);
    connect(propertyPalette_, &QComboBox::currentIndexChanged, this, [this](int index) { if (!refreshing_) palette_->setCurrentIndex(index); });
    for (auto* control : {grid_, colorScale_}) connect(control, &QCheckBox::toggled, this, change);
    statusBar()->setFixedHeight(25); statusBar()->setSizeGripEnabled(false);
    auto* ready = label("● 就绪", "renderingBackend"); ready->setStyleSheet("font:11px Consolas;color:#91a9c0;"); statusBar()->addWidget(ready);
    statusTime_ = label({}, "statusTime"); statusFrequency_ = label({}, "statusFreq"); statusFile_ = label({}, "statusSignal");
    for (auto* value : {statusTime_, statusFrequency_, statusFile_}) { value->setStyleSheet("font:11px Consolas;color:#91a9c0;border-left:1px solid #294058;padding:0 12px;"); statusBar()->addWidget(value); }
    dataSourceStatus_ = label("演示数据 · 未运行 DSP"); dataSourceStatus_->setObjectName("dataSourceStatus");
    dataSourceStatus_->setStyleSheet("font:11px Consolas;color:#91a9c0;padding:0 12px;"); statusBar()->addPermanentWidget(dataSourceStatus_);
    connect(main_, &PlotWidget::backendChanged, ready, [ready](const QString& description) { ready->setToolTip(description); });
    for (auto* toggle : propScroll_->findChildren<QToolButton*>())
        if (toggle->property("uiRole").toString() == "sectionToggle")
            connect(toggle, &QToolButton::toggled, this, [this] { saveUiState(); });
    ready->setToolTip(main_->renderingBackend()); updateBottom(); qApp->installEventFilter(this);
}

void MainWindow::rebuildTree() {
    QSignalBlocker block(tree_); tree_->clear();
    auto* root = new QTreeWidgetItem(tree_, {"▾ ◧ " + q(session_.project().name)}); root->setExpanded(true); root->setFlags(Qt::ItemIsEnabled); root->setData(0, Qt::UserRole, "project"); root->setSizeHint(0, QSize(0, 35)); root->setBackground(0, QColor("#1b2f45"));
    QFont bold = tree_->font(); bold.setBold(true); root->setFont(0, bold);
    for (const auto& file : session_.project().files) {
        auto* item = new QTreeWidgetItem(root, {"◈ " + q(file.metadata.name)}); item->setData(0, Qt::UserRole, "file"); item->setData(0, Qt::UserRole + 1, q(file.metadata.id)); item->setFlags(Qt::ItemIsEnabled); item->setSizeHint(0, QSize(0, 32)); item->setExpanded(true);
        item->setToolTip(0, q(file.metadata.path)); const bool active = file.metadata.id == session_.project().activeFileId;
        if (active) { item->setBackground(0, QColor("#214c70")); item->setForeground(0, QColor("#e0f5ff")); }
        const double duration = static_cast<double>(file.metadata.sampleCount) / file.metadata.sampleRateHz;
        const QString bandwidth = file.metadata.declaredBandwidthHz > 0 ? " · BW " + number(file.metadata.declaredBandwidthHz / 1e6) + " MHz" : QString{};
        auto* info = new QTreeWidgetItem(item, {number(file.metadata.sampleRateHz / 1e6) + " MS/s · " + number(file.metadata.centerFrequencyHz / 1e6) + " MHz" + bandwidth + " · " + number(duration) + " s"});
        info->setData(0, Qt::UserRole, "metadata"); info->setFlags(Qt::ItemIsEnabled); info->setForeground(0, QColor("#8da8c2")); info->setFont(0, QFont("Consolas", 8)); info->setSizeHint(0, QSize(0, 25));
        if (!active) continue;
        auto* marks = new QTreeWidgetItem(item, {QString("信号区域 (%1) · 已选 %2").arg(file.marks.size()).arg(file.selectedMarkIds.size())}); marks->setData(0, Qt::UserRole, "marks"); marks->setFlags(Qt::ItemIsEnabled); marks->setExpanded(true); marks->setSizeHint(0, QSize(0, 30)); marks->setForeground(0, QColor("#93b9d6"));
        auto* actions = new QTreeWidgetItem(marks); actions->setFlags(Qt::ItemIsEnabled); actions->setData(0, Qt::UserRole, "treeActions"); actions->setSizeHint(0, QSize(0, 30));
        auto* controls = new QWidget; auto* actionRow = new QHBoxLayout(controls); actionRow->setContentsMargins(0, 1, 0, 1); actionRow->setSpacing(4);
        auto* all = push("全选", "treeSelectAll", actionRow); auto* remove = push("删除所选", "treeDeleteMarks", actionRow); all->setProperty("uiRole", "outline"); remove->setProperty("uiRole", "outline"); all->setEnabled(!file.marks.empty()); remove->setEnabled(!file.selectedMarkIds.empty()); actionRow->addStretch(); tree_->setItemWidget(actions, 0, controls);
        connect(all, &QPushButton::clicked, this, &MainWindow::selectAllMarks); connect(remove, &QPushButton::clicked, this, &MainWindow::deleteMarks);
        for (const auto& mark : file.marks) {
            auto* node = new QTreeWidgetItem(marks, {"▱ " + q(mark.name)}); node->setData(0, Qt::UserRole, "mark"); node->setData(0, Qt::UserRole + 1, q(mark.id)); node->setSizeHint(0, QSize(0, 31)); node->setToolTip(0, "单击选择 · Ctrl 多选 · Shift 连选 · 双击定位");
            node->setSelected(std::find(file.selectedMarkIds.begin(), file.selectedMarkIds.end(), mark.id) != file.selectedMarkIds.end()); if (mark.id == file.activeMarkId) tree_->setCurrentItem(node, 0, QItemSelectionModel::NoUpdate);
        }
        auto* channels = new QTreeWidgetItem(item, {QString("窄带通道 (%1)").arg(file.channels.size())}); channels->setData(0, Qt::UserRole, "channels"); channels->setFlags(Qt::ItemIsEnabled); channels->setExpanded(true); channels->setSizeHint(0, QSize(0, 30)); channels->setForeground(0, QColor("#93b9d6"));
        for (const auto& channel : file.channels) {
            auto* node = new QTreeWidgetItem(channels, {"◇ " + q(channel.name) + " · " + coordinate(channel.centerFrequencyHz, channel.bandwidthHz, false)});
            node->setData(0, Qt::UserRole, "channel"); node->setData(0, Qt::UserRole + 1, q(channel.id));
            node->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); node->setSizeHint(0, QSize(0, 31));
            node->setToolTip(0, channel.processingState == ChannelProcessingState::Ready ? "打开真实窄带分析工作区" : "通道配置待确认");
            if (channel.id == session_.project().activeChannelId) { node->setBackground(0, QColor("#3d442f")); node->setForeground(0, QColor("#fff0bf")); }
        }
        if (file.activeMarkId.empty()) tree_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
    }
    treeStructure_ = treeStructure();
    int height = 35;
    for (const auto& file : session_.project().files) {
        height += 57;
        if (file.metadata.id == session_.project().activeFileId) height += 90 + 31 * static_cast<int>(file.marks.size() + file.channels.size());
    }
    tree_->setFixedHeight(height + 2);
}

QStringList MainWindow::treeStructure() const {
    QStringList structure{q(session_.project().activeFileId)};
    for (const auto& file : session_.project().files) {
        structure.append("file:" + q(file.metadata.id));
        if (file.metadata.id == session_.project().activeFileId) {
            for (const auto& mark : file.marks) structure.append("mark:" + q(mark.id));
            for (const auto& channel : file.channels) structure.append("channel:" + q(channel.id));
        }
    }
    return structure;
}

void MainWindow::restoreUiState() {
    if (!qApp->property("uiStatePersistenceEnabled").toBool()) return;
    restoringUiState_ = true;
    QSettings settings;
    settings.beginGroup(QStringLiteral("ui"));
    const auto geometry = settings.value(QStringLiteral("geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    resourcesCollapsed_ = settings.value(QStringLiteral("resourcesCollapsed"), false).toBool();
    propertiesCollapsed_ = settings.value(QStringLiteral("propertiesCollapsed"), false).toBool();
    resultToggle_->setChecked(settings.value(QStringLiteral("resultsExpanded"), false).toBool());
    bottomTab_ = std::clamp(settings.value(QStringLiteral("bottomTab"), 0).toInt(), 0, 2);
    for (const auto& id : {"fileSection", "viewSection", "markSection", "psdSection", "specSection"}) {
        if (auto* toggle = findChild<QToolButton*>(QString::fromLatin1(id) + QStringLiteral("Toggle")))
            toggle->setChecked(settings.value(QStringLiteral("section/") + QString::fromLatin1(id), true).toBool());
    }
    const auto splitterState = settings.value(QStringLiteral("graphSplitter")).toByteArray();
    splitterStateRestored_ = !splitterState.isEmpty() && graphs_->restoreState(splitterState);
    settings.endGroup();
    restoringUiState_ = false;
    updateBottom();
}

void MainWindow::saveUiState() {
    if (restoringUiState_ || !graphs_ || !qApp->property("uiStatePersistenceEnabled").toBool()) return;
    QSettings settings;
    settings.beginGroup(QStringLiteral("ui"));
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("resourcesCollapsed"), resourcesCollapsed_);
    settings.setValue(QStringLiteral("propertiesCollapsed"), propertiesCollapsed_);
    settings.setValue(QStringLiteral("resultsExpanded"), resultToggle_->isChecked());
    settings.setValue(QStringLiteral("bottomTab"), bottomTab_);
    settings.setValue(QStringLiteral("graphSplitter"), graphs_->saveState());
    for (const auto& id : {"fileSection", "viewSection", "markSection", "psdSection", "specSection"}) {
        if (auto* toggle = findChild<QToolButton*>(QString::fromLatin1(id) + QStringLiteral("Toggle")))
            settings.setValue(QStringLiteral("section/") + QString::fromLatin1(id), toggle->isChecked());
    }
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
    if (values.isEmpty() || values.value(QStringLiteral("sourcePath")).toString() != sidebarSourcePath(file.metadata)) return;

    bool ok = false;
    const double bandwidth = values.value(QStringLiteral("effectiveBandwidthHz"), file.metadata.effectiveBandwidthHz).toDouble(&ok);
    if (ok && std::isfinite(bandwidth) && bandwidth >= file.metadata.sampleRateHz / 65536.0 && bandwidth <= file.metadata.sampleRateHz)
        file.metadata.effectiveBandwidthHz = bandwidth;
}

void MainWindow::applyGlobalRightSidebarSettings(const DisplaySettings& fallback) {
    DisplaySettings shared = fallback;
    QVariantMap values;
    if (qApp->property("uiStatePersistenceEnabled").toBool()) {
        QSettings settings;
        settings.beginGroup(QStringLiteral("rightSidebar"));
        values = settings.value(QStringLiteral("globalSettings")).toMap();
        if (values.isEmpty()) {
            if (const auto* active = session_.activeFile()) {
                const auto legacy = settings.value(sidebarSettingsKey(active->metadata)).toMap();
                if (legacy.value(QStringLiteral("sourcePath")).toString() == sidebarSourcePath(active->metadata))
                    values = legacy;
            }
        }
        settings.endGroup();
    }
    const auto integer = [&values](const QString& name, int fallbackValue) {
        bool ok = false; const int value = values.value(name).toInt(&ok); return ok ? value : fallbackValue;
    };
    const auto real = [&values](const QString& name, double fallbackValue) {
        bool ok = false; const double value = values.value(name).toDouble(&ok); return ok && std::isfinite(value) ? value : fallbackValue;
    };
    const auto supportedSize = [](int size, int minimum, int maximum) {
        return size >= minimum && size <= maximum && (size & (size - 1)) == 0;
    };
    const int mainMode = integer(QStringLiteral("mainMode"), static_cast<int>(shared.mainMode));
    if (mainMode >= 0 && mainMode <= 1) shared.mainMode = static_cast<MainMode>(mainMode);
    const int auxiliaryMode = integer(QStringLiteral("auxiliaryMode"), static_cast<int>(shared.auxiliaryMode));
    if (auxiliaryMode >= 0 && auxiliaryMode <= 1) shared.auxiliaryMode = static_cast<AuxiliaryMode>(auxiliaryMode);
    const int waveformMode = integer(QStringLiteral("waveformMode"), static_cast<int>(shared.waveformMode));
    if (waveformMode >= 0 && waveformMode <= 3) shared.waveformMode = static_cast<WaveformMode>(waveformMode);
    const int palette = integer(QStringLiteral("palette"), static_cast<int>(shared.palette));
    if (palette >= 0 && palette <= static_cast<int>(Palette::CoolEditClassic)) shared.palette = static_cast<Palette>(palette);
    const int stftSize = integer(QStringLiteral("stftSize"), shared.stftSize);
    if (supportedSize(stftSize, 256, 65536)) shared.stftSize = stftSize;
    const int psdSize = integer(QStringLiteral("psdSize"), shared.psdSize);
    if (supportedSize(psdSize, 1024, 8192)) shared.psdSize = psdSize;
    const double dynamicRange = real(QStringLiteral("dynamicRangeDb"), shared.dynamicRangeDb);
    if (dynamicRange > 0 && dynamicRange <= 10000) shared.dynamicRangeDb = dynamicRange;
    const double referenceLevel = real(QStringLiteral("referenceLevelDb"), shared.referenceLevelDb);
    if (referenceLevel >= -200 && referenceLevel <= 100) shared.referenceLevelDb = referenceLevel;
    shared.absoluteFrequency = values.value(QStringLiteral("absoluteFrequency"), shared.absoluteFrequency).toBool();
    shared.grid = values.value(QStringLiteral("grid"), shared.grid).toBool();
    shared.waveformAutoFit = values.value(QStringLiteral("waveformAutoFit"), true).toBool();
    shared.colorScale = values.value(QStringLiteral("colorScale"), shared.colorScale).toBool();
    sharedPsdFromSelectionPreference_ = values.value(QStringLiteral("psdFromSelection"), shared.psdFromSelection).toBool();
    shared.waveformMin = real(QStringLiteral("waveformMin"), shared.waveformMin);
    shared.waveformMax = real(QStringLiteral("waveformMax"), shared.waveformMax);
    shared.psdMin = real(QStringLiteral("psdMin"), shared.psdMin);
    shared.psdMax = real(QStringLiteral("psdMax"), shared.psdMax);
    if (!std::isfinite(shared.waveformMin) || !std::isfinite(shared.waveformMax) || shared.waveformMax - shared.waveformMin < 2.0) { shared.waveformMin = -32768; shared.waveformMax = 32768; }
    if (!std::isfinite(shared.psdMin) || !std::isfinite(shared.psdMax) || shared.psdMax - shared.psdMin < 2.0) { shared.psdMin = -100; shared.psdMax = 0; }
    shared.waveformMin = std::clamp(shared.waveformMin, -65536.0, 65534.0);
    shared.waveformMax = std::clamp(shared.waveformMax, shared.waveformMin + 2.0, 65536.0);
    shared.psdMin = std::clamp(shared.psdMin, -180.0, 48.0);
    shared.psdMax = std::clamp(shared.psdMax, shared.psdMin + 2.0, 50.0);
    shared.auxiliaryMin = shared.auxiliaryMode == AuxiliaryMode::Waveform ? shared.waveformMin : shared.psdMin;
    shared.auxiliaryMax = shared.auxiliaryMode == AuxiliaryMode::Waveform ? shared.waveformMax : shared.psdMax;
    for (auto& file : session_.project().files) {
        auto& display = file.display;
        display.mainMode = shared.mainMode;
        display.auxiliaryMode = shared.auxiliaryMode;
        display.waveformMode = shared.waveformMode;
        display.palette = shared.palette;
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
        display.psdFromSelection = sharedPsdFromSelectionPreference_ && findMark(file, file.activeMarkId);
        file.view = clampRange(file.view, file.metadata, display.stftSize, display.psdSize);
    }
}

void MainWindow::propagateGlobalRightSidebarSettings() {
    auto* active = session_.activeFile();
    if (!active) return;
    active->display.psdFromSelection = sharedPsdFromSelectionPreference_ && findMark(*active, active->activeMarkId);
    const auto shared = active->display;
    for (auto& file : session_.project().files) {
        if (file.metadata.id == active->metadata.id) continue;
        auto& display = file.display;
        display.mainMode = shared.mainMode;
        display.auxiliaryMode = shared.auxiliaryMode;
        display.waveformMode = shared.waveformMode;
        display.palette = shared.palette;
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
        display.psdFromSelection = sharedPsdFromSelectionPreference_ && findMark(file, file.activeMarkId);
        file.view = clampRange(file.view, file.metadata, display.stftSize, display.psdSize);
    }
}

void MainWindow::scheduleRightSidebarSettingsSave() {
    if (rightSidebarSaveTimer_ && qApp->property("uiStatePersistenceEnabled").toBool())
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
            {QStringLiteral("effectiveBandwidthHz"), file.metadata.effectiveBandwidthHz}
        };
        settings.setValue(key, values);
    }
    if (const auto* file = session_.activeFile()) {
        const auto& display = file->display;
        settings.setValue(QStringLiteral("globalSettings"), QVariantMap{
            {QStringLiteral("mainMode"), static_cast<int>(display.mainMode)},
            {QStringLiteral("auxiliaryMode"), static_cast<int>(display.auxiliaryMode)},
            {QStringLiteral("waveformMode"), static_cast<int>(display.waveformMode)},
            {QStringLiteral("palette"), static_cast<int>(display.palette)},
            {QStringLiteral("stftSize"), display.stftSize},
            {QStringLiteral("psdSize"), display.psdSize},
            {QStringLiteral("dynamicRangeDb"), display.dynamicRangeDb},
            {QStringLiteral("referenceLevelDb"), display.referenceLevelDb},
            {QStringLiteral("absoluteFrequency"), display.absoluteFrequency},
            {QStringLiteral("grid"), display.grid},
            {QStringLiteral("colorScale"), display.colorScale},
            {QStringLiteral("psdFromSelection"), sharedPsdFromSelectionPreference_},
            {QStringLiteral("waveformMin"), display.waveformMin},
            {QStringLiteral("waveformMax"), display.waveformMax},
            {QStringLiteral("waveformAutoFit"), display.waveformAutoFit},
            {QStringLiteral("psdMin"), display.psdMin},
            {QStringLiteral("psdMax"), display.psdMax}
        });
    }
    settings.endGroup();
    settings.sync();
}

void MainWindow::updateRecentProjectsMenu() {
    if (!recentProjectsMenu_) return;
    recentProjectsMenu_->clear();
    const auto paths = QSettings().value(QStringLiteral("recentProjects")).toStringList();
    for (const auto& path : paths) {
        const QFileInfo info(path);
        const QString displayName = info.fileName().compare("project.json", Qt::CaseInsensitive) == 0
            ? QFileInfo(info.absolutePath()).fileName() : info.completeBaseName();
        auto* action = recentProjectsMenu_->addAction(displayName.isEmpty() ? path : displayName);
        action->setObjectName(QStringLiteral("recentProjectAction"));
        action->setToolTip(path); action->setStatusTip(path); action->setEnabled(QFileInfo::exists(path));
        connect(action, &QAction::triggered, this, [this, path] { openProject(path); });
    }
    if (paths.isEmpty()) {
        auto* empty = recentProjectsMenu_->addAction("暂无历史工程"); empty->setEnabled(false);
    } else {
        recentProjectsMenu_->addSeparator();
        auto* clear = recentProjectsMenu_->addAction("清除历史工程");
        connect(clear, &QAction::triggered, this, [this] {
            QSettings().remove(QStringLiteral("recentProjects")); updateRecentProjectsMenu();
        });
    }
}

void MainWindow::rememberProject(const QString& path) {
    if (!qApp->property("uiStatePersistenceEnabled").toBool()) return;
    const auto absolutePath = QFileInfo(path).absoluteFilePath();
    auto paths = QSettings().value(QStringLiteral("recentProjects")).toStringList();
    paths.removeAll(absolutePath); paths.prepend(absolutePath);
    while (paths.size() > 10) paths.removeLast();
    QSettings().setValue(QStringLiteral("recentProjects"), paths);
    updateRecentProjectsMenu();
}

void MainWindow::updateTree() {
    if (tree_->topLevelItemCount() == 1 && treeStructure_ == treeStructure()) { syncTreeState(); return; }
    if (treeRebuildPending_) return; treeRebuildPending_ = true;
    QTimer::singleShot(0, this, [this] { treeRebuildPending_ = false; rebuildTree(); });
}
void MainWindow::syncTreeState() {
    QSignalBlocker block(tree_); auto* root = tree_->topLevelItem(0); root->setText(0, "▾ ◧ " + q(session_.project().name));
    for (std::size_t index = 0; index < session_.project().files.size(); ++index) {
        const auto& file = session_.project().files[index]; auto* item = root->child(static_cast<int>(index)); item->setText(0, "◈ " + q(file.metadata.name));
        if (file.metadata.id != session_.project().activeFileId) continue;
        auto* marks = childOfKind(item, "marks"); auto* channels = childOfKind(item, "channels");
        marks->setText(0, QString("信号区域 (%1) · 已选 %2").arg(file.marks.size()).arg(file.selectedMarkIds.size()));
        if (auto* controls = tree_->itemWidget(marks->child(0), 0)) { controls->findChild<QPushButton*>("treeSelectAll")->setEnabled(!file.marks.empty()); controls->findChild<QPushButton*>("treeDeleteMarks")->setEnabled(!file.selectedMarkIds.empty()); }
        for (std::size_t i = 0; i < file.marks.size(); ++i) {
            const auto& mark = file.marks[i]; auto* node = marks->child(static_cast<int>(i) + 1); node->setText(0, "▱ " + q(mark.name));
            const bool selected = std::find(file.selectedMarkIds.begin(), file.selectedMarkIds.end(), mark.id) != file.selectedMarkIds.end(); if (node->isSelected() != selected) node->setSelected(selected);
        }
        channels->setText(0, QString("窄带通道 (%1)").arg(file.channels.size()));
        for (std::size_t i = 0; i < file.channels.size(); ++i) { const auto& channel = file.channels[i]; channels->child(static_cast<int>(i))->setText(0, "◇ " + q(channel.name) + " · " + coordinate(channel.centerFrequencyHz, channel.bandwidthHz, false)); }
    }
}

void MainWindow::refresh() {
    if (refreshing_) return; refreshing_ = true;
    const auto* file = session_.activeFile(); updateTree(); projectLabel_->setText(q(session_.project().name));
    const bool narrowOpen = session_.project().narrowbandWorkspaceOpen && session_.activeChannel() != nullptr;
    workspaceStack_->setCurrentIndex(narrowOpen ? 1 : 0);
    workspaceModeButton_->setChecked(!narrowOpen);
    if (narrowband_ && narrowOpen) narrowband_->refreshFromSession();
    const bool projectReady = !projectPath_.isEmpty() || !session_.project().files.empty();
    for (auto* control : std::array<QWidget*, 15>{mainMode_, auxMode_, waveformMode_, palette_, propertyPalette_, dynamic_, reference_, freqMode_, grid_, colorScale_, psd_, stft_, psdScope_, extract_, effectiveBandwidth_}) control->setEnabled(file != nullptr);
    waveformMode_->setEnabled(file && !file->metadata.demo && file->display.auxiliaryMode == AuxiliaryMode::Waveform);
    for (auto* button : auxiliaryButtons_) button->setEnabled(file != nullptr); for (auto* button : mainButtons_) button->setEnabled(file != nullptr);
    for (const auto& id : {"navMaximize", "auxMaximize", "specMaximize"}) findChild<QPushButton*>(id)->setEnabled(file != nullptr);
    addSignalAction_->setEnabled(projectReady); saveAction_->setEnabled(!projectPath_.isEmpty() || !session_.project().files.empty());
    removeAction_->setEnabled(file != nullptr); deleteAction_->setEnabled(file && !file->selectedMarkIds.empty()); backAction_->setEnabled(session_.canBack()); forwardAction_->setEnabled(session_.canForward());
    empty_->setVisible(!file); if (!file) empty_->raise();
    if (auto* title = findChild<QLabel*>("emptyWorkflowTitle")) title->setText(projectReady ? "工程已就绪 · 等待添加信号" : "开始宽带数据分析");
    if (auto* help = findChild<QLabel*>("emptyWorkflowHelp")) help->setText(projectReady ? "当前工程：" + q(session_.project().name) + "。添加信号后即可进行宽带数据操作。" : "第一步：新建工程（自动创建工程文件夹）或打开已有工程。");
    if (auto* actions = findChild<QWidget*>("emptyProjectActions")) actions->setVisible(!projectReady);
    if (auto* addButton = findChild<QPushButton*>("emptyAddSignal")) addButton->setVisible(projectReady);
    if (auto* addButton = findChild<QPushButton*>("projectAddSignal")) { addButton->setVisible(projectReady); addButton->setEnabled(projectReady); }
    if (auto* saveButton = findChild<QPushButton*>("projectSave")) saveButton->setEnabled(saveAction_->isEnabled());
    if (file) {
        const auto& display = file->display; mainMode_->setCurrentIndex(static_cast<int>(display.mainMode)); auxMode_->setCurrentIndex(static_cast<int>(display.auxiliaryMode)); palette_->setCurrentIndex(static_cast<int>(display.palette)); propertyPalette_->setCurrentIndex(static_cast<int>(display.palette));
        dataSourceStatus_->setText(file->metadata.demo ? "演示数据 · 未运行 DSP" : "实际 IQ · FFT 已启用");
        dataSourceStatus_->setToolTip(file->metadata.demo ? "当前文件使用原型演示数据" : "int16 IQ 数据来自磁盘文件；波形、PSD、时频图均使用真实采样");
        for (const auto& item : {std::pair<QComboBox*, double>{dynamic_, display.dynamicRangeDb}, {reference_, display.referenceLevelDb}}) {
            int index = item.first->findData(item.second);
            if (index < 0) { item.first->addItem(number(item.second) + (item.first == dynamic_ ? " dB" : " dBFS"), item.second); index = item.first->count() - 1; }
            item.first->setCurrentIndex(index);
        }
        waveformMode_->setCurrentIndex(static_cast<int>(display.waveformMode));
        effectiveBandwidth_->setRange(file->metadata.sampleRateHz / 65536.0 / 1e6, file->metadata.sampleRateHz / 1e6);
        effectiveBandwidth_->setValue(file->metadata.effectiveBandwidthHz / 1e6);
        freqMode_->setCurrentIndex(display.absoluteFrequency ? 0 : 1); grid_->setChecked(display.grid); colorScale_->setChecked(display.colorScale); psdScope_->setCurrentIndex(display.psdFromSelection ? 1 : 0);
        psd_->setCurrentText(QString::number(display.psdSize)); stft_->setCurrentText(QString::number(display.stftSize));
        for (int i = 0; i < 2; ++i) { auxiliaryButtons_[i]->setChecked(i == auxMode_->currentIndex()); mainButtons_[i]->setChecked(i == mainMode_->currentIndex()); }
        const double duration = static_cast<double>(file->metadata.sampleCount) / file->metadata.sampleRateHz;
        scope_->setText(QString("%1 · Fₛ %2 MS/s · fc %3 MHz").arg(q(file->metadata.name), number(file->metadata.sampleRateHz / 1e6), number(file->metadata.centerFrequencyHz / 1e6)));
        navStatus_->setText("0–" + number(duration) + " s"); auxStatus_->setText(auxiliary_->statusText()); rangeTag_->setText("动态 " + number(display.dynamicRangeDb) + " dB");
        const bool waterfall = display.mainMode == MainMode::Waterfall;
        specMode_->setText("正在框选信号 · 右键或 Esc 退出"); specMode_->setVisible(main_->isCreating() && width() > 990);
        specAxis_->setText(waterfall ? "X 频率 · Y 时间↓" : "X 时间 · Y 频率");
        specAxis_->setToolTip(waterfall ? "X 轴：频率；Y 轴：时间；颜色：功率" : "X 轴：时间；Y 轴：频率；颜色：功率");
        fileValues_[0]->setText(q(file->metadata.name)); fileValues_[1]->setText(number(file->metadata.sampleRateHz / 1e6) + " MS/s"); fileValues_[2]->setText(number(file->metadata.centerFrequencyHz / 1e6) + " MHz"); fileValues_[3]->setText("0–" + number(duration) + " s");
        viewValues_[0]->setText(timeRange(file->view.time, file->metadata.sampleRateHz)); viewValues_[1]->setText(frequencyRange(file->view.frequency));
        viewData_->setText("| 视图 ΔT " + coordinate(static_cast<double>(file->view.time.end - file->view.time.begin) / file->metadata.sampleRateHz, 0, true) + " · ΔF " + coordinate(file->view.frequency.upperHz - file->view.frequency.lowerHz, 0, false));
        const auto* mark = findMark(*file, file->activeMarkId);
        extract_->setEnabled(!file->marks.empty());
        if (mark) {
            const double span = static_cast<double>(mark->range.time.end - mark->range.time.begin) / file->metadata.sampleRateHz;
            markValues_[0]->setText(coordinate(static_cast<double>(mark->range.time.begin) / file->metadata.sampleRateHz, span, true)); markValues_[1]->setText(coordinate(static_cast<double>(mark->range.time.end) / file->metadata.sampleRateHz, span, true)); markValues_[2]->setText(frequencyRange(mark->range.frequency));
            selectionData_->setText("| 标记 " + q(mark->name) + " · " + coordinate((mark->range.frequency.lowerHz + mark->range.frequency.upperHz) / 2, 0, false));
        } else { for (auto* value : markValues_) value->setText("—"); selectionData_->setText("| 无信号标记"); }
        selectedCount_->setText(QString("已选中 %1 个标记。拖动选中框移动，边线和顶点调整；Esc 回滚当前编辑。").arg(file->selectedMarkIds.size()));
        rename_->setEnabled(mark != nullptr); locate_->setEnabled(mark != nullptr); delete_->setEnabled(!file->selectedMarkIds.empty());
        if (cursorFileId_ != q(file->metadata.id)) { cursorFileId_ = q(file->metadata.id); cursorSample_ = file->view.time.begin + (file->view.time.end - file->view.time.begin) / 2; cursorFrequency_ = file->metadata.centerFrequencyHz; }
        updateCursor(cursorSample_, cursorFrequency_);
    } else {
        scope_->setText("未添加 IQ 文件"); navStatus_->setText("—"); auxStatus_->setText("—");
        for (auto* value : fileValues_) value->setText("—"); for (auto* value : viewValues_) value->setText("—"); for (auto* value : markValues_) value->setText("—");
        viewData_->clear(); cursorData_->clear(); selectionData_->setText("无信号标记"); statusTime_->clear(); statusFrequency_->clear(); statusFile_->clear(); cursorFileId_.clear();
    }
    updatePropertyContext(); updateBottom(); for (auto* plot : {navigation_, auxiliary_, main_}) plot->syncState(); refreshing_ = false;
}

void MainWindow::activateTreeChannel(const QString& channelId) {
    cancelInteractions(false);
    if (!session_.activateChannel(channelId.toStdString())) return;
    session_.project().narrowbandWorkspaceOpen = true;
    workspaceStack_->setCurrentIndex(1); refresh();
    log("已打开窄带通道 · 真实 DDC 按可见时段按需计算");
}

void MainWindow::locateActiveChannelSource() {
    const auto* channel = session_.activeChannel(); if (!channel) return;
    const auto channelId = channel->id;
    const auto* file = session_.fileForChannel(channelId); if (!file) return;
    const auto fileId = file->metadata.id; const auto markId = channel->sourceMarkId;
    cancelInteractions(false);
    if (!session_.activateFile(fileId) || !session_.focusMark(markId)) return;
    session_.project().narrowbandWorkspaceOpen = false; workspaceStack_->setCurrentIndex(0);
    refresh(); log("已返回来源文件并定位源标记；再次打开通道可恢复窄带视图");
}

void MainWindow::showChannelDialog(const QString& channelId) {
    cancelInteractions(false);
    auto* file = session_.activeFile();
    const bool editing = !channelId.isEmpty();
    Channel original;
    if (editing) {
        file = session_.fileForChannel(channelId.toStdString());
        if (!file) return;
        const auto found = std::find_if(file->channels.begin(), file->channels.end(), [&](const Channel& value) { return value.id == channelId.toStdString(); });
        if (found == file->channels.end()) return;
        original = *found;
    }
    if (!file || file->marks.empty()) {
        QMessageBox::information(this, "创建窄带通道", "请先为 IQ 源添加至少一个信号标记。"); return;
    }

    auto* dialog = new QDialog(this); dialog->setObjectName("channelConfigDialog");
    dialog->setWindowTitle(editing ? "修改窄带通道" : "从源标记创建窄带通道");
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setModal(true); dialog->setMinimumWidth(560);
    auto* outer = new QVBoxLayout(dialog); outer->setContentsMargins(20, 17, 20, 16); outer->setSpacing(10);
    auto* intro = label(editing ? "修改将作为一个配置事务提交；取消不会改变现有通道。" :
        "选择一个来源标记并设置真实 DDC 参数。预览和校验通过后才创建通道。", {}, "help");
    intro->setWordWrap(true); outer->addWidget(intro);
    auto* form = new QFormLayout; form->setSpacing(9); form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* name = new QLineEdit(editing ? q(original.name) : QStringLiteral("窄带通道 %1").arg(file->channels.size() + 1)); name->setObjectName("channelName");
    auto* sourceMark = new QComboBox; sourceMark->setObjectName("channelSourceMark");
    for (const auto& mark : file->marks) {
        sourceMark->addItem(q(mark.name) + " · " + timeRange(mark.range.time, file->metadata.sampleRateHz) +
            " · " + frequencyRange(mark.range.frequency), q(mark.id));
    }
    if (editing) sourceMark->setCurrentIndex(std::max(0, sourceMark->findData(q(original.sourceMarkId))));
    else if (!file->activeMarkId.empty()) sourceMark->setCurrentIndex(std::max(0, sourceMark->findData(q(file->activeMarkId))));
    const auto sampled = fullRange(file->metadata).frequency;
    auto* center = new QDoubleSpinBox; center->setObjectName("channelCenterMHz"); center->setRange(sampled.lowerHz / 1e6, sampled.upperHz / 1e6); center->setDecimals(6); center->setSingleStep(.1); center->setSuffix(" MHz");
    auto* bandwidth = new QDoubleSpinBox; bandwidth->setObjectName("channelBandwidthMHz"); bandwidth->setRange(.000001, file->metadata.sampleRateHz / 1e6); bandwidth->setDecimals(6); bandwidth->setSingleStep(.1); bandwidth->setSuffix(" MHz");
    auto* outputRate = new QDoubleSpinBox; outputRate->setObjectName("channelOutputRateMSps"); outputRate->setRange(.000001, std::max(.000002, file->metadata.sampleRateHz / 1e6)); outputRate->setDecimals(6); outputRate->setSingleStep(1); outputRate->setSuffix(" MS/s");
    auto* timeMode = new QComboBox; timeMode->setObjectName("channelTimeScope"); timeMode->addItems({"所选标记时段", "完整源文件"});
    auto* filter = new QComboBox; filter->setObjectName("channelFilter"); filter->addItems({"快速预览 · 40 dB", "标准 · 60 dB", "高抑制 · 80 dB"});
    auto* preserveTime = new QCheckBox("保持源文件绝对时间显示"); preserveTime->setObjectName("channelPreserveSourceTime"); preserveTime->setChecked(!editing || original.preserveSourceTime);
    auto* autoOpen = new QCheckBox("确认后打开窄带工作区"); autoOpen->setObjectName("channelAutoOpen"); autoOpen->setChecked(true);
    if (editing) { center->setValue(original.centerFrequencyHz / 1e6); bandwidth->setValue(original.bandwidthHz / 1e6); outputRate->setValue(original.outputSampleRateHz / 1e6); timeMode->setCurrentIndex(original.wholeSource ? 1 : 0); filter->setCurrentIndex(static_cast<int>(original.filter)); }
    else if (const auto* mark = findMark(*file, file->marks[static_cast<std::size_t>(sourceMark->currentIndex())].id)) {
        center->setValue((mark->range.frequency.lowerHz + mark->range.frequency.upperHz) / 2e6);
        bandwidth->setValue(std::max(.001, (mark->range.frequency.upperHz - mark->range.frequency.lowerHz) / 1e6));
        outputRate->setValue(std::max(4.0, bandwidth->value() * 1.3));
    }
    form->addRow("通道名称", name); form->addRow("来源标记（单选）", sourceMark);
    form->addRow("射频中心", center); form->addRow("有效带宽", bandwidth); form->addRow("输出采样率", outputRate);
    form->addRow("提取时段", timeMode); form->addRow("FIR 滤波", filter); form->addRow("时间坐标", preserveTime);
    outer->addLayout(form);
    auto* preview = label({}, "channelDspPlanPreview", "help"); preview->setWordWrap(true); preview->setMinimumHeight(58); preview->setStyleSheet("background:#0c1a2a;border:1px solid #29445e;padding:8px;color:#95c9dc;"); outer->addWidget(preview);
    auto* validation = label({}, "channelValidation"); validation->setWordWrap(true); validation->setStyleSheet("color:#ff9b8d;"); outer->addWidget(validation);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(editing ? "应用通道参数" : "创建通道"); buttons->button(QDialogButtonBox::Ok)->setObjectName("confirmChannelConfig");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消"); outer->addWidget(buttons);
    auto* confirm = buttons->button(QDialogButtonBox::Ok);
    auto validate = [this, file, editing, original, name, sourceMark, center, bandwidth, outputRate, timeMode, filter, preview, validation, confirm] {
        QString reason;
        if (name->text().trimmed().isEmpty() || name->text().trimmed().size() > 80) reason = "通道名称长度必须为 1–80 个字符。";
        const auto markId = sourceMark->currentData().toString().toStdString();
        const auto* mark = findMark(*file, markId);
        if (!mark && reason.isEmpty()) reason = "来源标记已失效，请重新选择。";
        const double fc = center->value() * 1e6, bw = bandwidth->value() * 1e6, fs = outputRate->value() * 1e6;
        Channel candidate = editing ? original : Channel{};
        candidate.centerFrequencyHz = fc; candidate.bandwidthHz = bw; candidate.outputSampleRateHz = fs;
        candidate.filter = static_cast<ChannelFilter>(filter->currentIndex());
        if (mark) {
            candidate.sourceMarkId = mark->id;
            candidate.sourceTime = timeMode->currentIndex() == 1 ? TimeRange{0, file->metadata.sampleCount} : mark->range.time;
        }
        ChannelDspPlan plan;
        const bool planned = reason.isEmpty() && makeChannelDspPlan(file->metadata, candidate, plan, reason);
        if (reason.isEmpty() && !planned) reason = "DSP 参数校验未通过。";
        if (planned) {
            std::size_t taps = 0; for (const auto& stage : plan.stageCoefficients) taps += stage.size();
            preview->setText(QString("DspPlan：过渡带 ±%1 kHz · 截止 %2 kHz · FIR %3 阶 / %4 taps · 有理重采样 L/M=%5/%6 · %7 dB 目标\n采样率约束：Fs′ ≥ BW + 2Δf = %8 MS/s；窗口按需提取，不预扫描全时段。")
                .arg(plan.transitionHz / 1e3, 0, 'f', 3).arg(plan.cutoffHz / 1e3, 0, 'f', 3)
                .arg(taps ? taps - 1 : 0).arg(taps).arg(plan.interpolation).arg(plan.decimation)
                .arg(plan.stopbandAttenuationDb, 0, 'f', 0).arg((bw + 2 * plan.transitionHz) / 1e6, 0, 'f', 3));
        } else {
            preview->setText(QString("默认优先 4 MS/s；FIR、通带/过渡带和 L/M 约束均按实际 DspPlan 校验。"));
            if (reason.isEmpty()) reason = QStringLiteral("当前设置不满足窄带处理要求。");
        }
        validation->setText(reason); confirm->setEnabled(reason.isEmpty() && planned);
    };
    connect(name, &QLineEdit::textChanged, dialog, [validate] { validate(); });
    connect(sourceMark, &QComboBox::currentIndexChanged, dialog, [this, editing, original, file, sourceMark, center, bandwidth, validate] {
        if (!editing && sourceMark->currentIndex() >= 0) if (const auto* mark = findMark(*file, sourceMark->currentData().toString().toStdString())) {
            center->setValue((mark->range.frequency.lowerHz + mark->range.frequency.upperHz) / 2e6);
            bandwidth->setValue(std::max(.001, (mark->range.frequency.upperHz - mark->range.frequency.lowerHz) / 1e6));
        }
        validate();
    });
    for (auto* control : {static_cast<QWidget*>(center), static_cast<QWidget*>(bandwidth), static_cast<QWidget*>(outputRate), static_cast<QWidget*>(timeMode), static_cast<QWidget*>(filter)}) {
        if (auto* spin = qobject_cast<QDoubleSpinBox*>(control)) connect(spin, &QDoubleSpinBox::valueChanged, dialog, [validate] { validate(); });
        else if (auto* combo = qobject_cast<QComboBox*>(control)) connect(combo, &QComboBox::currentIndexChanged, dialog, [validate] { validate(); });
    }
    validate();
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, editing, channelId, original, file, name, sourceMark, center, bandwidth, outputRate, timeMode, filter, preserveTime, autoOpen] {
        const auto* mark = findMark(*file, sourceMark->currentData().toString().toStdString()); if (!mark) return;
        const auto range = timeMode->currentIndex() == 1 ? TimeRange{0, file->metadata.sampleCount} : mark->range.time;
        bool ok = false;
        if (editing) {
            auto updated = original; updated.name = name->text().trimmed().toStdString(); updated.sourceMarkId = mark->id;
            updated.centerFrequencyHz = center->value() * 1e6; updated.bandwidthHz = bandwidth->value() * 1e6;
            updated.outputSampleRateHz = outputRate->value() * 1e6; updated.filter = static_cast<ChannelFilter>(filter->currentIndex());
            updated.wholeSource = timeMode->currentIndex() == 1; updated.preserveSourceTime = preserveTime->isChecked();
            updated.sourceTime = range;
            if (updated.visibleSourceTime.begin < range.begin || updated.visibleSourceTime.end > range.end) {
                const auto preview = static_cast<SampleIndex>(std::max(1.0, std::min(4'000'000'000.0, file->metadata.sampleRateHz * .25)));
                updated.visibleSourceTime = {range.begin, range.begin + std::min(range.end - range.begin, preview)};
            }
            updated.visibleBasebandFrequency = {-updated.outputSampleRateHz / 2, updated.outputSampleRateHz / 2};
            ok = session_.updateChannel(channelId.toStdString(), updated);
        } else {
            const auto id = session_.createChannel(name->text().trimmed().toStdString(), mark->id, center->value() * 1e6,
                bandwidth->value() * 1e6, outputRate->value() * 1e6, range, static_cast<ChannelFilter>(filter->currentIndex()),
                timeMode->currentIndex() == 1, preserveTime->isChecked());
            ok = !id.empty();
        }
        if (!ok) { QMessageBox::warning(dialog, "通道参数未应用", "配置未能提交；当前通道仍保持原状态。"); return; }
        session_.project().narrowbandWorkspaceOpen = autoOpen->isChecked();
        workspaceStack_->setCurrentIndex(autoOpen->isChecked() ? 1 : 0);
        refresh(); dialog->accept(); log(editing ? "窄带通道参数已更新，旧计算缓存将按配置版本失效" : "窄带通道已创建；真实 DDC 将按可见时段按需计算");
    });
    dialog->open();
}

void MainWindow::updatePropertyContext() {
    const auto* file = session_.activeFile(); const auto* channel = session_.activeChannel();
    const bool narrow = session_.project().narrowbandWorkspaceOpen && channel && file;
    const bool marked = !narrow && file && findMark(*file, file->activeMarkId);
    const bool psd = !narrow && file && file->display.auxiliaryMode == AuxiliaryMode::Psd;
    channelSection_->setVisible(narrow);
    fileSection_->setVisible(!narrow);
    viewSection_->setVisible(!narrow);
    psdSection_->setVisible(!narrow);
    markSection_->setVisible(marked);
    if (narrow) {
        channelValues_[0]->setText(q(file->metadata.name));
        const auto* sourceMark = findMark(*file, channel->sourceMarkId);
        channelValues_[1]->setText(sourceMark ? q(sourceMark->name) : "来源标记丢失");
        channelValues_[2]->setText(number(channel->centerFrequencyHz / 1e6) + " MHz");
        channelValues_[3]->setText(number(channel->bandwidthHz / 1e6) + " MHz");
        channelValues_[4]->setText(number(channel->outputSampleRateHz / 1e6) + " MS/s");
        channelValues_[5]->setText(timeRange(channel->sourceTime, file->metadata.sampleRateHz));
    }
    const auto context = (file ? q(file->metadata.id) : QString{}) + ":" + QString::number(psd) + ":" +
        QString::number(marked) + ":" + QString::number(narrow) + ":" +
        (channel ? q(channel->id) + ":" + QString::number(channel->configVersion) : QString{});
    if (context == propertyContext_) return; propertyContext_ = context;
    auto* first = narrow ? channelSection_ : marked ? markSection_ : psd ? psdSection_ : fileSection_;
    propertyLayout_->removeWidget(first); propertyLayout_->insertWidget(0, first);
    propertyLayout_->activate();
}
void MainWindow::updateBottom() {
    const bool open = resultToggle_->isChecked(); bottom_->setFixedHeight(open ? 150 : 29); bottomContent_->setVisible(open); bottomContent_->setCurrentIndex(bottomTab_); resultToggle_->setText(open ? "⌄" : "⌃");
    for (int i = 0; i < 3; ++i) bottomButtons_[i]->setChecked(i == bottomTab_);
    const auto* file = session_.activeFile();
    resultSummary_->setText(file ? QString("%1    标记 %2 个，通道 %3 个。").arg(q(file->metadata.name)).arg(file->marks.size()).arg(file->channels.size()) : "空工程 · 请添加 IQ 文件");
    if (session_.project().narrowbandWorkspaceOpen && session_.activeChannel())
        taskSummary_->setText("窄带通道 · " + (narrowband_ ? narrowband_->dataStatusText() : QStringLiteral("等待后台处理")));
    else taskSummary_->setText(!file ? "空工程 · 无后台任务。" : file->metadata.demo ?
        "演示图谱 · 未运行真实 DSP。" : "真实 IQ · 图谱抽取与 PSD/STFT 计算在后台线程执行。");
}
void MainWindow::updateCursor(SampleIndex sample, double frequencyHz) {
    const auto* file = session_.activeFile(); if (!file) return; cursorSample_ = sample; cursorFrequency_ = frequencyHz;
    const auto t = coordinate(static_cast<double>(sample) / file->metadata.sampleRateHz, static_cast<double>(file->view.time.end - file->view.time.begin) / file->metadata.sampleRateHz, true);
    const auto f = coordinate(frequencyHz, file->view.frequency.upperHz - file->view.frequency.lowerHz, false);
    cursorData_->setText("游标 T " + t + " · F " + f); statusTime_->setText("T " + t); statusFrequency_->setText("F " + f); statusFile_->setText(q(file->metadata.name)); main_->setCursorCoordinates(sample, frequencyHz);
}

void MainWindow::enforceLayout(bool reset) {
    if (!graphs_ || layoutBusy_) return; layoutBusy_ = true;
    const int w = width(); const bool hideResources = resourcesCollapsed_ || w <= 990, hideProperties = propertiesCollapsed_ || w <= 1150;
    resources_->setFixedWidth(hideResources ? 38 : w <= 1490 ? (w <= 1150 ? 194 : 218) : 270);
    properties_->setFixedWidth(hideProperties ? 38 : w <= 1490 ? 248 : 294);
    resourceTitle_->setVisible(!hideResources); resourceTools_->setVisible(!hideResources); resourceBody_->setVisible(!hideResources);
    propertiesTitle_->setVisible(!hideProperties); propScroll_->setVisible(!hideProperties); propRail_->setVisible(hideProperties); findChild<QPushButton*>("propClose")->setVisible(!hideProperties);
    scope_->setVisible(w > 1330); projectLabel_->setVisible(w > 990);
    specMode_->setVisible(main_ && main_->isCreating() && w > 990); specAxis_->setVisible(w > 1330); rangeTag_->setVisible(w > 990); cursorData_->setVisible(w > 990); viewData_->setVisible(w > 1150);
    findChild<QLabel*>("navHint")->setVisible(w > 1150);
    if (maximizedPanel_ < 0) {
        for (int i = 1; i < graphs_->count(); ++i) {
            graphs_->handle(i)->setObjectName(i == 1 ? "navSplit" : "auxSplit"); graphs_->handle(i)->installEventFilter(this);
        }
        const int available = std::max(0, graphs_->height() - 14);
        const int budget = std::min(366, std::max(80, available - 62 - 116)); const int others = std::max(0, available - budget);
        const auto sizes = graphs_->sizes(); int nav = reset || sizes.size() < 3 ? 82 : sizes[0]; int aux = reset || sizes.size() < 3 ? (height() < 850 ? 176 : 205) : sizes[1];
        const int navMinimum = std::min(62, others), navMaximum = std::min(128, std::max(0, others - 116)); nav = std::clamp(nav, std::min(navMinimum, navMaximum), navMaximum);
        const int auxMinimum = std::min(116, std::max(0, others - nav)), auxMaximum = std::max(0, others - nav); aux = std::clamp(aux, auxMinimum, auxMaximum);
        panels_[0]->setMinimumHeight(navMinimum); panels_[0]->setMaximumHeight(std::max(navMinimum, navMaximum)); panels_[1]->setMinimumHeight(auxMinimum); panels_[1]->setMaximumHeight(std::max(auxMinimum, auxMaximum)); panels_[2]->setMinimumHeight(80);
        graphs_->setSizes({nav, aux, std::max(80, graphs_->height() - nav - aux - 20)});
    } else { maxVeil_->setGeometry(rect()); maxHost_->setGeometry(12, 86, std::max(1, width() - 24), std::max(1, height() - 120)); maxVeil_->raise(); maxHost_->raise(); }
    layoutBusy_ = false;
}
void MainWindow::toggleMaximized(int index) {
    const bool creating = main_->isCreating(); cancelInteractions(false); const int previous = maximizedPanel_;
    if (previous >= 0) {
        maxHost_->layout()->removeWidget(panels_[previous]); panels_[previous]->setParent(graphs_); graphs_->insertWidget(previous, panels_[previous]); panels_[previous]->show(); maximizedPanel_ = -1;
        maxHost_->hide(); maxVeil_->hide(); graphs_->setSizes(panelSizes_); enforceLayout();
    }
    if (previous == index) { if (creating && !main_->isCreating()) main_->setCreating(true); return; }
    panelSizes_ = graphs_->sizes(); maximizedPanel_ = index; panels_[index]->setMaximumHeight(QWIDGETSIZE_MAX); panels_[index]->setMinimumHeight(0);
    panels_[index]->setParent(maxHost_); maxHost_->layout()->addWidget(panels_[index]); panels_[index]->show(); maxVeil_->show(); maxHost_->show(); enforceLayout();
    if (creating && !main_->isCreating()) main_->setCreating(true);
}
void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event); if (!graphs_) return; cancelInteractions(false); enforceLayout(); QTimer::singleShot(0, this, [this] { enforceLayout(); });
}
void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { cancelInteractions(); if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_); event->accept(); return; }
    QMainWindow::keyPressEvent(event);
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (destroying_) return QMainWindow::eventFilter(watched, event);
    if ((watched == tree_ || watched == tree_->viewport()) && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress))
        treeSelectionModifiers_ = event->type() == QEvent::MouseButtonPress ? static_cast<QMouseEvent*>(event)->modifiers() : static_cast<QKeyEvent*>(event)->modifiers();
    if (watched == tree_->viewport() && event->type() == QEvent::Leave) main_->setHoveredMark({});
    if (watched->objectName() == "graphArea" && event->type() == QEvent::Resize && empty_) empty_->setGeometry(static_cast<QWidget*>(watched)->rect().adjusted(5, 5, -5, -5));
    if (auto* widget = qobject_cast<QWidget*>(watched); widget && (widget == this || isAncestorOf(widget))) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && !QApplication::activeModalWidget()) {
            const bool active = (main_ && (main_->isCreating() || main_->hasPendingInteraction())) || (auxiliary_ && auxiliary_->hasPendingInteraction()) || (navigation_ && navigation_->hasPendingInteraction()) || static_cast<PrototypeSplitter*>(graphs_)->isDragging(); cancelInteractions();
            if (!active && maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_); event->accept(); return true;
        }
        if (event->type() == QEvent::ContextMenu && widget->property("chartIndex").isValid()) {
            const auto* context = static_cast<QContextMenuEvent*>(event); const int index = widget->property("chartIndex").toInt();
            auto* plot = std::array<PlotWidget*, 3>{navigation_, auxiliary_, main_}[index]; auto* menu = plot->createContextMenu(plot->mapFromGlobal(context->globalPos())); menu->exec(context->globalPos()); delete menu; return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::selectMark(const QString& id, Qt::KeyboardModifiers modifiers) {
    auto* file = session_.activeFile(); if (!file) return; const auto key = id.toStdString(); auto ids = file->selectedMarkIds;
    if (modifiers & Qt::ShiftModifier) {
        auto first = std::find_if(file->marks.begin(), file->marks.end(), [this](const Mark& mark) { return mark.id == selectionAnchor_; }); auto last = std::find_if(file->marks.begin(), file->marks.end(), [key](const Mark& mark) { return mark.id == key; });
        if (first == file->marks.end()) first = last;
        if (first != file->marks.end() && last != file->marks.end()) { if (!(modifiers & Qt::ControlModifier)) ids.clear(); if (first > last) std::swap(first, last); for (auto it = first; it <= last; ++it) ids.push_back(it->id); }
    } else if (modifiers & Qt::ControlModifier) { const auto it = std::find(ids.begin(), ids.end(), key); if (it == ids.end()) ids.push_back(key); else ids.erase(it); selectionAnchor_ = key; }
    else { ids = {key}; selectionAnchor_ = key; }
    session_.selectMarks(std::move(ids), key);
    if (auto* active = session_.activeFile()) active->display.psdFromSelection = sharedPsdFromSelectionPreference_ && findMark(*active, active->activeMarkId);
    propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave();
}
void MainWindow::selectAllMarks() {
    cancelInteractions(false); if (const auto* file = session_.activeFile()) { std::vector<std::string> ids; for (const auto& mark : file->marks) ids.push_back(mark.id); session_.selectMarks(std::move(ids)); if (auto* active = session_.activeFile()) active->display.psdFromSelection = sharedPsdFromSelectionPreference_ && findMark(*active, active->activeMarkId); propagateGlobalRightSidebarSettings(); refresh(); scheduleRightSidebarSettingsSave(); }
}
void MainWindow::locateMark() { cancelInteractions(false); if (const auto* file = session_.activeFile()) session_.focusMark(file->activeMarkId); refresh(); }
void MainWindow::cancelInteractions(bool exitCreating) {
    if (main_) main_->cancelGesture(exitCreating); if (auxiliary_) auxiliary_->cancelGesture(); if (navigation_) navigation_->cancelGesture();
    if (graphs_) static_cast<PrototypeSplitter*>(graphs_)->cancel();
}
void MainWindow::deleteMarks() {
    cancelInteractions(false); const auto* file = session_.activeFile(); if (!file || file->selectedMarkIds.empty()) return;
    std::unordered_set<std::string> selected(file->selectedMarkIds.begin(), file->selectedMarkIds.end());
    QStringList affected;
    for (const auto& channel : file->channels) if (selected.contains(channel.sourceMarkId)) affected << q(channel.name);
    if (!affected.empty()) {
        const auto prompt = QStringLiteral("删除所选 %1 个源标记会同时删除引用这些标记的 %2 个窄带通道：\n\n%3\n\n继续吗？")
            .arg(file->selectedMarkIds.size()).arg(affected.size()).arg(affected.join("\n"));
        if (QMessageBox::question(this, "删除标记及关联通道", prompt) != QMessageBox::Yes) return;
    }
    const auto count = session_.deleteSelectedMarks(); refresh();
    if (count.marks) log(QString("已从当前文件删除 %1 个标记，并移除引用这些标记的 %2 个演示通道").arg(count.marks).arg(count.channels));
}
void MainWindow::renameMark() {
    cancelInteractions(false); auto* file = session_.activeFile(); if (!file) return; auto* mark = findMark(*file, file->activeMarkId); if (!mark) return;
    const auto fileId = file->metadata.id, markId = mark->id; bool ok = false;
    const auto name = QInputDialog::getText(this, "重命名标记", "标记名称（最多 80 个字符）", QLineEdit::Normal, q(mark->name), &ok).trimmed().left(80);
    file = session_.activeFile(); if (!ok || name.isEmpty() || !file || file->metadata.id != fileId) return;
    mark = findMark(*file, markId); if (mark) { mark->name = name.toStdString(); refresh(); log("已重命名信号标记"); }
}
void MainWindow::showNewProjectDialog() {
    if ((!projectPath_.isEmpty() || !session_.project().files.empty()) &&
        QMessageBox::question(this, "新建工程", "新建工程会切换当前工程，继续吗？") != QMessageBox::Yes) return;

    auto* dialog = new QDialog(this); dialog->setObjectName("newProjectDialog"); dialog->setWindowTitle("新建 Signal Studio 工程");
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setModal(true); dialog->setMinimumWidth(520);
    auto* layout = new QVBoxLayout(dialog); layout->setContentsMargins(20, 18, 20, 18); layout->setSpacing(12);
    auto* intro = label("新工程将创建独立文件夹，并在其中保存 project.json。", {}, "help"); intro->setWordWrap(true); layout->addWidget(intro);
    auto* form = new QFormLayout; form->setSpacing(10);
    auto* name = new QLineEdit("Signal Studio Project"); name->setObjectName("newProjectName");
    auto* location = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)); location->setObjectName("newProjectLocation");
    auto* locationRow = new QWidget(dialog); auto* rowLayout = new QHBoxLayout(locationRow); rowLayout->setContentsMargins(0, 0, 0, 0); rowLayout->setSpacing(6);
    rowLayout->addWidget(location, 1); auto* browse = push("浏览…", "browseProjectLocation", rowLayout); browse->setProperty("uiRole", "outline");
    form->addRow("工程名称", name); form->addRow("保存位置", locationRow); layout->addLayout(form);
    auto* target = label({}, "newProjectTarget", "help"); target->setWordWrap(true); layout->addWidget(target);
    auto updateTarget = [name, location, target] { target->setText("将创建：" + QDir(location->text()).filePath(name->text().trimmed())); };
    connect(name, &QLineEdit::textChanged, dialog, [updateTarget] { updateTarget(); });
    connect(location, &QLineEdit::textChanged, dialog, [updateTarget] { updateTarget(); }); updateTarget();
    connect(browse, &QPushButton::clicked, dialog, [dialog, location] {
        const auto path = QFileDialog::getExistingDirectory(dialog, "选择工程保存位置", location->text());
        if (!path.isEmpty()) location->setText(path);
    });
    auto* error = label({}, "newProjectError"); error->setStyleSheet("color:#ff9b8d;"); error->setWordWrap(true); layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("创建工程文件夹"); buttons->button(QDialogButtonBox::Ok)->setObjectName("createProjectButton");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消"); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, name, location, error] {
        QString message;
        if (!createProject(location->text(), name->text(), &message)) { error->setText(message); return; }
        dialog->accept();
    });
    dialog->open();
}

bool MainWindow::createProject(const QString& parentDirectory, const QString& projectName, QString* error) {
    auto setError = [error](const QString& message) { if (error) *error = message; };
    const QString name = projectName.trimmed();
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains('\\')) {
        setError("工程名称不能为空，也不能包含路径分隔符。"); return false;
    }
    QDir parent(parentDirectory);
    if (!parent.exists()) { setError("工程保存位置不存在，请重新选择。"); return false; }
    if (!parent.mkdir(name)) { setError("无法创建工程文件夹；请检查名称、权限，或选择一个尚不存在的文件夹。"); return false; }
    const QString folder = parent.absoluteFilePath(name);
    const QString projectFile = QDir(folder).filePath("project.json");
    Project candidate; candidate.name = name.toUtf8().toStdString();
    QString message;
    if (!ProjectStore::save(projectFile, candidate, message)) {
        QFile::remove(projectFile); QDir().rmdir(folder);
        setError("工程文件初始化失败：" + message); return false;
    }
    cancelInteractions(); if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
    session_.replaceProject(std::move(candidate)); projectPath_ = QFileInfo(projectFile).absoluteFilePath();
    selectionAnchor_.clear(); rememberProject(projectPath_); refresh(); log("已创建工程文件夹：" + folder);
    if (error) error->clear();
    return true;
}

void MainWindow::openDemoProject() {
    if ((!projectPath_.isEmpty() || !session_.project().files.empty()) &&
        QMessageBox::question(this, "打开演示工程", "打开演示工程会切换当前工程，继续吗？") != QMessageBox::Yes) return;
    cancelInteractions(); if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
    session_.newProject(); session_.project().name = "演示工程";
    const auto first = session_.addDemoFile(); session_.addDemoFile(); session_.addDemoFile(); session_.activateFile(first);
    projectPath_.clear(); selectionAnchor_.clear(); refresh(); log("已打开演示工程");
}

void MainWindow::openNarrowbandDemoProject() {
    if ((!projectPath_.isEmpty() || !session_.project().files.empty()) &&
        QMessageBox::question(this, "打开窄带演示工程", "打开窄带演示工程会切换当前工程，继续吗？") != QMessageBox::Yes) return;
    const QString fixturePath = QStringLiteral(":/signalstudio/demo/narrowband_demo.iq");
    QFile resource(fixturePath);
    if (!resource.open(QIODevice::ReadOnly) || resource.size() != 1'048'576) {
        QMessageBox::critical(this, "窄带演示工程不可用", "内置确定性 IQ 资源缺失或长度错误。"); return;
    }
    Int16IqFile fixture; QString error;
    if (!fixture.open(fixturePath, error) || fixture.sampleCount() != 262'144) {
        QMessageBox::critical(this, "窄带演示工程不可用", "内置 IQ 样例校验失败：" + error); return;
    }

    cancelInteractions(); if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_);
    session_.newProject(); session_.project().name = "窄带通道演示工程 · NB-A1.1";
    FileMetadata metadata;
    metadata.name = "IQ0 · 阿拉善信号样例"; metadata.path = fixturePath.toStdString();
    metadata.sampleRateHz = 102.4e6; metadata.centerFrequencyHz = 2500e6; metadata.sampleCount = fixture.sampleCount();
    metadata.demo = false; metadata.demoSeed = 1; metadata.declaredBandwidthHz = 102.4e6; metadata.effectiveBandwidthHz = 102.4e6;
    const auto fileId = session_.addDemoFile(std::move(metadata));
    auto* source = session_.activeFile();
    if (fileId.empty() || !source) { QMessageBox::critical(this, "窄带演示工程不可用", "无法创建样例工程文件状态。"); return; }
    const auto time1 = TimeRange{0, 196'608}, time2 = TimeRange{65'536, 262'144};
    source->marks.push_back({"demo-mark-signal-01", "Signal-01", {time1, {2510.6e6, 2511.8e6}}});
    source->marks.push_back({"demo-mark-signal-02", "Signal-02", {time2, {2493.0e6, 2494.2e6}}});
    session_.selectMarks({"demo-mark-signal-01"}, "demo-mark-signal-01");
    const auto channelId = session_.createChannel("Signal-01", "demo-mark-signal-01", 2511.2e6, 1.2e6,
        4e6, time1, ChannelFilter::Standard, false, true);
    if (channelId.empty()) { QMessageBox::critical(this, "窄带演示工程不可用", "样例通道参数未能通过校验。"); return; }
    session_.project().narrowbandWorkspaceOpen = true;
    projectPath_.clear(); selectionAnchor_.clear(); workspaceStack_->setCurrentIndex(1); refresh();
    log("已载入内置确定性 IQ 样例；此演示工程会运行真实 DDC、FIR、重采样、PSD 与 STFT，识别/解调仍为合成示例");
}

void MainWindow::showAddFileDialog() {
    cancelInteractions(false); auto* dialog = new QDialog(this); dialog->setObjectName("addFileDialog"); dialog->setWindowTitle("向工程添加 int16 IQ 文件"); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setModal(true); dialog->setFixedWidth(470);
    auto* layout = new QVBoxLayout(dialog); layout->setContentsMargins(18, 18, 18, 18); layout->setSpacing(12);
    auto* heading = label("向工程添加 RAW IQ 文件"); heading->setStyleSheet("font-size:16px;font-weight:600;"); layout->addWidget(heading);
    auto* note = label("格式：小端 int16，I/Q 交替；FS、FC、BW 从文件名读取。只映射文件，不将整份数据载入内存。", {}, "help"); note->setWordWrap(true); layout->addWidget(note);
    auto* form = new QFormLayout; form->setSpacing(12); auto* fileField = new QWidget; auto* fileRow = new QHBoxLayout(fileField); fileRow->setContentsMargins(0, 0, 0, 0); fileRow->setSpacing(6);
    auto* choose = push("选择文件…", "chooseIqFile", fileRow); choose->setProperty("uiRole", "outline"); auto* fileName = label("未选择文件", "fileInput"); fileName->setMinimumWidth(0); fileRow->addWidget(fileName, 1); row(form, "选择文件", fileField);
    auto* fs = new QDoubleSpinBox; fs->setObjectName("loadFs"); fs->setRange(1, 1e12); fs->setDecimals(0); fs->setValue(40000000); fs->setSingleStep(1000000);
    auto* fc = new QDoubleSpinBox; fc->setObjectName("loadFc"); fc->setRange(0, 1e15); fc->setDecimals(0); fc->setValue(100000000); fc->setSingleStep(1000000);
    auto* duration = new QDoubleSpinBox; duration->setObjectName("loadDuration"); duration->setRange(.101, 1e9); duration->setDecimals(3); duration->setValue(180);
    for (auto* field : {fs, fc, duration}) field->setReadOnly(true);
    row(form, "采样率 (Hz)", fs); row(form, "中心频率 (Hz)", fc); row(form, "文件时长", duration); layout->addLayout(form);
    auto* progress = new QProgressBar; progress->setObjectName("loadProgress"); progress->setTextVisible(false); progress->setRange(0, 100); progress->setValue(0); progress->setFixedHeight(8); layout->addWidget(progress);
    auto* progressText = label("选择文件后读取文件名参数并检查长度", "progressText", "help"); layout->addWidget(progressText);
    auto* actions = new QHBoxLayout; actions->addStretch(); auto* cancel = push("取消", "cancelOpen", actions); cancel->setProperty("uiRole", "outline"); auto* add = push("添加 IQ 数据", "simulateOpen", actions); add->setProperty("uiRole", "primary"); add->setEnabled(false); layout->addLayout(actions);
    connect(choose, &QPushButton::clicked, dialog, [dialog, fileName, fs, fc, duration, progress, progressText, add] {
        const auto path = QFileDialog::getOpenFileName(dialog, "选择交替 int16 IQ 文件", {}, "IQ 文件 (*.iq *.dat *.raw *.bin);;所有文件 (*)");
        if (path.isEmpty()) return;
        QString error; const auto info = describeInt16IqFile(path, error);
        fileName->setText(QFileInfo(path).fileName()); fileName->setToolTip(path); fileName->setProperty("path", path);
        if (!info) { progress->setValue(0); progressText->setText(error); add->setEnabled(false); return; }
        fs->setValue(info->metadata.sampleRateHz); fc->setValue(info->metadata.centerFrequencyHz);
        duration->setValue(static_cast<double>(info->metadata.sampleCount) / info->metadata.sampleRateHz);
        progress->setValue(100);
        const QString bw = info->declaredBandwidthHz > 0 ? QString(" · BW %1 MHz").arg(number(info->declaredBandwidthHz / 1e6)) : QString{};
        progressText->setText(QString("已检查 %1 字节 · %2 个复采样点%3").arg(info->byteSize).arg(info->metadata.sampleCount).arg(bw));
        add->setEnabled(true);
    });
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(add, &QPushButton::clicked, dialog, [this, dialog, fileName, add, progressText] {
        const auto path = fileName->property("path").toString(); QString error;
        add->setEnabled(false);
        if (!addIqFile(path, &error)) { progressText->setText(error); add->setEnabled(true); return; }
        dialog->accept();
    });
    dialog->open();
}

bool MainWindow::addIqFile(const QString& path, QString* error) {
    QString message;
    const auto descriptor = describeInt16IqFile(path, message);
    if (!descriptor) { if (error) *error = message; return false; }
    auto metadata = descriptor->metadata;
    metadata.declaredBandwidthHz = descriptor->declaredBandwidthHz;
    metadata.effectiveBandwidthHz = descriptor->declaredBandwidthHz > 0 ? descriptor->declaredBandwidthHz : metadata.sampleRateHz;
    const auto* activeBeforeImport = session_.activeFile();
    const bool hasActiveBeforeImport = activeBeforeImport != nullptr;
    const DisplaySettings priorSettings = activeBeforeImport ? activeBeforeImport->display : DisplaySettings{};
    const auto name = QString::fromUtf8(metadata.name.data(), static_cast<qsizetype>(metadata.name.size()));
    const auto id = session_.addDemoFile(std::move(metadata));
    if (id.empty()) { if (error) *error = QStringLiteral("文件元数据无效，未能加入工程"); return false; }
    auto* imported = session_.activeFile();
    if (imported && imported->metadata.id == id) restoreRightSidebarSettings(*imported);
    applyGlobalRightSidebarSettings(hasActiveBeforeImport ? priorSettings :
        (imported && imported->metadata.id == id ? imported->display : DisplaySettings{}));
    cancelInteractions(false); selectionAnchor_.clear(); refresh(); log("已加入真实 int16 IQ 文件：" + name);
    scheduleRightSidebarSettingsSave();
    if (error) error->clear();
    return true;
}

bool MainWindow::openProject(const QString& path) {
    cancelInteractions(); Project candidate; QString error;
    const QFileInfo requested(path);
    const QString projectFile = requested.isDir() ? QDir(path).filePath("project.json") : path;
    if (!ProjectStore::load(projectFile, candidate, error)) { QMessageBox::critical(this, "打开失败", error); return false; }
    const auto* current = session_.activeFile();
    const DisplaySettings fallbackSettings = current ? current->display :
        (candidate.files.empty() ? DisplaySettings{} : candidate.files.front().display);
    if (maximizedPanel_ >= 0) toggleMaximized(maximizedPanel_); session_.replaceProject(std::move(candidate));
    applyGlobalRightSidebarSettings(fallbackSettings);
    projectPath_ = QFileInfo(projectFile).absoluteFilePath(); rememberProject(projectPath_); selectionAnchor_.clear(); refresh(); scheduleRightSidebarSettingsSave(); log("已打开工程：" + projectPath_); return true;
}
bool MainWindow::saveProject(const QString& path) {
    const QFileInfo requested(path);
    const QString projectFile = requested.isDir() ? QDir(path).filePath("project.json") : path;
    cancelInteractions(false); QString error; if (!ProjectStore::save(projectFile, session_.project(), error)) { QMessageBox::critical(this, "保存失败", error); return false; }
    projectPath_ = QFileInfo(projectFile).absoluteFilePath(); rememberProject(projectPath_); refresh(); log("工程已保存：" + projectPath_); return true;
}
void MainWindow::log(const QString& message) {
    if (!results_) return; results_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  信息  " + message);
    statusBar()->showMessage(message, 2500);
}
} // namespace signalstudio
