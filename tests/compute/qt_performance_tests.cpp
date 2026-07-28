#include "signal_studio/compute/compute.hpp"
#include "signal_studio/dsp/browse_performance.hpp"

#include <QApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace signal;
using Clock = std::chrono::steady_clock;

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string{message});
}

template <typename T> void require(const core::Result<T>& result, std::string_view message) {
  require(static_cast<bool>(result), message);
}

class PerformanceCanvas final : public QWidget {
public:
  explicit PerformanceCanvas(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setWindowTitle(QStringLiteral("Signal Studio MS-02 Qt 性能验收"));
  }

  void apply_feedback(dsp::BrowseCommandFeedback feedback) {
    feedback_ = std::move(feedback);
    update();
  }

  void apply_frame(std::shared_ptr<const dsp::AtomicBrowseFrame> frame) {
    frame_ = std::move(frame);
    update();
  }

  [[nodiscard]] std::uint64_t paint_count() const noexcept {
    return paint_count_;
  }
  [[nodiscard]] std::uint64_t visible_feedback_generation() const noexcept {
    return visible_feedback_generation_;
  }
  [[nodiscard]] std::uint64_t visible_frame_generation() const noexcept {
    return visible_frame_generation_;
  }
  [[nodiscard]] bool three_views_complete() const noexcept {
    return time_count_ > 0U && spectrum_count_ > 0U && stft_count_ > 0U;
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter{this};
    painter.fillRect(rect(), QColor{18, 22, 30});
    painter.setPen(QColor{220, 230, 242});
    painter.drawText(QRect{12, 6, width() - 24, 24}, Qt::AlignLeft | Qt::AlignVCenter,
                     QString::fromUtf8(feedback_.status_text));
    if (feedback_.visible)
      visible_feedback_generation_ = feedback_.generation;

    time_count_ = 0U;
    spectrum_count_ = 0U;
    stft_count_ = 0U;
    if (frame_ && frame_->complete) {
      const QRect time_rect{12, 36, width() - 24, 140};
      const QRect spectrum_rect{12, 188, width() - 24, 140};
      const QRect stft_rect{12, 340, width() - 24, height() - 352};
      painter.setPen(QColor{80, 100, 130});
      painter.drawRect(time_rect);
      painter.drawRect(spectrum_rect);
      painter.drawRect(stft_rect);
      draw_line(painter, time_rect, frame_->time_envelope, QColor{80, 210, 180});
      draw_line(painter, spectrum_rect, frame_->spectrum_db_per_hz, QColor{245, 180, 70});
      draw_spectrogram(painter, stft_rect, *frame_);
      time_count_ = frame_->time_envelope.size();
      spectrum_count_ = frame_->spectrum_db_per_hz.size();
      stft_count_ = frame_->spectrogram_db_per_hz.size();
      visible_frame_generation_ = frame_->generation;
    }
    ++paint_count_;
  }

private:
  static void draw_line(QPainter& painter, const QRect& area, const std::vector<float>& values, QColor color) {
    if (values.size() < 2U)
      return;
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    const auto span = std::max(1.0e-6F, *maximum - *minimum);
    painter.setPen(QPen{color, 1.0});
    QPointF previous;
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto x = area.left() + static_cast<double>(index) * area.width() / static_cast<double>(values.size() - 1U);
      const auto normalized = (values[index] - *minimum) / span;
      const auto y = area.bottom() - static_cast<double>(normalized) * area.height();
      const QPointF current{x, y};
      if (index != 0U)
        painter.drawLine(previous, current);
      previous = current;
    }
  }

  static void draw_spectrogram(QPainter& painter, const QRect& area, const dsp::AtomicBrowseFrame& frame) {
    if (frame.spectrogram_columns == 0U || frame.spectrogram_rows == 0U ||
        frame.spectrogram_db_per_hz.size() !=
            static_cast<std::size_t>(frame.spectrogram_columns) * frame.spectrogram_rows)
      return;
    const auto cell_width = static_cast<double>(area.width()) / frame.spectrogram_columns;
    const auto cell_height = static_cast<double>(area.height()) / frame.spectrogram_rows;
    painter.setPen(Qt::NoPen);
    for (std::uint32_t row = 0; row < frame.spectrogram_rows; ++row) {
      for (std::uint32_t column = 0; column < frame.spectrogram_columns; ++column) {
        const auto value =
            frame.spectrogram_db_per_hz[static_cast<std::size_t>(row) * frame.spectrogram_columns + column];
        const auto normalized = std::clamp((value + 150.0F) / 150.0F, 0.0F, 1.0F);
        painter.setBrush(QColor::fromHsvF(0.70F - 0.70F * normalized, 0.85F, 0.25F + 0.75F * normalized));
        painter.drawRect(QRectF{area.left() + column * cell_width, area.top() + row * cell_height,
                                std::max(1.0, cell_width), std::max(1.0, cell_height)});
      }
    }
  }

  dsp::BrowseCommandFeedback feedback_;
  std::shared_ptr<const dsp::AtomicBrowseFrame> frame_;
  std::uint64_t paint_count_{};
  std::uint64_t visible_feedback_generation_{};
  std::uint64_t visible_frame_generation_{};
  std::size_t time_count_{};
  std::size_t spectrum_count_{};
  std::size_t stft_count_{};
};

core::Result<compute::PerformanceSummary> measure(std::size_t samples, const std::function<bool(std::size_t)>& body) {
  std::vector<compute::PerformanceSample> observations;
  observations.reserve(samples);
  for (std::size_t sample = 0; sample < samples; ++sample) {
    const auto start = Clock::now();
    if (!body(sample))
      return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::internal_failure},
                                   "Qt 专项样本执行失败");
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    observations.push_back({std::max(std::chrono::microseconds{1}, latency), 0U});
  }
  return compute::summarize_performance(observations);
}

dsp::BrowseViewport viewport(std::uint64_t begin = 0U) {
  return {data::SampleRange::from_count(begin, 16'384U).value(), 512U, 32U};
}

bool same_viewport(const dsp::BrowseViewport& left, const dsp::BrowseViewport& right) {
  return left.logical_range == right.logical_range && left.pixel_width == right.pixel_width &&
         left.spectrogram_rows == right.spectrogram_rows;
}

dsp::BrowseViewport distributed_viewport(const dsp::LogicalRecordingSource& source, std::uint64_t numerator,
                                         std::uint64_t denominator) {
  constexpr std::uint64_t window_frames = 16'384U;
  require(denominator != 0U && numerator <= denominator && source.plan().logical_frames >= window_frames,
          "Qt 分布式视窗参数无效");
  const auto maximum_begin = source.plan().logical_frames - window_frames;
  const auto quotient = maximum_begin / denominator;
  const auto remainder = maximum_begin % denominator;
  return viewport(quotient * numerator + remainder * numerator / denominator);
}

std::shared_ptr<dsp::AtomicBrowseFrame> synthetic_frame(const dsp::BrowseCommandFeedback& feedback) {
  auto frame = std::make_shared<dsp::AtomicBrowseFrame>();
  frame->generation = feedback.generation;
  frame->viewport = feedback.viewport;
  frame->time_envelope.resize(512U);
  frame->spectrum_db_per_hz.resize(512U);
  frame->spectrogram_columns = 128U;
  frame->spectrogram_rows = 24U;
  frame->spectrogram_db_per_hz.resize(static_cast<std::size_t>(frame->spectrogram_columns) * frame->spectrogram_rows);
  for (std::size_t index = 0; index < frame->time_envelope.size(); ++index) {
    frame->time_envelope[index] = static_cast<float>(std::sin(index * 0.03));
    frame->spectrum_db_per_hz[index] = -120.0F + 30.0F * static_cast<float>(std::cos(index * 0.02));
  }
  for (std::size_t index = 0; index < frame->spectrogram_db_per_hz.size(); ++index)
    frame->spectrogram_db_per_hz[index] = static_cast<float>(-145.0 + (index % 128U) * 0.4);
  frame->source_scope = "Qt 实际绘制性能专项";
  frame->complete = true;
  return frame;
}

void show_canvas(PerformanceCanvas& canvas) {
  canvas.resize(900, 600);
  canvas.show();
  // offscreen 与原生 Windows 平台均需要有界事件泵完成 polish/show/expose。
  // 原生窗口的首个 WM_PAINT 可能晚于 show() 返回，最多等待 1 秒，不让测试永久挂起。
  const auto deadline = Clock::now() + std::chrono::seconds{1};
  do {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    canvas.repaint();
    if (canvas.isVisible() && canvas.paint_count() > 0U)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  } while (Clock::now() < deadline);
  require(canvas.isVisible() && canvas.paint_count() > 0U, "Qt QWidget 未进入可见且实际绘制状态");
}

void test_feedback(PerformanceCanvas& canvas) {
  dsp::BrowseInteractionSequencer interaction;
  const auto summary = measure(60U, [&](std::size_t sample) {
    const auto feedback = interaction.issue(viewport(sample * 64U));
    const auto before = canvas.paint_count();
    canvas.apply_feedback(feedback);
    canvas.repaint();
    return canvas.paint_count() > before && canvas.visible_feedback_generation() == feedback.generation;
  });
  require(summary && summary.value().sample_count == 60U && summary.value().p95 <= std::chrono::milliseconds{50},
          "QWidget 普通命令可见 paintEvent 反馈 P95 超过 50 ms");
}

void test_continuous_paint(PerformanceCanvas& canvas) {
  dsp::BrowseInteractionSequencer interaction;
  const auto start = Clock::now();
  for (std::size_t sample = 0; sample < 120U; ++sample) {
    const auto feedback = interaction.issue(viewport(sample * 64U));
    auto frame = synthetic_frame(feedback);
    require(interaction.publish(frame), "Qt 连续交互未发布当前代际");
    canvas.apply_feedback(feedback);
    canvas.apply_frame(interaction.frame());
    canvas.repaint();
    require(canvas.visible_frame_generation() == feedback.generation && canvas.three_views_complete(),
            "Qt 连续交互未实际绘制完整三图");
  }
  const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  const auto fps = 120.0 / std::max(elapsed, 1.0e-9);
  require(fps >= 30.0, "QWidget 三图连续实际 paintEvent 低于 30 FPS");
  std::cout << "qt_actual_paint_fps=" << fps << '\n';
}

data::SignalDescriptor recording_descriptor() {
  data::SignalDescriptor descriptor;
  descriptor.signal_kind = data::SignalKind::complex;
  descriptor.scalar_type = data::ScalarType::int16;
  descriptor.component_layout = data::ComponentLayout::interleaved;
  descriptor.component_order = data::ComponentOrder::iq;
  descriptor.endianness = data::Endianness::little;
  descriptor.sample_rate_hz = 50'000'000.0;
  descriptor.center_frequency_hz = 1'425'000'000.0;
  descriptor.requested_sample_range = data::SampleRange::from_count(0U, 4096U).value();
  return descriptor;
}

void test_atomic_three_views(PerformanceCanvas& canvas) {
  auto source = dsp::LogicalRecordingSource::open(
      std::filesystem::path{SIGNAL_STUDIO_MS02_PERF_RECORDING}, recording_descriptor(),
      "x310-1425MHz-50MSps-sha256-" SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256, 100'000'000'000ULL,
      256ULL * 1024ULL * 1024ULL);
  require(source, "Qt 三图专项无法打开真实 X310 录制");
  auto backend = dsp::make_cpu_fft_backend();
  require(backend, "Qt 三图专项 oneMKL 后端不可用");
  const auto cache = std::filesystem::temp_directory_path() /
                     ("signal-studio-ms02-qt-cache-" + std::to_string(Clock::now().time_since_epoch().count()));
  auto session = dsp::BrowsePerformanceSession::create(source.value(), backend.value(), cache);
  require(session, "Qt 三图专项浏览会话创建失败");
  std::array<dsp::BrowseViewport, 60U> hot_viewports;
  for (std::size_t index = 0U; index < hot_viewports.size(); ++index) {
    hot_viewports[index] = distributed_viewport(*source.value(), index + 1U, hot_viewports.size() + 1U);
    require(session.value()->build_frame(hot_viewports[index], index + 1U), "Qt 三图专项多视窗缓存准备失败");
  }
  dsp::BrowseInteractionSequencer interaction;
  const auto stale_feedback = interaction.issue(hot_viewports[0]);
  const auto stale_frame = session.value()->restore_cached_frame(hot_viewports[0], stale_feedback.generation);
  require(stale_frame, "Qt 三图专项无法准备旧代际结果");
  const auto current_feedback = interaction.issue(hot_viewports[1]);
  require(!interaction.publish(stale_frame.value()) &&
              same_viewport(interaction.feedback().viewport, current_feedback.viewport),
          "Qt 三图专项未拒绝视窗变化前的旧代际结果");

  const auto hot = measure(60U, [&](std::size_t index) {
    const auto& actual_viewport = hot_viewports[index];
    const auto feedback = interaction.issue(actual_viewport);
    const auto frame = session.value()->restore_cached_frame(actual_viewport, feedback.generation);
    if (!frame || !same_viewport(frame.value()->viewport, feedback.viewport) || !interaction.publish(frame.value()))
      return false;
    canvas.apply_feedback(feedback);
    canvas.apply_frame(interaction.frame());
    canvas.repaint();
    return canvas.visible_frame_generation() == feedback.generation && canvas.three_views_complete();
  });
  const auto cold = measure(30U, [&](std::size_t index) {
    const auto actual_viewport = distributed_viewport(*source.value(), index * 2U + 1U, 60U);
    const auto feedback = interaction.issue(actual_viewport);
    const auto frame = session.value()->build_frame(actual_viewport, feedback.generation, false);
    if (!frame || !same_viewport(frame.value()->viewport, feedback.viewport) || !interaction.publish(frame.value()))
      return false;
    canvas.apply_feedback(feedback);
    canvas.apply_frame(interaction.frame());
    canvas.repaint();
    return canvas.visible_frame_generation() == feedback.generation && canvas.three_views_complete();
  });
  std::error_code remove_error;
  std::filesystem::remove_all(cache, remove_error);
  require(hot && cold && hot.value().p95 <= std::chrono::milliseconds{150} &&
              cold.value().p95 <= std::chrono::seconds{1},
          "真实 X310 视窗变化至 QWidget 三图同代际实际绘制延迟超限");
}

} // namespace

int main(int argc, char** argv) {
  QApplication application{argc, argv};
  application.setQuitOnLastWindowClosed(false);
  try {
    if (argc == 2 && std::string_view{argv[1]} == "--startup-smoke") {
      require(QGuiApplication::platformName() == QStringLiteral("windows"),
              "清空 Qt 插件环境后未由旁置 platforms/qwindows[d].dll 初始化 Windows 平台");
      std::cout << "Qt默认Windows平台插件直接启动 PASS\n";
      return 0;
    }
    if (argc != 3 || std::string_view{argv[1]} != "--case")
      throw std::runtime_error("用法：signal_studio_ms02_qt_performance_tests --case <需求编号>");
    PerformanceCanvas canvas;
    show_canvas(canvas);
    const std::map<std::string_view, std::function<void(PerformanceCanvas&)>> cases{
        {"NFR-PERF-001", test_feedback},
        {"NFR-PERF-002", test_continuous_paint},
        {"NFR-PERF-010", test_atomic_three_views},
    };
    const auto found = cases.find(argv[2]);
    if (found == cases.end())
      throw std::runtime_error("未知 Qt 性能需求编号");
    found->second(canvas);
    std::cout << argv[2] << " Qt实际可见绘制 PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
