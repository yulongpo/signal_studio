#pragma once

#include "application/session.h"
#include <QImage>
#include <QTimer>
#include <QWidget>
#include <optional>

namespace signalstudio {
class AcceleratedSurface;

class PlotWidget : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Navigation, Auxiliary, Main };
    PlotWidget(Session& session, Kind kind, QWidget* parent = nullptr);
    QRectF plotRect() const;
    QPointF toPixel(SampleIndex sample, double frequencyHz) const;
    void setCreating(bool enabled);
    bool isCreating() const { return creating_; }
    void cancelGesture(bool exitCreating = false);
    void syncState();
    bool gpuReady() const;
    QString renderingBackend() const;
    quint64 textureUploadCount() const;
    quint64 powerGenerationCount() const { return powerGenerations_; }
signals:
    void stateChanged();
    void statusMessage(const QString& text);
    void markSelectionRequested(const QString& id, Qt::KeyboardModifiers modifiers);
    void backendChanged(const QString& description);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent*) override;
private:
    enum class Tool { Box, Mark, PanX, PanY, Navigate, AuxiliaryY };
    struct Gesture {
        Tool tool;
        std::string fileId;
        QPointF start, current;
        ViewRange view;
        std::string markId;
        ViewRange mark;
        int edges = 0;
        bool changed = false;
        double auxiliaryMin = -1, auxiliaryMax = 1;
    };
    struct Hit { std::string id; int edges = 0; };
    // Offset from the integer view origin; never convert the origin to float for editing.
    struct Coordinate { long double sample; double frequency; };
    Coordinate fromPixel(QPointF position, const ViewRange& view) const;
    QRectF markRect(const ViewRange&) const;
    Hit hitMark(QPointF) const;
    void finishWheel();
    void finishGesture();
    void updateHeatmap();
    void paintScene(QPainter& painter, bool accelerated);
    void repaintChart();
    Session& session_;
    Kind kind_;
    bool creating_ = false;
    bool releasing_ = false;
    std::optional<Gesture> gesture_;
    QTimer wheelTimer_;
    std::optional<ViewRange> wheelBase_;
    std::string wheelFile_;
    bool wheelY_ = false;
    QString powerKey_, colorKey_;
    std::vector<float> power_;
    QImage heatmap_;
    QSize heatSize_;
    AcceleratedSurface* surface_ = nullptr;
    bool softwareFallback_ = false;
    quint64 powerGenerations_ = 0;
};
} // namespace signalstudio
