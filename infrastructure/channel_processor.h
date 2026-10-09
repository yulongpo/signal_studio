#pragma once

#include "domain/project.h"

#include <QString>
#include <QSize>

#include <complex>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace signalstudio {

struct ChannelDspPlan {
    std::uint64_t sourceRateHz = 0;
    std::uint64_t outputRateHz = 0;
    std::uint32_t interpolation = 0;
    std::uint32_t decimation = 0;
    int sourceDecimationStages = 0;
    double transitionHz = 0;
    double cutoffHz = 0;
    double stopbandAttenuationDb = 0;
    std::size_t estimatedWorkingBytes = 0;
    std::vector<std::vector<double>> stageCoefficients;
};

struct ChannelSampleData {
    TimeRange outputSamples;
    TimeRange sourceSamples;
    std::vector<std::complex<float>> samples;
    bool leftPadded = false;
    bool rightPadded = false;
};

struct ChannelSampleCacheStats {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
};

// Demand-driven, fixed-size output IQ blocks. One instance is owned by a
// workspace; callers serialize requests and cancel/join before clearing it.
class ChannelSampleCache final {
public:
    static constexpr std::uint64_t blockSamples = 16'384;
    static constexpr std::size_t defaultBudgetBytes = 128U * 1024U * 1024U;

    explicit ChannelSampleCache(std::size_t budgetBytes = defaultBudgetBytes);
    bool process(const FileMetadata& source, const Channel& channel,
                 const ChannelDspPlan& plan, TimeRange outputSamples,
                 ChannelSampleData& result,
                 const std::function<bool()>& cancelled = {});
    void clear();
    ChannelSampleCacheStats stats() const;

private:
    struct Entry {
        std::shared_ptr<const ChannelSampleData> data;
        std::list<std::string>::iterator lru;
        std::size_t bytes = 0;
    };

    std::size_t budgetBytes_;
    std::size_t bytes_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::list<std::string> lru_;
    std::unordered_map<std::string, Entry> entries_;
};

bool makeChannelDspPlan(const FileMetadata& source, const Channel& channel,
                        ChannelDspPlan& plan, QString& error);
bool processChannelSamples(const FileMetadata& source, const Channel& channel,
                           const ChannelDspPlan& plan, TimeRange outputSamples,
                           ChannelSampleData& result,
                           const std::function<bool()>& cancelled = {},
                           const std::function<void(std::uint64_t, std::uint64_t)>& progress = {});
bool channelWaveform(const ChannelSampleData& data, int points, NarrowbandWaveform mode,
                     std::vector<float>& output);
bool channelWaveformIQ(const ChannelSampleData& data, int points,
                       std::vector<float>& i, std::vector<float>& q);
bool channelPsd(const ChannelSampleData& data, double outputSampleRateHz,
                FrequencyRange frequencies, int fftSize, int points,
                std::vector<float>& output,
                const std::function<bool()>& cancelled = {});
bool channelSpectrogram(const ChannelSampleData& data, double outputSampleRateHz,
                        FrequencyRange frequencies, QSize pixels, int fftSize,
                        std::vector<float>& output,
                        const std::function<bool()>& cancelled = {});

} // namespace signalstudio
