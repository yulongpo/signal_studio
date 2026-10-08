#include "application/session.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {

constexpr std::size_t maximumHistory = 40;

// Integer ratio calculation keeps the whole uint64 range exact and avoids overflow.
SampleIndex ratio(SampleIndex value, SampleIndex numerator, SampleIndex denominator) {
    return value / denominator * numerator + value % denominator * numerator / denominator;
}

ViewRange defaultView(const FileMetadata& metadata, int stftSize) {
    auto view = fullRange(metadata);
    view.time = {ratio(metadata.sampleCount, 7, 20), ratio(metadata.sampleCount, 11, 20)};
    return clampRange(view, metadata, stftSize);
}

void pushBounded(std::vector<ViewRange>& list, const ViewRange& view) {
    if (list.size() == maximumHistory) list.erase(list.begin());
    list.push_back(view);
}

} // namespace

Session::Session() {
    project_.name = "射频信号分析工程";
    const auto firstId = addDemoFile();
    addDemoFile();
    activateFile(firstId);
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
    FileState file;
    const bool second = demoSequence_++ % 2 != 0;
    file.metadata = {nextId("file-"), second ? "capture_2450MHz.iq" : "wideband_100MHz.iq", "",
                     second ? 20e6 : 40e6, second ? 2450e6 : 100e6,
                     second ? SampleIndex{4'800'000'000} : SampleIndex{19'200'000'000}, true};
    if (second) {
        file.display.mainMode = MainMode::Waterfall;
        file.display.auxiliaryMode = AuxiliaryMode::Psd;
        file.display.palette = Palette::Viridis;
        file.display.stftSize = 4096;
        file.display.psdSize = 8192;
        file.display.dynamicRangeDb = 60;
        file.display.referenceLevelDb = -20;
        file.display.absoluteFrequency = false;
        file.display.grid = false;
        file.display.colorScale = true;
        file.display.psdFromSelection = true;
        file.display.auxiliaryMin = -110;
        file.display.auxiliaryMax = -10;
        file.marks.push_back({nextId("mark-"), "标记 01", {{2'000'000'000, 2'320'000'000}, {2447e6, 2453e6}}});
    } else {
        file.marks.push_back({nextId("mark-"), "标记 01", {{7'600'000'000, 8'400'000'000}, {96e6, 110e6}}});
        file.marks.push_back({nextId("mark-"), "标记 02", {{8'000'000'000, 8'880'000'000}, {100e6, 114e6}}});
    }
    file.view = defaultView(file.metadata, file.display.stftSize);
    file.activeMarkId = file.marks.front().id;
    file.selectedMarkIds = {file.activeMarkId};
    const auto id = file.metadata.id;
    project_.files.push_back(std::move(file));
    project_.activeFileId = id;
    return id;
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
    file->marks.push_back({id, "标记 " + std::to_string(file->marks.size() + 1), range});
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
    range = clampRange(range, file->metadata, file->display.stftSize);
    if (file->view == range) return false;
    if (record) {
        auto& history = histories_[file->metadata.id];
        pushBounded(history.past, file->view);
        history.future.clear();
    }
    file->view = range;
    return true;
}

bool Session::back() {
    auto* file = activeFile();
    if (!file || !canBack()) return false;
    auto& history = histories_.at(file->metadata.id);
    pushBounded(history.future, file->view);
    file->view = clampRange(history.past.back(), file->metadata, file->display.stftSize);
    history.past.pop_back();
    return true;
}

bool Session::forward() {
    auto* file = activeFile();
    if (!file || !canForward()) return false;
    auto& history = histories_.at(file->metadata.id);
    pushBounded(history.past, file->view);
    file->view = clampRange(history.future.back(), file->metadata, file->display.stftSize);
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
    setView(defaultView(file->metadata, file->display.stftSize));
    file->display.auxiliaryMin = file->display.auxiliaryMode == AuxiliaryMode::Psd ? -100 : -1;
    file->display.auxiliaryMax = file->display.auxiliaryMode == AuxiliaryMode::Psd ? 0 : 1;
}

bool Session::createChannelFromActiveMark() {
    auto* file = activeFile();
    if (!file) return false;
    const auto* mark = findMark(*file, file->activeMarkId);
    if (!mark) return false;
    const auto range = mark->range.frequency;
    file->channels.push_back({nextId("channel-"), "演示窄带通道 " + std::to_string(file->channels.size() + 1), mark->id,
        range.lowerHz + (range.upperHz - range.lowerHz) / 2, range.upperHz - range.lowerHz});
    return true;
}

void Session::replaceProject(Project project) {
    project_ = std::move(project);
    histories_.clear();
    demoSequence_ = project_.files.size();
}

} // namespace signalstudio
