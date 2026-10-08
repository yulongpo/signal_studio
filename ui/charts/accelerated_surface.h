#pragma once

#include <QImage>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPointer>
#include <QRectF>
#include <QString>

#include <functional>
#include <memory>

class QOpenGLBuffer;
class QOpenGLContext;
class QOpenGLShaderProgram;
class QOpenGLTexture;
class QOpenGLVertexArrayObject;
class QPainter;
class QPaintEvent;

namespace signalstudio {

// The parent owns business coordinates and mouse gestures. This child only
// renders the heatmap texture and the supplied painter overlays.
class AcceleratedSurface : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    using PainterCallback = std::function<void(QPainter&)>;

    explicit AcceleratedSurface(QWidget* parent = nullptr);
    ~AcceleratedSurface() override;

    void setPainter(PainterCallback painter);
    void setHeatmap(const QImage& image, const QRectF& target, const QString& revision);
    bool isReady() const;
    QString backendDescription() const;
    quint64 textureUploadCount() const { return textureUploads_; }

signals:
    void backendReady(QString description);
    void backendFailed(QString reason);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void paintEvent(QPaintEvent* event) override;

private:
    void cleanup();
    void fail(const QString& reason);
    bool uploadHeatmap();
    void drawHeatmap();

    PainterCallback painter_;
    QImage heatmap_;
    QRectF target_;
    QString revision_;
    QString backend_ = QStringLiteral("OpenGL 初始化中");
    bool hasRevision_ = false;
    bool textureDirty_ = false;
    bool textureUploaded_ = false;
    bool ready_ = false;
    bool failureReported_ = false;
    bool cleaning_ = false;
    quint64 textureUploads_ = 0;
    QPointer<QOpenGLContext> resourceContext_;
    QMetaObject::Connection destructionConnection_;
    std::unique_ptr<QOpenGLShaderProgram> program_;
    std::unique_ptr<QOpenGLTexture> texture_;
    std::unique_ptr<QOpenGLBuffer> vertices_;
    std::unique_ptr<QOpenGLVertexArrayObject> vertexArray_;
};

} // namespace signalstudio
