#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "domain/sample_format.h"

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
enum class WaveformMode { I, Q, IqRms, Envelope };
enum class Palette { Turbo, Viridis, Gray, Plasma, Inferno, Magma, Cividis, CoolEditClassic };

enum class SpectralMethod { Periodogram, Bartlett, Welch, Multitaper, Burg };
enum class SpectrumStatistic { Mean, Maximum, Minimum };
enum class SpectralWindow { Rectangular, Hann, Hamming, Blackman, BlackmanHarris, FlatTop, Kaiser };
enum class PsdScope { Visible, SourceMark, Whole };
struct SpectralParameters {
    SpectralMethod method = SpectralMethod::Welch;
    SpectralWindow window = SpectralWindow::Hann;
    double overlap = .5;
    double segmentMilliseconds = 0; // Zero selects N/B automatically.
    double kaiserBeta = 8.6, timeBandwidth = 3.5;
    int tapers = 6, burgOrder = 16;
    bool removeMean = false;
    bool operator==(const SpectralParameters&) const = default;
};
struct PsdSettings {
    SpectralParameters parameters;
    SpectrumStatistic statistic = SpectrumStatistic::Mean;
    PsdScope scope = PsdScope::Visible;
    bool operator==(const PsdSettings&) const = default;
};
struct SpectrogramSettings {
    SpectralParameters parameters;
    bool operator==(const SpectrogramSettings&) const = default;
};
enum class LoadStatus { Ready, Loading, Partial, Failed };
struct FileAvailability {
    SampleIndex availableSamples = 0;
    LoadStatus status = LoadStatus::Ready;
    std::uint64_t generation = 0;
    std::string fingerprint, error;
};
struct EnvelopePoint { SampleIndex begin = 0, end = 0, peakSample = 0; float peak = 0; };

enum class ChannelFilter { FastPreview, Standard, HighRejection };
enum class ChannelProcessingState { Ready, LegacyNeedsReview, SourceMissing, Invalid };
enum class NarrowbandPage { Observe, Modulation, DeepLearning, Demodulation };
enum class NarrowbandWaveform { IQ, Magnitude, Phase, Envelope };

struct DisplaySettings {
    MainMode mainMode = MainMode::TimeFrequency;
    AuxiliaryMode auxiliaryMode = AuxiliaryMode::Waveform;
    WaveformMode waveformMode = WaveformMode::IqRms;
    Palette palette = Palette::CoolEditClassic;
    int stftSize = 2048;
    int psdSize = 4096;
    PsdSettings psd;
    SpectrogramSettings spectrogram;
    double dynamicRangeDb = 80;
    double referenceLevelDb = 0;
    bool absoluteFrequency = true;
    bool grid = true;
    bool colorScale = false;
    bool psdFromSelection = false;
    // Compatibility fields expose the active mode; Session maintains both saved ranges.
    double auxiliaryMin = -32768;
    double auxiliaryMax = 32768;
    double waveformMin = -32768;
    double waveformMax = 32768;
    bool waveformAutoFit = true;
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
    FileAvailability availability;
    SampleFormat sampleFormat;
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
    TimeRange sourceTime;
    double outputSampleRateHz = 4e6;
    ChannelFilter filter = ChannelFilter::Standard;
    ChannelProcessingState processingState = ChannelProcessingState::LegacyNeedsReview;
    std::uint64_t configVersion = 1;
    bool wholeSource = false;
    bool preserveSourceTime = true;
    NarrowbandPage page = NarrowbandPage::Observe;
    TimeRange visibleSourceTime;
    FrequencyRange visibleBasebandFrequency;
    int psdFftSize = 4096;
    int stftFftSize = 2048;
    PsdSettings psd{SpectralParameters{}, SpectrumStatistic::Mean, PsdScope::Whole};
    SpectrogramSettings spectrogram;
    NarrowbandWaveform waveform = NarrowbandWaveform::IQ;
    double waveformAxisMinimum = -32768.0;
    double waveformAxisMaximum = 32768.0;
    bool waveformAutoScale = true;
    double psdAxisMinimum = -120.0;
    double psdAxisMaximum = 0.0;
    double symbolRate = 250e3;
    int eyePeriods = 2;
    int eyeTraces = 64;
    int eyeComponent = 0;
    int selectedBit = -1;
    double constellationMinimum = -1.0;
    double constellationMaximum = 1.0;
    bool relativeTime = false;
    bool absoluteFrequencyLabels = false;
};

struct ChannelViewSnapshot {
    std::string channelId;
    TimeRange sourceTime;
    FrequencyRange basebandFrequency;
    double waveformAxisMinimum = -32768.0;
    double waveformAxisMaximum = 32768.0;
    bool waveformAutoScale = true;
    double psdAxisMinimum = -120.0;
    double psdAxisMaximum = 0.0;
    bool operator==(const ChannelViewSnapshot&) const = default;
};
struct FileState {
    FileMetadata metadata;
    ViewRange view;
    DisplaySettings display;
    std::vector<Mark> marks;
    std::vector<Channel> channels;
    std::vector<std::string> selectedMarkIds;
    std::string activeMarkId;
    std::vector<EnvelopePoint> navigationEnvelope; // Session-only; rebuilt by actual source scanning.
};
struct Project {
    std::string name = "未命名工程";
    std::string activeFileId;
    std::string activeChannelId;
    bool narrowbandWorkspaceOpen = false;
    std::vector<FileState> files;
};

struct ViewSnapshot {
    std::string fileId;
    ViewRange view;
    double waveformMin = -32768;
    double waveformMax = 32768;
    double psdMin = -100;
    double psdMax = 0;
    bool waveformAutoFit = true;
    bool operator==(const ViewSnapshot&) const = default;
};

ViewRange fullRange(const FileMetadata& metadata);
double analysisFrequencyOffset(const FileMetadata& metadata);
SampleIndex availableSamples(const FileMetadata& metadata);
ViewRange clampRange(ViewRange range, const FileMetadata& metadata, int stftSize, int psdSize = 0);
Mark* findMark(FileState& file, const std::string& id);
const Mark* findMark(const FileState& file, const std::string& id);

} // namespace signalstudio
