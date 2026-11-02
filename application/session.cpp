#include "application/session.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {

constexpr std::size_t maximumHistory = 40;

// Integer ratio calculation keeps the whole uint64 range exact and avoids
// overflow.
SampleIndex ratio(SampleIndex value, SampleIndex numerator,
                  SampleIndex denominator) {
  return value / denominator * numerator +
         value % denominator * numerator / denominator;
}

ViewRange defaultView(const FileMetadata& metadata, int stftSize, int psdSize) {
  auto view = fullRange(metadata);
  const long double minimumVisibleSamples =
      static_cast<long double>(metadata.sampleRateHz) * 0.01L;
  const auto minimumVisible =
      minimumVisibleSamples >= static_cast<long double>(metadata.sampleCount)
          ? metadata.sampleCount
          : static_cast<SampleIndex>(std::ceil(minimumVisibleSamples));
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
  if (display.auxiliaryMode == AuxiliaryMode::Waveform) {
    display.waveformMin = display.auxiliaryMin;
    display.waveformMax = display.auxiliaryMax;
  } else {
    display.psdMin = display.auxiliaryMin;
    display.psdMax = display.auxiliaryMax;
  }
}

void loadActiveAuxiliary(DisplaySettings& display) {
  display.auxiliaryMin = display.auxiliaryMode == AuxiliaryMode::Waveform
                             ? display.waveformMin
                             : display.psdMin;
  display.auxiliaryMax = display.auxiliaryMode == AuxiliaryMode::Waveform
                             ? display.waveformMax
                             : display.psdMax;
}

ViewSnapshot snapshotFor(const FileState& file) {
  const auto& display = file.display;
  auto result =
      ViewSnapshot{file.metadata.id,       file.view,      display.waveformMin,
                   display.waveformMax,    display.psdMin, display.psdMax,
                   display.waveformAutoFit};
  if (display.auxiliaryMode == AuxiliaryMode::Waveform) {
    result.waveformMin = display.auxiliaryMin;
    result.waveformMax = display.auxiliaryMax;
  } else {
    result.psdMin = display.auxiliaryMin;
    result.psdMax = display.auxiliaryMax;
  }
  return result;
}

std::pair<double, double> clampAuxiliary(double minimum, double maximum,
                                         AuxiliaryMode mode) {
  if (minimum > maximum) std::swap(minimum, maximum);
  if (mode == AuxiliaryMode::Psd) {
    if (validPsdRange(minimum, maximum)) return {minimum, maximum};
    const double width = std::clamp(maximum - minimum, .001, maximumPsdSpan);
    const double center = std::midpoint(minimum, maximum);
    return {center - width / 2, center + width / 2};
  }
  const double lower = -65536;
  const double upper = 65536;
  const double width = std::clamp(maximum - minimum, 2.0, upper - lower);
  const double begin = std::clamp(minimum + (maximum - minimum) / 2 - width / 2,
                                  lower, upper - width);
  return {begin, begin + width};
}

std::string numbered(const std::string& prefix, std::size_t number) {
  return prefix + (number < 10 ? "0" : "") + std::to_string(number);
}

}  // namespace

Session::Session() { project_.name = "射频信号分析工程"; }

FileState* Session::activeFile() {
  const auto found = std::find_if(
      project_.files.begin(), project_.files.end(), [&](const FileState& file) {
        return file.metadata.id == project_.activeFileId;
      });
  return found == project_.files.end() ? nullptr : &*found;
}

const FileState* Session::activeFile() const {
  const auto found = std::find_if(
      project_.files.begin(), project_.files.end(), [&](const FileState& file) {
        return file.metadata.id == project_.activeFileId;
      });
  return found == project_.files.end() ? nullptr : &*found;
}

FileState* Session::fileForChannel(const std::string& channelId) {
  for (auto& file : project_.files)
    if (std::any_of(
            file.channels.begin(), file.channels.end(),
            [&](const Channel& channel) { return channel.id == channelId; }))
      return &file;
  return nullptr;
}

const FileState* Session::fileForChannel(const std::string& channelId) const {
  for (const auto& file : project_.files)
    if (std::any_of(
            file.channels.begin(), file.channels.end(),
            [&](const Channel& channel) { return channel.id == channelId; }))
      return &file;
  return nullptr;
}

Channel* Session::activeChannel() {
  auto* file = fileForChannel(project_.activeChannelId);
  if (!file) return nullptr;
  const auto found =
      std::find_if(file->channels.begin(), file->channels.end(),
                   [&](const Channel& channel) {
                     return channel.id == project_.activeChannelId;
                   });
  return found == file->channels.end() ? nullptr : &*found;
}

const Channel* Session::activeChannel() const {
  const auto* file = fileForChannel(project_.activeChannelId);
  if (!file) return nullptr;
  const auto found =
      std::find_if(file->channels.begin(), file->channels.end(),
                   [&](const Channel& channel) {
                     return channel.id == project_.activeChannelId;
                   });
  return found == file->channels.end() ? nullptr : &*found;
}

bool Session::acceptProjectWrite(std::string& error) const {
  if (!projectWriteGuard_) return true;
  return projectWriteGuard_(error);
}
void Session::setProjectWriteGuard(std::function<bool(std::string&)> guard) {
  projectWriteGuard_ = std::move(guard);
  annotations_.setWriteGuard([this](std::string& error) {
    return acceptProjectWrite(error);
  });
}

void Session::newProject() {
  if (std::string blocked; !acceptProjectWrite(blocked)) return;
  ++projectGeneration_;
  annotations_.clear();
  project_ = Project{};
  histories_.clear();
  channelHistories_.clear();
  demoSequence_ = 0;
  cursors_.clear();
  spectra_.clear();
  spectralLru_.clear();
}

std::string Session::nextId(const std::string& prefix) {
  for (;;) {
    const auto candidate = prefix + std::to_string(nextIdentifier_++);
    bool exists = false;
    for (const auto& file : project_.files) {
      exists =
          exists || file.metadata.id == candidate || findMark(file, candidate);
      exists = exists || std::any_of(file.channels.begin(), file.channels.end(),
                                     [&](const Channel& channel) {
                                       return channel.id == candidate;
                                     });
    }
    if (!exists) return candidate;
  }
}

std::string Session::addDemoFile() {
  static const FileMetadata defaults[]{
      {"", "wideband_100MHz.iq", "", 40e6, 100e6, 19'200'000'000, true, 1},
      {"", "capture_2450MHz.iq", "", 20e6, 2450e6, 4'800'000'000, true, 3},
      {"", "telemetry_915MHz.iq", "", 10e6, 915e6, 1'200'000'000, true, 7}};
  return addDemoFile(defaults[demoSequence_ % 3]);
}

std::string Session::addDemoFile(FileMetadata metadata) {
  if (std::string blocked; !acceptProjectWrite(blocked)) return {};
  if (metadata.name.empty() || metadata.sampleCount == 0 ||
      !std::isfinite(metadata.sampleRateHz) || metadata.sampleRateHz <= 0 ||
      !std::isfinite(metadata.centerFrequencyHz))
    return {};
  const double maximumBandwidth =
      metadata.sampleRateHz /
      (metadata.sampleFormat.structure == SampleStructure::Real ? 2 : 1);
  if (metadata.effectiveBandwidthHz <= 0)
    metadata.effectiveBandwidthHz = maximumBandwidth;
  if (metadata.effectiveBandwidthHz > maximumBandwidth) return {};
  const auto bounds = fullRange(metadata);
  if (!std::isfinite(bounds.frequency.lowerHz) ||
      !std::isfinite(bounds.frequency.upperHz) ||
      bounds.frequency.lowerHz >= bounds.frequency.upperHz)
    return {};
  if (!metadata.id.empty() &&
      std::any_of(project_.files.begin(), project_.files.end(),
                  [&](const FileState& file) {
                    return file.metadata.id == metadata.id;
                  }))
    return {};
  if (metadata.id.empty()) metadata.id = nextId("file-");
  if (metadata.demoSeed <= 0)
    metadata.demoSeed = static_cast<int>(project_.files.size() + 1);
  FileState file;
  file.metadata = std::move(metadata);
  if (!file.metadata.demo) {
    file.display.waveformMin = -32768;
    file.display.waveformMax = 32768;
    file.display.auxiliaryMin = -32768;
    file.display.auxiliaryMax = 32768;
  }
  file.view =
      defaultView(file.metadata, file.display.stftSize, file.display.psdSize);
  const auto id = file.metadata.id;
  project_.files.push_back(std::move(file));
  project_.activeFileId = id;
  project_.activeChannelId.clear();
  project_.narrowbandWorkspaceOpen = false;
  ++demoSequence_;
  return id;
}

std::string Session::addDemoFile(const std::string& name, double sampleRateHz,
                                 double centerFrequencyHz,
                                 double durationSeconds,
                                 const std::string& path) {
  if (!std::isfinite(durationSeconds) || durationSeconds <= 0 ||
      !std::isfinite(sampleRateHz) || sampleRateHz <= 0)
    return {};
  const long double count =
      static_cast<long double>(sampleRateHz) * durationSeconds;
  if (count < 1 || count >= static_cast<long double>(
                                std::numeric_limits<SampleIndex>::max()))
    return {};
  return addDemoFile({"", name, path, sampleRateHz, centerFrequencyHz,
                      static_cast<SampleIndex>(count), true,
                      static_cast<int>(project_.files.size() + 1)});
}

bool Session::activateFile(const std::string& id) {
  if (std::none_of(
          project_.files.begin(), project_.files.end(),
          [&](const FileState& file) { return file.metadata.id == id; }))
    return false;
  // Re-selecting the open file still clears the active channel and closes the
  // narrowband workspace. Those fields are serialized navigation.
  if (project_.activeFileId != id || !project_.activeChannelId.empty() ||
      project_.narrowbandWorkspaceOpen) {
    if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  }
  if (auto* previous = activeFile()) {
    annotations_.endRevision(*previous);
    annotations_.hover(*previous);
  }
  project_.activeFileId = id;
  project_.activeChannelId.clear();
  project_.narrowbandWorkspaceOpen = false;
  if (auto* current = activeFile()) {
    const auto reconciled = annotations_.reconcileSelection(*current);
    if (!reconciled.ok) return false;
    annotations_.hover(*current);
  }
  return true;
}

bool Session::removeActiveFile() {
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  const auto found = std::find_if(
      project_.files.begin(), project_.files.end(), [&](const FileState& file) {
        return file.metadata.id == project_.activeFileId;
      });
  if (found == project_.files.end()) return false;
  const auto index = static_cast<std::size_t>(found - project_.files.begin());
  std::unordered_set<std::string> removedChannels;
  for (const auto& channel : found->channels)
    removedChannels.insert(channel.id);
  annotations_.clear(found->metadata.id);
  histories_.erase(found->metadata.id);
  clearCursor(found->metadata.id);
  installSpectrogram(found->metadata.id, {});
  for (const auto& id : removedChannels) {
    channelHistories_.erase(id);
    clearCursor(id);
    installSpectrogram(id, {});
  }
  project_.files.erase(found);
  project_.activeFileId =
      project_.files.empty()
          ? std::string{}
          : project_.files[std::min(index, project_.files.size() - 1)]
                .metadata.id;
  if (removedChannels.contains(project_.activeChannelId)) {
    project_.activeChannelId.clear();
    project_.narrowbandWorkspaceOpen = false;
  }
  return true;
}

std::string Session::addMark(ViewRange range) {
  auto* file = activeFile();
  if (!file) return {};
  const auto result = annotations_.createManual(
      *file, numbered("Region ", file->marks.size() + 1), range);
  return result.ok ? result.id : std::string{};
}

AnnotationResult Session::commitAnnotationGeometry(const std::string& id,
                                                   ViewRange range) {
  auto* file = activeFile();
  if (!file) return {false, false, {}, "无活动文件"};
  const auto* annotation = findMark(*file, id);
  if (!annotation) return {false, false, {}, "标记已失效"};
  const auto* segment = activeSegment(*annotation);
  if (!segment) {
    if (!annotation->observation || range.time != annotation->observation->time)
      return {false, false, {}, "时间未定位对象只能编辑频率"};
    return annotations_.commitObservationFrequency(*file, id, range.frequency);
  }
  return annotations_.commitGeometry(*file, id, segment->id, range);
}

AnnotationResult Session::selectMarks(std::vector<std::string> ids,
                                      std::string active) {
  auto* file = activeFile();
  if (!file) return {false, false, {}, "无活动文件"};
  return annotations_.select(*file, std::move(ids), std::move(active));
}

DeleteResult Session::deleteSelectedMarks(bool removeChannels) {
  auto* file = activeFile();
  if (!file) return {};
  const auto selected = file->selectedMarkIds;
  std::unordered_set<std::string> ids(selected.begin(), selected.end());
  if (removeChannels &&
      std::any_of(file->channels.begin(), file->channels.end(),
                  [&](const Channel& channel) {
                    return ids.contains(channel.sourceMarkId);
                  })) {
    if (std::string blocked; !acceptProjectWrite(blocked)) return {};
  }
  const auto result = annotations_.deleteBatch(*file, selected);
  if (!result.ok) return {};
  int removed = 0;
  if (removeChannels) {
    std::unordered_set<std::string> channels;
    for (const auto& c : file->channels)
      if (ids.contains(c.sourceMarkId)) channels.insert(c.id);
    std::erase_if(file->channels,
                  [&](const auto& c) { return channels.contains(c.id); });
    removed = static_cast<int>(channels.size());
    for (const auto& id : channels) {
      channelHistories_.erase(id);
      clearCursor(id);
      installSpectrogram(id, {});
    }
    if (channels.contains(project_.activeChannelId)) {
      project_.activeChannelId.clear();
      project_.narrowbandWorkspaceOpen = false;
    }
  }
  {
    // deleteBatch already notified. Clearing the rest of the selection is the
    // same committed write.
    AnnotationService::WroteSuppression suppression(annotations_);
    const auto cleared = annotations_.select(*file, {});
    if (!cleared.ok) return {static_cast<int>(selected.size()), removed};
    file->display.psdFromSelection = false;
  }
  return {static_cast<int>(selected.size()), removed};
}

bool Session::focusMark(const std::string& id) {
  auto* file = activeFile();
  if (!file) return false;
  const auto* mark = findMark(*file, id);
  if (!mark || mark->deleted) return false;
  const auto* segment = activeSegment(*mark);
  if (!segment && !mark->observation) return false;
  auto range = segment ? segment->range : *mark->observation;
  const auto margin =
      std::max(SampleIndex{1}, (range.time.end - range.time.begin) / 5);
  range.time.begin = range.time.begin > margin ? range.time.begin - margin : 0;
  range.time.end +=
      std::min(margin, file->metadata.sampleCount - range.time.end);
  const auto frequencyMargin =
      (range.frequency.upperHz - range.frequency.lowerHz) * .2;
  range.frequency.lowerHz -= frequencyMargin;
  range.frequency.upperHz += frequencyMargin;
  const bool selected =
      std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(),
                id) != file->selectedMarkIds.end();
  const auto anchor = file->selectionAnchorId;
  const auto before = annotations_.captureGuarded(*file);
  {
    AnnotationService::WroteSuppression suppression(annotations_);
    const auto result = annotations_.select(
        *file, selected ? file->selectedMarkIds : std::vector<std::string>{id},
        id, segment ? segment->id : std::string{});
    if (!result.ok) return false;
    if (selected) file->selectionAnchorId = anchor;
  }
  annotations_.notifyGuardedChange(before, *file);
  setView(range);
  return true;
}

bool Session::setView(ViewRange range, bool record) {
  auto* file = activeFile();
  if (!file) return false;
  range = clampRange(range, file->metadata, file->display.stftSize,
                     file->display.psdSize);
  if (file->view == range) return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  const auto previous = snapshot();
  file->view = range;
  noteProjectWrite();
  if (record) commitViewChange(previous);
  return true;
}

bool Session::setPsdFromSelection(bool enabled) {
  auto* file = activeFile();
  if (!file) return false;
  const auto* mark = findMark(*file, file->activeMarkId);
  if (enabled && (!mark || mark->deleted || !activeSegment(*mark)))
    return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  file->display.psdFromSelection = enabled;
  file->display.psd.scope = enabled ? PsdScope::SourceMark : PsdScope::Visible;
  return true;
}

bool Session::setEffectiveBandwidthHz(double bandwidthHz) {
  auto* file = activeFile();
  if (!file || !std::isfinite(bandwidthHz) || bandwidthHz <= 0 ||
      bandwidthHz >
          file->metadata.sampleRateHz /
              (file->metadata.sampleFormat.structure == SampleStructure::Real
                   ? 2.0
                   : 1.0) ||
      bandwidthHz < file->metadata.sampleRateHz / 65536.0 ||
      bandwidthHz == file->metadata.effectiveBandwidthHz)
    return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  file->metadata.effectiveBandwidthHz = bandwidthHz;
  file->view = clampRange(file->view, file->metadata, file->display.stftSize,
                          file->display.psdSize);
  return true;
}

bool Session::setAuxiliaryMode(AuxiliaryMode mode) {
  auto* file = activeFile();
  if (!file || file->display.auxiliaryMode == mode) return false;
  if (mode != AuxiliaryMode::Waveform && mode != AuxiliaryMode::Psd)
    return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  saveActiveAuxiliary(file->display);
  file->display.auxiliaryMode = mode;
  loadActiveAuxiliary(file->display);
  return true;
}

bool Session::setAuxiliaryRange(double minimum, double maximum, bool record) {
  auto* file = activeFile();
  if (!file || !std::isfinite(minimum) || !std::isfinite(maximum) ||
      !std::isfinite(maximum - minimum))
    return false;
  const auto range =
      clampAuxiliary(minimum, maximum, file->display.auxiliaryMode);
  if (file->display.auxiliaryMode == AuxiliaryMode::Psd &&
      !validPsdRange(range.first, range.second))
    return false;
  if (range.first == file->display.auxiliaryMin &&
      range.second == file->display.auxiliaryMax)
    return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  const auto previous = snapshot();
  file->display.auxiliaryMin = range.first;
  file->display.auxiliaryMax = range.second;
  if (file->display.auxiliaryMode == AuxiliaryMode::Waveform)
    file->display.waveformAutoFit = false;
  saveActiveAuxiliary(file->display);
  if (record) commitViewChange(previous);
  return true;
}

ViewSnapshot Session::snapshot() const {
  const auto* file = activeFile();
  if (!file) return {};
  return snapshotFor(*file);
}

bool Session::restoreSnapshot(const ViewSnapshot& previous) {
  const auto found = std::find_if(project_.files.begin(), project_.files.end(),
                                  [&](const FileState& file) {
                                    return file.metadata.id == previous.fileId;
                                  });
  if (found == project_.files.end()) return false;
  auto* file = &*found;
  if (!std::isfinite(previous.waveformMin) ||
      !std::isfinite(previous.waveformMax) || !std::isfinite(previous.psdMin) ||
      !std::isfinite(previous.psdMax))
    return false;
  const auto wave = clampAuxiliary(previous.waveformMin, previous.waveformMax,
                                   AuxiliaryMode::Waveform);
  const auto psd =
      clampAuxiliary(previous.psdMin, previous.psdMax, AuxiliaryMode::Psd);
  if (!validPsdRange(psd.first, psd.second)) return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  file->view = clampRange(previous.view, file->metadata, file->display.stftSize,
                          file->display.psdSize);
  file->display.waveformMin = wave.first;
  file->display.waveformMax = wave.second;
  file->display.psdMin = psd.first;
  file->display.psdMax = psd.second;
  file->display.waveformAutoFit = previous.waveformAutoFit;
  loadActiveAuxiliary(file->display);
  return true;
}

bool Session::commitViewChange(const ViewSnapshot& previous) {
  const auto found = std::find_if(project_.files.begin(), project_.files.end(),
                                  [&](const FileState& file) {
                                    return file.metadata.id == previous.fileId;
                                  });
  if (found == project_.files.end() || previous == snapshotFor(*found))
    return false;
  const auto* file = &*found;
  auto& history = histories_[file->metadata.id];
  pushBounded(history.past, previous);
  history.future.clear();
  return true;
}

bool Session::back() {
  auto* file = activeFile();
  if (!file || !canBack()) return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  auto& history = histories_.at(file->metadata.id);
  pushBounded(history.future, snapshot());
  restoreSnapshot(history.past.back());
  history.past.pop_back();
  return true;
}

bool Session::forward() {
  auto* file = activeFile();
  if (!file || !canForward()) return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
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
  if (std::string blocked; !acceptProjectWrite(blocked)) return;
  const auto previous = snapshot();
  file->view = defaultView(file->metadata, file->display.stftSize,
                           file->display.psdSize);
  file->display.waveformMin = -32768;
  file->display.waveformMax = 32768;
  file->display.waveformAutoFit = true;
  file->display.psdMin = -100;
  file->display.psdMax = 0;
  loadActiveAuxiliary(file->display);
  commitViewChange(previous);
}

bool Session::createChannelFromActiveMark() {
  auto* file = activeFile();
  if (!file || file->selectedMarkIds.size() != 1) return false;
  if (std::any_of(file->marks.begin(), file->marks.end(),
                  [](const Annotation& mark) { return mark.fileId.empty(); })) {
    if (std::string blocked; !acceptProjectWrite(blocked)) return false;
    if (!attachLegacyAnnotations(*file).ok) return false;
  }
  auto request = annotationChannelRequest(*file, file->activeMarkId,
                                          file->view.time, projectGeneration_);
  request.name = numbered("Channel ", file->channels.size() + 1);
  return createAnnotationChannel(request).ok;
}

AnnotationResult Session::createAnnotationChannel(
    const AnnotationChannelRequest& request) {
  auto* file = activeFile();
  if (!file) return {false, false, {}, "请先打开来源文件。", "source"};
  if (request.sourceIdentity.projectGeneration &&
      request.sourceIdentity.projectGeneration != projectGeneration_)
    return {false, false, {}, "工程已切换，请重新打开通道配置。", "source"};
  Channel channel;
  if (const auto bound =
          annotations_.bindChannelSource(*file, channel, request);
      !bound)
    return bound;
  if (std::string blocked; !acceptProjectWrite(blocked))
    return {false, false, {}, std::move(blocked), "project"};
  channel.id = nextId("channel-");
  channel.processingState = ChannelProcessingState::Ready;
  channel.configVersion = 1;
  const auto previewSamples = static_cast<SampleIndex>(
      std::max(1.0, std::min(4'000'000'000.0,
                             std::floor(file->metadata.sampleRateHz * .25))));
  const auto begin = channel.sourceTime.begin;
  const auto initialSpan =
      std::min(channel.sourceTime.end - begin, previewSamples);
  channel.visibleSourceTime = {begin, begin + initialSpan};
  channel.visibleBasebandFrequency = {-channel.outputSampleRateHz / 2,
                                      channel.outputSampleRateHz / 2};
  const auto id = channel.id;
  file->channels.push_back(std::move(channel));
  project_.activeChannelId = id;
  project_.activeFileId = file->metadata.id;
  project_.narrowbandWorkspaceOpen = true;
  return {true, true, id, {}};
}

std::string Session::createChannel(const std::string& name,
                                   const std::string& sourceMarkId,
                                   double centerFrequencyHz, double bandwidthHz,
                                   double outputSampleRateHz,
                                   TimeRange sourceTime, ChannelFilter filter,
                                   bool wholeSource, bool preserveSourceTime) {
  auto* file = activeFile();
  if (!file || wholeSource || !std::isfinite(centerFrequencyHz) ||
      !std::isfinite(bandwidthHz) || bandwidthHz <= 0)
    return {};
  if (std::any_of(file->marks.begin(), file->marks.end(),
                  [](const Annotation& mark) { return mark.fileId.empty(); })) {
    if (std::string blocked; !acceptProjectWrite(blocked)) return {};
    if (!attachLegacyAnnotations(*file).ok) return {};
  }
  auto request = annotationChannelRequest(*file, sourceMarkId, file->view.time,
                                          projectGeneration_);
  request.name = name;
  request.range = {sourceTime,
                   {centerFrequencyHz - bandwidthHz / 2,
                    centerFrequencyHz + bandwidthHz / 2}};
  request.outputSampleRateHz = outputSampleRateHz;
  request.sampling.mode = ChannelSamplingMode::Manual;
  request.filter = filter;
  request.preserveSourceTime = preserveSourceTime;
  const auto created = createAnnotationChannel(request);
  return created.ok ? created.id : std::string{};
}

bool Session::updateChannel(const std::string& channelId,
                            const Channel& replacement) {
  auto* file = fileForChannel(channelId);
  if (!file || replacement.id != channelId ||
      !validateChannelName(replacement.name) ||
      (!findMark(*file, replacement.sourceMarkId) &&
       channelSourceRelation(*file, replacement) != ChannelSourceRelation::Retired) ||
      !std::isfinite(replacement.centerFrequencyHz) ||
      !std::isfinite(replacement.bandwidthHz) ||
      !std::isfinite(replacement.outputSampleRateHz) ||
      replacement.bandwidthHz <= 0 ||
      replacement.outputSampleRateHz + 1e-9 < replacement.bandwidthHz * 1.3 ||
      replacement.sourceTime.begin >= replacement.sourceTime.end ||
      replacement.sourceTime.end > file->metadata.sampleCount ||
      replacement.sourceTime.end > availableSamples(file->metadata) ||
      file->metadata.availability.status == LoadStatus::Failed ||
      replacement.centerFrequencyHz - replacement.bandwidthHz * .65 <
          fullRange(file->metadata).frequency.lowerHz ||
      replacement.centerFrequencyHz + replacement.bandwidthHz * .65 >
          fullRange(file->metadata).frequency.upperHz)
    return false;
  auto* old = [&]() -> Channel* {
    const auto found = std::find_if(
        file->channels.begin(), file->channels.end(),
        [&](const Channel& channel) { return channel.id == channelId; });
    return found == file->channels.end() ? nullptr : &*found;
  }();
  if (!old || replacement.sourceMarkId != old->sourceMarkId ||
      replacement.annotationId != old->annotationId ||
      replacement.segmentId != old->segmentId ||
      replacement.revisionId != old->revisionId ||
      replacement.sourceIdentity != old->sourceIdentity ||
      replacement.sourceTime != old->sourceTime ||
      replacement.wholeSource != old->wholeSource ||
      bool(replacement.sourceSnapshot) != bool(old->sourceSnapshot) ||
      (old->sourceSnapshot &&
       *replacement.sourceSnapshot != *old->sourceSnapshot))
    return false;
  auto value = replacement;
  if (old->configVersion == std::numeric_limits<std::uint64_t>::max())
    return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  value.configVersion = old->configVersion + 1;
  value.processingState = ChannelProcessingState::Ready;
  *old = std::move(value);
  channelHistories_.erase(channelId);
  installSpectrogram(channelId, {});
  return true;
}

bool Session::removeChannel(const std::string& channelId) {
  auto* file = fileForChannel(channelId);
  if (!file) return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  std::erase_if(file->channels, [&](const Channel& channel) {
    return channel.id == channelId;
  });
  channelHistories_.erase(channelId);
  clearCursor(channelId);
  installSpectrogram(channelId, {});
  if (project_.activeChannelId == channelId) {
    project_.activeChannelId.clear();
    project_.narrowbandWorkspaceOpen = false;
  }
  return true;
}
bool Session::activateChannel(const std::string& channelId) {
  auto* file = fileForChannel(channelId);
  if (!file) return false;
  if (project_.activeFileId == file->metadata.id &&
      project_.activeChannelId == channelId &&
      project_.narrowbandWorkspaceOpen)
    return true;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  project_.activeFileId = file->metadata.id;
  project_.activeChannelId = channelId;
  project_.narrowbandWorkspaceOpen = true;
  return true;
}

ChannelViewSnapshot Session::channelViewSnapshot() const {
  const auto* channel = activeChannel();
  if (!channel) return {};
  return {channel->id,
          channel->visibleSourceTime,
          channel->visibleBasebandFrequency,
          channel->waveformAxisMinimum,
          channel->waveformAxisMaximum,
          channel->waveformAutoScale,
          channel->psdAxisMinimum,
          channel->psdAxisMaximum};
}

bool Session::setChannelView(TimeRange sourceTime,
                             FrequencyRange basebandFrequency, bool record) {
  auto* channel = activeChannel();
  if (!channel || !std::isfinite(basebandFrequency.lowerHz) ||
      !std::isfinite(basebandFrequency.upperHz))
    return false;
  sourceTime.begin = std::clamp(sourceTime.begin, channel->sourceTime.begin,
                                channel->sourceTime.end);
  sourceTime.end =
      std::clamp(sourceTime.end, sourceTime.begin, channel->sourceTime.end);
  const double half = channel->outputSampleRateHz / 2;
  basebandFrequency.lowerHz =
      std::clamp(basebandFrequency.lowerHz, -half, half);
  basebandFrequency.upperHz =
      std::clamp(basebandFrequency.upperHz, basebandFrequency.lowerHz, half);
  if (sourceTime.begin >= sourceTime.end ||
      basebandFrequency.lowerHz >= basebandFrequency.upperHz)
    return false;
  if (sourceTime == channel->visibleSourceTime &&
      basebandFrequency == channel->visibleBasebandFrequency)
    return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  const auto before = channelViewSnapshot();
  channel->visibleSourceTime = sourceTime;
  channel->visibleBasebandFrequency = basebandFrequency;
  if (record) commitChannelViewChange(before);
  return true;
}

bool Session::setChannelAmplitudeRange(double minimum, double maximum,
                                       bool autoScale, bool record) {
  auto* channel = activeChannel();
  if (!channel || !std::isfinite(minimum) || !std::isfinite(maximum) ||
      minimum >= maximum || maximum - minimum > 2.0e12)
    return false;
  if (channel->waveformAxisMinimum == minimum &&
      channel->waveformAxisMaximum == maximum &&
      channel->waveformAutoScale == autoScale)
    return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  const auto before = channelViewSnapshot();
  channel->waveformAxisMinimum = minimum;
  channel->waveformAxisMaximum = maximum;
  channel->waveformAutoScale = autoScale;
  if (record) commitChannelViewChange(before);
  return true;
}

bool Session::setChannelPsdRange(double minimum, double maximum, bool record) {
  auto* channel = activeChannel();
  if (!channel || !validPsdRange(minimum, maximum)) return false;
  if (channel->psdAxisMinimum == minimum && channel->psdAxisMaximum == maximum)
    return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
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
    const auto found = std::find_if(
        file.channels.begin(), file.channels.end(),
        [&](const Channel& value) { return value.id == snapshot.channelId; });
    if (found != file.channels.end()) {
      channel = &*found;
      break;
    }
  }
  if (!channel || snapshot.sourceTime.begin >= snapshot.sourceTime.end ||
      !std::isfinite(snapshot.basebandFrequency.lowerHz) ||
      !std::isfinite(snapshot.basebandFrequency.upperHz) ||
      snapshot.basebandFrequency.lowerHz >=
          snapshot.basebandFrequency.upperHz ||
      !std::isfinite(snapshot.waveformAxisMinimum) ||
      !std::isfinite(snapshot.waveformAxisMaximum) ||
      snapshot.waveformAxisMinimum >= snapshot.waveformAxisMaximum ||
      !validPsdRange(snapshot.psdAxisMinimum, snapshot.psdAxisMaximum))
    return false;
  const auto sampleBegin =
      std::clamp(snapshot.sourceTime.begin, channel->sourceTime.begin,
                 channel->sourceTime.end);
  const auto sampleEnd =
      std::clamp(snapshot.sourceTime.end, sampleBegin, channel->sourceTime.end);
  const double half = channel->outputSampleRateHz / 2;
  const auto frequencyLower =
      std::clamp(snapshot.basebandFrequency.lowerHz, -half, half);
  const auto frequencyUpper =
      std::clamp(snapshot.basebandFrequency.upperHz, frequencyLower, half);
  if (sampleBegin >= sampleEnd || frequencyLower >= frequencyUpper)
    return false;
  const TimeRange nextTime{sampleBegin, sampleEnd};
  const FrequencyRange nextFrequency{frequencyLower, frequencyUpper};
  if (channel->visibleSourceTime == nextTime &&
      channel->visibleBasebandFrequency == nextFrequency &&
      channel->waveformAxisMinimum == snapshot.waveformAxisMinimum &&
      channel->waveformAxisMaximum == snapshot.waveformAxisMaximum &&
      channel->waveformAutoScale == snapshot.waveformAutoScale &&
      channel->psdAxisMinimum == snapshot.psdAxisMinimum &&
      channel->psdAxisMaximum == snapshot.psdAxisMaximum)
    return true;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  channel->visibleSourceTime = nextTime;
  channel->visibleBasebandFrequency = nextFrequency;
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
    const auto found = std::find_if(
        file.channels.begin(), file.channels.end(),
        [&](const Channel& value) { return value.id == previous.channelId; });
    if (found != file.channels.end()) {
      channel = &*found;
      break;
    }
  }
  if (!channel) return false;
  const ChannelViewSnapshot current{channel->id,
                                    channel->visibleSourceTime,
                                    channel->visibleBasebandFrequency,
                                    channel->waveformAxisMinimum,
                                    channel->waveformAxisMaximum,
                                    channel->waveformAutoScale,
                                    channel->psdAxisMinimum,
                                    channel->psdAxisMaximum};
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
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
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
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  history.past.push_back(current);
  if (history.past.size() > 40) history.past.erase(history.past.begin());
  const auto target = history.future.back();
  history.future.pop_back();
  return restoreChannelViewSnapshot(target);
}

void Session::replaceProject(Project project) {
  if (std::string blocked; !acceptProjectWrite(blocked)) return;
  for (auto& file : project.files) {
    const auto migrated = attachLegacyAnnotations(file);
    if (!migrated.ok) return;
  }
  annotations_.clear();
  ++projectGeneration_;
  project_ = std::move(project);
  histories_.clear();
  channelHistories_.clear();
  demoSequence_ = project_.files.size();
  cursors_.clear();
  spectra_.clear();
  spectralLru_.clear();
}

bool Session::setPowerDisplayRange(PowerDisplayRange range) {
  if (!range.valid() || !activeFile()) return false;
  if (std::string blocked; !acceptProjectWrite(blocked)) return false;
  for (auto& file : project_.files) {
    file.display.referenceLevelDb = range.referenceLevelDb;
    file.display.dynamicRangeDb = range.dynamicRangeDb;
    file.display.psdMin = range.lowerDb();
    file.display.psdMax = range.referenceLevelDb;
    if (file.display.auxiliaryMode == AuxiliaryMode::Psd) {
      file.display.auxiliaryMin = range.lowerDb();
      file.display.auxiliaryMax = range.referenceLevelDb;
    }
    for (auto& channel : file.channels) {
      channel.psdAxisMinimum = range.lowerDb();
      channel.psdAxisMaximum = range.referenceLevelDb;
    }
  }
  return true;
}

const LinkedCursorState& Session::linkedCursor(
    const std::string& context) const {
  static const LinkedCursorState empty;
  const auto found = cursors_.find(context);
  return found == cursors_.end() ? empty : found->second;
}
void Session::pinCursor(const std::string& context, SampleIndex sample,
                        double frequencyHz, bool selectFrame,
                        std::uint64_t frameId) {
  auto& cursor = cursors_[context];
  cursor.pinned = true;
  cursor.sourceSample = sample;
  cursor.frequencyHz = frequencyHz;
  if (selectFrame) cursor.framePsd = true;
  if (selectFrame) cursor.selectedFrame = frameId;
  if (const auto* frame = selectedSpectralFrame(context))
    cursor.selectedFrame = frame->id;
}
void Session::clearCursor(const std::string& context) {
  cursors_.erase(context);
}
void Session::setFramePsd(const std::string& context, bool enabled) {
  auto& cursor = cursors_[context];
  cursor.framePsd = enabled && cursor.pinned;
}
void Session::installSpectrogram(const std::string& context,
                                 std::shared_ptr<const SpectrogramData> data) {
  if (!data) {
    spectra_.erase(context);
    std::erase(spectralLru_, context);
    return;
  }
  spectra_[context] = std::move(data);
  if (cursors_.contains(context)) cursors_[context].selectedFrame = 0;
  std::erase(spectralLru_, context);
  spectralLru_.push_back(context);
  std::size_t bytes = 0;
  for (const auto& item : spectra_)
    if (item.second) bytes += item.second->bytes();
  // Display payloads have a separate 64 MiB budget, keeping total analysis
  // <=128 MiB.
  while (bytes > 64U * 1024U * 1024U && spectralLru_.size() > 1) {
    const auto oldest = spectralLru_.front();
    spectralLru_.erase(spectralLru_.begin());
    if (const auto found = spectra_.find(oldest); found != spectra_.end()) {
      if (found->second) bytes -= found->second->bytes();
      spectra_.erase(found);
    }
  }
  if (const auto* frame = selectedSpectralFrame(context))
    cursors_[context].selectedFrame = frame->id;
}
std::shared_ptr<const SpectrogramData> Session::spectrogram(
    const std::string& context) const {
  const auto found = spectra_.find(context);
  return found == spectra_.end() ? nullptr : found->second;
}
const SpectralFrame* Session::selectedSpectralFrame(
    const std::string& context) const {
  const auto& cursor = linkedCursor(context);
  const auto data = spectrogram(context);
  if (!cursor.pinned || !data || cursor.sourceSample < data->sourceView.begin ||
      cursor.sourceSample >= data->sourceView.end)
    return nullptr;
  if (cursor.selectedFrame)
    for (const auto& frame : data->frames)
      if (frame->id == cursor.selectedFrame) return frame.get();
  return data->frameAt(cursor.sourceSample);
}

}  // namespace signalstudio
