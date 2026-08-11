#include "signal_studio/dsp/analysis.hpp"
#include "signal_studio/core/services.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <numeric>
#include <sstream>
#include <type_traits>

namespace signal::dsp {

namespace analysis_internal {
[[nodiscard]] core::Result<std::vector<double>> convolve_centered(std::span<const double> input,
                                                                  std::span<const double> coefficients);
[[nodiscard]] core::Result<std::vector<double>> savitzky_golay_kernel(std::uint32_t window_length,
                                                                      std::uint32_t polynomial_order);
} // namespace analysis_internal

namespace {

[[nodiscard]] core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::dsp, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] core::Result<std::vector<data::ComplexSample>> to_complex(const data::SignalSlice& samples,
                                                                        std::span<const double> window) {
  if (samples.size() != window.size()) {
    return error(core::ErrorReason::invalid_argument, "样本数必须等于窗长度");
  }
  std::vector<data::ComplexSample> converted;
  converted.reserve(window.size());
  if (samples.kind() == data::SignalKind::real) {
    const auto values = samples.real_values();
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (!std::isfinite(values[index])) {
        return error(core::ErrorReason::invalid_argument, "样本包含 NaN 或 Inf");
      }
      converted.push_back({values[index] * window[index], 0.0});
    }
  } else {
    const auto values = samples.complex_values();
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (!std::isfinite(values[index].real) || !std::isfinite(values[index].imag)) {
        return error(core::ErrorReason::invalid_argument, "样本包含 NaN 或 Inf");
      }
      converted.push_back({values[index].real * window[index], values[index].imag * window[index]});
    }
  }
  return converted;
}

[[nodiscard]] double magnitude_squared(const data::ComplexSample& value) noexcept {
  return value.real * value.real + value.imag * value.imag;
}

[[nodiscard]] std::vector<std::uint64_t> ordered_bins(std::uint64_t length, SpectrumSidedness sidedness) {
  std::vector<std::uint64_t> bins;
  if (sidedness == SpectrumSidedness::one_sided) {
    bins.resize(static_cast<std::size_t>(length / 2U + 1U));
    for (std::uint64_t index = 0; index < bins.size(); ++index) {
      bins[static_cast<std::size_t>(index)] = index;
    }
    return bins;
  }
  bins.resize(static_cast<std::size_t>(length));
  const auto shift = (length + 1U) / 2U;
  for (std::uint64_t index = 0; index < length; ++index) {
    bins[static_cast<std::size_t>(index)] = (index + shift) % length;
  }
  return bins;
}

[[nodiscard]] double clamped_db(double linear) noexcept {
  return 10.0 * std::log10(std::max(linear, std::numeric_limits<double>::min()));
}

[[nodiscard]] core::Status validate_fft_output(const FftResult& transformed, std::uint64_t expected_length) {
  if (transformed.bins.size() != expected_length) {
    return error(core::ErrorReason::internal_failure, "FFT 后端返回了错误的 bin 数");
  }
  if (std::ranges::any_of(transformed.bins, [](const data::ComplexSample& value) {
        return !std::isfinite(value.real) || !std::isfinite(value.imag);
      })) {
    return error(core::ErrorReason::internal_failure, "FFT 后端返回了 NaN 或 Inf bin");
  }
  return core::Status::success();
}

[[nodiscard]] std::string provenance_summary(const compute::BackendProvenance& provenance) {
  std::ostringstream output;
  output << "requested=" << static_cast<unsigned>(provenance.requested)
         << ",actual=" << static_cast<unsigned>(provenance.actual) << ",backend=" << provenance.backend_id
         << ",device=" << provenance.device << ",precision=" << provenance.precision
         << ",degraded=" << provenance.degraded << ",verified=" << provenance.consistency_verified
         << ",version=" << provenance.version << ",reason=" << provenance.reason;
  return output.str();
}

[[nodiscard]] core::Status validate_provenance_consistency(const compute::BackendProvenance& expected,
                                                           const compute::BackendProvenance& current,
                                                           std::string_view analysis_kind) {
  if (expected == current) {
    return core::Status::success();
  }
  return error(core::ErrorReason::internal_failure,
               std::string{analysis_kind} + " 在同一分析请求内检测到混合 FFT provenance",
               "首帧{" + provenance_summary(expected) + "}；当前帧{" + provenance_summary(current) + "}");
}

constexpr std::uint64_t maximum_exact_double_integer = std::uint64_t{1} << 53U;

class PsdEstimator final : public IPsdEstimator {
public:
  explicit PsdEstimator(std::shared_ptr<IFftBackend> backend) : backend_(std::move(backend)) {}

  [[nodiscard]] core::Result<PsdResult> process(const data::SignalSlice& samples,
                                                const SpectrumRequest& request) override {
    return calculate_psd(*backend_, samples, request);
  }

private:
  std::shared_ptr<IFftBackend> backend_;
};

class StftProcessor final : public IStftProcessor {
public:
  explicit StftProcessor(std::shared_ptr<IFftBackend> backend) : backend_(std::move(backend)) {}

  [[nodiscard]] core::Result<StftResult> process(const data::SignalSlice& samples,
                                                 const StftRequest& request) override {
    return calculate_stft(*backend_, samples, request);
  }

private:
  std::shared_ptr<IFftBackend> backend_;
};

[[nodiscard]] bool cancellation_requested(const std::shared_ptr<const std::atomic_bool>& cancellation) noexcept {
  return cancellation != nullptr && cancellation->load(std::memory_order_relaxed);
}

[[nodiscard]] core::Status cancellation_status(std::string message) {
  return error(core::ErrorReason::cancelled, std::move(message), "未发布部分分析结果");
}

struct ResolvedSpectrumDimensions final {
  std::uint64_t frame_length{};
  std::uint64_t fft_length{};
};

struct ResolvedSpectrogramDimensions final {
  std::uint64_t frame_length{};
  std::uint64_t fft_length{};
  std::uint64_t hop_length{};
  std::uint64_t rows{};
};

[[nodiscard]] core::Result<ResolvedSpectrumDimensions>
resolve_spectrum_dimensions(const SpectrumAnalysisSettings& settings, std::uint64_t available_samples) {
  if (available_samples == 0U) {
    return error(core::ErrorReason::invalid_argument, "频谱分析没有可用样本");
  }
  const auto frame_length =
      settings.frame_length == 0U
          ? (settings.fft_length == 0U ? available_samples : std::min(available_samples, settings.fft_length))
          : settings.frame_length;
  const auto fft_length = settings.fft_length == 0U ? frame_length : settings.fft_length;
  if (frame_length < 2U || fft_length < frame_length) {
    return error(core::ErrorReason::invalid_argument, "频谱 FFT 长度必须不小于至少 2 点的分析帧长度");
  }
  if ((fft_length > frame_length || available_samples < frame_length) &&
      settings.zero_padding_policy != ZeroPaddingPolicy::enabled) {
    return error(core::ErrorReason::invalid_argument, "当前频谱参数需要补零，但 zero_padding_policy 未启用");
  }
  if (frame_length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      fft_length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return error(core::ErrorReason::invalid_argument, "频谱帧长或 FFT 长度超出当前进程可表示范围");
  }
  return ResolvedSpectrumDimensions{frame_length, fft_length};
}

[[nodiscard]] core::Result<ResolvedSpectrogramDimensions>
resolve_spectrogram_dimensions(const SpectrogramAnalysisSettings& settings, std::uint64_t available_samples) {
  if (available_samples == 0U) {
    return error(core::ErrorReason::invalid_argument, "STFT 没有可用样本");
  }
  const auto frame_length =
      settings.frame_length == 0U
          ? (settings.fft_length == 0U ? available_samples : std::min(available_samples, settings.fft_length))
          : settings.frame_length;
  const auto fft_length = settings.fft_length == 0U ? frame_length : settings.fft_length;
  const auto hop_length =
      settings.hop_length == 0U ? std::max<std::uint64_t>(1U, frame_length / 4U) : settings.hop_length;
  if (frame_length < 2U || fft_length < frame_length || hop_length == 0U || hop_length > frame_length) {
    return error(core::ErrorReason::invalid_argument, "STFT 帧长、FFT 长度或 Hop Length 无效");
  }
  if ((fft_length > frame_length || available_samples < frame_length ||
       settings.boundary_policy == SpectrogramBoundaryPolicy::pad_incomplete) &&
      settings.padding_policy != ZeroPaddingPolicy::enabled) {
    return error(core::ErrorReason::invalid_argument, "当前 STFT 参数需要补零，但 padding_policy 未启用");
  }
  std::uint64_t rows{};
  if (settings.boundary_policy == SpectrogramBoundaryPolicy::drop_incomplete) {
    if (available_samples < frame_length) {
      return error(core::ErrorReason::invalid_argument, "STFT 输入不足一帧且禁止不完整帧补零");
    }
    rows = 1U + (available_samples - frame_length) / hop_length;
  } else {
    rows = 1U + (available_samples - 1U) / hop_length;
  }
  if (frame_length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      fft_length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      rows > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return error(core::ErrorReason::invalid_argument, "STFT 尺寸超出当前进程可表示范围");
  }
  return ResolvedSpectrogramDimensions{frame_length, fft_length, hop_length, rows};
}

[[nodiscard]] core::Status validate_window_specification(const WindowSpecification& specification) {
  if (!std::isfinite(specification.parameter)) {
    return error(core::ErrorReason::invalid_argument, "窗函数参数必须为有限值");
  }
  switch (specification.kind) {
  case WindowKind::kaiser:
    return specification.parameter >= 0.0 && specification.parameter <= 50.0
               ? core::Status::success()
               : error(core::ErrorReason::invalid_argument, "Kaiser beta 必须位于 [0, 50]");
  case WindowKind::tukey:
    return specification.parameter >= 0.0 && specification.parameter <= 1.0
               ? core::Status::success()
               : error(core::ErrorReason::invalid_argument, "Tukey alpha 必须位于 [0, 1]");
  case WindowKind::rectangular:
  case WindowKind::hann:
  case WindowKind::hamming:
  case WindowKind::blackman:
  case WindowKind::blackman_harris:
  case WindowKind::flat_top:
    return specification.parameter == 0.0
               ? core::Status::success()
               : error(core::ErrorReason::invalid_argument, "无参数窗函数的 parameter 必须为 0");
  }
  return error(core::ErrorReason::invalid_argument, "未知窗函数");
}

[[nodiscard]] core::Status validate_spectrum_smoothing(const SpectrumSmoothingSettings& settings) {
  switch (settings.kind) {
  case SpectrumSmoothingKind::none:
    return core::Status::success();
  case SpectrumSmoothingKind::moving_average:
    if (settings.window_length < 3U || settings.window_length % 2U == 0U) {
      return error(core::ErrorReason::invalid_argument, "滑动平均窗长必须为不小于 3 的奇数");
    }
    return core::Status::success();
  case SpectrumSmoothingKind::gaussian:
    if (settings.window_length < 3U || settings.window_length % 2U == 0U || !(settings.gaussian_sigma > 0.0) ||
        !std::isfinite(settings.gaussian_sigma)) {
      return error(core::ErrorReason::invalid_argument, "高斯平滑要求奇数窗长和有限正 Sigma");
    }
    return core::Status::success();
  case SpectrumSmoothingKind::savitzky_golay:
    if (settings.window_length < 3U || settings.window_length % 2U == 0U ||
        settings.polynomial_order >= settings.window_length || settings.polynomial_order > 12U) {
      return error(core::ErrorReason::invalid_argument, "Savitzky-Golay 窗长或阶数无效");
    }
    return core::Status::success();
  }
  return error(core::ErrorReason::invalid_argument, "未知频谱平滑方式");
}

[[nodiscard]] core::Status validate_spectrogram_smoothing(const SpectrogramSmoothingSettings& settings) {
  switch (settings.frequency_mode) {
  case SpectrogramFrequencySmoothingKind::none:
    break;
  case SpectrogramFrequencySmoothingKind::gaussian:
    if (settings.frequency_kernel_length < 3U || settings.frequency_kernel_length % 2U == 0U ||
        !(settings.frequency_sigma > 0.0) || !std::isfinite(settings.frequency_sigma)) {
      return error(core::ErrorReason::invalid_argument, "STFT 频率高斯平滑要求奇数核长和有限正 Sigma");
    }
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知 STFT 频率平滑方式");
  }
  switch (settings.time_mode) {
  case SpectrogramTimeSmoothingKind::none:
    return core::Status::success();
  case SpectrogramTimeSmoothingKind::exponential:
    return settings.time_exponential_alpha > 0.0 && settings.time_exponential_alpha <= 1.0 &&
                   std::isfinite(settings.time_exponential_alpha)
               ? core::Status::success()
               : error(core::ErrorReason::invalid_argument, "STFT 时间指数平滑 alpha 必须位于 (0, 1]");
  }
  return error(core::ErrorReason::invalid_argument, "未知 STFT 时间平滑方式");
}

struct PreparedFrame final {
  std::vector<data::ComplexSample> values;
  Window window;
};

[[nodiscard]] core::Result<PreparedFrame> prepare_frame(const data::SignalSlice& samples, std::uint64_t offset,
                                                        std::uint64_t frame_length, std::uint64_t fft_length,
                                                        const WindowSpecification& window_specification,
                                                        DetrendPolicy detrend_policy,
                                                        const std::shared_ptr<const std::atomic_bool>& cancellation) {
  if (offset >= samples.size()) {
    return error(core::ErrorReason::invalid_argument, "分析帧起点超出输入");
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("分析帧准备前已取消");
  }
  const auto available = std::min(frame_length, samples.size() - offset);
  auto window = make_window(window_specification, frame_length);
  if (!window) {
    return window.error();
  }
  data::ComplexSample mean{};
  constexpr std::uint64_t cancellation_interval = 4'096U;
  if (detrend_policy == DetrendPolicy::remove_mean) {
    if (samples.kind() == data::SignalKind::real) {
      const auto source = samples.real_values();
      for (std::uint64_t index = 0; index < available; ++index) {
        if (index % cancellation_interval == 0U && cancellation_requested(cancellation)) {
          return cancellation_status("去趋势统计已取消");
        }
        const auto value = source[static_cast<std::size_t>(offset + index)];
        if (!std::isfinite(value)) {
          return error(core::ErrorReason::invalid_argument, "样本包含 NaN 或 Inf");
        }
        mean.real += value;
      }
    } else {
      const auto source = samples.complex_values();
      for (std::uint64_t index = 0; index < available; ++index) {
        if (index % cancellation_interval == 0U && cancellation_requested(cancellation)) {
          return cancellation_status("去趋势统计已取消");
        }
        const auto value = source[static_cast<std::size_t>(offset + index)];
        if (!std::isfinite(value.real) || !std::isfinite(value.imag)) {
          return error(core::ErrorReason::invalid_argument, "样本包含 NaN 或 Inf");
        }
        mean.real += value.real;
        mean.imag += value.imag;
      }
    }
    mean.real /= static_cast<double>(available);
    mean.imag /= static_cast<double>(available);
  }
  PreparedFrame result;
  result.window = std::move(window.value());
  result.values.resize(static_cast<std::size_t>(fft_length));
  if (samples.kind() == data::SignalKind::real) {
    const auto source = samples.real_values();
    for (std::uint64_t index = 0; index < available; ++index) {
      if (index % cancellation_interval == 0U && cancellation_requested(cancellation)) {
        return cancellation_status("频谱输入加窗已取消");
      }
      const auto value = source[static_cast<std::size_t>(offset + index)];
      if (!std::isfinite(value)) {
        return error(core::ErrorReason::invalid_argument, "样本包含 NaN 或 Inf");
      }
      result.values[static_cast<std::size_t>(index)].real =
          (value - mean.real) * result.window.coefficients[static_cast<std::size_t>(index)];
    }
  } else {
    const auto source = samples.complex_values();
    for (std::uint64_t index = 0; index < available; ++index) {
      if (index % cancellation_interval == 0U && cancellation_requested(cancellation)) {
        return cancellation_status("频谱输入加窗已取消");
      }
      const auto value = source[static_cast<std::size_t>(offset + index)];
      if (!std::isfinite(value.real) || !std::isfinite(value.imag)) {
        return error(core::ErrorReason::invalid_argument, "样本包含 NaN 或 Inf");
      }
      const auto coefficient = result.window.coefficients[static_cast<std::size_t>(index)];
      result.values[static_cast<std::size_t>(index)] = {(value.real - mean.real) * coefficient,
                                                        (value.imag - mean.imag) * coefficient};
    }
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("频谱输入提交 FFT 前已取消");
  }
  return result;
}

struct SpectralFrame final {
  std::vector<double> frequency_hz;
  std::vector<double> amplitude;
  std::vector<double> power;
  std::vector<double> density;
  double equivalent_noise_bandwidth_hz{};
  compute::BackendProvenance provenance;
};

[[nodiscard]] core::Result<SpectralFrame>
calculate_frame(IFftPlan& plan, const data::SignalSlice& samples, std::uint64_t offset, std::uint64_t frame_length,
                std::uint64_t fft_length, double sample_rate_hz, double center_frequency_hz,
                const WindowSpecification& window_specification, SpectrumSidedness sidedness,
                SpectrumNormalization normalization, DetrendPolicy detrend_policy,
                const std::shared_ptr<const std::atomic_bool>& cancellation) {
  auto prepared =
      prepare_frame(samples, offset, frame_length, fft_length, window_specification, detrend_policy, cancellation);
  if (!prepared) {
    return prepared.error();
  }
  const auto plan_specification = plan.spec();
  if (plan_specification.length != fft_length || plan_specification.direction != FftDirection::forward) {
    return error(core::ErrorReason::invalid_argument, "复用 FFT 计划与当前分析帧不匹配");
  }
  auto transformed = plan.process(prepared.value().values);
  if (!transformed) {
    return transformed.error();
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("FFT 完成后分析已取消");
  }
  if (const auto status = validate_fft_output(transformed.value(), fft_length); !status) {
    return status;
  }
  const auto sum =
      std::accumulate(prepared.value().window.coefficients.begin(), prepared.value().window.coefficients.end(), 0.0);
  double sum_squares{};
  for (const auto coefficient : prepared.value().window.coefficients) {
    sum_squares += coefficient * coefficient;
  }
  double amplitude_denominator{};
  double density_denominator{};
  switch (normalization) {
  case SpectrumNormalization::coherent_gain:
    amplitude_denominator = sum;
    density_denominator = sample_rate_hz * sum * sum / static_cast<double>(frame_length);
    break;
  case SpectrumNormalization::window_power:
    amplitude_denominator = std::sqrt(static_cast<double>(frame_length) * sum_squares);
    density_denominator = sample_rate_hz * sum_squares;
    break;
  case SpectrumNormalization::none:
    amplitude_denominator = 1.0;
    density_denominator = sample_rate_hz;
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知频谱归一化方式");
  }
  if (!(amplitude_denominator > 0.0) || !(density_denominator > 0.0)) {
    return error(core::ErrorReason::internal_failure, "频谱归一化分母无效");
  }
  const auto bins = ordered_bins(fft_length, sidedness);
  SpectralFrame result;
  result.provenance = transformed.value().provenance;
  result.equivalent_noise_bandwidth_hz =
      prepared.value().window.equivalent_noise_bandwidth_bins * sample_rate_hz / static_cast<double>(frame_length);
  result.frequency_hz.reserve(bins.size());
  result.amplitude.reserve(bins.size());
  result.power.reserve(bins.size());
  result.density.reserve(bins.size());
  for (std::size_t output_index = 0; output_index < bins.size(); ++output_index) {
    if (output_index % 4'096U == 0U && cancellation_requested(cancellation)) {
      return cancellation_status("频谱归一化已取消");
    }
    const auto bin = bins[output_index];
    const auto squared = magnitude_squared(transformed.value().bins[static_cast<std::size_t>(bin)]);
    const auto doubled =
        sidedness == SpectrumSidedness::one_sided && bin != 0U && !(fft_length % 2U == 0U && bin == fft_length / 2U);
    auto power = squared / (amplitude_denominator * amplitude_denominator);
    auto density = squared / density_denominator;
    if (doubled) {
      power *= 2.0;
      density *= 2.0;
    }
    const auto amplitude = std::sqrt(power);
    const auto frequency =
        sidedness == SpectrumSidedness::one_sided
            ? static_cast<double>(bin) * sample_rate_hz / static_cast<double>(fft_length)
            : static_cast<double>(static_cast<std::int64_t>(bin) -
                                  (bin >= (fft_length + 1U) / 2U ? static_cast<std::int64_t>(fft_length) : 0)) *
                  sample_rate_hz / static_cast<double>(fft_length);
    result.frequency_hz.push_back(center_frequency_hz + frequency);
    result.amplitude.push_back(amplitude);
    result.power.push_back(power);
    result.density.push_back(density);
  }
  return result;
}

[[nodiscard]] bool is_density_quantity(SpectrumOutputQuantity quantity) noexcept {
  return quantity == SpectrumOutputQuantity::psd_dbfs_per_hz ||
         quantity == SpectrumOutputQuantity::linear_power_density;
}

[[nodiscard]] const std::vector<double>& select_power_domain(const SpectralFrame& frame,
                                                             SpectrumOutputQuantity quantity) {
  return is_density_quantity(quantity) ? frame.density : frame.power;
}

[[nodiscard]] std::vector<double> amplitude_from_power(std::span<const double> power) {
  std::vector<double> amplitude;
  amplitude.reserve(power.size());
  for (const auto value : power) {
    amplitude.push_back(std::sqrt(std::max(0.0, value)));
  }
  return amplitude;
}

/// Input is always canonical linear power or linear power density. Amplitude quantities are derived
/// after accumulation so one-sided amplitude remains the square root of one-sided power.
[[nodiscard]] std::vector<double> encode_quantity(std::span<const double> power_domain,
                                                  SpectrumOutputQuantity quantity) {
  std::vector<double> encoded;
  encoded.reserve(power_domain.size());
  for (const auto value : power_domain) {
    switch (quantity) {
    case SpectrumOutputQuantity::magnitude_dbfs:
      encoded.push_back(10.0 * std::log10(std::max(value, std::numeric_limits<double>::min())));
      break;
    case SpectrumOutputQuantity::linear_amplitude:
      encoded.push_back(std::sqrt(std::max(0.0, value)));
      break;
    case SpectrumOutputQuantity::power_dbfs:
    case SpectrumOutputQuantity::psd_dbfs_per_hz:
      encoded.push_back(clamped_db(value));
      break;
    case SpectrumOutputQuantity::linear_power:
    case SpectrumOutputQuantity::linear_power_density:
      encoded.push_back(value);
      break;
    }
  }
  return encoded;
}

class FrameAccumulator final {
public:
  FrameAccumulator(SpectrumAccumulationMode mode, double alpha, bool welch_default_average)
      : mode_(mode == SpectrumAccumulationMode::none && welch_default_average ? SpectrumAccumulationMode::linear_average
                                                                              : mode),
        alpha_(alpha) {}

  [[nodiscard]] core::Status add(std::span<const double> frame) {
    if (frame.empty()) {
      return error(core::ErrorReason::invalid_argument, "不能累积空频谱帧");
    }
    if (count_ == 0U) {
      values_.assign(frame.begin(), frame.end());
      if (mode_ == SpectrumAccumulationMode::linear_average) {
        // The first frame is already the first term of the running sum.
      }
      ++count_;
      return core::Status::success();
    }
    if (frame.size() != values_.size()) {
      return error(core::ErrorReason::internal_failure, "频谱累积帧尺寸不一致");
    }
    switch (mode_) {
    case SpectrumAccumulationMode::none:
      break;
    case SpectrumAccumulationMode::linear_average:
      for (std::size_t index = 0; index < values_.size(); ++index) {
        values_[index] += frame[index];
      }
      break;
    case SpectrumAccumulationMode::exponential_average:
      for (std::size_t index = 0; index < values_.size(); ++index) {
        values_[index] = (1.0 - alpha_) * values_[index] + alpha_ * frame[index];
      }
      break;
    case SpectrumAccumulationMode::maximum_hold:
      for (std::size_t index = 0; index < values_.size(); ++index) {
        values_[index] = std::max(values_[index], frame[index]);
      }
      break;
    }
    ++count_;
    return core::Status::success();
  }

  [[nodiscard]] core::Result<std::vector<double>> finish() && {
    if (count_ == 0U) {
      return error(core::ErrorReason::invalid_argument, "没有可累积的频谱帧");
    }
    if (mode_ == SpectrumAccumulationMode::linear_average) {
      const auto divisor = static_cast<double>(count_);
      for (auto& value : values_) {
        value /= divisor;
      }
    }
    return std::move(values_);
  }

private:
  SpectrumAccumulationMode mode_;
  double alpha_{};
  std::uint64_t count_{};
  std::vector<double> values_;
};

[[nodiscard]] std::uint64_t output_bin_count(std::uint64_t fft_length, SpectrumSidedness sidedness) noexcept {
  return sidedness == SpectrumSidedness::one_sided ? fft_length / 2U + 1U : fft_length;
}

[[nodiscard]] core::Result<std::vector<std::uint64_t>> spectrum_offsets(const SpectrumAnalysisSettings& settings,
                                                                        const ResolvedSpectrumDimensions& dimensions,
                                                                        std::uint64_t available_samples) {
  std::vector<std::uint64_t> offsets;
  if (settings.estimator.kind == PsdEstimatorKind::welch) {
    const auto overlap_samples = static_cast<std::uint64_t>(
        std::llround(settings.estimator.welch_overlap * static_cast<double>(dimensions.frame_length)));
    const auto hop = std::max<std::uint64_t>(1U, dimensions.frame_length - overlap_samples);
    const auto possible =
        available_samples < dimensions.frame_length ? 1U : 1U + (available_samples - dimensions.frame_length) / hop;
    auto count = possible;
    if (settings.estimator.welch_segment_count != 0U) {
      count = std::min(count, settings.estimator.welch_segment_count);
    }
    if (settings.accumulation.averaging_count != 0U) {
      count = std::min(count, settings.accumulation.averaging_count);
    }
    offsets.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
      offsets.push_back(index * hop);
    }
    return offsets;
  }
  auto count = std::uint64_t{1};
  if (settings.analysis_range_policy == AnalysisRangePolicy::all_complete_frames &&
      settings.accumulation.mode != SpectrumAccumulationMode::none && available_samples >= dimensions.frame_length) {
    count = available_samples / dimensions.frame_length;
    if (settings.accumulation.averaging_count != 0U) {
      count = std::min(count, settings.accumulation.averaging_count);
    }
  }
  offsets.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    offsets.push_back(index * dimensions.frame_length);
  }
  return offsets;
}

[[nodiscard]] std::string percent_encode(std::string_view value) {
  constexpr char hexadecimal[] = "0123456789ABCDEF";
  std::string output;
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '.' ||
        byte == '_' || byte == '-' || byte == '/') {
      output.push_back(static_cast<char>(byte));
    } else {
      output.push_back('%');
      output.push_back(hexadecimal[byte >> 4U]);
      output.push_back(hexadecimal[byte & 0x0fU]);
    }
  }
  return output;
}

[[nodiscard]] core::Result<std::string> percent_decode(std::string_view value) {
  const auto hex_value = [](char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'A' && character <= 'F') {
      return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    return -1;
  };
  std::string output;
  output.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') {
      output.push_back(value[index]);
      continue;
    }
    if (index + 2U >= value.size()) {
      return error(core::ErrorReason::invalid_argument, "参数字符串百分号转义不完整");
    }
    const auto high = hex_value(value[index + 1U]);
    const auto low = hex_value(value[index + 2U]);
    if (high < 0 || low < 0) {
      return error(core::ErrorReason::invalid_argument, "参数字符串百分号转义无效");
    }
    output.push_back(static_cast<char>((high << 4) | low));
    index += 2U;
  }
  return output;
}

[[nodiscard]] std::string canonical_double(double value) {
  if (value == 0.0) {
    value = 0.0;
  }
  std::array<char, 64U> buffer{};
  const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general,
                                       std::numeric_limits<double>::max_digits10);
  if (converted.ec != std::errc{}) {
    return {};
  }
  return std::string(buffer.data(), converted.ptr);
}

void append_field(std::string& output, std::string_view key, std::string_view value) {
  output.append(key);
  output.push_back('=');
  output.append(value);
  output.push_back('\n');
}

template <typename Enum> void append_enum(std::string& output, std::string_view key, Enum value) {
  append_field(output, key, std::to_string(static_cast<std::underlying_type_t<Enum>>(value)));
}

void append_double(std::string& output, std::string_view key, double value) {
  append_field(output, key, canonical_double(value));
}

void append_uint(std::string& output, std::string_view key, std::uint64_t value) {
  append_field(output, key, std::to_string(value));
}

void append_bool(std::string& output, std::string_view key, bool value) {
  append_field(output, key, value ? "1" : "0");
}

void append_string(std::string& output, std::string_view key, std::string_view value) {
  append_field(output, key, percent_encode(value));
}

void append_double_vector(std::string& output, std::string_view key, std::span<const double> values) {
  std::string encoded;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0U) {
      encoded.push_back(',');
    }
    encoded += canonical_double(values[index]);
  }
  append_field(output, key, encoded);
}

[[nodiscard]] core::Result<std::map<std::string, std::string, std::less<>>> parse_fields(std::string_view text) {
  std::map<std::string, std::string, std::less<>> fields;
  std::size_t position{};
  while (position < text.size()) {
    const auto line_end = text.find('\n', position);
    const auto end = line_end == std::string_view::npos ? text.size() : line_end;
    const auto line = text.substr(position, end - position);
    if (!line.empty()) {
      const auto separator = line.find('=');
      if (separator == std::string_view::npos || separator == 0U) {
        return error(core::ErrorReason::invalid_argument, "分析参数行缺少 key=value");
      }
      auto [iterator, inserted] =
          fields.emplace(std::string(line.substr(0U, separator)), std::string(line.substr(separator + 1U)));
      static_cast<void>(iterator);
      if (!inserted) {
        return error(core::ErrorReason::invalid_argument, "分析参数包含重复字段");
      }
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    position = line_end + 1U;
  }
  return fields;
}

[[nodiscard]] core::Status parse_uint_value(std::string_view text, std::uint64_t& value) {
  const auto converted = std::from_chars(text.data(), text.data() + text.size(), value);
  return converted.ec == std::errc{} && converted.ptr == text.data() + text.size()
             ? core::Status::success()
             : error(core::ErrorReason::invalid_argument, "分析参数整数格式无效");
}

[[nodiscard]] core::Status parse_double_value(std::string_view text, double& value) {
  const auto converted = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  return converted.ec == std::errc{} && converted.ptr == text.data() + text.size() && std::isfinite(value)
             ? core::Status::success()
             : error(core::ErrorReason::invalid_argument, "分析参数浮点格式无效");
}

template <typename Enum>
[[nodiscard]] core::Status parse_enum_field(const std::map<std::string, std::string, std::less<>>& fields,
                                            std::string_view key, Enum& value) {
  const auto found = fields.find(key);
  if (found == fields.end()) {
    return core::Status::success();
  }
  std::uint64_t parsed{};
  if (const auto status = parse_uint_value(found->second, parsed);
      !status || parsed > static_cast<std::uint64_t>(std::numeric_limits<std::underlying_type_t<Enum>>::max())) {
    return error(core::ErrorReason::invalid_argument, "分析参数枚举格式无效", std::string(key));
  }
  value = static_cast<Enum>(parsed);
  return core::Status::success();
}

[[nodiscard]] core::Status parse_uint_field(const std::map<std::string, std::string, std::less<>>& fields,
                                            std::string_view key, std::uint64_t& value) {
  const auto found = fields.find(key);
  return found == fields.end() ? core::Status::success() : parse_uint_value(found->second, value);
}

[[nodiscard]] core::Status parse_uint32_field(const std::map<std::string, std::string, std::less<>>& fields,
                                              std::string_view key, std::uint32_t& value) {
  std::uint64_t parsed = value;
  if (const auto status = parse_uint_field(fields, key, parsed); !status) {
    return status;
  }
  if (parsed > std::numeric_limits<std::uint32_t>::max()) {
    return error(core::ErrorReason::invalid_argument, "分析参数超出 uint32 范围", std::string(key));
  }
  value = static_cast<std::uint32_t>(parsed);
  return core::Status::success();
}

[[nodiscard]] core::Status parse_double_field(const std::map<std::string, std::string, std::less<>>& fields,
                                              std::string_view key, double& value) {
  const auto found = fields.find(key);
  return found == fields.end() ? core::Status::success() : parse_double_value(found->second, value);
}

[[nodiscard]] core::Status parse_bool_field(const std::map<std::string, std::string, std::less<>>& fields,
                                            std::string_view key, bool& value) {
  const auto found = fields.find(key);
  if (found == fields.end()) {
    return core::Status::success();
  }
  if (found->second == "0") {
    value = false;
    return core::Status::success();
  }
  if (found->second == "1") {
    value = true;
    return core::Status::success();
  }
  return error(core::ErrorReason::invalid_argument, "分析参数布尔格式无效", std::string(key));
}

[[nodiscard]] core::Status parse_string_field(const std::map<std::string, std::string, std::less<>>& fields,
                                              std::string_view key, std::string& value) {
  const auto found = fields.find(key);
  if (found == fields.end()) {
    return core::Status::success();
  }
  auto decoded = percent_decode(found->second);
  if (!decoded) {
    return decoded.error();
  }
  value = std::move(decoded.value());
  return core::Status::success();
}

[[nodiscard]] core::Result<std::vector<double>> parse_double_vector(std::string_view text) {
  std::vector<double> values;
  if (text.empty()) {
    return values;
  }
  std::size_t position{};
  while (position <= text.size()) {
    const auto separator = text.find(',', position);
    const auto end = separator == std::string_view::npos ? text.size() : separator;
    double value{};
    if (const auto status = parse_double_value(text.substr(position, end - position), value); !status) {
      return status;
    }
    values.push_back(value);
    if (separator == std::string_view::npos) {
      break;
    }
    position = separator + 1U;
  }
  return values;
}

[[nodiscard]] bool checked_multiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

[[nodiscard]] bool checked_add(std::uint64_t value, std::uint64_t& total) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) {
    return false;
  }
  total += value;
  return true;
}

} // namespace

std::string_view spectrum_output_unit(SpectrumOutputQuantity quantity, SpectrumNormalization normalization) noexcept {
  if (normalization == SpectrumNormalization::none) {
    switch (quantity) {
    case SpectrumOutputQuantity::magnitude_dbfs:
      return "dB(re 1 raw FFT amplitude unit)";
    case SpectrumOutputQuantity::power_dbfs:
      return "dB(re 1 raw FFT power unit)";
    case SpectrumOutputQuantity::psd_dbfs_per_hz:
      return "dB(re 1 raw FFT power unit/Hz)";
    case SpectrumOutputQuantity::linear_amplitude:
      return "raw FFT amplitude unit";
    case SpectrumOutputQuantity::linear_power:
      return "raw FFT power unit";
    case SpectrumOutputQuantity::linear_power_density:
      return "raw FFT power unit/Hz";
    }
    return "unknown";
  }
  switch (quantity) {
  case SpectrumOutputQuantity::magnitude_dbfs:
  case SpectrumOutputQuantity::power_dbfs:
    return "dBFS";
  case SpectrumOutputQuantity::psd_dbfs_per_hz:
    return "dBFS/Hz";
  case SpectrumOutputQuantity::linear_amplitude:
    return "FS";
  case SpectrumOutputQuantity::linear_power:
    return "FS^2";
  case SpectrumOutputQuantity::linear_power_density:
    return "FS^2/Hz";
  }
  return "unknown";
}

core::Result<FftResult> IFftBackend::execute(const FftSpec& spec, std::span<const data::ComplexSample> input) {
  auto plan = create_plan(spec);
  if (!plan) {
    return plan.error();
  }
  return plan.value()->process(input);
}

core::Result<std::shared_ptr<IPsdEstimator>> make_psd_estimator(std::shared_ptr<IFftBackend> backend) {
  if (!backend) {
    return error(core::ErrorReason::invalid_argument, "PSD 估计器要求有效 FFT 后端");
  }
  return std::shared_ptr<IPsdEstimator>(std::make_shared<PsdEstimator>(std::move(backend)));
}

core::Result<std::shared_ptr<IStftProcessor>> make_stft_processor(std::shared_ptr<IFftBackend> backend) {
  if (!backend) {
    return error(core::ErrorReason::invalid_argument, "STFT 处理器要求有效 FFT 后端");
  }
  return std::shared_ptr<IStftProcessor>(std::make_shared<StftProcessor>(std::move(backend)));
}

std::span<const WindowDescriptor> window_catalog() noexcept {
  static constexpr std::array catalog{
      WindowDescriptor{WindowKind::rectangular, "Rectangular", "矩形窗", "", 0.0, 0.0, 0.0, 1.0, 1.0,
                       "相干采样、需要最窄主瓣的稳态单音", "幅度保持最好，主瓣最窄",
                       "旁瓣最高，非 Bin 对齐时泄漏最明显"},
      WindowDescriptor{WindowKind::hann, "Hann", "汉宁窗", "", 0.0, 0.0, 0.0, 0.5, 1.5, "通用频谱与 STFT",
                       "幅度校正稳定，分辨率与动态范围平衡", "旁瓣衰减快于矩形窗"},
      WindowDescriptor{WindowKind::hamming, "Hamming", "海明窗", "", 0.0, 0.0, 0.0, 0.54, 1.3628,
                       "邻近强分量下的通用分析", "主瓣略宽于 Hann，幅度损失可校正", "第一旁瓣较低，远端衰减较慢"},
      WindowDescriptor{WindowKind::blackman, "Blackman", "布莱克曼窗", "", 0.0, 0.0, 0.0, 0.42, 1.7268,
                       "需要更低泄漏的多音分析", "主瓣较宽，弱窄峰可能被展宽", "旁瓣显著低于 Hann/Hamming"},
      WindowDescriptor{WindowKind::blackman_harris, "Blackman-Harris", "布莱克曼-哈里斯窗", "", 0.0, 0.0, 0.0, 0.35875,
                       2.0044, "强弱信号共存和高动态范围测量", "主瓣宽、幅度需 CG 校正",
                       "极低旁瓣，适合抑制强分量泄漏"},
      WindowDescriptor{WindowKind::flat_top, "Flat Top", "平顶窗", "", 0.0, 0.0, 0.0, 0.21557895, 3.7702,
                       "高精度单音幅度测量", "峰顶平坦、幅度误差最小但主瓣最宽", "频率分辨率较低，噪声带宽最大"},
      WindowDescriptor{WindowKind::kaiser, "Kaiser", "凯泽窗", "beta", 0.0, 50.0, 8.6, 0.4208, 1.7214,
                       "需要连续权衡主瓣宽度与旁瓣的分析", "beta 增大时主瓣变宽、幅度增益降低",
                       "beta 增大时旁瓣泄漏降低"},
      WindowDescriptor{WindowKind::tukey, "Tukey", "图基窗", "alpha", 0.0, 1.0, 0.5, 0.75, 1.2222,
                       "短突发和边界渐变的 STFT", "alpha 在矩形窗与 Hann 窗之间连续权衡",
                       "alpha 增大时边界泄漏降低但主瓣变宽"},
  };
  return catalog;
}

core::Result<Window> make_window(WindowKind kind, std::uint64_t length) {
  return make_window(WindowSpecification{kind, 0.0}, length);
}

core::Result<Window> make_window(const WindowSpecification& specification, std::uint64_t length) {
  if (length < 2U || length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return error(core::ErrorReason::invalid_argument, "窗长度必须至少为 2 且可由当前进程表示");
  }
  if (!std::isfinite(specification.parameter)) {
    return error(core::ErrorReason::invalid_argument, "窗函数参数必须为有限值");
  }
  switch (specification.kind) {
  case WindowKind::kaiser:
    if (specification.parameter < 0.0 || specification.parameter > 50.0) {
      return error(core::ErrorReason::invalid_argument, "Kaiser beta 必须位于 [0, 50]");
    }
    break;
  case WindowKind::tukey:
    if (specification.parameter < 0.0 || specification.parameter > 1.0) {
      return error(core::ErrorReason::invalid_argument, "Tukey alpha 必须位于 [0, 1]");
    }
    break;
  case WindowKind::rectangular:
  case WindowKind::hann:
  case WindowKind::hamming:
  case WindowKind::blackman:
  case WindowKind::blackman_harris:
  case WindowKind::flat_top:
    if (specification.parameter != 0.0) {
      return error(core::ErrorReason::invalid_argument, "无参数窗函数的 parameter 必须为 0");
    }
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知窗函数");
  }
  Window result;
  result.coefficients.resize(static_cast<std::size_t>(length));
  const auto denominator = static_cast<double>(length - 1U);
  const auto kaiser_denominator =
      specification.kind == WindowKind::kaiser ? std::cyl_bessel_i(0.0, specification.parameter) : 1.0;
  for (std::uint64_t index = 0; index < length; ++index) {
    const auto phase = 2.0 * std::numbers::pi * static_cast<double>(index) / denominator;
    double coefficient{};
    switch (specification.kind) {
    case WindowKind::rectangular:
      coefficient = 1.0;
      break;
    case WindowKind::hann:
      coefficient = 0.5 - 0.5 * std::cos(phase);
      break;
    case WindowKind::hamming:
      coefficient = 0.54 - 0.46 * std::cos(phase);
      break;
    case WindowKind::blackman:
      coefficient = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
      break;
    case WindowKind::blackman_harris:
      coefficient =
          0.35875 - 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase) - 0.01168 * std::cos(3.0 * phase);
      break;
    case WindowKind::flat_top:
      coefficient = 0.21557895 - 0.41663158 * std::cos(phase) + 0.277263158 * std::cos(2.0 * phase) -
                    0.083578947 * std::cos(3.0 * phase) + 0.006947368 * std::cos(4.0 * phase);
      break;
    case WindowKind::kaiser: {
      const auto position = 2.0 * static_cast<double>(index) / denominator - 1.0;
      coefficient =
          std::cyl_bessel_i(0.0, specification.parameter * std::sqrt(std::max(0.0, 1.0 - position * position))) /
          kaiser_denominator;
      break;
    }
    case WindowKind::tukey: {
      const auto alpha = specification.parameter;
      const auto position = static_cast<double>(index) / denominator;
      if (alpha == 0.0) {
        coefficient = 1.0;
      } else if (position < alpha / 2.0) {
        coefficient = 0.5 * (1.0 + std::cos(std::numbers::pi * (2.0 * position / alpha - 1.0)));
      } else if (position <= 1.0 - alpha / 2.0) {
        coefficient = 1.0;
      } else {
        coefficient = 0.5 * (1.0 + std::cos(std::numbers::pi * (2.0 * position / alpha - 2.0 / alpha + 1.0)));
      }
      break;
    }
    default:
      return error(core::ErrorReason::invalid_argument, "未知窗函数");
    }
    if (!std::isfinite(coefficient)) {
      return error(core::ErrorReason::internal_failure, "窗函数系数计算产生 NaN 或 Inf");
    }
    result.coefficients[static_cast<std::size_t>(index)] = coefficient;
  }
  const auto sum = std::accumulate(result.coefficients.begin(), result.coefficients.end(), 0.0);
  double sum_squares{};
  for (const auto coefficient : result.coefficients) {
    sum_squares += coefficient * coefficient;
  }
  if (!(sum > 0.0) || !(sum_squares > 0.0)) {
    return error(core::ErrorReason::internal_failure, "窗函数增益计算失败");
  }
  result.coherent_gain = sum / static_cast<double>(length);
  result.equivalent_noise_bandwidth_bins = static_cast<double>(length) * sum_squares / (sum * sum);
  return result;
}

core::Status validate_spectrum_analysis_settings(const SpectrumAnalysisSettings& settings,
                                                 std::uint64_t available_samples, data::SignalKind signal_kind) {
  if (const auto dimensions = resolve_spectrum_dimensions(settings, available_samples); !dimensions) {
    return dimensions.error();
  }
  if (const auto status = validate_window_specification(settings.window); !status) {
    return status;
  }
  if (settings.sidedness == SpectrumSidedness::one_sided && signal_kind == data::SignalKind::complex) {
    return error(core::ErrorReason::invalid_argument, "复信号不能使用会丢失负频率的一侧频谱");
  }
  switch (settings.analysis_range_policy) {
  case AnalysisRangePolicy::first_frame:
  case AnalysisRangePolicy::all_complete_frames:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知分析范围策略");
  }
  switch (settings.zero_padding_policy) {
  case ZeroPaddingPolicy::forbidden:
  case ZeroPaddingPolicy::enabled:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知频谱补零策略");
  }
  switch (settings.sidedness) {
  case SpectrumSidedness::one_sided:
  case SpectrumSidedness::two_sided_shifted:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知频谱单双边策略");
  }
  switch (settings.frequency_reference) {
  case data::FrequencyReference::baseband:
  case data::FrequencyReference::absolute:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知频率参考");
  }
  switch (settings.output_quantity) {
  case SpectrumOutputQuantity::magnitude_dbfs:
  case SpectrumOutputQuantity::power_dbfs:
  case SpectrumOutputQuantity::psd_dbfs_per_hz:
  case SpectrumOutputQuantity::linear_amplitude:
  case SpectrumOutputQuantity::linear_power:
  case SpectrumOutputQuantity::linear_power_density:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知频谱输出量");
  }
  switch (settings.normalization) {
  case SpectrumNormalization::coherent_gain:
  case SpectrumNormalization::window_power:
  case SpectrumNormalization::none:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知频谱归一化");
  }
  if (is_density_quantity(settings.output_quantity) && settings.normalization != SpectrumNormalization::window_power &&
      settings.normalization != SpectrumNormalization::none) {
    return error(core::ErrorReason::invalid_argument, "PSD/功率密度只允许 Window power 或 None 归一化");
  }
  if (!is_density_quantity(settings.output_quantity) &&
      settings.normalization != SpectrumNormalization::coherent_gain &&
      settings.normalization != SpectrumNormalization::none) {
    return error(core::ErrorReason::invalid_argument, "幅度、功率及 dBFS 输出只允许 Coherent gain 或 None 归一化");
  }
  switch (settings.detrend_policy) {
  case DetrendPolicy::none:
  case DetrendPolicy::remove_mean:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知去趋势策略");
  }
  switch (settings.estimator.kind) {
  case PsdEstimatorKind::periodogram:
    break;
  case PsdEstimatorKind::welch:
    if (!std::isfinite(settings.estimator.welch_overlap) || settings.estimator.welch_overlap < 0.0 ||
        settings.estimator.welch_overlap >= 1.0) {
      return error(core::ErrorReason::invalid_argument, "Welch overlap 必须位于 [0, 1)");
    }
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知 PSD 估计器");
  }
  switch (settings.accumulation.mode) {
  case SpectrumAccumulationMode::none:
  case SpectrumAccumulationMode::linear_average:
  case SpectrumAccumulationMode::maximum_hold:
    break;
  case SpectrumAccumulationMode::exponential_average:
    if (!std::isfinite(settings.accumulation.exponential_alpha) || !(settings.accumulation.exponential_alpha > 0.0) ||
        settings.accumulation.exponential_alpha > 1.0) {
      return error(core::ErrorReason::invalid_argument, "指数平均 alpha 必须位于 (0, 1]");
    }
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知频谱累积方式");
  }
  if (const auto status = validate_spectrum_smoothing(settings.smoothing); !status) {
    return status;
  }
  switch (settings.measurement_source) {
  case MeasurementSource::raw:
  case MeasurementSource::smoothed:
    return core::Status::success();
  }
  return error(core::ErrorReason::invalid_argument, "未知测量来源");
}

core::Status validate_spectrogram_analysis_settings(const SpectrogramAnalysisSettings& settings,
                                                    std::uint64_t available_samples, data::SignalKind signal_kind) {
  if (const auto dimensions = resolve_spectrogram_dimensions(settings, available_samples); !dimensions) {
    return dimensions.error();
  }
  if (const auto status = validate_window_specification(settings.window); !status) {
    return status;
  }
  if (settings.sidedness == SpectrumSidedness::one_sided && signal_kind == data::SignalKind::complex) {
    return error(core::ErrorReason::invalid_argument, "复信号不能使用会丢失负频率的一侧 STFT");
  }
  switch (settings.sidedness) {
  case SpectrumSidedness::one_sided:
  case SpectrumSidedness::two_sided_shifted:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知 STFT 单双边策略");
  }
  switch (settings.boundary_policy) {
  case SpectrogramBoundaryPolicy::drop_incomplete:
  case SpectrogramBoundaryPolicy::pad_incomplete:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知 STFT 边界策略");
  }
  switch (settings.padding_policy) {
  case ZeroPaddingPolicy::forbidden:
  case ZeroPaddingPolicy::enabled:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知 STFT 补零策略");
  }
  switch (settings.output_quantity) {
  case SpectrumOutputQuantity::magnitude_dbfs:
  case SpectrumOutputQuantity::power_dbfs:
  case SpectrumOutputQuantity::psd_dbfs_per_hz:
  case SpectrumOutputQuantity::linear_amplitude:
  case SpectrumOutputQuantity::linear_power:
  case SpectrumOutputQuantity::linear_power_density:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知 STFT 输出量");
  }
  switch (settings.normalization) {
  case SpectrumNormalization::coherent_gain:
  case SpectrumNormalization::window_power:
  case SpectrumNormalization::none:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知 STFT 归一化");
  }
  if (is_density_quantity(settings.output_quantity) && settings.normalization != SpectrumNormalization::window_power &&
      settings.normalization != SpectrumNormalization::none) {
    return error(core::ErrorReason::invalid_argument, "STFT PSD/功率密度只允许 Window power 或 None 归一化");
  }
  if (!is_density_quantity(settings.output_quantity) &&
      settings.normalization != SpectrumNormalization::coherent_gain &&
      settings.normalization != SpectrumNormalization::none) {
    return error(core::ErrorReason::invalid_argument, "STFT 幅度、功率及 dBFS 输出只允许 Coherent gain 或 None 归一化");
  }
  switch (settings.detrend_policy) {
  case DetrendPolicy::none:
  case DetrendPolicy::remove_mean:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知 STFT 去趋势策略");
  }
  return validate_spectrogram_smoothing(settings.smoothing);
}

core::Status validate_analysis_settings(const AnalysisSettingsSnapshot& settings, std::uint64_t available_samples,
                                        const data::SignalDescriptor& descriptor, bool include_spectrum,
                                        bool include_spectrogram) {
  if (settings.schema.rfind("signal.analysis-settings/1.", 0U) != 0U || settings.algorithm_version.empty()) {
    return error(core::ErrorReason::invalid_argument, "分析参数 schema 或算法版本无效");
  }
  if (!(descriptor.sample_rate_hz > 0.0) || !std::isfinite(descriptor.sample_rate_hz)) {
    return error(core::ErrorReason::invalid_argument, "分析参数要求有限正采样率");
  }
  if (!include_spectrum && !include_spectrogram) {
    return error(core::ErrorReason::invalid_argument, "至少必须验证频谱或时频图中的一个分析视图");
  }
  if (include_spectrum) {
    if (const auto status =
            validate_spectrum_analysis_settings(settings.spectrum, available_samples, descriptor.signal_kind);
        !status) {
      return status;
    }
  }
  if (include_spectrogram) {
    if (const auto status =
            validate_spectrogram_analysis_settings(settings.spectrogram, available_samples, descriptor.signal_kind);
        !status) {
      return status;
    }
  }
  if (!std::isfinite(settings.prefilter.group_delay_samples) || settings.prefilter.group_delay_samples < 0.0) {
    return error(core::ErrorReason::invalid_argument, "分析前滤波群时延必须为有限非负值");
  }
  switch (settings.prefilter.boundary) {
  case BoundaryPolicy::zero_pad:
  case BoundaryPolicy::preserve_state:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知分析前滤波边界策略");
  }
  if (!settings.prefilter.enabled) {
    return core::Status::success();
  }
  if (settings.prefilter.chain.nodes.empty()) {
    return error(core::ErrorReason::invalid_argument, "启用分析前滤波时必须提供滤波节点");
  }
  for (const auto& node : settings.prefilter.chain.nodes) {
    if (node.kind != NodeKind::fir_filter && node.kind != NodeKind::iir_filter) {
      return error(core::ErrorReason::invalid_argument, "分析前滤波只允许复用 FIR/IIR 处理链节点");
    }
    if (node.enabled) {
      if (const auto status = validate_node(node, descriptor); !status) {
        return status;
      }
    }
  }
  return core::Status::success();
}

core::Result<std::string> serialize_analysis_settings(const AnalysisSettingsSnapshot& settings) {
  if (settings.schema.rfind("signal.analysis-settings/1.", 0U) != 0U || settings.algorithm_version.empty()) {
    return error(core::ErrorReason::invalid_argument, "分析参数 schema 或算法版本无效");
  }
  auto chain = export_chain_template(settings.prefilter.chain);
  if (!chain) {
    return chain.error().with_context("序列化分析前滤波链");
  }

  std::string output;
  output.reserve(4096U + chain.value().size());
  append_string(output, "schema", settings.schema);
  append_string(output, "algorithm_version", settings.algorithm_version);

  append_enum(output, "spectrum.range", settings.spectrum.analysis_range_policy);
  append_uint(output, "spectrum.frame_length", settings.spectrum.frame_length);
  append_uint(output, "spectrum.fft_length", settings.spectrum.fft_length);
  append_enum(output, "spectrum.zero_padding", settings.spectrum.zero_padding_policy);
  append_enum(output, "spectrum.window.kind", settings.spectrum.window.kind);
  append_double(output, "spectrum.window.parameter", settings.spectrum.window.parameter);
  append_enum(output, "spectrum.sidedness", settings.spectrum.sidedness);
  append_enum(output, "spectrum.frequency_reference", settings.spectrum.frequency_reference);
  append_enum(output, "spectrum.output_quantity", settings.spectrum.output_quantity);
  append_enum(output, "spectrum.normalization", settings.spectrum.normalization);
  append_enum(output, "spectrum.detrend", settings.spectrum.detrend_policy);
  append_enum(output, "spectrum.estimator.kind", settings.spectrum.estimator.kind);
  append_double(output, "spectrum.estimator.welch_overlap", settings.spectrum.estimator.welch_overlap);
  append_uint(output, "spectrum.estimator.welch_segments", settings.spectrum.estimator.welch_segment_count);
  append_enum(output, "spectrum.accumulation.mode", settings.spectrum.accumulation.mode);
  append_uint(output, "spectrum.accumulation.count", settings.spectrum.accumulation.averaging_count);
  append_double(output, "spectrum.accumulation.alpha", settings.spectrum.accumulation.exponential_alpha);
  append_uint(output, "spectrum.accumulation.hold_generation", settings.spectrum.accumulation.hold_reset_generation);
  append_enum(output, "spectrum.smoothing.kind", settings.spectrum.smoothing.kind);
  append_uint(output, "spectrum.smoothing.window_length", settings.spectrum.smoothing.window_length);
  append_double(output, "spectrum.smoothing.gaussian_sigma", settings.spectrum.smoothing.gaussian_sigma);
  append_uint(output, "spectrum.smoothing.polynomial_order", settings.spectrum.smoothing.polynomial_order);
  append_enum(output, "spectrum.measurement_source", settings.spectrum.measurement_source);

  append_uint(output, "spectrogram.frame_length", settings.spectrogram.frame_length);
  append_uint(output, "spectrogram.fft_length", settings.spectrogram.fft_length);
  append_uint(output, "spectrogram.hop_length", settings.spectrogram.hop_length);
  append_enum(output, "spectrogram.window.kind", settings.spectrogram.window.kind);
  append_double(output, "spectrogram.window.parameter", settings.spectrogram.window.parameter);
  append_enum(output, "spectrogram.sidedness", settings.spectrogram.sidedness);
  append_enum(output, "spectrogram.boundary", settings.spectrogram.boundary_policy);
  append_enum(output, "spectrogram.padding", settings.spectrogram.padding_policy);
  append_enum(output, "spectrogram.detrend", settings.spectrogram.detrend_policy);
  append_enum(output, "spectrogram.output_quantity", settings.spectrogram.output_quantity);
  append_enum(output, "spectrogram.normalization", settings.spectrogram.normalization);
  append_enum(output, "spectrogram.smoothing.frequency_mode", settings.spectrogram.smoothing.frequency_mode);
  append_uint(output, "spectrogram.smoothing.frequency_kernel", settings.spectrogram.smoothing.frequency_kernel_length);
  append_double(output, "spectrogram.smoothing.frequency_sigma", settings.spectrogram.smoothing.frequency_sigma);
  append_enum(output, "spectrogram.smoothing.time_mode", settings.spectrogram.smoothing.time_mode);
  append_double(output, "spectrogram.smoothing.time_alpha", settings.spectrogram.smoothing.time_exponential_alpha);

  append_bool(output, "prefilter.enabled", settings.prefilter.enabled);
  append_enum(output, "prefilter.boundary", settings.prefilter.boundary);
  append_string(output, "prefilter.backend_id", settings.prefilter.backend_id);
  append_double(output, "prefilter.group_delay_samples", settings.prefilter.group_delay_samples);
  append_string(output, "prefilter.chain", chain.value());
  return output;
}

core::Result<AnalysisSettingsSnapshot> parse_analysis_settings(std::string_view text) {
  auto fields = parse_fields(text);
  if (!fields) {
    return fields.error();
  }
  AnalysisSettingsSnapshot result;
  if (const auto status = parse_string_field(fields.value(), "schema", result.schema); !status) {
    return status;
  }
  if (result.schema.rfind("signal.analysis-settings/1.", 0U) != 0U) {
    return error(core::ErrorReason::unavailable, "不支持的分析参数主版本", result.schema);
  }
  if (const auto status = parse_string_field(fields.value(), "algorithm_version", result.algorithm_version); !status) {
    return status;
  }

#define SIGNAL_PARSE_ENUM(key, member)                                                                                 \
  if (const auto status = parse_enum_field(fields.value(), key, member); !status) {                                    \
    return status;                                                                                                     \
  }
#define SIGNAL_PARSE_UINT(key, member)                                                                                 \
  if (const auto status = parse_uint_field(fields.value(), key, member); !status) {                                    \
    return status;                                                                                                     \
  }
#define SIGNAL_PARSE_UINT32(key, member)                                                                               \
  if (const auto status = parse_uint32_field(fields.value(), key, member); !status) {                                  \
    return status;                                                                                                     \
  }
#define SIGNAL_PARSE_DOUBLE(key, member)                                                                               \
  if (const auto status = parse_double_field(fields.value(), key, member); !status) {                                  \
    return status;                                                                                                     \
  }

  SIGNAL_PARSE_ENUM("spectrum.range", result.spectrum.analysis_range_policy);
  SIGNAL_PARSE_UINT("spectrum.frame_length", result.spectrum.frame_length);
  SIGNAL_PARSE_UINT("spectrum.fft_length", result.spectrum.fft_length);
  SIGNAL_PARSE_ENUM("spectrum.zero_padding", result.spectrum.zero_padding_policy);
  SIGNAL_PARSE_ENUM("spectrum.window.kind", result.spectrum.window.kind);
  SIGNAL_PARSE_DOUBLE("spectrum.window.parameter", result.spectrum.window.parameter);
  SIGNAL_PARSE_ENUM("spectrum.sidedness", result.spectrum.sidedness);
  SIGNAL_PARSE_ENUM("spectrum.frequency_reference", result.spectrum.frequency_reference);
  SIGNAL_PARSE_ENUM("spectrum.output_quantity", result.spectrum.output_quantity);
  SIGNAL_PARSE_ENUM("spectrum.normalization", result.spectrum.normalization);
  SIGNAL_PARSE_ENUM("spectrum.detrend", result.spectrum.detrend_policy);
  SIGNAL_PARSE_ENUM("spectrum.estimator.kind", result.spectrum.estimator.kind);
  SIGNAL_PARSE_DOUBLE("spectrum.estimator.welch_overlap", result.spectrum.estimator.welch_overlap);
  SIGNAL_PARSE_UINT("spectrum.estimator.welch_segments", result.spectrum.estimator.welch_segment_count);
  SIGNAL_PARSE_ENUM("spectrum.accumulation.mode", result.spectrum.accumulation.mode);
  SIGNAL_PARSE_UINT("spectrum.accumulation.count", result.spectrum.accumulation.averaging_count);
  SIGNAL_PARSE_DOUBLE("spectrum.accumulation.alpha", result.spectrum.accumulation.exponential_alpha);
  SIGNAL_PARSE_UINT("spectrum.accumulation.hold_generation", result.spectrum.accumulation.hold_reset_generation);
  SIGNAL_PARSE_ENUM("spectrum.smoothing.kind", result.spectrum.smoothing.kind);
  SIGNAL_PARSE_UINT32("spectrum.smoothing.window_length", result.spectrum.smoothing.window_length);
  SIGNAL_PARSE_DOUBLE("spectrum.smoothing.gaussian_sigma", result.spectrum.smoothing.gaussian_sigma);
  SIGNAL_PARSE_UINT32("spectrum.smoothing.polynomial_order", result.spectrum.smoothing.polynomial_order);
  SIGNAL_PARSE_ENUM("spectrum.measurement_source", result.spectrum.measurement_source);

  SIGNAL_PARSE_UINT("spectrogram.frame_length", result.spectrogram.frame_length);
  SIGNAL_PARSE_UINT("spectrogram.fft_length", result.spectrogram.fft_length);
  SIGNAL_PARSE_UINT("spectrogram.hop_length", result.spectrogram.hop_length);
  SIGNAL_PARSE_ENUM("spectrogram.window.kind", result.spectrogram.window.kind);
  SIGNAL_PARSE_DOUBLE("spectrogram.window.parameter", result.spectrogram.window.parameter);
  SIGNAL_PARSE_ENUM("spectrogram.sidedness", result.spectrogram.sidedness);
  SIGNAL_PARSE_ENUM("spectrogram.boundary", result.spectrogram.boundary_policy);
  SIGNAL_PARSE_ENUM("spectrogram.padding", result.spectrogram.padding_policy);
  SIGNAL_PARSE_ENUM("spectrogram.detrend", result.spectrogram.detrend_policy);
  SIGNAL_PARSE_ENUM("spectrogram.output_quantity", result.spectrogram.output_quantity);
  SIGNAL_PARSE_ENUM("spectrogram.normalization", result.spectrogram.normalization);
  SIGNAL_PARSE_ENUM("spectrogram.smoothing.frequency_mode", result.spectrogram.smoothing.frequency_mode);
  SIGNAL_PARSE_UINT32("spectrogram.smoothing.frequency_kernel", result.spectrogram.smoothing.frequency_kernel_length);
  SIGNAL_PARSE_DOUBLE("spectrogram.smoothing.frequency_sigma", result.spectrogram.smoothing.frequency_sigma);
  SIGNAL_PARSE_ENUM("spectrogram.smoothing.time_mode", result.spectrogram.smoothing.time_mode);
  SIGNAL_PARSE_DOUBLE("spectrogram.smoothing.time_alpha", result.spectrogram.smoothing.time_exponential_alpha);

  if (const auto status = parse_bool_field(fields.value(), "prefilter.enabled", result.prefilter.enabled); !status) {
    return status;
  }
  SIGNAL_PARSE_ENUM("prefilter.boundary", result.prefilter.boundary);
  if (const auto status = parse_string_field(fields.value(), "prefilter.backend_id", result.prefilter.backend_id);
      !status) {
    return status;
  }
  SIGNAL_PARSE_DOUBLE("prefilter.group_delay_samples", result.prefilter.group_delay_samples);
  std::string chain_text;
  if (const auto status = parse_string_field(fields.value(), "prefilter.chain", chain_text); !status) {
    return status;
  }
  if (!chain_text.empty()) {
    std::vector<std::string> serialized_tokens;
    std::istringstream tokens{chain_text};
    std::string token;
    while (tokens >> std::quoted(token)) {
      serialized_tokens.push_back(token);
    }
    auto chain = import_chain_template(chain_text, serialized_tokens);
    if (!chain) {
      return chain.error().with_context("解析分析前滤波链");
    }
    result.prefilter.chain = std::move(chain.value());
  }

#undef SIGNAL_PARSE_ENUM
#undef SIGNAL_PARSE_UINT
#undef SIGNAL_PARSE_UINT32
#undef SIGNAL_PARSE_DOUBLE

  if (const auto status = validate_window_specification(result.spectrum.window); !status) {
    return status;
  }
  if (const auto status = validate_window_specification(result.spectrogram.window); !status) {
    return status;
  }
  if (const auto status = validate_spectrum_smoothing(result.spectrum.smoothing); !status) {
    return status;
  }
  if (const auto status = validate_spectrogram_smoothing(result.spectrogram.smoothing); !status) {
    return status;
  }
  const auto available_samples =
      std::max<std::uint64_t>({2U, result.spectrum.frame_length, result.spectrogram.frame_length});
  if (const auto status =
          validate_spectrum_analysis_settings(result.spectrum, available_samples, data::SignalKind::real);
      !status) {
    return status.with_context("解析频谱参数");
  }
  if (const auto status =
          validate_spectrogram_analysis_settings(result.spectrogram, available_samples, data::SignalKind::real);
      !status) {
    return status.with_context("解析 STFT 参数");
  }
  switch (result.prefilter.boundary) {
  case BoundaryPolicy::zero_pad:
  case BoundaryPolicy::preserve_state:
    break;
  default:
    return error(core::ErrorReason::invalid_argument, "未知分析前滤波边界策略");
  }
  if (!std::isfinite(result.prefilter.group_delay_samples) || result.prefilter.group_delay_samples < 0.0) {
    return error(core::ErrorReason::invalid_argument, "分析前滤波群时延必须为有限非负值");
  }
  return result;
}

core::Result<AnalysisSettingsHash> hash_analysis_settings(const AnalysisSettingsSnapshot& settings) {
  auto serialized = serialize_analysis_settings(settings);
  if (!serialized) {
    return serialized.error();
  }
  const auto bytes = std::as_bytes(std::span<const char>{serialized.value().data(), serialized.value().size()});
  auto digest = core::hash_bytes(bytes);
  if (!digest) {
    return digest.error();
  }
  return AnalysisSettingsHash{"sha256", digest.value().hex()};
}

core::Result<AnalysisCostEstimate> estimate_analysis_cost(const AnalysisSettingsSnapshot& settings,
                                                          std::uint64_t available_samples, double sample_rate_hz,
                                                          std::uint64_t host_budget_bytes,
                                                          std::uint64_t device_budget_bytes, bool include_spectrum,
                                                          bool include_spectrogram) {
  if (!(sample_rate_hz > 0.0) || !std::isfinite(sample_rate_hz)) {
    return error(core::ErrorReason::invalid_argument, "资源估计要求有限正采样率");
  }
  if (!include_spectrum && !include_spectrogram) {
    return error(core::ErrorReason::invalid_argument, "资源估计至少要求一个可见分析视图");
  }
  std::optional<ResolvedSpectrumDimensions> spectrum;
  std::optional<ResolvedSpectrogramDimensions> spectrogram;
  std::vector<std::uint64_t> offsets;
  if (include_spectrum) {
    auto resolved = resolve_spectrum_dimensions(settings.spectrum, available_samples);
    if (!resolved) {
      return resolved.error();
    }
    auto resolved_offsets = spectrum_offsets(settings.spectrum, resolved.value(), available_samples);
    if (!resolved_offsets) {
      return resolved_offsets.error();
    }
    spectrum = resolved.value();
    offsets = std::move(resolved_offsets.value());
  }
  if (include_spectrogram) {
    auto resolved = resolve_spectrogram_dimensions(settings.spectrogram, available_samples);
    if (!resolved) {
      return resolved.error();
    }
    spectrogram = resolved.value();
  }

  AnalysisCostEstimate estimate;
  estimate.input_samples = available_samples;
  if (spectrum) {
    estimate.spectrum_frame_length = spectrum->frame_length;
    estimate.spectrum_fft_length = spectrum->fft_length;
    estimate.spectrum_output_bins = output_bin_count(spectrum->fft_length, settings.spectrum.sidedness);
    estimate.spectrum_segment_count = offsets.size();
  }
  if (spectrogram) {
    estimate.spectrogram_frame_length = spectrogram->frame_length;
    estimate.spectrogram_fft_length = spectrogram->fft_length;
    estimate.spectrogram_rows = spectrogram->rows;
    estimate.spectrogram_columns = output_bin_count(spectrogram->fft_length, settings.spectrogram.sidedness);
  }
  if (estimate.spectrum_segment_count > std::numeric_limits<std::uint64_t>::max() - estimate.spectrogram_rows) {
    return error(core::ErrorReason::invalid_argument, "资源估计 FFT 次数溢出");
  }
  estimate.fft_execution_count = estimate.spectrum_segment_count + estimate.spectrogram_rows;
  if (spectrum) {
    estimate.spectrum_bin_spacing_hz = sample_rate_hz / static_cast<double>(estimate.spectrum_fft_length);
    auto spectrum_window = make_window(settings.spectrum.window, estimate.spectrum_frame_length);
    if (!spectrum_window) {
      return spectrum_window.error();
    }
    estimate.spectrum_rbw_hz = spectrum_window.value().equivalent_noise_bandwidth_bins * sample_rate_hz /
                               static_cast<double>(estimate.spectrum_frame_length);
  }
  if (spectrogram) {
    estimate.spectrogram_time_step_seconds = static_cast<double>(spectrogram->hop_length) / sample_rate_hz;
  }

  std::uint64_t matrix_elements{};
  std::uint64_t matrix_bytes{};
  if (!checked_multiply(estimate.spectrogram_rows, estimate.spectrogram_columns, matrix_elements) ||
      !checked_multiply(matrix_elements, 128U, matrix_bytes)) {
    return error(core::ErrorReason::invalid_argument, "资源估计 STFT 矩阵尺寸溢出");
  }
  std::uint64_t spectrum_bytes{};
  if (!checked_multiply(estimate.spectrum_output_bins, 256U, spectrum_bytes)) {
    return error(core::ErrorReason::invalid_argument, "资源估计频谱尺寸溢出");
  }
  std::uint64_t input_bytes{};
  std::uint64_t fft_workspace_bytes{};
  const auto maximum_fft_length = std::max(estimate.spectrum_fft_length, estimate.spectrogram_fft_length);
  if (!checked_multiply(available_samples, sizeof(data::ComplexSample) * 2U, input_bytes) ||
      !checked_multiply(maximum_fft_length, sizeof(data::ComplexSample) * 6U, fft_workspace_bytes)) {
    return error(core::ErrorReason::invalid_argument, "资源估计输入或 FFT 工作区尺寸溢出");
  }
  estimate.host_memory_bytes = matrix_bytes;
  if (!checked_add(spectrum_bytes, estimate.host_memory_bytes) ||
      !checked_add(input_bytes, estimate.host_memory_bytes) ||
      !checked_add(fft_workspace_bytes, estimate.host_memory_bytes)) {
    return error(core::ErrorReason::invalid_argument, "资源估计主机内存溢出");
  }
  if (!checked_multiply(maximum_fft_length, sizeof(data::ComplexSample) * 3U, estimate.device_memory_bytes)) {
    return error(core::ErrorReason::invalid_argument, "资源估计设备内存溢出");
  }
  const auto fft_cost = [](std::uint64_t length) {
    return 5.0 * static_cast<double>(length) * std::log2(static_cast<double>(length));
  };
  estimate.estimated_operations =
      static_cast<double>(estimate.spectrum_segment_count) * fft_cost(estimate.spectrum_fft_length) +
      static_cast<double>(estimate.spectrogram_rows) * fft_cost(estimate.spectrogram_fft_length);
  estimate.within_host_budget = host_budget_bytes == 0U || estimate.host_memory_bytes <= host_budget_bytes;
  estimate.within_device_budget = device_budget_bytes == 0U || estimate.device_memory_bytes <= device_budget_bytes;
  return estimate;
}

AnalysisInvalidation classify_analysis_change(const AnalysisSettingsSnapshot& before,
                                              const AnalysisSettingsSnapshot& after) {
  AnalysisInvalidation invalidation = AnalysisInvalidation::none;
  const auto before_chain = export_chain_template(before.prefilter.chain);
  const auto after_chain = export_chain_template(after.prefilter.chain);
  const auto prefilter_changed = before.prefilter.enabled != after.prefilter.enabled ||
                                 before.prefilter.boundary != after.prefilter.boundary ||
                                 before.prefilter.backend_id != after.prefilter.backend_id ||
                                 before.prefilter.group_delay_samples != after.prefilter.group_delay_samples ||
                                 !before_chain || !after_chain || before_chain.value() != after_chain.value();
  if (prefilter_changed) {
    invalidation |= AnalysisInvalidation::prefilter;
    invalidation |= AnalysisInvalidation::spectrum_transform;
    invalidation |= AnalysisInvalidation::spectrogram_transform;
  }
  if (before.schema != after.schema || before.algorithm_version != after.algorithm_version) {
    invalidation |= AnalysisInvalidation::spectrum_transform;
    invalidation |= AnalysisInvalidation::spectrogram_transform;
  }

  auto before_spectrum = before.spectrum;
  auto after_spectrum = after.spectrum;
  const auto spectrum_smoothing_changed = before_spectrum.smoothing != after_spectrum.smoothing;
  before_spectrum.smoothing = {};
  after_spectrum.smoothing = {};
  before_spectrum.measurement_source = MeasurementSource::raw;
  after_spectrum.measurement_source = MeasurementSource::raw;
  if (before_spectrum != after_spectrum) {
    invalidation |= AnalysisInvalidation::spectrum_transform;
  } else if (spectrum_smoothing_changed) {
    invalidation |= AnalysisInvalidation::spectrum_smoothing;
  }

  auto before_spectrogram = before.spectrogram;
  auto after_spectrogram = after.spectrogram;
  const auto spectrogram_smoothing_changed = before_spectrogram.smoothing != after_spectrogram.smoothing;
  before_spectrogram.smoothing = {};
  after_spectrogram.smoothing = {};
  if (before_spectrogram != after_spectrogram) {
    invalidation |= AnalysisInvalidation::spectrogram_transform;
  } else if (spectrogram_smoothing_changed) {
    invalidation |= AnalysisInvalidation::spectrogram_smoothing;
  }
  return invalidation;
}

core::Result<std::vector<double>> smooth_spectrum(std::span<const double> values,
                                                  const SpectrumSmoothingSettings& settings,
                                                  std::shared_ptr<const std::atomic_bool> cancellation) {
  if (values.empty() || std::ranges::any_of(values, [](double value) { return !std::isfinite(value); })) {
    return error(core::ErrorReason::invalid_argument, "频谱平滑输入必须为非空有限序列");
  }
  if (const auto status = validate_spectrum_smoothing(settings); !status) {
    return status;
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("频谱平滑开始前已取消");
  }
  if (settings.kind == SpectrumSmoothingKind::none) {
    return std::vector<double>(values.begin(), values.end());
  }
  std::vector<double> kernel;
  if (settings.kind == SpectrumSmoothingKind::moving_average) {
    kernel.assign(settings.window_length, 1.0 / static_cast<double>(settings.window_length));
  } else if (settings.kind == SpectrumSmoothingKind::gaussian) {
    kernel.resize(settings.window_length);
    const auto half = static_cast<std::int64_t>(settings.window_length / 2U);
    double sum{};
    for (std::int64_t offset = -half; offset <= half; ++offset) {
      const auto scaled = static_cast<double>(offset) / settings.gaussian_sigma;
      const auto coefficient = std::exp(-0.5 * scaled * scaled);
      kernel[static_cast<std::size_t>(offset + half)] = coefficient;
      sum += coefficient;
    }
    for (auto& coefficient : kernel) {
      coefficient /= sum;
    }
  } else {
    auto savitzky = analysis_internal::savitzky_golay_kernel(settings.window_length, settings.polynomial_order);
    if (!savitzky) {
      return savitzky.error();
    }
    kernel = std::move(savitzky.value());
  }
  auto smoothed = analysis_internal::convolve_centered(values, kernel);
  if (!smoothed) {
    return smoothed.error();
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("频谱平滑完成后已取消");
  }
  return smoothed;
}

core::Result<data::SignalBuffer> apply_analysis_prefilter(ISignalKernelBackend& backend,
                                                          const data::SignalSlice& samples,
                                                          const data::SignalDescriptor& descriptor,
                                                          const AnalysisPrefilterSettings& settings,
                                                          std::shared_ptr<const std::atomic_bool> cancellation) {
  if (cancellation_requested(cancellation)) {
    return cancellation_status("分析前滤波开始前已取消");
  }
  if (!settings.enabled) {
    if (samples.kind() == data::SignalKind::real) {
      return data::SignalBuffer::from_real(
          std::vector<double>(samples.real_values().begin(), samples.real_values().end()));
    }
    return data::SignalBuffer::from_complex(
        std::vector<data::ComplexSample>(samples.complex_values().begin(), samples.complex_values().end()));
  }
  if (!settings.backend_id.empty() && settings.backend_id != backend.backend_id()) {
    return error(core::ErrorReason::unavailable, "分析前滤波请求的后端与实际后端不一致",
                 settings.backend_id + " != " + std::string(backend.backend_id()));
  }
  if (settings.chain.nodes.empty()) {
    return error(core::ErrorReason::invalid_argument, "启用分析前滤波时必须提供滤波节点");
  }
  for (const auto& node : settings.chain.nodes) {
    if (node.kind != NodeKind::fir_filter && node.kind != NodeKind::iir_filter) {
      return error(core::ErrorReason::invalid_argument, "分析前滤波只允许 FIR/IIR 节点，不得提前执行通道或重采样业务");
    }
  }
  auto processed = process_chain(backend, {samples, descriptor, settings.chain, settings.boundary, cancellation}, {});
  if (!processed) {
    return processed.error();
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("分析前滤波完成后已取消");
  }
  return std::move(processed.value().samples);
}

core::Result<SpectrumResult> calculate_spectrum(IFftBackend& backend, const data::SignalSlice& samples,
                                                const SpectrumRequest& request) {
  if (!(request.sample_rate_hz > 0.0) || !std::isfinite(request.sample_rate_hz) ||
      !std::isfinite(request.center_frequency_hz) || samples.size() < 2U) {
    return error(core::ErrorReason::invalid_argument, "频谱请求的采样率、中心频率或样本无效");
  }
  if (request.sidedness == SpectrumSidedness::one_sided && samples.kind() == data::SignalKind::complex) {
    return error(core::ErrorReason::invalid_argument, "复信号不能使用会丢失负频率的一侧频谱");
  }
  const auto window = make_window(request.window, samples.size());
  if (!window) {
    return window.error();
  }
  const auto input = to_complex(samples, window.value().coefficients);
  if (!input) {
    return input.error();
  }
  const FftSpec spec{samples.size(), FftDirection::forward};
  const auto validation = backend.validate(spec);
  if (!validation) {
    return validation;
  }
  auto transformed = backend.execute(spec, input.value());
  if (!transformed) {
    return transformed.error();
  }
  if (const auto output_status = validate_fft_output(transformed.value(), samples.size()); !output_status) {
    return output_status;
  }
  const auto bins = ordered_bins(samples.size(), request.sidedness);
  SpectrumResult result;
  result.provenance = transformed.value().provenance;
  result.frequency_hz.reserve(bins.size());
  result.magnitude_dbfs.reserve(bins.size());
  const auto coherent_sum = window.value().coherent_gain * static_cast<double>(samples.size());
  for (const auto bin : bins) {
    const auto& value = transformed.value().bins[static_cast<std::size_t>(bin)];
    auto amplitude = std::sqrt(magnitude_squared(value)) / coherent_sum;
    if (request.sidedness == SpectrumSidedness::one_sided && bin != 0U &&
        !(samples.size() % 2U == 0U && bin == samples.size() / 2U)) {
      amplitude *= 2.0;
    }
    const auto frequency =
        request.sidedness == SpectrumSidedness::one_sided
            ? static_cast<double>(bin) * request.sample_rate_hz / static_cast<double>(samples.size())
            : (static_cast<double>(static_cast<std::int64_t>(bin) - (bin >= (samples.size() + 1U) / 2U
                                                                         ? static_cast<std::int64_t>(samples.size())
                                                                         : 0)) *
               request.sample_rate_hz / static_cast<double>(samples.size()));
    result.frequency_hz.push_back(request.center_frequency_hz + frequency);
    result.magnitude_dbfs.push_back(20.0 * std::log10(std::max(amplitude, std::numeric_limits<double>::min())));
  }
  return result;
}

core::Result<PsdResult> calculate_psd(IFftBackend& backend, const data::SignalSlice& samples,
                                      const SpectrumRequest& request) {
  if (!(request.sample_rate_hz > 0.0) || !std::isfinite(request.sample_rate_hz) ||
      !std::isfinite(request.center_frequency_hz) || samples.size() < 2U) {
    return error(core::ErrorReason::invalid_argument, "PSD 请求的采样率、中心频率或样本无效");
  }
  if (request.sidedness == SpectrumSidedness::one_sided && samples.kind() == data::SignalKind::complex) {
    return error(core::ErrorReason::invalid_argument, "复信号不能使用一侧 PSD");
  }
  const auto window = make_window(request.window, samples.size());
  if (!window) {
    return window.error();
  }
  const auto input = to_complex(samples, window.value().coefficients);
  if (!input) {
    return input.error();
  }
  auto transformed = backend.execute({samples.size(), FftDirection::forward}, input.value());
  if (!transformed) {
    return transformed.error();
  }
  if (const auto output_status = validate_fft_output(transformed.value(), samples.size()); !output_status) {
    return output_status;
  }
  const auto bins = ordered_bins(samples.size(), request.sidedness);
  double window_energy{};
  for (const auto coefficient : window.value().coefficients) {
    window_energy += coefficient * coefficient;
  }
  const auto density_denominator = request.sample_rate_hz * window_energy;
  PsdResult result;
  result.provenance = transformed.value().provenance;
  result.equivalent_noise_bandwidth_hz =
      window.value().equivalent_noise_bandwidth_bins * request.sample_rate_hz / static_cast<double>(samples.size());
  result.frequency_hz.reserve(bins.size());
  result.db_per_hz.reserve(bins.size());
  for (const auto bin : bins) {
    auto density = magnitude_squared(transformed.value().bins[static_cast<std::size_t>(bin)]) / density_denominator;
    if (request.sidedness == SpectrumSidedness::one_sided && bin != 0U &&
        !(samples.size() % 2U == 0U && bin == samples.size() / 2U)) {
      density *= 2.0;
    }
    const auto frequency =
        request.sidedness == SpectrumSidedness::one_sided
            ? static_cast<double>(bin) * request.sample_rate_hz / static_cast<double>(samples.size())
            : (static_cast<double>(static_cast<std::int64_t>(bin) - (bin >= (samples.size() + 1U) / 2U
                                                                         ? static_cast<std::int64_t>(samples.size())
                                                                         : 0)) *
               request.sample_rate_hz / static_cast<double>(samples.size()));
    result.frequency_hz.push_back(request.center_frequency_hz + frequency);
    result.db_per_hz.push_back(clamped_db(density));
  }
  return result;
}

core::Result<StftResult> calculate_stft(IFftBackend& backend, const data::SignalSlice& samples,
                                        const StftRequest& request) {
  if (request.fft_length < 2U || request.hop_length == 0U || request.hop_length > request.fft_length ||
      samples.size() < request.fft_length) {
    return error(core::ErrorReason::invalid_argument, "STFT 帧长、步长或样本数无效");
  }
  const auto rows = 1U + (samples.size() - request.fft_length) / request.hop_length;
  StftResult result;
  result.rows = rows;
  for (std::uint64_t row = 0; row < rows; ++row) {
    const auto frame = samples.slice(row * request.hop_length, request.fft_length);
    if (!frame) {
      return frame.error();
    }
    const auto psd =
        calculate_psd(backend, frame.value(),
                      {request.sample_rate_hz, request.center_frequency_hz, request.window, request.sidedness});
    if (!psd) {
      return psd.error();
    }
    if (row == 0U) {
      result.frequency_hz = psd.value().frequency_hz;
      result.columns = result.frequency_hz.size();
      result.provenance = psd.value().provenance;
      result.db_per_hz.reserve(static_cast<std::size_t>(rows * result.columns));
    } else if (const auto status = validate_provenance_consistency(result.provenance, psd.value().provenance, "STFT");
               !status) {
      return status;
    }
    result.time_seconds.push_back(
        (static_cast<double>(row * request.hop_length) + static_cast<double>(request.fft_length - 1U) / 2.0) /
        request.sample_rate_hz);
    for (const auto value : psd.value().db_per_hz) {
      result.db_per_hz.push_back(static_cast<float>(value));
    }
  }
  return result;
}

core::Result<SpectrumResult> calculate_spectrum(IFftBackend& backend, const data::SignalSlice& samples,
                                                double sample_rate_hz, double center_frequency_hz,
                                                const SpectrumAnalysisSettings& settings,
                                                std::shared_ptr<const std::atomic_bool> cancellation) {
  if (!(sample_rate_hz > 0.0) || !std::isfinite(sample_rate_hz) || !std::isfinite(center_frequency_hz)) {
    return error(core::ErrorReason::invalid_argument, "参数化频谱的采样率或中心频率无效");
  }
  if (const auto status = validate_spectrum_analysis_settings(settings, samples.size(), samples.kind()); !status) {
    return status;
  }
  const auto dimensions = resolve_spectrum_dimensions(settings, samples.size());
  if (!dimensions) {
    return dimensions.error();
  }
  const auto offsets = spectrum_offsets(settings, dimensions.value(), samples.size());
  if (!offsets) {
    return offsets.error();
  }
  const auto frequency_center =
      settings.frequency_reference == data::FrequencyReference::absolute ? center_frequency_hz : 0.0;
  const auto welch = settings.estimator.kind == PsdEstimatorKind::welch;
  FrameAccumulator power_accumulator{settings.accumulation.mode, settings.accumulation.exponential_alpha, welch};
  FrameAccumulator density_accumulator{settings.accumulation.mode, settings.accumulation.exponential_alpha, welch};
  auto plan = backend.create_plan({dimensions.value().fft_length, FftDirection::forward});
  if (!plan) {
    return plan.error();
  }
  std::vector<double> frequencies;
  compute::BackendProvenance provenance;
  double enbw_hz{};
  for (const auto offset : offsets.value()) {
    auto frame = calculate_frame(*plan.value(), samples, offset, dimensions.value().frame_length,
                                 dimensions.value().fft_length, sample_rate_hz, frequency_center, settings.window,
                                 settings.sidedness, settings.normalization, settings.detrend_policy, cancellation);
    if (!frame) {
      return frame.error();
    }
    if (frequencies.empty()) {
      frequencies = frame.value().frequency_hz;
      provenance = frame.value().provenance;
      enbw_hz = frame.value().equivalent_noise_bandwidth_hz;
    } else if (const auto status = validate_provenance_consistency(provenance, frame.value().provenance, "频谱/PSD");
               !status) {
      return status;
    }
    if (const auto status = power_accumulator.add(frame.value().power); !status) {
      return status;
    }
    if (const auto status = density_accumulator.add(frame.value().density); !status) {
      return status;
    }
  }
  auto raw_power = std::move(power_accumulator).finish();
  auto raw_density = std::move(density_accumulator).finish();
  if (!raw_power || !raw_density) {
    return !raw_power ? raw_power.error() : raw_density.error();
  }
  auto raw_power_values = std::move(raw_power).value();
  auto raw_density_values = std::move(raw_density).value();
  const auto& raw_linear = is_density_quantity(settings.output_quantity) ? raw_density_values : raw_power_values;
  auto smoothed_linear = smooth_spectrum(raw_linear, settings.smoothing, cancellation);
  if (!smoothed_linear) {
    return smoothed_linear.error();
  }
  std::vector<double> smoothed_power = smoothed_linear.value();
  if (is_density_quantity(settings.output_quantity)) {
    auto smoothed_power_result = smooth_spectrum(raw_power_values, settings.smoothing, cancellation);
    if (!smoothed_power_result) {
      return smoothed_power_result.error();
    }
    smoothed_power = std::move(smoothed_power_result.value());
  }
  const auto raw_amplitude = amplitude_from_power(raw_power_values);
  AnalysisSettingsSnapshot hash_snapshot;
  hash_snapshot.spectrum = settings;
  auto settings_hash = hash_analysis_settings(hash_snapshot);
  if (!settings_hash) {
    return settings_hash.error();
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("参数化频谱发布前已取消");
  }
  SpectrumResult result;
  result.frequency_hz = std::move(frequencies);
  result.magnitude_dbfs = encode_quantity(smoothed_power, SpectrumOutputQuantity::magnitude_dbfs);
  result.provenance = std::move(provenance);
  result.raw_linear_values = raw_linear;
  result.raw_amplitude_linear = raw_amplitude;
  result.raw_power_linear = raw_power_values;
  result.raw_density_linear = raw_density_values;
  result.raw_values = encode_quantity(raw_linear, settings.output_quantity);
  result.values = encode_quantity(smoothed_linear.value(), settings.output_quantity);
  result.output_quantity = settings.output_quantity;
  result.normalization = settings.normalization;
  result.settings_hash = std::move(settings_hash.value());
  result.frame_length = dimensions.value().frame_length;
  result.fft_length = dimensions.value().fft_length;
  result.bin_spacing_hz = sample_rate_hz / static_cast<double>(dimensions.value().fft_length);
  result.equivalent_noise_bandwidth_hz = enbw_hz;
  result.resolution_bandwidth_hz = enbw_hz;
  return result;
}

core::Result<SpectrumResult> resmooth_spectrum(const SpectrumResult& source, const SpectrumAnalysisSettings& settings,
                                               std::shared_ptr<const std::atomic_bool> cancellation) {
  if (source.raw_linear_values.empty() || source.raw_linear_values.size() != source.frequency_hz.size() ||
      source.raw_amplitude_linear.size() != source.raw_linear_values.size() ||
      (settings.frame_length != 0U && source.frame_length != settings.frame_length) ||
      (settings.fft_length != 0U && source.fft_length != settings.fft_length) ||
      source.output_quantity != settings.output_quantity || source.normalization != settings.normalization) {
    return error(core::ErrorReason::invalid_argument, "频谱平滑复用要求变换尺寸、输出类型和未平滑线性结果保持不变");
  }
  auto smoothed_linear = smooth_spectrum(source.raw_linear_values, settings.smoothing, cancellation);
  if (!smoothed_linear) {
    return smoothed_linear.error();
  }
  AnalysisSettingsSnapshot hash_snapshot;
  hash_snapshot.spectrum = settings;
  auto settings_hash = hash_analysis_settings(hash_snapshot);
  if (!settings_hash) {
    return settings_hash.error();
  }
  SpectrumResult result = source;
  std::vector<double> raw_power(source.raw_amplitude_linear.size());
  std::ranges::transform(source.raw_amplitude_linear, raw_power.begin(),
                         [](double amplitude) { return amplitude * amplitude; });
  auto smoothed_power = smooth_spectrum(raw_power, settings.smoothing, cancellation);
  if (!smoothed_power) {
    return smoothed_power.error();
  }
  result.magnitude_dbfs = encode_quantity(smoothed_power.value(), SpectrumOutputQuantity::magnitude_dbfs);
  result.raw_values = encode_quantity(source.raw_linear_values, settings.output_quantity);
  result.values = encode_quantity(smoothed_linear.value(), settings.output_quantity);
  result.settings_hash = std::move(settings_hash.value());
  return result;
}

core::Result<SpectrumPsdResult> calculate_spectrum_psd(IFftBackend& backend, const data::SignalSlice& samples,
                                                       double sample_rate_hz, double center_frequency_hz,
                                                       const SpectrumAnalysisSettings& settings,
                                                       std::shared_ptr<const std::atomic_bool> cancellation) {
  auto spectrum = calculate_spectrum(backend, samples, sample_rate_hz, center_frequency_hz, settings, cancellation);
  if (!spectrum) {
    return spectrum.error();
  }
  if (spectrum.value().raw_power_linear.size() != spectrum.value().frequency_hz.size() ||
      spectrum.value().raw_density_linear.size() != spectrum.value().frequency_hz.size()) {
    return error(core::ErrorReason::internal_failure, "共享频谱变换缺少构造 PSD 所需的原始功率域数据");
  }
  auto smoothed_density = smooth_spectrum(spectrum.value().raw_density_linear, settings.smoothing, cancellation);
  if (!smoothed_density) {
    return smoothed_density.error();
  }
  const auto dimensions = resolve_spectrum_dimensions(settings, samples.size());
  if (!dimensions) {
    return dimensions.error();
  }
  const auto offsets = spectrum_offsets(settings, dimensions.value(), samples.size());
  if (!offsets) {
    return offsets.error();
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("共享频谱/PSD 发布前已取消");
  }
  PsdResult psd;
  psd.frequency_hz = spectrum.value().frequency_hz;
  psd.db_per_hz = encode_quantity(smoothed_density.value(), SpectrumOutputQuantity::psd_dbfs_per_hz);
  psd.equivalent_noise_bandwidth_hz = spectrum.value().equivalent_noise_bandwidth_hz;
  psd.provenance = spectrum.value().provenance;
  psd.raw_linear_values = spectrum.value().raw_linear_values;
  psd.raw_density_linear = spectrum.value().raw_density_linear;
  psd.raw_values = spectrum.value().raw_values;
  psd.values = spectrum.value().values;
  psd.raw_db_per_hz = encode_quantity(spectrum.value().raw_density_linear, SpectrumOutputQuantity::psd_dbfs_per_hz);
  psd.output_quantity = settings.output_quantity;
  psd.normalization = settings.normalization;
  psd.settings_hash = spectrum.value().settings_hash;
  psd.frame_length = spectrum.value().frame_length;
  psd.fft_length = spectrum.value().fft_length;
  psd.segment_count = offsets.value().size();
  psd.bin_spacing_hz = spectrum.value().bin_spacing_hz;
  psd.resolution_bandwidth_hz = spectrum.value().resolution_bandwidth_hz;
  return SpectrumPsdResult{std::move(spectrum.value()), std::move(psd)};
}

core::Result<PsdResult> calculate_psd(IFftBackend& backend, const data::SignalSlice& samples, double sample_rate_hz,
                                      double center_frequency_hz, const SpectrumAnalysisSettings& settings,
                                      std::shared_ptr<const std::atomic_bool> cancellation) {
  auto combined =
      calculate_spectrum_psd(backend, samples, sample_rate_hz, center_frequency_hz, settings, std::move(cancellation));
  if (!combined) {
    return combined.error();
  }
  return std::move(combined.value().psd);
}

core::Result<PsdResult> resmooth_psd(const PsdResult& source, const SpectrumAnalysisSettings& settings,
                                     std::shared_ptr<const std::atomic_bool> cancellation) {
  if (source.raw_linear_values.empty() || source.raw_linear_values.size() != source.frequency_hz.size() ||
      source.raw_density_linear.size() != source.raw_linear_values.size() ||
      (settings.frame_length != 0U && source.frame_length != settings.frame_length) ||
      (settings.fft_length != 0U && source.fft_length != settings.fft_length) ||
      source.output_quantity != settings.output_quantity || source.normalization != settings.normalization) {
    return error(core::ErrorReason::invalid_argument, "PSD 平滑复用要求变换尺寸、输出类型和未平滑线性结果保持不变");
  }
  auto smoothed_linear = smooth_spectrum(source.raw_linear_values, settings.smoothing, cancellation);
  if (!smoothed_linear) {
    return smoothed_linear.error();
  }
  auto smoothed_density = smooth_spectrum(source.raw_density_linear, settings.smoothing, cancellation);
  if (!smoothed_density) {
    return smoothed_density.error();
  }
  AnalysisSettingsSnapshot hash_snapshot;
  hash_snapshot.spectrum = settings;
  auto settings_hash = hash_analysis_settings(hash_snapshot);
  if (!settings_hash) {
    return settings_hash.error();
  }
  PsdResult result = source;
  result.db_per_hz = encode_quantity(smoothed_density.value(), SpectrumOutputQuantity::psd_dbfs_per_hz);
  result.raw_values = encode_quantity(source.raw_linear_values, settings.output_quantity);
  result.values = encode_quantity(smoothed_linear.value(), settings.output_quantity);
  result.raw_db_per_hz = encode_quantity(source.raw_density_linear, SpectrumOutputQuantity::psd_dbfs_per_hz);
  result.settings_hash = std::move(settings_hash.value());
  return result;
}

core::Result<StftResult> calculate_stft(IFftBackend& backend, const data::SignalSlice& samples, double sample_rate_hz,
                                        double center_frequency_hz, const SpectrogramAnalysisSettings& settings,
                                        std::shared_ptr<const std::atomic_bool> cancellation,
                                        std::uint64_t source_sample_offset) {
  if (!(sample_rate_hz > 0.0) || !std::isfinite(sample_rate_hz) || !std::isfinite(center_frequency_hz)) {
    return error(core::ErrorReason::invalid_argument, "参数化 STFT 的采样率或中心频率无效");
  }
  if (const auto status = validate_spectrogram_analysis_settings(settings, samples.size(), samples.kind()); !status) {
    return status;
  }
  const auto dimensions = resolve_spectrogram_dimensions(settings, samples.size());
  if (!dimensions) {
    return dimensions.error();
  }
  const auto columns = output_bin_count(dimensions.value().fft_length, settings.sidedness);
  std::uint64_t cell_count{};
  if (!checked_multiply(dimensions.value().rows, columns, cell_count) ||
      cell_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return error(core::ErrorReason::invalid_argument, "STFT 输出矩阵尺寸溢出");
  }
  std::vector<double> raw_linear;
  std::vector<double> raw_density;
  raw_linear.reserve(static_cast<std::size_t>(cell_count));
  raw_density.reserve(static_cast<std::size_t>(cell_count));
  StftResult result;
  result.rows = dimensions.value().rows;
  result.columns = columns;
  result.time_seconds.reserve(static_cast<std::size_t>(dimensions.value().rows));
  auto plan = backend.create_plan({dimensions.value().fft_length, FftDirection::forward});
  if (!plan) {
    return plan.error();
  }
  for (std::uint64_t row = 0; row < dimensions.value().rows; ++row) {
    const auto offset = row * dimensions.value().hop_length;
    auto frame = calculate_frame(*plan.value(), samples, offset, dimensions.value().frame_length,
                                 dimensions.value().fft_length, sample_rate_hz, center_frequency_hz, settings.window,
                                 settings.sidedness, settings.normalization, settings.detrend_policy, cancellation);
    if (!frame) {
      return frame.error();
    }
    if (row == 0U) {
      result.frequency_hz = frame.value().frequency_hz;
      result.provenance = frame.value().provenance;
      result.resolution_bandwidth_hz = frame.value().equivalent_noise_bandwidth_hz;
    } else if (const auto status = validate_provenance_consistency(result.provenance, frame.value().provenance, "STFT");
               !status) {
      return status;
    }
    const auto& selected = select_power_domain(frame.value(), settings.output_quantity);
    raw_linear.insert(raw_linear.end(), selected.begin(), selected.end());
    raw_density.insert(raw_density.end(), frame.value().density.begin(), frame.value().density.end());
    result.time_seconds.push_back((static_cast<double>(source_sample_offset) + static_cast<double>(offset) +
                                   static_cast<double>(dimensions.value().frame_length - 1U) / 2.0) /
                                  sample_rate_hz);
  }
  const auto smooth_matrix = [&](std::span<const double> values) -> core::Result<std::vector<double>> {
    std::vector<double> output(values.begin(), values.end());
    if (settings.smoothing.frequency_mode == SpectrogramFrequencySmoothingKind::gaussian) {
      const SpectrumSmoothingSettings frequency_settings{SpectrumSmoothingKind::gaussian,
                                                         settings.smoothing.frequency_kernel_length,
                                                         settings.smoothing.frequency_sigma, 0U};
      for (std::uint64_t row = 0; row < dimensions.value().rows; ++row) {
        const auto begin = output.begin() + static_cast<std::ptrdiff_t>(row * columns);
        auto row_smoothed = smooth_spectrum(std::span<const double>{begin, static_cast<std::size_t>(columns)},
                                            frequency_settings, cancellation);
        if (!row_smoothed) {
          return row_smoothed.error();
        }
        std::copy(row_smoothed.value().begin(), row_smoothed.value().end(), begin);
      }
    }
    if (settings.smoothing.time_mode == SpectrogramTimeSmoothingKind::exponential) {
      const auto alpha = settings.smoothing.time_exponential_alpha;
      for (std::uint64_t row = 1U; row < dimensions.value().rows; ++row) {
        if (cancellation_requested(cancellation)) {
          return cancellation_status("STFT 时间平滑已取消");
        }
        for (std::uint64_t column = 0; column < columns; ++column) {
          const auto index = static_cast<std::size_t>(row * columns + column);
          const auto previous = static_cast<std::size_t>((row - 1U) * columns + column);
          output[index] = (1.0 - alpha) * output[previous] + alpha * output[index];
        }
      }
    }
    return output;
  };
  auto smoothed = smooth_matrix(raw_linear);
  if (!smoothed) {
    return smoothed.error();
  }
  std::vector<double> smoothed_density = smoothed.value();
  if (!is_density_quantity(settings.output_quantity)) {
    auto smoothed_density_result = smooth_matrix(raw_density);
    if (!smoothed_density_result) {
      return smoothed_density_result.error();
    }
    smoothed_density = std::move(smoothed_density_result.value());
  }
  const auto raw_encoded = encode_quantity(raw_linear, settings.output_quantity);
  const auto smoothed_encoded = encode_quantity(smoothed.value(), settings.output_quantity);
  const auto density_db = encode_quantity(raw_density, SpectrumOutputQuantity::psd_dbfs_per_hz);
  const auto smoothed_density_db = encode_quantity(smoothed_density, SpectrumOutputQuantity::psd_dbfs_per_hz);
  result.raw_linear_values = std::move(raw_linear);
  result.raw_density_linear = std::move(raw_density);
  result.raw_values.reserve(raw_encoded.size());
  result.values.reserve(smoothed_encoded.size());
  result.raw_db_per_hz.reserve(density_db.size());
  result.db_per_hz.reserve(smoothed_density_db.size());
  for (std::size_t index = 0; index < raw_encoded.size(); ++index) {
    result.raw_values.push_back(static_cast<float>(raw_encoded[index]));
    result.values.push_back(static_cast<float>(smoothed_encoded[index]));
    result.raw_db_per_hz.push_back(static_cast<float>(density_db[index]));
    result.db_per_hz.push_back(static_cast<float>(smoothed_density_db[index]));
  }
  AnalysisSettingsSnapshot hash_snapshot;
  hash_snapshot.spectrogram = settings;
  auto settings_hash = hash_analysis_settings(hash_snapshot);
  if (!settings_hash) {
    return settings_hash.error();
  }
  if (cancellation_requested(cancellation)) {
    return cancellation_status("参数化 STFT 发布前已取消");
  }
  result.output_quantity = settings.output_quantity;
  result.normalization = settings.normalization;
  result.settings_hash = std::move(settings_hash.value());
  result.frame_length = dimensions.value().frame_length;
  result.fft_length = dimensions.value().fft_length;
  result.hop_length = dimensions.value().hop_length;
  result.bin_spacing_hz = sample_rate_hz / static_cast<double>(dimensions.value().fft_length);
  return result;
}

core::Result<StftResult> resmooth_stft(const StftResult& source, const SpectrogramAnalysisSettings& settings,
                                       std::shared_ptr<const std::atomic_bool> cancellation) {
  std::uint64_t cell_count{};
  if (!checked_multiply(source.rows, source.columns, cell_count) || cell_count != source.raw_linear_values.size() ||
      source.raw_density_linear.size() != source.raw_linear_values.size() ||
      (settings.frame_length != 0U && source.frame_length != settings.frame_length) ||
      (settings.fft_length != 0U && source.fft_length != settings.fft_length) ||
      (settings.hop_length != 0U && source.hop_length != settings.hop_length) ||
      source.output_quantity != settings.output_quantity || source.normalization != settings.normalization) {
    return error(core::ErrorReason::invalid_argument, "STFT 平滑复用要求变换尺寸、输出类型和未平滑线性矩阵保持不变");
  }
  if (const auto status = validate_spectrogram_smoothing(settings.smoothing); !status) {
    return status;
  }
  const auto smooth_matrix = [&](std::span<const double> values) -> core::Result<std::vector<double>> {
    std::vector<double> output(values.begin(), values.end());
    if (settings.smoothing.frequency_mode == SpectrogramFrequencySmoothingKind::gaussian) {
      const SpectrumSmoothingSettings frequency_settings{SpectrumSmoothingKind::gaussian,
                                                         settings.smoothing.frequency_kernel_length,
                                                         settings.smoothing.frequency_sigma, 0U};
      for (std::uint64_t row = 0; row < source.rows; ++row) {
        const auto begin = output.begin() + static_cast<std::ptrdiff_t>(row * source.columns);
        auto row_smoothed = smooth_spectrum(std::span<const double>{begin, static_cast<std::size_t>(source.columns)},
                                            frequency_settings, cancellation);
        if (!row_smoothed) {
          return row_smoothed.error();
        }
        std::copy(row_smoothed.value().begin(), row_smoothed.value().end(), begin);
      }
    }
    if (settings.smoothing.time_mode == SpectrogramTimeSmoothingKind::exponential) {
      const auto alpha = settings.smoothing.time_exponential_alpha;
      for (std::uint64_t row = 1U; row < source.rows; ++row) {
        if (cancellation_requested(cancellation)) {
          return cancellation_status("STFT 时间平滑复用已取消");
        }
        for (std::uint64_t column = 0; column < source.columns; ++column) {
          const auto index = static_cast<std::size_t>(row * source.columns + column);
          const auto previous = static_cast<std::size_t>((row - 1U) * source.columns + column);
          output[index] = (1.0 - alpha) * output[previous] + alpha * output[index];
        }
      }
    }
    return output;
  };
  auto smoothed = smooth_matrix(source.raw_linear_values);
  if (!smoothed) {
    return smoothed.error();
  }
  auto smoothed_density = smooth_matrix(source.raw_density_linear);
  if (!smoothed_density) {
    return smoothed_density.error();
  }
  const auto raw_encoded = encode_quantity(source.raw_linear_values, settings.output_quantity);
  const auto smoothed_encoded = encode_quantity(smoothed.value(), settings.output_quantity);
  const auto density_db = encode_quantity(source.raw_density_linear, SpectrumOutputQuantity::psd_dbfs_per_hz);
  const auto smoothed_density_db = encode_quantity(smoothed_density.value(), SpectrumOutputQuantity::psd_dbfs_per_hz);
  StftResult result = source;
  result.raw_values.clear();
  result.values.clear();
  result.raw_db_per_hz.clear();
  result.db_per_hz.clear();
  result.raw_values.reserve(raw_encoded.size());
  result.values.reserve(smoothed_encoded.size());
  result.raw_db_per_hz.reserve(density_db.size());
  result.db_per_hz.reserve(smoothed_density_db.size());
  for (std::size_t index = 0; index < raw_encoded.size(); ++index) {
    result.raw_values.push_back(static_cast<float>(raw_encoded[index]));
    result.values.push_back(static_cast<float>(smoothed_encoded[index]));
    result.raw_db_per_hz.push_back(static_cast<float>(density_db[index]));
    result.db_per_hz.push_back(static_cast<float>(smoothed_density_db[index]));
  }
  AnalysisSettingsSnapshot hash_snapshot;
  hash_snapshot.spectrogram = settings;
  auto settings_hash = hash_analysis_settings(hash_snapshot);
  if (!settings_hash) {
    return settings_hash.error();
  }
  result.settings_hash = std::move(settings_hash.value());
  return result;
}

core::Result<SpectrumResult> calculate_spectrum(IFftBackend& fft_backend, ISignalKernelBackend& kernel_backend,
                                                const data::SignalSlice& samples,
                                                const data::SignalDescriptor& descriptor,
                                                const AnalysisSettingsSnapshot& settings,
                                                std::shared_ptr<const std::atomic_bool> cancellation) {
  if (const auto status = validate_analysis_settings(settings, samples.size(), descriptor); !status) {
    return status;
  }
  auto filtered = apply_analysis_prefilter(kernel_backend, samples, descriptor, settings.prefilter, cancellation);
  if (!filtered) {
    return filtered.error();
  }
  auto result = calculate_spectrum(fft_backend, filtered.value().view(), descriptor.sample_rate_hz,
                                   descriptor.center_frequency_hz.value_or(0.0), settings.spectrum, cancellation);
  if (!result) {
    return result.error();
  }
  auto hash = hash_analysis_settings(settings);
  if (!hash) {
    return hash.error();
  }
  result.value().settings_hash = std::move(hash.value());
  result.value().prefilter_applied = settings.prefilter.enabled;
  return result;
}

core::Result<PsdResult> calculate_psd(IFftBackend& fft_backend, ISignalKernelBackend& kernel_backend,
                                      const data::SignalSlice& samples, const data::SignalDescriptor& descriptor,
                                      const AnalysisSettingsSnapshot& settings,
                                      std::shared_ptr<const std::atomic_bool> cancellation) {
  if (const auto status = validate_analysis_settings(settings, samples.size(), descriptor); !status) {
    return status;
  }
  auto filtered = apply_analysis_prefilter(kernel_backend, samples, descriptor, settings.prefilter, cancellation);
  if (!filtered) {
    return filtered.error();
  }
  auto result = calculate_psd(fft_backend, filtered.value().view(), descriptor.sample_rate_hz,
                              descriptor.center_frequency_hz.value_or(0.0), settings.spectrum, cancellation);
  if (!result) {
    return result.error();
  }
  auto hash = hash_analysis_settings(settings);
  if (!hash) {
    return hash.error();
  }
  result.value().settings_hash = std::move(hash.value());
  result.value().prefilter_applied = settings.prefilter.enabled;
  return result;
}

core::Result<StftResult> calculate_stft(IFftBackend& fft_backend, ISignalKernelBackend& kernel_backend,
                                        const data::SignalSlice& samples, const data::SignalDescriptor& descriptor,
                                        const AnalysisSettingsSnapshot& settings,
                                        std::shared_ptr<const std::atomic_bool> cancellation) {
  if (const auto status = validate_analysis_settings(settings, samples.size(), descriptor); !status) {
    return status;
  }
  auto filtered = apply_analysis_prefilter(kernel_backend, samples, descriptor, settings.prefilter, cancellation);
  if (!filtered) {
    return filtered.error();
  }
  auto result = calculate_stft(fft_backend, filtered.value().view(), descriptor.sample_rate_hz,
                               descriptor.center_frequency_hz.value_or(0.0), settings.spectrogram, cancellation,
                               descriptor.requested_sample_range.begin());
  if (!result) {
    return result.error();
  }
  auto hash = hash_analysis_settings(settings);
  if (!hash) {
    return hash.error();
  }
  result.value().settings_hash = std::move(hash.value());
  result.value().prefilter_applied = settings.prefilter.enabled;
  return result;
}

double spectrogram_overlap_ratio(const SpectrogramAnalysisSettings& settings) noexcept {
  if (settings.frame_length == 0U) {
    return 0.0;
  }
  const auto effective_hop =
      settings.hop_length == 0U ? std::max<std::uint64_t>(1U, settings.frame_length / 4U) : settings.hop_length;
  if (effective_hop > settings.frame_length) {
    return 0.0;
  }
  return 1.0 - static_cast<double>(effective_hop) / static_cast<double>(settings.frame_length);
}

core::Result<std::uint64_t> time_to_sample(double seconds, double sample_rate_hz, std::uint64_t loaded_sample_count) {
  if (!std::isfinite(seconds) || seconds < 0.0 || !std::isfinite(sample_rate_hz) || !(sample_rate_hz > 0.0)) {
    return error(core::ErrorReason::invalid_argument, "时间或采样率无效");
  }
  const auto sample = std::round(seconds * sample_rate_hz);
  if (sample < 0.0 || sample > static_cast<double>(loaded_sample_count)) {
    return error(core::ErrorReason::invalid_argument, "时间超出已加载样本范围");
  }
  return static_cast<std::uint64_t>(sample);
}

core::Result<double> sample_to_time(std::uint64_t sample, double sample_rate_hz) {
  if (!(sample_rate_hz > 0.0) || !std::isfinite(sample_rate_hz)) {
    return error(core::ErrorReason::invalid_argument, "采样率无效");
  }
  return static_cast<double>(sample) / sample_rate_hz;
}

core::Result<std::uint64_t> frequency_to_bin(double frequency_hz, double sample_rate_hz, std::uint64_t fft_length,
                                             SpectrumSidedness sidedness) {
  if (!std::isfinite(frequency_hz) || !std::isfinite(sample_rate_hz) || !(sample_rate_hz > 0.0) || fft_length < 2U ||
      fft_length > maximum_exact_double_integer) {
    return error(core::ErrorReason::invalid_argument, "频率映射参数无效");
  }
  const auto minimum = sidedness == SpectrumSidedness::one_sided ? 0.0 : -sample_rate_hz / 2.0;
  const auto maximum = sample_rate_hz / 2.0;
  if (frequency_hz < minimum || frequency_hz > maximum) {
    return error(core::ErrorReason::invalid_argument, "频率超出 FFT 可表示范围");
  }
  auto raw = std::llround((frequency_hz / sample_rate_hz) * static_cast<double>(fft_length));
  if (sidedness == SpectrumSidedness::two_sided_shifted) {
    raw += static_cast<std::int64_t>(fft_length / 2U);
    if (fft_length % 2U == 0U && raw == static_cast<std::int64_t>(fft_length)) {
      raw = 0;
    }
  }
  return static_cast<std::uint64_t>(std::clamp<std::int64_t>(raw, 0, static_cast<std::int64_t>(fft_length - 1U)));
}

core::Result<double> bin_to_frequency(std::uint64_t bin, double sample_rate_hz, std::uint64_t fft_length,
                                      SpectrumSidedness sidedness) {
  if (fft_length < 2U || fft_length > maximum_exact_double_integer || !std::isfinite(sample_rate_hz) ||
      !(sample_rate_hz > 0.0)) {
    return error(core::ErrorReason::invalid_argument, "FFT bin 映射参数无效");
  }
  const auto maximum_bin = sidedness == SpectrumSidedness::one_sided ? fft_length / 2U : fft_length - 1U;
  if (bin > maximum_bin) {
    return error(core::ErrorReason::invalid_argument, "FFT bin 映射参数无效");
  }
  const auto signed_bin = sidedness == SpectrumSidedness::one_sided
                              ? static_cast<std::int64_t>(bin)
                              : static_cast<std::int64_t>(bin) - static_cast<std::int64_t>(fft_length / 2U);
  return static_cast<double>(signed_bin) * sample_rate_hz / static_cast<double>(fft_length);
}

} // namespace signal::dsp
