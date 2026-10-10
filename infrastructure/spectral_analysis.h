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
SpectralAnalysisPlan makeSpectralAnalysisPlan(double sampleRateHz, FrequencyRange frequencies, int points,
    const SpectralParameters& parameters);
std::shared_ptr<SpectrogramData> analyzeSpectrogram(const SpectralSource& source, TimeRange view,
    FrequencyRange frequencies, int points, int maximumFrames, const SpectralCancel& cancelled = {});
std::shared_ptr<SpectralFrame> averageSpectrum(const SpectrogramData& data);
std::shared_ptr<SpectrogramData> analyzeSpectrogram(const SpectralSource& source, TimeRange view,
    FrequencyRange frequencies, int points, int maximumFrames, const SpectrogramSettings& settings,
    int minimumFrames, const SpectralCancel& cancelled = {});
using SpectrumProgress = std::function<void(std::uint64_t, std::uint64_t, std::shared_ptr<const SpectralFrame>)>;
std::shared_ptr<SpectralFrame> analyzeSpectrum(const SpectralSource& source, TimeRange range,
    FrequencyRange frequencies, int points, const PsdSettings& settings, std::string& error,
    const SpectralCancel& cancelled = {}, const SpectrumProgress& progress = {});
std::vector<double> spectralWindow(SpectralWindow window, int length, double beta = 8.6);
std::vector<std::vector<double>> dpssWindows(int length, double timeBandwidth, int count, const SpectralCancel& cancelled = {});
bool validSpectralParameters(const SpectralParameters& parameters);
std::string spectralParameterKey(const SpectralParameters& parameters);
// A bounded raster is a display reduction; all N analysis bins stay in frames.
std::vector<float> spectralRaster(const SpectrogramData& data, int width, int height, MainMode mode);
std::vector<float> spectrumDb(const SpectralFrame& frame);
} // namespace signalstudio
