#include "signal_studio/workbench/workbench.hpp"

#include "ui_SignalDiagnosticsPanel.h"
#include "ui_SignalInspectorPanel.h"
#include "ui_SignalResultCenterPanel.h"
#include "ui_SignalSettingsPanel.h"
#include "ui_SignalTaskCenterPanel.h"
#include "ui_SignalWorkbenchMainWindow.h"

#include <QtCore/QByteArray>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QKeySequence>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeWidget>
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

class QtWorkbenchWindow final : public IWorkbenchWindow {
public:
  QtWorkbenchWindow(WorkbenchConfiguration configuration,
                    std::unique_ptr<visualization::IAnalysisWorkspace> center_workspace,
                    std::shared_ptr<CommandRegistry> commands, std::shared_ptr<PanelRegistry> panels,
                    std::shared_ptr<IDiagnosticsProvider> diagnostics)
      : window_(std::make_unique<QMainWindow>()), workspace_(std::move(center_workspace)),
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

  [[nodiscard]] void* native_handle() noexcept override {
    return window_.get();
  }

  void show() override {
    window_->show();
    normalizeInteractiveTargets();
    QTimer::singleShot(0, window_.get(), [this] { normalizeInteractiveTargets(); });
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
      QMenuBar, QMenu, QToolBar, QStatusBar { background:%4; color:%2; }
      QMenuBar::item:selected, QMenu::item:selected { background:#16334E; }
      QToolBar { border-bottom:1px solid #203A55; spacing:4px; padding:3px; }
      QToolButton, QPushButton {
        background:#10243A; color:%2; border:1px solid #2B4867; border-radius:2px;
        min-width:28px; min-height:28px; padding:0 7px;
      }
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
      QTreeWidget, QTableWidget, QPlainTextEdit {
        background:#0A1727; alternate-background-color:#0E1B2D;
        border:1px solid #203A55; gridline-color:#203A55;
      }
      QHeaderView::section { background:#13263D; color:%2; border:0; padding:5px; }
      QLabel#SectionHeading { color:%5; font-weight:600; }
    )")
                               .arg(QString::fromStdString(configuration_.theme.canvas),
                                    QString::fromStdString(configuration_.theme.primary_text),
                                    QString::fromStdString(configuration_.theme.ui_font),
                                    QString::fromStdString(configuration_.theme.panel),
                                    QString::fromStdString(configuration_.theme.accent)));
    auto* central = static_cast<QWidget*>(workspace_->native_handle());
    window_->setCentralWidget(central);
    window_->statusBar()->setSizeGripEnabled(true);
    status_label_ = new QLabel(QString::fromStdString(configuration_.content.status_text), window_.get());
    status_label_->setAccessibleName("工作台状态通知");
    window_->statusBar()->addWidget(status_label_, 1);
    auto* backend = new QLabel(QString::fromStdString(configuration_.content.resource_text), window_.get());
    backend->setAccessibleName("资源状态");
    window_->statusBar()->addPermanentWidget(backend);
  }

  void buildCommands() {
    auto* file_menu = window_->menuBar()->addMenu("文件");
    auto* view_menu = window_->menuBar()->addMenu("视图");
    auto* analysis_menu = window_->menuBar()->addMenu("分析");
    auto* tools_menu = window_->menuBar()->addMenu("工具");
    auto* help_menu = window_->menuBar()->addMenu("帮助");
    auto* toolbar = main_ui_.mainToolBar;
    toolbar->setObjectName("WorkbenchCommandToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
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
      if (command.id.starts_with("view.")) {
        view_menu->addAction(action);
      } else {
        tools_menu->addAction(action);
      }
    }
    file_menu->addAction("关闭", QKeySequence::Close, window_.get(), &QWidget::close);
    analysis_menu->addAction("从 Selection 创建通道");
    help_menu->addAction("键盘与可访问性");
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
      window_->menuBar()->actions().at(1)->menu()->addAction(action);
    }
    const auto task = docks_.find("task-center");
    const auto result = docks_.find("result-center");
    if (task != docks_.end() && result != docks_.end()) {
      window_->tabifyDockWidget(task->second, result->second);
      task->second->raise();
      window_->resizeDocks({task->second}, {140}, Qt::Vertical);
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
      inspector->second->raise();
    }
  }

  [[nodiscard]] QWidget* createPanelContent(const std::string& id) {
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

  // window_ 必须先声明；析构时 workspace_ 先删除中心 QWidget，使 QObject
  // 子对象关系从 QMainWindow 中安全解除，再销毁主窗口。
  std::unique_ptr<QMainWindow> window_;
  Ui::SignalWorkbenchMainWindow main_ui_;
  std::unique_ptr<visualization::IAnalysisWorkspace> workspace_;
  std::shared_ptr<CommandRegistry> commands_;
  std::shared_ptr<PanelRegistry> panels_;
  std::shared_ptr<IDiagnosticsProvider> diagnostics_;
  WorkbenchConfiguration configuration_;
  std::optional<WorkbenchLayout> saved_layout_;
  std::map<std::string, QDockWidget*, std::less<>> docks_;
  QLabel* status_label_{};
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
