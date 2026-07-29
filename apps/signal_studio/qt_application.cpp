#include "qt_application.hpp"

#include "ui_SignalImportWizard.h"
#include "ui_SignalInspectorPage.h"
#include "ui_SignalLoadProgressDialog.h"
#include "ui_SignalResultCenterPage.h"
#include "ui_SignalStudioProjectHome.h"

#include "signal_studio/data/io.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLayout>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QTreeWidgetItem>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

namespace signal::studio {
namespace {

[[nodiscard]] core::Status ui_failure(std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::core, core::ErrorReason::internal_failure}, std::move(message),
                               std::move(diagnostic));
}

[[nodiscard]] QString status_message(const core::Status& status) {
  auto text = QString::fromStdString(std::string{status.message()});
  if (!status.diagnostic().empty()) {
    text += "\n\n" + QString::fromStdString(std::string{status.diagnostic()});
  }
  return text;
}

class RuntimeDiagnostics final : public workbench::IDiagnosticsProvider {
public:
  [[nodiscard]] workbench::DiagnosticsSnapshot snapshot() const override {
    return {"runtime",
            {{"Qt", qVersion(), "可用", "查看平台插件"},
             {"平台", QGuiApplication::platformName().toStdString(), "当前", "复制诊断"},
             {"计算", "CPU / CUDA 12.4 可选", "就绪", "重新探测"},
             {"模式", "离线且源数据只读", "健康", "查看说明"}}};
  }
};

[[nodiscard]] core::Result<data::SignalDescriptor> descriptor_from_dialog(const Ui::SignalImportWizard& ui,
                                                                          const std::filesystem::path& path) {
  if (ui.formatCombo->currentIndex() == 1) {
    auto wav = data::read_wav_descriptor(path, true, data::ComponentOrder::iq);
    if (!wav) {
      return wav.error();
    }
    return wav.value().descriptor;
  }
  std::error_code error;
  const auto bytes = std::filesystem::file_size(path, error);
  if (error || bytes == 0U) {
    return core::Status::failure({core::ErrorDomain::data, core::ErrorReason::unavailable}, "源文件不存在或为空",
                                 error.message());
  }
  data::SignalDescriptor descriptor;
  descriptor.signal_kind = ui.kindCombo->currentIndex() == 0 ? data::SignalKind::complex : data::SignalKind::real;
  descriptor.scalar_type = ui.scalarCombo->currentIndex() == 0   ? data::ScalarType::int16
                           : ui.scalarCombo->currentIndex() == 1 ? data::ScalarType::float32
                                                                 : data::ScalarType::int8;
  descriptor.component_layout = descriptor.signal_kind == data::SignalKind::complex ? data::ComponentLayout::interleaved
                                                                                    : data::ComponentLayout::real;
  descriptor.component_order = descriptor.signal_kind == data::SignalKind::complex
                                   ? data::ComponentOrder::iq
                                   : data::ComponentOrder::not_applicable;
  descriptor.endianness =
      descriptor.scalar_type == data::ScalarType::int8
          ? data::Endianness::not_applicable
          : (ui.byteOrderCombo->currentIndex() == 0 ? data::Endianness::little : data::Endianness::big);
  descriptor.sample_rate_hz = ui.sampleRateSpin->value();
  if (ui.centerFrequencySpin->value() > 0.0) {
    descriptor.center_frequency_hz = ui.centerFrequencySpin->value();
  }
  descriptor.amplitude_mode = descriptor.scalar_type == data::ScalarType::int16 ? "int16_scaled" : "linear";
  descriptor.scale_factor = descriptor.scalar_type == data::ScalarType::int16 ? 1.0 / 32768.0 : 1.0;
  auto frame_bytes = descriptor.frame_bytes();
  if (!frame_bytes || bytes % frame_bytes.value() != 0U) {
    return core::Status::failure({core::ErrorDomain::data, core::ErrorReason::invalid_argument},
                                 "文件字节数与所选完整样本帧不对齐");
  }
  auto range = data::SampleRange::from_count(0U, bytes / frame_bytes.value());
  if (!range) {
    return range.error();
  }
  descriptor.requested_sample_range = range.value();
  descriptor.provenance = {{"source", {data::FieldOrigin::user, true}},
                           {"sample_rate_hz", {data::FieldOrigin::user, true}}};
  if (descriptor.center_frequency_hz) {
    descriptor.provenance.emplace("center_frequency_hz", data::FieldProvenance{data::FieldOrigin::user, true});
  }
  if (const auto validated = descriptor.validate(); !validated) {
    return validated;
  }
  return descriptor;
}

[[nodiscard]] QString hint_summary(const FilenameHints& hints) {
  QStringList parts;
  if (hints.sample_rate_hz) {
    parts << QString("采样率 %1 Hz").arg(*hints.sample_rate_hz, 0, 'f', 3);
  }
  if (hints.center_frequency_hz) {
    parts << QString("中心频率 %1 Hz").arg(*hints.center_frequency_hz, 0, 'f', 3);
  }
  if (hints.scalar_type) {
    parts << "复数 Int16 / I-Q 交错";
  }
  return parts.empty() ? "未从文件名检测到可用提示" : parts.join("；") + "（待确认）";
}

} // namespace

class QtApplication::Impl final {
public:
  Impl(QApplication& qt_application, std::filesystem::path state_directory)
      : qt_application_(qt_application), state_directory_(std::move(state_directory)), controller_(state_directory_) {}

  ~Impl() {
    // 顶层对话框由主窗口的 QObject 树持有，但其 destroyed 回调会更新本对象中的
    // Ui 包装器。必须在成员逆序析构开始前主动销毁，避免主窗口最后清理子对象时
    // 回调访问已经析构的 unique_ptr。
    preview_coordinator_.cancel_current();
    if (preview_task_) {
      static_cast<void>(preview_task_->cancel());
    }
    if (active_import_) {
      static_cast<void>(active_import_->handle().cancel());
    }
    if (active_analysis_) {
      static_cast<void>(active_analysis_->cancel());
    }
    destroy_dialog(progress_dialog_);
    progress_ui_.reset();
    destroy_dialog(import_dialog_);
    import_ui_.reset();
  }

  [[nodiscard]] core::Status initialize() {
    workspace_ = visualization::make_analysis_workspace({.title = "宽带浏览", .source_label = "未绑定数据源"});
    if (!workspace_) {
      return ui_failure("无法创建宽带分析工作区");
    }
    primary_workspace_ = workspace_.get();
    workspace_native_ = static_cast<QWidget*>(workspace_->native_handle());

    commands_ = std::make_shared<workbench::CommandRegistry>();
    panels_ = std::make_shared<workbench::PanelRegistry>();
    diagnostics_ = std::make_shared<RuntimeDiagnostics>();
    if (const auto status = register_commands(); !status) {
      return status;
    }
    workbench::WorkbenchConfiguration configuration{.application_name = "Signal Studio", .window_title = "项目首页"};
    configuration.content = controller_.workbench_content();
    window_ = workbench::make_workbench_window(std::move(configuration), std::move(workspace_), commands_, panels_,
                                               diagnostics_);
    if (!window_) {
      return ui_failure("无法创建 Signal Studio 工作台");
    }
    native_window_ = static_cast<QMainWindow*>(window_->native_handle());
    if (const auto status = build_home_page(); !status) {
      return status;
    }
    if (const auto status = build_inspector_page(); !status) {
      return status;
    }
    if (const auto status = build_result_page(); !status) {
      return status;
    }
    refresh_all();
    return window_->show_page("p01");
  }

  [[nodiscard]] core::Status prepare_automation_input(const std::filesystem::path& source) {
    const auto automation_session = state_directory_ / ("automation-" + task::TaskId::generate().value);
    const auto project = automation_session / "automation.signal-workspace";
    std::error_code error;
    std::filesystem::create_directories(project.parent_path(), error);
    if (error) {
      return ui_failure("自动化工程目录不可创建", error.message());
    }
    auto status = controller_.create_project(project, "ms04-automation");
    if (!status) {
      return status;
    }
    auto request = make_request_for_path(source, true);
    if (!request) {
      return request.error();
    }
    auto task = controller_.start_import(request.value());
    if (!task) {
      return task.error();
    }
    auto imported = controller_.finalize_import(task.value());
    if (!imported) {
      return imported.error();
    }
    auto analysis = controller_.analyze(imported.value());
    if (!analysis) {
      return analysis.error();
    }
    if (const auto applied = apply_analysis(analysis.value()); !applied) {
      return applied;
    }
    auto result = controller_.commit_measurement(analysis.value(), "automation-selection");
    if (!result) {
      return result.error();
    }
    refresh_all();
    return core::Status::success();
  }

  void show() {
    window_->show();
  }

  [[nodiscard]] void* native_handle() noexcept {
    return native_window_;
  }

  [[nodiscard]] void* capture_handle() noexcept {
    if (progress_dialog_ != nullptr && progress_dialog_->isVisible()) {
      return progress_dialog_;
    }
    if (import_dialog_ != nullptr && import_dialog_->isVisible()) {
      return import_dialog_;
    }
    return native_window_;
  }

  [[nodiscard]] core::Status save_screenshot(const std::filesystem::path& path) {
    if (native_window_ == nullptr) {
      return ui_failure("主窗口尚未初始化");
    }
    auto image = native_window_->grab();
    auto* overlay = static_cast<QWidget*>(capture_handle());
    if (overlay != nullptr && overlay != native_window_) {
      QPainter painter{&image};
      painter.fillRect(image.rect(), QColor{1, 8, 18, 172});
      const auto dialog = overlay->grab();
      const auto x = std::max(0, (image.width() - dialog.width()) / 2);
      const auto y = std::max(0, (image.height() - dialog.height()) / 2);
      painter.drawPixmap(x, y, dialog);
    }
    const auto target = QString::fromStdWString(path.wstring());
    if (!image.save(target, "PNG")) {
      return ui_failure("无法保存自动化截图", path.string());
    }
    return core::Status::success();
  }

  [[nodiscard]] core::Status show_page(std::string_view page_id) {
    refresh_all();
    return window_->show_page(page_id);
  }

  void show_import_wizard() {
    if (import_dialog_) {
      import_dialog_->raise();
      import_dialog_->activateWindow();
      return;
    }
    import_dialog_ = new QDialog(native_window_);
    import_dialog_->setAttribute(Qt::WA_DontShowOnScreen, native_window_->testAttribute(Qt::WA_DontShowOnScreen));
    import_ui_ = std::make_unique<Ui::SignalImportWizard>();
    import_ui_->setupUi(import_dialog_);
    import_ui_->buttonBox->button(QDialogButtonBox::Ok)->setText("开始导入");
    import_ui_->buttonBox->button(QDialogButtonBox::Cancel)->setText("取消");
    import_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    apply_dialog_style(import_dialog_);
    QObject::connect(import_dialog_, &QObject::destroyed, native_window_, [this] {
      preview_coordinator_.cancel_current();
      if (preview_task_) {
        static_cast<void>(preview_task_->cancel());
      }
      import_dialog_ = nullptr;
      import_ui_.reset();
    });
    QObject::connect(import_ui_->browseSourceButton, &QPushButton::clicked, import_dialog_, [this] {
      const auto path = QFileDialog::getOpenFileName(import_dialog_, "选择信号录制", {},
                                                     "信号数据 (*.raw *.iq *.sc16 *.wav);;所有文件 (*)");
      if (!path.isEmpty()) {
        import_ui_->sourcePathEdit->setText(path);
        update_import_hints(path.toStdWString());
      }
    });
    QObject::connect(import_ui_->sourcePathEdit, &QLineEdit::editingFinished, import_dialog_,
                     [this] { update_import_hints(import_ui_->sourcePathEdit->text().toStdWString()); });
    QObject::connect(import_ui_->confirmHintsCheck, &QCheckBox::toggled, import_dialog_, [this](bool checked) {
      if (checked) {
        begin_bounded_preview();
      } else {
        preview_coordinator_.cancel_current();
        if (preview_task_) {
          static_cast<void>(preview_task_->cancel());
        }
        import_ui_->previewStatusLabel->setText("确认格式后将由任务运行时读取有限样本");
      }
    });
    QObject::connect(import_ui_->buttonBox, &QDialogButtonBox::rejected, import_dialog_, &QDialog::reject);
    QObject::connect(import_ui_->buttonBox, &QDialogButtonBox::accepted, import_dialog_,
                     [this] { begin_import_from_dialog(); });
    if (const auto& imported = controller_.current_signal(); imported) {
      const auto path = QString::fromStdWString(imported->source_path.wstring());
      import_ui_->sourcePathEdit->setText(path);
      update_import_hints(imported->source_path);
    }
    import_dialog_->show();
  }

  void show_progress_preview(const std::filesystem::path& source) {
    auto request = make_request_for_path(source, true);
    if (!request) {
      show_error(request.error());
      return;
    }
    ensure_project_for_automation();
    auto task = controller_.start_import(request.value());
    if (!task) {
      show_error(task.error());
      return;
    }
    active_import_ = std::move(task.value());
    static_cast<void>(active_import_->handle().pause());
    create_progress_dialog(source);
    progress_ui_->loadStatusLabel->setText("任务已真实提交并暂停，可继续或取消");
    progress_ui_->pauseResumeButton->setText("继续");
  }

private:
  static void destroy_dialog(QDialog*& dialog) noexcept {
    if (dialog == nullptr) {
      return;
    }
    QObject::disconnect(dialog, nullptr, nullptr, nullptr);
    delete dialog;
    dialog = nullptr;
  }

  [[nodiscard]] core::Status register_commands() {
    const auto add = [this](workbench::Command command) { return commands_->add(std::move(command)); };
    if (auto status = add({"file.new-project",
                           "新建",
                           "新建 Signal Studio 工程",
                           "Ctrl+N",
                           {},
                           [] { return true; },
                           [this] { return choose_new_project(); }});
        !status) {
      return status;
    }
    if (auto status = add({"file.open-project",
                           "打开",
                           "打开现有工程",
                           "Ctrl+O",
                           {},
                           [] { return true; },
                           [this] { return choose_open_project(); }});
        !status) {
      return status;
    }
    if (auto status = add({"file.import-signal",
                           "导入信号",
                           "打开 RAW/IQ 导入向导",
                           "Ctrl+I",
                           {},
                           [] { return true; },
                           [this] {
                             show_import_wizard();
                             return core::Status::success();
                           }});
        !status) {
      return status;
    }
    if (auto status = add({"file.save-project",
                           "保存",
                           "原子保存当前工程",
                           "Ctrl+S",
                           {},
                           [this] { return !controller_.project_path().empty(); },
                           [this] { return controller_.save_project(); }});
        !status) {
      return status;
    }
    return add({"analysis.commit-measurement",
                "保存测量",
                "提交当前真实 PSD 测量结果",
                {},
                {},
                [this] { return controller_.current_analysis().has_value(); },
                [this] {
                  auto result = controller_.commit_measurement(*controller_.current_analysis());
                  if (!result) {
                    return result.error();
                  }
                  refresh_all();
                  return core::Status::success();
                }});
  }

  [[nodiscard]] core::Status build_home_page() {
    home_page_ = new QWidget(native_window_);
    home_ui_ = std::make_unique<Ui::SignalStudioProjectHome>();
    home_ui_->setupUi(home_page_);
    home_ui_->pageCodeLabel->setObjectName("PageEyebrow");
    home_ui_->heroTitleLabel->setObjectName("PageTitle");
    home_ui_->heroPanel->setObjectName("PagePanel");
    home_ui_->recentPanel->setObjectName("PagePanel");
    home_ui_->systemPanel->setObjectName("PagePanel");
    home_ui_->heroDescriptionLabel->setStyleSheet("color:#8FA8C2;");
    home_ui_->recentProjectsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    home_ui_->recentProjectsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    QObject::connect(home_ui_->newProjectButton, &QPushButton::clicked, home_page_,
                     [this] { static_cast<void>(choose_new_project()); });
    QObject::connect(home_ui_->openProjectButton, &QPushButton::clicked, home_page_,
                     [this] { static_cast<void>(choose_open_project()); });
    QObject::connect(home_ui_->importSignalButton, &QPushButton::clicked, home_page_, [this] { show_import_wizard(); });
    QObject::connect(home_ui_->diagnosticsButton, &QPushButton::clicked, home_page_,
                     [this] { static_cast<void>(window_->show_page("p07")); });
    return window_->install_page({"p01", "项目首页", home_page_, true});
  }

  [[nodiscard]] core::Status build_inspector_page() {
    inspector_page_ = new QWidget(native_window_);
    inspector_ui_ = std::make_unique<Ui::SignalInspectorPage>();
    inspector_ui_->setupUi(inspector_page_);
    inspector_ui_->inspectorCodeLabel->setObjectName("PageEyebrow");
    inspector_ui_->inspectorTitleLabel->setObjectName("PageTitle");
    inspector_ui_->eyeAvailabilityLabel->setStyleSheet("color:#8FA8C2;");
    inspector_workspace_ =
        visualization::make_analysis_workspace({.title = "CH-01 基础检视",
                                                .source_label = "未绑定通道",
                                                .show_waveform = true,
                                                .show_psd = true,
                                                .show_spectrogram = true,
                                                .extra_charts = {visualization::ChartKind::constellation}});
    if (!inspector_workspace_) {
      return ui_failure("无法创建检视器分析工作区");
    }
    auto* analysis_widget = static_cast<QWidget*>(inspector_workspace_->native_handle());
    inspector_ui_->analysisHostLayout->addWidget(analysis_widget);
    return window_->install_page({"p03", "窄带检视器", inspector_page_, true});
  }

  [[nodiscard]] core::Status build_result_page() {
    result_page_ = new QWidget(native_window_);
    result_ui_ = std::make_unique<Ui::SignalResultCenterPage>();
    result_ui_->setupUi(result_page_);
    result_ui_->resultCodeLabel->setObjectName("PageEyebrow");
    result_ui_->resultTitleLabel->setObjectName("PageTitle");
    result_ui_->resultDescriptionLabel->setStyleSheet("color:#8FA8C2;");
    result_ui_->resultDetailPanel->setObjectName("PagePanel");
    result_ui_->resultDetailHeading->setObjectName("SectionHeading");
    result_ui_->resultsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    result_ui_->resultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (const auto* label : {"全部结果", "当前数据源", "已过期"}) {
      result_ui_->resultCategories->addTopLevelItem(new QTreeWidgetItem({label}));
    }
    result_ui_->resultCategories->setCurrentItem(result_ui_->resultCategories->topLevelItem(0));
    QObject::connect(result_ui_->resultSearchEdit, &QLineEdit::textChanged, result_page_,
                     [this] { refresh_results(); });
    QObject::connect(result_ui_->resultStateCombo, &QComboBox::currentIndexChanged, result_page_,
                     [this] { refresh_results(); });
    QObject::connect(result_ui_->resultCategories, &QTreeWidget::currentItemChanged, result_page_,
                     [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                       if (current == nullptr) {
                         return;
                       }
                       const auto index = result_ui_->resultCategories->indexOfTopLevelItem(current);
                       if (index >= 0 && index <= 2 && result_ui_->resultStateCombo->currentIndex() != index) {
                         result_ui_->resultStateCombo->setCurrentIndex(index);
                       }
                     });
    QObject::connect(result_ui_->resultsTable, &QTableWidget::currentCellChanged, result_page_,
                     [this](int row, int, int, int) { refresh_result_detail(row); });
    QObject::connect(result_ui_->exportResultButton, &QPushButton::clicked, result_page_,
                     [this] { export_selected_result(); });
    QObject::connect(result_ui_->locateResultButton, &QPushButton::clicked, result_page_,
                     [this] { static_cast<void>(window_->show_page("p02")); });
    return window_->install_page({"p05", "结果中心", result_page_, true});
  }

  [[nodiscard]] core::Status choose_new_project() {
    const auto path = QFileDialog::getSaveFileName(native_window_, "新建 Signal Studio 工程", {},
                                                   "Signal Studio 工程 (*.signal-workspace)");
    if (path.isEmpty()) {
      return core::Status::success();
    }
    auto status = controller_.create_project(path.toStdWString(), QFileInfo(path).completeBaseName().toStdString());
    if (!status) {
      show_error(status);
      return status;
    }
    refresh_all();
    show_import_wizard();
    return status;
  }

  [[nodiscard]] core::Status choose_open_project() {
    const auto path = QFileDialog::getOpenFileName(native_window_, "打开 Signal Studio 工程", {},
                                                   "Signal Studio 工程 (*.signal-workspace)");
    if (path.isEmpty()) {
      return core::Status::success();
    }
    auto status = controller_.open_project(path.toStdWString());
    if (!status) {
      show_error(status);
      return status;
    }
    refresh_all();
    return window_->show_page("p01");
  }

  void update_import_hints(const std::filesystem::path& path) {
    if (!import_ui_) {
      return;
    }
    const auto hints = parse_filename_hints(path);
    import_ui_->filenameHintsLabel->setText(hint_summary(hints));
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    import_ui_->sourceFactsLabel->setText(
        error ? "文件不可访问"
              : QString("%1 · %2 字节 · 只读访问").arg(QString::fromStdWString(path.filename().wstring())).arg(bytes));
    if (hints.sample_rate_hz) {
      import_ui_->sampleRateSpin->setValue(*hints.sample_rate_hz);
    }
    if (hints.center_frequency_hz) {
      import_ui_->centerFrequencySpin->setValue(*hints.center_frequency_hz);
    }
    import_ui_->formatCombo->setCurrentIndex(path.extension() == L".wav" || path.extension() == L".WAV" ? 1 : 0);
  }

  void begin_import_from_dialog() {
    const auto path = std::filesystem::path{import_ui_->sourcePathEdit->text().toStdWString()};
    if (path.empty()) {
      set_import_error("请选择源文件");
      return;
    }
    const auto filename_hints = parse_filename_hints(path);
    const auto has_hints =
        filename_hints.sample_rate_hz || filename_hints.center_frequency_hz || filename_hints.scalar_type;
    if (has_hints && !import_ui_->confirmHintsCheck->isChecked()) {
      set_import_error("检测到文件名提示；必须勾选确认，或手动改名后重新选择。");
      return;
    }
    if (controller_.project_path().empty()) {
      set_import_error("请先新建或打开工程，再开始导入。");
      return;
    }
    auto descriptor = descriptor_from_dialog(*import_ui_, path);
    if (!descriptor) {
      set_import_error(QString::fromStdString(std::string{descriptor.error().message()}));
      return;
    }
    ImportRequest request{
        path, descriptor.value(),
        import_ui_->formatCombo->currentIndex() == 1 ? data::SourceFormat::wav : data::SourceFormat::raw,
        static_cast<std::uint64_t>(import_ui_->initialBytesSpin->value()) * 1024U * 1024U, 512U * 1024U};
    auto task = controller_.start_import(std::move(request));
    if (!task) {
      set_import_error(QString::fromStdString(std::string{task.error().message()}));
      return;
    }
    active_import_ = std::move(task.value());
    import_dialog_->accept();
    create_progress_dialog(path);
  }

  void begin_bounded_preview() {
    if (!import_ui_) {
      return;
    }
    const auto path = std::filesystem::path{import_ui_->sourcePathEdit->text().toStdWString()};
    if (path.empty()) {
      set_import_error("请选择源文件后再预览。");
      return;
    }
    if (import_ui_->formatCombo->currentIndex() == 1) {
      auto wav = data::read_wav_descriptor(path, true, data::ComponentOrder::iq);
      if (!wav) {
        set_import_error(QString::fromStdString(std::string{wav.error().message()}));
      } else {
        import_ui_->previewStatusLabel->setText(QString("WAV 容器已验证：%1 帧；数据区将在导入任务中按容器偏移读取。")
                                                    .arg(wav.value().descriptor.requested_sample_range.size()));
      }
      return;
    }
    auto description = descriptor_from_dialog(*import_ui_, path);
    if (!description) {
      set_import_error(QString::fromStdString(std::string{description.error().message()}));
      return;
    }
    preview_coordinator_.cancel_current();
    if (preview_task_) {
      static_cast<void>(preview_task_->cancel());
    }
    const auto token = preview_coordinator_.begin_request();
    preview_state_ = std::make_shared<PreviewState>();
    task::TaskSpec spec;
    spec.task_id = task::TaskId::generate();
    spec.task_type = "signal-studio.bounded-preview";
    spec.priority = task::TaskPriority::interactive;
    spec.resources = {.cpu_units = 1U, .io_units = 1U, .runtime_threads = 1U};
    spec.idempotency_key = path.string() + ":preview:" + spec.task_id.value;
    spec.provenance = {
        {controller_.workspace().project_id.empty() ? "preview-uncommitted" : controller_.workspace().project_id},
        {"preview-pending"},
        {"data-source", path.string()}};
    spec.timeout = std::chrono::seconds{30};
    auto submitted = controller_.task_runtime().submit(
        std::move(spec),
        [path, description = description.value(), token, state = preview_state_](task::TaskContext& context) mutable {
          if (!context.checkpoint()) {
            return task::TaskExecutionResult::completed();
          }
          auto preview = data::create_bounded_preview(
              path, description,
              {.maximum_samples = 4096U, .maximum_read_bytes = 2U * 1024U * 1024U, .spectrum_bins = 256U}, token);
          std::lock_guard lock{state->mutex};
          if (preview) {
            state->preview = std::move(preview.value());
            return task::TaskExecutionResult::completed();
          }
          state->error = preview.error();
          return task::TaskExecutionResult::failed({"SS-PREVIEW", std::string{preview.error().message()},
                                                    std::string{preview.error().diagnostic()}, "核对格式后重试", true,
                                                    "retry", "log://signal-studio/preview"});
        });
    if (!submitted) {
      set_import_error(QString::fromStdString(std::string{submitted.error().message()}));
      return;
    }
    preview_task_ = std::move(submitted.value());
    import_ui_->previewStatusLabel->setText("正在任务运行时中生成 4096 样本有界预览…");
    preview_timer_ = new QTimer(import_dialog_);
    preview_timer_->setInterval(25);
    QObject::connect(preview_timer_, &QTimer::timeout, import_dialog_, [this] { poll_preview(); });
    preview_timer_->start();
  }

  void poll_preview() {
    if (!preview_task_ || !preview_state_ || !import_ui_) {
      return;
    }
    auto status = preview_task_->status();
    if (!status || !task::is_terminal(status.value().state)) {
      return;
    }
    preview_timer_->stop();
    std::optional<data::PreviewResult> preview;
    std::optional<core::Status> error;
    {
      std::lock_guard lock{preview_state_->mutex};
      preview = std::move(preview_state_->preview);
      error = preview_state_->error;
    }
    preview_task_.reset();
    preview_state_.reset();
    if (!preview) {
      import_ui_->previewStatusLabel->setText(error ? QString::fromStdString(std::string{error->message()})
                                                    : "预览已取消");
      return;
    }
    import_ui_->previewStatusLabel->setText(QString("有界预览完成：%1 样本，RMS %2，%3 个频谱点，%4 条质量提示。")
                                                .arg(preview->statistics.sample_count)
                                                .arg(preview->statistics.rms, 0, 'g', 6)
                                                .arg(preview->spectrum.magnitude.size())
                                                .arg(preview->warnings.size()));
  }

  void create_progress_dialog(const std::filesystem::path& path) {
    if (progress_dialog_) {
      progress_dialog_->close();
    }
    progress_dialog_ = new QDialog(native_window_);
    progress_dialog_->setAttribute(Qt::WA_DontShowOnScreen, native_window_->testAttribute(Qt::WA_DontShowOnScreen));
    progress_ui_ = std::make_unique<Ui::SignalLoadProgressDialog>();
    progress_ui_->setupUi(progress_dialog_);
    progress_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    progress_dialog_->setModal(false);
    apply_dialog_style(progress_dialog_);
    progress_ui_->sourceNameLabel->setText(QString::fromStdWString(path.filename().wstring()));
    progress_ui_->sourcePathLabel->setText(QString::fromStdWString(path.wstring()));
    std::error_code size_error;
    const auto source_bytes = std::filesystem::file_size(path, size_error);
    progress_ui_->sourceSizeLabel->setText(
        size_error ? "未知"
                   : QString("%1 GiB").arg(static_cast<double>(source_bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 3));
    const auto hints = parse_filename_hints(path);
    progress_ui_->signalFormatLabel->setText(hints.scalar_type ? "复数 Int16 / I-Q 交错" : "待确认格式");
    progress_ui_->sampleRateValueLabel->setText(
        hints.sample_rate_hz ? QString("%1 MS/s").arg(*hints.sample_rate_hz / 1.0e6, 0, 'f', 3) : "待确认");
    QObject::connect(progress_dialog_, &QObject::destroyed, native_window_, [this] {
      progress_dialog_ = nullptr;
      progress_ui_.reset();
    });
    QObject::connect(progress_ui_->pauseResumeButton, &QPushButton::clicked, progress_dialog_,
                     [this] { toggle_pause(); });
    QObject::connect(progress_ui_->cancelLoadButton, &QPushButton::clicked, progress_dialog_, [this] {
      if (active_import_) {
        static_cast<void>(active_import_->handle().cancel());
        progress_ui_->loadStatusLabel->setText("正在取消；等待完整样本帧检查点");
      }
    });
    progress_timer_ = new QTimer(progress_dialog_);
    progress_timer_->setInterval(25);
    QObject::connect(progress_timer_, &QTimer::timeout, progress_dialog_, [this] { poll_import(); });
    progress_timer_->start();
    progress_dialog_->show();
  }

  void toggle_pause() {
    if (!active_import_ || !progress_ui_) {
      return;
    }
    const auto status = active_import_->handle().status();
    if (!status) {
      show_error(status.error());
      return;
    }
    if (status.value().state == task::TaskState::paused) {
      if (const auto resumed = active_import_->handle().resume(); !resumed) {
        show_error(resumed);
      } else {
        progress_ui_->pauseResumeButton->setText("暂停");
      }
    } else if (const auto paused = active_import_->handle().pause(); !paused) {
      show_error(paused);
    } else {
      progress_ui_->pauseResumeButton->setText("继续");
    }
  }

  void poll_import() {
    if (!active_import_ || !progress_ui_) {
      return;
    }
    const auto status = active_import_->handle().status();
    if (!status) {
      finish_progress_with_error(status.error());
      return;
    }
    progress_ui_->loadProgressBar->setValue(static_cast<int>(std::clamp(status.value().progress, 0.0, 1.0) * 100.0));
    progress_ui_->loadStatusLabel->setText(
        QString("%1 · %2").arg(QString::fromStdString(std::string{task::to_string(status.value().state)}),
                               QString::fromStdString(status.value().status_text)));
    if (!task::is_terminal(status.value().state)) {
      return;
    }
    progress_timer_->stop();
    auto imported = controller_.finalize_import(*active_import_);
    active_import_.reset();
    if (!imported) {
      finish_progress_with_error(imported.error());
      return;
    }
    if (imported.value().partial_read) {
      progress_ui_->partialPrefixLabel->setText(QString("取消完成：已校验前缀 [%1, %2) 保持只读可用。")
                                                    .arg(imported.value().loaded->range().begin())
                                                    .arg(imported.value().loaded->range().end()));
    }
    begin_analysis(imported.value());
  }

  void begin_analysis(ImportedSignal imported) {
    if (!progress_ui_) {
      return;
    }
    progress_ui_->progressTitleLabel->setText("正在计算基础 PSD / STFT");
    progress_ui_->loadStatusLabel->setText("分析已提交到任务运行时，UI 保持响应");
    progress_ui_->loadProgressBar->setRange(0, 0);
    progress_ui_->pauseResumeButton->setEnabled(false);
    progress_ui_->cancelLoadButton->setEnabled(false);
    analysis_state_ = std::make_shared<AnalysisState>();
    task::TaskSpec spec;
    spec.task_id = task::TaskId::generate();
    spec.task_type = "signal-studio.basic-analysis";
    spec.priority = task::TaskPriority::interactive;
    spec.resources = {.cpu_units = 1U, .runtime_threads = 1U};
    spec.idempotency_key = imported.fingerprint.version_id + ":basic-analysis-v1";
    spec.provenance = {{controller_.workspace().project_id},
                       {imported.fingerprint.version_id},
                       {"data-source", imported.source_path.string()}};
    spec.timeout = std::chrono::minutes{5};
    auto submitted = controller_.task_runtime().submit(
        std::move(spec),
        [this, imported = std::move(imported), state = analysis_state_](task::TaskContext& context) mutable {
          static_cast<void>(context.report_progress(0.1, "准备真实样本范围"));
          auto result = controller_.analyze(imported);
          {
            std::lock_guard lock{state->mutex};
            if (result) {
              state->bundle = std::move(result.value());
            } else {
              state->error = result.error();
            }
          }
          if (!result) {
            return task::TaskExecutionResult::failed({"SS-ANALYSIS", std::string{result.error().message()},
                                                      std::string{result.error().diagnostic()}, "核对导入参数后重试",
                                                      true, "retry", "log://signal-studio/analysis"});
          }
          static_cast<void>(context.report_progress(1.0, "PSD/STFT 已完成"));
          return task::TaskExecutionResult::completed();
        });
    if (!submitted) {
      finish_progress_with_error(submitted.error());
      return;
    }
    active_analysis_ = std::move(submitted.value());
    analysis_timer_ = new QTimer(progress_dialog_);
    analysis_timer_->setInterval(25);
    QObject::connect(analysis_timer_, &QTimer::timeout, progress_dialog_, [this] { poll_analysis(); });
    analysis_timer_->start();
  }

  void poll_analysis() {
    if (!active_analysis_ || !analysis_state_ || !progress_ui_) {
      return;
    }
    auto status = active_analysis_->status();
    if (!status) {
      finish_progress_with_error(status.error());
      return;
    }
    if (!task::is_terminal(status.value().state)) {
      return;
    }
    analysis_timer_->stop();
    std::optional<AnalysisBundle> bundle;
    std::optional<core::Status> error;
    {
      std::lock_guard lock{analysis_state_->mutex};
      bundle = std::move(analysis_state_->bundle);
      error = analysis_state_->error;
    }
    active_analysis_.reset();
    analysis_state_.reset();
    if (!bundle) {
      finish_progress_with_error(error.value_or(ui_failure("分析任务未返回结果")));
      return;
    }
    if (const auto applied = apply_analysis(*bundle); !applied) {
      finish_progress_with_error(applied);
      return;
    }
    auto committed = controller_.commit_measurement(*bundle, "initial-visible-range", "CH-01");
    if (!committed) {
      finish_progress_with_error(committed.error());
      return;
    }
    progress_ui_->loadProgressBar->setRange(0, 100);
    progress_ui_->loadProgressBar->setValue(100);
    progress_ui_->loadStatusLabel->setText("导入、基础分析和可追溯结果提交已完成");
    refresh_all();
    static_cast<void>(window_->show_page("p02"));
    QTimer::singleShot(500, progress_dialog_, &QDialog::accept);
  }

  [[nodiscard]] core::Status apply_analysis(const AnalysisBundle& bundle) {
    auto* primary = find_primary_workspace();
    if (primary == nullptr) {
      return ui_failure("主分析工作区句柄不可用");
    }
    if (auto applied = primary->apply_viewport(bundle.viewport); !applied) {
      return applied;
    }
    if (auto bound = primary->bind_frame(bundle.frame); !bound) {
      return bound;
    }
    if (auto applied = inspector_workspace_->apply_viewport(bundle.viewport); !applied) {
      return applied;
    }
    if (auto bound = inspector_workspace_->bind_frame(bundle.frame); !bound) {
      return bound;
    }
    inspector_ui_->inspectorVersionLabel->setText(
        QString("CH-01 · %1 · 参数 v1").arg(QString::fromStdString(bundle.frame.data_source_version_id.substr(0, 12))));
    return core::Status::success();
  }

  [[nodiscard]] visualization::IAnalysisWorkspace* find_primary_workspace() noexcept {
    return primary_workspace_;
  }

  void refresh_all() {
    if (window_) {
      static_cast<void>(window_->update_content(controller_.workbench_content()));
    }
    refresh_recent();
    refresh_results();
  }

  void refresh_recent() {
    if (!home_ui_) {
      return;
    }
    auto recent = controller_.recent_projects();
    home_ui_->recentProjectsTable->clearContents();
    if (!recent) {
      home_ui_->recentProjectsTable->setRowCount(0);
      return;
    }
    home_ui_->recentProjectsTable->setRowCount(static_cast<int>(recent.value().size()));
    for (std::size_t index = 0; index < recent.value().size(); ++index) {
      const auto& path = recent.value()[index];
      home_ui_->recentProjectsTable->setItem(static_cast<int>(index), 0,
                                             new QTableWidgetItem(QString::fromStdWString(path.stem().wstring())));
      home_ui_->recentProjectsTable->setItem(
          static_cast<int>(index), 1, new QTableWidgetItem(QString::fromStdWString(path.parent_path().wstring())));
      home_ui_->recentProjectsTable->setItem(static_cast<int>(index), 2,
                                             new QTableWidgetItem(std::filesystem::exists(path) ? "可打开" : "已移动"));
    }
  }

  void refresh_results() {
    if (!result_ui_) {
      return;
    }
    core::ArtifactFilter filter;
    const auto search = result_ui_->resultSearchEdit->text().toStdString();
    const auto current = controller_.current_signal();
    if (result_ui_->resultStateCombo->currentIndex() == 1 && current) {
      filter.data_source_version_id = current->fingerprint.version_id;
    }
    auto records = controller_.results(filter);
    result_ui_->resultsTable->clearContents();
    if (!records) {
      result_ui_->resultsTable->setRowCount(0);
      result_ui_->resultSummaryLabel->setText("结果索引不可用");
      return;
    }
    std::vector<core::ArtifactRecord> visible;
    std::size_t current_count{};
    std::size_t stale_count{};
    for (const auto& record : records.value()) {
      const auto matches_search =
          search.empty() || record.descriptor.id.find(search) != std::string::npos ||
          std::string{core::artifact_kind_name(record.descriptor.kind)}.find(search) != std::string::npos ||
          record.descriptor.provenance.data_source_version_id.find(search) != std::string::npos;
      if (!matches_search) {
        continue;
      }
      const auto is_current =
          current && record.descriptor.provenance.data_source_version_id == current->fingerprint.version_id;
      is_current ? ++current_count : ++stale_count;
      if (result_ui_->resultStateCombo->currentIndex() == 2 && is_current) {
        continue;
      }
      visible.push_back(record);
    }
    const std::array category_counts{records.value().size(), current_count, stale_count};
    for (int index = 0; index < 3; ++index) {
      if (auto* item = result_ui_->resultCategories->topLevelItem(index); item != nullptr) {
        const auto label = index == 0 ? "全部结果" : index == 1 ? "当前数据源" : "已过期";
        item->setText(0, QString("%1    %2").arg(label).arg(category_counts[static_cast<std::size_t>(index)]));
      }
    }
    result_ui_->resultsTable->setRowCount(static_cast<int>(visible.size()));
    visible_results_ = visible;
    for (std::size_t index = 0; index < visible.size(); ++index) {
      const auto& record = visible[index];
      const auto row = static_cast<int>(index);
      const auto is_current =
          current && record.descriptor.provenance.data_source_version_id == current->fingerprint.version_id;
      result_ui_->resultsTable->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(record.descriptor.id)));
      result_ui_->resultsTable->setItem(
          row, 1,
          new QTableWidgetItem(QString::fromStdString(std::string{core::artifact_kind_name(record.descriptor.kind)})));
      result_ui_->resultsTable->setItem(row, 2, new QTableWidgetItem(is_current ? "● 当前" : "! 已过期"));
      result_ui_->resultsTable->setItem(row, 3,
                                        new QTableWidgetItem(QString::fromStdString(
                                            record.descriptor.provenance.data_source_version_id.substr(0, 16))));
      result_ui_->resultsTable->setItem(row, 4,
                                        new QTableWidgetItem(QString::fromStdString(
                                            std::string{core::artifact_format_name(record.descriptor.format)})));
    }
    result_ui_->resultSummaryLabel->setText(QString("%1 个结果").arg(visible.size()));
    if (visible.empty()) {
      refresh_result_detail(-1);
    } else {
      result_ui_->resultsTable->setCurrentCell(0, 0);
      refresh_result_detail(0);
    }
  }

  void refresh_result_detail(int row) {
    const auto valid = row >= 0 && static_cast<std::size_t>(row) < visible_results_.size();
    result_ui_->locateResultButton->setEnabled(valid);
    result_ui_->exportResultButton->setEnabled(valid);
    if (!valid) {
      result_ui_->resultDetailTitleLabel->setText("未选择结果");
      result_ui_->resultDetailTypeValue->setText("—");
      result_ui_->resultDetailValidityValue->setText("—");
      result_ui_->resultDetailSourceValue->setText("—");
      result_ui_->resultDetailFormatValue->setText("—");
      return;
    }
    const auto& record = visible_results_[static_cast<std::size_t>(row)];
    const auto current = controller_.current_signal();
    const auto is_current =
        current && record.descriptor.provenance.data_source_version_id == current->fingerprint.version_id;
    result_ui_->resultDetailTitleLabel->setText(QString::fromStdString(record.descriptor.id));
    result_ui_->resultDetailTypeValue->setText(
        QString::fromStdString(std::string{core::artifact_kind_name(record.descriptor.kind)}));
    result_ui_->resultDetailValidityValue->setText(is_current ? "● 当前" : "! 已过期");
    result_ui_->resultDetailSourceValue->setText(
        QString::fromStdString(record.descriptor.provenance.data_source_version_id));
    result_ui_->resultDetailFormatValue->setText(
        QString::fromStdString(std::string{core::artifact_format_name(record.descriptor.format)}));
  }

  void export_selected_result() {
    const auto row = result_ui_->resultsTable->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= visible_results_.size()) {
      QMessageBox::information(native_window_, "导出结果", "请先选择一个结果。");
      return;
    }
    const auto destination = QFileDialog::getExistingDirectory(native_window_, "选择结果包导出目录");
    if (destination.isEmpty()) {
      return;
    }
    const auto& record = visible_results_[static_cast<std::size_t>(row)];
    auto exported = controller_.export_result(record, std::filesystem::path{destination.toStdWString()} /
                                                          std::filesystem::path{record.descriptor.id});
    if (!exported) {
      show_error(exported.error());
      return;
    }
    QMessageBox::information(native_window_, "导出完成",
                             QString("结果包已导出到：\n%1").arg(QString::fromStdWString(exported.value().wstring())));
  }

  [[nodiscard]] core::Result<ImportRequest> make_request_for_path(const std::filesystem::path& path,
                                                                  bool confirm_hints) const {
    if (path.extension() == L".wav" || path.extension() == L".WAV") {
      auto descriptor = data::read_wav_descriptor(path, true, data::ComponentOrder::iq);
      if (!descriptor) {
        return descriptor.error();
      }
      return ImportRequest{path, descriptor.value().descriptor, data::SourceFormat::wav, 8U * 1024U * 1024U,
                           512U * 1024U};
    }
    auto descriptor = make_confirmed_descriptor(path, parse_filename_hints(path), confirm_hints);
    if (!descriptor) {
      return descriptor.error();
    }
    return ImportRequest{path, descriptor.value(), data::SourceFormat::raw, 8U * 1024U * 1024U, 512U * 1024U};
  }

  void ensure_project_for_automation() {
    if (!controller_.project_path().empty()) {
      return;
    }
    const auto project =
        std::filesystem::temp_directory_path() / "signal-studio-ms04-preview" / "preview.signal-workspace";
    static_cast<void>(controller_.create_project(project, "ms04-preview"));
  }

  void set_import_error(const QString& message) {
    import_ui_->validationLabel->setStyleSheet("color:#FF6B7A;");
    import_ui_->validationLabel->setText(message);
  }

  void finish_progress_with_error(const core::Status& status) {
    if (progress_timer_) {
      progress_timer_->stop();
    }
    if (analysis_timer_) {
      analysis_timer_->stop();
    }
    if (progress_ui_) {
      progress_ui_->loadProgressBar->setRange(0, 100);
      progress_ui_->loadStatusLabel->setText(status_message(status));
      progress_ui_->loadStatusLabel->setStyleSheet("color:#FF6B7A;");
      progress_ui_->pauseResumeButton->setEnabled(false);
      progress_ui_->cancelLoadButton->setText("关闭");
      progress_ui_->cancelLoadButton->setEnabled(true);
      QObject::disconnect(progress_ui_->cancelLoadButton, nullptr, nullptr, nullptr);
      QObject::connect(progress_ui_->cancelLoadButton, &QPushButton::clicked, progress_dialog_, &QDialog::reject);
    } else {
      show_error(status);
    }
  }

  void show_error(const core::Status& status) const {
    QMessageBox box{QMessageBox::Critical, "Signal Studio 错误", status_message(status), QMessageBox::Ok,
                    native_window_};
    box.setDetailedText(QString::fromStdString(std::string{status.diagnostic()}));
    box.exec();
  }

  static void apply_dialog_style(QWidget* widget) {
    widget->setStyleSheet("QDialog{background:#08111F;color:#E5F1FF;font-family:'Microsoft YaHei UI';}"
                          "QLabel{color:#E5F1FF;} QLineEdit,QComboBox,QSpinBox,QDoubleSpinBox,QPlainTextEdit,"
                          "QTabWidget::pane{background:#0A1727;border:1px solid #29435E;min-height:28px;}"
                          "QGroupBox,QFrame#contentFrame,QFrame#sourceFrame{background:#0A1727;"
                          "border:1px solid #29435E;margin-top:6px;}"
                          "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;color:#B7CBE0;}"
                          "QLabel#wizardCodeLabel,QLabel#progressCodeLabel{color:#35CFE8;font-weight:600;}"
                          "QLabel#wizardTitleLabel,QLabel#progressTitleLabel{font-size:18px;font-weight:600;}"
                          "QPushButton{background:#10243A;border:1px solid #2B4867;padding:6px 12px;min-height:28px;}"
                          "QPushButton:hover{border-color:#20D3EE;} QProgressBar{border:1px solid #29435E;"
                          "background:#0A1727;text-align:center;} QProgressBar::chunk{background:#20D3EE;}"
                          "QTabBar::tab{background:#0A1727;border:1px solid #29435E;padding:7px 11px;}"
                          "QTabBar::tab:selected{border-top:2px solid #20D3EE;}");
  }

  struct AnalysisState final {
    std::mutex mutex;
    std::optional<AnalysisBundle> bundle;
    std::optional<core::Status> error;
  };

  struct PreviewState final {
    std::mutex mutex;
    std::optional<data::PreviewResult> preview;
    std::optional<core::Status> error;
  };

  QApplication& qt_application_;
  std::filesystem::path state_directory_;
  ApplicationController controller_;
  std::shared_ptr<workbench::CommandRegistry> commands_;
  std::shared_ptr<workbench::PanelRegistry> panels_;
  std::shared_ptr<RuntimeDiagnostics> diagnostics_;
  std::unique_ptr<workbench::IWorkbenchWindow> window_;
  std::unique_ptr<visualization::IAnalysisWorkspace> workspace_;
  visualization::IAnalysisWorkspace* primary_workspace_{};
  QWidget* workspace_native_{};
  QMainWindow* native_window_{};

  QWidget* home_page_{};
  QWidget* inspector_page_{};
  QWidget* result_page_{};
  std::unique_ptr<Ui::SignalStudioProjectHome> home_ui_;
  std::unique_ptr<Ui::SignalInspectorPage> inspector_ui_;
  std::unique_ptr<Ui::SignalResultCenterPage> result_ui_;
  std::unique_ptr<visualization::IAnalysisWorkspace> inspector_workspace_;
  std::vector<core::ArtifactRecord> visible_results_;

  QDialog* import_dialog_{};
  QDialog* progress_dialog_{};
  std::unique_ptr<Ui::SignalImportWizard> import_ui_;
  std::unique_ptr<Ui::SignalLoadProgressDialog> progress_ui_;
  QTimer* progress_timer_{};
  QTimer* analysis_timer_{};
  std::optional<ImportTask> active_import_;
  std::optional<task::TaskHandle> active_analysis_;
  std::shared_ptr<AnalysisState> analysis_state_;
  data::PreviewCoordinator preview_coordinator_;
  QTimer* preview_timer_{};
  std::optional<task::TaskHandle> preview_task_;
  std::shared_ptr<PreviewState> preview_state_;
};

QtApplication::QtApplication(QApplication& application, std::filesystem::path state_directory)
    : impl_(std::make_unique<Impl>(application, std::move(state_directory))) {}

QtApplication::~QtApplication() = default;

core::Status QtApplication::initialize() {
  return impl_->initialize();
}

core::Status QtApplication::prepare_automation_input(const std::filesystem::path& source) {
  return impl_->prepare_automation_input(source);
}

core::Status QtApplication::show_page(std::string_view page_id) {
  return impl_->show_page(page_id);
}

void QtApplication::show() {
  impl_->show();
}

void* QtApplication::native_handle() noexcept {
  return impl_->native_handle();
}

void* QtApplication::capture_handle() noexcept {
  return impl_->capture_handle();
}

core::Status QtApplication::save_screenshot(const std::filesystem::path& path) {
  return impl_->save_screenshot(path);
}

void QtApplication::show_import_wizard() {
  impl_->show_import_wizard();
}

void QtApplication::show_progress_preview(const std::filesystem::path& source) {
  impl_->show_progress_preview(source);
}

} // namespace signal::studio
