#include "ui/charts/accelerated_surface.h"

#include <QColor>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLPixelTransferOptions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QSurfaceFormat>
#include <QTimer>

#include <array>
#include <cmath>
#include <utility>

namespace signalstudio {
namespace {

QString glText(const GLubyte* text) {
    return text ? QString::fromLatin1(reinterpret_cast<const char*>(text)) : QStringLiteral("unknown");
}

bool isSoftwareRenderer(const QString& renderer) {
    constexpr std::array names{"llvmpipe", "softpipe", "swiftshader", "software",
                               "gdi generic", "microsoft basic render", "warp", "lavapipe", "swrast"};
    for (const auto* name : names)
        if (renderer.contains(QLatin1String(name), Qt::CaseInsensitive)) return true;
    return false;
}

struct Vertex {
    GLfloat x, y, u, v;
};

} // namespace

AcceleratedSurface::AcceleratedSurface(QWidget* parent) : QOpenGLWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    // No GL resources are created until Qt makes the widget context current.
}

AcceleratedSurface::~AcceleratedSurface() {
    cleanup();
}

void AcceleratedSurface::setPainter(PainterCallback painter) {
    painter_ = std::move(painter);
    update();
}

void AcceleratedSurface::setHeatmap(const QImage& image, const QRectF& target, const QString& revision) {
    target_ = target;
    // Repositioning a cached image (including resize) never uploads its pixels.
    // The producer owns revision identity; identical revisions retain the image.
    if (!hasRevision_ || revision_ != revision) {
        heatmap_ = image;
        revision_ = revision;
        hasRevision_ = true;
        textureDirty_ = !heatmap_.isNull();
        if (heatmap_.isNull()) textureUploaded_ = false;
    }
    update();
}

bool AcceleratedSurface::isReady() const {
    return ready_ && isValid();
}

QString AcceleratedSurface::backendDescription() const {
    return backend_;
}

void AcceleratedSurface::initializeGL() {
    failureReported_ = false;
    ready_ = false;
    auto* current = context();
    if (!current || !current->isValid()) {
        fail(QStringLiteral("无法创建有效的 OpenGL 上下文"));
        return;
    }
    resourceContext_ = current;
    destructionConnection_ = connect(current, &QOpenGLContext::aboutToBeDestroyed,
                                     this, &AcceleratedSurface::cleanup, Qt::DirectConnection);
    initializeOpenGLFunctions();
    const auto vendor = glText(glGetString(GL_VENDOR));
    const auto renderer = glText(glGetString(GL_RENDERER));
    const auto version = glText(glGetString(GL_VERSION));
    backend_ = QStringLiteral("OpenGL %1 | %2 | %3").arg(version, vendor, renderer);
    if (isSoftwareRenderer(renderer)) {
        fail(QStringLiteral("检测到软件 OpenGL 渲染器，使用 QWidget 回退：%1").arg(backend_));
        return;
    }

    const bool es = current->isOpenGLES();
    const bool core = !es && current->format().profile() == QSurfaceFormat::CoreProfile;
    const QByteArray vertexSource = core ? QByteArrayLiteral(
        "#version 150\n"
        "in vec2 position;\n"
        "in vec2 texcoord;\n"
        "out vec2 uv;\n"
        "void main() { uv = texcoord; gl_Position = vec4(position, 0.0, 1.0); }\n") :
        QByteArray(es ? "#version 100\n" : "#version 120\n") + QByteArrayLiteral(
        "attribute vec2 position;\n"
        "attribute vec2 texcoord;\n"
        "varying vec2 uv;\n"
        "void main() { uv = texcoord; gl_Position = vec4(position, 0.0, 1.0); }\n");
    const QByteArray fragmentSource = core ? QByteArrayLiteral(
        "#version 150\n"
        "uniform sampler2D heatmap;\n"
        "in vec2 uv;\n"
        "out vec4 fragmentColor;\n"
        "void main() { fragmentColor = texture(heatmap, uv); }\n") :
        QByteArray(es ? "#version 100\nprecision mediump float;\n" : "#version 120\n") + QByteArrayLiteral(
        "uniform sampler2D heatmap;\n"
        "varying vec2 uv;\n"
        "void main() { gl_FragColor = texture2D(heatmap, uv); }\n");

    program_ = std::make_unique<QOpenGLShaderProgram>();
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource) ||
        !program_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource)) {
        fail(QStringLiteral("OpenGL 热图着色器编译失败：%1").arg(program_->log()));
        return;
    }
    program_->bindAttributeLocation("position", 0);
    program_->bindAttributeLocation("texcoord", 1);
    if (!program_->link()) {
        fail(QStringLiteral("OpenGL 热图着色器链接失败：%1").arg(program_->log()));
        return;
    }

    vertices_ = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
    if (!vertices_->create()) {
        fail(QStringLiteral("无法创建 OpenGL 顶点缓冲"));
        return;
    }
    vertices_->setUsagePattern(QOpenGLBuffer::DynamicDraw);
    vertexArray_ = std::make_unique<QOpenGLVertexArrayObject>();
    const bool hasVao = vertexArray_->create();
    if (core && !hasVao) {
        fail(QStringLiteral("OpenGL core profile 需要有效的 VAO"));
        return;
    }
    texture_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    if (!texture_->create()) {
        fail(QStringLiteral("无法创建 OpenGL 热图纹理"));
        return;
    }
    textureDirty_ = !heatmap_.isNull();
    textureUploaded_ = false;
    ready_ = true;
    emit backendReady(backend_);
}

void AcceleratedSurface::resizeGL(int, int) {
    // Only quad positions and the viewport change. Pixel storage remains valid.
}

void AcceleratedSurface::paintEvent(QPaintEvent* event) {
    QOpenGLWidget::paintEvent(event);
    // initializeGL is not called when the platform cannot create a context.
    if (isVisible() && !isValid() && !failureReported_)
        fail(QStringLiteral("平台无法初始化 QOpenGLWidget，使用 QWidget 回退"));
}

void AcceleratedSurface::paintGL() {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#0a1728"));
    if (ready_ && !heatmap_.isNull() && !target_.isEmpty()) {
        painter.beginNativePainting();
        if (uploadHeatmap()) drawHeatmap();
        painter.endNativePainting();
    }
    if (painter_) painter_(painter);
}

bool AcceleratedSurface::uploadHeatmap() {
    if (!textureDirty_) return textureUploaded_;
    if (!texture_ || heatmap_.isNull()) return false;
    const QImage pixels = heatmap_.convertToFormat(QImage::Format_RGBA8888);
    if (pixels.isNull()) {
        fail(QStringLiteral("无法准备 OpenGL 热图像素"));
        return false;
    }
    GLint maximumSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumSize);
    if (pixels.width() > maximumSize || pixels.height() > maximumSize) {
        fail(QStringLiteral("热图尺寸超过 OpenGL 纹理上限 %1").arg(maximumSize));
        return false;
    }
    // Discard errors from Qt's preceding painter work before inspecting upload.
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    if (!texture_->isStorageAllocated() || texture_->width() != pixels.width() ||
        texture_->height() != pixels.height()) {
        // Qt textures cannot resize allocated storage. Keep the wrapper and
        // allocate a new GL name only when the producer changes image dimensions.
        if (texture_->isStorageAllocated()) texture_->destroy();
        if (!texture_->isCreated() && !texture_->create()) {
            fail(QStringLiteral("无法重新创建 OpenGL 热图纹理"));
            return false;
        }
        texture_->setSize(pixels.width(), pixels.height());
        texture_->setFormat(context()->isOpenGLES() && context()->format().majorVersion() < 3
                                ? QOpenGLTexture::RGBAFormat : QOpenGLTexture::RGBA8_UNorm);
        texture_->setMipLevels(1);
        texture_->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8);
        texture_->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
        texture_->setWrapMode(QOpenGLTexture::ClampToEdge);
    }
    if (!texture_->isStorageAllocated()) {
        fail(QStringLiteral("无法分配 OpenGL 热图纹理存储"));
        return false;
    }
    QOpenGLPixelTransferOptions transfer;
    transfer.setAlignment(1);
    texture_->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, pixels.constBits(), &transfer);
    const auto error = glGetError();
    if (error != GL_NO_ERROR) {
        fail(QStringLiteral("OpenGL 热图上传失败 (0x%1)").arg(error, 0, 16));
        return false;
    }
    ++textureUploads_;
    textureDirty_ = false;
    textureUploaded_ = true;
    return true;
}

void AcceleratedSurface::drawHeatmap() {
    if (!textureUploaded_ || !program_ || !vertices_ || width() <= 0 || height() <= 0) return;
    GLint oldViewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glViewport(0, 0, static_cast<GLsizei>(std::lround(width() * devicePixelRatioF())),
               static_cast<GLsizei>(std::lround(height() * devicePixelRatioF())));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    const GLfloat left = static_cast<GLfloat>(2.0 * target_.left() / width() - 1.0);
    const GLfloat right = static_cast<GLfloat>(2.0 * target_.right() / width() - 1.0);
    const GLfloat top = static_cast<GLfloat>(1.0 - 2.0 * target_.top() / height());
    const GLfloat bottom = static_cast<GLfloat>(1.0 - 2.0 * target_.bottom() / height());
    // QImage's first scanline maps to v=0 at the quad's top, avoiding a mirrored
    // image or a second CPU image allocation merely to flip the texture.
    const std::array<Vertex, 4> quad{{{left, top, 0, 0}, {right, top, 1, 0},
                                     {left, bottom, 0, 1}, {right, bottom, 1, 1}}};
    if (vertexArray_ && vertexArray_->isCreated()) vertexArray_->bind();
    if (!program_->bind() || !vertices_->bind()) {
        program_->release();
        if (vertexArray_ && vertexArray_->isCreated()) vertexArray_->release();
        glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
        fail(QStringLiteral("无法绑定 OpenGL 热图绘制资源"));
        return;
    }
    vertices_->allocate(quad.data(), static_cast<int>(sizeof(quad)));
    program_->enableAttributeArray(0);
    program_->enableAttributeArray(1);
    program_->setAttributeBuffer(0, GL_FLOAT, 0, 2, static_cast<int>(sizeof(Vertex)));
    program_->setAttributeBuffer(1, GL_FLOAT, 2 * static_cast<int>(sizeof(GLfloat)), 2, static_cast<int>(sizeof(Vertex)));
    texture_->bind(0);
    program_->setUniformValue("heatmap", 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    texture_->release(0);
    program_->disableAttributeArray(0);
    program_->disableAttributeArray(1);
    vertices_->release();
    program_->release();
    if (vertexArray_ && vertexArray_->isCreated()) vertexArray_->release();
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    const auto error = glGetError();
    if (error != GL_NO_ERROR)
        fail(QStringLiteral("OpenGL 热图绘制失败 (0x%1)").arg(error, 0, 16));
}

void AcceleratedSurface::fail(const QString& reason) {
    ready_ = false;
    if (failureReported_) return;
    const auto message = backend_.startsWith(QStringLiteral("OpenGL ")) &&
                         backend_ != QStringLiteral("OpenGL 初始化中") && !reason.contains(backend_)
                             ? QStringLiteral("%1；%2").arg(reason, backend_) : reason;
    backend_ = message;
    failureReported_ = true;
    // A parent may hide the surface in response. Notify after active GL/painter
    // callbacks finish rather than mutate the widget hierarchy inside a paint.
    QTimer::singleShot(0, this, [this, message] { emit backendFailed(message); });
}

void AcceleratedSurface::cleanup() {
    if (cleaning_) return;
    cleaning_ = true;
    ready_ = false;
    disconnect(destructionConnection_);
    destructionConnection_ = {};
    auto* resourceContext = resourceContext_.data();
    bool madeCurrent = false;
    // A context may already have been destroyed by Qt. Never attempt to make an
    // invalid/dangling context current; Qt wrappers can then discard their guards.
    if (resourceContext && resourceContext->isValid()) {
        makeCurrent();
        madeCurrent = QOpenGLContext::currentContext() == resourceContext;
    }
    texture_.reset();
    program_.reset();
    vertices_.reset();
    vertexArray_.reset();
    if (madeCurrent) doneCurrent();
    resourceContext_.clear();
    textureUploaded_ = false;
    textureDirty_ = !heatmap_.isNull();
    cleaning_ = false;
}

} // namespace signalstudio
