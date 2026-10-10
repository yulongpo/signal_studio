#include "infrastructure/import_controller.h"
#include "infrastructure/raw_sample_reader.h"
#include <QFileInfo>
#include <algorithm>

namespace signalstudio {
namespace {
QString sourcePath(const FileMetadata& metadata){const QFileInfo info(QString::fromStdString(metadata.path));return info.canonicalFilePath().isEmpty()?info.absoluteFilePath():info.canonicalFilePath();}
bool sameSource(const FileMetadata& a,const FileMetadata& b){return sourcePath(a)==sourcePath(b)&&a.sampleFormat==b.sampleFormat&&a.sampleRateHz==b.sampleRateHz&&a.centerFrequencyHz==b.centerFrequencyHz&&a.declaredBandwidthHz==b.declaredBandwidthHz;}
}
bool ImportController::add(ImportRow row,QString& error){
    if(rows_.size()>=256){error="队列最多256个文件";return false;}
    for(const auto& existing:rows_)if(sameSource(existing.metadata,row.metadata)){error="同路径与解释参数已在队列中";return false;}
    rows_.push_back(std::move(row));error.clear();return true;
}
bool ImportController::start(const std::vector<std::size_t>& indices){
    if(running())return false;lastRun_=indices;pending_.clear();
    for(auto i:indices)if(i<rows_.size()){
        auto& row=rows_[i];if(row.status==ImportStatus::Ready)continue;
        FileMetadata checked;QString error;
        if(row.committed)continue;
        if(!row.metadataError.isEmpty())error=row.metadataError;
        else if(!row.confirmed)error="请确认采样结构、编码及缺失的采集参数";
        else if(std::any_of(rows_.begin(),rows_.begin()+i,[&](const auto& other){return sameSource(other.metadata,row.metadata);} ))error="解释参数修改后与已有队列项重复";
        else describeRawFile(QString::fromStdString(row.metadata.path),row.metadata.sampleFormat,row.metadata.sampleRateHz,row.metadata.centerFrequencyHz,row.metadata.declaredBandwidthHz,checked,error);
        row.error=error;row.load={};
        if(!error.isEmpty()){row.status=ImportStatus::Failed;continue;}
        row.metadata=std::move(checked);row.status=ImportStatus::Pending;pending_.push_back(i);
    }
    next();return running();
}
void ImportController::next(){
    if(loader_||pending_.empty())return;active_=pending_.front();pending_.erase(pending_.begin());
    auto& row=rows_[active_];row.status=ImportStatus::Reading;
    loader_=std::make_unique<SourceLoader>(QString::fromStdString(row.metadata.path),row.metadata.sampleCount,row.metadata.sampleFormat);
}
void ImportController::poll(){
    if(!loader_)return;auto& row=rows_[active_];row.load=loader_->snapshot();if(!row.load.finished)return;
    row.error=row.load.error;row.metadata.availability.fingerprint=row.load.fingerprint.toStdString();
    row.metadata.availability.availableSamples=row.load.loaded;
    row.metadata.availability.status=!row.error.isEmpty()?LoadStatus::Failed:row.load.loaded==row.metadata.sampleCount?LoadStatus::Ready:LoadStatus::Partial;
    row.status=!row.error.isEmpty()?ImportStatus::Failed:row.load.loaded==row.metadata.sampleCount?ImportStatus::Ready:row.load.loaded?ImportStatus::Partial:ImportStatus::Cancelled;
    loader_.reset();active_=invalid;next();
}
void ImportController::stopAll(){for(auto i:pending_)rows_[i].status=ImportStatus::Cancelled;pending_.clear();if(loader_)loader_->stop();}
void ImportController::stopRow(std::size_t i){if(i==active_&&loader_)loader_->stop();else {pending_.erase(std::remove(pending_.begin(),pending_.end(),i),pending_.end());if(i<rows_.size()&&rows_[i].status==ImportStatus::Pending)rows_[i].status=ImportStatus::Cancelled;}}
bool ImportController::canCommit() const {return !running()&&std::any_of(rows_.begin(),rows_.end(),[](const auto& row){return !row.committed&&(row.status==ImportStatus::Ready||row.status==ImportStatus::Partial)&&row.load.loaded>0;});}
std::vector<FileState> ImportController::sources() const {std::vector<FileState> sources;
    for(const auto& row:rows_)if(!row.committed&&(row.status==ImportStatus::Ready||row.status==ImportStatus::Partial)&&row.load.loaded){FileState file;file.metadata=row.metadata;file.view=fullRange(file.metadata);file.navigationEnvelope=row.load.envelope;if(file.metadata.sampleFormat.structure==SampleStructure::Real)file.display.waveformMode=WaveformMode::I;sources.push_back(std::move(file));}return sources;
}
void ImportController::markCommitted(const FileMetadata& metadata){for(auto& row:rows_)if(sameSource(row.metadata,metadata))row.committed=true;}
void ImportController::restart(){if(running())return;for(auto i:lastRun_)if(i<rows_.size())rows_[i].status=ImportStatus::Pending;start(lastRun_);}
}
