#pragma once

#include "signal_studio/visualization/viewport.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace signal::visualization {

/// A measurement cursor on the time or frequency axis.
struct Cursor final {
  double position{};
  std::string label;
  friend bool operator==(const Cursor&, const Cursor&) = default;
};

/// A rectangular selection on a 2D view (time x frequency, or x y).
struct Selection final {
  double x_lo{};
  double x_hi{};
  double y_lo{};
  double y_hi{};
  friend bool operator==(const Selection&, const Selection&) = default;
};

/// Computed measurement between two cursors or over a selection (API-VIS-006).
struct Measurement final {
  std::string label;
  double value{};
  std::string unit;
  friend bool operator==(const Measurement&, const Measurement&) = default;
};

/// Overlay model backing cursor/selection/measurement overlays shared across linked views
/// (API-VIS-006). Qt-free; the views read this model when painting.
class OverlayModel final {
public:
  OverlayModel() = default;

  void add_cursor(Cursor cursor);
  void clear_cursors() noexcept;
  [[nodiscard]] const std::vector<Cursor>& cursors() const noexcept {
    return cursors_;
  }

  /// Set a frequency selection. Right-drag left-to-right selects; the controller interprets
  /// direction per the approved interaction spec.
  [[nodiscard]] core::Status set_frequency_selection(double lo_hz, double hi_hz);
  void clear_selection() noexcept;
  [[nodiscard]] const std::optional<Selection>& selection() const noexcept {
    return selection_;
  }

  void set_measurement(Measurement measurement);
  void clear_measurement() noexcept;
  [[nodiscard]] const std::optional<Measurement>& measurement() const noexcept {
    return measurement_;
  }

private:
  std::vector<Cursor> cursors_;
  std::optional<Selection> selection_;
  std::optional<Measurement> measurement_;
};

} // namespace signal::visualization
