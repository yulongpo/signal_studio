#include "ui/import/source_preview_widget.h"
#include "infrastructure/int16_iq_file.h"
#include "ui/controls/adaptive_value_edit.h"
#include <QPainter>
#include <QPainterPath>
#include <QMetaObject>
#include <array>

namespace signalstudio {
SourcePreviewWidget::SourcePreviewWidget(QWidget* parent):QWidget(parent),worker_([this]{run();}){setObjectName("sourcePreview");setMinimumSize(425,610);}
SourcePreviewWidget::~SourcePreviewWidget(){stop_=true;++generation_;condition_.notify_one();worker_.join();}
void SourcePreviewWidget::clear(){++generation_;{std::lock_guard lock(mutex_);pending_.reset();}data_.reset();error_.clear();status_="未选择数据文件";update();}
void SourcePreviewWidget::preview(FileMetadata metadata,SampleIndex start){
    const auto generation=++generation_;data_.reset();error_.clear();status_="正在读取本地样本…";
    {std::lock_guard lock(mutex_);pending_=Request{std::move(metadata),start,generation};}condition_.notify_one();update();
}
void SourcePreviewWidget::run(){for(;;){Request request;
    {std::unique_lock lock(mutex_);condition_.wait(lock,[this]{return stop_||pending_.has_value();});if(stop_)return;request=std::move(*pending_);pending_.reset();}
    const auto cancelled=[this,g=request.generation]{return stop_||generation_!=g;};
    Data data;data.metadata=request.metadata;QString error;Int16IqFile reader;
    const auto total=availableSamples(data.metadata);
    const auto count=std::min<SampleIndex>(16384,std::max<std::uint64_t>(1,4*1024*1024/bytesPerFrame(data.metadata.sampleFormat)));
    data.begin=total?std::min(request.start,total-1):0;const TimeRange range{data.begin,std::min(total,data.begin+std::min(count,total-data.begin))};
    if(!reader.open(data.metadata,error)){}
    else if(!reader.spectralSource().read(range,data.samples,cancelled))error="读取预览片段失败";
    else {auto source=reader.spectralSource();source.sampleRateHz=data.metadata.sampleRateHz;
        const auto frequency=data.metadata.sampleFormat.structure==SampleStructure::Real?FrequencyRange{0,source.sampleRateHz/2}:FrequencyRange{-source.sampleRateHz/2,source.sampleRateHz/2};
        std::string message;PsdSettings psd;data.psd=analyzeSpectrum(source,range,frequency,1024,psd,message,cancelled);
        if(!data.psd)error=QString::fromStdString(message);
        else {SpectrogramSettings stft;data.stft=analyzeSpectrogram(source,range,frequency,256,12,stft,1,cancelled);if(!data.stft||data.stft->frames.empty())error="时频预览失败";}
    }
    if(cancelled())continue;
    QMetaObject::invokeMethod(this,[this,g=request.generation,data=std::move(data),error]() mutable {
        if(stop_||generation_!=g)return;error_=error;
        if(error.isEmpty()){data_=std::move(data);status_=QString("本地文件采样 · [%1, %2) · %3").arg(data_->begin).arg(data_->begin+data_->samples.size()).arg(QString::fromStdString(formatId(data_->metadata.sampleFormat)));}
        else status_="预览失败："+error;update();emit previewFinished();
    },Qt::QueuedConnection);
}}
void SourcePreviewWidget::paintEvent(QPaintEvent*){
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#0e1b2e"));
    p.setFont(QFont("Microsoft YaHei UI",9));p.setPen(QColor("#cbe1f0"));p.drawText(QRectF(0,0,width(),25),"信号预览 / Preview");
    p.setPen(QColor("#61d6ee"));p.drawText(QRectF(0,27,width(),23),Qt::AlignRight,status_);
    const int w=width(),half=(w-12)/2;const QRectF psd(0,57,w,230),wave(0,299,half,174),scatter(half+12,299,w-half-12,174),stft(0,485,w,143);
    const bool real=data_&&data_->metadata.sampleFormat.structure==SampleStructure::Real;
    auto card=[&](QRectF card,const QString& title,const QString& detail){p.setPen(QColor("#2b4a61"));p.setBrush(QColor("#0b1929"));p.drawRoundedRect(card.adjusted(1,1,-1,-1),8,8);p.setPen(QColor("#c5e3f0"));p.drawText(card.adjusted(12,8,-8,-card.height()+29),title);p.setPen(QColor("#789bb5"));p.drawText(card.adjusted(12,8,-10,-card.height()+29),Qt::AlignRight,detail);return card.adjusted(38,39,-15,-28);};
    auto ps=card(psd,"功率谱 · PSD","Welch · Hann · 1024"),wa=card(wave,real?"实数波形":"I / Q 波形","原始采样"),sc=card(scatter,real?"幅值直方图":"IQ 散点",real?"Real":"I / Q"),st=card(stft,"短时时频图",real?"实数单边谱":"预览最多 12 帧");
    auto grid=[&](QRectF r){p.setPen(QColor("#233d52"));for(int j=0;j<=6;++j)p.drawLine(QPointF(r.left()+r.width()*j/6,r.top()),QPointF(r.left()+r.width()*j/6,r.bottom()));for(int j=0;j<=3;++j)p.drawLine(QPointF(r.left(),r.top()+r.height()*j/3),QPointF(r.right(),r.top()+r.height()*j/3));};grid(ps);grid(wa);grid(sc);
    if(!data_)return;
    const auto& d=*data_;const auto powers=spectrumDb(*d.psd);const float peak=*std::max_element(powers.begin(),powers.end()),low=peak-80;
    auto line=[&](const std::vector<float>& values,QRectF r,double lo,double hi,QColor color){if(values.empty())return;p.save();p.setClipRect(r);QPainterPath path;for(std::size_t n=0;n<values.size();++n){QPointF point(r.left()+r.width()*n/std::max<std::size_t>(1,values.size()-1),r.bottom()-r.height()*(values[n]-lo)/std::max(1e-12,hi-lo));if(n)path.lineTo(point);else path.moveTo(point);}p.setPen(QPen(color,1.2));p.drawPath(path);p.restore();};line(powers,ps,low,peak+3,QColor("#53deea"));
    p.setPen(QColor("#789bb5"));for(int j=0;j<=3;++j)p.drawText(QRectF(psd.left()+3,ps.top()+ps.height()*j/3-7,33,15),QString::number(peak+3-(83.*j/3),'f',0));
    const auto& f=d.metadata;const double fs=f.sampleRateHz;
    p.drawText(psd.adjusted(12,psd.height()-24,-12,-3),Qt::AlignLeft,formatAdaptiveValue(real?0:-fs/2,UnitKind::Frequency));
    p.drawText(psd.adjusted(12,psd.height()-24,-12,-3),Qt::AlignRight,formatAdaptiveValue(fs/2,UnitKind::Frequency));
    p.drawText(psd.adjusted(12,psd.height()-24,-12,-3),Qt::AlignCenter,real?"实数单边谱":QString("RF %1").arg(formatAdaptiveValue(f.centerFrequencyHz,UnitKind::Frequency)));
    std::vector<float> i,q;const auto count=std::min<std::size_t>(256,d.samples.size());double amplitude=1e-9;
    for(const auto& z:d.samples)amplitude=std::max(amplitude,static_cast<double>(std::max(std::abs(z.real()),std::abs(z.imag()))));
    for(std::size_t n=0;n<count;++n){i.push_back(d.samples[n].real());q.push_back(d.samples[n].imag());}line(i,wa,-amplitude,amplitude,QColor("#6ae2ed"));if(!real)line(q,wa,-amplitude,amplitude,QColor("#f4bd71"));
    p.save();p.setClipRect(sc);p.setPen(QColor("#4eb3cb"));
    if(real){std::array<int,32> bins{};for(const auto& z:d.samples){const auto k=std::clamp(static_cast<int>((z.real()/amplitude+1)*16),0,31);++bins[k];}const auto max=*std::max_element(bins.begin(),bins.end());p.setBrush(QColor("#349cb5"));for(int k=0;k<32;++k){const auto height=sc.height()*bins[k]/std::max(1,max);p.drawRect(QRectF(sc.left()+sc.width()*k/32,sc.bottom()-height,sc.width()/32-1,height));}}
    else for(std::size_t n=0;n<std::min<std::size_t>(4096,d.samples.size());++n)p.drawPoint(QPointF(sc.center().x()+d.samples[n].real()/amplitude*sc.width()/2,sc.center().y()-d.samples[n].imag()/amplitude*sc.height()/2));p.restore();
    if(d.stft){const auto values=spectralRaster(*d.stft,256,64,MainMode::Waterfall);if(!values.empty()){QImage image(256,64,QImage::Format_RGB32);for(int y=0;y<64;++y)for(int x=0;x<256;++x){const double t=std::clamp((values[y*256+x]-low)/83.,0.,1.);image.setPixelColor(x,y,QColor::fromHsvF(.69-.57*t,.75,.2+.8*t));}p.drawImage(st,image);}}
}
}
