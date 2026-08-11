#pragma once

#include "signal_studio/compute/compute.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/dsp/pipeline.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::dsp {

enum class FftDirection : std::uint8_t { forward, inverse };
enum class WindowKind : std::uint8_t {
  rectangular,
  hann,
  hamming,
  blackman,
  blackman_harris,
  flat_top,
  kaiser,
  tukey,
};
enum class SpectrumSidedness : std::uint8_t { one_sided, two_sided_shifted };
enum class AnalysisRangePolicy : std::uint8_t { first_frame, all_complete_frames };
enum class ZeroPaddingPolicy : std::uint8_t { forbidden, enabled };
enum class SpectrumOutputQuantity : std::uint8_t {
  magnitude_dbfs,
  power_dbfs,
  psd_dbfs_per_hz,
  linear_amplitude,
  linear_power,
  linear_power_density,
};
enum class SpectrumNormalization : std::uint8_t { coherent_gain, window_power, none };
enum class DetrendPolicy : std::uint8_t { none, remove_mean };
enum class PsdEstimatorKind : std::uint8_t { periodogram, welch };
enum class SpectrumAccumulationMode : std::uint8_t {
  none,
  linear_average,
  exponential_average,
  maximum_hold,
};
enum class SpectrumSmoothingKind : std::uint8_t { none, moving_average, gaussian, savitzky_golay };
enum class MeasurementSource : std::uint8_t { raw, smoothed };
enum class SpectrogramBoundaryPolicy : std::uint8_t { drop_incomplete, pad_incomplete };
enum class SpectrogramFrequencySmoothingKind : std::uint8_t { none, gaussian };
enum class SpectrogramTimeSmoothingKind : std::uint8_t { none, exponential };

struct WindowSpecification final {
  WindowKind kind{WindowKind::hann};
  /// Kaiser: beta in [0, 50]; Tukey: alpha in [0, 1]; all other windows require 0.
  double parameter{};
  friend bool operator==(const WindowSpecification&, const WindowSpecification&) = default;
};

struct PsdEstimatorSettings final {
  PsdEstimatorKind kind{PsdEstimatorKind::periodogram};
  double welch_overlap{0.5};
  /// Zero means all complete segments allowed by the bounded input.
  std::uint64_t welch_segment_count{};
  friend bool operator==(const PsdEstimatorSettings&, const PsdEstimatorSettings&) = default;
};

struct SpectrumAccumulationSettings final {
  SpectrumAccumulationMode mode{SpectrumAccumulationMode::none};
  /// Zero means all frames selected by the estimator.
  std::uint64_t averaging_count{};
  double exponential_alpha{1.0};
  std::uint64_t hold_reset_generation{};
  friend bool operator==(const SpectrumAccumulationSettings&, const SpectrumAccumulationSettings&) = default;
};

struct SpectrumSmoothingSettings final {
  SpectrumSmoothingKind kind{SpectrumSmoothingKind::none};
  std::uint32_t window_length{};
  double gaussian_sigma{};
  std::uint32_t polynomial_order{};
  friend bool operator==(const SpectrumSmoothingSettings&, const SpectrumSmoothingSettings&) = default;
};

struct SpectrumAnalysisSettings final {
  AnalysisRangePolicy analysis_range_policy{AnalysisRangePolicy::first_frame};
  /// Zero selects a bounded automatic value from the available input.
  std::uint64_t frame_length{};
  /// Zero selects frame_length. A larger value requires enabled zero padding.
  std::uint64_t fft_length{};
  ZeroPaddingPolicy zero_padding_policy{ZeroPaddingPolicy::forbidden};
  WindowSpecification window;
  SpectrumSidedness sidedness{SpectrumSidedness::two_sided_shifted};
  /// Output-coordinate hint retained for DSP API compatibility. Signal Studio keeps analysis results
  /// in baseband coordinates and applies absolute/baseband selection through its display mapping.
  data::FrequencyReference frequency_reference{data::FrequencyReference::baseband};
  SpectrumOutputQuantity output_quantity{SpectrumOutputQuantity::magnitude_dbfs};
  SpectrumNormalization normalization{SpectrumNormalization::coherent_gain};
  DetrendPolicy detrend_policy{DetrendPolicy::none};
  PsdEstimatorSettings estimator;
  SpectrumAccumulationSettings accumulation;
  SpectrumSmoothingSettings smoothing;
  MeasurementSource measurement_source{MeasurementSource::raw};
  friend bool operator==(const SpectrumAnalysisSettings&, const SpectrumAnalysisSettings&) = default;
};

struct SpectrogramSmoothingSettings final {
  SpectrogramFrequencySmoothingKind frequency_mode{SpectrogramFrequencySmoothingKind::none};
  std::uint32_t frequency_kernel_length{};
  double frequency_sigma{};
  SpectrogramTimeSmoothingKind time_mode{SpectrogramTimeSmoothingKind::none};
  double time_exponential_alpha{1.0};
  friend bool operator==(const SpectrogramSmoothingSettings&, const SpectrogramSmoothingSettings&) = default;
};

struct SpectrogramAnalysisSettings final {
  /// Zero selects a bounded automatic value from the available input.
  std::uint64_t frame_length{};
  /// Zero selects frame_length. A larger value requires enabled padding.
  std::uint64_t fft_length{};
  /// This is the sole persisted overlap source; overlap_ratio is derived as 1-hop/frame.
  std::uint64_t hop_length{};
  WindowSpecification window;
  SpectrumSidedness sidedness{SpectrumSidedness::two_sided_shifted};
  SpectrogramBoundaryPolicy boundary_policy{SpectrogramBoundaryPolicy::drop_incomplete};
  ZeroPaddingPolicy padding_policy{ZeroPaddingPolicy::forbidden};
  DetrendPolicy detrend_policy{DetrendPolicy::none};
  SpectrumOutputQuantity output_quantity{SpectrumOutputQuantity::psd_dbfs_per_hz};
  SpectrumNormalization normalization{SpectrumNormalization::window_power};
  SpectrogramSmoothingSettings smoothing;
  friend bool operator==(const SpectrogramAnalysisSettings&, const SpectrogramAnalysisSettings&) = default;
};

struct AnalysisPrefilterSettings final {
  bool enabled{};
  ChainSnapshot chain;
  BoundaryPolicy boundary{BoundaryPolicy::zero_pad};
  std::string backend_id;
  double group_delay_samples{};
};

struct AnalysisSettingsSnapshot final {
  std::string schema{"signal.analysis-settings/1.0"};
  std::string algorithm_version{"signal.dsp.analysis/1.0"};
  SpectrumAnalysisSettings spectrum;
  SpectrogramAnalysisSettings spectrogram;
  AnalysisPrefilterSettings prefilter;
};

struct AnalysisSettingsHash final {
  std::string algorithm{"sha256"};
  std::string hex;
  [[nodiscard]] std::string stable_text() const {
    return algorithm + ":" + hex;
  }
  friend bool operator==(const AnalysisSettingsHash&, const AnalysisSettingsHash&) = default;
};

struct AnalysisCostEstimate final {
  std::uint64_t input_samples{};
  std::uint64_t spectrum_frame_length{};
  std::uint64_t spectrum_fft_length{};
  std::uint64_t spectrum_output_bins{};
  std::uint64_t spectrum_segment_count{};
  std::uint64_t spectrogram_frame_length{};
  std::uint64_t spectrogram_fft_length{};
  std::uint64_t spectrogram_rows{};
  std::uint64_t spectrogram_columns{};
  std::uint64_t fft_execution_count{};
  std::uint64_t host_memory_bytes{};
  std::uint64_t device_memory_bytes{};
  double estimated_operations{};
  double spectrum_bin_spacing_hz{};
  double spectrum_rbw_hz{};
  double spectrogram_time_step_seconds{};
  bool within_host_budget{true};
  bool within_device_budget{true};
};

enum class AnalysisInvalidation : std::uint32_t {
  none = 0U,
  spectrum_smoothing = 1U << 0U,
  spectrogram_smoothing = 1U << 1U,
  spectrum_transform = 1U << 2U,
  spectrogram_transform = 1U << 3U,
  prefilter = 1U << 4U,
};

[[nodiscard]] constexpr AnalysisInvalidation operator|(AnalysisInvalidation left, AnalysisInvalidation right) noexcept {
  return static_cast<AnalysisInvalidation>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr AnalysisInvalidation& operator|=(AnalysisInvalidation& left, AnalysisInvalidation right) noexcept {
  left = left | right;
  return left;
}

[[nodiscard]] constexpr bool has_invalidation(AnalysisInvalidation value, AnalysisInvalidation flag) noexcept {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0U;
}

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

struct WindowDescriptor final {
  WindowKind kind{WindowKind::hann};
  std::string_view english_name;
  std::string_view chinese_name;
  std::string_view parameter_name;
  double parameter_minimum{};
  double parameter_maximum{};
  double recommended_parameter{};
  double reference_coherent_gain{};
  double reference_enbw_bins{};
  std::string_view recommended_use;
  std::string_view amplitude_characteristics;
  std::string_view leakage_characteristics;
};

/// 返回八种内置窗的稳定目录。参考 CG/ENBW 为长窗极限值；Kaiser 使用 beta=8.6，Tukey 使用 alpha=0.5。
[[nodiscard]] std::span<const WindowDescriptor> window_catalog() noexcept;
[[nodiscard]] core::Result<Window> make_window(WindowKind kind, std::uint64_t length);
[[nodiscard]] core::Result<Window> make_window(const WindowSpecification& specification, std::uint64_t length);

[[nodiscard]] core::Status validate_spectrum_analysis_settings(const SpectrumAnalysisSettings& settings,
                                                               std::uint64_t available_samples,
                                                               data::SignalKind signal_kind);
[[nodiscard]] core::Status validate_spectrogram_analysis_settings(const SpectrogramAnalysisSettings& settings,
                                                                  std::uint64_t available_samples,
                                                                  data::SignalKind signal_kind);
[[nodiscard]] core::Status validate_analysis_settings(const AnalysisSettingsSnapshot& settings,
                                                      std::uint64_t available_samples,
                                                      const data::SignalDescriptor& descriptor,
                                                      bool include_spectrum = true, bool include_spectrogram = true);
[[nodiscard]] core::Result<std::string> serialize_analysis_settings(const AnalysisSettingsSnapshot& settings);
[[nodiscard]] core::Result<AnalysisSettingsSnapshot> parse_analysis_settings(std::string_view text);
[[nodiscard]] core::Result<AnalysisSettingsHash> hash_analysis_settings(const AnalysisSettingsSnapshot& settings);
[[nodiscard]] core::Result<AnalysisCostEstimate>
estimate_analysis_cost(const AnalysisSettingsSnapshot& settings, std::uint64_t available_samples, double sample_rate_hz,
                       std::uint64_t host_budget_bytes = 0U, std::uint64_t device_budget_bytes = 0U,
                       bool include_spectrum = true, bool include_spectrogram = true);
[[nodiscard]] AnalysisInvalidation classify_analysis_change(const AnalysisSettingsSnapshot& before,
                                                            const AnalysisSettingsSnapshot& after);
/// Returns the semantic unit of encoded output values. Normalization::none uses explicit raw-FFT
/// units and is never described as full-scale calibrated.
[[nodiscard]] std::string_view spectrum_output_unit(SpectrumOutputQuantity quantity,
                                                    SpectrumNormalization normalization) noexcept;
[[nodiscard]] double spectrogram_overlap_ratio(const SpectrogramAnalysisSettings& settings) noexcept;
[[nodiscard]] core::Result<std::vector<double>>
smooth_spectrum(std::span<const double> values, const SpectrumSmoothingSettings& settings,
                std::shared_ptr<const std::atomic_bool> cancellation = nullptr);
[[nodiscard]] core::Result<data::SignalBuffer>
apply_analysis_prefilter(ISignalKernelBackend& backend, const data::SignalSlice& samples,
                         const data::SignalDescriptor& descriptor, const AnalysisPrefilterSettings& settings,
                         std::shared_ptr<const std::atomic_bool> cancellation = nullptr);

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
  /// Linear-domain, unsmoothed values retained so display smoothing never reruns the FFT.
  std::vector<double> raw_linear_values;
  std::vector<double> raw_amplitude_linear;
  std::vector<double> raw_power_linear;
  std::vector<double> raw_density_linear;
  std::vector<double> raw_values;
  std::vector<double> values;
  SpectrumOutputQuantity output_quantity{SpectrumOutputQuantity::magnitude_dbfs};
  SpectrumNormalization normalization{SpectrumNormalization::coherent_gain};
  AnalysisSettingsHash settings_hash;
  std::uint64_t frame_length{};
  std::uint64_t fft_length{};
  double bin_spacing_hz{};
  double equivalent_noise_bandwidth_hz{};
  double resolution_bandwidth_hz{};
  bool prefilter_applied{};
};

[[nodiscard]] core::Result<SpectrumResult> calculate_spectrum(IFftBackend& backend, const data::SignalSlice& samples,
                                                              const SpectrumRequest& request);
[[nodiscard]] core::Result<SpectrumResult>
calculate_spectrum(IFftBackend& backend, const data::SignalSlice& samples, double sample_rate_hz,
                   double center_frequency_hz, const SpectrumAnalysisSettings& settings,
                   std::shared_ptr<const std::atomic_bool> cancellation = nullptr);
[[nodiscard]] core::Result<SpectrumResult>
calculate_spectrum(IFftBackend& fft_backend, ISignalKernelBackend& kernel_backend, const data::SignalSlice& samples,
                   const data::SignalDescriptor& descriptor, const AnalysisSettingsSnapshot& settings,
                   std::shared_ptr<const std::atomic_bool> cancellation = nullptr);
[[nodiscard]] core::Result<SpectrumResult>
resmooth_spectrum(const SpectrumResult& source, const SpectrumAnalysisSettings& settings,
                  std::shared_ptr<const std::atomic_bool> cancellation = nullptr);

struct PsdResult final {
  std::vector<double> frequency_hz;
  std::vector<double> db_per_hz;
  double equivalent_noise_bandwidth_hz{};
  compute::BackendProvenance provenance;
  /// Linear-domain, unsmoothed values retained for minimal smoothing-only invalidation.
  std::vector<double> raw_linear_values;
  std::vector<double> raw_density_linear;
  std::vector<double> raw_values;
  std::vector<double> values;
  std::vector<double> raw_db_per_hz;
  SpectrumOutputQuantity output_quantity{SpectrumOutputQuantity::psd_dbfs_per_hz};
  SpectrumNormalization normalization{SpectrumNormalization::window_power};
  AnalysisSettingsHash settings_hash;
  std::uint64_t frame_length{};
  std::uint64_t fft_length{};
  std::uint64_t segment_count{};
  double bin_spacing_hz{};
  double resolution_bandwidth_hz{};
  bool prefilter_applied{};
};

struct SpectrumPsdResult final {
  SpectrumResult spectrum;
  PsdResult psd;
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
[[nodiscard]] core::Result<PsdResult> calculate_psd(IFftBackend& backend, const data::SignalSlice& samples,
                                                    double sample_rate_hz, double center_frequency_hz,
                                                    const SpectrumAnalysisSettings& settings,
                                                    std::shared_ptr<const std::atomic_bool> cancellation = nullptr);
[[nodiscard]] core::Result<SpectrumPsdResult>
calculate_spectrum_psd(IFftBackend& backend, const data::SignalSlice& samples, double sample_rate_hz,
                       double center_frequency_hz, const SpectrumAnalysisSettings& settings,
                       std::shared_ptr<const std::atomic_bool> cancellation = nullptr);
[[nodiscard]] core::Result<PsdResult> calculate_psd(IFftBackend& fft_backend, ISignalKernelBackend& kernel_backend,
                                                    const data::SignalSlice& samples,
                                                    const data::SignalDescriptor& descriptor,
                                                    const AnalysisSettingsSnapshot& settings,
                                                    std::shared_ptr<const std::atomic_bool> cancellation = nullptr);
[[nodiscard]] core::Result<PsdResult> resmooth_psd(const PsdResult& source, const SpectrumAnalysisSettings& settings,
                                                   std::shared_ptr<const std::atomic_bool> cancellation = nullptr);

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
  /// Row-major linear-domain, unsmoothed values retained for smoothing-only recomputation.
  std::vector<double> raw_linear_values;
  std::vector<double> raw_density_linear;
  std::vector<float> raw_values;
  std::vector<float> values;
  std::vector<float> raw_db_per_hz;
  SpectrumOutputQuantity output_quantity{SpectrumOutputQuantity::psd_dbfs_per_hz};
  SpectrumNormalization normalization{SpectrumNormalization::window_power};
  AnalysisSettingsHash settings_hash;
  std::uint64_t frame_length{};
  std::uint64_t fft_length{};
  std::uint64_t hop_length{};
  double bin_spacing_hz{};
  double resolution_bandwidth_hz{};
  bool prefilter_applied{};
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
[[nodiscard]] core::Result<StftResult> calculate_stft(IFftBackend& backend, const data::SignalSlice& samples,
                                                      double sample_rate_hz, double center_frequency_hz,
                                                      const SpectrogramAnalysisSettings& settings,
                                                      std::shared_ptr<const std::atomic_bool> cancellation = nullptr,
                                                      std::uint64_t source_sample_offset = 0U);
[[nodiscard]] core::Result<StftResult> calculate_stft(IFftBackend& fft_backend, ISignalKernelBackend& kernel_backend,
                                                      const data::SignalSlice& samples,
                                                      const data::SignalDescriptor& descriptor,
                                                      const AnalysisSettingsSnapshot& settings,
                                                      std::shared_ptr<const std::atomic_bool> cancellation = nullptr);
[[nodiscard]] core::Result<StftResult> resmooth_stft(const StftResult& source,
                                                     const SpectrogramAnalysisSettings& settings,
                                                     std::shared_ptr<const std::atomic_bool> cancellation = nullptr);

[[nodiscard]] core::Result<std::uint64_t> time_to_sample(double seconds, double sample_rate_hz,
                                                         std::uint64_t loaded_sample_count);
[[nodiscard]] core::Result<double> sample_to_time(std::uint64_t sample, double sample_rate_hz);
[[nodiscard]] core::Result<std::uint64_t> frequency_to_bin(double frequency_hz, double sample_rate_hz,
                                                           std::uint64_t fft_length, SpectrumSidedness sidedness);
[[nodiscard]] core::Result<double> bin_to_frequency(std::uint64_t bin, double sample_rate_hz, std::uint64_t fft_length,
                                                    SpectrumSidedness sidedness);

} // namespace signal::dsp
