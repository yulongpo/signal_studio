#include "qt_application.hpp"

#include "ui_SignalAnalysisSettingsPanel.h"
#include "ui_SignalImportWizard.h"
#include "ui_SignalInspectorPage.h"
#include "ui_SignalLoadProgressDialog.h"
#include "ui_SignalResultCenterPage.h"
#include "ui_SignalStudioProjectHome.h"

#include "signal_studio/data/io.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
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
#include <vector>

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

[[nodiscard]] core::Result<std::vector<double>> parse_coefficients(const QString& text) {
  std::vector<double> values;
  if (text.trimmed().isEmpty()) {
    return values;
  }
  for (const auto& item : text.split(',', Qt::SkipEmptyParts)) {
    bool ok{};
    const auto value = item.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(value)) {
      return core::Status::failure({core::ErrorDomain::dsp, core::ErrorReason::invalid_argument},
                                   "自定义滤波系数必须为逗号分隔的有限数值");
    }
    values.push_back(value);
  }
  return values;
}

[[nodiscard]] std::uint64_t combo_length(const QComboBox* combo, bool allow_auto) {
  const auto text = combo->currentText().trimmed();
  if (allow_auto && text.compare("自动", Qt::CaseInsensitive) == 0) {
    return 0U;
  }
  bool ok{};
  const auto value = text.toULongLong(&ok);
  return ok ? value : 0U;
}

} // namespace

class QtApplication::Impl final {
public:
  enum class FormalCommitPhase : std::uint8_t { cancellable, canceling, committing, finalized };

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
    if (active_filter_preview_) {
      static_cast<void>(controller_.task_runtime().cancel(*active_filter_preview_));
    }
    if (active_import_) {
      static_cast<void>(active_import_->handle().cancel());
    }
    if (active_analysis_) {
      if (claim_analysis_cancellation()) {
        if (analysis_cancellation_) {
          analysis_cancellation_->store(true, std::memory_order_release);
        }
        static_cast<void>(controller_.task_runtime().cancel(*active_analysis_));
      }
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
    primary_workspace_->set_visibility_callback(
        [this](visualization::ChartKind kind, bool visible) { handle_chart_visibility_change(kind, visible); });

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
    if (const auto status = build_analysis_settings_panel(); !status) {
      return status;
    }
    if (const auto status = build_result_page(); !status) {
      return status;
    }
    refresh_all();
    return window_->show_page("p01");
  }

  [[nodiscard]] core::Status prepare_automation_input(const std::filesystem::path& source) {
    const auto session_nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto automation_session = state_directory_ / ("a-" + std::to_string(session_nonce));
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

  [[nodiscard]] core::Status validate_analysis_settings_panel() {
    if (analysis_settings_panel_ == nullptr || analysis_settings_ui_ == nullptr ||
        analysis_settings_panel_->parentWidget() == nullptr) {
      return ui_failure("Designer 分析参数面板未安装到 Workbench Inspector");
    }
    const std::array required_controls{
        "fftLengthCombo",
        "windowCombo",
        "estimatorCombo",
        "averagingCombo",
        "spectrumSmoothingCombo",
        "stftFftLengthCombo",
        "stftOverlapSpin",
        "stftWindowParameterSpin",
        "stftFrequencySmoothingCombo",
        "stftTimeSmoothingCombo",
        "prefilterEnabledCheck",
        "filterKindCombo",
        "referenceLevelSpin",
        "dynamicRangeSpin",
        "colorMapCombo",
        "interpolationCombo",
        "applyAnalysisSettingsButton",
        "applyPsdOnlyButton",
        "applyStftOnlyButton",
        "applySharedButton",
        "cancelAnalysisSettingsButton",
        "restoreViewDefaultsButton",
        "restoreSoftwareDefaultsButton",
        "savePresetButton",
        "deletePresetButton",
        "analysisPresetCombo",
    };
    for (const auto* name : required_controls) {
      if (analysis_settings_panel_->findChild<QWidget*>(name) == nullptr) {
        return ui_failure("分析参数面板缺少 Designer 控件", name);
      }
    }
    for (auto* combo : {analysis_settings_ui_->windowCombo, analysis_settings_ui_->stftWindowCombo}) {
      if (combo->count() != static_cast<int>(dsp::window_catalog().size())) {
        return ui_failure("窗函数选择器没有完整列出八种窗");
      }
      for (auto index = 0; index < combo->count(); ++index) {
        const auto help = combo->itemData(index, Qt::ToolTipRole).toString();
        if (!help.contains("CG=") || !help.contains("ENBW=") || !help.contains("用途：") || !help.contains("幅度：") ||
            !help.contains("泄漏：") || !help.contains('/')) {
          return ui_failure("窗函数帮助缺少中英文名、参数、CG、ENBW、用途或幅度/泄漏说明", std::to_string(index));
        }
      }
    }

    const auto original_settings = controller_.analysis_settings();
    const auto original_display = controller_.analysis_display_settings();
    const auto task_count = controller_.task_history().size();
    {
      QSignalBlocker reference_blocker{analysis_settings_ui_->referenceLevelSpin};
      QSignalBlocker range_blocker{analysis_settings_ui_->dynamicRangeSpin};
      QSignalBlocker color_blocker{analysis_settings_ui_->colorMapCombo};
      QSignalBlocker interpolation_blocker{analysis_settings_ui_->interpolationCombo};
      analysis_settings_ui_->referenceLevelSpin->setValue(-17.0);
      analysis_settings_ui_->dynamicRangeSpin->setValue(87.0);
      analysis_settings_ui_->colorMapCombo->setCurrentText("Viridis");
      analysis_settings_ui_->interpolationCombo->setCurrentText("linear");
    }
    apply_display_settings_only();
    const auto& display = controller_.analysis_display_settings();
    if (display.mapping.reference_level != -17.0 || display.mapping.dynamic_range != 87.0 ||
        display.mapping.color_map != "Viridis" || display.interpolation != "linear" ||
        controller_.task_history().size() != task_count) {
      set_analysis_panel_settings(original_settings, original_display);
      return ui_failure("纯显示参数更新错误地提交了 DSP 任务或未更新显示映射");
    }

    set_analysis_advanced_mode(true);
    if (analysis_settings_ui_->frameLengthSpin->isHidden() || analysis_settings_ui_->stftFftLengthCombo->isHidden()) {
      set_analysis_panel_settings(original_settings, original_display);
      return ui_failure("高级模式未显示完整 FFT/STFT 参数");
    }
    analysis_settings_ui_->frameLengthSpin->setValue(1024);
    analysis_settings_ui_->fftLengthCombo->setCurrentText("2048");
    analysis_settings_ui_->zeroPaddingCheck->setChecked(true);
    analysis_settings_ui_->stftFrameLengthSpin->setValue(256);
    analysis_settings_ui_->stftFftLengthCombo->setCurrentText("512");
    analysis_settings_ui_->stftPaddingCheck->setChecked(true);
    analysis_settings_ui_->windowCombo->setCurrentText("Kaiser");
    analysis_settings_ui_->windowParameterSpin->setValue(8.6);
    analysis_settings_ui_->stftWindowCombo->setCurrentText("Tukey");
    analysis_settings_ui_->stftWindowParameterSpin->setValue(0.25);
    analysis_settings_ui_->normalizationCombo->setCurrentIndex(2);
    analysis_settings_ui_->stftNormalizationCombo->setCurrentIndex(2);
    auto modeled = analysis_settings_from_panel();
    if (!modeled || modeled.value().spectrum.frame_length != 1024U || modeled.value().spectrum.fft_length != 2048U ||
        modeled.value().spectrogram.frame_length != 256U || modeled.value().spectrogram.fft_length != 512U ||
        modeled.value().spectrum.window.parameter != 8.6 || modeled.value().spectrogram.window.parameter != 0.25 ||
        modeled.value().spectrum.normalization != dsp::SpectrumNormalization::none ||
        modeled.value().spectrogram.normalization != dsp::SpectrumNormalization::none) {
      set_analysis_panel_settings(original_settings, original_display);
      return ui_failure("Designer 控件值未进入类型化分析参数模型");
    }
    analysis_settings_ui_->prefilterEnabledCheck->setChecked(true);
    analysis_settings_ui_->filterKindCombo->setCurrentIndex(0);
    analysis_settings_ui_->filterShapeCombo->setCurrentIndex(0);
    analysis_settings_ui_->filterOrderSpin->setValue(31);
    analysis_settings_ui_->filterHighSpin->setValue(1'000.0);
    update_analysis_derived_information();
    QElapsedTimer preview_deadline;
    preview_deadline.start();
    while (active_filter_preview_ && preview_deadline.elapsed() < 5'000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      QThread::msleep(1);
    }
    if (active_filter_preview_ ||
        !analysis_settings_ui_->filterPreviewLabel->text().startsWith("后台 ProcessingChain 预览")) {
      const auto preview_status = analysis_settings_ui_->filterPreviewLabel->text().toStdString();
      set_analysis_panel_settings(original_settings, original_display);
      return ui_failure("分析前滤波预览未在后台任务中生成真实频率响应与群时延", preview_status);
    }

    const auto imported = controller_.current_signal();
    if (!imported || !imported->loaded) {
      set_analysis_panel_settings(original_settings, original_display);
      return ui_failure("参数面板操作闭环验收要求真实自动化输入");
    }
    const auto wait_until_idle = [this](qint64 timeout_ms) {
      QElapsedTimer deadline;
      deadline.start();
      while (active_analysis_ && deadline.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
      }
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      return !active_analysis_;
    };
    const auto hash_matches = [](const dsp::AnalysisSettingsSnapshot& left,
                                 const dsp::AnalysisSettingsSnapshot& right) {
      const auto left_hash = dsp::hash_analysis_settings(left);
      const auto right_hash = dsp::hash_analysis_settings(right);
      return left_hash && right_hash && left_hash.value() == right_hash.value();
    };

    set_analysis_panel_settings(original_settings, original_display);
    const auto original_fft_text = analysis_settings_ui_->fftLengthCombo->currentText();
    analysis_settings_ui_->fftLengthCombo->setCurrentText(original_fft_text == "256" ? "512" : "256");
    analysis_settings_ui_->cancelAnalysisSettingsButton->click();
    if (analysis_settings_ui_->fftLengthCombo->currentText() != original_fft_text) {
      return ui_failure("取消按钮没有丢弃未应用的参数修改");
    }

    analysis_settings_ui_->referenceLevelSpin->setValue(-33.0);
    analysis_settings_ui_->restoreViewDefaultsButton->click();
    if (analysis_settings_ui_->referenceLevelSpin->value() != -33.0) {
      return ui_failure("恢复视图默认错误覆盖了当前显示参数");
    }
    analysis_settings_ui_->restoreSoftwareDefaultsButton->click();
    if (analysis_settings_ui_->referenceLevelSpin->value() != AnalysisDisplaySettings{}.mapping.reference_level) {
      return ui_failure("恢复软件默认没有恢复显示参数");
    }
    static_cast<void>(controller_.set_analysis_display_settings(original_display));

    auto requested = original_settings;
    requested.spectrum.analysis_range_policy = dsp::AnalysisRangePolicy::all_complete_frames;
    requested.spectrum.frame_length = 256U;
    requested.spectrum.fft_length = 256U;
    requested.spectrum.zero_padding_policy = dsp::ZeroPaddingPolicy::forbidden;
    requested.spectrum.estimator = {dsp::PsdEstimatorKind::periodogram, 0.0, 1U};
    requested.spectrum.accumulation = {};
    requested.spectrum.smoothing = {};
    requested.spectrogram.frame_length = 128U;
    requested.spectrogram.fft_length = 128U;
    requested.spectrogram.hop_length = 64U;
    requested.spectrogram.padding_policy = dsp::ZeroPaddingPolicy::forbidden;
    requested.spectrogram.boundary_policy = dsp::SpectrogramBoundaryPolicy::drop_incomplete;
    requested.spectrogram.smoothing = {};
    requested.prefilter.enabled = false;
    requested.prefilter.chain.nodes.clear();
    set_analysis_panel_settings(requested, original_display);
    auto all_expected = analysis_settings_from_panel();
    if (!all_expected) {
      return all_expected.error();
    }
    analysis_settings_ui_->applyAnalysisSettingsButton->click();
    if (!wait_until_idle(30'000) || !controller_.current_analysis() ||
        !hash_matches(controller_.current_analysis()->settings, all_expected.value())) {
      return ui_failure("全部应用按钮没有提交匹配的真实 PSD/STFT 参数结果");
    }

    const auto before_psd_only = controller_.current_analysis()->settings;
    auto psd_panel = before_psd_only;
    psd_panel.spectrum.window = {dsp::WindowKind::hamming, 0.0};
    psd_panel.spectrogram.window = {dsp::WindowKind::kaiser, 8.6};
    set_analysis_panel_settings(psd_panel, original_display);
    analysis_settings_ui_->applyPsdOnlyButton->click();
    if (!wait_until_idle(30'000) || !controller_.current_analysis() ||
        controller_.current_analysis()->settings.spectrum.window.kind != dsp::WindowKind::hamming ||
        controller_.current_analysis()->settings.spectrogram.window != before_psd_only.spectrogram.window) {
      return ui_failure("仅应用 PSD 按钮错误修改了 STFT 或未修改频谱");
    }

    const auto before_stft_only = controller_.current_analysis()->settings;
    auto stft_panel = before_stft_only;
    stft_panel.spectrum.window = {dsp::WindowKind::blackman, 0.0};
    stft_panel.spectrogram.window = {dsp::WindowKind::tukey, 0.25};
    set_analysis_panel_settings(stft_panel, original_display);
    analysis_settings_ui_->applyStftOnlyButton->click();
    if (!wait_until_idle(30'000) || !controller_.current_analysis() ||
        controller_.current_analysis()->settings.spectrum.window != before_stft_only.spectrum.window ||
        controller_.current_analysis()->settings.spectrogram.window.kind != dsp::WindowKind::tukey) {
      return ui_failure("仅应用 STFT 按钮错误修改了频谱或未修改时频参数");
    }

    auto shared_panel = controller_.current_analysis()->settings;
    shared_panel.spectrum.frame_length = 256U;
    shared_panel.spectrum.fft_length = 256U;
    shared_panel.spectrum.window = {dsp::WindowKind::hann, 0.0};
    shared_panel.spectrogram.frame_length = 128U;
    shared_panel.spectrogram.fft_length = 128U;
    shared_panel.spectrogram.window = {dsp::WindowKind::tukey, 0.5};
    set_analysis_panel_settings(shared_panel, original_display);
    analysis_settings_ui_->applySharedButton->click();
    if (!wait_until_idle(30'000) || !controller_.current_analysis()) {
      return ui_failure("应用共享参数按钮未完成真实异步分析");
    }
    const auto shared_result = controller_.current_analysis()->settings;
    if (shared_result.spectrum.frame_length != shared_result.spectrogram.frame_length ||
        shared_result.spectrum.fft_length != shared_result.spectrogram.fft_length ||
        shared_result.spectrum.window != shared_result.spectrogram.window) {
      return ui_failure("应用共享参数没有把帧长、FFT 和窗函数真实同步到 PSD/STFT");
    }

    auto hold_panel = shared_result;
    hold_panel.spectrum.accumulation.mode = dsp::SpectrumAccumulationMode::maximum_hold;
    hold_panel.spectrum.accumulation.hold_reset_generation = 11U;
    set_analysis_panel_settings(hold_panel, original_display);
    if (!analysis_settings_ui_->resetMaximumHoldButton->isEnabled()) {
      return ui_failure("最大保持复位控件未随累积模式启用");
    }
    analysis_settings_ui_->resetMaximumHoldButton->click();
    if (!wait_until_idle(30'000) || !controller_.current_analysis() ||
        controller_.current_analysis()->settings.spectrum.accumulation.hold_reset_generation != 12U) {
      return ui_failure("最大保持复位按钮未真实递增 generation 并提交后台重算");
    }

    const auto display_tasks_before = controller_.task_history().size();
    const auto settings_before_axis_apply = controller_.analysis_settings();
    analysis_settings_ui_->frequencyAxisModeCombo->setCurrentText("baseband");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    analysis_settings_ui_->frequencyAxisModeCombo->setCurrentText("absolute-if-available");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    analysis_settings_ui_->applyAnalysisSettingsButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (controller_.analysis_display_settings().frequency_axis_mode != "absolute-if-available" ||
        controller_.task_history().size() != display_tasks_before || active_analysis_ ||
        !hash_matches(controller_.analysis_settings(), settings_before_axis_apply)) {
      return ui_failure("频率轴显示往返后点击应用错误触发 DSP、修改分析设置或未保存最终显示模式");
    }

    set_analysis_panel_settings(shared_result, original_display);
    const auto tasks_before_invalid = controller_.task_history().size();
    analysis_settings_ui_->frameLengthSpin->setValue(512);
    analysis_settings_ui_->fftLengthCombo->setCurrentText("256");
    analysis_settings_ui_->zeroPaddingCheck->setChecked(false);
    analysis_settings_ui_->applyAnalysisSettingsButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    if (active_analysis_ || controller_.task_history().size() != tasks_before_invalid ||
        analysis_settings_ui_->parameterWarningLabel->text().isEmpty()) {
      return ui_failure("非法参数没有在 UI 层阻止任务提交");
    }

    set_analysis_panel_settings(shared_result, original_display);
    constexpr auto preset_name = "MS45 UI smoke preset";
    QTimer::singleShot(0, native_window_, [] {
      for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* dialog = qobject_cast<QInputDialog*>(widget)) {
          dialog->setTextValue("MS45 UI smoke preset");
          dialog->accept();
          return;
        }
      }
    });
    analysis_settings_ui_->savePresetButton->click();
    if (!controller_.user_analysis_presets().contains(preset_name)) {
      return ui_failure("保存预设按钮没有写入当前工程");
    }
    const auto project = controller_.project_path();
    if (project.empty() || !controller_.open_project(project) ||
        !controller_.user_analysis_presets().contains(preset_name)) {
      return ui_failure("工程关闭/重开后用户预设没有恢复");
    }
    refresh_analysis_presets();
    const auto preset_index = analysis_settings_ui_->analysisPresetCombo->findData(QString::fromUtf8("user:") +
                                                                                   QString::fromUtf8(preset_name));
    if (preset_index < 0) {
      return ui_failure("重开工程后用户预设没有恢复到 Designer 控件");
    }
    analysis_settings_ui_->analysisPresetCombo->setCurrentIndex(preset_index);
    analysis_settings_ui_->deletePresetButton->click();
    if (controller_.user_analysis_presets().contains(preset_name)) {
      return ui_failure("删除预设按钮没有从工程移除用户预设");
    }

    set_analysis_panel_settings(original_settings, original_display);
    set_analysis_advanced_mode(false);
    return core::Status::success();
  }

  [[nodiscard]] core::Status validate_analysis_runtime() {
    const auto imported = controller_.current_signal();
    if (!imported || !imported->loaded || primary_workspace_ == nullptr) {
      return ui_failure("异步分析运行时验收要求先通过 --input 准备真实信号");
    }
    const auto wait_until_idle = [this](qint64 timeout_ms) {
      QElapsedTimer deadline;
      deadline.start();
      while (active_analysis_ && deadline.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
      }
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      return !active_analysis_;
    };

    commit_after_analysis_ = true;
    parameter_recompute_ = false;
    begin_analysis(*imported, controller_.analysis_settings(), AnalysisViewSelection{true, true});
    if (!wait_until_idle(30'000)) {
      return ui_failure("正式分析任务未在 30 秒内完成");
    }
    const auto formal_analysis = controller_.current_analysis();
    if (!formal_analysis) {
      return ui_failure("异步分析运行时验收缺少正式分析结果");
    }
    const auto formal_history = controller_.task_history();
    const auto formal_status = std::ranges::find_if(formal_history, [&](const task::TaskStatus& status) {
      return status.task_id.value == formal_analysis->task_id;
    });
    if (formal_status == formal_history.end() || formal_status->state != task::TaskState::completed) {
      return ui_failure("正式分析任务未通过 TaskRuntime 原子完成门禁", formal_analysis->task_id);
    }
    auto artifacts = controller_.results();
    if (!artifacts) {
      return artifacts.error();
    }
    const auto formal_artifact = std::ranges::find_if(artifacts.value(), [&](const core::ArtifactRecord& artifact) {
      return artifact.descriptor.provenance.task_id == formal_analysis->task_id;
    });
    if (formal_artifact == artifacts.value().end()) {
      return ui_failure("正式分析任务没有对应的 Artifact provenance", formal_analysis->task_id);
    }
    const std::array expected_artifact_files{formal_artifact->payload_path,
                                             formal_artifact->package_path / "manifest.json",
                                             formal_artifact->package_path / ".artifact-index"};
    if (formal_status->committed_artifacts.size() != expected_artifact_files.size()) {
      return ui_failure("TaskRuntime 未登记完整 Artifact 文件集合", formal_analysis->task_id);
    }
    for (const auto& expected_path : expected_artifact_files) {
      const auto record = std::ranges::find_if(formal_status->committed_artifacts, [&](const auto& candidate) {
        return candidate.path.lexically_normal() == expected_path.lexically_normal();
      });
      if (record == formal_status->committed_artifacts.end()) {
        return ui_failure("TaskRuntime 制品集合缺少 Artifact 文件", expected_path.string());
      }
      auto digest = core::hash_file(record->path);
      std::error_code file_error;
      const auto file_size = std::filesystem::file_size(record->path, file_error);
      if (!digest || digest.value().hex() != record->sha256_digest || file_error || file_size != record->size_bytes ||
          record->size_bytes == 0U) {
        return ui_failure("TaskRuntime Artifact 文件 checksum 或大小无效", record->path.string());
      }
    }
    {
      ApplicationController restarted{state_directory_};
      const auto recovered_history = restarted.task_history();
      if (std::ranges::none_of(recovered_history, [&](const task::TaskStatus& status) {
            return status.task_id.value == formal_analysis->task_id && status.state == task::TaskState::completed &&
                   status.committed_artifacts == formal_status->committed_artifacts;
          })) {
        return ui_failure("重启后无法从 TaskRuntime journal 追溯正式分析制品及 checksum", formal_analysis->task_id);
      }
    }

    auto canceled_settings = controller_.analysis_settings();
    canceled_settings.spectrum.smoothing = {dsp::SpectrumSmoothingKind::gaussian, 7U, 1.75, 0U};
    const auto analysis_before_cancel = controller_.current_analysis();
    begin_analysis(*imported, canceled_settings, AnalysisViewSelection{true, true});
    const auto explicitly_canceled_task = active_analysis_;
    if (!explicitly_canceled_task) {
      return ui_failure("显式取消验收未创建可取消的分析任务");
    }
    cancel_active_analysis_from_ui();
    if (!wait_until_idle(30'000)) {
      return ui_failure("显式取消的分析任务未在 30 秒内终止");
    }
    const auto analysis_after_cancel = controller_.current_analysis();
    const auto canceled_status = controller_.task_runtime().status(*explicitly_canceled_task);
    if (!analysis_before_cancel || !analysis_after_cancel ||
        analysis_before_cancel->task_id != analysis_after_cancel->task_id || !canceled_status ||
        (canceled_status.value().state != task::TaskState::canceled &&
         canceled_status.value().state != task::TaskState::stale) ||
        !canceled_status.value().committed_artifacts.empty()) {
      return ui_failure("显式取消后仍发布了新分析或 TaskRuntime 制品");
    }

    auto final_settings = controller_.analysis_settings();
    std::vector<double> submission_ms;
    submission_ms.reserve(10U);
    for (std::size_t index = 0; index < 10U; ++index) {
      final_settings.spectrum.smoothing = {dsp::SpectrumSmoothingKind::gaussian, 5U,
                                           0.55 + 0.05 * static_cast<double>(index), 0U};
      const auto started = std::chrono::steady_clock::now();
      begin_analysis(*imported, final_settings, AnalysisViewSelection{true, true});
      submission_ms.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    }
    if (!wait_until_idle(30'000)) {
      return ui_failure("快速参数切换后最新异步分析未在 30 秒内完成");
    }
    auto expected_hash = dsp::hash_analysis_settings(final_settings);
    const auto latest = controller_.current_analysis();
    if (!expected_hash || !latest || latest->settings_hash != expected_hash.value()) {
      return ui_failure("快速参数切换后提交的不是最新参数结果");
    }
    std::ranges::sort(submission_ms);
    const auto p95_index = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(submission_ms.size())) - 1.0);
    const auto p95_submission_ms = submission_ms.at(p95_index);
    if (p95_submission_ms > 50.0) {
      return ui_failure("普通参数切换的 UI 提交 P95 超过 50 ms", std::to_string(p95_submission_ms) + " ms");
    }

    begin_analysis(*imported, final_settings, AnalysisViewSelection{true, true});
    const auto hide_started = std::chrono::steady_clock::now();
    if (const auto hidden = primary_workspace_->set_chart_visible(visualization::ChartKind::spectrogram, false);
        !hidden) {
      return hidden;
    }
    const auto hide_resubmit_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hide_started).count();
    if (hide_resubmit_ms > 500.0) {
      return ui_failure("隐藏 STFT 未在 500 ms 门禁内取消并降低计算", std::to_string(hide_resubmit_ms) + " ms");
    }
    if (!wait_until_idle(30'000)) {
      return ui_failure("隐藏 STFT 后的频谱专属任务未在 30 秒内完成");
    }
    const auto hidden_result = controller_.current_analysis();
    if (!hidden_result || hidden_result->psd.values.empty() || !hidden_result->stft.values.empty() ||
        !hidden_result->frame.stft_db.empty()) {
      return ui_failure("隐藏 STFT 后仍计算或提交了时频矩阵");
    }

    if (const auto shown = primary_workspace_->set_chart_visible(visualization::ChartKind::spectrogram, true); !shown) {
      return shown;
    }
    if (!wait_until_idle(30'000)) {
      return ui_failure("恢复 STFT 后的最新异步任务未在 30 秒内完成");
    }
    const auto restored_result = controller_.current_analysis();
    if (!restored_result || restored_result->stft.values.empty()) {
      return ui_failure("恢复 STFT 可见性后未重新计算时频结果");
    }
    const auto history = controller_.task_history();
    const auto superseded = std::ranges::count_if(history, [](const task::TaskStatus& status) {
      return status.state == task::TaskState::canceled || status.state == task::TaskState::stale;
    });
    std::printf("MS45_UI_RUNTIME p95_submit_ms=%.3f hide_resubmit_ms=%.3f superseded_tasks=%lld\n", p95_submission_ms,
                hide_resubmit_ms, static_cast<long long>(superseded));
    return core::Status::success();
  }

  void show_advanced_analysis_settings(bool advanced) {
    set_analysis_advanced_mode(advanced);
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
                [this] { return static_cast<bool>(controller_.current_analysis()); },
                [this] {
                  const auto current = controller_.current_analysis();
                  if (!current) {
                    return ui_failure("当前没有可提交的分析结果");
                  }
                  auto result = controller_.commit_measurement(*current);
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

  [[nodiscard]] core::Status build_analysis_settings_panel() {
    analysis_settings_panel_ = new QWidget(native_window_);
    analysis_settings_ui_ = std::make_unique<Ui::SignalAnalysisSettingsPanel>();
    analysis_settings_ui_->setupUi(analysis_settings_panel_);
    analysis_settings_panel_->setObjectName("SignalAnalysisSettingsPanel");
    analysis_settings_ui_->analysisSettingsHeading->setObjectName("SectionHeading");
    analysis_settings_ui_->parameterWarningLabel->setStyleSheet("color:#E6B95C;");
    analysis_settings_ui_->presetDescriptionLabel->setStyleSheet("color:#8FA8C2;");
    analysis_settings_ui_->derivedInformationLabel->setStyleSheet("color:#B7CBE0;");
    const auto install_window_help = [](QComboBox* combo) {
      const auto catalog = dsp::window_catalog();
      for (std::size_t index = 0; index < catalog.size() && index < static_cast<std::size_t>(combo->count()); ++index) {
        const auto& descriptor = catalog[index];
        auto parameter = descriptor.parameter_name.empty()
                             ? QString("无参数")
                             : QString("%1 ∈ [%2, %3]，推荐 %4")
                                   .arg(QString::fromUtf8(descriptor.parameter_name.data(),
                                                          static_cast<qsizetype>(descriptor.parameter_name.size())))
                                   .arg(descriptor.parameter_minimum)
                                   .arg(descriptor.parameter_maximum)
                                   .arg(descriptor.recommended_parameter);
        const auto tooltip =
            QString("%1 / %2\n%3\n参考 CG=%4，ENBW=%5 bins\n用途：%6\n幅度：%7\n泄漏：%8")
                .arg(QString::fromUtf8(descriptor.chinese_name.data(),
                                       static_cast<qsizetype>(descriptor.chinese_name.size())))
                .arg(QString::fromUtf8(descriptor.english_name.data(),
                                       static_cast<qsizetype>(descriptor.english_name.size())))
                .arg(parameter)
                .arg(descriptor.reference_coherent_gain, 0, 'g', 6)
                .arg(descriptor.reference_enbw_bins, 0, 'g', 6)
                .arg(QString::fromUtf8(descriptor.recommended_use.data(),
                                       static_cast<qsizetype>(descriptor.recommended_use.size())))
                .arg(QString::fromUtf8(descriptor.amplitude_characteristics.data(),
                                       static_cast<qsizetype>(descriptor.amplitude_characteristics.size())))
                .arg(QString::fromUtf8(descriptor.leakage_characteristics.data(),
                                       static_cast<qsizetype>(descriptor.leakage_characteristics.size())));
        combo->setItemData(static_cast<int>(index), tooltip, Qt::ToolTipRole);
      }
    };
    install_window_help(analysis_settings_ui_->windowCombo);
    install_window_help(analysis_settings_ui_->stftWindowCombo);
    if (const auto status = window_->install_inspector_extension("signal-analysis-settings", analysis_settings_panel_);
        !status) {
      return status;
    }

    derived_settings_timer_ = new QTimer(analysis_settings_panel_);
    derived_settings_timer_->setSingleShot(true);
    derived_settings_timer_->setInterval(250);
    QObject::connect(derived_settings_timer_, &QTimer::timeout, analysis_settings_panel_,
                     [this] { update_analysis_derived_information(); });
    const auto queue_derived = [this] { derived_settings_timer_->start(); };
    for (auto* combo : analysis_settings_panel_->findChildren<QComboBox*>()) {
      QObject::connect(combo, &QComboBox::currentTextChanged, analysis_settings_panel_,
                       [queue_derived](const QString&) { queue_derived(); });
    }
    for (auto* spin : analysis_settings_panel_->findChildren<QSpinBox*>()) {
      QObject::connect(spin, &QSpinBox::valueChanged, analysis_settings_panel_,
                       [queue_derived](int) { queue_derived(); });
    }
    for (auto* spin : analysis_settings_panel_->findChildren<QDoubleSpinBox*>()) {
      QObject::connect(spin, &QDoubleSpinBox::valueChanged, analysis_settings_panel_,
                       [queue_derived](double) { queue_derived(); });
    }
    for (auto* check : analysis_settings_panel_->findChildren<QCheckBox*>()) {
      QObject::connect(check, &QCheckBox::toggled, analysis_settings_panel_,
                       [queue_derived](bool) { queue_derived(); });
    }
    for (auto* edit : analysis_settings_panel_->findChildren<QLineEdit*>()) {
      QObject::connect(edit, &QLineEdit::textChanged, analysis_settings_panel_,
                       [queue_derived](const QString&) { queue_derived(); });
    }
    QObject::connect(analysis_settings_ui_->settingsModeCombo, &QComboBox::currentIndexChanged,
                     analysis_settings_panel_, [this](int index) { set_analysis_advanced_mode(index == 1); });
    QObject::connect(analysis_settings_ui_->filterKindCombo, &QComboBox::currentIndexChanged, analysis_settings_panel_,
                     [this](int index) {
                       const auto builtin_iir = index == 1;
                       if (builtin_iir) {
                         analysis_settings_ui_->filterOrderSpin->setValue(2);
                       }
                       analysis_settings_ui_->filterOrderSpin->setEnabled(!builtin_iir);
                       analysis_settings_ui_->filterOrderSpin->setToolTip(
                           builtin_iir ? "现有成熟 IIR 内核为可验证二阶节，阶数固定为 2。" : QString{});
                     });
    QObject::connect(analysis_settings_ui_->outputQuantityCombo, &QComboBox::currentIndexChanged,
                     analysis_settings_panel_, [this](int index) {
                       if (index == 0 || index == 3) {
                         analysis_settings_ui_->normalizationCombo->setCurrentIndex(0);
                       } else {
                         analysis_settings_ui_->normalizationCombo->setCurrentIndex(1);
                       }
                     });
    QObject::connect(analysis_settings_ui_->stftOutputQuantityCombo, &QComboBox::currentIndexChanged,
                     analysis_settings_panel_, [this](int index) {
                       if (index == 0 || index == 3) {
                         analysis_settings_ui_->stftNormalizationCombo->setCurrentIndex(0);
                       } else {
                         analysis_settings_ui_->stftNormalizationCombo->setCurrentIndex(1);
                       }
                     });
    QObject::connect(analysis_settings_ui_->analysisPresetCombo, &QComboBox::activated, analysis_settings_panel_,
                     [this](int index) { apply_selected_analysis_preset(index); });
    QObject::connect(analysis_settings_ui_->applyAnalysisSettingsButton, &QPushButton::clicked,
                     analysis_settings_panel_, [this] { apply_analysis_settings_from_panel(0); });
    QObject::connect(analysis_settings_ui_->applyPsdOnlyButton, &QPushButton::clicked, analysis_settings_panel_,
                     [this] { apply_analysis_settings_from_panel(1); });
    QObject::connect(analysis_settings_ui_->applyStftOnlyButton, &QPushButton::clicked, analysis_settings_panel_,
                     [this] { apply_analysis_settings_from_panel(2); });
    QObject::connect(analysis_settings_ui_->applySharedButton, &QPushButton::clicked, analysis_settings_panel_, [this] {
      analysis_settings_ui_->stftFrameLengthSpin->setValue(analysis_settings_ui_->frameLengthSpin->value());
      analysis_settings_ui_->stftFftLengthCombo->setCurrentText(analysis_settings_ui_->fftLengthCombo->currentText());
      analysis_settings_ui_->stftWindowCombo->setCurrentIndex(analysis_settings_ui_->windowCombo->currentIndex());
      analysis_settings_ui_->stftWindowParameterSpin->setValue(analysis_settings_ui_->windowParameterSpin->value());
      apply_analysis_settings_from_panel(0);
    });
    QObject::connect(analysis_settings_ui_->averagingCombo, &QComboBox::currentIndexChanged, analysis_settings_panel_,
                     [this](int index) {
                       analysis_settings_ui_->resetMaximumHoldButton->setEnabled(
                           index == static_cast<int>(dsp::SpectrumAccumulationMode::maximum_hold));
                     });
    QObject::connect(analysis_settings_ui_->resetMaximumHoldButton, &QPushButton::clicked, analysis_settings_panel_,
                     [this] {
                       ++hold_reset_generation_;
                       apply_analysis_settings_from_panel(0);
                     });
    QObject::connect(
        analysis_settings_ui_->cancelAnalysisSettingsButton, &QPushButton::clicked, analysis_settings_panel_, [this] {
          set_analysis_panel_settings(controller_.analysis_settings(), controller_.analysis_display_settings());
        });
    QObject::connect(analysis_settings_ui_->restoreViewDefaultsButton, &QPushButton::clicked, analysis_settings_panel_,
                     [this] {
                       if (controller_.current_signal()) {
                         const auto presets = controller_.built_in_analysis_presets(
                             *controller_.current_signal(), analysis_settings_ui_->backendCombo->currentIndex() == 0);
                         set_analysis_panel_settings(presets.at(1U).settings, controller_.analysis_display_settings());
                       }
                     });
    QObject::connect(analysis_settings_ui_->restoreSoftwareDefaultsButton, &QPushButton::clicked,
                     analysis_settings_panel_, [this] {
                       AnalysisDisplaySettings display;
                       if (controller_.current_signal()) {
                         const auto presets = controller_.built_in_analysis_presets(
                             *controller_.current_signal(), analysis_settings_ui_->backendCombo->currentIndex() == 0);
                         set_analysis_panel_settings(presets.at(1U).settings, display);
                       } else {
                         set_analysis_panel_settings({}, display);
                       }
                     });
    QObject::connect(analysis_settings_ui_->savePresetButton, &QPushButton::clicked, analysis_settings_panel_,
                     [this] { save_analysis_preset(); });
    QObject::connect(analysis_settings_ui_->deletePresetButton, &QPushButton::clicked, analysis_settings_panel_,
                     [this] { delete_analysis_preset(); });
    const auto display_only = [this] { apply_display_settings_only(); };
    QObject::connect(analysis_settings_ui_->referenceLevelSpin, &QDoubleSpinBox::valueChanged, analysis_settings_panel_,
                     [display_only](double) { display_only(); });
    QObject::connect(analysis_settings_ui_->dynamicRangeSpin, &QDoubleSpinBox::valueChanged, analysis_settings_panel_,
                     [display_only](double) { display_only(); });
    QObject::connect(analysis_settings_ui_->colorMapCombo, &QComboBox::currentTextChanged, analysis_settings_panel_,
                     [display_only](const QString&) { display_only(); });
    QObject::connect(analysis_settings_ui_->interpolationCombo, &QComboBox::currentTextChanged,
                     analysis_settings_panel_, [display_only](const QString&) { display_only(); });
    QObject::connect(analysis_settings_ui_->frequencyAxisModeCombo, &QComboBox::currentTextChanged,
                     analysis_settings_panel_, [display_only](const QString&) { display_only(); });
    QObject::connect(analysis_settings_ui_->amplitudeScaleCombo, &QComboBox::currentTextChanged,
                     analysis_settings_panel_, [display_only](const QString&) { display_only(); });
    QObject::connect(analysis_settings_ui_->rangeModeCombo, &QComboBox::currentTextChanged, analysis_settings_panel_,
                     [display_only](const QString&) { display_only(); });
    QObject::connect(analysis_settings_ui_->displayMinimumSpin, &QDoubleSpinBox::valueChanged, analysis_settings_panel_,
                     [display_only](double) { display_only(); });
    QObject::connect(analysis_settings_ui_->displayMaximumSpin, &QDoubleSpinBox::valueChanged, analysis_settings_panel_,
                     [display_only](double) { display_only(); });

    refresh_analysis_presets();
    set_analysis_panel_settings(controller_.analysis_settings(), controller_.analysis_display_settings());
    set_analysis_advanced_mode(false);
    update_analysis_derived_information();
    return core::Status::success();
  }

  void set_analysis_advanced_mode(bool advanced) {
    const std::array<QWidget*, 50U> advanced_widgets{
        analysis_settings_ui_->frameLengthLabel,
        analysis_settings_ui_->frameLengthSpin,
        analysis_settings_ui_->windowParameterLabel,
        analysis_settings_ui_->windowParameterSpin,
        analysis_settings_ui_->welchOverlapLabel,
        analysis_settings_ui_->welchOverlapSpin,
        analysis_settings_ui_->welchSegmentsLabel,
        analysis_settings_ui_->welchSegmentsSpin,
        analysis_settings_ui_->averagingCountLabel,
        analysis_settings_ui_->averagingCountSpin,
        analysis_settings_ui_->exponentialAlphaLabel,
        analysis_settings_ui_->exponentialAlphaSpin,
        analysis_settings_ui_->smoothingWindowLabel,
        analysis_settings_ui_->smoothingWindowSpin,
        analysis_settings_ui_->gaussianSigmaLabel,
        analysis_settings_ui_->gaussianSigmaSpin,
        analysis_settings_ui_->savgolOrderLabel,
        analysis_settings_ui_->savgolOrderSpin,
        analysis_settings_ui_->zeroPaddingLabel,
        analysis_settings_ui_->zeroPaddingCheck,
        analysis_settings_ui_->sidednessLabel,
        analysis_settings_ui_->sidednessCombo,
        analysis_settings_ui_->normalizationLabel,
        analysis_settings_ui_->normalizationCombo,
        analysis_settings_ui_->detrendLabel,
        analysis_settings_ui_->detrendCombo,
        analysis_settings_ui_->measurementSourceLabel,
        analysis_settings_ui_->measurementSourceCombo,
        analysis_settings_ui_->stftFrameLengthLabel,
        analysis_settings_ui_->stftFrameLengthSpin,
        analysis_settings_ui_->stftFftLengthLabel,
        analysis_settings_ui_->stftFftLengthCombo,
        analysis_settings_ui_->stftWindowLabel,
        analysis_settings_ui_->stftWindowCombo,
        analysis_settings_ui_->stftWindowParameterLabel,
        analysis_settings_ui_->stftWindowParameterSpin,
        analysis_settings_ui_->stftPaddingLabel,
        analysis_settings_ui_->stftPaddingCheck,
        analysis_settings_ui_->stftBoundaryLabel,
        analysis_settings_ui_->stftBoundaryCombo,
        analysis_settings_ui_->stftOutputQuantityLabel,
        analysis_settings_ui_->stftOutputQuantityCombo,
        analysis_settings_ui_->stftNormalizationLabel,
        analysis_settings_ui_->stftNormalizationCombo,
        analysis_settings_ui_->stftDetrendLabel,
        analysis_settings_ui_->stftDetrendCombo,
        analysis_settings_ui_->customNumeratorLabel,
        analysis_settings_ui_->customNumeratorEdit,
        analysis_settings_ui_->customDenominatorLabel,
        analysis_settings_ui_->customDenominatorEdit,
    };
    for (auto* widget : advanced_widgets) {
      widget->setVisible(advanced);
    }
  }

  void refresh_analysis_presets() {
    if (!analysis_settings_ui_) {
      return;
    }
    QSignalBlocker blocker{analysis_settings_ui_->analysisPresetCombo};
    analysis_settings_ui_->analysisPresetCombo->clear();
    static const std::array<std::pair<const char*, const char*>, 6U> builtins{{
        {"快速预览", "quick-preview"},
        {"平衡分析", "balanced-analysis"},
        {"高分辨率", "high-resolution"},
        {"低噪声 PSD", "low-noise-psd"},
        {"突发信号", "burst-signal"},
        {"窄带精细分析", "narrowband-fine"},
    }};
    for (const auto& [name, id] : builtins) {
      analysis_settings_ui_->analysisPresetCombo->addItem(name, id);
    }
    for (const auto& [name, settings] : controller_.user_analysis_presets()) {
      static_cast<void>(settings);
      analysis_settings_ui_->analysisPresetCombo->addItem(QString("用户 · %1").arg(QString::fromStdString(name)),
                                                          QString("user:%1").arg(QString::fromStdString(name)));
    }
    analysis_settings_ui_->analysisPresetCombo->setCurrentIndex(1);
  }

  void apply_selected_analysis_preset(int index) {
    const auto current_signal = controller_.current_signal();
    if (index < 0 || !current_signal) {
      return;
    }
    const auto prefer_cuda = analysis_settings_ui_->backendCombo->currentIndex() == 0;
    const auto viewport_samples =
        primary_workspace_ ? primary_workspace_->viewport().time_viewport.size() : std::uint64_t{};
    const auto id = analysis_settings_ui_->analysisPresetCombo->itemData(index).toString();
    if (id.startsWith("user:")) {
      const auto presets = controller_.user_analysis_presets();
      const auto name = id.sliced(5).toStdString();
      if (const auto found = presets.find(name); found != presets.end()) {
        set_analysis_panel_settings(found->second, controller_.analysis_display_settings());
        static_cast<void>(controller_.set_active_analysis_preset("user:" + name, found->second));
        analysis_settings_ui_->presetDescriptionLabel->setText("用户工程预设；完整参数快照会参与哈希和工程持久化。");
      }
      return;
    }
    const auto presets = controller_.built_in_analysis_presets(*current_signal, prefer_cuda, viewport_samples);
    const auto found =
        std::ranges::find_if(presets, [&](const AnalysisPreset& preset) { return preset.id == id.toStdString(); });
    if (found != presets.end()) {
      set_analysis_panel_settings(found->settings, controller_.analysis_display_settings());
      static_cast<void>(controller_.set_active_analysis_preset(found->id, found->settings));
      analysis_settings_ui_->presetDescriptionLabel->setText(QString::fromStdString(found->description));
    }
  }

  void set_analysis_panel_settings(const dsp::AnalysisSettingsSnapshot& settings,
                                   const AnalysisDisplaySettings& display) {
    if (!analysis_settings_ui_) {
      return;
    }
    analysis_panel_loading_ = true;
    analysis_settings_ui_->frameLengthSpin->setValue(static_cast<int>(std::clamp<std::uint64_t>(
        settings.spectrum.frame_length == 0U ? 16'384U : settings.spectrum.frame_length, 16U, 1'048'576U)));
    analysis_settings_ui_->fftLengthCombo->setCurrentText(
        settings.spectrum.fft_length == 0U ? "自动" : QString::number(settings.spectrum.fft_length));
    analysis_settings_ui_->windowCombo->setCurrentIndex(static_cast<int>(settings.spectrum.window.kind));
    analysis_settings_ui_->windowParameterSpin->setValue(settings.spectrum.window.parameter);
    static constexpr std::array<dsp::SpectrumOutputQuantity, 6U> outputs{
        dsp::SpectrumOutputQuantity::psd_dbfs_per_hz,  dsp::SpectrumOutputQuantity::magnitude_dbfs,
        dsp::SpectrumOutputQuantity::power_dbfs,       dsp::SpectrumOutputQuantity::linear_power_density,
        dsp::SpectrumOutputQuantity::linear_amplitude, dsp::SpectrumOutputQuantity::linear_power,
    };
    const auto output = std::ranges::find(outputs, settings.spectrum.output_quantity);
    analysis_settings_ui_->outputQuantityCombo->setCurrentIndex(
        output == outputs.end() ? 0 : static_cast<int>(std::distance(outputs.begin(), output)));
    analysis_settings_ui_->estimatorCombo->setCurrentIndex(
        settings.spectrum.estimator.kind == dsp::PsdEstimatorKind::welch ? 1 : 0);
    analysis_settings_ui_->welchOverlapSpin->setValue(
        static_cast<int>(std::llround(settings.spectrum.estimator.welch_overlap * 100.0)));
    analysis_settings_ui_->welchSegmentsSpin->setValue(
        static_cast<int>(std::max<std::uint64_t>(1U, settings.spectrum.estimator.welch_segment_count)));
    analysis_settings_ui_->averagingCombo->setCurrentIndex(static_cast<int>(settings.spectrum.accumulation.mode));
    hold_reset_generation_ = settings.spectrum.accumulation.hold_reset_generation;
    analysis_settings_ui_->resetMaximumHoldButton->setEnabled(settings.spectrum.accumulation.mode ==
                                                              dsp::SpectrumAccumulationMode::maximum_hold);
    analysis_settings_ui_->averagingCountSpin->setValue(
        static_cast<int>(std::max<std::uint64_t>(1U, settings.spectrum.accumulation.averaging_count)));
    analysis_settings_ui_->exponentialAlphaSpin->setValue(settings.spectrum.accumulation.exponential_alpha);
    analysis_settings_ui_->spectrumSmoothingCombo->setCurrentIndex(static_cast<int>(settings.spectrum.smoothing.kind));
    analysis_settings_ui_->smoothingWindowSpin->setValue(
        static_cast<int>(std::max<std::uint32_t>(1U, settings.spectrum.smoothing.window_length)));
    analysis_settings_ui_->gaussianSigmaSpin->setValue(
        settings.spectrum.smoothing.gaussian_sigma > 0.0 ? settings.spectrum.smoothing.gaussian_sigma : 1.0);
    analysis_settings_ui_->savgolOrderSpin->setValue(
        static_cast<int>(std::max<std::uint32_t>(1U, settings.spectrum.smoothing.polynomial_order)));
    analysis_settings_ui_->zeroPaddingCheck->setChecked(settings.spectrum.zero_padding_policy ==
                                                        dsp::ZeroPaddingPolicy::enabled);
    analysis_settings_ui_->sidednessCombo->setCurrentIndex(
        settings.spectrum.sidedness == dsp::SpectrumSidedness::one_sided ? 1 : 0);
    analysis_settings_ui_->normalizationCombo->setCurrentIndex(
        settings.spectrum.normalization == dsp::SpectrumNormalization::window_power    ? 0
        : settings.spectrum.normalization == dsp::SpectrumNormalization::coherent_gain ? 1
                                                                                       : 2);
    analysis_settings_ui_->detrendCombo->setCurrentIndex(
        settings.spectrum.detrend_policy == dsp::DetrendPolicy::remove_mean ? 1 : 0);
    analysis_settings_ui_->measurementSourceCombo->setCurrentIndex(
        settings.spectrum.measurement_source == dsp::MeasurementSource::smoothed ? 1 : 0);

    const auto stft_frame = settings.spectrogram.frame_length == 0U ? 1024U : settings.spectrogram.frame_length;
    analysis_settings_ui_->stftFrameLengthSpin->setValue(
        static_cast<int>(std::clamp<std::uint64_t>(stft_frame, 16U, 1'048'576U)));
    analysis_settings_ui_->stftFftLengthCombo->setCurrentText(
        settings.spectrogram.fft_length == 0U ? "自动" : QString::number(settings.spectrogram.fft_length));
    const auto stft_hop = settings.spectrogram.hop_length == 0U ? std::max<std::uint64_t>(1U, stft_frame / 4U)
                                                                : settings.spectrogram.hop_length;
    analysis_settings_ui_->stftOverlapSpin->setValue(static_cast<int>(
        std::llround((1.0 - static_cast<double>(stft_hop) / static_cast<double>(stft_frame)) * 100.0)));
    analysis_settings_ui_->stftFrequencySmoothingCombo->setCurrentIndex(
        settings.spectrogram.smoothing.frequency_mode == dsp::SpectrogramFrequencySmoothingKind::gaussian ? 1 : 0);
    analysis_settings_ui_->stftTimeSmoothingCombo->setCurrentIndex(
        settings.spectrogram.smoothing.time_mode == dsp::SpectrogramTimeSmoothingKind::exponential
            ? (settings.spectrogram.smoothing.frequency_mode == dsp::SpectrogramFrequencySmoothingKind::gaussian ? 2
                                                                                                                 : 1)
            : 0);
    analysis_settings_ui_->stftTimeAlphaSpin->setValue(settings.spectrogram.smoothing.time_exponential_alpha);
    analysis_settings_ui_->stftWindowCombo->setCurrentIndex(static_cast<int>(settings.spectrogram.window.kind));
    analysis_settings_ui_->stftWindowParameterSpin->setValue(settings.spectrogram.window.parameter);
    analysis_settings_ui_->stftPaddingCheck->setChecked(settings.spectrogram.padding_policy ==
                                                        dsp::ZeroPaddingPolicy::enabled);
    analysis_settings_ui_->stftBoundaryCombo->setCurrentIndex(
        settings.spectrogram.boundary_policy == dsp::SpectrogramBoundaryPolicy::pad_incomplete ? 1 : 0);
    const auto stft_output = std::ranges::find(outputs, settings.spectrogram.output_quantity);
    analysis_settings_ui_->stftOutputQuantityCombo->setCurrentIndex(
        stft_output == outputs.end() ? 0 : static_cast<int>(std::distance(outputs.begin(), stft_output)));
    analysis_settings_ui_->stftNormalizationCombo->setCurrentIndex(
        settings.spectrogram.normalization == dsp::SpectrumNormalization::window_power    ? 0
        : settings.spectrogram.normalization == dsp::SpectrumNormalization::coherent_gain ? 1
                                                                                          : 2);
    analysis_settings_ui_->stftDetrendCombo->setCurrentIndex(
        settings.spectrogram.detrend_policy == dsp::DetrendPolicy::remove_mean ? 1 : 0);

    if (!settings.prefilter.chain.nodes.empty()) {
      const auto& node = settings.prefilter.chain.nodes.front();
      analysis_settings_ui_->prefilterEnabledCheck->setChecked(settings.prefilter.enabled);
      analysis_settings_ui_->filterKindCombo->setCurrentIndex(node.kind == dsp::NodeKind::iir_filter ? 1 : 0);
      analysis_settings_ui_->filterShapeCombo->setCurrentIndex(static_cast<int>(node.filter_shape));
      if (const auto order = node.parameters.find("order"); order != node.parameters.end()) {
        analysis_settings_ui_->filterOrderSpin->setValue(static_cast<int>(order->second));
      }
      if (const auto cutoff = node.parameters.find("cutoff_hz"); cutoff != node.parameters.end()) {
        analysis_settings_ui_->filterHighSpin->setValue(cutoff->second);
      }
      if (const auto low = node.parameters.find("low_cutoff_hz"); low != node.parameters.end()) {
        analysis_settings_ui_->filterLowSpin->setValue(low->second);
      }
      if (const auto high = node.parameters.find("high_cutoff_hz"); high != node.parameters.end()) {
        analysis_settings_ui_->filterHighSpin->setValue(high->second);
      }
      if (const auto quality = node.parameters.find("q"); quality != node.parameters.end()) {
        analysis_settings_ui_->filterQSpin->setValue(quality->second);
      }
      QStringList numerator;
      for (const auto value : node.numerator) {
        numerator.push_back(QString::number(value, 'g', 17));
      }
      QStringList denominator;
      for (const auto value : node.denominator) {
        denominator.push_back(QString::number(value, 'g', 17));
      }
      analysis_settings_ui_->customNumeratorEdit->setText(numerator.join(','));
      analysis_settings_ui_->customDenominatorEdit->setText(denominator.join(','));
    } else {
      analysis_settings_ui_->prefilterEnabledCheck->setChecked(false);
    }
    analysis_settings_ui_->boundaryCombo->setCurrentIndex(
        settings.prefilter.boundary == dsp::BoundaryPolicy::preserve_state ? 1 : 0);
    analysis_settings_ui_->referenceLevelSpin->setValue(display.mapping.reference_level);
    analysis_settings_ui_->dynamicRangeSpin->setValue(display.mapping.dynamic_range);
    analysis_settings_ui_->colorMapCombo->setCurrentText(QString::fromStdString(display.mapping.color_map));
    analysis_settings_ui_->interpolationCombo->setCurrentText(QString::fromStdString(display.interpolation));
    analysis_settings_ui_->frequencyAxisModeCombo->setCurrentText(QString::fromStdString(display.frequency_axis_mode));
    analysis_settings_ui_->amplitudeScaleCombo->setCurrentIndex(
        display.mapping.amplitude_scale == visualization::AmplitudeScale::logarithmic ? 0 : 1);
    analysis_settings_ui_->rangeModeCombo->setCurrentIndex(
        display.mapping.range_mode == visualization::RangeMode::automatic ? 0 : 1);
    analysis_settings_ui_->displayMinimumSpin->setValue(display.mapping.minimum);
    analysis_settings_ui_->displayMaximumSpin->setValue(display.mapping.maximum);
    analysis_panel_loading_ = false;
    update_analysis_derived_information();
  }

  [[nodiscard]] core::Result<dsp::AnalysisSettingsSnapshot> analysis_settings_from_panel() const {
    auto settings = controller_.analysis_settings();
    settings.spectrum.analysis_range_policy = dsp::AnalysisRangePolicy::all_complete_frames;
    settings.spectrum.frame_length = static_cast<std::uint64_t>(analysis_settings_ui_->frameLengthSpin->value());
    settings.spectrum.fft_length = combo_length(analysis_settings_ui_->fftLengthCombo, true);
    settings.spectrum.zero_padding_policy = analysis_settings_ui_->zeroPaddingCheck->isChecked()
                                                ? dsp::ZeroPaddingPolicy::enabled
                                                : dsp::ZeroPaddingPolicy::forbidden;
    settings.spectrum.window.kind = static_cast<dsp::WindowKind>(analysis_settings_ui_->windowCombo->currentIndex());
    settings.spectrum.window.parameter = settings.spectrum.window.kind == dsp::WindowKind::kaiser ||
                                                 settings.spectrum.window.kind == dsp::WindowKind::tukey
                                             ? analysis_settings_ui_->windowParameterSpin->value()
                                             : 0.0;
    settings.spectrum.sidedness = analysis_settings_ui_->sidednessCombo->currentIndex() == 1
                                      ? dsp::SpectrumSidedness::one_sided
                                      : dsp::SpectrumSidedness::two_sided_shifted;
    static constexpr std::array<dsp::SpectrumOutputQuantity, 6U> outputs{
        dsp::SpectrumOutputQuantity::psd_dbfs_per_hz,  dsp::SpectrumOutputQuantity::magnitude_dbfs,
        dsp::SpectrumOutputQuantity::power_dbfs,       dsp::SpectrumOutputQuantity::linear_power_density,
        dsp::SpectrumOutputQuantity::linear_amplitude, dsp::SpectrumOutputQuantity::linear_power,
    };
    settings.spectrum.output_quantity =
        outputs.at(static_cast<std::size_t>(analysis_settings_ui_->outputQuantityCombo->currentIndex()));
    settings.spectrum.normalization =
        analysis_settings_ui_->normalizationCombo->currentIndex() == 0   ? dsp::SpectrumNormalization::window_power
        : analysis_settings_ui_->normalizationCombo->currentIndex() == 1 ? dsp::SpectrumNormalization::coherent_gain
                                                                         : dsp::SpectrumNormalization::none;
    settings.spectrum.detrend_policy = analysis_settings_ui_->detrendCombo->currentIndex() == 1
                                           ? dsp::DetrendPolicy::remove_mean
                                           : dsp::DetrendPolicy::none;
    settings.spectrum.measurement_source = analysis_settings_ui_->measurementSourceCombo->currentIndex() == 1
                                               ? dsp::MeasurementSource::smoothed
                                               : dsp::MeasurementSource::raw;
    settings.spectrum.frequency_reference = data::FrequencyReference::baseband;
    settings.spectrum.estimator.kind = analysis_settings_ui_->estimatorCombo->currentIndex() == 1
                                           ? dsp::PsdEstimatorKind::welch
                                           : dsp::PsdEstimatorKind::periodogram;
    settings.spectrum.estimator.welch_overlap =
        static_cast<double>(analysis_settings_ui_->welchOverlapSpin->value()) / 100.0;
    settings.spectrum.estimator.welch_segment_count =
        static_cast<std::uint64_t>(analysis_settings_ui_->welchSegmentsSpin->value());
    settings.spectrum.accumulation.mode =
        static_cast<dsp::SpectrumAccumulationMode>(analysis_settings_ui_->averagingCombo->currentIndex());
    settings.spectrum.accumulation.averaging_count =
        static_cast<std::uint64_t>(analysis_settings_ui_->averagingCountSpin->value());
    settings.spectrum.accumulation.exponential_alpha = analysis_settings_ui_->exponentialAlphaSpin->value();
    settings.spectrum.accumulation.hold_reset_generation = hold_reset_generation_;
    settings.spectrum.smoothing.kind =
        static_cast<dsp::SpectrumSmoothingKind>(analysis_settings_ui_->spectrumSmoothingCombo->currentIndex());
    settings.spectrum.smoothing.window_length =
        settings.spectrum.smoothing.kind == dsp::SpectrumSmoothingKind::none
            ? 0U
            : static_cast<std::uint32_t>(analysis_settings_ui_->smoothingWindowSpin->value());
    settings.spectrum.smoothing.gaussian_sigma =
        settings.spectrum.smoothing.kind == dsp::SpectrumSmoothingKind::gaussian
            ? analysis_settings_ui_->gaussianSigmaSpin->value()
            : 0.0;
    settings.spectrum.smoothing.polynomial_order =
        settings.spectrum.smoothing.kind == dsp::SpectrumSmoothingKind::savitzky_golay
            ? static_cast<std::uint32_t>(analysis_settings_ui_->savgolOrderSpin->value())
            : 0U;

    settings.spectrogram.frame_length = static_cast<std::uint64_t>(analysis_settings_ui_->stftFrameLengthSpin->value());
    settings.spectrogram.fft_length = combo_length(analysis_settings_ui_->stftFftLengthCombo, true);
    const auto overlap = static_cast<double>(analysis_settings_ui_->stftOverlapSpin->value()) / 100.0;
    settings.spectrogram.hop_length =
        std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::llround(
                                        static_cast<double>(settings.spectrogram.frame_length) * (1.0 - overlap))));
    settings.spectrogram.window.kind =
        static_cast<dsp::WindowKind>(analysis_settings_ui_->stftWindowCombo->currentIndex());
    settings.spectrogram.window.parameter = settings.spectrogram.window.kind == dsp::WindowKind::kaiser ||
                                                    settings.spectrogram.window.kind == dsp::WindowKind::tukey
                                                ? analysis_settings_ui_->stftWindowParameterSpin->value()
                                                : 0.0;
    settings.spectrogram.sidedness = settings.spectrum.sidedness;
    settings.spectrogram.padding_policy = analysis_settings_ui_->stftPaddingCheck->isChecked()
                                              ? dsp::ZeroPaddingPolicy::enabled
                                              : dsp::ZeroPaddingPolicy::forbidden;
    settings.spectrogram.boundary_policy = analysis_settings_ui_->stftBoundaryCombo->currentIndex() == 1
                                               ? dsp::SpectrogramBoundaryPolicy::pad_incomplete
                                               : dsp::SpectrogramBoundaryPolicy::drop_incomplete;
    settings.spectrogram.output_quantity =
        outputs.at(static_cast<std::size_t>(analysis_settings_ui_->stftOutputQuantityCombo->currentIndex()));
    settings.spectrogram.normalization =
        analysis_settings_ui_->stftNormalizationCombo->currentIndex() == 0   ? dsp::SpectrumNormalization::window_power
        : analysis_settings_ui_->stftNormalizationCombo->currentIndex() == 1 ? dsp::SpectrumNormalization::coherent_gain
                                                                             : dsp::SpectrumNormalization::none;
    settings.spectrogram.detrend_policy = analysis_settings_ui_->stftDetrendCombo->currentIndex() == 1
                                              ? dsp::DetrendPolicy::remove_mean
                                              : dsp::DetrendPolicy::none;
    settings.spectrogram.smoothing.frequency_mode =
        analysis_settings_ui_->stftFrequencySmoothingCombo->currentIndex() == 1 ||
                analysis_settings_ui_->stftTimeSmoothingCombo->currentIndex() == 2
            ? dsp::SpectrogramFrequencySmoothingKind::gaussian
            : dsp::SpectrogramFrequencySmoothingKind::none;
    settings.spectrogram.smoothing.frequency_kernel_length =
        settings.spectrogram.smoothing.frequency_mode == dsp::SpectrogramFrequencySmoothingKind::gaussian
            ? static_cast<std::uint32_t>(analysis_settings_ui_->smoothingWindowSpin->value())
            : 0U;
    settings.spectrogram.smoothing.frequency_sigma =
        settings.spectrogram.smoothing.frequency_mode == dsp::SpectrogramFrequencySmoothingKind::gaussian
            ? analysis_settings_ui_->gaussianSigmaSpin->value()
            : 0.0;
    settings.spectrogram.smoothing.time_mode = analysis_settings_ui_->stftTimeSmoothingCombo->currentIndex() > 0
                                                   ? dsp::SpectrogramTimeSmoothingKind::exponential
                                                   : dsp::SpectrogramTimeSmoothingKind::none;
    settings.spectrogram.smoothing.time_exponential_alpha = analysis_settings_ui_->stftTimeAlphaSpin->value();

    settings.prefilter.enabled = analysis_settings_ui_->prefilterEnabledCheck->isChecked();
    settings.prefilter.boundary = analysis_settings_ui_->boundaryCombo->currentIndex() == 1
                                      ? dsp::BoundaryPolicy::preserve_state
                                      : dsp::BoundaryPolicy::zero_pad;
    settings.prefilter.backend_id.clear();
    settings.prefilter.chain.nodes.clear();
    settings.prefilter.group_delay_samples = 0.0;
    if (settings.prefilter.enabled) {
      dsp::NodeSpec node;
      node.id = "analysis-prefilter";
      node.kind = analysis_settings_ui_->filterKindCombo->currentIndex() == 1 ? dsp::NodeKind::iir_filter
                                                                              : dsp::NodeKind::fir_filter;
      node.implementation_id = "signal.dsp.builtin/1.0";
      if (const auto imported = controller_.current_signal(); imported) {
        node.contract.input_unit = imported->descriptor.amplitude_mode;
        node.contract.output_unit = imported->descriptor.amplitude_mode;
        node.contract.produces_complex = imported->descriptor.signal_kind == data::SignalKind::complex;
      }
      node.filter_shape = static_cast<dsp::FilterShape>(analysis_settings_ui_->filterShapeCombo->currentIndex());
      if (node.filter_shape == dsp::FilterShape::custom) {
        auto numerator = parse_coefficients(analysis_settings_ui_->customNumeratorEdit->text());
        auto denominator = parse_coefficients(analysis_settings_ui_->customDenominatorEdit->text());
        if (!numerator || !denominator) {
          return !numerator ? numerator.error() : denominator.error();
        }
        if (numerator.value().empty()) {
          return core::Status::failure({core::ErrorDomain::dsp, core::ErrorReason::invalid_argument},
                                       "自定义 FIR/IIR 必须提供分子系数");
        }
        node.numerator = std::move(numerator.value());
        node.denominator = std::move(denominator.value());
      } else {
        const auto order = node.kind == dsp::NodeKind::iir_filter
                               ? 2.0
                               : static_cast<double>(analysis_settings_ui_->filterOrderSpin->value());
        node.parameters.emplace("order", order);
        if (node.filter_shape == dsp::FilterShape::lowpass || node.filter_shape == dsp::FilterShape::highpass) {
          node.parameters.emplace("cutoff_hz", analysis_settings_ui_->filterHighSpin->value());
          node.parameters.emplace("q", analysis_settings_ui_->filterQSpin->value());
        } else {
          node.parameters.emplace("low_cutoff_hz", analysis_settings_ui_->filterLowSpin->value());
          node.parameters.emplace("high_cutoff_hz", analysis_settings_ui_->filterHighSpin->value());
        }
      }
      settings.prefilter.chain.nodes.push_back(std::move(node));
    }
    if (const auto serialized = dsp::serialize_analysis_settings(settings); !serialized) {
      return serialized.error();
    }
    return settings;
  }

  [[nodiscard]] AnalysisDisplaySettings display_settings_from_panel() const {
    auto settings = controller_.analysis_display_settings();
    settings.mapping.reference_level = analysis_settings_ui_->referenceLevelSpin->value();
    settings.mapping.dynamic_range = analysis_settings_ui_->dynamicRangeSpin->value();
    settings.mapping.minimum = analysis_settings_ui_->displayMinimumSpin->value();
    settings.mapping.maximum = analysis_settings_ui_->displayMaximumSpin->value();
    settings.mapping.amplitude_scale = analysis_settings_ui_->amplitudeScaleCombo->currentIndex() == 0
                                           ? visualization::AmplitudeScale::logarithmic
                                           : visualization::AmplitudeScale::linear;
    settings.mapping.range_mode = analysis_settings_ui_->rangeModeCombo->currentIndex() == 0
                                      ? visualization::RangeMode::automatic
                                      : visualization::RangeMode::manual;
    settings.mapping.color_map = analysis_settings_ui_->colorMapCombo->currentText().toStdString();
    settings.interpolation = analysis_settings_ui_->interpolationCombo->currentText().toStdString();
    settings.frequency_axis_mode = analysis_settings_ui_->frequencyAxisModeCombo->currentText().toStdString();
    return settings;
  }

  void update_analysis_derived_information() {
    if (!analysis_settings_ui_ || analysis_panel_loading_) {
      return;
    }
    auto settings = analysis_settings_from_panel();
    if (!settings) {
      analysis_settings_ui_->derivedInformationLabel->setText("参数未就绪");
      analysis_settings_ui_->parameterWarningLabel->setText(
          QString::fromStdString(std::string{settings.error().message()}));
      return;
    }
    const auto imported = controller_.current_signal();
    if (!imported || !imported->loaded) {
      analysis_settings_ui_->derivedInformationLabel->setText("等待数据源：应用时将依据真实已加载范围计算资源预算。");
      return;
    }
    auto views = current_analysis_views();
    if (!views.spectrum && !views.spectrogram) {
      views = {true, true};
    }
    auto estimate = dsp::estimate_analysis_cost(settings.value(), imported->loaded->samples().size(),
                                                imported->descriptor.sample_rate_hz, 1024ULL * 1024ULL * 1024ULL,
                                                512ULL * 1024ULL * 1024ULL, views.spectrum, views.spectrogram);
    if (!estimate) {
      analysis_settings_ui_->derivedInformationLabel->setText("资源估计失败");
      analysis_settings_ui_->parameterWarningLabel->setText(
          QString::fromStdString(std::string{estimate.error().message()}));
      return;
    }
    const auto& cost = estimate.value();
    double spectrum_cg{};
    double spectrum_enbw_bins{};
    double enbw_hz{};
    if (auto window = dsp::make_window(settings.value().spectrum.window, cost.spectrum_frame_length); window) {
      spectrum_cg = window.value().coherent_gain;
      spectrum_enbw_bins = window.value().equivalent_noise_bandwidth_bins;
      enbw_hz =
          spectrum_enbw_bins * imported->descriptor.sample_rate_hz / static_cast<double>(cost.spectrum_frame_length);
    }
    double stft_cg{};
    double stft_enbw_bins{};
    if (auto window = dsp::make_window(settings.value().spectrogram.window, cost.spectrogram_frame_length); window) {
      stft_cg = window.value().coherent_gain;
      stft_enbw_bins = window.value().equivalent_noise_bandwidth_bins;
    }
    std::string backend_reason;
    auto backend =
        dsp::make_auto_fft_backend(analysis_settings_ui_->backendCombo->currentIndex() == 0, &backend_reason);
    const auto performance_tier = cost.estimated_operations < 5.0e7   ? "交互"
                                  : cost.estimated_operations < 5.0e8 ? "中等"
                                                                      : "重载";
    analysis_settings_ui_->derivedInformationLabel->setText(
        QString("Bin %1 · Δf %2 Hz · 频谱窗 CG/ENBW %3/%4 bins · RBW %5 Hz · STFT %6×%7 · "
                "STFT窗 CG/ENBW %8/%9 bins · Δt %10 ms · FFT %11 次 · %12 GFLOP-est · "
                "主机/显存 %13/%14 MiB · %15 · %16")
            .arg(cost.spectrum_output_bins)
            .arg(cost.spectrum_bin_spacing_hz, 0, 'g', 7)
            .arg(spectrum_cg, 0, 'g', 6)
            .arg(spectrum_enbw_bins, 0, 'g', 6)
            .arg(enbw_hz > 0.0 ? enbw_hz : cost.spectrum_rbw_hz, 0, 'g', 7)
            .arg(cost.spectrogram_rows)
            .arg(cost.spectrogram_columns)
            .arg(stft_cg, 0, 'g', 6)
            .arg(stft_enbw_bins, 0, 'g', 6)
            .arg(cost.spectrogram_time_step_seconds * 1000.0, 0, 'g', 6)
            .arg(cost.fft_execution_count)
            .arg(cost.estimated_operations / 1.0e9, 0, 'f', 3)
            .arg(static_cast<double>(cost.host_memory_bytes) / (1024.0 * 1024.0), 0, 'f', 2)
            .arg(static_cast<double>(cost.device_memory_bytes) / (1024.0 * 1024.0), 0, 'f', 2)
            .arg(performance_tier)
            .arg(backend ? QString::fromStdString(std::string{backend.value()->backend_id()})
                         : QString::fromStdString(backend_reason)));
    analysis_settings_ui_->parameterWarningLabel->setText(settings.value().spectrum.fft_length >
                                                                  settings.value().spectrum.frame_length
                                                              ? "已启用补零：只改善频率采样密度，不提高真实分辨率。"
                                                              : "参数合法；昂贵计算只会在显式应用后启动。");
    begin_filter_preview(settings.value(), *imported);
  }

  void begin_filter_preview(const dsp::AnalysisSettingsSnapshot& settings, const ImportedSignal& imported) {
    ++filter_preview_generation_;
    if (active_filter_preview_) {
      static_cast<void>(controller_.task_runtime().cancel(*active_filter_preview_));
      active_filter_preview_.reset();
    }
    if (!settings.prefilter.enabled || settings.prefilter.chain.nodes.empty() || !imported.loaded) {
      filter_preview_state_.reset();
      analysis_settings_ui_->filterPreviewLabel->setText("分析前滤波关闭；原始样本直接进入频谱与 STFT。");
      return;
    }
    const auto available = imported.loaded->samples();
    const auto preview_count = std::min<std::uint64_t>(available.size(), 512U);
    auto preview_slice = available.slice(0U, preview_count);
    if (!preview_slice) {
      filter_preview_state_.reset();
      analysis_settings_ui_->filterPreviewLabel->setText(
          QString::fromStdString(std::string{preview_slice.error().message()}));
      return;
    }
    auto preview_range = data::SampleRange::from_count(imported.loaded->range().begin(), preview_count);
    if (!preview_range) {
      filter_preview_state_.reset();
      analysis_settings_ui_->filterPreviewLabel->setText(
          QString::fromStdString(std::string{preview_range.error().message()}));
      return;
    }
    auto preview_input = [&preview_slice] {
      if (preview_slice.value().kind() == data::SignalKind::real) {
        const auto values = preview_slice.value().real_values();
        return data::SignalBuffer::from_real(std::vector<double>{values.begin(), values.end()});
      }
      const auto values = preview_slice.value().complex_values();
      return data::SignalBuffer::from_complex(std::vector<data::ComplexSample>{values.begin(), values.end()});
    }();
    auto descriptor = imported.descriptor;
    descriptor.requested_sample_range = preview_range.value();
    const auto node = settings.prefilter.chain.nodes.front();
    const auto generation = filter_preview_generation_;
    filter_preview_state_ = std::make_shared<FilterPreviewState>();
    filter_preview_state_->generation = generation;
    filter_preview_state_->project_id = controller_.workspace().project_id;
    filter_preview_state_->source_version = imported.fingerprint.version_id;
    task::TaskSpec spec;
    spec.task_id = task::TaskId::generate();
    active_filter_preview_ = spec.task_id;
    spec.task_type = "signal-studio.filter-preview";
    spec.priority = task::TaskPriority::interactive;
    spec.resources = {.cpu_units = 1U, .runtime_threads = 1U};
    spec.idempotency_key = imported.fingerprint.version_id + ":filter-preview:" + std::to_string(generation) +
                           ":attempt:" + spec.task_id.value;
    spec.provenance = {{filter_preview_state_->project_id},
                       {imported.fingerprint.version_id},
                       {"data-source", imported.source_path.string()}};
    spec.timeout = std::chrono::seconds{30};
    auto submitted = controller_.task_runtime().submit(
        std::move(spec), [state = filter_preview_state_, preview_input = std::move(preview_input), descriptor,
                          node](task::TaskContext& context) mutable {
          if (!context.checkpoint()) {
            return task::TaskExecutionResult::completed();
          }
          auto backend = dsp::make_auto_signal_kernel_backend();
          if (!backend) {
            std::lock_guard lock{state->mutex};
            state->error = backend.error();
            return task::TaskExecutionResult::completed();
          }
          auto preview = dsp::preview_node(*backend.value(), preview_input.view(), descriptor, node);
          if (!context.checkpoint()) {
            return task::TaskExecutionResult::completed();
          }
          std::lock_guard lock{state->mutex};
          state->backend_id = std::string{backend.value()->backend_id()};
          if (preview) {
            state->preview = std::move(preview.value());
          } else {
            state->error = preview.error();
          }
          return task::TaskExecutionResult::completed();
        });
    if (!submitted) {
      active_filter_preview_.reset();
      filter_preview_state_.reset();
      analysis_settings_ui_->filterPreviewLabel->setText(
          QString::fromStdString(std::string{submitted.error().message()}));
      return;
    }
    analysis_settings_ui_->filterPreviewLabel->setText("正在后台生成 ProcessingChain 频率响应和群时延预览…");
    if (!filter_preview_timer_) {
      filter_preview_timer_ = new QTimer(analysis_settings_panel_);
      filter_preview_timer_->setInterval(25);
      QObject::connect(filter_preview_timer_, &QTimer::timeout, analysis_settings_panel_,
                       [this] { poll_filter_preview(); });
    }
    filter_preview_timer_->start();
  }

  void poll_filter_preview() {
    if (!active_filter_preview_ || !filter_preview_state_) {
      return;
    }
    auto status = controller_.task_runtime().status(*active_filter_preview_);
    if (!status || !task::is_terminal(status.value().state)) {
      return;
    }
    filter_preview_timer_->stop();
    auto state = std::move(filter_preview_state_);
    active_filter_preview_.reset();
    const auto current = controller_.current_signal();
    if (state->generation != filter_preview_generation_ || !current ||
        state->project_id != controller_.workspace().project_id ||
        state->source_version != current->fingerprint.version_id) {
      return;
    }
    std::optional<dsp::NodePreview> preview;
    std::optional<core::Status> error;
    std::string backend_id;
    {
      std::lock_guard lock{state->mutex};
      preview = std::move(state->preview);
      error = state->error;
      backend_id = std::move(state->backend_id);
    }
    if (!preview) {
      analysis_settings_ui_->filterPreviewLabel->setText(error ? QString::fromStdString(std::string{error->message()})
                                                               : "滤波预览已取消");
      return;
    }
    const auto peak = [](const data::SignalSlice& values) {
      double result{};
      if (values.kind() == data::SignalKind::real) {
        for (const auto value : values.real_values()) {
          result = std::max(result, std::abs(value));
        }
      } else {
        for (const auto value : values.complex_values()) {
          result = std::max(result, std::hypot(value.real, value.imag));
        }
      }
      return result;
    };
    const auto midpoint = preview->response_magnitude_db.size() / 2U;
    analysis_settings_ui_->filterPreviewLabel->setText(
        QString("后台 ProcessingChain 预览：H(0)=%1 dB，H(Fs/4)=%2 dB，H(Fs/2)=%3 dB；峰值前/后=%4/%5；"
                "数值群时延=%6 样本；后端=%7。")
            .arg(preview->response_magnitude_db.front(), 0, 'g', 5)
            .arg(preview->response_magnitude_db[midpoint], 0, 'g', 5)
            .arg(preview->response_magnitude_db.back(), 0, 'g', 5)
            .arg(peak(preview->before.view()), 0, 'g', 5)
            .arg(peak(preview->after.view()), 0, 'g', 5)
            .arg(preview->group_delay_samples, 0, 'g', 5)
            .arg(QString::fromStdString(backend_id)));
  }

  void apply_display_settings_only() {
    if (analysis_panel_loading_ || !analysis_settings_ui_) {
      return;
    }
    auto display = display_settings_from_panel();
    if (const auto status = controller_.set_analysis_display_settings(display); !status) {
      analysis_settings_ui_->parameterWarningLabel->setText(QString::fromStdString(std::string{status.message()}));
      return;
    }
    if (primary_workspace_) {
      static_cast<void>(primary_workspace_->set_display_mapping(display.mapping, display.interpolation));
    }
    if (inspector_workspace_) {
      static_cast<void>(inspector_workspace_->set_display_mapping(display.mapping, display.interpolation));
    }
    if (const auto current = controller_.current_analysis()) {
      if (const auto applied = apply_analysis(*current); !applied) {
        analysis_settings_ui_->parameterWarningLabel->setText(QString::fromStdString(std::string{applied.message()}));
        return;
      }
    }
    analysis_settings_ui_->parameterWarningLabel->setText("显示映射已实时更新；未提交 FFT、PSD、STFT 或滤波任务。");
  }

  void apply_analysis_settings_from_panel(int scope) {
    const auto imported = controller_.current_signal();
    if (!imported || !imported->loaded) {
      analysis_settings_ui_->parameterWarningLabel->setText("请先导入数据源再应用分析参数。");
      return;
    }
    auto requested = analysis_settings_from_panel();
    if (!requested) {
      analysis_settings_ui_->parameterWarningLabel->setText(
          QString::fromStdString(std::string{requested.error().message()}));
      return;
    }
    if (scope == 1) {
      requested.value().spectrogram = controller_.analysis_settings().spectrogram;
    } else if (scope == 2) {
      requested.value().spectrum = controller_.analysis_settings().spectrum;
    }
    auto views = current_analysis_views();
    if (!views.spectrum && !views.spectrogram) {
      analysis_settings_ui_->parameterWarningLabel->setText("请至少显示频谱或时频图中的一个视图。");
      return;
    }
    if (const auto status = dsp::validate_analysis_settings(requested.value(), imported->loaded->samples().size(),
                                                            imported->descriptor, views.spectrum, views.spectrogram);
        !status) {
      analysis_settings_ui_->parameterWarningLabel->setText(QString::fromStdString(std::string{status.message()}));
      return;
    }
    auto display = display_settings_from_panel();
    if (const auto status = controller_.set_analysis_display_settings(display); !status) {
      analysis_settings_ui_->parameterWarningLabel->setText(QString::fromStdString(std::string{status.message()}));
      return;
    }
    const auto current_hash = dsp::hash_analysis_settings(controller_.analysis_settings());
    const auto requested_hash = dsp::hash_analysis_settings(requested.value());
    if (!current_hash || !requested_hash) {
      const auto& failure = !current_hash ? current_hash.error() : requested_hash.error();
      analysis_settings_ui_->parameterWarningLabel->setText(QString::fromStdString(std::string{failure.message()}));
      return;
    }
    if (current_hash.value() == requested_hash.value()) {
      apply_display_settings_only();
      if (const auto saved = controller_.save_project(); !saved) {
        analysis_settings_ui_->parameterWarningLabel->setText(
            QString("显示设置已应用但工程保存失败：%1").arg(QString::fromStdString(std::string{saved.message()})));
        return;
      }
      analysis_settings_ui_->parameterWarningLabel->setText("仅显示映射发生变化；未提交 FFT、PSD、STFT 或滤波任务。");
      return;
    }
    parameter_recompute_ = true;
    commit_after_analysis_ = false;
    analysis_settings_ui_->parameterWarningLabel->setText("正在后台重算；旧图谱保留到最新请求原子提交。");
    begin_analysis(*imported, requested.value(), views);
  }

  void save_analysis_preset() {
    auto settings = analysis_settings_from_panel();
    if (!settings) {
      analysis_settings_ui_->parameterWarningLabel->setText(
          QString::fromStdString(std::string{settings.error().message()}));
      return;
    }
    bool ok{};
    const auto name = QInputDialog::getText(native_window_, "保存分析预设", "预设名称", QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty()) {
      return;
    }
    if (const auto status = controller_.save_user_analysis_preset(name.trimmed().toStdString(), settings.value());
        !status) {
      show_error(status);
      return;
    }
    if (!controller_.project_path().empty()) {
      if (const auto saved = controller_.save_project(); !saved) {
        show_error(saved);
        return;
      }
    }
    refresh_analysis_presets();
    analysis_settings_ui_->parameterWarningLabel->setText("用户预设已保存到当前工程。");
  }

  void delete_analysis_preset() {
    const auto id = analysis_settings_ui_->analysisPresetCombo->currentData().toString();
    if (!id.startsWith("user:")) {
      analysis_settings_ui_->parameterWarningLabel->setText("内置预设不可删除。");
      return;
    }
    if (const auto status = controller_.delete_user_analysis_preset(id.sliced(5).toStdString()); !status) {
      show_error(status);
      return;
    }
    if (!controller_.project_path().empty()) {
      if (const auto saved = controller_.save_project(); !saved) {
        show_error(saved);
        return;
      }
    }
    refresh_analysis_presets();
    analysis_settings_ui_->parameterWarningLabel->setText("用户预设已删除。");
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
    if (!cancel_active_analysis_for_project_change()) {
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
    if (!cancel_active_analysis_for_project_change()) {
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
    commit_after_analysis_ = true;
    parameter_recompute_ = false;
    begin_analysis(imported.value());
  }

  [[nodiscard]] AnalysisViewSelection current_analysis_views() const {
    if (!primary_workspace_) {
      return {};
    }
    return {primary_workspace_->chart_visible(visualization::ChartKind::psd),
            primary_workspace_->chart_visible(visualization::ChartKind::spectrogram)};
  }

  [[nodiscard]] bool claim_analysis_cancellation() {
    if (!analysis_state_ || !analysis_state_->commit_artifact) {
      return true;
    }
    auto expected = FormalCommitPhase::cancellable;
    return analysis_state_->formal_phase.compare_exchange_strong(expected, FormalCommitPhase::canceling,
                                                                 std::memory_order_acq_rel);
  }

  void handle_chart_visibility_change(visualization::ChartKind kind, bool visible) {
    if (kind != visualization::ChartKind::psd && kind != visualization::ChartKind::spectrum &&
        kind != visualization::ChartKind::spectrogram && kind != visualization::ChartKind::waterfall) {
      return;
    }
    if (!claim_analysis_cancellation()) {
      if (analysis_settings_ui_) {
        analysis_settings_ui_->parameterWarningLabel->setText("正式制品正在原子提交；完成后再刷新图表可见性。");
      }
      return;
    }
    if (analysis_cancellation_) {
      analysis_cancellation_->store(true, std::memory_order_release);
    }
    if (active_analysis_) {
      static_cast<void>(controller_.task_runtime().cancel(*active_analysis_));
      active_analysis_.reset();
    }
    const auto views = current_analysis_views();
    const auto signal = controller_.current_signal();
    if (signal && (views.spectrum || views.spectrogram)) {
      parameter_recompute_ = true;
      commit_after_analysis_ = false;
      begin_analysis(*signal, controller_.analysis_settings(), views);
    } else if (analysis_settings_ui_) {
      analysis_settings_ui_->parameterWarningLabel->setText(visible ? "图表已显示；等待可用数据源。"
                                                                    : "所有频域图已隐藏；后台频域任务已取消。");
    }
  }

  [[nodiscard]] bool cancel_active_analysis_for_project_change() {
    if (!claim_analysis_cancellation()) {
      if (progress_ui_) {
        progress_ui_->loadStatusLabel->setText("正式制品正在原子提交；完成前不会切换工程。");
      }
      return false;
    }
    if (analysis_cancellation_) {
      analysis_cancellation_->store(true, std::memory_order_release);
    }
    if (active_analysis_) {
      static_cast<void>(controller_.task_runtime().cancel(*active_analysis_));
    }
    if (active_import_) {
      static_cast<void>(active_import_->handle().cancel());
      active_import_.reset();
      if (progress_timer_) {
        progress_timer_->stop();
      }
      if (progress_ui_) {
        progress_ui_->loadStatusLabel->setText("工程已切换；旧导入已取消且不会写入新工程。");
      }
    }
    preview_coordinator_.cancel_current();
    if (preview_task_) {
      static_cast<void>(preview_task_->cancel());
      preview_task_.reset();
      preview_state_.reset();
    }
    ++filter_preview_generation_;
    if (active_filter_preview_) {
      static_cast<void>(controller_.task_runtime().cancel(*active_filter_preview_));
      active_filter_preview_.reset();
      filter_preview_state_.reset();
    }
    static_cast<void>(controller_.task_runtime().issue_view_request("signal-studio.analysis"));
    return true;
  }

  void cancel_active_analysis_from_ui() {
    if (!claim_analysis_cancellation()) {
      if (progress_ui_) {
        progress_ui_->loadStatusLabel->setText("正式制品正在原子提交，取消边界已关闭");
      }
      return;
    }
    static_cast<void>(controller_.task_runtime().issue_view_request("signal-studio.analysis"));
    if (analysis_cancellation_) {
      analysis_cancellation_->store(true, std::memory_order_release);
    }
    if (active_analysis_) {
      static_cast<void>(controller_.task_runtime().cancel(*active_analysis_));
    }
    if (progress_ui_) {
      progress_ui_->loadStatusLabel->setText("正在取消；半成品不会发布");
    }
  }

  void begin_analysis(ImportedSignal imported,
                      std::optional<dsp::AnalysisSettingsSnapshot> requested_settings = std::nullopt,
                      std::optional<AnalysisViewSelection> requested_views = std::nullopt) {
    if (!claim_analysis_cancellation()) {
      if (analysis_settings_ui_) {
        analysis_settings_ui_->parameterWarningLabel->setText("正式制品正在原子提交；完成后再应用新参数。");
      }
      return;
    }
    if (analysis_cancellation_) {
      analysis_cancellation_->store(true, std::memory_order_release);
    }
    if (active_analysis_) {
      static_cast<void>(controller_.task_runtime().cancel(*active_analysis_));
      active_analysis_.reset();
    }
    const auto settings = requested_settings.value_or(controller_.analysis_settings());
    const auto views = requested_views.value_or(current_analysis_views());
    const auto prefer_cuda = analysis_settings_ui_ && analysis_settings_ui_->backendCombo->currentIndex() == 0;
    auto settings_hash = dsp::hash_analysis_settings(settings);
    if (!settings_hash) {
      finish_progress_with_error(settings_hash.error());
      return;
    }
    if (progress_ui_) {
      progress_ui_->progressTitleLabel->setText("正在计算参数化 PSD / STFT");
      progress_ui_->loadStatusLabel->setText("分析已提交到任务运行时；旧图谱保持可见");
      progress_ui_->loadProgressBar->setRange(0, 0);
      progress_ui_->pauseResumeButton->setEnabled(false);
      progress_ui_->cancelLoadButton->setEnabled(true);
    }
    analysis_state_ = std::make_shared<AnalysisState>();
    analysis_state_->commit_artifact = commit_after_analysis_;
    analysis_state_->parameter_recompute = parameter_recompute_;
    analysis_cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto view_request = controller_.task_runtime().issue_view_request("signal-studio.analysis");
    task::TaskSpec spec;
    spec.task_id = task::TaskId::generate();
    const auto task_id = spec.task_id.value;
    active_analysis_ = spec.task_id;
    spec.task_type = "signal-studio.parameterized-analysis";
    spec.priority = task::TaskPriority::interactive;
    spec.resources = {.cpu_units = 1U, .gpu_units = prefer_cuda ? 1U : 0U, .runtime_threads = 1U};
    spec.idempotency_key = imported.fingerprint.version_id + ":" + settings_hash.value().stable_text() +
                           (prefer_cuda ? ":cuda-preferred" : ":cpu-only") +
                           ":views:" + std::to_string(static_cast<unsigned>(views.spectrum)) +
                           std::to_string(static_cast<unsigned>(views.spectrogram)) +
                           ":request:" + std::to_string(view_request.generation);
    spec.provenance = {{controller_.workspace().project_id},
                       {imported.fingerprint.version_id},
                       {"data-source", imported.source_path.string()}};
    spec.view_request = view_request;
    spec.timeout = std::chrono::minutes{5};
    // Only analysis requests that produce a formal artifact need crash-recoverable task lineage.
    // Ephemeral view recomputations are guarded by view generations; journaling every rapid
    // cancel/stale transition would synchronize the complete history on the UI thread.
    spec.persistent = analysis_state_->commit_artifact;
    const auto launch_ready = std::make_shared<std::atomic_bool>(false);
    auto submitted = controller_.task_runtime().submit(std::move(spec), [this, imported = std::move(imported), settings,
                                                                         prefer_cuda, views, view_request, task_id,
                                                                         cancellation = analysis_cancellation_,
                                                                         state = analysis_state_, launch_ready](
                                                                            task::TaskContext& context) mutable {
      const auto fail_task = [&state](const core::Status& status) {
        {
          std::lock_guard lock{state->mutex};
          state->error = status;
        }
        return task::TaskExecutionResult::failed(
            {"SS-ANALYSIS", std::string{status.message()}, std::string{status.diagnostic()},
             "核对分析参数、工程和结果目录后重试", true, "retry", "log://signal-studio/analysis"});
      };
      while (!launch_ready->load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      static_cast<void>(context.report_progress(0.1, "准备真实样本范围"));
      if (!context.checkpoint()) {
        cancellation->store(true, std::memory_order_release);
        return task::TaskExecutionResult::completed();
      }
      auto result = controller_.analyze(imported, settings, prefer_cuda, cancellation, view_request, task_id, views);
      if (!context.checkpoint()) {
        cancellation->store(true, std::memory_order_release);
        return task::TaskExecutionResult::completed();
      }
      if (!result) {
        return fail_task(result.error());
      }
      auto bundle = std::move(result.value());
      if (state->commit_artifact) {
        // The final checkpoint is the cancellation boundary for a formal result. Before it,
        // cancellation/staleness wins; after it, the worker owns publication and artifact commit.
        if (!context.checkpoint()) {
          cancellation->store(true, std::memory_order_release);
          return task::TaskExecutionResult::completed();
        }
        auto expected_phase = FormalCommitPhase::cancellable;
        if (!state->formal_phase.compare_exchange_strong(expected_phase, FormalCommitPhase::committing,
                                                         std::memory_order_acq_rel)) {
          cancellation->store(true, std::memory_order_release);
          return task::TaskExecutionResult::completed();
        }
        static_cast<void>(context.report_progress(0.9, "正在提交正式分析制品"));
        const auto previous_analysis = controller_.current_analysis();
        const auto previous_settings = controller_.analysis_settings();
        const auto previous_views = previous_analysis ? previous_analysis->views : views;
        const auto restore_previous_settings = [&] {
          return controller_.set_analysis_settings(previous_settings, previous_views);
        };
        if (const auto settings_status = controller_.set_analysis_settings(bundle.settings, bundle.views);
            !settings_status) {
          return fail_task(settings_status);
        }
        if (!controller_.commit_analysis(bundle, view_request)) {
          const auto restored = restore_previous_settings();
          const auto commit_failure = ui_failure("正式分析未获得最新结果提交许可", task_id);
          return fail_task(restored ? commit_failure : restored.with_context(std::string{commit_failure.message()}));
        }
        auto artifact = controller_.commit_measurement(bundle, "initial-visible-range", "CH-01");
        if (!artifact) {
          const auto restored = restore_previous_settings();
          controller_.restore_analysis_after_failed_task(task_id, previous_analysis);
          return fail_task(restored ? artifact.error() : restored.with_context("正式 Artifact 提交失败后恢复旧设置"));
        }
        const auto fail_formal_artifact = [&](const core::Status& status) {
          {
            std::lock_guard lock{state->mutex};
            state->bundle.reset();
          }
          const auto restored = restore_previous_settings();
          const auto rollback = controller_.rollback_measurement(artifact.value());
          controller_.restore_analysis_after_failed_task(task_id, previous_analysis);
          if (!restored) {
            return fail_task(restored.with_context("正式制品提交失败后恢复旧设置"));
          }
          return fail_task(
              rollback ? status
                       : rollback.with_context("正式制品提交失败后回滚未完全成功：" + std::string{status.message()}));
        };
        {
          std::lock_guard lock{state->mutex};
          state->bundle = std::move(bundle);
        }
        static_cast<void>(context.report_progress(1.0, "正在登记正式制品完整性并完成任务"));
        const std::array artifact_files{artifact.value().payload_path, artifact.value().package_path / "manifest.json",
                                        artifact.value().package_path / ".artifact-index"};
        if (const auto completed = context.complete_with_existing_artifacts(artifact_files); !completed) {
          return fail_formal_artifact(completed);
        }
        state->formal_phase.store(FormalCommitPhase::finalized, std::memory_order_release);
        return task::TaskExecutionResult::completed();
      }
      {
        std::lock_guard lock{state->mutex};
        state->bundle = std::move(bundle);
      }
      static_cast<void>(context.report_progress(1.0, "PSD/STFT 已完成"));
      return task::TaskExecutionResult::completed();
    });
    if (progress_ui_) {
      QObject::disconnect(progress_ui_->cancelLoadButton, nullptr, nullptr, nullptr);
      QObject::connect(progress_ui_->cancelLoadButton, &QPushButton::clicked, progress_dialog_,
                       [this] { cancel_active_analysis_from_ui(); });
    }
    if (analysis_timer_) {
      analysis_timer_->stop();
      analysis_timer_->deleteLater();
    }
    analysis_timer_ = new QTimer(progress_dialog_ ? static_cast<QObject*>(progress_dialog_)
                                                  : static_cast<QObject*>(analysis_settings_panel_));
    analysis_timer_->setInterval(25);
    QObject::connect(analysis_timer_, &QTimer::timeout, analysis_timer_, [this] { poll_analysis(); });
    analysis_timer_->start();
    launch_ready->store(true, std::memory_order_release);
    if (!submitted) {
      active_analysis_.reset();
      finish_progress_with_error(submitted.error());
      return;
    }
  }

  void poll_analysis() {
    if (!active_analysis_ || !analysis_state_) {
      return;
    }
    auto status = controller_.task_runtime().status(*active_analysis_);
    if (!status) {
      if (progress_ui_) {
        finish_progress_with_error(status.error());
      } else if (analysis_settings_ui_) {
        analysis_settings_ui_->parameterWarningLabel->setText(
            QString::fromStdString(std::string{status.error().message()}));
      }
      return;
    }
    if (!task::is_terminal(status.value().state)) {
      return;
    }
    analysis_timer_->stop();
    std::optional<AnalysisBundle> bundle;
    std::optional<core::Status> error;
    bool commit_artifact{};
    bool parameter_recompute{};
    {
      std::lock_guard lock{analysis_state_->mutex};
      bundle = std::move(analysis_state_->bundle);
      error = analysis_state_->error;
      commit_artifact = analysis_state_->commit_artifact;
      parameter_recompute = analysis_state_->parameter_recompute;
    }
    active_analysis_.reset();
    analysis_state_.reset();
    if (status.value().state == task::TaskState::canceled || status.value().state == task::TaskState::stale) {
      if (analysis_settings_ui_) {
        analysis_settings_ui_->parameterWarningLabel->setText("分析已取消；旧图谱保持不变。");
      }
      return;
    }
    if (!bundle) {
      const auto failure_status = error.value_or(ui_failure("分析任务未返回结果"));
      if (progress_ui_) {
        finish_progress_with_error(failure_status);
      } else if (analysis_settings_ui_) {
        analysis_settings_ui_->parameterWarningLabel->setText(
            QString::fromStdString(std::string{failure_status.message()}));
      }
      return;
    }
    if (!commit_artifact && !controller_.commit_analysis(*bundle, bundle->view_request)) {
      if (analysis_settings_ui_) {
        analysis_settings_ui_->parameterWarningLabel->setText("旧分析请求已完成但未获得最新结果提交许可。");
      }
      return;
    }
    if (!commit_artifact) {
      if (const auto updated = controller_.set_analysis_settings(bundle->settings, bundle->views); !updated) {
        if (progress_ui_) {
          finish_progress_with_error(updated);
        }
        return;
      }
    }
    if (const auto applied = apply_analysis(*bundle); !applied) {
      if (progress_ui_) {
        finish_progress_with_error(applied);
      }
      return;
    }
    if (!commit_artifact) {
      if (const auto saved = controller_.save_project(); !saved) {
        if (progress_ui_) {
          finish_progress_with_error(saved);
        } else if (analysis_settings_ui_) {
          analysis_settings_ui_->parameterWarningLabel->setText(
              QString("分析已计算但工程保存失败：%1").arg(QString::fromStdString(std::string{saved.message()})));
        }
        return;
      }
    }
    set_analysis_panel_settings(bundle->settings, controller_.analysis_display_settings());
    if (analysis_settings_ui_) {
      analysis_settings_ui_->parameterWarningLabel->setText(
          QString("最新参数已提交 · %1 · %2 · %3 ms")
              .arg(QString::fromStdString(bundle->settings_hash.hex.substr(0U, 12U)))
              .arg(bundle->cache_hit ? "缓存命中" : "真实重算")
              .arg(bundle->compute_duration.count()));
    }
    if (progress_ui_) {
      progress_ui_->loadProgressBar->setRange(0, 100);
      progress_ui_->loadProgressBar->setValue(100);
      progress_ui_->loadStatusLabel->setText("导入、参数化分析和可追溯结果提交已完成");
    }
    refresh_all();
    static_cast<void>(window_->show_page("p02"));
    if (progress_dialog_ && commit_artifact) {
      QTimer::singleShot(500, progress_dialog_, &QDialog::accept);
    }
    commit_after_analysis_ = false;
    static_cast<void>(parameter_recompute);
    parameter_recompute_ = false;
  }

  [[nodiscard]] core::Status apply_analysis(const AnalysisBundle& bundle) {
    auto* primary = find_primary_workspace();
    if (primary == nullptr) {
      return ui_failure("主分析工作区句柄不可用");
    }
    auto viewport = bundle.viewport;
    auto frame = bundle.frame;
    const auto& display = controller_.analysis_display_settings();
    const auto want_absolute = display.frequency_axis_mode != "baseband" && frame.center_frequency_hz != 0;
    if (want_absolute != frame.absolute_frequency) {
      const auto offset = want_absolute ? frame.center_frequency_hz : -frame.center_frequency_hz;
      viewport.effective_frequency_range.begin_hz += offset;
      viewport.effective_frequency_range.end_hz += offset;
      viewport.frequency_viewport.begin_hz += offset;
      viewport.frequency_viewport.end_hz += offset;
      frame.frequency_range.begin_hz += offset;
      frame.frequency_range.end_hz += offset;
      frame.absolute_frequency = want_absolute;
    }
    if (auto mapped = primary->set_display_mapping(display.mapping, display.interpolation); !mapped) {
      return mapped;
    }
    if (auto applied = primary->apply_viewport(viewport); !applied) {
      return applied;
    }
    frame.request_id = viewport.request_id;
    if (auto bound = primary->bind_frame(frame); !bound) {
      return bound;
    }
    if (auto mapped = inspector_workspace_->set_display_mapping(display.mapping, display.interpolation); !mapped) {
      return mapped;
    }
    if (auto applied = inspector_workspace_->apply_viewport(viewport); !applied) {
      return applied;
    }
    if (auto bound = inspector_workspace_->bind_frame(frame); !bound) {
      return bound;
    }
    inspector_ui_->inspectorVersionLabel->setText(
        QString("CH-01 · %1 · 参数 %2 · %3%4")
            .arg(QString::fromStdString(bundle.frame.data_source_version_id.substr(0, 12)))
            .arg(QString::fromStdString(bundle.settings_hash.hex.substr(0, 12)))
            .arg(QString::fromStdString(bundle.backend_id))
            .arg(bundle.settings.prefilter.enabled ? " · 已滤波" : ""));
    return core::Status::success();
  }

  [[nodiscard]] visualization::IAnalysisWorkspace* find_primary_workspace() noexcept {
    return primary_workspace_;
  }

  void refresh_all() {
    if (window_) {
      static_cast<void>(window_->update_content(controller_.workbench_content()));
    }
    if (analysis_settings_ui_) {
      refresh_analysis_presets();
      set_analysis_panel_settings(controller_.analysis_settings(), controller_.analysis_display_settings());
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
    const auto current_parameter_hash = dsp::hash_analysis_settings(controller_.analysis_settings());
    const auto is_current_record = [&](const core::ArtifactRecord& record) {
      return current && current_parameter_hash &&
             record.descriptor.provenance.data_source_version_id == current->fingerprint.version_id &&
             record.descriptor.provenance.parameter_version == current_parameter_hash.value().stable_text();
    };
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
      const auto is_current = is_current_record(record);
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
      const auto is_current = is_current_record(record);
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
    const auto current_parameter_hash = dsp::hash_analysis_settings(controller_.analysis_settings());
    const auto is_current =
        current && current_parameter_hash &&
        record.descriptor.provenance.data_source_version_id == current->fingerprint.version_id &&
        record.descriptor.provenance.parameter_version == current_parameter_hash.value().stable_text();
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
    bool commit_artifact{};
    bool parameter_recompute{};
    std::atomic<FormalCommitPhase> formal_phase{FormalCommitPhase::cancellable};
  };

  struct PreviewState final {
    std::mutex mutex;
    std::optional<data::PreviewResult> preview;
    std::optional<core::Status> error;
  };

  struct FilterPreviewState final {
    std::mutex mutex;
    std::uint64_t generation{};
    std::string project_id;
    std::string source_version;
    std::string backend_id;
    std::optional<dsp::NodePreview> preview;
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
  QWidget* analysis_settings_panel_{};
  std::unique_ptr<Ui::SignalAnalysisSettingsPanel> analysis_settings_ui_;
  QTimer* derived_settings_timer_{};
  QTimer* filter_preview_timer_{};
  std::optional<task::TaskId> active_filter_preview_;
  std::shared_ptr<FilterPreviewState> filter_preview_state_;
  std::uint64_t filter_preview_generation_{};
  std::uint64_t hold_reset_generation_{};
  bool analysis_panel_loading_{};
  std::unique_ptr<visualization::IAnalysisWorkspace> inspector_workspace_;
  std::vector<core::ArtifactRecord> visible_results_;

  QDialog* import_dialog_{};
  QDialog* progress_dialog_{};
  std::unique_ptr<Ui::SignalImportWizard> import_ui_;
  std::unique_ptr<Ui::SignalLoadProgressDialog> progress_ui_;
  QTimer* progress_timer_{};
  QTimer* analysis_timer_{};
  std::optional<ImportTask> active_import_;
  std::optional<task::TaskId> active_analysis_;
  std::shared_ptr<AnalysisState> analysis_state_;
  std::shared_ptr<std::atomic_bool> analysis_cancellation_;
  bool commit_after_analysis_{};
  bool parameter_recompute_{};
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

core::Status QtApplication::validate_analysis_settings_panel() {
  return impl_->validate_analysis_settings_panel();
}

core::Status QtApplication::validate_analysis_runtime() {
  return impl_->validate_analysis_runtime();
}

void QtApplication::set_analysis_settings_advanced(bool advanced) {
  impl_->show_advanced_analysis_settings(advanced);
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
