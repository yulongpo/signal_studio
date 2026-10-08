#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace signalstudio {

using SampleIndex = std::uint64_t;

// Half-open sample ranges avoid a floating-point business time coordinate.
struct TimeRange {
    SampleIndex begin = 0;
    SampleIndex end = 0;
    bool operator==(const TimeRange&) const = default;
};
struct FrequencyRange {
    double lowerHz = 0;
    double upperHz = 0;
    bool operator==(const FrequencyRange&) const = default;
};
struct ViewRange {
    TimeRange time;
    FrequencyRange frequency;
    bool operator==(const ViewRange&) const = default;
};

enum class MainMode { TimeFrequency, Waterfall };
enum class AuxiliaryMode { Waveform, Psd };
enum class Palette { Turbo, Viridis, Gray };

struct DisplaySettings {
    MainMode mainMode = MainMode::TimeFrequency;
    AuxiliaryMode auxiliaryMode = AuxiliaryMode::Waveform;
    Palette palette = Palette::Turbo;
    int stftSize = 2048;
    int psdSize = 4096;
    double dynamicRangeDb = 70;
    double referenceLevelDb = -10;
    bool absoluteFrequency = true;
    bool grid = true;
    bool colorScale = false;
    bool psdFromSelection = false;
    double auxiliaryMin = -1;
    double auxiliaryMax = 1;
};

struct FileMetadata {
    std::string id;
    std::string name;
    std::string path;
    double sampleRateHz = 0;
    double centerFrequencyHz = 0;
    SampleIndex sampleCount = 0;
    bool demo = true;
};
struct Mark {
    std::string id;
    std::string name;
    ViewRange range;
};
struct Channel {
    std::string id;
    std::string name;
    std::string sourceMarkId;
    double centerFrequencyHz = 0;
    double bandwidthHz = 0;
};
struct FileState {
    FileMetadata metadata;
    ViewRange view;
    DisplaySettings display;
    std::vector<Mark> marks;
    std::vector<Channel> channels;
    std::vector<std::string> selectedMarkIds;
    std::string activeMarkId;
};
struct Project {
    std::string name = "未命名工程";
    std::string activeFileId;
    std::vector<FileState> files;
};

ViewRange fullRange(const FileMetadata& metadata);
ViewRange clampRange(ViewRange range, const FileMetadata& metadata, int stftSize);
Mark* findMark(FileState& file, const std::string& id);
const Mark* findMark(const FileState& file, const std::string& id);

} // namespace signalstudio
