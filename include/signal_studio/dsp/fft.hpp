#pragma once

#include "signal_studio/compute/backend.hpp"
#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace signal::dsp {

enum class FftDirection : std::uint8_t { forward = 0, inverse = 1 };

/// FFT plan specification (API-DSP-001). size is the transform length; backends must accept any
/// positive length they support and reject lengths they do not.
struct FftSpec final {
  std::uint64_t size{};
  FftDirection direction{FftDirection::forward};
  friend bool operator==(const FftSpec&, const FftSpec&) = default;
};

/// Immutable FFT result. For a forward transform of a real-valued input (imag==0) of length N,
/// bin k holds the complex amplitude of frequency k*fs/N. Magnitudes are NOT normalized; callers
/// divide by N (or by N*coherent_gain for windowed inputs) to recover linear amplitude.
struct FftResult final {
  std::vector<data::ComplexSample> bins;
  std::uint64_t size{};
  FftDirection direction{};
  compute::BackendProvenance provenance;
  friend bool operator==(const FftResult&, const FftResult&) = default;
};

class FftPlan {
public:
  virtual ~FftPlan() = default;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
  [[nodiscard]] virtual FftDirection direction() const noexcept = 0;
  [[nodiscard]] virtual compute::BackendProvenance provenance() const = 0;
  /// Execute the plan. input.size() must equal size().
  [[nodiscard]] virtual core::Result<std::vector<data::ComplexSample>>
  execute(std::span<const data::ComplexSample> input) const = 0;
};

/// Abstract FFT backend (API-DSP-001). Concrete adapters (oneMKL CPU, cuFFT GPU) implement this.
/// Public API exposes no third-party types.
class IFftBackend {
public:
  virtual ~IFftBackend() = default;
  [[nodiscard]] virtual compute::ComputeDeviceType device_type() const noexcept = 0;
  [[nodiscard]] virtual compute::BackendProvenance provenance() const = 0;
  [[nodiscard]] virtual core::Result<std::unique_ptr<FftPlan>> create_plan(const FftSpec& spec) = 0;
};

/// Construct the best available FFT backend for the requested device. CUDA returns a cuFFT backend
/// when CUDA is built in and a device is present; CPU returns oneMKL when built in, otherwise an
/// unavailable status (recorded as an environment deviation, never faked).
[[nodiscard]] core::Result<std::unique_ptr<IFftBackend>> make_fft_backend(compute::ComputeDeviceType device);

/// Convenience: forward FFT of a complex block using the provided backend.
[[nodiscard]] core::Result<FftResult> fft(std::span<const data::ComplexSample> input, IFftBackend& backend);

/// Convenience: inverse FFT. Output is scaled by 1/N (cuFFT inverse with CUFFT_INVERSE produces
/// unscaled output; this function divides by N to recover the original signal).
[[nodiscard]] core::Result<FftResult> ifft(std::span<const data::ComplexSample> input, IFftBackend& backend);

/// True when the current build includes a working FFT backend for the given device.
[[nodiscard]] bool fft_backend_available(compute::ComputeDeviceType device) noexcept;

} // namespace signal::dsp
