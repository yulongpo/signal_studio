#include "application/session.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {

constexpr std::size_t maximumHistory = 40;

// Integer ratio calculation keeps the whole uint64 range exact and avoids overflow.
SampleIndex ratio(SampleIndex value, SampleIndex numerator, SampleIndex denominator) {
    return value / denominator * numerator + value % denominator * numerator / denominator;
}

ViewRange defaultView(const FileMetadata& metadata, int stftSize, int psdSize) {
    auto view = fullRange(metadata);
    view.time = {ratio(metadata.sampleCount, 7, 20), ratio(metadata.sampleCount, 11, 20)};
    return clampRange(view, metadata, stftSize, psdSize);
}

void pushBounded(std::vector<ViewSnapshot>& list, const ViewSnapshot& view) {
    if (list.size() == maximumHistory) list.erase(list.begin());
    list.push_back(view);
}

void saveActiveAuxiliary(DisplaySettings& display) {
    if(display.auxiliaryMode==AuxiliaryMode::Waveform) {
        display.waveformMin=display.auxiliaryMin;display.waveformMax=display.auxiliaryMax;
    } else {
        display.psdMin=display.auxiliaryMin;display.psdMax=display.auxiliaryMax;
    }
}

void loadActiveAuxiliary(DisplaySettings& display) {
    display.auxiliaryMin=display.auxiliaryMode==AuxiliaryMode::Waveform?display.waveformMin:display.psdMin;
    display.auxiliaryMax=display.auxiliaryMode==AuxiliaryMode::Waveform?display.waveformMax:display.psdMax;
}

ViewSnapshot snapshotFor(const FileState& file) {
    const auto& display=file.display;
    auto result=ViewSnapshot{file.metadata.id,file.view,display.waveformMin,display.waveformMax,display.psdMin,display.psdMax};
    if(display.auxiliaryMode==AuxiliaryMode::Waveform) {
        result.waveformMin=display.auxiliaryMin;result.waveformMax=display.auxiliaryMax;
    } else {result.psdMin=display.auxiliaryMin;result.psdMax=display.auxiliaryMax;}
    return result;
}

std::pair<double,double> clampAuxiliary(double minimum,double maximum,AuxiliaryMode mode) {
    if(minimum>maximum)std::swap(minimum,maximum);
    const double lower=mode==AuxiliaryMode::Waveform?-160:-180;
    const double upper=mode==AuxiliaryMode::Waveform?160:50;
    const double width=std::clamp(maximum-minimum,2.0,upper-lower);
    const double begin=std::clamp(minimum+(maximum-minimum)/2-width/2,lower,upper-width);
    return {begin,begin+width};
}

std::string numbered(const std::string& prefix,std::size_t number) {
    return prefix+(number<10?"0":"")+std::to_string(number);
}

} // namespace

Session::Session() {
    project_.name = "射频信号分析工程";
}

FileState* Session::activeFile() {
    const auto found = std::find_if(project_.files.begin(), project_.files.end(),
        [&](const FileState& file) { return file.metadata.id == project_.activeFileId; });
    return found == project_.files.end() ? nullptr : &*found;
}

const FileState* Session::activeFile() const {
    const auto found = std::find_if(project_.files.begin(), project_.files.end(),
        [&](const FileState& file) { return file.metadata.id == project_.activeFileId; });
    return found == project_.files.end() ? nullptr : &*found;
}

void Session::newProject() {
    project_ = Project{};
    histories_.clear();
    demoSequence_ = 0;
}

std::string Session::nextId(const std::string& prefix) {
    for (;;) {
        const auto candidate = prefix + std::to_string(nextIdentifier_++);
        bool exists = false;
        for (const auto& file : project_.files) {
            exists = exists || file.metadata.id == candidate || findMark(file, candidate);
            exists = exists || std::any_of(file.channels.begin(), file.channels.end(),
                [&](const Channel& channel) { return channel.id == candidate; });
        }
        if (!exists) return candidate;
    }
}

std::string Session::addDemoFile() {
    static const FileMetadata defaults[]{
        {"","wideband_100MHz.iq","",40e6,100e6,19'200'000'000,true,1},
        {"","capture_2450MHz.iq","",20e6,2450e6,4'800'000'000,true,3},
        {"","telemetry_915MHz.iq","",10e6,915e6,1'200'000'000,true,7}};
    return addDemoFile(defaults[demoSequence_%3]);
}

std::string Session::addDemoFile(FileMetadata metadata) {
    if(metadata.name.empty()||metadata.sampleCount==0||!std::isfinite(metadata.sampleRateHz)||
       metadata.sampleRateHz<=0||!std::isfinite(metadata.centerFrequencyHz))return {};
    if (metadata.effectiveBandwidthHz <= 0) metadata.effectiveBandwidthHz = metadata.sampleRateHz;
    if (metadata.effectiveBandwidthHz > metadata.sampleRateHz) return {};
    const auto bounds=fullRange(metadata);
    if(!std::isfinite(bounds.frequency.lowerHz)||!std::isfinite(bounds.frequency.upperHz)||
       bounds.frequency.lowerHz>=bounds.frequency.upperHz)return {};
    if(!metadata.id.empty()&&std::any_of(project_.files.begin(),project_.files.end(),
       [&](const FileState& file){return file.metadata.id==metadata.id;}))return {};
    if(metadata.id.empty())metadata.id=nextId("file-");
    if(metadata.demoSeed<=0)metadata.demoSeed=static_cast<int>(project_.files.size()+1);
    FileState file;
    file.metadata=std::move(metadata);
    if (!file.metadata.demo) {
        file.display.waveformMin = -80;
        file.display.waveformMax = 0;
        file.display.auxiliaryMin = -80;
        file.display.auxiliaryMax = 0;
    }
    file.view = defaultView(file.metadata, file.display.stftSize, file.display.psdSize);
    const auto id = file.metadata.id;
    project_.files.push_back(std::move(file));
    project_.activeFileId = id;
    ++demoSequence_;
    return id;
}

std::string Session::addDemoFile(const std::string& name,double sampleRateHz,
    double centerFrequencyHz,double durationSeconds,const std::string& path) {
    if(!std::isfinite(durationSeconds)||durationSeconds<=0||!std::isfinite(sampleRateHz)||sampleRateHz<=0)return {};
    const long double count=static_cast<long double>(sampleRateHz)*durationSeconds;
    if(count<1||count>=static_cast<long double>(std::numeric_limits<SampleIndex>::max()))return {};
    return addDemoFile({"",name,path,sampleRateHz,centerFrequencyHz,static_cast<SampleIndex>(count),true,
                        static_cast<int>(project_.files.size()+1)});
}

bool Session::activateFile(const std::string& id) {
    if (std::none_of(project_.files.begin(), project_.files.end(),
        [&](const FileState& file) { return file.metadata.id == id; })) return false;
    project_.activeFileId = id;
    return true;
}

bool Session::removeActiveFile() {
    const auto found = std::find_if(project_.files.begin(), project_.files.end(),
        [&](const FileState& file) { return file.metadata.id == project_.activeFileId; });
    if (found == project_.files.end()) return false;
    const auto index = static_cast<std::size_t>(found - project_.files.begin());
    histories_.erase(found->metadata.id);
    project_.files.erase(found);
    project_.activeFileId = project_.files.empty() ? std::string{} :
        project_.files[std::min(index, project_.files.size() - 1)].metadata.id;
    return true;
}

std::string Session::addMark(ViewRange range) {
    auto* file = activeFile();
    if (!file || !std::isfinite(range.frequency.lowerHz) || !std::isfinite(range.frequency.upperHz)) return {};
    const auto bounds = fullRange(file->metadata);
    if (range.time.begin > range.time.end) std::swap(range.time.begin, range.time.end);
    if (range.frequency.lowerHz > range.frequency.upperHz)
        std::swap(range.frequency.lowerHz, range.frequency.upperHz);
    range.time.begin = std::min(range.time.begin, bounds.time.end);
    range.time.end = std::min(range.time.end, bounds.time.end);
    range.frequency.lowerHz = std::clamp(range.frequency.lowerHz, bounds.frequency.lowerHz, bounds.frequency.upperHz);
    range.frequency.upperHz = std::clamp(range.frequency.upperHz, bounds.frequency.lowerHz, bounds.frequency.upperHz);
    if (range.time.begin >= range.time.end || range.frequency.lowerHz >= range.frequency.upperHz) return {};
    const auto id = nextId("mark-");
    file->marks.push_back({id, numbered("Region ",file->marks.size()+1), range});
    selectMarks({id}, id);
    return id;
}

void Session::selectMarks(std::vector<std::string> ids, std::string active) {
    auto* file = activeFile();
    if (!file) return;
    std::unordered_set<std::string> seen;
    file->selectedMarkIds.clear();
    for (auto& id : ids)
        if (findMark(*file, id) && seen.insert(id).second) file->selectedMarkIds.push_back(std::move(id));
    if (!active.empty() && seen.contains(active)) file->activeMarkId = std::move(active);
    else if (!seen.contains(file->activeMarkId))
        file->activeMarkId = file->selectedMarkIds.empty() ? std::string{} : file->selectedMarkIds.back();
    if (file->activeMarkId.empty()) file->display.psdFromSelection = false;
}

DeleteResult Session::deleteSelectedMarks() {
    auto* file = activeFile();
    if (!file) return {};
    std::unordered_set<std::string> selected(file->selectedMarkIds.begin(), file->selectedMarkIds.end());
    const auto marksBefore = file->marks.size();
    const auto channelsBefore = file->channels.size();
    std::erase_if(file->marks, [&](const Mark& mark) { return selected.contains(mark.id); });
    std::erase_if(file->channels, [&](const Channel& channel) { return selected.contains(channel.sourceMarkId); });
    file->selectedMarkIds.clear();
    file->activeMarkId.clear();
    file->display.psdFromSelection = false;
    return {static_cast<int>(marksBefore - file->marks.size()),
            static_cast<int>(channelsBefore - file->channels.size())};
}

bool Session::focusMark(const std::string& id) {
    auto* file = activeFile();
    if (!file) return false;
    const auto* mark = findMark(*file, id);
    if (!mark) return false;
    auto range = mark->range;
    const auto margin = std::max(SampleIndex{1}, (range.time.end - range.time.begin) / 5);
    range.time.begin = range.time.begin > margin ? range.time.begin - margin : 0;
    range.time.end += std::min(margin, file->metadata.sampleCount - range.time.end);
    const auto frequencyMargin = (range.frequency.upperHz - range.frequency.lowerHz) * .2;
    range.frequency.lowerHz -= frequencyMargin;
    range.frequency.upperHz += frequencyMargin;
    selectMarks({id}, id);
    setView(range);
    return true;
}

bool Session::setView(ViewRange range, bool record) {
    auto* file = activeFile();
    if (!file) return false;
    range = clampRange(range, file->metadata, file->display.stftSize, file->display.psdSize);
    if (file->view == range) return false;
    const auto previous=snapshot();
    file->view = range;
    if(record)commitViewChange(previous);
    return true;
}

bool Session::setPsdFromSelection(bool enabled) {
    auto* file=activeFile();if(!file)return false;
    if(enabled&&!findMark(*file,file->activeMarkId))return false;
    file->display.psdFromSelection=enabled;
    return true;
}

bool Session::setEffectiveBandwidthHz(double bandwidthHz) {
    auto* file = activeFile();
    if (!file || !std::isfinite(bandwidthHz) || bandwidthHz <= 0 ||
        bandwidthHz > file->metadata.sampleRateHz ||
        bandwidthHz < file->metadata.sampleRateHz / 65536.0 ||
        bandwidthHz == file->metadata.effectiveBandwidthHz) return false;
    file->metadata.effectiveBandwidthHz = bandwidthHz;
    file->view = clampRange(file->view, file->metadata, file->display.stftSize, file->display.psdSize);
    return true;
}

bool Session::setAuxiliaryMode(AuxiliaryMode mode) {
    auto* file=activeFile();if(!file||file->display.auxiliaryMode==mode)return false;
    if(mode!=AuxiliaryMode::Waveform&&mode!=AuxiliaryMode::Psd)return false;
    saveActiveAuxiliary(file->display);
    file->display.auxiliaryMode=mode;
    loadActiveAuxiliary(file->display);
    return true;
}

bool Session::setAuxiliaryRange(double minimum,double maximum,bool record) {
    auto* file=activeFile();if(!file||!std::isfinite(minimum)||!std::isfinite(maximum)||
        !std::isfinite(maximum-minimum))return false;
    const auto range=clampAuxiliary(minimum,maximum,file->display.auxiliaryMode);
    if(range.first==file->display.auxiliaryMin&&range.second==file->display.auxiliaryMax)return false;
    const auto previous=snapshot();
    file->display.auxiliaryMin=range.first;file->display.auxiliaryMax=range.second;
    saveActiveAuxiliary(file->display);
    if(record)commitViewChange(previous);
    return true;
}

ViewSnapshot Session::snapshot() const {
    const auto* file=activeFile();if(!file)return {};
    return snapshotFor(*file);
}

bool Session::restoreSnapshot(const ViewSnapshot& previous) {
    const auto found=std::find_if(project_.files.begin(),project_.files.end(),
        [&](const FileState& file){return file.metadata.id==previous.fileId;});
    if(found==project_.files.end())return false;
    auto* file=&*found;
    if(!std::isfinite(previous.waveformMin)||!std::isfinite(previous.waveformMax)||
       !std::isfinite(previous.psdMin)||!std::isfinite(previous.psdMax))return false;
    const auto wave=clampAuxiliary(previous.waveformMin,previous.waveformMax,AuxiliaryMode::Waveform);
    const auto psd=clampAuxiliary(previous.psdMin,previous.psdMax,AuxiliaryMode::Psd);
    file->view=clampRange(previous.view,file->metadata,file->display.stftSize,file->display.psdSize);
    file->display.waveformMin=wave.first;file->display.waveformMax=wave.second;
    file->display.psdMin=psd.first;file->display.psdMax=psd.second;
    loadActiveAuxiliary(file->display);
    return true;
}

bool Session::commitViewChange(const ViewSnapshot& previous) {
    const auto found=std::find_if(project_.files.begin(),project_.files.end(),
        [&](const FileState& file){return file.metadata.id==previous.fileId;});
    if(found==project_.files.end()||previous==snapshotFor(*found))return false;
    const auto* file=&*found;
    auto& history=histories_[file->metadata.id];
    pushBounded(history.past,previous);history.future.clear();
    return true;
}

bool Session::back() {
    auto* file = activeFile();
    if (!file || !canBack()) return false;
    auto& history = histories_.at(file->metadata.id);
    pushBounded(history.future, snapshot());
    restoreSnapshot(history.past.back());
    history.past.pop_back();
    return true;
}

bool Session::forward() {
    auto* file = activeFile();
    if (!file || !canForward()) return false;
    auto& history = histories_.at(file->metadata.id);
    pushBounded(history.past, snapshot());
    restoreSnapshot(history.future.back());
    history.future.pop_back();
    return true;
}

bool Session::canBack() const {
    const auto* file = activeFile();
    if (!file) return false;
    const auto found = histories_.find(file->metadata.id);
    return found != histories_.end() && !found->second.past.empty();
}

bool Session::canForward() const {
    const auto* file = activeFile();
    if (!file) return false;
    const auto found = histories_.find(file->metadata.id);
    return found != histories_.end() && !found->second.future.empty();
}

void Session::resetView() {
    auto* file = activeFile();
    if (!file) return;
    const auto previous=snapshot();
    file->view=defaultView(file->metadata,file->display.stftSize,file->display.psdSize);
    file->display.waveformMin=-60;file->display.waveformMax=60;
    file->display.psdMin=-100;file->display.psdMax=0;
    loadActiveAuxiliary(file->display);
    commitViewChange(previous);
}

bool Session::createChannelFromActiveMark() {
    auto* file = activeFile();
    if (!file) return false;
    const auto* mark = findMark(*file, file->activeMarkId);
    if (!mark) return false;
    const auto range = mark->range.frequency;
    file->channels.push_back({nextId("channel-"), numbered("Channel ",file->channels.size()+1), mark->id,
        range.lowerHz + (range.upperHz - range.lowerHz) / 2, range.upperHz - range.lowerHz});
    return true;
}

void Session::replaceProject(Project project) {
    project_ = std::move(project);
    histories_.clear();
    demoSequence_ = project_.files.size();
}

} // namespace signalstudio
