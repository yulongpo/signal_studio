#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/dsp/fft.hpp"
#include "signal_studio/dsp/windows.hpp"

#include <cstdint>
#include <vector>

namespace signal::dsp {

/// STFT request (API-DSP-003). Produces a time-frequency tile matrix.
struct StftRequest final {
  WindowType window{WindowType::hann};
  std::uint64_t nfft{};
  std::uint64_t hop_samples{};
  bool output_magnitude{true};  // false -> linear power (|X|^2)
  friend bool operator==(const StftRequest&, const StftRequest&) = default;
};

struct StftResult final {
  /// time_bins.size() == frame_count; each frame's center time in seconds.
  std::vector<double> time_bins;
  /// freq_bins.size() == nfft/2+1 (one-sided) for real input, or nfft (two-sided) for complex.
  std::vector<double> freq_bins;
  /// Row-major magnitudes/powers: frames * freq_bins.
  std::vector<double> matrix;
  std::uint64_t frame_count{};
  std::uint64_t freq_count{};
  std::uint64_t nfft{};
  std::uint64_t hop_samples{};
  double sample_rate_hz{};
  bool magnitude{true};
  compute::BackendProvenance provenance;
  friend bool operator==(const StftResult&, const StftResult&) = default;
};

class IStftProcessor {
 public:
  virtual ~IStftProcessor() = default;
  [[nodiscard]] virtual core::Result<StftResult>
  process(const data::SignalSlice& slice, double sample_rate_hz, const StftRequest& request) = 0;
};

class StftProcessor final : public IStftProcessor {
 public:
  explicit StftProcessor(IFftBackend& backend);
  [[nodiscard]] core::Result<StftResult>
  process(const data::SignalSlice& slice, double sample_rate_hz, const StftRequest& request) override;

 private:
  IFftBackend& backend_;
};

}  // namespace signal::dsp
