#include "signal_studio/workbench/workbench.hpp"

#include "ui_SignalDiagnosticsPanel.h"
#include "ui_SignalInspectorPanel.h"
#include "ui_SignalResultCenterPanel.h"
#include "ui_SignalSettingsPanel.h"
#include "ui_SignalTaskCenterPanel.h"
#include "ui_SignalWorkbenchMainWindow.h"

#include <QtCore/QByteArray>
#include <QtCore/QEvent>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <memory>
#include <utility>

namespace signal::workbench {
namespace {

[[nodiscard]] Qt::DockWidgetArea to_qt_area(PanelArea area) {
  switch (area) {
  case PanelArea::left:
    return Qt::LeftDockWidgetArea;
  case PanelArea::right:
    return Qt::RightDockWidgetArea;
  case PanelArea::bottom:
    return Qt::BottomDockWidgetArea;
  case PanelArea::center:
    return Qt::RightDockWidgetArea;
  }
  return Qt::LeftDockWidgetArea;
}

[[nodiscard]] PanelArea from_qt_area(Qt::DockWidgetArea area) {
  if (area == Qt::RightDockWidgetArea) {
    return PanelArea::right;
  }
  if (area == Qt::BottomDockWidgetArea || area == Qt::TopDockWidgetArea) {
    return PanelArea::bottom;
  }
  return PanelArea::left;
}

void ensure_default_panels(PanelRegistry& registry) {
  const std::vector<PanelDescriptor> defaults{
      {"navigator", "项目与数据源", PanelArea::left, false, false, "显示项目、数据源、Selection 与全局页面入口"},
      {"inspector", "属性检查器", PanelArea::right, true, true, "显示当前对象、视口和参数的单位与范围"},
      {"task-center", "任务中心", PanelArea::bottom, true, true, "显示任务状态、进度、实际后端和控制命令"},
      {"result-center", "结果中心", PanelArea::bottom, true, true, "按数据源版本显示可定位的结果"},
      {"settings", "设置", PanelArea::right, true, true, "显示主题、DPI 和工作台参数"},
      {"diagnostics", "诊断", PanelArea::right, true, true, "显示工具链、后端、缓存和恢复动作"},
  };
  for (const auto& panel : defaults) {
    if (!registry.find(panel.id)) {
      static_cast<void>(registry.add(panel));
    }
  }
}

class DpiAwareMainWindow final : public QMainWindow {
protected:
  bool event(QEvent* event) override {
    const auto type = event->type();
    const auto result = QMainWindow::event(event);
    if (type == QEvent::DevicePixelRatioChange || type == QEvent::ScreenChangeInternal) {
      QTimer::singleShot(0, this, [this] {
        for (auto* widget : findChildren<QWidget*>()) {
          widget->updateGeometry();
          widget->update();
        }
        updateGeometry();
        update();
      });
    }
    return result;
  }
};

class QtWorkbenchWindow final : public IWorkbenchWindow {
public:
  QtWorkbenchWindow(WorkbenchConfiguration configuration,
                    std::unique_ptr<visualization::IAnalysisWorkspace> center_workspace,
                    std::shared_ptr<CommandRegistry> commands, std::shared_ptr<PanelRegistry> panels,
                    std::shared_ptr<IDiagnosticsProvider> diagnostics)
      : window_(std::make_unique<DpiAwareMainWindow>()), workspace_(std::move(center_workspace)),
        commands_(std::move(commands)), panels_(std::move(panels)), diagnostics_(std::move(diagnostics)),
        configuration_(std::move(configuration)) {
    main_ui_.setupUi(window_.get());
    ensure_default_panels(*panels_);
    ensureDefaultCommands();
    configureWindow();
    buildCommands();
    buildPanels();
    normalizeInteractiveTargets();
  }

  ~QtWorkbenchWindow() noexcept override {
    // Visualization 工作区对其根 QWidget 保持唯一所有权；QStackedWidget 只负责暂时承载。
    // 析构前显式解除 QObject 父子关系，避免 Qt 与 C++ 所有权在关闭路径上重复销毁同一对象。
    if (workspace_ != nullptr) {
      auto* widget = static_cast<QWidget*>(workspace_->native_handle());
      if (widget != nullptr) {
        if (central_stack_ != nullptr) {
          central_stack_->removeWidget(widget);
        }
        widget->setParent(nullptr);
      }
      workspace_.reset();
    }
    window_.reset();
  }

  [[nodiscard]] void* native_handle() noexcept override {
    return window_.get();
  }

  void show() override {
    window_->show();
    normalizeInteractiveTargets();
    QTimer::singleShot(0, window_.get(), [this] { normalizeInteractiveTargets(); });
  }

  [[nodiscard]] core::Status install_page(WorkbenchPage page) override {
    if (page.id.empty() || page.title.empty() || page.native_widget == nullptr ||
        page.id.find_first_of("\t\r\n") != std::string::npos ||
        page.title.find_first_of("\t\r\n") != std::string::npos) {
      return core::Status::failure({core::ErrorDomain::workbench, core::ErrorReason::invalid_argument},
                                   "应用页面需要有效 ID、标题和原生 QWidget");
    }
    if (page.id == "p02") {
      return core::Status::failure({core::ErrorDomain::workbench, core::ErrorReason::unavailable},
                                   "P02 由工作台持有的 Visualization 工作区管理，不能被替换");
    }
    auto* widget = static_cast<QWidget*>(page.native_widget);
    const auto existing = page_indices_.find(page.id);
    int index{};
    if (existing != page_indices_.end()) {
      index = existing->second;
      auto* previous = central_stack_->widget(index);
      central_stack_->removeWidget(previous);
      central_stack_->insertWidget(index, widget);
      page_tabs_->setTabText(index, QString::fromStdString(page.title));
      if (previous != widget) {
        previous->deleteLater();
      }
    } else {
      index = central_stack_->addWidget(widget);
      page_tabs_->addTab(QString::fromStdString(page.title));
      page_indices_.emplace(page.id, index);
      if (view_menu_ != nullptr) {
        addPageAction(page.id, page.title);
      }
    }
    page_tabs_->setTabVisible(index, page.visible_in_tabs);
    normalizeInteractiveTargets();
    return core::Status::success();
  }

  [[nodiscard]] core::Status show_page(std::string_view page_id) override {
    const auto page = page_indices_.find(page_id);
    if (page == page_indices_.end()) {
      return core::Status::failure({core::ErrorDomain::workbench, core::ErrorReason::invalid_argument},
                                   "工作台页面不存在", std::string{page_id});
    }
    showPage(page->second);
    return core::Status::success();
  }

  [[nodiscard]] core::Status update_content(WorkbenchContent content) override {
    configuration_.content = std::move(content);
    if (status_label_ != nullptr) {
      status_label_->setText(QString::fromStdString(configuration_.content.status_text));
    }
    if (resource_label_ != nullptr) {
      resource_label_->setText(QString::fromStdString(configuration_.content.resource_text));
    }
    if (source_label_ != nullptr) {
      source_label_->setText(QString::fromStdString(configuration_.content.source_summary));
    }
    for (const auto* id : {"navigator", "inspector", "task-center", "result-center"}) {
      const auto dock = docks_.find(id);
      if (dock != docks_.end()) {
        dock->second->setWidget(createPanelContent(id));
      }
    }
    populateTaskTable();
    normalizeInteractiveTargets();
    return core::Status::success();
  }

  [[nodiscard]] core::Status restore_layout(const WorkbenchLayout& layout) override {
    for (const auto& [id, area] : layout.areas) {
      const auto dock = docks_.find(id);
      if (dock != docks_.end()) {
        window_->addDockWidget(to_qt_area(area), dock->second);
      }
    }
    for (const auto& [id, dock] : docks_) {
      dock->setVisible(std::ranges::find(layout.visible_panels, id) != layout.visible_panels.end());
    }
    if (!layout.native_state.empty()) {
      const QByteArray state(reinterpret_cast<const char*>(layout.native_state.data()),
                             static_cast<qsizetype>(layout.native_state.size()));
      if (!window_->restoreState(state, 1)) {
        return core::Status::failure({core::ErrorDomain::workbench, core::ErrorReason::invalid_argument},
                                     "工作台原生布局状态不兼容");
      }
    }
    return core::Status::success();
  }

  [[nodiscard]] WorkbenchLayout save_layout() const override {
    WorkbenchLayout layout;
    for (const auto& [id, dock] : docks_) {
      layout.areas.emplace(id, from_qt_area(window_->dockWidgetArea(dock)));
      if (dock->isVisible()) {
        layout.visible_panels.push_back(id);
      }
    }
    const auto state = window_->saveState(1);
    layout.native_state.assign(reinterpret_cast<const std::uint8_t*>(state.constData()),
                               reinterpret_cast<const std::uint8_t*>(state.constData()) + state.size());
    return layout;
  }

  [[nodiscard]] std::vector<std::string> visible_panels() const override {
    std::vector<std::string> result;
    for (const auto& [id, dock] : docks_) {
      if (dock->isVisible()) {
        result.push_back(id);
      }
    }
    return result;
  }

  [[nodiscard]] std::string accessibility_summary() const override {
    return configuration_.application_name + " " + configuration_.window_title + "；中心：" +
           workspace_->accessibility_summary() + "；可见面板 " + std::to_string(visible_panels().size()) + " 个";
  }

private:
  void ensureDefaultCommands() {
    const auto add_if_missing = [this](Command command) {
      const auto existing = commands_->can_execute(command.id);
      if (!existing) {
        static_cast<void>(commands_->add(std::move(command)));
      }
    };
    add_if_missing({"view.fit-all", "适合全部", "显式恢复当前图表的有效全范围", "F", "{}",
                    [this] { return workspace_->viewport().effective_frequency_range.bandwidth_hz() > 0; },
                    [this] { return workspace_->fit_frequency_to_data(); }});
    add_if_missing({"view.locate-selection", "定位到选区", "将时间视窗显式定位到当前选区", "Ctrl+L", "{}",
                    [this] { return !workspace_->selections().empty(); },
                    [this] {
                      const auto selections = workspace_->selections();
                      return selections.empty()
                                 ? core::Status::failure({core::ErrorDomain::workbench, core::ErrorReason::unavailable},
                                                         "当前没有可定位 Selection")
                                 : workspace_->locate_selection(selections.front().id);
                    }});
    add_if_missing(
        {"view.screenshot", "图谱截图", "选择轴、图例、色标、游标和参数摘要", "Ctrl+Shift+S", "{}", [] { return true; },
         [this] {
           const auto path = QFileDialog::getSaveFileName(window_.get(), "保存图谱截图", {}, "PNG 图像 (*.png)");
           return path.isEmpty() ? core::Status::failure({core::ErrorDomain::workbench, core::ErrorReason::cancelled},
                                                         "图谱截图已取消")
                                 : workspace_->save_screenshot(std::filesystem::path(path.toStdWString()),
                                                               visualization::ScreenshotOptions{});
         }});
    add_if_missing({"workbench.save-layout", "保存布局", "保存 Dock、顺序和高度", "Ctrl+Alt+S", "{}",
                    [] { return true; },
                    [this] {
                      saved_layout_ = save_layout();
                      return core::Status::success();
                    }});
  }

  void normalizeInteractiveTargets() {
    for (auto* button : window_->findChildren<QAbstractButton*>()) {
      button->setMinimumSize(28, 28);
      if (button->focusPolicy() == Qt::NoFocus) {
        button->setFocusPolicy(Qt::StrongFocus);
      }
    }
  }

  void configureWindow() {
    window_->setObjectName("SignalWorkbenchMainWindow");
    window_->setWindowTitle(
        QString::fromStdString(configuration_.application_name + " · " + configuration_.window_title));
    window_->setAccessibleName("信号处理公共工作台");
    window_->setAccessibleDescription("包含中心可视化、属性检查器、任务中心、结果中心、设置和诊断 Dock");
    window_->resize(static_cast<int>(configuration_.minimum_width), static_cast<int>(configuration_.minimum_height));
    window_->setMinimumSize(960, 640);
    window_->setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    window_->setStyleSheet(QString(R"(
      QMainWindow, QWidget { background:%1; color:%2; font-family:"%3"; font-size:12px; }
      QMenuBar {
        background:%1; color:%2; border-bottom:1px solid #203A55;
        min-height:29px; max-height:29px;
      }
      QMenuBar::item { background:transparent; color:%2; padding:4px 10px; }
      QMenuBar::item#SignalStudioBrand { color:#06111F; background:%5; font-weight:700; }
      QWidget#ApplicationMenuStrip {
        background:%1; border-bottom:1px solid #203A55;
        min-height:29px; max-height:29px;
      }
      QLabel#SignalStudioBrand {
        color:#06111F; background:%5; font-weight:700; padding:0 8px;
      }
      QLabel#ApplicationContext { color:#8FA8C2; padding:0 8px; }
      QToolButton#ApplicationMenuButton {
        background:transparent; color:%2; border:0; min-height:28px;
        padding:0 9px;
      }
      QToolButton#ApplicationMenuButton:hover,
      QToolButton#ApplicationMenuButton:pressed { background:#16334E; }
      QMenu, QToolBar, QStatusBar { background:%4; color:%2; }
      QMenuBar::item:selected, QMenu::item:selected { background:#16334E; }
      QToolBar { border-bottom:1px solid #203A55; spacing:2px; padding:2px 6px; }
      QToolButton, QPushButton {
        background:#10243A; color:%2; border:1px solid #2B4867; border-radius:2px;
        min-width:28px; min-height:28px; padding:0 7px;
      }
      QToolBar QToolButton { border-color:transparent; padding:0 5px; }
      QToolBar QToolButton:hover { background:#16334E; border-color:#2B4867; }
      QToolButton:focus, QPushButton:focus, QTreeWidget:focus, QTableWidget:focus {
        border:2px solid %5;
      }
      QDockWidget { color:%2; }
      QDockWidget::title {
        background:#13263D; border-bottom:1px solid #2B4867; padding:6px;
        text-align:left;
      }
      QWidget#DockTitleBar, QWidget#DockTitleBar QLabel {
        background:#13263D;
      }
      QWidget#DockTitleBar QToolButton {
        background:#13263D; border:1px solid #2B4867; border-radius:2px;
        min-width:28px; min-height:28px; padding:0;
      }
      QTabBar#WorkspaceTabs { background:%1; border-bottom:1px solid #203A55; }
      QTabBar#WorkspaceTabs::tab {
        background:%1; color:#8FA8C2; min-width:64px; min-height:27px;
        padding:0 10px; border-right:1px solid #203A55;
      }
      QTabBar#WorkspaceTabs::tab:selected {
        color:%2; background:#10243A; border-top:2px solid %5;
      }
      QTreeWidget, QTableWidget, QPlainTextEdit {
        background:#0A1727; alternate-background-color:#0E1B2D;
        border:1px solid #203A55; gridline-color:#203A55;
      }
      QTreeWidget::item:selected, QTableWidget::item:selected {
        background:#12324A; color:%2;
      }
      QHeaderView::section { background:#13263D; color:%2; border:0; padding:5px; }
      QLabel#SectionHeading { color:%5; font-weight:600; }
      QLabel#PageEyebrow { color:%5; font-family:"Cascadia Mono"; font-size:10px; font-weight:600; }
      QLabel#PageTitle { color:%2; font-size:20px; font-weight:700; }
      QFrame#PagePanel { background:#0A1727; border:1px solid #29435E; }
      QLineEdit { background:#0A1727; border:1px solid #2B4867; min-height:28px; padding:0 7px; }
      QProgressBar { background:#142840; border:0; min-height:5px; max-height:5px; text-align:right; }
      QProgressBar::chunk { background:#20BDEB; }
    )")
                               .arg(QString::fromStdString(configuration_.theme.canvas),
                                    QString::fromStdString(configuration_.theme.primary_text),
                                    QString::fromStdString(configuration_.theme.ui_font),
                                    QString::fromStdString(configuration_.theme.panel),
                                    QString::fromStdString(configuration_.theme.accent)));
    auto* central_host = new QWidget(window_.get());
    auto* shell_layout = new QHBoxLayout(central_host);
    shell_layout->setContentsMargins(0, 0, 0, 0);
    shell_layout->setSpacing(0);
    auto* page_column = new QWidget(central_host);
    auto* central_layout = new QVBoxLayout(page_column);
    central_layout->setContentsMargins(0, 0, 0, 0);
    central_layout->setSpacing(0);
    page_tabs_ = new QTabBar(central_host);
    page_tabs_->setObjectName("WorkspaceTabs");
    page_tabs_->setDocumentMode(true);
    page_tabs_->setExpanding(false);
    page_tabs_->setElideMode(Qt::ElideRight);
    central_layout->addWidget(page_tabs_);
    central_stack_ = new QStackedWidget(central_host);
    central_layout->addWidget(central_stack_, 1);

    auto* home = new QWidget(central_stack_);
    auto* home_layout = new QVBoxLayout(home);
    home_layout->setContentsMargins(20, 18, 20, 18);
    auto* home_eyebrow = new QLabel("P01 / PROJECT", home);
    home_eyebrow->setObjectName("PageEyebrow");
    auto* home_title = new QLabel("项目首页", home);
    home_title->setObjectName("PageTitle");
    auto* home_summary = new QLabel(QString("当前项目  %1\n当前数据源  %2")
                                        .arg(QString::fromStdString(configuration_.content.project_name.empty()
                                                                        ? configuration_.window_title
                                                                        : configuration_.content.project_name),
                                             QString::fromStdString(configuration_.content.source_summary)),
                                    home);
    home_summary->setStyleSheet("color:#8FA8C2; line-height:1.5;");
    home_layout->addWidget(home_eyebrow);
    home_layout->addWidget(home_title);
    home_layout->addWidget(home_summary);
    home_layout->addStretch();
    central_stack_->addWidget(home);
    page_tabs_->addTab("P01  首页");
    page_indices_.emplace("p01", 0);

    auto* central = static_cast<QWidget*>(workspace_->native_handle());
    central_stack_->addWidget(central);
    page_tabs_->addTab("P02  宽带");
    page_indices_.emplace("p02", 1);
    central_stack_->addWidget(createTaskPage(central_stack_));
    page_tabs_->addTab("P04  任务");
    page_indices_.emplace("p04", 2);
    page_tabs_->setTabVisible(2, false);
    central_stack_->addWidget(createSettingsPage(central_stack_));
    page_tabs_->addTab("P07  设置");
    page_indices_.emplace("p07", 3);
    page_tabs_->setTabVisible(3, false);
    page_tabs_->setCurrentIndex(1);
    central_stack_->setCurrentIndex(1);
    QObject::connect(page_tabs_, &QTabBar::currentChanged, central_stack_, &QStackedWidget::setCurrentIndex);

    auto* bottom_bar = new QFrame(page_column);
    bottom_bar->setObjectName("WorkspaceBottomBar");
    bottom_bar->setStyleSheet("QFrame#WorkspaceBottomBar{background:#0A1727;border-top:1px solid #203A55;}"
                              "QToolButton{border:0;background:#0A1727;color:#8FA8C2;}"
                              "QToolButton:hover{background:#12324A;color:#E5F1FF;}");
    auto* bottom_layout = new QHBoxLayout(bottom_bar);
    bottom_layout->setContentsMargins(4, 0, 4, 0);
    bottom_layout->setSpacing(2);
    const auto add_bottom_action = [this, bottom_bar, bottom_layout](const QString& text, int page) {
      auto* button = new QToolButton(bottom_bar);
      button->setText(text);
      QObject::connect(button, &QToolButton::clicked, window_.get(), [this, page] { showPage(page); });
      bottom_layout->addWidget(button);
    };
    add_bottom_action("任务  3", 2);
    add_bottom_action("结果  5", 1);
    add_bottom_action("日志  5", 1);
    add_bottom_action("标记  2", 1);
    bottom_layout->addStretch();
    auto* open_full_page = new QToolButton(bottom_bar);
    open_full_page->setText("打开完整页面");
    QObject::connect(open_full_page, &QToolButton::clicked, window_.get(), [this] { showPage(2); });
    bottom_layout->addWidget(open_full_page);
    central_layout->addWidget(bottom_bar);
    shell_layout->addWidget(page_column, 1);

    inspector_edge_ = new QToolButton(central_host);
    inspector_edge_->setObjectName("InspectorEdgeButton");
    inspector_edge_->setText("属\n性");
    inspector_edge_->setToolTip("显示属性检查器");
    inspector_edge_->setFixedWidth(27);
    inspector_edge_->setStyleSheet("QToolButton{border:0;border-left:1px solid #203A55;background:#08111F;"
                                   "color:#8FA8C2;padding:0;}"
                                   "QToolButton:hover{background:#12324A;color:#E5F1FF;}");
    QObject::connect(inspector_edge_, &QToolButton::clicked, window_.get(), [this] {
      const auto inspector = docks_.find("inspector");
      if (inspector != docks_.end()) {
        inspector->second->show();
        inspector->second->raise();
      }
    });
    shell_layout->addWidget(inspector_edge_);
    window_->setCentralWidget(central_host);
    window_->statusBar()->setSizeGripEnabled(true);
    status_label_ = new QLabel(QString::fromStdString(configuration_.content.status_text), window_.get());
    status_label_->setAccessibleName("工作台状态通知");
    window_->statusBar()->addWidget(status_label_, 1);
    resource_label_ = new QLabel(QString::fromStdString(configuration_.content.resource_text), window_.get());
    resource_label_->setAccessibleName("资源状态");
    window_->statusBar()->addPermanentWidget(resource_label_);
  }

  [[nodiscard]] QWidget* createTaskPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setObjectName("TaskCenterPage");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(10);
    auto* eyebrow = new QLabel("P04 / OPERATIONS", page);
    eyebrow->setObjectName("PageEyebrow");
    auto* title = new QLabel("任务中心", page);
    title->setObjectName("PageTitle");
    auto* description = new QLabel("交互请求优先；旧 ViewRequest 过期后不得提交。", page);
    description->setStyleSheet("color:#8FA8C2;");
    layout->addWidget(eyebrow);
    layout->addWidget(title);
    layout->addWidget(description);

    auto* filter_panel = new QFrame(page);
    filter_panel->setObjectName("PagePanel");
    auto* filters = new QHBoxLayout(filter_panel);
    filters->setContentsMargins(8, 5, 8, 5);
    filters->setSpacing(0);
    for (const auto* text : {"全部", "运行中", "已暂停", "等待中", "已取消", "失败", "已完成"}) {
      auto* button = new QToolButton(filter_panel);
      button->setText(text);
      button->setCheckable(true);
      button->setChecked(QString(text) == "全部");
      button->setAutoExclusive(true);
      filters->addWidget(button);
    }
    auto* search = new QLineEdit(filter_panel);
    search->setPlaceholderText("搜索任务 / RequestId");
    search->setMaximumWidth(240);
    filters->addSpacing(8);
    filters->addWidget(search);
    filters->addStretch();
    auto* priority = new QLabel("● 交互请求优先", filter_panel);
    priority->setStyleSheet("color:#20D3EE;");
    filters->addWidget(priority);
    layout->addWidget(filter_panel);

    task_page_table_ = new QTableWidget(page);
    task_page_table_->setObjectName("TaskCenterPageTable");
    task_page_table_->setColumnCount(7);
    task_page_table_->setHorizontalHeaderLabels({"任务", "对象", "优先级", "状态", "进度", "耗时", "操作"});
    task_page_table_->setShowGrid(false);
    task_page_table_->setAlternatingRowColors(true);
    task_page_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    task_page_table_->verticalHeader()->setVisible(false);
    populateTaskTable();
    task_page_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    task_page_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    layout->addWidget(task_page_table_, 1);

    auto* detail = new QFrame(page);
    detail->setObjectName("PagePanel");
    auto* detail_layout = new QVBoxLayout(detail);
    detail_layout->setContentsMargins(8, 7, 8, 7);
    auto* detail_title = new QLabel("任务详情 · 当前 ViewRequest", detail);
    detail_title->setStyleSheet("font-weight:600;");
    auto* detail_text =
        new QLabel("执行阶段  STFT 瓦片\n取消规则  新视窗请求到达时立即过期\n资源  CPU · 有界缓存", detail);
    detail_text->setStyleSheet("color:#8FA8C2;");
    detail_layout->addWidget(detail_title);
    detail_layout->addWidget(detail_text);
    layout->addWidget(detail);
    return page;
  }

  void populateTaskTable() {
    if (task_page_table_ == nullptr) {
      return;
    }
    task_page_table_->clearContents();
    task_page_table_->setRowCount(static_cast<int>(configuration_.content.tasks.size()));
    for (std::size_t index = 0; index < configuration_.content.tasks.size(); ++index) {
      const auto& entry = configuration_.content.tasks[index];
      const auto row = static_cast<int>(index);
      task_page_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(entry.task)));
      task_page_table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(entry.source)));
      task_page_table_->setItem(row, 2, new QTableWidgetItem(index < 2 ? "交互" : "分析"));
      task_page_table_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(entry.state_text)));
      auto* progress_cell = new QWidget(task_page_table_);
      auto* progress_layout = new QHBoxLayout(progress_cell);
      progress_layout->setContentsMargins(0, 0, 0, 0);
      progress_layout->setSpacing(6);
      auto* progress = new QProgressBar(progress_cell);
      auto progress_text = QString::fromStdString(entry.progress_text);
      progress_text.remove('%');
      bool progress_ok{};
      progress->setValue(progress_text.toInt(&progress_ok));
      if (!progress_ok) {
        progress->setValue(0);
      }
      progress->setTextVisible(false);
      auto* progress_label = new QLabel(QString::fromStdString(entry.progress_text), progress_cell);
      progress_label->setStyleSheet("color:#8FA8C2; font-family:'Cascadia Mono'; font-size:10px;");
      progress_layout->addWidget(progress, 1);
      progress_layout->addWidget(progress_label);
      task_page_table_->setCellWidget(row, 4, progress_cell);
      task_page_table_->setItem(row, 5, new QTableWidgetItem(index == 0 ? "0.18 s" : "—"));
      task_page_table_->setItem(row, 6, new QTableWidgetItem("暂停  取消  详情"));
      task_page_table_->setRowHeight(row, 52);
    }
  }

  [[nodiscard]] QWidget* createSettingsPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    page->setObjectName("SettingsDiagnosticsPage");
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(12, 8, 12, 12);
    root->setSpacing(8);
    auto* eyebrow = new QLabel("P07 / SYSTEM", page);
    eyebrow->setObjectName("PageEyebrow");
    auto* title = new QLabel("设置与诊断", page);
    title->setObjectName("PageTitle");
    auto* description = new QLabel("即时生效项与需要重启的设置明确区分。", page);
    description->setStyleSheet("color:#8FA8C2;");
    root->addWidget(eyebrow);
    root->addWidget(title);
    root->addWidget(description);

    auto* body = new QHBoxLayout;
    body->setSpacing(10);
    auto* section_list = new QTreeWidget(page);
    section_list->setObjectName("SettingsSectionList");
    section_list->setHeaderHidden(true);
    section_list->setMaximumWidth(180);
    for (const auto* section : {"性能与缓存", "渲染与图谱", "默认导入", "项目与自动保存", "日志与隐私", "版本与诊断"}) {
      section_list->addTopLevelItem(new QTreeWidgetItem({section}));
    }
    section_list->setCurrentItem(section_list->topLevelItem(0));
    body->addWidget(section_list);

    auto* form_panel = new QFrame(page);
    form_panel->setObjectName("PagePanel");
    form_panel->setMinimumWidth(580);
    form_panel->setMaximumWidth(760);
    form_panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* form = new QFormLayout(form_panel);
    form->setContentsMargins(12, 10, 12, 10);
    form->setSpacing(8);
    auto* heading = new QLabel("性能与缓存", form_panel);
    heading->setObjectName("SectionHeading");
    form->addRow(heading);
    auto* cache = new QProgressBar(form_panel);
    cache->setRange(0, 100);
    cache->setValue(25);
    cache->setFormat("25%");
    form->addRow("内存缓存上限", cache);
    const auto add_read_only_field = [form_panel, form](const char* label, const char* value) {
      auto* field = new QLineEdit(QString::fromUtf8(value), form_panel);
      field->setReadOnly(true);
      form->addRow(QString::fromUtf8(label), field);
    };
    add_read_only_field("CPU 工作线程", "4");
    add_read_only_field("I/O 并发", "2");
    add_read_only_field("缓存目录", "D:\\SignalStudioCache");
    add_read_only_field("缓存配额", "250 GB");
    body->addWidget(form_panel, 3);

    auto* environment = new QFrame(page);
    environment->setObjectName("PagePanel");
    environment->setMinimumWidth(250);
    environment->setMaximumWidth(280);
    auto* environment_layout = new QVBoxLayout(environment);
    environment_layout->setContentsMargins(10, 10, 10, 10);
    auto* environment_title = new QLabel("当前环境  运行时", environment);
    environment_title->setStyleSheet("font-weight:600;");
    auto* dpr_text = new QLabel(environment);
    dpr_text->setObjectName("DpiEnvironmentSummary");
    dpr_text->setText(QString("Qt  %1\n平台  %2\n逻辑 DPI  自动\n缩放  %3×\n后端  CPU / CUDA 可选")
                          .arg(qVersion(), QGuiApplication::platformName())
                          .arg(window_->devicePixelRatioF(), 0, 'f', 2));
    dpr_text->setStyleSheet("color:#8FA8C2; line-height:1.5;");
    environment_layout->addWidget(environment_title);
    environment_layout->addWidget(dpr_text);
    environment_layout->addStretch();
    body->addWidget(environment, 1);
    root->addLayout(body, 1);
    return page;
  }

  void showPage(int index) {
    if (index < 0 || index >= central_stack_->count()) {
      return;
    }
    page_tabs_->setTabVisible(index, true);
    page_tabs_->setCurrentIndex(index);
  }

  void buildCommands() {
    auto* menu_strip = new QWidget(window_.get());
    menu_strip->setObjectName("ApplicationMenuStrip");
    auto* menu_layout = new QHBoxLayout(menu_strip);
    menu_layout->setContentsMargins(8, 0, 8, 0);
    menu_layout->setSpacing(0);
    auto* brand = new QLabel("SS  Signal Studio", menu_strip);
    brand->setObjectName("SignalStudioBrand");
    menu_layout->addWidget(brand);
    auto* context = new QLabel("宽带浏览", menu_strip);
    context->setObjectName("ApplicationContext");
    menu_layout->addWidget(context);
    auto make_menu = [this, menu_strip, menu_layout](const char* label) {
      auto* menu = new QMenu(QString::fromUtf8(label), window_.get());
      auto* button = new QToolButton(menu_strip);
      button->setObjectName("ApplicationMenuButton");
      button->setText(QString::fromUtf8(label));
      button->setMenu(menu);
      button->setPopupMode(QToolButton::InstantPopup);
      button->setAutoRaise(true);
      menu_layout->addWidget(button);
      return menu;
    };
    auto* file_menu = make_menu("文件");
    view_menu_ = make_menu("视图");
    auto* analysis_menu = make_menu("分析");
    auto* tools_menu = make_menu("工具");
    auto* help_menu = make_menu("帮助");
    menu_layout->addStretch();
    auto* offline = new QLabel("● 离线   Project schema 1.1   ⚙   ?", menu_strip);
    offline->setStyleSheet("color:#46E6B0; padding:0 4px;");
    menu_layout->addWidget(offline);
    window_->setMenuWidget(menu_strip);
    auto* toolbar = main_ui_.mainToolBar;
    toolbar->setObjectName("WorkbenchCommandToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setIconSize(QSize(15, 15));
    toolbar->setMinimumHeight(37);
    toolbar->setMaximumHeight(37);
    const auto icon_for = [this](std::string_view id) {
      if (id == "view.fit-all") {
        return window_->style()->standardIcon(QStyle::SP_DesktopIcon);
      }
      if (id == "view.screenshot") {
        return window_->style()->standardIcon(QStyle::SP_DialogSaveButton);
      }
      if (id == "workbench.save-layout") {
        return window_->style()->standardIcon(QStyle::SP_DriveHDIcon);
      }
      return window_->style()->standardIcon(QStyle::SP_ArrowRight);
    };
    for (const auto& command : commands_->commands()) {
      auto* action = new QAction(icon_for(command.id), QString::fromStdString(command.label), window_.get());
      action->setObjectName(QString::fromStdString(command.id));
      action->setToolTip(QString::fromStdString(command.description));
      action->setStatusTip(QString::fromStdString(command.description));
      if (!command.shortcut.empty()) {
        action->setShortcut(QKeySequence(QString::fromStdString(command.shortcut)));
      }
      QObject::connect(action, &QAction::triggered, window_.get(), [this, id = command.id, label = command.label] {
        const auto status = commands_->execute(id);
        status_label_->setText(
            status ? QString("● 已执行 · %1").arg(QString::fromStdString(label))
                   : QString("! 不可执行 · %1").arg(QString::fromStdString(std::string(status.message()))));
      });
      toolbar->addAction(action);
      if (command.id.starts_with("file.")) {
        file_menu->addAction(action);
      } else if (command.id.starts_with("view.")) {
        view_menu_->addAction(action);
      } else if (command.id.starts_with("analysis.")) {
        analysis_menu->addAction(action);
      } else {
        tools_menu->addAction(action);
      }
    }
    auto* task_toolbar_action = new QAction("任务", window_.get());
    task_toolbar_action->setObjectName("toolbar.task-center");
    task_toolbar_action->setToolTip("打开任务中心页面");
    QObject::connect(task_toolbar_action, &QAction::triggered, window_.get(), [this] { showPage(2); });
    toolbar->addAction(task_toolbar_action);
    auto* toolbar_spacer = new QWidget(toolbar);
    toolbar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(toolbar_spacer);
    source_label_ = new QLabel(QString::fromStdString(configuration_.content.source_summary), toolbar);
    source_label_->setStyleSheet("color:#E5F1FF; font-family:'Cascadia Mono'; font-size:10px;");
    toolbar->addWidget(source_label_);
    auto* current = new QLabel("  ● 当前", toolbar);
    current->setStyleSheet("color:#46E6B0; padding:0 8px;");
    toolbar->addWidget(current);
    file_menu->addAction("关闭", QKeySequence::Close, window_.get(), &QWidget::close);
    analysis_menu->addAction("从 Selection 创建通道");
    help_menu->addAction("键盘与可访问性");

    addPageAction("p01", "项目首页");
    addPageAction("p02", "宽带浏览");
    addPageAction("p04", "任务中心页面");
    addPageAction("p07", "设置与诊断页面");
  }

  void addPageAction(const std::string& page_id, const std::string& title) {
    if (view_menu_ == nullptr || page_actions_.contains(page_id)) {
      return;
    }
    auto* action = view_menu_->addAction(QString::fromStdString(title));
    const auto object_name = page_id == "p01"   ? "page.home"
                             : page_id == "p02" ? "page.wideband"
                             : page_id == "p04" ? "page.task-center"
                             : page_id == "p07" ? "page.settings"
                                                : "page." + page_id;
    action->setObjectName(QString::fromStdString(object_name));
    QObject::connect(action, &QAction::triggered, window_.get(),
                     [this, page_id] { static_cast<void>(show_page(page_id)); });
    page_actions_.emplace(page_id, action);
  }

  void buildPanels() {
    for (const auto& descriptor : panels_->panels()) {
      auto* dock = new QDockWidget(QString::fromStdString(descriptor.title), window_.get());
      dock->setObjectName(QString::fromStdString(descriptor.id));
      dock->setAccessibleName(QString::fromStdString(descriptor.title));
      dock->setAccessibleDescription(QString::fromStdString(descriptor.accessible_description));
      QDockWidget::DockWidgetFeatures features{};
      if (descriptor.closable) {
        features |= QDockWidget::DockWidgetClosable;
      }
      if (descriptor.movable) {
        features |= QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable;
      }
      dock->setFeatures(features);
      auto* title_bar = new QWidget(dock);
      title_bar->setObjectName("DockTitleBar");
      title_bar->setAccessibleName(QString::fromStdString(descriptor.title + " 面板标题栏"));
      auto* title_layout = new QHBoxLayout(title_bar);
      title_layout->setContentsMargins(7, 2, 3, 2);
      title_layout->setSpacing(3);
      auto* title_label = new QLabel(QString::fromStdString(descriptor.title), title_bar);
      if (descriptor.id == "navigator") {
        title_label->setText("NAVIGATOR   项目与数据源");
        title_label->setStyleSheet("color:#E5F1FF; font-weight:600;");
      }
      title_layout->addWidget(title_label);
      title_layout->addStretch();
      if (descriptor.movable) {
        auto* float_button = new QToolButton(title_bar);
        float_button->setIcon(window_->style()->standardIcon(QStyle::SP_TitleBarNormalButton));
        float_button->setToolTip("浮动或停靠面板");
        float_button->setAccessibleName(QString::fromStdString("浮动或停靠" + descriptor.title));
        QObject::connect(float_button, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
        title_layout->addWidget(float_button);
      }
      if (descriptor.closable) {
        auto* close_button = new QToolButton(title_bar);
        close_button->setIcon(window_->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        close_button->setToolTip("关闭面板");
        close_button->setAccessibleName(QString::fromStdString("关闭" + descriptor.title));
        QObject::connect(close_button, &QToolButton::clicked, dock, &QDockWidget::close);
        title_layout->addWidget(close_button);
      }
      dock->setTitleBarWidget(title_bar);
      dock->setWidget(createPanelContent(descriptor.id));
      window_->addDockWidget(to_qt_area(descriptor.default_area), dock);
      docks_.emplace(descriptor.id, dock);
      auto* action = dock->toggleViewAction();
      action->setText(QString::fromStdString(descriptor.title));
      action->setShortcutContext(Qt::ApplicationShortcut);
      view_menu_->addAction(action);
    }
    const auto task = docks_.find("task-center");
    const auto result = docks_.find("result-center");
    if (task != docks_.end() && result != docks_.end()) {
      window_->tabifyDockWidget(task->second, result->second);
      window_->resizeDocks({task->second}, {140}, Qt::Vertical);
      task->second->show();
      task->second->raise();
    }
    const auto inspector = docks_.find("inspector");
    if (inspector != docks_.end()) {
      window_->resizeDocks({inspector->second}, {300}, Qt::Horizontal);
    }
    const auto settings = docks_.find("settings");
    const auto diagnostics = docks_.find("diagnostics");
    if (inspector != docks_.end() && settings != docks_.end() && diagnostics != docks_.end()) {
      window_->tabifyDockWidget(inspector->second, settings->second);
      window_->tabifyDockWidget(inspector->second, diagnostics->second);
      inspector->second->show();
      inspector->second->raise();
      if (inspector_edge_ != nullptr) {
        inspector_edge_->setVisible(false);
        QObject::connect(inspector->second, &QDockWidget::visibilityChanged, inspector_edge_,
                         [this](bool visible) { inspector_edge_->setVisible(!visible); });
      }
    }
    const auto navigator = docks_.find("navigator");
    if (navigator != docks_.end()) {
      navigator->second->setMinimumWidth(188);
      window_->resizeDocks({navigator->second}, {230}, Qt::Horizontal);
    }
    for (const auto& [id, dock] : docks_) {
      if (id != "navigator" && id != "inspector" && id != "task-center") {
        dock->hide();
      }
    }
  }

  [[nodiscard]] QWidget* createPanelContent(const std::string& id) {
    if (id == "navigator") {
      auto* content = new QWidget;
      content->setObjectName("ProjectNavigator");
      auto* layout = new QVBoxLayout(content);
      layout->setContentsMargins(7, 7, 7, 7);
      layout->setSpacing(5);
      auto* workspace_title = new QLabel("⌂  Signal Studio 工作区", content);
      workspace_title->setStyleSheet("color:#9CCBFF; font-weight:600; padding:6px 2px;");
      layout->addWidget(workspace_title);
      auto* current_label = new QLabel("当前项目", content);
      current_label->setStyleSheet("color:#8FA8C2; font-size:10px;");
      layout->addWidget(current_label);
      auto* tree = new QTreeWidget(content);
      tree->setObjectName("ProjectNavigatorTree");
      tree->setAccessibleName("项目与数据源导航");
      tree->setHeaderHidden(true);
      tree->setRootIsDecorated(false);
      tree->setIndentation(13);
      tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      tree->setTextElideMode(Qt::ElideRight);
      tree->setFrameShape(QFrame::NoFrame);
      tree->setStyleSheet("QTreeWidget{border:0;background:#08111F;}"
                          "QTreeWidget::item{min-height:24px;border:0;}"
                          "QTreeWidget::item:selected{background:#12324A;color:#E5F1FF;}");
      for (const auto& entry : configuration_.content.navigation) {
        auto* item = new QTreeWidgetItem(
            {QString("%1%2%3").arg(QString(static_cast<int>(entry.depth * 2), QLatin1Char(' ')),
                                   entry.current ? "● " : "  ", QString::fromStdString(entry.label)),
             QString::fromStdString(entry.badge)});
        if (entry.current) {
          item->setForeground(0, QColor("#E5F1FF"));
          item->setBackground(0, QColor("#12324A"));
        }
        tree->addTopLevelItem(item);
      }
      tree->setColumnCount(2);
      tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
      tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
      layout->addWidget(tree, 1);

      auto* global_label = new QLabel("全局", content);
      global_label->setStyleSheet("color:#8FA8C2; font-size:10px;");
      layout->addWidget(global_label);
      const auto add_navigation = [this, layout, content](const QString& label, int page_index) {
        auto* button = new QToolButton(content);
        button->setText(label);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setStyleSheet("QToolButton{text-align:left;border-color:transparent;padding-left:7px;}"
                              "QToolButton:hover{background:#12324A;border-color:#2B4867;}");
        QObject::connect(button, &QToolButton::clicked, window_.get(), [this, page_index] { showPage(page_index); });
        layout->addWidget(button);
      };
      add_navigation("☷  任务中心", 2);
      add_navigation("⬡  插件与模型", 1);
      add_navigation("⚙  设置与诊断", 3);
      auto* project =
          new QLabel(QString("PROJECT\n%1").arg(QString::fromStdString(configuration_.content.project_name)), content);
      project->setStyleSheet("color:#8FA8C2; font-family:'Cascadia Mono'; font-size:10px;"
                             "border-top:1px solid #203A55; padding-top:6px;");
      layout->addWidget(project);
      return content;
    }
    if (id == "inspector") {
      auto* content = new QWidget;
      Ui::SignalInspectorPanel ui;
      ui.setupUi(content);
      ui.sectionHeading->setObjectName("SectionHeading");
      if (configuration_.content.inspector.empty()) {
        ui.inspectorFormLayout->addRow(new QLabel("未选择对象", content));
      } else {
        for (const auto& entry : configuration_.content.inspector) {
          ui.inspectorFormLayout->addRow(QString::fromStdString(entry.label),
                                         new QLabel(QString::fromStdString(entry.value), content));
        }
      }
      return content;
    }
    if (id == "task-center") {
      auto* content = new QWidget;
      Ui::SignalTaskCenterPanel ui;
      ui.setupUi(content);
      auto* tree = ui.taskTree;
      tree->setAccessibleName("任务中心列表");
      for (const auto& entry : configuration_.content.tasks) {
        tree->addTopLevelItem(
            new QTreeWidgetItem({QString::fromStdString(entry.task), QString::fromStdString(entry.state_text),
                                 QString::fromStdString(entry.progress_text), QString::fromStdString(entry.backend),
                                 QString::fromStdString(entry.source)}));
      }
      if (configuration_.content.tasks.empty()) {
        tree->setAccessibleDescription("当前无任务");
      }
      tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
      tree->header()->setStretchLastSection(true);
      return content;
    }
    if (id == "result-center") {
      auto* content = new QWidget;
      Ui::SignalResultCenterPanel ui;
      ui.setupUi(content);
      auto* table = ui.resultTable;
      table->setRowCount(static_cast<int>(configuration_.content.results.size()));
      table->setAccessibleName("结果中心列表");
      for (std::size_t index = 0; index < configuration_.content.results.size(); ++index) {
        const auto& entry = configuration_.content.results[index];
        const auto row = static_cast<int>(index);
        table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(entry.result)));
        table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(entry.validity_text)));
        table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(entry.data_source_version_id)));
        table->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(entry.location_action)));
      }
      if (configuration_.content.results.empty()) {
        table->setAccessibleDescription("当前无结果");
      }
      table->horizontalHeader()->setStretchLastSection(true);
      return content;
    }
    if (id == "settings") {
      auto* content = new QWidget;
      Ui::SignalSettingsPanel ui;
      ui.setupUi(content);
      ui.pointerTargetValue->setText(
          QString("%1 × %1 px").arg(static_cast<qulonglong>(configuration_.theme.pointer_target)));
      ui.focusRingValue->setText(QString("%1 px · %2")
                                     .arg(static_cast<qulonglong>(configuration_.theme.focus_ring_width))
                                     .arg(QString::fromStdString(configuration_.theme.accent)));
      return content;
    }
    if (id == "diagnostics") {
      auto* content = new QWidget;
      Ui::SignalDiagnosticsPanel ui;
      ui.setupUi(content);
      auto* tree = ui.diagnosticsTree;
      tree->setAccessibleName("诊断快照");
      if (diagnostics_) {
        for (const auto& item : diagnostics_->snapshot().items) {
          tree->addTopLevelItem(
              new QTreeWidgetItem({QString::fromStdString(item.key + ": " + item.value),
                                   QString::fromStdString(item.state_text), QString::fromStdString(item.action)}));
        }
      } else {
        tree->addTopLevelItem(new QTreeWidgetItem({"诊断提供者", "! 未提供", "配置提供者"}));
      }
      tree->header()->setStretchLastSection(true);
      return content;
    }
    auto* fallback = new QLabel(QString("面板 %1").arg(QString::fromStdString(id)));
    fallback->setAlignment(Qt::AlignCenter);
    return fallback;
  }

  // 显式析构路径先解除中心 QWidget 的 Qt 父子关系，再分别销毁工作区和主窗口。
  std::unique_ptr<QMainWindow> window_;
  Ui::SignalWorkbenchMainWindow main_ui_;
  std::unique_ptr<visualization::IAnalysisWorkspace> workspace_;
  std::shared_ptr<CommandRegistry> commands_;
  std::shared_ptr<PanelRegistry> panels_;
  std::shared_ptr<IDiagnosticsProvider> diagnostics_;
  WorkbenchConfiguration configuration_;
  std::optional<WorkbenchLayout> saved_layout_;
  std::map<std::string, QDockWidget*, std::less<>> docks_;
  std::map<std::string, int, std::less<>> page_indices_;
  std::map<std::string, QAction*, std::less<>> page_actions_;
  QMenu* view_menu_{};
  QTabBar* page_tabs_{};
  QStackedWidget* central_stack_{};
  QTableWidget* task_page_table_{};
  QLabel* status_label_{};
  QLabel* resource_label_{};
  QToolButton* inspector_edge_{};
  QLabel* source_label_{};
};

} // namespace

std::unique_ptr<IWorkbenchWindow>
make_workbench_window(WorkbenchConfiguration configuration,
                      std::unique_ptr<visualization::IAnalysisWorkspace> center_workspace,
                      std::shared_ptr<CommandRegistry> commands, std::shared_ptr<PanelRegistry> panels,
                      std::shared_ptr<IDiagnosticsProvider> diagnostics) {
  if (QApplication::instance() == nullptr || !center_workspace || !commands || !panels ||
      !validate_theme(configuration.theme)) {
    return {};
  }
  return std::make_unique<QtWorkbenchWindow>(std::move(configuration), std::move(center_workspace), std::move(commands),
                                             std::move(panels), std::move(diagnostics));
}

} // namespace signal::workbench
