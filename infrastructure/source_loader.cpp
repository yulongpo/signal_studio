#include "infrastructure/source_loader.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QtEndian>
#include <cmath>

namespace signalstudio {
QString iqSourceFingerprint(const QString& path) {
    const QFileInfo info(path);
    return QString::number(info.size()) + "/" + QString::number(info.lastModified().toMSecsSinceEpoch());
}
SourceLoader::SourceLoader(QString path, SampleIndex target) : worker_([this,path,target]{run(path,target);}) {}
SourceLoader::~SourceLoader(){stop();if(worker_.joinable())worker_.join();}
SourceLoadSnapshot SourceLoader::snapshot() const {std::lock_guard lock(mutex_);return snapshot_;}
void SourceLoader::run(QString path,SampleIndex target) {
    SourceLoadSnapshot state; state.target=target;state.fingerprint=iqSourceFingerprint(path);
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){state.error=file.errorString();state.finished=true;std::lock_guard lock(mutex_);snapshot_=state;return;}
    SampleIndex bucketWidth=1;
    std::vector<EnvelopePoint> completed;EnvelopePoint current;
    const auto publish=[&]{state.envelope=completed;if(current.end>current.begin)state.envelope.push_back(current);std::lock_guard lock(mutex_);snapshot_=state;};
    publish();
    while(state.loaded<target&&!stop_) {
        const auto samples=std::min<SampleIndex>(1'048'576,target-state.loaded);
        const auto bytes=file.read(static_cast<qint64>(samples*4));
        if(bytes.size()!=static_cast<qint64>(samples*4)){state.error="IQ 文件读取失败或长度已改变";break;}
        for(SampleIndex n=0;n<samples;++n){
            const auto index=state.loaded+n;
            if(current.begin==current.end){current.begin=index;current.end=index;current.peak=0;current.peakSample=index;}
            const auto* raw=reinterpret_cast<const uchar*>(bytes.constData()+n*4);
            const double i=qFromLittleEndian<qint16>(raw),q=qFromLittleEndian<qint16>(raw+2);
            const float magnitude=static_cast<float>(std::hypot(i,q));
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
