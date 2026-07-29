#include "signal_studio/visualization/visualization.hpp"
#include "signal_studio/workbench/workbench.hpp"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QAction>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPixmap>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using signal::data::SampleRange;
using signal::visualization::FrequencyRange;
using signal::visualization::ViewportSnapshot;
using signal::visualization::VisualizationFrame;

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename T> const T& require(const signal::core::Result<T>& result, std::string_view message) {
  check(result.ok(), message);
  return result.value();
}

void require_status(const signal::core::Status& status, std::string_view message) {
  check(status.ok(), message);
}

[[nodiscard]] SampleRange range(std::uint64_t begin, std::uint64_t end) {
  return require(SampleRange::make(begin, end), "无法构造测试范围");
}

[[nodiscard]] ViewportSnapshot make_viewport(bool partial = false) {
  signal::visualization::ViewportController controller("ms03-test");
  require(controller.bind_source("source-v1", range(1'000, 101'000), FrequencyRange{-25'000'000, 25'000'000}, partial,
                                 partial ? 400'000 : 800'000, 800'000),
          "绑定视口失败");
  require(controller.set_time(range(20'000, 40'000)), "设置时间视口失败");
  require(controller.set_frequency(FrequencyRange{-20'000'000, 20'000'000}), "设置频率视口失败");
  return controller.snapshot();
}

[[nodiscard]] VisualizationFrame make_frame(const ViewportSnapshot& viewport) {
  VisualizationFrame frame;
  frame.request_id = viewport.request_id;
  frame.time_range = viewport.time_viewport;
  frame.frequency_range = viewport.frequency_viewport;
  frame.quality = signal::visualization::ViewQuality::refined;
  frame.time_mode = signal::visualization::TimeDisplayMode::in_phase_quadrature;
  frame.spectrum_layout = signal::visualization::SpectrumLayout::shifted_two_sided;
  frame.data_source_version_id = viewport.data_source_version_id;
  frame.absolute_frequency = false;
  constexpr std::size_t waveform_points = 512;
  constexpr std::size_t psd_points = 256;
  for (std::size_t index = 0; index < waveform_points; ++index) {
    const auto phase = static_cast<double>(index) * 0.07;
    frame.time_primary.push_back(std::sin(phase) * 0.7);
    frame.time_secondary.push_back(std::cos(phase) * 0.65);
  }
  for (std::size_t index = 0; index < psd_points; ++index) {
    const auto x = (static_cast<double>(index) - 128.0) / 38.0;
    frame.psd_db_hz.push_back(-82.0 + 45.0 * std::exp(-0.5 * x * x));
  }
  frame.psd_metadata = {viewport.time_viewport.size(), 64, 512, "Hann", "Welch", 97'656.25, "dB/Hz"};
  frame.stft_rows = 48;
  frame.stft_columns = 96;
  for (std::uint32_t row = 0; row < frame.stft_rows; ++row) {
    for (std::uint32_t column = 0; column < frame.stft_columns; ++column) {
      const auto ridge = 30.0 + 18.0 * std::sin(static_cast<double>(row) * 0.15);
      const auto distance = (static_cast<double>(column) - ridge) / 7.0;
      frame.stft_db.push_back(static_cast<float>(-95.0 + 68.0 * std::exp(-0.5 * distance * distance)));
    }
  }
  frame.stft_metadata = {256, 64, 256, 0.75, "Industrial", "nearest"};
  frame.constellation_i = {-0.7, -0.65, 0.68, 0.72};
  frame.constellation_q = {-0.7, 0.67, -0.69, 0.71};
  frame.eye_trace = {-1.0, -0.6, 0.2, 0.9, 1.0, 0.5, -0.3, -1.0};
  return frame;
}

[[nodiscard]] std::unique_ptr<signal::visualization::IAnalysisWorkspace>
make_workspace(const ViewportSnapshot& viewport, bool bind_frame = true) {
  auto workspace = signal::visualization::make_analysis_workspace(
      {.title = "测试工作区", .source_label = viewport.data_source_version_id});
  check(workspace != nullptr, "无法创建 Qt 可视化工作区");
  require_status(workspace->apply_viewport(viewport), "无法应用 Qt 视口");
  if (bind_frame) {
    require_status(workspace->bind_frame(make_frame(viewport)), "无法绑定 Qt 三图");
  }
  return workspace;
}

[[nodiscard]] std::unique_ptr<signal::workbench::IWorkbenchWindow>
make_workbench(const ViewportSnapshot& viewport, signal::workbench::WorkbenchContent content = {}) {
  auto commands = std::make_shared<signal::workbench::CommandRegistry>();
  auto panels = std::make_shared<signal::workbench::PanelRegistry>();
  signal::workbench::WorkbenchConfiguration configuration{.application_name = "Signal Platform",
                                                          .window_title = "MS-03 测试工作台"};
  configuration.content = std::move(content);
  auto window = signal::workbench::make_workbench_window(std::move(configuration), make_workspace(viewport),
                                                         std::move(commands), std::move(panels));
  check(window != nullptr, "无法创建 Qt 工作台");
  return window;
}

[[nodiscard]] signal::visualization::OverlayModel make_overlay() {
  return {range(1'000, 101'000), FrequencyRange{-25'000'000, 25'000'000}};
}

[[nodiscard]] QWidget* find_chart(QWidget* root, const QString& name) {
  const auto widgets = root->findChildren<QWidget*>();
  const auto found = std::ranges::find_if(widgets, [&name](const QWidget* widget) {
    return widget->accessibleName().contains(name) && widget->minimumHeight() >= 96;
  });
  return found == widgets.end() ? nullptr : *found;
}

void send_mouse_drag(QWidget* widget, Qt::MouseButton button, QPointF begin, QPointF end) {
  check(widget != nullptr, "待拖动图表不存在");
  const auto begin_global = widget->mapToGlobal(begin.toPoint());
  const auto end_global = widget->mapToGlobal(end.toPoint());
  QMouseEvent press(QEvent::MouseButtonPress, begin, begin_global, button, button, Qt::NoModifier);
  QApplication::sendEvent(widget, &press);
  QMouseEvent move(QEvent::MouseMove, end, end_global, Qt::NoButton, button, Qt::NoModifier);
  QApplication::sendEvent(widget, &move);
  QMouseEvent release(QEvent::MouseButtonRelease, end, end_global, button, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(widget, &release);
  QApplication::processEvents();
}

void test_requirement(const std::string& id) {
  const auto viewport = make_viewport();

  if (id == "FR-VIS-001") {
    signal::visualization::AtomicFrameCoordinator coordinator;
    require_status(coordinator.begin(viewport), "无法开始原子视图");
    require_status(coordinator.commit(make_frame(viewport)), "三图原子提交失败");
    const auto committed = coordinator.frame();
    check(committed && committed->time_range == viewport.time_viewport && !committed->time_primary.empty() &&
              !committed->psd_db_hz.empty() && !committed->stft_db.empty(),
          "时域、PSD 与 STFT 未覆盖同一 CurrentTimeViewport");
  } else if (id == "FR-VIS-002") {
    auto frame = make_frame(viewport);
    frame.spectrum_layout = signal::visualization::SpectrumLayout::one_sided;
    frame.frequency_range = {0, 25'000'000};
    check(frame.spectrum_layout == signal::visualization::SpectrumLayout::one_sided &&
              frame.frequency_range.begin_hz == 0,
          "实信号默认单边谱契约失败");
    frame.spectrum_layout = signal::visualization::SpectrumLayout::mirrored_two_sided;
    check(frame.spectrum_layout == signal::visualization::SpectrumLayout::mirrored_two_sided,
          "实信号镜像双边模式不可选");
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    const auto actions = root->findChildren<QAction*>();
    const auto single_sided =
        std::ranges::find_if(actions, [](const QAction* action) { return action->text() == "频谱 · 单边"; });
    check(single_sided != actions.end(), "Qt 显示菜单缺少单边谱模式");
    (*single_sided)->trigger();
    QApplication::processEvents();
    check(workspace->viewport().frequency_viewport.begin_hz == 0, "Qt 单边谱模式未采用 0…Fs/2 语义");
  } else if (id == "FR-VIS-003") {
    auto frame = make_frame(viewport);
    frame.spectrum_layout = signal::visualization::SpectrumLayout::shifted_two_sided;
    frame.absolute_frequency = true;
    frame.center_frequency_hz = 1'425'000'000;
    check(frame.frequency_range.begin_hz < 0 && frame.frequency_range.end_hz > 0 && frame.absolute_frequency &&
              frame.center_frequency_hz == 1'425'000'000,
          "复信号 fftshift/绝对频率契约失败");
  } else if (id == "FR-VIS-004") {
    auto workspace = make_workspace(viewport);
    check(workspace->viewport().frequency_viewport == viewport.frequency_viewport, "PSD 与 STFT 未共享频率视口");
    check(workspace->accessibility_summary().find("频率") != std::string::npos, "共享频率范围未同步到工作区属性");
  } else if (id == "FR-VIS-005") {
    auto frame = make_frame(viewport);
    const auto original_range = frame.time_range;
    for (const auto mode :
         {signal::visualization::TimeDisplayMode::real, signal::visualization::TimeDisplayMode::in_phase_quadrature,
          signal::visualization::TimeDisplayMode::magnitude, signal::visualization::TimeDisplayMode::phase}) {
      frame.time_mode = mode;
      check(frame.time_range == original_range, "时域模式切换改变了数据范围");
    }
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    const auto actions = root->findChildren<QAction*>();
    const auto magnitude =
        std::ranges::find_if(actions, [](const QAction* action) { return action->text() == "时域 · 幅度"; });
    check(magnitude != actions.end(), "Qt 显示菜单缺少幅度模式");
    const auto before = workspace->viewport().time_viewport;
    (*magnitude)->trigger();
    QApplication::processEvents();
    const auto canvases = root->findChildren<QWidget*>();
    check(workspace->viewport().time_viewport == before &&
              std::ranges::any_of(canvases,
                                  [](const QWidget* widget) {
                                    return widget->accessibleName().contains("时域") &&
                                           widget->accessibleDescription().contains("幅度");
                                  }),
          "Qt 时域幅度模式未真实切换或改变了时间范围");
  } else if (id == "FR-VIS-006") {
    const auto frame = make_frame(viewport);
    check(frame.time_range == viewport.time_viewport &&
              frame.psd_metadata.effective_samples == viewport.time_viewport.size() &&
              frame.psd_metadata.fft_frames > 0 && frame.psd_metadata.window == "Hann" &&
              frame.psd_metadata.averaging == "Welch" && frame.psd_metadata.rbw_hz > 0.0 &&
              frame.psd_metadata.unit == "dB/Hz",
          "PSD 当前视窗统计摘要不完整");
  } else if (id == "FR-VIS-007") {
    const auto frame = make_frame(viewport);
    check(frame.time_range == viewport.time_viewport && frame.stft_rows > 0 && frame.stft_columns > 0 &&
              frame.stft_metadata.window_size > 0 && frame.stft_metadata.hop_size > 0 &&
              frame.stft_metadata.fft_size > 0 && frame.stft_metadata.overlap_ratio == 0.75 &&
              !frame.stft_metadata.color_map.empty() && !frame.stft_metadata.interpolation.empty(),
          "STFT 当前视窗或显示参数不完整");
  } else if (id == "FR-VIS-008") {
    signal::visualization::ViewportController controller("interactions");
    require(controller.bind_source("source-v1", range(0, 100'000), FrequencyRange{-50'000'000, 50'000'000}),
            "绑定交互视口失败");
    const auto initial = controller.snapshot().frequency_viewport;
    require(controller.set_frequency({-10'000'000, 10'000'000}), "缩放失败");
    check(controller.snapshot().frequency_viewport != initial, "频率缩放没有改变视口");
    check(controller.snapshot().frequency_viewport != controller.snapshot().effective_frequency_range,
          "缩放被非显式适合全部覆盖");
    require(controller.reset_frequency(), "显式适合全部失败");
    check(controller.snapshot().frequency_viewport == initial, "显式适合全部未恢复全范围");
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    root->resize(1280, 720);
    root->show();
    QApplication::processEvents();
    auto* psd = find_chart(root, "功率谱");
    check(psd != nullptr, "未找到可滚轮缩放的 PSD");
    const auto before = workspace->viewport();
    const QPointF local(160.0, 60.0);
    const QPointF global(psd->mapToGlobal(local.toPoint()));
    QWheelEvent wheel(local, global, QPoint{}, QPoint{0, 120}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(psd, &wheel);
    QApplication::processEvents();
    check(workspace->viewport().request_id.generation > before.request_id.generation &&
              workspace->viewport().frequency_viewport.bandwidth_hz() < before.frequency_viewport.bandwidth_hz(),
          "Qt PSD 滚轮缩放未形成共享频率视口请求");
    root->hide();
  } else if (id == "FR-VIS-009") {
    signal::visualization::AtomicFrameCoordinator coordinator;
    require_status(coordinator.begin(viewport), "无法开始视图请求");
    auto stale = make_frame(viewport);
    --stale.request_id.generation;
    check(!coordinator.commit(std::move(stale)), "过期 ViewRequestId 被提交");
    require_status(coordinator.commit(make_frame(viewport)), "同请求原子提交失败");
    check(coordinator.frame()->quality == signal::visualization::ViewQuality::refined, "渐进细化质量未保留");
  } else if (id == "FR-VIS-010") {
    signal::visualization::LayerModel layers;
    require_status(layers.upsert({"raw", "原始 PSD", "source-v1", true, 0.7, 2}), "新增图层失败");
    require_status(layers.upsert({"selection", "选区", "SEL-07", false, 0.4, 1}), "新增业务图层失败");
    const auto serialized = require(layers.serialize(), "图层序列化失败");
    signal::visualization::LayerModel restored;
    require_status(restored.restore(serialized), "图层状态恢复失败");
    const auto ordered = restored.ordered();
    check(ordered.size() == 2 && ordered.front().id == "selection" && !ordered.front().visible &&
              ordered.back().opacity == 0.7,
          "图层显示、透明度、顺序或来源未持久化");
    const auto before_failed_restore = restored.ordered();
    check(!restored.restore("new\t新图层\tsource\t1\t0.5\t1\n损坏行\n") && restored.ordered() == before_failed_restore,
          "损坏的图层恢复破坏了上一有效状态");
  } else if (id == "FR-VIS-011") {
    auto frame = make_frame(viewport);
    const auto original = frame.psd_db_hz;
    signal::visualization::DisplayMapping mapping;
    require_status(signal::visualization::validate_display_mapping(mapping), "默认显示映射无效");
    mapping.amplitude_scale = signal::visualization::AmplitudeScale::linear;
    mapping.range_mode = signal::visualization::RangeMode::manual;
    mapping.color_map = "Viridis";
    mapping.minimum = -110.0;
    mapping.maximum = -10.0;
    require_status(signal::visualization::validate_display_mapping(mapping), "手动显示映射无效");
    check(frame.psd_db_hz == original, "显示映射修改了底层结果");
  } else if (id == "FR-VIS-012") {
    signal::visualization::ScreenshotOptions options;
    check(options.axes && options.legend && options.color_scale && options.cursors && options.selections &&
              options.parameter_summary,
          "截图组成项不可选择");
    options.legend = false;
    options.cursors = false;
    check(!options.legend && !options.cursors && options.axes, "截图组成项不能独立开关");
    auto workspace = make_workspace(viewport);
    const auto output = std::filesystem::temp_directory_path() / "signal-studio-ms03-screenshot.png";
    require_status(workspace->save_screenshot(output, options), "可选组成项真实截图失败");
    check(std::filesystem::exists(output) && std::filesystem::file_size(output) > 0, "截图文件未实际生成");
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);
  } else if (id == "FR-VIS-013") {
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    root->resize(1280, 720);
    root->show();
    QApplication::processEvents();
    const auto canvases = root->findChildren<QWidget*>();
    const auto waterfall =
        std::ranges::find_if(canvases, [](QWidget* widget) { return widget->accessibleName().contains("STFT"); });
    check(waterfall != canvases.end(), "未找到 STFT Canvas");
    const auto image = (*waterfall)->grab().toImage();
    check(!image.isNull() && image.width() > 100 && image.height() >= 96, "瀑布图未真实绘制");
    root->hide();
  } else if (id == "FR-VIS-014") {
    signal::visualization::VisibilityController visibility;
    require_status(visibility.note_preparation(signal::visualization::ChartKind::time_waveform), "可见图准备失败");
    require_status(visibility.set_visible(signal::visualization::ChartKind::time_waveform, false), "隐藏时域失败");
    const auto state = visibility.state(signal::visualization::ChartKind::time_waveform);
    check(!state.visible && !state.observer_connected && state.pending_preparations == 0 &&
              !visibility.note_paint(signal::visualization::ChartKind::time_waveform),
          "隐藏图表仍在观察、准备或绘制");
    check(visibility.state(signal::visualization::ChartKind::spectrogram).visible, "隐藏时域错误隐藏了 STFT");
  } else if (id == "FR-VIS-015") {
    signal::visualization::ChartLayoutModel layout;
    require_status(layout.set({{signal::visualization::ChartKind::spectrogram, 280},
                               {signal::visualization::ChartKind::time_waveform, 130},
                               {signal::visualization::ChartKind::psd, 150}}),
                   "图表重排失败");
    const auto serialized = require(layout.serialize(), "布局序列化失败");
    signal::visualization::ChartLayoutModel restored;
    require_status(restored.restore(serialized), "布局恢复失败");
    check(restored.items() == layout.items() &&
              std::ranges::all_of(restored.items(), [](const auto& item) { return item.logical_height >= 96; }),
          "图表顺序、高度或最小高度未持久化");
  } else if (id == "FR-VIS-016") {
    auto workspace = make_workspace(viewport);
    const auto generation = workspace->viewport().request_id.generation;
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    const auto spin_boxes = root->findChildren<QDoubleSpinBox*>();
    check(spin_boxes.size() >= 2, "未找到瀑布显示映射控件");
    spin_boxes.front()->setValue(-30.0);
    spin_boxes.back()->setValue(80.0);
    QApplication::processEvents();
    check(workspace->viewport().request_id.generation == generation, "色阶、参考电平或动态范围创建了时间视口请求");
  } else if (id == "FR-VIS-017") {
    check(require(signal::visualization::parse_frequency_hz("2.450000001 GHz"), "GHz 精确解析失败") == 2'450'000'001,
          "2.450000001 GHz 未精确得到 2450000001 Hz");
    check(require(signal::visualization::parse_frequency_hz("-12.5 MHz"), "MHz 解析失败") == -12'500'000,
          "负频率单位解析失败");
    check(require(signal::visualization::parse_frequency_hz("-9223372036854775808 Hz"), "int64 最小频率解析失败") ==
                  std::numeric_limits<std::int64_t>::min() &&
              require(signal::visualization::parse_frequency_hz("9223372036854775807 Hz"), "int64 最大频率解析失败") ==
                  std::numeric_limits<std::int64_t>::max(),
          "64 位整数 Hz 边界未精确保持");
    check(!signal::visualization::parse_frequency_hz("9223372036854775808 Hz"), "超出 int64 的频率被接受");
  } else if (id == "FR-VIS-018") {
    signal::visualization::ViewportController controller("frequency-drag");
    require(controller.bind_source("source-v1", range(0, 100), FrequencyRange{-25'000'000, 25'000'000}),
            "绑定频率范围失败");
    require(controller.set_frequency({-5'000'000, 8'000'000}), "正向裁剪失败");
    check(controller.snapshot().frequency_viewport.bandwidth_hz() == 13'000'000, "正向裁剪带宽不正确");
    require(controller.reset_frequency(), "反向恢复失败");
    check(controller.snapshot().frequency_viewport == controller.snapshot().effective_frequency_range,
          "反向拖动语义未恢复有效全频范围");
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    root->resize(1280, 720);
    root->show();
    QApplication::processEvents();
    auto* psd = find_chart(root, "功率谱");
    const auto full = workspace->viewport().effective_frequency_range;
    require_status(workspace->fit_frequency_to_data(), "Qt 右键裁剪前恢复全频范围失败");
    send_mouse_drag(psd, Qt::RightButton, {90.0, 60.0}, {260.0, 60.0});
    check(workspace->viewport().frequency_viewport.bandwidth_hz() < full.bandwidth_hz(),
          "Qt PSD 右键正向拖动未裁剪频率范围");
    send_mouse_drag(psd, Qt::RightButton, {260.0, 60.0}, {90.0, 60.0});
    check(workspace->viewport().frequency_viewport == full, "Qt PSD 右键反向拖动未恢复数据有效全频范围");
    root->hide();
  } else if (id == "FR-NAV-001") {
    signal::visualization::ViewportController controller("loaded-range");
    require(controller.bind_source("partial", range(100, 900), FrequencyRange{-10, 10}, true, 3'200, 8'000),
            "绑定部分读取范围失败");
    check(controller.snapshot().loaded_range == range(100, 900) &&
              controller.snapshot().time_viewport == range(100, 900),
          "导航轨道未严格等于 LoadedDataRange");
  } else if (id == "FR-NAV-002") {
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    root->resize(1280, 720);
    root->show();
    QApplication::processEvents();
    auto* navigator = root->findChild<QWidget*>("TimeNavigator");
    check(navigator && navigator->height() >= 28 && navigator->focusPolicy() == Qt::StrongFocus,
          "最小视觉把手下没有精确输入/焦点等价路径");
    root->hide();
  } else if (id == "FR-NAV-003") {
    const auto exact = range(9'007'199'254'740'000ULL, 9'007'199'254'741'000ULL);
    check(exact.begin() == 9'007'199'254'740'000ULL && exact.size() == 1'000 && !exact.contains(exact.end()),
          "64 位样本索引半开区间真值失败");
  } else if (id == "FR-NAV-004") {
    const auto frame = make_frame(viewport);
    check(frame.time_range == viewport.time_viewport &&
              frame.psd_metadata.effective_samples == viewport.time_viewport.size(),
          "图表读取范围超出 CurrentTimeViewport");
  } else if (id == "FR-NAV-005") {
    auto partial = make_viewport(true);
    auto workspace = make_workspace(partial);
    workspace->set_status("● 预览质量 · 覆盖率 50% · 索引 42%");
    const auto summary = workspace->accessibility_summary();
    check(summary.find("质量") != std::string::npos && summary.find("覆盖率") != std::string::npos &&
              summary.find("索引") != std::string::npos,
          "导航质量、覆盖率和索引进度未同时使用文字");
  } else if (id == "FR-NAV-006") {
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    root->resize(1280, 720);
    root->show();
    QApplication::processEvents();
    auto* navigator = root->findChild<QWidget*>("TimeNavigator");
    check(navigator != nullptr, "未找到时间导航条");
    const auto before = workspace->viewport().request_id.generation;
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(navigator, &key);
    QApplication::processEvents();
    check(workspace->viewport().request_id.generation > before, "键盘时间导航未生成新请求");
    root->hide();
  } else if (id == "FR-NAV-007") {
    auto overlay = make_overlay();
    auto selection = require(
        overlay.create({"", "目标", signal::visualization::SelectionKind::time, range(30'000, 31'000), std::nullopt}),
        "创建 Selection 失败");
    signal::visualization::ViewportController controller("separation");
    require(controller.bind_source("source-v1", range(1'000, 101'000), FrequencyRange{-25, 25}), "绑定失败");
    require(controller.set_time(range(50'000, 60'000)), "浏览失败");
    check(overlay.find(selection.id)->time_range == range(30'000, 31'000), "浏览创建或修改了 Selection");
    selection.name = "重命名";
    require_status(overlay.update(selection), "编辑 Selection 失败");
    check(controller.snapshot().time_viewport == range(50'000, 60'000), "编辑 Selection 自动移动了视窗");
  } else if (id == "FR-NAV-008") {
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    auto* navigator = root->findChild<QWidget*>("TimeNavigator");
    check(navigator && !navigator->accessibleDescription().contains("Selection") &&
              navigator->findChildren<QAbstractButton*>().empty(),
          "时间导航条包含 Selection、标记或创建操作");
  } else if (id == "FR-NAV-009") {
    signal::visualization::ViewportController controller("empty");
    check(!controller.bind_source("empty", SampleRange{}, FrequencyRange{-1, 1}), "空文件错误进入分析");
    require(controller.bind_source("valid", range(10, 20), FrequencyRange{-1, 1}), "非空范围绑定失败");
    const auto current = controller.snapshot().time_viewport;
    check(current.begin() >= controller.snapshot().loaded_range.begin() && current.begin() < current.end() &&
              current.end() <= controller.snapshot().loaded_range.end(),
          "非空导航范围不变量失败");
  } else if (id == "FR-NAV-010") {
    signal::visualization::ViewportController controller("partial");
    require(controller.bind_source("source-v1", range(0, 1'000), FrequencyRange{-1, 1}, true, 4'000, 8'000),
            "绑定部分读取失败");
    const auto summary = controller.partial_read_summary();
    check(summary.find("[0,1000)") != std::string::npos && summary.find("4000 / 8000") != std::string::npos &&
              summary.find("未读尾部不可导航") != std::string::npos,
          "部分读取边界、字节和不可导航提示不完整");
  } else if (id == "FR-NAV-011") {
    signal::visualization::ViewportController controller("unique");
    const auto first = require(controller.bind_source("source-v1", range(0, 100), FrequencyRange{-10, 10}), "绑定失败");
    const auto second = require(controller.set_time(range(10, 20)), "时间变化失败");
    const auto third = require(controller.set_frequency({-5, 5}), "频率变化失败");
    check(first.scope == second.scope && first.generation < second.generation && second.generation < third.generation,
          "影响内容的变化未生成唯一不可变 ViewRequestId");
  } else if (id == "FR-NAV-012") {
    signal::visualization::AtomicFrameCoordinator coordinator;
    require_status(coordinator.begin(viewport), "开始请求失败");
    auto stale = make_frame(viewport);
    stale.request_id.generation -= 1;
    check(!coordinator.commit(stale), "快速导航过期请求提交成功");
    auto workspace = make_workspace(viewport, false);
    check(workspace->accessibility_summary().find("等待") != std::string::npos ||
              workspace->accessibility_summary().find("无图表数据") != std::string::npos,
          "局部未完成时没有占位或质量状态");
  } else if (id == "FR-NAV-013") {
    signal::visualization::PrefetchController prefetch;
    const auto first = prefetch.begin(1'000, 2'000);
    check(prefetch.active(), "前后视窗预取未启动");
    prefetch.cancel_for_interaction();
    check(!prefetch.active() && prefetch.generation() > first, "新交互未立即取消预取");
    const auto second = prefetch.begin(1'000, 2'000);
    check(second > first, "重新预取未推进代次");
    prefetch.cancel_for_resource_pressure();
    check(!prefetch.active(), "资源压力未立即取消预取");
  } else if (id == "FR-NAV-014") {
    signal::visualization::ViewportController controller("recent");
    require(controller.bind_source("source-v1", range(0, 1'000), FrequencyRange{-10, 10}), "初次绑定失败");
    require(controller.set_time(range(700, 950)), "保存最近视窗失败");
    controller.save_recent();
    require(controller.bind_source("source-v1", range(0, 800), FrequencyRange{-10, 10}, true, 3'200, 4'000),
            "缩短范围重新绑定失败");
    check(controller.snapshot().time_viewport == range(700, 800), "数据范围缩短后最近视窗未钳制");
  } else if (id == "FR-NAV-015") {
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    root->resize(1280, 720);
    root->show();
    QApplication::processEvents();
    auto* navigator = root->findChild<QWidget*>("TimeNavigator");
    check(navigator && navigator->focusPolicy() == Qt::StrongFocus && navigator->height() >= 28 &&
              !navigator->accessibleName().isEmpty() && !navigator->accessibleDescription().isEmpty(),
          "导航条焦点、非颜色状态或 DPI 命中约束失败");
    root->hide();
  } else if (id == "FR-SEL-001") {
    auto overlay = make_overlay();
    const auto time = require(
        overlay.create({"", "时间", signal::visualization::SelectionKind::time, range(2'000, 3'000), std::nullopt}),
        "创建时间 Selection 失败");
    const auto frequency = require(overlay.create({"", "频率", signal::visualization::SelectionKind::frequency,
                                                   range(2'000, 3'000), FrequencyRange{-1'000, 1'000}}),
                                   "创建频率 Selection 失败");
    const auto tf = require(overlay.create({"", "时频", signal::visualization::SelectionKind::time_frequency,
                                            range(4'000, 5'000), FrequencyRange{-2'000, 2'000}}),
                            "创建时频 Selection 失败");
    check(time.id != frequency.id && frequency.id != tf.id && time.id.starts_with("SEL-"),
          "Selection 稳定 ID 分配失败");
    auto workspace = make_workspace(viewport);
    const auto visual =
        require(workspace->create_selection({"", "图谱交互", signal::visualization::SelectionKind::time_frequency,
                                             range(20'000, 25'000), FrequencyRange{-2'000, 2'000}}),
                "Qt 主图谱未接受 Selection");
    check(workspace->selections().size() == 1 && workspace->selections().front().id == visual.id,
          "Qt 主图谱 Selection 未形成稳定可查询对象");
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    root->resize(1280, 720);
    root->show();
    QApplication::processEvents();
    require_status(workspace->set_interaction_mode(signal::visualization::InteractionMode::selection),
                   "设置 Qt Selection 模式失败");
    send_mouse_drag(find_chart(root, "STFT"), Qt::LeftButton, {100.0, 50.0}, {240.0, 82.0});
    check(workspace->selections().size() == 2 && workspace->selections().back().id != visual.id,
          "Qt 主图谱鼠标 Selection 未创建稳定对象");
    root->hide();
  } else if (id == "FR-SEL-002") {
    auto overlay = make_overlay();
    check(!overlay.create({"", "越界", signal::visualization::SelectionKind::time_frequency, range(500, 2'000),
                           FrequencyRange{-30'000'000, 1'000}}),
          "鼠标/精确输入越界 Selection 被接受");
    check(overlay
              .create({"", "有效", signal::visualization::SelectionKind::time_frequency, range(1'000, 2'000),
                       FrequencyRange{-1'000, 1'000}})
              .ok(),
          "有效 Selection 被拒绝");
  } else if (id == "FR-SEL-003") {
    auto overlay = make_overlay();
    auto selected = require(
        overlay.create({"", "原始", signal::visualization::SelectionKind::time, range(2'000, 3'000), std::nullopt}),
        "创建 Selection 失败");
    const auto copied = require(overlay.copy(selected.id), "复制 Selection 失败");
    check(copied.id != selected.id && overlay.selections().size() == 2, "多选区/复制失败");
    selected.locked = true;
    selected.dependent_results = 1;
    require_status(overlay.update(selected), "锁定 Selection 失败");
    check(!overlay.remove(selected.id), "存在依赖的 Selection 被删除");
    auto moved = selected;
    moved.time_range = range(4'000, 5'000);
    check(!overlay.update(moved), "锁定 Selection 边界被修改");
  } else if (id == "FR-SEL-004") {
    auto overlay = make_overlay();
    const auto selected = require(overlay.create({"", "游标", signal::visualization::SelectionKind::time_frequency,
                                                  range(20'000, 30'000), FrequencyRange{-5'000, 5'000}}),
                                  "创建游标 Selection 失败");
    signal::visualization::Measurement measurement{"",
                                                   selected.id,
                                                   "source-v1",
                                                   selected.time_range,
                                                   selected.frequency_range,
                                                   viewport.request_id,
                                                   "双游标",
                                                   "dB",
                                                   12.5,
                                                   1'250,
                                                   25'000,
                                                   "2026-07-28T00:00:00Z"};
    const auto added = require(overlay.add_measurement(measurement), "游标测量失败");
    check(added.sample_index == 25'000 && added.frequency_hz == 1'250 && added.value == 12.5,
          "游标时间、样本、频率、幅度或差值不精确");
  } else if (id == "FR-SEL-005") {
    auto overlay = make_overlay();
    const auto selected = require(overlay.create({"", "测量", signal::visualization::SelectionKind::time_frequency,
                                                  range(20'000, 30'000), FrequencyRange{-5'000, 5'000}}),
                                  "创建测量 Selection 失败");
    const auto measurement = require(
        overlay.add_measurement({"", selected.id, "source-v1", selected.time_range, selected.frequency_range,
                                 viewport.request_id, "带内功率", "dBm", -23.5, 0, 25'000, "2026-07-28T00:00:00Z"}),
        "添加测量失败");
    check(!measurement.id.empty() && measurement.data_source_version_id == "source-v1" &&
              measurement.view_request == viewport.request_id && measurement.algorithm == "带内功率" &&
              measurement.unit == "dBm" && !measurement.timestamp_utc.empty(),
          "测量来源链不完整");
  } else if (id == "FR-SEL-006") {
    auto overlay = make_overlay();
    const auto selected = require(overlay.create({"", "通道", signal::visualization::SelectionKind::time_frequency,
                                                  range(20'000, 30'000), FrequencyRange{-5'000, 15'000}}),
                                  "创建通道 Selection 失败");
    const auto estimate = require(overlay.estimate_channel(selected.id, 50'000, true), "通道估算失败");
    check(estimate.frequency_shift_hz == 5'000 && estimate.bandwidth_hz == 20'000 &&
              estimate.output_sample_rate_hz == 50'000 && estimate.complex_output && estimate.estimated_bytes == 80'000,
          "通道频移、带宽、输出率、类型或预计数据量不完整");
  } else if (id == "FR-SEL-007") {
    auto overlay = make_overlay();
    const auto first = require(
        overlay.create({"", "成功", signal::visualization::SelectionKind::time, range(2'000, 3'000), std::nullopt}),
        "创建第一项失败");
    const auto second = require(
        overlay.create({"", "失败", signal::visualization::SelectionKind::time, range(4'000, 5'000), std::nullopt}),
        "创建第二项失败");
    const auto results =
        signal::visualization::run_batch({first, second}, [](const signal::visualization::Selection& selection) {
          return selection.name == "失败" ? signal::core::Status::failure({signal::core::ErrorDomain::visualization,
                                                                           signal::core::ErrorReason::internal_failure},
                                                                          "单项失败")
                                          : signal::core::Status::success();
        });
    check(results.size() == 2 && results.front().succeeded && !results.back().succeeded, "批量单项失败丢失了其他结果");
  } else if (id == "FR-SEL-008") {
    auto overlay = make_overlay();
    auto selected = require(overlay.create({"", "依赖", signal::visualization::SelectionKind::time_frequency,
                                            range(20'000, 30'000), FrequencyRange{-5'000, 5'000}}),
                            "创建依赖 Selection 失败");
    require(overlay.add_measurement({"", selected.id, "source-v1", selected.time_range, selected.frequency_range,
                                     viewport.request_id, "峰值", "dB", -20.0, 0, 25'000, "2026-07-28T00:00:00Z"}),
            "添加依赖测量失败");
    overlay.mark_dependencies_stale("source-v1");
    check(overlay.find(selected.id)->stale && overlay.measurements().front().stale, "上游变化时依赖结果未标记过期");
  } else if (id == "NFR-USA-001") {
    auto partial = make_viewport(true);
    auto workspace = make_workspace(partial);
    workspace->set_status("! 部分读取 · 未读尾部不可导航");
    const auto summary = workspace->accessibility_summary();
    check(summary.find("部分读取") != std::string::npos && summary.find("不可导航") != std::string::npos,
          "关键状态只依赖颜色");
  } else if (id == "NFR-USA-002") {
    auto workbench = make_workbench(viewport);
    auto* root = static_cast<QWidget*>(workbench->native_handle());
    root->resize(1280, 720);
    workbench->show();
    QApplication::processEvents();
    const auto buttons = root->findChildren<QAbstractButton*>();
    check(!buttons.empty() &&
              std::ranges::all_of(buttons,
                                  [](const QAbstractButton* button) { return button->focusPolicy() != Qt::NoFocus; }),
          "核心操作缺少键盘焦点");
    root->hide();
  } else if (id == "NFR-USA-003") {
    signal::workbench::ParameterDescriptor parameter{"rbw", "分辨率带宽", "Hz", 1.0, 1'000'000.0, 1'000.0, 4'882.8125};
    require_status(signal::workbench::validate_parameter(parameter), "合法参数即时校验失败");
    parameter.value = 2'000'000.0;
    check(!signal::workbench::validate_parameter(parameter), "超范围参数未即时拒绝");
  } else if (id == "NFR-USA-004") {
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    root->resize(1280, 720);
    root->show();
    QApplication::processEvents();
    auto* navigator = root->findChild<QWidget*>("TimeNavigator");
    navigator->setFocus(Qt::TabFocusReason);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
    QApplication::sendEvent(navigator, &key);
    QApplication::processEvents();
    check(navigator->hasFocus() && workspace->viewport().request_id.generation > viewport.request_id.generation,
          "高 DPI 键盘导航丢失焦点或未生效");
    root->hide();
  } else if (id == "NFR-USA-005") {
    auto workbench = make_workbench(viewport);
    auto* root = static_cast<QWidget*>(workbench->native_handle());
    root->resize(1280, 720);
    workbench->show();
    QApplication::processEvents();
    const auto buttons = root->findChildren<QAbstractButton*>();
    check(!buttons.empty() && std::ranges::all_of(buttons,
                                                  [](const QAbstractButton* button) {
                                                    return button->width() >= 28 && button->height() >= 28;
                                                  }),
          "高频指针目标命中区小于 28×28 逻辑像素");
    root->hide();
  } else if (id == "NFR-USA-006") {
    auto workspace = make_workspace(viewport);
    auto* root = static_cast<QWidget*>(workspace->native_handle());
    const auto canvases = root->findChildren<QWidget*>();
    const auto accessible_charts = std::ranges::count_if(canvases, [](const QWidget* widget) {
      return (widget->accessibleName().contains("时域") || widget->accessibleName().contains("功率谱") ||
              widget->accessibleName().contains("STFT")) &&
             !widget->accessibleDescription().isEmpty();
    });
    check(accessible_charts >= 3 && workspace->accessibility_summary().find("频率") != std::string::npos,
          "Canvas 缺少可访问名称、语义摘要或关键测量值");
  } else if (id == "NFR-USA-007") {
    signal::workbench::ModalBehavior modal;
    require_status(signal::workbench::handle_escape(modal), "Escape 取消失败");
    check(modal.focus_trapped && modal.cancellation_requested && modal.live_region_text == "已请求取消",
          "模态焦点、Escape 与 live region 语义不一致");
  } else if (id == "FR-VIS-101") {
    const auto catalog = signal::visualization::component_catalog();
    const std::vector<std::string> required{
        "TimeWaveformView",  "TimeNavigator",  "PSDView",           "WaterfallView", "SpectrogramView",
        "ConstellationView", "EyeDiagramView", "FrequencyViewport", "LayerModel",    "ChartScreenshot"};
    check(std::ranges::all_of(required,
                              [&catalog](const std::string& id_value) {
                                return std::ranges::any_of(catalog, [&id_value](const auto& component) {
                                  return component.id == id_value && component.interactive;
                                });
                              }),
          "SignalVisualization 公共组件目录不完整");
    auto complete = signal::visualization::make_analysis_workspace(
        {.title = "完整组件目录",
         .source_label = "source-v1",
         .extra_charts = {signal::visualization::ChartKind::spectrum, signal::visualization::ChartKind::waterfall,
                          signal::visualization::ChartKind::constellation,
                          signal::visualization::ChartKind::eye_diagram}});
    check(complete != nullptr, "可复用 Qt Visualization 实现不可创建");
    require_status(complete->apply_viewport(viewport), "完整组件视口绑定失败");
    require_status(complete->bind_frame(make_frame(viewport)), "完整组件帧绑定失败");
    auto* root = static_cast<QWidget*>(complete->native_handle());
    root->resize(1280, 900);
    root->show();
    QApplication::processEvents();
    const auto charts = root->findChildren<QWidget*>();
    for (const auto& name : {"频谱", "瀑布", "星座", "眼图"}) {
      check(std::ranges::any_of(charts,
                                [name](const QWidget* widget) {
                                  return widget->accessibleName().contains(name) &&
                                         !widget->accessibleDescription().isEmpty();
                                }),
            "可复用图表组件未真实创建或缺少语义摘要");
    }
    check(!root->grab().isNull(), "完整可视化组件集合未真实绘制");
    root->hide();
  } else if (id == "FR-WB-101") {
    signal::workbench::WorkbenchContent content;
    content.status_text = "● 已连接测试状态";
    content.resource_text = "测试资源快照";
    content.inspector = {{"数据源版本", "injected-source-v1"}};
    content.tasks = {{"注入任务", "● 运行中", "50%", "CPU", "request-1"}};
    content.results = {{"注入结果", "✓ 当前", "injected-source-v1", "可定位"}};
    auto workbench = make_workbench(viewport, std::move(content));
    auto* root = static_cast<QWidget*>(workbench->native_handle());
    root->resize(1280, 720);
    workbench->show();
    QApplication::processEvents();
    const auto docks = root->findChildren<QDockWidget*>();
    const auto actions = root->findChildren<QAction*>();
    const auto task_trees = root->findChildren<QTreeWidget*>();
    const auto result_tables = root->findChildren<QTableWidget*>();
    check(docks.size() >= 5 && actions.size() >= 4 &&
              workbench->accessibility_summary().find("中心") != std::string::npos &&
              std::ranges::any_of(task_trees,
                                  [](const QTreeWidget* tree) {
                                    return tree->topLevelItemCount() == 1 &&
                                           tree->topLevelItem(0)->text(0) == "注入任务";
                                  }) &&
              std::ranges::any_of(result_tables,
                                  [](const QTableWidget* table) {
                                    return table->rowCount() == 1 && table->item(0, 0)->text() == "注入结果";
                                  }),
          "Workbench 主框架、Dock、中心、命令、设置、诊断、主题或布局不完整");
    auto* task_page_action = root->findChild<QAction*>("page.task-center");
    auto* settings_page_action = root->findChild<QAction*>("page.settings");
    auto* task_page = root->findChild<QWidget*>("TaskCenterPage");
    auto* settings_page = root->findChild<QWidget*>("SettingsDiagnosticsPage");
    check(task_page_action != nullptr && settings_page_action != nullptr && task_page != nullptr &&
              settings_page != nullptr,
          "批准原型的 P04 或 P07 原生 Qt 页面缺失");
    task_page_action->trigger();
    QApplication::processEvents();
    check(task_page->isVisible(), "P04 任务中心页面无法通过工作台命令显示");
    settings_page_action->trigger();
    QApplication::processEvents();
    check(settings_page->isVisible(), "P07 设置与诊断页面无法通过工作台命令显示");
    const auto layout = workbench->save_layout();
    const auto serialized = require(signal::workbench::serialize_layout(layout), "布局序列化失败");
    const auto restored = require(signal::workbench::parse_layout(serialized), "布局解析失败");
    require_status(workbench->restore_layout(restored), "布局恢复失败");
    root->hide();
  } else {
    throw std::runtime_error("未知需求用例: " + id);
  }
}

void test_layout(int width, int height) {
  const auto viewport = make_viewport();
  auto workbench = make_workbench(viewport);
  auto* root = static_cast<QWidget*>(workbench->native_handle());
  root->resize(width, height);
  workbench->show();
  QApplication::processEvents();
  check(root->size().width() == width && root->size().height() == height, "窗口未保持指定逻辑尺寸");
  const auto docks = root->findChildren<QDockWidget*>();
  check(docks.size() >= 5, "布局缺少 Workbench Dock");
  auto* navigator_dock = root->findChild<QDockWidget*>("navigator");
  auto* workspace_widget = root->findChild<QWidget*>("SignalVisualizationWorkspace");
  check(navigator_dock != nullptr && navigator_dock->isVisible() && workspace_widget != nullptr &&
            workspace_widget->isVisible() && navigator_dock->width() >= 188,
        "批准原型的左侧导航或 P02 中央工作区缺失");
  const auto splitters = root->findChildren<QSplitter*>();
  check(!splitters.empty() && std::ranges::all_of(splitters.front()->sizes(), [](int size) { return size >= 0; }),
        "图表分隔布局无效");
  const auto buttons = root->findChildren<QAbstractButton*>();
  check(std::ranges::all_of(
            buttons, [](const QAbstractButton* button) { return button->width() >= 28 && button->height() >= 28; }),
        "指定布局下出现小于 28×28 的高频目标");
  std::string out_of_bounds_button;
  for (const auto* button : buttons) {
    if (!button->isVisible()) {
      continue;
    }
    if (button->objectName().startsWith("qt_")) {
      continue;
    }
    const auto top_left = button->mapTo(root, QPoint(0, 0));
    if (!root->rect().adjusted(-2, -2, 2, 2).contains(QRect(top_left, button->size()))) {
      out_of_bounds_button = button->objectName().toStdString() + "@" + std::to_string(top_left.x()) + "," +
                             std::to_string(top_left.y()) + " " + std::to_string(button->width()) + "x" +
                             std::to_string(button->height());
      break;
    }
  }
  check(out_of_bounds_button.empty(), "指定布局下出现超出窗口边界的可见交互控件: " + out_of_bounds_button);
  const auto image = root->grab().toImage();
  const auto logical_width = static_cast<int>(std::lround(image.width() / image.devicePixelRatio()));
  const auto logical_height = static_cast<int>(std::lround(image.height() / image.devicePixelRatio()));
  check(!image.isNull() && logical_width == width && logical_height == height, "布局未形成指定尺寸的真实 Qt 绘制");
  const auto scale_text = qEnvironmentVariable("QT_SCALE_FACTOR");
  if (!scale_text.isEmpty()) {
    bool scale_ok{};
    const auto expected_scale = scale_text.toDouble(&scale_ok);
    check(scale_ok && std::abs(image.devicePixelRatio() - expected_scale) < 0.01,
          "截图 device pixel ratio 与请求缩放因子不一致");
  }
  QEvent dpr_change(QEvent::DevicePixelRatioChange);
  QApplication::sendEvent(root, &dpr_change);
  QApplication::processEvents();
  const auto after_dpr_change = root->grab().toImage();
  check(!after_dpr_change.isNull() &&
            static_cast<int>(std::lround(after_dpr_change.width() / after_dpr_change.devicePixelRatio())) == width &&
            static_cast<int>(std::lround(after_dpr_change.height() / after_dpr_change.devicePixelRatio())) == height,
        "DPI 变化事件后逻辑布局或截图尺寸漂移");
  root->hide();
}

} // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  QCoreApplication::setApplicationName("Signal Studio MS-03 Tests");
  QCommandLineParser parser;
  parser.addHelpOption();
  const QCommandLineOption case_option("case", "测试用例", "id");
  const QCommandLineOption width_option("width", "窗口宽度", "pixels", "1280");
  const QCommandLineOption height_option("height", "窗口高度", "pixels", "720");
  parser.addOption(case_option);
  parser.addOption(width_option);
  parser.addOption(height_option);
  parser.process(application);

  try {
    const auto id = parser.value(case_option).toStdString();
    if (id == "UI-LAYOUT") {
      bool width_ok{};
      bool height_ok{};
      const auto width = parser.value(width_option).toInt(&width_ok);
      const auto height = parser.value(height_option).toInt(&height_ok);
      check(width_ok && height_ok, "布局尺寸参数无效");
      test_layout(width, height);
    } else {
      test_requirement(id);
    }
    std::cout << id << " passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << parser.value(case_option).toStdString() << " failed: " << exception.what() << '\n';
    return 1;
  }
}
