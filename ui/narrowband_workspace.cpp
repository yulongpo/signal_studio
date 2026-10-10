#include "ui/narrowband_workspace.h"

#include "infrastructure/channel_processor.h"
#include "infrastructure/int16_iq_file.h"
#include "ui/charts/accelerated_surface.h"
#include "ui/charts/chart_interaction.h"
#include "ui/charts/palette.h"

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
#include <QSignalBlocker>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
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
QLabel* textLabel(const QString& value, const QString& id = {}) {
    auto* label = new QLabel(value); label->setWordWrap(true);
    if (!id.isEmpty()) label->setObjectName(id);
    return label;
}
QPushButton* actionButton(const QString& value, const QString& id, QLayout* layout) {
    auto* button = new QPushButton(value); button->setObjectName(id); button->setCursor(Qt::PointingHandCursor);
    layout->addWidget(button); return button;
}
QString waveformAxisLabel(NarrowbandWaveform mode) {
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
        softwareFallback_ = qApp && qApp->property("softwareRenderer").toBool();
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        surface_ = new AcceleratedSurface(this); surface_->setMinimumSize(80, 70); layout->addWidget(surface_);
        surface_->setPainter([this](QPainter& painter) { draw(painter); });
        connect(surface_, &AcceleratedSurface::backendFailed, this, [this](const QString& reason) {
            softwareFallback_ = true; surface_->hide(); status_ = QStringLiteral("QPainter 软件回退 · %1").arg(reason); update();
        });
        if (softwareFallback_) surface_->hide();
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
    void setSeries(std::vector<std::vector<float>> series, QString status = {}) {
        series_ = std::move(series); status_ = std::move(status);
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
    void setTimeAxis(TimeRange range, double sampleRateHz, bool relative) {
        if (sampleRateHz <= 0 || range.end <= range.begin) return;
        timeStartSeconds_ = relative ? 0.0 : static_cast<double>(range.begin) / sampleRateHz;
        timeEndSeconds_ = relative ? static_cast<double>(range.end - range.begin) / sampleRateHz :
            static_cast<double>(range.end) / sampleRateHz;
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
                surface_->setHeatmap(scalarRaster_, plotRect(), rasterRevision_, chart_palette::texture(palette_));
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
    void setRaster(QImage image, QString status = {}, QString revision = {}) {
        if (!image.isNull()) {
            scalarRaster_ = image.format() == QImage::Format_Grayscale8 ? image : image.convertToFormat(QImage::Format_Grayscale8);
            raster_ = QImage(scalarRaster_.size(), QImage::Format_Indexed8);
            raster_.setColorTable(chart_palette::colorTable(palette_));
            for (int y = 0; y < scalarRaster_.height(); ++y)
                std::copy_n(scalarRaster_.constScanLine(y), scalarRaster_.width(), raster_.scanLine(y));
        } else {
            scalarRaster_ = {}; raster_ = {};
        }
        status_ = std::move(status);
        const auto key = revision.isEmpty() ?
            QString::number(reinterpret_cast<quintptr>(this), 16) + QLatin1Char('/') + QString::number(scalarRaster_.cacheKey()) : revision;
        rasterRevision_ = key;
        surface_->setHeatmap(scalarRaster_, scalarRaster_.isNull() ? QRectF{} : plotRect(), key, chart_palette::texture(palette_));
        invalidate();
    }
    void setPoints(std::vector<std::complex<float>> points, QString status = {}) {
        points_ = std::move(points); status_ = std::move(status); surface_->setHeatmap({}, {}, QString{}); markGeometryDirty();
    }
    void setEye(std::vector<std::vector<float>> traces, QString status = {}) {
        series_ = std::move(traces); status_ = std::move(status); surface_->setHeatmap({}, {}, QString{}); markGeometryDirty();
    }
    QString status() const { return status_; }
    bool isDisplaySettled() const { return softwareFallback_ || (surface_ && surface_->isReady() && !surface_->hasPendingUploads()); }
    bool gpuReady() const { return !softwareFallback_ && surface_ && surface_->isReady(); }
    quint64 textureUploadCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->textureUploadCount(); }
    quint64 completedFrameCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->completedFrameCount(); }
    quint64 gpuDataDrawCallCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->chartDrawCallCount() + surface_->heatmapDrawCallCount(); }
    quint64 gpuVertexUploadCount() const { return softwareFallback_ || !surface_ ? 0 : surface_->chartVertexUploadCount(); }
    QString backendDescription() const { return softwareFallback_ || !surface_ ? QStringLiteral("QPainter 软件绘制") : surface_->backendDescription(); }
    void invalidateVisibleOverlay() { invalidate(); }
    std::function<void(QPointF, QPointF, int, bool)> gesture;
    std::function<void()> cancelPendingGesture;
    std::function<void()> resetYAxis;
    std::function<void(bool)> navigateHistory;
    bool hasPendingInteraction() const { return dragging_; }
    void cancelInteraction() {
        dragging_ = false;
        if (mouseGrabber() == this) releaseMouse();
        invalidate();
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
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
        if (event->button() == Qt::LeftButton) {
            dragStart_ = dragEnd_ = event->position(); dragging_ = true;
            setFocus(); grabMouse(); event->accept(); return;
        }
        if (event->button() == Qt::RightButton) {
            cancelInteraction();
            if (cancelPendingGesture) cancelPendingGesture();
            event->accept(); return;
        }
        QWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_) { dragEnd_ = event->position(); invalidate(); event->accept(); return; }
        if (mode_ == ChartMode::Curve && samplePointsVisible_ && !series_.empty() && !series_.front().empty()) {
            const QRectF plot = plotRect();
            if (plot.contains(event->position())) {
                const auto& values = series_.front();
                const double unit = std::clamp((event->position().x() - plot.left()) / plot.width(), 0.0, 1.0);
                const auto index = static_cast<std::size_t>(std::llround(unit * (values.size() - 1)));
                const double x = plot.left() + plot.width() * index / std::max<std::size_t>(1, values.size() - 1);
                const double y = plot.top() + (yAxisMaximum_ - values[index]) /
                    std::max(1e-12, yAxisMaximum_ - yAxisMinimum_) * plot.height();
                if (std::hypot(event->position().x() - x, event->position().y() - y) <= 12.0) {
                    const double time = timeStartSeconds_ + (timeEndSeconds_ - timeStartSeconds_) * unit;
                    QString readout = QStringLiteral("输出样本 #%1\nt = %2 s\n%3 = %4")
                        .arg(samplePointOrigin_ + index)
                        .arg(time, 0, 'g', 12)
                        .arg(yAxisLabel_)
                        .arg(values[index], 0, 'g', 8);
                    if (series_.size() > 1 && index < series_[1].size())
                        readout += QStringLiteral("\nQ = %1").arg(series_[1][index], 0, 'g', 8);
                    QToolTip::showText(mapToGlobal(event->position().toPoint() + QPoint(12, 12)), readout, this);
                    event->accept(); return;
                }
            }
        }
        QToolTip::hideText();
        QWidget::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragEnd_ = event->position(); dragging_ = false;
            if (mouseGrabber() == this) releaseMouse();
            if (gesture) gesture(QPointF(dragStart_.x() / std::max(1, width()), dragStart_.y() / std::max(1, height())),
                                 QPointF(dragEnd_.x() / std::max(1, width()), dragEnd_.y() / std::max(1, height())), 0,
                                 (dragEnd_ - dragStart_).manhattanLength() > 7);
            invalidate(); event->accept(); return;
        }
        QWidget::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            cancelInteraction();
            if (cancelPendingGesture) cancelPendingGesture();
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
                const double x = plot.left() + plot.width() * index / std::max<std::size_t>(1, values.size() - 1);
                double y = .5;
                if (eye) y = .5 - std::clamp<double>(values[index], -1.3, 1.3) * .34;
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
            const double side = std::min(plot.width(), plot.height());
            constexpr int sectors = 8;
            for (const auto& point : points_) {
                const QPointF center(plot.center().x() + point.real() * side * .42,
                                     plot.center().y() - point.imag() * side * .42);
                for (int sector = 0; sector < sectors; ++sector) {
                    const double a0 = 2.0 * std::numbers::pi * sector / sectors;
                    const double a1 = 2.0 * std::numbers::pi * (sector + 1) / sectors;
                    append(center, color);
                    append(center + QPointF(3.0 * std::cos(a0), 3.0 * std::sin(a0)), color);
                    append(center + QPointF(3.0 * std::cos(a1), 3.0 * std::sin(a1)), color);
                }
            }
            draws.push_back({AcceleratedSurface::ChartDrawCall::Topology::Triangles, first,
                             static_cast<quint32>(vertices.size() - first)});
        } else if (mode_ != ChartMode::Navigation) {
            const std::array<QColor, 3> colors{QColor("#58cef2"), QColor("#edc16f"), QColor("#f179ac")};
            for (std::size_t series = 0; series < series_.size(); ++series)
                addLine(series_[series], mode_ == ChartMode::Eye ? QColor(72, 207, 196, 80 + series % 75) : colors[series % colors.size()],
                        mode_ == ChartMode::Eye);
            if (mode_ == ChartMode::Curve && samplePointsVisible_) {
                constexpr int sectors = 8;
                for (std::size_t series = 0; series < series_.size(); ++series) {
                    const auto& values = series_[series];
                    const auto first = static_cast<quint32>(vertices.size());
                    for (std::size_t index = 0; index < values.size(); ++index) {
                        const QPointF center(plot.left() + plot.width() * index / std::max<std::size_t>(1, values.size() - 1),
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
        painter.setPen(QPen(QColor("#607a91"), 1)); painter.drawRect(rect);
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
                label = QString::number(1.0 - tick * .5, 'g', 3);
            else if (mode_ == ChartMode::Timeline)
                label = QString::number(1.0 - tick * .25, 'g', 3);
            if (!label.isEmpty()) painter.drawText(QRectF(0, rect.top() + rect.height() * tick / 4.0 - 8, 48, 16), Qt::AlignRight, label);
        }
        const QString verticalLabel = mode_ == ChartMode::Curve || mode_ == ChartMode::Spectrum ? yAxisLabel_ :
            mode_ == ChartMode::Heatmap ? frequencyAxisLabel_ : mode_ == ChartMode::Scatter ? QStringLiteral("Q (归一化)") :
            mode_ == ChartMode::Eye ? QStringLiteral("幅度 (归一化)") : QStringLiteral("置信度 (0–1)");
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
                label = QString::number(-1.0 + tick * .5, 'g', 3);
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
            mode_ == ChartMode::Scatter ? QStringLiteral("I (归一化)") :
            mode_ == ChartMode::Eye ? QStringLiteral("时间 (符号周期)") : timeAxisLabel_;
        setProperty("xAxisLabel", horizontalLabel);
        painter.drawText(QRectF(rect.left(), height() - 16, rect.width(), 14), Qt::AlignHCenter | Qt::AlignBottom, horizontalLabel);
        if (software && mode_ == ChartMode::Scatter) {
            painter.save(); painter.setClipRect(rect); painter.setPen(Qt::NoPen); painter.setBrush(QColor("#53c8f1"));
            const qreal side = std::min(rect.width(), rect.height());
            for (const auto& point : points_) {
                const QPointF p(rect.center().x() + point.real() * side * .42,
                                rect.center().y() - point.imag() * side * .42);
                painter.drawEllipse(p, 3.0, 3.0);
            }
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
                    const QPointF p(rect.left() + rect.width() * i / std::max<std::size_t>(1, values.size() - 1),
                        rect.center().y() - std::clamp<double>(values[i], -1.3, 1.3) * rect.height() * .34);
                    if (!i) path.moveTo(p); else path.lineTo(p);
                }
                painter.setPen(QPen(QColor(72, 207, 196, 55 + s % 95), 1)); painter.drawPath(path);
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
                    const QPointF p(rect.left() + rect.width() * i / std::max<std::size_t>(1, values.size() - 1),
                                    rect.bottom() - normalized * rect.height());
                    if (!i) path.moveTo(p); else path.lineTo(p);
                }
                painter.setPen(QPen(colors[s % colors.size()], 1.4)); painter.drawPath(path);
                if (mode_ == ChartMode::Curve && samplePointsVisible_) {
                    painter.setPen(Qt::NoPen); painter.setBrush(colors[s % colors.size()]);
                    for (std::size_t i = 0; i < values.size(); ++i) {
                        const QPointF p(rect.left() + rect.width() * i / std::max<std::size_t>(1, values.size() - 1),
                            rect.top() + (yAxisMaximum_ - values[i]) /
                                std::max(1e-12, yAxisMaximum_ - yAxisMinimum_) * rect.height());
                        painter.drawEllipse(p, 2.2, 2.2);
                    }
                }
            }
            painter.restore();
        }
        if (!status_.isEmpty()) {
            painter.fillRect(QRectF(rect.left() + 6, rect.top() + 5, rect.width() - 12, 20), QColor(7, 17, 28, 190));
            painter.setPen(QColor("#aac1d4")); painter.drawText(QRectF(rect.left() + 11, rect.top() + 5, rect.width() - 20, 20), Qt::AlignVCenter, status_);
        }
        if (dragging_) {
            painter.setPen(QPen(QColor("#f1cf7f"), 1, Qt::DashLine));
            painter.drawRect(QRectF(dragStart_, dragEnd_).normalized());
        }
    }
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
    QString status_;
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
    auto* heading = new QWidget; heading->setProperty("uiRole", "chartHead"); heading->setFixedHeight(30);
    auto* row = new QHBoxLayout(heading); row->setContentsMargins(9, 0, 9, 0);
    auto* name = textLabel(title); name->setStyleSheet("font-weight:600;color:#d5e6f5;"); row->addWidget(name); row->addStretch();
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

QImage spectrumImage(const std::vector<float>& values, QSize size) {
    if (values.size() != static_cast<std::size_t>(size.width()) * static_cast<std::size_t>(size.height()) ||
        size.width() < 2 || size.height() < 2) return {};
    QImage image(size, QImage::Format_Grayscale8);
    if (image.isNull()) return {};
    for (int y = 0; y < size.height(); ++y) {
        auto* line = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const double db = values[static_cast<std::size_t>(y) * size.width() + x];
            line[x] = static_cast<uchar>(std::lround(std::clamp((db + 120.0) / 105.0, 0.0, 1.0) * 255));
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
    )");
    buildPages();
    recognitionTimer_ = new QTimer(this); recognitionTimer_->setInterval(85);
    connect(recognitionTimer_, &QTimer::timeout, this, &NarrowbandWorkspace::updateRecognition);
    chartWheelTimer_ = new QTimer(this); chartWheelTimer_->setSingleShot(true); chartWheelTimer_->setInterval(220);
    connect(chartWheelTimer_, &QTimer::timeout, this, &NarrowbandWorkspace::finishChartWheel);
    refreshFromSession();
}

NarrowbandWorkspace::~NarrowbandWorkspace() { finishChartWheel(); cancelWork(); }

void NarrowbandWorkspace::setCallbacks(std::function<void()> locateSource, std::function<void()> editChannel,
                                       std::function<void()> returnToWide, std::function<void(const QString&)> log) {
    locateSource_ = std::move(locateSource); editChannel_ = std::move(editChannel);
    returnToWide_ = std::move(returnToWide); log_ = std::move(log);
}

QString NarrowbandWorkspace::dataStatusText() const {
    return dataStatus_ ? dataStatus_->text() : QStringLiteral("窄带数据状态未知");
}

int NarrowbandWorkspace::currentPageIndex() const { return pages_ ? pages_->currentIndex() : -1; }

void NarrowbandWorkspace::activatePageForAcceptance(int index) {
    if (index >= 0 && index < static_cast<int>(NarrowbandPage::Demodulation) + 1)
        showPage(static_cast<NarrowbandPage>(index));
}

bool NarrowbandWorkspace::visibleChartsSettled() const {
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

void NarrowbandWorkspace::invalidateVisibleOverlays() {
    for (auto* chart : charts_) if (chart && chart->isVisible()) chart->invalidateVisibleOverlay();
}

void NarrowbandWorkspace::buildPages() {
    auto* root = new QVBoxLayout(this); root->setContentsMargins(5, 4, 5, 5); root->setSpacing(4);
    auto* header = new QWidget; header->setObjectName("narrowbandHeader"); header->setFixedHeight(36);
    auto* headerRow = new QHBoxLayout(header); headerRow->setContentsMargins(4, 0, 4, 0); headerRow->setSpacing(5);
    const std::array<QString, 4> titles{"信号观察", "调制分析", "深度学习识别", "解调工作台"};
    const std::array<QString, 4> ids{"observePage", "modulationPage", "deepLearningPage", "demodulationPage"};
    for (int i = 0; i < 4; ++i) {
        pageButtons_[i] = actionButton(titles[i], ids[i], headerRow); pageButtons_[i]->setCheckable(true);
        connect(pageButtons_[i], &QPushButton::clicked, this, [this, i] { showPage(static_cast<NarrowbandPage>(i)); });
    }
    headerRow->addSpacing(10); caption_ = textLabel("未选择窄带通道", "narrowbandCaption");
    caption_->setStyleSheet("font:11px Consolas;color:#85b4d3;"); headerRow->addWidget(caption_, 1);
    auto* locate = actionButton("⌖ 来源", "narrowbandLocateSource", headerRow);
    auto* edit = actionButton("⚙ 通道参数", "narrowbandEditChannel", headerRow);
    auto* close = actionButton("返回宽带", "narrowbandClose", headerRow);
    connect(locate, &QPushButton::clicked, this, [this] { if (locateSource_) locateSource_(); });
    connect(edit, &QPushButton::clicked, this, [this] { if (editChannel_) editChannel_(); });
    connect(close, &QPushButton::clicked, this, [this] { if (returnToWide_) returnToWide_(); });
    root->addWidget(header);

    auto* chartToolbar = new QWidget; chartToolbar->setObjectName("narrowbandChartToolbar");
    auto* chartToolbarRow = new QHBoxLayout(chartToolbar); chartToolbarRow->setContentsMargins(6, 0, 6, 0); chartToolbarRow->setSpacing(6);
    dataStatus_ = textLabel("等待通道数据", "narrowbandDataStatus"); dataStatus_->setStyleSheet("color:#78cbdf;font:10px Consolas;");
    waveformMode_ = new QComboBox; waveformMode_->setObjectName("narrowbandWaveformMode");
    waveformMode_->addItems({"I / Q", "幅度 (RMS)", "相位", "幅度包络"});
    displayPsdFft_ = new QComboBox; displayPsdFft_->setObjectName("narrowbandPsdFft");
    displayPsdFft_->addItems({"PSD 1024", "PSD 2048", "PSD 4096", "PSD 8192"}); displayPsdFft_->setCurrentIndex(2);
    displayStftFft_ = new QComboBox; displayStftFft_->setObjectName("narrowbandStftFft");
    for (int order = 8; order <= 16; ++order) displayStftFft_->addItem("STFT " + QString::number(1 << order), 1 << order);
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
        if (auto* channel = session_.activeChannel()) channel->psdFftSize = 1024 << index;
        requestDisplay();
    });
    connect(displayStftFft_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (auto* channel = session_.activeChannel()) channel->stftFftSize = displayStftFft_->itemData(index).toInt();
        requestDisplay();
    });
    connect(frequencyMode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (auto* channel = session_.activeChannel()) channel->absoluteFrequencyLabels = index == 1;
        updateFrequencyAxisLabels();
    });
    pages_ = new QStackedWidget; pages_->setObjectName("narrowbandPageStack"); root->addWidget(pages_, 1);
    auto makePage = [this](int index) {
        auto* page = new QWidget; page->setObjectName(QStringLiteral("nbPage%1").arg(index));
        pageWidgets_[static_cast<std::size_t>(index)] = page; pages_->addWidget(page); return page;
    };

    auto* observe = makePage(0); auto* observeLayout = new QGridLayout(observe);
    observeLayout->setContentsMargins(0, 0, 0, 0); observeLayout->setSpacing(5);
    observeLayout->addWidget(plotPanel("I / Q 双线波形", "narrowbandWaveformPanel", ChartMode::Curve, waveform_), 0, 0);
    observeLayout->addWidget(plotPanel("功率谱密度", "narrowbandPsdPanel", ChartMode::Spectrum, psd_), 0, 1);
    observeLayout->addWidget(plotPanel("窄带时频图 · dBFS/Hz", "narrowbandStftPanel", ChartMode::Heatmap, stft_), 1, 0, 1, 2);
    observeLayout->addWidget(plotPanel("星座预览", "narrowbandConstellationPreview", ChartMode::Scatter, constellationSmall_), 2, 0);
    auto* previewInfo = textLabel("合成示例 · 未执行载波恢复、定时同步或解调", "syntheticConstellationNote");
    previewInfo->setStyleSheet("color:#dfbc78;font-size:10px;padding:6px;");
    observeLayout->addWidget(previewInfo, 2, 1);
    observeLayout->setRowStretch(0, 1); observeLayout->setRowStretch(1, 2); observeLayout->setRowStretch(2, 1);
    observeLayout->setColumnStretch(0, 1); observeLayout->setColumnStretch(1, 1);

    auto* modulation = makePage(1); auto* modulationLayout = new QGridLayout(modulation);
    modulationLayout->setContentsMargins(0, 0, 0, 0); modulationLayout->setSpacing(5);
    modulationLayout->addWidget(plotPanel("大星座图", "narrowbandConstellationLarge", ChartMode::Scatter, constellationLarge_), 0, 0);
    modulationLayout->addWidget(plotPanel("眼图 · 同步未执行", "narrowbandEyeLarge", ChartMode::Eye, eyeLarge_), 0, 1);
    modulationLayout->addWidget(plotPanel("联动时频图", "narrowbandModulationStft", ChartMode::Heatmap, modulationStft_), 1, 0, 1, 2);
    auto* modulationControls = new QWidget; auto* modulationRow = new QHBoxLayout(modulationControls);
    modulationRow->setContentsMargins(6, 4, 6, 4);
    auto* modulationType = new QComboBox; modulationType->setObjectName("modulationFormat");
    modulationType->addItems({"未知 / 自动", "BPSK", "QPSK", "8PSK", "16QAM", "64QAM", "2FSK", "4FSK", "MSK", "GMSK", "OFDM"});
    auto* symbolRate = new QDoubleSpinBox; symbolRate->setObjectName("symbolRate"); symbolRate->setRange(1, 20'000'000); symbolRate->setDecimals(0); symbolRate->setValue(250'000); symbolRate->setSuffix(" Sym/s");
    eyeComponent_ = new QComboBox; eyeComponent_->setObjectName("eyeComponent"); eyeComponent_->addItems({"I + Q", "I 分量", "Q 分量"});
    eyePeriods_ = new QComboBox; eyePeriods_->setObjectName("eyePeriods"); eyePeriods_->addItems({"1 周期", "2 周期", "3 周期", "4 周期"});
    eyeTraces_ = new QComboBox; eyeTraces_->setObjectName("eyeTraces"); eyeTraces_->addItems({"32 条", "64 条", "128 条", "256 条"}); eyeTraces_->setCurrentIndex(1);
    auto* sync = new QCheckBox("演示同步选项"); sync->setObjectName("syncOptionsDemo");
    auto* identify = actionButton("运行识别演示", "modulationIdentify", modulationRow); identify->setProperty("uiRole", "primary");
    modulationRow->addWidget(textLabel("调制格式")); modulationRow->addWidget(modulationType); modulationRow->addWidget(textLabel("符号率"));
    modulationRow->addWidget(symbolRate); modulationRow->addWidget(textLabel("眼图")); modulationRow->addWidget(eyeComponent_);
    modulationRow->addWidget(eyePeriods_); modulationRow->addWidget(eyeTraces_); modulationRow->addWidget(sync); modulationRow->addStretch();
    modulationRow->addWidget(identify); modulationLayout->addWidget(modulationControls, 2, 0, 1, 2);
    modulationLayout->setRowStretch(0, 3); modulationLayout->setRowStretch(1, 2);
    connect(modulationType, &QComboBox::currentTextChanged, this, [this](const QString& value) {
        if (analysisStatus_) analysisStatus_->setText("调制格式为人工选择示意：" + value + " · 未做真实判决");
    });
    connect(symbolRate, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (auto* channel = session_.activeChannel()) channel->symbolRate = value;
    });
    auto refreshEyeControls = [this] {
        const auto* channel = session_.activeChannel(); if (!channel) return;
        const auto traces = channel->eyeTraces;
        const auto component = eyeComponent_ ? eyeComponent_->currentIndex() : 0;
        auto lines = syntheticEye(traces, 192, channel ? channel->eyePeriods : 2, 9);
        if (component != 0) {
            const float sign = component == 1 ? 1.f : -1.f;
            for (auto& line : lines) for (auto& point : line) point *= sign;
        }
        if (eyeLarge_) eyeLarge_->setEye(std::move(lines), "合成示例 · 未执行定时同步");
        if (eyeDemod_) eyeDemod_->setEye(syntheticEye(traces, 192, channel ? channel->eyePeriods : 2, 17), "合成示例 · 未执行定时同步");
    };
    connect(eyeComponent_, &QComboBox::currentIndexChanged, this, [this, refreshEyeControls](int index) {
        if (auto* channel = session_.activeChannel()) channel->eyeComponent = index;
        refreshEyeControls();
    });
    connect(eyePeriods_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (auto* channel = session_.activeChannel()) channel->eyePeriods = index + 1;
    });
    connect(eyeTraces_, &QComboBox::currentIndexChanged, this, [this, refreshEyeControls](int index) {
        if (auto* channel = session_.activeChannel()) channel->eyeTraces = 32 << index;
        refreshEyeControls();
    });
    connect(identify, &QPushButton::clicked, this, [this] { showPage(NarrowbandPage::DeepLearning); });

    auto* deep = makePage(2); auto* deepLayout = new QHBoxLayout(deep);
    deepLayout->setContentsMargins(0, 0, 0, 0); deepLayout->setSpacing(5);
    auto* configPanel = new QWidget; configPanel->setObjectName("demoRecognitionConfig"); configPanel->setMinimumWidth(260); configPanel->setMaximumWidth(360);
    auto* configLayout = new QVBoxLayout(configPanel); configLayout->setContentsMargins(10, 8, 10, 8); configLayout->setSpacing(7);
    auto* configTitle = textLabel("深度学习识别 · UI 演示"); configTitle->setStyleSheet("font-size:14px;font-weight:650;"); configLayout->addWidget(configTitle);
    auto* disclaimer = textLabel("合成分数与进度；权重不会加载，未执行真实模型推理。", "recognitionSyntheticDisclosure");
    disclaimer->setStyleSheet("color:#e5bf77;background:#332b1a;padding:7px;font-size:10px;"); configLayout->addWidget(disclaimer);
    auto* form = new QFormLayout; form->setVerticalSpacing(6); form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    model_ = new QComboBox; model_->setObjectName("recognitionModel");
    model_->addItems({"ResNet18 · v1.0", "CLDNN · v1.0", "Transformer · v1.0"});
    fftSize_ = new QComboBox; fftSize_->setObjectName("recognitionFrameLength"); fftSize_->addItems({"1024", "2048", "4096", "8192"}); fftSize_->setCurrentText("2048");
    recognitionNormalization_ = new QComboBox; recognitionNormalization_->setObjectName("recognitionNormalization"); recognitionNormalization_->addItems({"IQ RMS 归一化", "峰值归一化", "不归一化"});
    recognitionOverlap_ = new QComboBox; recognitionOverlap_->setObjectName("recognitionOverlap"); recognitionOverlap_->addItems({"0%", "25%", "50%", "75%", "90%"}); recognitionOverlap_->setCurrentIndex(2);
    recognitionScope_ = new QComboBox; recognitionScope_->setObjectName("recognitionScope"); recognitionScope_->addItems({"当前可见时间窗", "来源标记时段", "通道完整时段"});
    recognitionSampleRate_ = new QDoubleSpinBox; recognitionSampleRate_->setObjectName("recognitionSampleRate"); recognitionSampleRate_->setRange(.1, 1000); recognitionSampleRate_->setDecimals(2); recognitionSampleRate_->setValue(4); recognitionSampleRate_->setSuffix(" MS/s");
    recognitionThreshold_ = new QDoubleSpinBox; recognitionThreshold_->setObjectName("recognitionThreshold"); recognitionThreshold_->setRange(.01, .99); recognitionThreshold_->setSingleStep(.05); recognitionThreshold_->setValue(.5);
    auto* device = new QComboBox; device->setObjectName("recognitionDevice"); device->addItems({"演示无设备", "CPU · 设计选项", "GPU · 设计选项"});
    form->addRow("模型", model_); form->addRow("窗口点数", fftSize_); form->addRow("归一化", recognitionNormalization_); form->addRow("重叠率", recognitionOverlap_);
    form->addRow("分析范围", recognitionScope_); form->addRow("目标采样率", recognitionSampleRate_); form->addRow("类别阈值", recognitionThreshold_); form->addRow("设备", device);
    configLayout->addLayout(form);
    recognitionAllowResample_ = new QCheckBox("采样率不匹配时允许预处理重采样（选项）"); recognitionAllowResample_->setObjectName("recognitionAllowResample"); recognitionAllowResample_->setChecked(true); configLayout->addWidget(recognitionAllowResample_);
    auto* manageModel = actionButton("模型管理 / 选择权重文件…", "recognitionModelManager", configLayout); manageModel->setProperty("uiRole", "outline");
    connect(manageModel, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "选择模型文件名（演示不会加载权重）", {}, "Model files (*.onnx *.pt *.pth *.engine);;All files (*.*)");
        if (!path.isEmpty()) { selectedModelFile_ = QFileInfo(path).fileName(); if (analysisStatus_) analysisStatus_->setText("已记录文件名：" + selectedModelFile_ + " · 未读取文件内容"); }
    });
    auto* runRow = new QWidget; auto* runLayout = new QHBoxLayout(runRow); runLayout->setContentsMargins(0, 0, 0, 0);
    runRecognition_ = actionButton("运行识别演示", "runRecognition", runLayout); runRecognition_->setProperty("uiRole", "primary");
    stopRecognition_ = actionButton("停止", "stopRecognition", runLayout); stopRecognition_->setEnabled(false);
    connect(runRecognition_, &QPushButton::clicked, this, [this] { startDemoRecognition(); });
    connect(stopRecognition_, &QPushButton::clicked, this, [this] { stopDemoRecognition(); });
    configLayout->addWidget(runRow);
    recognitionProgress_ = new QProgressBar; recognitionProgress_->setObjectName("recognitionProgress"); recognitionProgress_->setRange(0, 100); recognitionProgress_->setValue(0); configLayout->addWidget(recognitionProgress_);
    analysisStatus_ = textLabel("待校验 · synthetic=true", "recognitionStatus"); analysisStatus_->setStyleSheet("color:#83c8d8;font-size:10px;"); configLayout->addWidget(analysisStatus_);
    auto* modelInfo = textLabel("模板：ResNet18 / CLDNN / Transformer\n权重哈希：空\n实际推理提供方：无 · 前端合成", "recognitionModelInfo");
    modelInfo->setStyleSheet("color:#94aeca;font:10px Consolas;"); configLayout->addWidget(modelInfo); configLayout->addStretch(); deepLayout->addWidget(configPanel);
    auto* resultSide = new QWidget; auto* resultLayout = new QVBoxLayout(resultSide); resultLayout->setContentsMargins(0, 0, 0, 0); resultLayout->setSpacing(5);
    auto* resultCards = new QWidget; auto* cardRow = new QHBoxLayout(resultCards); cardRow->setContentsMargins(0, 0, 0, 0); cardRow->setSpacing(5);
    for (const auto& [title, value] : std::array<std::pair<QString, QString>, 4>{{{"信号片段", "18"}, {"主类别", "QPSK"}, {"平均置信度", "0.82"}, {"预测时间", "演示"}}}) {
        auto* card = textLabel(title + "\n" + value); card->setObjectName("recognitionStatCard");
        card->setStyleSheet("background:#16253a;border:1px solid #2d4964;padding:9px;color:#c9def0;"); cardRow->addWidget(card);
    }
    resultLayout->addWidget(resultCards);
    recognitionTop5_ = textLabel("Top-5 候选\n等待演示识别结果", "recognitionTop5");
    recognitionTop5_->setStyleSheet("background:#111f32;border:1px solid #2c455e;padding:10px;color:#c2d7e8;"); resultLayout->addWidget(recognitionTop5_);
    resultLayout->addWidget(plotPanel("片段置信度时间轴 · 合成示例", "recognitionTimelinePanel", ChartMode::Timeline, timeline_), 1);
    resultLayout->addWidget(plotPanel("当前通道时频图 · 真实 IQ", "recognitionStftPanel", ChartMode::Heatmap, recognitionStft_), 2);
    auto* exportRow = new QWidget; auto* exportLayout = new QHBoxLayout(exportRow); exportLayout->setContentsMargins(0, 0, 0, 0); exportLayout->addStretch();
    auto* exportJson = actionButton("导出 JSON", "exportRecognitionJson", exportLayout); exportJson->setProperty("uiRole", "outline");
    connect(exportJson, &QPushButton::clicked, this, &NarrowbandWorkspace::exportRecognition); resultLayout->addWidget(exportRow);
    deepLayout->addWidget(resultSide, 1);
    connect(recognitionThreshold_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        updateRecognitionDetail();
        if (analysisStatus_) analysisStatus_->setText(QStringLiteral("阈值 %.2f 只重判未知类别 · 合成分数不会重算").arg(value));
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
    auto* demodCharts = new QGridLayout; demodCharts->setSpacing(5);
    demodCharts->addWidget(plotPanel("解调星座", "demodConstellationPanel", ChartMode::Scatter, constellationDemod_), 0, 0);
    demodCharts->addWidget(plotPanel("眼图", "demodEyePanel", ChartMode::Eye, eyeDemod_), 0, 1);
    demodLayout->addLayout(demodCharts, 2);
    auto* bitHead = new QWidget; auto* bitRow = new QHBoxLayout(bitHead); bitRow->setContentsMargins(8, 2, 8, 2);
    bitRow->addWidget(textLabel("解调位流 · 合成示例 · 未执行真实解调")); auto* selectAllBits = new QCheckBox("全选"); selectAllBits->setObjectName("selectAllBits"); bitRow->addWidget(selectAllBits);
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
    connect(locateBit, &QPushButton::clicked, this, [this] {
        if (analysisStatus_) analysisStatus_->setText("示意定位：合成位流没有实际源符号时间映射");
        if (log_) log_("位流示意回定位；无实际解调样本可定位");
    });

    // All time-domain gestures update one channel view; PSD and STFT frequency
    // gestures update the same baseband window used by every page.
    auto bindGesture = [this](NarrowbandChart* chart, ChartMode mode) {
        chart->gesture = [this, chart, mode](QPointF a, QPointF b, int wheel, bool selection) {
            auto* channel = session_.activeChannel();
            if (!channel) return;
            auto time = channel->visibleSourceTime; auto frequency = channel->visibleBasebandFrequency;
            const QRectF plot = chart->plotRect();
            enum class Zone { Plot, XAxis, YAxis, Outside };
            const auto zoneAt = [chart, plot, mode](QPointF point) {
                const QPointF pixel(point.x() * chart->width(), point.y() * chart->height());
                if (mode == ChartMode::Navigation) return Zone::Plot;
                if (plot.contains(pixel)) return Zone::Plot;
                if (pixel.x() >= 0 && pixel.x() < plot.left() && pixel.y() >= plot.top() && pixel.y() <= plot.bottom()) return Zone::YAxis;
                if (pixel.y() > plot.bottom() && pixel.y() <= chart->height() && pixel.x() >= plot.left() && pixel.x() <= plot.right()) return Zone::XAxis;
                return Zone::Outside;
            };
            const Zone zone = zoneAt(a);
            if (zone == Zone::Outside) return;
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
                const auto minimum = std::max<std::uint64_t>(1, rateHz(channel->outputSampleRateHz) / 65536);
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
                const double nextSpan = std::clamp(span * factor, channel->outputSampleRateHz / 4096.0, channel->outputSampleRateHz);
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
                    waveform_->setYAxisRange(range.first, range.last, waveformAxisLabel(channel->waveform));
                } else if (selection) {
                    commitPointerGesture();
                    const auto range = chart_interaction::panByFraction(
                        {channel->waveformAxisMinimum, channel->waveformAxisMaximum}, dragYFraction);
                    session_.setChannelAmplitudeRange(range.first, range.last, false, true);
                    waveform_->setYAxisRange(channel->waveformAxisMinimum, channel->waveformAxisMaximum,
                                             waveformAxisLabel(channel->waveform));
                }
                return;
            }
            if (mode == ChartMode::Spectrum && zone == Zone::YAxis) {
                if (wheel) {
                    const double span = channel->psdAxisMaximum - channel->psdAxisMinimum;
                    const double nextSpan = std::clamp(span * std::pow(1.2, -wheel / 120.0), .01, 1000.0);
                    const auto range = chart_interaction::zoomAround(
                        {channel->psdAxisMinimum, channel->psdAxisMaximum}, nextSpan / span, 1.0 - y0);
                    beginWheel(); session_.setChannelPsdRange(range.first, range.last, false);
                    psd_->setYAxisRange(range.first, range.last, QStringLiteral("dBFS/Hz"));
                } else if (selection) {
                    commitPointerGesture();
                    const auto range = chart_interaction::panByFraction(
                        {channel->psdAxisMinimum, channel->psdAxisMaximum}, dragYFraction);
                    session_.setChannelPsdRange(range.first, range.last, true);
                    psd_->setYAxisRange(channel->psdAxisMinimum, channel->psdAxisMaximum, QStringLiteral("dBFS/Hz"));
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
                        channel->outputSampleRateHz / 4096.0, channel->outputSampleRateHz);
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
                const double full = channel->outputSampleRateHz;
                frequency = {full / 2 - yBottom * full, full / 2 - yTop * full};
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
    charts_ = {waveform_, psd_, stft_, modulationStft_, recognitionStft_,
               constellationSmall_, constellationLarge_, constellationDemod_, eyeLarge_, eyeDemod_, timeline_};
    for (auto* chart : charts_) if (chart) {
        chart->cancelPendingGesture = [this] { cancelChartWheel(); };
        chart->navigateHistory = [this](bool forward) {
            finishChartWheel();
            if (forward ? session_.channelForward() : session_.channelBack()) refreshFromSession();
        };
    }
    const auto applyGrid = [this](bool visible) {
        for (auto* chart : charts_) if (chart) chart->setGridVisible(visible);
        QSettings().setValue(QStringLiteral("narrowband/gridVisible"), visible);
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
        psd_->setYAxisRange(channel->psdAxisMinimum, channel->psdAxisMaximum, QStringLiteral("dBFS/Hz"));
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
        cancelWork(); sampleCache_.clear(); caption_->setText("未选择窄带通道"); dataStatus_->setText("等待通道数据");
        if (waveform_) waveform_->setSeries({}, "当前没有活动通道");
        return;
    }
    for (auto* chart : charts_) if (chart) chart->setPalette(file->display.palette);
    if (!recognitionSegments_.empty() && recognitionChannelId_ == channel->id) {
        if (recognitionConfigVersion_ != channel->configVersion)
            markRecognitionStale("通道配置已变化");
        else if (recognitionScope_ && recognitionScope_->currentIndex() == 0 &&
                 (recognitionRangeBegin_ != channel->visibleSourceTime.begin || recognitionRangeEnd_ != channel->visibleSourceTime.end))
            markRecognitionStale("分析时间范围已变化");
    } else if (!recognitionSegments_.empty() && recognitionChannelId_ != channel->id) {
        markRecognitionStale("已切换活动通道");
    }
    updateCaption();
    const auto index = static_cast<int>(channel->page);
    if (pages_->currentIndex() != index) pages_->setCurrentIndex(index);
    for (int i = 0; i < 4; ++i) pageButtons_[i]->setChecked(i == index);
    { QSignalBlocker blocker(waveformMode_); waveformMode_->setCurrentIndex(static_cast<int>(channel->waveform)); }
    const int psdIndex = channel->psdFftSize <= 1024 ? 0 : channel->psdFftSize <= 2048 ? 1 : channel->psdFftSize <= 4096 ? 2 : 3;
    { QSignalBlocker blocker(displayPsdFft_); displayPsdFft_->setCurrentIndex(psdIndex); }
    const auto stftIndex = displayStftFft_->findData(channel->stftFftSize);
    { QSignalBlocker blocker(displayStftFft_); displayStftFft_->setCurrentIndex(stftIndex < 0 ? 5 : stftIndex); }
    { QSignalBlocker blocker(frequencyMode_); frequencyMode_->setCurrentIndex(channel->absoluteFrequencyLabels ? 1 : 0); }
    { QSignalBlocker blocker(eyePeriods_); eyePeriods_->setCurrentIndex(std::clamp(channel->eyePeriods - 1, 0, 3)); }
    const auto traceIndex = channel->eyeTraces <= 32 ? 0 : channel->eyeTraces <= 64 ? 1 : channel->eyeTraces <= 128 ? 2 : 3;
    { QSignalBlocker blocker(eyeTraces_); eyeTraces_->setCurrentIndex(traceIndex); }
    { QSignalBlocker blocker(eyeComponent_); eyeComponent_->setCurrentIndex(channel->eyeComponent); }
    updateFrequencyAxisLabels();
    if (waveform_)
        waveform_->setYAxisRange(channel->waveformAxisMinimum, channel->waveformAxisMaximum,
                                 waveformAxisLabel(channel->waveform));
    if (psd_) psd_->setYAxisRange(channel->psdAxisMinimum, channel->psdAxisMaximum, QStringLiteral("dBFS/Hz"));
    if (navigation_) {
        const auto total = std::max<std::uint64_t>(1, channel->sourceTime.end - channel->sourceTime.begin);
        navigation_->setNavigationViewport(
            static_cast<double>(channel->visibleSourceTime.begin - channel->sourceTime.begin) / total,
            static_cast<double>(channel->visibleSourceTime.end - channel->sourceTime.begin) / total);
        navigation_->setSeries({}, QStringLiteral("%1 – %2 · 源时间窗位置").arg(channel->sourceTime.begin).arg(channel->sourceTime.end));
    }
    if (constellationSmall_) constellationSmall_->setPoints(syntheticConstellation(), "合成示例 · 未执行符号同步");
    if (constellationLarge_) constellationLarge_->setPoints(syntheticConstellation(81), "合成示例 · 未执行符号同步");
    if (constellationDemod_) constellationDemod_->setPoints(syntheticConstellation(103), "合成示例 · 未执行载波恢复");
    if (eyeSmall_) eyeSmall_->setEye(syntheticEye(channel->eyeTraces, 96, channel->eyePeriods), "合成示例 · 未执行定时同步");
    auto eyeLines = syntheticEye(channel->eyeTraces, 192, channel->eyePeriods);
    if (channel->eyeComponent != 0) {
        const float sign = channel->eyeComponent == 1 ? 1.f : -1.f;
        for (auto& line : eyeLines) for (auto& point : line) point *= sign;
    }
    if (eyeLarge_) eyeLarge_->setEye(std::move(eyeLines), "合成示例 · 未执行定时同步");
    if (eyeDemod_) eyeDemod_->setEye(syntheticEye(channel->eyeTraces, 192, channel->eyePeriods, 17), "合成示例 · 未执行定时同步");
    requestDisplay();
}

void NarrowbandWorkspace::showPage(NarrowbandPage page) {
    if (auto* channel = session_.activeChannel()) channel->page = page;
    pages_->setCurrentIndex(static_cast<int>(page));
    for (int i = 0; i < 4; ++i) pageButtons_[i]->setChecked(i == static_cast<int>(page));
    requestDisplay();
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
                           !channel->preserveSourceTime || channel->relativeTime);
        if (chart == eyeLarge_ || chart == eyeDemod_ || chart == eyeSmall_)
            chart->setSymbolPeriod(channel->eyePeriods);
    }
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

void NarrowbandWorkspace::requestDisplay() {
    const auto* active = session_.activeChannel(); const auto* file = session_.activeFile();
    if (!active || !file || !waveform_ || !psd_ || !stft_) return;
    updateFrequencyAxisLabels();
    const bool unchanged = hasDisplayRequest_ && lastChannelId_ == active->id && lastSourcePath_ == file->metadata.path &&
        lastConfigVersion_ == active->configVersion && lastVisibleTime_ == active->visibleSourceTime &&
        lastVisibleFrequency_ == active->visibleBasebandFrequency && lastWaveform_ == active->waveform &&
        lastPsdFft_ == active->psdFftSize && lastStftFft_ == active->stftFftSize;
    if (unchanged) return;
    const bool invalidateChannelCache = hasDisplayRequest_ && lastChannelId_ == active->id &&
        lastConfigVersion_ != active->configVersion;
    cancelWork();
    if (invalidateChannelCache) sampleCache_.clear();
    const auto channel = *active; const auto source = file->metadata;
    lastChannelId_ = channel.id; lastSourcePath_ = source.path; lastConfigVersion_ = channel.configVersion;
    lastVisibleTime_ = channel.visibleSourceTime; lastVisibleFrequency_ = channel.visibleBasebandFrequency;
    lastWaveform_ = channel.waveform; lastPsdFft_ = channel.psdFftSize; lastStftFft_ = channel.stftFftSize;
    hasDisplayRequest_ = true;
    const auto generation = ++requestGeneration_; const auto config = channel.configVersion;
    QString error; ChannelDspPlan plan;
    if (channel.processingState != ChannelProcessingState::Ready || source.demo || source.path.empty()) {
        const auto status = channel.processingState == ChannelProcessingState::LegacyNeedsReview ?
            QStringLiteral("旧工程通道待确认参数") : source.demo ? QStringLiteral("演示源没有 IQ 样本 · 真实 DDC 未运行") :
            QStringLiteral("源 IQ 文件不可用 · 真实 DDC 未运行");
        waveform_->setSeries({}, status); psd_->setSeries({}, status); stft_->setRaster({}, status);
        if (modulationStft_) modulationStft_->setRaster({}, status); if (recognitionStft_) recognitionStft_->setRaster({}, status);
        dataStatus_->setText(status);
        analysisStatus_->setText(status); return;
    }
    if (!makeChannelDspPlan(source, channel, plan, error)) {
        waveform_->setSeries({}, error); psd_->setSeries({}, error); stft_->setRaster({}, error);
        if (modulationStft_) modulationStft_->setRaster({}, error); if (recognitionStft_) recognitionStft_->setRaster({}, error);
        dataStatus_->setText("参数校验失败"); analysisStatus_->setText(error); return;
    }
    const auto sourceRate = rateHz(source.sampleRateHz), outputRate = rateHz(channel.outputSampleRateHz);
    std::uint64_t firstOutput = 0, lastOutput = 0;
    if (!sourceRate || !outputRate || channel.visibleSourceTime.begin < channel.sourceTime.begin ||
        !mulDiv(channel.visibleSourceTime.begin - channel.sourceTime.begin, outputRate, sourceRate, false, firstOutput) ||
        !mulDiv(channel.visibleSourceTime.end - channel.sourceTime.begin, outputRate, sourceRate, true, lastOutput)) {
        waveform_->setSeries({}, "输出采样映射超出支持范围"); dataStatus_->setText("映射无效"); return;
    }
    std::uint64_t outputTotal = 0;
    if (!mulDiv(channel.sourceTime.end - channel.sourceTime.begin, outputRate, sourceRate, true, outputTotal)) {
        waveform_->setSeries({}, "通道输出样本数超出 uint64 范围"); dataStatus_->setText("输出长度无效"); return;
    }
    lastOutput = std::min(lastOutput, outputTotal);
    if (lastOutput <= firstOutput) lastOutput = std::min(outputTotal, firstOutput + static_cast<std::uint64_t>(channel.stftFftSize));
    constexpr std::uint64_t maximumDisplaySamples = 1'500'000;
    if (lastOutput - firstOutput > maximumDisplaySamples) {
        lastOutput = firstOutput + maximumDisplaySamples;
        if (lastOutput > outputTotal) { lastOutput = outputTotal; firstOutput = lastOutput > maximumDisplaySamples ? lastOutput - maximumDisplaySamples : 0; }
    }
    if (lastOutput <= firstOutput) {
        const auto message = QStringLiteral("当前可见范围没有输出样本");
        waveform_->setSeries({}, message); psd_->setSeries({}, message); stft_->setRaster({}, message);
        dataStatus_->setText("等待有效时间窗"); return;
    }
    const auto outputSpan = lastOutput - firstOutput;
    const auto physicalPlotWidth = static_cast<std::uint64_t>(std::max(1.0,
        std::floor(waveform_->plotRect().width() * waveform_->devicePixelRatioF())));
    const bool samplePointsVisible = outputSpan <= physicalPlotWidth;
    const int waveformPoints = static_cast<int>(samplePointsVisible ? outputSpan :
        std::clamp(std::llround(waveform_->plotRect().width() * waveform_->devicePixelRatioF() * 2.0), 1400LL, 65536LL));
    waveform_->setSeries({}, "正在按需读取并处理当前可见 IQ…");
    psd_->setSeries({}, "正在计算当前可见窗的 PSD…"); stft_->setRaster({}, "正在计算真实 STFT…");
    dataStatus_->setText(QStringLiteral("处理中 · 配置 v%1 · 请求 %2").arg(config).arg(generation));
    auto cancel = std::make_shared<std::atomic_bool>(false); cancellation_ = cancel;
    worker_ = std::thread([this, channel, source, plan = std::move(plan), firstOutput, lastOutput, waveformPoints,
                           samplePointsVisible, generation, config, cancel]() mutable {
        ChannelSampleData data; const bool ok = sampleCache_.process(source, channel, plan, {firstOutput, lastOutput}, data,
            [cancel] { return cancel->load(std::memory_order_relaxed); });
        std::vector<float> waveI, waveQ, psd;
        QImage stft;
        QString status;
        bool psdReady = false, stftReady = false;
        if (ok && !cancel->load(std::memory_order_relaxed)) {
            const bool waveformReady = channel.waveform == NarrowbandWaveform::IQ ?
                channelWaveformIQ(data, waveformPoints, waveI, waveQ) :
                channelWaveform(data, waveformPoints, channel.waveform, waveI);
            psdReady = waveformReady && channelPsd(data, channel.outputSampleRateHz,
                channel.visibleBasebandFrequency, channel.psdFftSize, 1200, psd,
                [cancel] { return cancel->load(); });
            std::vector<float> matrix;
            stftReady = waveformReady && channelSpectrogram(data, channel.outputSampleRateHz,
                channel.visibleBasebandFrequency, QSize(560, 240), channel.stftFftSize, matrix,
                [cancel] { return cancel->load(); });
            if (stftReady) {
                stft = spectrumImage(matrix, QSize(560, 240));
                stftReady = !stft.isNull();
            }
            status = waveformReady ? QStringLiteral("真实 DDC + FIR + 有理重采样波形 · ADC 计数") :
                QStringLiteral("当前范围不足以生成波形数据");
            if (!psdReady) status += QStringLiteral(" · PSD 范围短于 FFT 或数据不足");
            if (!stftReady) status += QStringLiteral(" · STFT 范围短于 FFT 或数据不足");
        } else status = cancel->load() ? QStringLiteral("计算已取消") : QStringLiteral("IQ 读取或窄带计算失败");
        QMetaObject::invokeMethod(this, [this, generation, config, waveI = std::move(waveI), waveQ = std::move(waveQ),
                                         psd = std::move(psd), stft = std::move(stft), status = std::move(status),
                                         samplePointsVisible, psdReady, stftReady, firstOutput]() mutable {
            installFrame(generation, config, std::move(waveI), std::move(waveQ), std::move(psd),
                         std::move(stft), std::move(status), samplePointsVisible,
                         psdReady, stftReady, firstOutput);
        }, Qt::QueuedConnection);
    });
}

void NarrowbandWorkspace::installFrame(std::uint64_t generation, std::uint64_t configVersion,
                                       std::vector<float> waveformI, std::vector<float> waveformQ,
                                       std::vector<float> psd, QImage stft, QString status,
                                       bool samplePointsVisible, bool psdReady, bool stftReady,
                                       std::uint64_t firstOutputSample) {
    auto* channel = session_.activeChannel();
    if (!channel || generation != requestGeneration_ || channel->configVersion != configVersion) return;
    frameGeneration_ = generation;
    if (channel->waveformAutoScale && !waveformI.empty()) {
        double minimum = std::numeric_limits<double>::infinity();
        double maximum = -std::numeric_limits<double>::infinity();
        const auto include = [&](const std::vector<float>& values) {
            for (const float value : values) {
                if (!std::isfinite(value)) continue;
                minimum = std::min(minimum, static_cast<double>(value));
                maximum = std::max(maximum, static_cast<double>(value));
            }
        };
        include(waveformI);
        include(waveformQ);
        if (std::isfinite(minimum) && std::isfinite(maximum)) {
            double span = maximum - minimum;
            const double minimumSpan = channel->waveform == NarrowbandWaveform::Phase ? .01 : 1.0;
            if (span < minimumSpan) {
                const double center = (minimum + maximum) / 2.0;
                span = std::max(minimumSpan, std::abs(center) * 1e-6);
                minimum = center - span / 2.0;
                maximum = center + span / 2.0;
            }
            const double padding = span / 6.0;
            session_.setChannelAmplitudeRange(minimum - padding, maximum + padding, true, false);
        }
    }
    if (waveform_)
        waveform_->setYAxisRange(channel->waveformAxisMinimum, channel->waveformAxisMaximum,
                                 waveformAxisLabel(channel->waveform));
    if (waveform_) {
        waveform_->setSamplePointOrigin(firstOutputSample);
        waveform_->setSamplePointsVisible(samplePointsVisible && !waveformI.empty());
    }
    if (psd_) psd_->setYAxisRange(channel->psdAxisMinimum, channel->psdAxisMaximum, QStringLiteral("dBFS/Hz"));
    if (!waveformI.empty()) {
        if (waveformQ.empty()) waveform_->setSeries({std::move(waveformI)}, status);
        else waveform_->setSeries({std::move(waveformI), std::move(waveformQ)}, status);
    } else waveform_->setSeries({}, status);
    if (psdReady && !psd.empty()) psd_->setSeries({std::move(psd)}, status);
    else psd_->setSeries({}, QStringLiteral("当前可见范围短于 PSD FFT 点数；未自动更换 FFT"));
    const auto revision = QStringLiteral("nb-%1-%2").arg(configVersion).arg(generation);
    if (stftReady && !stft.isNull()) {
        stft_->setRaster(stft, status, revision);
        if (modulationStft_) modulationStft_->setRaster(stft, status, revision);
        if (recognitionStft_) recognitionStft_->setRaster(stft, status, revision);
    } else {
        const QString unavailable = QStringLiteral("当前可见范围短于 STFT FFT 点数；未自动更换 FFT");
        stft_->setRaster({}, unavailable);
        if (modulationStft_) modulationStft_->setRaster({}, unavailable);
        if (recognitionStft_) recognitionStft_->setRaster({}, unavailable);
    }
    dataStatus_->setText(status);
    if (analysisStatus_) analysisStatus_->setText(status);
}

void NarrowbandWorkspace::cancelWork() {
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
    ++requestGeneration_;
    if (worker_.joinable()) worker_.join();
    cancellation_.reset();
}

void NarrowbandWorkspace::updateCaption() {
    const auto* channel = session_.activeChannel(); const auto* file = session_.activeFile();
    if (!channel || !file) return;
    const double center = channel->centerFrequencyHz / 1e6, bandwidth = channel->bandwidthHz / 1e6;
    caption_->setText(QStringLiteral("%1  ·  %2 / %3  ·  Fc %4 MHz  BW %5 MHz  Fs′ %6 MS/s")
        .arg(str(channel->name), str(file->metadata.name), str(channel->sourceMarkId))
        .arg(center, 0, 'f', 3).arg(bandwidth, 0, 'f', 3).arg(channel->outputSampleRateHz / 1e6, 0, 'f', 3));
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
    analysisStatus_->setText("校验通过 · 合成识别开始 · 权重不会加载或执行");
    runRecognition_->setEnabled(false); stopRecognition_->setEnabled(true); recognitionProgress_->setValue(0); recognitionTimer_->start();
    if (!selectedModelFile_.isEmpty() && log_) log_("识别演示使用模型文件名作为界面记录；未读取或执行权重");
}

void NarrowbandWorkspace::stopDemoRecognition() {
    stopRecognition();
    if (analysisStatus_) analysisStatus_->setText("识别演示已停止");
}

void NarrowbandWorkspace::stopRecognition() {
    if (recognitionTimer_) recognitionTimer_->stop();
    recognitionActive_ = false;
    if (runRecognition_) runRecognition_->setEnabled(session_.activeChannel() != nullptr);
    if (stopRecognition_) stopRecognition_->setEnabled(false);
}

void NarrowbandWorkspace::updateRecognition() {
    if (!recognitionActive_) return;
    recognitionProgressValue_ = std::min(100, recognitionProgressValue_ + 6 + static_cast<int>(recognitionGeneration_ % 4));
    recognitionProgress_->setValue(recognitionProgressValue_);
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
        if (analysisStatus_) analysisStatus_->setText(QStringLiteral("完成 · %1 个确定性合成分段 · 预测非真实模型输出").arg(count));
        if (timeline_) {
            std::vector<float> scores; scores.reserve(recognitionSegments_.size());
            for (const auto& segment : recognitionSegments_) scores.push_back(static_cast<float>(segment.scores[static_cast<std::size_t>(segment.topClass)]));
            timeline_->setSeries({std::move(scores)}, QStringLiteral("synthetic=true · %1 segments · demo classifier").arg(count));
        }
        selectedRecognitionSegment_ = 0; updateRecognitionDetail();
        if (log_) log_("合成识别任务完成；18 段 Top-5 仅用于界面演示");
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
    if (!recognitionTop5_ || recognitionSegments_.empty()) return;
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
    QString detail = QStringLiteral("片段 %1 / %2 · %3%4\n源样本 [%5, %6)\nTop-5 原始合成分数：")
        .arg(index + 1).arg(recognitionSegments_.size())
        .arg(predicted).arg(stale ? QStringLiteral(" · Stale") : QString{})
        .arg(QString::number(segment.sourceBegin)).arg(QString::number(segment.sourceEnd));
    for (int rank = 0; rank < 5; ++rank) {
        const int category = order[static_cast<std::size_t>(rank)];
        detail += QStringLiteral("\n%1. %2  %3%")
            .arg(rank + 1).arg(QString::fromLatin1(labels[category]))
            .arg(segment.scores[static_cast<std::size_t>(category)] * 100, 0, 'f', 1);
    }
    recognitionTop5_->setText(detail);
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
    if (log_) log_("已导出 synthetic=true 的演示识别结果");
}

void NarrowbandWorkspace::copyBitstream() { if (bitstream_) QApplication::clipboard()->setText(bitstream_->toPlainText()); }
void NarrowbandWorkspace::exportBitstream() {
    if (!bitstream_) return;
    const auto path = QFileDialog::getSaveFileName(this, "导出演示位流", "narrowband-demo-bits.txt", "Text (*.txt)");
    if (path.isEmpty()) return;
    QFile file(path); if (file.open(QIODevice::WriteOnly | QIODevice::Text)) file.write(bitstream_->toPlainText().toUtf8());
}

} // namespace signalstudio
