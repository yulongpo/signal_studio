#pragma once
#include <cstddef>
#include <span>
#include <vector>

namespace signalstudio::display {
struct TracePoint { std::size_t index; float value; };
// Retain each pixel bucket's extrema in original source order, plus endpoints.
// Indices refer to the immutable source; this is display reduction only.
std::vector<TracePoint> extremaEnvelope(std::span<const float> samples, std::size_t columns);
// Keep peaks of every non-overlapping source rectangle. No interpolation or
// enlargement: invalid dimensions, short input, or target>source return empty.
// An entirely non-finite rectangle produces NaN.
std::vector<float> peakReduce2D(std::span<const float> samples, int sourceWidth, int sourceHeight,
                              int targetWidth, int targetHeight);
} // namespace signalstudio::display
