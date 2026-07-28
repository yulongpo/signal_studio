#include "signal_studio/visualization/visualization.hpp"
#include "signal_studio/workbench/workbench.hpp"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using signal::visualization::FrequencyRange;
using signal::visualization::VisualizationFrame;

[[nodiscard]] VisualizationFrame make_demo_frame(const signal::visualization::ViewportSnapshot& viewport) {
  VisualizationFrame frame;
  frame.request_id = viewport.request_id;
  frame.time_range = viewport.time_viewport;
  frame.frequency_range = viewport.frequency_viewport;
  frame.quality = signal::visualization::ViewQuality::refined;
  frame.time_mode = signal::visualization::TimeDisplayMode::in_phase_quadrature;
  frame.spectrum_layout = signal::visualization::SpectrumLayout::shifted_two_sided;
  frame.data_source_version_id = viewport.data_source_version_id;
  frame.absolute_frequency = true;
  frame.center_frequency_hz = 1'245'000'000;

  constexpr std::size_t time_points = 1'600;
  frame.time_primary.resize(time_points);
  frame.time_secondary.resize(time_points);
  for (std::size_t index = 0; index < time_points; ++index) {
    const auto x = static_cast<double>(index) / static_cast<double>(time_points - 1U);
    frame.time_primary[index] = 0.10 * std::sin(x * 190.0) + 0.38 * std::sin(x * 3.4 + 0.25);
    frame.time_secondary[index] = 0.08 * std::cos(x * 167.0) + 0.36 * std::sin(x * 3.4 + 1.18);
  }

  constexpr std::size_t psd_points = 1'024;
  frame.psd_db_hz.resize(psd_points);
  const auto gaussian = [](double x, double center, double width, double height) {
    const auto normalized = (x - center) / width;
    return height * std::exp(-0.5 * normalized * normalized);
  };
  for (std::size_t index = 0; index < psd_points; ++index) {
    const auto x = static_cast<double>(index) / static_cast<double>(psd_points - 1U);
    frame.psd_db_hz[index] = -82.0 + 7.0 * std::sin(x * 83.0) + gaussian(x, 0.16, 0.018, 54.0) +
                             gaussian(x, 0.43, 0.025, 45.0) + gaussian(x, 0.77, 0.032, 37.0) +
                             gaussian(x, 0.91, 0.027, 32.0);
  }
  frame.psd_metadata.effective_samples = viewport.time_viewport.size();
  frame.psd_metadata.fft_frames = 1'220;
  frame.psd_metadata.fft_size = 4'096;
  frame.psd_metadata.window = "Hann";
  frame.psd_metadata.averaging = "Welch";
  frame.psd_metadata.rbw_hz = 4'882.8125;

  frame.stft_rows = 150;
  frame.stft_columns = 480;
  frame.stft_db.resize(static_cast<std::size_t>(frame.stft_rows) * frame.stft_columns);
  for (std::uint32_t row = 0; row < frame.stft_rows; ++row) {
    const auto time = static_cast<double>(row) / static_cast<double>(frame.stft_rows - 1U);
    const auto ridge_a = 0.26 + 0.14 * std::sin(time * 5.8);
    const auto ridge_b = 0.72 + 0.16 * std::sin(time * 5.8 + 2.4);
    for (std::uint32_t column = 0; column < frame.stft_columns; ++column) {
      const auto frequency = static_cast<double>(column) / static_cast<double>(frame.stft_columns - 1U);
      const auto distance_a = (frequency - ridge_a) / 0.035;
      const auto distance_b = (frequency - ridge_b) / 0.045;
      const auto horizontal = 7.0 * std::sin(time * 61.0) * std::exp(-std::pow((frequency - 0.5) / 0.35, 2));
      frame.stft_db[static_cast<std::size_t>(row) * frame.stft_columns + column] =
          static_cast<float>(-96.0 + 72.0 * std::exp(-0.5 * distance_a * distance_a) +
                             60.0 * std::exp(-0.5 * distance_b * distance_b) + horizontal);
    }
  }
  frame.stft_metadata.window_size = 1'024;
  frame.stft_metadata.hop_size = 256;
  frame.stft_metadata.fft_size = 1'024;
  frame.stft_metadata.overlap_ratio = 0.75;
  frame.stft_metadata.color_map = "Industrial";
  frame.stft_metadata.interpolation = "nearest";

  constexpr std::size_t constellation_points = 160;
  for (std::size_t index = 0; index < constellation_points; ++index) {
    const auto quadrant = index % 4U;
    const auto jitter = 0.025 * std::sin(static_cast<double>(index) * 2.17);
    frame.constellation_i.push_back((quadrant < 2U ? -0.65 : 0.65) + jitter);
    frame.constellation_q.push_back((quadrant % 2U == 0U ? -0.65 : 0.65) - jitter);
  }
  constexpr std::size_t eye_points = 512;
  for (std::size_t index = 0; index < eye_points; ++index) {
    const auto phase = static_cast<double>(index % 128U) / 127.0;
    frame.eye_trace.push_back(std::tanh((phase - 0.5) * 8.0) * (index / 128U % 2U == 0U ? 1.0 : -1.0));
  }
  return frame;
}

class DemoDiagnostics final : public signal::workbench::IDiagnosticsProvider {
public:
  [[nodiscard]] signal::workbench::DiagnosticsSnapshot snapshot() const override {
    return {"2026-07-28T00:00:00Z",
            {{"Qt", qVersion(), "✓ 可用", "查看插件"},
             {"平台", QGuiApplication::platformName().toStdString(), "✓ 当前", "复制诊断"},
             {"Compute", "CPU / CUDA 可选", "● 就绪", "重新探测"},
             {"缓存", "逻辑视图缓存", "● 健康", "打开目录"}}};
  }
};

} // namespace

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  QCoreApplication::setApplicationName("Signal Platform Visualization Demo");
  QCoreApplication::setApplicationVersion("1.0");

  QCommandLineParser parser;
  parser.setApplicationDescription("SignalVisualization 与 SignalWorkbench 可复用 Qt 演示");
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption screenshot_option("screenshot", "将当前工作台截图保存到指定路径", "path");
  const QCommandLineOption width_option("width", "窗口宽度", "pixels", "1280");
  const QCommandLineOption height_option("height", "窗口高度", "pixels", "720");
  const QCommandLineOption startup_smoke("startup-smoke", "仅验证平台插件和窗口构造");
  parser.addOption(screenshot_option);
  parser.addOption(width_option);
  parser.addOption(height_option);
  parser.addOption(startup_smoke);
  parser.process(application);

  signal::visualization::ViewportController controller("ms03-demo");
  const auto loaded = signal::data::SampleRange::make(0, 250'000'000);
  const auto current = signal::data::SampleRange::make(60'000'000, 85'000'000);
  if (!loaded || !current ||
      !controller.bind_source("x310-demo@v1", loaded.value(), FrequencyRange{1'220'000'000, 1'270'000'000}) ||
      !controller.set_time(current.value())) {
    std::cerr << "无法建立演示视口\n";
    return 2;
  }

  auto workspace =
      signal::visualization::make_analysis_workspace({.title = "宽带视窗", .source_label = "x310-demo@v1"});
  if (!workspace || !workspace->apply_viewport(controller.snapshot())) {
    std::cerr << "无法创建 Qt 可视化工作区\n";
    return 3;
  }
  auto frame = make_demo_frame(controller.snapshot());
  if (!workspace->bind_frame(std::move(frame))) {
    std::cerr << "无法原子挂载演示三图\n";
    return 4;
  }
  const auto demo_selection = workspace->create_selection(
      {"", "演示目标突发", signal::visualization::SelectionKind::time_frequency,
       signal::data::SampleRange::make(68'000'000, 81'000'000).value(), FrequencyRange{1'249'000'000, 1'260'000'000}});
  if (!demo_selection) {
    std::cerr << "无法建立演示 Selection\n";
    return 5;
  }

  auto commands = std::make_shared<signal::workbench::CommandRegistry>();
  auto panels = std::make_shared<signal::workbench::PanelRegistry>();
  auto diagnostics = std::make_shared<DemoDiagnostics>();
  signal::workbench::WorkbenchConfiguration configuration{.application_name = "Signal Platform",
                                                          .window_title = "可视化工作台"};
  configuration.content.status_text = "● 就绪 · CPU · 缓存 0%";
  configuration.content.resource_text = "线程 4 · 内存 1.6 GB";
  configuration.content.inspector = {{"数据源版本", "x310-demo@v1"},
                                     {"时间范围", "[60,000,000, 85,000,000)"},
                                     {"频率范围", "1.220 GHz — 1.270 GHz"},
                                     {"单位", "dB/Hz"},
                                     {"窗函数", "Hann"},
                                     {"RBW", "4.88 kHz"}};
  configuration.content.tasks = {{"视图细化", "● 运行中", "64%", "CPU", "VR-000184"},
                                 {"频谱概要", "✓ 完成", "100%", "CPU", "x310-demo@v1"}};
  configuration.content.results = {{"SEL-1 目标突发", "● 当前", "x310-demo@v1", "可定位"},
                                   {"批量测量 02", "! 已过期", "x310-demo@v0", "查看来源"}};
  auto window = signal::workbench::make_workbench_window(std::move(configuration), std::move(workspace), commands,
                                                         panels, diagnostics);
  if (!window) {
    std::cerr << "无法创建 Qt Workbench\n";
    return 6;
  }
  auto* native_window = static_cast<QWidget*>(window->native_handle());
  bool width_ok{};
  bool height_ok{};
  const auto width = parser.value(width_option).toInt(&width_ok);
  const auto height = parser.value(height_option).toInt(&height_ok);
  if (!width_ok || !height_ok || width < 960 || height < 640) {
    std::cerr << "截图窗口尺寸无效\n";
    return 7;
  }
  native_window->resize(width, height);
  window->show();

  if (parser.isSet(startup_smoke)) {
    std::cout << "platform=" << QGuiApplication::platformName().toStdString()
              << " panels=" << window->visible_panels().size() << '\n';
    QTimer::singleShot(0, &application, &QCoreApplication::quit);
  } else if (parser.isSet(screenshot_option)) {
    const auto path = QFileInfo(parser.value(screenshot_option)).absoluteFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QTimer::singleShot(250, native_window, [native_window, path, &application] {
      const auto screenshot = native_window->grab();
      if (!screenshot.save(path, "PNG")) {
        application.exit(8);
        return;
      }
      application.quit();
    });
  }

  return application.exec();
}
