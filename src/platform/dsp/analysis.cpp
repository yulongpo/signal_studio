#include "signal_studio/dsp/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>

namespace signal::dsp {
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

} // namespace

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

core::Result<Window> make_window(WindowKind kind, std::uint64_t length) {
  if (length < 2U || length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return error(core::ErrorReason::invalid_argument, "窗长度必须至少为 2 且可由当前进程表示");
  }
  Window result;
  result.coefficients.resize(static_cast<std::size_t>(length));
  const auto denominator = static_cast<double>(length - 1U);
  for (std::uint64_t index = 0; index < length; ++index) {
    const auto phase = 2.0 * std::numbers::pi * static_cast<double>(index) / denominator;
    double coefficient{};
    switch (kind) {
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
