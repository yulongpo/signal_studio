#pragma once

#include <QImage>
#include <QJsonObject>
#include <QRhiWidget>
#include <QRectF>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class QPainter;
class QPaintEvent;
class QRhiCommandBuffer;
class QRhiResourceUpdateBatch;

namespace signalstudio {

// The parent owns coordinates and gestures. This child uploads scalar heatmaps,
// palette textures and chart geometry, then composites a QPainter overlay.
class AcceleratedSurface : public QRhiWidget {
    Q_OBJECT
public:
    using PainterCallback = std::function<void(QPainter&)>;
    // Coordinates are widget-relative fractions: (0,0) is the top left and
    // (1,1) the bottom right. Convert to backend NDC only at GPU upload time.
    struct ChartVertex { float x = 0, y = 0, r = 1, g = 1, b = 1, a = 1; };
    struct ChartDrawCall {
        enum class Topology { LineStrip, Triangles, TriangleStrip } topology = Topology::LineStrip;
        quint32 firstVertex = 0;
        quint32 vertexCount = 0;
    };
    struct GpuChartVertex { float x = 0, y = 0, u = 0, v = 0; };

    explicit AcceleratedSurface(QWidget* parent = nullptr);
    ~AcceleratedSurface() override;

    void setPainter(PainterCallback painter);
    void setInversePainter(PainterCallback painter);
    void setChartGeometry(std::vector<ChartVertex> vertices, std::vector<ChartDrawCall> draws,
                          const QString& revision, const QRectF& clipRect = {});
    void setHeatmap(const QImage& image, const QRectF& target, const QString& revision,
                    const QImage& palette = {});
    void setHeatmapSourceRect(const QRectF& normalizedSource);
    void invalidateOverlay();
    bool isReady() const;
    QString backendDescription() const;
    quint64 textureUploadCount() const { return textureUploads_; }
    quint64 completedFrameCount() const { return completedFrames_; }
    quint64 overlayUploadCount() const { return overlayUploads_; }
    quint64 inverseOverlayDrawCallCount() const { return inverseOverlayDrawCalls_; }
    bool hasPendingUploads() const { return heatmapDirty_ || overlayDirty_ || chartGeometryDirty_ || paletteDirty_ || chartPaletteDirty_; }
    quint64 chartVertexUploadCount() const { return chartVertexUploads_; }
    quint64 chartDrawCallCount() const { return chartDrawCalls_; }
    quint64 heatmapDrawCallCount() const { return heatmapDrawCalls_; }
    QRectF heatmapTargetRect() const { return target_; }
    quint64 paletteUploadCount() const { return paletteUploads_; }
    quint64 chartPaletteUploadCount() const { return chartPaletteUploads_; }
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
    bool ensureChartBuffer();
    bool ensureChartPaletteTexture();
    enum class TextureLayer { Heatmap, Overlay, InverseOverlay };
    bool ensureTexture(TextureLayer layer, const QSize& pixelSize);
    bool ensurePaletteTexture();
    bool prepareUploads(QRhiResourceUpdateBatch* updates);
    void fail(const QString& reason);

    PainterCallback painter_;
    PainterCallback inversePainter_;
    QImage heatmap_;
    QImage overlay_;
    QImage inverseOverlay_;
    QImage palette_;
    QImage chartPalette_;
    QRectF target_;
    QRectF chartClipRect_;
    QRectF sourceRect_{0, 0, 1, 1};
    QString revision_;
    QString backend_ = QStringLiteral("QRhi 初始化中");
    bool hasRevision_ = false;
    bool heatmapDirty_ = false;
    bool heatmapUploaded_ = false;
    bool overlayDirty_ = true;
    bool chartGeometryDirty_ = true;
    bool paletteDirty_ = true;
    bool chartPaletteDirty_ = true;
    bool ready_ = false;
    bool failureReported_ = false;
    bool supportedApi_ = true;
    qreal overlayDpr_ = 0;
    quint64 textureUploads_ = 0;
    quint64 completedFrames_ = 0;
    quint64 overlayUploads_ = 0;
    quint64 inverseOverlayDrawCalls_ = 0;
    quint64 chartVertexUploads_ = 0;
    quint64 chartDrawCalls_ = 0;
    quint64 heatmapDrawCalls_ = 0;
    quint64 paletteUploads_ = 0;
    quint64 chartPaletteUploads_ = 0;
    quint64 renderInvocations_ = 0;
    QSize lastRenderSize_;
    quint64 chartDrawCallsSkipped_ = 0;
    quint64 lastChartDrawLoopIterations_ = 0;
    quint32 lastChartDrawStage_ = 0;
    qsizetype lastChartVertexCount_ = 0;
    qsizetype lastChartDrawRecordCount_ = 0;
    QString chartGeometryRevision_;
    std::vector<GpuChartVertex> chartVertices_;
    std::vector<ChartDrawCall> chartDraws_;
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
