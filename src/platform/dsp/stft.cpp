#include "signal_studio/dsp/stft.hpp"

#include <cmath>

namespace signal::dsp {

namespace {
core::Status dsp_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, reason}, std::move(message));
}
} // namespace

StftProcessor::StftProcessor(IFftBackend& backend) : backend_(backend) {}

core::Result<StftResult> StftProcessor::process(const data::SignalSlice& slice, double sample_rate_hz,
                                                const StftRequest& request) {
  if (sample_rate_hz <= 0.0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "sample rate must be positive");
  }
  if (request.nfft == 0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "nfft must be positive");
  }
  if (request.hop_samples == 0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "hop must be positive");
  }
  const std::uint64_t total = slice.size();
  if (total < request.nfft) {
    return dsp_failure(core::ErrorReason::invalid_argument, "signal shorter than one FFT frame");
  }
  WindowSpec wspec;
  wspec.type = request.window;
  wspec.length = request.nfft;
  wspec.symmetric = false;
  auto window_result = generate_window(wspec);
  if (!window_result.ok()) {
    return core::Status(window_result.error());
  }
  const std::vector<double>& window = *window_result;

  const std::uint64_t max_start = total - request.nfft;
  const std::uint64_t frame_count = max_start / request.hop_samples + 1;
  const bool real_input = (slice.kind() == data::SignalKind::real);
  const std::uint64_t freq_count = real_input ? (request.nfft / 2 + 1) : request.nfft;

  StftResult result;
  result.nfft = request.nfft;
  result.hop_samples = request.hop_samples;
  result.sample_rate_hz = sample_rate_hz;
  result.frame_count = frame_count;
  result.freq_count = freq_count;
  result.magnitude = request.output_magnitude;
  result.time_bins.resize(static_cast<std::size_t>(frame_count));
  result.freq_bins.resize(static_cast<std::size_t>(freq_count));
  result.matrix.assign(static_cast<std::size_t>(frame_count * freq_count), 0.0);

  for (std::uint64_t k = 0; k < freq_count; ++k) {
    result.freq_bins[static_cast<std::size_t>(k)] =
        static_cast<double>(k) * sample_rate_hz / static_cast<double>(request.nfft);
  }

  std::vector<data::ComplexSample> frame(static_cast<std::size_t>(request.nfft));
  for (std::uint64_t f = 0; f < frame_count; ++f) {
    const std::uint64_t start = f * request.hop_samples;
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
    result.time_bins[static_cast<std::size_t>(f)] = static_cast<double>(start + request.nfft / 2) / sample_rate_hz;
    for (std::uint64_t k = 0; k < freq_count; ++k) {
      const auto& bin = fft_result->bins[static_cast<std::size_t>(k)];
      const double power = bin.real * bin.real + bin.imag * bin.imag;
      result.matrix[static_cast<std::size_t>(f * freq_count + k)] = request.output_magnitude ? std::sqrt(power) : power;
    }
  }
  result.provenance = backend_.provenance();
  return result;
}

} // namespace signal::dsp
