#include "ui/charts/plot_widget.h"
#include "ui/charts/accelerated_surface.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace signalstudio {
namespace {
constexpr int Left = 1, Right = 2, Top = 4, Bottom = 8, Move = 16;
SampleIndex sampleIndex(long double value, SampleIndex maximum) {
    if(value<=0) return 0;
    if(value>=static_cast<long double>(maximum)) return maximum;
    return static_cast<SampleIndex>(value);
}
SampleIndex offsetSample(SampleIndex origin, long double offset, SampleIndex maximum) {
    origin=std::min(origin,maximum);
    if(offset>=0) return origin+sampleIndex(offset,maximum-origin);
    return origin-sampleIndex(-offset,origin);
}
QString tick(double value, double span, bool frequency) {
    double unit = frequency ? (span >= 1e6 ? 1e6 : span >= 1e3 ? 1e3 : 1) :
        (span >= 1 ? 1 : span >= .001 ? .001 : .000001);
    const double step = std::max(span / unit / 5, 1e-12);
    int precision = std::clamp(static_cast<int>(std::ceil(-std::log10(step))) + 1, 0, 8);
    return QString::number(value / unit, 'f', precision);
}
QString unitLabel(double span, bool frequency) {
    return frequency ? (span >= 1e6 ? "MHz" : span >= 1e3 ? "kHz" : "Hz") :
        (span >= 1 ? "s" : span >= .001 ? "ms" : "us");
}
QRgb color(float level, Palette palette) {
    level = std::clamp(level, 0.0f, 1.0f);
    if (palette == Palette::Gray) { int v = static_cast<int>(level * 255); return qRgb(v,v,v); }
    const std::array<QColor, 6> turbo{QColor("#111b50"),QColor("#1558b0"),QColor("#13bcd4"),QColor("#41df97"),QColor("#f1d84d"),QColor("#ed523e")};
    const std::array<QColor, 6> viridis{QColor("#440154"),QColor("#414487"),QColor("#2a788e"),QColor("#22a884"),QColor("#7ad151"),QColor("#fde725")};
    const auto& stops = palette == Palette::Turbo ? turbo : viridis;
    const double offset = level * 5;
    const int i = std::min(4, static_cast<int>(offset));
    const double a = offset - i;
    return qRgb(static_cast<int>(stops[i].red()*(1-a)+stops[i+1].red()*a),
                static_cast<int>(stops[i].green()*(1-a)+stops[i+1].green()*a),
                static_cast<int>(stops[i].blue()*(1-a)+stops[i+1].blue()*a));
}
Qt::CursorShape edgeCursor(int edges) {
    if (edges == Move) return Qt::SizeAllCursor;
    if ((edges & (Left|Right)) && (edges & (Top|Bottom)))
        return edges == (Left|Top) || edges == (Right|Bottom) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    return edges & (Left|Right) ? Qt::SizeHorCursor : Qt::SizeVerCursor;
}
}

PlotWidget::PlotWidget(Session& session, Kind kind, QWidget* parent)
    : QWidget(parent), session_(session), kind_(kind) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(150, kind == Kind::Navigation ? 40 : kind == Kind::Auxiliary ? 80 : 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(kind == Kind::Navigation ? Qt::PointingHandCursor : Qt::CrossCursor);
    wheelTimer_.setSingleShot(true);
    wheelTimer_.setInterval(220);
    connect(&wheelTimer_, &QTimer::timeout, this, &PlotWidget::finishWheel);
    if(QGuiApplication::platformName()!="offscreen"&&QGuiApplication::platformName()!="minimal"&&!qApp->property("softwareRenderer").toBool()) {
        surface_=new AcceleratedSurface(this);surface_->setGeometry(rect());
        surface_->setPainter([this](QPainter& painter){paintScene(painter,true);});
        connect(surface_,&AcceleratedSurface::backendReady,this,[this](const QString& backend){emit backendChanged(backend);});
        connect(surface_,&AcceleratedSurface::backendFailed,this,[this](const QString& reason){softwareFallback_=true;surface_->hide();QWidget::update();emit backendChanged("软件回退 · "+reason);});
    }
}

bool PlotWidget::gpuReady() const { return surface_&&!softwareFallback_&&surface_->isReady(); }
QString PlotWidget::renderingBackend() const {return surface_&&!softwareFallback_?surface_->backendDescription():"QPainter 软件绘制";}
quint64 PlotWidget::textureUploadCount() const {return surface_?surface_->textureUploadCount():0;}
void PlotWidget::repaintChart() {
    if(surface_&&!softwareFallback_) {
        updateHeatmap();
        surface_->setHeatmap(session_.activeFile()&&kind_==Kind::Main?heatmap_:QImage(),plotRect(),powerKey_+"/"+colorKey_);
        surface_->invalidateOverlay();
    } else QWidget::update();
}

QRectF PlotWidget::plotRect() const {
    return kind_ == Kind::Navigation ? QRectF(15, 6, std::max(1,width()-30), std::max(1,height()-22)) :
        QRectF(76, 12, std::max(1,width()-94), std::max(1,height()-50));
}

QPointF PlotWidget::toPixel(SampleIndex sample, double frequencyHz) const {
    auto* file = session_.activeFile(); if (!file) return {};
    const auto& v = kind_ == Kind::Navigation ? fullRange(file->metadata) : file->view;
    const QRectF r = plotRect();
    const double delta=sample>=v.time.begin?static_cast<double>(sample-v.time.begin):-static_cast<double>(v.time.begin-sample);
    const double t = delta/static_cast<double>(v.time.end-v.time.begin);
    const double f = (frequencyHz-v.frequency.lowerHz)/(v.frequency.upperHz-v.frequency.lowerHz);
    const bool waterfall = kind_ == Kind::Main && file->display.mainMode == MainMode::Waterfall;
    return waterfall ? QPointF(r.left()+r.width()*f, r.top()+r.height()*t) :
        QPointF(r.left()+r.width()*t, r.bottom()-r.height()*f);
}

PlotWidget::Coordinate PlotWidget::fromPixel(QPointF position, const ViewRange& v) const {
    const QRectF r = plotRect(); const auto* file = session_.activeFile();
    const double x = std::clamp((position.x()-r.left())/r.width(),0.0,1.0);
    const double y = std::clamp((position.y()-r.top())/r.height(),0.0,1.0);
    const bool waterfall = kind_ == Kind::Main && file && file->display.mainMode == MainMode::Waterfall;
    return {(waterfall?y:x)*static_cast<long double>(v.time.end-v.time.begin),
        v.frequency.lowerHz+(waterfall?x:1-y)*(v.frequency.upperHz-v.frequency.lowerHz)};
}

QRectF PlotWidget::markRect(const ViewRange& v) const {
    return QRectF(toPixel(v.time.begin,v.frequency.lowerHz),toPixel(v.time.end,v.frequency.upperHz)).normalized();
}

void PlotWidget::setCreating(bool enabled) {
    cancelGesture(); creating_ = enabled && session_.activeFile() && kind_ == Kind::Main;
    repaintChart(); emit statusMessage(creating_ ? "持续选择已开启 · 拖动创建标记，Esc 退出" : "持续选择已关闭");
}

void PlotWidget::syncState() {
    if (gesture_ && (!session_.activeFile() || session_.activeFile()->metadata.id != gesture_->fileId)) cancelGesture(true);
    if (!session_.activeFile()) creating_ = false;
    repaintChart();
}

void PlotWidget::finishWheel() {
    wheelTimer_.stop();
    if (wheelBase_) {
        if (auto* f = session_.activeFile(); f && f->metadata.id == wheelFile_) {
            const auto final = f->view;
            f->view = *wheelBase_; session_.setView(final);
        }
        wheelBase_.reset(); emit stateChanged();
    }
}

void PlotWidget::cancelGesture(bool exitCreating) {
    finishWheel();
    if (gesture_) {
        const auto g = *gesture_; gesture_.reset();
        if (auto* f=session_.activeFile(); f && f->metadata.id==g.fileId) {
            f->view=g.view;
            f->display.auxiliaryMin=g.auxiliaryMin;f->display.auxiliaryMax=g.auxiliaryMax;
            if (auto* m=findMark(*f,g.markId)) m->range=g.mark;
        }
        releasing_=true; if (mouseGrabber()==this) releaseMouse(); releasing_=false;
        emit stateChanged();
    }
    if (exitCreating) creating_=false;
    repaintChart();
}

void PlotWidget::updateHeatmap() {
    const auto* f=session_.activeFile(); if (!f || kind_!=Kind::Main) return;
    const QSize size(std::clamp(static_cast<int>(plotRect().width()),1,960),std::clamp(static_cast<int>(plotRect().height()),1,500));
    const auto& v=f->view;
    const QString key=QString::fromStdString(f->metadata.id)+QString("/%1/%2/%3/%4/%5/%6/%7")
        .arg(size.width()).arg(size.height()).arg(v.time.begin).arg(v.time.end)
        .arg(v.frequency.lowerHz,0,'g',17).arg(v.frequency.upperHz,0,'g',17).arg(static_cast<int>(f->display.mainMode));
    if (key != powerKey_) {
        ++powerGenerations_;
        powerKey_=key; heatSize_=size; power_.resize(static_cast<size_t>(size.width())*size.height());
        const auto bounds=fullRange(f->metadata);
        const bool waterfall=f->display.mainMode==MainMode::Waterfall;
        for (int y=0;y<size.height();++y) for (int x=0;x<size.width();++x) {
            const double xr=static_cast<double>(x)/std::max(1,size.width()-1), yr=static_cast<double>(y)/std::max(1,size.height()-1);
            const long double sample=v.time.begin+(waterfall?yr:xr)*(v.time.end-v.time.begin);
            const double frequency=v.frequency.lowerHz+(waterfall?xr:1-yr)*(v.frequency.upperHz-v.frequency.lowerHz);
            const double t=static_cast<double>(sample/f->metadata.sampleCount), freq=(frequency-bounds.frequency.lowerHz)/f->metadata.sampleRateHz;
            const double noise=.05*std::sin(t*8733+freq*4771)+.05*std::sin(t*6241-freq*9331);
            const double carrier=.38*std::exp(-std::pow((freq-.79)/.005,2))+.30*std::exp(-std::pow((freq-.22)/.008,2));
            const double wide=.49*std::exp(-std::pow((freq-.57)/.053,2))*(.7+.3*std::sin(t*82));
            const double burst=(std::sin(t*103)>0 && freq>.29 && freq<.57) ? .34 : 0;
            power_[static_cast<size_t>(y)*size.width()+x]=static_cast<float>(-88+70*(.14+noise+carrier+wide+burst));
        }
        colorKey_.clear();
    }
    const QString colors=QString("%1/%2/%3").arg(static_cast<int>(f->display.palette)).arg(f->display.dynamicRangeDb).arg(f->display.referenceLevelDb);
    if (colors!=colorKey_) {
        colorKey_=colors; heatmap_=QImage(heatSize_,QImage::Format_RGB32);
        for(int y=0;y<heatSize_.height();++y) {
            auto* row=reinterpret_cast<QRgb*>(heatmap_.scanLine(y));
            for(int x=0;x<heatSize_.width();++x) row[x]=color(static_cast<float>((power_[static_cast<size_t>(y)*heatSize_.width()+x]-
                f->display.referenceLevelDb+f->display.dynamicRangeDb)/f->display.dynamicRangeDb),f->display.palette);
        }
    }
}

void PlotWidget::paintEvent(QPaintEvent*) {
    if(surface_&&!softwareFallback_)return;
    QPainter p(this);p.fillRect(rect(),QColor("#0a1728"));paintScene(p,false);
}
void PlotWidget::paintScene(QPainter& p,bool accelerated) {
    p.setRenderHint(QPainter::Antialiasing); const QRectF r=plotRect();
    auto* f=session_.activeFile();
    if (!f) { p.setPen(QColor("#8fa7bf")); p.drawText(rect(),Qt::AlignCenter,kind_==Kind::Main?"空工程 · 从文件菜单添加演示文件":"无活动文件"); return; }
    const auto& v=kind_==Kind::Navigation?fullRange(f->metadata):f->view;
    const bool waterfall=kind_==Kind::Main&&f->display.mainMode==MainMode::Waterfall;
    const bool psd=kind_==Kind::Auxiliary&&f->display.auxiliaryMode==AuxiliaryMode::Psd;
    if(kind_==Kind::Main&&!accelerated) { updateHeatmap(); p.drawImage(r,heatmap_); }
    if(f->display.grid && kind_!=Kind::Navigation) {
        p.setPen(QColor(130,160,184,40));
        for(int i=0;i<=5;++i) { double x=r.left()+r.width()*i/5; p.drawLine(QPointF(x,r.top()),QPointF(x,r.bottom())); }
        for(int i=0;i<=4;++i) { double y=r.top()+r.height()*i/4; p.drawLine(QPointF(r.left(),y),QPointF(r.right(),y)); }
    }
    if(kind_!=Kind::Main) {
        p.save(); p.setClipRect(r); QPainterPath path;
        const int points=std::max(1,static_cast<int>(r.width()));
        for(int i=0;i<=points;++i) {
            const double ratio=static_cast<double>(i)/points;
            const double t=static_cast<double>((static_cast<long double>(v.time.begin)+ratio*(v.time.end-v.time.begin))/f->metadata.sampleCount);
            double value=0;
            if(psd) {
                const double freq=v.frequency.lowerHz+ratio*(v.frequency.upperHz-v.frequency.lowerHz);
                const double z=(freq-f->metadata.centerFrequencyHz)/f->metadata.sampleRateHz;
                const double power=-86+48*std::exp(-std::pow((z-.06)/.09,2))+19*std::exp(-std::pow((z-.29)/.012,2))+2*std::sin(z*1493);
                value=(power-f->display.auxiliaryMin)/(f->display.auxiliaryMax-f->display.auxiliaryMin);
            } else {
                const double amplitude=(.26+.44*std::exp(-std::pow((t-.47)/.035,2)))*std::sin(t*14913)*std::cos(t*2209);
                value=kind_==Kind::Navigation ? .5+amplitude*.5 : (amplitude-f->display.auxiliaryMin)/(f->display.auxiliaryMax-f->display.auxiliaryMin);
            }
            const QPointF point(r.left()+ratio*r.width(),r.bottom()-r.height()*value);
            if(i==0) path.moveTo(point); else path.lineTo(point);
        }
        p.setPen(QPen(QColor(psd?"#40b5ef":"#49d2bf"),1)); p.drawPath(path); p.restore();
    }
    if(kind_==Kind::Navigation) {
        const double start=static_cast<double>(static_cast<long double>(f->view.time.begin)/f->metadata.sampleCount);
        const double span=static_cast<double>(static_cast<long double>(f->view.time.end-f->view.time.begin)/f->metadata.sampleCount);
        QRectF window(r.left()+r.width()*start,r.top(),r.width()*span,r.height());
        p.fillRect(window,QColor(64,181,239,35)); p.setPen(QColor("#69bddc")); p.drawRect(window);
    }
    p.setFont(QFont("Consolas",9)); p.setPen(QColor("#9cb8ce"));
    const double t0=static_cast<double>(static_cast<long double>(v.time.begin)/f->metadata.sampleRateHz);
    const double tspan=static_cast<double>(static_cast<long double>(v.time.end-v.time.begin)/f->metadata.sampleRateHz);
    const double fspan=v.frequency.upperHz-v.frequency.lowerHz;
    const double f0=v.frequency.lowerHz-(f->display.absoluteFrequency?0:f->metadata.centerFrequencyHz);
    const bool xFrequency=waterfall||psd;
    const double x0=xFrequency?f0:t0,xspan=xFrequency?fspan:tspan;
    const bool relativeTicks=std::abs(x0)>xspan*1000;
    for(int i=0;i<=5;++i) {
        double x=r.left()+r.width()*i/5;
        QString label=tick((relativeTicks?0:x0)+xspan*i/5,xspan,xFrequency);
        p.drawText(QRectF(x-45,r.bottom()+3,90,16),Qt::AlignHCenter,label);
    }
    if(kind_!=Kind::Navigation) {
        for(int i=0;i<=4;++i) {
            double y=r.bottom()-r.height()*i/4;
            QString label;
            if(kind_==Kind::Main) label=waterfall?tick(t0+tspan*(1-i/4.0),tspan,false):tick(f0+fspan*i/4,fspan,true);
            else label=QString::number(f->display.auxiliaryMin+(f->display.auxiliaryMax-f->display.auxiliaryMin)*i/4.0,'f',psd?0:2);
            p.drawText(QRectF(15,y-8,54,16),Qt::AlignRight|Qt::AlignVCenter,label);
        }
        p.drawText(QRectF(r.left(),height()-18,r.width(),16),Qt::AlignCenter,
            (xFrequency?"频率 (":"时间 (")+unitLabel(xspan,xFrequency)+")"+(relativeTicks?QString(" · 原点 %1").arg(tick(x0,xspan,xFrequency)):""));
        p.save(); p.translate(12,r.center().y()); p.rotate(-90);
        const QString ylabel=kind_==Kind::Auxiliary?(psd?"功率 (dB，演示)":"归一化幅度"):
            (waterfall?"时间 (":"频率 (")+unitLabel(waterfall?tspan:fspan,!waterfall)+")";
        p.drawText(QRectF(-r.height()/2,-8,r.height(),16),Qt::AlignCenter,ylabel); p.restore();
    }
    if(kind_==Kind::Main) {
        p.save(); p.setClipRect(r.adjusted(-1,-1,1,1));
        for(const auto& m:f->marks) {
            const auto box=markRect(m.range); if(!box.intersects(r)) continue;
            const bool selected=std::find(f->selectedMarkIds.begin(),f->selectedMarkIds.end(),m.id)!=f->selectedMarkIds.end();
            p.fillRect(box,QColor(243,207,103,selected?40:15)); p.setPen(QPen(QColor(selected?"#f3d77a":"#b5c8d8"),selected?2:1)); p.drawRect(box);
            p.drawText(box.topLeft()+QPointF(5,15),QString::fromStdString(m.name));
            if(selected) {
                const std::array<QPointF,8> handles{box.topLeft(),QPointF(box.center().x(),box.top()),box.topRight(),QPointF(box.right(),box.center().y()),box.bottomRight(),QPointF(box.center().x(),box.bottom()),box.bottomLeft(),QPointF(box.left(),box.center().y())};
                for(const auto& point:handles) if(r.contains(point)) p.fillRect(QRectF(point-QPointF(3,3),QSizeF(6,6)),QColor("#f3d77a"));
            }
        }
        if(gesture_&&gesture_->tool==Tool::Box) {
            p.setPen(QPen(QColor(creating_?"#f3d77a":"#7cd9f1"),1,Qt::DashLine)); p.setBrush(QColor(64,181,239,35));
            p.drawRect(QRectF(gesture_->start,gesture_->current).normalized());
        }
        p.restore();
        if(creating_) { p.setPen(QColor("#ffe4a0")); p.drawText(r.adjusted(10,8,-10,-8),Qt::AlignTop|Qt::AlignHCenter,"持续选择信号 · 拖动创建 · Esc 退出"); }
        if(f->display.colorScale) {
            QRectF bar(r.right()-140,r.top()+9,125,9);
            for(int i=0;i<125;++i) p.fillRect(QRectF(bar.left()+i,bar.top(),1,bar.height()),QColor::fromRgb(color(i/124.0f,f->display.palette)));
        }
    }
    p.setPen(QColor("#658da8")); p.drawText(r.adjusted(8,5,-8,-5),Qt::AlignTop|Qt::AlignRight,"演示数据");
}

PlotWidget::Hit PlotWidget::hitMark(QPointF point) const {
    const auto* f=session_.activeFile(); const QRectF plot=plotRect();
    if(!f||creating_||kind_!=Kind::Main||!plot.contains(point)) return {};
    for(auto it=f->marks.rbegin();it!=f->marks.rend();++it) {
        if(std::find(f->selectedMarkIds.begin(),f->selectedMarkIds.end(),it->id)==f->selectedMarkIds.end()) continue;
        const QRectF box=markRect(it->range), clip=box.intersected(plot);
        if(clip.isEmpty()) continue;
        int horizontal=0,vertical=0;
        if(std::abs(point.x()-box.left())<=6 && box.left()>=plot.left() && box.left()<=plot.right() && point.y()>=clip.top()-6 && point.y()<=clip.bottom()+6) horizontal=Left;
        else if(std::abs(point.x()-box.right())<=6 && box.right()>=plot.left() && box.right()<=plot.right() && point.y()>=clip.top()-6 && point.y()<=clip.bottom()+6) horizontal=Right;
        if(std::abs(point.y()-box.top())<=6 && box.top()>=plot.top() && box.top()<=plot.bottom() && point.x()>=clip.left()-6 && point.x()<=clip.right()+6) vertical=Top;
        else if(std::abs(point.y()-box.bottom())<=6 && box.bottom()>=plot.top() && box.bottom()<=plot.bottom() && point.x()>=clip.left()-6 && point.x()<=clip.right()+6) vertical=Bottom;
        if(horizontal||vertical) return {it->id,horizontal|vertical};
        if(clip.contains(point)) return {it->id,Move};
    }
    for(auto it=f->marks.rbegin();it!=f->marks.rend();++it) if(markRect(it->range).contains(point)) return {it->id,Move};
    return {};
}

void PlotWidget::mousePressEvent(QMouseEvent* e) {
    if(e->button()!=Qt::LeftButton || !session_.activeFile()) return;
    cancelGesture(); setFocus(); auto* f=session_.activeFile();
    const auto point=e->position(); const auto hit=hitMark(point); Tool tool=Tool::Box;
    if(kind_==Kind::Navigation) tool=Tool::Navigate;
    else if(point.x()<plotRect().left()) tool=Tool::PanY;
    else if(point.y()>plotRect().bottom()) tool=Tool::PanX;
    else if(!plotRect().contains(point)) return;
    else if(!hit.id.empty()) tool=Tool::Mark;
    else if(kind_==Kind::Auxiliary) tool=Tool::PanX;
    if(kind_==Kind::Auxiliary&&tool==Tool::PanY) tool=Tool::AuxiliaryY;
    if(tool==Tool::Mark) {
        emit markSelectionRequested(QString::fromStdString(hit.id),e->modifiers()); f=session_.activeFile();
        if(!f || !findMark(*f,hit.id)) return;
        if(e->modifiers()&(Qt::ControlModifier|Qt::ShiftModifier)) return;
    }
    Gesture g{tool,f->metadata.id,point,point,f->view,hit.id,{},hit.edges,false};
    g.auxiliaryMin=f->display.auxiliaryMin;g.auxiliaryMax=f->display.auxiliaryMax;
    if(auto* mark=findMark(*f,hit.id)) g.mark=mark->range;
    gesture_=g; grabMouse();
}

void PlotWidget::mouseMoveEvent(QMouseEvent* e) {
    auto* f=session_.activeFile(); if(!f) return;
    const QPointF point=e->position();
    if(!gesture_) {
        const auto hit=hitMark(point);
        setCursor(!hit.id.empty()?edgeCursor(hit.edges):point.x()<plotRect().left()||point.y()>plotRect().bottom()?Qt::OpenHandCursor:kind_==Kind::Navigation?Qt::PointingHandCursor:Qt::CrossCursor);
        if(plotRect().contains(point)) {
            const auto c=fromPixel(point,kind_==Kind::Navigation?fullRange(f->metadata):f->view);
            const auto origin=kind_==Kind::Navigation?SampleIndex{0}:f->view.time.begin;
            emit statusMessage(QString("T %1 s · F %2 MHz").arg((static_cast<double>(origin)+static_cast<double>(c.sample))/f->metadata.sampleRateHz,0,'g',9).arg(c.frequency/1e6,0,'g',9));
        }
        return;
    }
    auto& g=*gesture_; if(f->metadata.id!=g.fileId) { cancelGesture(true); return; }
    g.current=QPointF(std::clamp(point.x(),plotRect().left(),plotRect().right()),std::clamp(point.y(),plotRect().top(),plotRect().bottom()));
    if(QLineF(g.start,point).length()<=5 && !g.changed) return;
    g.changed=true;
    const auto start=fromPixel(g.start,g.view),current=fromPixel(point,g.view);
    if(g.tool==Tool::Mark) {
        auto* m=findMark(*f,g.markId); if(!m) { cancelGesture();return; }
        const auto bounds=fullRange(f->metadata);
        const long double dt=current.sample-start.sample; const double df=current.frequency-start.frequency;
        auto next=g.mark;
        if(g.edges==Move) {
            const auto width=g.mark.time.end-g.mark.time.begin;
            next.time.begin=offsetSample(g.mark.time.begin,dt,f->metadata.sampleCount-width);
            next.time.end=next.time.begin+width;
            const double bandwidth=g.mark.frequency.upperHz-g.mark.frequency.lowerHz;
            next.frequency.lowerHz=std::clamp(g.mark.frequency.lowerHz+df,bounds.frequency.lowerHz,bounds.frequency.upperHz-bandwidth);
            next.frequency.upperHz=std::min(bounds.frequency.upperHz,next.frequency.lowerHz+bandwidth);
        } else {
            const bool waterfall=f->display.mainMode==MainMode::Waterfall;
            const SampleIndex minT=std::min<SampleIndex>(f->display.stftSize,g.mark.time.end-g.mark.time.begin);
            const double minF=std::min(f->metadata.sampleRateHz/f->display.stftSize,g.mark.frequency.upperHz-g.mark.frequency.lowerHz);
            if(g.edges&(waterfall?Top:Left)) next.time.begin=offsetSample(g.mark.time.begin,dt,g.mark.time.end-minT);
            if(g.edges&(waterfall?Bottom:Right)) next.time.end=std::max(g.mark.time.begin+minT,offsetSample(g.mark.time.end,dt,f->metadata.sampleCount));
            if(g.edges&(waterfall?Left:Bottom)) next.frequency.lowerHz=std::clamp(g.mark.frequency.lowerHz+df,bounds.frequency.lowerHz,g.mark.frequency.upperHz-minF);
            if(g.edges&(waterfall?Right:Top)) next.frequency.upperHz=std::clamp(g.mark.frequency.upperHz+df,g.mark.frequency.lowerHz+minF,bounds.frequency.upperHz);
        }
        m->range=next; setCursor(g.edges==Move?Qt::ClosedHandCursor:edgeCursor(g.edges));
    } else if(g.tool==Tool::AuxiliaryY) {
        const double span=g.auxiliaryMax-g.auxiliaryMin;
        const bool psd=f->display.auxiliaryMode==AuxiliaryMode::Psd;
        const double low=psd?-180:-4,high=psd?50:4;
        f->display.auxiliaryMin=std::clamp(g.auxiliaryMin+(point.y()-g.start.y())/plotRect().height()*span,low,high-span);
        f->display.auxiliaryMax=f->display.auxiliaryMin+span;emit stateChanged();
    } else if(g.tool==Tool::PanX||g.tool==Tool::PanY||g.tool==Tool::Navigate) {
        ViewRange next=g.view;
        const bool freqAxis=kind_==Kind::Main ? (f->display.mainMode==MainMode::Waterfall ? g.tool==Tool::PanX : g.tool==Tool::PanY) :
            kind_==Kind::Auxiliary&&f->display.auxiliaryMode==AuxiliaryMode::Psd;
        if(g.tool==Tool::Navigate) {
            const auto full=fullRange(f->metadata); const auto absolute=fromPixel(point,full);
            const auto width=g.view.time.end-g.view.time.begin;
            const auto first=sampleIndex(absolute.sample-width/2,f->metadata.sampleCount-width);
            next.time={first,first+width};
        } else if(freqAxis) {
            double delta;
            if(kind_==Kind::Auxiliary) delta=-(point.x()-g.start.x())/plotRect().width()*(g.view.frequency.upperHz-g.view.frequency.lowerHz);
            else delta=start.frequency-current.frequency;
            next.frequency.lowerHz+=delta;next.frequency.upperHz+=delta;
        } else {
            const auto width=g.view.time.end-g.view.time.begin;
            const auto first=offsetSample(g.view.time.begin,start.sample-current.sample,f->metadata.sampleCount-width);
            next.time={first,first+width};
        }
        session_.setView(next,false); emit stateChanged(); setCursor(Qt::ClosedHandCursor);
    }
    repaintChart();
}

void PlotWidget::finishGesture() {
    if(!gesture_) return;
    const auto g=*gesture_; gesture_.reset(); releasing_=true; if(mouseGrabber()==this) releaseMouse(); releasing_=false;
    auto* f=session_.activeFile(); if(!f||f->metadata.id!=g.fileId) return;
    if(g.tool==Tool::Box&&g.changed&&std::abs(g.current.x()-g.start.x())>=8&&std::abs(g.current.y()-g.start.y())>=8) {
        const auto a=fromPixel(g.start,g.view),b=fromPixel(g.current,g.view);
        ViewRange range{{offsetSample(g.view.time.begin,std::min(a.sample,b.sample),f->metadata.sampleCount),offsetSample(g.view.time.begin,std::max(a.sample,b.sample),f->metadata.sampleCount)},
            {std::min(a.frequency,b.frequency),std::max(a.frequency,b.frequency)}};
        if(creating_) { session_.addMark(range); emit statusMessage("信号标记已保存 · 持续选择保持开启"); }
        else session_.setView(range);
    } else if(g.tool==Tool::Navigate&&!g.changed) {
        const auto absolute=fromPixel(g.start,fullRange(f->metadata)); const auto width=g.view.time.end-g.view.time.begin;
        const auto first=sampleIndex(absolute.sample-width/2,f->metadata.sampleCount-width);
        auto next=g.view;next.time={first,first+width};session_.setView(next);
    } else if(g.tool!=Tool::Mark&&g.tool!=Tool::Box&&g.tool!=Tool::AuxiliaryY&&g.changed) {
        const auto final=f->view;f->view=g.view;session_.setView(final);
    } else if(g.tool==Tool::Box&&!g.changed&&!creating_) session_.selectMarks({});
    emit stateChanged();repaintChart();
}

void PlotWidget::mouseReleaseEvent(QMouseEvent* e) { if(e->button()==Qt::LeftButton) { mouseMoveEvent(e);finishGesture(); } }
void PlotWidget::mouseDoubleClickEvent(QMouseEvent*) { cancelGesture(); session_.resetView();emit stateChanged(); }

void PlotWidget::wheelEvent(QWheelEvent* e) {
    auto* f=session_.activeFile(); if(!f) return; if(gesture_) cancelGesture();
    const bool yAxis=e->position().x()<plotRect().left();
    const double delta=e->pixelDelta().isNull()?e->angleDelta().y()/3.0:e->pixelDelta().y();
    const double factor=std::exp(std::clamp(-delta*.005,-2.0,2.0));
    if(kind_==Kind::Auxiliary&&yAxis) {
        finishWheel();const bool psd=f->display.auxiliaryMode==AuxiliaryMode::Psd;
        const double low=psd?-180:-4,high=psd?50:4;
        const double ratio=std::clamp((plotRect().bottom()-e->position().y())/plotRect().height(),0.0,1.0);
        const double pivot=f->display.auxiliaryMin+(f->display.auxiliaryMax-f->display.auxiliaryMin)*ratio;
        const double span=std::clamp((f->display.auxiliaryMax-f->display.auxiliaryMin)*factor,psd?2:.02,high-low);
        f->display.auxiliaryMin=std::clamp(pivot-span*ratio,low,high-span);f->display.auxiliaryMax=f->display.auxiliaryMin+span;
        emit stateChanged();e->accept();return;
    }
    if(wheelBase_&&(wheelFile_!=f->metadata.id||wheelY_!=yAxis)) finishWheel();
    if(!wheelBase_) { wheelBase_=f->view;wheelFile_=f->metadata.id;wheelY_=yAxis; }
    const bool frequency=kind_==Kind::Main ? (f->display.mainMode==MainMode::Waterfall?!yAxis:yAxis) : kind_==Kind::Auxiliary&&f->display.auxiliaryMode==AuxiliaryMode::Psd;
    auto next=f->view;
    if(frequency) {
        const double ratio=kind_==Kind::Auxiliary||f->display.mainMode==MainMode::Waterfall?
            std::clamp((e->position().x()-plotRect().left())/plotRect().width(),0.0,1.0):std::clamp((plotRect().bottom()-e->position().y())/plotRect().height(),0.0,1.0);
        const double pivot=next.frequency.lowerHz+(next.frequency.upperHz-next.frequency.lowerHz)*ratio;
        next.frequency.lowerHz=pivot-(pivot-next.frequency.lowerHz)*factor;next.frequency.upperHz=pivot+(next.frequency.upperHz-pivot)*factor;
    } else {
        const auto c=fromPixel(e->position(),next);const auto width=sampleIndex(static_cast<long double>(next.time.end-next.time.begin)*factor,f->metadata.sampleCount);
        const auto first=offsetSample(next.time.begin,c.sample*(1-factor),f->metadata.sampleCount-width);
        next.time={first,first+width};
    }
    session_.setView(next,false);wheelTimer_.start();emit stateChanged();e->accept();
}

void PlotWidget::contextMenuEvent(QContextMenuEvent* e) {
    cancelGesture(); if(!session_.activeFile()) return;
    QMenu menu(this);
    if(kind_==Kind::Main) {
        auto* toggle=menu.addAction("选择信号（持续模式）");toggle->setCheckable(true);toggle->setChecked(creating_);
        connect(toggle,&QAction::triggered,this,[this](bool checked){setCreating(checked);});
        menu.addSeparator();
        const auto* f=session_.activeFile();
        for(auto it=f->marks.rbegin();it!=f->marks.rend();++it) if(markRect(it->range).contains(e->pos())) {
            const auto id=it->id;auto* choice=menu.addAction("选中 "+QString::fromStdString(it->name));
            connect(choice,&QAction::triggered,this,[this,id]{emit markSelectionRequested(QString::fromStdString(id),Qt::NoModifier);});
        }
        auto* focus=menu.addAction("定位活动标记");focus->setEnabled(!f->activeMarkId.empty());
        connect(focus,&QAction::triggered,this,[this]{if(auto* file=session_.activeFile())session_.focusMark(file->activeMarkId);emit stateChanged();});
    }
    menu.addAction("显示完整文件",this,[this]{if(auto* f=session_.activeFile())session_.setView(fullRange(f->metadata));emit stateChanged();});
    menu.addAction("恢复默认视图",this,[this]{session_.resetView();emit stateChanged();});
    menu.exec(e->globalPos());
}

void PlotWidget::keyPressEvent(QKeyEvent* e) {
    if(e->key()==Qt::Key_Escape) { cancelGesture(true);emit statusMessage("已取消拖动并退出持续选择");e->accept(); }
    else QWidget::keyPressEvent(e);
}
void PlotWidget::resizeEvent(QResizeEvent* e) { if(gesture_) cancelGesture();if(surface_)surface_->setGeometry(rect());QWidget::resizeEvent(e);repaintChart(); }
bool PlotWidget::event(QEvent* e) {
    if((e->type()==QEvent::UngrabMouse&&!releasing_)||e->type()==QEvent::WindowDeactivate||e->type()==QEvent::Hide) cancelGesture(e->type()==QEvent::WindowDeactivate);
    return QWidget::event(e);
}
} // namespace signalstudio
