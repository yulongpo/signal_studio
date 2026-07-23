#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace signal::dsp {

/// Time-domain statistics for a real-valued block (API-DSP adjacent: time-domain statistics).
/// All values are computed in a single pass where possible.
struct TimeDomainStats final {
  std::uint64_t count{};
  double mean{};
  double variance{};
  double rms{};
  double peak{}; // max absolute sample
  std::uint64_t peak_index{};
  double min{};
  double max{};
  double dc_offset{}; // == mean
  friend bool operator==(const TimeDomainStats&, const TimeDomainStats&) = default;
};

[[nodiscard]] core::Result<TimeDomainStats> compute_real_stats(const data::SignalSlice& slice);

/// Complex (IQ) metrics: magnitude and phase statistics, plus standard IQ quality measures.
struct ComplexStats final {
  std::uint64_t count{};
  double magnitude_mean{};
  double magnitude_rms{};
  double magnitude_peak{};
  std::uint64_t magnitude_peak_index{};
  double phase_mean_radians{};
  double dc_real{};
  double dc_imag{};
  /// RMS magnitude deviation from the mean magnitude (AC power of the envelope).
  double magnitude_ac_rms{};
  friend bool operator==(const ComplexStats&, const ComplexStats&) = default;
};

[[nodiscard]] core::Result<ComplexStats> compute_complex_stats(const data::SignalSlice& slice);

} // namespace signal::dsp
