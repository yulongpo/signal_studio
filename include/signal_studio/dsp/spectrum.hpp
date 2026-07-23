#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/dsp/fft.hpp"
#include "signal_studio/dsp/windows.hpp"

#include <cstdint>
#include <vector>

namespace signal::dsp {

enum class SpectrumScaling : std::uint8_t {
  /// V^2/Hz power spectral density. Y-axis of the approved power-spectrum view is dB/Hz.
  density = 0,
  /// V^2 power spectrum (no per-Hz normalization).
  spectrum = 1,
};

/// Welch PSD request (API-DSP-002). Window, ENBW, units and averaging strategy are explicit.
struct PsdRequest final {
  WindowType window{WindowType::hann};
  std::uint64_t nfft{};
  std::uint64_t overlap_samples{};
  SpectrumScaling scaling{SpectrumScaling::density};
  /// When true, return the one-sided spectrum for real input (folds negative frequencies,
  /// doubling power except at DC and Nyquist).
  bool one_sided{true};
  friend bool operator==(const PsdRequest&, const PsdRequest&) = default;
};

struct PsdResult final {
  /// Frequency axis in Hz, length == bins.size().
  std::vector<double> frequencies_hz;
  /// PSD in V^2/Hz (density) or V^2 (spectrum). Not yet in dB; callers convert with 10*log10.
  std::vector<double> power;
  std::uint64_t nfft{};
  std::uint64_t frame_count{};
  double sample_rate_hz{};
  double enbw_hz{};
  WindowType window{WindowType::hann};
  SpectrumScaling scaling{SpectrumScaling::density};
  compute::BackendProvenance provenance;
  friend bool operator==(const PsdResult&, const PsdResult&) = default;
};

class IPsdEstimator {
 public:
  virtual ~IPsdEstimator() = default;
  [[nodiscard]] virtual core::Result<PsdResult>
  process(const data::SignalSlice& slice, double sample_rate_hz, const PsdRequest& request) = 0;
};

/// Welch estimator backed by an injected FFT backend (oneMKL/cuFFT adapter). The estimator itself
/// is backend-agnostic; it records the backend provenance in every result for cache keying.
class WelchPsdEstimator final : public IPsdEstimator {
 public:
  explicit WelchPsdEstimator(IFftBackend& backend);
  [[nodiscard]] core::Result<PsdResult>
  process(const data::SignalSlice& slice, double sample_rate_hz, const PsdRequest& request) override;

 private:
  IFftBackend& backend_;
};

/// Convert a linear PSD (V^2/Hz) to dB/Hz. Zeros are clamped to a floor to avoid -inf.
[[nodiscard]] std::vector<double> to_db_hz(std::span<const double> power, double floor_db = -300.0);

}  // namespace signal::dsp
