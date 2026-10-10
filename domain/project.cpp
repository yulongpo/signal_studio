#include "domain/project.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace signalstudio {

ViewRange fullRange(const FileMetadata& metadata) {
    const double bandwidth = metadata.effectiveBandwidthHz > 0 && std::isfinite(metadata.effectiveBandwidthHz) ?
        std::min(metadata.effectiveBandwidthHz, metadata.sampleRateHz) : metadata.sampleRateHz;
    return {{0, metadata.sampleCount},
            {metadata.centerFrequencyHz - bandwidth / 2,
             metadata.centerFrequencyHz + bandwidth / 2}};
}

ViewRange clampRange(ViewRange range, const FileMetadata& metadata, int stftSize, int psdSize) {
    (void)stftSize; (void)psdSize;
    const auto bounds = fullRange(metadata);
    const auto sampleCount = metadata.sampleCount;
    if (sampleCount == 0 || !std::isfinite(metadata.sampleRateHz) ||
        metadata.sampleRateHz <= 0 || !std::isfinite(metadata.centerFrequencyHz))
        return bounds;

    if (range.time.begin > range.time.end) std::swap(range.time.begin, range.time.end);
    auto width = std::min(range.time.end - range.time.begin, sampleCount);
    const SampleIndex minimumTime = 1;
    width = std::max(width, minimumTime);
    const auto midpoint = range.time.begin + (range.time.end - range.time.begin) / 2;
    auto begin = midpoint > width / 2 ? midpoint - width / 2 : 0;
    begin = std::min(begin, sampleCount - width);
    range.time = {begin, begin + width};

    if (!std::isfinite(range.frequency.lowerHz) || !std::isfinite(range.frequency.upperHz)) {
        range.frequency = bounds.frequency;
        return range;
    }
    if (range.frequency.lowerHz > range.frequency.upperHz)
        std::swap(range.frequency.lowerHz, range.frequency.upperHz);
    const long double lower = bounds.frequency.lowerHz;
    const long double upper = bounds.frequency.upperHz;
    const auto total = upper - lower;
    const auto minimumFrequency = std::min(total,
        std::max(static_cast<long double>(1e-9),
                 static_cast<long double>(std::numeric_limits<double>::epsilon()) *
                 std::max(1.0, std::abs(metadata.centerFrequencyHz)) * 8));
    auto frequencyWidth = std::clamp(static_cast<long double>(range.frequency.upperHz) -
        range.frequency.lowerHz, minimumFrequency, total);
    const auto center = static_cast<long double>(range.frequency.lowerHz) +
        (static_cast<long double>(range.frequency.upperHz) - range.frequency.lowerHz) / 2;
    const auto frequencyBegin = std::clamp(center - frequencyWidth / 2, lower, upper - frequencyWidth);
    range.frequency = {static_cast<double>(frequencyBegin),
                       static_cast<double>(frequencyBegin + frequencyWidth)};
    return range;
}

Mark* findMark(FileState& file, const std::string& id) {
    const auto found = std::find_if(file.marks.begin(), file.marks.end(),
        [&](const Mark& mark) { return mark.id == id; });
    return found == file.marks.end() ? nullptr : &*found;
}

const Mark* findMark(const FileState& file, const std::string& id) {
    const auto found = std::find_if(file.marks.begin(), file.marks.end(),
        [&](const Mark& mark) { return mark.id == id; });
    return found == file.marks.end() ? nullptr : &*found;
}

} // namespace signalstudio
