#pragma once

#include "domain/project.h"

#include <unordered_map>

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
    void selectMarks(std::vector<std::string> ids, std::string active = {});
    DeleteResult deleteSelectedMarks();
    bool focusMark(const std::string& id);
    bool setView(ViewRange range, bool record = true);
    bool setPsdFromSelection(bool enabled);
    bool setEffectiveBandwidthHz(double bandwidthHz);
    bool setAuxiliaryMode(AuxiliaryMode mode);
    bool setAuxiliaryRange(double minimum, double maximum, bool record = true);
    ViewSnapshot snapshot() const;
    bool restoreSnapshot(const ViewSnapshot& snapshot);
    bool commitViewChange(const ViewSnapshot& previous);
    bool back();
    bool forward();
    bool canBack() const;
    bool canForward() const;
    void resetView();
    bool createChannelFromActiveMark();
    std::string createChannel(const std::string& name, const std::string& sourceMarkId,
                              double centerFrequencyHz, double bandwidthHz,
                              double outputSampleRateHz, TimeRange sourceTime,
                              ChannelFilter filter, bool wholeSource,
                              bool preserveSourceTime);
    bool updateChannel(const std::string& channelId, const Channel& replacement);
    bool activateChannel(const std::string& channelId);
    ChannelViewSnapshot channelViewSnapshot() const;
    bool setChannelView(TimeRange sourceTime, FrequencyRange basebandFrequency, bool record = true);
    bool setChannelAmplitudeRange(double minimum, double maximum, bool autoScale, bool record = true);
    bool setChannelPsdRange(double minimum, double maximum, bool record = true);
    bool restoreChannelViewSnapshot(const ChannelViewSnapshot& snapshot);
    bool commitChannelViewChange(const ChannelViewSnapshot& previous);
    bool channelBack();
    bool channelForward();
    void replaceProject(Project project);

private:
    struct History {
        std::vector<ViewSnapshot> past;
        std::vector<ViewSnapshot> future;
    };
    std::string nextId(const std::string& prefix);
    Project project_;
    std::unordered_map<std::string, History> histories_;
    struct ChannelHistory { std::vector<ChannelViewSnapshot> past, future; };
    std::unordered_map<std::string, ChannelHistory> channelHistories_;
    std::uint64_t nextIdentifier_ = 1;
    std::uint64_t demoSequence_ = 0;
};

} // namespace signalstudio
