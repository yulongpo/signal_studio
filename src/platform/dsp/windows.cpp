#include "signal_studio/dsp/windows.hpp"

#include <cmath>
#include <numbers>

namespace signal::dsp {

std::string_view to_string(WindowType type) noexcept {
  switch (type) {
    case WindowType::rectangular: return "rectangular";
    case WindowType::hann: return "hann";
    case WindowType::hamming: return "hamming";
    case WindowType::blackman: return "blackman";
    case WindowType::blackman_harris: return "blackman-harris";
    case WindowType::flattop: return "flattop";
  }
  return "unknown";
}

bool is_known_window_type(WindowType type) noexcept {
  return type == WindowType::rectangular || type == WindowType::hann || type == WindowType::hamming ||
         type == WindowType::blackman || type == WindowType::blackman_harris || type == WindowType::flattop;
}

core::Result<std::vector<double>> generate_window(const WindowSpec& spec) {
  if (!is_known_window_type(spec.type)) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, core::ErrorReason::invalid_argument},
                                 "unknown window type");
  }
  if (spec.length == 0) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, core::ErrorReason::invalid_argument},
                                 "window length must be positive");
  }
  const auto n = static_cast<std::uint64_t>(spec.length);
  std::vector<double> w;
  w.reserve(static_cast<std::size_t>(n));
  // Periodic (DFT-even) windows divide by N; symmetric windows divide by (N-1).
  const double span = spec.symmetric ? static_cast<double>(n - 1) : static_cast<double>(n);
  for (std::uint64_t i = 0; i < n; ++i) {
    const double t = (span > 0) ? (static_cast<double>(i) / span) : 0.0;
    double value = 1.0;
    switch (spec.type) {
      case WindowType::rectangular:
        value = 1.0;
        break;
      case WindowType::hann:
        value = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * t);
        break;
      case WindowType::hamming:
        value = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * t);
        break;
      case WindowType::blackman:
        value = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * t) +
               0.08 * std::cos(4.0 * std::numbers::pi * t);
        break;
      case WindowType::blackman_harris:
        value = 0.35875 - 0.48829 * std::cos(2.0 * std::numbers::pi * t) +
               0.14128 * std::cos(4.0 * std::numbers::pi * t) -
               0.01168 * std::cos(6.0 * std::numbers::pi * t);
        break;
      case WindowType::flattop:
        value = 0.21557895 - 0.41663158 * std::cos(2.0 * std::numbers::pi * t) +
               0.277263158 * std::cos(4.0 * std::numbers::pi * t) -
               0.083578947 * std::cos(6.0 * std::numbers::pi * t) +
               0.006947368 * std::cos(8.0 * std::numbers::pi * t);
        break;
    }
    w.push_back(value);
  }
  return w;
}

double window_coherent_gain(std::string_view window_name) noexcept {
  if (window_name == "rectangular") return 1.0;
  if (window_name == "hann") return 0.5;
  if (window_name == "hamming") return 0.54;
  if (window_name == "blackman") return 0.42;
  if (window_name == "blackman-harris") return 0.35875;
  if (window_name == "flattop") return 0.21557895;
  return 1.0;
}

double window_enbw_bins(WindowType type) noexcept {
  switch (type) {
    case WindowType::rectangular: return 1.0;
    case WindowType::hann: return 1.5;
    case WindowType::hamming: return 1.3628;
    case WindowType::blackman: return 1.7268;
    case WindowType::blackman_harris: return 2.0044;
    case WindowType::flattop: return 3.7683;
  }
  return 1.0;
}

}  // namespace signal::dsp
