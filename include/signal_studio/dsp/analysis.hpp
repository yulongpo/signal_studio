#pragma once

#include "signal_studio/compute/compute.hpp"
#include "signal_studio/data/signal.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::dsp {

enum class FftDirection : std::uint8_t { forward, inverse };
enum class WindowKind : std::uint8_t { rectangular, hann, hamming, blackman };
enum class SpectrumSidedness : std::uint8_t { one_sided, two_sided_shifted };

struct FftSpec final {
  std::uint64_t length{};
  FftDirection direction{FftDirection::forward};
};

struct FftResult final {
  std::vector<data::ComplexSample> bins;
  compute::BackendProvenance provenance;
};

class IFftPlan {
public:
  virtual ~IFftPlan() = default;
  [[nodiscard]] virtual FftSpec spec() const noexcept = 0;
  [[nodiscard]] virtual core::Result<FftResult> process(std::span<const data::ComplexSample> input) = 0;
};

class IFftBackend {
public:
  virtual ~IFftBackend() = default;
  [[nodiscard]] virtual std::string_view backend_id() const noexcept = 0;
  [[nodiscard]] virtual core::Status validate(const FftSpec& spec) const = 0;
  [[nodiscard]] virtual core::Result<std::shared_ptr<IFftPlan>> create_plan(const FftSpec& spec) = 0;
  [[nodiscard]] core::Result<FftResult> execute(const FftSpec& spec, std::span<const data::ComplexSample> input);
};

[[nodiscard]] core::Result<std::shared_ptr<IFftBackend>> make_cpu_fft_backend();
[[nodiscard]] core::Result<std::shared_ptr<IFftBackend>> make_cuda_fft_backend();
[[nodiscard]] core::Result<std::shared_ptr<IFftBackend>> make_auto_fft_backend(bool prefer_cuda,
                                                                               std::string* selection_reason = nullptr);
[[nodiscard]] bool cpu_fft_available() noexcept;
[[nodiscard]] bool cuda_fft_available() noexcept;

struct Window final {
  std::vector<double> coefficients;
  double coherent_gain{};
  double equivalent_noise_bandwidth_bins{};
};

[[nodiscard]] core::Result<Window> make_window(WindowKind kind, std::uint64_t length);

struct SpectrumRequest final {
  double sample_rate_hz{};
  double center_frequency_hz{};
  WindowKind window{WindowKind::hann};
  SpectrumSidedness sidedness{SpectrumSidedness::two_sided_shifted};
};

struct SpectrumResult final {
  std::vector<double> frequency_hz;
  std::vector<double> magnitude_dbfs;
  compute::BackendProvenance provenance;
};

[[nodiscard]] core::Result<SpectrumResult> calculate_spectrum(IFftBackend& backend, const data::SignalSlice& samples,
                                                              const SpectrumRequest& request);

struct PsdResult final {
  std::vector<double> frequency_hz;
  std::vector<double> db_per_hz;
  double equivalent_noise_bandwidth_hz{};
  compute::BackendProvenance provenance;
};

class IPsdEstimator {
public:
  virtual ~IPsdEstimator() = default;
  [[nodiscard]] virtual core::Result<PsdResult> process(const data::SignalSlice& samples,
                                                        const SpectrumRequest& request) = 0;
};

[[nodiscard]] core::Result<std::shared_ptr<IPsdEstimator>> make_psd_estimator(std::shared_ptr<IFftBackend> backend);
[[nodiscard]] core::Result<PsdResult> calculate_psd(IFftBackend& backend, const data::SignalSlice& samples,
                                                    const SpectrumRequest& request);

struct StftRequest final {
  double sample_rate_hz{};
  double center_frequency_hz{};
  std::uint64_t fft_length{};
  std::uint64_t hop_length{};
  WindowKind window{WindowKind::hann};
  SpectrumSidedness sidedness{SpectrumSidedness::two_sided_shifted};
};

struct StftResult final {
  std::vector<double> time_seconds;
  std::vector<double> frequency_hz;
  std::vector<float> db_per_hz;
  std::uint64_t rows{};
  std::uint64_t columns{};
  compute::BackendProvenance provenance;
};

class IStftProcessor {
public:
  virtual ~IStftProcessor() = default;
  [[nodiscard]] virtual core::Result<StftResult> process(const data::SignalSlice& samples,
                                                         const StftRequest& request) = 0;
};

[[nodiscard]] core::Result<std::shared_ptr<IStftProcessor>> make_stft_processor(std::shared_ptr<IFftBackend> backend);
[[nodiscard]] core::Result<StftResult> calculate_stft(IFftBackend& backend, const data::SignalSlice& samples,
                                                      const StftRequest& request);

[[nodiscard]] core::Result<std::uint64_t> time_to_sample(double seconds, double sample_rate_hz,
                                                         std::uint64_t loaded_sample_count);
[[nodiscard]] core::Result<double> sample_to_time(std::uint64_t sample, double sample_rate_hz);
[[nodiscard]] core::Result<std::uint64_t> frequency_to_bin(double frequency_hz, double sample_rate_hz,
                                                           std::uint64_t fft_length, SpectrumSidedness sidedness);
[[nodiscard]] core::Result<double> bin_to_frequency(std::uint64_t bin, double sample_rate_hz, std::uint64_t fft_length,
                                                    SpectrumSidedness sidedness);

} // namespace signal::dsp
