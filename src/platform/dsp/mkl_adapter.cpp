#include "mkl_benchmark_probe.hpp"
#include "signal_studio/dsp/analysis.hpp"
#include "signal_studio/dsp/pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#if defined(SIGNAL_STUDIO_HAVE_ONEMKL)
#include <mkl_dfti.h>
#include <mkl_lapacke.h>
#include <mkl_service.h>
#include <mkl_vsl.h>
#endif
#include <samplerate.h>

namespace signal::dsp {
namespace {

[[nodiscard]] core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::dsp, reason}, std::move(message), std::move(diagnostic));
}

#if defined(SIGNAL_STUDIO_HAVE_ONEMKL)

[[nodiscard]] MKL_Complex16 to_mkl(const data::ComplexSample& value) noexcept {
  return {value.real, value.imag};
}

[[nodiscard]] data::ComplexSample from_mkl(const MKL_Complex16& value) noexcept {
  return {value.real, value.imag};
}

[[nodiscard]] bool stable_denominator(std::span<const double> denominator) {
  if (denominator.empty() || denominator.front() == 0.0 ||
      std::ranges::any_of(denominator, [](double value) { return !std::isfinite(value); })) {
    return false;
  }
  std::vector<long double> polynomial;
  polynomial.reserve(denominator.size());
  const auto leading = static_cast<long double>(denominator.front());
  for (const auto coefficient : denominator) {
    polynomial.push_back(static_cast<long double>(coefficient) / leading);
  }
  constexpr long double margin = 64.0L * std::numeric_limits<long double>::epsilon();
  while (polynomial.size() > 1U) {
    const auto degree = polynomial.size() - 1U;
    const auto first = polynomial.front();
    const auto last = polynomial.back();
    if (!(std::abs(last) + margin < std::abs(first))) {
      return false;
    }
    std::vector<long double> reduced(degree);
    for (std::size_t index = 0; index < degree; ++index) {
      reduced[index] = first * polynomial[index] - last * polynomial[degree - index];
    }
    const auto scale = std::abs(reduced.front());
    if (!(scale > margin)) {
      return false;
    }
    for (auto& value : reduced) {
      value /= scale;
    }
    polynomial = std::move(reduced);
  }
  return true;
}

class OneMklFftBackend final : public IFftBackend {
public:
  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return "oneMKL-DFTI";
  }

  [[nodiscard]] core::Status validate(const FftSpec& spec) const override {
    if (spec.length < 2U || spec.length > static_cast<std::uint64_t>(std::numeric_limits<MKL_LONG>::max())) {
      return error(core::ErrorReason::invalid_argument, "oneMKL FFT 长度无效");
    }
    return core::Status::success();
  }

  [[nodiscard]] core::Result<std::shared_ptr<IFftPlan>> create_plan(const FftSpec& spec) override {
    if (const auto status = validate(spec); !status) {
      return status;
    }
    auto selected = plan(spec);
    if (!selected) {
      return selected.error();
    }
    return std::static_pointer_cast<IFftPlan>(selected.value());
  }

  [[nodiscard]] benchmark_internal::PlanCacheCounters benchmark_cache_counters() {
    std::scoped_lock cache_lock(cache_mutex_);
    return {cache_hits_, cache_misses_, plans_.size()};
  }

private:
  struct Plan final : IFftPlan {
    ~Plan() {
      if (descriptor != nullptr) {
        static_cast<void>(DftiFreeDescriptor(&descriptor));
      }
    }
    [[nodiscard]] FftSpec spec() const noexcept override {
      return specification;
    }
    [[nodiscard]] core::Result<FftResult> process(std::span<const data::ComplexSample> input) override {
      if (input.size() != specification.length) {
        return error(core::ErrorReason::invalid_argument, "oneMKL FFT 输入长度与计划不一致");
      }
      std::vector<MKL_Complex16> values;
      values.reserve(input.size());
      for (const auto& sample : input) {
        if (!std::isfinite(sample.real) || !std::isfinite(sample.imag)) {
          return error(core::ErrorReason::invalid_argument, "oneMKL FFT 输入包含 NaN 或 Inf");
        }
        values.push_back(to_mkl(sample));
      }
      std::scoped_lock plan_lock(mutex);
      const auto status = specification.direction == FftDirection::forward
                              ? DftiComputeForward(descriptor, values.data())
                              : DftiComputeBackward(descriptor, values.data());
      if (status != DFTI_NO_ERROR) {
        return error(core::ErrorReason::internal_failure, "oneMKL DFTI 执行失败",
                     DftiErrorMessage(status) == nullptr ? std::to_string(status) : DftiErrorMessage(status));
      }
      FftResult result;
      result.bins.reserve(values.size());
      for (const auto& value : values) {
        result.bins.push_back(from_mkl(value));
      }
      char version[256]{};
      mkl_get_version_string(version, static_cast<int>(sizeof(version)));
      result.provenance = {compute::BackendKind::cpu_scalar,
                           compute::BackendKind::cpu_scalar,
                           "oneMKL-DFTI",
                           version,
                           "host-cpu",
                           "批准的 sequential/lp64 CPU FFT 适配器与复用计划缓存",
                           false,
                           false,
                           "complex-float64"};
      return result;
    }
    FftSpec specification;
    DFTI_DESCRIPTOR_HANDLE descriptor{};
    std::mutex mutex;
  };

  [[nodiscard]] core::Result<std::shared_ptr<Plan>> plan(const FftSpec& spec) {
    const auto key = std::pair{spec.length, spec.direction};
    std::scoped_lock cache_lock(cache_mutex_);
    if (const auto found = plans_.find(key); found != plans_.end()) {
      ++cache_hits_;
      return found->second;
    }
    ++cache_misses_;
    auto created = std::make_shared<Plan>();
    created->specification = spec;
    auto status =
        DftiCreateDescriptor(&created->descriptor, DFTI_DOUBLE, DFTI_COMPLEX, 1, static_cast<MKL_LONG>(spec.length));
    if (status == DFTI_NO_ERROR && spec.direction == FftDirection::inverse) {
      status = DftiSetValue(created->descriptor, DFTI_BACKWARD_SCALE, 1.0 / static_cast<double>(spec.length));
    }
    if (status == DFTI_NO_ERROR) {
      status = DftiCommitDescriptor(created->descriptor);
    }
    if (status != DFTI_NO_ERROR) {
      return error(core::ErrorReason::internal_failure, "oneMKL DFTI 计划创建失败",
                   DftiErrorMessage(status) == nullptr ? std::to_string(status) : DftiErrorMessage(status));
    }
    plans_.emplace(key, created);
    return created;
  }

  std::mutex cache_mutex_;
  std::map<std::pair<std::uint64_t, FftDirection>, std::shared_ptr<Plan>> plans_;
  std::uint64_t cache_hits_{};
  std::uint64_t cache_misses_{};
};

[[nodiscard]] core::Result<std::vector<data::ComplexSample>> vsl_convolution(std::span<const data::ComplexSample> x,
                                                                             std::span<const double> coefficients) {
  if (x.empty() || coefficients.empty() || x.size() > static_cast<std::size_t>(std::numeric_limits<MKL_INT>::max()) ||
      coefficients.size() > static_cast<std::size_t>(std::numeric_limits<MKL_INT>::max())) {
    return error(core::ErrorReason::invalid_argument, "oneMKL VSL 卷积输入无效");
  }
  const auto output_count = x.size() + coefficients.size() - 1U;
  if (output_count > static_cast<std::size_t>(std::numeric_limits<MKL_INT>::max())) {
    return error(core::ErrorReason::invalid_argument, "oneMKL VSL 卷积输出过大");
  }
  std::vector<MKL_Complex16> input;
  std::vector<MKL_Complex16> kernel;
  std::vector<MKL_Complex16> output(output_count);
  input.reserve(x.size());
  kernel.reserve(coefficients.size());
  for (const auto& sample : x) {
    input.push_back(to_mkl(sample));
  }
  for (const auto coefficient : coefficients) {
    kernel.push_back({coefficient, 0.0});
  }
  VSLConvTaskPtr task{};
  auto status = vslzConvNewTask1D(&task, VSL_CONV_MODE_AUTO, static_cast<MKL_INT>(input.size()),
                                  static_cast<MKL_INT>(kernel.size()), static_cast<MKL_INT>(output.size()));
  if (status == VSL_STATUS_OK) {
    status = vslzConvExec1D(task, input.data(), 1, kernel.data(), 1, output.data(), 1);
  }
  if (task != nullptr) {
    const auto delete_status = vslConvDeleteTask(&task);
    if (status == VSL_STATUS_OK) {
      status = delete_status;
    }
  }
  if (status != VSL_STATUS_OK) {
    return error(core::ErrorReason::internal_failure, "oneMKL VSL 卷积执行失败", std::to_string(status));
  }
  std::vector<data::ComplexSample> converted;
  converted.reserve(output.size());
  for (const auto& value : output) {
    converted.push_back(from_mkl(value));
  }
  return converted;
}

class OneMklSignalKernelBackend final : public ISignalKernelBackend {
public:
  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return "oneMKL-VSL-LAPACKE+libsamplerate-0.2.2";
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>> analytic_signal(std::span<const double> input) override {
    if (input.size() < 2U) {
      return error(core::ErrorReason::invalid_argument, "解析信号至少需要两个样本");
    }
    OneMklFftBackend fft;
    std::vector<data::ComplexSample> values;
    values.reserve(input.size());
    for (const auto value : input) {
      values.push_back({value, 0.0});
    }
    auto spectrum = fft.execute({input.size(), FftDirection::forward}, values);
    if (!spectrum) {
      return spectrum.error();
    }
    const auto nyquist = input.size() / 2U;
    for (std::size_t index = 1; index < spectrum.value().bins.size(); ++index) {
      const bool positive = index < (input.size() + 1U) / 2U;
      const bool even_nyquist = input.size() % 2U == 0U && index == nyquist;
      if (positive) {
        spectrum.value().bins[index].real *= 2.0;
        spectrum.value().bins[index].imag *= 2.0;
      } else if (!even_nyquist) {
        spectrum.value().bins[index] = {};
      }
    }
    auto analytic = fft.execute({input.size(), FftDirection::inverse}, spectrum.value().bins);
    if (!analytic) {
      return analytic.error();
    }
    return std::move(analytic.value().bins);
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>> convolve(std::span<const data::ComplexSample> input,
                                                                        std::span<const double> coefficients,
                                                                        FilterState& state,
                                                                        BoundaryPolicy boundary) override {
    if (input.empty() || coefficients.empty()) {
      return error(core::ErrorReason::invalid_argument, "FIR 输入和系数不得为空");
    }
    const auto history_count = coefficients.size() - 1U;
    std::vector<data::ComplexSample> extended;
    if (boundary == BoundaryPolicy::preserve_state) {
      if (state.input_history.size() > history_count) {
        return error(core::ErrorReason::invalid_argument, "FIR 跨块状态长度不兼容");
      }
      extended.insert(extended.end(), history_count - state.input_history.size(), {});
      extended.insert(extended.end(), state.input_history.begin(), state.input_history.end());
    } else {
      extended.insert(extended.end(), history_count, {});
    }
    extended.insert(extended.end(), input.begin(), input.end());
    auto full = vsl_convolution(extended, coefficients);
    if (!full) {
      return full.error();
    }
    std::vector<data::ComplexSample> output(full.value().begin() + static_cast<std::ptrdiff_t>(history_count),
                                            full.value().begin() +
                                                static_cast<std::ptrdiff_t>(history_count + input.size()));
    state.input_history.assign(extended.end() - static_cast<std::ptrdiff_t>(history_count), extended.end());
    state.processed_samples += input.size();
    return output;
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>>
  solve_iir(std::span<const data::ComplexSample> input, std::span<const double> numerator,
            std::span<const double> denominator, FilterState& state, BoundaryPolicy boundary) override {
    if (input.empty() || numerator.empty() || denominator.empty() || denominator.front() == 0.0 ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<MKL_INT>::max()) ||
        denominator.size() > static_cast<std::size_t>(std::numeric_limits<MKL_INT>::max()) ||
        std::ranges::any_of(numerator, [](double value) { return !std::isfinite(value); }) ||
        std::ranges::any_of(denominator, [](double value) { return !std::isfinite(value); })) {
      return error(core::ErrorReason::invalid_argument, "IIR 输入或系数无效");
    }
    if (!stable_denominator(denominator)) {
      return error(core::ErrorReason::invalid_argument, "IIR 分母的根不全部位于单位圆内");
    }
    const auto count = input.size();
    const auto input_history_count = numerator.size() - 1U;
    const auto output_history_count = denominator.size() - 1U;
    if (state.input_history.size() > input_history_count || state.output_history.size() > output_history_count) {
      return error(core::ErrorReason::invalid_argument, "IIR 跨块状态长度不兼容");
    }
    std::vector<data::ComplexSample> input_history(input_history_count - state.input_history.size());
    std::vector<data::ComplexSample> output_history(output_history_count - state.output_history.size());
    if (boundary == BoundaryPolicy::preserve_state) {
      input_history.insert(input_history.end(), state.input_history.begin(), state.input_history.end());
      output_history.insert(output_history.end(), state.output_history.begin(), state.output_history.end());
    } else {
      input_history.resize(input_history_count);
      output_history.resize(output_history_count);
    }
    const auto band_width = denominator.size() - 1U;
    if (count > std::numeric_limits<std::size_t>::max() / (band_width + 1U)) {
      return error(core::ErrorReason::invalid_argument, "IIR 带状系统尺寸溢出");
    }
    std::vector<MKL_Complex16> matrix(count * (band_width + 1U));
    std::vector<MKL_Complex16> rhs(count);
    for (std::size_t row = 0; row < count; ++row) {
      for (std::size_t coefficient = 0; coefficient < denominator.size() && coefficient <= row; ++coefficient) {
        const auto column = row - coefficient;
        matrix[coefficient + column * (band_width + 1U)] = {denominator[coefficient], 0.0};
      }
      data::ComplexSample value{};
      for (std::size_t coefficient = 0; coefficient < numerator.size(); ++coefficient) {
        const auto index = static_cast<std::ptrdiff_t>(row) - static_cast<std::ptrdiff_t>(coefficient);
        const auto& sample = index >= 0 ? input[static_cast<std::size_t>(index)]
                                        : input_history[input_history.size() - static_cast<std::size_t>(-index)];
        value.real += numerator[coefficient] * sample.real;
        value.imag += numerator[coefficient] * sample.imag;
      }
      for (std::size_t coefficient = std::max<std::size_t>(1U, row + 1U); coefficient < denominator.size();
           ++coefficient) {
        const auto history_index = output_history.size() - (coefficient - row);
        value.real -= denominator[coefficient] * output_history[history_index].real;
        value.imag -= denominator[coefficient] * output_history[history_index].imag;
      }
      rhs[row] = to_mkl(value);
    }
    const auto status = LAPACKE_ztbtrs(LAPACK_COL_MAJOR, 'L', 'N', 'N', static_cast<MKL_INT>(count),
                                       static_cast<MKL_INT>(band_width), 1, matrix.data(),
                                       static_cast<MKL_INT>(band_width + 1U), rhs.data(), static_cast<MKL_INT>(count));
    if (status != 0) {
      return error(core::ErrorReason::internal_failure, "oneMKL LAPACKE 带状三角 IIR 系统求解失败",
                   std::to_string(status));
    }
    std::vector<data::ComplexSample> output;
    output.reserve(count);
    for (const auto& value : rhs) {
      output.push_back(from_mkl(value));
    }
    std::vector<data::ComplexSample> combined_input(input_history);
    combined_input.insert(combined_input.end(), input.begin(), input.end());
    state.input_history.assign(combined_input.end() - static_cast<std::ptrdiff_t>(input_history_count),
                               combined_input.end());
    std::vector<data::ComplexSample> combined_output(output_history);
    combined_output.insert(combined_output.end(), output.begin(), output.end());
    state.output_history.assign(combined_output.end() - static_cast<std::ptrdiff_t>(output_history_count),
                                combined_output.end());
    state.processed_samples += input.size();
    return output;
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>>
  resample(std::span<const data::ComplexSample> input, std::uint32_t numerator, std::uint32_t denominator,
           std::span<const double> anti_alias_coefficients, FilterState& state, bool end_of_input) override {
    if (input.empty() || numerator == 0U || denominator == 0U || anti_alias_coefficients.empty() ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<long>::max()) ||
        std::ranges::any_of(anti_alias_coefficients, [](double value) { return !std::isfinite(value); })) {
      return error(core::ErrorReason::invalid_argument, "重采样输入、比例或抗混叠系数无效");
    }
    if (std::ranges::any_of(
            input, [](const auto& sample) { return !std::isfinite(sample.real) || !std::isfinite(sample.imag); })) {
      return error(core::ErrorReason::invalid_argument, "重采样输入包含 NaN 或 Inf");
    }

    struct SampleRateState final {
      SampleRateState(std::uint32_t interpolation, std::uint32_t decimation, int& status)
          : numerator(interpolation), denominator(decimation), handle(src_new(SRC_SINC_BEST_QUALITY, 2, &status)) {}
      ~SampleRateState() {
        if (handle != nullptr) {
          handle = src_delete(handle);
        }
      }
      std::uint32_t numerator{};
      std::uint32_t denominator{};
      SRC_STATE* handle{};
    };

    std::shared_ptr<SampleRateState> sample_rate_state;
    if (state.backend_state) {
      sample_rate_state = std::shared_ptr<SampleRateState>(state.backend_state,
                                                           static_cast<SampleRateState*>(state.backend_state.get()));
      if (sample_rate_state->numerator != numerator || sample_rate_state->denominator != denominator) {
        return error(core::ErrorReason::invalid_argument, "流式重采样期间不得改变采样率比例");
      }
    } else {
      int creation_status{};
      sample_rate_state = std::make_shared<SampleRateState>(numerator, denominator, creation_status);
      if (creation_status != 0 || sample_rate_state->handle == nullptr) {
        return error(core::ErrorReason::unavailable, "libsamplerate 状态创建失败", src_strerror(creation_status));
      }
      state.backend_state = sample_rate_state;
    }

    const auto ratio = static_cast<double>(numerator) / static_cast<double>(denominator);
    const auto capacity_estimate =
        std::ceil((static_cast<double>(input.size()) + 4096.0) * std::max(1.0, ratio)) + 4096.0;
    if (!(capacity_estimate > 0.0) || capacity_estimate > static_cast<double>(std::numeric_limits<long>::max())) {
      return error(core::ErrorReason::invalid_argument, "libsamplerate 输出容量超出可表示范围");
    }
    std::vector<float> interleaved_input(input.size() * 2U);
    for (std::size_t index = 0; index < input.size(); ++index) {
      interleaved_input[index * 2U] = static_cast<float>(input[index].real);
      interleaved_input[index * 2U + 1U] = static_cast<float>(input[index].imag);
    }
    const auto output_capacity = static_cast<std::size_t>(capacity_estimate);
    std::vector<float> interleaved_output(output_capacity * 2U);
    SRC_DATA request{};
    request.data_in = interleaved_input.data();
    request.data_out = interleaved_output.data();
    request.input_frames = static_cast<long>(input.size());
    request.output_frames = static_cast<long>(output_capacity);
    request.end_of_input = end_of_input ? 1 : 0;
    request.src_ratio = ratio;
    const auto status = src_process(sample_rate_state->handle, &request);
    if (status != 0 || request.input_frames_used != request.input_frames) {
      return error(core::ErrorReason::internal_failure, "libsamplerate 流式重采样失败",
                   status != 0 ? src_strerror(status) : "输出容量不足，输入未完全消费");
    }
    std::vector<data::ComplexSample> output(static_cast<std::size_t>(request.output_frames_gen));
    for (std::size_t index = 0; index < output.size(); ++index) {
      output[index] = {static_cast<double>(interleaved_output[index * 2U]),
                       static_cast<double>(interleaved_output[index * 2U + 1U])};
    }
    state.processed_samples += input.size();
    state.resample_phase =
        (state.processed_samples * static_cast<std::uint64_t>(numerator)) % static_cast<std::uint64_t>(denominator);
    if (end_of_input) {
      state.backend_state.reset();
    }
    return output;
  }
};

#endif

} // namespace

namespace analysis_internal {

core::Result<std::vector<double>> convolve_centered(std::span<const double> input,
                                                    std::span<const double> coefficients) {
#if defined(SIGNAL_STUDIO_HAVE_ONEMKL)
  if (input.empty() || coefficients.empty() || coefficients.size() % 2U == 0U ||
      std::ranges::any_of(input, [](double value) { return !std::isfinite(value); }) ||
      std::ranges::any_of(coefficients, [](double value) { return !std::isfinite(value); })) {
    return error(core::ErrorReason::invalid_argument, "中心卷积要求有限输入和奇数长度核");
  }
  const auto half = coefficients.size() / 2U;
  std::vector<data::ComplexSample> extended;
  extended.reserve(input.size() + 2U * half);
  extended.insert(extended.end(), half, data::ComplexSample{input.front(), 0.0});
  for (const auto value : input) {
    extended.push_back({value, 0.0});
  }
  extended.insert(extended.end(), half, data::ComplexSample{input.back(), 0.0});
  auto convolution = vsl_convolution(extended, coefficients);
  if (!convolution) {
    return convolution.error();
  }
  const auto offset = coefficients.size() - 1U;
  if (convolution.value().size() < offset + input.size()) {
    return error(core::ErrorReason::internal_failure, "oneMKL 中心卷积输出长度无效");
  }
  std::vector<double> output;
  output.reserve(input.size());
  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto& value = convolution.value()[offset + index];
    if (!std::isfinite(value.real) || !std::isfinite(value.imag) || std::abs(value.imag) > 1e-12) {
      return error(core::ErrorReason::internal_failure, "oneMKL 中心卷积返回非有限或非实数结果");
    }
    output.push_back(value.real);
  }
  return output;
#else
  static_cast<void>(input);
  static_cast<void>(coefficients);
  return error(core::ErrorReason::unavailable, "频谱平滑要求已批准的 oneMKL VSL 卷积适配器");
#endif
}

core::Result<std::vector<double>> savitzky_golay_kernel(std::uint32_t window_length, std::uint32_t polynomial_order) {
#if defined(SIGNAL_STUDIO_HAVE_ONEMKL)
  if (window_length < 3U || window_length % 2U == 0U || polynomial_order >= window_length || polynomial_order > 12U) {
    return error(core::ErrorReason::invalid_argument, "Savitzky-Golay 窗长或阶数无效");
  }
  const auto terms = static_cast<MKL_INT>(polynomial_order + 1U);
  std::vector<double> normal_matrix(static_cast<std::size_t>(terms) * static_cast<std::size_t>(terms));
  const auto half = static_cast<std::int64_t>(window_length / 2U);
  for (MKL_INT row = 0; row < terms; ++row) {
    for (MKL_INT column = 0; column < terms; ++column) {
      double sum{};
      for (std::int64_t offset = -half; offset <= half; ++offset) {
        sum += std::pow(static_cast<double>(offset), static_cast<int>(row + column));
      }
      normal_matrix[static_cast<std::size_t>(row) * static_cast<std::size_t>(terms) +
                    static_cast<std::size_t>(column)] = sum;
    }
  }
  std::vector<double> evaluation(static_cast<std::size_t>(terms));
  evaluation.front() = 1.0;
  const auto status = LAPACKE_dposv(LAPACK_ROW_MAJOR, 'U', terms, 1, normal_matrix.data(), terms, evaluation.data(), 1);
  if (status != 0) {
    return error(core::ErrorReason::internal_failure, "oneMKL LAPACKE 无法求解 Savitzky-Golay 最小二乘核",
                 std::to_string(status));
  }
  std::vector<double> coefficients(window_length);
  double coefficient_sum{};
  for (std::int64_t offset = -half; offset <= half; ++offset) {
    double power{1.0};
    double coefficient{};
    for (MKL_INT term = 0; term < terms; ++term) {
      coefficient += power * evaluation[static_cast<std::size_t>(term)];
      power *= static_cast<double>(offset);
    }
    coefficients[static_cast<std::size_t>(offset + half)] = coefficient;
    coefficient_sum += coefficient;
  }
  if (!std::isfinite(coefficient_sum) || std::abs(coefficient_sum) < 1e-15) {
    return error(core::ErrorReason::internal_failure, "Savitzky-Golay 核归一化失败");
  }
  for (auto& coefficient : coefficients) {
    coefficient /= coefficient_sum;
  }
  return coefficients;
#else
  static_cast<void>(window_length);
  static_cast<void>(polynomial_order);
  return error(core::ErrorReason::unavailable, "Savitzky-Golay 要求已批准的 oneMKL LAPACKE 适配器");
#endif
}

} // namespace analysis_internal

core::Result<std::shared_ptr<IFftBackend>> make_cpu_fft_backend() {
#if defined(SIGNAL_STUDIO_HAVE_ONEMKL)
  return std::shared_ptr<IFftBackend>(std::make_shared<OneMklFftBackend>());
#else
  return error(core::ErrorReason::unavailable, "oneMKL CPU FFT 适配器未构建",
               "请运行 scripts/install-ms02-dependencies.ps1 并重新配置");
#endif
}

bool cpu_fft_available() noexcept {
#if defined(SIGNAL_STUDIO_HAVE_ONEMKL)
  return true;
#else
  return false;
#endif
}

core::Result<benchmark_internal::PlanCacheCounters> benchmark_internal::inspect_mkl_plan_cache(IFftBackend& backend) {
#if defined(SIGNAL_STUDIO_HAVE_ONEMKL)
  auto* mkl = dynamic_cast<OneMklFftBackend*>(&backend);
  if (mkl == nullptr) {
    return error(core::ErrorReason::invalid_argument, "缓存探针仅支持直接 oneMKL FFT 后端");
  }
  return mkl->benchmark_cache_counters();
#else
  static_cast<void>(backend);
  return error(core::ErrorReason::unavailable, "oneMKL 缓存探针不可用");
#endif
}

core::Result<std::shared_ptr<ISignalKernelBackend>> make_cpu_signal_kernel_backend() {
#if defined(SIGNAL_STUDIO_HAVE_ONEMKL)
  return std::shared_ptr<ISignalKernelBackend>(std::make_shared<OneMklSignalKernelBackend>());
#else
  return error(core::ErrorReason::unavailable, "oneMKL VSL/LAPACKE 信号核适配器未构建",
               "请运行 scripts/install-ms02-dependencies.ps1 并重新配置");
#endif
}

} // namespace signal::dsp
