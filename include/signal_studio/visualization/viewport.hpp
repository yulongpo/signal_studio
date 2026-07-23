#pragma once

#include "signal_studio/core/result.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace signal::visualization {

/// Half-open loaded-data range in samples (mirrors data::SampleRange semantics). The time
/// navigator's full extent equals the actually-read data length, never the file length.
struct LoadedDataRange final {
  std::uint64_t begin{};
  std::uint64_t end{};
  double sample_rate_hz{};
  [[nodiscard]] std::uint64_t size() const noexcept {
    return end - begin;
  }
  [[nodiscard]] double duration_seconds() const noexcept {
    return sample_rate_hz > 0.0 ? static_cast<double>(size()) / sample_rate_hz : 0.0;
  }
  friend bool operator==(const LoadedDataRange&, const LoadedDataRange&) = default;
};

/// A clamped [min, max] viewport over a real-valued axis. Used for both time (seconds) and
/// frequency (Hz). All mutators clamp to the loaded extent and notify subscribers.
class ViewportRange final {
public:
  ViewportRange() = default;
  ViewportRange(double minimum, double maximum);

  [[nodiscard]] double minimum() const noexcept {
    return minimum_;
  }
  [[nodiscard]] double maximum() const noexcept {
    return maximum_;
  }
  [[nodiscard]] double span() const noexcept {
    return maximum_ - minimum_;
  }
  [[nodiscard]] bool contains(double value) const noexcept;

  /// Set the visible window. Clamps to [minimum, maximum]; rejects inverted ranges.
  [[nodiscard]] core::Status set_window(double lo, double hi);
  /// Zoom around a center point by a factor > 0 (< 1 zooms in, > 1 zooms out).
  [[nodiscard]] core::Status zoom(double center, double factor);
  /// Pan by a signed fraction of the current span.
  [[nodiscard]] core::Status pan(double fraction_of_span);
  void reset() {
    lo_ = minimum_;
    hi_ = maximum_;
    notify();
  }

  [[nodiscard]] double window_lo() const noexcept {
    return lo_;
  }
  [[nodiscard]] double window_hi() const noexcept {
    return hi_;
  }

  using Subscriber = std::function<void()>;
  [[nodiscard]] std::uint64_t subscribe(Subscriber subscriber);
  void unsubscribe(std::uint64_t id);

private:
  void clamp_window();
  void notify();
  double minimum_{};
  double maximum_{};
  double lo_{};
  double hi_{};
  std::uint64_t next_id_{1};
  std::vector<std::pair<std::uint64_t, Subscriber>> subscribers_;
};

/// Shared integer-Hz frequency viewport (API-VIS-002/003). Power spectrum and spectrogram share
/// one FrequencyViewport so their frequency axes stay synchronized.
class FrequencyViewport final {
public:
  FrequencyViewport() = default;
  explicit FrequencyViewport(double max_hz);
  [[nodiscard]] const ViewportRange& range() const noexcept {
    return range_;
  }
  [[nodiscard]] ViewportRange& range() noexcept {
    return range_;
  }
  [[nodiscard]] core::Status set_frequency_window(double lo_hz, double hi_hz);

private:
  ViewportRange range_;
};

/// Shared time viewport (API-VIS-004). Time navigator and waveform/spectrogram share it; the
/// spectrogram's visible time range is controlled by the time navigator.
class TimeViewport final {
public:
  TimeViewport() = default;
  explicit TimeViewport(LoadedDataRange loaded);
  [[nodiscard]] const LoadedDataRange& loaded() const noexcept {
    return loaded_;
  }
  [[nodiscard]] const ViewportRange& range() const noexcept {
    return range_;
  }
  [[nodiscard]] ViewportRange& range() noexcept {
    return range_;
  }
  [[nodiscard]] core::Status set_loaded_range(LoadedDataRange loaded);

private:
  LoadedDataRange loaded_;
  ViewportRange range_;
};

/// Controller coordinating linked time/frequency viewports across multiple views (API-VIS-002).
class ViewportController final {
public:
  ViewportController() = default;
  explicit ViewportController(LoadedDataRange loaded, double max_frequency_hz);
  [[nodiscard]] TimeViewport& time() noexcept {
    return time_;
  }
  [[nodiscard]] FrequencyViewport& frequency() noexcept {
    return frequency_;
  }
  [[nodiscard]] const TimeViewport& time() const noexcept {
    return time_;
  }
  [[nodiscard]] const FrequencyViewport& frequency() const noexcept {
    return frequency_;
  }
  /// Frequency unit auto-selection: Hz/kHz/MHz/GHz based on the visible maximum.
  [[nodiscard]] std::string_view frequency_unit() const noexcept;

private:
  TimeViewport time_;
  FrequencyViewport frequency_;
};

[[nodiscard]] std::string_view frequency_unit_for(double hz) noexcept;
/// Format a frequency with Hz-level precision and auto unit (Hz/kHz/MHz/GHz).
[[nodiscard]] std::string format_frequency(double hz) noexcept;

} // namespace signal::visualization
