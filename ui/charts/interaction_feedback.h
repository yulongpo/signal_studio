#pragma once
#include <QPainter>
#include <QRectF>
namespace signalstudio::chart_feedback {
enum class Zone { None, Plot, XAxis, YAxis };
inline Zone zoneAt(QPointF point, QRectF plot) {
    if (plot.contains(point)) return Zone::Plot;
    if (point.x() >= plot.left() && point.x() <= plot.right() && point.y() > plot.bottom()) return Zone::XAxis;
    if (point.x() >= 0 && point.x() < plot.left() && point.y() >= plot.top() && point.y() <= plot.bottom()) return Zone::YAxis;
    return Zone::None;
}
inline Qt::CursorShape cursor(Zone zone, bool pan = false, bool selecting = false) {
    if (pan) return Qt::ClosedHandCursor;
    if (selecting || zone == Zone::Plot) return Qt::CrossCursor;
    return zone == Zone::XAxis || zone == Zone::YAxis ? Qt::OpenHandCursor : Qt::ArrowCursor;
}
inline void drawAxisHighlight(QPainter& painter, QRectF widget, QRectF plot, Zone zone) {
    if (zone != Zone::XAxis && zone != Zone::YAxis) return;
    painter.save();
    const auto hover = zone == Zone::XAxis ? QRectF(plot.left(),plot.bottom(),plot.width(),widget.bottom()-plot.bottom()) : QRectF(0,plot.top(),plot.left(),plot.height());
    painter.fillRect(hover,QColor(46,148,203,38)); painter.setPen(QPen(QColor(106,197,234,204),1.5));
    painter.drawLine(zone == Zone::XAxis ? QLineF(plot.bottomLeft(),plot.bottomRight()) : QLineF(plot.topLeft(),plot.bottomLeft()));
    painter.restore();
}
} // namespace signalstudio::chart_feedback
