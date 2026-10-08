#include "ui/charts/plot_widget.h"
#include "ui/charts/accelerated_surface.h"
#include "infrastructure/int16_iq_file.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QWidgetAction>
#include <QElapsedTimer>
#include <QMetaObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace signalstudio {
namespace {
constexpr int Left = 1, Right = 2, Top = 4, Bottom = 8, Move = 16;
QPointer<PlotWidget> activeGesture;
QPointer<PlotWidget> activeWheel;

SampleIndex sampleIndex(long double value, SampleIndex maximum) {
    if (value <= 0) return 0;
    if (value >= static_cast<long double>(maximum)) return maximum;
    return static_cast<SampleIndex>(value);
}
SampleIndex offsetSample(SampleIndex origin, long double offset, SampleIndex maximum) {
    origin = std::min(origin, maximum);
    return offset >= 0 ? origin + sampleIndex(offset, maximum - origin) : origin - sampleIndex(-offset, origin);
}
double seconds(SampleIndex sample, double sampleRate) { return static_cast<double>(sample) / sampleRate; }
double randomValue(double x) { const double n = std::sin(x * 92.743 + 12.41) * 143758.327; return n - std::floor(n); }
QColor cssColor(const char* value) {
    const QString hex = QString::fromLatin1(value);
    if (hex.size() != 9) return QColor(hex);
    return QColor(hex.mid(1, 2).toInt(nullptr, 16), hex.mid(3, 2).toInt(nullptr, 16),
                  hex.mid(5, 2).toInt(nullptr, 16), hex.mid(7, 2).toInt(nullptr, 16));
}
QFont canvasFont(int pixels = 10, bool labels = false) {
    QFont font(labels ? QStringLiteral("Microsoft YaHei UI") : QStringLiteral("Consolas"));
    font.setPixelSize(pixels);
    return font;
}
QString numberText(double value, int decimals) {
    if (!std::isfinite(value)) return QStringLiteral("—");
    decimals = std::clamp(decimals, 0, 12);
    if (std::abs(value) < std::pow(10.0, -decimals) / 2) value = 0;
    auto text = QString::number(value, 'f', decimals);
    if (text.contains('.')) { while (text.endsWith('0')) text.chop(1); if (text.endsWith('.')) text.chop(1); }
    return text;
}
int tickDecimals(double step) { return std::clamp(static_cast<int>(std::ceil(-std::log10(std::max(std::abs(step), 1e-12)))) + 2, 0, 12); }
struct Unit { QString name; double scale; };
Unit displayUnit(bool frequency, double span) {
    span = std::abs(span);
    return frequency ? (span >= 1e6 ? Unit{"MHz", 1e-6} : span >= 1e3 ? Unit{"kHz", 1e-3} : Unit{"Hz", 1}) :
                       (span >= 1 ? Unit{"s", 1} : span >= .001 ? Unit{"ms", 1e3} : Unit{"us", 1e6});
}
QString coordinateText(bool frequency, double value, double span = 0, double step = 0) {
    const double magnitude = std::abs(value);
    const auto unit = displayUnit(frequency, span > 0 && magnitude < span * 1000 ? span : magnitude > 0 ? magnitude : span > 0 ? span : frequency ? 1e6 : 1);
    const double resolution = std::abs(step != 0 ? step : span > 0 ? span / 5 : frequency ? 1 : 1e-6);
    return numberText(value * unit.scale, tickDecimals(resolution * unit.scale)) + " " + unit.name;
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
Axis axisScale(bool frequency, double start, double span, int count, double offset = 0) {
    Axis axis;
    axis.start = start; axis.span = span; axis.count = count;
    axis.unit = displayUnit(frequency, std::abs(span));
    axis.decimals = tickDecimals(span / std::max(1, count) * axis.unit.scale);
    const double lo = std::min(start, start + span) - offset;
    const double hi = std::max(start, start + span) - offset;
    axis.relative = std::abs(span) > 0 && (std::max(std::abs(lo), std::abs(hi)) >= std::abs(span) * 1000 ||
                    numberText(lo * axis.unit.scale, axis.decimals).size() > 9 || numberText(hi * axis.unit.scale, axis.decimals).size() > 9);
    axis.displayStart = axis.relative ? (span < 0 ? std::abs(span) : 0) : start - offset;
    axis.label = QString(frequency ? "频率" : "时间") + (axis.relative ? " Δ" : "") + " (" + axis.unit.name + ")" +
                 (!axis.relative && offset != 0 ? " · 基带" : "");
    if (axis.relative) axis.originLabel = QString(frequency ? "F₀ = " : "T₀ = ") +
        coordinateText(frequency, lo, 0, std::abs(span) / std::max(1, count)) + (offset != 0 ? "（基带）" : "");
    return axis;
}
QString rangeText(bool frequency, double start, double span) {
    const auto axis = axisScale(frequency, start, span, 5);
    return axis.relative ? coordinateText(frequency, start, 0, span / 5) + " + [" + axis.tick(0) + "–" + axis.tick(1) + "] " + axis.unit.name :
                           axis.tick(0) + "–" + axis.tick(1) + " " + axis.unit.name;
}
QRgb color(float level, Palette palette) {
    static const std::vector<std::array<int, 3>> turbo{{8,14,52},{20,58,141},{10,144,216},{22,207,222},{40,225,138},{224,229,70},{250,159,52},{188,49,52}};
    static const std::vector<std::array<int, 3>> viridis{{40,18,71},{62,71,129},{49,112,142},{37,154,141},{89,189,94},{206,219,53}};
    static const std::vector<std::array<int, 3>> gray{{11,17,26},{62,75,95},{133,149,159},{220,228,234}};
    static const std::vector<std::array<int, 3>> plasma{{13,8,135},{126,3,168},{204,71,120},{248,149,64},{240,249,33}};
    static const std::vector<std::array<int, 3>> inferno{{0,0,4},{87,16,110},{188,55,84},{249,142,9},{252,255,164}};
    static const std::vector<std::array<int, 3>> magma{{0,0,4},{81,18,124},{183,55,121},{251,136,97},{252,253,191}};
    static const std::vector<std::array<int, 3>> cividis{{0,32,76},{59,82,139},{124,123,120},{188,167,72},{253,231,55}};
    static const std::vector<std::array<int, 3>> coolEditClassic{
        {11,4,41}, {22,2,64}, {43,0,98}, {105,0,113},
        {161,0,96}, {215,12,45}, {245,66,11}, {255,175,28}, {255,234,46}
    };
    const auto& stops = [&]() -> const std::vector<std::array<int, 3>>& {
        switch (palette) {
        case Palette::Turbo: return turbo;
        case Palette::Viridis: return viridis;
        case Palette::Gray: return gray;
        case Palette::Plasma: return plasma;
        case Palette::Inferno: return inferno;
        case Palette::Magma: return magma;
        case Palette::Cividis: return cividis;
        case Palette::CoolEditClassic: return coolEditClassic;
        }
        return turbo;
    }();
    const double z = std::clamp(static_cast<double>(level), 0.0, .999) * (stops.size() - 1);
    const auto index = static_cast<std::size_t>(z);
    const double ratio = z - index;
    const auto channel = [&](int i) { return static_cast<int>(std::lround(stops[index][i] * (1 - ratio) + stops[index + 1][i] * ratio)); };
    return qRgb(channel(0), channel(1), channel(2));
}
Qt::CursorShape edgeCursor(int edges) {
    if (edges == Move) return Qt::SizeAllCursor;
    if ((edges & (Left | Right)) && (edges & (Top | Bottom)))
        return edges == (Left | Top) || edges == (Right | Bottom) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    return edges & (Left | Right) ? Qt::SizeHorCursor : Qt::SizeVerCursor;
}
std::array<std::pair<QPointF, int>, 8> handles(const QRectF& rect) {
    return {{{rect.topLeft(), Left | Top}, {{rect.center().x(), rect.top()}, Top}, {rect.topRight(), Right | Top},
             {{rect.right(), rect.center().y()}, Right}, {rect.bottomRight(), Right | Bottom},
             {{rect.center().x(), rect.bottom()}, Bottom}, {rect.bottomLeft(), Left | Bottom}, {{rect.left(), rect.center().y()}, Left}}};
}
void drawPill(QPainter& painter, QRectF canvas, const QString& text, bool creation) {
    painter.save(); painter.setFont(canvasFont(creation ? 10 : 11, true));
    const double paddingX = creation ? 13 : 12, paddingY = creation ? 7 : 5;
    const QFontMetricsF metrics(painter.font());
    const double width = std::min(canvas.width() * .95, metrics.horizontalAdvance(text) + paddingX * 2 + 2);
    const double height = (creation ? 10 : 11) * 1.4 + paddingY * 2 + 2;
    const QRectF rect(canvas.center().x() - width / 2, creation ? 10 : 7, width, height);
    painter.setPen(creation ? QColor("#edc56d") : QColor("#3d7898"));
    painter.setBrush(creation ? cssColor("#6a4a1be8") : cssColor("#19354eeb"));
    painter.drawRoundedRect(rect, 4, 4);
    painter.setPen(creation ? QColor("#fff5d8") : QColor("#a9eeff"));
    painter.drawText(rect.adjusted(paddingX, 0, -paddingX, 0), Qt::AlignCenter,
                     metrics.elidedText(text, Qt::ElideRight, static_cast<int>(width - 2 * paddingX)));
    painter.restore();
}
void addMenuHeading(QMenu& menu, const QString& text) {
    auto* action = new QWidgetAction(&menu);
    auto* label = new QLabel(text, &menu);
    label->setStyleSheet("color:#7fadc8;padding:6px 11px;font-size:11px;font-weight:600;");
    action->setDefaultWidget(label); menu.addAction(action);
}
} // namespace

struct HeatmapPayload {
    QString key;
    FileMetadata metadata;
    ViewRange view;
    MainMode mode = MainMode::TimeFrequency;
    QSize pixels;
    quint64 generation = 0;
    int fftSize = 2048;
    bool preview = false;
    std::vector<float> power;
    QString error;
    std::size_t sourcePoints = 0;
    double elapsedMs = 0;
};

struct CurvePayload {
    QString key;
    quint64 generation = 0;
    std::vector<float> samples;
    QString error;
    double elapsedMs = 0;
};

// One worker owns a replaceable mailbox. It never reads Session or QWidget.
// A newer generation cancels work at each raster row, and the GUI checks it
// again before installing a result. Destruction joins before Session can die.
class HeatmapWorker {
public:
    using Callback = std::function<void(std::shared_ptr<HeatmapPayload>)>;
    explicit HeatmapWorker(Callback callback) : callback_(std::move(callback)), thread_([this] { run(); }) {}
    ~HeatmapWorker() {
        { std::lock_guard lock(mutex_); stop_ = true; pending_.reset(); }
        changed_.notify_one(); thread_.join();
    }
    void submit(std::shared_ptr<HeatmapPayload> request) {
        latest_ = request->generation;
        { std::lock_guard lock(mutex_); if (pending_) ++discarded_; pending_ = std::move(request); pendingFlag_ = true; }
        changed_.notify_one();
    }
    void cancel(quint64 generation) {
        latest_ = generation;
        std::lock_guard lock(mutex_); if (pending_) { pending_.reset(); pendingFlag_ = false; ++discarded_; }
    }
    quint64 discarded() const { return discarded_.load(); }
    bool idle() const { return !working_ && !pendingFlag_; }
private:
    void run() {
        for (;;) {
            std::shared_ptr<HeatmapPayload> job;
            { std::unique_lock lock(mutex_); changed_.wait(lock, [this] { return stop_ || pending_; });
              if (stop_) return; working_ = true; job = std::move(pending_); pendingFlag_ = false; }
            QElapsedTimer timer; timer.start();
            const int width = job->pixels.width(), height = job->pixels.height();
            job->power.resize(static_cast<std::size_t>(width) * height);
            job->sourcePoints = job->power.size();
            bool abandoned = false;
            const auto& view = job->view; const auto& metadata = job->metadata;
            if (!metadata.demo) {
                Int16IqFile iq;
                QString error;
                if (!iq.open(QString::fromUtf8(metadata.path.data(), static_cast<qsizetype>(metadata.path.size())), error)) {
                    job->error = error;
                } else if (!iq.spectrogram(metadata, view, job->mode, job->pixels, job->fftSize, job->power,
                                           [this, generation = job->generation] { return stop_ || generation != latest_; })) {
                    if (stop_ || job->generation != latest_) { ++discarded_; working_ = false; continue; }
                    job->error = QStringLiteral("IQ 时频计算失败");
                }
            } else {
            for (int y = 0; y < height; ++y) {
                if (stop_ || job->generation != latest_) { abandoned = true; break; }
                for (int x = 0; x < width; ++x) {
                    const double xr = static_cast<double>(x) / (width - 1), yr = static_cast<double>(y) / (height - 1);
                    const bool waterfall = job->mode == MainMode::Waterfall;
                    const double frequency = view.frequency.lowerHz + (waterfall ? xr : 1 - yr) * (view.frequency.upperHz - view.frequency.lowerHz);
                    const double fn = (frequency - metadata.centerFrequencyHz) / metadata.sampleRateHz;
                    const double tn = static_cast<double>(view.time.begin) / metadata.sampleCount +
                        (waterfall ? yr : xr) * static_cast<double>(view.time.end - view.time.begin) / metadata.sampleCount;
                    double z = .07 + .14 * randomValue(std::floor(tn * 1200) * .83 + std::floor(fn * 1000) * 7 + metadata.demoSeed) + .07 * std::sin(tn * 85 + fn * 91);
                    for (const double center : {-.29, -.12, .09, .31}) z += .38 * std::exp(-std::pow((fn - center) / .005, 2));
                    if (tn > .22 && tn < .75) {
                        z += .58 * std::exp(-std::pow((fn - .075) / .035, 2)) * (.45 + .55 * std::pow(std::sin(tn * 120 + metadata.demoSeed), 2));
                        z += .28 * std::exp(-std::pow((fn + .17) / .02, 2));
                    }
                    if (tn > .38 && tn < .54 && std::sin(tn * 140) > .2) z += .48 * std::exp(-std::pow((fn + .02) / .09, 2));
                    if (tn > .18 && tn < .75) z += .3 * std::exp(-std::pow((fn - (-.28 + .5 * (tn - .18))) / .008, 2));
                    job->power[static_cast<std::size_t>(y) * width + x] = static_cast<float>(z);
                }
            }
            }
            if (abandoned || stop_ || job->generation != latest_) { ++discarded_; working_ = false; continue; }
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
        WaveformMode waveformMode = WaveformMode::IqRms;
        quint64 generation = 0;
    };
    explicit CurveWorker(Callback callback) : callback_(std::move(callback)), thread_([this] { run(); }) {}
    ~CurveWorker() {
        { std::lock_guard lock(mutex_); stop_ = true; pending_.reset(); }
        changed_.notify_one(); thread_.join();
    }
    void submit(Request request) {
        latest_ = request.generation;
        { std::lock_guard lock(mutex_); pending_ = std::move(request); }
        changed_.notify_one();
    }
    quint64 discarded() const { return discarded_.load(); }
private:
    void run() {
        for (;;) {
            Request request;
            { std::unique_lock lock(mutex_); changed_.wait(lock, [this] { return stop_ || pending_.has_value(); });
              if (stop_) return; request = std::move(*pending_); pending_.reset(); }
            QElapsedTimer timer; timer.start();
            auto result = std::make_shared<CurvePayload>(); result->key = request.key; result->generation = request.generation;
            Int16IqFile iq; QString error;
            if (!iq.open(QString::fromUtf8(request.metadata.path.data(), static_cast<qsizetype>(request.metadata.path.size())), error)) {
                result->error = error;
            } else {
                const auto cancelled = [this, generation = request.generation] { return stop_ || generation != latest_; };
                const bool ok = request.psd ? iq.psd(request.time, request.frequency, request.metadata.sampleRateHz,
                    request.metadata.centerFrequencyHz, request.fftSize, request.points, result->samples, cancelled) :
                    iq.waveform(request.time, request.points, request.waveformMode, result->samples, cancelled);
                if (!ok) {
                    if (stop_ || request.generation != latest_) { ++discarded_; continue; }
                    result->error = QStringLiteral("IQ 曲线计算失败");
                }
            }
            if (stop_ || request.generation != latest_) { ++discarded_; continue; }
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

PlotWidget::PlotWidget(Session& session, Kind kind, QWidget* parent) : QWidget(parent), session_(session), kind_(kind) {
    setMouseTracking(true); setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(150, kind == Kind::Navigation ? 20 : kind == Kind::Auxiliary ? 60 : 110);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(kind == Kind::Navigation ? Qt::PointingHandCursor : Qt::CrossCursor);
    wheelTimer_.setSingleShot(true); wheelTimer_.setInterval(220);
    connect(&wheelTimer_, &QTimer::timeout, this, &PlotWidget::finishWheel);
    renderSettleTimer_.setSingleShot(true); renderSettleTimer_.setInterval(60);
    connect(&renderSettleTimer_, &QTimer::timeout, this, [this] { preview_ = false; repaintChart(); });
    if (kind_ == Kind::Main) worker_ = std::make_unique<HeatmapWorker>([this](std::shared_ptr<HeatmapPayload> result) {
        QMetaObject::invokeMethod(this, [this, result = std::move(result)] { acceptHeatmap(result); }, Qt::QueuedConnection);
    });
    if (kind_ == Kind::Auxiliary) curveWorker_ = std::make_unique<CurveWorker>([this](std::shared_ptr<CurvePayload> result) {
        QMetaObject::invokeMethod(this, [this, result = std::move(result)] { acceptCurve(result); }, Qt::QueuedConnection);
    });
    if (kind_ == Kind::Navigation) navigationWorker_ = std::make_unique<CurveWorker>([this](std::shared_ptr<CurvePayload> result) {
        QMetaObject::invokeMethod(this, [this, result = std::move(result)] { acceptNavigationCurve(result); }, Qt::QueuedConnection);
    });
    if (QGuiApplication::platformName() != "offscreen" && QGuiApplication::platformName() != "minimal" && !qApp->property("softwareRenderer").toBool()) {
        surface_ = new AcceleratedSurface(this); surface_->setGeometry(rect());
        surface_->setPainter([this](QPainter& painter) { paintScene(painter, true); });
        connect(surface_, &AcceleratedSurface::backendReady, this, [this](const QString& backend) { emit backendChanged(backend); });
        connect(surface_, &AcceleratedSurface::backendFailed, this, [this](const QString& reason) {
            softwareFallback_ = true; surface_->hide(); QWidget::update(); emit backendChanged("软件回退 · " + reason);
        });
    }
}
PlotWidget::~PlotWidget() { wheelTimer_.stop(); renderSettleTimer_.stop(); navigationWorker_.reset(); curveWorker_.reset(); worker_.reset(); }
bool PlotWidget::gpuReady() const { return surface_ && !softwareFallback_ && surface_->isReady(); }
QString PlotWidget::renderingBackend() const { return surface_ && !softwareFallback_ ? surface_->backendDescription() : "QPainter 软件绘制"; }
quint64 PlotWidget::textureUploadCount() const { return surface_ ? surface_->textureUploadCount() : 0; }
quint64 PlotWidget::completedFrameCount() const { return surface_ ? surface_->completedFrameCount() : 0; }
void PlotWidget::repaintChart() {
    updateHeatmap();
    if (surface_ && !softwareFallback_) {
        const auto [target, source] = heatmapPlacement();
        surface_->setHeatmap(heatmap_, target, powerKey_ + "/" + colorKey_);
        surface_->setHeatmapSourceRect(source);
        surface_->invalidateOverlay();
    } else QWidget::update();
}
void PlotWidget::beginPreview() { preview_ = true; renderSettleTimer_.start(); }
QRectF PlotWidget::plotRect() const {
    if (kind_ == Kind::Navigation) return QRectF(13, 0, std::max(1, width() - 26), std::max(1, height()));
    const auto* file = session_.activeFile();
    const int right = kind_ == Kind::Main && file && file->display.colorScale ? 70 : 22;
    return QRectF(88, 12, std::max(1, width() - 88 - right), std::max(1, height() - 46));
}
QPointF PlotWidget::toPixel(SampleIndex sample, double frequency) const {
    const auto* file = session_.activeFile(); if (!file) return {};
    const auto view = kind_ == Kind::Navigation ? fullRange(file->metadata) : file->view;
    const auto rect = plotRect();
    const double delta = sample >= view.time.begin ? static_cast<double>(sample - view.time.begin) : -static_cast<double>(view.time.begin - sample);
    const double time = delta / static_cast<double>(view.time.end - view.time.begin);
    const double f = (frequency - view.frequency.lowerHz) / (view.frequency.upperHz - view.frequency.lowerHz);
    return kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall ?
        QPointF(rect.left() + rect.width() * f, rect.top() + rect.height() * time) :
        QPointF(rect.left() + rect.width() * time, rect.bottom() - rect.height() * f);
}
PlotWidget::Coordinate PlotWidget::fromPixel(QPointF point, const ViewRange& view) const {
    const auto rect = plotRect(); const auto* file = session_.activeFile();
    const double x = std::clamp((point.x() - rect.left()) / rect.width(), 0.0, 1.0);
    const double y = std::clamp((point.y() - rect.top()) / rect.height(), 0.0, 1.0);
    const bool waterfall = kind_ == Kind::Main && file && file->display.mainMode == MainMode::Waterfall;
    return {(waterfall ? y : x) * static_cast<long double>(view.time.end - view.time.begin),
            view.frequency.lowerHz + (waterfall ? x : 1 - y) * (view.frequency.upperHz - view.frequency.lowerHz)};
}
QRectF PlotWidget::markRect(const ViewRange& range) const {
    return QRectF(toPixel(range.time.begin, range.frequency.lowerHz), toPixel(range.time.end, range.frequency.upperHz)).normalized();
}
QRectF PlotWidget::navigationWindow() const {
    const auto* file = session_.activeFile(); if (!file) return {};
    const auto plot = plotRect();
    const double start = static_cast<double>(file->view.time.begin) / file->metadata.sampleCount;
    const double span = static_cast<double>(file->view.time.end - file->view.time.begin) / file->metadata.sampleCount;
    return QRectF(plot.left() + plot.width() * start, 0, plot.width() * span, height());
}
PlotWidget::Zone PlotWidget::zoneAt(QPointF point) const {
    const auto plot = plotRect();
    if (kind_ == Kind::Navigation) return rect().contains(point.toPoint()) ? Zone::Plot : Zone::None;
    if (point.x() >= 0 && point.x() < plot.left() && point.y() >= plot.top() && point.y() <= plot.bottom()) return Zone::YAxis;
    if (point.y() >= plot.bottom() && point.y() < height() && point.x() >= plot.left() && point.x() <= plot.right()) return Zone::XAxis;
    if (plot.contains(point)) return Zone::Plot;
    return Zone::None;
}
QString PlotWidget::statusText() const {
    const auto* file = session_.activeFile(); if (!file) return "—";
    if (kind_ == Kind::Navigation) return rangeText(false, 0, seconds(file->metadata.sampleCount, file->metadata.sampleRateHz));
    if (kind_ == Kind::Auxiliary) return QString(file->display.auxiliaryMode == AuxiliaryMode::Psd ? "X 频率 · Y " : "X 时间 · Y ") +
        numberText(file->display.auxiliaryMin, 0) + "…" + numberText(file->display.auxiliaryMax, 0) + (file->display.auxiliaryMode == AuxiliaryMode::Psd ? " dB" : "");
    return file->display.mainMode == MainMode::Waterfall ? "X 频率 · Y 时间↓ · 颜色功率" : "X 时间 · Y 频率 · 颜色功率";
}
QString PlotWidget::tipText() const {
    const auto* file = session_.activeFile(); if (!file || kind_ != Kind::Auxiliary) return {};
    return file->display.auxiliaryMode == AuxiliaryMode::Waveform ?
        QString(file->display.waveformMode == WaveformMode::I ? "I 分量（归一化）" : file->display.waveformMode == WaveformMode::Q ? "Q 分量（归一化）" : "IQ RMS 包络 (dBFS)") + " · 当前时间 " + rangeText(false,
        seconds(file->view.time.begin, file->metadata.sampleRateHz), seconds(file->view.time.end - file->view.time.begin, file->metadata.sampleRateHz)) :
        QString(file->display.psdFromSelection && findMark(*file, file->activeMarkId) ? "当前信号标记" : "当前时间窗") + " · FFT " + QString::number(file->display.psdSize);
}
void PlotWidget::setCursorCoordinates(SampleIndex sample, double frequency) {
    if (cursorSample_ == sample && cursorFrequency_ == frequency) return;
    cursorSample_ = sample; cursorFrequency_ = frequency; repaintChart();
}
void PlotWidget::setHoveredMark(const QString& id) {
    const auto markId = id.toStdString();
    if (hoveredMark_ == markId) return;
    hoveredMark_ = markId; repaintChart();
}
void PlotWidget::setCreating(bool enabled) {
    cancelGesture(); creating_ = enabled && session_.activeFile() && kind_ == Kind::Main;
    hoveredMark_.clear(); setCursor(creating_ ? Qt::CrossCursor : Qt::ArrowCursor);
    repaintChart(); emit stateChanged();
    emit statusMessage(creating_ ? "持续选择信号已开启；右键菜单关闭，Esc 退出" : "持续选择信号已关闭");
}
void PlotWidget::syncState() {
    const auto* file = session_.activeFile();
    const auto id = file ? file->metadata.id : std::string{};
    if (id != displayedFile_) {
        cancelGesture(true); hoveredMark_.clear(); hoverZone_ = Zone::None;
        requestedPowerKey_.clear(); renderedPower_.reset(); heatmap_ = {}; powerKey_.clear(); colorKey_.clear();
        ++renderGeneration_; ++heatmapRequestGeneration_; committedGeneration_ = 0; requestedHeatSize_ = {}; if (worker_) worker_->cancel(heatmapRequestGeneration_);
        displayedView_ = file ? std::optional<ViewRange>{file->view} : std::nullopt;
        if (file) {
            cursorSample_ = displayedFile_.empty() && file->metadata.demoSeed == 1 ?
                sampleIndex(200.0L * file->metadata.sampleRateHz, file->metadata.sampleCount) :
                file->view.time.begin + (file->view.time.end - file->view.time.begin) / 2;
            cursorFrequency_ = file->metadata.centerFrequencyHz;
            if (kind_ == Kind::Main) emit cursorChanged(cursorSample_, cursorFrequency_);
        }
        displayedFile_ = id;
    } else if (file && (file->display.mainMode != displayedMainMode_ || file->display.auxiliaryMode != displayedAuxiliaryMode_ ||
               file->display.stftSize != displayedFft_ || file->display.colorScale != displayedColorScale_)) cancelGesture();
    if (!file) creating_ = false;
    if (file) {
        if (displayedView_ && *displayedView_ != file->view) beginPreview();
        displayedView_ = file->view;
        displayedMainMode_ = file->display.mainMode; displayedAuxiliaryMode_ = file->display.auxiliaryMode;
        displayedFft_ = file->display.stftSize; displayedColorScale_ = file->display.colorScale;
    }
    repaintChart();
}
void PlotWidget::finishWheel() {
    wheelTimer_.stop();
    if (!wheelBase_) return;
    const auto before = *wheelBase_; wheelBase_.reset();
    if (activeWheel == this) activeWheel.clear();
    if (session_.commitViewChange(before)) emit stateChanged();
}
void PlotWidget::cancelGesture(bool exitCreating) {
    finishWheel();
    if (gesture_) {
        const auto gesture = *gesture_; gesture_.reset();
        if (activeGesture == this) activeGesture.clear();
        session_.restoreSnapshot(gesture.before);
        for (auto& file : session_.project().files) if (file.metadata.id == gesture.before.fileId)
            if (auto* mark = findMark(file, gesture.markId)) mark->range = gesture.mark;
        releasing_ = true; if (mouseGrabber() == this) releaseMouse(); releasing_ = false;
        emit stateChanged();
    }
    if (exitCreating) creating_ = false;
    hoverZone_ = Zone::None; hoveredMark_.clear();
    repaintChart();
}
void PlotWidget::updateHeatmap() {
    const auto* file = session_.activeFile();
    if (!file || kind_ != Kind::Main) return;
    // Root's cursor/status refresh can reach this before syncState when the
    // navigation or auxiliary pane changes the shared business view.
    if (file->metadata.id == displayedFile_ && displayedView_ && *displayedView_ != file->view) {
        beginPreview(); displayedView_ = file->view;
    }
    const auto rect = plotRect();
    // Settled size retains the A1.4.3 simulated matrix. Preview is bounded by
    // the physical pixel budget and a smaller total raster budget.
    const QSize pixels(static_cast<int>(std::lround(std::clamp(rect.width() * (preview_ ? .20 : .65), 120.0, preview_ ? 300.0 : 750.0))),
                       static_cast<int>(std::lround(std::clamp(rect.height() * (preview_ ? .20 : .8), 90.0, preview_ ? 180.0 : 430.0))));
    requestedHeatSize_ = pixels;
    const auto& view = file->view;
    const QString key = QString::fromStdString(file->metadata.id) + QString("/%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11/%12/%13")
        .arg(file->metadata.demoSeed).arg(static_cast<int>(file->display.mainMode)).arg(view.time.begin).arg(view.time.end)
        .arg(view.frequency.lowerHz, 0, 'g', 17).arg(view.frequency.upperHz, 0, 'g', 17).arg(pixels.width()).arg(pixels.height())
        .arg(file->metadata.sampleRateHz, 0, 'g', 17).arg(file->metadata.centerFrequencyHz, 0, 'g', 17).arg(file->metadata.sampleCount)
        .arg(file->metadata.path.c_str()).arg(file->display.stftSize);
    if (key != requestedPowerKey_) {
        requestedPowerKey_ = key; ++renderGeneration_; ++heatmapRequestGeneration_;
        if (worker_) worker_->cancel(heatmapRequestGeneration_);
        const auto cached = std::find_if(powerCache_.begin(), powerCache_.end(), [&](const auto& item) { return item->key == key; });
        if (cached != powerCache_.end()) {
            ++heatmapCacheHits_; renderedPower_ = *cached; powerKey_ = key; heatSize_ = pixels; colorKey_.clear();
            committedGeneration_ = renderGeneration_; acceptedPreview_ = preview_;
        } else {
            auto request = std::make_shared<HeatmapPayload>(); request->key = key; request->metadata = file->metadata;
            request->view = view; request->mode = file->display.mainMode; request->pixels = pixels;
            request->generation = heatmapRequestGeneration_; request->preview = preview_; request->fftSize = file->display.stftSize;
            // Same-view reductions inspect every cached source cell. A narrow
            // ridge is retained rather than skipped by point sampling.
            const auto source = std::find_if(powerCache_.begin(), powerCache_.end(), [&](const auto& item) {
                return item->view == view && item->metadata.id == file->metadata.id && item->mode == file->display.mainMode &&
                    item->metadata.sampleRateHz == file->metadata.sampleRateHz && item->metadata.centerFrequencyHz == file->metadata.centerFrequencyHz &&
                    item->metadata.sampleCount == file->metadata.sampleCount && item->metadata.demoSeed == file->metadata.demoSeed &&
                    item->metadata.path == file->metadata.path && item->fftSize == file->display.stftSize &&
                    item->pixels.width() >= pixels.width() && item->pixels.height() >= pixels.height();
            });
            if (source != powerCache_.end()) {
                QElapsedTimer timer; timer.start();
                request->power = display::peakReduce2D((*source)->power, (*source)->pixels.width(), (*source)->pixels.height(), pixels.width(), pixels.height());
                request->sourcePoints = (*source)->power.size(); request->elapsedMs = timer.nsecsElapsed() / 1e6;
                ++matrixReductions_; acceptHeatmap(std::move(request));
            } else { ++powerGenerations_; worker_->submit(std::move(request)); }
        }
    }
    if (!renderedPower_) return;
    const QString colors = QString("%1/%2/%3").arg(static_cast<int>(file->display.palette)).arg(file->display.dynamicRangeDb, 0, 'g', 17).arg(file->display.referenceLevelDb, 0, 'g', 17);
    if (colors == colorKey_) return;
    ++colorTransformGenerations_;
    colorKey_ = colors; heatmap_ = QImage(heatSize_, QImage::Format_Indexed8);
    QList<QRgb> table; table.reserve(256);
    for (int i = 0; i < 256; ++i) table.push_back(color(i / 255.0f, file->display.palette));
    heatmap_.setColorTable(table);
    for (int y = 0; y < heatSize_.height(); ++y) {
        auto* row = heatmap_.scanLine(y);
        for (int x = 0; x < heatSize_.width(); ++x) {
            const double power = renderedPower_->power[static_cast<std::size_t>(y) * heatSize_.width() + x];
            const double level = file->metadata.demo ? power * 80 / file->display.dynamicRangeDb + file->display.referenceLevelDb / 140 :
                (power - (file->display.referenceLevelDb - file->display.dynamicRangeDb)) / file->display.dynamicRangeDb;
            row[x] = static_cast<uchar>(std::lround(std::clamp(level, 0.0, 1.0) * 255));
        }
    }
}
void PlotWidget::acceptHeatmap(std::shared_ptr<HeatmapPayload> result) {
    if (result->generation != heatmapRequestGeneration_ || result->key != requestedPowerKey_) { ++staleResults_; return; }
    renderedPower_ = result; powerKey_ = result->key; heatSize_ = result->pixels; colorKey_.clear();
    committedGeneration_ = renderGeneration_; acceptedPreview_ = preview_;
    powerCache_.push_front(std::move(result)); if (powerCache_.size() > 4) powerCache_.pop_back();
    repaintChart();
}
std::pair<QRectF, QRectF> PlotWidget::heatmapPlacement() const {
    const auto* file = session_.activeFile();
    if (!file || !renderedPower_ || heatmap_.isNull() || renderedPower_->metadata.id != file->metadata.id || renderedPower_->mode != file->display.mainMode) return {};
    const auto mapped = markRect(renderedPower_->view); const auto target = mapped.intersected(plotRect());
    if (target.isEmpty() || mapped.isEmpty()) return {};
    const QRectF uv((target.left() - mapped.left()) / mapped.width(), (target.top() - mapped.top()) / mapped.height(), target.width() / mapped.width(), target.height() / mapped.height());
    return {target, uv};
}
qsizetype PlotWidget::drawnPointCount() const {
    return kind_ == Kind::Main ? renderedPower_ ? static_cast<qsizetype>(renderedPower_->power.size()) : 0 :
        kind_ == Kind::Auxiliary ? static_cast<qsizetype>(curveTrace_.size()) : (width() - 26) * 2;
}
qsizetype PlotWidget::sourcePointCount() const { return kind_ == Kind::Auxiliary ? static_cast<qsizetype>(curveSource_.size()) :
    kind_ == Kind::Main && renderedPower_ ? static_cast<qsizetype>(renderedPower_->sourcePoints) : drawnPointCount(); }
QString PlotWidget::renderQuality() const { return preview_ ? "preview" : "settled"; }
bool PlotWidget::isDisplaySettled() const {
    if (preview_ || (surface_ && !softwareFallback_ && surface_->hasPendingUploads())) return false;
    const auto* file = session_.activeFile();
    if (!file) return true;
    if (kind_ == Kind::Main)
        return renderedPower_ && committedGeneration_ == renderGeneration_ && worker_->idle() && renderedPower_->key == requestedPowerKey_ &&
            renderedPower_->metadata.id == file->metadata.id && renderedPower_->mode == file->display.mainMode;
    if (kind_ == Kind::Auxiliary && !file->metadata.demo) return curveCompleted_ && !curvePending_;
    if (kind_ == Kind::Navigation && !file->metadata.demo) return navigationCurveCompleted_ && !navigationCurvePending_;
    return true;
}
QJsonObject PlotWidget::renderStatistics() const {
    return {{"quality", renderQuality()}, {"settled", isDisplaySettled()}, {"generation", static_cast<qint64>(renderGeneration_)},
        {"requestedGeneration", static_cast<qint64>(renderGeneration_)}, {"committedGeneration", static_cast<qint64>(committedGeneration_)},
        {"workerIdle", !worker_ || worker_->idle()}, {"acceptedHeatmapPreview", acceptedPreview_},
        {"gpuUploadsPending", surface_ && !softwareFallback_ && surface_->hasPendingUploads()},
        {"requestedHeatmapWidth", requestedHeatSize_.width()}, {"requestedHeatmapHeight", requestedHeatSize_.height()},
        {"requestedPixels", static_cast<qint64>(requestedHeatSize_.width()) * requestedHeatSize_.height()},
        {"drawnPoints", static_cast<qint64>(drawnPointCount())}, {"sourcePoints", static_cast<qint64>(sourcePointCount())},
        {"powerGenerations", static_cast<qint64>(powerGenerations_)}, {"colorTransformGenerations", static_cast<qint64>(colorTransformGenerations_)},
        {"curveSourceGenerations", static_cast<qint64>(curveGenerations_)},
        {"curvePathElements", curvePath_.elementCount()},
        {"drawnLineSegments", static_cast<qint64>(kind_ == Kind::Auxiliary ? curveSegments_.size() :
            kind_ == Kind::Navigation ? navigationSegments_[0].size() + navigationSegments_[1].size() : 0)},
        {"curveRasterMethod", kind_ == Kind::Main ? "none" : "QPainter::drawLines cached adjacent QLineF segments"},
        {"curveReductions", static_cast<qint64>(curveReductions_)}, {"curveCacheHits", static_cast<qint64>(curveCacheHits_)},
        {"heatmapCacheHits", static_cast<qint64>(heatmapCacheHits_)}, {"staleDrops", static_cast<qint64>(staleResults_ + (worker_ ? worker_->discarded() : 0))},
        {"matrixReductions", static_cast<qint64>(matrixReductions_)},
        {"cpuWallTimings", surface_ ? surface_->cpuWallTimings() : QJsonObject{}},
        {"heatmapWidth", heatSize_.width()}, {"heatmapHeight", heatSize_.height()},
        {"lastMatrixMs", renderedPower_ ? renderedPower_->elapsedMs : 0.0}, {"textureUploads", static_cast<qint64>(textureUploadCount())},
        {"overlayUploads", static_cast<qint64>(surface_ ? surface_->overlayUploadCount() : 0)}};
}
void PlotWidget::updateCurve() {
    const auto* file = session_.activeFile(); if (!file || kind_ != Kind::Auxiliary) return;
    const auto plot = plotRect(); const auto& view = file->view;
    const bool psd = file->display.auxiliaryMode == AuxiliaryMode::Psd;
    const double duration = seconds(file->metadata.sampleCount, file->metadata.sampleRateHz);
    const double timeStart = seconds(view.time.begin, file->metadata.sampleRateHz), timeSpan = seconds(view.time.end - view.time.begin, file->metadata.sampleRateHz);
    const double frequencySpan = view.frequency.upperHz - view.frequency.lowerHz;
    TimeRange calculationTime = view.time;
    if (psd && file->display.psdFromSelection) {
        if (const auto* mark = findMark(*file, file->activeMarkId)) calculationTime = mark->range.time;
    }
    const int seed = file->metadata.demoSeed;
    const int dense = file->metadata.demo ? std::clamp(static_cast<int>(std::lround(plot.width() * 12)), 4096, 65536) :
        std::clamp(static_cast<int>(std::lround(plot.width() * 2)), 512, 8192);
    const auto filePath = QString::fromUtf8(file->metadata.path.data(), static_cast<qsizetype>(file->metadata.path.size()));
    const QString sourceKey = QString::fromStdString(file->metadata.id) + QString("/%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11/%12/%13")
        .arg(seed).arg(psd).arg(psd ? calculationTime.begin : view.time.begin).arg(psd ? calculationTime.end : view.time.end)
        .arg(psd ? view.frequency.lowerHz : 0, 0, 'g', 17).arg(psd ? view.frequency.upperHz : 0, 0, 'g', 17)
        .arg(file->metadata.sampleRateHz, 0, 'g', 17).arg(duration, 0, 'g', 17).arg(dense).arg(file->metadata.centerFrequencyHz, 0, 'g', 17)
        .arg(filePath).arg(psd ? file->display.psdSize : 0).arg(file->display.psdFromSelection)
        + QString("/%1").arg(static_cast<int>(file->display.waveformMode));
    if (curveSourceKey_ != sourceKey) {
        curveSourceKey_ = sourceKey; ++curveGenerations_; ++renderGeneration_; curveSource_.clear(); curveTrace_.clear();
        curveSegments_.clear(); curvePath_ = {}; curvePathKey_.clear(); curveReductionKey_.clear(); curvePending_ = false;
        curveCompleted_ = false; curveError_.clear();
        if (!file->metadata.demo) {
            CurveWorker::Request request;
            request.key = sourceKey; request.metadata = file->metadata; request.time = calculationTime;
            request.frequency = view.frequency; request.psd = psd; request.fftSize = file->display.psdSize;
            request.points = dense; request.waveformMode = file->display.waveformMode; request.generation = ++curveRequestGeneration_;
            curvePending_ = true;
            curveWorker_->submit(std::move(request));
            return;
        }
        curveSource_.resize(dense);
        for (int i = 0; i < dense; ++i) {
            const double u = static_cast<double>(i) / (dense - 1), time = timeStart + u * timeSpan;
            const double frequency = view.frequency.lowerHz + u * frequencySpan;
            double value;
            if (psd) {
                const auto peak = [&](double center, double spread, double height) { return height * std::exp(-std::pow((frequency - center) / spread, 2)); };
                value = -88 + peak(file->metadata.centerFrequencyHz - file->metadata.sampleRateHz * .18, file->metadata.sampleRateHz * .017, 45) +
                    peak(file->metadata.centerFrequencyHz + file->metadata.sampleRateHz * .07, file->metadata.sampleRateHz * .03, 62) +
                    peak(file->metadata.centerFrequencyHz + file->metadata.sampleRateHz * .29, file->metadata.sampleRateHz * .012, 34) + (randomValue(i * .7 + seed) - .5) * 6;
            } else {
                const auto peak = [&](double center, double spread) { return std::exp(-std::pow((time - center) / spread, 2)); };
                const double envelope = 3 + 36 * peak(duration * .47, duration * .024) + 22 * peak(duration * .65, duration * .04) + 15 * peak(duration * .21, duration * .028);
                value = std::sin(time * (30 + seed * 5)) * envelope - std::sin(time * 49 + seed) * 2;
            }
            curveSource_[i] = static_cast<float>(value);
        }
        curveReductionKey_.clear();
    } else {
        ++curveCacheHits_;
        if (!file->metadata.demo && (curvePending_ || !curveCompleted_)) return;
    }
    const auto columns = static_cast<std::size_t>(std::max(1.0, std::floor(plot.width() * devicePixelRatioF() * (preview_ ? .5 : 1))));
    const auto reductionKey = sourceKey + "/" + QString::number(columns);
    if (curveReductionKey_ != reductionKey) {
        curveReductionKey_ = reductionKey; ++curveReductions_; ++renderGeneration_;
        curveTrace_ = display::extremaEnvelope(curveSource_, columns); curvePathKey_.clear();
    }
    const auto pathKey = reductionKey + QString("/%1/%2/%3/%4").arg(plot.width()).arg(plot.height())
        .arg(file->display.auxiliaryMin, 0, 'g', 17).arg(file->display.auxiliaryMax, 0, 'g', 17);
    if (curvePathKey_ != pathKey) {
        curvePathKey_ = pathKey; curvePath_ = {};
        curveSegments_.clear();
        curveSegments_.reserve(curveTrace_.empty() ? 0 : curveTrace_.size() - 1);
        QPointF previous;
        for (const auto& point : curveTrace_) {
            const double u = static_cast<double>(point.index) / (curveSource_.size() - 1);
            const QPointF pixel(plot.left() + u * plot.width(), plot.top() + (file->display.auxiliaryMax - point.value) / (file->display.auxiliaryMax - file->display.auxiliaryMin) * plot.height());
            if (curvePath_.elementCount() == 0) curvePath_.moveTo(pixel);
            else { curvePath_.lineTo(pixel); curveSegments_.emplace_back(previous, pixel); }
            previous = pixel;
        }
        committedGeneration_ = renderGeneration_; acceptedPreview_ = preview_;
    }
}

void PlotWidget::acceptCurve(std::shared_ptr<CurvePayload> result) {
    if (!result || result->generation != curveRequestGeneration_ || result->key != curveSourceKey_) return;
    curvePending_ = false; curveCompleted_ = true; curveError_ = result->error;
    curveSource_ = std::move(result->samples); curveReductionKey_.clear(); curvePathKey_.clear();
    ++curveGenerations_; ++renderGeneration_;
    repaintChart();
}

void PlotWidget::acceptNavigationCurve(std::shared_ptr<CurvePayload> result) {
    if (!result || result->generation != navigationRequestGeneration_ || result->key != navigationCurveKey_) return;
    navigationCurvePending_ = false; navigationCurveCompleted_ = true; navigationCurveError_ = result->error;
    navigationCurve_ = std::move(result->samples); navigationKey_.clear();
    ++curveGenerations_; ++renderGeneration_;
    repaintChart();
}

void PlotWidget::paintEvent(QPaintEvent*) {
    if (surface_ && !softwareFallback_) return;
    QPainter painter(this); painter.fillRect(rect(), QColor("#0a1728")); paintScene(painter, false);
}
void PlotWidget::paintScene(QPainter& painter, bool accelerated) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    auto* file = session_.activeFile();
    if (!file) return;
    const auto plot = plotRect(); const auto& view = file->view;
    const double duration = seconds(file->metadata.sampleCount, file->metadata.sampleRateHz);
    const bool waterfall = kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall;
    const bool psd = kind_ == Kind::Auxiliary && file->display.auxiliaryMode == AuxiliaryMode::Psd;
    const double timeStart = seconds(view.time.begin, file->metadata.sampleRateHz);
    const double timeSpan = seconds(view.time.end - view.time.begin, file->metadata.sampleRateHz);
    const double frequencySpan = view.frequency.upperHz - view.frequency.lowerHz;
    const int seed = file->metadata.demoSeed;
    if (kind_ == Kind::Navigation) {
        painter.fillRect(rect(), QColor("#091b2b"));
        const auto filePath = QString::fromUtf8(file->metadata.path.data(), static_cast<qsizetype>(file->metadata.path.size()));
        const auto navigationKey = QString::fromStdString(file->metadata.id) + QString("/%1/%2/%3/%4").arg(seed).arg(width()).arg(height()).arg(filePath);
        if (navigationKey_ != navigationKey) {
            navigationKey_ = navigationKey; ++curveGenerations_; ++renderGeneration_;
            if (!file->metadata.demo && navigationCurveKey_ != navigationKey) {
                navigationCurveKey_ = navigationKey; navigationCurve_.clear(); navigationCurveError_.clear();
                navigationCurvePending_ = true; navigationCurveCompleted_ = false;
                CurveWorker::Request request;
                request.key = navigationKey; request.metadata = file->metadata; request.time = {0, file->metadata.sampleCount};
                request.points = std::max(1, width() - 26); request.generation = ++navigationRequestGeneration_;
                navigationWorker_->submit(std::move(request));
            }
            for (int direction = 0; direction < 2; ++direction) {
                auto& path = navigationPaths_[direction]; path = {};
                auto& segments = navigationSegments_[direction]; segments.clear();
                if (!file->metadata.demo && navigationCurve_.empty()) continue;
                segments.reserve(static_cast<std::size_t>(std::max(0, width() - 27)));
                QPointF previous;
                for (int x = 13; x < width() - 13; ++x) {
                    const double u = (x - 13) / plot.width();
                    double envelope = .1 + .3 * std::exp(-std::pow((u - .25) * 6, 2)) + .28 * std::exp(-std::pow((u - .58) * 9, 2)) + .23 * std::exp(-std::pow((u - .8) * 13, 2));
                    if (!file->metadata.demo)
                        envelope = std::clamp((navigationCurve_[static_cast<std::size_t>(x - 13)] + 80) / 80.0, .02, 1.0);
                    const double centerY = file->metadata.demo ? height() * .43 : height() * .5;
                    const QPointF point(x, centerY + (direction ? 1 : -1) * envelope * height() * .42);
                    if (x == 13) path.moveTo(point);
                    else { path.lineTo(point); segments.emplace_back(previous, point); }
                    previous = point;
                }
            }
            committedGeneration_ = renderGeneration_;
        } else ++curveCacheHits_;
        painter.setPen(QPen(QColor("#4fb4e1"), 1));
        for (const auto& segments : navigationSegments_)
            if (!segments.empty()) painter.drawLines(segments.data(), static_cast<int>(segments.size()));
        if (!file->metadata.demo && navigationCurvePending_) {
            painter.setFont(canvasFont()); painter.setPen(QColor("#b5d9ec"));
            painter.drawText(QPointF(15, 15), "正在读取 IQ 导航预览…");
        } else if (!navigationCurveError_.isEmpty()) {
            painter.setFont(canvasFont()); painter.setPen(QColor("#edb6a0"));
            painter.drawText(QPointF(15, 15), "IQ 导航预览失败 · " + navigationCurveError_);
        }
        const auto window = navigationWindow();
        painter.fillRect(window, cssColor("#38b6e128")); painter.setPen(QColor("#8ed9ff"));
        painter.drawRect(QRectF(window.left() + .5, .5, window.width(), std::max(0, height() - 1)));
        painter.setFont(canvasFont()); painter.setPen(QColor("#a5bed4"));
        for (int i = 0; i <= 6; ++i) painter.drawText(QPointF(13 + i * plot.width() / 6 - 10, height() - 3), coordinateText(false, i * duration / 6));
        return;
    }
    if (kind_ == Kind::Main && !accelerated) {
        updateHeatmap(); const auto [target, source] = heatmapPlacement();
        if (!target.isEmpty()) painter.drawImage(target, heatmap_, QRectF(source.x() * heatmap_.width(), source.y() * heatmap_.height(), source.width() * heatmap_.width(), source.height() * heatmap_.height()));
    }
    if (file->display.grid) {
        painter.setPen(QPen(kind_ == Kind::Main ? cssColor("#b9dfff20") : cssColor("#31466177"), 1));
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
        updateCurve(); painter.save(); painter.setClipRect(plot);
        // Adjacent endpoints and the original square caps keep the thin trace
        // connected. drawLines selects Qt's LinesHint raster route instead of
        // constructing/stroking one outline for the entire dense curve. The
        // diagnostic QPainterPath retains the exact same ordered vertices.
        const QColor waveformColor = file->display.waveformMode == WaveformMode::I ? QColor("#51d7c5") :
            file->display.waveformMode == WaveformMode::Q ? QColor("#f1aa63") : QColor("#51d7c5");
        painter.setPen(QPen(psd ? QColor("#5abffa") : waveformColor, 1.5));
        if (!curveSegments_.empty()) painter.drawLines(curveSegments_.data(), static_cast<int>(curveSegments_.size()));
        if (gesture_ && gesture_->tool == Tool::AuxiliaryZoom) {
            const double left = std::clamp(std::min(gesture_->start.x(), gesture_->current.x()), plot.left(), plot.right());
            const double right = std::clamp(std::max(gesture_->start.x(), gesture_->current.x()), plot.left(), plot.right());
            painter.setPen(QPen(QColor("#9ceaff"), 1, Qt::DashLine)); painter.setBrush(cssColor("#7ed3f02a"));
            painter.drawRect(QRectF(left, plot.top(), right - left, plot.height()));
        }
        painter.restore();
    }
    painter.setFont(canvasFont()); painter.setPen(QColor("#aac2d7"));
    const double frequencyOffset = file->display.absoluteFrequency ? 0 : file->metadata.centerFrequencyHz;
    const Axis x = waterfall || psd ? axisScale(true, view.frequency.lowerHz, frequencySpan, 5, frequencyOffset) : axisScale(false, timeStart, timeSpan, 5);
    const Axis y = kind_ == Kind::Main ? waterfall ? axisScale(false, timeStart + timeSpan, -timeSpan, 4) : axisScale(true, view.frequency.lowerHz, frequencySpan, 4, frequencyOffset) : Axis{};
    const QFontMetricsF metrics(painter.font());
    double previousRight = -std::numeric_limits<double>::infinity();
    for (int i = 0; i <= x.count; ++i) {
        const auto label = x.tick(static_cast<double>(i) / x.count);
        const double width = metrics.horizontalAdvance(label), position = plot.left() + plot.width() * i / x.count;
        const double left = std::clamp(position - width / 2, plot.left() - 3, std::max(plot.left() - 3, plot.right() - width + 3));
        if (left >= previousRight + 8) { painter.drawText(QPointF(left, plot.bottom() + 14), label); previousRight = left + width; }
    }
    double previousTop = std::numeric_limits<double>::infinity();
    for (int i = 0; i <= 4; ++i) {
        const double position = plot.bottom() - plot.height() * i / 4;
        const auto label = kind_ == Kind::Main ? y.tick(i / 4.0) : QString::number(file->display.auxiliaryMin + (file->display.auxiliaryMax - file->display.auxiliaryMin) * i / 4, 'f', psd ? 0 : 1);
        if (position + 3 <= previousTop - 12) {
            painter.drawText(QPointF(plot.left() - 7 - metrics.horizontalAdvance(label), position + 3), label); previousTop = position + 3;
        }
    }
    painter.drawText(QRectF(plot.left(), height() - 15, plot.width(), 14), Qt::AlignHCenter | Qt::AlignBottom, x.label);
    painter.save(); painter.translate(15, plot.center().y()); painter.rotate(-90);
    const QString verticalTitle = kind_ == Kind::Main ? y.label : psd ? "PSD (dBFS/Hz)" : file->metadata.demo ? "幅度（演示）" : "IQ RMS (dBFS)";
    painter.drawText(QPointF(-metrics.horizontalAdvance(verticalTitle) / 2, 0), verticalTitle);
    painter.restore();
    int originRow = 0;
    for (const auto& label : {x.originLabel, y.originLabel}) if (!label.isEmpty()) {
        const double top = plot.top() + 13 + originRow++ * 15;
        painter.fillRect(QRectF(plot.left() + 4, top - 10, metrics.horizontalAdvance(label) + 10, 14), cssColor("#071628da"));
        painter.setPen(QColor("#d4edff")); painter.drawText(QPointF(plot.left() + 9, top), label);
    }
    if (hoverZone_ == Zone::XAxis || hoverZone_ == Zone::YAxis) {
        const QRectF hover = hoverZone_ == Zone::XAxis ? QRectF(plot.left(), plot.bottom(), plot.width(), height() - plot.bottom()) : QRectF(0, plot.top(), plot.left(), plot.height());
        painter.fillRect(hover, cssColor("#2e94cb26")); painter.setPen(QPen(cssColor("#6ac5eacc"), 1.5));
        painter.drawLine(hoverZone_ == Zone::XAxis ? QLineF(plot.bottomLeft(), plot.bottomRight()) : QLineF(plot.topLeft(), plot.bottomLeft()));
    }
    if (kind_ == Kind::Auxiliary) {
        const auto tip = tipText(); painter.setFont(canvasFont());
        const QFontMetricsF fm(painter.font()); const double width = std::min(static_cast<double>(this->width() - 24), std::ceil(fm.horizontalAdvance(tip)) + 12);
        const QRectF box(this->width() - 12 - width, 7, width, fm.height() + 6);
        painter.setPen(Qt::NoPen); painter.setBrush(cssColor("#091728c7")); painter.drawRoundedRect(box, 3, 3);
        painter.setPen(QColor("#c8dfef")); painter.drawText(box.adjusted(6, 0, -6, 0), Qt::AlignCenter, fm.elidedText(tip, Qt::ElideRight, static_cast<int>(width - 12)));
        if (!file->metadata.demo && (curvePending_ || !curveError_.isEmpty())) {
            const QString status = !curveError_.isEmpty() ? "IQ 错误 · " + curveError_ : psd ? "正在计算真实 IQ PSD…" : "正在计算真实 IQ 波形…";
            const double statusWidth = std::min(static_cast<double>(this->width() - 24), std::ceil(fm.horizontalAdvance(status)) + 12);
            const QRectF statusBox(plot.left() + 8, plot.top() + 8, statusWidth, fm.height() + 6);
            painter.fillRect(statusBox, cssColor("#091728d8")); painter.setPen(QColor("#d5e7f5"));
            painter.drawText(statusBox.adjusted(6, 0, -6, 0), Qt::AlignCenter, fm.elidedText(status, Qt::ElideRight, static_cast<int>(statusWidth - 12)));
        }
        return;
    }
    if (file->display.colorScale) {
        const double left = width() - 38, top = plot.top() + 4, span = std::max(1.0, plot.height() - 8);
        for (int i = 0; i < span; ++i) painter.fillRect(QRectF(left, top + i, 13, 1), QColor::fromRgb(color(static_cast<float>(1 - i / span), file->display.palette)));
        painter.setPen(QColor("#8197b1")); painter.setBrush(Qt::NoBrush); painter.drawRect(QRectF(left, top, 13, span));
        painter.setFont(canvasFont()); painter.setPen(QColor("#bad5eb"));
        for (int i = 0; i <= 4; ++i) painter.drawText(QPointF(left + 17, top + i * span / 4 + 4), numberText(file->display.referenceLevelDb - std::round(file->display.dynamicRangeDb * i / 4), 0));
    }
    painter.save(); painter.setClipRect(plot); painter.setFont(canvasFont(11, true));
    for (const auto& mark : file->marks) {
        const auto box = markRect(mark.range); if (box.isEmpty() || !box.intersects(plot)) continue;
        const bool selected = std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(), mark.id) != file->selectedMarkIds.end();
        const bool hovered = hoveredMark_ == mark.id && !creating_;
        if (hovered) {
            painter.setBrush(Qt::NoBrush);
            for (const int thickness : {10, 6}) { painter.setPen(QPen(QColor(230, 251, 255, thickness == 10 ? 12 : 24), thickness)); painter.drawRect(box); }
        }
        painter.setBrush(hovered ? cssColor("#58dbff43") : selected ? cssColor("#f3ce4c26") : cssColor("#b4c5d015"));
        QPen pen(QColor(hovered ? "#b8f5ff" : selected ? "#f5d86c" : "#8baec4"), hovered ? 2.6 : selected ? 1.6 : 1);
        if (!selected && !hovered) pen.setDashPattern({5, 3}); painter.setPen(pen); painter.drawRect(box);
        if (hovered) {
            const auto label = QString::fromStdString(mark.name) + " · 点击选中";
            const double labelWidth = std::ceil(QFontMetricsF(painter.font()).horizontalAdvance(label)) + 16;
            const double left = std::clamp(box.left() + 4, plot.left() + 3, std::max(plot.left() + 3, plot.right() - labelWidth - 3));
            const double top = std::clamp(box.top() + 4, plot.top() + 3, std::max(plot.top() + 3, plot.bottom() - 21));
            painter.fillRect(QRectF(left, top, labelWidth, 19), QColor("#10374f")); painter.setPen(QColor("#95e3ff")); painter.setBrush(Qt::NoBrush); painter.drawRect(QRectF(left + .5, top + .5, labelWidth - 1, 18));
            painter.setPen(QColor("#e2f9ff")); painter.drawText(QPointF(left + 8, top + 13), label);
        }
        if (selected && !creating_) for (const auto& [point, edge] : handles(box)) {
            Q_UNUSED(edge); if (!plot.contains(point)) continue;
            const QRectF handle(point - QPointF(3, 3), QSizeF(6, 6));
            painter.fillRect(handle, QColor(mark.id == file->activeMarkId ? "#f7df7f" : "#b6dce9")); painter.setPen(QColor("#344653")); painter.setBrush(Qt::NoBrush); painter.drawRect(handle);
        }
    }
    if (gesture_ && (gesture_->tool == Tool::ZoomBox || gesture_->tool == Tool::CreateMark)) {
        const bool mark = gesture_->tool == Tool::CreateMark;
        const auto box = QRectF(gesture_->start, gesture_->current).normalized().intersected(plot);
        QPen pen(QColor(mark ? "#f4cc70" : "#9ceaff"), 1); pen.setDashPattern({4, 3}); painter.setPen(pen);
        painter.setBrush(mark ? cssColor("#f0ca5c27") : cssColor("#7ed3f026")); painter.drawRect(box);
    }
    if (cursorSample_ >= view.time.begin && cursorSample_ <= view.time.end && cursorFrequency_ >= view.frequency.lowerHz && cursorFrequency_ <= view.frequency.upperHz) {
        const auto point = toPixel(cursorSample_, cursorFrequency_); QPen pen(cssColor("#f0dd80a3"), 1); pen.setDashPattern({2, 5});
        painter.setPen(pen); painter.drawLine(QPointF(point.x(), plot.top()), QPointF(point.x(), plot.bottom())); painter.drawLine(QPointF(plot.left(), point.y()), QPointF(plot.right(), point.y()));
    }
    painter.restore();
    if (findMark(*file, file->activeMarkId)) drawPill(painter, rect(), "信号标记已保存（当前文件）", false);
    if (creating_) drawPill(painter, rect(), "持续选择信号 · 拖动创建标记 · 右键菜单关闭 / Esc 退出", true);
    if ((!renderedPower_ || renderedPower_->key != requestedPowerKey_) || (renderedPower_ && !renderedPower_->error.isEmpty())) {
        const QString text = renderedPower_ && !renderedPower_->error.isEmpty() ? "IQ 图谱错误 · " + renderedPower_->error : "正在更新图谱…"; painter.setFont(canvasFont(10, true));
        const QFontMetricsF fm(painter.font()); const double labelWidth = std::ceil(fm.horizontalAdvance(text)) + 16;
        const QRectF label(plot.right() - labelWidth - 6, plot.bottom() - 25, labelWidth, 19);
        painter.setPen(Qt::NoPen); painter.setBrush(cssColor("#091728dd")); painter.drawRoundedRect(label, 3, 3);
        painter.setPen(QColor("#b5d9ec")); painter.drawText(label, Qt::AlignCenter, text);
    }
}

std::vector<std::string> PlotWidget::marksAt(QPointF point) const {
    std::vector<std::string> ids; const auto* file = session_.activeFile();
    if (!file || creating_ || kind_ != Kind::Main || zoneAt(point) != Zone::Plot) return ids;
    for (auto it = file->marks.rbegin(); it != file->marks.rend(); ++it) {
        const auto clip = markRect(it->range).intersected(plotRect());
        if (!clip.isEmpty() && clip.contains(point)) ids.push_back(it->id);
    }
    return ids;
}
PlotWidget::Hit PlotWidget::hitMark(QPointF point) const {
    const auto* file = session_.activeFile(); const auto plot = plotRect();
    if (!file || creating_ || kind_ != Kind::Main || zoneAt(point) != Zone::Plot) return {};
    std::vector<const Mark*> selected;
    for (auto it = file->marks.rbegin(); it != file->marks.rend(); ++it)
        if (std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(), it->id) != file->selectedMarkIds.end()) selected.push_back(&*it);
    for (const auto* mark : selected) {
        const auto box = markRect(mark->range), clip = box.intersected(plot); if (clip.isEmpty()) continue;
        int bestEdge = 0; double bestDistance = std::numeric_limits<double>::infinity();
        for (const auto& [handle, edge] : handles(box)) if (plot.contains(handle) && std::abs(point.x() - handle.x()) <= 6 && std::abs(point.y() - handle.y()) <= 6) {
            const double distance = std::pow(point.x() - handle.x(), 2) + std::pow(point.y() - handle.y(), 2);
            const bool corner = (edge & (Left | Right)) && (edge & (Top | Bottom));
            if (distance < bestDistance || (distance == bestDistance && corner)) { bestEdge = edge; bestDistance = distance; }
        }
        if (bestEdge) return {mark->id, bestEdge, true};
        const auto consider = [&](int edge, double coordinate, double actual, bool visible, bool inSpan) {
            const double distance = std::abs(coordinate - actual);
            if (visible && inSpan && distance <= 6 && distance < bestDistance) { bestDistance = distance; bestEdge = edge; }
        };
        consider(Left, point.x(), box.left(), box.left() >= plot.left() && box.left() <= plot.right(), point.y() >= clip.top() && point.y() <= clip.bottom());
        consider(Right, point.x(), box.right(), box.right() >= plot.left() && box.right() <= plot.right(), point.y() >= clip.top() && point.y() <= clip.bottom());
        consider(Top, point.y(), box.top(), box.top() >= plot.top() && box.top() <= plot.bottom(), point.x() >= clip.left() && point.x() <= clip.right());
        consider(Bottom, point.y(), box.bottom(), box.bottom() >= plot.top() && box.bottom() <= plot.bottom(), point.x() >= clip.left() && point.x() <= clip.right());
        if (bestEdge) return {mark->id, bestEdge, true};
        if (clip.contains(point)) return {mark->id, Move, true};
    }
    const auto ids = marksAt(point); return ids.empty() ? Hit{} : Hit{ids.front(), 0, false};
}
void PlotWidget::updateHover(QPointF point) {
    const auto* file = session_.activeFile(); if (!file) { setCursor(Qt::ArrowCursor); return; }
    hoverZone_ = zoneAt(point);
    if (kind_ == Kind::Navigation) {
        setCursor(gesture_ && gesture_->navigationInside && gesture_->changed ? Qt::ClosedHandCursor : navigationWindow().contains(point) ? Qt::OpenHandCursor : Qt::PointingHandCursor);
        emit interactionHint("全局导航：滚轮缩放 · 拖动范围平移 · 单击跳转"); return;
    }
    const auto hit = gesture_ ? Hit{} : hitMark(point);
    const auto underPointer = gesture_ ? std::vector<std::string>{} : marksAt(point);
    hoveredMark_ = underPointer.empty() ? std::string{} : underPointer.front();
    if (gesture_) setCursor(gesture_->tool == Tool::MarkEdit ? gesture_->edges == Move ? Qt::ClosedHandCursor : edgeCursor(gesture_->edges) :
            gesture_->tool == Tool::PanTime || gesture_->tool == Tool::PanFrequency || gesture_->tool == Tool::AuxiliaryY ? Qt::ClosedHandCursor : Qt::CrossCursor);
    else if (creating_ && kind_ == Kind::Main) setCursor(hoverZone_ == Zone::Plot ? Qt::CrossCursor : Qt::ArrowCursor);
    else if (hit.selected) setCursor(edgeCursor(hit.edges));
    else if (hoverZone_ == Zone::XAxis || hoverZone_ == Zone::YAxis) setCursor(Qt::OpenHandCursor);
    else setCursor(!hit.id.empty() ? Qt::PointingHandCursor : hoverZone_ == Zone::Plot ? Qt::CrossCursor : Qt::ArrowCursor);
    QString hint;
    if (creating_ && kind_ == Kind::Main) hint = "持续选择信号：拖动创建 · 右键菜单关闭 · Esc 退出";
    else if (const auto* mark = findMark(*file, hit.id)) hint = QString::fromStdString(mark->name) + (hit.selected ? "：拖动移动 · 边线和顶点调整" : "：单击选中 · 拖动空白区域放大");
    else if (kind_ == Kind::Main) {
        const bool waterfall = file->display.mainMode == MainMode::Waterfall;
        const auto title = QString(waterfall ? "瀑布图" : "时频图");
        hint = hoverZone_ == Zone::YAxis ? title + " Y 轴：滚轮缩放" + (waterfall ? "时间" : "频率") + " · 按住拖动平移" :
            hoverZone_ == Zone::XAxis ? title + " X 轴：滚轮缩放" + (waterfall ? "频率" : "时间") + " · 按住拖动平移" :
            hoverZone_ == Zone::Plot ? title + "：滚轮缩放" + (waterfall ? "频率" : "时间") + " · 拖动矩形放大" : title + "：请将鼠标移入图内或坐标轴区域";
    } else hint = hoverZone_ == Zone::YAxis ? "辅助图谱 Y 轴：滚轮缩放幅度 · 拖动平移" :
        hoverZone_ == Zone::XAxis ? "辅助图谱 X 轴：滚轮缩放 · 拖动平移" : hoverZone_ == Zone::Plot ? "辅助图谱：滚轮缩放 X · 拖动范围放大" : "辅助图谱：移至图谱或坐标轴进行交互";
    emit interactionHint(hint);
}

void PlotWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        if (activeGesture && activeGesture != this && activeGesture->window() == window()) activeGesture->cancelGesture();
        cancelGesture(); event->accept(); return;
    }
    auto* file = session_.activeFile(); const auto point = event->position(); const auto zone = zoneAt(point);
    if (event->button() != Qt::LeftButton || !file || zone == Zone::None) return;
    if (creating_ && kind_ == Kind::Main && zone != Zone::Plot) return;
    if (activeGesture && activeGesture != this && activeGesture->window() == window()) activeGesture->cancelGesture();
    if (activeWheel && activeWheel != this && activeWheel->window() == window()) activeWheel->finishWheel();
    cancelGesture(); setFocus(); file = session_.activeFile();
    const auto hit = hitMark(point);
    Gesture gesture;
    gesture.before = session_.snapshot(); gesture.start = point; gesture.current = point; gesture.device = event->pointingDevice();
    gesture.mainMode = file->display.mainMode; gesture.auxiliaryMode = file->display.auxiliaryMode;
    gesture.clickMarkId = hit.id;
    if (kind_ == Kind::Navigation) { gesture.tool = Tool::Navigate; gesture.navigationInside = navigationWindow().contains(point); }
    else if (kind_ == Kind::Auxiliary) gesture.tool = zone == Zone::YAxis ? Tool::AuxiliaryY : zone == Zone::XAxis ?
        file->display.auxiliaryMode == AuxiliaryMode::Psd ? Tool::PanFrequency : Tool::PanTime : Tool::AuxiliaryZoom;
    else if (creating_) gesture.tool = Tool::CreateMark;
    else if (zone == Zone::YAxis || zone == Zone::XAxis) gesture.tool = (file->display.mainMode == MainMode::Waterfall ? zone == Zone::XAxis : zone == Zone::YAxis) ? Tool::PanFrequency : Tool::PanTime;
    else if (hit.selected) {
        gesture.tool = Tool::MarkEdit; gesture.markId = hit.id; gesture.edges = hit.edges;
        gesture.mark = findMark(*file, hit.id)->range; file->activeMarkId = hit.id;
    } else gesture.tool = Tool::ZoomBox;
    gesture_ = gesture; activeGesture = this; grabMouse(); updateHover(point); repaintChart(); event->accept();
}
void PlotWidget::mouseMoveEvent(QMouseEvent* event) {
    auto* file = session_.activeFile(); if (!file) return;
    const auto point = event->position();
    if (gesture_ && gesture_->device == event->pointingDevice() && gesture_->before.fileId == file->metadata.id &&
        (gesture_->tool == Tool::PanTime || gesture_->tool == Tool::PanFrequency || gesture_->tool == Tool::Navigate)) beginPreview();
    if (kind_ == Kind::Main) {
        const auto coordinate = fromPixel(point, file->view);
        cursorSample_ = offsetSample(file->view.time.begin, coordinate.sample, file->metadata.sampleCount); cursorFrequency_ = coordinate.frequency;
        emit cursorChanged(cursorSample_, cursorFrequency_);
    }
    if (!gesture_) { updateHover(point); repaintChart(); return; }
    auto& gesture = *gesture_;
    if (gesture.device != event->pointingDevice()) return;
    if (file->metadata.id != gesture.before.fileId || file->display.mainMode != gesture.mainMode || file->display.auxiliaryMode != gesture.auxiliaryMode) { cancelGesture(file->metadata.id != gesture.before.fileId); return; }
    const auto plot = plotRect(); gesture.current = QPointF(std::clamp(point.x(), plot.left(), plot.right()), std::clamp(point.y(), plot.top(), plot.bottom()));
    const double dx = point.x() - gesture.start.x(), dy = point.y() - gesture.start.y();
    const bool axisPan = gesture.tool == Tool::PanTime || gesture.tool == Tool::PanFrequency || gesture.tool == Tool::AuxiliaryY;
    const bool moved = gesture.tool == Tool::Navigate ? std::abs(dx) > 4 :
        std::max(std::abs(dx), std::abs(dy)) > (axisPan ? 0 : 5);
    if (!gesture.changed && !moved) { updateHover(point); repaintChart(); return; }
    gesture.changed = true;
    if (gesture.tool == Tool::AuxiliaryY) beginPreview();
    const auto& base = gesture.before.view;
    if (gesture.tool == Tool::MarkEdit) {
        auto* mark = findMark(*file, gesture.markId); if (!mark) { cancelGesture(); return; }
        const auto start = fromPixel(gesture.start, base), current = fromPixel(point, base);
        const long double dt = current.sample - start.sample; const double df = current.frequency - start.frequency;
        const auto bounds = fullRange(file->metadata); auto next = gesture.mark;
        if (gesture.edges == Move) {
            const auto span = gesture.mark.time.end - gesture.mark.time.begin;
            next.time.begin = offsetSample(gesture.mark.time.begin, dt, file->metadata.sampleCount - span); next.time.end = next.time.begin + span;
            const double width = gesture.mark.frequency.upperHz - gesture.mark.frequency.lowerHz;
            next.frequency.lowerHz = std::clamp(gesture.mark.frequency.lowerHz + df, bounds.frequency.lowerHz, bounds.frequency.upperHz - width); next.frequency.upperHz = next.frequency.lowerHz + width;
        } else {
            const bool waterfall = file->display.mainMode == MainMode::Waterfall;
            const auto minTime = std::min<SampleIndex>(file->display.stftSize, gesture.mark.time.end - gesture.mark.time.begin);
            const double minFrequency = std::min(file->metadata.sampleRateHz / file->display.stftSize, gesture.mark.frequency.upperHz - gesture.mark.frequency.lowerHz);
            if (gesture.edges & (waterfall ? Top : Left)) next.time.begin = offsetSample(gesture.mark.time.begin, dt, gesture.mark.time.end - minTime);
            if (gesture.edges & (waterfall ? Bottom : Right)) next.time.end = std::max(gesture.mark.time.begin + minTime, offsetSample(gesture.mark.time.end, dt, file->metadata.sampleCount));
            if (gesture.edges & (waterfall ? Left : Bottom)) next.frequency.lowerHz = std::clamp(gesture.mark.frequency.lowerHz + df, bounds.frequency.lowerHz, gesture.mark.frequency.upperHz - minFrequency);
            if (gesture.edges & (waterfall ? Right : Top)) next.frequency.upperHz = std::clamp(gesture.mark.frequency.upperHz + df, gesture.mark.frequency.lowerHz + minFrequency, bounds.frequency.upperHz);
        }
        mark->range = next;
    } else if (gesture.tool == Tool::AuxiliaryY) {
        const bool psd = file->display.auxiliaryMode == AuxiliaryMode::Psd;
        const double low = psd ? gesture.before.psdMin : gesture.before.waveformMin;
        const double high = psd ? gesture.before.psdMax : gesture.before.waveformMax;
        const double delta = dy / plot.height() * (high - low); session_.setAuxiliaryRange(low + delta, high + delta, false);
    } else if (gesture.tool == Tool::PanTime || gesture.tool == Tool::PanFrequency || gesture.tool == Tool::Navigate) {
        auto next = base;
        if (gesture.tool == Tool::Navigate) {
            if (gesture.navigationInside) {
                const auto span = base.time.end - base.time.begin;
                const auto first = offsetSample(base.time.begin, dx / plot.width() * static_cast<long double>(file->metadata.sampleCount), file->metadata.sampleCount - span);
                next.time = {first, first + span}; session_.setView(next, false);
            }
        } else if (gesture.tool == Tool::PanTime) {
            const bool vertical = kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall;
            const auto span = base.time.end - base.time.begin;
            const auto first = offsetSample(base.time.begin, -(vertical ? dy / plot.height() : dx / plot.width()) * static_cast<long double>(span), file->metadata.sampleCount - span);
            next.time = {first, first + span}; session_.setView(next, false);
        } else {
            const bool vertical = kind_ == Kind::Main && file->display.mainMode == MainMode::TimeFrequency;
            const double delta = (vertical ? dy / plot.height() : -dx / plot.width()) * (base.frequency.upperHz - base.frequency.lowerHz);
            next.frequency.lowerHz += delta; next.frequency.upperHz += delta; session_.setView(next, false);
        }
    }
    updateHover(point); repaintChart();
    if (gesture.tool == Tool::MarkEdit || gesture.tool == Tool::AuxiliaryY || gesture.tool == Tool::PanTime || gesture.tool == Tool::PanFrequency || gesture.tool == Tool::Navigate) emit stateChanged();
}
void PlotWidget::finishGesture(Qt::KeyboardModifiers modifiers) {
    if (!gesture_) return;
    const auto gesture = *gesture_; gesture_.reset(); if (activeGesture == this) activeGesture.clear();
    releasing_ = true; if (mouseGrabber() == this) releaseMouse(); releasing_ = false;
    auto* file = session_.activeFile(); if (!file || file->metadata.id != gesture.before.fileId) return;
    const auto& base = gesture.before.view;
    if (gesture.tool == Tool::MarkEdit) {
        if (!gesture.changed) emit markSelectionRequested(QString::fromStdString(gesture.markId), modifiers);
        else emit statusMessage("已更新标记范围");
    } else if (gesture.tool == Tool::ZoomBox || gesture.tool == Tool::CreateMark) {
        const auto box = QRectF(gesture.start, gesture.current).normalized().intersected(plotRect());
        if (box.width() >= 8 && box.height() >= 8) {
            const auto first = fromPixel(box.topLeft(), base), last = fromPixel(box.bottomRight(), base);
            ViewRange next{{offsetSample(base.time.begin, std::min(first.sample, last.sample), file->metadata.sampleCount), offsetSample(base.time.begin, std::max(first.sample, last.sample), file->metadata.sampleCount)},
                           {std::min(first.frequency, last.frequency), std::max(first.frequency, last.frequency)}};
            if (gesture.tool == Tool::CreateMark) {
                if (next.time.end - next.time.begin < std::min<SampleIndex>(file->display.stftSize, file->metadata.sampleCount) ||
                    next.frequency.upperHz - next.frequency.lowerHz < file->metadata.sampleRateHz / file->display.stftSize) emit statusMessage("标记范围小于当前 STFT 分辨率");
                else { session_.addMark(next); emit statusMessage("已创建信号标记；持续选择信号仍开启"); }
            } else { session_.setView(next); emit statusMessage("矩形区域放大"); }
        } else if (!gesture.changed && !gesture.clickMarkId.empty() && gesture.tool == Tool::ZoomBox) emit markSelectionRequested(QString::fromStdString(gesture.clickMarkId), modifiers);
    } else if (gesture.tool == Tool::AuxiliaryZoom) {
        if (std::abs(gesture.current.x() - gesture.start.x()) >= 8) {
            const auto plot = plotRect();
            const double low = std::clamp((std::min(gesture.start.x(), gesture.current.x()) - plot.left()) / plot.width(), 0.0, 1.0);
            const double high = std::clamp((std::max(gesture.start.x(), gesture.current.x()) - plot.left()) / plot.width(), 0.0, 1.0);
            auto next = base;
            if (file->display.auxiliaryMode == AuxiliaryMode::Waveform) next.time = {offsetSample(base.time.begin, low * static_cast<long double>(base.time.end - base.time.begin), file->metadata.sampleCount), offsetSample(base.time.begin, high * static_cast<long double>(base.time.end - base.time.begin), file->metadata.sampleCount)};
            else { const double width = base.frequency.upperHz - base.frequency.lowerHz; next.frequency = {base.frequency.lowerHz + low * width, base.frequency.lowerHz + high * width}; }
            session_.setView(next);
        }
    } else if (gesture.tool == Tool::Navigate && !gesture.changed) {
        const auto full = fullRange(file->metadata); const auto coordinate = fromPixel(gesture.start, full);
        const auto span = base.time.end - base.time.begin; const auto first = sampleIndex(coordinate.sample - span / 2, file->metadata.sampleCount - span);
        auto next = base; next.time = {first, first + span}; session_.setView(next);
    } else if (gesture.changed) session_.commitViewChange(gesture.before);
    emit stateChanged(); repaintChart();
}
void PlotWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !gesture_ || gesture_->device != event->pointingDevice()) return;
    mouseMoveEvent(event); finishGesture(event->modifiers()); updateHover(event->position()); repaintChart(); event->accept();
}
void PlotWidget::mouseDoubleClickEvent(QMouseEvent* event) { mousePressEvent(event); }
void PlotWidget::wheelEvent(QWheelEvent* event) {
    auto* file = session_.activeFile(); const auto zone = zoneAt(event->position());
    event->accept(); if (!file || gesture_ || (activeGesture && activeGesture->window() == window()) || zone == Zone::None) return;
    const bool psd = kind_ == Kind::Auxiliary && file->display.auxiliaryMode == AuxiliaryMode::Psd;
    const bool waterfall = kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall;
    const WheelAxis axis = kind_ == Kind::Auxiliary && zone == Zone::YAxis ? WheelAxis::AuxiliaryY :
        kind_ == Kind::Main ? (waterfall ? zone == Zone::YAxis ? WheelAxis::Time : WheelAxis::Frequency : zone == Zone::YAxis ? WheelAxis::Frequency : WheelAxis::Time) : psd ? WheelAxis::Frequency : WheelAxis::Time;
    const int scrollLines = QApplication::wheelScrollLines();
    const double pixels = event->pixelDelta().isNull() ? -event->angleDelta().y() / 120.0 * (scrollLines < 0 ? std::max(40, height()) : scrollLines * 16) : -event->pixelDelta().y();
    if (!std::isfinite(pixels) || pixels == 0) return;
    if (kind_ == Kind::Auxiliary || kind_ == Kind::Main) beginPreview();
    if (activeWheel && activeWheel != this && activeWheel->window() == window()) activeWheel->finishWheel();
    if (wheelBase_ && (wheelBase_->fileId != file->metadata.id || wheelAxis_ != axis)) finishWheel();
    if (!wheelBase_) { wheelBase_ = session_.snapshot(); wheelAxis_ = axis; activeWheel = this; }
    const double factor = std::exp(std::clamp(pixels, -480.0, 480.0) * .002); const auto plot = plotRect();
    const double x = std::clamp((event->position().x() - plot.left()) / plot.width(), 0.0, 1.0), y = std::clamp((event->position().y() - plot.top()) / plot.height(), 0.0, 1.0);
    if (axis == WheelAxis::AuxiliaryY) {
        const double low = file->display.auxiliaryMin, high = file->display.auxiliaryMax, pivot = high - y * (high - low);
        session_.setAuxiliaryRange(pivot - (pivot - low) * factor, pivot + (high - pivot) * factor, false);
    } else {
        auto next = file->view;
        if (axis == WheelAxis::Frequency) {
            const double ratio = kind_ == Kind::Main && !waterfall ? 1 - y : x;
            const double pivot = next.frequency.lowerHz + ratio * (next.frequency.upperHz - next.frequency.lowerHz);
            next.frequency = {pivot - (pivot - next.frequency.lowerHz) * factor, pivot + (next.frequency.upperHz - pivot) * factor};
        } else {
            const auto width = next.time.end - next.time.begin;
            const auto span = sampleIndex(static_cast<long double>(width) * factor, file->metadata.sampleCount);
            const auto pivotOffset = kind_ == Kind::Navigation ? static_cast<long double>(x) * file->metadata.sampleCount - static_cast<long double>(next.time.begin) : (waterfall ? y : x) * static_cast<long double>(width);
            const auto first = offsetSample(next.time.begin, pivotOffset * (1 - factor), file->metadata.sampleCount - span); next.time = {first, first + span};
        }
        session_.setView(next, false);
    }
    wheelTimer_.start(); updateHover(event->position()); emit stateChanged(); repaintChart();
}
QMenu* PlotWidget::createContextMenu(const QPoint& point) {
    if (activeGesture && activeGesture != this && activeGesture->window() == window()) activeGesture->cancelGesture();
    if (activeWheel && activeWheel != this && activeWheel->window() == window()) activeWheel->finishWheel();
    cancelGesture();
    auto* menu = new QMenu(this); menu->setObjectName("chartContextMenu");
    menu->setStyleSheet("QMenu{background:#1b2b41;color:#dfecfa;border:1px solid #56718b;border-radius:6px;padding:5px;min-width:248px;}QMenu::item{padding:9px 12px;}QMenu::item:selected{background:#275475;}QMenu::item:disabled{color:#718195;}QMenu::item:checked{background:#63491e;color:#ffe3a0;}QMenu::separator{height:1px;background:#375168;margin:4px 0;}");
    const auto* file = session_.activeFile();
    addMenuHeading(*menu, QString(kind_ == Kind::Main ? "宽带图谱" : kind_ == Kind::Auxiliary ? "辅助分析" : "全局导航") + " · 视图与标记");
    if (kind_ == Kind::Main) {
        auto* toggle = menu->addAction("选择信号（持续模式）"); toggle->setObjectName("contextMarkToggle"); toggle->setCheckable(true); toggle->setChecked(creating_); toggle->setEnabled(file);
        connect(toggle, &QAction::triggered, this, [this](bool checked) { setCreating(checked); });
        const auto overlaps = marksAt(point);
        if (overlaps.size() > 1) {
            menu->addSeparator(); addMenuHeading(*menu, "选择重叠标记");
            for (const auto& id : overlaps) if (const auto* mark = findMark(*file, id)) {
                auto* choice = menu->addAction(QString::fromStdString(mark->name)); choice->setData(QString::fromStdString(id));
                connect(choice, &QAction::triggered, this, [this, id] { emit markSelectionRequested(QString::fromStdString(id), Qt::NoModifier); });
            }
        }
        if (file && findMark(*file, file->activeMarkId)) {
            auto* rename = menu->addAction("重命名当前标记"); rename->setObjectName("contextRenameAction"); connect(rename, &QAction::triggered, this, &PlotWidget::markRenameRequested);
            auto* locate = menu->addAction("定位当前标记"); locate->setObjectName("contextLocateAction"); connect(locate, &QAction::triggered, this, [this] { if (auto* file = session_.activeFile()) session_.focusMark(file->activeMarkId); emit stateChanged(); });
            auto* remove = menu->addAction(QString("删除所选标记 (%1)").arg(file->selectedMarkIds.size())); remove->setObjectName("contextDeleteAction"); connect(remove, &QAction::triggered, this, &PlotWidget::markDeleteRequested);
        }
    }
    menu->addSeparator();
    const auto addViewAction = [&](const QString& label, const char* name, bool enabled, auto callback) {
        auto* action = menu->addAction(label); action->setObjectName(QString::fromLatin1(name)); action->setEnabled(enabled); connect(action, &QAction::triggered, this, callback);
    };
    addViewAction("↶ 视图后退", "contextBackAction", file && session_.canBack(), [this] { session_.back(); emit stateChanged(); });
    addViewAction("↷ 视图前进", "contextForwardAction", file && session_.canForward(), [this] { session_.forward(); emit stateChanged(); });
    menu->addSeparator();
    addViewAction("⤢ 适应全部数据", "contextFitAction", file, [this] { if (const auto* file = session_.activeFile()) session_.setView(fullRange(file->metadata)); emit stateChanged(); });
    addViewAction("↺ 恢复默认视图", "contextResetAction", file, [this] { session_.resetView(); emit stateChanged(); });
    return menu;
}
void PlotWidget::contextMenuEvent(QContextMenuEvent* event) { auto* menu = createContextMenu(event->pos()); menu->exec(event->globalPos()); delete menu; event->accept(); }
void PlotWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { cancelGesture(true); emit stateChanged(); emit statusMessage("已取消拖动并退出持续选择"); event->accept(); }
    else if (event->key() == Qt::Key_Delete && kind_ == Kind::Main) { emit markDeleteRequested(); event->accept(); }
    else if (event->matches(QKeySequence::SelectAll) && kind_ == Kind::Main) {
        if (const auto* file = session_.activeFile()) { std::vector<std::string> ids; for (const auto& mark : file->marks) ids.push_back(mark.id); session_.selectMarks(ids, ids.empty() ? std::string{} : ids.back()); emit stateChanged(); }
        event->accept();
    } else QWidget::keyPressEvent(event);
}
void PlotWidget::resizeEvent(QResizeEvent* event) { beginPreview(); cancelGesture(); if (surface_) surface_->setGeometry(rect()); QWidget::resizeEvent(event); repaintChart(); }
bool PlotWidget::event(QEvent* event) {
    if ((event->type() == QEvent::UngrabMouse && !releasing_) || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        cancelGesture(event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide);
    else if (event->type() == QEvent::Leave && !gesture_) { hoverZone_ = Zone::None; hoveredMark_.clear(); setCursor(kind_ == Kind::Navigation ? Qt::PointingHandCursor : Qt::CrossCursor); emit interactionHint("鼠标指向图谱查看操作提示"); repaintChart(); }
    return QWidget::event(event);
}
} // namespace signalstudio
