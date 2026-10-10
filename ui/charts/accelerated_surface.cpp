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
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
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

QImage defaultPalette() {
    constexpr std::array<std::array<int, 3>, 8> stops{{
        {{8, 14, 52}}, {{20, 58, 141}}, {{10, 144, 216}}, {{22, 207, 222}},
        {{40, 225, 138}}, {{224, 229, 70}}, {{250, 159, 52}}, {{188, 49, 52}}}};
    QImage image(256, 1, QImage::Format_RGBA8888);
    auto* row = image.scanLine(0);
    for (int index = 0; index < 256; ++index) {
        const double position = index / 255.0 * (stops.size() - 1);
        const auto segment = std::min<std::size_t>(static_cast<std::size_t>(position), stops.size() - 2);
        const double fraction = position - segment;
        for (int channel = 0; channel < 3; ++channel)
            row[index * 4 + channel] = static_cast<uchar>(std::lround(
                stops[segment][channel] * (1 - fraction) + stops[segment + 1][channel] * fraction));
        row[index * 4 + 3] = 255;
    }
    return image;
}

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
    QShader overlayFragmentShader;
    std::unique_ptr<QRhiBuffer> vertices;
    std::unique_ptr<QRhiBuffer> chartVertices;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiTexture> heatmap;
    std::unique_ptr<QRhiTexture> overlay;
    std::unique_ptr<QRhiTexture> inverseOverlay;
    std::unique_ptr<QRhiTexture> palette;
    std::unique_ptr<QRhiTexture> chartPalette;
    bool paletteCreated = false;
    bool chartPaletteCreated = false;
    std::unique_ptr<QRhiShaderResourceBindings> heatmapBindings;
    std::unique_ptr<QRhiShaderResourceBindings> overlayBindings;
    std::unique_ptr<QRhiShaderResourceBindings> inverseBindings;
    std::unique_ptr<QRhiShaderResourceBindings> chartBindings;
    std::unique_ptr<QRhiRenderPassDescriptor> renderPass;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::unique_ptr<QRhiGraphicsPipeline> overlayPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> inversePipeline;
    int samples = 0;

    ~Resources() {
        pipeline.reset();
        overlayPipeline.reset();
        inversePipeline.reset();
        heatmapBindings.reset();
        overlayBindings.reset();
        inverseBindings.reset();
        chartBindings.reset();
        heatmap.reset();
        overlay.reset();
        inverseOverlay.reset();
        palette.reset();
        chartPalette.reset();
        sampler.reset();
        vertices.reset();
        chartVertices.reset();
        renderPass.reset();
    }
};

AcceleratedSurface::AcceleratedSurface(QWidget* parent) : QRhiWidget(parent), palette_(defaultPalette()),
    chartPalette_(256, 1, QImage::Format_RGBA8888) {
    chartPalette_.fill(Qt::transparent);
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
void AcceleratedSurface::setInversePainter(PainterCallback painter) {
    inversePainter_ = std::move(painter);
    invalidateOverlay();
}

void AcceleratedSurface::setChartGeometry(std::vector<ChartVertex> vertices,
                                          std::vector<ChartDrawCall> draws,
                                          const QString& revision, const QRectF& clipRect) {
    if (chartClipRect_ != clipRect) { chartClipRect_ = clipRect; update(); }
    if (chartGeometryRevision_ == revision) return;
    chartGeometryRevision_ = revision;

    // Keep chart primitives on the GPU, but expand line strips to thin triangle
    // quads and join independent triangles with degenerate strip connectors.
    // Reusing the proven QRhi texture pipeline and a per-chart color LUT avoids
    // a second backend-specific pipeline. CPU code only prepares vertices; it
    // never rasterizes chart contents or uploads a finished chart image.
    std::vector<ChartVertex> expandedVertices;
    std::vector<ChartDrawCall> expandedDraws;
    const double logicalWidth = std::max(1, width());
    const double logicalHeight = std::max(1, height());
    for (const auto& draw : draws) {
        const quint64 end = static_cast<quint64>(draw.firstVertex) + draw.vertexCount;
        if (draw.vertexCount < 2 || end > vertices.size()) continue;
        if (draw.topology == ChartDrawCall::Topology::Triangles) {
            const auto first = static_cast<quint32>(expandedVertices.size());
            expandedVertices.insert(expandedVertices.end(), vertices.begin() + draw.firstVertex,
                                  vertices.begin() + static_cast<std::ptrdiff_t>(end));
            expandedDraws.push_back({ChartDrawCall::Topology::Triangles, first, draw.vertexCount});
            continue;
        }

        const auto first = static_cast<quint32>(expandedVertices.size());
        for (quint32 index = 1; index < draw.vertexCount; ++index) {
            const auto& a = vertices[draw.firstVertex + index - 1];
            const auto& b = vertices[draw.firstVertex + index];
            const double dxPixels = (static_cast<double>(b.x) - a.x) * logicalWidth;
            const double dyPixels = (static_cast<double>(b.y) - a.y) * logicalHeight;
            const double length = std::hypot(dxPixels, dyPixels);
            if (!std::isfinite(length) || length < 1e-5) continue;
            constexpr double halfWidthPixels = 0.7;
            const float offsetX = static_cast<float>((-dyPixels / length) * halfWidthPixels / logicalWidth);
            const float offsetY = static_cast<float>((dxPixels / length) * halfWidthPixels / logicalHeight);
            const ChartVertex aPlus{a.x + offsetX, a.y + offsetY, a.r, a.g, a.b, a.a};
            const ChartVertex aMinus{a.x - offsetX, a.y - offsetY, a.r, a.g, a.b, a.a};
            const ChartVertex bPlus{b.x + offsetX, b.y + offsetY, b.r, b.g, b.b, b.a};
            const ChartVertex bMinus{b.x - offsetX, b.y - offsetY, b.r, b.g, b.b, b.a};
            expandedVertices.insert(expandedVertices.end(), {aPlus, aMinus, bPlus,
                                                             bPlus, aMinus, bMinus});
        }
        const auto count = static_cast<quint32>(expandedVertices.size()) - first;
        if (count >= 3) expandedDraws.push_back({ChartDrawCall::Topology::Triangles, first, count});
    }

    chartPalette_.fill(Qt::transparent);
    std::unordered_map<QRgb, quint16> paletteSlots;
    std::vector<QColor> paletteColors;
    paletteColors.reserve(32);
    const auto colorSlot = [&](const ChartVertex& vertex) -> quint16 {
        QColor color;
        color.setRgbF(std::clamp<double>(vertex.r, 0.0, 1.0),
                      std::clamp<double>(vertex.g, 0.0, 1.0),
                      std::clamp<double>(vertex.b, 0.0, 1.0),
                      std::clamp<double>(vertex.a, 0.0, 1.0));
        const QRgb key = color.rgba();
        if (const auto found = paletteSlots.find(key); found != paletteSlots.end()) return found->second;
        quint16 slot = 0;
        if (paletteColors.size() < 256) {
            slot = static_cast<quint16>(paletteColors.size());
            paletteColors.push_back(color);
            chartPalette_.setPixelColor(slot, 0, color);
        } else {
            double nearestDistance = std::numeric_limits<double>::infinity();
            for (std::size_t index = 0; index < paletteColors.size(); ++index) {
                const auto& candidate = paletteColors[index];
                const double dr = candidate.redF() - color.redF();
                const double dg = candidate.greenF() - color.greenF();
                const double db = candidate.blueF() - color.blueF();
                const double da = candidate.alphaF() - color.alphaF();
                const double distance = dr * dr + dg * dg + db * db + da * da;
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    slot = static_cast<quint16>(index);
                }
            }
        }
        paletteSlots.emplace(key, slot);
        return slot;
    };

    std::vector<GpuChartVertex> encodedVertices;
    std::vector<ChartDrawCall> encodedDraws;
    encodedVertices.reserve(expandedVertices.size() + expandedDraws.size() * 8);
    for (const auto& draw : expandedDraws) {
        const quint64 end = static_cast<quint64>(draw.firstVertex) + draw.vertexCount;
        if (draw.vertexCount < 3 || end > expandedVertices.size()) continue;
        const auto first = static_cast<quint32>(encodedVertices.size());
        for (quint32 index = 0; index + 2 < draw.vertexCount; index += 3) {
            const auto encode = [&colorSlot](const ChartVertex& vertex) {
                const auto slot = colorSlot(vertex);
                return GpuChartVertex{vertex.x, vertex.y,
                    (static_cast<float>(slot) + 0.5f) / 256.0f, 0.5f};
            };
            const auto a = encode(expandedVertices[draw.firstVertex + index]);
            const auto b = encode(expandedVertices[draw.firstVertex + index + 1]);
            const auto c = encode(expandedVertices[draw.firstVertex + index + 2]);
            if (encodedVertices.size() == first) {
                encodedVertices.insert(encodedVertices.end(), {a, b, c});
            } else {
                // Degenerate connectors join independent triangles into one
                // triangle-strip draw. Geometry and palette lookup stay on GPU.
                encodedVertices.push_back(encodedVertices.back());
                encodedVertices.insert(encodedVertices.end(), {a, a, b, c});
            }
        }
        const auto count = static_cast<quint32>(encodedVertices.size()) - first;
        if (count >= 3) encodedDraws.push_back({ChartDrawCall::Topology::TriangleStrip, first, count});
    }
    chartVertices_ = std::move(encodedVertices);
    chartDraws_ = std::move(encodedDraws);
    chartGeometryDirty_ = true;
    chartPaletteDirty_ = true;
    update();
}

void AcceleratedSurface::setHeatmap(const QImage& image, const QRectF& target, const QString& revision,
                                    const QImage& palette) {
    if (target_ != target) overlayDirty_ = true;
    target_ = target;
    const auto nextPalette = palette.isNull() ? QImage{} : palette.convertToFormat(QImage::Format_RGBA8888);
    if (!nextPalette.isNull() &&
        (palette_.size() != nextPalette.size() || palette_.cacheKey() != nextPalette.cacheKey())) {
        palette_ = nextPalette;
        paletteDirty_ = true;
    }
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
        {"chartVertexUploads", static_cast<qint64>(chartVertexUploads_)},
        {"chartDrawCalls", static_cast<qint64>(chartDrawCalls_)},
        {"lastChartVertexCount", static_cast<qint64>(lastChartVertexCount_)},
        {"lastChartDrawRecordCount", static_cast<qint64>(lastChartDrawRecordCount_)},
        {"chartDrawCallsSkipped", static_cast<qint64>(chartDrawCallsSkipped_)},
        {"lastChartDrawLoopIterations", static_cast<qint64>(lastChartDrawLoopIterations_)},
        {"lastChartDrawStage", static_cast<qint64>(lastChartDrawStage_)},
        {"renderInvocations", static_cast<qint64>(renderInvocations_)},
        {"lastRenderWidth", lastRenderSize_.width()}, {"lastRenderHeight", lastRenderSize_.height()},
        {"heatmapDrawCalls", static_cast<qint64>(heatmapDrawCalls_)},
        {"paletteUploads", static_cast<qint64>(paletteUploads_)},
        {"chartPaletteUploads", static_cast<qint64>(chartPaletteUploads_)},
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
    resources_->fragmentShader = loadShader(QStringLiteral(":/signalstudio/shaders/heatmap.frag.qsb"));
    resources_->overlayFragmentShader = loadShader(QStringLiteral(":/signalstudio/shaders/texture.frag.qsb"));
    if (!resources_->vertexShader.isValid() || !resources_->fragmentShader.isValid() ||
        !resources_->overlayFragmentShader.isValid()) {
        fail(QStringLiteral("QRhi 纹理着色器资源缺失或无效"));
        return false;
    }
    resources_->vertices.reset(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                               static_cast<quint32>(8 * sizeof(Vertex))));
    resources_->chartVertices.reset(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                                     static_cast<quint32>(sizeof(GpuChartVertex))));
    resources_->sampler.reset(rhi()->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                               QRhiSampler::None, QRhiSampler::ClampToEdge,
                                               QRhiSampler::ClampToEdge));
    if (!resources_->vertices->create() || !resources_->chartVertices->create() || !resources_->sampler->create()) {
        fail(QStringLiteral("无法创建 QRhi 顶点缓冲或纹理采样器"));
        return false;
    }
    if (!ensurePaletteTexture() || !ensureChartPaletteTexture() ||
        !ensureTexture(TextureLayer::Heatmap, QSize(1, 1)) || !ensureTexture(TextureLayer::Overlay, QSize(1, 1)) ||
        (inversePainter_ && !ensureTexture(TextureLayer::InverseOverlay, QSize(1, 1)))) return false;
    paletteDirty_ = true;
    heatmapDirty_ = !heatmap_.isNull();
    heatmapUploaded_ = false;
    overlayDirty_ = true;
    return true;
}

bool AcceleratedSurface::ensurePipeline() {
    auto* pass = renderTarget()->renderPassDescriptor();
    const int samples = renderTarget()->sampleCount();
    const bool compatiblePass = resources_->renderPass && resources_->samples == samples &&
                               resources_->renderPass->isCompatible(pass);
    if (resources_->pipeline && resources_->overlayPipeline && (!inversePainter_ || resources_->inversePipeline) && compatiblePass)
        return true;
    if (!compatiblePass) {
        resources_->pipeline.reset();
        resources_->overlayPipeline.reset();
        resources_->inversePipeline.reset();
        resources_->renderPass.reset(pass->newCompatibleRenderPassDescriptor());
        resources_->samples = samples;
    } else {
        // Texture replacement invalidates only the pipelines that bind those
        // textures. Keep the compatible render-pass descriptor and the
        // independent chart-data pipelines alive.
        resources_->pipeline.reset();
        resources_->overlayPipeline.reset();
        resources_->inversePipeline.reset();
    }
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
    resources_->overlayPipeline.reset(rhi()->newGraphicsPipeline());
    auto* overlayPipeline = resources_->overlayPipeline.get();
    overlayPipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
    overlayPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    overlayPipeline->setSampleCount(samples);
    overlayPipeline->setCullMode(QRhiGraphicsPipeline::None);
    overlayPipeline->setDepthTest(false);
    overlayPipeline->setDepthWrite(false);
    overlayPipeline->setShaderStages({{QRhiShaderStage::Vertex, resources_->vertexShader},
                                      {QRhiShaderStage::Fragment, resources_->overlayFragmentShader}});
    overlayPipeline->setVertexInputLayout(layout);
    overlayPipeline->setTargetBlends({blend});
    overlayPipeline->setShaderResourceBindings(resources_->overlayBindings.get());
    overlayPipeline->setRenderPassDescriptor(resources_->renderPass.get());
    if (!overlayPipeline->create()) {
        fail(QStringLiteral("无法创建 QRhi 覆盖层合成管线"));
        return false;
    }
    if (inversePainter_) {
        resources_->inversePipeline.reset(rhi()->newGraphicsPipeline());
        auto* inverse = resources_->inversePipeline.get();
        inverse->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        inverse->setSampleCount(samples);
        inverse->setCullMode(QRhiGraphicsPipeline::None);
        inverse->setDepthTest(false); inverse->setDepthWrite(false);
        inverse->setShaderStages({{QRhiShaderStage::Vertex, resources_->vertexShader},
                                 {QRhiShaderStage::Fragment, resources_->overlayFragmentShader}});
        inverse->setVertexInputLayout(layout);
        // A premultiplied white alpha mask inverts the actual destination:
        // alpha*(1-dst) + (1-alpha)*dst. Transparent pixels leave it unchanged.
        auto inverseBlend = blend;
        inverseBlend.srcColor = QRhiGraphicsPipeline::OneMinusDstColor;
        inverseBlend.srcAlpha = QRhiGraphicsPipeline::Zero;
        inverseBlend.dstAlpha = QRhiGraphicsPipeline::One;
        inverse->setTargetBlends({inverseBlend});
        inverse->setShaderResourceBindings(resources_->inverseBindings.get());
        inverse->setRenderPassDescriptor(resources_->renderPass.get());
        if (!inverse->create()) { fail(QStringLiteral("无法创建 QRhi 游标反色合成管线")); return false; }
    }
    resources_->samples = samples;
    return true;
}

bool AcceleratedSurface::ensureChartBuffer() {
    if (chartVertices_.empty()) return true;
    const auto bytes = static_cast<quint64>(chartVertices_.size()) * sizeof(GpuChartVertex);
    if (bytes > std::numeric_limits<quint32>::max()) {
        fail(QStringLiteral("GPU 图谱顶点缓冲超过 QRhi 单缓冲上限"));
        return false;
    }
    auto* buffer = resources_->chartVertices.get();
    if (buffer->size() < static_cast<quint32>(bytes)) {
        quint32 capacity = std::max<quint32>(buffer->size(), static_cast<quint32>(sizeof(GpuChartVertex)));
        while (capacity < bytes && capacity <= std::numeric_limits<quint32>::max() / 2) capacity *= 2;
        if (capacity < bytes) capacity = static_cast<quint32>(bytes);
        buffer->destroy();
        buffer->setSize(capacity);
        if (!buffer->create()) {
            fail(QStringLiteral("无法创建 GPU 图谱顶点缓冲"));
            return false;
        }
    }
    return true;
}

bool AcceleratedSurface::ensureTexture(TextureLayer layer, const QSize& pixelSize) {
    const bool overlay = layer != TextureLayer::Heatmap;
    const bool inverse = layer == TextureLayer::InverseOverlay;
    auto& texture = inverse ? resources_->inverseOverlay : overlay ? resources_->overlay : resources_->heatmap;
    auto& bindings = inverse ? resources_->inverseBindings : overlay ? resources_->overlayBindings : resources_->heatmapBindings;
    if (texture && texture->pixelSize() == pixelSize && bindings) return true;
    const int maximum = rhi()->resourceLimit(QRhi::TextureSizeMax);
    if (pixelSize.isEmpty() || pixelSize.width() > maximum || pixelSize.height() > maximum) {
        fail(QStringLiteral("QRhi 纹理尺寸无效或超过设备上限 %1").arg(maximum));
        return false;
    }
    if (texture && texture->pixelSize() != pixelSize) {
        // QRhi textures are native resources after create(). Recreate the
        // texture and every binding/pipeline that refers to it when the plot
        // or overlay changes size; mutating a live D3D11 texture descriptor
        // and calling create() again is not a valid resize operation.
        resources_->pipeline.reset();
        resources_->overlayPipeline.reset();
        resources_->inversePipeline.reset();
        bindings.reset();
        texture->destroy();
        texture->setPixelSize(pixelSize);
    } else if (!texture) {
        texture.reset(rhi()->newTexture(QRhiTexture::RGBA8, pixelSize));
    }
    if (!texture->create()) {
        fail(QStringLiteral("无法创建 QRhi %1纹理").arg(overlay ? QStringLiteral("覆盖层") : QStringLiteral("热图")));
        return false;
    }
    if (!bindings) bindings.reset(rhi()->newShaderResourceBindings());
    if (overlay) {
        bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
            0, QRhiShaderResourceBinding::FragmentStage, texture.get(), resources_->sampler.get())});
    } else {
        bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
            0, QRhiShaderResourceBinding::FragmentStage, texture.get(), resources_->sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, resources_->palette.get(), resources_->sampler.get())});
    }
    if (!bindings->create()) {
        fail(QStringLiteral("无法创建 QRhi 纹理资源绑定"));
        return false;
    }
    return true;
}

bool AcceleratedSurface::ensurePaletteTexture() {
    const QSize size(256, 1);
    const int maximum = rhi()->resourceLimit(QRhi::TextureSizeMax);
    if (size.width() > maximum) {
        fail(QStringLiteral("QRhi 颜色查找纹理超过设备尺寸限制"));
        return false;
    }
    if (!resources_->palette) resources_->palette.reset(rhi()->newTexture(QRhiTexture::RGBA8, size));
    else if (resources_->palette->pixelSize() != size) {
        resources_->palette->setPixelSize(size);
        resources_->paletteCreated = false;
    }
    if (resources_->paletteCreated) return true;
    if (!resources_->palette->create()) {
        fail(QStringLiteral("无法创建 QRhi 图谱颜色查找纹理"));
        return false;
    }
    resources_->paletteCreated = true;
    return true;
}

bool AcceleratedSurface::ensureChartPaletteTexture() {
    if (resources_->chartPaletteCreated && resources_->chartBindings) return true;
    const QSize size(256, 1);
    const int maximum = rhi()->resourceLimit(QRhi::TextureSizeMax);
    if (size.width() > maximum) {
        fail(QStringLiteral("QRhi 曲线颜色查找纹理超过设备尺寸限制"));
        return false;
    }
    if (!resources_->chartPalette)
        resources_->chartPalette.reset(rhi()->newTexture(QRhiTexture::RGBA8, size));
    else if (resources_->chartPalette->pixelSize() != size) {
        resources_->chartBindings.reset();
        resources_->chartPalette->destroy();
        resources_->chartPalette->setPixelSize(size);
        resources_->chartPaletteCreated = false;
    }
    if (!resources_->chartPaletteCreated && !resources_->chartPalette->create()) {
        fail(QStringLiteral("无法创建 QRhi 曲线颜色查找纹理"));
        return false;
    }
    resources_->chartPaletteCreated = true;
    if (!resources_->chartBindings) resources_->chartBindings.reset(rhi()->newShaderResourceBindings());
    resources_->chartBindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
        0, QRhiShaderResourceBinding::FragmentStage, resources_->chartPalette.get(), resources_->sampler.get())});
    if (!resources_->chartBindings->create()) {
        fail(QStringLiteral("无法创建 QRhi 曲线颜色资源绑定"));
        return false;
    }
    return true;
}

bool AcceleratedSurface::prepareUploads(QRhiResourceUpdateBatch* updates) {
    const CpuWallScope prepareTimer(prepareUploadsTiming_);
    if (heatmapDirty_) {
        const CpuWallScope heatmapTimer(heatmapUploadPreparationTiming_);
        QImage pixels(heatmap_.size(), QImage::Format_RGBA8888);
        if (pixels.isNull()) {
            fail(QStringLiteral("无法创建 QRhi 标量图谱上传图像"));
            return false;
        }
        for (int y = 0; y < heatmap_.height(); ++y) {
            auto* output = pixels.scanLine(y);
            const auto* source = heatmap_.constScanLine(y);
            for (int x = 0; x < heatmap_.width(); ++x) {
                int value = 0;
                if (heatmap_.format() == QImage::Format_Indexed8)
                    value = source[x];
                else if (heatmap_.format() == QImage::Format_Grayscale8)
                    value = source[x];
                else
                    value = qGray(heatmap_.pixel(x, y));
                output[x * 4] = static_cast<uchar>(value);
                output[x * 4 + 1] = static_cast<uchar>(value);
                output[x * 4 + 2] = static_cast<uchar>(value);
                output[x * 4 + 3] = 255;
            }
        }
        if (!ensurePaletteTexture()) return false;
        if (pixels.isNull() || !ensureTexture(TextureLayer::Heatmap, pixels.size())) return false;
        updates->uploadTexture(resources_->heatmap.get(), pixels);
        ++textureUploads_;
        heatmapDirty_ = false;
        heatmapUploaded_ = true;
    }
    if (paletteDirty_) {
        if (!ensurePaletteTexture()) return false;
        updates->uploadTexture(resources_->palette.get(), palette_.convertToFormat(QImage::Format_RGBA8888));
        ++paletteUploads_;
        paletteDirty_ = false;
    }
    if (chartPaletteDirty_) {
        if (!ensureChartPaletteTexture()) return false;
        updates->uploadTexture(resources_->chartPalette.get(), chartPalette_);
        ++chartPaletteUploads_;
        chartPaletteDirty_ = false;
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
            if (!ensureTexture(TextureLayer::Overlay, pixels)) return false;
            updates->uploadTexture(resources_->overlay.get(), overlay_);
        }
        if (inversePainter_) {
            if (inverseOverlay_.size() != pixels || !qFuzzyCompare(inverseOverlay_.devicePixelRatio(), dpr)) {
                inverseOverlay_ = QImage(pixels, QImage::Format_RGBA8888_Premultiplied);
                if (inverseOverlay_.isNull()) { fail(QStringLiteral("无法创建游标反色蒙版")); return false; }
                inverseOverlay_.setDevicePixelRatio(dpr);
            }
            inverseOverlay_.fill(Qt::transparent);
            QPainter painter(&inverseOverlay_); painter.setFont(font());
            painter.setRenderHint(QPainter::Antialiasing); inversePainter_(painter); painter.end();
            if (!ensureTexture(TextureLayer::InverseOverlay, pixels)) return false;
            updates->uploadTexture(resources_->inverseOverlay.get(), inverseOverlay_);
        }
        ++overlayUploads_;
        overlayDirty_ = false;
    }
    if (chartGeometryDirty_) {
        if (!ensureChartBuffer()) return false;
        if (!chartVertices_.empty()) {
            auto gpuVertices = chartVertices_;
            const bool yUp = rhi()->isYUpInNDC();
            for (auto& vertex : gpuVertices) {
                vertex.x = 2.0f * vertex.x - 1.0f;
                vertex.y = yUp ? 1.0f - 2.0f * vertex.y : 2.0f * vertex.y - 1.0f;
            }
            updates->updateDynamicBuffer(resources_->chartVertices.get(), 0,
                static_cast<quint32>(gpuVertices.size() * sizeof(GpuChartVertex)), gpuVertices.data());
            ++chartVertexUploads_;
        }
        chartGeometryDirty_ = false;
    }
    return true;
}

void AcceleratedSurface::render(QRhiCommandBuffer* commandBuffer) {
    ++renderInvocations_;
    lastRenderSize_ = size();
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
    if (!ensurePipeline()) {
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
        ++heatmapDrawCalls_;
    }
    lastChartVertexCount_ = static_cast<qsizetype>(chartVertices_.size());
    lastChartDrawRecordCount_ = static_cast<qsizetype>(chartDraws_.size());
    lastChartDrawLoopIterations_ = 0;
    lastChartDrawStage_ = 0;
    if (!chartVertices_.empty() && !chartDraws_.empty()) {
        const QRhiCommandBuffer::VertexInput chartInput(resources_->chartVertices.get(), 0);
        const QRectF clip = chartClipRect_.isEmpty() ? QRectF(QPointF(0, 0), size()) :
            chartClipRect_.intersected(QRectF(QPointF(0, 0), size()));
        const double sx = double(output.width()) / width(), sy = double(output.height()) / height();
        // QRhi scissor coordinates always have a bottom-left origin, including
        // D3D11. Axes and overlays use the full canvas after the data draws.
        const int left = int(std::ceil(clip.left() * sx));
        const int right = int(std::floor(clip.right() * sx));
        const int bottom = int(std::ceil((height() - clip.bottom()) * sy));
        const int top = int(std::floor((height() - clip.top()) * sy));
        for (const auto& draw : chartDraws_) {
            ++lastChartDrawLoopIterations_;
            if (draw.vertexCount < 3 || static_cast<quint64>(draw.firstVertex) + draw.vertexCount > chartVertices_.size()) {
                ++chartDrawCallsSkipped_;
                continue;
            }
            lastChartDrawStage_ = 1;
            commandBuffer->setGraphicsPipeline(resources_->overlayPipeline.get());
            commandBuffer->setScissor(QRhiScissor(left, bottom, std::max(0, right - left), std::max(0, top - bottom)));
            lastChartDrawStage_ = 2;
            commandBuffer->setShaderResources(resources_->chartBindings.get());
            lastChartDrawStage_ = 3;
            commandBuffer->setVertexInput(0, 1, &chartInput);
            lastChartDrawStage_ = 4;
            commandBuffer->draw(draw.vertexCount, 1, draw.firstVertex);
            lastChartDrawStage_ = 5;
            ++chartDrawCalls_;
            lastChartDrawStage_ = 6;
        }
    }
    commandBuffer->setGraphicsPipeline(resources_->overlayPipeline.get());
    commandBuffer->setScissor(QRhiScissor(0, 0, output.width(), output.height()));
    commandBuffer->setShaderResources(resources_->overlayBindings.get());
    // Chart draws bind a different vertex buffer. The overlay must use the
    // full-widget quad again so axes, grid and gesture feedback stay aligned.
    commandBuffer->setVertexInput(0, 1, &input);
    commandBuffer->draw(4, 1, 4);
    if (inversePainter_) {
        commandBuffer->setGraphicsPipeline(resources_->inversePipeline.get());
        commandBuffer->setShaderResources(resources_->inverseBindings.get());
        commandBuffer->draw(4, 1, 4);
        ++inverseOverlayDrawCalls_;
    }
    commandBuffer->endPass();
}

void AcceleratedSurface::releaseResources() {
    ready_ = false;
    resources_.reset();
    heatmapUploaded_ = false;
    heatmapDirty_ = !heatmap_.isNull();
    paletteDirty_ = true;
    chartPaletteDirty_ = true;
    overlayDirty_ = true;
    chartGeometryDirty_ = true;
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
