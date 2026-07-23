#include "signal_studio/visualization/views.hpp"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace signal::visualization {

std::string_view to_string(ChartViewKind kind) noexcept {
  switch (kind) {
  case ChartViewKind::time_waveform:
    return "time-waveform";
  case ChartViewKind::spectrum:
    return "spectrum";
  case ChartViewKind::spectrogram:
    return "spectrogram";
  case ChartViewKind::constellation:
    return "constellation";
  case ChartViewKind::eye_diagram:
    return "eye-diagram";
  }
  return "unknown";
}

TimeNavigator::TimeNavigator(LoadedDataRange loaded) : viewport_(loaded) {}

core::Status TimeNavigator::set_loaded_range(LoadedDataRange loaded) {
  return viewport_.set_loaded_range(loaded);
}

namespace {

/// Base widget implementing the common IChartView contract. QWidget without Q_OBJECT: paint-only
/// widgets need no signals/slots, so no MOC step is required.
class BaseChartWidget : public QWidget, public IChartView {
public:
  explicit BaseChartWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumSize(160, 90); // accessible hit target scaling handled by host layout
    setAttribute(Qt::WA_OpaquePaintEvent, false);
  }

  void bind(std::shared_ptr<const IDataSeries> series) override {
    series_ = std::move(series);
    update();
  }
  std::shared_ptr<const IDataSeries> bound_series() const noexcept override {
    return series_;
  }
  void set_visible(bool visible) override {
    visible_ = visible;
    if (!visible)
      hide();
    else
      show();
    update();
  }
  bool visible() const noexcept override {
    return visible_;
  }
  void* native_widget() noexcept override {
    return static_cast<QWidget*>(this);
  }
  void set_needs_data_callback(NeedsDataCallback callback) override {
    needs_cb_ = std::move(callback);
  }
  bool needs_data() const noexcept override {
    if (!visible_)
      return false;
    return needs_cb_ ? needs_cb_() : true;
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Qt::black);
    if (!visible_ || !series_) {
      p.setPen(Qt::gray);
      p.drawText(rect(), Qt::AlignCenter, visible_ ? "(no data)" : "(hidden)");
      return;
    }
    paint_content(p);
  }
  virtual void paint_content(QPainter& p) = 0;

  std::shared_ptr<const IDataSeries> series_;
  bool visible_{true};
  NeedsDataCallback needs_cb_;
};

class TimeWaveformWidget final : public BaseChartWidget {
public:
  using BaseChartWidget::BaseChartWidget;
  ChartViewKind kind() const noexcept override {
    return ChartViewKind::time_waveform;
  }

protected:
  void paint_content(QPainter& p) override {
    auto rs = std::dynamic_pointer_cast<const RealSeries>(series_);
    if (!rs)
      return;
    const auto samples = rs->samples();
    if (samples.empty())
      return;
    double mn = samples[0], mx = samples[0];
    for (double v : samples) {
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    if (mx - mn < 1e-12) {
      mx = mn + 1.0;
    }
    const double x_step = static_cast<double>(width()) / static_cast<double>(samples.size());
    p.setPen(QPen(Qt::green, 1));
    QPainterPath path;
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const double x = i * x_step;
      const double y = height() - (samples[i] - mn) / (mx - mn) * height();
      if (i == 0)
        path.moveTo(x, y);
      else
        path.lineTo(x, y);
    }
    p.drawPath(path);
  }
};

class SpectrumWidget final : public BaseChartWidget {
public:
  using BaseChartWidget::BaseChartWidget;
  ChartViewKind kind() const noexcept override {
    return ChartViewKind::spectrum;
  }

protected:
  void paint_content(QPainter& p) override {
    auto ss = std::dynamic_pointer_cast<const SpectrumSeries>(series_);
    if (!ss)
      return;
    const auto freq = ss->frequencies_hz();
    const auto power = ss->power_db();
    if (freq.empty())
      return;
    double fmn = freq.front(), fmx = freq.back();
    double pmn = power[0], pmx = power[0];
    for (double v : power) {
      pmn = std::min(pmn, v);
      pmx = std::max(pmx, v);
    }
    if (pmx - pmn < 1e-12)
      pmx = pmn + 1.0;
    const double xs = (fmx > fmn) ? static_cast<double>(width()) / (fmx - fmn) : 1.0;
    p.setPen(QPen(Qt::cyan, 1));
    QPainterPath path;
    for (std::size_t i = 0; i < freq.size(); ++i) {
      const double x = (freq[i] - fmn) * xs;
      const double y = height() - (power[i] - pmn) / (pmx - pmn) * height();
      if (i == 0)
        path.moveTo(x, y);
      else
        path.lineTo(x, y);
    }
    p.drawPath(path);
  }
};

class SpectrogramWidget final : public BaseChartWidget {
public:
  using BaseChartWidget::BaseChartWidget;
  ChartViewKind kind() const noexcept override {
    return ChartViewKind::spectrogram;
  }
  void set_color_scale(ColorScale scale) {
    scale_ = scale;
    update();
  }

protected:
  void paint_content(QPainter& p) override {
    auto sg = std::dynamic_pointer_cast<const SpectrogramSeries>(series_);
    if (!sg)
      return;
    const std::uint64_t frames = sg->frame_count();
    const std::uint64_t freqs = sg->freq_count();
    if (frames == 0 || freqs == 0)
      return;
    const auto mag = sg->magnitudes_db();
    double mn = mag[0], mx = mag[0];
    for (double v : mag) {
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    if (mx - mn < 1e-12)
      mx = mn + 1.0;
    const double cell_w = static_cast<double>(width()) / frames;
    const double cell_h = static_cast<double>(height()) / freqs;
    for (std::uint64_t f = 0; f < frames; ++f) {
      for (std::uint64_t k = 0; k < freqs; ++k) {
        const double v = (mag[f * freqs + k] - mn) / (mx - mn);
        const std::uint32_t rgb = color_rgb(scale_, v);
        const int r = static_cast<int>((rgb >> 16) & 0xFF);
        const int g = static_cast<int>((rgb >> 8) & 0xFF);
        const int b = static_cast<int>(rgb & 0xFF);
        p.fillRect(QRectF(f * cell_w, height() - (k + 1) * cell_h, std::ceil(cell_w), std::ceil(cell_h)),
                   QColor(r, g, b));
      }
    }
  }

private:
  ColorScale scale_{ColorScale::viridis};
};

class ConstellationWidget final : public BaseChartWidget {
public:
  using BaseChartWidget::BaseChartWidget;
  ChartViewKind kind() const noexcept override {
    return ChartViewKind::constellation;
  }

protected:
  void paint_content(QPainter& p) override {
    auto cs = std::dynamic_pointer_cast<const ComplexSeries>(series_);
    if (!cs)
      return;
    const auto re = cs->real();
    const auto im = cs->imag();
    if (re.empty())
      return;
    double mn = re[0], mx = re[0];
    for (std::size_t i = 0; i < re.size(); ++i) {
      mn = std::min({mn, re[i], im[i]});
      mx = std::max({mx, re[i], im[i]});
    }
    if (mx - mn < 1e-12)
      mx = mn + 1.0;
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(Qt::yellow));
    const double scale = static_cast<double>(height()) / (mx - mn);
    for (std::size_t i = 0; i < re.size(); ++i) {
      const double x = (re[i] - mn) * scale;
      const double y = height() - (im[i] - mn) * scale;
      p.drawEllipse(QPointF(x, y), 1.5, 1.5);
    }
  }
};

class EyeDiagramWidget final : public BaseChartWidget {
public:
  using BaseChartWidget::BaseChartWidget;
  ChartViewKind kind() const noexcept override {
    return ChartViewKind::eye_diagram;
  }
  void set_symbol_samples(std::uint64_t n) {
    symbol_samples_ = std::max<std::uint64_t>(n, 1);
    update();
  }

protected:
  void paint_content(QPainter& p) override {
    auto rs = std::dynamic_pointer_cast<const RealSeries>(series_);
    if (!rs)
      return;
    const auto samples = rs->samples();
    if (samples.empty() || symbol_samples_ == 0)
      return;
    double mn = samples[0], mx = samples[0];
    for (double v : samples) {
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    if (mx - mn < 1e-12)
      mx = mn + 1.0;
    const double x_step = static_cast<double>(width()) / symbol_samples_;
    p.setPen(QPen(QColor(0, 255, 128, 180), 1));
    std::size_t i = 0;
    while (i < samples.size()) {
      QPainterPath path;
      for (std::uint64_t s = 0; s < symbol_samples_ && i < samples.size(); ++s, ++i) {
        const double x = s * x_step;
        const double y = height() - (samples[i] - mn) / (mx - mn) * height();
        if (s == 0)
          path.moveTo(x, y);
        else
          path.lineTo(x, y);
      }
      p.drawPath(path);
    }
  }

private:
  std::uint64_t symbol_samples_{8};
};

core::Result<std::unique_ptr<IChartView>> make_view_failed() {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::visualization, core::ErrorReason::unavailable},
                               "Qt application context is not available to create chart widgets");
}

} // namespace

core::Result<std::unique_ptr<IChartView>> make_time_waveform_view() {
  if (!QApplication::instance())
    return make_view_failed();
  return std::unique_ptr<IChartView>(std::make_unique<TimeWaveformWidget>());
}
core::Result<std::unique_ptr<IChartView>> make_spectrum_view() {
  if (!QApplication::instance())
    return make_view_failed();
  return std::unique_ptr<IChartView>(std::make_unique<SpectrumWidget>());
}
core::Result<std::unique_ptr<IChartView>> make_spectrogram_view() {
  if (!QApplication::instance())
    return make_view_failed();
  return std::unique_ptr<IChartView>(std::make_unique<SpectrogramWidget>());
}
core::Result<std::unique_ptr<IChartView>> make_constellation_view() {
  if (!QApplication::instance())
    return make_view_failed();
  return std::unique_ptr<IChartView>(std::make_unique<ConstellationWidget>());
}
core::Result<std::unique_ptr<IChartView>> make_eye_diagram_view() {
  if (!QApplication::instance())
    return make_view_failed();
  return std::unique_ptr<IChartView>(std::make_unique<EyeDiagramWidget>());
}

} // namespace signal::visualization
