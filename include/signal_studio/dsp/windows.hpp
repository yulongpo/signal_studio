#pragma once

#include "signal_studio/core/result.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace signal::dsp {

/// Approved window functions for spectral estimation. Selection follows the BL1.0 chart-display
/// and PSD/STFT specs; rectangular is retained for analytical reference only.
enum class WindowType : std::uint8_t {
  rectangular = 0,
  hann = 1,
  hamming = 2,
  blackman = 3,
  blackman_harris = 4,
  flattop = 5,
};

struct WindowSpec final {
  WindowType type{WindowType::hann};
  std::uint64_t length{};
  bool symmetric{true}; // symmetric for design, periodic (DFT-even) for spectral analysis
  friend bool operator==(const WindowSpec&, const WindowSpec&) = default;
};

[[nodiscard]] std::string_view to_string(WindowType type) noexcept;
[[nodiscard]] bool is_known_window_type(WindowType type) noexcept;

/// Generate a window of the requested length. length must be > 0.
[[nodiscard]] core::Result<std::vector<double>> generate_window(const WindowSpec& spec);

/// Coherent gain (sum/length). Used to normalize FFT magnitudes back to input amplitude.
[[nodiscard]] double window_coherent_gain(std::string_view window_name) noexcept;
/// Equivalent noise bandwidth (ENBW) in bins, for a periodic window of the given type.
[[nodiscard]] double window_enbw_bins(WindowType type) noexcept;

} // namespace signal::dsp
