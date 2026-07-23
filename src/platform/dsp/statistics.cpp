#include "signal_studio/dsp/statistics.hpp"

#include <cmath>

namespace signal::dsp {

namespace {
template <typename T>
double sum_magnitude_peak(const T* values, std::uint64_t count, double& min_out, double& max_out,
                          std::uint64_t& peak_index_out) {
  double sum = 0.0;
  double peak = 0.0;
  std::uint64_t peak_index = 0;
  min_out = 0.0;
  max_out = 0.0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const double v = values[i];
    sum += v;
    if (i == 0 || v < min_out)
      min_out = v;
    if (i == 0 || v > max_out)
      max_out = v;
    const double a = std::fabs(v);
    if (a > peak) {
      peak = a;
      peak_index = i;
    }
  }
  peak_index_out = peak_index;
  return sum;
}
} // namespace

core::Result<TimeDomainStats> compute_real_stats(const data::SignalSlice& slice) {
  if (slice.kind() != data::SignalKind::real) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, core::ErrorReason::invalid_argument},
                                 "compute_real_stats requires a real-valued slice");
  }
  const auto values = slice.real_values();
  const std::uint64_t count = slice.size();
  TimeDomainStats stats;
  stats.count = count;
  if (count == 0) {
    return stats;
  }
  double min_v = 0.0;
  double max_v = 0.0;
  std::uint64_t peak_index = 0;
  const double sum = sum_magnitude_peak<double>(values.data(), count, min_v, max_v, peak_index);
  const double mean = sum / static_cast<double>(count);
  double sq_sum = 0.0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const double d = values[i] - mean;
    sq_sum += d * d;
  }
  const double variance = sq_sum / static_cast<double>(count);
  stats.mean = mean;
  stats.dc_offset = mean;
  stats.variance = variance;
  stats.rms = std::sqrt(variance + mean * mean);
  stats.peak = std::fabs(values[static_cast<std::size_t>(peak_index)]);
  stats.peak_index = peak_index;
  stats.min = min_v;
  stats.max = max_v;
  return stats;
}

core::Result<ComplexStats> compute_complex_stats(const data::SignalSlice& slice) {
  if (slice.kind() != data::SignalKind::complex) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, core::ErrorReason::invalid_argument},
                                 "compute_complex_stats requires a complex-valued slice");
  }
  const auto values = slice.complex_values();
  const std::uint64_t count = slice.size();
  ComplexStats stats;
  stats.count = count;
  if (count == 0) {
    return stats;
  }
  double mag_sum = 0.0;
  double mag_peak = 0.0;
  std::uint64_t mag_peak_index = 0;
  double real_sum = 0.0;
  double imag_sum = 0.0;
  double sin_sum = 0.0;
  double cos_sum = 0.0;
  std::vector<double> mags;
  mags.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    const double re = values[i].real;
    const double im = values[i].imag;
    real_sum += re;
    imag_sum += im;
    const double mag = std::sqrt(re * re + im * im);
    mags.push_back(mag);
    mag_sum += mag;
    if (mag > mag_peak) {
      mag_peak = mag;
      mag_peak_index = i;
    }
    sin_sum += std::sin(std::atan2(im, re));
    cos_sum += std::cos(std::atan2(im, re));
  }
  const double mean = mag_sum / static_cast<double>(count);
  double ac_sq = 0.0;
  double mag_sq_sum = 0.0;
  for (double m : mags) {
    const double d = m - mean;
    ac_sq += d * d;
    mag_sq_sum += m * m;
  }
  stats.magnitude_mean = mean;
  stats.magnitude_rms = std::sqrt(mag_sq_sum / static_cast<double>(count));
  stats.magnitude_peak = mag_peak;
  stats.magnitude_peak_index = mag_peak_index;
  stats.phase_mean_radians = std::atan2(sin_sum, cos_sum);
  stats.dc_real = real_sum / static_cast<double>(count);
  stats.dc_imag = imag_sum / static_cast<double>(count);
  stats.magnitude_ac_rms = std::sqrt(ac_sq / static_cast<double>(count));
  return stats;
}

} // namespace signal::dsp
