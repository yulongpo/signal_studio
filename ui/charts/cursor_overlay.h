#pragma once
#include <algorithm>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QHelpEvent>
#include <QToolTip>
#include <QWidget>
#include <QVariant>
#include <QStringList>
#include <vector>
#include <functional>
namespace signalstudio::cursor_overlay {
inline void paintInverseOverlay(QPainter& destination, const QRect& canvas, const std::function<void(QPainter&)>& paintMask) {
    const auto dpr=destination.device()->devicePixelRatioF();
    QImage mask(QSize(qCeil(canvas.width()*dpr),qCeil(canvas.height()*dpr)),QImage::Format_ARGB32_Premultiplied);
    mask.setDevicePixelRatio(dpr); mask.fill(Qt::transparent);
    QPainter painter(&mask); painter.setFont(destination.font()); painter.setRenderHint(QPainter::Antialiasing);
    paintMask(painter); painter.end();
    destination.save(); destination.setCompositionMode(QPainter::CompositionMode_Difference);
    destination.drawImage(canvas.topLeft(),mask); destination.restore();
}
struct Readout { QString x, y, value, details; };
inline Readout parse(const QString& text, bool crosshair, bool swapAxes = false) {
    Readout result;
    for (const auto& line : text.split('\n')) {
        if (line.startsWith("t =") || line.startsWith("f =")) {
            if (!crosshair || (line.startsWith("t =") != swapAxes)) result.x = line;
            else result.y = line;
        } else if (line.contains(" = ")) {
            if (!result.value.isEmpty()) result.value += '\n';
            result.value += line.section(" · ", 0, 0);
            if(line.contains(" · ")) { if(!result.details.isEmpty()) result.details+='\n'; result.details+=line.section(" · ",1); }
        } else { if (!result.details.isEmpty()) result.details += '\n'; result.details += line; }
    }
    if (result.value.isEmpty() && result.details.contains("不可用")) result.value = "数据不可用";
    return result;
}
struct Layout { QRectF x, y, value; QRectF bounds() const { return x.united(y).united(value); }
    std::vector<QRectF> rectangles() const { return {x,y,value}; } };
inline bool tooltip(QWidget* chart,QEvent* event) {
    if(event->type()!=QEvent::ToolTip) return false;
    const auto* help=static_cast<QHelpEvent*>(event);
    for(const QString prefix : {QStringLiteral("pinned"),QStringLiteral("cursor")}) {
        const auto rectangle=chart->property((prefix+"ValueRect").toUtf8()).toRectF();
        const auto details=chart->property((prefix+"ReadoutDetails").toUtf8()).toString();
        if(!rectangle.isEmpty() && rectangle.contains(help->pos()) && !details.isEmpty()) {
            QToolTip::showText(help->globalPos(),details,chart,rectangle.toAlignedRect()); return true;
        }
    }
    return false;
}
inline Layout draw(QPainter& painter, QRectF plot, QPointF position, bool crosshair,
                   const Readout& data, bool pinned, const std::vector<QRectF>& occupied = {}, bool inverseMask = false) {
    painter.save(); painter.setClipRect(plot);
    if (!inverseMask) painter.setCompositionMode(QPainter::CompositionMode_Difference);
    QPen pen(Qt::white, pinned ? 2.0 : 1.0); if (!pinned) pen.setDashPattern({3,4}); painter.setPen(pen);
    painter.drawLine(QPointF(position.x(),plot.top()),QPointF(position.x(),plot.bottom()));
    if (crosshair) painter.drawLine(QPointF(plot.left(),position.y()),QPointF(plot.right(),position.y()));
    QFont font = painter.font(); font.setPointSizeF(9); painter.setFont(font); const QFontMetricsF metrics(font);
    const auto size = [&](const QString& text) {
        double width=0; const auto lines=text.split('\n');
        for (const auto& line:lines) width=std::max(width,metrics.horizontalAdvance(line));
        return QSizeF(std::min(width+4,std::max(1.0,plot.width()-8)),std::min(metrics.height()*lines.size()+4,std::max(1.0,plot.height()-8)));
    };
    const auto clamp = [&](QRectF box) {
        box.moveLeft(std::clamp(box.left(),plot.left()+4,std::max(plot.left()+4,plot.right()-box.width()-4)));
        box.moveTop(std::clamp(box.top(),plot.top()+4,std::max(plot.top()+4,plot.bottom()-box.height()-4))); return box;
    };
    // Every label stays wholly on one side of each cursor line. At an edge,
    // switch sides instead of centering the text over the cursor.
    const auto avoidCursor = [&](QRectF box) {
        const double right=plot.right()-4-position.x()-8, left=position.x()-8-plot.left()-4;
        box.setWidth(std::min(box.width(),std::max(1.0,std::max(left,right))));
        box=clamp(box);
        if(box.left()<position.x()+8 && box.right()>position.x()-8) {
            if(right>=box.width()) box.moveLeft(position.x()+8);
            else box.moveRight(position.x()-8);
        }
        if(crosshair) {
            const double above=position.y()-8-plot.top()-4, below=plot.bottom()-4-position.y()-8;
            box.setHeight(std::min(box.height(),std::max(1.0,std::max(above,below))));
            box=clamp(box);
            if(box.top()<position.y()+8 && box.bottom()>position.y()-8) {
                if(above>=box.height()) box.moveBottom(position.y()-8);
                else box.moveTop(position.y()+8);
            }
        }
        return box;
    };
    Layout layout;
    if (!data.x.isEmpty()) { const auto extent=size(data.x); layout.x=avoidCursor(QRectF(QPointF(position.x()+8,plot.bottom()-extent.height()-4),extent)); }
    if (!data.y.isEmpty()) {
        const auto extent=size(data.y); layout.y=avoidCursor(QRectF(QPointF(plot.left()+4,position.y()-extent.height()-8),extent));
        if(layout.y.intersects(layout.x)) layout.y=avoidCursor(QRectF(QPointF(plot.left()+4,position.y()+8),extent));
    }
    if (!data.value.isEmpty()) {
        const auto extent=size(data.value);
        if (!crosshair) layout.value=avoidCursor(QRectF(QPointF(position.x()+8,plot.top()+26),extent));
        else {
            for(const auto anchor : {QPointF(position.x()+8,position.y()-extent.height()-8),QPointF(position.x()+8,position.y()+8),
                QPointF(position.x()-extent.width()-8,position.y()-extent.height()-8),QPointF(position.x()-extent.width()-8,position.y()+8),
                QPointF(layout.x.left()-extent.width()-8,position.y()-extent.height()-8),QPointF(layout.x.right()+8,position.y()-extent.height()-8),
                QPointF(layout.y.right()+8,position.y()+8),QPointF(layout.y.right()+8,position.y()-extent.height()-8)}) {
                const auto candidate=avoidCursor(QRectF(anchor,extent));
                layout.value=candidate;
                if(!candidate.intersects(layout.x) && !candidate.intersects(layout.y)) break;
            }
        }
    }
    const auto paint = [&](QRectF& box, const QString& text) {
        if (text.isEmpty() || box.isEmpty()) return;
        for (const auto& obstruction:occupied) if (!obstruction.isEmpty() && box.intersects(obstruction)) { box={}; return; }
        painter.save(); painter.setClipRect(box,Qt::IntersectClip); painter.setPen(Qt::white);
        painter.drawText(box.adjusted(2,2,-2,-2),Qt::AlignLeft|Qt::AlignTop,text); painter.restore();
    };
    paint(layout.x,data.x); paint(layout.y,data.y); paint(layout.value,data.value);
    painter.restore(); return layout;
}
} // namespace signalstudio::cursor_overlay
