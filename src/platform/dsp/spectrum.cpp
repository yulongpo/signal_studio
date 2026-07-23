#include "signal_studio/dsp/spectrum.hpp"

#include <algorithm>
#include <cmath>

namespace signal::dsp {

namespace {
core::Status dsp_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, reason}, std::move(message));
}

double window_sum_sq(const std::vector<double>& w) {
  double s = 0.0;
  for (double v : w) s += v * v;
  return s;
}
}  // namespace

WelchPsdEstimator::WelchPsdEstimator(IFftBackend& backend) : backend_(backend) {}

core::Result<PsdResult>
WelchPsdEstimator::process(const data::SignalSlice& slice, double sample_rate_hz, const PsdRequest& request) {
  if (sample_rate_hz <= 0.0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "sample rate must be positive");
  }
  if (request.nfft == 0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "nfft must be positive");
  }
  if (request.overlap_samples >= request.nfft) {
    return dsp_failure(core::ErrorReason::invalid_argument, "overlap must be less than nfft");
  }
  const std::uint64_t total = slice.size();
  if (total < request.nfft) {
    return dsp_failure(core::ErrorReason::invalid_argument, "signal shorter than one FFT frame");
  }
  WindowSpec wspec;
  wspec.type = request.window;
  wspec.length = request.nfft;
  wspec.symmetric = false;  // periodic (DFT-even) for spectral analysis
  auto window_result = generate_window(wspec);
  if (!window_result.ok()) {
    return core::Status(window_result.error());
  }
  const std::vector<double>& window = *window_result;
  const double win_sum_sq = window_sum_sq(window);
  if (win_sum_sq <= 0.0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "window has zero energy");
  }

  const std::uint64_t hop = request.nfft - request.overlap_samples;
  const std::uint64_t max_start = (total >= request.nfft) ? (total - request.nfft) : 0;
  const std::uint64_t frame_count = max_start / hop + 1;

  std::vector<double> accumulated(static_cast<std::size_t>(request.nfft), 0.0);
  const bool real_input = (slice.kind() == data::SignalKind::real);

  std::vector<data::ComplexSample> frame(static_cast<std::size_t>(request.nfft));
  for (std::uint64_t f = 0; f < frame_count; ++f) {
    const std::uint64_t start = f * hop;
    if (real_input) {
      const auto real_values = slice.real_values();
      for (std::uint64_t i = 0; i < request.nfft; ++i) {
        frame[i].real = real_values[static_cast<std::size_t>(start + i)] * window[static_cast<std::size_t>(i)];
        frame[i].imag = 0.0;
      }
    } else {
      const auto complex_values = slice.complex_values();
      for (std::uint64_t i = 0; i < request.nfft; ++i) {
        const auto& s = complex_values[static_cast<std::size_t>(start + i)];
        frame[i].real = s.real * window[static_cast<std::size_t>(i)];
        frame[i].imag = s.imag * window[static_cast<std::size_t>(i)];
      }
    }
    auto fft_result = fft(frame, backend_);
    if (!fft_result.ok()) {
      return core::Status(fft_result.error());
    }
    for (std::uint64_t k = 0; k < request.nfft; ++k) {
      const auto& bin = fft_result->bins[static_cast<std::size_t>(k)];
      accumulated[static_cast<std::size_t>(k)] += bin.real * bin.real + bin.imag * bin.imag;
    }
  }

  // Average and normalize.
  std::vector<double> power(static_cast<std::size_t>(request.nfft));
  const double density_scale = (request.scaling == SpectrumScaling::density)
                                   ? (1.0 / (sample_rate_hz * win_sum_sq))
                                   : 1.0;
  for (std::uint64_t k = 0; k < request.nfft; ++k) {
    power[static_cast<std::size_t>(k)] =
        accumulated[static_cast<std::size_t>(k)] * density_scale / static_cast<double>(frame_count);
  }

  PsdResult result;
  result.nfft = request.nfft;
  result.frame_count = frame_count;
  result.sample_rate_hz = sample_rate_hz;
  result.enbw_hz = window_enbw_bins(request.window) * (sample_rate_hz / static_cast<double>(request.nfft));
  result.window = request.window;
  result.scaling = request.scaling;

  const bool one_sided = request.one_sided && real_input;
  if (one_sided) {
    const std::uint64_t out_count = request.nfft / 2 + 1;
    result.frequencies_hz.resize(static_cast<std::size_t>(out_count));
    result.power.resize(static_cast<std::size_t>(out_count));
    for (std::uint64_t k = 0; k < out_count; ++k) {
      result.frequencies_hz[static_cast<std::size_t>(k)] =
          static_cast<double>(k) * sample_rate_hz / static_cast<double>(request.nfft);
      double p = power[static_cast<std::size_t>(k)];
      // Fold negative-frequency power into the one-sided spectrum (double except DC/Nyquist).
      if (k != 0 && k != request.nfft / 2) {
        p *= 2.0;
      }
      result.power[static_cast<std::size_t>(k)] = p;
    }
  } else {
    result.frequencies_hz.resize(static_cast<std::size_t>(request.nfft));
    result.power.resize(static_cast<std::size_t>(request.nfft));
    for (std::uint64_t k = 0; k < request.nfft; ++k) {
      result.frequencies_hz[static_cast<std::size_t>(k)] =
          static_cast<double>(k) * sample_rate_hz / static_cast<double>(request.nfft);
      result.power[static_cast<std::size_t>(k)] = power[static_cast<std::size_t>(k)];
    }
  }
  result.provenance = backend_.provenance();
  return result;
}

std::vector<double> to_db_hz(std::span<const double> power, double floor_db) {
  std::vector<double> out(power.size());
  const double floor_lin = std::pow(10.0, floor_db / 10.0);
  for (std::size_t i = 0; i < power.size(); ++i) {
    const double p = std::max(power[i], floor_lin);
    out[i] = 10.0 * std::log10(p);
  }
  return out;
}

}  // namespace signal::dsp
