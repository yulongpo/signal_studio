#include "signal_studio/visualization/data_series.hpp"

namespace signal::visualization {

RealSeries::RealSeries(std::string name, std::vector<double> samples, double sample_rate_hz)
    : name_(std::move(name)), samples_(std::move(samples)), sample_rate_hz_(sample_rate_hz) {}

SpectrumSeries::SpectrumSeries(std::string name, std::vector<double> frequencies_hz,
                               std::vector<double> power_db, double sample_rate_hz)
    : name_(std::move(name)),
      frequencies_(std::move(frequencies_hz)),
      power_db_(std::move(power_db)),
      sample_rate_hz_(sample_rate_hz) {}

SpectrogramSeries::SpectrogramSeries(std::string name, std::vector<double> time_bins,
                                     std::vector<double> freq_bins, std::vector<double> magnitudes_db,
                                     std::uint64_t frame_count, std::uint64_t freq_count)
    : name_(std::move(name)),
      time_bins_(std::move(time_bins)),
      freq_bins_(std::move(freq_bins)),
      magnitudes_(std::move(magnitudes_db)),
      frame_count_(frame_count),
      freq_count_(freq_count) {}

ComplexSeries::ComplexSeries(std::string name, std::vector<double> real, std::vector<double> imag,
                             double sample_rate_hz)
    : name_(std::move(name)), real_(std::move(real)), imag_(std::move(imag)), sample_rate_hz_(sample_rate_hz) {}

}  // namespace signal::visualization
