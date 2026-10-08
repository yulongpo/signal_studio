#pragma once

#include <QImage>
#include <QJsonObject>
#include <QRhiWidget>
#include <QRectF>
#include <QString>

#include <functional>
#include <memory>

class QPainter;
class QPaintEvent;
class QRhiCommandBuffer;
class QRhiResourceUpdateBatch;

namespace signalstudio {

// The parent owns coordinates and gestures. This child uploads cached heatmap
// and raster overlays, then composites them through one QRhi graphics pipeline.
class AcceleratedSurface : public QRhiWidget {
    Q_OBJECT
public:
    using PainterCallback = std::function<void(QPainter&)>;

    explicit AcceleratedSurface(QWidget* parent = nullptr);
    ~AcceleratedSurface() override;

    void setPainter(PainterCallback painter);
    void setHeatmap(const QImage& image, const QRectF& target, const QString& revision);
    void setHeatmapSourceRect(const QRectF& normalizedSource);
    void invalidateOverlay();
    bool isReady() const;
    QString backendDescription() const;
    quint64 textureUploadCount() const { return textureUploads_; }
    quint64 completedFrameCount() const { return completedFrames_; }
    quint64 overlayUploadCount() const { return overlayUploads_; }
    bool hasPendingUploads() const { return heatmapDirty_ || overlayDirty_; }
    QJsonObject cpuWallTimings() const;

signals:
    void backendReady(QString description);
    void backendFailed(QString reason);

protected:
    void paintEvent(QPaintEvent* event) override;
    void initialize(QRhiCommandBuffer* commandBuffer) override;
    void render(QRhiCommandBuffer* commandBuffer) override;
    void releaseResources() override;

private:
    struct CpuWallStage {
        double lastMs = 0;
        double maxMs = 0;
        double totalMs = 0;
        quint64 samples = 0;
        void record(double elapsedMs) {
            lastMs = elapsedMs;
            if (elapsedMs > maxMs) maxMs = elapsedMs;
            totalMs += elapsedMs;
            ++samples;
        }
    };
    struct Resources;
    bool createResources();
    bool ensurePipeline();
    bool ensureTexture(bool overlay, const QSize& pixelSize);
    bool prepareUploads(QRhiResourceUpdateBatch* updates);
    void fail(const QString& reason);

    PainterCallback painter_;
    QImage heatmap_;
    QImage overlay_;
    QRectF target_;
    QRectF sourceRect_{0, 0, 1, 1};
    QString revision_;
    QString backend_ = QStringLiteral("QRhi 初始化中");
    bool hasRevision_ = false;
    bool heatmapDirty_ = false;
    bool heatmapUploaded_ = false;
    bool overlayDirty_ = true;
    bool ready_ = false;
    bool failureReported_ = false;
    bool supportedApi_ = true;
    qreal overlayDpr_ = 0;
    quint64 textureUploads_ = 0;
    quint64 completedFrames_ = 0;
    quint64 overlayUploads_ = 0;
    CpuWallStage prepareUploadsTiming_;
    CpuWallStage overlayImagePreparationTiming_;
    CpuWallStage overlayPainterTiming_;
    CpuWallStage overlayUploadEnqueueTiming_;
    CpuWallStage heatmapUploadPreparationTiming_;
    CpuWallStage renderTiming_;
    CpuWallStage paintEventTiming_;
    std::unique_ptr<Resources> resources_;
};

} // namespace signalstudio
