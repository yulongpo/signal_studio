#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "application/annotation_service.h"
#include "domain/power_display.h"
#include "domain/project.h"
#include "domain/spectral_data.h"

namespace signalstudio {

struct DeleteResult {
  int marks = 0;
  int channels = 0;
};

class Session {
 public:
  Session();
  Project& project() { return project_; }
  const Project& project() const { return project_; }
  std::uint64_t projectGeneration() const { return projectGeneration_; }
  FileState* activeFile();
  const FileState* activeFile() const;
  Channel* activeChannel();
  const Channel* activeChannel() const;
  FileState* fileForChannel(const std::string& channelId);
  const FileState* fileForChannel(const std::string& channelId) const;
  void newProject();
  std::string addDemoFile();
  std::string addDemoFile(FileMetadata metadata);
  std::string addDemoFile(const std::string& name, double sampleRateHz,
                          double centerFrequencyHz, double durationSeconds,
                          const std::string& path = {});
  bool activateFile(const std::string& id);
  bool removeActiveFile();
  std::string addMark(ViewRange range);
  AnnotationService& annotations() { return annotations_; }
  const AnnotationService& annotations() const { return annotations_; }
  AnnotationResult commitAnnotationGeometry(const std::string& id,
                                            ViewRange range);
  void selectMarks(std::vector<std::string> ids, std::string active = {});
  DeleteResult deleteSelectedMarks(bool removeChannels = false);
  bool focusMark(const std::string& id);
  bool setView(ViewRange range, bool record = true);
  bool setPsdFromSelection(bool enabled);
  bool setEffectiveBandwidthHz(double bandwidthHz);
  bool setAuxiliaryMode(AuxiliaryMode mode);
  bool setAuxiliaryRange(double minimum, double maximum, bool record = true);
  bool setPowerDisplayRange(PowerDisplayRange range);
  ViewSnapshot snapshot() const;
  bool restoreSnapshot(const ViewSnapshot& snapshot);
  bool commitViewChange(const ViewSnapshot& previous);
  bool back();
  bool forward();
  bool canBack() const;
  bool canForward() const;
  void resetView();
  bool createChannelFromActiveMark();
  AnnotationResult createAnnotationChannel(const AnnotationChannelRequest&);
  std::string createChannel(const std::string& name,
                            const std::string& sourceMarkId,
                            double centerFrequencyHz, double bandwidthHz,
                            double outputSampleRateHz, TimeRange sourceTime,
                            ChannelFilter filter, bool wholeSource,
                            bool preserveSourceTime);
  bool updateChannel(const std::string& channelId, const Channel& replacement);
  bool activateChannel(const std::string& channelId);
  bool removeChannel(const std::string& channelId);
  ChannelViewSnapshot channelViewSnapshot() const;
  bool setChannelView(TimeRange sourceTime, FrequencyRange basebandFrequency,
                      bool record = true);
  bool setChannelAmplitudeRange(double minimum, double maximum, bool autoScale,
                                bool record = true);
  bool setChannelPsdRange(double minimum, double maximum, bool record = true);
  bool restoreChannelViewSnapshot(const ChannelViewSnapshot& snapshot);
  bool commitChannelViewChange(const ChannelViewSnapshot& previous);
  bool channelBack();
  bool channelForward();
  void replaceProject(Project project);
  // Empty when a detection commit may still replace this project. Callers that
  // edit marks, the view, or the project file stop instead of racing that save.
  bool acceptProjectWrite(std::string& error) const;
  void setProjectWriteGuard(std::function<bool(std::string&)> guard);
  std::uint64_t contentEpoch() const { return contentEpoch_; }
  void noteProjectWrite() { ++contentEpoch_; }
  const LinkedCursorState& linkedCursor(const std::string& context) const;
  void pinCursor(const std::string& context, SampleIndex sample,
                 double frequencyHz, bool selectFrame,
                 std::uint64_t frameId = 0);
  void clearCursor(const std::string& context);
  void setFramePsd(const std::string& context, bool enabled);
  void installSpectrogram(const std::string& context,
                          std::shared_ptr<const SpectrogramData> data);
  std::shared_ptr<const SpectrogramData> spectrogram(
      const std::string& context) const;
  const SpectralFrame* selectedSpectralFrame(const std::string& context) const;

 private:
  struct History {
    std::vector<ViewSnapshot> past;
    std::vector<ViewSnapshot> future;
  };
  std::string nextId(const std::string& prefix);
  Project project_;
  std::uint64_t projectGeneration_ = 1;
  std::uint64_t contentEpoch_ = 1;
  std::function<bool(std::string&)> projectWriteGuard_;
  std::unordered_map<std::string, History> histories_;
  struct ChannelHistory {
    std::vector<ChannelViewSnapshot> past, future;
  };
  std::unordered_map<std::string, ChannelHistory> channelHistories_;
  std::uint64_t nextIdentifier_ = 1;
  std::unordered_map<std::string, LinkedCursorState> cursors_;
  std::unordered_map<std::string, std::shared_ptr<const SpectrogramData>>
      spectra_;
  std::vector<std::string> spectralLru_;
  std::uint64_t demoSequence_ = 0;
  AnnotationService annotations_;
};

}  // namespace signalstudio
