#pragma once
#include "domain/project.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace signalstudio {
struct SpectralAnalysisPlan {
    FrequencyRange frequencies; // Provider baseband Hz, upper endpoint excluded.
    double inputRateHz = 0, analysisRateHz = 0, binHz = 0;
    double requiredSeconds = 0, noiseBandwidthHz = 0;
    int points = 0, analysisSamples = 0, decimationStages = 0;
    std::uint64_t decimation = 1, inputSamples = 0;
    bool valid = false;
    std::string reason;
};
struct SpectralFrame {
    std::uint64_t id = 0;
    TimeRange providerSamples, sourceSamples;
    SampleIndex sourceCenter = 0;
    FrequencyRange frequencies;
    double binHz = 0;
    std::vector<float> linearPower;
    double frequencyAt(std::size_t bin) const { return frequencies.lowerHz + bin * binHz; }
    std::size_t binAt(double frequency) const {
        if (linearPower.empty() || !(binHz > 0)) return 0;
        return static_cast<std::size_t>(std::clamp(std::llround((frequency - frequencies.lowerHz) / binHz),
            0LL, static_cast<long long>(linearPower.size() - 1)));
    }
    float dbAt(std::size_t bin) const {
        return bin < linearPower.size() ? static_cast<float>(10 * std::log10(std::max(1e-20f, linearPower[bin]))) : NAN;
    }
};
struct SpectrogramData {
    SpectralAnalysisPlan plan;
    TimeRange providerView, sourceView;
    std::vector<std::shared_ptr<const SpectralFrame>> frames;
    std::string error;
    std::size_t bytes() const {
        std::size_t result = 0;
        for (const auto& frame : frames) result += sizeof(SpectralFrame) + frame->linearPower.size() * sizeof(float);
        return result;
    }
    const SpectralFrame* frameAt(SampleIndex sample) const {
        if (frames.empty() || sample < sourceView.begin || sample >= sourceView.end) return nullptr;
        const auto next = std::lower_bound(frames.begin(), frames.end(), sample,
            [](const auto& frame, SampleIndex index) { return frame->sourceCenter < index; });
        if (next == frames.begin()) return next->get();
        if (next == frames.end()) return frames.back().get();
        const auto before = std::prev(next);
        return sample - (*before)->sourceCenter <= (*next)->sourceCenter - sample ? before->get() : next->get();
    }
};
struct LinkedCursorState {
    bool pinned = false, framePsd = false;
    SampleIndex sourceSample = 0;
    double frequencyHz = 0; // RF for files; baseband for channels.
    std::uint64_t selectedFrame = 0;
};
} // namespace signalstudio
