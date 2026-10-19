#pragma once
#include "domain/spectral_data.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <span>

namespace signalstudio {
inline constexpr double maximumPsdSpan = 10300.0;
struct PowerDisplayRange {
    double referenceLevelDb = 0;
    double dynamicRangeDb = 80;
    double lowerDb() const { return referenceLevelDb - dynamicRangeDb; }
    bool valid() const {
        return std::isfinite(referenceLevelDb) && std::isfinite(dynamicRangeDb) &&
            referenceLevelDb >= -200 && referenceLevelDb <= 100 && dynamicRangeDb > 0 && dynamicRangeDb <= 10000;
    }
};
inline bool validPsdRange(double lower, double upper) {
    return std::isfinite(lower) && std::isfinite(upper) && lower < upper && upper - lower <= maximumPsdSpan;
}
struct PowerFitResult {
    PowerDisplayRange range;
    bool valid = false, limited = false;
};
struct PowerAnalysisSnapshot {
    std::shared_ptr<const SpectrogramData> heatmap;
    std::shared_ptr<const SpectralFrame> psd;
    bool ready = false;
};
// Scan linear power without per-bin logarithms or copies of the full matrix.
inline PowerFitResult fitPowerDisplayRange(const SpectrogramData& heatmap, const SpectralFrame* psd) {
    double minimum = std::numeric_limits<double>::infinity(), maximum = 0;
    const auto include = [&](std::span<const float> values) {
        for (const auto value : values) if (std::isfinite(value) && value > 0) {
            minimum = std::min(minimum, static_cast<double>(value));
            maximum = std::max(maximum, static_cast<double>(value));
        }
    };
    for (const auto& frame : heatmap.frames) if (frame) include(frame->linearPower);
    if (psd) include(psd->linearPower);
    if (!(maximum > 0)) return {};
    const double lower = 10 * std::log10(std::max(1e-20, minimum)) - 3;
    const double upper = 10 * std::log10(std::max(1e-20, maximum)) + 3;
    PowerFitResult result;
    result.range.referenceLevelDb = std::clamp(upper, -200.0, 100.0);
    result.range.dynamicRangeDb = std::clamp(result.range.referenceLevelDb - lower, .001, 10000.0);
    result.limited = std::abs(result.range.referenceLevelDb - upper) > 1e-9 || std::abs(result.range.lowerDb() - lower) > 1e-9;
    result.valid = result.range.valid();
    return result;
}
} // namespace signalstudio
