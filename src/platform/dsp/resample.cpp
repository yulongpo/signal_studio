#include "signal_studio/dsp/resample.hpp"

#include "signal_studio/dsp/filter.hpp"
#include "signal_studio/dsp/windows.hpp"

#include <algorithm>
#include <cmath>

namespace signal::dsp {

namespace {
core::Status dsp_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, reason}, std::move(message));
}

double sinc(double x) noexcept {
  if (std::fabs(x) < 1e-12) return 1.0;
  return std::sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
}
}  // namespace

bool ResampleRatio::valid() const noexcept {
  return num > 0 && den > 0;
}

core::Result<std::vector<double>>
PolyphaseResampler::process(const ResampleRatio& ratio, std::span<const double> input) {
  if (!ratio.valid()) {
    return dsp_failure(core::ErrorReason::invalid_argument, "resample ratio must be positive");
  }
  if (input.empty()) {
    return std::vector<double>{};
  }
  const std::uint32_t L = ratio.num;
  const std::uint32_t M = ratio.den;
  if (L == M) {
    return std::vector<double>(input.begin(), input.end());
  }

  // Design the combined anti-alias + anti-image lowpass at the high rate (L * fs_in).
  // Cutoff = 1 / (2 * max(L, M)) cycles/high-rate-sample; gain = L.
  const std::uint32_t g = std::max(L, M);
  const double fc = 1.0 / (2.0 * static_cast<double>(g));
  const std::uint64_t taps_per_phase = 16;
  const std::uint64_t n_taps = taps_per_phase * L;
  const std::uint64_t order = n_taps - 1;
  WindowSpec wspec;
  wspec.type = WindowType::hann;
  wspec.length = n_taps;
  wspec.symmetric = true;
  auto window_result = generate_window(wspec);
  if (!window_result.ok()) {
    return core::Status(window_result.error());
  }
  std::vector<double> h(static_cast<std::size_t>(n_taps));
  const double mid = static_cast<double>(order) / 2.0;
  double sum = 0.0;
  for (std::uint64_t i = 0; i < n_taps; ++i) {
    const double n = static_cast<double>(i) - mid;
    const double v = 2.0 * fc * sinc(2.0 * fc * n) * (*window_result)[static_cast<std::size_t>(i)];
    h[static_cast<std::size_t>(i)] = v * static_cast<double>(L);  // gain L
    sum += v;
  }
  (void)sum;  // gain is set via L scaling; DC normalization handled by L factor on non-zero phase.

  // Polyphase decomposition: phase p uses h[p], h[p+L], h[p+2L], ...
  const std::uint64_t poly_taps = taps_per_phase;
  // y[k] = sum_{j=0}^{poly_taps-1} h[(k*M mod L) + j*L] * x[floor(k*M / L) - j]
  // Outputs from k=0 with zero initial state (standard polyphase resampler transient).
  std::vector<double> output;
  for (std::uint64_t k = 0;; ++k) {
    const std::uint64_t t = k * M;
    const std::uint64_t base = t / L;
    if (base >= input.size()) {
      break;  // no more input
    }
    const std::uint32_t p = static_cast<std::uint32_t>(t % L);
    double acc = 0.0;
    for (std::uint64_t j = 0; j < poly_taps; ++j) {
      if (j > base) break;  // x index would be negative -> zero contribution
      const std::uint64_t idx = base - j;
      const std::uint64_t coeff_idx = static_cast<std::uint64_t>(p) + j * L;
      acc += h[static_cast<std::size_t>(coeff_idx)] * input[static_cast<std::size_t>(idx)];
    }
    output.push_back(acc);
    if (output.size() > 100'000'000) {
      return dsp_failure(core::ErrorReason::internal_failure, "resampler output exceeded safety bound");
    }
  }
  return output;
}

}  // namespace signal::dsp
