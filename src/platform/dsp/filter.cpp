#include "signal_studio/dsp/filter.hpp"

#include <cmath>
#include <numbers>

namespace signal::dsp {

namespace {
core::Status dsp_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, reason}, std::move(message));
}

double sinc(double x) noexcept {
  if (std::fabs(x) < 1e-12) return 1.0;
  return std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
}

/// Windowed-sinc lowpass at normalized cutoff fc in (0, 0.5) (cycles/sample).
core::Result<std::vector<double>> design_lowpass(std::uint64_t order, double fc, WindowType window_type) {
  const std::uint64_t taps = order + 1;
  WindowSpec wspec;
  wspec.type = window_type;
  wspec.length = taps;
  wspec.symmetric = true;
  auto window_result = generate_window(wspec);
  if (!window_result.ok()) {
    return core::Status(window_result.error());
  }
  std::vector<double> h(static_cast<std::size_t>(taps));
  const double m = static_cast<double>(order) / 2.0;
  double sum = 0.0;
  for (std::uint64_t i = 0; i < taps; ++i) {
    const double n = static_cast<double>(i) - m;
    const double v = 2.0 * fc * sinc(2.0 * fc * n) * (*window_result)[static_cast<std::size_t>(i)];
    h[static_cast<std::size_t>(i)] = v;
    sum += v;
  }
  // Normalize for unity DC gain.
  for (auto& v : h) v /= sum;
  return h;
}
}  // namespace

std::string_view to_string(FilterType type) noexcept {
  switch (type) {
    case FilterType::lowpass: return "lowpass";
    case FilterType::highpass: return "highpass";
    case FilterType::bandpass: return "bandpass";
    case FilterType::bandstop: return "bandstop";
  }
  return "unknown";
}

FirFilter::FirFilter(std::vector<double> coefficients, std::uint64_t order)
    : coefficients_(std::move(coefficients)), order_(order) {}

core::Result<FirFilter> FirFilter::design(const FilterSpec& spec) {
  if (spec.sample_rate_hz <= 0.0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "sample rate must be positive");
  }
  if (spec.order == 0 || (spec.order % 2) != 0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "FIR order must be a positive even number");
  }
  if (spec.cutoff_low_hz <= 0.0 || spec.cutoff_low_hz >= spec.sample_rate_hz / 2.0) {
    return dsp_failure(core::ErrorReason::invalid_argument, "cutoff_low out of (0, fs/2)");
  }
  const double nyquist = spec.sample_rate_hz / 2.0;
  const double fc_low = spec.cutoff_low_hz / spec.sample_rate_hz;
  if (spec.type == FilterType::lowpass) {
    auto h = design_lowpass(spec.order, fc_low, spec.window);
    if (!h.ok()) return core::Status(h.error());
    return FirFilter(std::move(*h), spec.order);
  }
  if (spec.type == FilterType::highpass) {
    auto h = design_lowpass(spec.order, fc_low, spec.window);
    if (!h.ok()) return core::Status(h.error());
    const std::uint64_t m = spec.order / 2;
    for (std::uint64_t i = 0; i < h->size(); ++i) {
      (*h)[i] = -(*h)[i];
    }
    (*h)[m] += 1.0;
    return FirFilter(std::move(*h), spec.order);
  }
  if (spec.cutoff_high_hz <= spec.cutoff_low_hz || spec.cutoff_high_hz >= nyquist) {
    return dsp_failure(core::ErrorReason::invalid_argument, "cutoff_high must satisfy low < high < fs/2");
  }
  const double fc_high = spec.cutoff_high_hz / spec.sample_rate_hz;
  auto lo = design_lowpass(spec.order, fc_low, spec.window);
  if (!lo.ok()) return core::Status(lo.error());
  auto hi = design_lowpass(spec.order, fc_high, spec.window);
  if (!hi.ok()) return core::Status(hi.error());
  if (spec.type == FilterType::bandpass) {
    // bp = lp(high) - lp(low)
    std::vector<double> h(lo->size());
    for (std::size_t i = 0; i < h.size(); ++i) {
      h[i] = (*hi)[i] - (*lo)[i];
    }
    return FirFilter(std::move(h), spec.order);
  }
  // bandstop = lp(low) + hp(high) = lp(low) + (delta - lp(high))
  const std::uint64_t m = spec.order / 2;
  std::vector<double> h(lo->size());
  for (std::size_t i = 0; i < h.size(); ++i) {
    h[i] = (*lo)[i] - (*hi)[i];
  }
  h[m] += 1.0;
  return FirFilter(std::move(h), spec.order);
}

std::uint64_t FirFilter::taps() const noexcept {
  return coefficients_.size();
}

const std::vector<double>& FirFilter::coefficients() const noexcept {
  return coefficients_;
}

core::Status FirFilter::process(std::span<const double> input, FilterState& state,
                                std::span<double> output) const {
  if (output.size() != input.size()) {
    return dsp_failure(core::ErrorReason::invalid_argument, "output size must equal input size");
  }
  const std::size_t taps = coefficients_.size();
  if (state.delays.size() != taps - 1) {
    state.delays.assign(taps - 1, 0.0);
  }
  for (std::size_t n = 0; n < input.size(); ++n) {
    double acc = coefficients_[0] * input[n];
    for (std::size_t k = 1; k < taps; ++k) {
      const double sample = (k - 1 < state.delays.size()) ? state.delays[k - 1] : 0.0;
      acc += coefficients_[k] * sample;
    }
    output[n] = acc;
    // Shift delay line: insert input[n] at front.
    for (std::size_t k = state.delays.size(); k-- > 1;) {
      state.delays[k] = state.delays[k - 1];
    }
    if (!state.delays.empty()) {
      state.delays[0] = input[n];
    }
  }
  return core::Status::success();
}

void FirFilter::reset(FilterState& state) const {
  state.delays.assign(coefficients_.size() - 1, 0.0);
}

}  // namespace signal::dsp
