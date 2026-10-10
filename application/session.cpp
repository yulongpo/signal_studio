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
    const long double minimumVisibleSamples = static_cast<long double>(metadata.sampleRateHz) * 0.01L;
    const auto minimumVisible = minimumVisibleSamples >= static_cast<long double>(metadata.sampleCount) ?
        metadata.sampleCount : static_cast<SampleIndex>(std::ceil(minimumVisibleSamples));
    const auto firstFivePercent = ratio(metadata.sampleCount, 1, 20);
    const auto visibleSamples = std::max(firstFivePercent, minimumVisible);
    view.time = {0, visibleSamples};
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
    auto result=ViewSnapshot{file.metadata.id,file.view,display.waveformMin,display.waveformMax,display.psdMin,display.psdMax,display.waveformAutoFit};
    if(display.auxiliaryMode==AuxiliaryMode::Waveform) {
        result.waveformMin=display.auxiliaryMin;result.waveformMax=display.auxiliaryMax;
    } else {result.psdMin=display.auxiliaryMin;result.psdMax=display.auxiliaryMax;}
    return result;
}

std::pair<double,double> clampAuxiliary(double minimum,double maximum,AuxiliaryMode mode) {
    if(minimum>maximum)std::swap(minimum,maximum);
    const double lower=mode==AuxiliaryMode::Waveform?-65536:-180;
    const double upper=mode==AuxiliaryMode::Waveform?65536:50;
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

FileState* Session::fileForChannel(const std::string& channelId) {
    for (auto& file : project_.files)
        if (std::any_of(file.channels.begin(), file.channels.end(), [&](const Channel& channel) { return channel.id == channelId; }))
            return &file;
    return nullptr;
}

const FileState* Session::fileForChannel(const std::string& channelId) const {
    for (const auto& file : project_.files)
        if (std::any_of(file.channels.begin(), file.channels.end(), [&](const Channel& channel) { return channel.id == channelId; }))
            return &file;
    return nullptr;
}

Channel* Session::activeChannel() {
    auto* file = fileForChannel(project_.activeChannelId);
    if (!file) return nullptr;
    const auto found = std::find_if(file->channels.begin(), file->channels.end(),
        [&](const Channel& channel) { return channel.id == project_.activeChannelId; });
    return found == file->channels.end() ? nullptr : &*found;
}

const Channel* Session::activeChannel() const {
    const auto* file = fileForChannel(project_.activeChannelId);
    if (!file) return nullptr;
    const auto found = std::find_if(file->channels.begin(), file->channels.end(),
        [&](const Channel& channel) { return channel.id == project_.activeChannelId; });
    return found == file->channels.end() ? nullptr : &*found;
}

void Session::newProject() {
    ++projectGeneration_;
    project_ = Project{};
    histories_.clear();
    channelHistories_.clear();
    demoSequence_ = 0;
    cursors_.clear(); spectra_.clear(); spectralLru_.clear();
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
        file.display.waveformMin = -32768;
        file.display.waveformMax = 32768;
        file.display.auxiliaryMin = -32768;
        file.display.auxiliaryMax = 32768;
    }
    file.view = defaultView(file.metadata, file.display.stftSize, file.display.psdSize);
    const auto id = file.metadata.id;
    project_.files.push_back(std::move(file));
    project_.activeFileId = id;
    project_.activeChannelId.clear();
    project_.narrowbandWorkspaceOpen = false;
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
    project_.activeChannelId.clear();
    project_.narrowbandWorkspaceOpen = false;
    return true;
}

bool Session::removeActiveFile() {
    const auto found = std::find_if(project_.files.begin(), project_.files.end(),
        [&](const FileState& file) { return file.metadata.id == project_.activeFileId; });
    if (found == project_.files.end()) return false;
    const auto index = static_cast<std::size_t>(found - project_.files.begin());
    std::unordered_set<std::string> removedChannels;
    for (const auto& channel : found->channels) removedChannels.insert(channel.id);
    histories_.erase(found->metadata.id); clearCursor(found->metadata.id); installSpectrogram(found->metadata.id, {});
    for (const auto& id : removedChannels) { channelHistories_.erase(id); clearCursor(id); installSpectrogram(id, {}); }
    project_.files.erase(found);
    project_.activeFileId = project_.files.empty() ? std::string{} :
        project_.files[std::min(index, project_.files.size() - 1)].metadata.id;
    if (removedChannels.contains(project_.activeChannelId)) {
        project_.activeChannelId.clear(); project_.narrowbandWorkspaceOpen = false;
    }
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
    std::unordered_set<std::string> removedChannels;
    for (const auto& channel : file->channels)
        if (selected.contains(channel.sourceMarkId)) removedChannels.insert(channel.id);
    std::erase_if(file->channels, [&](const Channel& channel) { return removedChannels.contains(channel.id); });
    for (const auto& id : removedChannels) { channelHistories_.erase(id); clearCursor(id); installSpectrogram(id, {}); }
    if (removedChannels.contains(project_.activeChannelId)) {
        project_.activeChannelId.clear(); project_.narrowbandWorkspaceOpen = false;
    }
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
    if(file->display.auxiliaryMode==AuxiliaryMode::Waveform)file->display.waveformAutoFit=false;
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
    file->display.waveformAutoFit=previous.waveformAutoFit;
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
    file->display.waveformMin=-32768;file->display.waveformMax=32768;
    file->display.waveformAutoFit=true;
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

std::string Session::createChannel(const std::string& name, const std::string& sourceMarkId,
                                   double centerFrequencyHz, double bandwidthHz,
                                   double outputSampleRateHz, TimeRange sourceTime,
                                   ChannelFilter filter, bool wholeSource,
                                   bool preserveSourceTime) {
    auto* file = activeFile();
    if (!file || name.empty() || name.size() > 320 || !std::isfinite(centerFrequencyHz) ||
        !std::isfinite(bandwidthHz) || !std::isfinite(outputSampleRateHz) ||
        bandwidthHz <= 0 || outputSampleRateHz <= 0) return {};
    const auto* mark = findMark(*file, sourceMarkId);
    if (!mark) return {};
    const auto transition = bandwidthHz * .15;
    const auto input = fullRange(file->metadata).frequency;
    const auto guardBand = bandwidthHz / 2 + transition;
    const auto sourceBegin = wholeSource ? SampleIndex{0} : sourceTime.begin;
    const auto sourceEnd = wholeSource ? file->metadata.sampleCount : sourceTime.end;
    if (sourceBegin >= sourceEnd || sourceEnd > file->metadata.sampleCount ||
        (!wholeSource && (sourceBegin < mark->range.time.begin || sourceEnd > mark->range.time.end)) ||
        centerFrequencyHz - guardBand < input.lowerHz || centerFrequencyHz + guardBand > input.upperHz ||
        outputSampleRateHz + 1e-9 < bandwidthHz + 2 * transition) return {};

    Channel channel;
    channel.id = nextId("channel-");
    channel.name = name;
    channel.sourceMarkId = sourceMarkId;
    channel.centerFrequencyHz = centerFrequencyHz;
    channel.bandwidthHz = bandwidthHz;
    channel.sourceTime = {sourceBegin, sourceEnd};
    channel.outputSampleRateHz = outputSampleRateHz;
    channel.filter = filter;
    channel.processingState = ChannelProcessingState::Ready;
    channel.configVersion = 1;
    channel.wholeSource = wholeSource;
    channel.preserveSourceTime = preserveSourceTime;
    const auto previewSamples = static_cast<SampleIndex>(std::max(1.0,
        std::min(4'000'000'000.0, std::floor(file->metadata.sampleRateHz * .25))));
    const auto initialSourceSpan = std::min(sourceEnd - sourceBegin, previewSamples);
    channel.visibleSourceTime = {sourceBegin, sourceBegin + initialSourceSpan};
    channel.visibleBasebandFrequency = {-outputSampleRateHz / 2, outputSampleRateHz / 2};
    file->channels.push_back(channel);
    project_.activeChannelId = channel.id;
    project_.activeFileId = file->metadata.id;
    project_.narrowbandWorkspaceOpen = true;
    return channel.id;
}

bool Session::updateChannel(const std::string& channelId, const Channel& replacement) {
    auto* file = fileForChannel(channelId);
    if (!file || replacement.id != channelId || !findMark(*file, replacement.sourceMarkId) ||
        !std::isfinite(replacement.centerFrequencyHz) || !std::isfinite(replacement.bandwidthHz) ||
        !std::isfinite(replacement.outputSampleRateHz) || replacement.bandwidthHz <= 0 ||
        replacement.outputSampleRateHz + 1e-9 < replacement.bandwidthHz * 1.3 ||
        replacement.sourceTime.begin >= replacement.sourceTime.end || replacement.sourceTime.end > file->metadata.sampleCount ||
        replacement.centerFrequencyHz - replacement.bandwidthHz * .65 < fullRange(file->metadata).frequency.lowerHz ||
        replacement.centerFrequencyHz + replacement.bandwidthHz * .65 > fullRange(file->metadata).frequency.upperHz)
        return false;
    auto* old = [&]() -> Channel* {
        const auto found = std::find_if(file->channels.begin(), file->channels.end(),
            [&](const Channel& channel) { return channel.id == channelId; });
        return found == file->channels.end() ? nullptr : &*found;
    }();
    if (!old) return false;
    auto value = replacement;
    if (old->configVersion == std::numeric_limits<std::uint64_t>::max()) return false;
    value.configVersion = old->configVersion + 1;
    value.processingState = ChannelProcessingState::Ready;
    *old = std::move(value);
    channelHistories_.erase(channelId); installSpectrogram(channelId, {});
    return true;
}

bool Session::removeChannel(const std::string& channelId) {
    auto* file = fileForChannel(channelId); if (!file) return false;
    std::erase_if(file->channels, [&](const Channel& channel) { return channel.id == channelId; });
    channelHistories_.erase(channelId); clearCursor(channelId); installSpectrogram(channelId, {});
    if (project_.activeChannelId == channelId) { project_.activeChannelId.clear(); project_.narrowbandWorkspaceOpen = false; }
    return true;
}
bool Session::activateChannel(const std::string& channelId) {
    auto* file = fileForChannel(channelId);
    if (!file) return false;
    project_.activeFileId = file->metadata.id;
    project_.activeChannelId = channelId;
    project_.narrowbandWorkspaceOpen = true;
    return true;
}

ChannelViewSnapshot Session::channelViewSnapshot() const {
    const auto* channel = activeChannel();
    if (!channel) return {};
    return {channel->id, channel->visibleSourceTime, channel->visibleBasebandFrequency,
            channel->waveformAxisMinimum, channel->waveformAxisMaximum, channel->waveformAutoScale,
            channel->psdAxisMinimum, channel->psdAxisMaximum};
}

bool Session::setChannelView(TimeRange sourceTime, FrequencyRange basebandFrequency, bool record) {
    auto* channel = activeChannel();
    if (!channel || !std::isfinite(basebandFrequency.lowerHz) || !std::isfinite(basebandFrequency.upperHz)) return false;
    sourceTime.begin = std::clamp(sourceTime.begin, channel->sourceTime.begin, channel->sourceTime.end);
    sourceTime.end = std::clamp(sourceTime.end, sourceTime.begin, channel->sourceTime.end);
    const double half = channel->outputSampleRateHz / 2;
    basebandFrequency.lowerHz = std::clamp(basebandFrequency.lowerHz, -half, half);
    basebandFrequency.upperHz = std::clamp(basebandFrequency.upperHz, basebandFrequency.lowerHz, half);
    if (sourceTime.begin >= sourceTime.end || basebandFrequency.lowerHz >= basebandFrequency.upperHz) return false;
    if (sourceTime == channel->visibleSourceTime && basebandFrequency == channel->visibleBasebandFrequency) return false;
    const auto before = channelViewSnapshot();
    channel->visibleSourceTime = sourceTime;
    channel->visibleBasebandFrequency = basebandFrequency;
    if (record) commitChannelViewChange(before);
    return true;
}

bool Session::setChannelAmplitudeRange(double minimum, double maximum, bool autoScale, bool record) {
    auto* channel = activeChannel();
    if (!channel || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum || maximum - minimum > 2.0e12)
        return false;
    if (channel->waveformAxisMinimum == minimum && channel->waveformAxisMaximum == maximum &&
        channel->waveformAutoScale == autoScale) return false;
    const auto before = channelViewSnapshot();
    channel->waveformAxisMinimum = minimum;
    channel->waveformAxisMaximum = maximum;
    channel->waveformAutoScale = autoScale;
    if (record) commitChannelViewChange(before);
    return true;
}

bool Session::setChannelPsdRange(double minimum, double maximum, bool record) {
    auto* channel = activeChannel();
    if (!channel || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum || maximum - minimum > 1000.0)
        return false;
    if (channel->psdAxisMinimum == minimum && channel->psdAxisMaximum == maximum) return false;
    const auto before = channelViewSnapshot();
    channel->psdAxisMinimum = minimum;
    channel->psdAxisMaximum = maximum;
    if (record) commitChannelViewChange(before);
    return true;
}

bool Session::restoreChannelViewSnapshot(const ChannelViewSnapshot& snapshot) {
    if (snapshot.channelId.empty()) return false;
    Channel* channel = nullptr;
    for (auto& file : project_.files) {
        const auto found = std::find_if(file.channels.begin(), file.channels.end(), [&](const Channel& value) {
            return value.id == snapshot.channelId;
        });
        if (found != file.channels.end()) { channel = &*found; break; }
    }
    if (!channel || snapshot.sourceTime.begin >= snapshot.sourceTime.end ||
        !std::isfinite(snapshot.basebandFrequency.lowerHz) || !std::isfinite(snapshot.basebandFrequency.upperHz) ||
        snapshot.basebandFrequency.lowerHz >= snapshot.basebandFrequency.upperHz ||
        !std::isfinite(snapshot.waveformAxisMinimum) || !std::isfinite(snapshot.waveformAxisMaximum) ||
        snapshot.waveformAxisMinimum >= snapshot.waveformAxisMaximum ||
        !std::isfinite(snapshot.psdAxisMinimum) || !std::isfinite(snapshot.psdAxisMaximum) ||
        snapshot.psdAxisMinimum >= snapshot.psdAxisMaximum) return false;
    const auto sampleBegin = std::clamp(snapshot.sourceTime.begin, channel->sourceTime.begin, channel->sourceTime.end);
    const auto sampleEnd = std::clamp(snapshot.sourceTime.end, sampleBegin, channel->sourceTime.end);
    const double half = channel->outputSampleRateHz / 2;
    const auto frequencyLower = std::clamp(snapshot.basebandFrequency.lowerHz, -half, half);
    const auto frequencyUpper = std::clamp(snapshot.basebandFrequency.upperHz, frequencyLower, half);
    if (sampleBegin >= sampleEnd || frequencyLower >= frequencyUpper) return false;
    channel->visibleSourceTime = {sampleBegin, sampleEnd};
    channel->visibleBasebandFrequency = {frequencyLower, frequencyUpper};
    channel->waveformAxisMinimum = snapshot.waveformAxisMinimum;
    channel->waveformAxisMaximum = snapshot.waveformAxisMaximum;
    channel->waveformAutoScale = snapshot.waveformAutoScale;
    channel->psdAxisMinimum = snapshot.psdAxisMinimum;
    channel->psdAxisMaximum = snapshot.psdAxisMaximum;
    return true;
}

bool Session::commitChannelViewChange(const ChannelViewSnapshot& previous) {
    if (previous.channelId.empty()) return false;
    Channel* channel = nullptr;
    for (auto& file : project_.files) {
        const auto found = std::find_if(file.channels.begin(), file.channels.end(), [&](const Channel& value) {
            return value.id == previous.channelId;
        });
        if (found != file.channels.end()) { channel = &*found; break; }
    }
    if (!channel) return false;
    const ChannelViewSnapshot current{channel->id, channel->visibleSourceTime, channel->visibleBasebandFrequency,
        channel->waveformAxisMinimum, channel->waveformAxisMaximum, channel->waveformAutoScale,
        channel->psdAxisMinimum, channel->psdAxisMaximum};
    if (previous == current) return false;
    auto& history = channelHistories_[channel->id];
    if (history.past.empty() || history.past.back() != previous) {
        history.past.push_back(previous);
        if (history.past.size() > 40) history.past.erase(history.past.begin());
    }
    history.future.clear();
    return true;
}

bool Session::channelBack() {
    const auto current = channelViewSnapshot();
    auto* channel = activeChannel();
    if (!channel) return false;
    auto& history = channelHistories_[channel->id];
    if (history.past.empty()) return false;
    history.future.push_back(current);
    if (history.future.size() > 40) history.future.erase(history.future.begin());
    const auto target = history.past.back();
    history.past.pop_back();
    return restoreChannelViewSnapshot(target);
}

bool Session::channelForward() {
    const auto current = channelViewSnapshot();
    auto* channel = activeChannel();
    if (!channel) return false;
    auto& history = channelHistories_[channel->id];
    if (history.future.empty()) return false;
    history.past.push_back(current);
    if (history.past.size() > 40) history.past.erase(history.past.begin());
    const auto target = history.future.back();
    history.future.pop_back();
    return restoreChannelViewSnapshot(target);
}

void Session::replaceProject(Project project) {
    ++projectGeneration_;
    project_ = std::move(project);
    histories_.clear();
    channelHistories_.clear();
    demoSequence_ = project_.files.size();
    cursors_.clear(); spectra_.clear(); spectralLru_.clear();
}

const LinkedCursorState& Session::linkedCursor(const std::string& context) const {
    static const LinkedCursorState empty;
    const auto found = cursors_.find(context);
    return found == cursors_.end() ? empty : found->second;
}
void Session::pinCursor(const std::string& context, SampleIndex sample, double frequencyHz, bool selectFrame) {
    auto& cursor = cursors_[context]; cursor.pinned = true;
    cursor.sourceSample = sample; cursor.frequencyHz = frequencyHz;
    if (selectFrame) cursor.framePsd = true;
    if (const auto* frame = selectedSpectralFrame(context)) cursor.selectedFrame = frame->id;
}
void Session::clearCursor(const std::string& context) { cursors_.erase(context); }
void Session::setFramePsd(const std::string& context, bool enabled) {
    auto& cursor = cursors_[context]; cursor.framePsd = enabled && cursor.pinned;
}
void Session::installSpectrogram(const std::string& context, std::shared_ptr<const SpectrogramData> data) {
    if (!data) { spectra_.erase(context); std::erase(spectralLru_, context); return; }
    spectra_[context] = std::move(data);
    std::erase(spectralLru_, context); spectralLru_.push_back(context);
    std::size_t bytes = 0;
    for (const auto& item : spectra_) if (item.second) bytes += item.second->bytes();
    // Display payloads have a separate 64 MiB budget, keeping total analysis <=128 MiB.
    while (bytes > 64U * 1024U * 1024U && spectralLru_.size() > 1) {
        const auto oldest = spectralLru_.front(); spectralLru_.erase(spectralLru_.begin());
        if (const auto found = spectra_.find(oldest); found != spectra_.end()) {
            if (found->second) bytes -= found->second->bytes();
            spectra_.erase(found);
        }
    }
    if (const auto* frame = selectedSpectralFrame(context)) cursors_[context].selectedFrame = frame->id;
}
std::shared_ptr<const SpectrogramData> Session::spectrogram(const std::string& context) const {
    const auto found = spectra_.find(context); return found == spectra_.end() ? nullptr : found->second;
}
const SpectralFrame* Session::selectedSpectralFrame(const std::string& context) const {
    const auto& cursor = linkedCursor(context);
    const auto data = spectrogram(context);
    return cursor.pinned && data ? data->frameAt(cursor.sourceSample) : nullptr;
}

} // namespace signalstudio
