#include "ui/parameter_input.h"
#include "domain/constellation.h"
#include "infrastructure/source_loader.h"
#include <chrono>
#include "infrastructure/spectral_analysis.h"
#include "ui/charts/cursor_overlay.h"
#include "ui/charts/interaction_feedback.h"
#include "ui/narrowband_workspace.h"
#include "ui/source_units.h"
#include "ui/brand/brand_assets.h"

#include "infrastructure/channel_processor.h"
#include "infrastructure/int16_iq_file.h"
#include "ui/charts/accelerated_surface.h"
#include "ui/charts/chart_interaction.h"
#include "ui/charts/palette.h"
#include "ui/checkbox_style.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
#include <QContextMenuEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QClipboard>
#include <QApplication>
#include <QColor>
#include <QList>
#include <QPainter>
#include <QKeyEvent>
#include <QPainterPath>
#include <QFile>
#include <QTextCursor>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <numbers>
#include <random>

namespace signalstudio {
namespace {

QString str(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
bool consumesAnalysis(int page) {
    return page == static_cast<int>(NarrowbandPage::Constellation) ||
           page == static_cast<int>(NarrowbandPage::Eye);
}
NarrowbandAnalysisSettings processingSettings(const Channel& channel) {
    auto settings = channelAnalysisSettings(channel);
    // Draw budgets and axis limits are projections of the same stage snapshot.
    settings.pointCount = 20000;
    settings.constellationAutoScale = true;
    settings.eyeComponent = 0;
    settings.eyeTraces = 256;
    settings.eyePointsPerUi = 16;
    return settings;
}
void enableStage(NarrowbandAnalysisSettings& s, AnalysisStage stage) {
    if ((stage==AnalysisStage::CfoCorrected || stage==AnalysisStage::CarrierRecovered) && s.cfoMode==CfoMode::Off)s.cfoMode=CfoMode::Automatic;
    if (stage == AnalysisStage::MatchedFiltered) s.matchedFilter = true;
    if (stage >= AnalysisStage::TimingAligned) s.timingRecovery = true;
    if (stage == AnalysisStage::CarrierRecovered) s.carrierRecovery = true;
}
void demoteStage(NarrowbandAnalysisSettings& s, AnalysisStage removed, AnalysisStage fallback) {
    for (auto* stage : {&s.constellationStage, &s.eyeStage})
        if (*stage >= removed) *stage = fallback;
}
QLabel* textLabel(const QString& value, const QString& id = {}) {
    auto* label = new QLabel(value); label->setWordWrap(true);
    if (!id.isEmpty()) label->setObjectName(id);
    return label;
}
QPushButton* actionButton(const QString& value, const QString& id, QLayout* layout) {
    auto* button = new QPushButton(value); button->setObjectName(id); button->setCursor(Qt::PointingHandCursor);
    layout->addWidget(button); return button;
}
void displayFftValue(QComboBox* combo, int points) {
    const QSignalBlocker blocker(combo);
    for (int row = 0; row < combo->count(); ++row) {
        const auto text = QString::number(combo->itemData(row).toInt());
        if (combo->itemText(row) != text) combo->setItemText(row, text);
    }
    int index = combo->findData(points);
    if (index < 0 && points >= 16 && points <= 262144 && (points & (points - 1)) == 0) {
        combo->addItem(QString::number(points), points);
        index = combo->count() - 1;
    }
    if (index >= 0) combo->setCurrentIndex(index);
}
QString waveformAxisLabel(NarrowbandWaveform mode, const FileMetadata* metadata = nullptr) {
    if (metadata) return sourceUnits(waveformAxisLabel(mode), *metadata);
    switch (mode) {
    case NarrowbandWaveform::IQ: return QStringLiteral("I / Q (ADC 计数等效值)");
    case NarrowbandWaveform::Magnitude: return QStringLiteral("幅度 RMS (ADC 计数等效值)");
    case NarrowbandWaveform::Phase: return QStringLiteral("相位 (rad)");
    case NarrowbandWaveform::Envelope: return QStringLiteral("幅度包络 (ADC 计数等效值)");
    }
    return QStringLiteral("幅值 (ADC 计数等效值)");
}
enum class ChartMode { Curve, Spectrum, Heatmap, Scatter, Eye, Timeline, Navigation };

} // namespace

class NarrowbandChart final : public QWidget {
public:
    explicit NarrowbandChart(ChartMode mode, QWidget* parent = nullptr) : QWidget(parent), mode_(mode) {
        setMinimumSize(100, 90); setMouseTracking(true); setFocusPolicy(Qt::StrongFocus);
        softwareFallback_ = (qApp && qApp->property("softwareRenderer").toBool()) || QGuiApplication::platformName()=="offscreen";
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        if(!softwareFallback_) {
            surface_ = new AcceleratedSurface(this); surface_->setMinimumSize(80, 70); layout->addWidget(surface_);
            surface_->setPainter([this](QPainter& painter) { draw(painter); });
            if (mode_ == ChartMode::Curve || mode_ == ChartMode::Spectrum || mode_ == ChartMode::Heatmap)
                surface_->setInversePainter([this](QPainter& painter) { drawCursors(painter, true); });
            connect(surface_, &AcceleratedSurface::backendFailed, this, [this](const QString& reason) {
                softwareFallback_ = true; surface_->hide();
                setStatus(QStringLiteral("QPainter 软件回退 · %1").arg(reason)); update();
            });
        }
        setProperty("equalAxisScale", mode_ == ChartMode::Scatter);
        setProperty("xAxisLabel", QString{});
        setProperty("yAxisLabel", QString{});
        setProperty("gridVisible", gridVisible_);
        setProperty("samplePointsVisible", false);
        setProperty("samplePointCount", 0);
        setProperty("paletteIndex", static_cast<int>(palette_));
        if (mode_ == ChartMode::Timeline) {
            yAxisMinimum_ = 0.0; yAxisMaximum_ = 1.0;
            yAxisLabel_ = QStringLiteral("置信度 (0–1)");
            setProperty("xAxisLabel", QStringLiteral("源文件时间 (s)"));
            setProperty("yAxisLabel", yAxisLabel_);
        } else if (mode_ == ChartMode::Eye) {
            yAxisMinimum_ = -1.0; yAxisMaximum_ = 1.0;
            yAxisLabel_ = QStringLiteral("幅度 (归一化)");
            setProperty("xAxisLabel", QStringLiteral("时间 (符号周期)"));
            setProperty("yAxisLabel", yAxisLabel_);
        } else if (mode_ == ChartMode::Scatter) {
            yAxisMinimum_=-1; yAxisMaximum_=1; yAxisLabel_="归一化";
            setProperty("xAxisLabel", QStringLiteral("I (归一化)"));
            setProperty("yAxisLabel", QStringLiteral("Q (归一化)"));
        }
        setStyleSheet("background:#111e2e;");
    }
    QRectF plotRect() const {
        const QRectF base(66, 14, std::max(1, width() - 82), std::max(1, height() - 55));
        if (mode_ != ChartMode::Scatter) return base;
        const double side = std::max(1.0, std::min(base.width(), base.height()));
        return QRectF(base.center().x() - side / 2, base.center().y() - side / 2, side, side);
    }
    void setSeries(std::vector<std::vector<float>> series, QString status = {}, QString summary = {}) {
        series_ = std::move(series); setStatus(std::move(status), std::move(summary));
        setProperty("samplePointCount", samplePointsVisible_ && !series_.empty() ? static_cast<qulonglong>(series_.front().size()) : 0);
        updateFitDiagnostics(); markGeometryDirty();
    }
    void setNavigationViewport(double first, double last) {
        navigationFirst_ = std::clamp(first, 0.0, 1.0);
        navigationLast_ = std::clamp(last, navigationFirst_, 1.0);
        invalidate();
    }
    void setFrequencyAxis(FrequencyRange range, double centerHz, bool absolute) {
        frequencyRange_ = range;
        frequencyCenterHz_ = centerHz;
        frequencyAbsolute_ = absolute;
        const double offset = absolute ? centerHz : 0.0;
        const double maximum = std::max(std::abs(range.lowerHz + offset), std::abs(range.upperHz + offset));
        frequencyScale_ = maximum >= 1e9 ? 1e9 : maximum >= 1e6 ? 1e6 : maximum >= 1e3 ? 1e3 : 1.0;
        frequencyUnit_ = maximum >= 1e9 ? QStringLiteral("GHz") : maximum >= 1e6 ? QStringLiteral("MHz") :
            maximum >= 1e3 ? QStringLiteral("kHz") : QStringLiteral("Hz");
        frequencyAxisLabel_ = QStringLiteral("%1频率 (%2)").arg(absolute ? QStringLiteral("RF ") : QStringLiteral("基带 "), frequencyUnit_);
        frequencyAxisText_ = QStringLiteral("%1 %2–%3 %4")
            .arg(absolute ? QStringLiteral("RF") : QStringLiteral("基带"),
                 QString::number((range.lowerHz + offset) / frequencyScale_, 'f', 3),
                 QString::number((range.upperHz + offset) / frequencyScale_, 'f', 3), frequencyUnit_);
        setProperty("frequencyAxisLabel", frequencyAxisLabel_);
        if (mode_ == ChartMode::Spectrum) setProperty("xAxisLabel", frequencyAxisLabel_);
        if (mode_ == ChartMode::Heatmap) setProperty("yAxisLabel", frequencyAxisLabel_);
        invalidate();
    }
    void setTimeAxis(TimeRange range, double sampleRateHz, bool relative, SampleIndex origin = 0) {
        timeSourceRange_ = range; sourceRate_ = sampleRateHz; relativeOrigin_ = relative ? origin : 0;
        if (sampleRateHz <= 0 || range.end <= range.begin) return;
        timeStartSeconds_ = static_cast<double>(range.begin - relativeOrigin_) / sampleRateHz;
        timeEndSeconds_ = static_cast<double>(range.end - relativeOrigin_) / sampleRateHz;
        const double span = timeEndSeconds_ - timeStartSeconds_;
        timeScale_ = span < .001 ? 1e-6 : span < 1.0 ? 1e-3 : 1.0;
        timeUnit_ = span < .001 ? QStringLiteral("µs") : span < 1.0 ? QStringLiteral("ms") : QStringLiteral("s");
        timeAxisLabel_ = QStringLiteral("%1 (%2)").arg(relative ? QStringLiteral("通道相对时间") : QStringLiteral("源文件时间"), timeUnit_);
        setProperty("timeAxisLabel", timeAxisLabel_);
        if (mode_ == ChartMode::Curve || mode_ == ChartMode::Heatmap || mode_ == ChartMode::Timeline)
            setProperty("xAxisLabel", timeAxisLabel_);
        invalidate();
    }
    void setSymbolPeriod(int periods) { symbolPeriods_ = std::clamp(periods, 1, 4); invalidate(); }
    void setGridVisible(bool visible) {
        if (gridVisible_ == visible) return;
        gridVisible_ = visible; setProperty("gridVisible", visible); invalidate();
    }
    void setPalette(Palette palette) {
        if (palette_ != palette) {
            palette_ = palette;
            if (!scalarRaster_.isNull()) {
                raster_.setColorTable(chart_palette::colorTable(palette_));
                if(surface_)surface_->setHeatmap(scalarRaster_, plotRect(), rasterRevision_, chart_palette::texture(palette_));
                invalidate();
            }
        }
        setProperty("paletteIndex", static_cast<int>(palette_));
    }
    void setSamplePointsVisible(bool visible) {
        if (samplePointsVisible_ == visible) return;
        samplePointsVisible_ = visible; setProperty("samplePointsVisible", visible);
        setProperty("samplePointCount", visible && !series_.empty() ? static_cast<qulonglong>(series_.front().size()) : 0);
        markGeometryDirty();
    }
    void setSamplePointOrigin(std::uint64_t firstOutputSample) { samplePointOrigin_ = firstOutputSample; }
    void setYAxisRange(double minimum, double maximum, QString label) {
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum) return;
        if (yAxisMinimum_ == minimum && yAxisMaximum_ == maximum && yAxisLabel_ == label) return;
        yAxisMinimum_ = minimum; yAxisMaximum_ = maximum; yAxisLabel_ = std::move(label);
        setProperty("yAxisLabel", yAxisLabel_);
        updateFitDiagnostics();
        markGeometryDirty();
    }
    void setRaster(QImage image, QString status = {}, QString revision = {}, QString summary = {}) {
        if (!image.isNull()) {
            scalarRaster_ = image.format() == QImage::Format_Grayscale8 ? image : image.convertToFormat(QImage::Format_Grayscale8);
            raster_ = QImage(scalarRaster_.size(), QImage::Format_Indexed8);
            raster_.setColorTable(chart_palette::colorTable(palette_));
            for (int y = 0; y < scalarRaster_.height(); ++y)
                std::copy_n(scalarRaster_.constScanLine(y), scalarRaster_.width(), raster_.scanLine(y));
        } else {
            scalarRaster_ = {}; raster_ = {};
        }
        setStatus(std::move(status), std::move(summary));
        const auto key = revision.isEmpty() ?
            QString::number(reinterpret_cast<quintptr>(this), 16) + QLatin1Char('/') + QString::number(scalarRaster_.cacheKey()) : revision;
        rasterRevision_ = key;
        if(surface_)surface_->setHeatmap(scalarRaster_, scalarRaster_.isNull() ? QRectF{} : plotRect(), key, chart_palette::texture(palette_));
        invalidate();
    }
    void setPoints(std::vector<std::complex<float>> points, QString status = {}, QString summary = {}) {
        points_ = std::move(points); setProperty("constellationPointCount",static_cast<qulonglong>(points_.size()));
        if(points_.empty()){setProperty("realIqSource",false);setProperty("sourceChannelId",QString{});setProperty("actualSampleRateHz",qulonglong{0});}
        setStatus(std::move(status), std::move(summary)); if(surface_)surface_->setHeatmap({}, {}, QString{}); markGeometryDirty();
    }
    void setIqRange(double minimum,double maximum,const QString& unit) {
        setYAxisRange(minimum,maximum,unit);
        setProperty("iqAxisMinimum",minimum);setProperty("iqAxisMaximum",maximum);
        setProperty("iqUnit",unit);
    }
    QPointF iqPoint(std::complex<float> point) const {
        const auto rect=plotRect();const double span=std::max(1e-15,yAxisMaximum_-yAxisMinimum_);
        return {rect.left()+(point.real()-yAxisMinimum_)/span*rect.width(),
                rect.bottom()-(point.imag()-yAxisMinimum_)/span*rect.height()};
    }
    void setEye(std::vector<std::vector<float>> traces, QString status = {}, QString summary = {}) {
        series_ = std::move(traces);eyeComponents_.clear();setProperty("realEyeSource",false);
        setProperty("eyeTraceCount",static_cast<qulonglong>(series_.size()));
        setStatus(std::move(status), std::move(summary)); if(surface_)surface_->setHeatmap({}, {}, QString{}); markGeometryDirty();
    }
    void setRealEye(const NarrowbandAnalysisResult& result,int component,int traces,int pointsPerUi) {
        series_.clear();eyeComponents_.clear();
        const auto count=std::min(result.eyes.size(),static_cast<std::size_t>(traces));
        const auto stride=std::max(1,result.eyePointsPerUi/pointsPerUi);
        for(std::size_t row=0;row<count;++row) {
            const auto index=count>1?row*(result.eyes.size()-1)/(count-1):0;
            const auto& trace=result.eyes[index];
            const auto append=[this,stride](const std::vector<float>& values,int color){
                std::vector<float> line;line.reserve((values.size()+stride-1)/stride);
                for(std::size_t p=0;p<values.size();p+=stride)line.push_back(values[p]);
                series_.push_back(std::move(line));eyeComponents_.push_back(color);
            };
            if(component!=2)append(trace.i,0);
            if(component!=1)append(trace.q,1);
        }
        setYAxisRange(result.eyeAxisMinimum,result.eyeAxisMaximum,result.gain!=1?"归一化 IQ 幅度":"解码 IQ 幅度");
        setSymbolPeriod(result.eyePeriods);
        setProperty("realEyeSource",true);setProperty("eyeTraceCount",static_cast<qulonglong>(count));
        setProperty("eyeComponentCount",component==0?2:1);setProperty("eyePeriods",result.eyePeriods);
        setProperty("eyePointsPerUi",pointsPerUi);setProperty("physicalSamplesPerSymbol",result.samplesPerSymbol);
        setProperty("eyePointsPerTrace",series_.empty()?0:static_cast<int>(series_.front().size()));
        setStatus(str(result.eyeMessage),QString("真实 IQ · %1 条 · %2 UI · SPS=%3 · %4").arg(count).arg(result.eyePeriods).arg(result.samplesPerSymbol,0,'g',6)
          .arg(result.eyeStage>=AnalysisStage::TimingAligned && result.timingLocked?"定时对齐":"周期假设，未锁定"));
        if(surface_)surface_->setHeatmap({}, {}, QString{});markGeometryDirty();
    }
    double amplitudeUnit(float value) const {return (yAxisMaximum_-value)/std::max(1e-15,yAxisMaximum_-yAxisMinimum_);}
    QColor eyeColor(std::size_t series) const {
        if(series<eyeComponents_.size())return eyeComponents_[series]==0?QColor(88,206,242,80):QColor(237,193,111,80);
        return QColor(72,207,196,80+static_cast<int>(series%75));
    }
    QString status() const { return status_; }
    QString displayStatus() const { return displayStatus_; }
    std::optional<std::pair<double, double>> valueRange() const {
        double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
        for (const auto& line : series_) for (const float value : line) {
            if (!std::isfinite(value)) continue;
            minimum = std::min(minimum, static_cast<double>(value));
            maximum = std::max(maximum, static_cast<double>(value));
        }
        if (!std::isfinite(minimum) || !std::isfinite(maximum)) return std::nullopt;
        return std::pair{minimum, maximum};
    }
    bool isDisplaySettled() const { return softwareFallback_ || (surface_ && surface_->isReady() && !surface_->hasPendingUploads()); }
    bool gpuReady() const { return !softwareFallback_ && surface_ && surface_->isReady(); }
    quint64 textureUploadCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->textureUploadCount(); }
    quint64 completedFrameCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->completedFrameCount(); }
    quint64 gpuDataDrawCallCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->chartDrawCallCount() + surface_->heatmapDrawCallCount(); }
    quint64 gpuVertexUploadCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->chartVertexUploadCount(); }
    quint64 inverseCursorDrawCallCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->inverseOverlayDrawCallCount(); }
    QString backendDescription() const { return softwareFallback_ || !surface_ ? QStringLiteral("QPainter 软件绘制") : surface_->backendDescription(); }
    void invalidateVisibleOverlay() { invalidate(); }
    std::function<void(QPointF, QPointF, int, bool)> gesture;
    std::function<void()> cancelPendingGesture;
    std::function<void()> resetYAxis;
    std::function<void(bool)> navigateHistory;
    bool hasPendingInteraction() const { return dragging_; }
    void cancelInteraction() {
        dragging_ = false; hoverZone_ = chart_feedback::Zone::None; hoverPosition_.reset(); setCursor(Qt::ArrowCursor);
        setProperty("interactionZone",static_cast<int>(hoverZone_));
        if (mouseGrabber() == this) releaseMouse();
        invalidate();
    }
    std::function<void(QPointF)> pinRequested;
    std::function<void()> autoPowerFitRequested;
    std::function<bool()> autoPowerFitAvailable;
    std::function<void()> clearCursorRequested, averagePsdRequested, framePsdRequested, expandAnalysisRequested;
    std::function<bool()> pendingContextGesture;
    void setLinkedCursor(LinkedCursorState cursor) { pinnedCursor_ = cursor; invalidate(); }
    void setSpectralData(std::shared_ptr<const SpectrogramData> spectrum) { spectralData_ = std::move(spectrum); invalidate(); }
    void setSamplePositions(std::vector<SampleIndex> positions) { samplePositions_ = std::move(positions); markGeometryDirty(); }
    double seriesUnit(std::size_t index, std::size_t count) const {
        if (mode_ == ChartMode::Curve && samplePositions_.size() == count && timeSourceRange_.end > timeSourceRange_.begin)
            return static_cast<double>(samplePositions_[index] - timeSourceRange_.begin) / (timeSourceRange_.end - timeSourceRange_.begin);
        return static_cast<double>(index) / std::max<std::size_t>(1, mode_ == ChartMode::Spectrum ? count : count - 1);
    }
    bool event(QEvent* event) override {
        if(cursor_overlay::tooltip(this,event)) return true;
        if (event->type() == QEvent::Leave) { hoverPosition_.reset(); if (!dragging_) { hoverZone_ = chart_feedback::Zone::None; setCursor(Qt::ArrowCursor); setProperty("interactionZone",static_cast<int>(hoverZone_)); } invalidate(); }
        return QWidget::event(event);
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
        // Hidden pages receive data before their final layout size is known.
        // Reposition the same GPU texture when the chart becomes visible.
        if (surface_ && !scalarRaster_.isNull())
            surface_->setHeatmap(scalarRaster_, plotRect(), rasterRevision_, chart_palette::texture(palette_));
        markGeometryDirty();
        QWidget::resizeEvent(event);
    }
    void paintEvent(QPaintEvent* event) override {
        if (!softwareFallback_) { QWidget::paintEvent(event); return; }
        Q_UNUSED(event);
        QPainter painter(this);
        draw(painter, true);
    }
    void wheelEvent(QWheelEvent* event) override {
        const auto pos = event->position(); const int delta = event->angleDelta().y();
        const QPointF normalized(pos.x() / std::max(1, width()), pos.y() / std::max(1, height()));
        if (gesture) gesture(normalized, normalized, delta, false);
        event->accept();
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && (gesture || pinRequested)) {
            dragZone_ = chart_feedback::zoneAt(event->position(), plotRect());
            if (dragZone_ == chart_feedback::Zone::None) return;
            dragStart_ = dragEnd_ = event->position(); dragging_ = true;
            updateHover(event->position()); setFocus(); grabMouse(); event->accept(); return;
        }
        if (event->button() == Qt::RightButton) {
            cancelInteraction();
            if (cancelPendingGesture) cancelPendingGesture();
            event->accept(); return;
        }
        QWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_) { dragEnd_ = event->position(); updateHover(event->position()); invalidate(); event->accept(); return; }
        hoverPosition_ = plotRect().contains(event->position()) ? std::optional<QPointF>{event->position()} : std::nullopt;
        updateHover(event->position()); QToolTip::hideText(); invalidate(); QWidget::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragEnd_ = event->position(); dragging_ = false;
            if (mouseGrabber() == this) releaseMouse();
            const bool click = std::max(std::abs(dragEnd_.x() - dragStart_.x()), std::abs(dragEnd_.y() - dragStart_.y())) <= 5;
            if (click && pinRequested && plotRect().contains(dragEnd_)) pinRequested(dragEnd_);
            else if (gesture) gesture(QPointF(dragStart_.x() / std::max(1, width()), dragStart_.y() / std::max(1, height())),
                                 QPointF(dragEnd_.x() / std::max(1, width()), dragEnd_.y() / std::max(1, height())), 0,
                                 !click);
            updateHover(event->position()); invalidate(); event->accept(); return;
        }
        QWidget::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            const bool pending = dragging_ || (pendingContextGesture && pendingContextGesture());
            cancelInteraction();
            if (pending && cancelPendingGesture) cancelPendingGesture();
            else if (clearCursorRequested) clearCursorRequested();
            invalidate(); event->accept(); return;
        }
        QWidget::keyPressEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);
        if (mode_ == ChartMode::Curve || mode_ == ChartMode::Spectrum) {
            auto* reset = menu.addAction(mode_ == ChartMode::Curve ? QStringLiteral("自动适配纵轴") : QStringLiteral("重置 PSD 纵轴"));
            connect(reset, &QAction::triggered, this, [this] { if (resetYAxis) resetYAxis(); });
            menu.addSeparator();
        }
        if (pinRequested) {
            auto* clear = menu.addAction("清除驻留游标"); clear->setObjectName("nbClearCursor");
            clear->setEnabled(pinnedCursor_.pinned);
            connect(clear, &QAction::triggered, this, [this] { if (clearCursorRequested) clearCursorRequested(); });
            auto* average = menu.addAction("当前窗平均谱"); average->setObjectName("nbAveragePsd");
            connect(average, &QAction::triggered, this, [this] { if (averagePsdRequested) averagePsdRequested(); });
            auto* frame = menu.addAction("驻留帧谱（点数随 STFT）"); frame->setObjectName("nbFramePsd"); frame->setEnabled(pinnedCursor_.pinned);
            connect(frame, &QAction::triggered, this, [this] { if (framePsdRequested) framePsdRequested(); });
            auto* fit = menu.addAction("自动适配电平"); fit->setObjectName("nbAutoPowerFit");
            fit->setEnabled(autoPowerFitAvailable && autoPowerFitAvailable());
            connect(fit, &QAction::triggered, this, [this] { if (autoPowerFitRequested) autoPowerFitRequested(); });
            auto* expand = menu.addAction("扩展时间窗以满足分析点数"); expand->setObjectName("nbExpandAnalysisTime");
            connect(expand, &QAction::triggered, this, [this] { if (expandAnalysisRequested) expandAnalysisRequested(); });
            menu.addSeparator();
        }
        auto* back = menu.addAction(QStringLiteral("视图后退"));
        auto* forward = menu.addAction(QStringLiteral("视图前进"));
        connect(back, &QAction::triggered, this, [this] { if (navigateHistory) navigateHistory(false); });
        connect(forward, &QAction::triggered, this, [this] { if (navigateHistory) navigateHistory(true); });
        menu.exec(event->globalPos());
        event->accept();
    }
private:
    void updateFitDiagnostics() {
        if (mode_ != ChartMode::Curve || series_.empty()) {
            setProperty("fitDataVerticalFraction", -1.0);
            setProperty("fitDataCenterOffsetFraction", -1.0);
            return;
        }
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (const auto& line : series_) for (const float value : line) {
            if (!std::isfinite(value)) continue;
            low = std::min(low, static_cast<double>(value));
            high = std::max(high, static_cast<double>(value));
        }
        const double axisSpan = yAxisMaximum_ - yAxisMinimum_;
        if (!std::isfinite(low) || !std::isfinite(high) || axisSpan <= 0) {
            setProperty("fitDataVerticalFraction", -1.0);
            setProperty("fitDataCenterOffsetFraction", -1.0);
            return;
        }
        setProperty("fitDataVerticalFraction", (high - low) / axisSpan);
        setProperty("fitDataCenterOffsetFraction", ((high + low) / 2.0 - (yAxisMaximum_ + yAxisMinimum_) / 2.0) / axisSpan);
    }
    void invalidate() { if (surface_ && !softwareFallback_) surface_->invalidateOverlay(); update(); }
    void markGeometryDirty() { geometryDirty_ = true; ++geometryRevision_; invalidate(); }
    void rebuildGeometry() {
        if (!geometryDirty_ || !surface_ || softwareFallback_) return;
        geometryDirty_ = false;
        std::vector<AcceleratedSurface::ChartVertex> vertices;
        std::vector<AcceleratedSurface::ChartDrawCall> draws;
        const QRectF plot = plotRect();
        const auto append = [&](QPointF point, const QColor& color) {
            const float alpha = color.alphaF();
            vertices.push_back({static_cast<float>(point.x() / std::max(1, width())),
                                static_cast<float>(point.y() / std::max(1, height())),
                                static_cast<float>(color.redF() * alpha), static_cast<float>(color.greenF() * alpha),
                                static_cast<float>(color.blueF() * alpha), alpha});
        };
        const auto addLine = [&](const std::vector<float>& values, const QColor& color, bool eye) {
            if (values.size() < 2) return;
            const auto first = static_cast<quint32>(vertices.size());
            const double span = std::max(1e-12, yAxisMaximum_ - yAxisMinimum_);
            for (std::size_t index = 0; index < values.size(); ++index) {
                const double x = plot.left() + plot.width() * seriesUnit(index, values.size());
                double y = .5;
                if (eye) y = amplitudeUnit(values[index]);
                else if (mode_ == ChartMode::Curve || mode_ == ChartMode::Spectrum)
                    y = 1.0 - std::clamp((static_cast<double>(values[index]) - yAxisMinimum_) / span, 0.0, 1.0);
                else if (mode_ == ChartMode::Timeline) y = 1.0 - std::clamp<double>(values[index], 0.0, 1.0);
                else y = 1.0 - (.5 + .45 * std::clamp<double>(values[index], -1.0, 1.0));
                append(QPointF(x, plot.top() + y * plot.height()), color);
            }
            draws.push_back({AcceleratedSurface::ChartDrawCall::Topology::LineStrip, first,
                             static_cast<quint32>(values.size())});
        };
        if (mode_ == ChartMode::Scatter && !points_.empty()) {
            const auto first = static_cast<quint32>(vertices.size());
            const QColor color("#53c8f1");
            constexpr int sectors = 8;
            int outside=0;
            for (const auto& point : points_) {
                const QPointF center=iqPoint(point);
                if(!plot.contains(center)) {++outside;continue;}
                for (int sector = 0; sector < sectors; ++sector) {
                    const double a0 = 2.0 * std::numbers::pi * sector / sectors;
                    const double a1 = 2.0 * std::numbers::pi * (sector + 1) / sectors;
                    append(center, color);
                    append(center + QPointF(3.0 * std::cos(a0), 3.0 * std::sin(a0)), color);
                    append(center + QPointF(3.0 * std::cos(a1), 3.0 * std::sin(a1)), color);
                }
            }
            setProperty("outsidePointCount",outside);
            draws.push_back({AcceleratedSurface::ChartDrawCall::Topology::Triangles, first,
                             static_cast<quint32>(vertices.size() - first)});
        } else if (mode_ != ChartMode::Navigation) {
            const std::array<QColor, 3> colors{QColor("#58cef2"), QColor("#edc16f"), QColor("#f179ac")};
            for (std::size_t series = 0; series < series_.size(); ++series)
                addLine(series_[series], mode_ == ChartMode::Eye ? eyeColor(series) : colors[series % colors.size()],
                        mode_ == ChartMode::Eye);
            if (mode_ == ChartMode::Curve && samplePointsVisible_) {
                constexpr int sectors = 8;
                for (std::size_t series = 0; series < series_.size(); ++series) {
                    const auto& values = series_[series];
                    const auto first = static_cast<quint32>(vertices.size());
                    for (std::size_t index = 0; index < values.size(); ++index) {
                        const QPointF center(plot.left() + plot.width() * seriesUnit(index, values.size()),
                            plot.top() + (yAxisMaximum_ - values[index]) /
                                std::max(1e-12, yAxisMaximum_ - yAxisMinimum_) * plot.height());
                        for (int sector = 0; sector < sectors; ++sector) {
                            const double a0 = 2.0 * std::numbers::pi * sector / sectors;
                            const double a1 = 2.0 * std::numbers::pi * (sector + 1) / sectors;
                            append(center, colors[series % colors.size()]);
                            append(center + QPointF(2.2 * std::cos(a0), 2.2 * std::sin(a0)), colors[series % colors.size()]);
                            append(center + QPointF(2.2 * std::cos(a1), 2.2 * std::sin(a1)), colors[series % colors.size()]);
                        }
                    }
                    if (vertices.size() > first)
                        draws.push_back({AcceleratedSurface::ChartDrawCall::Topology::Triangles, first,
                                         static_cast<quint32>(vertices.size() - first)});
                }
            }
        }
        surface_->setChartGeometry(std::move(vertices), std::move(draws),
            QString::number(reinterpret_cast<quintptr>(this), 16) + QLatin1Char('/') + QString::number(geometryRevision_), plot);
    }
    void draw(QPainter& painter, bool software = false) {
        if (!software) rebuildGeometry();
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF rect = plotRect();
        if(mode_==ChartMode::Heatmap)setProperty("heatmapDrawRect",raster_.isNull()?QRectF{}:rect);
        if (software)
            painter.fillRect(QRectF(0, 0, width(), height()), QColor("#101b2a"));
        if (software && mode_ == ChartMode::Heatmap && !raster_.isNull()) {
            painter.save(); painter.setClipRect(rect); painter.drawImage(rect, raster_); painter.restore();
        }
        if (gridVisible_) {
            painter.setPen(QPen(QColor("#2a4258"), 1));
            for (int i = 0; i <= 4; ++i) {
                const qreal x = rect.left() + rect.width() * i / 4;
                const qreal y = rect.top() + rect.height() * i / 4;
                painter.drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
                painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
            }
        }
        if(mode_==ChartMode::Scatter && yAxisMinimum_<=0 && yAxisMaximum_>=0) {
            const auto zero=iqPoint({0,0});painter.setPen(QPen(QColor("#7896ad"),1,Qt::DashLine));
            painter.drawLine(QPointF(zero.x(),rect.top()),QPointF(zero.x(),rect.bottom()));
            painter.drawLine(QPointF(rect.left(),zero.y()),QPointF(rect.right(),zero.y()));
            setProperty("iqZeroPixel",zero);
        }
        painter.setPen(QPen(QColor("#607a91"), 1)); painter.drawRect(rect);
        chart_feedback::drawAxisHighlight(painter, this->rect(), rect, hoverZone_);
        painter.setFont(QFont("Consolas", 8)); painter.setPen(QColor("#86a4bd"));
        for (int tick = 0; tick <= 4; ++tick) {
            QString label;
            if (mode_ == ChartMode::Curve || mode_ == ChartMode::Spectrum)
                label = QString::number(yAxisMaximum_ - (yAxisMaximum_ - yAxisMinimum_) * tick / 4.0, 'g', 4);
            else if (mode_ == ChartMode::Heatmap) {
                const double value = frequencyRange_.upperHz - (frequencyRange_.upperHz - frequencyRange_.lowerHz) * tick / 4.0 +
                    (frequencyAbsolute_ ? frequencyCenterHz_ : 0.0);
                label = QString::number(value / frequencyScale_, 'g', 4);
            } else if (mode_ == ChartMode::Scatter || mode_ == ChartMode::Eye)
                label = QString::number(yAxisMaximum_-(yAxisMaximum_-yAxisMinimum_)*tick/4,'g',4);
            else if (mode_ == ChartMode::Timeline)
                label = QString::number(1.0 - tick * .25, 'g', 3);
            if (!label.isEmpty()) painter.drawText(QRectF(0, rect.top() + rect.height() * tick / 4.0 - 8, 48, 16), Qt::AlignRight, label);
        }
        const QString verticalLabel = mode_ == ChartMode::Curve || mode_ == ChartMode::Spectrum ? yAxisLabel_ :
            mode_ == ChartMode::Heatmap ? frequencyAxisLabel_ : mode_ == ChartMode::Scatter ? "Q ("+yAxisLabel_+")" :
            mode_ == ChartMode::Eye ? yAxisLabel_ : QStringLiteral("置信度 (0–1)");
        setProperty("yAxisLabel", verticalLabel);
        if (!verticalLabel.isEmpty()) {
            painter.save();
            painter.translate(10, rect.center().y());
            painter.rotate(-90);
            painter.drawText(QRectF(-rect.height() / 2, -8, rect.height(), 16), Qt::AlignCenter, verticalLabel);
            painter.restore();
        }
        for (int tick = 0; tick <= 4; ++tick) {
            QString label;
            if (mode_ == ChartMode::Spectrum) {
                const double offset = frequencyAbsolute_ ? frequencyCenterHz_ : 0.0;
                label = QString::number((frequencyRange_.lowerHz + offset +
                    (frequencyRange_.upperHz - frequencyRange_.lowerHz) * tick / 4.0) / frequencyScale_, 'g', 4);
            } else if (mode_ == ChartMode::Scatter)
                label = QString::number(yAxisMinimum_+(yAxisMaximum_-yAxisMinimum_)*tick/4,'g',4);
            else if (mode_ == ChartMode::Eye)
                label = QString::number(symbolPeriods_ * tick / 4.0, 'g', 3);
            else if (mode_ == ChartMode::Curve || mode_ == ChartMode::Heatmap || mode_ == ChartMode::Timeline) {
                const double value = timeStartSeconds_ + (timeEndSeconds_ - timeStartSeconds_) * tick / 4.0;
                label = QString::number(value / timeScale_, 'g', 4);
            }
            if (!label.isEmpty()) {
                const double x = rect.left() + rect.width() * tick / 4.0;
                painter.drawText(QRectF(x - 42, rect.bottom() + 1, 84, 14), Qt::AlignHCenter | Qt::AlignTop, label);
            }
        }
        const QString horizontalLabel = mode_ == ChartMode::Spectrum ? frequencyAxisLabel_ :
            mode_ == ChartMode::Scatter ? "I ("+yAxisLabel_+")" :
            mode_ == ChartMode::Eye ? QStringLiteral("时间 (符号周期)") : timeAxisLabel_;
        setProperty("xAxisLabel", horizontalLabel);
        painter.drawText(QRectF(rect.left(), height() - 16, rect.width(), 14), Qt::AlignHCenter | Qt::AlignBottom, horizontalLabel);
        if (software && mode_ == ChartMode::Scatter) {
            painter.save(); painter.setClipRect(rect); painter.setPen(Qt::NoPen); painter.setBrush(QColor("#53c8f1"));
            int outside=0;
            for (const auto& point : points_) {
                const QPointF p=iqPoint(point);
                if(!rect.contains(p)) {++outside;continue;}
                painter.drawEllipse(p, 3.0, 3.0);
            }
            setProperty("outsidePointCount",outside);
            painter.restore();
        } else if (mode_ == ChartMode::Navigation) {
            painter.fillRect(rect, QColor("#1b2c40"));
            const QRectF viewport(rect.left() + rect.width() * navigationFirst_, rect.top(),
                                  std::max(2.0, rect.width() * (navigationLast_ - navigationFirst_)), rect.height());
            painter.fillRect(viewport, QColor(51, 166, 213, 80));
            painter.setPen(QPen(QColor("#5ac8f0"), 1.5)); painter.drawRect(viewport);
        } else if (software && mode_ == ChartMode::Eye) {
            painter.save(); painter.setClipRect(rect); const int count = static_cast<int>(series_.size());
            for (int s = 0; s < count; ++s) {
                QPainterPath path; const auto& values = series_[static_cast<std::size_t>(s)];
                for (std::size_t i = 0; i < values.size(); ++i) {
                    const QPointF p(rect.left() + rect.width() * seriesUnit(i, values.size()),
                        rect.top()+amplitudeUnit(values[i])*rect.height());
                    if (!i) path.moveTo(p); else path.lineTo(p);
                }
                painter.setPen(QPen(eyeColor(static_cast<std::size_t>(s)), 1)); painter.drawPath(path);
            }
            painter.restore();
        } else if (software && !series_.empty()) {
            painter.save(); painter.setClipRect(rect); const std::array<QColor, 3> colors{QColor("#58cef2"), QColor("#edc16f"), QColor("#f179ac")};
            for (std::size_t s = 0; s < series_.size(); ++s) {
                const auto& values = series_[s]; if (values.empty()) continue;
                QPainterPath path;
                for (std::size_t i = 0; i < values.size(); ++i) {
                    double normalized = mode_ == ChartMode::Curve || mode_ == ChartMode::Spectrum ?
                        (values[i] - yAxisMinimum_) / std::max(1e-12, yAxisMaximum_ - yAxisMinimum_) :
                        mode_ == ChartMode::Timeline ? values[i] : mode_ == ChartMode::Heatmap ? .5 + .45 * values[i] : .5 + .5 * values[i];
                    normalized = std::clamp(normalized, 0.0, 1.0);
                    const QPointF p(rect.left() + rect.width() * seriesUnit(i, values.size()),
                                    rect.bottom() - normalized * rect.height());
                    if (!i) path.moveTo(p); else path.lineTo(p);
                }
                painter.setPen(QPen(colors[s % colors.size()], 1.4)); painter.drawPath(path);
                if (mode_ == ChartMode::Curve && samplePointsVisible_) {
                    painter.setPen(Qt::NoPen); painter.setBrush(colors[s % colors.size()]);
                    for (std::size_t i = 0; i < values.size(); ++i) {
                        const QPointF p(rect.left() + rect.width() * seriesUnit(i, values.size()),
                            rect.top() + (yAxisMaximum_ - values[i]) /
                                std::max(1e-12, yAxisMaximum_ - yAxisMinimum_) * rect.height());
                        painter.drawEllipse(p, 2.2, 2.2);
                    }
                }
            }
            painter.restore();
        }
        if (!displayStatus_.isEmpty()) {
            painter.fillRect(QRectF(rect.left() + 6, rect.top() + 5, rect.width() - 12, 20), QColor(7, 17, 28, 190));
            painter.setPen(QColor("#aac1d4"));
            painter.drawText(QRectF(rect.left() + 11, rect.top() + 5, rect.width() - 20, 20), Qt::AlignVCenter,
                painter.fontMetrics().elidedText(displayStatus_, Qt::ElideRight, std::max(1, static_cast<int>(rect.width() - 20))));
        }
        if(mode_==ChartMode::Eye && !eyeComponents_.empty()) {
            const QRectF legend(rect.right()-80,rect.top()+28,76,19);painter.fillRect(legend,QColor(7,17,28,210));
            painter.setPen(QColor("#58cef2"));painter.drawText(legend.adjusted(5,0,-38,0),Qt::AlignCenter,"I");
            painter.setPen(QColor("#edc16f"));painter.drawText(legend.adjusted(38,0,-5,0),Qt::AlignCenter,"Q");
        }
        if (dragging_ && dragZone_ == chart_feedback::Zone::Plot && mode_ != ChartMode::Timeline) {
            painter.setPen(QPen(QColor("#f1cf7f"), 1, Qt::DashLine));
            painter.drawRect(QRectF(dragStart_, dragEnd_).normalized());
        }
        if (software) drawCursors(painter);
    }
    void drawCursors(QPainter& painter, bool inverseMask = false) {
        if (!inverseMask) {
            cursor_overlay::paintInverseOverlay(painter,rect(),[this](QPainter& mask) { drawCursors(mask,true); }); return;
        }
        if (!pinRequested || timeSourceRange_.end <= timeSourceRange_.begin) return;
        const auto plot = plotRect(); const double span = frequencyRange_.upperHz - frequencyRange_.lowerHz;
        setProperty("cursorPinned", pinnedCursor_.pinned); setProperty("framePsd", pinnedCursor_.framePsd);
        setProperty("pinnedSample", QVariant::fromValue<qulonglong>(pinnedCursor_.sourceSample));
        setProperty("pinnedFrequencyHz", pinnedCursor_.frequencyHz);
        cursor_overlay::Layout pinLayout;
        const auto render = [&](QPointF point, bool isPinned) {
        const double x = std::clamp((point.x() - plot.left()) / plot.width(), 0.0, 1.0);
        const double y = std::clamp((plot.bottom() - point.y()) / plot.height(), 0.0, 1.0);
        const auto sample = isPinned ? pinnedCursor_.sourceSample : timeSourceRange_.begin + std::min(timeSourceRange_.end - timeSourceRange_.begin - 1,
            static_cast<SampleIndex>(x * (timeSourceRange_.end - timeSourceRange_.begin)));
        const double frequency = isPinned ? pinnedCursor_.frequencyHz : frequencyRange_.lowerHz + (mode_ == ChartMode::Spectrum ? x : y) * span;
        const double time = static_cast<double>(sample - relativeOrigin_) / sourceRate_;
        setProperty("cursorTimeSeconds", time); setProperty("cursorSourceSample", QVariant::fromValue<qulonglong>(sample));
        QString text;
        if (mode_ != ChartMode::Spectrum) text = QStringLiteral("t = %1 %2").arg(time / timeScale_, 0, 'g', 10).arg(timeUnit_);
        if (mode_ != ChartMode::Curve) text += (text.isEmpty() ? QString{} : "\n") +
            QStringLiteral("f = %1 %2").arg((frequency + (frequencyAbsolute_ ? frequencyCenterHz_ : 0)) / frequencyScale_, 0, 'g', 10).arg(frequencyUnit_);
        if (mode_ == ChartMode::Heatmap) {
            const auto* frame = spectralData_ ? spectralData_->frameAtFraction(x) : nullptr;
            if(isPinned&&spectralData_)for(const auto& f:spectralData_->frames)if(f->id==pinnedCursor_.selectedFrame){frame=f.get();break;}
            if (frame) {
                const auto bin = frame->binAt(frequency);
                text += QStringLiteral("\nP = %1 dBFS/Hz · bin %2\n帧中心 %3 %4 · Δf %5 Hz")
                    .arg(frame->dbAt(bin), 0, 'f', 2).arg(bin)
                    .arg(static_cast<double>(frame->sourceCenter - relativeOrigin_) / sourceRate_ / timeScale_, 0, 'g', 10).arg(timeUnit_).arg(frame->binHz, 0, 'g', 6);
                setProperty("cursorPowerDb", frame->dbAt(bin)); setProperty("cursorFrameId", QVariant::fromValue<qulonglong>(frame->id));
            } else text += "\n分析数据不可用 / 正在更新";
        } else if (!series_.empty() && !series_.front().empty()) {
            const auto& values = series_.front();
            const auto index = std::min(values.size() - 1, static_cast<std::size_t>(std::llround(x * (mode_ == ChartMode::Spectrum ? values.size() : values.size() - 1))));
            const auto valueLabel=mode_==ChartMode::Spectrum ? QStringLiteral("P") : series_.size()>1 ? QStringLiteral("I") : yAxisLabel_.section('(',0,0).trimmed();
            const auto valueUnit=mode_==ChartMode::Spectrum ? property("sourcePowerUnit").toString() : yAxisLabel_.contains("rad") ? QStringLiteral("rad") : property("sourceAmplitudeUnit").toString();
            text += QStringLiteral("\n%1 = %2 %3").arg(valueLabel).arg(values[index], 0, 'g', 8).arg(valueUnit);
            if (mode_ == ChartMode::Curve && series_.size() > 1 && index < series_[1].size()) text += QStringLiteral("\nQ = %1 %2").arg(series_[1][index], 0, 'g', 8).arg(valueUnit);
            text += mode_ == ChartMode::Spectrum ? QStringLiteral("\n%1 · N=%2").arg(pinnedCursor_.framePsd ? "驻留帧谱" : "平均谱").arg(values.size()) :
                QStringLiteral("\n%1 · 源样本 #%2").arg(samplePointsVisible_ ? "逐样本" : "显示分箱").arg(sample);
            if (mode_ == ChartMode::Spectrum) setProperty("cursorPowerDb", values[index]);
        } else text += "\n数据不可用 / 正在更新";
            text.replace("dBFS/Hz", property("sourcePowerUnit").toString());
            const auto readout=cursor_overlay::parse(text,mode_==ChartMode::Heatmap);
            const auto layout=cursor_overlay::draw(painter,plot,point,mode_==ChartMode::Heatmap,
                readout,isPinned,isPinned ? std::vector<QRectF>{} : pinLayout.rectangles(),inverseMask);
            const QString prefix=isPinned ? "pinned" : "cursor";
            setProperty((prefix+"Readout").toUtf8(),text); setProperty((prefix+"ReadoutRect").toUtf8(),layout.bounds());
            setProperty((prefix+"ReadoutDetails").toUtf8(),readout.details);
            setProperty((prefix+"XLabelRect").toUtf8(),layout.x); setProperty((prefix+"YLabelRect").toUtf8(),layout.y); setProperty((prefix+"ValueRect").toUtf8(),layout.value);
            return layout;
        };
        for(const QString prefix : {QStringLiteral("pinned"),QStringLiteral("cursor")}) {
            setProperty((prefix+"ReadoutDetails").toUtf8(),QString{});
            for(const auto* suffix : {"XLabelRect","YLabelRect","ValueRect"}) setProperty((prefix+suffix).toUtf8(),QRectF{});
        }
        setProperty("pinnedReadout",QString{}); setProperty("pinnedReadoutRect",QRectF{});
        if (pinnedCursor_.pinned) {
            const double t=pinnedCursor_.sourceSample>=timeSourceRange_.begin ? double(pinnedCursor_.sourceSample-timeSourceRange_.begin)/(timeSourceRange_.end-timeSourceRange_.begin) : -1;
            const double f=(pinnedCursor_.frequencyHz-frequencyRange_.lowerHz)/span;
            const bool visible=mode_==ChartMode::Curve ? t>=0 && t<1 : mode_==ChartMode::Spectrum ? f>=0 && f<1 : t>=0 && t<1 && f>=0 && f<1;
            if (visible) pinLayout=render(QPointF(plot.left()+plot.width()*(mode_==ChartMode::Spectrum ? f : t),mode_==ChartMode::Heatmap ? plot.bottom()-plot.height()*f : plot.center().y()),true);
        }
        setProperty("hoverCursorVisible",hoverPosition_.has_value());
        if (hoverPosition_) render(*hoverPosition_,false); else { setProperty("cursorReadout",QString{}); setProperty("cursorReadoutRect",QRectF{}); }
    }
    std::optional<QPointF> hoverPosition_;
    LinkedCursorState pinnedCursor_;
    TimeRange timeSourceRange_;
    SampleIndex relativeOrigin_ = 0;
    double sourceRate_ = 1;
    std::shared_ptr<const SpectrogramData> spectralData_;
    std::vector<SampleIndex> samplePositions_;

    void updateHover(QPointF point) {
        const bool axes = mode_ == ChartMode::Curve || mode_ == ChartMode::Spectrum || mode_ == ChartMode::Heatmap;
        hoverZone_ = (gesture || pinRequested) ? chart_feedback::zoneAt(point,plotRect()) : chart_feedback::Zone::None;
        if (!axes && hoverZone_ != chart_feedback::Zone::Plot) hoverZone_ = chart_feedback::Zone::None;
        setProperty("interactionZone",static_cast<int>(hoverZone_));
        const bool pan=dragging_ && (dragZone_ == chart_feedback::Zone::XAxis || dragZone_ == chart_feedback::Zone::YAxis);
        setCursor(mode_ == ChartMode::Timeline && hoverZone_ == chart_feedback::Zone::Plot ? Qt::PointingHandCursor : chart_feedback::cursor(hoverZone_,pan,dragging_ && !pan));
    }
    void setStatus(QString details, QString summary = {}) {
        status_ = std::move(details);
        displayStatus_ = summary.isEmpty() ? status_ : std::move(summary);
        setToolTip(status_);
        if(surface_)surface_->setToolTip(status_);
        if (auto* panel = parentWidget()) panel->setToolTip(status_);
    }
    chart_feedback::Zone hoverZone_ = chart_feedback::Zone::None, dragZone_ = chart_feedback::Zone::None;
    ChartMode mode_;
    AcceleratedSurface* surface_ = nullptr;
    std::vector<std::vector<float>> series_;
    std::vector<std::complex<float>> points_;
    FrequencyRange frequencyRange_;
    double frequencyCenterHz_ = 0;
    double yAxisMinimum_ = -32768.0;
    double yAxisMaximum_ = 32768.0;
    QString yAxisLabel_ = QStringLiteral("ADC 计数等效值");
    bool frequencyAbsolute_ = false;
    bool gridVisible_ = true;
    bool samplePointsVisible_ = false;
    int symbolPeriods_ = 2;
    bool geometryDirty_ = true;
    std::uint64_t geometryRevision_ = 0;
    QImage raster_;
    QImage scalarRaster_;
    QString rasterRevision_;
    Palette palette_ = Palette::CoolEditClassic;
    std::vector<int> eyeComponents_;
    QString status_;
    QString displayStatus_;
    QString frequencyAxisText_;
    QString frequencyAxisLabel_ = QStringLiteral("基带频率 (Hz)");
    QString frequencyUnit_ = QStringLiteral("Hz");
    QString timeUnit_ = QStringLiteral("s");
    QString timeAxisLabel_ = QStringLiteral("源文件时间 (s)");
    double frequencyScale_ = 1.0;
    double timeScale_ = 1.0;
    double timeStartSeconds_ = 0.0, timeEndSeconds_ = 1.0;
    std::uint64_t samplePointOrigin_ = 0;
    QPointF dragStart_, dragEnd_;
    double navigationFirst_ = 0.0, navigationLast_ = 1.0;
    bool dragging_ = false;
    bool softwareFallback_ = false;
};

namespace {

QWidget* plotPanel(const QString& title, const QString& id, ChartMode mode, NarrowbandChart*& chart) {
    auto* result = new QWidget; result->setObjectName(id); result->setProperty("uiRole", "chartPanel");
    auto* layout = new QVBoxLayout(result); layout->setContentsMargins(1, 1, 1, 1); layout->setSpacing(0);
    auto* heading = new QWidget; heading->setObjectName(id + "Head"); heading->setProperty("uiRole", "chartHead"); heading->setFixedHeight(30);
    auto* row = new QHBoxLayout(heading); row->setContentsMargins(9, 0, 9, 0);
    auto* name = textLabel(title, id + "Title"); name->setStyleSheet("font-weight:600;color:#d5e6f5;"); row->addWidget(name); row->addStretch();
    layout->addWidget(heading); chart = new NarrowbandChart(mode); chart->setObjectName(id + QStringLiteral("Chart"));
    layout->addWidget(chart, 1); return result;
}

std::uint64_t rateHz(double value) { return value > 0 && value < 4'000'000'000.0 ? static_cast<std::uint64_t>(std::llround(value)) : 0; }
bool mulDiv(std::uint64_t value, std::uint64_t numerator, std::uint64_t denominator, bool ceil,
            std::uint64_t& output) {
    if (!denominator) return false;
    const auto quotient = value / denominator, remainder = value % denominator;
    if (numerator && quotient > std::numeric_limits<std::uint64_t>::max() / numerator) return false;
    const auto whole = quotient * numerator;
    if (numerator && remainder > std::numeric_limits<std::uint64_t>::max() / numerator) return false;
    const auto product = remainder * numerator;
    const auto fraction = product / denominator;
    if (whole > std::numeric_limits<std::uint64_t>::max() - fraction) return false;
    output = whole + fraction;
    if (ceil && product % denominator) { if (output == std::numeric_limits<std::uint64_t>::max()) return false; ++output; }
    return true;
}

QImage spectrumImage(const std::vector<float>& values, QSize size, double referenceDb = -15, double rangeDb = 105) {
    if (values.size() != static_cast<std::size_t>(size.width()) * static_cast<std::size_t>(size.height()) ||
        size.width() < 2 || size.height() < 2) return {};
    QImage image(size, QImage::Format_Grayscale8);
    if (image.isNull()) return {};
    for (int y = 0; y < size.height(); ++y) {
        auto* line = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const double db = values[static_cast<std::size_t>(y) * size.width() + x];
            line[x] = static_cast<uchar>(std::lround(std::clamp((db - referenceDb + rangeDb) / rangeDb, 0.0, 1.0) * 255));
        }
    }
    return image;
}

std::vector<std::complex<float>> syntheticConstellation(std::uint32_t seed = 77) {
    std::mt19937 generator(seed); std::normal_distribution<float> noise(0.f, .055f);
    std::vector<std::complex<float>> points; points.reserve(320);
    for (int i = 0; i < 320; ++i) {
        const int quadrant = (i * 7 + i / 3) % 4;
        const float real = quadrant & 1 ? .7071f : -.7071f;
        const float imag = quadrant & 2 ? .7071f : -.7071f;
        points.emplace_back(real + noise(generator), imag + noise(generator));
    }
    return points;
}
std::vector<std::vector<float>> syntheticEye(int traces, int points, int periods = 2, std::uint32_t seed = 9) {
    std::mt19937 generator(seed); std::normal_distribution<float> noise(0.f, .045f);
    std::vector<std::vector<float>> result(static_cast<std::size_t>(traces));
    for (int row = 0; row < traces; ++row) {
        auto& line = result[static_cast<std::size_t>(row)]; line.resize(static_cast<std::size_t>(points));
        const float phase = static_cast<float>(row) * .137f;
        for (int i = 0; i < points; ++i) {
            const float t = static_cast<float>(i) / std::max(1, points - 1) * 2.f * std::clamp(periods, 1, 8);
            line[static_cast<std::size_t>(i)] = std::sin((t + phase) * 3.14159265f / 2.f) + noise(generator);
        }
    }
    return result;
}

} // namespace

NarrowbandWorkspace::NarrowbandWorkspace(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    setObjectName("narrowbandWorkspace");
    setStyleSheet(R"(QWidget { background:#101d2f;color:#dbe8f8;font-family:'Segoe UI','Microsoft YaHei UI';font-size:12px; }
      QPushButton,QComboBox,QSpinBox { border:1px solid #314963;background:#122239;border-radius:4px;padding:4px 8px; }
      QPushButton:hover { background:#25435e; } QProgressBar { border:1px solid #314963;background:#0c1725;height:11px;text-align:center; }
      QProgressBar::chunk { background:#31acd0; } QPlainTextEdit { background:#0c1725;border:1px solid #29435c;color:#89e0d4;font:13px Consolas; }
    )" + signalstudio::checkboxStyleSheet());
    buildPages();
    analysisService_=std::make_unique<NarrowbandAnalysisService>(sampleCache_);
    auto* analysisTimer=new QTimer(this);analysisTimer->setInterval(20);
    connect(analysisTimer,&QTimer::timeout,this,&NarrowbandWorkspace::installAnalysis);analysisTimer->start();
    recognitionTimer_ = new QTimer(this); recognitionTimer_->setInterval(85);
    connect(recognitionTimer_, &QTimer::timeout, this, &NarrowbandWorkspace::updateRecognition);
    chartWheelTimer_ = new QTimer(this); chartWheelTimer_->setSingleShot(true); chartWheelTimer_->setInterval(220);
    connect(chartWheelTimer_, &QTimer::timeout, this, &NarrowbandWorkspace::finishChartWheel);
    displaySettleTimer_ = new QTimer(this); displaySettleTimer_->setSingleShot(true); displaySettleTimer_->setInterval(60);
    connect(displaySettleTimer_, &QTimer::timeout, this, [this] { requestDisplay(true); });
    refreshFromSession();
}

NarrowbandWorkspace::~NarrowbandWorkspace() { cancelAnalysis();analysisService_.reset();finishChartWheel(); cancelPsd(); cancelWork(); }

void NarrowbandWorkspace::setCallbacks(std::function<void()> locateSource, std::function<void()> editChannel,
                                       std::function<void()> returnToWide, std::function<void(const QString&)> log) {
    locateSource_ = std::move(locateSource); editChannel_ = std::move(editChannel);
    returnToWide_ = std::move(returnToWide); log_ = std::move(log);
}

QString NarrowbandWorkspace::dataStatusText() const {
    return dataStatus_ ? dataStatus_->text() : QStringLiteral("窄带数据状态未知");
}

QString NarrowbandWorkspace::displayDiagnosticsText() const {
    if (psdStatus_.isEmpty() || psdStatus_ == displayDiagnostics_) return displayDiagnostics_;
    return displayDiagnostics_ + "\n" + psdStatus_;
}

void NarrowbandWorkspace::setDataStatus(const QString& summary, const QString& details) {
    displayDiagnostics_ = details.isEmpty() ? summary : details;
    if (dataStatus_) {
        dataStatus_->setText(summary);
        dataStatus_->setToolTip(displayDiagnosticsText());
    }
}

int NarrowbandWorkspace::currentPageIndex() const { return pages_ ? pages_->currentIndex() : -1; }

void NarrowbandWorkspace::activatePageForAcceptance(int index) {
    if (index >= 0 && index < static_cast<int>(pageWidgets_.size()))
        showPage(static_cast<NarrowbandPage>(index));
}

void NarrowbandWorkspace::activatePage(NarrowbandPage page) { showPage(page); }

void NarrowbandWorkspace::setSyncPanelExpanded(bool expanded) {
    if (auto* channel = session_.activeChannel()) channel->syncPanelExpanded = expanded;
    if (syncPanelToggle_) {
        const QSignalBlocker blocker(syncPanelToggle_);
        syncPanelToggle_->setChecked(expanded);
        syncPanelToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    }
    if (syncPanel_) syncPanel_->setVisible(expanded);
    emit displayParametersChanged();
}

bool NarrowbandWorkspace::visibleChartsSettled() const {
    if (displayPending_) return false;
    if (consumesAnalysis(currentPageIndex()) && analysisPending_)return false;
    bool found = false;
    for (const auto* chart : charts_) {
        if (!chart || !chart->isVisible()) continue;
        found = true;
        if (!chart->isDisplaySettled()) return false;
    }
    return found;
}

bool NarrowbandWorkspace::visibleChartsGpuReady() const {
    bool found = false;
    for (const auto* chart : charts_) {
        if (!chart || !chart->isVisible()) continue;
        found = true;
        if (!chart->gpuReady()) return false;
    }
    return found;
}

quint64 NarrowbandWorkspace::visibleTextureUploads() const {
    quint64 total = 0;
    for (const auto* chart : charts_) if (chart && chart->isVisible()) total += chart->textureUploadCount();
    return total;
}

quint64 NarrowbandWorkspace::visibleCompletedFrames() const {
    quint64 total = 0;
    for (const auto* chart : charts_) if (chart && chart->isVisible()) total += chart->completedFrameCount();
    return total;
}

quint64 NarrowbandWorkspace::visibleGpuDataDrawCalls() const {
    quint64 count = 0;
    for (const auto* chart : charts_)
        if (chart && chart->isVisible()) count += chart->gpuDataDrawCallCount();
    return count;
}

quint64 NarrowbandWorkspace::visibleGpuVertexUploads() const {
    quint64 count = 0;
    for (const auto* chart : charts_)
        if (chart && chart->isVisible()) count += chart->gpuVertexUploadCount();
    return count;
}

QString NarrowbandWorkspace::visibleBackendDescription() const {
    QStringList backends;
    for (const auto* chart : charts_) {
        if (chart && chart->isVisible() && !backends.contains(chart->backendDescription())) backends.push_back(chart->backendDescription());
    }
    return backends.join(QStringLiteral("; "));
}

QJsonObject NarrowbandWorkspace::renderStatistics() const {
    const auto cache = displayCacheStats_;
    quint64 inverseCalls=0;
    for(const auto* chart : charts_) if(chart && chart->isVisible()) inverseCalls+=chart->inverseCursorDrawCallCount();
    QJsonObject result{{"pending", displayPending_}, {"requestGeneration", QString::number(requestGeneration_)},
        {"frameGeneration", QString::number(frameGeneration_)}, {"iqCacheBytes", static_cast<qint64>(cache.bytes)},
        {"iqCacheHits", static_cast<qint64>(cache.hits)}, {"iqCacheMisses", static_cast<qint64>(cache.misses)},
        {"gpuVertexUploads", static_cast<qint64>(visibleGpuVertexUploads())}, {"textureUploads", static_cast<qint64>(visibleTextureUploads())},
        {"gpuDataDrawCalls", static_cast<qint64>(visibleGpuDataDrawCalls())}, {"backend", visibleBackendDescription()}};
    result["inverseCursorDrawCalls"]=static_cast<qint64>(inverseCalls);
    result["analysisGeneration"]=QString::number(analysisGeneration_);
    result["analysisPending"]=analysisPending_;
    result["pageIndex"]=currentPageIndex();
    if (const auto* channel=session_.activeChannel()) {
        result["syncPanelExpanded"]=channel->syncPanelExpanded;
        result["constellationView"]=QJsonObject{{"pointBudget",channel->analysis.pointCount},
            {"renderedPoints",static_cast<qint64>(constellationLarge_->property("constellationPointCount").toULongLong())},
            {"autoScale",channel->analysis.constellationAutoScale},
            {"axisMinimum",constellationLarge_->property("iqAxisMinimum").toDouble()},
            {"axisMaximum",constellationLarge_->property("iqAxisMaximum").toDouble()},
            {"visible",constellationLarge_->isVisible()}};
        result["eyeView"]=QJsonObject{{"component",channel->eyeComponent},
            {"traceBudget",channel->eyeTraces},{"pointsPerUi",channel->analysis.eyePointsPerUi},
            {"renderedTraces",static_cast<qint64>(eyeLarge_->property("eyeTraceCount").toULongLong())},
            {"visible",eyeLarge_->isVisible()}};
    }
    if (const auto* channel = session_.activeChannel()) if (const auto spectrum = session_.spectrogram(channel->id)) {
        result["analysisPoints"] = spectrum->plan.points; result["binHz"] = spectrum->plan.binHz;
        result["requiredSeconds"] = spectrum->plan.requiredSeconds; result["noiseBandwidthHz"] = spectrum->plan.noiseBandwidthHz;
        result["paddedFrames"]=static_cast<qint64>(spectrum->paddedFrames);result["effectiveOverlap"]=spectrum->effectiveOverlap;result["effectiveHop"]=QString::number(spectrum->effectiveHop);result["repeatedObservation"]=spectrum->repeatedObservation;
        result["actualFrames"] = static_cast<qint64>(spectrum->frames.size()); result["matrixBytes"] = static_cast<qint64>(spectrum->bytes());
        result["analysisError"] = QString::fromStdString(spectrum->error);
    }
    return result;
}

void NarrowbandWorkspace::invalidateVisibleOverlays() {
    for (auto* chart : charts_) if (chart && chart->isVisible()) chart->invalidateVisibleOverlay();
}

void NarrowbandWorkspace::buildPages() {
    auto* root = new QVBoxLayout(this); root->setContentsMargins(5, 4, 5, 5); root->setSpacing(4);
    auto* header = new QWidget; header->setObjectName("narrowbandHeader"); header->setFixedHeight(36);
    auto* headerRow = new QHBoxLayout(header); headerRow->setContentsMargins(4, 0, 4, 0); headerRow->setSpacing(5);
    headerRow->addWidget(BrandAssets::label("icon",QSize(24,24),header,"narrowbandBrand","primary"));
    // The visual order is independent of the stable legacy numeric API.
    const std::array<QString, 5> titles{"信号观察", "星座图", "调制识别", "解调分析", "眼图"};
    const std::array<QString, 5> ids{"observePage", "modulationPage", "deepLearningPage", "demodulationPage", "eyePage"};
    for (const int i : {0, 1, 4, 2, 3}) {
        pageButtons_[i] = actionButton(titles[i], ids[i], headerRow); pageButtons_[i]->setCheckable(true);
        connect(pageButtons_[i], &QPushButton::clicked, this, [this, i] { showPage(static_cast<NarrowbandPage>(i)); });
    }
    headerRow->addSpacing(10); caption_ = textLabel("未选择窄带通道", "narrowbandCaption");
    caption_->setWordWrap(false); caption_->setMinimumWidth(0); caption_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    caption_->setStyleSheet("color:#85b4d3;"); headerRow->addWidget(caption_, 1);
    auto* locate = actionButton("⌖ 来源", "narrowbandLocateSource", headerRow);
    auto* edit = actionButton("⚙ 通道参数", "narrowbandEditChannel", headerRow);
    auto* close = actionButton("返回宽带", "narrowbandClose", headerRow);
    connect(locate, &QPushButton::clicked, this, [this] { if (locateSource_) locateSource_(); });
    connect(edit, &QPushButton::clicked, this, [this] { if (editChannel_) editChannel_(); });
    connect(close, &QPushButton::clicked, this, [this] { if (returnToWide_) returnToWide_(); });
    root->addWidget(header);
    syncPanelToggle_ = new QToolButton;
    syncPanelToggle_->setObjectName("narrowbandSyncSettingsToggle");
    syncPanelToggle_->setText("同步与参数");
    syncPanelToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    syncPanelToggle_->setCheckable(true); syncPanelToggle_->setArrowType(Qt::RightArrow);
    headerRow->insertWidget(6, syncPanelToggle_);
    connect(syncPanelToggle_, &QToolButton::toggled, this, &NarrowbandWorkspace::setSyncPanelExpanded);

    auto* chartToolbar = new QWidget; chartToolbar->setObjectName("narrowbandChartToolbar");
    auto* chartToolbarRow = new QHBoxLayout(chartToolbar); chartToolbarRow->setContentsMargins(6, 0, 6, 0); chartToolbarRow->setSpacing(6);
    dataStatus_ = textLabel("等待通道数据", "narrowbandDataStatus"); dataStatus_->setStyleSheet("color:#78cbdf;font:10px Consolas;");
    dataStatus_->setMinimumWidth(0); dataStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    waveformMode_ = new QComboBox; waveformMode_->setObjectName("narrowbandWaveformMode");
    waveformMode_->addItems({"I / Q", "幅度 (RMS)", "相位", "幅度包络"});
    displayPsdFft_ = new QComboBox; displayPsdFft_->setObjectName("narrowbandPsdFft");
    for(int order=5;order<=16;++order)displayPsdFft_->addItem(QString::number(1<<order),1<<order); displayPsdFft_->setCurrentIndex(7);
    displayStftFft_ = new QComboBox; displayStftFft_->setObjectName("narrowbandStftFft");
    for (int order = 5; order <= 16; ++order) displayStftFft_->addItem(QString::number(1 << order), 1 << order);
    displayStftFft_->setCurrentIndex(5);
    frequencyMode_ = new QComboBox; frequencyMode_->setObjectName("channelFrequencyMode");
    frequencyMode_->addItems({"基带", "RF"});
    grid_ = new QCheckBox("显示网格"); grid_->setObjectName("narrowbandGrid");
    grid_->setChecked(QSettings().value(QStringLiteral("narrowband/gridVisible"), true).toBool());
    chartToolbarRow->addWidget(textLabel("波形")); chartToolbarRow->addWidget(waveformMode_);
    chartToolbarRow->addWidget(displayPsdFft_); chartToolbarRow->addWidget(displayStftFft_);
    chartToolbarRow->addWidget(frequencyMode_); chartToolbarRow->addWidget(grid_);
    chartToolbarRow->addStretch(); chartToolbarRow->addWidget(dataStatus_, 1);
    root->addWidget(chartToolbar);
    connect(waveformMode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (auto* channel = session_.activeChannel()) {
            channel->waveform = static_cast<NarrowbandWaveform>(index);
            channel->waveformAutoScale = true;
        }
        requestDisplay();
    });
    connect(displayPsdFft_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (auto* channel = session_.activeChannel()) channel->psdFftSize = displayPsdFft_->itemData(index).toInt();
        requestDisplay(); emit displayParametersChanged();
    });
    connect(displayStftFft_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (auto* channel = session_.activeChannel()) channel->stftFftSize = displayStftFft_->itemData(index).toInt();
        requestDisplay(); emit displayParametersChanged();
    });
    connect(frequencyMode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (auto* channel = session_.activeChannel()) channel->absoluteFrequencyLabels = index == 1;
        updateFrequencyAxisLabels(); emit displayParametersChanged();
    });
    auto* body = new QHBoxLayout; body->setContentsMargins(0,0,0,0); body->setSpacing(5);
    root->addLayout(body, 1);
    pages_ = new QStackedWidget; pages_->setObjectName("narrowbandPageStack"); body->addWidget(pages_, 1);
    syncPanel_ = new QScrollArea; syncPanel_->setObjectName("narrowbandSharedSyncPanel");
    syncPanel_->setWidgetResizable(true); syncPanel_->setFrameShape(QFrame::NoFrame);
    syncPanel_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    syncPanel_->setMinimumWidth(330); syncPanel_->setMaximumWidth(410);
    auto* shared = new QWidget; shared->setObjectName("narrowbandSharedSyncContents");
    auto* sharedLayout = new QVBoxLayout(shared); sharedLayout->setContentsMargins(8,6,8,6);
    syncPanel_->setWidget(shared); body->addWidget(syncPanel_); syncPanel_->hide();
    connect(pages_,&QStackedWidget::currentChanged,this,&NarrowbandWorkspace::pageChanged);
    auto makePage = [this](int index) {
        auto* page = new QWidget; page->setObjectName(QStringLiteral("nbPage%1").arg(index));
        pageWidgets_[static_cast<std::size_t>(index)] = page; pages_->addWidget(page); return page;
    };

    auto* observe = makePage(0); auto* observeLayout = new QVBoxLayout(observe);
    observeLayout->setContentsMargins(0, 0, 0, 0); observeLayout->setSpacing(5);
    auto* observeSplitter = new QSplitter(Qt::Vertical); observeSplitter->setObjectName("narrowbandObserveSplitter");
    observeSplitter->setChildrenCollapsible(false); observeSplitter->setHandleWidth(5);
    auto* observeTop = new QWidget; observeTop->setObjectName("narrowbandObserveTopRow");
    auto* observeTopLayout = new QHBoxLayout(observeTop); observeTopLayout->setContentsMargins(0, 0, 0, 0); observeTopLayout->setSpacing(5);
    observeTopLayout->addWidget(plotPanel("波形", "narrowbandWaveformPanel", ChartMode::Curve, waveform_), 1);
    observeTopLayout->addWidget(plotPanel("功率谱密度", "narrowbandPsdPanel", ChartMode::Spectrum, psd_), 1);
    observeSplitter->addWidget(observeTop);
    observeSplitter->addWidget(plotPanel("时频图", "narrowbandStftPanel", ChartMode::Heatmap, stft_));
    observeSplitter->setStretchFactor(0, 1); observeSplitter->setStretchFactor(1, 1); observeSplitter->setSizes({320, 640});
    const auto observeState = QSettings().value(QStringLiteral("narrowband/observeSplitterState")).toByteArray();
    if (!observeState.isEmpty()) observeSplitter->restoreState(observeState);
    connect(observeSplitter, &QSplitter::splitterMoved, this, [observeSplitter] {
        QSettings().setValue(QStringLiteral("narrowband/observeSplitterState"), observeSplitter->saveState());
    });
    observeLayout->addWidget(observeSplitter);

    auto* modulation = makePage(1); auto* modulationLayout = new QGridLayout(modulation);
    modulationLayout->setContentsMargins(0, 0, 0, 0); modulationLayout->setSpacing(5);
    auto* constellationPanel=plotPanel("星座 · 真实 IQ", "narrowbandConstellationLarge", ChartMode::Scatter, constellationLarge_);
    modulationLayout->addWidget(constellationPanel, 0, 0);
    auto* chartParameters=new QWidget(modulation);chartParameters->hide();
    auto* pointCount=new QComboBox(chartParameters);pointCount->setObjectName("constellationPointBudget");
    for(const int n:{256,1024,4096,20000})pointCount->addItem(QString::number(n)+" 点",n);
    pointCount->setCurrentIndex(2);
    auto* constellationStage=new QComboBox(chartParameters);constellationStage->setObjectName("constellationAnalysisStage");
    constellationStage->addItems({"原始 IQ","CFO 校正","匹配滤波","定时符号","载波恢复"});
    connect(constellationStage,&QComboBox::currentIndexChanged,this,[this](int value){if(auto* c=session_.activeChannel()){c->analysis.constellationStage=static_cast<AnalysisStage>(value);enableStage(c->analysis,c->analysis.constellationStage);}refreshFromSession();requestAnalysis(true);});
    auto* autoScale=new QCheckBox("自动缩放",chartParameters);autoScale->setObjectName("constellationAutoScale");autoScale->setChecked(true);
    auto* constellationSettings=new QWidget(chartParameters);constellationSettings->setObjectName("narrowbandConstellationDisplaySettings");
    auto* constellationForm=new QFormLayout(constellationSettings);constellationForm->setContentsMargins(4,4,4,4);constellationForm->setSpacing(8);
    auto* axisMin=new ParameterDoubleSpinBox;axisMin->setObjectName("constellationAxisMinimum");axisMin->setRange(-1e12,1e12);axisMin->setDecimals(6);axisMin->setValue(-1);axisMin->setEnabled(false);
    auto* axisMax=new ParameterDoubleSpinBox;axisMax->setObjectName("constellationAxisMaximum");axisMax->setRange(-1e12,1e12);axisMax->setDecimals(6);axisMax->setValue(1);axisMax->setEnabled(false);
    auto* dc=new QCheckBox("去 DC");dc->setObjectName("analysisRemoveDc");constellationForm->addRow(dc);
    auto* norm=new QCheckBox("功率归一化");norm->setObjectName("analysisNormalizePower");constellationForm->addRow(norm);
    constellationForm->addRow("I/Q 轴下限",axisMin);constellationForm->addRow("I/Q 轴上限",axisMax);
    connect(pointCount,&QComboBox::currentIndexChanged,this,[this,pointCount]{if(auto* c=session_.activeChannel())c->analysis.pointCount=pointCount->currentData().toInt();renderAnalysis();emit displayParametersChanged();});
    connect(autoScale,&QCheckBox::toggled,this,[this,axisMin,axisMax](bool enabled){axisMin->setEnabled(!enabled);axisMax->setEnabled(!enabled);if(auto* c=session_.activeChannel())c->analysis.constellationAutoScale=enabled;renderAnalysis();emit displayParametersChanged();});
    for(auto* axis:{axisMin,axisMax})connect(axis,&QDoubleSpinBox::valueChanged,this,[this,axisMin,axisMax]{
      if(axisMin->value()>=axisMax->value()) {setParameterError(axisMin,"I/Q 轴下限必须小于上限。");return;}
      setParameterError(axisMin,{});if(auto* c=session_.activeChannel()){c->constellationMinimum=axisMin->value();c->constellationMaximum=axisMax->value();}renderAnalysis();emit displayParametersChanged();
    });
    connect(dc,&QCheckBox::toggled,this,[this](bool v){if(auto* c=session_.activeChannel())c->analysis.removeDc=v;requestAnalysis(true);});
    connect(norm,&QCheckBox::toggled,this,[this](bool v){if(auto* c=session_.activeChannel())c->analysis.normalizePower=v;requestAnalysis(true);});
    auto* eyePanel=plotPanel("眼图 · 真实 IQ", "narrowbandEyeLarge", ChartMode::Eye, eyeLarge_);
    // Reparent into its own page after the legacy-indexed pages are created.
    auto* eyeStage=new QComboBox(chartParameters);eyeStage->setObjectName("eyeAnalysisStage");eyeStage->addItems({"原始 DDC IQ","CFO 校正后","RRC 假设滤波","定时对齐","载波恢复后"});
    auto* eyeSettings=new QWidget(chartParameters);eyeSettings->setObjectName("narrowbandEyeDisplaySettings");
    auto* eyeForm=new QFormLayout(eyeSettings);eyeForm->setContentsMargins(4,4,4,4);eyeForm->setSpacing(8);
    auto* eyeDensity=new QComboBox;eyeDensity->setObjectName("eyeDrawingDensity");for(int n:{4,8,16})eyeDensity->addItem(QString("%1 点/UI").arg(n),n);eyeDensity->setCurrentIndex(2);eyeForm->addRow("绘图密度",eyeDensity);
    auto* eyeOffset=new ParameterDoubleSpinBox;eyeOffset->setObjectName("eyeTimingOffsetUi");eyeOffset->setRange(-2,2);eyeOffset->setDecimals(4);eyeOffset->setSingleStep(.05);eyeOffset->setSuffix(" UI");eyeForm->addRow("折叠偏移",eyeOffset);
    auto* rolloff=new ParameterDoubleSpinBox;rolloff->setObjectName("analysisRrcRolloff");rolloff->setRange(.05,1);rolloff->setDecimals(2);rolloff->setSingleStep(.05);rolloff->setValue(.35);rolloff->setPrefix("α ");eyeForm->addRow("RRC 滚降",rolloff);
    connect(eyeStage,&QComboBox::currentIndexChanged,this,[this](int index){if(auto* c=session_.activeChannel()){c->analysis.eyeStage=static_cast<AnalysisStage>(index);enableStage(c->analysis,c->analysis.eyeStage);}refreshFromSession();requestAnalysis(true);});
    connect(eyeDensity,&QComboBox::currentIndexChanged,this,[this,eyeDensity]{if(auto* c=session_.activeChannel())c->analysis.eyePointsPerUi=eyeDensity->currentData().toInt();renderAnalysis();emit displayParametersChanged();});
    connect(eyeOffset,&QDoubleSpinBox::valueChanged,this,[this](double v){if(auto* c=session_.activeChannel())c->analysis.eyeTimingOffsetUi=v;requestAnalysis(true);});
    connect(rolloff,&QDoubleSpinBox::valueChanged,this,[this](double v){if(auto* c=session_.activeChannel())c->analysis.rolloff=v;requestAnalysis(true);});
    modulationLayout->addWidget(plotPanel("时频图", "narrowbandModulationStft", ChartMode::Heatmap, modulationStft_), 1, 0);
    modulationLayout->setColumnStretch(0, 1);
    auto* modulationControls = new QWidget; auto* modulationForm = new QFormLayout(modulationControls);
    modulationForm->setContentsMargins(0, 4, 0, 4);
    modulationForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* modulationActions = new QWidget; auto* modulationRow = new QHBoxLayout(modulationActions);
    modulationRow->setContentsMargins(0,0,0,0); modulationRow->setSpacing(4);
    auto* modulationType = new QComboBox; modulationType->setObjectName("modulationFormat");
    modulationType->addItems({"未知（分析假设）", "BPSK", "QPSK", "16QAM", "FSK（不适用）", "OFDM（不适用）", "GMSK（不适用）", "M-PSK", "M-QAM", "M-APSK"});
    auto* order=new QComboBox;order->setObjectName("analysisModulationOrder");
    for(int m:{2,4,8,16,32,64,128,256})order->addItem(QString("M=%1").arg(m),m);
    order->setToolTip("PSK：2–256；QAM：4–4096（8 为矩形，32/128/512/2048 为交叉形）；APSK：8–256，可编辑环配置。");
    auto* symbolRateMode=new QComboBox;symbolRateMode->setObjectName("analysisSymbolRateMode");symbolRateMode->addItems({"Rs 待确认","手工 Rs","自动估计 Rs"});
    auto* symbolRate = new ParameterDoubleSpinBox; symbolRate->setObjectName("symbolRate"); symbolRate->setRange(1, 4e9); symbolRate->setDecimals(3); symbolRate->setValue(250'000); symbolRate->setSuffix(" Sym/s");
    // The main window discovers these singleton editors before hosting them
    // in the eye chart's control strip. Keep them owned while still hidden.
    eyeComponent_ = new QComboBox(chartParameters); eyeComponent_->setObjectName("eyeComponent"); eyeComponent_->addItems({"I + Q", "I 分量", "Q 分量"});
    eyePeriods_ = new QComboBox(chartParameters); eyePeriods_->setObjectName("eyePeriods");for(int n:{1,2,3,4})eyePeriods_->addItem(QString::number(n),n);
    eyeTraces_ = new QComboBox(chartParameters); eyeTraces_->setObjectName("eyeTraces");for(int n:{32,64,128,256})eyeTraces_->addItem(QString("%1 条").arg(n),n);eyeTraces_->setCurrentIndex(1);
    auto* sync = new QCheckBox("定时恢复"); sync->setObjectName("analysisTimingRecovery");
    connect(sync,&QCheckBox::toggled,this,[this](bool value){if(auto* c=session_.activeChannel()){auto& s=c->analysis;s.timingRecovery=value;if(!value){s.carrierRecovery=false;demoteStage(s,AnalysisStage::TimingAligned,s.matchedFilter?AnalysisStage::MatchedFiltered:AnalysisStage::RawIq);}}refreshFromSession();requestAnalysis(true);});
    modulationForm->addRow("调制假设", modulationType); modulationForm->addRow("阶数", order);
    modulationForm->addRow("符号率模式", symbolRateMode); modulationForm->addRow("符号率", symbolRate);
    modulationForm->addRow(sync); sharedLayout->addWidget(modulationControls);
    auto* run=actionButton("分析", "runNarrowbandAnalysis",modulationRow);
    auto* cancel=actionButton("取消", "cancelNarrowbandAnalysis",modulationRow);
    auto* recover=actionButton("同步恢复", "recoverNarrowbandSignal",modulationRow);recover->setProperty("uiRole","primary");
    sharedLayout->addWidget(modulationActions);
    connect(run,&QPushButton::clicked,this,[this,symbolRateMode]{
      if(auto* c=session_.activeChannel();c && c->analysis.symbolRateMode==SymbolRateMode::Unknown){symbolRateMode->setCurrentIndex(2);return;}
      requestAnalysis(true);
    });
    connect(cancel,&QPushButton::clicked,this,[this]{cancelAnalysis(true);if(modulationStatus_)setAnalysisStatus("分析已取消。");});
    modulationStatus_=textLabel("真实 IQ 分析待运行", "modulationAnalysisStatus");
    modulationStatus_->setWordWrap(true); sharedLayout->addWidget(modulationStatus_);
    auto* rateRowWidget=new QWidget;auto* rateRow=new QVBoxLayout(rateRowWidget);rateRow->setContentsMargins(0,0,0,0);
    rateRow->addWidget(textLabel("Rs 候选（评分非概率）"));
    auto* candidates=new QComboBox;candidates->setObjectName("analysisRateCandidates");candidates->setMinimumContentsLength(30);rateRow->addWidget(candidates,1);
    auto* adopt=actionButton("采用为手工 Rs", "adoptSymbolRateCandidate",rateRow);rateRow->addWidget(adopt);
    sharedLayout->addWidget(rateRowWidget);
    auto* syncSettings=new QWidget;syncSettings->setObjectName("narrowbandSyncSettings");auto* syncLayout=new QVBoxLayout(syncSettings);syncLayout->setContentsMargins(0,2,0,2);
    auto* left=new QFormLayout;auto* middle=new QFormLayout;auto* right=new QFormLayout;
    for (auto* form : {left, middle, right}) {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    syncLayout->addLayout(left);syncLayout->addLayout(middle);syncLayout->addLayout(right);
    auto* cfoMode=new QComboBox;cfoMode->setObjectName("analysisCfoMode");cfoMode->addItems({"关闭 CFO 校正","自动 CFO（分析假设）","手工 CFO"});left->addRow("频偏校正",cfoMode);
    auto* cfo=new ParameterDoubleSpinBox;cfo->setObjectName("analysisManualCfoHz");cfo->setRange(-2e9,2e9);cfo->setDecimals(3);cfo->setSuffix(" Hz");left->addRow("手工频偏",cfo);
    auto* matched=new QCheckBox("RRC 匹配滤波（滚降为假设）");matched->setObjectName("analysisMatchedFilter");left->addRow(matched);
    auto* carrier=new QCheckBox("载波相位恢复（所选模板）");carrier->setObjectName("analysisCarrierRecovery");middle->addRow(carrier);
    auto* timingBw=new ParameterDoubleSpinBox;timingBw->setObjectName("analysisTimingLoopBandwidth");timingBw->setRange(.0001,.1);timingBw->setDecimals(4);timingBw->setSingleStep(.0001);timingBw->setValue(.002);middle->addRow("定时环带宽／符号率",timingBw);
    auto* carrierBw=new ParameterDoubleSpinBox;carrierBw->setObjectName("analysisCarrierLoopBandwidth");carrierBw->setRange(.0001,.1);carrierBw->setDecimals(4);carrierBw->setSingleStep(.0001);carrierBw->setValue(.01);middle->addRow("载波环带宽／符号率",carrierBw);
    auto* phase=new ParameterDoubleSpinBox;phase->setObjectName("analysisManualPhaseRadians");phase->setRange(-20*std::numbers::pi,20*std::numbers::pi);phase->setDecimals(5);phase->setSuffix(" rad");right->addRow("手工 CFO 初始相位",phase);
    auto* span=new ParameterIntSpinBox;span->setObjectName("analysisRrcSpan");span->setRange(2,32);span->setValue(10);right->addRow("RRC 跨度／符号",span);
    auto* budget=new QComboBox;budget->setObjectName("analysisSampleBudget");for(int n:{1,2,4,8})budget->addItem(QString("%1 Mi 复样本").arg(n),n*1024*1024);right->addRow("分析预算（256 MiB 工作区）",budget);
    auto* rings=new ParameterTextEdit;rings->setObjectName("analysisApskRings");rings->setPlaceholderText("默认环；可填 4:1:45,12:2.85:15");
    rings->setToolTip("每环 点数:相对半径:相位角度，逗号分隔。半径递增、点数之和等于 M；空值使用分析预设，不代表特定编码标准。");
    right->addRow("APSK 环／半径／角度",rings);
    rings->apply=[this,rings](const QString& text){
      auto* c=session_.activeChannel();if(!c)return false;
      auto settings=c->analysis;settings.apskRings=text.trimmed().toStdString();ConstellationDefinition definition;std::string error;
      if(settings.hypothesis==ModulationHypothesis::Apsk && !makeConstellation(settings,definition,error)){setParameterError(rings,str(error));return false;}
      c->analysis.apskRings=settings.apskRings;requestAnalysis(true);return true;
    };
    sharedLayout->addWidget(syncSettings); sharedLayout->addStretch();
    connect(cfoMode,&QComboBox::currentIndexChanged,this,[this](int value){if(auto* c=session_.activeChannel())c->analysis.cfoMode=static_cast<CfoMode>(value);requestAnalysis(true);});
    connect(cfo,&QDoubleSpinBox::valueChanged,this,[this](double value){if(auto* c=session_.activeChannel())c->analysis.manualCfoHz=value;requestAnalysis(true);});
    connect(matched,&QCheckBox::toggled,this,[this](bool value){if(auto* c=session_.activeChannel()){c->analysis.matchedFilter=value;if(!value)for(auto* stage:{&c->analysis.constellationStage,&c->analysis.eyeStage})if(*stage==AnalysisStage::MatchedFiltered)*stage=AnalysisStage::RawIq;}refreshFromSession();requestAnalysis(true);});
    connect(carrier,&QCheckBox::toggled,this,[this](bool value){if(auto* c=session_.activeChannel()){c->analysis.carrierRecovery=value;if(value)c->analysis.timingRecovery=true;else demoteStage(c->analysis,AnalysisStage::CarrierRecovered,c->analysis.timingRecovery?AnalysisStage::TimingAligned:AnalysisStage::RawIq);}refreshFromSession();requestAnalysis(true);});
    connect(timingBw,&QDoubleSpinBox::valueChanged,this,[this](double value){if(auto* c=session_.activeChannel())c->analysis.timingLoopBandwidth=value;requestAnalysis(true);});
    connect(carrierBw,&QDoubleSpinBox::valueChanged,this,[this](double value){if(auto* c=session_.activeChannel())c->analysis.carrierLoopBandwidth=value;requestAnalysis(true);});
    connect(phase,&QDoubleSpinBox::valueChanged,this,[this](double value){if(auto* c=session_.activeChannel())c->analysis.manualPhaseRadians=value;requestAnalysis(true);});
    connect(span,&QSpinBox::valueChanged,this,[this](int value){if(auto* c=session_.activeChannel())c->analysis.filterSpanSymbols=value;requestAnalysis(true);});
    connect(budget,&QComboBox::currentIndexChanged,this,[this,budget]{if(auto* c=session_.activeChannel())c->analysis.sampleBudget=static_cast<std::size_t>(budget->currentData().toInt());requestAnalysis(true);});
    recover->setToolTip("按当前 PSK/QAM/APSK 及阶数启用 CFO、RRC、Gardner 和载波恢复；待确认 Rs 时先估计，保留手工 CFO/Rs。");
    connect(recover,&QPushButton::clicked,this,[this]{
      auto* c=session_.activeChannel();if(!c)return;
      if(!isSingleCarrierHypothesis(c->analysis.hypothesis)){setAnalysisStatus("无法同步：请先选择 PSK、QAM 或 APSK 调制假设及阶数；仅估计 Rs 不等于识别调制。");findChild<QComboBox*>("modulationFormat")->setFocus();return;}
      if(c->analysis.symbolRateMode==SymbolRateMode::Unknown){c->analysis.symbolRateMode=SymbolRateMode::Estimated;c->analysis.symbolRateHz=0;}
      if(c->analysis.cfoMode==CfoMode::Off)c->analysis.cfoMode=CfoMode::Automatic;
      c->analysis.matchedFilter=true;c->analysis.timingRecovery=true;c->analysis.carrierRecovery=true;
      c->analysis.constellationStage=AnalysisStage::CarrierRecovered;c->analysis.eyeStage=AnalysisStage::CarrierRecovered;
      refreshFromSession();requestAnalysis(true);
    });
    connect(adopt,&QPushButton::clicked,this,[this,candidates,symbolRateMode,symbolRate] {
      auto* c=session_.activeChannel();const auto* f=session_.activeFile();if(!c || !f || candidates->currentIndex()<0 || !analysisResult_ ||
        !analysisIdentityCurrent(analysisResult_->identity,f->metadata,*c,session_.projectGeneration(),analysisGeneration_))return;
      const auto rate=candidates->currentData().toDouble();if(!(rate>0))return;
      c->analysis.symbolRateMode=SymbolRateMode::Manual;c->analysis.symbolRateHz=rate;c->symbolRate=rate;
      {QSignalBlocker b(symbolRateMode);symbolRateMode->setCurrentIndex(1);}
      {QSignalBlocker b(symbolRate);displayParameterValue(symbolRate,rate);}requestAnalysis(true);
    });
    modulationLayout->setRowStretch(0, 3); modulationLayout->setRowStretch(1, 2);
    modulationType->setToolTip("仅为单载波分析假设，不是自动识别结果。");
    connect(modulationType, &QComboBox::currentIndexChanged, this, [this,modulationType](int index) {
        if(auto* c=session_.activeChannel()){c->analysis.hypothesis=static_cast<ModulationHypothesis>(index);if(index>=7){c->analysis.modulationOrder=index==7?8:index==8?32:16;c->analysis.apskRings.clear();}}
        modulationType->setToolTip("分析假设：" + modulationType->currentText() + "；不是识别结果。");refreshFromSession();requestAnalysis(true);
    });
    connect(order,&QComboBox::currentIndexChanged,this,[this,order](int){if(auto* c=session_.activeChannel()){c->analysis.modulationOrder=order->currentData().toInt();c->analysis.apskRings.clear();}refreshFromSession();requestAnalysis(true);});
    connect(symbolRate, &QDoubleSpinBox::valueChanged, this, [this,symbolRateMode](double value) {
        if (auto* channel = session_.activeChannel()) {channel->symbolRate = value;channel->analysis.symbolRateHz=value;channel->analysis.symbolRateMode=SymbolRateMode::Manual;}
        {QSignalBlocker block(symbolRateMode);symbolRateMode->setCurrentIndex(1);}requestAnalysis(true);
    });
    connect(symbolRateMode,&QComboBox::currentIndexChanged,this,[this,symbolRate](int index) {
      if(auto* c=session_.activeChannel()){c->analysis.symbolRateMode=static_cast<SymbolRateMode>(index);if(index==1){c->analysis.symbolRateHz=symbolRate->value();c->symbolRate=symbolRate->value();}else if(index==2)c->analysis.symbolRateHz=0;}
      requestAnalysis(true);
    });
    auto refreshEyeControls = [this] {
        const auto* channel = session_.activeChannel(); if (!channel) return;
        const auto traces = channel->eyeTraces;
        if (eyeDemod_) eyeDemod_->setEye(syntheticEye(traces, 192, channel ? channel->eyePeriods : 2, 17), "合成示例 · 未执行定时同步", "合成示例");
        renderAnalysis(); emit displayParametersChanged();
    };
    connect(eyeComponent_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (auto* channel = session_.activeChannel()) channel->eyeComponent = index;
        renderAnalysis(); emit displayParametersChanged();
    });
    connect(eyePeriods_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (auto* channel = session_.activeChannel()) channel->eyePeriods = eyePeriods_->currentData().toInt();
        requestAnalysis(true);
    });
    connect(eyeTraces_, &QComboBox::currentIndexChanged, this, [this, refreshEyeControls](int) {
        if (auto* channel = session_.activeChannel()) channel->eyeTraces = eyeTraces_->currentData().toInt();
        refreshEyeControls();
    });

    auto* deep = makePage(2); auto* deepLayout = new QHBoxLayout(deep);
    deepLayout->setContentsMargins(0, 0, 0, 0); deepLayout->setSpacing(5);
    auto* configPanel = new QWidget; configPanel->setObjectName("demoRecognitionConfig"); configPanel->setMinimumWidth(260); configPanel->setMaximumWidth(360);
    auto* configLayout = new QVBoxLayout(configPanel); configLayout->setContentsMargins(10, 8, 10, 8); configLayout->setSpacing(7);
    auto* configScroll = new QScrollArea; configScroll->setObjectName("recognitionConfigScroll");
    configScroll->setWidgetResizable(true); configScroll->setFrameShape(QFrame::NoFrame);
    configScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* configContents = new QWidget; configContents->setObjectName("recognitionConfigContents");
    auto* fieldsLayout = new QVBoxLayout(configContents); fieldsLayout->setContentsMargins(0, 0, 6, 0); fieldsLayout->setSpacing(8);
    auto* configTitle = textLabel("识别配置"); configTitle->setStyleSheet("font-size:14px;font-weight:650;"); fieldsLayout->addWidget(configTitle);
    auto* unavailable = textLabel("真实调制识别不可用：未部署识别模型与推理后端。", "recognitionUnavailable");
    unavailable->setProperty("modelAvailable", false); fieldsLayout->addWidget(unavailable);
    auto* realRecognition = actionButton("真实识别不可用", "runRealRecognition", fieldsLayout);
    realRecognition->setEnabled(false); realRecognition->setToolTip(unavailable->text());
    auto* disclaimer = textLabel("合成示例：分数与进度仅用于演示，未执行真实模型推理。", "recognitionSyntheticDisclosure");
    disclaimer->setToolTip("选择模型文件只记录文件名，不加载或执行权重；本页结果不代表真实信号识别能力。");
    disclaimer->setStyleSheet("color:#e5bf77;background:#332b1a;padding:7px;font-size:11px;"); fieldsLayout->addWidget(disclaimer);
    auto* form = new QFormLayout; form->setVerticalSpacing(6); form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    model_ = new QComboBox; model_->setObjectName("recognitionModel");
    model_->addItems({"ResNet18 · v1.0", "CLDNN · v1.0", "Transformer · v1.0"});
    fftSize_ = new QComboBox; fftSize_->setObjectName("recognitionFrameLength"); fftSize_->addItems({"1024", "2048", "4096", "8192"}); fftSize_->setCurrentText("2048");
    recognitionNormalization_ = new QComboBox; recognitionNormalization_->setObjectName("recognitionNormalization"); recognitionNormalization_->addItems({"IQ RMS 归一化", "峰值归一化", "不归一化"});
    recognitionOverlap_ = new QComboBox; recognitionOverlap_->setObjectName("recognitionOverlap"); recognitionOverlap_->addItems({"0%", "25%", "50%", "75%", "90%"}); recognitionOverlap_->setCurrentIndex(2);
    recognitionScope_ = new QComboBox; recognitionScope_->setObjectName("recognitionScope"); recognitionScope_->addItems({"当前可见时间窗", "来源标记时段", "通道完整时段"});
    recognitionSampleRate_ = new ParameterDoubleSpinBox; recognitionSampleRate_->setObjectName("recognitionSampleRate"); recognitionSampleRate_->setRange(.1, 1000); recognitionSampleRate_->setDecimals(2); recognitionSampleRate_->setValue(4); recognitionSampleRate_->setSuffix(" MS/s");
    recognitionThreshold_ = new ParameterDoubleSpinBox; recognitionThreshold_->setObjectName("recognitionThreshold"); recognitionThreshold_->setRange(.01, .99); recognitionThreshold_->setSingleStep(.05); recognitionThreshold_->setValue(.5);
    auto* device = new QComboBox; device->setObjectName("recognitionDevice"); device->addItems({"演示无设备", "CPU · 设计选项", "GPU · 设计选项"});
    form->addRow("模型", model_); form->addRow("窗口点数", fftSize_); form->addRow("归一化", recognitionNormalization_); form->addRow("重叠率", recognitionOverlap_);
    form->addRow("分析范围", recognitionScope_); form->addRow("目标采样率", recognitionSampleRate_); form->addRow("类别阈值", recognitionThreshold_); form->addRow("设备", device);
    fieldsLayout->addLayout(form);
    recognitionAllowResample_ = new QCheckBox("允许预处理重采样（演示）"); recognitionAllowResample_->setObjectName("recognitionAllowResample"); recognitionAllowResample_->setChecked(true);
    recognitionAllowResample_->setToolTip("仅校验演示配置中的目标采样率；识别演示不执行真实模型预处理。"); fieldsLayout->addWidget(recognitionAllowResample_);
    auto* manageModel = actionButton("选择模型文件…", "recognitionModelManager", fieldsLayout); manageModel->setProperty("uiRole", "outline");
    manageModel->setToolTip("演示只记录文件名，不加载或执行模型权重");
    connect(manageModel, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "选择模型文件名（演示不会加载权重）", {}, "Model files (*.onnx *.pt *.pth *.engine);;All files (*.*)");
        if (!path.isEmpty()) {
            selectedModelFile_ = QFileInfo(path).fileName();
            if (auto* button = findChild<QPushButton*>("recognitionModelManager"))
                button->setToolTip("已记录：" + selectedModelFile_ + "；未加载或执行权重");
        }
    });
    fieldsLayout->addStretch(); configScroll->setWidget(configContents); configLayout->addWidget(configScroll, 1);
    auto* taskControls = new QWidget; taskControls->setObjectName("recognitionTaskControls");
    auto* taskLayout = new QVBoxLayout(taskControls); taskLayout->setContentsMargins(0, 0, 0, 0); taskLayout->setSpacing(7);
    auto* runRow = new QWidget; auto* runLayout = new QHBoxLayout(runRow); runLayout->setContentsMargins(0, 0, 0, 0);
    runRecognition_ = actionButton("运行识别演示", "runRecognition", runLayout); runRecognition_->setProperty("uiRole", "primary");
    stopRecognition_ = actionButton("停止", "stopRecognition", runLayout); stopRecognition_->setEnabled(false);
    connect(runRecognition_, &QPushButton::clicked, this, [this] { startDemoRecognition(); });
    connect(stopRecognition_, &QPushButton::clicked, this, [this] { stopDemoRecognition(); });
    taskLayout->addWidget(runRow);
    recognitionProgress_ = new QProgressBar; recognitionProgress_->setObjectName("recognitionProgress"); recognitionProgress_->setRange(0, 100); recognitionProgress_->setValue(0); taskLayout->addWidget(recognitionProgress_);
    analysisStatus_ = textLabel("待运行 · 合成示例", "recognitionStatus"); analysisStatus_->setStyleSheet("color:#83c8d8;font-size:11px;"); taskLayout->addWidget(analysisStatus_);
    configLayout->addWidget(taskControls); deepLayout->addWidget(configPanel);
    auto* resultSide = new QWidget; auto* resultLayout = new QVBoxLayout(resultSide); resultLayout->setContentsMargins(0, 0, 0, 0); resultLayout->setSpacing(5);
    auto* resultCards = new QWidget; auto* cardRow = new QHBoxLayout(resultCards); cardRow->setContentsMargins(0, 0, 0, 0); cardRow->setSpacing(5);
    const std::array<QString, 4> statTitles{"信号片段", "主类别", "平均置信度", "任务状态"};
    for (std::size_t i = 0; i < recognitionStats_.size(); ++i) {
        auto* card = textLabel(statTitles[i] + "\n" + (i == 3 ? QStringLiteral("待运行") : QStringLiteral("—")), "recognitionStatCard");
        recognitionStats_[i] = card; card->setMinimumWidth(0); card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        card->setToolTip("合成识别示例统计；运行完成后按实际生成的演示结果更新，不代表真实推理。");
        card->setStyleSheet("background:#16253a;border:1px solid #2d4964;padding:9px;color:#c9def0;"); cardRow->addWidget(card, 1);
    }
    resultLayout->addWidget(resultCards);
    recognitionTop5_ = textLabel("Top-5 候选 · 等待合成示例结果", "recognitionTop5");
    recognitionTop5_->setStyleSheet("background:#111f32;border:1px solid #2c455e;padding:10px;color:#c2d7e8;"); resultLayout->addWidget(recognitionTop5_);
    resultLayout->addWidget(plotPanel("置信度时间轴 · 合成示例", "recognitionTimelinePanel", ChartMode::Timeline, timeline_), 1);
    resultLayout->addWidget(plotPanel("通道时频图", "recognitionStftPanel", ChartMode::Heatmap, recognitionStft_), 2);
    auto* exportRow = new QWidget; auto* exportLayout = new QHBoxLayout(exportRow); exportLayout->setContentsMargins(0, 0, 0, 0); exportLayout->addStretch();
    auto* exportJson = actionButton("导出 JSON", "exportRecognitionJson", exportLayout); exportJson->setProperty("uiRole", "outline");
    connect(exportJson, &QPushButton::clicked, this, &NarrowbandWorkspace::exportRecognition); resultLayout->addWidget(exportRow);
    deepLayout->addWidget(resultSide, 1);
    connect(recognitionThreshold_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        updateRecognitionDetail();
        if (analysisStatus_ && !recognitionActive_ && !recognitionSegments_.empty()) {
            analysisStatus_->setText(recognitionResultStale_ ? QStringLiteral("上次合成结果已过期") : QStringLiteral("合成示例完成 · 阈值 %1").arg(value, 0, 'f', 2));
            analysisStatus_->setToolTip("阈值只重判未知类别，已生成的合成分数不重新计算。");
        }
    });
    auto invalidateRecognition = [this] { markRecognitionStale("识别配置已变化"); };
    connect(model_, &QComboBox::currentIndexChanged, this, invalidateRecognition);
    connect(fftSize_, &QComboBox::currentIndexChanged, this, invalidateRecognition);
    connect(recognitionNormalization_, &QComboBox::currentIndexChanged, this, invalidateRecognition);
    connect(recognitionOverlap_, &QComboBox::currentIndexChanged, this, invalidateRecognition);
    connect(recognitionScope_, &QComboBox::currentIndexChanged, this, invalidateRecognition);
    connect(recognitionSampleRate_, &QDoubleSpinBox::valueChanged, this, [invalidateRecognition](double) { invalidateRecognition(); });
    connect(recognitionAllowResample_, &QCheckBox::toggled, this, invalidateRecognition);

    auto* demod = makePage(3); auto* demodLayout = new QVBoxLayout(demod); demodLayout->setContentsMargins(0, 0, 0, 0); demodLayout->setSpacing(5);
    auto* demodUnavailable = textLabel("真实解调位流不可用：尚未部署解调后端。以下为合成示例；同步恢复状态在公共参数面板查看。", "demodulationUnavailable");
    demodUnavailable->setProperty("modelAvailable", false); demodLayout->addWidget(demodUnavailable);
    auto* demodCharts = new QGridLayout; demodCharts->setSpacing(5);
    demodCharts->addWidget(plotPanel("解调星座 · 合成示例", "demodConstellationPanel", ChartMode::Scatter, constellationDemod_), 0, 0);
    demodCharts->addWidget(plotPanel("眼图 · 合成示例", "demodEyePanel", ChartMode::Eye, eyeDemod_), 0, 1);
    demodCharts->setColumnStretch(0, 1); demodCharts->setColumnStretch(1, 1);
    demodLayout->addLayout(demodCharts, 2);
    auto* bitHead = new QWidget; auto* bitRow = new QHBoxLayout(bitHead); bitRow->setContentsMargins(8, 2, 8, 2);
    auto* demodStatus = textLabel("位流 · 合成示例", "demodulationStatus");
    demodStatus->setToolTip("未执行真实解调；合成位流没有实际源符号时间映射。"); bitRow->addWidget(demodStatus);
    auto* selectAllBits = new QCheckBox("全选"); selectAllBits->setObjectName("selectAllBits"); bitRow->addWidget(selectAllBits);
    auto* copyBits = actionButton("复制", "copySyntheticBits", bitRow); auto* saveBits = actionButton("导出位流", "exportSyntheticBits", bitRow);
    bitRow->addStretch(); auto* locateBit = actionButton("示意回定位", "locateSyntheticBit", bitRow); demodLayout->addWidget(bitHead);
    bitstream_ = new QPlainTextEdit; bitstream_->setObjectName("syntheticBitstream"); bitstream_->setReadOnly(true); bitstream_->setMinimumHeight(95);
    bitstream_->setPlainText("00110110100101101001011010110100101101001011010110100101101011001010110100101101001011010110\n"
                             "11001010110100101101001011010110100101101011001010110100101101001011010110100101101001011\n"
                             "00101101001011010110100101101001011010110010101101001011010010110101101001011010010110101101");
    demodLayout->addWidget(bitstream_, 1);
    connect(copyBits, &QPushButton::clicked, this, &NarrowbandWorkspace::copyBitstream);
    connect(saveBits, &QPushButton::clicked, this, &NarrowbandWorkspace::exportBitstream);
    connect(selectAllBits, &QCheckBox::toggled, this, [this](bool checked) { if (checked) bitstream_->selectAll(); else bitstream_->moveCursor(QTextCursor::Start); });
    connect(locateBit, &QPushButton::clicked, this, [this, demodStatus] {
        demodStatus->setText("合成位流无实际源符号可定位");
        if (log_) log_("位流示意回定位；无实际解调样本可定位");
    });

    auto* eye = makePage(static_cast<int>(NarrowbandPage::Eye));
    auto* eyeLayout = new QVBoxLayout(eye); eyeLayout->setContentsMargins(0,0,0,0);
    eyeLayout->addWidget(eyePanel, 1);
    eyePanel->show();
    analysisSummary_ = textLabel("真实 IQ 分析待运行", "narrowbandAnalysisSummary");
    analysisSummary_->setMaximumHeight(42); root->addWidget(analysisSummary_);

    // All time-domain gestures update one channel view; PSD and STFT frequency
    // gestures update the same baseband window used by every page.
    auto bindGesture = [this](NarrowbandChart* chart, ChartMode mode) {
        chart->gesture = [this, chart, mode](QPointF a, QPointF b, int wheel, bool selection) {
            auto* channel = session_.activeChannel();
            if (!channel) return;
            auto time = channel->visibleSourceTime; auto frequency = channel->visibleBasebandFrequency;
            const QRectF plot = chart->plotRect();
            using Zone = chart_feedback::Zone;
            const auto zoneAt = [chart, plot, mode](QPointF point) {
                const QPointF pixel(point.x() * chart->width(), point.y() * chart->height());
                if (mode == ChartMode::Navigation) return Zone::Plot;
                return chart_feedback::zoneAt(pixel,plot);
            };
            const Zone zone = zoneAt(a);
            if (zone == Zone::None) return;
            const auto toUnitX = [chart, plot](QPointF point) {
                return chart_interaction::fractionAt(point.x() * chart->width(), plot.left(), plot.width());
            };
            const auto toUnitY = [chart, plot](QPointF point) {
                return chart_interaction::fractionAt(point.y() * chart->height(), plot.top(), plot.height());
            };
            double x0 = toUnitX(a), x1 = toUnitX(b);
            const double y0 = toUnitY(a), y1 = toUnitY(b);
            auto beginWheel = [this] {
                if (!chartWheelBase_) chartWheelBase_ = session_.channelViewSnapshot();
                if (chartWheelTimer_) chartWheelTimer_->start();
            };
            auto commitPointerGesture = [this] { finishChartWheel(); };
            const auto applyView = [this, &time, &frequency, wheel, &beginWheel, &commitPointerGesture] {
                if (wheel) { beginWheel(); session_.setChannelView(time, frequency, false); }
                else { commitPointerGesture(); session_.setChannelView(time, frequency, true); }
                requestDisplay();
            };
            const auto fraction = [](double value) {
                return static_cast<std::uint64_t>(std::llround(std::clamp(value, 0.0, 1.0) * 1'000'000));
            };
            const auto zoomTime = [&](double anchorRatio, bool globalNavigation) {
                const std::uint64_t total = channel->sourceTime.end - channel->sourceTime.begin;
                const std::uint64_t span = globalNavigation ? total : time.end - time.begin;
                const long double factor = std::pow(1.2L, -static_cast<long double>(wheel) / 120.0L);
                const std::uint64_t minimum = 1;
                const auto nextSpan = static_cast<std::uint64_t>(std::clamp(
                    std::round(static_cast<long double>(span) * factor), static_cast<long double>(minimum), static_cast<long double>(total)));
                std::uint64_t anchorOffset = 0, leftOffset = 0;
                mulDiv(globalNavigation ? total : span, fraction(anchorRatio), 1'000'000, false, anchorOffset);
                mulDiv(nextSpan, fraction(anchorRatio), 1'000'000, false, leftOffset);
                const auto anchor = (globalNavigation ? channel->sourceTime.begin : time.begin) + anchorOffset;
                const auto begin = std::clamp(anchor > leftOffset ? anchor - leftOffset : channel->sourceTime.begin,
                    channel->sourceTime.begin, channel->sourceTime.end - nextSpan);
                time = {begin, begin + nextSpan};
            };
            const auto panTime = [&](double deltaFraction, bool useFullRange) {
                const auto span = useFullRange ? channel->sourceTime.end - channel->sourceTime.begin : time.end - time.begin;
                std::uint64_t amount = 0;
                mulDiv(span, fraction(std::abs(deltaFraction)), 1'000'000, true, amount);
                std::uint64_t begin = time.begin;
                if (deltaFraction > 0) begin = begin > amount ? begin - amount : channel->sourceTime.begin;
                else begin = std::min(channel->sourceTime.end - (time.end - time.begin), begin + amount);
                begin = std::clamp(begin, channel->sourceTime.begin, channel->sourceTime.end - (time.end - time.begin));
                time = {begin, begin + (time.end - time.begin)};
            };
            const auto zoomFrequency = [&](double anchorRatio) {
                const double span = frequency.upperHz - frequency.lowerHz;
                const double factor = std::pow(1.2, -wheel / 120.0);
                const double nextSpan = std::clamp(span * factor, 1e-6, channel->outputSampleRateHz);
                const auto zoomed = chart_interaction::zoomAround(
                    {frequency.lowerHz, frequency.upperHz}, nextSpan / span, anchorRatio);
                frequency = {zoomed.first, zoomed.last};
            };
            const auto panFrequency = [&](double deltaFraction) {
                const auto shifted = chart_interaction::panByFraction(
                    {frequency.lowerHz, frequency.upperHz}, -deltaFraction);
                const double half = channel->outputSampleRateHz / 2.0;
                double lower = shifted.first;
                double upper = shifted.last;
                if (lower < -half) { upper += -half - lower; lower = -half; }
                if (upper > half) { lower -= upper - half; upper = half; }
                frequency = {std::max(-half, lower), std::min(half, upper)};
            };
            const auto dragXFraction = (b.x() - a.x()) * chart->width() / std::max(1.0, plot.width());
            const auto dragYFraction = (b.y() - a.y()) * chart->height() / std::max(1.0, plot.height());

            if (mode == ChartMode::Curve && zone == Zone::YAxis) {
                if (wheel) {
                    const double span = channel->waveformAxisMaximum - channel->waveformAxisMinimum;
                    const double nextSpan = std::clamp(span * std::pow(1.2, -wheel / 120.0), 1e-6, 2.0e12);
                    const auto range = chart_interaction::zoomAround(
                        {channel->waveformAxisMinimum, channel->waveformAxisMaximum}, nextSpan / span, 1.0 - y0);
                    beginWheel();
                    session_.setChannelAmplitudeRange(range.first, range.last, false, false);
                    waveform_->setYAxisRange(range.first, range.last, waveformAxisLabel(channel->waveform, session_.activeFile() ? &session_.activeFile()->metadata : nullptr));
                } else if (selection) {
                    commitPointerGesture();
                    const auto range = chart_interaction::panByFraction(
                        {channel->waveformAxisMinimum, channel->waveformAxisMaximum}, dragYFraction);
                    session_.setChannelAmplitudeRange(range.first, range.last, false, true);
                    waveform_->setYAxisRange(channel->waveformAxisMinimum, channel->waveformAxisMaximum,
                                             waveformAxisLabel(channel->waveform, session_.activeFile() ? &session_.activeFile()->metadata : nullptr));
                }
                return;
            }
            if (mode == ChartMode::Spectrum && zone == Zone::YAxis) {
                if (wheel) {
                    const double span = channel->psdAxisMaximum - channel->psdAxisMinimum;
                    const double nextSpan = std::clamp(span * std::pow(1.2, -wheel / 120.0), .001, maximumPsdSpan);
                    const auto range = chart_interaction::zoomAround(
                        {channel->psdAxisMinimum, channel->psdAxisMaximum}, nextSpan / span, 1.0 - y0);
                    beginWheel(); session_.setChannelPsdRange(range.first, range.last, false);
                    psd_->setYAxisRange(range.first, range.last, session_.activeFile() ? powerUnit(session_.activeFile()->metadata) : QStringLiteral("dBFS/Hz"));
                } else if (selection) {
                    commitPointerGesture();
                    const auto range = chart_interaction::panByFraction(
                        {channel->psdAxisMinimum, channel->psdAxisMaximum}, dragYFraction);
                    session_.setChannelPsdRange(range.first, range.last, true);
                    psd_->setYAxisRange(channel->psdAxisMinimum, channel->psdAxisMaximum, session_.activeFile() ? powerUnit(session_.activeFile()->metadata) : QStringLiteral("dBFS/Hz"));
                }
                return;
            }

            const auto total = channel->sourceTime.end - channel->sourceTime.begin;
            const auto span = time.end - time.begin;
            if (mode == ChartMode::Navigation) {
                const auto targetSpan = span;
                if (selection) {
                    std::uint64_t amount = 0;
                    mulDiv(total, fraction(std::abs(x1 - x0)), 1'000'000, true, amount);
                    const auto begin = x1 >= x0 ? std::min(channel->sourceTime.end - targetSpan, time.begin + amount) :
                        std::max(channel->sourceTime.begin, time.begin > amount ? time.begin - amount : channel->sourceTime.begin);
                    time = {begin, begin + targetSpan};
                } else if (wheel) {
                    zoomTime(x0, true);
                } else {
                    std::uint64_t offset = 0, left = 0;
                    mulDiv(total, fraction(x0), 1'000'000, false, offset);
                    mulDiv(targetSpan, 1, 2, false, left);
                    const auto anchor = channel->sourceTime.begin + offset;
                    const auto begin = std::clamp(anchor > left ? anchor - left : channel->sourceTime.begin,
                        channel->sourceTime.begin, channel->sourceTime.end - targetSpan);
                    time = {begin, begin + targetSpan};
                }
                applyView(); return;
            }
            if (mode == ChartMode::Spectrum) {
                if (zone == Zone::XAxis && selection) panFrequency(dragXFraction);
                else if (selection && zone == Zone::Plot) {
                    const auto range = chart_interaction::selectFractions(
                        {frequency.lowerHz, frequency.upperHz}, x0, x1);
                    frequency = {range.first, range.last};
                } else if (wheel && zone != Zone::YAxis) zoomFrequency(x0);
                else return;
                applyView(); return;
            }
            if (mode == ChartMode::Heatmap && zone == Zone::YAxis) {
                if (wheel) {
                    const double ratio = 1.0 - y0;
                    const double width = frequency.upperHz - frequency.lowerHz;
                    const double nextWidth = std::clamp(width * std::pow(1.2, -wheel / 120.0),
                        1e-6, channel->outputSampleRateHz);
                    const auto range = chart_interaction::zoomAround(
                        {frequency.lowerHz, frequency.upperHz}, nextWidth / width, ratio);
                    frequency = {range.first, range.last};
                } else if (selection) panFrequency(dragYFraction);
                else return;
                applyView(); return;
            }
            if (mode == ChartMode::Heatmap && selection && zone == Zone::Plot) {
                double yTop = std::min(y0, y1), yBottom = std::max(y0, y1);
                if (x1 < x0) std::swap(x0, x1);
                std::uint64_t beginOffset = 0, endOffset = 0;
                mulDiv(span, fraction(x0), 1'000'000, false, beginOffset); mulDiv(span, fraction(x1), 1'000'000, true, endOffset);
                time = {time.begin + beginOffset, time.begin + std::max(beginOffset + 1, endOffset)};
                const double full = frequency.upperHz - frequency.lowerHz;
                frequency = {frequency.upperHz - yBottom * full, frequency.upperHz - yTop * full};
            } else if (selection && zone == Zone::Plot && std::abs(b.x() - a.x()) > .01) {
                if (x1 < x0) std::swap(x0, x1);
                std::uint64_t beginOffset = 0, endOffset = 0;
                mulDiv(span, fraction(x0), 1'000'000, false, beginOffset); mulDiv(span, fraction(x1), 1'000'000, true, endOffset);
                time = {time.begin + beginOffset, time.begin + std::max(beginOffset + 1, endOffset)};
            } else if (wheel) {
                zoomTime(x0, false);
            } else if (mode == ChartMode::Curve && zone == Zone::XAxis && selection) {
                panTime(dragXFraction, false);
            } else if (mode == ChartMode::Heatmap && zone == Zone::XAxis && selection) {
                panTime(dragXFraction, false);
            } else return;
            if (time.end > channel->sourceTime.end) { const auto width = time.end - time.begin; time.end = channel->sourceTime.end; time.begin = time.end - std::min(width, total); }
            if (time.end <= time.begin) time = channel->visibleSourceTime;
            applyView();
        };
    };
    bindGesture(waveform_, ChartMode::Curve);
    bindGesture(psd_, ChartMode::Spectrum); bindGesture(stft_, ChartMode::Heatmap);
    bindGesture(modulationStft_, ChartMode::Heatmap); bindGesture(recognitionStft_, ChartMode::Heatmap);
    charts_ = {waveform_, psd_, stft_, modulationStft_, recognitionStft_,
               constellationLarge_, constellationDemod_, eyeLarge_, eyeDemod_, timeline_};
    for (auto* chart : charts_) if (chart) {
        chart->cancelPendingGesture = [this] { cancelChartWheel(); };
        chart->navigateHistory = [this](bool forward) {
            finishChartWheel();
            if (forward ? session_.channelForward() : session_.channelBack()) refreshFromSession();
        };
    }
    for (auto* chart : {waveform_, psd_, stft_, modulationStft_, recognitionStft_}) if (chart) {
        chart->autoPowerFitRequested = [this] { emit autoPowerFitRequested(); };
        chart->autoPowerFitAvailable = [this] { return property("autoPowerFitAvailable").toBool(); };
        chart->pinRequested = [this, chart](QPointF point) {
            const auto* channel = session_.activeChannel(); if (!channel) return;
            const auto plot = chart->plotRect();
            const double x = std::clamp((point.x() - plot.left()) / plot.width(), 0.0, 1.0);
            const double y = std::clamp((plot.bottom() - point.y()) / plot.height(), 0.0, 1.0);
            const auto& previous = session_.linkedCursor(channel->id);
            auto sample = previous.pinned ? previous.sourceSample : channel->visibleSourceTime.begin + (channel->visibleSourceTime.end - channel->visibleSourceTime.begin) / 2;
            double frequency = previous.pinned ? previous.frequencyHz : (channel->visibleBasebandFrequency.lowerHz + channel->visibleBasebandFrequency.upperHz) / 2;
            if (chart != psd_) sample = channel->visibleSourceTime.begin + std::min(channel->visibleSourceTime.end - channel->visibleSourceTime.begin - 1,
                static_cast<SampleIndex>(x * (channel->visibleSourceTime.end - channel->visibleSourceTime.begin)));
            if (chart != waveform_) frequency = channel->visibleBasebandFrequency.lowerHz + (chart == psd_ ? x : y) *
                (channel->visibleBasebandFrequency.upperHz - channel->visibleBasebandFrequency.lowerHz);
            std::uint64_t frameId=0;if(chart!=psd_&&chart!=waveform_)if(const auto data=session_.spectrogram(channel->id))if(const auto* frame=data->frameAtFraction(x))frameId=frame->id;
            session_.pinCursor(channel->id, sample, frequency, chart != psd_,frameId); updateLinkedCursors(); emit displayParametersChanged();
        };
        chart->clearCursorRequested = [this] { if (const auto* channel = session_.activeChannel()) session_.clearCursor(channel->id); updateLinkedCursors(); emit displayParametersChanged(); };
        chart->averagePsdRequested = [this] { if (const auto* channel = session_.activeChannel()) session_.setFramePsd(channel->id, false); updateLinkedCursors(); emit displayParametersChanged(); };
        chart->framePsdRequested = [this] { if (const auto* channel = session_.activeChannel()) session_.setFramePsd(channel->id, true); updateLinkedCursors(); emit displayParametersChanged(); };
        chart->expandAnalysisRequested = [this] { expandAnalysisTime(); };
        chart->pendingContextGesture = [this] { return chartWheelBase_.has_value(); };
    }
    const auto applyGrid = [this](bool visible) {
        for (auto* chart : charts_) if (chart) chart->setGridVisible(visible);
        QSettings().setValue(QStringLiteral("narrowband/gridVisible"), visible);
        emit displayParametersChanged();
    };
    for (auto* chart : charts_) if (chart) chart->setGridVisible(grid_->isChecked());
    connect(grid_, &QCheckBox::toggled, this, applyGrid);
    waveform_->resetYAxis = [this] {
        auto* channel = session_.activeChannel();
        if (!channel) return;
        finishChartWheel();
        channel->waveformAutoScale = true;
        hasDisplayRequest_ = false;
        requestDisplay();
    };
    psd_->resetYAxis = [this] {
        auto* channel = session_.activeChannel();
        if (!channel) return;
        finishChartWheel();
        session_.setChannelPsdRange(-120.0, 0.0, true);
        psd_->setYAxisRange(channel->psdAxisMinimum, channel->psdAxisMaximum, session_.activeFile() ? powerUnit(session_.activeFile()->metadata) : QStringLiteral("dBFS/Hz"));
    };
    if (timeline_) timeline_->gesture = [this](QPointF a, QPointF, int wheel, bool selection) {
        if (recognitionSegments_.empty() || selection || wheel) return;
        const auto fraction = std::clamp(a.x(), 0.0, 1.0);
        selectedRecognitionSegment_ = std::min(static_cast<int>(recognitionSegments_.size()) - 1,
            static_cast<int>(fraction * recognitionSegments_.size()));
        updateRecognitionDetail();
        auto* channel = session_.activeChannel();
        if (channel && channel->id == recognitionChannelId_) {
            const auto& segment = recognitionSegments_[static_cast<std::size_t>(selectedRecognitionSegment_)];
            session_.setChannelView({segment.sourceBegin, segment.sourceEnd}, channel->visibleBasebandFrequency);
            requestDisplay();
        }
    };
    pages_->setCurrentIndex(0); pageButtons_[0]->setChecked(true);
}

void NarrowbandWorkspace::refreshFromSession() {
    const auto* channel = session_.activeChannel();
    const auto* file = session_.activeFile();
    if (!channel || !file) {
        markRecognitionStale("通道或源文件已关闭");
        cancelAnalysis();cancelPsd(); cancelWork(); sampleCache_.clear(); caption_->setText("未选择窄带通道"); caption_->setToolTip({});
        psdStatus_.clear(); setDataStatus("等待通道数据");
        if (waveform_) waveform_->setSeries({}, "当前没有活动通道");
        syncPanel_->hide(); syncPanelToggle_->setEnabled(false);
        setAnalysisStatus("当前没有活动通道。");
        return;
    }
    for (auto* chart : charts_) if (chart) {
        chart->setPalette(file->display.palette);
        chart->setProperty("sourcePowerUnit", powerUnit(file->metadata));
        chart->setProperty("sourceAmplitudeUnit", amplitudeUnit(file->metadata));
    }
    if ((recognitionActive_ || !recognitionSegments_.empty()) && recognitionChannelId_ == channel->id) {
        if (recognitionConfigVersion_ != channel->configVersion)
            markRecognitionStale("通道配置已变化");
        else if (recognitionScope_ && recognitionScope_->currentIndex() == 0 &&
                 (recognitionRangeBegin_ != channel->visibleSourceTime.begin || recognitionRangeEnd_ != channel->visibleSourceTime.end))
            markRecognitionStale("分析时间范围已变化");
    } else if ((recognitionActive_ || !recognitionSegments_.empty()) && recognitionChannelId_ != channel->id) {
        markRecognitionStale("已切换活动通道");
    }
    updateCaption();
    syncPanelToggle_->setEnabled(true);
    { const QSignalBlocker blocker(syncPanelToggle_); syncPanelToggle_->setChecked(channel->syncPanelExpanded); }
    syncPanelToggle_->setArrowType(channel->syncPanelExpanded ? Qt::DownArrow : Qt::RightArrow);
    syncPanel_->setVisible(channel->syncPanelExpanded);
    const auto index = static_cast<int>(channel->page);
    if (pages_->currentIndex() != index) pages_->setCurrentIndex(index);
    for (int i = 0; i < static_cast<int>(pageButtons_.size()); ++i) pageButtons_[i]->setChecked(i == index);
    { QSignalBlocker blocker(waveformMode_); waveformMode_->setCurrentIndex(static_cast<int>(channel->waveform)); }
    if (auto* rate = findChild<QDoubleSpinBox*>("symbolRate")) {
        double displayed=channel->analysis.symbolRateHz>0?channel->analysis.symbolRateHz:channel->symbolRate;
        if(channel->analysis.symbolRateMode==SymbolRateMode::Estimated && analysisResult_ && analysisResult_->symbolRateHz>0 &&
           analysisIdentityCurrent(analysisResult_->identity,file->metadata,*channel,session_.projectGeneration(),analysisGeneration_))displayed=analysisResult_->symbolRateHz;
        const QSignalBlocker blocker(rate); displayParameterValue(rate,displayed);
        rate->setToolTip(channel->analysis.symbolRateMode==SymbolRateMode::Unknown?"旧数值尚未确认；选择手工 Rs 后才用于眼图。":"已提交的符号率假设；不是调制识别结果。");
    }
    if(auto* mode=findChild<QComboBox*>("analysisSymbolRateMode")){QSignalBlocker b(mode);mode->setCurrentIndex(static_cast<int>(channel->analysis.symbolRateMode));}
    if(auto* hypothesis=findChild<QComboBox*>("modulationFormat")){QSignalBlocker b(hypothesis);hypothesis->setCurrentIndex(static_cast<int>(channel->analysis.hypothesis));}
    if(auto* order=findChild<QComboBox*>("analysisModulationOrder")) {
      QSignalBlocker b(order);const auto h=channel->analysis.hypothesis;const int minimum=h==ModulationHypothesis::Apsk?8:h==ModulationHypothesis::Qam?4:2,maximum=h==ModulationHypothesis::Qam?4096:256;
      if(order->itemData(0).toInt()!=minimum || order->itemData(order->count()-1).toInt()!=maximum){order->clear();for(int m=minimum;m<=maximum;m*=2)order->addItem(QString("M=%1").arg(m),m);}
      order->setCurrentIndex(order->findData(channel->analysis.modulationOrder));order->setVisible(h>=ModulationHypothesis::Psk);
    }
    if(auto* rings=dynamic_cast<ParameterTextEdit*>(findChild<QLineEdit*>("analysisApskRings"))) {
      rings->displayText(str(channel->analysis.apskRings));rings->setEnabled(channel->analysis.hypothesis==ModulationHypothesis::Apsk);
      rings->setPlaceholderText(str(defaultApskRings(channel->analysis.modulationOrder)));
    }
    // Chart editors can live in the main window's shared overflow layer.
    if(auto* stage=window()->findChild<QComboBox*>("eyeAnalysisStage")){QSignalBlocker b(stage);stage->setCurrentIndex(static_cast<int>(channel->analysis.eyeStage));}
    if(auto* stage=window()->findChild<QComboBox*>("constellationAnalysisStage")){QSignalBlocker b(stage);stage->setCurrentIndex(static_cast<int>(channel->analysis.constellationStage));}
    if(auto* mode=findChild<QComboBox*>("analysisCfoMode")){QSignalBlocker b(mode);mode->setCurrentIndex(static_cast<int>(channel->analysis.cfoMode));}
    for(const auto& entry:std::array<std::pair<const char*,bool>,3>{{{"analysisMatchedFilter",channel->analysis.matchedFilter},{"analysisTimingRecovery",channel->analysis.timingRecovery},{"analysisCarrierRecovery",channel->analysis.carrierRecovery}}})
      if(auto* control=findChild<QCheckBox*>(entry.first)){QSignalBlocker b(control);control->setChecked(entry.second);}
    for(const auto& entry:std::array<std::pair<const char*,double>,4>{{{"analysisManualCfoHz",channel->analysis.manualCfoHz},{"analysisManualPhaseRadians",channel->analysis.manualPhaseRadians},{"analysisTimingLoopBandwidth",channel->analysis.timingLoopBandwidth},{"analysisCarrierLoopBandwidth",channel->analysis.carrierLoopBandwidth}}})
      if(auto* control=findChild<QDoubleSpinBox*>(entry.first)){QSignalBlocker b(control);displayParameterValue(control,entry.second);}
    if(auto* span=findChild<QSpinBox*>("analysisRrcSpan")){QSignalBlocker b(span);displayParameterValue(span,channel->analysis.filterSpanSymbols);}
    if(auto* budget=findChild<QComboBox*>("analysisSampleBudget")){QSignalBlocker b(budget);const auto n=static_cast<int>(channel->analysis.sampleBudget);if(budget->findData(n)<0)budget->addItem(QString("%1 复样本（已保存）").arg(n),n);budget->setCurrentIndex(budget->findData(n));}
    if(auto* density=window()->findChild<QComboBox*>("eyeDrawingDensity")){QSignalBlocker b(density);density->setCurrentIndex(density->findData(channel->analysis.eyePointsPerUi));}
    for(const auto& setting:std::array<std::pair<const char*,double>,2>{{{"eyeTimingOffsetUi",channel->analysis.eyeTimingOffsetUi},{"analysisRrcRolloff",channel->analysis.rolloff}}})
      if(auto* control=window()->findChild<QDoubleSpinBox*>(setting.first)){QSignalBlocker b(control);displayParameterValue(control,setting.second);}
    displayFftValue(displayStftFft_, channel->stftFftSize);
    { QSignalBlocker blocker(frequencyMode_); frequencyMode_->setCurrentIndex(channel->absoluteFrequencyLabels ? 1 : 0); }
    { QSignalBlocker blocker(eyePeriods_);if(eyePeriods_->findData(channel->eyePeriods)<0)eyePeriods_->addItem(QString("%1（旧设置）").arg(channel->eyePeriods),channel->eyePeriods);eyePeriods_->setCurrentIndex(eyePeriods_->findData(channel->eyePeriods)); }
    { QSignalBlocker blocker(eyeTraces_);if(eyeTraces_->findData(channel->eyeTraces)<0)eyeTraces_->addItem(QString("%1 条（旧设置）").arg(channel->eyeTraces),channel->eyeTraces);eyeTraces_->setCurrentIndex(eyeTraces_->findData(channel->eyeTraces)); }
    { QSignalBlocker blocker(eyeComponent_); eyeComponent_->setCurrentIndex(channel->eyeComponent); }
    updateFrequencyAxisLabels();
    if (waveform_)
        waveform_->setYAxisRange(channel->waveformAxisMinimum, channel->waveformAxisMaximum,
                                 waveformAxisLabel(channel->waveform, session_.activeFile() ? &session_.activeFile()->metadata : nullptr));
    if (psd_) psd_->setYAxisRange(channel->psdAxisMinimum, channel->psdAxisMaximum, session_.activeFile() ? powerUnit(session_.activeFile()->metadata) : QStringLiteral("dBFS/Hz"));
    if (navigation_) {
        const auto total = std::max<std::uint64_t>(1, channel->sourceTime.end - channel->sourceTime.begin);
        navigation_->setNavigationViewport(
            static_cast<double>(channel->visibleSourceTime.begin - channel->sourceTime.begin) / total,
            static_cast<double>(channel->visibleSourceTime.end - channel->sourceTime.begin) / total);
        navigation_->setSeries({}, QStringLiteral("%1 – %2 · 源时间窗位置").arg(channel->sourceTime.begin).arg(channel->sourceTime.end));
    }
    if(auto* points=window()->findChild<QComboBox*>("constellationPointBudget")){QSignalBlocker b(points);if(points->findData(channel->analysis.pointCount)<0)points->addItem(QString("%1 点").arg(channel->analysis.pointCount),channel->analysis.pointCount);points->setCurrentIndex(points->findData(channel->analysis.pointCount));}
    for(const auto& setting:std::array<std::pair<const char*,bool>,3>{{{"constellationAutoScale",channel->analysis.constellationAutoScale},{"analysisRemoveDc",channel->analysis.removeDc},{"analysisNormalizePower",channel->analysis.normalizePower}}}) {
      if(auto* control=window()->findChild<QCheckBox*>(setting.first)){QSignalBlocker b(control);control->setChecked(setting.second);}
    }
    for(const auto& axis:std::array<std::pair<const char*,double>,2>{{{"constellationAxisMinimum",channel->constellationMinimum},{"constellationAxisMaximum",channel->constellationMaximum}}})
      if(auto* control=window()->findChild<QDoubleSpinBox*>(axis.first)){QSignalBlocker b(control);displayParameterValue(control,axis.second);control->setEnabled(!channel->analysis.constellationAutoScale);}
    if (constellationDemod_) constellationDemod_->setPoints(syntheticConstellation(103), "合成示例 · 未执行载波恢复或真实解调", "合成示例");
    if (eyeDemod_) eyeDemod_->setEye(syntheticEye(channel->eyeTraces, 192, channel->eyePeriods, 17), "合成示例 · 未执行定时同步", "合成示例");
    requestDisplay();
    renderAnalysis();
}

void NarrowbandWorkspace::showPage(NarrowbandPage page) {
    const auto index = static_cast<int>(page);
    if (index < 0 || index >= static_cast<int>(pageWidgets_.size())) return;
    if (auto* channel = session_.activeChannel()) channel->page = page;
    pages_->setCurrentIndex(static_cast<int>(page));
    for (int i = 0; i < static_cast<int>(pageButtons_.size()); ++i) pageButtons_[i]->setChecked(i == static_cast<int>(page));
    requestDisplay();
    emit displayParametersChanged();
}

void NarrowbandWorkspace::updateFrequencyAxisLabels() {
    const auto* channel = session_.activeChannel();
    const auto* file = session_.activeFile();
    if (!channel || !file) return;
    for (auto* chart : charts_) {
        if (!chart) continue;
        chart->setFrequencyAxis(channel->visibleBasebandFrequency,
                                channel->centerFrequencyHz,
                                channel->absoluteFrequencyLabels);
        chart->setTimeAxis(channel->visibleSourceTime, file->metadata.sampleRateHz,
                           !channel->preserveSourceTime || channel->relativeTime, channel->sourceTime.begin);
        if (chart == eyeLarge_ || chart == eyeDemod_ || chart == eyeSmall_)
            chart->setSymbolPeriod(channel->eyePeriods);
    }
}

void NarrowbandWorkspace::updateLinkedCursors() {
    const auto* channel = session_.activeChannel(); if (!channel) return;
    const auto& cursor = session_.linkedCursor(channel->id);
    for (auto* chart : {waveform_, psd_, stft_, modulationStft_, recognitionStft_}) if (chart) chart->setLinkedCursor(cursor);
    const auto spectrum = session_.spectrogram(channel->id);
    const auto* frame = !displayPending_ && spectrum && spectrum->sourceView == channel->visibleSourceTime && spectrum->plan.frequencies == channel->visibleBasebandFrequency ?
        session_.selectedSpectralFrame(channel->id) : nullptr;
    const QString key = QString::fromStdString(channel->id) + QString("/%1/%2/%3/%4")
        .arg(frameGeneration_).arg(cursor.framePsd).arg(frame ? frame->id : 0).arg(frame ? reinterpret_cast<quintptr>(frame) : 0);
    if (key != linkedPsdKey_) {
        linkedPsdKey_ = key;
        if (cursor.framePsd) {
            QString summary = frame ? QStringLiteral("驻留帧谱") : QStringLiteral("驻留帧不可用 / 正在更新");
            if (frame && frame->paddedSamples) summary += QStringLiteral(" · 补零 %1").arg(frame->paddedSamples);
            psd_->setSeries(frame ? std::vector<std::vector<float>>{spectrumDb(*frame)} : std::vector<std::vector<float>>{},
                frame ? QStringLiteral("驻留帧谱 · N=%1 · 与 STFT 帧逐 bin 一致 · 有效观测 %2 ms · 补零 %3；补零不提高真实分辨率")
                    .arg(frame->linearPower.size()).arg(frame->observedSeconds * 1000, 0, 'g', 6).arg(frame->paddedSamples) : summary, summary);
            psd_->setProperty("effectiveFftPoints", frame ? static_cast<int>(frame->linearPower.size()) : channel->stftFftSize);
        } else {
            QString summary = psdStatus_;
            if (psdStatus_.startsWith("PSD 完成")) {
                summary = "统计谱";
                if (averagePower_ && averagePower_->paddedSamples) summary += QStringLiteral(" · 补零 %1").arg(averagePower_->paddedSamples);
            }
            psd_->setSeries(averagePsd_.empty() ? std::vector<std::vector<float>>{} : std::vector<std::vector<float>>{averagePsd_}, psdStatus_, summary);
            psd_->setProperty("effectiveFftPoints", channel->psdFftSize);
        }
    }
    if (displayPsdFft_) {
        displayFftValue(displayPsdFft_, cursor.framePsd && frame ? static_cast<int>(frame->linearPower.size()) :
            cursor.framePsd ? channel->stftFftSize : channel->psdFftSize);
        if(cursor.framePsd)displayPsdFft_->setItemText(displayPsdFft_->currentIndex(),QString("%1（随 STFT）").arg(displayPsdFft_->currentData().toInt()));
        displayPsdFft_->setEnabled(!cursor.framePsd);
        displayPsdFft_->setToolTip(cursor.framePsd ? "N 随 STFT；选择统计谱可修改独立PSD N" : "当前可见频段内的分析点数");
    }
    if (dataStatus_) dataStatus_->setToolTip(displayDiagnosticsText());
}
PowerAnalysisSnapshot NarrowbandWorkspace::powerSnapshot() const {
    PowerAnalysisSnapshot result;
    const auto* channel = session_.activeChannel(); if (!channel || displayPending_) return result;
    result.heatmap = session_.spectrogram(channel->id);
    if (session_.linkedCursor(channel->id).framePsd) {
        if (const auto* frame = session_.selectedSpectralFrame(channel->id)) result.psd = std::shared_ptr<const SpectralFrame>(result.heatmap, frame);
    } else result.psd = averagePower_;
    result.ready = result.heatmap && !result.heatmap->frames.empty() && result.psd;
    return result;
}
void NarrowbandWorkspace::updatePowerColors() {
    const auto* file = session_.activeFile(); if (!file || rasterPower_.empty()) return;
    const auto key = QString("nb-power/%1/%2/%3").arg(frameGeneration_).arg(file->display.referenceLevelDb, 0, 'g', 17).arg(file->display.dynamicRangeDb, 0, 'g', 17);
    if (key == powerColorKey_) return;
    powerColorKey_ = key;
    const auto image = spectrumImage(rasterPower_, QSize(560, 240), file->display.referenceLevelDb, file->display.dynamicRangeDb);
    for (auto* chart : {stft_, modulationStft_, recognitionStft_}) if (chart) chart->setRaster(image, chart->status(), key, chart->displayStatus());
}

void NarrowbandWorkspace::fitWaveformAmplitude() {
    auto* channel = session_.activeChannel();
    if (!channel || !channel->waveformAutoScale || !waveform_) return;
    const auto range = waveform_->valueRange();
    if (!range) return;
    auto [minimum, maximum] = *range;
    double span = maximum - minimum;
    const double minimumSpan = channel->waveform == NarrowbandWaveform::Phase ? .01 : 1.0;
    if (span < minimumSpan) {
        const double center = (minimum + maximum) / 2.0;
        span = std::max(minimumSpan, std::abs(center) * 1e-6);
        minimum = center - span / 2.0; maximum = center + span / 2.0;
    }
    const double padding = span / 6.0;
    session_.setChannelAmplitudeRange(minimum - padding, maximum + padding, true, false);
    waveform_->setYAxisRange(channel->waveformAxisMinimum, channel->waveformAxisMaximum,
        waveformAxisLabel(channel->waveform, session_.activeFile() ? &session_.activeFile()->metadata : nullptr));
}

void NarrowbandWorkspace::expandAnalysisTime() {
    const auto* channel = session_.activeChannel(); const auto* file = session_.activeFile(); if (!channel || !file) return;
    const auto stft = makeSpectralAnalysisPlan(channel->outputSampleRateHz, channel->visibleBasebandFrequency, channel->stftFftSize);
    const auto psd = makeSpectralAnalysisPlan(channel->outputSampleRateHz, channel->visibleBasebandFrequency, channel->psdFftSize);
    if (!stft.valid || !psd.valid) return;
    std::uint64_t required = 0;
    if (!mulDiv(std::max(stft.inputSamples, psd.inputSamples), rateHz(file->metadata.sampleRateHz), rateHz(channel->outputSampleRateHz), true, required)) return;
    const auto total = channel->sourceTime.end - channel->sourceTime.begin;
    if (required > total) { setDataStatus("通道总时长不足以满足当前分析点数"); return; }
    const auto& cursor = session_.linkedCursor(channel->id);
    const auto center = cursor.pinned ? cursor.sourceSample : channel->visibleSourceTime.begin + (channel->visibleSourceTime.end - channel->visibleSourceTime.begin) / 2;
    const auto alignment = static_cast<std::uint64_t>(std::ceil(file->metadata.sampleRateHz / channel->outputSampleRateHz));
    const auto span = std::min(total, std::max(required + std::min(alignment, total - required), channel->visibleSourceTime.end - channel->visibleSourceTime.begin));
    const auto first = std::clamp(center > span / 2 ? center - span / 2 : channel->sourceTime.begin, channel->sourceTime.begin, channel->sourceTime.end - span);
    session_.setChannelView({first, first + span}, channel->visibleBasebandFrequency); requestDisplay();
}

void NarrowbandWorkspace::finishChartWheel() {
    if (chartWheelTimer_) chartWheelTimer_->stop();
    if (!chartWheelBase_) return;
    session_.commitChannelViewChange(*chartWheelBase_);
    chartWheelBase_.reset();
}

bool NarrowbandWorkspace::hasPendingInteraction() const {
    if (chartWheelBase_) return true;
    return std::any_of(charts_.begin(), charts_.end(), [](const auto* chart) {
        return chart && chart->hasPendingInteraction();
    });
}

void NarrowbandWorkspace::cancelInteractions() {
    for (auto* chart : charts_) if (chart && chart->hasPendingInteraction()) chart->cancelInteraction();
    cancelChartWheel();
}

void NarrowbandWorkspace::cancelChartWheel() {
    if (chartWheelTimer_) chartWheelTimer_->stop();
    if (!chartWheelBase_) return;
    session_.restoreChannelViewSnapshot(*chartWheelBase_);
    chartWheelBase_.reset();
    refreshFromSession();
}

void NarrowbandWorkspace::requestDisplay(bool settled) {
    const auto* active = session_.activeChannel(); const auto* file = session_.activeFile();
    if (!active || !file || !waveform_ || !psd_ || !stft_) return;
    if(consumesAnalysis(currentPageIndex()))requestAnalysis();
    const auto prefix = availableSamples(file->metadata);
    if (file->metadata.availability.status == LoadStatus::Failed || !prefix ||
        active->sourceTime.begin >= active->sourceTime.end ||
        active->sourceTime.end > prefix) {
      cancelPsd();
      cancelWork();
      hasDisplayRequest_ = false;
      averagePower_.reset();
      averagePsd_.clear();
      linkedPsdKey_.clear();
      rasterPower_.clear();
      powerColorKey_.clear();
      session_.installSpectrogram(active->id, {});
      QString reason;
      if (file->metadata.availability.status == LoadStatus::Failed)
        reason = file->metadata.availability.error.empty()
                     ? QStringLiteral("来源读取失败，请重新读入或核对文件。")
                     : QStringLiteral("来源读取失败：") +
                           str(file->metadata.availability.error);
      else
        reason = QStringLiteral(
                     "通道时段 [%1, %2) 没有完整位于已读可用前缀 [0, %3) 内。")
                     .arg(active->sourceTime.begin)
                     .arg(active->sourceTime.end)
                     .arg(prefix);
      psdStatus_ = reason;
      setDataStatus(reason);
      psd_->setProperty("psdComplete", false);
      waveform_->setSamplePointsVisible(false);
      for (auto* chart : {waveform_, psd_}) chart->setSeries({}, reason);
      for (auto* chart : {stft_, modulationStft_, recognitionStft_})
        if (chart) chart->setRaster({}, reason);
      for (auto* chart :
           {waveform_, psd_, stft_, modulationStft_, recognitionStft_})
        if (chart) chart->setSpectralData({});
      return;
    }
    if(!file->metadata.demo&&active->processingState==ChannelProcessingState::Ready)requestPsd();
    updateFrequencyAxisLabels();
    const QFileInfo sourceInfo(QString::fromStdString(file->metadata.path));
    const auto fingerprint = QString::number(sourceInfo.size()) + "/" + QString::number(sourceInfo.lastModified().toMSecsSinceEpoch());
    const bool unchanged = lastProjectGeneration_ == session_.projectGeneration() && lastSourceFingerprint_ == fingerprint && hasDisplayRequest_ && lastChannelId_ == active->id && lastSourcePath_ == file->metadata.path &&
        lastConfigVersion_ == active->configVersion && lastVisibleTime_ == active->visibleSourceTime &&
        lastVisibleFrequency_ == active->visibleBasebandFrequency && lastWaveform_ == active->waveform &&
        lastStftFft_ == active->stftFftSize && lastSpectrogramSettings_==active->spectrogram;
    if (unchanged) { fitWaveformAmplitude(); updateLinkedCursors(); updatePowerColors(); return; }
    const bool invalidateChannelCache = hasDisplayRequest_ && lastChannelId_ == active->id &&
        lastConfigVersion_ != active->configVersion;
    cancelWork();
    linkedPsdKey_.clear(); rasterPower_.clear(); powerColorKey_.clear();
    session_.installSpectrogram(active->id, {});
    for (auto* chart : {waveform_, psd_, stft_, modulationStft_, recognitionStft_}) if (chart) chart->setSpectralData({});
    if (invalidateChannelCache || lastProjectGeneration_ != session_.projectGeneration()) sampleCache_.clear();
    if (!settled) {
        hasDisplayRequest_ = false; displayPending_ = true;
        waveform_->setSeries({}, "等待交互停止后计算…");
        stft_->setRaster({}, "等待频段分析…");
        if (modulationStft_) modulationStft_->setRaster({}, "等待频段分析…");
        if (recognitionStft_) recognitionStft_->setRaster({}, "等待频段分析…");
        setDataStatus("等待交互停止后计算…");
        displaySettleTimer_->start(); return;
    }
    const auto channel = *active; const auto source = file->metadata;
    lastProjectGeneration_ = session_.projectGeneration();
    lastSourceFingerprint_ = fingerprint;
    lastChannelId_ = channel.id; lastSourcePath_ = source.path; lastConfigVersion_ = channel.configVersion;
    lastVisibleTime_ = channel.visibleSourceTime; lastVisibleFrequency_ = channel.visibleBasebandFrequency;
    lastWaveform_ = channel.waveform; lastPsdFft_ = channel.psdFftSize; lastStftFft_ = channel.stftFftSize; lastSpectrogramSettings_=channel.spectrogram;
    hasDisplayRequest_ = true;
    const auto generation = ++requestGeneration_; const auto config = channel.configVersion;
    QString error; ChannelDspPlan plan;
    if (channel.processingState != ChannelProcessingState::Ready || source.demo || source.path.empty()) {
        const auto status = channel.processingState == ChannelProcessingState::LegacyNeedsReview ?
            QStringLiteral("旧工程通道待确认参数") : source.demo ? QStringLiteral("演示源没有 IQ 样本 · 真实 DDC 未运行") :
            QStringLiteral("源 IQ 文件不可用 · 真实 DDC 未运行");
        waveform_->setSeries({}, status); psd_->setSeries({}, status); stft_->setRaster({}, status);
        if (modulationStft_) modulationStft_->setRaster({}, status); if (recognitionStft_) recognitionStft_->setRaster({}, status);
        setDataStatus(status); return;
    }
    if (!makeChannelDspPlan(source, channel, plan, error)) {
        waveform_->setSeries({}, error); psd_->setSeries({}, error); stft_->setRaster({}, error);
        if (modulationStft_) modulationStft_->setRaster({}, error); if (recognitionStft_) recognitionStft_->setRaster({}, error);
        setDataStatus("参数校验失败：" + error); return;
    }
    const auto sourceRate = rateHz(source.sampleRateHz), outputRate = rateHz(channel.outputSampleRateHz);
    std::uint64_t firstOutput = 0, lastOutput = 0;
    if (!sourceRate || !outputRate || channel.visibleSourceTime.begin < channel.sourceTime.begin ||
        !mulDiv(channel.visibleSourceTime.begin - channel.sourceTime.begin, outputRate, sourceRate, true, firstOutput) ||
        !mulDiv(channel.visibleSourceTime.end - channel.sourceTime.begin, outputRate, sourceRate, false, lastOutput)) {
        waveform_->setSeries({}, "输出采样映射超出支持范围"); setDataStatus("输出采样映射超出支持范围"); return;
    }
    std::uint64_t outputTotal = 0;
    if (!mulDiv(channel.sourceTime.end - channel.sourceTime.begin, outputRate, sourceRate, true, outputTotal)) {
        waveform_->setSeries({}, "通道输出样本数超出 uint64 范围"); setDataStatus("输出长度无效", "通道输出样本数超出 uint64 范围"); return;
    }
    lastOutput = std::min(lastOutput, outputTotal);
    if (lastOutput <= firstOutput) {
        const auto message = QStringLiteral("当前可见范围没有输出样本");
        waveform_->setSeries({}, message); psd_->setSeries({}, message); stft_->setRaster({}, message);
        auto unavailable=std::make_shared<SpectrogramData>();unavailable->sourceView=channel.visibleSourceTime;unavailable->error="当前可见范围没有输出样本";session_.installSpectrogram(channel.id,unavailable);
        setDataStatus("当前可见范围没有输出样本"); return;
    }
    const auto outputSpan = lastOutput - firstOutput;
    const auto physicalPlotWidth = static_cast<std::uint64_t>(std::max(1.0,
        std::floor(waveform_->plotRect().width() * waveform_->devicePixelRatioF())));
    const bool samplePointsVisible = outputSpan <= physicalPlotWidth;
    const int waveformPoints = static_cast<int>(samplePointsVisible ? outputSpan :
        std::clamp(std::llround(waveform_->plotRect().width() * waveform_->devicePixelRatioF() * 2.0), 1400LL, 65536LL));
    waveform_->setSeries({}, "正在按需读取并处理当前可见 IQ…");
    stft_->setRaster({}, "正在计算真实 STFT…");
    setDataStatus("正在处理窄带数据…", QStringLiteral("处理中 · 配置 v%1 · 请求 %2").arg(config).arg(generation));
    displayPending_ = true;
    auto cancel = std::make_shared<std::atomic_bool>(false); cancellation_ = cancel;
    worker_ = std::thread([this, channel, source, plan = std::move(plan), firstOutput, lastOutput, waveformPoints,
                           samplePointsVisible, generation, config, cancel]() mutable {
        const auto cancelled = [cancel] { return cancel->load(std::memory_order_relaxed); };
        SpectralSource provider; provider.sampleRateHz = channel.outputSampleRateHz;
        const auto sourceRate = plan.sourceRateHz, outputRate = plan.outputRateHz;
        provider.sourceRange = [channel, sourceRate, outputRate](TimeRange range) {
            std::uint64_t begin = 0, end = 0;
            if (!mulDiv(range.begin, sourceRate, outputRate, false, begin) ||
                !mulDiv(range.end, sourceRate, outputRate, true, end)) return TimeRange{};
            return TimeRange{channel.sourceTime.begin + std::min(begin, channel.sourceTime.end - channel.sourceTime.begin),
                channel.sourceTime.begin + std::min(end, channel.sourceTime.end - channel.sourceTime.begin)};
        };
        provider.read = [this, &source, &channel, &plan](TimeRange range, std::vector<std::complex<float>>& values, const SpectralCancel& stop) {
            ChannelSampleData block;
            const bool ok = sampleCache_.process(source, channel, plan, range, block, stop);
            if (ok) values = std::move(block.samples);
            return ok;
        };
        std::vector<float> waveI(waveformPoints), waveQ, psd;
        if (channel.waveform == NarrowbandWaveform::IQ) waveQ.resize(waveformPoints);
        std::vector<SampleIndex> positions(waveformPoints);
        bool waveformReady = true;
        const auto span = lastOutput - firstOutput;
        const auto bucketOffset = [span, waveformPoints](std::uint64_t index) {
            return (span / waveformPoints) * index + (span % waveformPoints) * index / waveformPoints;
        };
        std::vector<std::complex<float>> values;
        for (int index = 0; index < waveformPoints && !cancelled(); ++index) {
            const auto begin = firstOutput + bucketOffset(index);
            const auto end = std::max(begin + 1, firstOutput + bucketOffset(index + 1));
            const auto middle = begin + (end - begin) / 2;
            positions[index] = provider.sourceRange({middle, middle + 1}).begin;
            if (channel.waveform == NarrowbandWaveform::IQ || channel.waveform == NarrowbandWaveform::Phase) {
                if (!provider.read({middle, middle + 1}, values, cancelled)) { waveformReady = false; break; }
                waveI[index] = channel.waveform == NarrowbandWaveform::Phase ? std::arg(values.front()) : values.front().real() * static_cast<float>(adcScale(source.sampleFormat));
                if (!waveQ.empty()) waveQ[index] = values.front().imag() * static_cast<float>(adcScale(source.sampleFormat));
            } else {
                double power = 0, peak = 0; std::uint64_t count = 0;
                for (auto first = begin; first < end && !cancelled(); ) {
                    const auto last = first + std::min<std::uint64_t>(16'384, end - first);
                    if (!provider.read({first, last}, values, cancelled)) { waveformReady = false; break; }
                    for (const auto value : values) { const double magnitude = std::norm(value); power += magnitude; peak = std::max(peak, magnitude); }
                    count += values.size(); first = last;
                }
                if (!waveformReady || !count) { waveformReady = false; break; }
                waveI[index] = static_cast<float>(adcScale(source.sampleFormat) * std::sqrt(channel.waveform == NarrowbandWaveform::Envelope ? peak : power / count));
            }
        }
        if (cancelled()) return;
        auto spectrum = analyzeSpectrogram(provider, {firstOutput, lastOutput}, channel.visibleBasebandFrequency, channel.stftFftSize, 240, channel.spectrogram, 10, cancelled);
        spectrum->sourceView = channel.visibleSourceTime;
        const std::shared_ptr<const SpectralFrame> average;
        const bool psdReady=false,stftReady=!spectrum->frames.empty();
        QImage stft; std::vector<float> rasterPower;
        if (stftReady) { rasterPower = spectralRaster(*spectrum, 560, 240, MainMode::TimeFrequency); stft = spectrumImage(rasterPower, QSize(560, 240)); }
        QString status = waveformReady ? QString("真实 DDC · 全可见窗分箱 · ")+amplitudeUnit(source) : QStringLiteral("IQ 读取或窄带计算失败");
        if (stftReady) status += QStringLiteral(" · N=%1 · Δf=%2 Hz · 帧 %3 · %4 ms · ENBW %5 Hz")
            .arg(spectrum->plan.points).arg(spectrum->plan.binHz, 0, 'g', 6).arg(spectrum->frames.size())
            .arg(spectrum->plan.requiredSeconds * 1000, 0, 'g', 6).arg(spectrum->plan.noiseBandwidthHz, 0, 'g', 6);
        else status += " · " + QString::fromStdString(spectrum->error);
        if(stftReady){std::uint64_t minValid=UINT64_MAX,maxValid=0,minPad=UINT64_MAX,maxPad=0;for(const auto& f:spectrum->frames){minValid=std::min(minValid,f->validSamples);maxValid=std::max(maxValid,f->validSamples);minPad=std::min(minPad,f->paddedSamples);maxPad=std::max(maxPad,f->paddedSamples);}status+=QString(" · 观测 %1–%2 ms · 补零 %3–%4/帧").arg(1000.0*minValid/spectrum->plan.analysisRateHz,0,'g',4).arg(1000.0*maxValid/spectrum->plan.analysisRateHz,0,'g',4).arg(minPad).arg(maxPad);}
        status += QString(" · 重叠 %1%→%2% · 步长 %3 · 补零帧 %4%5").arg(spectrum->requestedOverlap*100).arg(spectrum->effectiveOverlap*100,0,'f',2).arg(spectrum->effectiveHop).arg(spectrum->paddedFrames).arg(spectrum->repeatedObservation?" · 重复观测":"");
        if (cancelled()) return;
        if (!waveformReady) { waveI.clear(); waveQ.clear(); }
        QMetaObject::invokeMethod(this, [self = this, generation, config, waveI = std::move(waveI), waveQ = std::move(waveQ),
            psd = std::move(psd), stft = std::move(stft), status = std::move(status), samplePointsVisible,
            psdReady, stftReady, firstOutput, positions = std::move(positions), spectrum = std::move(spectrum), rasterPower = std::move(rasterPower), average]() mutable {
            self->installFrame(generation, config, std::move(waveI), std::move(waveQ), std::move(psd),
                std::move(stft), std::move(status), samplePointsVisible, psdReady, stftReady, firstOutput,
                std::move(positions), std::move(spectrum), std::move(rasterPower), average);
        }, Qt::QueuedConnection);
    });
}

void NarrowbandWorkspace::installFrame(std::uint64_t generation, std::uint64_t configVersion,
                                       std::vector<float> waveformI, std::vector<float> waveformQ,
                                       std::vector<float> psd, QImage stft, QString status,
                                       bool samplePointsVisible, bool psdReady, bool stftReady,
                                       std::uint64_t firstOutputSample, std::vector<SampleIndex> samplePositions,
                                       std::shared_ptr<const SpectrogramData> spectrum, std::vector<float> rasterPower, std::shared_ptr<const SpectralFrame> average) {
    auto* channel = session_.activeChannel();
    if (!channel || generation != requestGeneration_ || channel->configVersion != configVersion) return;
    frameGeneration_ = generation; displayPending_ = false; displayCacheStats_ = sampleCache_.stats();
    session_.installSpectrogram(channel->id, spectrum);
    if(average){averagePower_=std::move(average);averagePsd_=psd;} linkedPsdKey_.clear(); rasterPower_ = std::move(rasterPower); powerColorKey_.clear();
    waveform_->setSamplePositions(std::move(samplePositions));
    for (auto* chart : {stft_, modulationStft_, recognitionStft_}) if (chart) chart->setSpectralData(spectrum);
    if (waveform_)
        waveform_->setYAxisRange(channel->waveformAxisMinimum, channel->waveformAxisMaximum,
                                 waveformAxisLabel(channel->waveform, session_.activeFile() ? &session_.activeFile()->metadata : nullptr));
    if (waveform_) {
        waveform_->setSamplePointOrigin(firstOutputSample);
        waveform_->setSamplePointsVisible(samplePointsVisible && !waveformI.empty());
    }
    if (psd_) psd_->setYAxisRange(channel->psdAxisMinimum, channel->psdAxisMaximum, session_.activeFile() ? powerUnit(session_.activeFile()->metadata) : QStringLiteral("dBFS/Hz"));
    const bool waveformReady = !waveformI.empty();
    QString stftSummary = stftReady ? QStringLiteral("STFT") : QStringLiteral("STFT 不可用");
    if (spectrum) {
        std::uint64_t maximumPadding = 0;
        for (const auto& frame : spectrum->frames) maximumPadding = std::max(maximumPadding, frame->paddedSamples);
        if (maximumPadding) stftSummary += QStringLiteral(" · 补零最多 %1/帧").arg(maximumPadding);
        if (!spectrum->error.empty()) stftSummary += " · 分析异常";
        if (spectrum->repeatedObservation) stftSummary += " · 重复观测";
    }
    const QString summary = (waveformReady ? QStringLiteral("真实 DDC") : QStringLiteral("IQ 读取或窄带计算失败")) + " · " + stftSummary;
    if (stftSummary.contains("补零")) status += "；补零不提高真实分辨率";
    if (!waveformI.empty()) {
        if (waveformQ.empty()) waveform_->setSeries({std::move(waveformI)}, status, "真实 DDC");
        else waveform_->setSeries({std::move(waveformI), std::move(waveformQ)}, status, "真实 DDC");
    } else waveform_->setSeries({}, status);
    fitWaveformAmplitude();
    if (psdReady && !psd.empty()) psd_->setSeries({std::move(psd)}, status, "统计谱");
    const auto revision = QStringLiteral("nb-%1-%2").arg(configVersion).arg(generation);
    if (stftReady && !stft.isNull()) {
        stft_->setRaster(stft, status, revision, stftSummary);
        if (modulationStft_) modulationStft_->setRaster(stft, status, revision, stftSummary);
        if (recognitionStft_) recognitionStft_->setRaster(stft, status, revision, stftSummary);
    } else {
        const QString unavailable = status;
        stft_->setRaster({}, unavailable);
        if (modulationStft_) modulationStft_->setRaster({}, unavailable);
        if (recognitionStft_) recognitionStft_->setRaster({}, unavailable);
    }
    setDataStatus(summary, status);
    updateLinkedCursors(); updatePowerColors();
    emit displayParametersChanged();
}

void NarrowbandWorkspace::cancelPsd() {
    if(psdCancellation_)psdCancellation_->store(true);
    ++psdGeneration_;if(psdWorker_.joinable())psdWorker_.join();psdCancellation_.reset();psdRequestKey_.clear();
}
void NarrowbandWorkspace::requestPsd() {
    const auto* active=session_.activeChannel();const auto* file=session_.activeFile();if(!active||!file)return;
    const auto channel=*active;const auto source=file->metadata;
    TimeRange time=channel.psd.scope==PsdScope::Whole?channel.sourceTime:channel.visibleSourceTime;
    if(channel.psd.scope==PsdScope::SourceMark) {
        if(const auto* mark=findMark(*file,channel.sourceMarkId))time={std::max(channel.sourceTime.begin,mark->range.time.begin),std::min(channel.sourceTime.end,mark->range.time.end)};
    }
    const auto key=QString::fromStdString(channel.id)+QString("/%1/%2/%3/%4/%5/%6/%7/%8")
        .arg(session_.projectGeneration()).arg(channel.configVersion).arg(time.begin).arg(time.end)
        .arg(channel.visibleBasebandFrequency.lowerHz,0,'g',17).arg(channel.visibleBasebandFrequency.upperHz,0,'g',17)
        .arg(channel.psdFftSize).arg(static_cast<int>(channel.psd.statistic))+
        QString::fromStdString(spectralParameterKey(channel.psd.parameters))+iqSourceFingerprint(QString::fromStdString(source.path));
    if(key==psdRequestKey_)return;
    cancelPsd();psdRequestKey_=key;averagePower_.reset();averagePsd_.clear();linkedPsdKey_.clear();
    psd_->setProperty("psdComplete",false);psdStatus_="正在流式统计独立 PSD…";updateLinkedCursors();
    ChannelDspPlan plan;QString error;
    if(!makeChannelDspPlan(source,channel,plan,error)){psdStatus_=error;return;}
    std::uint64_t first=0,last=0;
    if(time.end<=time.begin||!mulDiv(time.begin-channel.sourceTime.begin,plan.outputRateHz,plan.sourceRateHz,true,first)||
        !mulDiv(time.end-channel.sourceTime.begin,plan.outputRateHz,plan.sourceRateHz,false,last)||last<=first){psdStatus_="PSD 统计范围没有有效输出样本";return;}
    const auto generation=++psdGeneration_,project=session_.projectGeneration();
    auto stop=std::make_shared<std::atomic_bool>(false);psdCancellation_=stop;
    psdWorker_=std::thread([this,channel,source,plan,first,last,generation,project,stop]{
        const auto cancelled=[stop]{return stop->load();};SpectralSource provider;provider.sampleRateHz=channel.outputSampleRateHz;
        provider.sourceRange=[channel,plan](TimeRange range){std::uint64_t begin=0,end=0;mulDiv(range.begin,plan.sourceRateHz,plan.outputRateHz,false,begin);mulDiv(range.end,plan.sourceRateHz,plan.outputRateHz,true,end);return TimeRange{channel.sourceTime.begin+begin,std::min(channel.sourceTime.end,channel.sourceTime.begin+end)};};
        provider.read=[this,&source,&channel,&plan](TimeRange range,auto& values,const auto& cancel){ChannelSampleData block;const bool ok=sampleCache_.process(source,channel,plan,range,block,cancel);if(ok)values=std::move(block.samples);return ok;};
        const auto deliver=[this,generation,project,channel](std::shared_ptr<const SpectralFrame> frame,QString status,bool complete){
            QMetaObject::invokeMethod(this,[this,generation,project,channel,frame=std::move(frame),status=std::move(status),complete]{
                const auto* active=session_.activeChannel();if(generation!=psdGeneration_||project!=session_.projectGeneration()||!active||active->id!=channel.id||active->configVersion!=channel.configVersion)return;
                averagePower_=frame;averagePsd_=frame?spectrumDb(*frame):std::vector<float>{};psdStatus_=status;linkedPsdKey_.clear();
                psd_->setProperty("psdComplete",complete);updateLinkedCursors();emit displayParametersChanged();
            },Qt::QueuedConnection);
        };
        auto lastUpdate=std::chrono::steady_clock::now();std::string error;
        const auto progress=[&](std::uint64_t done,std::uint64_t total,std::shared_ptr<const SpectralFrame> frame){
            const auto now=std::chrono::steady_clock::now();if(now-lastUpdate<std::chrono::milliseconds(150))return;lastUpdate=now;
            deliver(std::move(frame),QString("未完成 · PSD %1 / %2 段 (%3%)").arg(done).arg(total).arg(100.0*done/total,0,'f',1),false);
        };
        auto frame=analyzeSpectrum(provider,{first,last},channel.visibleBasebandFrequency,channel.psdFftSize,channel.psd,error,cancelled,progress);
        if(cancelled())return;
        const QString status=frame?QString("PSD 完成 · %1 · N=%2 · Δf=%3 Hz · 有效 %4 ms · 补零 %5 · ENBW %6 Hz · %7 段").arg(channel.psd.scope==PsdScope::Whole?"整个通道":"所选时间范围").arg(channel.psdFftSize).arg(frame->binHz,0,'g',6).arg(frame->observedSeconds*1000,0,'g',6).arg(frame->paddedSamples).arg(frame->noiseBandwidthHz,0,'g',6).arg(frame->processedFrames):QString::fromStdString(error);
        deliver(std::move(frame),status,true);
    });
}
void NarrowbandWorkspace::cancelAnalysis(bool userInitiated) {
    if(!analysisPending_ && !analysisResult_ && !lastAnalysisIdentity_ &&
       (!analysisService_ || !analysisService_->busy()))return;
    if(analysisService_)analysisService_->cancel();
    ++analysisGeneration_;analysisPending_=false;analysisResult_.reset();
    if(!userInitiated)lastAnalysisIdentity_.reset();
    if(auto* candidates=findChild<QComboBox*>("analysisRateCandidates")){QSignalBlocker b(candidates);candidates->clear();}
    if(constellationLarge_)constellationLarge_->setPoints({},"当前没有有效的真实星座结果。");
    if(eyeLarge_)eyeLarge_->setEye({},"当前没有有效的真实眼图结果。");
    setAnalysisStatus(userInitiated ? "分析已取消；点击分析可重新运行。" : "分析上下文已变化；旧结果已清除。");
}

void NarrowbandWorkspace::setAnalysisStatus(const QString& text) {
    if (modulationStatus_) modulationStatus_->setText(text);
    if (analysisSummary_) {
        analysisSummary_->setText(text.section('\n', 0, 0).left(220));
        analysisSummary_->setToolTip(text);
    }
}

void NarrowbandWorkspace::requestAnalysis(bool force) {
    const auto* channel=session_.activeChannel();const auto* file=session_.activeFile();
    if(!analysisService_ || !channel || !file)return;
    // Refresh can run before the result timer. Check the old physical revision
    // before replacing its request identity, so it cannot hide an external edit.
    if(invalidateChangedAnalysisSource() && !force)return;
    const auto sourceKey=analysisSourceKey(file->metadata,*channel);
    if(!force && lastAnalysisIdentity_ && lastAnalysisIdentity_->sourceKey==sourceKey &&
       lastAnalysisIdentity_->projectGeneration==session_.projectGeneration() &&
       lastAnalysisTime_==channel->visibleSourceTime && lastAnalysisSettings_==processingSettings(*channel)) {
      return;
    }
    if(channel->processingState!=ChannelProcessingState::Ready || file->metadata.demo) {
      cancelAnalysis();if(modulationStatus_)setAnalysisStatus("来源没有可用的真实通道 IQ。");return;
    }
    std::uint64_t first=0,last=0;
    if(channel->visibleSourceTime.begin<channel->sourceTime.begin ||
       !mulDiv(channel->visibleSourceTime.begin-channel->sourceTime.begin,rateHz(channel->outputSampleRateHz),rateHz(file->metadata.sampleRateHz),true,first) ||
       !mulDiv(channel->visibleSourceTime.end-channel->sourceTime.begin,rateHz(channel->outputSampleRateHz),rateHz(file->metadata.sampleRateHz),false,last)) {
      cancelAnalysis();if(modulationStatus_)setAnalysisStatus("分析区间不能映射到实际输出采样网格。");return;
    }
    AnalysisRequest request;
    request.identity={session_.projectGeneration(),++analysisGeneration_,channel->configVersion,channel->id,sourceKey,{first,last}};
    request.sampleBudget=channel->analysis.sampleBudget;
    lastAnalysisIdentity_=request.identity;lastAnalysisTime_=channel->visibleSourceTime;lastAnalysisSettings_=processingSettings(*channel);
    lastAnalysisPhysicalRevision_=analysisPhysicalRevision(file->metadata);
    analysisPending_=true;analysisResult_.reset();
    if(auto* candidates=findChild<QComboBox*>("analysisRateCandidates")){QSignalBlocker b(candidates);candidates->clear();}
    constellationLarge_->setPoints({},"正在读取真实连续窄带 IQ…");
    eyeLarge_->setEye({},"正在读取真实连续窄带 IQ…");
    if(modulationStatus_)setAnalysisStatus("正在分析真实 IQ；仅发布当前通道与配置结果…");
    analysisService_->submit(file->metadata,*channel,request,lastAnalysisSettings_);
}

bool NarrowbandWorkspace::invalidateChangedAnalysisSource() {
    const auto* channel=session_.activeChannel();const auto* file=session_.activeFile();
    if(!lastAnalysisIdentity_ || !channel || !file ||
       lastAnalysisIdentity_->channelId!=channel->id ||
       lastAnalysisIdentity_->projectGeneration!=session_.projectGeneration())return false;
    const auto revision=analysisPhysicalRevision(file->metadata);
    if(lastAnalysisPhysicalRevision_==revision)return false;
    cancelAnalysis(true);
    // Keep an up-to-date cancellation marker until an explicit analysis action.
    // Loader-prefix and normal context changes still follow automatic refresh.
    lastAnalysisIdentity_->sourceKey=analysisSourceKey(file->metadata,*channel);
    lastAnalysisIdentity_->channelConfigVersion=channel->configVersion;
    lastAnalysisIdentity_->requestGeneration=analysisGeneration_;
    lastAnalysisTime_=channel->visibleSourceTime;
    lastAnalysisSettings_=processingSettings(*channel);
    lastAnalysisPhysicalRevision_=revision;
    if(modulationStatus_)setAnalysisStatus("来源文件已变化；旧结果失效，请重新分析。");
    return true;
}

void NarrowbandWorkspace::installAnalysis() {
    if(!analysisService_)return;
    if(invalidateChangedAnalysisSource())return;
    const auto* channel=session_.activeChannel();const auto* file=session_.activeFile();
    if(lastAnalysisIdentity_ && (!channel || !file ||
       !analysisIdentityCurrent(*lastAnalysisIdentity_,file->metadata,*channel,session_.projectGeneration(),lastAnalysisIdentity_->requestGeneration) ||
       lastAnalysisTime_!=channel->visibleSourceTime || lastAnalysisSettings_!=processingSettings(*channel))) {
      cancelAnalysis();
      if(modulationStatus_)setAnalysisStatus("来源、可用前缀或分析上下文已变化；旧结果失效，请重新分析。");return;
    }
    auto result=analysisService_->takeResult();if(!result)return;
    if(!channel || !file || !analysisIdentityCurrent(result->identity,file->metadata,*channel,session_.projectGeneration(),analysisGeneration_) ||
       lastAnalysisTime_!=channel->visibleSourceTime || lastAnalysisSettings_!=processingSettings(*channel)) {
      if(lastAnalysisIdentity_ && result->identity.requestGeneration==lastAnalysisIdentity_->requestGeneration){cancelAnalysis();if(modulationStatus_)setAnalysisStatus("分析来源已变化；旧结果失效。");}return;
    }
    analysisPending_=false;analysisResult_=std::move(result);
    if(analysisResult_->status!=AnalysisStatus::Ready) {
      constellationLarge_->setPoints({},str(analysisResult_->message));
      eyeLarge_->setEye({},str(analysisResult_->message));
      if(modulationStatus_)setAnalysisStatus(str(analysisResult_->message));return;
    }
    if(auto* candidates=findChild<QComboBox*>("analysisRateCandidates")) {
      QSignalBlocker b(candidates);candidates->clear();
      for(const auto& candidate:analysisResult_->rateCandidates)candidates->addItem(QString("%1 Sym/s · 评分 %2 · %3 dB · %4")
        .arg(candidate.rateHz,0,'g',10).arg(candidate.score,0,'f',2).arg(candidate.prominenceDb,0,'f',1).arg(str(candidate.method)),candidate.rateHz);
    }
    renderAnalysis();
}

void NarrowbandWorkspace::renderAnalysis() {
    const auto* channel = session_.activeChannel(); const auto* file = session_.activeFile();
    if (!channel || !file || !analysisResult_ || analysisResult_->status != AnalysisStatus::Ready ||
        !analysisIdentityCurrent(analysisResult_->identity,file->metadata,*channel,session_.projectGeneration(),analysisGeneration_) ||
        lastAnalysisTime_ != channel->visibleSourceTime || lastAnalysisSettings_ != processingSettings(*channel)) return;
    const auto& data=*analysisResult_;
    const auto projectionKey=QStringList{QString::number(data.identity.requestGeneration),
        QString::number(channel->analysis.pointCount),QString::number(channel->analysis.constellationAutoScale),
        QString::number(channel->constellationMinimum,'g',17),QString::number(channel->constellationMaximum,'g',17),
        QString::number(channel->eyeComponent),QString::number(channel->eyeTraces),
        QString::number(channel->analysis.eyePointsPerUi)}.join('/');
    if(analysisProjectionKey_==projectionKey)return;
    analysisProjectionKey_=projectionKey;
    if(channel->analysis.symbolRateMode==SymbolRateMode::Estimated && data.symbolRateHz>0)
      if(auto* rate=findChild<QDoubleSpinBox*>("symbolRate")){QSignalBlocker b(rate);displayParameterValue(rate,data.symbolRateHz);}
    const double minimum=channel->analysis.constellationAutoScale?data.axisMinimum:channel->constellationMinimum;
    const double maximum=channel->analysis.constellationAutoScale?data.axisMaximum:channel->constellationMaximum;
    constellationLarge_->setIqRange(minimum,maximum,channel->analysis.normalizePower?"RMS 归一化":"解码 IQ 幅度");
    const auto count = std::min(data.constellation.size(), static_cast<std::size_t>(channel->analysis.pointCount));
    std::vector<std::complex<float>> points; points.reserve(count);
    for (std::size_t i=0; i<count; ++i) {
      const auto index = count > 1 ? i*(data.constellation.size()-1)/(count-1) : 0;
      points.push_back(data.constellation[index].value);
    }
    const auto detail=QString("%1 · %2 点 / %3 有效样本 · Fs %4 S/s · output [%5,%6) · source [%7,%8) · 排除边界 %9/%10")
      .arg(str(data.message)).arg(points.size()).arg(data.source->samples.size()).arg(data.source->outputRateHz)
      .arg(data.source->outputSamples.begin).arg(data.source->outputSamples.end)
      .arg(data.source->sourceSamples.begin).arg(data.source->sourceSamples.end).arg(data.source->discardedLeft).arg(data.source->discardedRight);
    const QStringList stageNames{"原始 IQ（未同步）","CFO 校正","匹配滤波","定时符号","载波恢复"};
    const bool fallback=channel->analysis.constellationStage!=data.constellationStage;
    const auto stageSummary=fallback?QString("同步未完成 · 请求%1 / 显示%2").arg(stageNames.at(static_cast<int>(channel->analysis.constellationStage)),stageNames.at(static_cast<int>(data.constellationStage))):
      QString("真实 IQ · %1 · %2 点").arg(stageNames.at(static_cast<int>(data.constellationStage))).arg(count);
    constellationLarge_->setPoints(std::move(points),detail,stageSummary);
    constellationLarge_->setProperty("requestedAnalysisStage",static_cast<int>(channel->analysis.constellationStage));
    constellationLarge_->setProperty("analysisStageFallback",fallback);
    constellationLarge_->setProperty("analysisStage",static_cast<int>(data.constellationStage));
    constellationLarge_->setProperty("timingLocked",data.timingLocked);constellationLarge_->setProperty("carrierLocked",data.carrierLocked);
    constellationLarge_->setProperty("realIqSource",true);
    constellationLarge_->setProperty("sourceChannelId",str(channel->id));
    constellationLarge_->setProperty("sourceConfigVersion",static_cast<qulonglong>(channel->configVersion));
    constellationLarge_->setProperty("actualSampleRateHz",static_cast<qulonglong>(data.source->outputRateHz));
    if(data.eyeStatus==AnalysisStatus::Ready)eyeLarge_->setRealEye(data,channel->eyeComponent,channel->eyeTraces,channel->analysis.eyePointsPerUi);
    else eyeLarge_->setEye({},str(data.eyeMessage));
    const auto eyeDetail=QString("\n%1 · K_B=%2× · 实际 SPS=%3 · 绘图插值 %4 点/UI · 有效轨迹 %5 · RRC 丢弃 %6 样本")
      .arg(str(data.eyeMessage)).arg(static_cast<double>(data.source->outputRateHz)/channel->bandwidthHz,0,'g',6)
      .arg(data.symbolRateHz>0?QString::number(data.samplesPerSymbol,'g',9):QString("未给定 Rs"))
      .arg(channel->analysis.eyePointsPerUi).arg(std::min(data.eyes.size(),static_cast<std::size_t>(channel->eyeTraces))).arg(data.filterDiscardedSamples);
    const auto syncDetail=QString("\n%1 · CFO %2 Hz · 定时周期 %3 samples · 捕获丢弃 %4 symbols%5")
      .arg(str(data.syncMessage)).arg(data.estimatedCfoHz,0,'g',9).arg(data.timingPeriodSamples,0,'g',9).arg(data.acquisitionSymbolsDiscarded)
      .arg(data.carrierLocked && data.conditionalEvm>=0?QString(" · 条件 EVM（所选模板）%1%").arg(data.conditionalEvm*100,0,'f',2):QString{});
    if(modulationStatus_)setAnalysisStatus(detail+eyeDetail+syncDetail);
}

void NarrowbandWorkspace::cancelWork() {
    if (displaySettleTimer_) displaySettleTimer_->stop();
    displayPending_ = false;
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
    ++requestGeneration_;
    if (worker_.joinable()) worker_.join();
    cancellation_.reset();
}

void NarrowbandWorkspace::updateCaption() {
    const auto* channel = session_.activeChannel(); const auto* file = session_.activeFile();
    if (!channel || !file) return;
    const double center = channel->centerFrequencyHz / 1e6, bandwidth = channel->bandwidthHz / 1e6;
    caption_->setText(str(channel->name));
    caption_->setToolTip(QStringLiteral("%1\n来源：%2 / %3\nFc %4 MHz · BW %5 MHz · Fs′ %6 MS/s")
        .arg(str(channel->name), str(file->metadata.name), str(channel->sourceMarkId))
        .arg(center, 0, 'f', 3).arg(bandwidth, 0, 'f', 3).arg(channel->outputSampleRateHz / 1e6, 0, 'f', 3));
    updateLinkedCursors();
}

void NarrowbandWorkspace::startDemoRecognition() {
    if (!runRecognition_ || !runRecognition_->isEnabled()) return;
    const auto* channel = session_.activeChannel(); if (!channel) return;
    const auto* file = session_.activeFile(); if (!file) return;
    const int frame = fftSize_ ? fftSize_->currentText().toInt() : 2048;
    TimeRange range = channel->visibleSourceTime;
    if (recognitionScope_ && recognitionScope_->currentIndex() == 1) range = channel->sourceTime;
    if (recognitionScope_ && recognitionScope_->currentIndex() == 2)
        range = channel->wholeSource ? TimeRange{0, file->metadata.sampleCount} : channel->sourceTime;
    const double targetRate = recognitionSampleRate_ ? recognitionSampleRate_->value() * 1e6 : channel->outputSampleRateHz;
    const double requiredRate = channel->bandwidthHz * 1.3;
    const long double durationSeconds = file->metadata.sampleRateHz > 0
        ? static_cast<long double>(range.end - range.begin) / file->metadata.sampleRateHz : 0;
    const long double availableTargetSamples = durationSeconds * targetRate;
    QString invalid;
    if (frame < 256 || (frame & (frame - 1)) != 0) invalid = "帧长必须是 2 的幂";
    else if (range.end <= range.begin) invalid = "分析范围没有有效样本";
    else if (file->metadata.sampleRateHz <= 0) invalid = "源文件采样率无效";
    else if (channel->bandwidthHz <= 0 || targetRate < requiredRate) invalid = "目标采样率需满足 Fs ≥ BW + 2Δf";
    else if (std::abs(targetRate - channel->outputSampleRateHz) > 0.5 &&
             recognitionAllowResample_ && !recognitionAllowResample_->isChecked()) invalid = "采样率不一致；请启用演示预处理重采样";
    else if (availableTargetSamples < frame) invalid = QStringLiteral("有效点数不足一帧：约 %1 / %2").arg(static_cast<qulonglong>(availableTargetSamples)).arg(frame);
    if (!invalid.isEmpty()) { analysisStatus_->setText("校验失败：" + invalid); return; }
    stopRecognition();
    recognitionSegments_.clear(); selectedRecognitionSegment_ = -1; recognitionResultStale_ = false;
    recognitionChannelId_ = channel->id; recognitionSourceFileId_ = file->metadata.id;
    recognitionSourceMarkId_ = channel->sourceMarkId; recognitionConfigVersion_ = channel->configVersion;
    recognitionRangeBegin_ = range.begin; recognitionRangeEnd_ = range.end;
    recognitionActive_ = true; recognitionProgressValue_ = 0; ++recognitionGeneration_;
    analysisStatus_->setText("合成示例运行中…"); analysisStatus_->setToolTip("分数与进度仅用于演示，未加载权重或执行真实模型推理。");
    if (timeline_) timeline_->setSeries({}, "等待合成示例结果");
    updateRecognitionDetail();
    runRecognition_->setEnabled(false); stopRecognition_->setEnabled(true); recognitionProgress_->setValue(0); recognitionTimer_->start();
    if (!selectedModelFile_.isEmpty() && log_) log_("识别演示使用模型文件名作为界面记录；未读取或执行权重");
}

void NarrowbandWorkspace::stopDemoRecognition() {
    stopRecognition();
    if (analysisStatus_) analysisStatus_->setText("识别演示已停止");
    if (recognitionSegments_.empty()) {
        if (recognitionTop5_) recognitionTop5_->setText("Top-5 候选 · 演示已停止，尚无完成结果");
        if (timeline_) timeline_->setSeries({}, "合成示例已停止");
    }
    if (recognitionStats_[3]) recognitionStats_[3]->setText("任务状态\n已停止");
}

void NarrowbandWorkspace::stopRecognition() {
    if (recognitionTimer_) recognitionTimer_->stop();
    recognitionActive_ = false;
    if (runRecognition_) runRecognition_->setEnabled(session_.activeChannel() != nullptr);
    if (stopRecognition_) stopRecognition_->setEnabled(false);
    updateRecognitionSummary();
}

void NarrowbandWorkspace::updateRecognition() {
    if (!recognitionActive_) return;
    recognitionProgressValue_ = std::min(100, recognitionProgressValue_ + 6 + static_cast<int>(recognitionGeneration_ % 4));
    recognitionProgress_->setValue(recognitionProgressValue_);
    if (analysisStatus_) analysisStatus_->setText(QStringLiteral("合成示例运行中 · %1%").arg(recognitionProgressValue_));
    if (recognitionProgressValue_ >= 100) {
        stopRecognition();
        const auto span = recognitionRangeEnd_ - recognitionRangeBegin_;
        const auto count = static_cast<int>(std::clamp<std::uint64_t>(span, 1, 18));
        constexpr std::array<const char*, 5> labels{"BPSK", "QPSK", "8PSK", "16QAM", "FSK"};
        recognitionSegments_.clear(); recognitionSegments_.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            std::uint64_t beginOffset = 0, endOffset = 0;
            mulDiv(span, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(count), false, beginOffset);
            mulDiv(span, static_cast<std::uint64_t>(i + 1), static_cast<std::uint64_t>(count), true, endOffset);
            RecognitionSegment segment;
            segment.sourceBegin = recognitionRangeBegin_ + beginOffset;
            segment.sourceEnd = recognitionRangeBegin_ + std::max(beginOffset + 1, endOffset);
            double total = 0;
            for (std::size_t k = 0; k < segment.scores.size(); ++k) {
                const auto phase = static_cast<double>(recognitionGeneration_) * .73 + i * .41 + k * 1.19;
                segment.scores[k] = .12 + std::abs(std::sin(phase)) + .25 * std::abs(std::cos(phase * .37));
                total += segment.scores[k];
            }
            for (auto& score : segment.scores) score /= total;
            segment.topClass = static_cast<int>(std::distance(segment.scores.begin(), std::max_element(segment.scores.begin(), segment.scores.end())));
            recognitionSegments_.push_back(segment);
        }
        if (analysisStatus_) analysisStatus_->setText(QStringLiteral("合成示例完成 · %1 个片段").arg(count));
        if (timeline_) {
            std::vector<float> scores; scores.reserve(recognitionSegments_.size());
            for (const auto& segment : recognitionSegments_) scores.push_back(static_cast<float>(segment.scores[static_cast<std::size_t>(segment.topClass)]));
            timeline_->setSeries({std::move(scores)}, QStringLiteral("合成示例 · %1 个片段，分数不代表真实模型输出").arg(count), "合成示例");
        }
        selectedRecognitionSegment_ = 0; updateRecognitionDetail();
        if (log_) log_(QStringLiteral("合成识别任务完成；%1 段 Top-5 仅用于界面演示").arg(count));
    }
}

void NarrowbandWorkspace::markRecognitionStale(const QString& reason) {
    if (recognitionActive_) {
        stopRecognition();
        if (analysisStatus_) analysisStatus_->setText(reason + " · 已取消当前演示任务");
    }
    if (recognitionSegments_.empty()) return;
    recognitionResultStale_ = true;
    if (analysisStatus_) analysisStatus_->setText(reason + " · 上次合成结果已过期");
    updateRecognitionDetail();
}

void NarrowbandWorkspace::updateRecognitionDetail() {
    updateRecognitionSummary();
    if (!recognitionTop5_) return;
    if (recognitionSegments_.empty()) {
        recognitionTop5_->setText(recognitionActive_ ? "Top-5 候选 · 合成示例运行中…" : "Top-5 候选 · 等待合成示例结果");
        recognitionTop5_->setToolTip({}); return;
    }
    const int index = std::clamp(selectedRecognitionSegment_, 0, static_cast<int>(recognitionSegments_.size()) - 1);
    selectedRecognitionSegment_ = index;
    const auto& segment = recognitionSegments_[static_cast<std::size_t>(index)];
    constexpr std::array<const char*, 5> labels{"BPSK", "QPSK", "8PSK", "16QAM", "FSK"};
    std::array<int, 5> order{0, 1, 2, 3, 4};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return segment.scores[static_cast<std::size_t>(a)] > segment.scores[static_cast<std::size_t>(b)]; });
    const auto* current = session_.activeChannel();
    const bool stale = recognitionResultStale_ || !current || current->id != recognitionChannelId_ || current->configVersion != recognitionConfigVersion_;
    const double threshold = recognitionThreshold_ ? recognitionThreshold_->value() : .5;
    const QString predicted = segment.scores[static_cast<std::size_t>(order[0])] < threshold ? QStringLiteral("未知 / 需复核") : QString::fromLatin1(labels[order[0]]);
    QString detail = QStringLiteral("片段 %1 / %2 · %3%4 · 合成示例")
        .arg(index + 1).arg(recognitionSegments_.size())
        .arg(predicted).arg(stale ? QStringLiteral(" · 已过期") : QString{});
    for (int rank = 0; rank < 5; ++rank) {
        const int category = order[static_cast<std::size_t>(rank)];
        detail += QStringLiteral("\n%1. %2  %3%")
            .arg(rank + 1).arg(QString::fromLatin1(labels[category]))
            .arg(segment.scores[static_cast<std::size_t>(category)] * 100, 0, 'f', 1);
    }
    recognitionTop5_->setText(detail);
    recognitionTop5_->setToolTip(QStringLiteral("源样本 [%1, %2)\nTop-5 原始合成分数，不代表真实模型输出。\n%3")
        .arg(segment.sourceBegin).arg(segment.sourceEnd).arg(detail));
}

void NarrowbandWorkspace::updateRecognitionSummary() {
    const std::array<QString, 4> titles{"信号片段", "主类别", "平均置信度", "任务状态"};
    std::array<QString, 4> values{"—", "—", "—", recognitionActive_ ? "运行中" : "待运行"};
    if (!recognitionSegments_.empty()) {
        constexpr std::array<const char*, 6> labels{"BPSK", "QPSK", "8PSK", "16QAM", "FSK", "未知 / 需复核"};
        std::array<std::size_t, 6> counts{};
        double confidence = 0;
        const double threshold = recognitionThreshold_ ? recognitionThreshold_->value() : .5;
        for (const auto& segment : recognitionSegments_) {
            const auto score = segment.scores[static_cast<std::size_t>(segment.topClass)];
            ++counts[score < threshold ? 5 : static_cast<std::size_t>(segment.topClass)]; confidence += score;
        }
        const auto mainClass = static_cast<std::size_t>(std::distance(counts.begin(), std::max_element(counts.begin(), counts.end())));
        const auto* channel = session_.activeChannel();
        const bool stale = recognitionResultStale_ || !channel || channel->id != recognitionChannelId_ || channel->configVersion != recognitionConfigVersion_;
        values = {QString::number(static_cast<qulonglong>(recognitionSegments_.size())), QString::fromUtf8(labels[mainClass]),
            QString::number(confidence / recognitionSegments_.size() * 100, 'f', 1) + "%", stale ? "已过期" : "合成完成"};
    }
    for (std::size_t i = 0; i < recognitionStats_.size(); ++i)
        if (recognitionStats_[i]) recognitionStats_[i]->setText(titles[i] + "\n" + values[i]);
}

void NarrowbandWorkspace::exportRecognition() {
    if (recognitionSegments_.empty()) { if (analysisStatus_) analysisStatus_->setText("没有可导出的已完成识别演示结果"); return; }
    const auto path = QFileDialog::getSaveFileName(this, "导出演示识别 JSON", "narrowband-demo-recognition.json", "JSON (*.json)");
    if (path.isEmpty()) return;
    constexpr std::array<const char*, 5> labels{"BPSK", "QPSK", "8PSK", "16QAM", "FSK"};
    QJsonArray segments;
    for (std::size_t i = 0; i < recognitionSegments_.size(); ++i) {
        const auto& segment = recognitionSegments_[i];
        QJsonArray scores; for (const auto score : segment.scores) scores.append(score);
        const double threshold = recognitionThreshold_ ? recognitionThreshold_->value() : .5;
        const QString predicted = segment.scores[static_cast<std::size_t>(segment.topClass)] < threshold
            ? QStringLiteral("未知 / 需复核") : QString::fromLatin1(labels[static_cast<std::size_t>(segment.topClass)]);
        segments.append(QJsonObject{{"index", static_cast<int>(i)}, {"sourceBegin", QString::number(segment.sourceBegin)}, {"sourceEnd", QString::number(segment.sourceEnd)},
            {"prediction", predicted}, {"scores", scores}, {"synthetic", true}});
    }
    const auto* channel = session_.activeChannel();
    const bool stale = recognitionResultStale_ || !channel || channel->id != recognitionChannelId_ || channel->configVersion != recognitionConfigVersion_;
    const auto targetRate = recognitionSampleRate_ ? recognitionSampleRate_->value() * 1e6 : 0.0;
    QJsonObject config{{"frameLength", fftSize_ ? fftSize_->currentText().toInt() : 2048},
        {"sampleRateHz", targetRate}, {"normalization", recognitionNormalization_ ? recognitionNormalization_->currentText() : QString{}},
        {"overlap", recognitionOverlap_ ? recognitionOverlap_->currentText() : QStringLiteral("50%")},
        {"scope", recognitionScope_ ? recognitionScope_->currentText() : QStringLiteral("当前可见时间窗")},
        {"threshold", recognitionThreshold_ ? recognitionThreshold_->value() : .5},
        {"allowResample", recognitionAllowResample_ && recognitionAllowResample_->isChecked()}};
    QJsonObject object{{"format", "signal-studio-demo-ml-result"}, {"warning", "SYNTHETIC UI MOCK: no model inference was performed"},
        {"synthetic", true}, {"sourceFileId", str(recognitionSourceFileId_)}, {"channelId", str(recognitionChannelId_)},
        {"sourceMarkId", str(recognitionSourceMarkId_)}, {"channelConfigVersion", QString::number(recognitionConfigVersion_)},
        {"sourceSampleRange", QJsonObject{{"begin", QString::number(recognitionRangeBegin_)}, {"end", QString::number(recognitionRangeEnd_)}}},
        {"model", model_ ? model_->currentText() : QStringLiteral("ResNet18")}, {"selectedFileName", selectedModelFile_}, {"modelHash", ""},
        {"device", "演示无设备"}, {"stale", stale}, {"configuration", config}, {"segments", segments}};
    QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0) {
        if (log_) log_("无法写入演示识别 JSON: " + file.errorString()); return;
    }
    if (log_) log_("已导出合成示例识别结果");
}

void NarrowbandWorkspace::copyBitstream() { if (bitstream_) QApplication::clipboard()->setText(bitstream_->toPlainText()); }
void NarrowbandWorkspace::exportBitstream() {
    if (!bitstream_) return;
    const auto path = QFileDialog::getSaveFileName(this, "导出演示位流", "narrowband-demo-bits.txt", "Text (*.txt)");
    if (path.isEmpty()) return;
    QFile file(path); if (file.open(QIODevice::WriteOnly | QIODevice::Text)) file.write(bitstream_->toPlainText().toUtf8());
}

} // namespace signalstudio
