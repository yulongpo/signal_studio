#include "infrastructure/source_loader.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QtEndian>
#include <cmath>
#include "infrastructure/raw_sample_reader.h"

namespace signalstudio {
QString iqSourceFingerprint(const QString& path) {
    const QFileInfo info(path);
    return QString::number(info.size()) + "/" + QString::number(info.lastModified().toMSecsSinceEpoch());
}
SourceLoader::SourceLoader(QString path, SampleIndex target) : SourceLoader(path,target,SampleFormat{}) {}
SourceLoader::SourceLoader(QString path, SampleIndex target,SampleFormat format) : worker_([this,path,target,format]{run(path,target,format);}) {}
SourceLoader::~SourceLoader(){stop();if(worker_.joinable())worker_.join();}
SourceLoadSnapshot SourceLoader::snapshot() const {std::lock_guard lock(mutex_);return snapshot_;}
void SourceLoader::run(QString path,SampleIndex target,SampleFormat format) {
    SourceLoadSnapshot state; state.target=target;state.fingerprint=iqSourceFingerprint(path);
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){state.error=file.errorString();state.finished=true;std::lock_guard lock(mutex_);snapshot_=state;return;}
    RawSampleReader reader;QString error;
    if(!reader.open(path,format,error)||target>reader.sampleCount()){state.error=error.isEmpty()?"目标前缀超出源":error;state.finished=true;std::lock_guard lock(mutex_);snapshot_=state;return;}
    SampleIndex bucketWidth=1;
    std::vector<EnvelopePoint> completed;EnvelopePoint current;
    const auto publish=[&]{state.envelope=completed;if(current.end>current.begin)state.envelope.push_back(current);std::lock_guard lock(mutex_);snapshot_=state;};
    publish();
    while(state.loaded<target&&!stop_) {
        const auto samples=std::min<SampleIndex>(std::max<std::uint64_t>(1,4*1024*1024/bytesPerFrame(format)),target-state.loaded);
        std::vector<std::complex<float>> decoded;
        // Only complete, successfully decoded blocks are published as available.
        if(!reader.read({state.loaded,state.loaded+samples},decoded,[this]{return stop_.load();},&error)){
            if(!stop_)state.error=error.isEmpty()?"样本解码失败":error;break;
        }
        for(SampleIndex n=0;n<samples;++n){
            const auto index=state.loaded+n;
            if(current.begin==current.end){current.begin=index;current.end=index;current.peak=0;current.peakSample=index;}
            const float magnitude=static_cast<float>(std::abs(decoded[static_cast<std::size_t>(n)])*adcScale(format));
            if(magnitude>=current.peak){current.peak=magnitude;current.peakSample=index;}current.end=index+1;
            if(current.end-current.begin==bucketWidth){
                completed.push_back(current);current={};
                if(completed.size()==2048){
                    std::vector<EnvelopePoint> merged;merged.reserve(1024);
                    for(std::size_t k=0;k<completed.size();k+=2){auto point=completed[k];point.end=completed[k+1].end;if(completed[k+1].peak>point.peak){point.peak=completed[k+1].peak;point.peakSample=completed[k+1].peakSample;}merged.push_back(point);}
                    completed=std::move(merged);bucketWidth*=2;
                }
            }
        }
        state.loaded+=samples;publish();
    }
    if(iqSourceFingerprint(path)!=state.fingerprint)state.error="源文件在读入过程中发生变化";
    state.finished=true;publish();
}
}
