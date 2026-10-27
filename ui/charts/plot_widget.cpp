#include "ui/charts/plot_widget.h"

#include <QApplication>
#include <QJsonArray>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QToolTip>
#include <QWheelEvent>
#include <QWidgetAction>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <thread>
#include <unordered_set>

#include "infrastructure/int16_iq_file.h"
#include "infrastructure/spectral_analysis.h"
#include "ui/charts/accelerated_surface.h"
#include "ui/charts/chart_interaction.h"
#include "ui/charts/cursor_overlay.h"
#include "ui/charts/palette.h"
#include "ui/source_units.h"

namespace signalstudio {
namespace {
using chart_palette::color;
using chart_palette::texture;
constexpr int Left = 1, Right = 2, Top = 4, Bottom = 8, Move = 16;
QPointer<PlotWidget> activeGesture;
QPointer<PlotWidget> activeWheel;
// GUI-thread-only peers. No Session ownership or persistent project state.
QList<PlotWidget*> axisWidgets;

SampleIndex sampleIndex(long double value, SampleIndex maximum) {
  if (value <= 0) return 0;
  if (value >= static_cast<long double>(maximum)) return maximum;
  return static_cast<SampleIndex>(value);
}
SampleIndex offsetSample(SampleIndex origin, long double offset,
                         SampleIndex maximum) {
  origin = std::min(origin, maximum);
  return offset >= 0 ? origin + sampleIndex(offset, maximum - origin)
                     : origin - sampleIndex(-offset, origin);
}
double seconds(SampleIndex sample, double sampleRate) {
  return static_cast<double>(sample) / sampleRate;
}
double randomValue(double x) {
  const double n = std::sin(x * 92.743 + 12.41) * 143758.327;
  return n - std::floor(n);
}
QColor cssColor(const char* value) {
  const QString hex = QString::fromLatin1(value);
  if (hex.size() != 9) return QColor(hex);
  return QColor(
      hex.mid(1, 2).toInt(nullptr, 16), hex.mid(3, 2).toInt(nullptr, 16),
      hex.mid(5, 2).toInt(nullptr, 16), hex.mid(7, 2).toInt(nullptr, 16));
}
QFont canvasFont(int pixels = 10, bool labels = false) {
  QFont font(labels ? QStringLiteral("Microsoft YaHei UI")
                    : QStringLiteral("Consolas"));
  font.setPixelSize(pixels);
  return font;
}
QString numberText(double value, int decimals) {
  if (!std::isfinite(value)) return QStringLiteral("—");
  decimals = std::clamp(decimals, 0, 12);
  if (std::abs(value) < std::pow(10.0, -decimals) / 2) value = 0;
  auto text = QString::number(value, 'f', decimals);
  if (text.contains('.')) {
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
  }
  return text;
}
int tickDecimals(double step) {
  return std::clamp(static_cast<int>(std::ceil(
                        -std::log10(std::max(std::abs(step), 1e-12)))) +
                        2,
                    0, 12);
}
struct Unit {
  QString name;
  double scale;
};
Unit displayUnit(bool frequency, double span) {
  span = std::abs(span);
  return frequency ? (span >= 1e9   ? Unit{"GHz", 1e-9}
                      : span >= 1e6 ? Unit{"MHz", 1e-6}
                      : span >= 1e3 ? Unit{"kHz", 1e-3}
                                    : Unit{"Hz", 1})
                   : (span >= 1      ? Unit{"s", 1}
                      : span >= .001 ? Unit{"ms", 1e3}
                                     : Unit{"us", 1e6});
}
QString coordinateText(bool frequency, double value, double span = 0,
                       double step = 0) {
  const double magnitude = std::abs(value);
  const auto unit =
      displayUnit(frequency, span > 0 && magnitude < span * 1000 ? span
                             : magnitude > 0                     ? magnitude
                             : span > 0                          ? span
                             : frequency                         ? 1e6
                                                                 : 1);
  const double resolution = std::abs(step != 0   ? step
                                     : span > 0  ? span / 5
                                     : frequency ? 1
                                                 : 1e-6);
  return numberText(value * unit.scale, tickDecimals(resolution * unit.scale)) +
         " " + unit.name;
}
struct Axis {
  double start = 0, span = 0;
  double displayStart = 0;
  Unit unit;
  int count = 5, decimals = 0;
  bool relative = false;
  QString label, originLabel;
  QString tick(double fraction) const {
    return numberText((displayStart + span * fraction) * unit.scale, decimals);
  }
};
Axis axisScale(bool frequency, double start, double span, int count,
               double offset = 0) {
  Axis axis;
  axis.start = start;
  axis.span = span;
  axis.count = count;
  axis.unit = displayUnit(frequency, std::abs(span));
  axis.decimals = tickDecimals(span / std::max(1, count) * axis.unit.scale);
  const double lo = std::min(start, start + span) - offset;
  const double hi = std::max(start, start + span) - offset;
  axis.relative =
      std::abs(span) > 0 &&
      (std::max(std::abs(lo), std::abs(hi)) >= std::abs(span) * 1000 ||
       numberText(lo * axis.unit.scale, axis.decimals).size() > 9 ||
       numberText(hi * axis.unit.scale, axis.decimals).size() > 9);
  axis.displayStart =
      axis.relative ? (span < 0 ? std::abs(span) : 0) : start - offset;
  axis.label = QString(frequency ? "频率" : "时间") +
               (axis.relative ? " Δ" : "") + " (" + axis.unit.name + ")" +
               (!axis.relative && offset != 0 ? " · 基带" : "");
  if (axis.relative)
    axis.originLabel =
        QString(frequency ? "F₀ = " : "T₀ = ") +
        coordinateText(frequency, lo, 0, std::abs(span) / std::max(1, count)) +
        (offset != 0 ? "（基带）" : "");
  return axis;
}
QString rangeText(bool frequency, double start, double span) {
  const auto axis = axisScale(frequency, start, span, 5);
  return axis.relative
             ? coordinateText(frequency, start, 0, span / 5) + " + [" +
                   axis.tick(0) + "–" + axis.tick(1) + "] " + axis.unit.name
             : axis.tick(0) + "–" + axis.tick(1) + " " + axis.unit.name;
}
Qt::CursorShape edgeCursor(int edges) {
  if (edges == Move) return Qt::SizeAllCursor;
  if ((edges & (Left | Right)) && (edges & (Top | Bottom)))
    return edges == (Left | Top) || edges == (Right | Bottom)
               ? Qt::SizeFDiagCursor
               : Qt::SizeBDiagCursor;
  return edges & (Left | Right) ? Qt::SizeHorCursor : Qt::SizeVerCursor;
}
std::array<std::pair<QPointF, int>, 8> handles(const QRectF& rect) {
  return {{{rect.topLeft(), Left | Top},
           {{rect.center().x(), rect.top()}, Top},
           {rect.topRight(), Right | Top},
           {{rect.right(), rect.center().y()}, Right},
           {rect.bottomRight(), Right | Bottom},
           {{rect.center().x(), rect.bottom()}, Bottom},
           {rect.bottomLeft(), Left | Bottom},
           {{rect.left(), rect.center().y()}, Left}}};
}
void drawPill(QPainter& painter, QRectF canvas, const QString& text,
              bool creation) {
  painter.save();
  painter.setFont(canvasFont(creation ? 10 : 11, true));
  const double paddingX = creation ? 13 : 12, paddingY = creation ? 7 : 5;
  const QFontMetricsF metrics(painter.font());
  const double width = std::min(
      canvas.width() * .95, metrics.horizontalAdvance(text) + paddingX * 2 + 2);
  const double height = (creation ? 10 : 11) * 1.4 + paddingY * 2 + 2;
  const QRectF rect(canvas.center().x() - width / 2, creation ? 10 : 7, width,
                    height);
  painter.setPen(creation ? QColor("#edc56d") : QColor("#3d7898"));
  painter.setBrush(creation ? cssColor("#6a4a1be8") : cssColor("#19354eeb"));
  painter.drawRoundedRect(rect, 4, 4);
  painter.setPen(creation ? QColor("#fff5d8") : QColor("#a9eeff"));
  painter.drawText(rect.adjusted(paddingX, 0, -paddingX, 0), Qt::AlignCenter,
                   metrics.elidedText(text, Qt::ElideRight,
                                      static_cast<int>(width - 2 * paddingX)));
  painter.restore();
}
void addMenuHeading(QMenu& menu, const QString& text) {
  auto* action = new QWidgetAction(&menu);
  auto* label = new QLabel(text, &menu);
  label->setStyleSheet(
      "color:#7fadc8;padding:6px 11px;font-size:11px;font-weight:600;");
  action->setDefaultWidget(label);
  menu.addAction(action);
}
}  // namespace

QString sourceFingerprint(const FileMetadata& metadata) {
  if (metadata.demo) return "demo";
  const QFileInfo info(QString::fromStdString(metadata.path));
  return QString::number(info.size()) + "/" +
         QString::number(info.lastModified().toMSecsSinceEpoch()) + "/" +
         QString::fromStdString(sampleFormatKey(metadata.sampleFormat)) + "/" +
         QString::number(metadata.availability.generation);
}

struct HeatmapPayload {
  QString key, fingerprint;
  FileMetadata metadata;
  ViewRange view;
  MainMode mode = MainMode::TimeFrequency;
  QSize pixels;
  quint64 generation = 0;
  int fftSize = 2048;
  bool preview = false;
  SpectrogramSettings settings;
  std::vector<float> power;
  std::shared_ptr<SpectrogramData> spectrum;
  QString error;
  std::size_t sourcePoints = 0;
  double elapsedMs = 0;
};

struct CurvePayload {
  QString key;
  quint64 generation = 0;
  std::vector<float> samples;
  std::shared_ptr<const SpectralFrame> average;
  QString error;
  double elapsedMs = 0;
};

// One worker owns a replaceable mailbox. It never reads Session or QWidget.
// A newer generation cancels work at each raster row, and the GUI checks it
// again before installing a result. Destruction joins before Session can die.
class HeatmapWorker {
 public:
  using Callback = std::function<void(std::shared_ptr<HeatmapPayload>)>;
  explicit HeatmapWorker(Callback callback)
      : callback_(std::move(callback)), thread_([this] { run(); }) {}
  ~HeatmapWorker() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      pending_.reset();
    }
    changed_.notify_one();
    thread_.join();
  }
  void submit(std::shared_ptr<HeatmapPayload> request) {
    latest_ = request->generation;
    {
      std::lock_guard lock(mutex_);
      if (pending_) ++discarded_;
      pending_ = std::move(request);
      pendingFlag_ = true;
    }
    changed_.notify_one();
  }
  void cancel(quint64 generation) {
    latest_ = generation;
    std::lock_guard lock(mutex_);
    if (pending_) {
      pending_.reset();
      pendingFlag_ = false;
      ++discarded_;
    }
  }
  quint64 discarded() const { return discarded_.load(); }
  bool idle() const { return !working_ && !pendingFlag_; }

 private:
  void run() {
    for (;;) {
      std::shared_ptr<HeatmapPayload> job;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return stop_ || pending_; });
        if (stop_) return;
        working_ = true;
        job = std::move(pending_);
        pendingFlag_ = false;
      }
      QElapsedTimer timer;
      timer.start();
      const int width = job->pixels.width(), height = job->pixels.height();
      const auto& view = job->view;
      const auto& metadata = job->metadata;
      const auto cancel = [this, generation = job->generation] {
        return stop_ || generation != latest_;
      };
      if (!metadata.demo) {
        Int16IqFile iq;
        QString error;
        if (!iq.open(metadata, error))
          job->error = error;
        else {
          iq.limitTo(availableSamples(metadata));
          auto source = iq.spectralSource();
          source.sampleRateHz = metadata.sampleRateHz;
          job->spectrum = analyzeSpectrogram(
              source, view.time,
              {view.frequency.lowerHz - analysisFrequencyOffset(metadata),
               view.frequency.upperHz - analysisFrequencyOffset(metadata)},
              job->fftSize, job->mode == MainMode::Waterfall ? height : width,
              job->settings, 1, cancel);
        }
      } else {
        job->spectrum = std::make_shared<SpectrogramData>();
        auto& data = *job->spectrum;
        data.plan = makeSpectralAnalysisPlan(
            metadata.sampleRateHz,
            {view.frequency.lowerHz - metadata.centerFrequencyHz,
             view.frequency.upperHz - metadata.centerFrequencyHz},
            job->fftSize, job->settings.parameters);
        data.providerView = data.sourceView = view.time;
        if (!data.plan.valid)
          data.error = data.plan.reason;
        else {
          const auto span = view.time.end - view.time.begin;
          const auto remaining =
              span > data.plan.inputSamples ? span - data.plan.inputSamples : 0;
          const auto hop = std::max<std::uint64_t>(
              1, static_cast<std::uint64_t>(
                     data.plan.inputSamples *
                     (1 - job->settings.parameters.overlap)));
          const int count = static_cast<int>(std::min<std::uint64_t>(
              1 + remaining / hop,
              std::min(job->mode == MainMode::Waterfall ? height : width,
                       std::max(1, 8'000'000 / data.plan.analysisSamples))));
          for (int index = 0; index < count && !cancel(); ++index) {
            const auto start = view.time.begin +
                               (count == 1 ? remaining / 2
                                           : (remaining / (count - 1)) * index +
                                                 (remaining % (count - 1)) *
                                                     index / (count - 1));
            auto frame = std::make_shared<SpectralFrame>();
            frame->id = index + 1;
            frame->providerSamples = frame->sourceSamples = {
                start, std::min(view.time.end, start + data.plan.inputSamples)};
            frame->sourceCenter =
                start + (frame->sourceSamples.end - start) / 2;
            frame->displayFraction =
                static_cast<long double>(frame->sourceCenter -
                                         view.time.begin) /
                span;
            frame->validSamples = std::min<std::uint64_t>(
                data.plan.analysisSamples,
                (view.time.end - start) / data.plan.decimation);
            frame->paddedSamples =
                data.plan.analysisSamples - frame->validSamples;
            frame->frequencies = data.plan.frequencies;
            frame->binHz = data.plan.binHz;
            frame->linearPower.resize(job->fftSize);
            const double tn =
                static_cast<double>(frame->sourceCenter) / metadata.sampleCount;
            for (int bin = 0; bin < job->fftSize; ++bin) {
              const double fn = frame->frequencyAt(bin) / metadata.sampleRateHz;
              double z = .07 +
                         .14 * randomValue(std::floor(tn * 1200) * .83 +
                                           std::floor(fn * 1000) * 7 +
                                           metadata.demoSeed) +
                         .07 * std::sin(tn * 85 + fn * 91);
              for (const double center : {-.29, -.12, .09, .31})
                z += .38 * std::exp(-std::pow((fn - center) / .005, 2));
              if (tn > .22 && tn < .75)
                z +=
                    .58 * std::exp(-std::pow((fn - .075) / .035, 2)) *
                    (.45 +
                     .55 * std::pow(std::sin(tn * 120 + metadata.demoSeed), 2));
              if (tn > .38 && tn < .54 && std::sin(tn * 140) > .2)
                z += .48 * std::exp(-std::pow((fn + .02) / .09, 2));
              if (tn > .18 && tn < .75)
                z += .3 * std::exp(-std::pow(
                              (fn - (-.28 + .5 * (tn - .18))) / .008, 2));
              frame->linearPower[bin] =
                  static_cast<float>(std::pow(10.0, (-140 + z * 140) / 10));
            }
            data.frames.push_back(std::move(frame));
          }
        }
      }
      if (job->spectrum) {
        job->error = QString::fromStdString(job->spectrum->error);
        job->power = spectralRaster(*job->spectrum, width, height, job->mode);
        job->sourcePoints = job->spectrum->frames.size() * job->fftSize;
        if (metadata.demo)
          for (auto& value : job->power) value = (value + 140) / 140;
      }
      if (job->power.empty())
        job->power.assign(static_cast<std::size_t>(width) * height,
                          metadata.demo ? 0.f : -200.f);
      if (cancel()) {
        ++discarded_;
        working_ = false;
        continue;
      }
      job->elapsedMs = timer.nsecsElapsed() / 1e6;
      callback_(std::move(job));
      working_ = false;
    }
  }
  Callback callback_;
  std::atomic<bool> stop_ = false;
  std::atomic<bool> working_ = false, pendingFlag_ = false;
  std::atomic<quint64> latest_ = 0, discarded_ = 0;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::shared_ptr<HeatmapPayload> pending_;
  std::thread thread_;
};

class CurveWorker {
 public:
  using Callback = std::function<void(std::shared_ptr<CurvePayload>)>;
  struct Request {
    QString key;
    FileMetadata metadata;
    TimeRange time;
    FrequencyRange frequency;
    int fftSize = 4096;
    int points = 1024;
    bool psd = false;
    PsdSettings settings;
    WaveformMode waveformMode = WaveformMode::IqRms;
    quint64 generation = 0;
  };
  explicit CurveWorker(Callback callback)
      : callback_(std::move(callback)), thread_([this] { run(); }) {}
  ~CurveWorker() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      pending_.reset();
    }
    changed_.notify_one();
    thread_.join();
  }
  void submit(Request request) {
    latest_ = request.generation;
    {
      std::lock_guard lock(mutex_);
      pending_ = std::move(request);
    }
    changed_.notify_one();
  }
  void cancel(quint64 generation) {
    latest_ = generation;
    std::lock_guard lock(mutex_);
    pending_.reset();
  }
  quint64 discarded() const { return discarded_.load(); }

 private:
  void run() {
    for (;;) {
      Request request;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return stop_ || pending_.has_value(); });
        if (stop_) return;
        request = std::move(*pending_);
        pending_.reset();
      }
      QElapsedTimer timer;
      timer.start();
      auto result = std::make_shared<CurvePayload>();
      result->key = request.key;
      result->generation = request.generation;
      Int16IqFile iq;
      QString error;
      if (!iq.open(request.metadata, error)) {
        result->error = error;
      } else {
        const auto cancelled = [this, generation = request.generation] {
          return stop_ || generation != latest_;
        };
        iq.limitTo(availableSamples(request.metadata));
        bool ok = false;
        if (request.psd) {
          auto source = iq.spectralSource();
          source.sampleRateHz = request.metadata.sampleRateHz;
          const FrequencyRange band{
              request.frequency.lowerHz -
                  analysisFrequencyOffset(request.metadata),
              request.frequency.upperHz -
                  analysisFrequencyOffset(request.metadata)};
          std::string message;
          result->average =
              analyzeSpectrum(source, request.time, band, request.fftSize,
                              request.settings, message, cancelled);
          result->error = QString::fromStdString(message);
          if (result->average) {
            result->samples = spectrumDb(*result->average);
            ok = true;
          }
        } else
          ok = iq.waveform(request.time, request.points, request.waveformMode,
                           result->samples, cancelled);
        if (!ok) {
          if (stop_ || request.generation != latest_) {
            ++discarded_;
            continue;
          }
          result->error = QStringLiteral("IQ 曲线计算失败");
        }
      }
      if (stop_ || request.generation != latest_) {
        ++discarded_;
        continue;
      }
      result->elapsedMs = timer.nsecsElapsed() / 1e6;
      callback_(std::move(result));
    }
  }
  Callback callback_;
  std::atomic<bool> stop_ = false;
  std::atomic<quint64> latest_ = 0, discarded_ = 0;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Request> pending_;
  std::thread thread_;
};

struct PlotWidget::AxisLayoutCache {
  struct Key {
    const FileState* file = nullptr;
    std::uint64_t projectGeneration = 0;
    ViewRange view{};
    double sampleRate = 0, frequencyOffset = 0, minimum = 0, maximum = 0;
    MainMode mainMode = MainMode::TimeFrequency;
    AuxiliaryMode auxiliaryMode = AuxiliaryMode::Waveform;
    WaveformMode waveformMode = WaveformMode::IqRms;
    SampleFormat format;
    bool demo = false;
    AxisTitleMode mode = AxisTitleMode::UnitAtTop;
    QFont font;
    QSize size;
    int dpiX = 0, dpiY = 0;
    double dpr = 1;
    bool operator==(const Key&) const = default;
  } key;
  Axis x, y;
  axis_layout::Labels labels;
  axis_layout::Budget budget;
};

const PlotWidget::AxisLayoutCache& PlotWidget::measuredAxes() const {
  const auto* file = session_.activeFile();
  AxisLayoutCache::Key key;
  key.file = file;
  key.projectGeneration = session_.projectGeneration();
  key.mode = axisTitleMode_;
  key.font = font();
  key.size = size();
  key.dpiX = logicalDpiX();
  key.dpiY = logicalDpiY();
  key.dpr = devicePixelRatioF();
  if (file) {
    key.view = file->view;
    key.sampleRate = file->metadata.sampleRateHz;
    key.frequencyOffset = file->display.absoluteFrequency
                              ? 0 : analysisFrequencyOffset(file->metadata);
    key.minimum = file->display.auxiliaryMin;
    key.maximum = file->display.auxiliaryMax;
    key.mainMode = file->display.mainMode;
    key.auxiliaryMode = file->display.auxiliaryMode;
    key.waveformMode = file->display.waveformMode;
    key.format = file->metadata.sampleFormat;
    key.demo = file->metadata.demo;
  }
  if (axisLayoutCache_ && axisLayoutCache_->key == key)
    return *axisLayoutCache_;
  auto cache = std::make_unique<AxisLayoutCache>();
  cache->key = key;
  if (file) {
    const auto& view = file->view;
    const auto& metadata = file->metadata;
    const double rate = std::max(1e-12, metadata.sampleRateHz);
    const double timeStart = seconds(view.time.begin, rate);
    const double timeSpan = seconds(view.time.end - view.time.begin, rate);
    const double frequencySpan = view.frequency.upperHz - view.frequency.lowerHz;
    const bool waterfall = kind_ == Kind::Main &&
                           key.mainMode == MainMode::Waterfall;
    const bool psd = kind_ == Kind::Auxiliary &&
                     key.auxiliaryMode == AuxiliaryMode::Psd;
    cache->x = waterfall || psd
                   ? axisScale(true, view.frequency.lowerHz, frequencySpan, 5,
                               key.frequencyOffset)
                   : axisScale(false, timeStart, timeSpan, 5);
    cache->y = waterfall
                   ? axisScale(false, timeStart + timeSpan, -timeSpan, 4)
                   : axisScale(true, view.frequency.lowerHz, frequencySpan, 4,
                               key.frequencyOffset);
    auto& labels = cache->labels;
    labels.xTitle = cache->x.label;
    for (int i = 0; i <= cache->x.count; ++i)
      labels.xTicks.append(cache->x.tick(static_cast<double>(i) / cache->x.count));
    for (int i = 0; i <= 4; ++i) {
      const double value = key.minimum + (key.maximum - key.minimum) * i / 4;
      labels.yTicks.append(kind_ == Kind::Main ? cache->y.tick(i / 4.0)
          : (std::abs(value) >= 1e7 || (value != 0 && std::abs(value) < 1e-4))
              ? QString::number(value, 'g', 6)
              : QString::number(value, 'f', psd ? 0 : 1));
    }
    labels.yTitle = sourceUnits(
        kind_ == Kind::Main ? cache->y.label
        : psd ? "功率谱密度（dBFS/Hz）"
        : metadata.demo ? "幅值（演示单位）"
        : key.waveformMode == WaveformMode::I ? "I 分量（ADC 计数）"
        : key.waveformMode == WaveformMode::Q ? "Q 分量（ADC 计数）"
        : key.waveformMode == WaveformMode::Envelope ? "幅度包络（ADC 计数）"
                                                    : "幅度 RMS（ADC 计数）",
        metadata);
    labels.yUnit = kind_ == Kind::Main
        ? (cache->y.relative ? QStringLiteral("Δ ") : QString{}) +
              cache->y.unit.name +
              (!cache->y.relative && key.frequencyOffset != 0 && !waterfall
                   ? QStringLiteral(" · 基带") : QString{})
        : psd ? powerUnit(metadata)
        : metadata.demo ? QStringLiteral("演示单位") : amplitudeUnit(metadata);
  }
  cache->budget = axis_layout::measure(
      cache->labels, QFontMetricsF(key.font, this),
      static_cast<axis_layout::TitleMode>(axisTitleMode_), width(), height());
  axisLayoutCache_ = std::move(cache);
  return *axisLayoutCache_;
}

axis_layout::Layout PlotWidget::axisLayout() const {
  auto budget = measuredAxes().budget;
  if (frozenAxisBudget_)
    budget = *frozenAxisBudget_;
  else
    for (const auto* peer : axisWidgets)
      if (peer != this && peer->kind_ != Kind::Navigation &&
          &peer->session_ == &session_ && peer->window() == window())
        budget = axis_layout::sharedBudget(budget, peer->measuredAxes().budget,
            axisTitleMode_ == AxisTitleMode::CompactHeader);
  const auto* file = session_.activeFile();
  return axis_layout::arrange(size(), budget,
      kind_ == Kind::Main && file && file->display.colorScale ? 70 : 22);
}

void PlotWidget::refreshAxisLayouts() {
  for (auto* peer : axisWidgets)
    if (peer->kind_ != Kind::Navigation && &peer->session_ == &session_ &&
        peer->window() == window()) {
      const auto budget = peer->axisLayout().budget;
      peer->setMinimumHeight(std::max(peer->kind_ == Kind::Main ? 110 : 60,
          static_cast<int>(std::ceil(budget.top + budget.bottom + 16))));
      peer->updateCurve();
      peer->repaintChart();
      emit peer->axisDescriptionChanged();
    }
}

void PlotWidget::freezeAxisLayouts() {
  for (auto* peer : axisWidgets)
    if (peer->kind_ != Kind::Navigation && &peer->session_ == &session_ &&
        peer->window() == window())
      peer->frozenAxisBudget_ = peer->axisLayout().budget;
}

void PlotWidget::releaseAxisLayouts() {
  for (auto* peer : axisWidgets)
    if (peer->kind_ != Kind::Navigation && &peer->session_ == &session_ &&
        peer->window() == window())
      peer->frozenAxisBudget_.reset();
  refreshAxisLayouts();
}

void PlotWidget::setAxisTitleMode(AxisTitleMode mode) {
  if (kind_ == Kind::Navigation || axisTitleMode_ == mode) return;
  if (mode != AxisTitleMode::UnitAtTop && mode != AxisTitleMode::Header &&
      mode != AxisTitleMode::Rotated && mode != AxisTitleMode::CompactHeader) return;
  for (auto* peer : axisWidgets)
    if (peer->kind_ != Kind::Navigation && &peer->session_ == &session_ &&
        peer->window() == window()) {
      peer->cancelGesture();
      peer->axisTitleMode_ = mode;
      peer->axisLayoutCache_.reset();
    }
  refreshAxisLayouts();
}

AxisDescription PlotWidget::axisDescription() const {
  const auto& axes = measuredAxes();
  const auto* file = session_.activeFile();
  if (!file) return {};
  const bool waterfall = kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall;
  const bool psd = kind_ == Kind::Auxiliary && file->display.auxiliaryMode == AuxiliaryMode::Psd;
  const auto physical = [&](const Axis& axis, bool frequency) {
    return QString(frequency ? (file->display.absoluteFrequency ? "RF频率" : "基带频率") : "时间") +
        (axis.relative ? " Δ" : "") + " (" + axis.unit.name + ")";
  };
  AxisDescription result;
  result.xTitle = result.xShort = physical(axes.x, waterfall || psd);
  result.yTitle = kind_ == Kind::Main ? physical(axes.y, !waterfall) : axes.labels.yTitle;
  result.yShort = kind_ == Kind::Main ? result.yTitle
      : psd ? QStringLiteral("PSD (%1)").arg(axes.labels.yUnit)
            : axes.labels.yTitle;
  result.xOrigin = axes.x.originLabel;
  result.yOrigin = kind_ == Kind::Main ? axes.y.originLabel : QString{};
  return result;
}

QJsonObject PlotWidget::axisLayoutStatistics() const {
  const auto layout = axisLayout();
  const auto& axes = measuredAxes();
  const auto rectJson = [](QRectF rect) {
    return QJsonObject{{"x", rect.x()}, {"y", rect.y()},
                       {"width", rect.width()}, {"height", rect.height()}};
  };
  return {{"mode", axisTitleMode_ == AxisTitleMode::UnitAtTop ? "A"
                  : axisTitleMode_ == AxisTitleMode::Header ? "B"
                  : axisTitleMode_ == AxisTitleMode::Rotated ? "C" : "CompactHeader"},
          {"plot", rectJson(plotRect())}, {"header", rectJson(layout.header)},
          {"xTickBand", rectJson(layout.xTicks)},
          {"xTitleBand", rectJson(layout.xTitle)},
          {"localLeft", axes.budget.left}, {"sharedLeft", layout.budget.left},
          {"tickLabelWidth", axes.budget.tickLabelWidth},
          {"tickHeight", axes.budget.tickHeight},
          {"leftGainFrom88", 88 - layout.budget.left},
          {"yTitle", axes.labels.yTitle}, {"yUnit", axes.labels.yUnit},
          {"xTitle", axes.labels.xTitle},
          {"headerText", axisDescription().fullText()},
          {"widgetWidth", width()}, {"widgetHeight", height()},
          {"localTop", axes.budget.top}, {"localBottom", axes.budget.bottom},
          {"xTicks", QJsonArray::fromStringList(axes.labels.xTicks)},
          {"yTicks", QJsonArray::fromStringList(axes.labels.yTicks)},
          {"xOrigin", axes.x.originLabel},
          {"yOrigin", kind_ == Kind::Main ? axes.y.originLabel : QString{}},
          {"font", font().toString()}, {"logicalDpiX", logicalDpiX()},
          {"logicalDpiY", logicalDpiY()}, {"dpr", devicePixelRatioF()}};
}

PlotWidget::PlotWidget(Session& session, Kind kind, QWidget* parent)
    : QWidget(parent), session_(session), kind_(kind) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(150, kind == Kind::Navigation  ? 20
                      : kind == Kind::Auxiliary ? 60
                                                : 110);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setCursor(kind == Kind::Navigation ? Qt::PointingHandCursor
                                     : Qt::CrossCursor);
  wheelTimer_.setSingleShot(true);
  wheelTimer_.setInterval(220);
  connect(&wheelTimer_, &QTimer::timeout, this, &PlotWidget::finishWheel);
  renderSettleTimer_.setSingleShot(true);
  renderSettleTimer_.setInterval(60);
  connect(&renderSettleTimer_, &QTimer::timeout, this, [this] {
    preview_ = false;
    repaintChart();
  });
  if (kind_ == Kind::Main)
    worker_ = std::make_unique<HeatmapWorker>(
        [this](std::shared_ptr<HeatmapPayload> result) {
          QMetaObject::invokeMethod(
              this,
              [this, result = std::move(result)] { acceptHeatmap(result); },
              Qt::QueuedConnection);
        });
  if (kind_ == Kind::Auxiliary)
    curveWorker_ = std::make_unique<CurveWorker>(
        [this](std::shared_ptr<CurvePayload> result) {
          QMetaObject::invokeMethod(
              this, [this, result = std::move(result)] { acceptCurve(result); },
              Qt::QueuedConnection);
        });
  if (kind_ == Kind::Navigation)
    navigationWorker_ = std::make_unique<CurveWorker>(
        [this](std::shared_ptr<CurvePayload> result) {
          QMetaObject::invokeMethod(
              this,
              [this, result = std::move(result)] {
                acceptNavigationCurve(result);
              },
              Qt::QueuedConnection);
        });
  if (QGuiApplication::platformName() != "offscreen" &&
      QGuiApplication::platformName() != "minimal" &&
      !qApp->property("softwareRenderer").toBool()) {
    surface_ = new AcceleratedSurface(this);
    surface_->setGeometry(rect());
    surface_->setPainter(
        [this](QPainter& painter) { paintScene(painter, true); });
    if (kind_ != Kind::Navigation)
      surface_->setInversePainter(
          [this](QPainter& painter) { drawCursors(painter, true); });
    connect(surface_, &AcceleratedSurface::backendReady, this,
            [this](const QString& backend) { emit backendChanged(backend); });
    connect(surface_, &AcceleratedSurface::backendFailed, this,
            [this](const QString& reason) {
              softwareFallback_ = true;
              surface_->hide();
              QWidget::update();
              emit backendChanged("软件回退 · " + reason);
            });
  }
  axisWidgets.append(this);
  if (kind_ != Kind::Navigation)
    QTimer::singleShot(0, this, [this] { refreshAxisLayouts(); });
}
PlotWidget::~PlotWidget() {
  axisWidgets.removeAll(this);
  wheelTimer_.stop();
  renderSettleTimer_.stop();
  navigationWorker_.reset();
  curveWorker_.reset();
  worker_.reset();
}
bool PlotWidget::gpuReady() const {
  return surface_ && !softwareFallback_ && surface_->isReady();
}
QString PlotWidget::renderingBackend() const {
  return surface_ && !softwareFallback_ ? surface_->backendDescription()
                                        : "QPainter 软件绘制";
}
quint64 PlotWidget::textureUploadCount() const {
  return surface_ ? surface_->textureUploadCount() : 0;
}
quint64 PlotWidget::completedFrameCount() const {
  return surface_ ? surface_->completedFrameCount() : 0;
}
void PlotWidget::repaintChart() {
  updateHeatmap();
  if (surface_ && !softwareFallback_) {
    const auto [target, source] = heatmapPlacement();
    const auto* file = session_.activeFile();
    const auto palette = file ? texture(file->display.palette) : QImage{};
    const auto revision =
        heatmap_.isNull()
            ? QStringLiteral("empty/") + QString::fromStdString(displayedFile_)
            : powerKey_ + QLatin1Char('/') + colorKey_;
    surface_->setHeatmap(heatmap_, target, revision, palette);
    surface_->setHeatmapSourceRect(source);
    surface_->invalidateOverlay();
  } else
    QWidget::update();
}
void PlotWidget::beginPreview() {
  preview_ = true;
  renderSettleTimer_.start();
}
QRectF PlotWidget::plotRect() const {
  if (kind_ == Kind::Navigation)
    return QRectF(13, 0, std::max(1, width() - 26), std::max(1, height()));
  return axisLayout().plot;
}
QPointF PlotWidget::toPixel(SampleIndex sample, double frequency) const {
  const auto* file = session_.activeFile();
  if (!file) return {};
  const auto view =
      kind_ == Kind::Navigation ? fullRange(file->metadata) : file->view;
  const auto rect = plotRect();
  const double delta = sample >= view.time.begin
                           ? static_cast<double>(sample - view.time.begin)
                           : -static_cast<double>(view.time.begin - sample);
  const double time =
      delta / static_cast<double>(view.time.end - view.time.begin);
  const double f = (frequency - view.frequency.lowerHz) /
                   (view.frequency.upperHz - view.frequency.lowerHz);
  return kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall
             ? QPointF(rect.left() + rect.width() * f,
                       rect.top() + rect.height() * time)
             : QPointF(rect.left() + rect.width() * time,
                       rect.bottom() - rect.height() * f);
}
PlotWidget::Coordinate PlotWidget::fromPixel(QPointF point,
                                             const ViewRange& view) const {
  const auto rect = plotRect();
  const auto* file = session_.activeFile();
  const double x =
      std::clamp((point.x() - rect.left()) / rect.width(), 0.0, 1.0);
  const double y =
      std::clamp((point.y() - rect.top()) / rect.height(), 0.0, 1.0);
  const bool waterfall = kind_ == Kind::Main && file &&
                         file->display.mainMode == MainMode::Waterfall;
  return {(waterfall ? y : x) *
              static_cast<long double>(view.time.end - view.time.begin),
          view.frequency.lowerHz +
              (waterfall ? x : 1 - y) *
                  (view.frequency.upperHz - view.frequency.lowerHz)};
}
bool PlotWidget::detectionRoiContextMatches() const {
  const auto* file = session_.activeFile();
  return detectionRoi_ && file && kind_ == Kind::Main &&
         file->metadata.id == detectionRoi_->fileId &&
         session_.projectGeneration() == detectionRoi_->projectGeneration &&
         file->metadata.availability.generation ==
             detectionRoi_->availabilityGeneration &&
         file->metadata.availability.status != LoadStatus::Loading &&
         file->display.mainMode == detectionRoi_->mainMode &&
         file->view == detectionRoi_->view &&
         fullRange(file->metadata) == detectionRoi_->bounds;
}
std::optional<ViewRange> PlotWidget::detectionRoiRange(
    const Gesture& gesture) const {
  if (!detectionRoiContextMatches() || !gesture.changed) return std::nullopt;
  const auto box = QRectF(gesture.start, gesture.current)
                       .normalized()
                       .intersected(plotRect());
  if (box.width() < 1 || box.height() < 1) return std::nullopt;
  const auto& draft = *detectionRoi_;
  const auto first = fromPixel(box.topLeft(), draft.view);
  const auto last = fromPixel(box.bottomRight(), draft.view);
  if (!std::isfinite(first.sample) || !std::isfinite(last.sample) ||
      !std::isfinite(first.frequency) || !std::isfinite(last.frequency))
    return std::nullopt;
  // Round offsets, never absolute sample indices. The upper edge is exclusive.
  ViewRange range{{offsetSample(draft.view.time.begin,
                                std::floor(std::min(first.sample, last.sample)),
                                draft.bounds.time.end),
                   offsetSample(draft.view.time.begin,
                                std::ceil(std::max(first.sample, last.sample)),
                                draft.bounds.time.end)},
                  {std::max(draft.bounds.frequency.lowerHz,
                            std::min(first.frequency, last.frequency)),
                   std::min(draft.bounds.frequency.upperHz,
                            std::max(first.frequency, last.frequency))}};
  range.time.begin = std::max(range.time.begin, draft.view.time.begin);
  range.time.end = std::min(range.time.end, draft.view.time.end);
  if (range.time.begin >= range.time.end ||
      range.frequency.lowerHz >= range.frequency.upperHz)
    return std::nullopt;
  return range;
}
QRectF PlotWidget::markRect(const ViewRange& range) const {
  return QRectF(toPixel(range.time.begin, range.frequency.lowerHz),
                toPixel(range.time.end, range.frequency.upperHz))
      .normalized();
}
QRectF PlotWidget::navigationWindow() const {
  const auto* file = session_.activeFile();
  if (!file) return {};
  const auto plot = plotRect();
  const double start = static_cast<double>(file->view.time.begin) /
                       availableSamples(file->metadata);
  const double span =
      static_cast<double>(file->view.time.end - file->view.time.begin) /
      availableSamples(file->metadata);
  return QRectF(plot.left() + plot.width() * start, 0, plot.width() * span,
                height());
}
PlotWidget::Zone PlotWidget::zoneAt(QPointF point) const {
  if (kind_ == Kind::Navigation)
    return rect().contains(point.toPoint()) ? Zone::Plot : Zone::None;
  return chart_feedback::zoneAt(point, plotRect());
}
QString PlotWidget::statusText() const {
  const auto* file = session_.activeFile();
  if (!file) return "—";
  if (kind_ == Kind::Navigation)
    return rangeText(
        false, 0,
        seconds(availableSamples(file->metadata), file->metadata.sampleRateHz));
  if (kind_ == Kind::Auxiliary)
    return QString(file->display.auxiliaryMode == AuxiliaryMode::Psd
                       ? "X 频率 · Y "
                       : "X 时间 · Y ") +
           numberText(file->display.auxiliaryMin, 0) + "…" +
           numberText(file->display.auxiliaryMax, 0) +
           (file->display.auxiliaryMode == AuxiliaryMode::Psd ? " dB" : "");
  return file->display.mainMode == MainMode::Waterfall
             ? "X 频率 · Y 时间↓ · 颜色功率"
             : "X 时间 · Y 频率 · 颜色功率";
}
QString PlotWidget::tipText() const {
  const auto* file = session_.activeFile();
  if (!file || kind_ != Kind::Auxiliary) return {};
  return sourceUnits(
      file->display.auxiliaryMode == AuxiliaryMode::Waveform
          ? QString(file->display.waveformMode == WaveformMode::I
                        ? "I 分量（ADC 计数）"
                    : file->display.waveformMode == WaveformMode::Q
                        ? "Q 分量（ADC 计数）"
                    : file->display.waveformMode == WaveformMode::Envelope
                        ? "幅度包络（ADC 计数）"
                        : "幅度 RMS（ADC 计数）") +
                " · 当前时间 " +
                rangeText(
                    false,
                    seconds(file->view.time.begin, file->metadata.sampleRateHz),
                    seconds(file->view.time.end - file->view.time.begin,
                            file->metadata.sampleRateHz))
          : QString(
                session_.linkedCursor(file->metadata.id).framePsd
                    ? "驻留帧谱 · 与 STFT 帧逐 bin 一致"
                : file->display.psd.scope == PsdScope::SourceMark ? "来源标记"
                : file->display.psd.scope == PsdScope::Whole ? "整个文件"
                                                             : "当前时间窗") +
                " · N=" +
                QString::number(
                    session_.linkedCursor(file->metadata.id).framePsd
                        ? file->display.stftSize
                        : file->display.psdSize) +
                "（可见频段）" +
                (averagePower_ &&
                         !session_.linkedCursor(file->metadata.id).framePsd
                     ? QString(
                           " · %1 · 有效 %2 ms · 补零 %3 · ENBW %4 Hz · %5 段")
                           .arg(file->display.psd.statistic ==
                                        SpectrumStatistic::Mean
                                    ? "平均谱"
                                : file->display.psd.statistic ==
                                        SpectrumStatistic::Maximum
                                    ? "最大谱"
                                    : "最小谱")
                           .arg(averagePower_->observedSeconds * 1000, 0, 'g',
                                6)
                           .arg(averagePower_->paddedSamples)
                           .arg(averagePower_->noiseBandwidthHz, 0, 'g', 6)
                           .arg(averagePower_->processedFrames)
                     : QString{}),
      file->metadata);
}
void PlotWidget::setCursorCoordinates(SampleIndex sample, double frequency) {
  if (cursorSample_ == sample && cursorFrequency_ == frequency) return;
  cursorSample_ = sample;
  cursorFrequency_ = frequency;
  repaintChart();
}
void PlotWidget::setHoveredMark(const QString& id, const QString& segmentId) {
  if (auto* file = session_.activeFile())
    if (session_.annotations().hover(*file, id.toStdString(),
                                     segmentId.toStdString())) {
      refreshAnnotationOverlay();
      emit annotationHoverChanged();
    }
}
void PlotWidget::setDetectionPreview(std::vector<ChartPreviewTrack> tracks,
                                     std::uint64_t dropped) {
  detectionPreview_ = std::move(tracks);
  detectionPreviewDropped_ = dropped;
  if (!selectedPreviewId_.empty() &&
      std::none_of(detectionPreview_.begin(), detectionPreview_.end(),
                   [&](const auto& track) { return track.id == selectedPreviewId_; }))
    selectedPreviewId_.clear();
  setProperty("detectionPreviewCount", static_cast<int>(detectionPreview_.size()));
  setProperty("selectedDetectionPreviewId",
              QString::fromStdString(selectedPreviewId_));
  // Overlay only. Clearing or updating dashed boxes must not rebuild the
  // spectrogram.
  if (kind_ != Kind::Main) return;
  if (surface_ && !softwareFallback_) surface_->invalidateOverlay();
  QWidget::update();
}
void PlotWidget::selectDetectionPreview(const QString& id) {
  const auto text = id.toStdString();
  if (!text.empty() &&
      std::none_of(detectionPreview_.begin(), detectionPreview_.end(),
                   [&](const auto& track) { return track.id == text; }))
    return;
  selectedPreviewId_ = text;
  setProperty("selectedDetectionPreviewId", id);
  if (kind_ != Kind::Main) return;
  if (surface_ && !softwareFallback_) surface_->invalidateOverlay();
  QWidget::update();
}
QString PlotWidget::selectedDetectionPreview() const {
  return QString::fromStdString(selectedPreviewId_);
}
void PlotWidget::refreshAnnotationOverlay() {
  const auto* file = session_.activeFile();
  setProperty("hoveredAnnotationId",
              file ? QString::fromStdString(file->hoveredMarkId) : QString{});
  setProperty(
      "hoveredAnnotationSegmentId",
      file ? QString::fromStdString(file->hoveredSegmentId) : QString{});
  repaintChart();
}
void PlotWidget::setCreating(bool enabled) {
  cancelGesture();
  creating_ = enabled && session_.activeFile() && kind_ == Kind::Main;
  setHoveredMark({});
  setCursor(creating_ ? Qt::CrossCursor : Qt::ArrowCursor);
  repaintChart();
  emit stateChanged();
  emit statusMessage(creating_ ? "持续选择信号已开启；右键菜单关闭，Esc 退出"
                               : "持续选择信号已关闭");
}
void PlotWidget::beginDetectionRoi() {
  if (activeGesture && activeGesture != this &&
      activeGesture->window() == window())
    activeGesture->cancelGesture();
  if (activeWheel && activeWheel != this && activeWheel->window() == window())
    activeWheel->finishWheel();
  cancelGesture();
  const auto* file = session_.activeFile();
  if (!file || kind_ != Kind::Main ||
      file->metadata.availability.status == LoadStatus::Loading ||
      !availableSamples(file->metadata)) {
    emit statusMessage("检测 ROI 需要已载入的可用数据");
    return;
  }
  const auto bounds = fullRange(file->metadata);
  const auto& view = file->view;
  if (!std::isfinite(bounds.frequency.lowerHz) ||
      !std::isfinite(bounds.frequency.upperHz) ||
      !std::isfinite(view.frequency.lowerHz) ||
      !std::isfinite(view.frequency.upperHz) ||
      view.time.begin >= view.time.end || view.time.end > bounds.time.end ||
      view.frequency.lowerHz < bounds.frequency.lowerHz ||
      view.frequency.upperHz > bounds.frequency.upperHz ||
      view.frequency.lowerHz >= view.frequency.upperHz) {
    emit statusMessage("当前视图没有合法的检测 ROI 物理范围");
    return;
  }
  DetectionRoiDraft draft;
  draft.fileId = file->metadata.id;
  draft.projectGeneration = session_.projectGeneration();
  draft.availabilityGeneration = file->metadata.availability.generation;
  draft.mainMode = file->display.mainMode;
  draft.view = view;
  draft.bounds = bounds;
  draft.previousCreating = creating_;
  detectionRoi_ = draft;
  creating_ = false;
  setHoveredMark({});
  setFocus(Qt::OtherFocusReason);
  setCursor(Qt::CrossCursor);
  repaintChart();
  emit detectionRoiPreviewChanged();
  emit stateChanged();
  emit statusMessage("检测 ROI：拖动后待确认 · Enter 确认 / Esc 恢复前一工具");
}
void PlotWidget::confirmDetectionRoi() {
  if (!detectionRoi_) return;
  if (!detectionRoiContextMatches()) {
    cancelDetectionRoi();
    emit statusMessage("数据或视图已变化，请重新选择检测 ROI");
    return;
  }
  if (gesture_ || !detectionRoi_->preview) return;
  const auto range = *detectionRoi_->preview;
  cancelGesture();
  emit detectionRoiConfirmed(range);
  emit statusMessage("检测 ROI 已确认；可继续配置检测");
}
void PlotWidget::cancelDetectionRoi() {
  if (!detectionRoi_) return;
  cancelGesture();
  emit statusMessage("检测 ROI 已取消；已恢复前一工具");
}
void PlotWidget::syncState() {
  if (kind_ != Kind::Navigation) emit axisDescriptionChanged();
  const auto* file = session_.activeFile();
  const auto id = file ? file->metadata.id : std::string{};
  if (id != displayedFile_ ||
      displayedProjectGeneration_ != session_.projectGeneration()) {
    if (displayedProjectGeneration_ != session_.projectGeneration()) {
      powerCache_.clear();
      curveSourceKey_.clear();
      navigationCurveKey_.clear();
      ++curveRequestGeneration_;
      ++navigationRequestGeneration_;
      if (curveWorker_) curveWorker_->cancel(curveRequestGeneration_);
      if (navigationWorker_)
        navigationWorker_->cancel(navigationRequestGeneration_);
      displayedProjectGeneration_ = session_.projectGeneration();
    }
    cancelGesture(true);
    setHoveredMark({});
    hoverZone_ = Zone::None;
    requestedPowerKey_.clear();
    renderedPower_.reset();
    heatmap_ = {};
    powerKey_.clear();
    colorKey_.clear();
    ++renderGeneration_;
    ++heatmapRequestGeneration_;
    committedGeneration_ = 0;
    requestedHeatSize_ = {};
    if (worker_) worker_->cancel(heatmapRequestGeneration_);
    displayedView_ = file ? std::optional<ViewRange>{file->view} : std::nullopt;
    if (file) {
      cursorSample_ =
          displayedFile_.empty() && file->metadata.demoSeed == 1
              ? sampleIndex(200.0L * file->metadata.sampleRateHz,
                            availableSamples(file->metadata))
              : file->view.time.begin +
                    (file->view.time.end - file->view.time.begin) / 2;
      cursorFrequency_ =
          file->metadata.sampleFormat.structure == SampleStructure::Real
              ? (file->view.frequency.lowerHz + file->view.frequency.upperHz) /
                    2
              : file->metadata.centerFrequencyHz;
      if (kind_ == Kind::Main)
        emit cursorChanged(cursorSample_, cursorFrequency_);
    }
    displayedFile_ = id;
  } else if (file && (file->display.mainMode != displayedMainMode_ ||
                      file->display.auxiliaryMode != displayedAuxiliaryMode_ ||
                      file->display.stftSize != displayedFft_ ||
                      file->display.colorScale != displayedColorScale_))
    cancelGesture();
  if (detectionRoi_ && !detectionRoiContextMatches()) cancelGesture();
  if (!file) creating_ = false;
  if (file) {
    if (displayedView_ && *displayedView_ != file->view) beginPreview();
    displayedView_ = file->view;
    displayedMainMode_ = file->display.mainMode;
    displayedAuxiliaryMode_ = file->display.auxiliaryMode;
    displayedFft_ = file->display.stftSize;
    displayedColorScale_ = file->display.colorScale;
  }
  if (kind_ == Kind::Auxiliary) updateCurve();
  repaintChart();
}
void PlotWidget::finishWheel() {
  wheelTimer_.stop();
  if (!wheelBase_) return;
  const auto before = *wheelBase_;
  wheelBase_.reset();
  if (activeWheel == this) activeWheel.clear();
  if (session_.commitViewChange(before)) emit stateChanged();
  releaseAxisLayouts();
}
void PlotWidget::cancelGesture(bool exitCreating) {
  finishWheel();
  const bool cancellingRoi = detectionRoi_.has_value();
  const bool cancellingGesture = gesture_.has_value();
  if (detectionRoi_) {
    creating_ = detectionRoi_->previousCreating;
    detectionRoi_.reset();
  }
  if (gesture_) {
    const auto gesture = *gesture_;
    gesture_.reset();
    if (activeGesture == this) activeGesture.clear();
    if (gesture.tool != Tool::DetectionRoi) {
      session_.restoreSnapshot(gesture.before);
    }
    releasing_ = true;
    if (mouseGrabber() == this) releaseMouse();
    releasing_ = false;
  }
  if (exitCreating) creating_ = false;
  if (cancellingGesture) releaseAxisLayouts();
  hoverZone_ = Zone::None;
  setHoveredMark({});
  setProperty("interactionZone", static_cast<int>(hoverZone_));
  setCursor(creating_ ? Qt::CrossCursor : Qt::ArrowCursor);
  repaintChart();
  if (cancellingRoi) emit detectionRoiPreviewChanged();
  if (cancellingRoi || cancellingGesture) emit stateChanged();
}
void PlotWidget::updateHeatmap() {
  const auto* file = session_.activeFile();
  if (!file || kind_ != Kind::Main ||
      file->metadata.availability.status == LoadStatus::Loading ||
      !availableSamples(file->metadata))
    return;
  // Root's cursor/status refresh can reach this before syncState when the
  // navigation or auxiliary pane changes the shared business view.
  if (file->metadata.id == displayedFile_ && displayedView_ &&
      *displayedView_ != file->view) {
    beginPreview();
    displayedView_ = file->view;
  }
  const auto rect = plotRect();
  // Settled size retains the A1.4.3 simulated matrix. Preview is bounded by
  // the physical pixel budget and a smaller total raster budget.
  const QSize pixels(static_cast<int>(std::lround(
                         std::clamp(rect.width() * (preview_ ? .20 : .65),
                                    120.0, preview_ ? 300.0 : 750.0))),
                     static_cast<int>(std::lround(
                         std::clamp(rect.height() * (preview_ ? .20 : .8), 90.0,
                                    preview_ ? 180.0 : 430.0))));
  requestedHeatSize_ = pixels;
  const auto& view = file->view;
  const auto fingerprint = sourceFingerprint(file->metadata);
  const QString key = QString::fromStdString(file->metadata.id) +
                      QString("/%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11/%12/%13")
                          .arg(file->metadata.demoSeed)
                          .arg(static_cast<int>(file->display.mainMode))
                          .arg(view.time.begin)
                          .arg(view.time.end)
                          .arg(view.frequency.lowerHz, 0, 'g', 17)
                          .arg(view.frequency.upperHz, 0, 'g', 17)
                          .arg(pixels.width())
                          .arg(pixels.height())
                          .arg(file->metadata.sampleRateHz, 0, 'g', 17)
                          .arg(file->metadata.centerFrequencyHz, 0, 'g', 17)
                          .arg(availableSamples(file->metadata))
                          .arg(file->metadata.path.c_str())
                          .arg(file->display.stftSize) +
                      "/" + fingerprint + "/project" +
                      QString::number(session_.projectGeneration()) +
                      QString::fromStdString(spectralParameterKey(
                          file->display.spectrogram.parameters));
  if (key != requestedPowerKey_) {
    requestedPowerKey_ = key;
    ++renderGeneration_;
    ++heatmapRequestGeneration_;
    if (worker_) worker_->cancel(heatmapRequestGeneration_);
    const auto cached =
        std::find_if(powerCache_.begin(), powerCache_.end(),
                     [&](const auto& item) { return item->key == key; });
    if (cached != powerCache_.end()) {
      ++heatmapCacheHits_;
      renderedPower_ = *cached;
      powerKey_ = key;
      heatSize_ = pixels;
      colorKey_.clear();
      committedGeneration_ = renderGeneration_;
      acceptedPreview_ = preview_;
      if (session_.spectrogram(file->metadata.id) != renderedPower_->spectrum) {
        session_.installSpectrogram(file->metadata.id,
                                    renderedPower_->spectrum);
        QTimer::singleShot(0, this, [this] { emit stateChanged(); });
      }
    } else {
      auto request = std::make_shared<HeatmapPayload>();
      request->key = key;
      request->fingerprint = fingerprint;
      request->metadata = file->metadata;
      request->view = view;
      request->mode = file->display.mainMode;
      request->pixels = pixels;
      request->generation = heatmapRequestGeneration_;
      request->preview = preview_;
      request->fftSize = file->display.stftSize;
      request->settings = file->display.spectrogram;
      // Rebuild the raster from retained frames so time-to-frame mapping
      // stays exact while frequency bins use peak-preserving display LOD.
      const auto source = std::find_if(
          powerCache_.begin(), powerCache_.end(), [&](const auto& item) {
            return item->fingerprint == fingerprint && item->view == view &&
                   item->metadata.id == file->metadata.id &&
                   item->mode == file->display.mainMode &&
                   item->metadata.sampleRateHz == file->metadata.sampleRateHz &&
                   item->metadata.centerFrequencyHz ==
                       file->metadata.centerFrequencyHz &&
                   availableSamples(item->metadata) ==
                       availableSamples(file->metadata) &&
                   item->metadata.demoSeed == file->metadata.demoSeed &&
                   item->metadata.path == file->metadata.path &&
                   item->fftSize == file->display.stftSize &&
                   item->pixels.width() >= pixels.width() &&
                   item->pixels.height() >= pixels.height();
          });
      if (source != powerCache_.end()) {
        QElapsedTimer timer;
        timer.start();
        request->power = spectralRaster(*(*source)->spectrum, pixels.width(),
                                        pixels.height(), request->mode);
        if (file->metadata.demo)
          for (auto& value : request->power) value = (value + 140) / 140;
        request->spectrum = (*source)->spectrum;
        request->sourcePoints = (*source)->sourcePoints;
        request->elapsedMs = timer.nsecsElapsed() / 1e6;
        ++matrixReductions_;
        acceptHeatmap(std::move(request));
      } else {
        ++powerGenerations_;
        worker_->submit(std::move(request));
      }
    }
  }
  if (!renderedPower_) return;
  const QString colors = QString("%1/%2/%3")
                             .arg(static_cast<int>(file->display.palette))
                             .arg(file->display.dynamicRangeDb, 0, 'g', 17)
                             .arg(file->display.referenceLevelDb, 0, 'g', 17);
  if (colors == colorKey_) return;
  ++colorTransformGenerations_;
  colorKey_ = colors;
  heatmap_ = QImage(heatSize_, QImage::Format_Indexed8);
  const auto table = chart_palette::colorTable(file->display.palette);
  heatmap_.setColorTable(table);
  for (int y = 0; y < heatSize_.height(); ++y) {
    auto* row = heatmap_.scanLine(y);
    for (int x = 0; x < heatSize_.width(); ++x) {
      const double power =
          renderedPower_
              ->power[static_cast<std::size_t>(y) * heatSize_.width() + x];
      const double level = file->metadata.demo
                               ? power * 80 / file->display.dynamicRangeDb +
                                     file->display.referenceLevelDb / 140
                               : (power - (file->display.referenceLevelDb -
                                           file->display.dynamicRangeDb)) /
                                     file->display.dynamicRangeDb;
      row[x] =
          static_cast<uchar>(std::lround(std::clamp(level, 0.0, 1.0) * 255));
    }
  }
}
void PlotWidget::acceptHeatmap(std::shared_ptr<HeatmapPayload> result) {
  if (result->generation != heatmapRequestGeneration_ ||
      result->key != requestedPowerKey_) {
    ++staleResults_;
    return;
  }
  renderedPower_ = result;
  powerKey_ = result->key;
  heatSize_ = result->pixels;
  colorKey_.clear();
  committedGeneration_ = renderGeneration_;
  acceptedPreview_ = preview_;
  const auto context = result->metadata.id;
  session_.installSpectrogram(context, result->spectrum);
  powerCache_.push_front(std::move(result));
  std::size_t bytes = 0;
  for (const auto& item : powerCache_)
    bytes += item->power.size() * sizeof(float) +
             (item->spectrum ? item->spectrum->bytes() : 0);
  while (powerCache_.size() > 1 &&
         (powerCache_.size() > 4 || bytes > 64U * 1024U * 1024U)) {
    const auto& item = powerCache_.back();
    bytes -= item->power.size() * sizeof(float) +
             (item->spectrum ? item->spectrum->bytes() : 0);
    powerCache_.pop_back();
  }
  repaintChart();
  emit stateChanged();
}
std::pair<QRectF, QRectF> PlotWidget::heatmapPlacement() const {
  const auto* file = session_.activeFile();
  if (!file || !renderedPower_ || heatmap_.isNull() ||
      renderedPower_->metadata.id != file->metadata.id ||
      renderedPower_->mode != file->display.mainMode)
    return {};
  const auto mapped = markRect(renderedPower_->view);
  const auto target = mapped.intersected(plotRect());
  if (target.isEmpty() || mapped.isEmpty()) return {};
  const QRectF uv((target.left() - mapped.left()) / mapped.width(),
                  (target.top() - mapped.top()) / mapped.height(),
                  target.width() / mapped.width(),
                  target.height() / mapped.height());
  return {target, uv};
}
qsizetype PlotWidget::drawnPointCount() const {
  return kind_ == Kind::Main
             ? renderedPower_
                   ? static_cast<qsizetype>(renderedPower_->power.size())
                   : 0
         : kind_ == Kind::Auxiliary ? static_cast<qsizetype>(curveTrace_.size())
                                    : (width() - 26) * 2;
}
qsizetype PlotWidget::sourcePointCount() const {
  return kind_ == Kind::Auxiliary ? static_cast<qsizetype>(curveSource_.size())
         : kind_ == Kind::Main && renderedPower_
             ? static_cast<qsizetype>(renderedPower_->sourcePoints)
             : drawnPointCount();
}
QString PlotWidget::renderQuality() const {
  return preview_ ? "preview" : "settled";
}
std::shared_ptr<const SpectrogramData> PlotWidget::currentSpectrogram() const {
  const auto* file = session_.activeFile();
  if (!file || !renderedPower_ || powerKey_ != requestedPowerKey_ ||
      !renderedPower_->spectrum || renderedPower_->spectrum->frames.empty() ||
      renderedPower_->spectrum->sourceView != file->view.time ||
      renderedPower_->spectrum->plan.frequencies !=
          FrequencyRange{file->view.frequency.lowerHz -
                             analysisFrequencyOffset(file->metadata),
                         file->view.frequency.upperHz -
                             analysisFrequencyOffset(file->metadata)})
    return {};
  return renderedPower_->spectrum;
}
std::shared_ptr<const SpectralFrame> PlotWidget::currentPowerFrame() const {
  const auto* file = session_.activeFile();
  if (!file || file->display.auxiliaryMode != AuxiliaryMode::Psd ||
      curvePending_ || !curveError_.isEmpty())
    return {};
  if (session_.linkedCursor(file->metadata.id).framePsd) {
    const auto spectrum = session_.spectrogram(file->metadata.id);
    const auto* frame = session_.selectedSpectralFrame(file->metadata.id);
    return spectrum && frame
               ? std::shared_ptr<const SpectralFrame>(spectrum, frame)
               : nullptr;
  }
  return averagePower_;
}
bool PlotWidget::isDisplaySettled() const {
  if (preview_ ||
      (surface_ && !softwareFallback_ && surface_->hasPendingUploads()))
    return false;
  const auto* file = session_.activeFile();
  if (!file) return true;
  if (kind_ == Kind::Main)
    return renderedPower_ && committedGeneration_ == renderGeneration_ &&
           worker_->idle() && renderedPower_->key == requestedPowerKey_ &&
           renderedPower_->metadata.id == file->metadata.id &&
           renderedPower_->mode == file->display.mainMode;
  if (kind_ == Kind::Auxiliary && !file->metadata.demo)
    return curveCompleted_ && !curvePending_;
  if (kind_ == Kind::Navigation && !file->metadata.demo)
    return navigationCurveCompleted_ && !navigationCurvePending_;
  return true;
}
QJsonObject PlotWidget::renderStatistics() const {
  QJsonObject result{
      {"axisLayout", axisLayoutStatistics()},
      {"quality", renderQuality()},
      {"settled", isDisplaySettled()},
      {"generation", static_cast<qint64>(renderGeneration_)},
      {"requestedGeneration", static_cast<qint64>(renderGeneration_)},
      {"committedGeneration", static_cast<qint64>(committedGeneration_)},
      {"workerIdle", !worker_ || worker_->idle()},
      {"acceptedHeatmapPreview", acceptedPreview_},
      {"gpuUploadsPending",
       surface_ && !softwareFallback_ && surface_->hasPendingUploads()},
      {"requestedHeatmapWidth", requestedHeatSize_.width()},
      {"requestedHeatmapHeight", requestedHeatSize_.height()},
      {"requestedPixels", static_cast<qint64>(requestedHeatSize_.width()) *
                              requestedHeatSize_.height()},
      {"drawnPoints", static_cast<qint64>(drawnPointCount())},
      {"sourcePoints", static_cast<qint64>(sourcePointCount())},
      {"powerGenerations", static_cast<qint64>(powerGenerations_)},
      {"colorTransformGenerations",
       static_cast<qint64>(colorTransformGenerations_)},
      {"curveSourceGenerations", static_cast<qint64>(curveGenerations_)},
      {"samplePointsVisible", curveShowsSamplePoints_},
      {"samplePointCount", static_cast<qint64>(samplePointCount())},
      {"curvePathElements", curvePath_.elementCount()},
      {"drawnLineSegments",
       static_cast<qint64>(kind_ == Kind::Auxiliary ? curveSegments_.size()
                           : kind_ == Kind::Navigation
                               ? navigationSegments_[0].size() +
                                     navigationSegments_[1].size()
                               : 0)},
      {"curveRasterMethod",
       kind_ == Kind::Main ? "QRhi heatmap texture"
                           : (surface_ && !softwareFallback_
                                  ? "QRhi dynamic vertex buffer"
                                  : "QPainter::drawLines software fallback")},
      {"curveReductions", static_cast<qint64>(curveReductions_)},
      {"curveCacheHits", static_cast<qint64>(curveCacheHits_)},
      {"heatmapCacheHits", static_cast<qint64>(heatmapCacheHits_)},
      {"staleDrops", static_cast<qint64>(staleResults_ +
                                         (worker_ ? worker_->discarded() : 0))},
      {"matrixReductions", static_cast<qint64>(matrixReductions_)},
      {"cpuWallTimings", surface_ ? surface_->cpuWallTimings() : QJsonObject{}},
      {"heatmapWidth", heatSize_.width()},
      {"heatmapHeight", heatSize_.height()},
      {"lastMatrixMs", renderedPower_ ? renderedPower_->elapsedMs : 0.0},
      {"textureUploads", static_cast<qint64>(textureUploadCount())},
      {"overlayUploads",
       static_cast<qint64>(surface_ ? surface_->overlayUploadCount() : 0)},
      {"inverseCursorDrawCalls",
       static_cast<qint64>(surface_ && !softwareFallback_
                               ? surface_->inverseOverlayDrawCallCount()
                               : 0)},
      {"gpuDataRenderer", surface_ && !softwareFallback_
                              ? "QRhi shader / vertex buffer"
                              : "QPainter software fallback"},
      {"gpuVertexUploads",
       static_cast<qint64>(surface_ && !softwareFallback_
                               ? surface_->chartVertexUploadCount()
                               : 0)},
      {"gpuChartDrawCalls",
       static_cast<qint64>(surface_ && !softwareFallback_
                               ? surface_->chartDrawCallCount()
                               : 0)},
      {"gpuHeatmapDrawCalls",
       static_cast<qint64>(surface_ && !softwareFallback_
                               ? surface_->heatmapDrawCallCount()
                               : 0)},
      {"gpuDataDrawCalls",
       static_cast<qint64>(surface_ && !softwareFallback_
                               ? surface_->chartDrawCallCount() +
                                     surface_->heatmapDrawCallCount()
                               : 0)},
      {"gpuPaletteUploads",
       static_cast<qint64>(surface_ && !softwareFallback_
                               ? surface_->paletteUploadCount()
                               : 0)},
      {"heatmapColorMapping", surface_ && !softwareFallback_
                                  ? "QRhi fragment shader + LUT texture"
                                  : "QImage color table software fallback"}};
  if (const auto* file = session_.activeFile()) {
    const auto& cursor = session_.linkedCursor(file->metadata.id);
    result["cursorPinned"] = cursor.pinned;
    result["framePsd"] = cursor.framePsd;
    result["pinnedSourceSample"] = QString::number(cursor.sourceSample);
    result["pinnedFrequencyHz"] = cursor.frequencyHz;
    if (const auto spectrum = session_.spectrogram(file->metadata.id)) {
      result["analysisPoints"] = spectrum->plan.points;
      result["binHz"] = spectrum->plan.binHz;
      result["requiredSeconds"] = spectrum->plan.requiredSeconds;
      result["noiseBandwidthHz"] = spectrum->plan.noiseBandwidthHz;
      result["actualSpectralFrames"] =
          static_cast<qint64>(spectrum->frames.size());
      result["paddedFrames"] = static_cast<qint64>(spectrum->paddedFrames);
      result["effectiveOverlap"] = spectrum->effectiveOverlap;
      result["analysisError"] = QString::fromStdString(spectrum->error);
      result["actualFrameInputSamples"] =
          QString::number(spectrum->plan.inputSamples);
    }
  }
  if (averagePower_) {
    result["psdObservedSeconds"] = averagePower_->observedSeconds;
    result["psdPadding"] = QString::number(averagePower_->paddedSamples);
    result["psdProcessedFrames"] =
        QString::number(averagePower_->processedFrames);
    result["psdNoiseBandwidthHz"] = averagePower_->noiseBandwidthHz;
  }
  return result;
}
void PlotWidget::updateCurve() {
  auto* file = session_.activeFile();
  if (!file || kind_ != Kind::Auxiliary) return;
  if (file->metadata.availability.status == LoadStatus::Loading ||
      availableSamples(file->metadata) == 0)
    return;
  const auto plot = plotRect();
  const auto& view = file->view;
  const bool psd = file->display.auxiliaryMode == AuxiliaryMode::Psd;
  const double duration =
      seconds(availableSamples(file->metadata), file->metadata.sampleRateHz);
  const double timeStart =
                   seconds(view.time.begin, file->metadata.sampleRateHz),
               timeSpan = seconds(view.time.end - view.time.begin,
                                  file->metadata.sampleRateHz);
  const double frequencySpan = view.frequency.upperHz - view.frequency.lowerHz;
  TimeRange calculationTime = view.time;
  if (psd && file->display.psd.scope == PsdScope::Whole)
    calculationTime = {0, availableSamples(file->metadata)};
  if (psd && (file->display.psdFromSelection ||
              file->display.psd.scope == PsdScope::SourceMark)) {
    if (const auto* mark = findMark(*file, file->activeMarkId);
        mark && !mark->deleted && activeSegment(*mark)) {
      calculationTime = {
          mark->range.time.begin,
          std::min(availableSamples(file->metadata), mark->range.time.end)};
      if (calculationTime.end < calculationTime.begin)
        calculationTime.end = calculationTime.begin;
    }
  }
  const int seed = file->metadata.demoSeed;
  const auto normalDense =
      file->metadata.demo
          ? std::clamp(static_cast<int>(std::lround(plot.width() * 12)), 4096,
                       65536)
          : std::clamp(static_cast<int>(std::lround(plot.width() * 2)), 512,
                       8192);
  const auto visibleSamples = view.time.end - view.time.begin;
  const auto pointDisplayLimit = static_cast<SampleIndex>(
      std::max(1.0, std::floor(plot.width() * devicePixelRatioF())));
  const bool exactSampleView =
      !psd && !file->metadata.demo && visibleSamples <= pointDisplayLimit;
  const auto& cursorState = session_.linkedCursor(file->metadata.id);
  const auto* pinnedFrame =
      psd && cursorState.framePsd
          ? session_.selectedSpectralFrame(file->metadata.id)
          : nullptr;
  const int dense =
      psd ? (pinnedFrame ? static_cast<int>(pinnedFrame->linearPower.size())
                         : file->display.psdSize)
      : exactSampleView ? static_cast<int>(visibleSamples)
                        : normalDense;
  const auto filePath =
      QString::fromUtf8(file->metadata.path.data(),
                        static_cast<qsizetype>(file->metadata.path.size()));
  const QString sourceKey =
      QString::fromStdString(file->metadata.id) +
      QString("/%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11/%12/%13")
          .arg(seed)
          .arg(psd)
          .arg(psd ? calculationTime.begin : view.time.begin)
          .arg(psd ? calculationTime.end : view.time.end)
          .arg(psd ? view.frequency.lowerHz : 0, 0, 'g', 17)
          .arg(psd ? view.frequency.upperHz : 0, 0, 'g', 17)
          .arg(file->metadata.sampleRateHz, 0, 'g', 17)
          .arg(duration, 0, 'g', 17)
          .arg(dense)
          .arg(file->metadata.centerFrequencyHz, 0, 'g', 17)
          .arg(filePath)
          .arg(psd ? file->display.psdSize : 0)
          .arg(file->display.psdFromSelection) +
      QString("/%1/frame%2/%3/%4")
          .arg(static_cast<int>(file->display.waveformMode))
          .arg(cursorState.framePsd)
          .arg(pinnedFrame ? pinnedFrame->id : 0)
          .arg(pinnedFrame ? reinterpret_cast<quintptr>(pinnedFrame) : 0) +
      "/" + sourceFingerprint(file->metadata) + "/project" +
      QString::number(session_.projectGeneration()) +
      (psd ? QString::fromStdString(
                 spectralParameterKey(file->display.psd.parameters)) +
                 QString::number(static_cast<int>(file->display.psd.statistic))
           : QString{});
  if (curveSourceKey_ != sourceKey) {
    if (curveWorker_) curveWorker_->cancel(++curveRequestGeneration_);
    curveSourceKey_ = sourceKey;
    ++curveGenerations_;
    ++renderGeneration_;
    curveSource_.clear();
    curveTrace_.clear();
    curveSegments_.clear();
    curvePath_ = {};
    curvePathKey_.clear();
    curveReductionKey_.clear();
    curvePending_ = false;
    curveCompleted_ = false;
    curveError_.clear();
    if (psd && cursorState.framePsd) {
      ++curveRequestGeneration_;
      curveCompleted_ = true;
      const auto spectrum = session_.spectrogram(file->metadata.id);
      if (pinnedFrame && spectrum && spectrum->sourceView == view.time &&
          spectrum->plan.frequencies ==
              FrequencyRange{view.frequency.lowerHz -
                                 analysisFrequencyOffset(file->metadata),
                             view.frequency.upperHz -
                                 analysisFrequencyOffset(file->metadata)})
        curveSource_ = spectrumDb(*pinnedFrame);
      else
        curveError_ = "驻留帧不在当前可见范围或正在更新";
    } else if (!file->metadata.demo) {
      if (psd) {
        const auto plan = makeSpectralAnalysisPlan(
            file->metadata.sampleRateHz,
            {view.frequency.lowerHz - analysisFrequencyOffset(file->metadata),
             view.frequency.upperHz - analysisFrequencyOffset(file->metadata)},
            file->display.psdSize, file->display.psd.parameters);
        if (!plan.valid) {
          curveError_ = QString::fromStdString(plan.reason);
          curveCompleted_ = true;
          return;
        }
      }
      CurveWorker::Request request;
      request.key = sourceKey;
      request.metadata = file->metadata;
      request.time = calculationTime;
      request.frequency = view.frequency;
      request.psd = psd;
      request.fftSize = file->display.psdSize;
      request.settings = file->display.psd;
      request.points = dense;
      request.waveformMode = file->display.waveformMode;
      request.generation = ++curveRequestGeneration_;
      curvePending_ = true;
      curveWorker_->submit(std::move(request));
      return;
    }
    if (!(psd && cursorState.framePsd)) {
      curveSource_.resize(dense);
      for (int i = 0; i < dense; ++i) {
        const double u = static_cast<double>(i) /
                         std::max(1, psd ? dense : dense - 1),
                     time = timeStart + u * timeSpan;
        const double frequency = view.frequency.lowerHz + u * frequencySpan;
        double value;
        if (psd) {
          const auto peak = [&](double center, double spread, double height) {
            return height *
                   std::exp(-std::pow((frequency - center) / spread, 2));
          };
          value = -88 +
                  peak(file->metadata.centerFrequencyHz -
                           file->metadata.sampleRateHz * .18,
                       file->metadata.sampleRateHz * .017, 45) +
                  peak(file->metadata.centerFrequencyHz +
                           file->metadata.sampleRateHz * .07,
                       file->metadata.sampleRateHz * .03, 62) +
                  peak(file->metadata.centerFrequencyHz +
                           file->metadata.sampleRateHz * .29,
                       file->metadata.sampleRateHz * .012, 34) +
                  (randomValue(i * .7 + seed) - .5) * 6;
        } else {
          const auto peak = [&](double center, double spread) {
            return std::exp(-std::pow((time - center) / spread, 2));
          };
          const double envelope = 3 +
                                  36 * peak(duration * .47, duration * .024) +
                                  22 * peak(duration * .65, duration * .04) +
                                  15 * peak(duration * .21, duration * .028);
          const double iComponent = std::sin(time * (30 + seed * 5)) * envelope;
          const double q = std::sin(time * 49 + seed) * 2;
          value = file->display.waveformMode == WaveformMode::Envelope
                      ? std::hypot(iComponent, q)
                      : iComponent - q;
        }
        curveSource_[i] = static_cast<float>(value);
      }
      if (psd) {
        auto average = std::make_shared<SpectralFrame>();
        average->linearPower.reserve(curveSource_.size());
        for (float value : curveSource_)
          average->linearPower.push_back(
              static_cast<float>(std::pow(10.0, value / 10.0)));
        averagePower_ = std::move(average);
      }
    }
    curveReductionKey_.clear();
  } else {
    ++curveCacheHits_;
    if (!file->metadata.demo && (curvePending_ || !curveCompleted_)) return;
  }
  curveShowsSamplePoints_ =
      exactSampleView && !psd && curveSource_.size() == visibleSamples;
  if (!psd && file->display.waveformAutoFit && !curveSource_.empty()) {
    const auto [minimum, maximum] =
        std::minmax_element(curveSource_.begin(), curveSource_.end());
    if (minimum != curveSource_.end() && std::isfinite(*minimum) &&
        std::isfinite(*maximum)) {
      const double center = (static_cast<double>(*minimum) + *maximum) * .5;
      const double dataSpan = static_cast<double>(*maximum) - *minimum;
      const double displaySpan = std::max(2.0, dataSpan / .75);
      file->display.waveformMin = file->display.auxiliaryMin =
          center - displaySpan * .5;
      file->display.waveformMax = file->display.auxiliaryMax =
          center + displaySpan * .5;
    }
  }
  const auto columns = static_cast<std::size_t>(std::max(
      1.0,
      std::floor(plot.width() * devicePixelRatioF() * (preview_ ? .5 : 1))));
  const auto reductionKey = sourceKey + "/" + QString::number(columns);
  if (curveReductionKey_ != reductionKey) {
    curveReductionKey_ = reductionKey;
    ++curveReductions_;
    ++renderGeneration_;
    if (curveShowsSamplePoints_) {
      curveTrace_.clear();
      curveTrace_.reserve(curveSource_.size());
      for (std::size_t index = 0; index < curveSource_.size(); ++index)
        curveTrace_.push_back({index, curveSource_[index]});
    } else
      curveTrace_ = display::extremaEnvelope(curveSource_, columns);
    curvePathKey_.clear();
  }
  const auto pathKey =
      reductionKey + QString("/%1/%2/%3/%4/%5/%6")
                         .arg(plot.width())
                         .arg(plot.height())
                         .arg(file->display.auxiliaryMin, 0, 'g', 17)
                         .arg(file->display.auxiliaryMax, 0, 'g', 17)
                         .arg(plot.left())
                         .arg(plot.top());
  if (curvePathKey_ != pathKey) {
    curvePathKey_ = pathKey;
    curvePath_ = {};
    curveSegments_.clear();
    curveSegments_.reserve(curveTrace_.empty() ? 0 : curveTrace_.size() - 1);
    QPointF previous;
    for (const auto& point : curveTrace_) {
      const double u =
          file->metadata.demo && !psd
              ? static_cast<double>(point.index) /
                    std::max<std::size_t>(1, curveSource_.size() - 1)
              : (point.index + (!psd && !curveShowsSamplePoints_ ? .5 : 0)) /
                    std::max<std::size_t>(1, curveSource_.size());
      const QPointF pixel(
          plot.left() + u * plot.width(),
          plot.top() +
              (file->display.auxiliaryMax - point.value) /
                  (file->display.auxiliaryMax - file->display.auxiliaryMin) *
                  plot.height());
      if (curvePath_.elementCount() == 0)
        curvePath_.moveTo(pixel);
      else {
        curvePath_.lineTo(pixel);
        curveSegments_.emplace_back(previous, pixel);
      }
      previous = pixel;
    }
    committedGeneration_ = renderGeneration_;
    acceptedPreview_ = preview_;
  }
}

void PlotWidget::acceptCurve(std::shared_ptr<CurvePayload> result) {
  if (!result || result->generation != curveRequestGeneration_ ||
      result->key != curveSourceKey_)
    return;
  curvePending_ = false;
  curveCompleted_ = true;
  curveError_ = result->error;
  curveSource_ = std::move(result->samples);
  averagePower_ = std::move(result->average);
  curveReductionKey_.clear();
  curvePathKey_.clear();
  ++curveGenerations_;
  ++renderGeneration_;
  repaintChart();
  emit stateChanged();
}

void PlotWidget::acceptNavigationCurve(std::shared_ptr<CurvePayload> result) {
  if (!result || result->generation != navigationRequestGeneration_ ||
      result->key != navigationCurveKey_)
    return;
  navigationCurvePending_ = false;
  navigationCurveCompleted_ = true;
  navigationCurveError_ = result->error;
  navigationCurve_ = std::move(result->samples);
  navigationKey_.clear();
  ++curveGenerations_;
  ++renderGeneration_;
  repaintChart();
}

void PlotWidget::pinAt(QPointF point) {
  const auto* file = session_.activeFile();
  if (!file || kind_ == Kind::Navigation) return;
  const auto& previous = session_.linkedCursor(file->metadata.id);
  SampleIndex sample =
      previous.pinned ? previous.sourceSample
                      : file->view.time.begin +
                            (file->view.time.end - file->view.time.begin) / 2;
  double frequency =
      previous.pinned
          ? previous.frequencyHz
          : (file->view.frequency.lowerHz + file->view.frequency.upperHz) / 2;
  const auto plot = plotRect();
  if (kind_ == Kind::Main) {
    const auto coordinate = fromPixel(point, file->view);
    sample = offsetSample(file->view.time.begin, coordinate.sample,
                          file->view.time.end - 1);
    frequency = coordinate.frequency;
  } else {
    const double u =
        std::clamp((point.x() - plot.left()) / plot.width(), 0.0, 1.0);
    if (file->display.auxiliaryMode == AuxiliaryMode::Psd)
      frequency =
          file->view.frequency.lowerHz +
          u * (file->view.frequency.upperHz - file->view.frequency.lowerHz);
    else
      sample = offsetSample(file->view.time.begin,
                            u * static_cast<long double>(file->view.time.end -
                                                         file->view.time.begin),
                            file->view.time.end - 1);
  }
  std::uint64_t frameId = 0;
  if (kind_ == Kind::Main) {
    const auto clickSpectrum = session_.spectrogram(file->metadata.id);
    const auto fraction = file->display.mainMode == MainMode::Waterfall
                              ? (point.y() - plot.top()) / plot.height()
                              : (point.x() - plot.left()) / plot.width();
    if (clickSpectrum)
      if (const auto* frame = clickSpectrum->frameAtFraction(fraction))
        frameId = frame->id;
  }
  session_.pinCursor(file->metadata.id, sample, frequency,
                     kind_ == Kind::Main ||
                         file->display.auxiliaryMode == AuxiliaryMode::Waveform,
                     frameId);
  emit stateChanged();
}
void PlotWidget::expandAnalysisTime() {
  const auto* file = session_.activeFile();
  if (!file) return;
  const auto frequencies = FrequencyRange{
      file->view.frequency.lowerHz - analysisFrequencyOffset(file->metadata),
      file->view.frequency.upperHz - analysisFrequencyOffset(file->metadata)};
  const auto stft = makeSpectralAnalysisPlan(
      file->metadata.sampleRateHz, frequencies, file->display.stftSize);
  const auto psd = makeSpectralAnalysisPlan(file->metadata.sampleRateHz,
                                            frequencies, file->display.psdSize);
  if (!stft.valid || !psd.valid) return;
  const auto count = std::max(stft.inputSamples, psd.inputSamples);
  if (count > availableSamples(file->metadata)) {
    emit statusMessage("文件总时长不足以满足当前分析点数");
    return;
  }
  auto view = file->view;
  const auto& cursor = session_.linkedCursor(file->metadata.id);
  const auto center =
      cursor.pinned ? cursor.sourceSample
                    : view.time.begin + (view.time.end - view.time.begin) / 2;
  const auto span = std::max(count, view.time.end - view.time.begin);
  view.time.begin = std::min(center > span / 2 ? center - span / 2 : 0,
                             availableSamples(file->metadata) - span);
  view.time.end = view.time.begin + span;
  session_.setView(view);
  emit stateChanged();
}
void PlotWidget::drawCursors(QPainter& painter, bool inverseMask) {
  if (!inverseMask) {
    cursor_overlay::paintInverseOverlay(
        painter, rect(), [this](QPainter& mask) { drawCursors(mask, true); });
    return;
  }
  const auto* file = session_.activeFile();
  if (!file || kind_ == Kind::Navigation) return;
  const auto plot = plotRect();
  const auto& view = file->view;
  const auto& pinned = session_.linkedCursor(file->metadata.id);
  const bool psd = kind_ == Kind::Auxiliary &&
                   file->display.auxiliaryMode == AuxiliaryMode::Psd;
  setProperty("cursorPinned", pinned.pinned);
  setProperty("pinnedSample",
              QVariant::fromValue<qulonglong>(pinned.sourceSample));
  setProperty("pinnedFrequencyHz", pinned.frequencyHz);
  setProperty("framePsd", pinned.framePsd);
  cursor_overlay::Layout pinLayout;
  const auto render = [&](QPointF position, bool isPinned) {
    SampleIndex sample = view.time.begin;
    double frequency = 0;
    QString text;
    const double u =
        std::clamp((position.x() - plot.left()) / plot.width(), 0.0, 1.0);
    if (kind_ == Kind::Main) {
      const auto coordinate = fromPixel(position, view);
      sample = isPinned ? pinned.sourceSample
                        : offsetSample(view.time.begin, coordinate.sample,
                                       view.time.end - 1);
      frequency = isPinned ? pinned.frequencyHz : coordinate.frequency;
      const double displayFrequency =
          frequency - (file->display.absoluteFrequency
                           ? 0
                           : analysisFrequencyOffset(file->metadata));
      text = "t = " +
             coordinateText(false, seconds(sample, file->metadata.sampleRateHz),
                            seconds(view.time.end - view.time.begin,
                                    file->metadata.sampleRateHz),
                            1 / file->metadata.sampleRateHz) +
             "\nf = " +
             coordinateText(true, displayFrequency,
                            view.frequency.upperHz - view.frequency.lowerHz);
      const auto spectrum = session_.spectrogram(file->metadata.id);
      const auto* frame =
          spectrum && spectrum->sourceView == view.time &&
                  spectrum->plan.frequencies ==
                      FrequencyRange{
                          view.frequency.lowerHz -
                              analysisFrequencyOffset(file->metadata),
                          view.frequency.upperHz -
                              analysisFrequencyOffset(file->metadata)}
              ? (isPinned
                     ? session_.selectedSpectralFrame(file->metadata.id)
                     : spectrum->frameAtFraction(
                           file->display.mainMode == MainMode::Waterfall
                               ? (position.y() - plot.top()) / plot.height()
                               : (position.x() - plot.left()) / plot.width()))
              : nullptr;
      if (frame) {
        const auto bin =
            frame->binAt(frequency - analysisFrequencyOffset(file->metadata));
        text +=
            QStringLiteral("\nP = %1 dBFS/Hz · bin %2\n帧中心 %3 s · Δf %4 Hz")
                .arg(frame->dbAt(bin), 0, 'f', 2)
                .arg(bin)
                .arg(seconds(frame->sourceCenter, file->metadata.sampleRateHz),
                     0, 'g', 10)
                .arg(frame->binHz, 0, 'g', 6);
        setProperty("cursorPowerDb", frame->dbAt(bin));
        setProperty("cursorFrameId",
                    QVariant::fromValue<qulonglong>(frame->id));
      } else
        text += "\n分析数据不可用 / 正在更新";
    } else if (psd) {
      frequency =
          isPinned ? pinned.frequencyHz
                   : view.frequency.lowerHz +
                         u * (view.frequency.upperHz - view.frequency.lowerHz);
      const double displayFrequency =
          frequency - (file->display.absoluteFrequency
                           ? 0
                           : analysisFrequencyOffset(file->metadata));
      text = "f = " +
             coordinateText(true, displayFrequency,
                            view.frequency.upperHz - view.frequency.lowerHz);
      if (!curveSource_.empty()) {
        const auto bin = std::min(
            curveSource_.size() - 1,
            static_cast<std::size_t>(std::llround(u * curveSource_.size())));
        text += QStringLiteral("\nP = %1 dBFS/Hz · bin %2\n%3 · N=%4")
                    .arg(curveSource_[bin], 0, 'f', 2)
                    .arg(bin)
                    .arg(pinned.framePsd ? "驻留帧谱" : "平均谱")
                    .arg(curveSource_.size());
        setProperty("cursorPowerDb", curveSource_[bin]);
      } else
        text += "\n分析数据不可用 / 正在更新";
    } else {
      sample = isPinned ? pinned.sourceSample
                        : offsetSample(view.time.begin,
                                       u * static_cast<long double>(
                                               view.time.end - view.time.begin),
                                       view.time.end - 1);
      text = "t = " +
             coordinateText(false, seconds(sample, file->metadata.sampleRateHz),
                            seconds(view.time.end - view.time.begin,
                                    file->metadata.sampleRateHz),
                            1 / file->metadata.sampleRateHz);
      if (!curveSource_.empty()) {
        const auto index = std::min(
            curveSource_.size() - 1,
            static_cast<std::size_t>(std::max(
                0LL, std::llround(u * curveSource_.size() -
                                  (curveShowsSamplePoints_ ? 0 : .5)))));
        text += QStringLiteral("\n%1 = %2 ADC 计数\n%3 · 源样本 #%4")
                    .arg(file->display.waveformMode == WaveformMode::I   ? "I"
                         : file->display.waveformMode == WaveformMode::Q ? "Q"
                         : file->display.waveformMode == WaveformMode::Envelope
                             ? "包络"
                             : "RMS")
                    .arg(curveSource_[index], 0, 'g', 8)
                    .arg(curveShowsSamplePoints_ ? "逐样本" : "显示分箱")
                    .arg(sample);
      } else
        text += "\n波形数据不可用 / 正在更新";
    }
    text = sourceUnits(text, file->metadata);
    const auto readout = cursor_overlay::parse(
        text, kind_ == Kind::Main,
        kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall);
    const auto layout = cursor_overlay::draw(
        painter, plot, position, kind_ == Kind::Main, readout, isPinned,
        isPinned ? std::vector<QRectF>{} : pinLayout.rectangles(), inverseMask);
    const QString prefix = isPinned ? "pinned" : "cursor";
    setProperty((prefix + "Readout").toUtf8(), text);
    setProperty((prefix + "ReadoutRect").toUtf8(), layout.bounds());
    setProperty((prefix + "ReadoutDetails").toUtf8(), readout.details);
    setProperty((prefix + "XLabelRect").toUtf8(), layout.x);
    setProperty((prefix + "YLabelRect").toUtf8(), layout.y);
    setProperty((prefix + "ValueRect").toUtf8(), layout.value);
    return layout;
  };
  for (const QString prefix :
       {QStringLiteral("pinned"), QStringLiteral("cursor")}) {
    setProperty((prefix + "ReadoutDetails").toUtf8(), QString{});
    for (const auto* suffix : {"XLabelRect", "YLabelRect", "ValueRect"})
      setProperty((prefix + suffix).toUtf8(), QRectF{});
  }
  setProperty("pinnedReadout", QString{});
  setProperty("pinnedReadoutRect", QRectF{});
  if (pinned.pinned) {
    QPointF point;
    bool visible = false;
    if (kind_ == Kind::Main) {
      visible = pinned.sourceSample >= view.time.begin &&
                pinned.sourceSample < view.time.end &&
                pinned.frequencyHz >= view.frequency.lowerHz &&
                pinned.frequencyHz < view.frequency.upperHz;
      point = toPixel(pinned.sourceSample, pinned.frequencyHz);
    } else {
      const double u =
          psd ? (pinned.frequencyHz - view.frequency.lowerHz) /
                    (view.frequency.upperHz - view.frequency.lowerHz)
          : pinned.sourceSample >= view.time.begin
              ? double(pinned.sourceSample - view.time.begin) /
                    (view.time.end - view.time.begin)
              : -1;
      visible = u >= 0 && u < 1;
      point = QPointF(plot.left() + u * plot.width(), plot.center().y());
    }
    if (visible) pinLayout = render(point, true);
  }
  setProperty("hoverCursorVisible", hoverPosition_.has_value());
  if (hoverPosition_)
    render(*hoverPosition_, false);
  else {
    setProperty("cursorReadout", QString{});
    setProperty("cursorReadoutRect", QRectF{});
  }
}

void PlotWidget::paintEvent(QPaintEvent*) {
  if (surface_ && !softwareFallback_) return;
  QPainter painter(this);
  painter.fillRect(rect(), QColor("#0a1728"));
  paintScene(painter, false);
}
void PlotWidget::paintScene(QPainter& painter, bool accelerated) {
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  auto* file = session_.activeFile();
  if (!file) {
    if (accelerated && surface_ && !softwareFallback_) {
      surface_->setHeatmap({}, {}, QStringLiteral("empty/no-active-file"));
      surface_->setChartGeometry(
          {}, {}, QStringLiteral("empty/no-active-file/geometry"));
    }
    return;
  }
  if (file->metadata.availability.status == LoadStatus::Loading ||
      availableSamples(file->metadata) == 0) {
    if (surface_ && !softwareFallback_) {
      surface_->setHeatmap({}, {}, "source-loading");
      surface_->setChartGeometry({}, {}, "source-loading");
    }
    painter.setPen(QColor("#aed3e8"));
    painter.setFont(canvasFont());
    if (kind_ == Kind::Navigation &&
        file->metadata.availability.status == LoadStatus::Loading) {
      const double fraction =
          static_cast<double>(file->metadata.availability.availableSamples) /
          std::max<SampleIndex>(1, file->metadata.sampleCount);
      painter.fillRect(rect(), QColor("#091b2b"));
      painter.fillRect(QRectF(0, 0, width() * fraction, height()),
                       QColor("#235c7c"));
      painter.drawText(
          rect(), Qt::AlignCenter,
          QString("读入 %1% · %2 字节")
              .arg(fraction * 100, 0, 'f', 1)
              .arg(file->metadata.availability.availableSamples * 4));
    } else
      painter.drawText(
          rect(), Qt::AlignCenter,
          file->metadata.availability.status == LoadStatus::Loading
              ? "等待读入完成或停止后分析"
              : QString::fromStdString(file->metadata.availability.error));
    return;
  }
  const auto plot = plotRect();
  const auto& view = file->view;
  const double duration =
      seconds(availableSamples(file->metadata), file->metadata.sampleRateHz);
  const bool waterfall =
      kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall;
  const bool psd = kind_ == Kind::Auxiliary &&
                   file->display.auxiliaryMode == AuxiliaryMode::Psd;
  const double timeStart =
      seconds(view.time.begin, file->metadata.sampleRateHz);
  const double timeSpan =
      seconds(view.time.end - view.time.begin, file->metadata.sampleRateHz);
  const double frequencySpan = view.frequency.upperHz - view.frequency.lowerHz;
  const int seed = file->metadata.demoSeed;
  if (kind_ == Kind::Navigation) {
    if (!accelerated) painter.fillRect(rect(), QColor("#091b2b"));
    const auto filePath =
        QString::fromUtf8(file->metadata.path.data(),
                          static_cast<qsizetype>(file->metadata.path.size()));
    const auto navigationKey = QString::fromStdString(file->metadata.id) +
                               QString("/%1/%2/%3/%4/%5/%6")
                                   .arg(seed)
                                   .arg(width())
                                   .arg(height())
                                   .arg(filePath)
                                   .arg(availableSamples(file->metadata))
                                   .arg(file->navigationEnvelope.size());
    if (navigationKey_ != navigationKey) {
      navigationKey_ = navigationKey;
      ++curveGenerations_;
      ++renderGeneration_;
      if (!file->metadata.demo && !file->navigationEnvelope.empty()) {
        navigationCurve_.clear();
        for (const auto& point : file->navigationEnvelope)
          navigationCurve_.push_back(point.peak);
        navigationCurveCompleted_ = true;
        navigationCurvePending_ = false;
      } else if (!file->metadata.demo &&
                 navigationCurveKey_ !=
                     filePath +
                         QString::number(availableSamples(file->metadata))) {
        navigationCurveKey_ =
            filePath + QString::number(availableSamples(file->metadata));
        navigationCurvePending_ = true;
        navigationCurveCompleted_ = false;
        CurveWorker::Request request;
        request.key = navigationCurveKey_;
        request.metadata = file->metadata;
        request.time = {0, availableSamples(file->metadata)};
        request.points = static_cast<int>(
            std::min<SampleIndex>(2048, availableSamples(file->metadata)));
        request.waveformMode = WaveformMode::Envelope;
        request.generation = ++navigationRequestGeneration_;
        navigationWorker_->submit(std::move(request));
      }
      navigationPaths_[0] = {};
      navigationPaths_[1] = {};
      navigationSegments_[0].clear();
      navigationSegments_[1].clear();
      const auto count = file->metadata.demo
                             ? std::min(2048, std::max(2, width() - 26))
                             : static_cast<int>(navigationCurve_.size());
      const auto maximum = count && !file->metadata.demo
                               ? *std::max_element(navigationCurve_.begin(),
                                                   navigationCurve_.end())
                               : 1.f;
      QPointF previous;
      for (int n = 0; n < count; ++n) {
        double u = (n + .5) / count;
        if (!file->metadata.demo && !file->navigationEnvelope.empty())
          u = static_cast<double>(file->navigationEnvelope[n].peakSample) /
              std::max<SampleIndex>(1, availableSamples(file->metadata));
        const double amplitude =
            file->metadata.demo
                ? .1 + .3 * std::exp(-std::pow((u - .25) * 6, 2)) +
                      .28 * std::exp(-std::pow((u - .58) * 9, 2))
                : navigationCurve_[n] / std::max(1.f, maximum);
        const QPointF point(plot.left() + u * plot.width(),
                            height() * .875 - amplitude * height() * .75);
        if (n == 0)
          navigationPaths_[0].moveTo(point);
        else {
          navigationPaths_[0].lineTo(point);
          navigationSegments_[0].emplace_back(previous, point);
        }
        previous = point;
      }
      committedGeneration_ = renderGeneration_;
    } else
      ++curveCacheHits_;
    if (accelerated && surface_ && !softwareFallback_) {
      std::vector<AcceleratedSurface::ChartVertex> vertices;
      std::vector<AcceleratedSurface::ChartDrawCall> draws;
      const QColor color("#4fb4e1");
      for (const auto& segments : navigationSegments_) {
        if (segments.empty()) continue;
        const auto first = static_cast<quint32>(vertices.size());
        const auto append = [&](QPointF point) {
          const float alpha = color.alphaF();
          vertices.push_back(
              {static_cast<float>(point.x() / std::max(1, width())),
               static_cast<float>(point.y() / std::max(1, height())),
               static_cast<float>(color.redF() * alpha),
               static_cast<float>(color.greenF() * alpha),
               static_cast<float>(color.blueF() * alpha), alpha});
        };
        append(segments.front().p1());
        for (const auto& segment : segments) append(segment.p2());
        draws.push_back({AcceleratedSurface::ChartDrawCall::Topology::LineStrip,
                         first, static_cast<quint32>(vertices.size() - first)});
      }
      surface_->setChartGeometry(
          std::move(vertices), std::move(draws),
          QStringLiteral("wide-navigation/") + navigationKey_, plot);
    } else {
      painter.setPen(QPen(QColor("#4fb4e1"), 1));
      for (const auto& segments : navigationSegments_)
        if (!segments.empty())
          painter.drawLines(segments.data(), static_cast<int>(segments.size()));
    }
    if (!file->metadata.demo && navigationCurvePending_) {
      painter.setFont(canvasFont());
      painter.setPen(QColor("#b5d9ec"));
      painter.drawText(QPointF(15, 15), "正在读取 IQ 导航预览…");
    } else if (!navigationCurveError_.isEmpty()) {
      painter.setFont(canvasFont());
      painter.setPen(QColor("#edb6a0"));
      painter.drawText(QPointF(15, 15),
                       "IQ 导航预览失败 · " + navigationCurveError_);
    }
    const auto window = navigationWindow();
    painter.fillRect(window, cssColor("#38b6e128"));
    painter.setPen(QColor("#8ed9ff"));
    painter.drawRect(QRectF(window.left() + .5, .5, window.width(),
                            std::max(0, height() - 1)));
    painter.setFont(canvasFont());
    painter.setPen(QColor("#a5bed4"));
    for (int i = 0; i <= 6; ++i)
      painter.drawText(QPointF(13 + i * plot.width() / 6 - 10, height() - 3),
                       coordinateText(false, i * duration / 6));
    return;
  }
  if (kind_ == Kind::Main && !accelerated) {
    updateHeatmap();
    const auto [target, source] = heatmapPlacement();
    if (!target.isEmpty())
      painter.drawImage(
          target, heatmap_,
          QRectF(source.x() * heatmap_.width(), source.y() * heatmap_.height(),
                 source.width() * heatmap_.width(),
                 source.height() * heatmap_.height()));
  }
  if (file->display.grid) {
    painter.setPen(QPen(
        kind_ == Kind::Main ? cssColor("#b9dfff20") : cssColor("#31466177"),
        1));
    const int divisions = kind_ == Kind::Main ? 8 : 9;
    for (int i = 0; i <= divisions; ++i) {
      const double x = plot.left() + plot.width() * i / divisions;
      painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    const int vertical = kind_ == Kind::Main ? 8 : 4;
    for (int i = 0; i <= vertical; ++i) {
      const double y = plot.top() + plot.height() * i / vertical;
      painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
  }
  if (kind_ == Kind::Auxiliary) {
    updateCurve();
    painter.save();
    painter.setClipRect(plot);
    // Adjacent endpoints keep the thin trace connected in either renderer.
    // The diagnostic path and the GPU vertex buffer use the same ordered data.
    const QColor waveformColor =
        file->display.waveformMode == WaveformMode::I   ? QColor("#51d7c5")
        : file->display.waveformMode == WaveformMode::Q ? QColor("#f1aa63")
                                                        : QColor("#51d7c5");
    if (accelerated && surface_ && !softwareFallback_) {
      std::vector<AcceleratedSurface::ChartVertex> vertices;
      std::vector<AcceleratedSurface::ChartDrawCall> draws;
      const QColor color = psd ? QColor("#5abffa") : waveformColor;
      const auto append = [&](QPointF point) {
        const float alpha = color.alphaF();
        vertices.push_back(
            {static_cast<float>(point.x() / std::max(1, width())),
             static_cast<float>(point.y() / std::max(1, height())),
             static_cast<float>(color.redF() * alpha),
             static_cast<float>(color.greenF() * alpha),
             static_cast<float>(color.blueF() * alpha), alpha});
      };
      if (!curveSegments_.empty()) {
        const auto first = static_cast<quint32>(vertices.size());
        append(curveSegments_.front().p1());
        for (const auto& segment : curveSegments_) append(segment.p2());
        draws.push_back({AcceleratedSurface::ChartDrawCall::Topology::LineStrip,
                         first, static_cast<quint32>(vertices.size() - first)});
      }
      if (!psd && curveShowsSamplePoints_ && !curveTrace_.empty()) {
        const auto first = static_cast<quint32>(vertices.size());
        constexpr int sectors = 8;
        for (const auto& point : curveTrace_) {
          const double x =
              plot.left() + plot.width() * point.index /
                                std::max<std::size_t>(1, curveSource_.size());
          const double y =
              plot.top() +
              (file->display.auxiliaryMax - point.value) /
                  (file->display.auxiliaryMax - file->display.auxiliaryMin) *
                  plot.height();
          const QPointF center(x, y);
          for (int sector = 0; sector < sectors; ++sector) {
            const double a0 = 2.0 * std::numbers::pi * sector / sectors;
            const double a1 = 2.0 * std::numbers::pi * (sector + 1) / sectors;
            append(center);
            append(center + QPointF(2.0 * std::cos(a0), 2.0 * std::sin(a0)));
            append(center + QPointF(2.0 * std::cos(a1), 2.0 * std::sin(a1)));
          }
        }
        draws.push_back({AcceleratedSurface::ChartDrawCall::Topology::Triangles,
                         first, static_cast<quint32>(vertices.size() - first)});
      }
      surface_->setChartGeometry(
          std::move(vertices), std::move(draws),
          QStringLiteral("wide-auxiliary/") + curvePathKey_, plot);
    } else {
      painter.setPen(QPen(psd ? QColor("#5abffa") : waveformColor, 1.5));
      if (!curveSegments_.empty())
        painter.drawLines(curveSegments_.data(),
                          static_cast<int>(curveSegments_.size()));
      if (!psd && curveShowsSamplePoints_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(waveformColor);
        for (const auto& segment : curveSegments_)
          painter.drawEllipse(segment.p1(), 2.0, 2.0);
        if (!curveSegments_.empty())
          painter.drawEllipse(curveSegments_.back().p2(), 2.0, 2.0);
      }
    }
    if (gesture_ && gesture_->tool == Tool::AuxiliaryZoom) {
      const double left =
          std::clamp(std::min(gesture_->start.x(), gesture_->current.x()),
                     plot.left(), plot.right());
      const double right =
          std::clamp(std::max(gesture_->start.x(), gesture_->current.x()),
                     plot.left(), plot.right());
      painter.setPen(QPen(QColor("#9ceaff"), 1, Qt::DashLine));
      painter.setBrush(cssColor("#7ed3f02a"));
      painter.drawRect(QRectF(left, plot.top(), right - left, plot.height()));
    }
    painter.restore();
  }
  painter.setFont(font());
  painter.setPen(QColor("#aac2d7"));
  const auto layout = axisLayout();
  const auto& axes = measuredAxes();
  const auto& x = axes.x;
  const auto& y = axes.y;
  const QFontMetricsF metrics(font(), this);
  double previousRight = -std::numeric_limits<double>::infinity();
  for (int i = 0; i <= x.count; ++i) {
    const auto& label = axes.labels.xTicks[i];
    const double width = axis_layout::textWidth(metrics, label),
                 position = plot.left() + plot.width() * i / x.count;
    const double left =
        std::clamp(position - width / 2, plot.left() - 3,
                   std::max(plot.left() - 3, plot.right() - width + 3));
    if (left >= previousRight + 8) {
      painter.drawText(QPointF(left - std::min(0.0, metrics.boundingRect(label).left()),
                                layout.xTicks.top() + metrics.ascent()), label);
      previousRight = left + width;
    }
  }
  double previousTop = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= 4; ++i) {
    const double position = plot.bottom() - plot.height() * i / 4;
    const auto& label = axes.labels.yTicks[i];
    const double top = position - metrics.height() / 2;
    if (top + metrics.height() <= previousTop - 3) {
      painter.drawText(
          QPointF(plot.left() - 7 -
                      std::max(metrics.horizontalAdvance(label),
                               metrics.boundingRect(label).right()),
                  top + metrics.ascent()),
          label);
      previousTop = top;
    }
  }
  if (axisTitleMode_ != AxisTitleMode::CompactHeader)
    painter.drawText(layout.xTitle, Qt::AlignHCenter | Qt::AlignTop,
                     axes.labels.xTitle);
  if (axisTitleMode_ == AxisTitleMode::Rotated) {
    painter.save();
    painter.translate(layout.rotatedTitle.left(), plot.bottom());
    painter.rotate(-90);
    painter.drawText(QRectF(0, 0, plot.height(), layout.rotatedTitle.width()),
                     Qt::AlignCenter | Qt::TextWordWrap, axes.labels.yTitle);
    painter.restore();
  } else if (axisTitleMode_ != AxisTitleMode::CompactHeader) {
    if (axisTitleMode_ == AxisTitleMode::Header)
      painter.fillRect(layout.header.adjusted(-4, -2, 4, 2), QColor("#102238"));
    painter.drawText(layout.header, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     axisTitleMode_ == AxisTitleMode::Header
                         ? axes.labels.yTitle : axes.labels.yUnit);
  }
  int originRow = 0;
  for (const auto& label : {x.originLabel,
                           kind_ == Kind::Main ? y.originLabel : QString{}})
    if (axisTitleMode_ != AxisTitleMode::CompactHeader && !label.isEmpty()) {
      const double top = plot.top() + 13 + originRow++ * 15;
      painter.fillRect(QRectF(plot.left() + 4, top - 10,
                              metrics.horizontalAdvance(label) + 10, 14),
                       cssColor("#071628da"));
      painter.setPen(QColor("#d4edff"));
      painter.drawText(QPointF(plot.left() + 9, top), label);
    }
  chart_feedback::drawAxisHighlight(painter, rect(), plot, hoverZone_);
  if (kind_ == Kind::Auxiliary) {
    const auto tip = tipText();
    painter.setFont(canvasFont());
    const QFontMetricsF fm(painter.font());
    const double width = std::min(static_cast<double>(this->width() - 24),
                                  std::ceil(fm.horizontalAdvance(tip)) + 12);
    const QRectF box(this->width() - 12 - width, plot.top() + 6,
                      width, fm.height() + 6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(cssColor("#091728c7"));
    painter.drawRoundedRect(box, 3, 3);
    painter.setPen(QColor("#c8dfef"));
    painter.drawText(
        box.adjusted(6, 0, -6, 0), Qt::AlignCenter,
        fm.elidedText(tip, Qt::ElideRight, static_cast<int>(width - 12)));
    if (curvePending_ || !curveError_.isEmpty()) {
      const QString status = !curveError_.isEmpty() ? "IQ 错误 · " + curveError_
                             : psd ? "正在计算真实 IQ PSD…"
                                   : "正在计算真实 IQ 波形…";
      const double statusWidth =
          std::min(static_cast<double>(this->width() - 24),
                   std::ceil(fm.horizontalAdvance(status)) + 12);
      const QRectF statusBox(plot.left() + 8, plot.top() + 8, statusWidth,
                             fm.height() + 6);
      painter.fillRect(statusBox, cssColor("#091728d8"));
      painter.setPen(QColor("#d5e7f5"));
      painter.drawText(statusBox.adjusted(6, 0, -6, 0), Qt::AlignCenter,
                       fm.elidedText(status, Qt::ElideRight,
                                     static_cast<int>(statusWidth - 12)));
    }
    if (!accelerated) drawCursors(painter);
    return;
  }
  if (file->display.colorScale) {
    const double left = width() - 38, top = plot.top() + 4,
                 span = std::max(1.0, plot.height() - 8);
    for (int i = 0; i < span; ++i)
      painter.fillRect(QRectF(left, top + i, 13, 1),
                       QColor::fromRgb(color(static_cast<float>(1 - i / span),
                                             file->display.palette)));
    painter.setPen(QColor("#8197b1"));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(left, top, 13, span));
    painter.setFont(canvasFont());
    painter.setPen(QColor("#bad5eb"));
    for (int i = 0; i <= 4; ++i)
      painter.drawText(
          QPointF(left + 17, top + i * span / 4 + 4),
          numberText(file->display.referenceLevelDb -
                         std::round(file->display.dynamicRangeDb * i / 4),
                     0));
  }
  painter.save();
  painter.setClipRect(plot);
  painter.setFont(canvasFont(11, true));
  const std::unordered_set<std::string> selectedIds(
      file->selectedMarkIds.begin(), file->selectedMarkIds.end());
  int handleCount = 0;
  for (const auto& mark : file->marks) {
    if (!annotationVisible(*file, mark)) continue;
    const bool selected = selectedIds.contains(mark.id);
    const bool current = file->activeMarkId == mark.id;
    const bool objectHovered =
        file->hoveredMarkId == mark.id && !creating_ && !detectionRoi_;
    const QColor sourceColor =
        mark.sourceKind == AnnotationSource::ImportedLabel ? QColor("#65d2bd")
        : mark.sourceKind == AnnotationSource::Manual      ? QColor("#edc56d")
                                                           : QColor("#51c4e9");
    for (std::size_t index = 0;
         index < std::max<std::size_t>(1, mark.segments.size()); ++index) {
      const auto* segment =
          mark.segments.empty() ? nullptr : &mark.segments[index];
      if (!segment && !mark.observation) continue;
      const bool hovered =
          objectHovered && (file->hoveredSegmentId.empty() ||
                            (segment && segment->id == file->hoveredSegmentId));
      const bool editing =
          gesture_ && gesture_->tool == Tool::MarkEdit &&
          gesture_->markId == mark.id &&
          (!segment || segment->id == gesture_->editContext.segmentId);
      const auto& range = editing   ? gesture_->draft
                          : segment ? segment->range
                                    : *mark.observation;
      const bool currentSegment =
          current && (!segment || segment->id == file->activeSegmentId);
      const auto box = markRect(range);
      if (box.isEmpty() || !box.intersects(plot)) continue;
      if (hovered) {
        painter.setBrush(Qt::NoBrush);
        for (const int thickness : {10, 6}) {
          painter.setPen(QPen(QColor(230, 251, 255, thickness == 10 ? 12 : 24),
                              thickness));
          painter.drawRect(box);
        }
      }
      auto fill = sourceColor;
      fill.setAlpha(currentSegment ? 61 : selected ? 46 : 23);
      painter.setBrush(fill);
      QPen sourcePen(sourceColor, currentSegment ? 3 : selected ? 2 : 1);
      if (!selected) sourcePen.setDashPattern({5, 3});
      painter.setPen(sourcePen);
      if (!segment) {
        painter.setBrush(QBrush(sourceColor, Qt::BDiagPattern));
        painter.drawRect(box);
        painter.drawText(box.adjusted(5, 5, -5, -5),
                         Qt::AlignTop | Qt::AlignLeft, "时间未定位 / 观察窗口");
      } else {
        painter.drawRect(box);
      }
      // Neutral outlines communicate state while the inner source stroke keeps
      // the three provenance colors visible in every selection state.
      painter.setBrush(Qt::NoBrush);
      if (currentSegment || (current && !segment)) {
        painter.setPen(QPen(QColor("#e1f8ff"), 1));
        painter.drawRect(box.adjusted(-3, -3, 3, 3));
      }
      if (hovered) {
        painter.setPen(QPen(QColor("#c4f7ff"), 1, Qt::DotLine));
        painter.drawRect(box.adjusted(2, 2, -2, -2));
        // The full name and gesture hint already go through interactionHint
        // into the existing readout/status area. Do not cover geometry or its
        // eight handles with a second floating label (especially long names).
      }
      if (currentSegment && !creating_ && !detectionRoi_ &&
          session_.annotations().canEdit(*file, mark.id))
        for (const auto& [point, edge] : handles(box)) {
          if (!segment && edge != (waterfall ? Left : Top) &&
              edge != (waterfall ? Right : Bottom))
            continue;
          if (!plot.contains(point)) continue;
          ++handleCount;
          const QRectF handle(point - QPointF(3, 3), QSizeF(6, 6));
          painter.fillRect(handle, QColor("#d9f8ff"));
          painter.setPen(QColor("#344653"));
          painter.setBrush(Qt::NoBrush);
          painter.drawRect(handle);
        }
    }
  }
  setProperty("annotationHandleCount", handleCount);
  setProperty("activeAnnotationId", QString::fromStdString(file->activeMarkId));
  setProperty("activeAnnotationSegmentId",
              QString::fromStdString(file->activeSegmentId));
  if (gesture_ &&
      (gesture_->tool == Tool::ZoomBox || gesture_->tool == Tool::CreateMark)) {
    const bool creatingMark = gesture_->tool == Tool::CreateMark;
    const auto box = QRectF(gesture_->start, gesture_->current)
                         .normalized()
                         .intersected(plot);
    QPen pen(QColor(creatingMark ? "#f4cc70" : "#9ceaff"), 1);
    pen.setDashPattern({4, 3});
    painter.setPen(pen);
    painter.setBrush(creatingMark ? cssColor("#f0ca5c27")
                                  : cssColor("#7ed3f026"));
    painter.drawRect(box);
  }
  if (detectionRoi_ && detectionRoiContextMatches()) {
    auto range = detectionRoi_->preview;
    if (gesture_ && gesture_->tool == Tool::DetectionRoi)
      range = detectionRoiRange(*gesture_);
    if (range) {
      const auto box = markRect(*range).intersected(plot);
      painter.setBrush(cssColor("#e3bf7025"));
      painter.setPen(QPen(QColor("#111727"), 5));
      painter.drawRect(box);
      QPen pen(QColor("#e3bf70"), 2);
      pen.setDashPattern({5, 3});
      painter.setPen(pen);
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(box);
    }
    if (!detectionRoi_->preview) {
      const QString text = "检测 ROI · 拖动选择 · Esc 恢复前一工具";
      const QFontMetricsF roiMetrics(painter.font());
      const double labelWidth =
          std::min(std::ceil(roiMetrics.horizontalAdvance(text)) + 20,
                   plot.width() - 16);
      const QRectF label(plot.left() + 8, plot.top() + 8,
                         std::max(1.0, labelWidth), 25);
      painter.setBrush(QColor("#2a3029"));
      painter.setPen(QPen(QColor("#e3bf70"), 1));
      painter.drawRoundedRect(label, 3, 3);
      painter.setPen(QColor("#f1d590"));
      painter.drawText(label.adjusted(7, 0, -7, 0), Qt::AlignVCenter,
                       roiMetrics.elidedText(text, Qt::ElideRight,
                                             std::max(1.0, labelWidth - 14)));
    }
  }
  if (kind_ == Kind::Main &&
      file->annotationLayers[static_cast<int>(AnnotationSource::Algorithm)] &&
      !detectionPreview_.empty()) {
    painter.setFont(canvasFont(11, true));
    for (const auto& preview : detectionPreview_) {
      if (preview.time.end <= preview.time.begin) continue;
      const auto box = markRect({preview.time, preview.frequency});
      if (box.isEmpty() || !box.intersects(plot)) continue;
      QColor color("#c9a6ff");
      color.setAlpha(36);
      painter.setBrush(QBrush(color, Qt::BDiagPattern));
      QPen pen(QColor("#c9a6ff"), 1, Qt::DashLine);
      painter.setPen(pen);
      painter.drawRect(box);
      painter.setPen(QColor("#f3e9ff"));
      painter.drawText(box.adjusted(4, 3, -4, -3), Qt::AlignTop | Qt::AlignLeft,
                       preview.timeLocalized ? "预览" : "预览 · 时间未定位");
    }
    if (detectionPreviewDropped_) {
      const QString dropped =
          QString("预览队列已丢弃 %1 条最旧事件").arg(detectionPreviewDropped_);
      painter.setPen(QColor("#f3e9ff"));
      painter.drawText(plot.adjusted(8, 8, -8, -8), Qt::AlignBottom | Qt::AlignLeft,
                       dropped);
    }
  }
  painter.restore();
  if (!accelerated) drawCursors(painter);

  if (creating_)
    drawPill(painter, rect(),
             "持续选择信号 · 拖动创建标记 · 右键菜单关闭 / Esc 退出", true);
  if ((!renderedPower_ || renderedPower_->key != requestedPowerKey_) ||
      (renderedPower_ && !renderedPower_->error.isEmpty())) {
    const QString text = renderedPower_ && !renderedPower_->error.isEmpty()
                             ? "IQ 图谱错误 · " + renderedPower_->error
                             : "正在更新图谱…";
    painter.setFont(canvasFont(10, true));
    const QFontMetricsF fm(painter.font());
    const double labelWidth = std::ceil(fm.horizontalAdvance(text)) + 16;
    const QRectF label(plot.right() - labelWidth - 6, plot.bottom() - 25,
                       labelWidth, 19);
    painter.setPen(Qt::NoPen);
    painter.setBrush(cssColor("#091728dd"));
    painter.drawRoundedRect(label, 3, 3);
    painter.setPen(QColor("#b5d9ec"));
    painter.drawText(label, Qt::AlignCenter, text);
  }
}

std::vector<PlotWidget::Hit> PlotWidget::markChoicesAt(QPointF point) const {
  std::vector<Hit> hits;
  const auto* file = session_.activeFile();
  if (!file || creating_ || detectionRoi_ || kind_ != Kind::Main ||
      zoneAt(point) != Zone::Plot)
    return hits;
  for (auto it = file->marks.rbegin(); it != file->marks.rend(); ++it) {
    if (!annotationVisible(*file, *it)) continue;
    for (const auto& s : it->segments)
      if (markRect(s.range).intersected(plotRect()).contains(point))
        hits.push_back({it->id, 0, false, s.id});
    if (it->segments.empty() && it->observation &&
        markRect(*it->observation).intersected(plotRect()).contains(point))
      hits.push_back({it->id, 0, false, {}});
  }
  return hits;
}
std::vector<std::string> PlotWidget::marksAt(QPointF point) const {
  std::vector<std::string> ids;
  for (const auto& hit : markChoicesAt(point))
    if (std::find(ids.begin(), ids.end(), hit.id) == ids.end())
      ids.push_back(hit.id);
  return ids;
}
void PlotWidget::addMarkChoices(QMenu* menu, const std::vector<Hit>& hits,
                                Qt::KeyboardModifiers modifiers) {
  const auto* file = session_.activeFile();
  if (!file) return;
  const auto generation = session_.projectGeneration();
  const auto fileId = file->metadata.id;
  const QStringList sources{"原始标签", "手动", "算法"};
  for (const auto& hit : hits) {
    const auto* annotation = findAnnotation(*file, hit.id);
    if (!annotation) continue;
    const auto segment = std::find_if(
        annotation->segments.begin(), annotation->segments.end(),
        [&](const auto& item) { return item.id == hit.segmentId; });
    const auto title =
        QString::fromStdString(annotation->name) + " · " +
        sources.value(static_cast<int>(annotation->sourceKind)) +
        (segment == annotation->segments.end()
             ? " · 时间未定位"
             : QString(" · 段 %1")
                   .arg(segment - annotation->segments.begin() + 1));
    auto* choice = menu->addAction(title);
    choice->setData(QString::fromStdString(hit.id));
    choice->setProperty("segmentId", QString::fromStdString(hit.segmentId));
    connect(choice, &QAction::triggered, this,
            [this, hit, modifiers, generation, fileId] {
              const auto* current = session_.activeFile();
              if (!current || current->metadata.id != fileId ||
                  session_.projectGeneration() != generation)
                return;
              emit markSegmentSelectionRequested(
                  QString::fromStdString(hit.id),
                  QString::fromStdString(hit.segmentId), modifiers);
            });
  }
}
void PlotWidget::selectAt(QPointF point, Qt::KeyboardModifiers modifiers) {
  const auto hits = markChoicesAt(point);
  if (hits.size() > 1) {
    auto* menu = new QMenu(this);
    menu->setObjectName("overlappingAnnotationsMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    addMenuHeading(*menu, "选择重叠标记 / 活动段");
    addMarkChoices(menu, hits, modifiers);
    menu->popup(mapToGlobal(point.toPoint()));
  } else if (!hits.empty()) {
    emit markSegmentSelectionRequested(
        QString::fromStdString(hits.front().id),
        QString::fromStdString(hits.front().segmentId), modifiers);
  } else if (!(modifiers & (Qt::ControlModifier | Qt::ShiftModifier))) {
    emit markSelectionRequested({}, modifiers);
  }
}
PlotWidget::Hit PlotWidget::hitMark(QPointF point) const {
  const auto* file = session_.activeFile();
  const auto plot = plotRect();
  if (!file || creating_ || detectionRoi_ || kind_ != Kind::Main ||
      zoneAt(point) != Zone::Plot)
    return {};
  std::vector<const Mark*> selected;
  if (const auto* current = findMark(*file, file->activeMarkId);
      current && (activeSegment(*current) || current->observation))
    selected.push_back(current);
  for (const auto* mark : selected) {
    if (!annotationVisible(*file, *mark) ||
        !session_.annotations().canEdit(*file, mark->id))
      continue;
    const bool observation = mark->segments.empty();
    const bool waterfall = file->display.mainMode == MainMode::Waterfall;
    const auto box = markRect(observation ? *mark->observation : mark->range);
    const auto clip = box.intersected(plot);
    if (clip.isEmpty()) continue;
    int bestEdge = 0;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const auto& [handle, edge] : handles(box))
      if (!observation || edge == (waterfall ? Left : Top) ||
          edge == (waterfall ? Right : Bottom))
        if (plot.contains(handle) && std::abs(point.x() - handle.x()) <= 6 &&
            std::abs(point.y() - handle.y()) <= 6) {
          const double distance = std::pow(point.x() - handle.x(), 2) +
                                  std::pow(point.y() - handle.y(), 2);
          const bool corner =
              (edge & (Left | Right)) && (edge & (Top | Bottom));
          if (distance < bestDistance || (distance == bestDistance && corner)) {
            bestEdge = edge;
            bestDistance = distance;
          }
        }
    if (bestEdge) return {mark->id, bestEdge, true, mark->activeSegmentId};
    const auto consider = [&](int edge, double coordinate, double actual,
                              bool visible, bool inSpan) {
      const double distance = std::abs(coordinate - actual);
      if (observation && edge != (waterfall ? Left : Top) &&
          edge != (waterfall ? Right : Bottom))
        return;
      if (visible && inSpan && distance <= 6 && distance < bestDistance) {
        bestDistance = distance;
        bestEdge = edge;
      }
    };
    consider(Left, point.x(), box.left(),
             box.left() >= plot.left() && box.left() <= plot.right(),
             point.y() >= clip.top() && point.y() <= clip.bottom());
    consider(Right, point.x(), box.right(),
             box.right() >= plot.left() && box.right() <= plot.right(),
             point.y() >= clip.top() && point.y() <= clip.bottom());
    consider(Top, point.y(), box.top(),
             box.top() >= plot.top() && box.top() <= plot.bottom(),
             point.x() >= clip.left() && point.x() <= clip.right());
    consider(Bottom, point.y(), box.bottom(),
             box.bottom() >= plot.top() && box.bottom() <= plot.bottom(),
             point.x() >= clip.left() && point.x() <= clip.right());
    if (bestEdge) return {mark->id, bestEdge, true, mark->activeSegmentId};
    if (clip.contains(point))
      return {mark->id, Move, true, mark->activeSegmentId};
  }
  // Other selected objects retain their existing body-drag gesture. Their
  // resize handles are displayed only after the drag makes them current.
  const std::unordered_set<std::string> selectedIds(
      file->selectedMarkIds.begin(), file->selectedMarkIds.end());
  for (auto it = file->marks.rbegin(); it != file->marks.rend(); ++it) {
    if (!selectedIds.contains(it->id) || it->id == file->activeMarkId ||
        !annotationVisible(*file, *it))
      continue;
    if ((activeSegment(*it) || it->observation) &&
        markRect(activeSegment(*it) ? it->range : *it->observation)
            .intersected(plot)
            .contains(point) &&
        session_.annotations().canEdit(*file, it->id))
      return {it->id, Move, true, it->activeSegmentId};
  }
  const auto hits = markChoicesAt(point);
  return hits.empty() ? Hit{} : hits.front();
}
void PlotWidget::updateHover(QPointF point) {
  const auto* file = session_.activeFile();
  if (!file) {
    setCursor(Qt::ArrowCursor);
    return;
  }
  hoverZone_ = zoneAt(point);
  setProperty("interactionZone", static_cast<int>(hoverZone_));
  if (kind_ == Kind::Navigation) {
    setCursor(gesture_ && gesture_->navigationInside && gesture_->changed
                  ? Qt::ClosedHandCursor
              : navigationWindow().contains(point) ? Qt::OpenHandCursor
                                                   : Qt::PointingHandCursor);
    emit interactionHint("全局导航：滚轮缩放 · 拖动范围平移 · 单击跳转");
    return;
  }
  const auto hit = gesture_ ? Hit{} : hitMark(point);
  const auto underPointer =
      gesture_ ? std::vector<Hit>{} : markChoicesAt(point);
  setHoveredMark(underPointer.empty()
                     ? QString{}
                     : QString::fromStdString(underPointer.front().id),
                 underPointer.empty()
                     ? QString{}
                     : QString::fromStdString(underPointer.front().segmentId));
  if (gesture_)
    setCursor(gesture_->tool == Tool::MarkEdit
                  ? gesture_->edges == Move ? Qt::ClosedHandCursor
                                            : edgeCursor(gesture_->edges)
              : gesture_->tool == Tool::PanTime ||
                      gesture_->tool == Tool::PanFrequency ||
                      gesture_->tool == Tool::AuxiliaryY
                  ? Qt::ClosedHandCursor
                  : Qt::CrossCursor);
  else if ((creating_ || detectionRoi_) && kind_ == Kind::Main)
    setCursor(hoverZone_ == Zone::Plot ? Qt::CrossCursor : Qt::ArrowCursor);
  else if (hit.selected)
    setCursor(edgeCursor(hit.edges));
  else
    setCursor(!hit.id.empty() ? Qt::PointingHandCursor
                              : chart_feedback::cursor(hoverZone_));
  QString hint;
  if (detectionRoi_)
    hint = detectionRoi_->preview
               ? "检测 ROI 待确认：Enter 确认 · Esc 取消并恢复前一工具"
               : "检测 ROI：拖动预览 · Enter 确认 · Esc 恢复前一工具";
  else if (creating_ && kind_ == Kind::Main)
    hint = "持续选择信号：拖动创建 · 右键菜单关闭 · Esc 退出";
  else if (const auto* mark = findMark(*file, hit.id))
    hint = QString::fromStdString(mark->name) +
           (hit.selected ? "：拖动移动 · 边线和顶点调整"
                         : "：单击选中 · 拖动空白区域放大");
  else if (kind_ == Kind::Main) {
    const bool waterfall = file->display.mainMode == MainMode::Waterfall;
    const auto title = QString(waterfall ? "瀑布图" : "时频图");
    hint = hoverZone_ == Zone::YAxis
               ? title + " Y 轴：滚轮缩放" + (waterfall ? "时间" : "频率") +
                     " · 按住拖动平移"
           : hoverZone_ == Zone::XAxis
               ? title + " X 轴：滚轮缩放" + (waterfall ? "频率" : "时间") +
                     " · 按住拖动平移"
           : hoverZone_ == Zone::Plot
               ? title + "：滚轮缩放" + (waterfall ? "频率" : "时间") +
                     " · 拖动矩形放大"
               : title + "：请将鼠标移入图内或坐标轴区域";
  } else
    hint = hoverZone_ == Zone::YAxis ? "辅助图谱 Y 轴：滚轮缩放幅度 · 拖动平移"
           : hoverZone_ == Zone::XAxis ? "辅助图谱 X 轴：滚轮缩放 · 拖动平移"
           : hoverZone_ == Zone::Plot ? "辅助图谱：滚轮缩放 X · 拖动范围放大"
                                      : "辅助图谱：移至图谱或坐标轴进行交互";
  emit interactionHint(hint);
}

void PlotWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::RightButton) {
    if (activeGesture && activeGesture != this &&
        activeGesture->window() == window())
      activeGesture->cancelGesture();
    cancelGesture();
    event->accept();
    return;
  }
  auto* file = session_.activeFile();
  const auto point = event->position();
  const auto zone = zoneAt(point);
  if (event->button() != Qt::LeftButton || !file || zone == Zone::None) return;
  if (creating_ && kind_ == Kind::Main && zone != Zone::Plot) return;
  if (detectionRoi_ && !detectionRoiContextMatches()) {
    cancelGesture();
    event->accept();
    return;
  }
  if (detectionRoi_ && gesture_) {
    event->accept();
    return;
  }
  if (activeGesture && activeGesture != this &&
      activeGesture->window() == window())
    activeGesture->cancelGesture();
  if (activeWheel && activeWheel != this && activeWheel->window() == window())
    activeWheel->finishWheel();
  const bool roiAxisPan = detectionRoi_ && zone != Zone::Plot;
  if (detectionRoi_ && zone == Zone::Plot) {
    finishWheel();
    detectionRoi_->preview.reset();
    emit detectionRoiPreviewChanged();
  } else
    cancelGesture();
  setFocus();
  file = session_.activeFile();
  const auto hit = hitMark(point);
  Gesture gesture;
  gesture.before = session_.snapshot();
  gesture.start = point;
  gesture.current = point;
  gesture.device = event->pointingDevice();
  gesture.mainMode = file->display.mainMode;
  gesture.auxiliaryMode = file->display.auxiliaryMode;
  gesture.clickMarkId = hit.id;
  gesture.clickSegmentId = hit.segmentId;
  if (kind_ == Kind::Navigation) {
    gesture.tool = Tool::Navigate;
    gesture.navigationInside = navigationWindow().contains(point);
  } else if (kind_ == Kind::Auxiliary)
    gesture.tool = zone == Zone::YAxis ? Tool::AuxiliaryY
                   : zone == Zone::XAxis
                       ? file->display.auxiliaryMode == AuxiliaryMode::Psd
                             ? Tool::PanFrequency
                             : Tool::PanTime
                       : Tool::AuxiliaryZoom;
  else if (detectionRoi_)
    gesture.tool = Tool::DetectionRoi;
  else if (creating_ && !roiAxisPan)
    gesture.tool = Tool::CreateMark;
  else if (zone == Zone::YAxis || zone == Zone::XAxis)
    gesture.tool =
        (file->display.mainMode == MainMode::Waterfall ? zone == Zone::XAxis
                                                       : zone == Zone::YAxis)
            ? Tool::PanFrequency
            : Tool::PanTime;
  else if (hit.selected &&
           !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
    if (hit.id != file->activeMarkId)
      session_.annotations().select(*file, file->selectedMarkIds, hit.id,
                                    hit.segmentId);
    gesture.tool = Tool::MarkEdit;
    gesture.markId = hit.id;
    gesture.edges = hit.edges;
    auto* annotation = findMark(*file, hit.id);
    gesture.observationOnly = annotation->segments.empty();
    gesture.mark =
        gesture.observationOnly ? *annotation->observation : annotation->range;
    gesture.draft = gesture.mark;
    gesture.editContext =
        annotationEditContext(*file, hit.id, session_.projectGeneration());
  } else
    gesture.tool = Tool::ZoomBox;
  freezeAxisLayouts();
  gesture_ = gesture;
  activeGesture = this;
  grabMouse();
  updateHover(point);
  repaintChart();
  event->accept();
}
void PlotWidget::mouseMoveEvent(QMouseEvent* event) {
  auto* file = session_.activeFile();
  if (!file) return;
  if (detectionRoi_ && !detectionRoiContextMatches()) {
    cancelGesture();
    return;
  }
  const auto point = event->position();
  hoverPosition_ =
      plotRect().contains(point) ? std::optional<QPointF>{point} : std::nullopt;
  if (gesture_ && gesture_->device == event->pointingDevice() &&
      gesture_->before.fileId == file->metadata.id &&
      (gesture_->tool == Tool::PanTime ||
       gesture_->tool == Tool::PanFrequency ||
       gesture_->tool == Tool::Navigate))
    beginPreview();
  if (kind_ == Kind::Main && plotRect().contains(point)) {
    const auto coordinate = fromPixel(point, file->view);
    cursorSample_ = offsetSample(file->view.time.begin, coordinate.sample,
                                 availableSamples(file->metadata));
    cursorFrequency_ = coordinate.frequency;
    emit cursorChanged(cursorSample_, cursorFrequency_);
  }
  if (!gesture_) {
    updateHover(point);
    repaintChart();
    return;
  }
  auto& gesture = *gesture_;
  if (gesture.device != event->pointingDevice()) return;
  if (file->metadata.id != gesture.before.fileId ||
      file->display.mainMode != gesture.mainMode ||
      file->display.auxiliaryMode != gesture.auxiliaryMode) {
    cancelGesture(file->metadata.id != gesture.before.fileId);
    return;
  }
  const auto plot = plotRect();
  gesture.current = QPointF(std::clamp(point.x(), plot.left(), plot.right()),
                            std::clamp(point.y(), plot.top(), plot.bottom()));
  const double dx = point.x() - gesture.start.x(),
               dy = point.y() - gesture.start.y();
  const bool axisPan = gesture.tool == Tool::PanTime ||
                       gesture.tool == Tool::PanFrequency ||
                       gesture.tool == Tool::AuxiliaryY;
  const bool moved =
      gesture.tool == Tool::Navigate
          ? std::abs(dx) > 4
          : std::max(std::abs(dx), std::abs(dy)) > (axisPan ? 0 : 5);
  if (!gesture.changed && !moved) {
    updateHover(point);
    repaintChart();
    return;
  }
  gesture.changed = true;
  if (gesture.tool == Tool::AuxiliaryY) beginPreview();
  const auto& base = gesture.before.view;
  if (gesture.tool == Tool::MarkEdit) {
    auto* mark = findMark(*file, gesture.markId);
    if (!mark ||
        !annotationEditContextMatches(*file, gesture.editContext,
                                      session_.projectGeneration()) ||
        !session_.annotations().canEdit(*file, gesture.markId)) {
      cancelGesture();
      return;
    }
    const auto start = fromPixel(gesture.start, base),
               current = fromPixel(point, base);
    const long double dt = current.sample - start.sample;
    const double df = current.frequency - start.frequency;
    const auto bounds = fullRange(file->metadata);
    auto next = gesture.mark;
    if (gesture.edges == Move) {
      if (!gesture.observationOnly) {
        const auto span = gesture.mark.time.end - gesture.mark.time.begin;
        next.time.begin = offsetSample(gesture.mark.time.begin, dt,
                                       availableSamples(file->metadata) - span);
        next.time.end = next.time.begin + span;
      }
      const double width =
          gesture.mark.frequency.upperHz - gesture.mark.frequency.lowerHz;
      next.frequency.lowerHz = std::clamp(gesture.mark.frequency.lowerHz + df,
                                          bounds.frequency.lowerHz,
                                          bounds.frequency.upperHz - width);
      next.frequency.upperHz = next.frequency.lowerHz + width;
    } else {
      const bool waterfall = file->display.mainMode == MainMode::Waterfall;
      const auto minTime = std::min<SampleIndex>(
          file->display.stftSize,
          gesture.mark.time.end - gesture.mark.time.begin);
      const double minFrequency = std::min(
          file->metadata.sampleRateHz / file->display.stftSize,
          gesture.mark.frequency.upperHz - gesture.mark.frequency.lowerHz);
      if (!gesture.observationOnly &&
          (gesture.edges & (waterfall ? Top : Left)))
        next.time.begin = offsetSample(gesture.mark.time.begin, dt,
                                       gesture.mark.time.end - minTime);
      if (!gesture.observationOnly &&
          (gesture.edges & (waterfall ? Bottom : Right)))
        next.time.end =
            std::max(gesture.mark.time.begin + minTime,
                     offsetSample(gesture.mark.time.end, dt,
                                  availableSamples(file->metadata)));
      if (gesture.edges & (waterfall ? Left : Bottom))
        next.frequency.lowerHz = std::clamp(
            gesture.mark.frequency.lowerHz + df, bounds.frequency.lowerHz,
            gesture.mark.frequency.upperHz - minFrequency);
      if (gesture.edges & (waterfall ? Right : Top))
        next.frequency.upperHz =
            std::clamp(gesture.mark.frequency.upperHz + df,
                       gesture.mark.frequency.lowerHz + minFrequency,
                       bounds.frequency.upperHz);
    }
    gesture.draft = next;
  } else if (gesture.tool == Tool::AuxiliaryY) {
    const bool psd = file->display.auxiliaryMode == AuxiliaryMode::Psd;
    const double low = psd ? gesture.before.psdMin : gesture.before.waveformMin;
    const double high =
        psd ? gesture.before.psdMax : gesture.before.waveformMax;
    const auto shifted =
        chart_interaction::panByFraction({low, high}, dy / plot.height());
    session_.setAuxiliaryRange(shifted.first, shifted.last, false);
  } else if (gesture.tool == Tool::PanTime ||
             gesture.tool == Tool::PanFrequency ||
             gesture.tool == Tool::Navigate) {
    auto next = base;
    if (gesture.tool == Tool::Navigate) {
      if (gesture.navigationInside) {
        const auto span = base.time.end - base.time.begin;
        const auto first = offsetSample(
            base.time.begin,
            dx / plot.width() *
                static_cast<long double>(availableSamples(file->metadata)),
            availableSamples(file->metadata) - span);
        next.time = {first, first + span};
        session_.setView(next, false);
      }
    } else if (gesture.tool == Tool::PanTime) {
      const bool vertical =
          kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall;
      const auto span = base.time.end - base.time.begin;
      const auto first =
          offsetSample(base.time.begin,
                       -(vertical ? dy / plot.height() : dx / plot.width()) *
                           static_cast<long double>(span),
                       availableSamples(file->metadata) - span);
      next.time = {first, first + span};
      session_.setView(next, false);
    } else {
      const bool vertical = kind_ == Kind::Main &&
                            file->display.mainMode == MainMode::TimeFrequency;
      const auto shifted = chart_interaction::panByFraction(
          {base.frequency.lowerHz, base.frequency.upperHz},
          vertical ? dy / plot.height() : -dx / plot.width());
      next.frequency = {shifted.first, shifted.last};
      session_.setView(next, false);
    }
  }
  updateHover(point);
  repaintChart();
  if (gesture.tool == Tool::MarkEdit || gesture.tool == Tool::AuxiliaryY ||
      gesture.tool == Tool::PanTime || gesture.tool == Tool::PanFrequency ||
      gesture.tool == Tool::Navigate)
    emit stateChanged();
}
void PlotWidget::finishGesture(Qt::KeyboardModifiers modifiers) {
  if (!gesture_) return;
  const auto releaseGeometry = qScopeGuard([this] { releaseAxisLayouts(); });
  const auto gesture = *gesture_;
  gesture_.reset();
  if (activeGesture == this) activeGesture.clear();
  releasing_ = true;
  if (mouseGrabber() == this) releaseMouse();
  releasing_ = false;
  auto* file = session_.activeFile();
  if (gesture.tool == Tool::DetectionRoi) {
    if (!detectionRoiContextMatches()) {
      cancelDetectionRoi();
      return;
    }
    detectionRoi_->preview = detectionRoiRange(gesture);
    const auto message = QString(
        detectionRoi_->preview ? "检测 ROI 待确认 · Enter 确认 / Esc 取消"
                               : "请拖出非空的检测 ROI；未改变视图或标记");
    repaintChart();
    emit detectionRoiPreviewChanged();
    emit statusMessage(message);
    return;
  }
  if (!file || file->metadata.id != gesture.before.fileId) return;
  const auto& base = gesture.before.view;
  if (gesture.tool == Tool::MarkEdit) {
    if (!gesture.changed) {
      selectAt(gesture.start, modifiers);
    } else if (auto* mark = findMark(*file, gesture.markId)) {
      const auto result =
          annotationEditContextMatches(*file, gesture.editContext,
                                       session_.projectGeneration())
              ? session_.commitAnnotationGeometry(mark->id, gesture.draft)
              : AnnotationResult{false, false, {}, "编辑对象或基础修订已改变"};
      emit statusMessage(result.ok
                             ? QStringLiteral("已提交一次标记修订；需重新复核")
                             : QString::fromStdString(result.error));
    }
  } else if (gesture.tool == Tool::ZoomBox ||
             gesture.tool == Tool::CreateMark) {
    const auto box = QRectF(gesture.start, gesture.current)
                         .normalized()
                         .intersected(plotRect());
    if (box.width() >= 8 && box.height() >= 8) {
      const auto first = fromPixel(box.topLeft(), base),
                 last = fromPixel(box.bottomRight(), base);
      ViewRange next{
          {offsetSample(base.time.begin, std::min(first.sample, last.sample),
                        availableSamples(file->metadata)),
           offsetSample(base.time.begin, std::max(first.sample, last.sample),
                        availableSamples(file->metadata))},
          {std::min(first.frequency, last.frequency),
           std::max(first.frequency, last.frequency)}};
      if (gesture.tool == Tool::CreateMark) {
        if (next.time.end - next.time.begin <
                std::min<SampleIndex>(file->display.stftSize,
                                      availableSamples(file->metadata)) ||
            next.frequency.upperHz - next.frequency.lowerHz <
                file->metadata.sampleRateHz / file->display.stftSize)
          emit statusMessage("标记范围小于当前 STFT 分辨率");
        else {
          session_.addMark(next);
          emit statusMessage("已创建信号标记；持续选择信号仍开启");
        }
      } else {
        session_.setView(next);
        emit statusMessage("矩形区域放大");
      }
    } else if (!gesture.changed && gesture.tool == Tool::ZoomBox)
      selectAt(gesture.start, modifiers);
  } else if (gesture.tool == Tool::AuxiliaryZoom) {
    if (std::abs(gesture.current.x() - gesture.start.x()) >= 8) {
      const auto plot = plotRect();
      const double low = chart_interaction::fractionAt(
          std::min(gesture.start.x(), gesture.current.x()), plot.left(),
          plot.width());
      const double high = chart_interaction::fractionAt(
          std::max(gesture.start.x(), gesture.current.x()), plot.left(),
          plot.width());
      auto next = base;
      if (file->display.auxiliaryMode == AuxiliaryMode::Waveform)
        next.time = {offsetSample(base.time.begin,
                                  low * static_cast<long double>(
                                            base.time.end - base.time.begin),
                                  availableSamples(file->metadata)),
                     offsetSample(base.time.begin,
                                  high * static_cast<long double>(
                                             base.time.end - base.time.begin),
                                  availableSamples(file->metadata))};
      else {
        const auto selected = chart_interaction::selectFractions(
            {base.frequency.lowerHz, base.frequency.upperHz}, low, high);
        next.frequency = {selected.first, selected.last};
      }
      session_.setView(next);
    }
  } else if (gesture.tool == Tool::Navigate && !gesture.changed) {
    const auto full = fullRange(file->metadata);
    const auto coordinate = fromPixel(gesture.start, full);
    const auto span = base.time.end - base.time.begin;
    const auto first = sampleIndex(coordinate.sample - span / 2,
                                   availableSamples(file->metadata) - span);
    auto next = base;
    next.time = {first, first + span};
    session_.setView(next);
  } else if (gesture.changed)
    session_.commitViewChange(gesture.before);
  emit stateChanged();
  repaintChart();
}
void PlotWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !gesture_ ||
      gesture_->device != event->pointingDevice())
    return;
  mouseMoveEvent(event);
  if (!gesture_) {
    event->accept();
    return;
  }
  const bool click = !gesture_->changed && !creating_ &&
                     plotRect().contains(event->position()) &&
                     (gesture_->tool == Tool::ZoomBox ||
                      gesture_->tool == Tool::AuxiliaryZoom ||
                      gesture_->tool == Tool::MarkEdit);
  finishGesture(event->modifiers());
  if (click) pinAt(event->position());
  updateHover(event->position());
  repaintChart();
  event->accept();
}
void PlotWidget::mouseDoubleClickEvent(QMouseEvent* event) {
  mousePressEvent(event);
}
void PlotWidget::wheelEvent(QWheelEvent* event) {
  if (detectionRoi_) cancelDetectionRoi();
  auto* file = session_.activeFile();
  const auto zone = zoneAt(event->position());
  event->accept();
  if (!file || file->metadata.availability.status == LoadStatus::Loading ||
      !availableSamples(file->metadata) || gesture_ ||
      (activeGesture && activeGesture->window() == window()) ||
      zone == Zone::None)
    return;
  const bool psd = kind_ == Kind::Auxiliary &&
                   file->display.auxiliaryMode == AuxiliaryMode::Psd;
  const bool waterfall =
      kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall;
  const WheelAxis axis =
      kind_ == Kind::Auxiliary && zone == Zone::YAxis ? WheelAxis::AuxiliaryY
      : kind_ == Kind::Main
          ? (waterfall
                 ? zone == Zone::YAxis ? WheelAxis::Time : WheelAxis::Frequency
             : zone == Zone::YAxis ? WheelAxis::Frequency
                                   : WheelAxis::Time)
      : psd ? WheelAxis::Frequency
            : WheelAxis::Time;
  const int scrollLines = QApplication::wheelScrollLines();
  const double pixels =
      event->pixelDelta().isNull()
          ? -event->angleDelta().y() / 120.0 *
                (scrollLines < 0 ? std::max(40, height()) : scrollLines * 16)
          : -event->pixelDelta().y();
  if (!std::isfinite(pixels) || pixels == 0) return;
  if (kind_ == Kind::Auxiliary || kind_ == Kind::Main) beginPreview();
  if (activeWheel && activeWheel != this && activeWheel->window() == window())
    activeWheel->finishWheel();
  if (wheelBase_ &&
      (wheelBase_->fileId != file->metadata.id || wheelAxis_ != axis))
    finishWheel();
  if (!wheelBase_) {
    freezeAxisLayouts();
    wheelBase_ = session_.snapshot();
    wheelAxis_ = axis;
    activeWheel = this;
  }
  const double factor = std::exp(std::clamp(pixels, -480.0, 480.0) * .002);
  const auto plot = plotRect();
  const double x = chart_interaction::fractionAt(event->position().x(),
                                                 plot.left(), plot.width());
  const double y = chart_interaction::fractionAt(event->position().y(),
                                                 plot.top(), plot.height());
  if (axis == WheelAxis::AuxiliaryY) {
    const auto range = chart_interaction::zoomAround(
        {file->display.auxiliaryMin, file->display.auxiliaryMax}, factor,
        1.0 - y);
    session_.setAuxiliaryRange(range.first, range.last, false);
  } else {
    auto next = file->view;
    if (axis == WheelAxis::Frequency) {
      const double ratio = kind_ == Kind::Main && !waterfall ? 1 - y : x;
      const auto range = chart_interaction::zoomAround(
          {next.frequency.lowerHz, next.frequency.upperHz}, factor, ratio);
      next.frequency = {range.first, range.last};
    } else {
      const auto width = next.time.end - next.time.begin;
      const auto span = sampleIndex(static_cast<long double>(width) * factor,
                                    availableSamples(file->metadata));
      const auto pivotOffset =
          kind_ == Kind::Navigation
              ? static_cast<long double>(width) * .5L
              : (waterfall ? y : x) * static_cast<long double>(width);
      const auto first =
          offsetSample(next.time.begin, pivotOffset * (1 - factor),
                       availableSamples(file->metadata) - span);
      next.time = {first, first + span};
    }
    session_.setView(next, false);
  }
  wheelTimer_.start();
  updateHover(event->position());
  emit stateChanged();
  repaintChart();
}
QMenu* PlotWidget::createContextMenu(const QPoint& point) {
  if (activeGesture && activeGesture != this &&
      activeGesture->window() == window())
    activeGesture->cancelGesture();
  if (activeWheel && activeWheel != this && activeWheel->window() == window())
    activeWheel->finishWheel();
  cancelGesture();
  auto* menu = new QMenu(this);
  menu->setObjectName("chartContextMenu");
  menu->setStyleSheet(
      "QMenu{background:#1b2b41;color:#dfecfa;border:1px solid "
      "#56718b;border-radius:6px;padding:5px;min-width:248px;}QMenu::item{"
      "padding:9px "
      "12px;}QMenu::item:selected{background:#275475;}QMenu::item:disabled{"
      "color:#718195;}QMenu::item:checked{background:#63491e;color:#ffe3a0;}"
      "QMenu::separator{height:1px;background:#375168;margin:4px 0;}");
  const auto* file = session_.activeFile();
  addMenuHeading(*menu, QString(kind_ == Kind::Main        ? "宽带图谱"
                                : kind_ == Kind::Auxiliary ? "辅助分析"
                                                           : "全局导航") +
                            " · 视图与标记");
  if (kind_ == Kind::Main) {
    auto* toggle = menu->addAction("选择信号（持续模式）");
    toggle->setObjectName("contextMarkToggle");
    toggle->setCheckable(true);
    toggle->setChecked(creating_);
    toggle->setEnabled(file);
    connect(toggle, &QAction::triggered, this,
            [this](bool checked) { setCreating(checked); });
    const auto overlaps = markChoicesAt(point);
    if (overlaps.size() > 1) {
      menu->addSeparator();
      addMenuHeading(*menu, "选择重叠标记");
      addMarkChoices(menu, overlaps, Qt::NoModifier);
    }
    if (file && findMark(*file, file->activeMarkId)) {
      const auto capabilities = session_.annotations().capabilities(*file, file->activeMarkId);
      auto* channel = menu->addAction("创建窄带通道…");
      channel->setObjectName("contextCreateChannelAction");
      channel->setEnabled(capabilities.createChannelDirectly || capabilities.convertToManual);
      if (!capabilities.createChannelDirectly) channel->setText("转换并创建窄带通道…");
      connect(channel, &QAction::triggered, this,
              &PlotWidget::annotationChannelRequested);
      auto* rename = menu->addAction("重命名当前标记");
      rename->setObjectName("contextRenameAction");
      rename->setEnabled(capabilities.editMetadata);
      connect(rename, &QAction::triggered, this,
              &PlotWidget::markRenameRequested);
      auto* locate = menu->addAction("定位当前标记");
      locate->setObjectName("contextLocateAction");
      connect(locate, &QAction::triggered, this, [this] {
        if (auto* file = session_.activeFile())
          session_.focusMark(file->activeMarkId);
        emit stateChanged();
      });
      auto* remove = menu->addAction(
          QString("删除所选标记 (%1)").arg(file->selectedMarkIds.size()));
      remove->setObjectName("contextDeleteAction");
      remove->setEnabled(!file->selectedMarkIds.empty() &&
          std::all_of(file->selectedMarkIds.begin(), file->selectedMarkIds.end(), [&](const auto& id) {
            return session_.annotations().capabilities(*file, id).deleteAnnotation;
          }));
      connect(remove, &QAction::triggered, this,
              &PlotWidget::markDeleteRequested);
    }
    auto* detect = menu->addAction("检测当前可见区域…");
    detect->setObjectName("contextWidebandDetectionAction");
    detect->setEnabled(file && !file->metadata.demo &&
                       file->metadata.availability.status !=
                           LoadStatus::Loading &&
                       availableSamples(file->metadata) > 0);
    connect(detect, &QAction::triggered, this,
            &PlotWidget::widebandDetectionRequested);
    auto* roi = menu->addAction("选择检测 ROI…");
    roi->setObjectName("contextDetectionRoiAction");
    roi->setEnabled(file &&
                    file->metadata.availability.status != LoadStatus::Loading &&
                    availableSamples(file->metadata) > 0);
    const auto sourceId = file ? file->metadata.id : std::string{};
    const auto project = session_.projectGeneration();
    const bool previousCreating = creating_;
    connect(roi, &QAction::triggered, this, [this, sourceId, project, previousCreating] {
      // Closing a native menu deactivates its owning window. Start the new
      // tool after that transition, preserving the tool captured by the menu.
      QTimer::singleShot(0, this, [this, sourceId, project, previousCreating] {
        const auto* active = session_.activeFile();
        if (!active || active->metadata.id != sourceId || session_.projectGeneration() != project) return;
        if (QApplication::applicationState() != Qt::ApplicationActive) return;
        if (previousCreating && !creating_) setCreating(true);
        emit detectionRoiRequested();
      });
    });
  }
  menu->addSeparator();
  const auto addViewAction = [&](const QString& label, const char* name,
                                 bool enabled, auto callback) {
    auto* action = menu->addAction(label);
    action->setObjectName(QString::fromLatin1(name));
    action->setEnabled(enabled);
    connect(action, &QAction::triggered, this, callback);
  };
  if (kind_ != Kind::Navigation) {
    addViewAction("清除驻留游标", "contextClearCursor",
                  file && session_.linkedCursor(file->metadata.id).pinned,
                  [this] {
                    if (const auto* current = session_.activeFile())
                      session_.clearCursor(current->metadata.id);
                    emit stateChanged();
                  });
    addViewAction("当前窗 / 标记平均谱", "contextAveragePsd", file, [this] {
      if (const auto* current = session_.activeFile())
        session_.setFramePsd(current->metadata.id, false);
      emit stateChanged();
    });
    addViewAction("驻留帧谱（点数随 STFT）", "contextFramePsd",
                  file && session_.linkedCursor(file->metadata.id).pinned,
                  [this] {
                    if (const auto* current = session_.activeFile())
                      session_.setFramePsd(current->metadata.id, true);
                    emit stateChanged();
                  });
    auto* fit = menu->addAction("自动适配电平");
    fit->setObjectName("contextAutoPowerFit");
    fit->setEnabled(property("autoPowerFitAvailable").toBool());
    connect(fit, &QAction::triggered, this, &PlotWidget::autoPowerFitRequested);
    addViewAction("扩展时间窗以满足分析点数", "contextExpandAnalysisTime", file,
                  [this] { expandAnalysisTime(); });
    menu->addSeparator();
  }
  addViewAction("↶ 视图后退", "contextBackAction", file && session_.canBack(),
                [this] {
                  session_.back();
                  emit stateChanged();
                });
  addViewAction("↷ 视图前进", "contextForwardAction",
                file && session_.canForward(), [this] {
                  session_.forward();
                  emit stateChanged();
                });
  menu->addSeparator();
  addViewAction("⤢ 适应全部数据", "contextFitAction", file, [this] {
    if (const auto* file = session_.activeFile())
      session_.setView(fullRange(file->metadata));
    emit stateChanged();
  });
  addViewAction("↺ 恢复默认视图", "contextResetAction", file, [this] {
    session_.resetView();
    emit stateChanged();
  });
  return menu;
}
void PlotWidget::contextMenuEvent(QContextMenuEvent* event) {
  auto* menu = createContextMenu(event->pos());
  menu->exec(event->globalPos());
  delete menu;
  event->accept();
}
void PlotWidget::keyPressEvent(QKeyEvent* event) {
  if (detectionRoi_ &&
      (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
    confirmDetectionRoi();
    event->accept();
    return;
  }
  if (detectionRoi_ && event->key() == Qt::Key_Escape) {
    cancelDetectionRoi();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Escape) {
    const bool pending = hasPendingInteraction() || creating_;
    cancelGesture(true);
    if (!pending) {
      if (const auto* file = session_.activeFile())
        session_.clearCursor(file->metadata.id);
    }
    emit stateChanged();
    emit statusMessage(pending ? "已取消拖动并退出持续选择" : "已清除驻留游标");
    event->accept();
  } else if (event->key() == Qt::Key_Delete && kind_ == Kind::Main) {
    emit markDeleteRequested();
    event->accept();
  } else if (event->matches(QKeySequence::SelectAll) && kind_ == Kind::Main) {
    if (const auto* file = session_.activeFile()) {
      std::vector<std::string> ids;
      for (const auto& mark : file->marks)
        if (annotationVisible(*file, mark)) ids.push_back(mark.id);
      const auto active =
          std::find(ids.begin(), ids.end(), file->activeMarkId) != ids.end()
              ? file->activeMarkId
          : ids.empty() ? std::string{}
                        : ids.front();
      session_.selectMarks(ids, active);
      emit stateChanged();
    }
    event->accept();
  } else
    QWidget::keyPressEvent(event);
}
void PlotWidget::resizeEvent(QResizeEvent* event) {
  beginPreview();
  cancelGesture();
  if (surface_) surface_->setGeometry(rect());
  QWidget::resizeEvent(event);
  if (kind_ == Kind::Navigation)
    repaintChart();
  else
    refreshAxisLayouts();
}
bool PlotWidget::event(QEvent* event) {
  if (event->type() == QEvent::FontChange ||
      event->type() == QEvent::ScreenChangeInternal ||
      event->type() == QEvent::DevicePixelRatioChange) {
    axisLayoutCache_.reset();
    // Style propagation can deliver FontChange while the window is being
    // constructed. Repaint peers after that propagation has completed.
    QTimer::singleShot(0, this, [this] {
      cancelGesture();
      refreshAxisLayouts();
    });
  }
  if (cursor_overlay::tooltip(this, event)) return true;
  if ((event->type() == QEvent::UngrabMouse && !releasing_) ||
      event->type() == QEvent::WindowDeactivate ||
      event->type() == QEvent::Hide ||
      (event->type() == QEvent::FocusOut && gesture_ && detectionRoi_))
    cancelGesture(!detectionRoi_ &&
                  (event->type() == QEvent::WindowDeactivate ||
                   event->type() == QEvent::Hide));
  else if (event->type() == QEvent::Leave && !gesture_) {
    hoverPosition_.reset();
    QToolTip::hideText();
    hoverZone_ = Zone::None;
    setHoveredMark({});
    setProperty("interactionZone", static_cast<int>(hoverZone_));
    setCursor(Qt::ArrowCursor);
    emit interactionHint("鼠标指向图谱查看操作提示");
    repaintChart();
  }
  return QWidget::event(event);
}
}  // namespace signalstudio
