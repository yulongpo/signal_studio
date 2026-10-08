#include "ui/charts/display_sampling.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace signalstudio::display {
std::vector<TracePoint> extremaEnvelope(std::span<const float> samples, std::size_t columns) {
    std::vector<TracePoint> result;
    if (columns == 0 || samples.empty()) return result;
    columns = std::min(columns, samples.size());
    result.reserve(std::min(samples.size(), columns * 2 + 2));
    const auto append = [&](std::size_t index) {
        if (std::isfinite(samples[index]) && (result.empty() || result.back().index != index))
            result.push_back({index, samples[index]});
    };
    const auto first = std::find_if(samples.begin(), samples.end(), [](float v) { return std::isfinite(v); });
    if (first == samples.end()) return result;
    append(static_cast<std::size_t>(first - samples.begin()));
    // Quotient/remainder partitioning avoids index*columns overflow.
    const auto base = samples.size() / columns, extra = samples.size() % columns;
    std::size_t begin = 0, remainder = 0;
    for (std::size_t column = 0; column < columns; ++column) {
        auto end = begin + base; remainder += extra;
        if (remainder >= columns) { ++end; remainder -= columns; }
        auto minimum = end, maximum = end;
        for (auto index = begin; index < end; ++index) {
            if (!std::isfinite(samples[index])) continue;
            if (minimum == end || samples[index] < samples[minimum]) minimum = index;
            if (maximum == end || samples[index] > samples[maximum]) maximum = index;
        }
        if (minimum != end) {
            append(std::min(minimum, maximum)); append(std::max(minimum, maximum));
        }
        begin = end;
    }
    const auto last = std::find_if(samples.rbegin(), samples.rend(), [](float v) { return std::isfinite(v); });
    append(samples.size() - 1 - static_cast<std::size_t>(last - samples.rbegin()));
    return result;
}
std::vector<float> peakReduce2D(std::span<const float> samples, int sw, int sh, int tw, int th) {
    if (sw <= 0 || sh <= 0 || tw <= 0 || th <= 0 || tw > sw || th > sh) return {};
    const auto width = static_cast<std::size_t>(sw), height = static_cast<std::size_t>(sh);
    if (height > std::numeric_limits<std::size_t>::max() / width || samples.size() < width * height) return {};
    std::vector<float> result(static_cast<std::size_t>(tw) * th, std::numeric_limits<float>::quiet_NaN());
    for (int y = 0; y < th; ++y) for (int x = 0; x < tw; ++x) {
        const auto x0 = static_cast<std::size_t>(x) * width / tw, x1 = static_cast<std::size_t>(x + 1) * width / tw;
        const auto y0 = static_cast<std::size_t>(y) * height / th, y1 = static_cast<std::size_t>(y + 1) * height / th;
        float peak = -std::numeric_limits<float>::infinity();
        for (auto sy = y0; sy < y1; ++sy) for (auto sx = x0; sx < x1; ++sx)
            if (std::isfinite(samples[sy * width + sx])) peak = std::max(peak, samples[sy * width + sx]);
        if (std::isfinite(peak)) result[static_cast<std::size_t>(y) * tw + x] = peak;
    }
    return result;
}
} // namespace signalstudio::display
