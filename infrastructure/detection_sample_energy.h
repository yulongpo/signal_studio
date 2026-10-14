#pragma once

#include <QString>
#include <complex>
#include <span>
#include <vector>

#include "infrastructure/detection_source.h"

namespace signalstudio {

// Exact-length, zero-phase FFT filtering of one physical search context.
// This is independent CPU DSP. No padding or resampling changes its length.
bool detectionBandEnergy(std::vector<std::complex<float>> samples,
                         double sampleRateHz, double relativeCenterHz,
                         double bandwidthHz, std::vector<float>& output,
                         std::uint64_t workingByteBudget, QString& error,
                         const DetectionSourceCancel& cancelled = {});

// Exposed for numerical verification; transforms in place and normalizes the
// inverse. Invalid length/resource limits fail before changing the data.
bool detectionComplexTransform(std::span<std::complex<float>> samples,
                               bool inverse, std::uint64_t workingByteBudget,
                               QString& error,
                               const DetectionSourceCancel& cancelled = {});

}  // namespace signalstudio
