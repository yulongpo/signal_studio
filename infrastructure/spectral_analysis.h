#pragma once
#include "domain/spectral_data.h"
#include <complex>
#include <functional>

namespace signalstudio {
using SpectralCancel = std::function<bool()>;
struct SpectralSource {
    double sampleRateHz = 0;
    // Bounded reads in the provider's integer sample grid.
    std::function<bool(TimeRange, std::vector<std::complex<float>>&, const SpectralCancel&)> read;
    std::function<TimeRange(TimeRange)> sourceRange;
};
SpectralAnalysisPlan makeSpectralAnalysisPlan(double sampleRateHz, FrequencyRange frequencies, int points);
std::shared_ptr<SpectrogramData> analyzeSpectrogram(const SpectralSource& source, TimeRange view,
    FrequencyRange frequencies, int points, int maximumFrames, const SpectralCancel& cancelled = {});
std::shared_ptr<SpectralFrame> averageSpectrum(const SpectrogramData& data);
// A bounded raster is a display reduction; all N analysis bins stay in frames.
std::vector<float> spectralRaster(const SpectrogramData& data, int width, int height, MainMode mode);
std::vector<float> spectrumDb(const SpectralFrame& frame);
} // namespace signalstudio
