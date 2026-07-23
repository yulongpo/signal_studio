#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/dsp/windows.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace signal::dsp {

enum class FilterType : std::uint8_t { lowpass = 0, highpass = 1, bandpass = 2, bandstop = 3 };

/// FIR filter design specification. order is the number of taps minus one (must be even for
/// linear-phase designs). For lowpass/highpass use cutoff_low_hz; for bandpass/bandstop use both.
struct FilterSpec final {
  FilterType type{FilterType::lowpass};
  std::uint64_t order{64};
  double sample_rate_hz{};
  double cutoff_low_hz{};
  double cutoff_high_hz{};
  WindowType window{WindowType::hann};
  friend bool operator==(const FilterSpec&, const FilterSpec&) = default;
};

/// Cross-block filter memory (API-DSP-005: state preserved across blocks).
struct FilterState final {
  std::vector<double> delays;
  friend bool operator==(const FilterState&, const FilterState&) = default;
};

class IFilter {
 public:
  virtual ~IFilter() = default;
  [[nodiscard]] virtual std::uint64_t taps() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<double>& coefficients() const noexcept = 0;
  /// Convolve input through the filter using state. output.size() must equal input.size().
  [[nodiscard]] virtual core::Status
  process(std::span<const double> input, FilterState& state, std::span<double> output) const = 0;
  virtual void reset(FilterState& state) const = 0;
};

/// Linear-phase FIR filter designed by the windowed-sinc method. The same mature design is used
/// for all four response types via spectral inversion/summing so no per-type approximation is
/// hand-rolled.
class FirFilter final : public IFilter {
 public:
  [[nodiscard]] static core::Result<FirFilter> design(const FilterSpec& spec);
  [[nodiscard]] std::uint64_t taps() const noexcept override;
  [[nodiscard]] const std::vector<double>& coefficients() const noexcept override;
  [[nodiscard]] core::Status
  process(std::span<const double> input, FilterState& state, std::span<double> output) const override;
  void reset(FilterState& state) const override;

 private:
  FirFilter(std::vector<double> coefficients, std::uint64_t order);
  std::vector<double> coefficients_;
  std::uint64_t order_;
};

[[nodiscard]] std::string_view to_string(FilterType type) noexcept;

}  // namespace signal::dsp
