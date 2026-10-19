#include "infrastructure/raw_sample_reader.h"
#include <QtEndian>
#include <bit>
#include <cmath>
#include <limits>
#include <QFileInfo>
#include "infrastructure/source_loader.h"

namespace signalstudio {
bool describeRawFile(const QString& path,const SampleFormat& format,double fs,double fc,double bandwidth,FileMetadata& target,QString& error){
    QFileInfo info(path);std::string message;std::uint64_t count=0;
    if(!info.isFile()||!info.isReadable()){error="数据文件不存在或不可读";return false;}
    if(!sampleCountForBytes(format,static_cast<std::uint64_t>(info.size()),count,message)){error=QString::fromStdString(message);return false;}
    const double maximum=fs/(format.structure==SampleStructure::Real?2:1);
    if(!std::isfinite(fs)||fs<=0||fs>1e12||!std::isfinite(static_cast<double>(count)/fs)||!std::isfinite(fc)||fc<0||fc>1e12||!std::isfinite(bandwidth)||bandwidth<0||bandwidth>maximum){error="采样率、频率、时长或有效带宽无效";return false;}
    FileMetadata m;m.name=info.fileName().toUtf8().toStdString();m.path=info.canonicalFilePath().toUtf8().toStdString();
    m.sampleFormat=format;m.sampleRateHz=fs;m.centerFrequencyHz=fc;m.sampleCount=count;m.demo=false;
    m.declaredBandwidthHz=bandwidth;m.effectiveBandwidthHz=bandwidth>0?bandwidth:maximum;
    m.availability.fingerprint=iqSourceFingerprint(path).toStdString();target=std::move(m);error.clear();return true;
}
RawSampleReader::~RawSampleReader(){if(mapped_)file_.unmap(mapped_);}
bool RawSampleReader::open(const QString& path,const SampleFormat& format,QString& error){
    if(mapped_)file_.unmap(mapped_);mapped_=nullptr;file_.close();count_=0;
    file_.setFileName(path);
    if(!file_.open(QIODevice::ReadOnly)){error=file_.errorString();return false;}
    std::string message;
    if(!sampleCountForBytes(format,static_cast<std::uint64_t>(file_.size()),count_,message)){error=QString::fromStdString(message);file_.close();return false;}
    mapped_=file_.map(0,file_.size());
    if(!mapped_){error="无法建立只读样本映射："+file_.errorString();file_.close();count_=0;return false;}
    format_=format;error.clear();return true;
}
double RawSampleReader::component(std::uint64_t componentIndex) const {
    const auto* p=mapped_+format_.headerBytes+componentIndex*bytesPerComponent(format_);
    return decode(p);
}
double RawSampleReader::decode(const uchar* p) const {
    const bool le=format_.byteOrder!=ByteOrder::Big;
    const auto u16=[&]{return le?qFromLittleEndian<quint16>(p):qFromBigEndian<quint16>(p);};
    const auto u32=[&]{return le?qFromLittleEndian<quint32>(p):qFromBigEndian<quint32>(p);};
    double value=0;
    switch(format_.componentEncoding){
    case ComponentEncoding::Int8:value=std::bit_cast<qint8>(*p);break;
    case ComponentEncoding::UInt8:value=static_cast<double>(*p)-128;break;
    case ComponentEncoding::Int16:value=std::bit_cast<qint16>(u16());break;
    case ComponentEncoding::Int32:value=std::bit_cast<qint32>(u32());break;
    case ComponentEncoding::Float32:value=std::bit_cast<float>(u32());break;
    case ComponentEncoding::Float64:value=std::bit_cast<double>(le?qFromLittleEndian<quint64>(p):qFromBigEndian<quint64>(p));break;
    }
    return value/adcScale(format_);
}
bool RawSampleReader::sampleAt(std::uint64_t n,std::complex<double>& output,QString* error) const {
    if(!mapped_||n>=count_){if(error)*error="样本索引越界";return false;}
    const auto c=format_.selectedChannel, channels=format_.channels;
    const bool complex=format_.structure==SampleStructure::ComplexIQ;
    std::uint64_t a=0,b=0;
    // PlanarIQ+TimeInterleaved: all I frames, then all Q frames;
    // PlanarIQ+ChannelPlanar: each channel's I region followed by its Q region.
    if(complex&&format_.iqLayout==IQLayout::PlanarIQ){
        if(format_.channelLayout==ChannelLayout::TimeInterleaved){a=n*channels+c;b=count_*channels+a;}
        else {a=(static_cast<std::uint64_t>(c)*2*count_)+n;b=a+count_;}
    } else {
        const auto time=format_.channelLayout==ChannelLayout::TimeInterleaved?n*channels+c:static_cast<std::uint64_t>(c)*count_+n;
        a=time*(complex?2:1);b=a+1;
        if(complex&&format_.iqLayout==IQLayout::QIInterleaved)std::swap(a,b);
    }
    const double i=component(a),q=complex?component(b):0;
    if(!std::isfinite(i)||!std::isfinite(q)||std::abs(i)>std::numeric_limits<float>::max()||std::abs(q)>std::numeric_limits<float>::max()){
        if(error)*error="样本包含非有限值或超出分析浮点范围";return false;
    }
    output={i,q};return true;
}
bool RawSampleReader::read(TimeRange range,std::vector<std::complex<float>>& output,const std::function<bool()>& cancel,QString* error) const {
    if(range.end>count_||range.begin>=range.end||range.end-range.begin>4'194'304){if(error)*error="读取范围越界或超过 32MiB 解码预算";return false;}
    const auto n=range.end-range.begin,bytes=bytesPerComponent(format_);
    const bool complex=format_.structure==SampleStructure::ComplexIQ,planar=complex&&format_.iqLayout==IQLayout::PlanarIQ;
    const auto stride=format_.channelLayout==ChannelLayout::TimeInterleaved?format_.channels:1;
    const auto frame=bytes*stride*(complex&&!planar?2:1),length=n*frame;
    if(length>32*1024*1024/(planar?2:1)){if(error)*error="读取磁盘片段超过 32MiB 预算";return false;}
    QFile stream(file_.fileName());
    if(!stream.open(QIODevice::ReadOnly)){if(error)*error=stream.errorString();return false;}
    const auto base=format_.channelLayout==ChannelLayout::TimeInterleaved?0:count_*format_.selectedChannel*bytes*(complex?2:1);
    const auto start=format_.headerBytes+base+range.begin*frame;
    if(cancel&&cancel())return false;
    if(!stream.seek(static_cast<qint64>(start))){if(error)*error="样本定位失败";return false;}
    const auto a=stream.read(static_cast<qint64>(length));QByteArray b;
    if(planar){
        const auto plane=count_*bytes*(format_.channelLayout==ChannelLayout::TimeInterleaved?format_.channels:1);
        if(!stream.seek(static_cast<qint64>(start+plane))){if(error)*error="Q 分块定位失败";return false;}
        b=stream.read(static_cast<qint64>(length));
    }
    if(a.size()!=static_cast<qint64>(length)||(planar&&b.size()!=static_cast<qint64>(length))){if(error)*error="源文件片段读取不完整";return false;}
    output.resize(static_cast<std::size_t>(n));
    const auto channelOffset=format_.channelLayout==ChannelLayout::TimeInterleaved?format_.selectedChannel*bytes*(complex&&!planar?2:1):0;
    for(std::size_t n=0;n<output.size();++n){
        if((n&1023)==0&&cancel&&cancel())return false;
        const auto* p=reinterpret_cast<const uchar*>(a.constData())+n*frame+channelOffset;
        double i=decode(p),q=complex?decode(planar?reinterpret_cast<const uchar*>(b.constData())+n*frame+channelOffset:p+bytes):0;
        if(complex&&format_.iqLayout==IQLayout::QIInterleaved)std::swap(i,q);
        if(!std::isfinite(i)||!std::isfinite(q)||std::abs(i)>std::numeric_limits<float>::max()||std::abs(q)>std::numeric_limits<float>::max()){
            if(error)*error="样本包含非有限值或超出分析浮点范围";return false;
        }
        output[n]={static_cast<float>(i),static_cast<float>(q)};
    }
    return true;
}
}
