#include "signal_studio/visualization/visualization.hpp"

#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtGui/QActionGroup>
#include <QtGui/QFocusEvent>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace signal::visualization {
namespace {

constexpr auto canvas_color = "#08111F";
constexpr auto panel_color = "#0E1B2D";
constexpr auto grid_color = "#203A55";
constexpr auto primary_text = "#E5F1FF";
constexpr auto muted_text = "#8FA8C2";
constexpr auto accent_cyan = "#20D3EE";
constexpr auto accent_magenta = "#D563DC";
constexpr auto selection_yellow = "#F2C94C";

[[nodiscard]] QRectF plot_rect(const QRect& bounds) {
  return QRectF(bounds).adjusted(50.0, 30.0, -14.0, -24.0);
}

[[nodiscard]] QColor heat_color(float value, double reference, double dynamic_range) {
  const auto rgb = [](double red, double green, double blue) {
    return QColor::fromRgbF(static_cast<float>(red), static_cast<float>(green), static_cast<float>(blue));
  };
  const auto normalized =
      std::clamp((static_cast<double>(value) - (reference - dynamic_range)) / dynamic_range, 0.0, 1.0);
  if (normalized < 0.34) {
    const auto ratio = normalized / 0.34;
    return rgb(0.02, 0.08 + 0.32 * ratio, 0.18 + 0.32 * ratio);
  }
  if (normalized < 0.72) {
    const auto ratio = (normalized - 0.34) / 0.38;
    return rgb(0.02 + 0.28 * ratio, 0.40 + 0.42 * ratio, 0.50 - 0.20 * ratio);
  }
  const auto ratio = (normalized - 0.72) / 0.28;
  return rgb(0.30 + 0.55 * ratio, 0.82 + 0.12 * ratio, 0.30 - 0.20 * ratio);
}

class TimeNavigatorWidget final : public QWidget {
public:
  explicit TimeNavigatorWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setObjectName("TimeNavigator");
    setAccessibleName("时间视窗导航条");
    setAccessibleDescription("轨道范围等于实际读入范围；支持方向键、PageUp、PageDown、Home 和 End");
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(52);
    setMaximumHeight(68);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }

  void setViewport(ViewportSnapshot viewport) {
    viewport_ = std::move(viewport);
    update();
  }

  void setChangeHandler(std::function<void(data::SampleRange, std::string)> change_handler) {
    change_handler_ = std::move(change_handler);
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(canvas_color));
    const QRectF track = QRectF(rect()).adjusted(12.0, 17.0, -12.0, -16.0);
    painter.setPen(QPen(QColor(grid_color), 1.0));
    painter.setBrush(QColor(panel_color));
    painter.drawRoundedRect(track, 3.0, 3.0);
    if (!viewport_.loaded_range.empty() && !viewport_.time_viewport.empty()) {
      const auto loaded_span = static_cast<double>(viewport_.loaded_range.size());
      const auto start_ratio =
          static_cast<double>(viewport_.time_viewport.begin() - viewport_.loaded_range.begin()) / loaded_span;
      const auto span_ratio = static_cast<double>(viewport_.time_viewport.size()) / loaded_span;
      const auto visual_width = std::max(28.0, track.width() * span_ratio);
      const auto available_x = std::max(0.0, track.width() - visual_width);
      const auto handle_x = track.left() + available_x * start_ratio;
      handle_ = QRectF(handle_x, track.top() - 2.0, visual_width, track.height() + 4.0);
      painter.setPen(QPen(QColor(accent_cyan), hasFocus() ? 2.0 : 1.0));
      painter.setBrush(QColor(32, 211, 238, 70));
      painter.drawRoundedRect(handle_, 3.0, 3.0);
      painter.fillRect(QRectF(handle_.left(), handle_.top(), 5.0, handle_.height()), QColor(accent_cyan));
      painter.fillRect(QRectF(handle_.right() - 5.0, handle_.top(), 5.0, handle_.height()), QColor(accent_cyan));
    }
    painter.setPen(QColor(muted_text));
    painter.drawText(QRectF(12.0, 1.0, width() - 24.0, 15.0), Qt::AlignLeft | Qt::AlignVCenter,
                     viewport_.partial_read ? "已解析时间导航 · 部分读取 · 未读尾部不可导航"
                                            : "已解析时间导航 · 实际读入范围");
    const auto left_text = QString::number(viewport_.loaded_range.begin());
    const auto right_text = QString::number(viewport_.loaded_range.end());
    painter.drawText(QRectF(12.0, height() - 15.0, width() / 2.0, 14.0), Qt::AlignLeft | Qt::AlignVCenter, left_text);
    painter.drawText(QRectF(width() / 2.0, height() - 15.0, width() / 2.0 - 12.0, 14.0),
                     Qt::AlignRight | Qt::AlignVCenter, right_text);
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && handle_.contains(event->position())) {
      constexpr auto edge_hit_width = 8.0;
      if (std::abs(event->position().x() - handle_.left()) <= edge_hit_width) {
        drag_mode_ = DragMode::resize_begin;
      } else if (std::abs(event->position().x() - handle_.right()) <= edge_hit_width) {
        drag_mode_ = DragMode::resize_end;
      } else {
        drag_mode_ = DragMode::move;
      }
      drag_origin_ = event->position().x();
      drag_start_ = viewport_.time_viewport.begin();
      drag_end_ = viewport_.time_viewport.end();
      setFocus(Qt::MouseFocusReason);
      event->accept();
      return;
    }
    QWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (drag_mode_ == DragMode::none || viewport_.loaded_range.empty()) {
      QWidget::mouseMoveEvent(event);
      return;
    }
    const auto track_width = std::max(1.0, static_cast<double>(width() - 24));
    const auto delta_ratio = (event->position().x() - drag_origin_) / track_width;
    const auto delta =
        static_cast<std::int64_t>(std::llround(delta_ratio * static_cast<double>(viewport_.loaded_range.size())));
    if (drag_mode_ == DragMode::move) {
      moveTo(drag_start_, delta, "鼠标平移");
    } else if (drag_mode_ == DragMode::resize_begin) {
      const auto minimum = viewport_.loaded_range.begin();
      const auto maximum = drag_end_ - 1U;
      std::uint64_t begin = drag_start_;
      if (delta < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-(delta + 1)) + 1U;
        begin = magnitude > begin - minimum ? minimum : begin - magnitude;
      } else {
        begin = std::min(maximum, begin + static_cast<std::uint64_t>(delta));
      }
      publish(begin, drag_end_, "鼠标调整起点");
    } else {
      const auto minimum = drag_start_ + 1U;
      const auto maximum = viewport_.loaded_range.end();
      std::uint64_t end = drag_end_;
      if (delta < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-(delta + 1)) + 1U;
        end = magnitude > end - minimum ? minimum : end - magnitude;
      } else {
        end = std::min(maximum, end + static_cast<std::uint64_t>(delta));
      }
      publish(drag_start_, end, "鼠标调整终点");
    }
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && drag_mode_ != DragMode::none) {
      drag_mode_ = DragMode::none;
      event->accept();
      return;
    }
    QWidget::mouseReleaseEvent(event);
  }

  void wheelEvent(QWheelEvent* event) override {
    if (viewport_.time_viewport.empty()) {
      event->ignore();
      return;
    }
    const auto current_span = viewport_.time_viewport.size();
    const auto factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
    const auto next_span =
        std::clamp(static_cast<std::uint64_t>(std::llround(static_cast<double>(current_span) * factor)),
                   std::uint64_t{1}, viewport_.loaded_range.size());
    const auto center = viewport_.time_viewport.begin() + viewport_.time_viewport.size() / 2U;
    const auto half = next_span / 2U;
    auto begin = center > half ? center - half : viewport_.loaded_range.begin();
    if (begin < viewport_.loaded_range.begin()) {
      begin = viewport_.loaded_range.begin();
    }
    if (begin > viewport_.loaded_range.end() - next_span) {
      begin = viewport_.loaded_range.end() - next_span;
    }
    publish(begin, begin + next_span, "滚轮缩放");
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (viewport_.time_viewport.empty()) {
      QWidget::keyPressEvent(event);
      return;
    }
    const auto span = viewport_.time_viewport.size();
    const auto step = std::max<std::uint64_t>(1U, span / 20U);
    switch (event->key()) {
    case Qt::Key_Left:
      moveTo(viewport_.time_viewport.begin(), -static_cast<std::int64_t>(step), "键盘左移");
      break;
    case Qt::Key_Right:
      moveTo(viewport_.time_viewport.begin(), static_cast<std::int64_t>(step), "键盘右移");
      break;
    case Qt::Key_PageUp:
      moveTo(viewport_.time_viewport.begin(), -static_cast<std::int64_t>(span), "向前页步进");
      break;
    case Qt::Key_PageDown:
      moveTo(viewport_.time_viewport.begin(), static_cast<std::int64_t>(span), "向后页步进");
      break;
    case Qt::Key_Home:
      publish(viewport_.loaded_range.begin(), viewport_.loaded_range.begin() + span, "定位到已读范围起点");
      break;
    case Qt::Key_End:
      publish(viewport_.loaded_range.end() - span, viewport_.loaded_range.end(), "定位到已读范围终点");
      break;
    default:
      QWidget::keyPressEvent(event);
      return;
    }
    event->accept();
  }

private:
  enum class DragMode : std::uint8_t { none, move, resize_begin, resize_end };

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void moveTo(std::uint64_t origin, std::int64_t delta, const char* reason) {
    const auto span = viewport_.time_viewport.size();
    const auto minimum = viewport_.loaded_range.begin();
    const auto maximum = viewport_.loaded_range.end() - span;
    std::uint64_t begin = origin;
    if (delta < 0) {
      const auto magnitude = static_cast<std::uint64_t>(-(delta + 1)) + 1U;
      begin = magnitude > origin - minimum ? minimum : origin - magnitude;
    } else {
      const auto magnitude = static_cast<std::uint64_t>(delta);
      begin = magnitude > maximum - origin ? maximum : origin + magnitude;
    }
    publish(begin, begin + span, reason);
  }

  void publish(std::uint64_t begin, std::uint64_t end, const char* reason) {
    const auto range = data::SampleRange::make(begin, end);
    if (range && change_handler_) {
      viewport_.time_viewport = range.value();
      change_handler_(range.value(), reason);
      update();
    }
  }

  ViewportSnapshot viewport_;
  QRectF handle_;
  DragMode drag_mode_{DragMode::none};
  double drag_origin_{};
  std::uint64_t drag_start_{};
  std::uint64_t drag_end_{};
  std::function<void(data::SampleRange, std::string)> change_handler_;
};

class SignalChartCanvas final : public QWidget {
public:
  explicit SignalChartCanvas(ChartKind kind, QString title, QWidget* parent = nullptr)
      : QWidget(parent), kind_(kind), title_(std::move(title)) {
    setObjectName(title_);
    setAccessibleName(title_);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(96);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    updateAccessibility();
  }

  void setFrame(const VisualizationFrame& frame) {
    frame_ = frame;
    if (!time_mode_selected_) {
      time_display_mode_ = frame.time_mode;
    }
    if (!spectrum_layout_selected_) {
      spectrum_layout_ = frame.spectrum_layout;
    }
    rebuildDerivedWaveform();
    if (kind_ == ChartKind::spectrogram) {
      rebuildHeatmap();
    }
    updateAccessibility();
    update();
  }

  void setViewport(const ViewportSnapshot& viewport) {
    viewport_ = viewport;
    updateAccessibility();
    update();
  }

  void setDisplayMapping(DisplayMapping mapping) {
    mapping_ = std::move(mapping);
    if (kind_ == ChartKind::spectrogram) {
      rebuildHeatmap();
    }
    update();
  }

  void setFrequencyHandler(std::function<void(FrequencyRange, bool, QString)> frequency_handler) {
    frequency_handler_ = std::move(frequency_handler);
  }

  void setInteractionMode(InteractionMode mode) {
    interaction_mode_ = mode;
    setCursor(mode == InteractionMode::pan      ? Qt::OpenHandCursor
              : mode == InteractionMode::cursor ? Qt::CrossCursor
                                                : Qt::ArrowCursor);
  }

  void setTimeDisplayMode(TimeDisplayMode mode) {
    time_display_mode_ = mode;
    time_mode_selected_ = true;
    rebuildDerivedWaveform();
    updateAccessibility();
    update();
  }

  void setSpectrumLayout(SpectrumLayout layout) {
    spectrum_layout_ = layout;
    spectrum_layout_selected_ = true;
    updateAccessibility();
    update();
  }

  void setSelectionHandler(std::function<void(Selection)> selection_handler) {
    selection_handler_ = std::move(selection_handler);
  }

  void setStatusHandler(std::function<void(QString)> status_handler) {
    status_handler_ = std::move(status_handler);
  }

  void setOverlays(std::vector<Selection> selections, std::vector<Measurement> measurements) {
    selections_ = std::move(selections);
    measurements_ = std::move(measurements);
    updateAccessibility();
    update();
  }

  void setScreenshotOptions(ScreenshotOptions options) {
    screenshot_options_ = options;
    update();
  }

  [[nodiscard]] ChartKind kind() const noexcept {
    return kind_;
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(canvas_color));
    if (screenshot_options_.legend || screenshot_options_.parameter_summary) {
      drawHeader(painter);
    }
    const auto area = plot_rect(rect());
    if (screenshot_options_.axes) {
      drawGrid(painter, area);
    }
    if (!frame_) {
      painter.setPen(QColor(muted_text));
      painter.drawText(area, Qt::AlignCenter, "等待同一 ViewRequestId 的原子三图结果");
    } else if (kind_ == ChartKind::time_waveform) {
      drawWaveform(painter, area);
    } else if (kind_ == ChartKind::psd || kind_ == ChartKind::spectrum) {
      drawPsd(painter, area);
    } else if (kind_ == ChartKind::spectrogram || kind_ == ChartKind::waterfall) {
      drawSpectrogram(painter, area);
    } else if (kind_ == ChartKind::constellation) {
      drawConstellation(painter, area);
    } else if (kind_ == ChartKind::eye_diagram) {
      drawEye(painter, area);
    }
    if (screenshot_options_.selections) {
      drawSelections(painter, area);
    }
    if (screenshot_options_.cursors) {
      drawCursors(painter, area);
    }
    if (screenshot_options_.axes) {
      drawAxes(painter, area);
    }
    if (screenshot_options_.color_scale && (kind_ == ChartKind::spectrogram || kind_ == ChartKind::waterfall)) {
      drawColorScale(painter, area);
    }
    if (drag_start_) {
      const QRectF selection = QRectF(*drag_start_, drag_current_).normalized().intersected(area);
      painter.setPen(QPen(QColor(selection_yellow), 1.0, Qt::DashLine));
      painter.setBrush(QColor(242, 201, 76, 35));
      painter.drawRect(selection);
      QString summary;
      if (kind_ == ChartKind::time_waveform) {
        const auto begin = sampleAt(selection.left(), area);
        const auto end = sampleAt(selection.right(), area);
        summary = QString("[%1,%2) · %3 样本").arg(begin).arg(end).arg(end - begin);
      } else {
        const auto frequency = frequencyAt(selection.left(), area);
        const auto end_frequency = frequencyAt(selection.right(), area);
        summary = QString("%1 → %2 · 带宽 %3")
                      .arg(QString::fromStdString(format_frequency_hz(frequency)))
                      .arg(QString::fromStdString(format_frequency_hz(end_frequency)))
                      .arg(QString::fromStdString(format_frequency_hz(std::abs(end_frequency - frequency))));
      }
      painter.setPen(QColor(primary_text));
      painter.drawText(selection.adjusted(4.0, 2.0, -4.0, -2.0), Qt::AlignTop | Qt::AlignLeft, summary);
    }
    if (hasFocus()) {
      painter.setPen(QPen(QColor(accent_cyan), 2.0));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(QRectF(rect()).adjusted(1.0, 1.0, -2.0, -2.0));
    }
  }

  void wheelEvent(QWheelEvent* event) override {
    if (!frequency_handler_ || viewport_.frequency_viewport.bandwidth_hz() <= 1) {
      event->ignore();
      return;
    }
    const auto area = plot_rect(rect());
    const auto anchor = frequencyAt(event->position().x(), area);
    const auto old_range = viewport_.frequency_viewport;
    const auto factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
    const auto next_bandwidth =
        std::clamp(static_cast<std::int64_t>(std::llround(static_cast<double>(old_range.bandwidth_hz()) * factor)),
                   std::int64_t{1}, viewport_.effective_frequency_range.bandwidth_hz());
    const auto anchor_ratio = std::clamp((event->position().x() - area.left()) / area.width(), 0.0, 1.0);
    auto begin = anchor - static_cast<std::int64_t>(std::llround(anchor_ratio * static_cast<double>(next_bandwidth)));
    begin = std::clamp(begin, viewport_.effective_frequency_range.begin_hz,
                       viewport_.effective_frequency_range.end_hz - next_bandwidth);
    const FrequencyRange next{begin, begin + next_bandwidth};
    frequency_handler_(next, false, "滚轮锚定缩放");
    event->accept();
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::RightButton && (kind_ == ChartKind::psd || kind_ == ChartKind::spectrum ||
                                               kind_ == ChartKind::spectrogram || kind_ == ChartKind::waterfall)) {
      setFocus(Qt::MouseFocusReason);
      drag_button_ = Qt::RightButton;
      drag_start_ = event->position();
      drag_current_ = event->position();
      update();
      event->accept();
      return;
    }
    if (event->button() == Qt::LeftButton && plot_rect(rect()).contains(event->position())) {
      setFocus(Qt::MouseFocusReason);
      if (interaction_mode_ == InteractionMode::pan && frequency_handler_ && kind_ != ChartKind::time_waveform) {
        pan_origin_x_ = event->position().x();
        pan_start_range_ = viewport_.frequency_viewport;
        setCursor(Qt::ClosedHandCursor);
      } else if (interaction_mode_ == InteractionMode::cursor) {
        if ((event->modifiers() & Qt::ShiftModifier) == 0 || cursors_.size() >= 2) {
          cursors_.clear();
        }
        cursors_.push_back(event->position());
        publishCursorSummary(event->position());
        updateAccessibility();
        update();
      } else {
        drag_button_ = Qt::LeftButton;
        drag_start_ = event->position();
        drag_current_ = event->position();
        update();
      }
      event->accept();
      return;
    }
    QWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (pan_start_range_ && frequency_handler_) {
      const auto area = plot_rect(rect());
      const auto delta_ratio = (event->position().x() - pan_origin_x_) / std::max(1.0, area.width());
      const auto delta_hz =
          static_cast<std::int64_t>(std::llround(-delta_ratio * static_cast<double>(pan_start_range_->bandwidth_hz())));
      const auto minimum = viewport_.effective_frequency_range.begin_hz;
      const auto maximum = viewport_.effective_frequency_range.end_hz - pan_start_range_->bandwidth_hz();
      const auto begin = std::clamp(pan_start_range_->begin_hz + delta_hz, minimum, maximum);
      frequency_handler_({begin, begin + pan_start_range_->bandwidth_hz()}, false, "鼠标拖动平移频率视口");
      event->accept();
      return;
    }
    if (drag_start_) {
      drag_current_ = event->position();
      update();
      event->accept();
      return;
    }
    QWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && pan_start_range_) {
      pan_start_range_.reset();
      setCursor(Qt::OpenHandCursor);
      event->accept();
      return;
    }
    if (event->button() == Qt::RightButton && drag_start_ && drag_button_ == Qt::RightButton) {
      const auto start_x = drag_start_->x();
      const auto end_x = event->position().x();
      const auto area = plot_rect(rect());
      if (frequency_handler_ && std::abs(end_x - start_x) >= 4.0) {
        if (end_x > start_x) {
          const auto begin = frequencyAt(start_x, area);
          const auto end = frequencyAt(end_x, area);
          if (begin < end) {
            frequency_handler_({begin, end}, false, "右键正向裁剪频率范围");
          }
        } else {
          frequency_handler_(viewport_.effective_frequency_range, true, "右键反向恢复有效全频范围");
        }
      }
      drag_start_.reset();
      drag_button_ = Qt::NoButton;
      update();
      event->accept();
      return;
    }
    if (event->button() == Qt::LeftButton && drag_start_ && drag_button_ == Qt::LeftButton) {
      const auto area = plot_rect(rect());
      const auto start = *drag_start_;
      const auto end = event->position();
      if (std::abs(end.x() - start.x()) >= 4.0 || std::abs(end.y() - start.y()) >= 4.0) {
        if (interaction_mode_ == InteractionMode::zoom && frequency_handler_ && kind_ != ChartKind::time_waveform) {
          const auto begin_frequency = frequencyAt(std::min(start.x(), end.x()), area);
          const auto end_frequency = frequencyAt(std::max(start.x(), end.x()), area);
          if (begin_frequency < end_frequency) {
            frequency_handler_({begin_frequency, end_frequency}, false, "框选缩放频率视口");
          }
        } else if (interaction_mode_ == InteractionMode::selection && selection_handler_) {
          selection_handler_(selectionFromDrag(start, end, area));
        }
      }
      drag_start_.reset();
      drag_button_ = Qt::NoButton;
      update();
      event->accept();
      return;
    }
    QWidget::mouseReleaseEvent(event);
  }

private:
  [[nodiscard]] std::uint64_t sampleAt(double x, const QRectF& area) const {
    if (viewport_.time_viewport.empty()) {
      return 0;
    }
    const auto ratio = std::clamp((x - area.left()) / std::max(1.0, area.width()), 0.0, 1.0);
    const auto offset =
        static_cast<std::uint64_t>(std::llround(ratio * static_cast<double>(viewport_.time_viewport.size())));
    return std::min(viewport_.time_viewport.end(), viewport_.time_viewport.begin() + offset);
  }

  [[nodiscard]] std::uint64_t sampleAtY(double y, const QRectF& area) const {
    if (viewport_.time_viewport.empty()) {
      return 0;
    }
    const auto ratio = std::clamp((y - area.top()) / std::max(1.0, area.height()), 0.0, 1.0);
    const auto offset =
        static_cast<std::uint64_t>(std::llround(ratio * static_cast<double>(viewport_.time_viewport.size())));
    return std::min(viewport_.time_viewport.end(), viewport_.time_viewport.begin() + offset);
  }

  [[nodiscard]] Selection selectionFromDrag(const QPointF& first, const QPointF& second, const QRectF& area) const {
    const auto time_from_x = [this, &area, &first, &second] {
      auto begin = sampleAt(std::min(first.x(), second.x()), area);
      auto end = sampleAt(std::max(first.x(), second.x()), area);
      if (end <= begin) {
        end = std::min(viewport_.time_viewport.end(), begin + 1U);
        if (end <= begin) {
          begin = begin == 0 ? 0 : begin - 1U;
        }
      }
      return data::SampleRange::make(begin, end).value();
    };
    if (kind_ == ChartKind::time_waveform) {
      return {"", "", SelectionKind::time, time_from_x(), std::nullopt};
    }
    auto begin_frequency = frequencyAt(std::min(first.x(), second.x()), area);
    auto end_frequency = frequencyAt(std::max(first.x(), second.x()), area);
    if (end_frequency <= begin_frequency) {
      if (begin_frequency < viewport_.frequency_viewport.end_hz) {
        end_frequency = begin_frequency + 1;
      } else {
        --begin_frequency;
      }
    }
    const auto frequency = FrequencyRange{begin_frequency, end_frequency};
    if (kind_ == ChartKind::psd || kind_ == ChartKind::spectrum) {
      return {"", "", SelectionKind::frequency, viewport_.time_viewport, frequency};
    }
    auto begin_sample = sampleAtY(std::min(first.y(), second.y()), area);
    auto end_sample = sampleAtY(std::max(first.y(), second.y()), area);
    if (end_sample <= begin_sample) {
      end_sample = std::min(viewport_.time_viewport.end(), begin_sample + 1U);
    }
    return {"", "", SelectionKind::time_frequency, data::SampleRange::make(begin_sample, end_sample).value(),
            frequency};
  }

  [[nodiscard]] double xForFrequency(std::int64_t frequency_hz, const QRectF& area) const {
    const auto span = std::max<std::int64_t>(1, viewport_.frequency_viewport.bandwidth_hz());
    const auto ratio =
        static_cast<double>(frequency_hz - viewport_.frequency_viewport.begin_hz) / static_cast<double>(span);
    return area.left() + std::clamp(ratio, 0.0, 1.0) * area.width();
  }

  [[nodiscard]] double xForSample(std::uint64_t sample, const QRectF& area) const {
    const auto span = std::max<std::uint64_t>(1U, viewport_.time_viewport.size());
    const auto offset = sample <= viewport_.time_viewport.begin() ? 0U : sample - viewport_.time_viewport.begin();
    return area.left() + std::clamp(static_cast<double>(offset) / static_cast<double>(span), 0.0, 1.0) * area.width();
  }

  [[nodiscard]] double yForSample(std::uint64_t sample, const QRectF& area) const {
    const auto span = std::max<std::uint64_t>(1U, viewport_.time_viewport.size());
    const auto offset = sample <= viewport_.time_viewport.begin() ? 0U : sample - viewport_.time_viewport.begin();
    return area.top() + std::clamp(static_cast<double>(offset) / static_cast<double>(span), 0.0, 1.0) * area.height();
  }

  void drawSelections(QPainter& painter, const QRectF& area) const {
    for (const auto& selection : selections_) {
      if (!selection.visible || selection.stale) {
        continue;
      }
      QRectF overlay;
      if (kind_ == ChartKind::time_waveform) {
        overlay = QRectF(QPointF(xForSample(selection.time_range.begin(), area), area.top()),
                         QPointF(xForSample(selection.time_range.end(), area), area.bottom()));
      } else if ((kind_ == ChartKind::psd || kind_ == ChartKind::spectrum) && selection.frequency_range) {
        overlay = QRectF(QPointF(xForFrequency(selection.frequency_range->begin_hz, area), area.top()),
                         QPointF(xForFrequency(selection.frequency_range->end_hz, area), area.bottom()));
      } else if ((kind_ == ChartKind::spectrogram || kind_ == ChartKind::waterfall) && selection.frequency_range) {
        overlay = QRectF(QPointF(xForFrequency(selection.frequency_range->begin_hz, area),
                                 yForSample(selection.time_range.begin(), area)),
                         QPointF(xForFrequency(selection.frequency_range->end_hz, area),
                                 yForSample(selection.time_range.end(), area)));
      } else {
        continue;
      }
      overlay = overlay.normalized().intersected(area);
      if (overlay.isEmpty()) {
        continue;
      }
      painter.setPen(QPen(QColor(selection_yellow), selection.locked ? 2.0 : 1.2,
                          selection.locked ? Qt::SolidLine : Qt::DashLine));
      painter.setBrush(QColor(242, 201, 76, 20));
      painter.drawRect(overlay);
      painter.setPen(QColor(selection_yellow));
      painter.drawText(overlay.adjusted(4.0, 2.0, -4.0, -2.0), Qt::AlignTop | Qt::AlignLeft,
                       QString::fromStdString(selection.id));
    }
  }

  void drawCursors(QPainter& painter, const QRectF& area) const {
    painter.setPen(QPen(QColor(accent_magenta), 1.0, Qt::DashLine));
    for (std::size_t index = 0; index < cursors_.size(); ++index) {
      const auto cursor = cursors_[index];
      painter.drawLine(QPointF(cursor.x(), area.top()), QPointF(cursor.x(), area.bottom()));
      painter.drawLine(QPointF(area.left(), cursor.y()), QPointF(area.right(), cursor.y()));
      painter.drawText(cursor + QPointF(4.0, -4.0), QString("C%1").arg(index + 1U));
    }
  }

  void drawColorScale(QPainter& painter, const QRectF& area) const {
    const QRectF scale(area.right() - 9.0, area.top(), 8.0, area.height());
    QLinearGradient gradient(scale.bottomLeft(), scale.topLeft());
    gradient.setColorAt(0.0, heat_color(static_cast<float>(mapping_.reference_level - mapping_.dynamic_range),
                                        mapping_.reference_level, mapping_.dynamic_range));
    gradient.setColorAt(0.5, heat_color(static_cast<float>(mapping_.reference_level - mapping_.dynamic_range / 2.0),
                                        mapping_.reference_level, mapping_.dynamic_range));
    gradient.setColorAt(1.0, heat_color(static_cast<float>(mapping_.reference_level), mapping_.reference_level,
                                        mapping_.dynamic_range));
    painter.fillRect(scale, gradient);
    painter.setPen(QColor(primary_text));
    painter.drawRect(scale);
  }

  void publishCursorSummary(const QPointF& point) {
    if (!status_handler_) {
      return;
    }
    const auto area = plot_rect(rect());
    QString summary;
    if (kind_ == ChartKind::time_waveform) {
      summary = QString("游标：样本 %1").arg(sampleAt(point.x(), area));
    } else if (kind_ == ChartKind::psd || kind_ == ChartKind::spectrum) {
      summary = QString("游标：%1").arg(QString::fromStdString(format_frequency_hz(frequencyAt(point.x(), area))));
    } else {
      summary = QString("游标：样本 %1 · %2")
                    .arg(sampleAtY(point.y(), area))
                    .arg(QString::fromStdString(format_frequency_hz(frequencyAt(point.x(), area))));
    }
    if (cursors_.size() == 2) {
      const auto delta_frequency = std::abs(frequencyAt(cursors_[1].x(), area) - frequencyAt(cursors_[0].x(), area));
      summary += QString(" · 双游标差值 %1").arg(QString::fromStdString(format_frequency_hz(delta_frequency)));
    }
    status_handler_(summary);
  }

  void drawHeader(QPainter& painter) const {
    painter.fillRect(QRectF(0.0, 0.0, width(), 26.0), QColor(panel_color));
    painter.setPen(QColor(accent_cyan));
    const auto chart_number = kind_ == ChartKind::time_waveform   ? "01"
                              : kind_ == ChartKind::psd           ? "02"
                              : kind_ == ChartKind::spectrum      ? "02"
                              : kind_ == ChartKind::spectrogram   ? "03"
                              : kind_ == ChartKind::waterfall     ? "03"
                              : kind_ == ChartKind::constellation ? "04"
                                                                  : "05";
    painter.drawText(QRectF(9.0, 0.0, 24.0, 26.0), Qt::AlignCenter, chart_number);
    painter.setPen(QColor(primary_text));
    painter.drawText(QRectF(34.0, 0.0, width() - 250.0, 26.0), Qt::AlignLeft | Qt::AlignVCenter, title_);
    painter.setPen(QColor(muted_text));
    const auto request =
        viewport_.request_id.scope.empty()
            ? QString("等待请求")
            : QString("VR-%1 · %2")
                  .arg(viewport_.request_id.generation, 6, 10, QLatin1Char('0'))
                  .arg(QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.begin_hz) + " — " +
                                              format_frequency_hz(viewport_.frequency_viewport.end_hz)));
    painter.drawText(QRectF(width() - 420.0, 0.0, 408.0, 26.0), Qt::AlignRight | Qt::AlignVCenter, request);
  }

  static void drawGrid(QPainter& painter, const QRectF& area) {
    painter.setPen(QPen(QColor(grid_color), 1.0));
    for (int index = 0; index <= 8; ++index) {
      const auto x = area.left() + area.width() * static_cast<double>(index) / 8.0;
      painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    for (int index = 0; index <= 4; ++index) {
      const auto y = area.top() + area.height() * static_cast<double>(index) / 4.0;
      painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
  }

  void drawWaveform(QPainter& painter, const QRectF& area) const {
    if (time_display_mode_ == TimeDisplayMode::magnitude) {
      drawSeries(painter, area, derived_waveform_, QColor(accent_cyan), 0.0, 1.5);
      return;
    }
    if (time_display_mode_ == TimeDisplayMode::phase) {
      drawSeries(painter, area, derived_waveform_, QColor(accent_magenta), -3.14159265358979323846,
                 3.14159265358979323846);
      return;
    }
    if (!frame_) {
      return;
    }
    const auto& frame = *frame_;
    drawSeries(painter, area, frame.time_primary, QColor(accent_cyan), -1.0, 1.0);
    if (time_display_mode_ == TimeDisplayMode::in_phase_quadrature && !frame.time_secondary.empty()) {
      drawSeries(painter, area, frame.time_secondary, QColor(accent_magenta), -1.0, 1.0);
    }
  }

  void rebuildDerivedWaveform() {
    derived_waveform_.clear();
    if (!frame_ || (time_display_mode_ != TimeDisplayMode::magnitude && time_display_mode_ != TimeDisplayMode::phase)) {
      return;
    }
    const auto count = frame_->time_secondary.empty()
                           ? frame_->time_primary.size()
                           : std::min(frame_->time_primary.size(), frame_->time_secondary.size());
    derived_waveform_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto primary = frame_->time_primary[index];
      const auto secondary = frame_->time_secondary.empty() ? 0.0 : frame_->time_secondary[index];
      derived_waveform_.push_back(time_display_mode_ == TimeDisplayMode::magnitude ? std::hypot(primary, secondary)
                                                                                   : std::atan2(secondary, primary));
    }
  }

  static void drawSeries(QPainter& painter, const QRectF& area, const std::vector<double>& values, const QColor& color,
                         double minimum, double maximum) {
    if (values.size() < 2 || maximum <= minimum) {
      return;
    }
    QPainterPath path;
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto x = area.left() + area.width() * static_cast<double>(index) / static_cast<double>(values.size() - 1U);
      const auto normalized = std::clamp((values[index] - minimum) / (maximum - minimum), 0.0, 1.0);
      const auto y = area.bottom() - normalized * area.height();
      if (index == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }
    painter.setPen(QPen(color, 1.35));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
  }

  void drawPsd(QPainter& painter, const QRectF& area) const {
    if (!frame_ || frame_->psd_db_hz.size() < 2) {
      return;
    }
    const auto& values = frame_->psd_db_hz;
    QPainterPath line;
    QPainterPath fill;
    fill.moveTo(area.left(), area.bottom());
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto x = area.left() + area.width() * static_cast<double>(index) / static_cast<double>(values.size() - 1U);
      const auto normalized =
          std::clamp((values[index] - mapping_.minimum) / (mapping_.maximum - mapping_.minimum), 0.0, 1.0);
      const auto y = area.bottom() - normalized * area.height();
      if (index == 0) {
        line.moveTo(x, y);
      } else {
        line.lineTo(x, y);
      }
      fill.lineTo(x, y);
    }
    fill.lineTo(area.right(), area.bottom());
    fill.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(32, 211, 238, 30));
    painter.drawPath(fill);
    painter.setPen(QPen(QColor(accent_cyan), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(line);
  }

  void drawSpectrogram(QPainter& painter, const QRectF& area) const {
    if (!heatmap_.isNull()) {
      painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
      painter.drawImage(area, heatmap_);
    }
  }

  void drawConstellation(QPainter& painter, const QRectF& area) const {
    if (!frame_) {
      return;
    }
    const auto& frame = *frame_;
    const auto reference_radius_x = area.width() * 0.42;
    const auto reference_radius_y = area.height() * 0.42;
    painter.setPen(QPen(QColor(muted_text), 1.0, Qt::DashLine));
    painter.drawLine(QPointF(area.center().x(), area.top()), QPointF(area.center().x(), area.bottom()));
    painter.drawLine(QPointF(area.left(), area.center().y()), QPointF(area.right(), area.center().y()));
    painter.drawEllipse(area.center(), reference_radius_x, reference_radius_y);
    painter.setPen(QPen(QColor(accent_cyan), 2.0));
    const auto count = std::min(frame.constellation_i.size(), frame.constellation_q.size());
    for (std::size_t index = 0; index < count; ++index) {
      const QPointF point(area.center().x() + frame.constellation_i[index] * area.width() * 0.42,
                          area.center().y() - frame.constellation_q[index] * area.height() * 0.42);
      painter.drawPoint(point);
    }
  }

  void drawEye(QPainter& painter, const QRectF& area) const {
    if (!frame_) {
      return;
    }
    drawSeries(painter, area, frame_->eye_trace, QColor(accent_cyan), -1.0, 1.0);
  }

  void drawAxes(QPainter& painter, const QRectF& area) const {
    painter.setPen(QColor(muted_text));
    if (kind_ == ChartKind::time_waveform) {
      painter.drawText(QRectF(area.left(), area.bottom() + 3.0, area.width(), 18.0), Qt::AlignCenter,
                       "当前时间视窗 · 64 位样本索引半开区间");
      painter.drawText(QRectF(3.0, area.top(), 44.0, area.height()), Qt::AlignTop | Qt::AlignRight, "幅度");
      return;
    }
    if (kind_ == ChartKind::constellation) {
      painter.drawText(QRectF(3.0, area.top(), 44.0, 18.0), Qt::AlignRight, "Q +1");
      painter.drawText(QRectF(3.0, area.bottom() - 18.0, 44.0, 18.0), Qt::AlignRight | Qt::AlignBottom, "Q -1");
      painter.drawText(QRectF(area.left(), area.bottom() + 3.0, area.width() / 2.0, 18.0), Qt::AlignLeft, "I -1");
      painter.drawText(QRectF(area.center().x(), area.bottom() + 3.0, area.width() / 2.0, 18.0), Qt::AlignRight,
                       "I +1");
      return;
    }
    if (kind_ == ChartKind::eye_diagram) {
      painter.drawText(QRectF(3.0, area.top(), 44.0, 18.0), Qt::AlignRight, "+1");
      painter.drawText(QRectF(3.0, area.bottom() - 18.0, 44.0, 18.0), Qt::AlignRight | Qt::AlignBottom, "-1");
      painter.drawText(QRectF(area.left(), area.bottom() + 3.0, area.width() / 2.0, 18.0), Qt::AlignLeft, "0 UI");
      painter.drawText(QRectF(area.center().x(), area.bottom() + 3.0, area.width() / 2.0, 18.0), Qt::AlignRight,
                       "2 UI");
      return;
    }
    if (kind_ == ChartKind::psd || kind_ == ChartKind::spectrum) {
      painter.drawText(QRectF(3.0, area.top(), 44.0, area.height()), Qt::AlignTop | Qt::AlignRight, "-20");
      painter.drawText(QRectF(3.0, area.bottom() - 18.0, 44.0, 18.0), Qt::AlignRight | Qt::AlignBottom, "-90");
      if (frame_ && screenshot_options_.parameter_summary) {
        painter.drawText(QRectF(area.left(), area.top() + 2.0, area.width() - 8.0, 18.0), Qt::AlignRight,
                         QString("FFT %1 · %2 · %3 · RBW %4 Hz · %5")
                             .arg(frame_->psd_metadata.fft_size)
                             .arg(QString::fromStdString(frame_->psd_metadata.window))
                             .arg(QString::fromStdString(frame_->psd_metadata.averaging))
                             .arg(frame_->psd_metadata.rbw_hz, 0, 'f', 2)
                             .arg(QString::fromStdString(frame_->psd_metadata.unit)));
      }
    } else {
      painter.drawText(QRectF(3.0, area.top(), 44.0, 18.0), Qt::AlignRight, "0 ms");
      painter.drawText(QRectF(3.0, area.bottom() - 18.0, 44.0, 18.0), Qt::AlignRight | Qt::AlignBottom, "500 ms");
      if (frame_ && screenshot_options_.parameter_summary &&
          (kind_ == ChartKind::spectrogram || kind_ == ChartKind::waterfall)) {
        painter.drawText(QRectF(area.left(), area.top() + 2.0, area.width() - 18.0, 18.0), Qt::AlignRight,
                         QString("窗 %1 · 步长 %2 · FFT %3 · 重叠 %4% · %5")
                             .arg(frame_->stft_metadata.window_size)
                             .arg(frame_->stft_metadata.hop_size)
                             .arg(frame_->stft_metadata.fft_size)
                             .arg(frame_->stft_metadata.overlap_ratio * 100.0, 0, 'f', 0)
                             .arg(QString::fromStdString(frame_->stft_metadata.interpolation)));
      }
    }
    painter.drawText(QRectF(area.left(), area.bottom() + 3.0, area.width() / 2.0, 18.0), Qt::AlignLeft,
                     QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.begin_hz)));
    painter.drawText(QRectF(area.center().x(), area.bottom() + 3.0, area.width() / 2.0, 18.0), Qt::AlignRight,
                     QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.end_hz)));
  }

  [[nodiscard]] std::int64_t frequencyAt(double x, const QRectF& area) const {
    const auto ratio = std::clamp((x - area.left()) / area.width(), 0.0, 1.0);
    return viewport_.frequency_viewport.begin_hz +
           static_cast<std::int64_t>(
               std::llround(ratio * static_cast<double>(viewport_.frequency_viewport.bandwidth_hz())));
  }

  void rebuildHeatmap() {
    heatmap_ = {};
    if (!frame_ || frame_->stft_rows == 0 || frame_->stft_columns == 0 ||
        frame_->stft_db.size() != static_cast<std::size_t>(frame_->stft_rows) * frame_->stft_columns) {
      return;
    }
    heatmap_ =
        QImage(static_cast<int>(frame_->stft_columns), static_cast<int>(frame_->stft_rows), QImage::Format_RGB32);
    for (std::uint32_t row = 0; row < frame_->stft_rows; ++row) {
      for (std::uint32_t column = 0; column < frame_->stft_columns; ++column) {
        const auto value = frame_->stft_db[static_cast<std::size_t>(row) * frame_->stft_columns + column];
        heatmap_.setPixelColor(static_cast<int>(column), static_cast<int>(row),
                               heat_color(value, mapping_.reference_level, mapping_.dynamic_range));
      }
    }
  }

  void updateAccessibility() {
    QString description;
    if (!frame_) {
      description = "无图表数据";
    } else if (kind_ == ChartKind::time_waveform) {
      static constexpr std::array<std::string_view, 4> modes{"实信号", "I/Q", "幅度", "相位"};
      description = QString("时域图，%1 模式，%2 个有效样本，范围 [%3,%4)")
                        .arg(QString::fromUtf8(
                            modes.at(static_cast<std::size_t>(time_display_mode_)).data(),
                            static_cast<qsizetype>(modes.at(static_cast<std::size_t>(time_display_mode_)).size())))
                        .arg(frame_->time_primary.size())
                        .arg(frame_->time_range.begin())
                        .arg(frame_->time_range.end());
    } else if (kind_ == ChartKind::psd || kind_ == ChartKind::spectrum) {
      static constexpr std::array<std::string_view, 3> layouts{"单边谱", "镜像双边谱", "fftshift 双边谱"};
      description = QString("功率谱密度，%1，%2 个频率点，单位 dB/Hz，FFT %3，RBW %4 Hz")
                        .arg(QString::fromUtf8(
                            layouts.at(static_cast<std::size_t>(spectrum_layout_)).data(),
                            static_cast<qsizetype>(layouts.at(static_cast<std::size_t>(spectrum_layout_)).size())))
                        .arg(frame_->psd_db_hz.size())
                        .arg(frame_->psd_metadata.fft_size)
                        .arg(frame_->psd_metadata.rbw_hz, 0, 'f', 2);
    } else if (kind_ == ChartKind::spectrogram || kind_ == ChartKind::waterfall) {
      description =
          QString("时频图，%1 行 %2 列，横轴频率，纵轴时间向下递增").arg(frame_->stft_rows).arg(frame_->stft_columns);
    } else if (kind_ == ChartKind::constellation) {
      description =
          QString("星座图，%1 个复数点").arg(std::min(frame_->constellation_i.size(), frame_->constellation_q.size()));
    } else {
      description = QString("眼图，%1 个轨迹样本").arg(frame_->eye_trace.size());
    }
    if (!selections_.empty()) {
      description += QString("；%1 个 Selection").arg(selections_.size());
    }
    if (!cursors_.empty()) {
      description += QString("；%1 个游标").arg(cursors_.size());
    }
    setAccessibleDescription(description);
  }

  ChartKind kind_;
  QString title_;
  std::optional<VisualizationFrame> frame_;
  ViewportSnapshot viewport_;
  DisplayMapping mapping_;
  TimeDisplayMode time_display_mode_{TimeDisplayMode::in_phase_quadrature};
  SpectrumLayout spectrum_layout_{SpectrumLayout::shifted_two_sided};
  bool time_mode_selected_{};
  bool spectrum_layout_selected_{};
  std::vector<double> derived_waveform_;
  ScreenshotOptions screenshot_options_;
  InteractionMode interaction_mode_{InteractionMode::selection};
  std::vector<Selection> selections_;
  std::vector<Measurement> measurements_;
  std::vector<QPointF> cursors_;
  QImage heatmap_;
  std::optional<QPointF> drag_start_;
  QPointF drag_current_;
  Qt::MouseButton drag_button_{Qt::NoButton};
  std::optional<FrequencyRange> pan_start_range_;
  double pan_origin_x_{};
  std::function<void(FrequencyRange, bool, QString)> frequency_handler_;
  std::function<void(Selection)> selection_handler_;
  std::function<void(QString)> status_handler_;
};

class AnalysisWorkspaceWidget final : public QWidget {
public:
  explicit AnalysisWorkspaceWidget(AnalysisWorkspaceConfiguration configuration)
      : configuration_(std::move(configuration)) {
    setObjectName("SignalVisualizationWorkspace");
    setAccessibleName("信号可视化工作区");
    setAccessibleDescription("包含时间导航、当前视窗时域、功率谱密度和时频图，PSD 与 STFT 共享频率视口");
    setStyleSheet(QString(R"(
      QWidget { background: %1; color: %2; font-family: "Microsoft YaHei UI"; font-size: 12px; }
      QFrame#ControlStrip { background: %3; border: 1px solid #203A55; }
      QPushButton, QToolButton, QComboBox, QLineEdit, QDoubleSpinBox {
        background: #10243A; border: 1px solid #2B4867; border-radius: 2px;
        color: %2; min-height: 28px; padding: 0 7px;
      }
      QPushButton:focus, QToolButton:focus, QComboBox:focus, QLineEdit:focus,
      QDoubleSpinBox:focus, QCheckBox:focus { border: 2px solid %4; }
      QPushButton:checked, QToolButton:checked { background: #0B4D5A; border-color: %4; }
      QCheckBox { spacing: 6px; min-height: 28px; }
      QSplitter::handle { background: #27445F; height: 3px; }
    )")
                      .arg(canvas_color, primary_text, panel_color, accent_cyan));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(5, 5, 5, 5);
    root->setSpacing(4);

    auto* controls = new QFrame(this);
    controls->setObjectName("ControlStrip");
    auto* control_stack = new QHBoxLayout(controls);
    control_stack->setContentsMargins(4, 2, 4, 2);
    control_stack->setSpacing(6);
    auto* interaction_layout = new QHBoxLayout;
    interaction_layout->setContentsMargins(0, 0, 0, 0);
    interaction_layout->setSpacing(4);
    auto* parameter_layout = new QHBoxLayout;
    parameter_layout->setContentsMargins(0, 0, 0, 0);
    parameter_layout->setSpacing(4);
    control_stack->addLayout(interaction_layout);
    control_stack->addLayout(parameter_layout);
    const auto add_mode = [interaction_layout](const QString& label, bool checked = false) {
      auto* button = new QToolButton;
      button->setText(label);
      button->setCheckable(true);
      button->setChecked(checked);
      button->setFocusPolicy(Qt::StrongFocus);
      button->setMinimumSize(44, 28);
      interaction_layout->addWidget(button);
      return button;
    };
    auto* interaction_group = new QButtonGroup(this);
    interaction_group->setExclusive(true);
    interaction_group->addButton(add_mode("选择", true), static_cast<int>(InteractionMode::selection));
    interaction_group->addButton(add_mode("缩放"), static_cast<int>(InteractionMode::zoom));
    interaction_group->addButton(add_mode("平移"), static_cast<int>(InteractionMode::pan));
    interaction_group->addButton(add_mode("游标"), static_cast<int>(InteractionMode::cursor));

    waveform_toggle_ = new QCheckBox("时域", controls);
    waveform_toggle_->setChecked(configuration_.show_waveform);
    waveform_toggle_->setAccessibleName("显示或隐藏时域图");
    psd_toggle_ = new QCheckBox("PSD", controls);
    psd_toggle_->setChecked(configuration_.show_psd);
    psd_toggle_->setAccessibleName("显示或隐藏功率谱密度图");
    interaction_layout->addWidget(waveform_toggle_);
    interaction_layout->addWidget(psd_toggle_);

    auto* display_button = new QToolButton(controls);
    display_button->setText("显示模式");
    display_button->setAccessibleName("时域和频谱显示模式");
    display_button->setPopupMode(QToolButton::InstantPopup);
    display_button->setFocusPolicy(Qt::StrongFocus);
    auto* display_menu = new QMenu(display_button);
    auto* time_group = new QActionGroup(display_menu);
    time_group->setExclusive(true);
    for (const auto& [label, mode] : std::initializer_list<std::pair<QString, TimeDisplayMode>>{
             {"时域 · 实信号", TimeDisplayMode::real},
             {"时域 · I/Q", TimeDisplayMode::in_phase_quadrature},
             {"时域 · 幅度", TimeDisplayMode::magnitude},
             {"时域 · 相位", TimeDisplayMode::phase}}) {
      auto* action = display_menu->addAction(label);
      action->setCheckable(true);
      action->setData(static_cast<int>(mode));
      action->setChecked(mode == TimeDisplayMode::in_phase_quadrature);
      time_group->addAction(action);
    }
    display_menu->addSeparator();
    auto* spectrum_group = new QActionGroup(display_menu);
    spectrum_group->setExclusive(true);
    for (const auto& [label, layout] : std::initializer_list<std::pair<QString, SpectrumLayout>>{
             {"频谱 · 单边", SpectrumLayout::one_sided},
             {"频谱 · 镜像双边", SpectrumLayout::mirrored_two_sided},
             {"频谱 · fftshift 双边", SpectrumLayout::shifted_two_sided}}) {
      auto* action = display_menu->addAction(label);
      action->setCheckable(true);
      action->setData(static_cast<int>(layout));
      action->setChecked(layout == SpectrumLayout::shifted_two_sided);
      spectrum_group->addAction(action);
    }
    QObject::connect(time_group, &QActionGroup::triggered, this, [this](QAction* action) {
      setTimeDisplayMode(static_cast<TimeDisplayMode>(action->data().toInt()));
    });
    QObject::connect(spectrum_group, &QActionGroup::triggered, this, [this](QAction* action) {
      setSpectrumLayout(static_cast<SpectrumLayout>(action->data().toInt()));
    });
    display_button->setMenu(display_menu);
    interaction_layout->addWidget(display_button);

    exact_button_ = new QToolButton(controls);
    exact_button_->setText("精确输入");
    exact_button_->setAccessibleName("精确时间视窗与 Selection 输入");
    exact_button_->setPopupMode(QToolButton::InstantPopup);
    exact_button_->setFocusPolicy(Qt::StrongFocus);
    auto* exact_menu = new QMenu(exact_button_);
    auto* exact_time = exact_menu->addAction("精确时间视窗…");
    auto* exact_selection = exact_menu->addAction("精确创建 Selection…");
    QObject::connect(exact_time, &QAction::triggered, this, [this] { editExactTime(); });
    QObject::connect(exact_selection, &QAction::triggered, this, [this] { editExactSelection(); });
    exact_button_->setMenu(exact_menu);
    interaction_layout->addWidget(exact_button_);
    interaction_layout->addStretch();

    auto* frequency_label = new QLabel("中心", controls);
    parameter_layout->addWidget(frequency_label);
    frequency_input_ = new QLineEdit("1.245 GHz", controls);
    frequency_input_->setAccessibleName("精确中心频率输入");
    frequency_input_->setAccessibleDescription("支持 Hz、kHz、MHz 和 GHz，内部保持 64 位整数 Hz");
    frequency_input_->setMaximumWidth(120);
    parameter_layout->addWidget(frequency_input_);
    auto* span_label = new QLabel("跨度", controls);
    parameter_layout->addWidget(span_label);
    span_input_ = new QLineEdit("50 MHz", controls);
    span_input_->setAccessibleName("精确频率跨度输入");
    span_input_->setMaximumWidth(95);
    parameter_layout->addWidget(span_input_);
    parameter_layout->addStretch();

    auto* color_label = new QLabel("色阶", controls);
    parameter_layout->addWidget(color_label);
    color_map_ = new QComboBox(controls);
    color_map_->addItems({"Industrial", "Viridis", "Grayscale"});
    color_map_->setAccessibleName("瀑布配色预设");
    parameter_layout->addWidget(color_map_);
    reference_ = new QDoubleSpinBox(controls);
    reference_->setRange(-200.0, 50.0);
    reference_->setValue(-20.0);
    reference_->setSuffix(" dB");
    reference_->setAccessibleName("瀑布参考电平");
    reference_->setMinimumWidth(104);
    reference_->setMaximumWidth(120);
    parameter_layout->addWidget(reference_);
    dynamic_range_ = new QDoubleSpinBox(controls);
    dynamic_range_->setRange(1.0, 180.0);
    dynamic_range_->setValue(70.0);
    dynamic_range_->setSuffix(" dB");
    dynamic_range_->setAccessibleName("瀑布动态范围");
    dynamic_range_->setMinimumWidth(104);
    dynamic_range_->setMaximumWidth(120);
    parameter_layout->addWidget(dynamic_range_);
    screenshot_button_ = new QPushButton("截图", controls);
    screenshot_button_->setAccessibleName("图谱截图选项");
    parameter_layout->addWidget(screenshot_button_);
    root->addWidget(controls);

    navigator_ = new TimeNavigatorWidget(this);
    root->addWidget(navigator_);
    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setChildrenCollapsible(false);
    waveform_ = new SignalChartCanvas(ChartKind::time_waveform, "时域波形 · I/Q", splitter_);
    psd_ = new SignalChartCanvas(ChartKind::psd, "功率谱密度 / PSD", splitter_);
    spectrogram_ = new SignalChartCanvas(ChartKind::spectrogram, "STFT 瀑布图 · 时间向下递增", splitter_);
    splitter_->addWidget(waveform_);
    splitter_->addWidget(psd_);
    splitter_->addWidget(spectrogram_);
    const auto extra_title = [](ChartKind kind) {
      switch (kind) {
      case ChartKind::spectrum:
        return QString("频谱 · 单边/双边");
      case ChartKind::waterfall:
        return QString("瀑布图 · 时间向下递增");
      case ChartKind::constellation:
        return QString("星座图");
      case ChartKind::eye_diagram:
        return QString("眼图");
      case ChartKind::time_waveform:
      case ChartKind::psd:
      case ChartKind::spectrogram:
        break;
      }
      return QString("可视化组件");
    };
    for (const auto kind : configuration_.extra_charts) {
      if (kind == ChartKind::time_waveform || kind == ChartKind::psd || kind == ChartKind::spectrogram ||
          std::ranges::any_of(extra_canvases_, [kind](const auto* canvas) { return canvas->kind() == kind; })) {
        continue;
      }
      auto* canvas = new SignalChartCanvas(kind, extra_title(kind), splitter_);
      splitter_->addWidget(canvas);
      extra_canvases_.push_back(canvas);
    }
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 4);
    splitter_->setStretchFactor(2, 5);
    QList<int> initial_sizes{150, 200, 250};
    for (qsizetype index = 0; index < static_cast<qsizetype>(extra_canvases_.size()); ++index) {
      splitter_->setStretchFactor(static_cast<int>(index + 3), 2);
      initial_sizes.push_back(180);
    }
    splitter_->setSizes(initial_sizes);
    root->addWidget(splitter_, 1);

    status_ = new QLabel("等待绑定视图", this);
    status_->setObjectName("VisualizationLiveRegion");
    status_->setAccessibleName("视图状态通知");
    status_->setMinimumHeight(28);
    status_->setStyleSheet("background:#0E1B2D; border:1px solid #203A55; padding:4px 8px;");
    root->addWidget(status_);

    const auto frequency_change = [this](FrequencyRange range, bool reset, const QString& reason) {
      viewport_.frequency_viewport = reset ? viewport_.effective_frequency_range : range;
      ++viewport_.request_id.generation;
      updateViewports();
      setStatus(QString("%1 · PSD 与 STFT 已同步 · %2")
                    .arg(reason)
                    .arg(QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.bandwidth_hz()))));
    };
    psd_->setFrequencyHandler(frequency_change);
    spectrogram_->setFrequencyHandler(frequency_change);
    for (auto* canvas : extra_canvases_) {
      if (canvas->kind() == ChartKind::spectrum || canvas->kind() == ChartKind::waterfall) {
        canvas->setFrequencyHandler(frequency_change);
      }
    }
    for (auto* canvas : allCanvases()) {
      canvas->setInteractionMode(InteractionMode::selection);
      canvas->setStatusHandler([this](const QString& status) { setStatus(status); });
      canvas->setSelectionHandler([this](Selection selection) {
        const auto created = createSelection(std::move(selection));
        if (!created) {
          setStatus(
              QString("Selection 创建失败：%1").arg(QString::fromStdString(std::string(created.error().message()))));
        }
      });
    }
    QObject::connect(interaction_group, &QButtonGroup::idClicked, this,
                     [this](int id) { setInteractionMode(static_cast<InteractionMode>(id)); });
    navigator_->setChangeHandler([this](data::SampleRange range, const std::string& reason) {
      viewport_.time_viewport = range;
      ++viewport_.request_id.generation;
      updateViewports();
      setStatus(
          QString::fromStdString(reason) +
          QString(" · 新请求 VR-%1 · Selection 未改变").arg(viewport_.request_id.generation, 6, 10, QLatin1Char('0')));
    });
    QObject::connect(waveform_toggle_, &QCheckBox::toggled, this, [this](bool visible) {
      waveform_->setVisible(visible);
      setStatus(visible ? "时域图已连接观察器" : "时域图已隐藏，专属准备与绘制已停止");
    });
    QObject::connect(psd_toggle_, &QCheckBox::toggled, this, [this](bool visible) {
      psd_->setVisible(visible);
      setStatus(visible ? "PSD 已连接观察器" : "PSD 已隐藏，专属准备与绘制已停止");
    });
    const auto display_change = [this] {
      DisplayMapping mapping;
      mapping.color_map = color_map_->currentText().toStdString();
      mapping.reference_level = reference_->value();
      mapping.dynamic_range = dynamic_range_->value();
      psd_->setDisplayMapping(mapping);
      spectrogram_->setDisplayMapping(mapping);
      for (auto* canvas : extra_canvases_) {
        canvas->setDisplayMapping(mapping);
      }
      setStatus("仅更新显示映射；未创建文件读取或时间视口请求");
    };
    QObject::connect(color_map_, &QComboBox::currentIndexChanged, this, [display_change](int) { display_change(); });
    QObject::connect(reference_, &QDoubleSpinBox::valueChanged, this, [display_change](double) { display_change(); });
    QObject::connect(dynamic_range_, &QDoubleSpinBox::valueChanged, this,
                     [display_change](double) { display_change(); });
    QObject::connect(frequency_input_, &QLineEdit::editingFinished, this, [this] { applyExactFrequency(); });
    QObject::connect(span_input_, &QLineEdit::editingFinished, this, [this] { applyExactFrequency(); });
    QObject::connect(screenshot_button_, &QPushButton::clicked, this, [this] {
      const auto path = QFileDialog::getSaveFileName(this, "保存图谱截图", {}, "PNG 图像 (*.png)");
      if (path.isEmpty()) {
        setStatus("图谱截图已取消");
        return;
      }
      const auto status = saveScreenshot(std::filesystem::path(path.toStdWString()), ScreenshotOptions{});
      setStatus(status ? QString("图谱截图已保存 · %1").arg(path)
                       : QString("图谱截图失败 · %1").arg(QString::fromStdString(std::string(status.message()))));
    });

    waveform_->setVisible(configuration_.show_waveform);
    psd_->setVisible(configuration_.show_psd);
    spectrogram_->setVisible(configuration_.show_spectrogram);
  }

protected:
  void resizeEvent(QResizeEvent* event) override {
    const auto compact = event->size().width() < 1120;
    exact_button_->setVisible(!compact);
    screenshot_button_->setVisible(!compact);
    QWidget::resizeEvent(event);
  }

public:
  void setFrame(const VisualizationFrame& frame) {
    for (auto* canvas : allCanvases()) {
      canvas->setFrame(frame);
    }
    setStatus(QString("当前 · VR-%1 · 三个视图原子挂载 · %2 个 PSD 点")
                  .arg(frame.request_id.generation, 6, 10, QLatin1Char('0'))
                  .arg(frame.psd_db_hz.size()));
  }

  void setViewport(const ViewportSnapshot& viewport) {
    if (!overlay_ || viewport_.data_source_version_id != viewport.data_source_version_id) {
      overlay_.emplace(viewport.loaded_range, viewport.effective_frequency_range);
    }
    viewport_ = viewport;
    updateViewports();
    frequency_input_->setText(QString::fromStdString(
        format_frequency_hz(viewport.frequency_viewport.begin_hz + viewport.frequency_viewport.bandwidth_hz() / 2)));
    span_input_->setText(QString::fromStdString(format_frequency_hz(viewport.frequency_viewport.bandwidth_hz())));
  }

  void setInteractionMode(InteractionMode mode) {
    for (auto* canvas : allCanvases()) {
      canvas->setInteractionMode(mode);
    }
    static constexpr std::array<std::string_view, 4> names{"Selection", "框选缩放", "拖动平移", "游标测量"};
    const auto index = static_cast<std::size_t>(mode);
    setStatus(QString("交互模式：%1")
                  .arg(QString::fromUtf8(names.at(index).data(), static_cast<qsizetype>(names.at(index).size()))));
  }

  void setTimeDisplayMode(TimeDisplayMode mode) {
    waveform_->setTimeDisplayMode(mode);
    for (auto* canvas : extra_canvases_) {
      if (canvas->kind() == ChartKind::time_waveform) {
        canvas->setTimeDisplayMode(mode);
      }
    }
    static constexpr std::array<std::string_view, 4> names{"实信号", "I/Q", "幅度", "相位"};
    const auto index = static_cast<std::size_t>(mode);
    setStatus(QString("时域显示模式：%1；CurrentTimeViewport 未改变")
                  .arg(QString::fromUtf8(names.at(index).data(), static_cast<qsizetype>(names.at(index).size()))));
  }

  void setSpectrumLayout(SpectrumLayout layout) {
    for (auto* canvas : allCanvases()) {
      if (canvas->kind() == ChartKind::psd || canvas->kind() == ChartKind::spectrum) {
        canvas->setSpectrumLayout(layout);
      }
    }
    if (layout == SpectrumLayout::one_sided && viewport_.effective_frequency_range.begin_hz < 0 &&
        viewport_.effective_frequency_range.end_hz > 0) {
      viewport_.frequency_viewport = {0, viewport_.effective_frequency_range.end_hz};
    } else {
      viewport_.frequency_viewport = viewport_.effective_frequency_range;
    }
    ++viewport_.request_id.generation;
    updateViewports();
    static constexpr std::array<std::string_view, 3> names{"单边谱", "镜像双边谱", "fftshift 双边谱"};
    const auto index = static_cast<std::size_t>(layout);
    setStatus(QString("频谱布局：%1；PSD 与 STFT 频率视口已同步")
                  .arg(QString::fromUtf8(names.at(index).data(), static_cast<qsizetype>(names.at(index).size()))));
  }

  void fitFrequencyToData() {
    viewport_.frequency_viewport = viewport_.effective_frequency_range;
    ++viewport_.request_id.generation;
    updateViewports();
    frequency_input_->setText(QString::fromStdString(
        format_frequency_hz(viewport_.frequency_viewport.begin_hz + viewport_.frequency_viewport.bandwidth_hz() / 2)));
    span_input_->setText(QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.bandwidth_hz())));
    setStatus("用户显式执行适合全部；PSD、STFT 与属性已同步");
  }

  [[nodiscard]] core::Result<Selection> createSelection(Selection selection) {
    if (!overlay_) {
      return core::Status::failure({core::ErrorDomain::visualization, core::ErrorReason::unavailable},
                                   "尚未绑定可创建 Selection 的数据范围");
    }
    auto created = overlay_->create(std::move(selection));
    if (!created) {
      return created.error();
    }
    const auto current = overlay_->selections();
    for (auto* canvas : allCanvases()) {
      canvas->setOverlays(current, overlay_->measurements());
    }
    setStatus(QString("已创建 %1 · 浏览视窗未自动移动").arg(QString::fromStdString(created.value().id)));
    return created;
  }

  [[nodiscard]] std::vector<Selection> selections() const {
    return overlay_ ? overlay_->selections() : std::vector<Selection>{};
  }

  [[nodiscard]] core::Status locateSelection(std::string_view id) {
    const auto selection = overlay_ ? overlay_->find(id) : std::nullopt;
    if (!selection) {
      return core::Status::failure({core::ErrorDomain::visualization, core::ErrorReason::invalid_argument},
                                   "待定位 Selection 不存在");
    }
    viewport_.time_viewport = selection->time_range;
    if (selection->frequency_range) {
      viewport_.frequency_viewport = *selection->frequency_range;
    }
    ++viewport_.request_id.generation;
    updateViewports();
    setStatus(QString("已显式定位到 %1").arg(QString::fromStdString(selection->id)));
    return core::Status::success();
  }

  [[nodiscard]] core::Status saveScreenshot(const std::filesystem::path& path, ScreenshotOptions options) {
    if (path.empty()) {
      return core::Status::failure({core::ErrorDomain::visualization, core::ErrorReason::invalid_argument},
                                   "图谱截图路径不能为空");
    }
    for (auto* canvas : allCanvases()) {
      canvas->setScreenshotOptions(options);
    }
    QApplication::processEvents();
    const auto image = grab().toImage();
    for (auto* canvas : allCanvases()) {
      canvas->setScreenshotOptions(ScreenshotOptions{});
    }
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, error);
    }
    if (error || image.isNull() || !image.save(QString::fromStdWString(path.wstring()), "PNG")) {
      return core::Status::failure({core::ErrorDomain::visualization, core::ErrorReason::internal_failure},
                                   "图谱截图保存失败", error.message());
    }
    return core::Status::success();
  }

  [[nodiscard]] const ViewportSnapshot& viewport() const noexcept {
    return viewport_;
  }

  [[nodiscard]] QString accessibilitySummary() const {
    return QString("信号可视化工作区；时间范围 [%1,%2)；频率 %3 至 %4；%5")
        .arg(viewport_.time_viewport.begin())
        .arg(viewport_.time_viewport.end())
        .arg(QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.begin_hz)))
        .arg(QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.end_hz)))
        .arg(status_->text());
  }

  void setStatus(const QString& status) {
    status_->setText("● 当前 · " + status);
    status_->setAccessibleDescription(status);
  }

private:
  [[nodiscard]] std::vector<SignalChartCanvas*> allCanvases() const {
    std::vector<SignalChartCanvas*> canvases{waveform_, psd_, spectrogram_};
    canvases.insert(canvases.end(), extra_canvases_.begin(), extra_canvases_.end());
    return canvases;
  }

  void editExactTime() {
    QDialog dialog(this);
    dialog.setWindowTitle("精确时间视窗");
    dialog.setAccessibleName("精确时间视窗输入对话框");
    auto* layout = new QFormLayout(&dialog);
    auto* start = new QLineEdit(QString::number(viewport_.time_viewport.begin()), &dialog);
    start->setAccessibleName("时间视窗起始样本");
    auto* span = new QLineEdit(QString::number(viewport_.time_viewport.size()), &dialog);
    span->setAccessibleName("时间视窗样本跨度");
    layout->addRow("起始样本", start);
    layout->addRow("样本跨度", span);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
      setStatus("精确时间视窗输入已取消");
      return;
    }
    bool start_ok{};
    bool span_ok{};
    const auto begin = start->text().toULongLong(&start_ok);
    const auto count = span->text().toULongLong(&span_ok);
    if (!start_ok || !span_ok || count == 0 || begin > std::numeric_limits<std::uint64_t>::max() - count) {
      setStatus("精确时间视窗无效；需要非零且不溢出的 64 位样本范围");
      return;
    }
    const auto range = data::SampleRange::make(begin, begin + count);
    if (!range || !viewport_.loaded_range.contains(range.value())) {
      setStatus("精确时间视窗超出 LoadedDataRange；原视窗保持不变");
      return;
    }
    viewport_.time_viewport = range.value();
    ++viewport_.request_id.generation;
    updateViewports();
    setStatus(QString("精确时间视窗已采用 · [%1,%2)").arg(begin).arg(begin + count));
  }

  void editExactSelection() {
    QDialog dialog(this);
    dialog.setWindowTitle("精确创建 Selection");
    dialog.setAccessibleName("精确 Selection 输入对话框");
    auto* layout = new QFormLayout(&dialog);
    auto* kind = new QComboBox(&dialog);
    kind->addItem("时间", static_cast<int>(SelectionKind::time));
    kind->addItem("频率", static_cast<int>(SelectionKind::frequency));
    kind->addItem("时频", static_cast<int>(SelectionKind::time_frequency));
    kind->setCurrentIndex(2);
    auto* start = new QLineEdit(QString::number(viewport_.time_viewport.begin()), &dialog);
    auto* end = new QLineEdit(QString::number(viewport_.time_viewport.end()), &dialog);
    auto* frequency_begin =
        new QLineEdit(QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.begin_hz)), &dialog);
    auto* frequency_end =
        new QLineEdit(QString::fromStdString(format_frequency_hz(viewport_.frequency_viewport.end_hz)), &dialog);
    layout->addRow("类型", kind);
    layout->addRow("起始样本", start);
    layout->addRow("结束样本（半开）", end);
    layout->addRow("起始频率", frequency_begin);
    layout->addRow("结束频率", frequency_end);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
      setStatus("精确 Selection 输入已取消");
      return;
    }
    bool start_ok{};
    bool end_ok{};
    const auto begin_sample = start->text().toULongLong(&start_ok);
    const auto end_sample = end->text().toULongLong(&end_ok);
    const auto time = data::SampleRange::make(begin_sample, end_sample);
    const auto selection_kind = static_cast<SelectionKind>(kind->currentData().toInt());
    std::optional<FrequencyRange> frequency;
    if (selection_kind != SelectionKind::time) {
      const auto first = parse_frequency_hz(frequency_begin->text().toStdString());
      const auto second = parse_frequency_hz(frequency_end->text().toStdString());
      if (!first || !second || first.value() >= second.value()) {
        setStatus("精确 Selection 频率边界无效；原对象未改变");
        return;
      }
      frequency = FrequencyRange{first.value(), second.value()};
    }
    if (!start_ok || !end_ok || !time) {
      setStatus("精确 Selection 时间边界无效；需要 64 位半开区间");
      return;
    }
    const auto created = createSelection({"", "", selection_kind, time.value(), frequency});
    if (!created) {
      setStatus(
          QString("精确 Selection 未采用 · %1").arg(QString::fromStdString(std::string(created.error().message()))));
    }
  }

  void updateViewports() {
    navigator_->setViewport(viewport_);
    for (auto* canvas : allCanvases()) {
      canvas->setViewport(viewport_);
    }
  }

  void applyExactFrequency() {
    const auto center = parse_frequency_hz(frequency_input_->text().toStdString());
    const auto span = parse_frequency_hz(span_input_->text().toStdString());
    if (!center || !span || span.value() <= 0) {
      frequency_input_->setStyleSheet("border:2px solid #FF6B7A;");
      span_input_->setStyleSheet("border:2px solid #FF6B7A;");
      setStatus("频率输入无效；保留原值，支持 Hz/kHz/MHz/GHz");
      return;
    }
    const auto half = span.value() / 2;
    const auto upper_half = span.value() - half;
    if (center.value() < std::numeric_limits<std::int64_t>::min() + half ||
        center.value() > std::numeric_limits<std::int64_t>::max() - upper_half) {
      frequency_input_->setStyleSheet("border:2px solid #FF6B7A;");
      span_input_->setStyleSheet("border:2px solid #FF6B7A;");
      setStatus("频率输入发生 64 位边界溢出；保留原值");
      return;
    }
    const FrequencyRange requested{center.value() - half, center.value() + upper_half};
    if (!viewport_.effective_frequency_range.contains(requested)) {
      frequency_input_->setStyleSheet("border:2px solid #FF6B7A;");
      span_input_->setStyleSheet("border:2px solid #FF6B7A;");
      setStatus("频率输入超出数据有效范围；保留原值");
      return;
    }
    frequency_input_->setStyleSheet({});
    span_input_->setStyleSheet({});
    viewport_.frequency_viewport = requested;
    ++viewport_.request_id.generation;
    updateViewports();
    setStatus(QString("精确频率已采用 · %1 · %2")
                  .arg(QString::fromStdString(format_frequency_hz(center.value())))
                  .arg(QString::fromStdString(format_frequency_hz(span.value()))));
  }

  AnalysisWorkspaceConfiguration configuration_;
  ViewportSnapshot viewport_;
  TimeNavigatorWidget* navigator_{};
  QSplitter* splitter_{};
  SignalChartCanvas* waveform_{};
  SignalChartCanvas* psd_{};
  SignalChartCanvas* spectrogram_{};
  std::vector<SignalChartCanvas*> extra_canvases_;
  std::optional<OverlayModel> overlay_;
  QCheckBox* waveform_toggle_{};
  QCheckBox* psd_toggle_{};
  QToolButton* exact_button_{};
  QPushButton* screenshot_button_{};
  QLineEdit* frequency_input_{};
  QLineEdit* span_input_{};
  QComboBox* color_map_{};
  QDoubleSpinBox* reference_{};
  QDoubleSpinBox* dynamic_range_{};
  QLabel* status_{};
};

class AnalysisWorkspaceImpl final : public IAnalysisWorkspace {
public:
  explicit AnalysisWorkspaceImpl(AnalysisWorkspaceConfiguration configuration)
      : widget_(std::make_unique<AnalysisWorkspaceWidget>(std::move(configuration))) {}

  [[nodiscard]] void* native_handle() noexcept override {
    return widget_.get();
  }

  [[nodiscard]] core::Status bind_frame(VisualizationFrame frame) override {
    if (frame.request_id != widget_->viewport().request_id || frame.time_range != widget_->viewport().time_viewport ||
        frame.frequency_range != widget_->viewport().frequency_viewport ||
        frame.data_source_version_id != widget_->viewport().data_source_version_id) {
      return core::Status::failure({core::ErrorDomain::visualization, core::ErrorReason::cancelled},
                                   "Qt 工作区拒绝过期或非同窗帧");
    }
    widget_->setFrame(frame);
    return core::Status::success();
  }

  [[nodiscard]] core::Status set_interaction_mode(InteractionMode mode) override {
    if (static_cast<std::uint32_t>(mode) > static_cast<std::uint32_t>(InteractionMode::cursor)) {
      return core::Status::failure({core::ErrorDomain::visualization, core::ErrorReason::invalid_argument},
                                   "交互模式无效");
    }
    widget_->setInteractionMode(mode);
    return core::Status::success();
  }

  [[nodiscard]] core::Status fit_frequency_to_data() override {
    if (widget_->viewport().effective_frequency_range.bandwidth_hz() <= 0) {
      return core::Status::failure({core::ErrorDomain::visualization, core::ErrorReason::unavailable},
                                   "尚未绑定可恢复的有效全频范围");
    }
    widget_->fitFrequencyToData();
    return core::Status::success();
  }

  [[nodiscard]] core::Result<Selection> create_selection(Selection selection) override {
    return widget_->createSelection(std::move(selection));
  }

  [[nodiscard]] std::vector<Selection> selections() const override {
    return widget_->selections();
  }

  [[nodiscard]] core::Status locate_selection(std::string_view id) override {
    return widget_->locateSelection(id);
  }

  [[nodiscard]] core::Status save_screenshot(const std::filesystem::path& path, ScreenshotOptions options) override {
    return widget_->saveScreenshot(path, options);
  }

  [[nodiscard]] core::Status apply_viewport(const ViewportSnapshot& viewport) override {
    if (viewport.loaded_range.empty() || viewport.time_viewport.empty() ||
        !viewport.loaded_range.contains(viewport.time_viewport) ||
        !viewport.effective_frequency_range.contains(viewport.frequency_viewport) ||
        viewport.request_id.scope.empty() || viewport.request_id.generation == 0) {
      return core::Status::failure({core::ErrorDomain::visualization, core::ErrorReason::invalid_argument},
                                   "Qt 工作区视口无效");
    }
    widget_->setViewport(viewport);
    return core::Status::success();
  }

  [[nodiscard]] ViewportSnapshot viewport() const override {
    return widget_->viewport();
  }

  [[nodiscard]] std::string accessibility_summary() const override {
    return widget_->accessibilitySummary().toStdString();
  }

  void set_status(std::string status) override {
    widget_->setStatus(QString::fromStdString(status));
  }

private:
  std::unique_ptr<AnalysisWorkspaceWidget> widget_;
};

} // namespace

std::unique_ptr<IAnalysisWorkspace> make_analysis_workspace(AnalysisWorkspaceConfiguration configuration) {
  if (QApplication::instance() == nullptr) {
    return {};
  }
  return std::make_unique<AnalysisWorkspaceImpl>(std::move(configuration));
}

} // namespace signal::visualization
