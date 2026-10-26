#pragma once

#include <QString>
#include <complex>
#include <functional>
#include <span>
#include <vector>

#include "infrastructure/detection_source.h"

namespace signalstudio {

using DetectionFilteredSampleObserver = std::function<bool(
    std::uint64_t, std::span<const std::complex<float>>, QString&)>;

// Exact-length, zero-phase FFT filtering of one physical search context.
// This is independent CPU DSP. No padding or resampling changes its length.
bool detectionBandEnergy(std::vector<std::complex<float>> samples,
                         double sampleRateHz, double relativeCenterHz,
                         double bandwidthHz, std::vector<float>& output,
                         std::uint64_t workingByteBudget, QString& error,
                         const DetectionSourceCancel& cancelled = {},
                         const DetectionFilteredSampleObserver& filteredObserver = {});

// Exposed for numerical verification; transforms in place and normalizes the
// inverse. Invalid length/resource limits fail before changing the data.
bool detectionComplexTransform(std::span<std::complex<float>> samples,
                               bool inverse, std::uint64_t workingByteBudget,
                               QString& error,
                               const DetectionSourceCancel& cancelled = {});

// Fixed-workspace FIR backend only. Different float32 operation order from
// LegacyExact; its callers must retain versioned numerical comparisons.
bool detectionBoundedRadixTwoTransform(std::vector<std::complex<float>>& samples,
    bool inverse,std::uint64_t workingByteBudget,QString& error,
    const DetectionSourceCancel& cancelled = {});

}  // namespace signalstudio
