#pragma once

#include <algorithm>
#include <cmath>

namespace signalstudio::chart_interaction {

struct LinearRange {
    double first = 0.0;
    double last = 1.0;
};

inline double fractionAt(double pixel, double origin, double extent) {
    if (!std::isfinite(pixel) || !std::isfinite(origin) || !std::isfinite(extent) || extent <= 0.0)
        return 0.0;
    return std::clamp((pixel - origin) / extent, 0.0, 1.0);
}

inline LinearRange zoomAround(LinearRange range, double factor, double anchorFraction) {
    const double span = range.last - range.first;
    if (!std::isfinite(span) || span <= 0.0 || !std::isfinite(factor) || factor <= 0.0)
        return range;
    const double anchor = range.first + std::clamp(anchorFraction, 0.0, 1.0) * span;
    const double nextSpan = span * factor;
    const double first = anchor - std::clamp(anchorFraction, 0.0, 1.0) * nextSpan;
    return {first, first + nextSpan};
}

inline LinearRange panByFraction(LinearRange range, double deltaFraction) {
    const double delta = (range.last - range.first) * deltaFraction;
    if (!std::isfinite(delta)) return range;
    return {range.first + delta, range.last + delta};
}

inline LinearRange selectFractions(LinearRange range, double firstFraction, double lastFraction) {
    const double low = std::clamp(std::min(firstFraction, lastFraction), 0.0, 1.0);
    const double high = std::clamp(std::max(firstFraction, lastFraction), 0.0, 1.0);
    if (high <= low) return range;
    const double span = range.last - range.first;
    return {range.first + low * span, range.first + high * span};
}

} // namespace signalstudio::chart_interaction
