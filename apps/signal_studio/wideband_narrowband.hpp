#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/visualization/viewport.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace signal::studio {

/// A narrowband channel extracted from a wideband capture. All frequencies are absolute Hz.
struct NarrowbandChannelSpec final {
  double center_frequency_hz{};
  double bandwidth_hz{};
  double output_sample_rate_hz{};
  friend bool operator==(const NarrowbandChannelSpec&, const NarrowbandChannelSpec&) = default;
};

/// Result of extracting a narrowband channel: the baseband-shifted, filtered, resampled complex
/// samples plus the channel provenance.
struct NarrowbandChannel final {
  NarrowbandChannelSpec spec;
  std::vector<data::ComplexSample> samples;
  double output_sample_rate_hz{};
  std::uint64_t source_start_sample{};
  friend bool operator==(const NarrowbandChannel&, const NarrowbandChannel&) = default;
};

/// Extract a narrowband channel from a complex IQ slice via digital down-conversion:
/// frequency-shift to baseband, low-pass filter (cutoff = bandwidth/2), then resample to the
/// output rate. The input must be complex; real input returns an error.
[[nodiscard]] core::Result<NarrowbandChannel>
extract_narrowband_channel(const data::SignalSlice& wideband, double sample_rate_hz,
                           const NarrowbandChannelSpec& spec, std::uint64_t source_start_sample = 0);

/// Linkage state between a wideband overview and a narrowband detail view. Qt-free; the GUI
/// reads this to synchronize viewports and cursors.
struct WidebandNarrowbandLink final {
  /// The wideband frequency selection [lo, hi] Hz that drives the narrowband center.
  double wideband_selection_lo_hz{};
  double wideband_selection_hi_hz{};
  /// The narrowband channel derived from the selection.
  NarrowbandChannelSpec channel;
  friend bool operator==(const WidebandNarrowbandLink&, const WidebandNarrowbandLink&) = default;
};

/// Controller for linked wideband/narrowband analysis. Given a wideband selection, it derives a
/// narrowband channel spec (center = midpoint, bandwidth = span, output rate = bandwidth * 2)
/// and exposes the linkage so both views stay synchronized.
class WidebandNarrowbandController final {
 public:
  WidebandNarrowbandController() = default;
  explicit WidebandNarrowbandController(double wideband_sample_rate_hz, double center_frequency_hz);

  /// Set the wideband frequency selection. Rejects inverted ranges and spans that exceed Nyquist.
  [[nodiscard]] core::Status set_wideband_selection(double lo_hz, double hi_hz);
  [[nodiscard]] std::optional<WidebandNarrowbandLink> link() const noexcept { return link_; }
  [[nodiscard]] double wideband_sample_rate_hz() const noexcept { return wideband_sample_rate_hz_; }

  /// Derive a channel spec from the current selection with optional overrides.
  [[nodiscard]] core::Result<NarrowbandChannelSpec> derive_channel_spec(
      std::optional<double> override_output_rate_hz = {}) const;

 private:
  double wideband_sample_rate_hz_{};
  double center_frequency_hz_{};
  std::optional<WidebandNarrowbandLink> link_;
};

}  // namespace signal::studio
