#include "ui/charts/accelerated_surface.h"

#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QTimer>
#include <QVariant>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <utility>

namespace signalstudio {
namespace {

template<class Stage>
class CpuWallScope {
public:
    explicit CpuWallScope(Stage& stage) : stage_(stage) { timer_.start(); }
    ~CpuWallScope() { stage_.record(timer_.nsecsElapsed() / 1e6); }
private:
    Stage& stage_;
    QElapsedTimer timer_;
};

bool isSoftwareRenderer(const QString& name) {
    constexpr std::array names{"llvmpipe", "softpipe", "swiftshader", "software",
                               "gdi generic", "microsoft basic render", "warp", "lavapipe", "swrast"};
    for (const auto* marker : names)
        if (name.contains(QLatin1String(marker), Qt::CaseInsensitive)) return true;
    return false;
}

QShader loadShader(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}

struct Vertex {
    float x, y, u, v;
};

std::array<Vertex, 4> makeQuad(const QRectF& rect, QSizeF canvas, bool yUp, const QRectF& uv = QRectF(0, 0, 1, 1)) {
    const float left = static_cast<float>(2.0 * rect.left() / canvas.width() - 1.0);
    const float right = static_cast<float>(2.0 * rect.right() / canvas.width() - 1.0);
    const auto y = [canvas, yUp](qreal position) {
        const qreal normalized = 1.0 - 2.0 * position / canvas.height();
        return static_cast<float>(yUp ? normalized : -normalized);
    };
    const float top = y(rect.top());
    const float bottom = y(rect.bottom());
    // Uploads preserve QImage scanline order: its top row is sampled at v=0.
    return {{{left, top, float(uv.left()), float(uv.top())}, {right, top, float(uv.right()), float(uv.top())},
             {left, bottom, float(uv.left()), float(uv.bottom())}, {right, bottom, float(uv.right()), float(uv.bottom())}}};
}

} // namespace

struct AcceleratedSurface::Resources {
    QRhi* owner = nullptr;
    QShader vertexShader;
    QShader fragmentShader;
    std::unique_ptr<QRhiBuffer> vertices;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiTexture> heatmap;
    std::unique_ptr<QRhiTexture> overlay;
    std::unique_ptr<QRhiShaderResourceBindings> heatmapBindings;
    std::unique_ptr<QRhiShaderResourceBindings> overlayBindings;
    std::unique_ptr<QRhiRenderPassDescriptor> renderPass;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    int samples = 0;

    ~Resources() {
        pipeline.reset();
        heatmapBindings.reset();
        overlayBindings.reset();
        heatmap.reset();
        overlay.reset();
        sampler.reset();
        vertices.reset();
        renderPass.reset();
    }
};

AcceleratedSurface::AcceleratedSurface(QWidget* parent) : QRhiWidget(parent) {
    connect(this,&QRhiWidget::frameSubmitted,this,[this]{++completedFrames_;});
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    const auto requested = qApp ? qApp->property("renderApi").toString().trimmed().toLower() : QString{};
#ifdef Q_OS_WIN
    setApi(requested == QStringLiteral("opengl") ? Api::OpenGL : Api::Direct3D11);
    supportedApi_ = requested.isEmpty() || requested == QStringLiteral("auto") ||
                    requested == QStringLiteral("d3d11") || requested == QStringLiteral("opengl");
#else
    setApi(Api::OpenGL);
    supportedApi_ = requested.isEmpty() || requested == QStringLiteral("auto") || requested == QStringLiteral("opengl");
#endif
    if (!supportedApi_)
        fail(QStringLiteral("当前 QRhi 骨架不支持请求的后端 %1；Vulkan 尚需平台初始化与独立验证").arg(requested));
    connect(this, &QRhiWidget::renderFailed, this, [this] {
        fail(QStringLiteral("QRhi 设备创建或帧提交失败，使用 QWidget 回退"));
    });
}

AcceleratedSurface::~AcceleratedSurface() {
    releaseResources();
}

void AcceleratedSurface::setPainter(PainterCallback painter) {
    painter_ = std::move(painter);
    invalidateOverlay();
}

void AcceleratedSurface::setHeatmap(const QImage& image, const QRectF& target, const QString& revision) {
    if (target_ != target) overlayDirty_ = true;
    target_ = target;
    // The producer owns revision identity. Placement and overlay updates never
    // replace or re-upload the pixels of an identical heatmap revision.
    if (!hasRevision_ || revision_ != revision) {
        heatmap_ = image;
        revision_ = revision;
        hasRevision_ = true;
        heatmapDirty_ = !heatmap_.isNull();
        overlayDirty_ = true;
        if (heatmap_.isNull()) heatmapUploaded_ = false;
    }
    update();
}

void AcceleratedSurface::invalidateOverlay() {
    overlayDirty_ = true;
    update();
}
void AcceleratedSurface::setHeatmapSourceRect(const QRectF& normalizedSource) {
    if (sourceRect_ == normalizedSource) return;
    sourceRect_ = normalizedSource;
    update();
}

bool AcceleratedSurface::isReady() const {
    return ready_ && resources_ != nullptr;
}

QString AcceleratedSurface::backendDescription() const {
    return backend_;
}

QJsonObject AcceleratedSurface::cpuWallTimings() const {
    const auto json = [](const CpuWallStage& stage) {
        return QJsonObject{{"lastMs", stage.lastMs}, {"maxMs", stage.maxMs},
            {"totalMs", stage.totalMs}, {"samples", static_cast<qint64>(stage.samples)}};
    };
    return {{"unit", "ms"}, {"measurement", "GUI-thread wall time measured with QElapsedTimer"},
        {"gpuTimestamps", false},
        {"prepareUploads", json(prepareUploadsTiming_)},
        {"overlayImagePreparation", json(overlayImagePreparationTiming_)},
        {"overlayPainter", json(overlayPainterTiming_)},
        {"overlayUploadEnqueue", json(overlayUploadEnqueueTiming_)},
        {"heatmapUploadPreparation", json(heatmapUploadPreparationTiming_)},
        {"render", json(renderTiming_)},
        {"paintEventIncludingQtSubmit", json(paintEventTiming_)}};
}

void AcceleratedSurface::paintEvent(QPaintEvent* event) {
    // The base calls render(), then endOffscreenFrame(). Including the base
    // captures Qt's D3D11 command execution/upload submission and Flush, which
    // are outside render(). This is wall time, not measured GPU execution time.
    const CpuWallScope timer(paintEventTiming_);
    QRhiWidget::paintEvent(event);
}

void AcceleratedSurface::initialize(QRhiCommandBuffer*) {
    if (!supportedApi_ || failureReported_) return;
    if (!rhi() || !renderTarget() || !renderTarget()->renderPassDescriptor()) {
        fail(QStringLiteral("QRhi 没有有效设备或渲染目标"));
        return;
    }
    if (resources_ && resources_->owner != rhi()) releaseResources();
    const bool firstInitialization = !resources_;
    if (firstInitialization && !createResources()) return;
    if (!ensurePipeline()) return;
    // Qt also invokes initialize on ordinary resize. Heatmap texture and its
    // uploaded revision survive while the QRhi device remains the same.
    if (overlay_.size() != renderTarget()->pixelSize() || !qFuzzyCompare(overlayDpr_, devicePixelRatioF()))
        overlayDirty_ = true;
    ready_ = true;
    if (firstInitialization) emit backendReady(backend_);
}

bool AcceleratedSurface::createResources() {
    const auto driver = rhi()->driverInfo();
    const auto device = QString::fromUtf8(driver.deviceName);
    backend_ = QStringLiteral("QRhi %1 | %2 | vendor=0x%3 device=0x%4")
                   .arg(QString::fromLatin1(rhi()->backendName()), device)
                   .arg(driver.vendorId, 0, 16).arg(driver.deviceId, 0, 16);
    if (rhi()->backend() == QRhi::Null || driver.deviceType == QRhiDriverInfo::CpuDevice ||
        isSoftwareRenderer(device)) {
        fail(QStringLiteral("检测到软件渲染设备，使用 QWidget 回退"));
        return false;
    }
    resources_ = std::make_unique<Resources>();
    resources_->owner = rhi();
    resources_->vertexShader = loadShader(QStringLiteral(":/signalstudio/shaders/texture.vert.qsb"));
    resources_->fragmentShader = loadShader(QStringLiteral(":/signalstudio/shaders/texture.frag.qsb"));
    if (!resources_->vertexShader.isValid() || !resources_->fragmentShader.isValid()) {
        fail(QStringLiteral("QRhi 纹理着色器资源缺失或无效"));
        return false;
    }
    resources_->vertices.reset(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                               static_cast<quint32>(8 * sizeof(Vertex))));
    resources_->sampler.reset(rhi()->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                               QRhiSampler::None, QRhiSampler::ClampToEdge,
                                               QRhiSampler::ClampToEdge));
    if (!resources_->vertices->create() || !resources_->sampler->create()) {
        fail(QStringLiteral("无法创建 QRhi 顶点缓冲或纹理采样器"));
        return false;
    }
    if (!ensureTexture(false, QSize(1, 1)) || !ensureTexture(true, QSize(1, 1))) return false;
    heatmapDirty_ = !heatmap_.isNull();
    heatmapUploaded_ = false;
    overlayDirty_ = true;
    return true;
}

bool AcceleratedSurface::ensurePipeline() {
    auto* pass = renderTarget()->renderPassDescriptor();
    const int samples = renderTarget()->sampleCount();
    if (resources_->pipeline && resources_->samples == samples && resources_->renderPass->isCompatible(pass))
        return true;
    resources_->pipeline.reset();
    resources_->renderPass.reset(pass->newCompatibleRenderPassDescriptor());
    resources_->pipeline.reset(rhi()->newGraphicsPipeline());
    auto* pipeline = resources_->pipeline.get();
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline->setSampleCount(samples);
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->setDepthTest(false);
    pipeline->setDepthWrite(false);
    pipeline->setShaderStages({{QRhiShaderStage::Vertex, resources_->vertexShader},
                               {QRhiShaderStage::Fragment, resources_->fragmentShader}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{static_cast<quint32>(sizeof(Vertex))}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                           {0, 1, QRhiVertexInputAttribute::Float2, static_cast<quint32>(2 * sizeof(float))}});
    pipeline->setVertexInputLayout(layout);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    pipeline->setTargetBlends({blend});
    pipeline->setShaderResourceBindings(resources_->heatmapBindings.get());
    pipeline->setRenderPassDescriptor(resources_->renderPass.get());
    if (!pipeline->create()) {
        fail(QStringLiteral("无法创建 QRhi 纹理合成管线"));
        return false;
    }
    resources_->samples = samples;
    return true;
}

bool AcceleratedSurface::ensureTexture(bool overlay, const QSize& pixelSize) {
    auto& texture = overlay ? resources_->overlay : resources_->heatmap;
    auto& bindings = overlay ? resources_->overlayBindings : resources_->heatmapBindings;
    if (texture && texture->pixelSize() == pixelSize && bindings) return true;
    const int maximum = rhi()->resourceLimit(QRhi::TextureSizeMax);
    if (pixelSize.isEmpty() || pixelSize.width() > maximum || pixelSize.height() > maximum) {
        fail(QStringLiteral("QRhi 纹理尺寸无效或超过设备上限 %1").arg(maximum));
        return false;
    }
    if (!texture) texture.reset(rhi()->newTexture(QRhiTexture::RGBA8, pixelSize));
    else texture->setPixelSize(pixelSize);
    if (!texture->create()) {
        fail(QStringLiteral("无法创建 QRhi %1纹理").arg(overlay ? QStringLiteral("覆盖层") : QStringLiteral("热图")));
        return false;
    }
    if (!bindings) bindings.reset(rhi()->newShaderResourceBindings());
    bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
        0, QRhiShaderResourceBinding::FragmentStage, texture.get(), resources_->sampler.get())});
    if (!bindings->create()) {
        fail(QStringLiteral("无法创建 QRhi 纹理资源绑定"));
        return false;
    }
    return true;
}

bool AcceleratedSurface::prepareUploads(QRhiResourceUpdateBatch* updates) {
    const CpuWallScope prepareTimer(prepareUploadsTiming_);
    if (heatmapDirty_) {
        const CpuWallScope heatmapTimer(heatmapUploadPreparationTiming_);
        const auto pixels = heatmap_.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        if (pixels.isNull() || !ensureTexture(false, pixels.size())) return false;
        updates->uploadTexture(resources_->heatmap.get(), pixels);
        ++textureUploads_;
        heatmapDirty_ = false;
        heatmapUploaded_ = true;
    }
    const auto pixels = renderTarget()->pixelSize();
    const auto dpr = devicePixelRatioF();
    if (overlay_.size() != pixels || !qFuzzyCompare(overlayDpr_, dpr)) overlayDirty_ = true;
    if (overlayDirty_) {
        {
            const CpuWallScope imageTimer(overlayImagePreparationTiming_);
            // Keep the raster allocation across ordinary frames. Qt may still
            // detach here if an earlier queued upload owns the same image.
            if (overlay_.size() != pixels || !qFuzzyCompare(overlayDpr_, dpr)) {
                overlay_ = QImage(pixels, QImage::Format_RGBA8888_Premultiplied);
                if (overlay_.isNull()) {
                    fail(QStringLiteral("无法创建 QRhi 覆盖层图像"));
                    return false;
                }
                overlayDpr_ = dpr;
                overlay_.setDevicePixelRatio(dpr);
            }
            overlay_.fill(Qt::transparent);
        }
        {
            const CpuWallScope painterTimer(overlayPainterTiming_);
            QPainter painter(&overlay_);
            painter.setFont(font());
            if (painter_) painter_(painter);
            painter.end();
        }
        {
            const CpuWallScope enqueueTimer(overlayUploadEnqueueTiming_);
            if (!ensureTexture(true, pixels)) return false;
            updates->uploadTexture(resources_->overlay.get(), overlay_);
        }
        ++overlayUploads_;
        overlayDirty_ = false;
    }
    return true;
}

void AcceleratedSurface::render(QRhiCommandBuffer* commandBuffer) {
    const CpuWallScope renderTimer(renderTiming_);
    if (!ready_ || !resources_ || !commandBuffer || width() <= 0 || height() <= 0) return;
    if (rhi()->isDeviceLost()) {
        fail(QStringLiteral("QRhi 图形设备丢失，使用 QWidget 回退"));
        return;
    }
    auto* updates = rhi()->nextResourceUpdateBatch();
    if (!prepareUploads(updates)) {
        updates->release();
        return;
    }
    const bool yUp = rhi()->isYUpInNDC();
    const auto heatmapQuad = makeQuad(target_, size(), yUp, sourceRect_);
    const auto overlayQuad = makeQuad(QRectF(QPointF(0, 0), size()), size(), yUp);
    std::array<Vertex, 8> vertices;
    std::copy(heatmapQuad.begin(), heatmapQuad.end(), vertices.begin());
    std::copy(overlayQuad.begin(), overlayQuad.end(), vertices.begin() + 4);
    updates->updateDynamicBuffer(resources_->vertices.get(), 0,
                                  static_cast<quint32>(sizeof(vertices)), vertices.data());
    commandBuffer->beginPass(renderTarget(), QColor("#0a1728"), {1.0f, 0}, updates);
    commandBuffer->setGraphicsPipeline(resources_->pipeline.get());
    const auto output = renderTarget()->pixelSize();
    commandBuffer->setViewport(QRhiViewport(0, 0, static_cast<float>(output.width()), static_cast<float>(output.height())));
    const QRhiCommandBuffer::VertexInput input(resources_->vertices.get(), 0);
    commandBuffer->setVertexInput(0, 1, &input);
    if (heatmapUploaded_ && !heatmap_.isNull() && !target_.isEmpty()) {
        commandBuffer->setShaderResources(resources_->heatmapBindings.get());
        commandBuffer->draw(4);
    }
    commandBuffer->setShaderResources(resources_->overlayBindings.get());
    commandBuffer->draw(4, 1, 4);
    commandBuffer->endPass();
}

void AcceleratedSurface::releaseResources() {
    ready_ = false;
    resources_.reset();
    heatmapUploaded_ = false;
    heatmapDirty_ = !heatmap_.isNull();
    overlayDirty_ = true;
}

void AcceleratedSurface::fail(const QString& reason) {
    ready_ = false;
    if (failureReported_) return;
    const auto message = backend_.startsWith(QStringLiteral("QRhi ")) &&
                         backend_ != QStringLiteral("QRhi 初始化中") && !reason.contains(backend_)
                             ? QStringLiteral("%1；%2").arg(reason, backend_) : reason;
    backend_ = message;
    failureReported_ = true;
    // Hiding a surface from its parent must happen after active QRhi callbacks.
    QTimer::singleShot(0, this, [this, message] { emit backendFailed(message); });
}

} // namespace signalstudio
