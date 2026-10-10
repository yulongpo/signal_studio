#pragma once
#include <algorithm>
#include <QFontMetricsF>
#include <QPainter>
#include <QStringList>

namespace signalstudio::cursor_overlay {
inline QRectF draw(QPainter& painter, QRectF plot, QPointF position, bool crosshair,
                   const QString& text, QColor color, bool pinned) {
    painter.save(); painter.setClipRect(plot);
    QPen pen(color, pinned ? 1.7 : 1.0);
    if (!pinned) pen.setDashPattern({3, 4});
    painter.setPen(pen);
    painter.drawLine(QPointF(position.x(), plot.top()), QPointF(position.x(), plot.bottom()));
    if (crosshair) painter.drawLine(QPointF(plot.left(), position.y()), QPointF(plot.right(), position.y()));
    QRectF box;
    if (!text.isEmpty()) {
        QFont font = painter.font(); font.setPointSizeF(9); painter.setFont(font);
        const QFontMetricsF metrics(font); const auto lines = text.split('\n');
        double width = 0;
        for (const auto& line : lines) width = std::max(width, metrics.horizontalAdvance(line));
        width = std::min(width + 16, std::max(1.0, plot.width() - 8));
        const double height = std::min(metrics.height() * lines.size() + 12, std::max(1.0, plot.height() - 8));
        double x = position.x() + 14, y = position.y() + 14;
        if (x + width > plot.right() - 4) x = position.x() - width - 14;
        if (y + height > plot.bottom() - 4) y = position.y() - height - 14;
        x = std::clamp(x, plot.left() + 4, std::max(plot.left() + 4, plot.right() - width - 4));
        y = std::clamp(y, plot.top() + 4, std::max(plot.top() + 4, plot.bottom() - height - 4));
        box = QRectF(x, y, width, height);
        painter.setPen(QPen(color, 1)); painter.setBrush(QColor(8, 21, 35, 235)); painter.drawRoundedRect(box, 3, 3);
        painter.setClipRect(box.adjusted(5, 3, -5, -3), Qt::IntersectClip);
        painter.setPen(QColor("#e8f4ff"));
        painter.drawText(box.adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignTop, text);
    }
    painter.restore(); return box;
}
} // namespace signalstudio::cursor_overlay
