#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/visualization/chart_view.hpp"
#include "signal_studio/visualization/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>

namespace signal::visualization {

/// Approved spectrogram color scales (task spec §7.3 point 6: waterfall supports multiple).
enum class ColorScale : std::uint8_t { viridis = 0, magma = 1, turbo = 2, grayscale = 3, inferno = 4 };

[[nodiscard]] inline std::string_view to_string(ColorScale scale) noexcept {
  switch (scale) {
  case ColorScale::viridis:
    return "viridis";
  case ColorScale::magma:
    return "magma";
  case ColorScale::turbo:
    return "turbo";
  case ColorScale::grayscale:
    return "grayscale";
  case ColorScale::inferno:
    return "inferno";
  }
  return "unknown";
}

namespace detail {
inline std::uint8_t chan(std::uint32_t rgb, int shift) noexcept {
  return static_cast<std::uint8_t>((rgb >> shift) & 0xFFu);
}
inline std::uint32_t rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
  return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
}
inline std::uint8_t clamp8(double v) noexcept {
  return static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
}
} // namespace detail

/// Map a normalized [0,1] magnitude to an RGB triple for the given color scale (display only).
[[nodiscard]] inline std::uint32_t color_rgb(ColorScale scale, double normalized) noexcept {
  double t = std::clamp(normalized, 0.0, 1.0);
  switch (scale) {
  case ColorScale::grayscale: {
    const std::uint8_t g = detail::clamp8(t * 255.0);
    return detail::rgb(g, g, g);
  }
  case ColorScale::viridis: {
    // Purple (68,1,84) -> teal (33,145,140) -> yellow (253,231,37).
    if (t < 0.5) {
      const double u = t * 2.0;
      return detail::rgb(detail::clamp8(68 + (33 - 68) * u), detail::clamp8(1 + (145 - 1) * u),
                         detail::clamp8(84 + (140 - 84) * u));
    }
    const double u = (t - 0.5) * 2.0;
    return detail::rgb(detail::clamp8(33 + (253 - 33) * u), detail::clamp8(145 + (231 - 145) * u),
                       detail::clamp8(140 + (37 - 140) * u));
  }
  case ColorScale::magma: {
    // Black (0,0,4) -> purple (79,18,123) -> pink (247,148,0) -> white (252,253,191).
    if (t < 0.4) {
      const double u = t / 0.4;
      return detail::rgb(detail::clamp8(0 + (79 - 0) * u), detail::clamp8(0 + (18 - 0) * u),
                         detail::clamp8(4 + (123 - 4) * u));
    }
    if (t < 0.75) {
      const double u = (t - 0.4) / 0.35;
      return detail::rgb(detail::clamp8(79 + (247 - 79) * u), detail::clamp8(18 + (148 - 18) * u),
                         detail::clamp8(123 + (0 - 123) * u));
    }
    const double u = (t - 0.75) / 0.25;
    return detail::rgb(detail::clamp8(247 + (252 - 247) * u), detail::clamp8(148 + (253 - 148) * u),
                       detail::clamp8(0 + (191 - 0) * u));
  }
  case ColorScale::turbo: {
    // Blue (48,18,59) -> green (34,167,132) -> yellow (240,238,0) -> red (122,4,3).
    if (t < 0.33) {
      const double u = t / 0.33;
      return detail::rgb(detail::clamp8(48 + (34 - 48) * u), detail::clamp8(18 + (167 - 18) * u),
                         detail::clamp8(59 + (132 - 59) * u));
    }
    if (t < 0.66) {
      const double u = (t - 0.33) / 0.33;
      return detail::rgb(detail::clamp8(34 + (240 - 34) * u), detail::clamp8(167 + (238 - 167) * u),
                         detail::clamp8(132 + (0 - 132) * u));
    }
    const double u = (t - 0.66) / 0.34;
    return detail::rgb(detail::clamp8(240 + (122 - 240) * u), detail::clamp8(238 + (4 - 238) * u),
                       detail::clamp8(0 + (3 - 0) * u));
  }
  case ColorScale::inferno: {
    // Black (0,0,4) -> purple (87,16,110) -> orange (252,138,0) -> yellow (252,255,164).
    if (t < 0.4) {
      const double u = t / 0.4;
      return detail::rgb(detail::clamp8(0 + (87 - 0) * u), detail::clamp8(0 + (16 - 0) * u),
                         detail::clamp8(4 + (110 - 4) * u));
    }
    if (t < 0.75) {
      const double u = (t - 0.4) / 0.35;
      return detail::rgb(detail::clamp8(87 + (252 - 87) * u), detail::clamp8(16 + (138 - 16) * u),
                         detail::clamp8(110 + (0 - 110) * u));
    }
    const double u = (t - 0.75) / 0.25;
    return detail::rgb(detail::clamp8(252 + (252 - 252) * u), detail::clamp8(138 + (255 - 138) * u),
                       detail::clamp8(0 + (164 - 0) * u));
  }
  }
  return 0;
}

/// Time navigator (API-VIS-004): a small-height overview strip whose full extent equals the
/// loaded data range and whose viewport controls the spectrogram's visible time window.
class TimeNavigator final {
public:
  TimeNavigator() = default;
  explicit TimeNavigator(LoadedDataRange loaded);
  [[nodiscard]] const TimeViewport& viewport() const noexcept {
    return viewport_;
  }
  [[nodiscard]] TimeViewport& viewport() noexcept {
    return viewport_;
  }
  [[nodiscard]] core::Status set_loaded_range(LoadedDataRange loaded);

private:
  TimeViewport viewport_;
};

/// Factory functions returning chart views. The returned IChartView owns a QWidget accessible
/// via native_widget(). Construction may fail if Qt cannot create an offscreen widget.
[[nodiscard]] core::Result<std::unique_ptr<IChartView>> make_time_waveform_view();
[[nodiscard]] core::Result<std::unique_ptr<IChartView>> make_spectrum_view();
[[nodiscard]] core::Result<std::unique_ptr<IChartView>> make_spectrogram_view();
[[nodiscard]] core::Result<std::unique_ptr<IChartView>> make_constellation_view();
[[nodiscard]] core::Result<std::unique_ptr<IChartView>> make_eye_diagram_view();

} // namespace signal::visualization
