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
enum class WaveformMode { I, Q, IqRms };
enum class Palette { Turbo, Viridis, Gray, Plasma, Inferno, Magma, Cividis, CoolEditClassic };

struct DisplaySettings {
    MainMode mainMode = MainMode::TimeFrequency;
    AuxiliaryMode auxiliaryMode = AuxiliaryMode::Waveform;
    WaveformMode waveformMode = WaveformMode::IqRms;
    Palette palette = Palette::CoolEditClassic;
    int stftSize = 2048;
    int psdSize = 4096;
    double dynamicRangeDb = 80;
    double referenceLevelDb = 0;
    bool absoluteFrequency = true;
    bool grid = true;
    bool colorScale = false;
    bool psdFromSelection = false;
    // Compatibility fields expose the active mode; Session maintains both saved ranges.
    double auxiliaryMin = -60;
    double auxiliaryMax = 60;
    double waveformMin = -60;
    double waveformMax = 60;
    double psdMin = -100;
    double psdMax = 0;
};

struct FileMetadata {
    std::string id;
    std::string name;
    std::string path;
    double sampleRateHz = 0;
    double centerFrequencyHz = 0;
    SampleIndex sampleCount = 0;
    bool demo = true;
    int demoSeed = 1;
    double declaredBandwidthHz = 0;
    // The centered band used by analysis. Zero means the complete sampled band.
    double effectiveBandwidthHz = 0;
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

struct ViewSnapshot {
    std::string fileId;
    ViewRange view;
    double waveformMin = -60;
    double waveformMax = 60;
    double psdMin = -100;
    double psdMax = 0;
    bool operator==(const ViewSnapshot&) const = default;
};

ViewRange fullRange(const FileMetadata& metadata);
ViewRange clampRange(ViewRange range, const FileMetadata& metadata, int stftSize, int psdSize = 0);
Mark* findMark(FileState& file, const std::string& id);
const Mark* findMark(const FileState& file, const std::string& id);

} // namespace signalstudio
