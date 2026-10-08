#include "ui/charts/display_sampling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

using namespace signalstudio::display;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkOrderedFinite(const std::vector<TracePoint>& trace) {
    for (std::size_t index = 0; index < trace.size(); ++index) {
        check(std::isfinite(trace[index].value), "Display trace must not emit NaN or infinity");
        if (index) check(trace[index - 1].index < trace[index].index,
                         "Display trace must preserve source order without duplicate indices");
    }
}

bool containsPoint(const std::vector<TracePoint>& trace, std::size_t index, float value) {
    return std::any_of(trace.begin(), trace.end(), [=](const TracePoint& point) {
        return point.index == index && point.value == value;
    });
}

void testMillionSampleBipolarSpikes() {
    std::vector<float> source(1'000'000, 0);
    // Adjacent opposite spikes would both disappear with representative-point
    // or mean sampling, even though they fit inside a single display column.
    source[117'123] = 97;
    source[117'124] = -131;
    source[999'998] = 43;
    constexpr std::size_t columns = 1024;
    const auto trace = extremaEnvelope(source, columns);
    checkOrderedFinite(trace);
    check(trace.size() <= 2 * columns + 2, "Million-sample trace must be bounded by display width");
    check(trace.front().index == 0 && trace.back().index == source.size() - 1,
          "Reduced trace must retain the source endpoints");
    check(containsPoint(trace, 117'123, 97) && containsPoint(trace, 117'124, -131),
          "A single display column must retain adjacent positive and negative spikes");
    check(containsPoint(trace, 999'998, 43), "A spike close to the final sample must remain visible");
    check(source[117'123] == 97 && source[117'124] == -131,
          "Display reduction must preserve the immutable source amplitudes");
}

void testNarrowPsdLine() {
    std::vector<float> psd(8192, -92);
    psd[4073] = -3;
    const auto trace = extremaEnvelope(psd, 117);
    checkOrderedFinite(trace);
    check(containsPoint(trace, 4073, -3), "A one-bin PSD line must survive coarse display reduction");
    const auto strongest = std::max_element(trace.begin(), trace.end(),
        [](const TracePoint& left, const TracePoint& right) { return left.value < right.value; });
    check(strongest != trace.end() && strongest->value == -3,
          "Display PSD peak must retain its amplitude rather than a bucket mean");
}

void testTraceBoundaryAndNonfiniteData() {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();
    const std::array source{nan, 3.f, 2.f, infinity, -5.f, 9.f, nan};
    const auto coarse = extremaEnvelope(source, 1);
    checkOrderedFinite(coarse);
    check(coarse.front().index == 1 && coarse.back().index == 5,
          "First and last finite samples must survive invalid endpoints");
    check(containsPoint(coarse, 1, 3) && containsPoint(coarse, 4, -5) && containsPoint(coarse, 5, 9),
          "One-column trace must retain extrema and finite endpoints without duplication");
    check(coarse.size() <= 4, "One-column trace must obey the extrema-plus-endpoints bound");
    const auto uncompressed = extremaEnvelope(source, source.size());
    checkOrderedFinite(uncompressed);
    check(uncompressed.size() == 4 && containsPoint(uncompressed, 2, 2),
          "A trace that fits the display must retain every finite source sample");
    const std::array equal{4.f, 4.f, 4.f};
    const auto identity = extremaEnvelope(equal, 100);
    check(identity.size() == equal.size() && identity.front().index == 0 && identity.back().index == 2,
          "Equal-valued samples fitting the display must retain distinct source indices");
    const std::array invalid{nan, infinity, -infinity};
    check(extremaEnvelope(invalid, 10).empty(), "Entirely nonfinite trace must be empty");
    check(extremaEnvelope(source, 0).empty() && extremaEnvelope(std::span<const float>{}, 10).empty(),
          "Zero display width and empty input must return empty traces");
}

void testTwoDimensionalRidgeAndPulse() {
    constexpr int width = 64, height = 32;
    std::vector<float> source(width * height, -98);
    for (int row = 0; row < height; ++row) source[row * width + 47] = -7;
    source[20 * width + 12] = 6;
    const auto reduced = peakReduce2D(source, width, height, 8, 4);
    check(reduced.size() == 32, "Reduced spectrogram dimensions must match requested display bins");
    for (int row = 0; row < 4; ++row)
        check(reduced[row * 8 + 5] == -7, "A one-pixel spectral ridge must survive in every reduced time row");
    check(reduced[2 * 8 + 1] == 6, "A one-pixel time-frequency pulse must retain its full peak amplitude");
    check(reduced[0] == -98, "Peak reduction must preserve a uniform noise floor away from signals");

    std::vector<float> edge(7 * 5, -100);
    edge.front() = -2;
    edge.back() = -1;
    const auto uneven = peakReduce2D(edge, 7, 5, 3, 2);
    check(uneven.size() == 6 && uneven.front() == -2 && uneven.back() == -1,
          "Uneven display reduction must include first and final source pixels");
}

void testTwoDimensionalInvalidData() {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();
    const std::array mixed{nan, infinity, -infinity, -30.f};
    const auto finite = peakReduce2D(mixed, 2, 2, 1, 1);
    check(finite.size() == 1 && finite[0] == -30,
          "A finite spectrogram peak must survive adjacent NaN and infinities");
    const std::array invalid{nan, infinity, -infinity, nan};
    const auto missing = peakReduce2D(invalid, 2, 2, 1, 1);
    check(missing.size() == 1 && std::isnan(missing[0]),
          "An entirely nonfinite source block must produce NaN rather than a fabricated signal");
    check(peakReduce2D(mixed, 0, 2, 1, 1).empty() &&
          peakReduce2D(mixed, 2, -1, 1, 1).empty() &&
          peakReduce2D(mixed, 2, 2, 0, 1).empty() &&
          peakReduce2D(mixed, 2, 2, 1, -1).empty(),
          "Invalid source or target dimensions must return empty data");
    check(peakReduce2D(std::span<const float>(mixed).first(3), 2, 2, 1, 1).empty(),
          "Short spectrogram input must be rejected before reading missing pixels");
    check(peakReduce2D(mixed, 2, 2, 3, 1).empty() && peakReduce2D(mixed, 2, 2, 1, 3).empty(),
          "Display reduction must reject enlargement in either dimension");
    check(peakReduce2D(std::span<const float>{}, std::numeric_limits<int>::max(),
                      std::numeric_limits<int>::max(), 1, 1).empty(),
          "Huge declared dimensions with missing input must not allocate or read a frame");
}

} // namespace

int main() {
    try {
        testMillionSampleBipolarSpikes();
        std::cout << "PASS million-sample bipolar spikes and bounded trace size\n";
        testNarrowPsdLine();
        std::cout << "PASS one-bin PSD line and unchanged peak amplitude\n";
        testTraceBoundaryAndNonfiniteData();
        std::cout << "PASS trace endpoints, source order and nonfinite filtering\n";
        testTwoDimensionalRidgeAndPulse();
        std::cout << "PASS single-pixel ridges, pulses and uneven frame boundaries\n";
        testTwoDimensionalInvalidData();
        std::cout << "PASS invalid dimensions, nonfinite blocks and enlargement rejection\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
