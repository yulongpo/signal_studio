#include "signal_studio/core/services.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/dsp/analysis.hpp"
#include "signal_studio/dsp/pipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
using namespace signal;
using namespace signal::dsp;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename T> void require(const core::Result<T>& result, std::string_view message) {
  require(static_cast<bool>(result), message);
}

void require(const core::Status& status, std::string_view message) {
  require(static_cast<bool>(status), message);
}

data::SampleRange range(std::uint64_t count) {
  const auto result = data::SampleRange::from_count(0U, count);
  require(result, "测试样本范围创建失败");
  return result.value();
}

data::SignalDescriptor descriptor(data::SignalKind kind, std::uint64_t count, double sample_rate = 48'000.0,
                                  std::string unit = "linear") {
  data::SignalDescriptor result;
  result.signal_kind = kind;
  result.scalar_type = data::ScalarType::float64;
  result.component_layout =
      kind == data::SignalKind::complex ? data::ComponentLayout::interleaved : data::ComponentLayout::real;
  result.component_order =
      kind == data::SignalKind::complex ? data::ComponentOrder::iq : data::ComponentOrder::not_applicable;
  result.endianness = data::Endianness::little;
  result.sample_rate_hz = sample_rate;
  result.requested_sample_range = range(count);
  result.amplitude_mode = std::move(unit);
  return result;
}

data::SignalBuffer complex_tone(std::size_t count, std::size_t bin, double amplitude = 1.0) {
  std::vector<data::ComplexSample> values;
  values.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto phase = 2.0 * std::numbers::pi * static_cast<double>(bin * index) / static_cast<double>(count);
    values.push_back({amplitude * std::cos(phase), amplitude * std::sin(phase)});
  }
  return data::SignalBuffer::from_complex(std::move(values));
}

data::SignalBuffer real_tone(std::size_t count, std::size_t bin, double amplitude = 1.0) {
  std::vector<double> values;
  values.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto phase = 2.0 * std::numbers::pi * static_cast<double>(bin * index) / static_cast<double>(count);
    values.push_back(amplitude * std::cos(phase));
  }
  return data::SignalBuffer::from_real(std::move(values));
}

std::shared_ptr<IFftBackend> fft_backend() {
  const auto result = make_cpu_fft_backend();
  require(result, "具名 DSP 测试要求已安装 oneMKL FFT 适配器");
  return result.value();
}

std::shared_ptr<ISignalKernelBackend> kernel_backend() {
  const auto result = make_cpu_signal_kernel_backend();
  require(result, "具名 DSP 测试要求已安装 oneMKL VSL/LAPACKE 适配器");
  return result.value();
}

NodeSpec node(std::string id, NodeKind kind) {
  NodeSpec result;
  result.id = std::move(id);
  result.kind = kind;
  result.contract.accepts_real = true;
  result.contract.accepts_complex = true;
  // 本测试夹具默认用于复输入处理链；契约必须声明该节点的实际输出仍为复数。
  result.contract.produces_complex = true;
  return result;
}

std::vector<double> lowpass(std::size_t count, double cutoff) {
  std::vector<double> coefficients(count);
  const auto center = static_cast<double>(count - 1U) / 2.0;
  double sum{};
  for (std::size_t index = 0; index < count; ++index) {
    const auto offset = static_cast<double>(index) - center;
    const auto sinc = std::abs(offset) < 1e-12
                          ? 2.0 * cutoff
                          : std::sin(2.0 * std::numbers::pi * cutoff * offset) / (std::numbers::pi * offset);
    const auto window =
        0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(count - 1U));
    coefficients[index] = sinc * window;
    sum += coefficients[index];
  }
  for (auto& coefficient : coefficients) {
    coefficient /= sum;
  }
  return coefficients;
}

void require_close(double actual, double expected, double tolerance, std::string_view message) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void require_samples_close(std::span<const data::ComplexSample> actual, std::span<const data::ComplexSample> expected,
                           double tolerance) {
  require(actual.size() == expected.size(), "样本数量不一致");
  for (std::size_t index = 0; index < actual.size(); ++index) {
    require_close(actual[index].real, expected[index].real, tolerance, "实部数值不一致");
    require_close(actual[index].imag, expected[index].imag, tolerance, "虚部数值不一致");
  }
}

double response_db(std::span<const double> numerator, std::span<const double> denominator,
                   double normalized_frequency) {
  data::ComplexSample top{};
  data::ComplexSample bottom{1.0, 0.0};
  for (std::size_t index = 0; index < numerator.size(); ++index) {
    const auto phase = -2.0 * std::numbers::pi * normalized_frequency * static_cast<double>(index);
    top.real += numerator[index] * std::cos(phase);
    top.imag += numerator[index] * std::sin(phase);
  }
  if (!denominator.empty()) {
    bottom = {};
    for (std::size_t index = 0; index < denominator.size(); ++index) {
      const auto phase = -2.0 * std::numbers::pi * normalized_frequency * static_cast<double>(index);
      bottom.real += denominator[index] * std::cos(phase);
      bottom.imag += denominator[index] * std::sin(phase);
    }
  }
  const auto top_power = top.real * top.real + top.imag * top.imag;
  const auto bottom_power = bottom.real * bottom.real + bottom.imag * bottom.imag;
  return 10.0 * std::log10(std::max(top_power / bottom_power, 1e-300));
}

std::string sha256_doubles(std::span<const double> values) {
  const auto digest = core::hash_bytes(std::as_bytes(values));
  require(digest, "golden 输入 SHA-256 计算失败");
  return digest.value().hex();
}

std::string sha256_complex(std::span<const data::ComplexSample> values) {
  const auto digest = core::hash_bytes(std::as_bytes(values));
  require(digest, "golden 复数输出 SHA-256 计算失败");
  return digest.value().hex();
}

class CancellingKernelBackend final : public ISignalKernelBackend {
public:
  CancellingKernelBackend(std::shared_ptr<ISignalKernelBackend> delegate,
                          std::shared_ptr<std::atomic_bool> cancellation)
      : delegate_(std::move(delegate)), cancellation_(std::move(cancellation)) {}

  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return delegate_->backend_id();
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>> analytic_signal(std::span<const double> input) override {
    return delegate_->analytic_signal(input);
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>> convolve(std::span<const data::ComplexSample> input,
                                                                        std::span<const double> coefficients,
                                                                        FilterState& state,
                                                                        BoundaryPolicy boundary) override {
    auto result = delegate_->convolve(input, coefficients, state, boundary);
    cancellation_->store(true, std::memory_order_release);
    return result;
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>>
  solve_iir(std::span<const data::ComplexSample> input, std::span<const double> numerator,
            std::span<const double> denominator, FilterState& state, BoundaryPolicy boundary) override {
    return delegate_->solve_iir(input, numerator, denominator, state, boundary);
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>>
  resample(std::span<const data::ComplexSample> input, std::uint32_t numerator, std::uint32_t denominator,
           std::span<const double> anti_alias_coefficients, FilterState& state, bool end_of_input) override {
    return delegate_->resample(input, numerator, denominator, anti_alias_coefficients, state, end_of_input);
  }

private:
  std::shared_ptr<ISignalKernelBackend> delegate_;
  std::shared_ptr<std::atomic_bool> cancellation_;
};

enum class MalformedFftResult : std::uint8_t { extra_bin, non_finite_bin };

class MalformedFftPlan final : public IFftPlan {
public:
  MalformedFftPlan(FftSpec specification, MalformedFftResult malformed)
      : specification_(specification), malformed_(malformed) {}

  [[nodiscard]] FftSpec spec() const noexcept override {
    return specification_;
  }

  [[nodiscard]] core::Result<FftResult> process(std::span<const data::ComplexSample>) override {
    std::vector<data::ComplexSample> bins(static_cast<std::size_t>(specification_.length));
    if (malformed_ == MalformedFftResult::extra_bin) {
      bins.push_back({});
    } else {
      bins.front().real = std::numeric_limits<double>::quiet_NaN();
    }
    return FftResult{std::move(bins), {}};
  }

private:
  FftSpec specification_;
  MalformedFftResult malformed_;
};

class MalformedFftBackend final : public IFftBackend {
public:
  explicit MalformedFftBackend(MalformedFftResult malformed) : malformed_(malformed) {}

  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return "test-malformed-fft";
  }

  [[nodiscard]] core::Status validate(const FftSpec& spec) const override {
    return spec.length >= 2U ? core::Status::success()
                             : core::Status::failure({core::ErrorDomain::dsp, core::ErrorReason::invalid_argument},
                                                     "测试 FFT 长度无效");
  }

  [[nodiscard]] core::Result<std::shared_ptr<IFftPlan>> create_plan(const FftSpec& spec) override {
    return std::shared_ptr<IFftPlan>(std::make_shared<MalformedFftPlan>(spec, malformed_));
  }

private:
  MalformedFftResult malformed_;
};

void test_dsp_001() {
  ProcessingChain chain;
  auto gain = node("gain", NodeKind::gain);
  gain.gain = 2.0;
  require(chain.append(gain), "处理链节点添加失败");
  ChannelSnapshot channel{"channel-a", range(1024U), "source-version-1", descriptor(data::SignalKind::complex, 1024U),
                          chain.snapshot()};
  auto independent = channel;
  independent.processing_chain.nodes.front().gain = 3.0;
  require(channel.input_selection == range(1024U) && channel.data_source_version_id == "source-version-1" &&
              channel.output_descriptor.sample_rate_hz == 48'000.0 &&
              channel.processing_chain.nodes.front().gain == 2.0 &&
              independent.processing_chain.nodes.front().gain == 3.0,
          "通道快照未独立保存 Selection、版本、输出描述符和处理链");
  ChannelSnapshot invalid;
  require(invalid.channel_id.empty() && invalid.data_source_version_id.empty(), "失败夹具初始状态错误");
}

void test_dsp_002() {
  const auto backend = kernel_backend();
  auto contract_node = node("contract", NodeKind::gain);
  contract_node.contract.accepts_real = false;
  contract_node.contract.accepts_complex = true;
  contract_node.contract.input_unit = "volts";
  contract_node.contract.output_unit = "linear";
  require(validate_node(contract_node, descriptor(data::SignalKind::complex, 8U, 48'000.0, "volts")),
          "复信号契约应通过");
  require(!validate_node(contract_node, descriptor(data::SignalKind::real, 8U)), "实信号契约冲突必须失败");
  contract_node.contract.sample_rate_denominator = 0.0;
  require(!validate_node(contract_node, descriptor(data::SignalKind::complex, 8U)), "零采样率关系必须失败");
  contract_node.contract.sample_rate_denominator = 1.0;

  const auto input = complex_tone(8U, 1U);
  const auto processed = process_chain(*backend,
                                       {input.view(),
                                        descriptor(data::SignalKind::complex, 8U, 48'000.0, "volts"),
                                        {"signal-processing-chain/1.0", "1.0.0", {contract_node}},
                                        BoundaryPolicy::zero_pad,
                                        nullptr},
                                       {});
  require(processed && processed.value().descriptor.amplitude_mode == "linear" &&
              processed.value().descriptor.sample_rate_hz == 48'000.0 &&
              processed.value().descriptor.signal_kind == data::SignalKind::complex,
          "节点契约未真实传播单位、采样率和实复类型");

  contract_node.contract.sample_rate_numerator = 2.0;
  contract_node.contract.sample_rate_denominator = 3.0;
  require(!validate_node(contract_node, descriptor(data::SignalKind::complex, 8U, 48'000.0, "volts")),
          "非重采样节点不得声明虚假的采样率关系");
  contract_node.contract.sample_rate_numerator = 1.0;
  contract_node.contract.sample_rate_denominator = 1.0;
  contract_node.contract.accepts_real = true;
  contract_node.contract.produces_complex = false;
  require(validate_node(contract_node, descriptor(data::SignalKind::real, 8U, 48'000.0, "volts")),
          "实输入增益节点应声明并产生实输出");
  contract_node.contract.produces_complex = true;
  require(!validate_node(contract_node, descriptor(data::SignalKind::real, 8U, 48'000.0, "volts")),
          "无法产生复数的节点不得声明 produces_complex");
  contract_node.contract.produces_complex = false;
  require(!validate_node(contract_node, descriptor(data::SignalKind::complex, 8U, 48'000.0, "volts")),
          "复输入增益节点不得把实际复输出声明为实数");

  auto complex_fir = node("complex-fir-contract", NodeKind::fir_filter);
  complex_fir.numerator = {0.25, 0.5, 0.25};
  require(validate_node(complex_fir, descriptor(data::SignalKind::complex, 8U)), "复输入 FIR 应声明并产生复输出");
  complex_fir.contract.produces_complex = false;
  require(!validate_node(complex_fir, descriptor(data::SignalKind::complex, 8U)),
          "复输入 FIR 的 produces_complex 错误声明必须失败");
}

void test_dsp_003() {
  const auto backend = kernel_backend();
  std::vector<data::ComplexSample> values{{2.0, 1.0}, {4.0, -1.0}, {6.0, 1.0}, {8.0, -1.0}};
  auto input = data::SignalBuffer::from_complex(values);
  auto remove = node("dc", NodeKind::remove_dc);
  auto gain = node("gain", NodeKind::gain);
  gain.gain = 0.5;
  auto iq = node("iq", NodeKind::iq_correction);
  iq.iq_gain_balance = 2.0;
  iq.iq_phase_radians = 0.1;
  const ProcessRequest request{input.view(),
                               descriptor(data::SignalKind::complex, values.size()),
                               {"signal-processing-chain/1.0", "1.0.0", {remove, gain, iq}},
                               BoundaryPolicy::preserve_state,
                               nullptr};
  const auto result = process_chain(*backend, request, {});
  require(result && result.value().applied_node_ids.size() == 3U, "去直流/增益/IQ 校正链失败");
  double mean_real{};
  double mean_imag{};
  for (const auto& sample : result.value().samples.view().complex_values()) {
    mean_real += sample.real;
    mean_imag += sample.imag;
  }
  require_close(mean_real, 0.0, 1e-12, "去直流后实部均值错误");
  require_close(mean_imag, 0.0, 1e-12, "去直流后虚部均值错误");
  require(!validate_node(iq, descriptor(data::SignalKind::real, 4U)), "实信号 IQ 校正必须给出契约错误");
}

void test_dsp_004() {
  const auto backend = kernel_backend();
  const auto fft = fft_backend();
  auto input = real_tone(128U, 8U);
  auto shift = node("shift", NodeKind::frequency_shift);
  shift.frequency_shift_hz = 3'000.0;
  shift.real_to_complex = RealToComplexMode::analytic_signal;
  const auto result = process_chain(*backend,
                                    {input.view(),
                                     descriptor(data::SignalKind::real, 128U),
                                     {"signal-processing-chain/1.0", "1.0.0", {shift}},
                                     BoundaryPolicy::preserve_state,
                                     nullptr},
                                    {});
  require(result && result.value().samples.kind() == data::SignalKind::complex &&
              result.value().descriptor.signal_kind == data::SignalKind::complex,
          "解析信号频移未产生复基带");
  const auto shifted_fft = fft->execute({128U, FftDirection::forward}, result.value().samples.view().complex_values());
  require(shifted_fft, "解析频移输出 FFT 失败");
  const auto power = [](const data::ComplexSample& value) { return value.real * value.real + value.imag * value.imag; };
  const auto desired_power = power(shifted_fft.value().bins[16U]);
  const auto image_power = power(shifted_fft.value().bins[112U]);
  require_close(std::sqrt(desired_power) / 128.0, 1.0, 1e-10, "解析频移峰值幅度错误");
  require(10.0 * std::log10(std::max(image_power / desired_power, 1e-300)) < -120.0, "解析信号镜像抑制低于 120 dB");
  require_close(result.value().samples.view().complex_values().front().real, 1.0, 1e-10, "解析频移起始相位错误");
  require_close(result.value().samples.view().complex_values().front().imag, 0.0, 1e-10, "解析频移正交相位错误");
  shift.real_to_complex = RealToComplexMode::forbidden;
  require(!validate_node(shift, descriptor(data::SignalKind::real, 128U)), "实信号未显式选择转换模式必须失败");
  shift.real_to_complex = RealToComplexMode::quadrature_mixer;
  shift.frequency_shift_hz = 3'000.0;
  const auto quadrature = process_chain(*backend,
                                        {input.view(),
                                         descriptor(data::SignalKind::real, 128U),
                                         {"signal-processing-chain/1.0", "1.0.0", {shift}},
                                         BoundaryPolicy::zero_pad,
                                         nullptr},
                                        {});
  require(quadrature && quadrature.value().samples.kind() == data::SignalKind::complex, "显式正交混频未产生复数 I/Q");
  const auto quadrature_values = quadrature.value().samples.view().complex_values();
  const auto expected_phase = 2.0 * std::numbers::pi * 8.0 / 128.0;
  require_close(quadrature_values[1].real, std::cos(expected_phase) * std::cos(expected_phase), 1e-10,
                "正交混频 I 分量错误");
  require_close(quadrature_values[1].imag, std::cos(expected_phase) * std::sin(expected_phase), 1e-10,
                "正交混频 Q 分量错误");
  shift.frequency_shift_hz = 30'000.0;
  require(!validate_node(shift, descriptor(data::SignalKind::real, 128U)), "超 Nyquist 频移必须失败");
}

void test_dsp_005() {
  const auto backend = kernel_backend();
  const auto input = complex_tone(256U, 7U);
  constexpr double sample_rate = 48'000.0;
  const auto verify_shape = [&](NodeKind kind, FilterShape shape, double pass_a_hz, double stop_a_hz,
                                double pass_b_hz = 0.0, double stop_b_hz = 0.0) {
    auto filter = node(kind == NodeKind::fir_filter ? "fir" : "iir", kind);
    filter.filter_shape = shape;
    filter.parameters["order"] = kind == NodeKind::fir_filter ? 96.0 : 2.0;
    if (shape == FilterShape::lowpass || shape == FilterShape::highpass) {
      filter.parameters["cutoff_hz"] = 6'000.0;
    } else {
      filter.parameters["low_cutoff_hz"] = 5'000.0;
      filter.parameters["high_cutoff_hz"] = 9'000.0;
    }
    const auto coefficients = resolve_filter_coefficients(filter, sample_rate);
    require(coefficients, "形态滤波器设计失败");
    const auto pass_db =
        response_db(coefficients.value().numerator, coefficients.value().denominator, pass_a_hz / sample_rate);
    const auto stop_db =
        response_db(coefficients.value().numerator, coefficients.value().denominator, stop_a_hz / sample_rate);
    require(pass_db > -3.1, "形态滤波器通带响应错误");
    require(stop_db < (kind == NodeKind::fir_filter ? -35.0 : -9.0), "形态滤波器阻带响应错误");
    if (pass_b_hz > 0.0) {
      require(response_db(coefficients.value().numerator, coefficients.value().denominator, pass_b_hz / sample_rate) >
                  -3.1,
              "双通带滤波器第二通带响应错误");
    }
    if (stop_b_hz > 0.0) {
      require(response_db(coefficients.value().numerator, coefficients.value().denominator, stop_b_hz / sample_rate) <
                  (kind == NodeKind::fir_filter ? -35.0 : -9.0),
              "双阻带滤波器第二阻带响应错误");
    }
    const auto filter_api = make_filter(backend);
    require(filter_api, "IFilter Stable API 创建失败");
    FilterState state;
    const auto filtered = filter_api.value()->process(filter, sample_rate, input.view().complex_values(), state,
                                                      BoundaryPolicy::zero_pad);
    require(filtered && filtered.value().size() == input.size(), "IFilter 形态执行失败");
    std::vector<data::ComplexSample> impulse_values(4096U);
    impulse_values.front().real = 1.0;
    const auto impulse = data::SignalBuffer::from_complex(std::move(impulse_values));
    const auto chained = process_chain(*backend,
                                       {impulse.view(),
                                        descriptor(data::SignalKind::complex, impulse.size(), sample_rate),
                                        {"signal-processing-chain/1.0", "1.0.0", {filter}},
                                        BoundaryPolicy::zero_pad,
                                        nullptr},
                                       {});
    require(chained, "主处理链未执行形态滤波器");
    const auto chained_fft = fft_backend()->execute({chained.value().samples.size(), FftDirection::forward},
                                                    chained.value().samples.view().complex_values());
    require(chained_fft, "主处理链形态滤波结果 FFT 失败");
    const auto bin_for = [&](double frequency_hz) {
      return static_cast<std::size_t>(
          std::llround(frequency_hz * static_cast<double>(chained.value().samples.size()) / sample_rate));
    };
    const auto pass_magnitude = std::hypot(chained_fft.value().bins[bin_for(pass_a_hz)].real,
                                           chained_fft.value().bins[bin_for(pass_a_hz)].imag);
    const auto stop_magnitude = std::hypot(chained_fft.value().bins[bin_for(stop_a_hz)].real,
                                           chained_fft.value().bins[bin_for(stop_a_hz)].imag);
    require(20.0 * std::log10(std::max(stop_magnitude / pass_magnitude, 1e-300)) <
                (kind == NodeKind::fir_filter ? -34.0 : -8.0),
            "主处理链实际输出未达到形态滤波阻带");
  };

  for (const auto kind : {NodeKind::fir_filter, NodeKind::iir_filter}) {
    verify_shape(kind, FilterShape::lowpass, 1'000.0, 15'000.0);
    verify_shape(kind, FilterShape::highpass, 15'000.0, 1'000.0);
    verify_shape(kind, FilterShape::bandpass, 7'000.0, 1'000.0, 0.0, 15'000.0);
    verify_shape(kind, FilterShape::bandstop, 1'000.0, 7'000.0, 15'000.0);
  }

  auto custom_fir = node("custom-fir", NodeKind::fir_filter);
  custom_fir.filter_shape = FilterShape::custom;
  custom_fir.numerator = {0.25, 0.5, 0.25};
  const auto custom_fir_coefficients = resolve_filter_coefficients(custom_fir, sample_rate);
  require(custom_fir_coefficients && custom_fir_coefficients.value().numerator == custom_fir.numerator,
          "自定义 FIR 系数未原样执行");
  auto custom_iir = node("custom-iir", NodeKind::iir_filter);
  custom_iir.filter_shape = FilterShape::custom;
  custom_iir.numerator = {0.5};
  custom_iir.denominator = {1.0, -0.5};
  const auto custom_iir_coefficients = resolve_filter_coefficients(custom_iir, sample_rate);
  require(custom_iir_coefficients && custom_iir_coefficients.value().denominator == custom_iir.denominator,
          "自定义 IIR 系数未原样执行");

  auto common_stable = node("common-stable", NodeKind::iir_filter);
  common_stable.numerator = {0.01, 0.02, 0.01};
  common_stable.denominator = {1.0, -1.8, 0.81};
  require(validate_node(common_stable, descriptor(data::SignalKind::complex, 256U)),
          "常见稳定二阶 IIR 被对角占优条件误拒");
  auto unstable = node("unstable", NodeKind::iir_filter);
  unstable.numerator = {1.0};
  unstable.denominator = {1.0, -1.1};
  require(!validate_node(unstable, descriptor(data::SignalKind::complex, 256U)), "不稳定 IIR 必须拒绝");
}

void test_dsp_006() {
  const auto backend = kernel_backend();
  const auto resampler_api = make_resampler(backend);
  require(resampler_api, "IResampler Stable API 创建失败");
  const auto input = complex_tone(600U, 13U);
  auto resample = node("resample", NodeKind::resample);
  resample.numerator = lowpass(127U, 6'000.0 / (48'000.0 * 2.0));
  resample.resample_numerator = 2U;
  resample.resample_denominator = 3U;
  resample.anti_alias_cutoff_hz = 6'000.0;
  resample.anti_alias_stopband_db = 60.0;
  resample.contract.sample_rate_numerator = 2.0;
  resample.contract.sample_rate_denominator = 3.0;
  const auto anti_alias_validation = validate_anti_alias_filter(resample.numerator, 48'000.0, {2U, 3U}, 6'000.0, 60.0);
  if (!anti_alias_validation) {
    std::cerr << "ANTI_ALIAS_DIAGNOSTIC message=" << anti_alias_validation.message()
              << " diagnostic=" << anti_alias_validation.diagnostic() << '\n';
  }
  require(anti_alias_validation, "真实抗混叠 FIR 频响校验失败");
  const auto result = process_chain(*backend,
                                    {input.view(),
                                     descriptor(data::SignalKind::complex, 600U),
                                     {"signal-processing-chain/1.0", "1.0.0", {resample}},
                                     BoundaryPolicy::preserve_state,
                                     nullptr},
                                    {});
  require(result && result.value().samples.size() == 400U, "有理重采样输出速率错误");
  require_close(result.value().descriptor.sample_rate_hz, 32'000.0, 1e-9, "重采样描述符速率错误");
  FilterState decimation_state;
  const auto decimated = resampler_api.value()->process({1U, 2U}, input.view().complex_values(),
                                                        lowpass(127U, 6'000.0 / 48'000.0), decimation_state);
  require(decimated && decimated.value().size() == 300U, "整数抽取路径失败");
  FilterState interpolation_state;
  const auto interpolated = resampler_api.value()->process(
      {2U, 1U}, input.view().complex_values(), lowpass(127U, 6'000.0 / (48'000.0 * 2.0)), interpolation_state);
  require(interpolated && interpolated.value().size() == 1200U, "整数插值路径失败");

  const auto passband = complex_tone(4096U, 171U);
  const auto stopband = complex_tone(4096U, 1536U);
  const auto anti_alias = lowpass(255U, 6'000.0 / 48'000.0);
  FilterState passband_state;
  FilterState stopband_state;
  const auto passband_output =
      resampler_api.value()->process({1U, 3U}, passband.view().complex_values(), anti_alias, passband_state);
  const auto stopband_output =
      resampler_api.value()->process({1U, 3U}, stopband.view().complex_values(), anti_alias, stopband_state);
  require(passband_output && stopband_output, "信号级混叠验证重采样失败");
  const auto mean_power = [](std::span<const data::ComplexSample> values) {
    double power{};
    const auto trim = std::min<std::size_t>(64U, values.size() / 8U);
    const auto stable = values.subspan(trim, values.size() - 2U * trim);
    for (const auto& value : stable) {
      power += value.real * value.real + value.imag * value.imag;
    }
    return power / static_cast<double>(stable.size());
  };
  require(mean_power(stopband_output.value()) <= mean_power(passband_output.value()) * 1.0e-3,
          "48 kHz 到 16 kHz 抽取未将 18 kHz 阻带音抑制至少 30 dB，存在可测混叠");

  auto ineffective = resample;
  ineffective.numerator = {1.0};
  require(!validate_node(ineffective, descriptor(data::SignalKind::complex, 600U)),
          "仅填写阻带元数据但 FIR 不具备抗混叠能力时必须失败");
  resample.anti_alias_stopband_db = 20.0;
  require(!validate_node(resample, descriptor(data::SignalKind::complex, 600U)), "缺少合格抗混叠元数据必须失败");
}

void test_dsp_007() {
  ProcessingChain chain;
  auto first = node("first", NodeKind::gain);
  auto second = node("second", NodeKind::remove_dc);
  require(chain.append(first) && chain.append(second), "节点添加失败");
  require(chain.set_enabled("first", false), "旁路操作失败");
  require(chain.duplicate("second", "copy"), "复制操作失败");
  require(chain.move("copy", 0U), "重排操作失败");
  require(chain.apply_preset("first", {{"gain", 2.5}, {"additive_offset", 1.0}}), "参数预设失败");
  require(chain.erase("second"), "删除操作失败");
  const auto snapshot = chain.snapshot();
  require(snapshot.nodes.size() == 2U && snapshot.nodes.front().id == "copy" && !snapshot.nodes.back().enabled &&
              snapshot.nodes.back().gain == 2.5,
          "节点操作结果错误");
  require(!chain.apply_preset("first", {{"unknown", 1.0}}), "未知预设字段必须失败");

  const auto backend = kernel_backend();
  const auto input = complex_tone(32U, 3U);
  auto bypassed = node("bypassed-invalid-contract", NodeKind::iq_correction);
  bypassed.enabled = false;
  bypassed.contract.accepts_complex = false;
  bypassed.contract.sample_rate_denominator = 0.0;
  bypassed.iq_gain_balance = 0.0;
  const auto bypass_result = process_chain(*backend,
                                           {input.view(),
                                            descriptor(data::SignalKind::complex, input.size()),
                                            {"signal-processing-chain/1.0", "1.0.0", {bypassed}},
                                            BoundaryPolicy::zero_pad,
                                            nullptr},
                                           {});
  require(bypass_result && bypass_result.value().applied_node_ids.empty(), "旁路节点仍在输入契约校验阶段阻断处理链");
  require_samples_close(bypass_result.value().samples.view().complex_values(), input.view().complex_values(), 0.0);
}

void test_dsp_008() {
  const auto backend = kernel_backend();
  const auto input = complex_tone(1000U, 17U);
  const auto taps = lowpass(31U, 0.15);
  FilterState whole_state;
  const auto whole = backend->convolve(input.view().complex_values(), taps, whole_state, BoundaryPolicy::zero_pad);
  require(whole, "整块 FIR 失败");
  FilterState chunk_state;
  const auto values = input.view().complex_values();
  const auto first = backend->convolve(values.first(377U), taps, chunk_state, BoundaryPolicy::zero_pad);
  const auto second = backend->convolve(values.subspan(377U), taps, chunk_state, BoundaryPolicy::preserve_state);
  require(first && second, "分块 FIR 失败");
  std::vector<data::ComplexSample> combined = first.value();
  combined.insert(combined.end(), second.value().begin(), second.value().end());
  require_samples_close(combined, whole.value(), 1e-12);

  FilterState whole_resample_state;
  const auto whole_resample = backend->resample(values, 2U, 3U, taps, whole_resample_state);
  FilterState chunk_resample_state;
  const auto first_resample = backend->resample(values.first(377U), 2U, 3U, taps, chunk_resample_state, false);
  const auto second_resample = backend->resample(values.subspan(377U), 2U, 3U, taps, chunk_resample_state, true);
  require(whole_resample && first_resample && second_resample, "重采样分块执行失败");
  std::vector<data::ComplexSample> combined_resample = first_resample.value();
  combined_resample.insert(combined_resample.end(), second_resample.value().begin(), second_resample.value().end());
  require_samples_close(combined_resample, whole_resample.value(), 1e-11);
  require(whole_resample_state.processed_samples == values.size() &&
              chunk_resample_state.processed_samples == values.size(),
          "重采样 processed_samples 必须按原始输入样本计数");

  const std::array<double, 3U> iir_b{0.4, 0.1, 0.05};
  const std::array<double, 4U> iir_a{1.0, -0.2, 0.05, -0.01};
  std::vector<data::ComplexSample> golden(values.size());
  for (std::size_t row = 0; row < values.size(); ++row) {
    for (std::size_t coefficient = 0; coefficient < iir_b.size() && coefficient <= row; ++coefficient) {
      golden[row].real += iir_b[coefficient] * values[row - coefficient].real;
      golden[row].imag += iir_b[coefficient] * values[row - coefficient].imag;
    }
    for (std::size_t coefficient = 1; coefficient < iir_a.size() && coefficient <= row; ++coefficient) {
      golden[row].real -= iir_a[coefficient] * golden[row - coefficient].real;
      golden[row].imag -= iir_a[coefficient] * golden[row - coefficient].imag;
    }
  }
  FilterState whole_iir_state;
  const auto whole_iir = backend->solve_iir(values, iir_b, iir_a, whole_iir_state, BoundaryPolicy::zero_pad);
  FilterState chunk_iir_state;
  const auto first_iir =
      backend->solve_iir(values.first(377U), iir_b, iir_a, chunk_iir_state, BoundaryPolicy::zero_pad);
  const auto second_iir =
      backend->solve_iir(values.subspan(377U), iir_b, iir_a, chunk_iir_state, BoundaryPolicy::preserve_state);
  require(whole_iir && first_iir && second_iir, "高阶 IIR 分块执行失败");
  std::vector<data::ComplexSample> combined_iir = first_iir.value();
  combined_iir.insert(combined_iir.end(), second_iir.value().begin(), second_iir.value().end());
  require_samples_close(whole_iir.value(), golden, 1e-11);
  require_samples_close(combined_iir, golden, 1e-11);
}

void test_dsp_009() {
  const auto delegate = kernel_backend();
  const auto input = complex_tone(131'072U, 5U);
  auto fir = node("fir", NodeKind::fir_filter);
  fir.numerator = lowpass(31U, 0.15);
  auto gain = node("gain", NodeKind::gain);
  const auto cancellation = std::make_shared<std::atomic_bool>(false);
  CancellingKernelBackend backend(delegate, cancellation);
  const auto cancelled_result = process_chain(backend,
                                              {input.view(),
                                               descriptor(data::SignalKind::complex, input.size()),
                                               {"signal-processing-chain/1.0", "1.0.0", {fir, gain}},
                                               BoundaryPolicy::preserve_state,
                                               cancellation},
                                              {});
  require(!cancelled_result && cancelled_result.error().code().reason == core::ErrorReason::cancelled,
          "节点执行中途取消后仍发布部分处理结果");

  cancellation->store(false, std::memory_order_release);
  const auto short_input = complex_tone(4096U, 5U);
  const auto cancelled_after_final_kernel = process_chain(backend,
                                                          {short_input.view(),
                                                           descriptor(data::SignalKind::complex, short_input.size()),
                                                           {"signal-processing-chain/1.0", "1.0.0", {fir}},
                                                           BoundaryPolicy::preserve_state,
                                                           cancellation},
                                                          {});
  require(!cancelled_after_final_kernel &&
              cancelled_after_final_kernel.error().code().reason == core::ErrorReason::cancelled,
          "最终内核返回后到结果发布前发生取消时仍发布了结果");

  const auto long_input = complex_tone(4'194'304U, 7U);
  auto cancellable_gain = node("cancellable-gain", NodeKind::gain);
  cancellable_gain.gain = 1.25;
  cancellation->store(false, std::memory_order_release);
  std::atomic_bool process_started{false};
  auto simple_node_result = std::async(std::launch::async, [&] {
    process_started.store(true, std::memory_order_release);
    return process_chain(*delegate,
                         {long_input.view(),
                          descriptor(data::SignalKind::complex, long_input.size()),
                          {"signal-processing-chain/1.0", "1.0.0", {cancellable_gain}},
                          BoundaryPolicy::zero_pad,
                          cancellation},
                         {});
  });
  while (!process_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  cancellation->store(true, std::memory_order_release);
  const auto cancelled_simple_node = simple_node_result.get();
  require(!cancelled_simple_node && cancelled_simple_node.error().code().reason == core::ErrorReason::cancelled,
          "简单节点长循环未响应有界取消或在发布前复核取消");
  ProcessingChain chain;
  auto upstream = node("upstream", NodeKind::gain);
  require(chain.append(upstream) && chain.append(fir) && chain.append(gain), "失效链夹具失败");
  ProcessingCache cache;
  require(cache.put("upstream", complex_tone(8U, 1U)) && cache.put("fir", complex_tone(8U, 2U)) &&
              cache.put("gain", complex_tone(8U, 3U)),
          "节点缓存写入失败");
  const auto invalidated = cache.invalidate_downstream(chain.snapshot(), "fir");
  require(invalidated == std::vector<std::string>({"fir", "gain"}) && cache.contains("upstream") &&
              !cache.contains("fir") && !cache.contains("gain") && cache.size() == 1U,
          "参数变化未真实保留上游并只删除自身及下游缓存");
  require(cache.invalidate_downstream(chain.snapshot(), "missing").empty() && cache.size() == 1U,
          "未知节点不得失效真实缓存");
}

void test_dsp_010() {
  const auto backend = kernel_backend();
  const auto input = complex_tone(256U, 5U);
  auto fir = node("fir", NodeKind::fir_filter);
  fir.numerator = lowpass(31U, 0.15);
  const auto preview = preview_node(*backend, input.view(), descriptor(data::SignalKind::complex, 256U), fir);
  require(preview && preview.value().before.size() == preview.value().after.size() &&
              preview.value().response_frequency_hz.size() == 129U &&
              preview.value().response_magnitude_db.size() == 129U,
          "节点级前后对比或频响预览失败");
  require_close(preview.value().group_delay_samples, 15.0, 1e-12, "FIR 时延错误");
  auto iir = node("iir", NodeKind::iir_filter);
  iir.numerator = {0.5};
  iir.denominator = {1.0, -0.5};
  const auto iir_preview = preview_node(*backend, input.view(), descriptor(data::SignalKind::complex, 256U), iir);
  require(iir_preview && std::isfinite(iir_preview.value().group_delay_samples), "IIR 数值相位时延未计算");
}

void test_dsp_011() {
  static_assert(!std::is_aggregate_v<ProcessingProvenance>);
  static_assert(!std::is_default_constructible_v<ProcessingProvenance>);
  auto gain = node("gain", NodeKind::gain);
  gain.implementation_id = "signal.dsp.builtin/1.0";
  gain.gain = 2.0;
  gain.parameters = {{"gain", 2.0}};
  auto filter = node("filter", NodeKind::fir_filter);
  filter.implementation_id = "signal.dsp.onemkl/1.0";
  filter.filter_shape = FilterShape::custom;
  filter.numerator = {0.25, 0.5, 0.25};
  const ChainSnapshot chain{"signal-processing-chain/1.0", "1.0.0", {gain, filter}};
  const auto provenance = make_processing_provenance(
      "sha256:abc", range(100U), "source-version", chain, "oneMKL-VSL-LAPACKE",
      {data::SignalKind::complex, 100U, 48'000.0, "volts"}, {data::SignalKind::complex, 100U, 48'000.0, "linear"});
  require(provenance, "不可变处理 provenance 派生失败");
  const auto serialized = serialize_processing_provenance(provenance.value());
  require(serialized && serialized.value().find("sha256:abc") != std::string::npos &&
              serialized.value().find("source-version") != std::string::npos &&
              serialized.value().find("signal.dsp.builtin/1.0") != std::string::npos &&
              serialized.value().find("0.25") != std::string::npos &&
              serialized.value().find("48'000") == std::string::npos &&
              serialized.value().find("48000") != std::string::npos &&
              serialized.value().find("volts") != std::string::npos &&
              serialized.value().find("linear") != std::string::npos &&
              serialized.value().find("oneMKL-VSL-LAPACKE") != std::string::npos,
          "派生 provenance 缺少实现、完整参数/系数、源区间或输入输出摘要");
  require(!make_processing_provenance("", range(100U), "source-version", chain, "oneMKL",
                                      {data::SignalKind::complex, 100U, 48'000.0, "linear"},
                                      {data::SignalKind::complex, 100U, 48'000.0, "linear"}),
          "缺少源指纹必须在不可变 provenance 创建阶段失败");
}

void test_dsp_012() {
  auto filter = node("filter", NodeKind::resample);
  filter.implementation_id = "signal.dsp.onemkl/1.0";
  filter.filter_shape = FilterShape::lowpass;
  filter.numerator = lowpass(15U, 0.2);
  filter.denominator = {};
  filter.resample_numerator = 2U;
  filter.resample_denominator = 3U;
  filter.anti_alias_cutoff_hz = 6'000.0;
  filter.anti_alias_stopband_db = 60.0;
  filter.contract.sample_rate_numerator = 2.0;
  filter.contract.sample_rate_denominator = 3.0;
  filter.parameters = {{"quality", 1.0}};
  const ChainSnapshot chain{"signal-processing-chain/1.0", "1.0.0", {filter}};
  const auto serialized = export_chain_template(chain);
  require(serialized, "处理链模板导出失败");
  const std::array<std::string, 1> available{"signal.dsp.onemkl/1.0"};
  const auto restored = import_chain_template(serialized.value(), available);
  require(restored && restored.value().nodes.size() == 1U &&
              restored.value().nodes.front().numerator == filter.numerator &&
              restored.value().nodes.front().resample_numerator == 2U &&
              restored.value().nodes.front().resample_denominator == 3U &&
              restored.value().nodes.front().contract.sample_rate_numerator == 2.0 &&
              restored.value().nodes.front().filter_shape == FilterShape::lowpass,
          "处理链模板未完整确定性往返");
  const std::array<std::string, 1> missing{"other"};
  require(!import_chain_template(serialized.value(), missing), "缺少依赖实现必须失败");
  auto incompatible = serialized.value();
  const auto position = incompatible.find("1.0.0");
  incompatible.replace(position, 5U, "2.0.0");
  require(!import_chain_template(incompatible, available), "不兼容模板版本必须失败");
}

void test_num_001() {
  constexpr double sample_rate = 50'000'000.0;
  const auto sample = time_to_sample(0.12345678, sample_rate, 10'000'000U);
  require(sample, "时间到样本映射失败");
  const auto time = sample_to_time(sample.value(), sample_rate);
  require(time, "样本到时间映射失败");
  require(std::abs(time.value() * sample_rate - 6'172'839.0) <= 1.0, "时间映射误差超过一个输入样本");
  require(!time_to_sample(-1.0, sample_rate, 10U), "负时间必须失败");
}

void test_num_002() {
  constexpr double sample_rate = 48'000.0;
  const auto circular_error = [](double expected, double actual) {
    const auto direct = std::abs(expected - actual);
    return std::min(direct, sample_rate - direct);
  };
  for (const auto length : {1024U, 1025U}) {
    const auto bin_width = sample_rate / static_cast<double>(length);
    for (const auto requested : {-sample_rate / 2.0, -sample_rate / 2.0 + 0.49 * bin_width, -3'000.0, 0.0, 3'000.0,
                                 sample_rate / 2.0 - 0.49 * bin_width, sample_rate / 2.0}) {
      const auto bin = frequency_to_bin(requested, sample_rate, length, SpectrumSidedness::two_sided_shifted);
      require(bin, "双边频率到 bin 映射失败");
      const auto frequency = bin_to_frequency(bin.value(), sample_rate, length, SpectrumSidedness::two_sided_shifted);
      require(frequency, "双边 bin 到频率映射失败");
      require(circular_error(requested, frequency.value()) <= 0.5 * bin_width + 1e-12,
              "双边频率钳位/往返误差超过 0.5 bin");
    }
  }
  const auto positive_nyquist =
      frequency_to_bin(sample_rate / 2.0, sample_rate, 1024U, SpectrumSidedness::two_sided_shifted);
  const auto negative_nyquist =
      frequency_to_bin(-sample_rate / 2.0, sample_rate, 1024U, SpectrumSidedness::two_sided_shifted);
  require(positive_nyquist && negative_nyquist && positive_nyquist.value() == 0U && negative_nyquist.value() == 0U,
          "偶数 N 的 ±Nyquist 必须映射到同一个周期 FFT bin，而不是钳位到 N-1");
  require(!frequency_to_bin(sample_rate, sample_rate, 1024U, SpectrumSidedness::one_sided), "超范围频率必须失败");
  require(!frequency_to_bin(0.0, std::numeric_limits<double>::infinity(), 1024U, SpectrumSidedness::two_sided_shifted),
          "非有限采样率不得进入频率到 bin 映射");
  require(!bin_to_frequency(0U, std::numeric_limits<double>::infinity(), 1024U, SpectrumSidedness::two_sided_shifted),
          "非有限采样率不得进入 bin 到频率映射");
  constexpr std::uint64_t unsupported_length = std::uint64_t{1} << 54U;
  require(!frequency_to_bin(0.0, sample_rate, unsupported_length, SpectrumSidedness::two_sided_shifted),
          "超过 double 精确整数范围的 FFT 长度必须明确拒绝");
  require(!bin_to_frequency(0U, sample_rate, unsupported_length, SpectrumSidedness::two_sided_shifted),
          "超大 FFT 长度不得产生不可信频率结果");
}

void test_num_003() {
  auto backend = fft_backend();
  constexpr std::size_t length = 1024U;
  constexpr std::size_t tone_bin = 64U;
  constexpr double sample_rate = 48'000.0;
  const auto input = complex_tone(length, tone_bin);
  const auto spectrum = calculate_spectrum(
      *backend, input.view(), {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::two_sided_shifted});
  require(spectrum, "oneMKL FFT 频谱失败");
  const auto peak = std::max_element(spectrum.value().magnitude_dbfs.begin(), spectrum.value().magnitude_dbfs.end());
  require_close(*peak, 0.0, 0.1, "FFT 幅值与解析参考误差超过 0.1 dB");
  const auto psd = calculate_psd(*backend, input.view(),
                                 {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::two_sided_shifted});
  require(psd, "oneMKL PSD 失败");
  const auto peak_psd = *std::max_element(psd.value().db_per_hz.begin(), psd.value().db_per_hz.end());
  const auto reference = 10.0 * std::log10(static_cast<double>(length) / sample_rate);
  require_close(peak_psd, reference, 0.1, "PSD dB/Hz 归一化与解析参考误差超过 0.1 dB");
  require(psd.value().equivalent_noise_bandwidth_hz > 0.0, "PSD ENBW 未记录");

  MalformedFftBackend extra_bin_backend(MalformedFftResult::extra_bin);
  require(!calculate_psd(extra_bin_backend, input.view(),
                         {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::two_sided_shifted}),
          "PSD 未拒绝 FFT 后端返回的错误 bin 数");
  MalformedFftBackend non_finite_backend(MalformedFftResult::non_finite_bin);
  require(!calculate_psd(non_finite_backend, input.view(),
                         {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::two_sided_shifted}),
          "PSD 未拒绝 FFT 后端返回的非有限 bin");

  const auto tone_fft = backend->execute({length, FftDirection::forward}, input.view().complex_values());
  require(tone_fft, "单音 golden FFT 失败");
  double maximum_error{};
  double squared_error{};
  for (std::size_t index = 0; index < length; ++index) {
    const data::ComplexSample expected =
        index == tone_bin ? data::ComplexSample{static_cast<double>(length), 0.0} : data::ComplexSample{};
    const auto real_error = tone_fft.value().bins[index].real - expected.real;
    const auto imag_error = tone_fft.value().bins[index].imag - expected.imag;
    const auto absolute_error = std::hypot(real_error, imag_error);
    maximum_error = std::max(maximum_error, absolute_error);
    squared_error += absolute_error * absolute_error;
  }

  std::vector<data::ComplexSample> two_tone_values(length);
  for (std::size_t index = 0; index < length; ++index) {
    const auto phase_a = 2.0 * std::numbers::pi * 32.0 * static_cast<double>(index) / static_cast<double>(length);
    const auto phase_b = 2.0 * std::numbers::pi * 96.0 * static_cast<double>(index) / static_cast<double>(length);
    two_tone_values[index] = {std::cos(phase_a) + 0.5 * std::cos(phase_b), std::sin(phase_a) + 0.5 * std::sin(phase_b)};
  }
  const auto two_tone_fft = backend->execute({length, FftDirection::forward}, two_tone_values);
  require(two_tone_fft, "双音 golden FFT 失败");
  require_close(std::hypot(two_tone_fft.value().bins[32U].real, two_tone_fft.value().bins[32U].imag) / length, 1.0,
                1e-10, "双音主峰幅值错误");
  require_close(std::hypot(two_tone_fft.value().bins[96U].real, two_tone_fft.value().bins[96U].imag) / length, 0.5,
                1e-10, "双音次峰幅值错误");

  constexpr std::size_t chirp_length = 512U;
  std::vector<data::ComplexSample> chirp_values(chirp_length);
  double chirp_phase{};
  for (std::size_t index = 0; index < chirp_length; ++index) {
    const auto bin = 16.0 + 80.0 * static_cast<double>(index) / static_cast<double>(chirp_length - 1U);
    chirp_phase += 2.0 * std::numbers::pi * bin / static_cast<double>(chirp_length);
    chirp_values[index] = {std::cos(chirp_phase), std::sin(chirp_phase)};
  }
  const auto chirp = data::SignalBuffer::from_complex(chirp_values);
  const auto chirp_stft = calculate_stft(
      *backend, chirp.view(), {sample_rate, 0.0, 64U, 32U, WindowKind::hann, SpectrumSidedness::two_sided_shifted});
  require(chirp_stft, "线性调频 golden STFT 失败");
  std::size_t previous_ridge{};
  for (std::size_t row = 0; row < chirp_stft.value().rows; ++row) {
    const auto begin =
        chirp_stft.value().db_per_hz.begin() + static_cast<std::ptrdiff_t>(row * chirp_stft.value().columns);
    const auto ridge = static_cast<std::size_t>(
        std::distance(begin, std::max_element(begin, begin + static_cast<std::ptrdiff_t>(chirp_stft.value().columns))));
    require(row == 0U || ridge >= previous_ridge, "线性调频 STFT 脊线未单调上升");
    previous_ridge = ridge;
  }

  std::uint32_t random_state = 0x13579bdfU;
  std::vector<double> noise_values(length);
  double noise_power{};
  for (auto& value : noise_values) {
    random_state = random_state * 1664525U + 1013904223U;
    value = static_cast<double>((random_state >> 8U) & 0x00ffffffU) / 16777216.0 - 0.5;
    noise_power += value * value;
  }
  noise_power /= static_cast<double>(noise_values.size());
  const auto noise = data::SignalBuffer::from_real(noise_values);
  const auto noise_psd =
      calculate_psd(*backend, noise.view(), {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::one_sided});
  require(noise_psd, "确定性噪声 PSD 失败");
  const auto bin_width = sample_rate / static_cast<double>(length);
  double integrated_noise_power{};
  for (const auto density_db : noise_psd.value().db_per_hz) {
    integrated_noise_power += std::pow(10.0, density_db / 10.0) * bin_width;
  }
  require_close(integrated_noise_power, noise_power, 1e-10, "噪声 PSD 积分与时域功率不一致");

  const auto dc = data::SignalBuffer::from_real(std::vector<double>(length, 0.25));
  const auto dc_spectrum = calculate_spectrum(
      *backend, dc.view(), {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::one_sided});
  require(dc_spectrum, "DC golden 频谱失败");
  require_close(dc_spectrum.value().frequency_hz.front(), 0.0, 1e-12, "DC 峰值频率错误");
  require_close(dc_spectrum.value().magnitude_dbfs.front(), 20.0 * std::log10(0.25), 0.1, "DC 幅值错误");

  auto invalid_values = std::vector<double>(length, 0.0);
  invalid_values[7U] = std::numeric_limits<double>::quiet_NaN();
  auto invalid = data::SignalBuffer::from_real(invalid_values);
  require(!calculate_spectrum(*backend, invalid.view(),
                              {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::one_sided}),
          "NaN 输入必须拒绝");
  invalid_values[7U] = std::numeric_limits<double>::infinity();
  invalid = data::SignalBuffer::from_real(invalid_values);
  require(!calculate_psd(*backend, invalid.view(),
                         {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::one_sided}),
          "Inf 输入必须拒绝");

  std::vector<double> clipped_values(length);
  for (std::size_t index = 0; index < length; ++index) {
    clipped_values[index] = index % 2U == 0U ? 2.0 : -2.0;
  }
  const auto clipped = data::SignalBuffer::from_real(clipped_values);
  const auto clipped_spectrum = calculate_spectrum(
      *backend, clipped.view(), {sample_rate, 0.0, WindowKind::rectangular, SpectrumSidedness::one_sided});
  require(clipped_spectrum && *std::max_element(clipped_spectrum.value().magnitude_dbfs.begin(),
                                                clipped_spectrum.value().magnitude_dbfs.end()) > 5.9,
          "超满量程 clipping 幅值未如实保留");

  const auto input_hash = sha256_complex(input.view().complex_values());
  const auto parameter_hash =
      sha256_doubles(std::array{static_cast<double>(length), sample_rate,
                                static_cast<double>(static_cast<std::uint8_t>(FftDirection::forward)), 0.1});
  const auto output_hash = sha256_complex(tone_fft.value().bins);
  const auto rms_error = std::sqrt(squared_error / static_cast<double>(length));
  const auto passed = maximum_error <= 0.1 && rms_error <= 0.1;
  require(input_hash.size() == 64U && parameter_hash.size() == 64U && output_hash.size() == 64U && passed,
          "golden 摘要或容差判定失败");
  std::cout << "GOLDEN_EVIDENCE {\"reference\":\"analytic-formula/1.0\",\"backend\":\""
            << spectrum.value().provenance.backend_id << "\",\"version\":\"" << spectrum.value().provenance.version
            << "\",\"device\":\"" << spectrum.value().provenance.device << "\",\"input_digest\":\"sha256:" << input_hash
            << "\",\"parameter_digest\":\"sha256:" << parameter_hash << "\",\"output_digest\":\"sha256:" << output_hash
            << "\",\"tolerance\":{\"maximum_absolute_error\":0.1,\"rms_error\":0.1},\"max_error\":" << maximum_error
            << ",\"rms_error\":" << rms_error << ",\"pass\":" << (passed ? "true" : "false")
            << ",\"cases\":[\"tone\",\"two-tone\",\"chirp\",\"noise\",\"dc\",\"nan\",\"inf\",\"clipping\"]}\n";
}

void test_num_004() {
  std::vector<std::byte> bytes(400U);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(index & 0xffU);
  }
  const auto exported = export_bit_exact_bypass(bytes, 4U, data::SampleRange::make(10U, 20U).value());
  require(exported && exported.value() == std::vector<std::byte>(bytes.begin() + 40, bytes.begin() + 80),
          "无处理区间导出不是位精确");
  require(!export_bit_exact_bypass(bytes, 4U, data::SampleRange::make(90U, 110U).value()), "越界导出必须失败");
}

void test_num_005() {
  const auto loaded = data::SampleRange::make(1'000U, 9'000U);
  const auto selection = data::SampleRange::make(2'345U, 6'789U);
  require(loaded && selection && loaded.value().contains(selection.value()), "范围夹具无效");
  const auto begin_time = sample_to_time(selection.value().begin(), 50'000'000.0);
  const auto end_time = sample_to_time(selection.value().end(), 50'000'000.0);
  require(begin_time && end_time, "范围时间映射失败");
  const auto begin_roundtrip = time_to_sample(begin_time.value(), 50'000'000.0, loaded.value().end());
  const auto end_roundtrip = time_to_sample(end_time.value(), 50'000'000.0, loaded.value().end());
  require(begin_roundtrip && end_roundtrip && begin_roundtrip.value() == selection.value().begin() &&
              end_roundtrip.value() == selection.value().end(),
          "视窗、Selection、LoadedDataRange、导出边界往返发生样本漂移");
}

#if defined(SIGNAL_STUDIO_HAVE_EXTERNAL_TEST_DATA)
void validate_external_recordings(IFftBackend& backend) {
  const std::filesystem::path root{SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR};
  const auto wav_path = root / "20241110-174401-662_bw_12800000_sampleTime_0.4_rollOff_0.3.wav";
  const auto wav = data::FileDataSource::open_wav(wav_path, "external-wav", true);
  require(wav, "外部批准 WAV 无法打开");
  const auto wav_range = data::SampleRange::from_count(0U, 4096U);
  require(wav_range, "WAV 范围失败");
  const auto wav_read = wav.value()->read({wav_range.value(), 4096U * 16U, {}});
  require(wav_read && wav_read.value().samples.size() == 4096U, "外部 WAV 有界读取失败");
  const auto wav_psd =
      calculate_psd(backend, wav_read.value().samples.view(),
                    {wav.value()->descriptor().sample_rate_hz, 0.0, WindowKind::hann,
                     wav_read.value().samples.kind() == data::SignalKind::complex ? SpectrumSidedness::two_sided_shifted
                                                                                  : SpectrumSidedness::one_sided});
  require(wav_psd && !wav_psd.value().db_per_hz.empty() &&
              std::ranges::all_of(wav_psd.value().db_per_hz, [](double value) { return std::isfinite(value); }),
          "外部 WAV 的 PSD/频率轴验证失败");

  data::SignalDescriptor raw_descriptor;
  raw_descriptor.signal_kind = data::SignalKind::complex;
  raw_descriptor.scalar_type = data::ScalarType::int16;
  raw_descriptor.component_layout = data::ComponentLayout::interleaved;
  raw_descriptor.component_order = data::ComponentOrder::iq;
  raw_descriptor.endianness = data::Endianness::little;
  raw_descriptor.sample_rate_hz = 50'000'000.0;
  raw_descriptor.center_frequency_hz = 1'245'000'000.0;
  raw_descriptor.requested_sample_range = range(4096U);
  const auto raw = data::FileDataSource::open_raw(root / "x310_capture_cf1245MHz_sr50MSps_20260521_144927.sc16",
                                                  raw_descriptor, "external-sc16");
  require(raw, "外部批准 SC16 无法打开");
  const auto raw_read = raw.value()->read({range(4096U), 4096U * 4U, {}});
  require(raw_read && raw_read.value().samples.size() == 4096U, "外部 SC16 有界读取失败");
  const auto raw_stft = calculate_stft(
      backend, raw_read.value().samples.view(),
      {50'000'000.0, 1'245'000'000.0, 512U, 256U, WindowKind::hann, SpectrumSidedness::two_sided_shifted});
  require(raw_stft && raw_stft.value().rows == 15U && raw_stft.value().columns == 512U &&
              raw_stft.value().frequency_hz.front() < 1'245'000'000.0 &&
              raw_stft.value().frequency_hz.back() > 1'245'000'000.0,
          "外部 SC16 的 FFT/PSD/STFT/频率轴验证失败");
}
#endif

void test_dsp_101() {
  auto cpu = fft_backend();
  const std::array<data::ComplexSample, 4> impulse{{{1.0, 0.0}, {}, {}, {}}};
  const auto plan = cpu->create_plan({4U, FftDirection::forward});
  require(plan && plan.value()->spec().length == 4U, "IFftBackend::create_plan Stable API 失败");
  const auto cpu_result = plan.value()->process(impulse);
  require(cpu_result && cpu_result.value().bins.size() == 4U &&
              std::ranges::all_of(cpu_result.value().bins,
                                  [](const auto& value) {
                                    return std::abs(value.real - 1.0) < 1e-12 && std::abs(value.imag) < 1e-12;
                                  }) &&
              cpu_result.value().provenance.backend_id == "oneMKL-DFTI",
          "IFftBackend 未真实封装 oneMKL DFTI");
  std::cout << "CPU_PROVENANCE version=" << cpu_result.value().provenance.version << '\n';
  const auto kernels = make_auto_signal_kernel_backend();
  require(kernels && kernels.value()->backend_id().find("SignalCompute") != std::string_view::npos &&
              kernels.value()->backend_id().find("oneMKL") != std::string_view::npos &&
              kernels.value()->backend_id().find("libsamplerate") != std::string_view::npos,
          "滤波/重采样未由 SignalCompute 统一选择成熟 oneMKL/libsamplerate 适配器");
  const auto psd_estimator = make_psd_estimator(cpu);
  const auto stft_processor = make_stft_processor(cpu);
  const auto signal = complex_tone(64U, 4U);
  require(psd_estimator && psd_estimator.value()->process(
                               signal.view(), {48'000.0, 0.0, WindowKind::hann, SpectrumSidedness::two_sided_shifted}),
          "IPsdEstimator::process Stable API 失败");
  require(stft_processor && stft_processor.value()->process(signal.view(), {48'000.0, 0.0, 16U, 8U, WindowKind::hann,
                                                                            SpectrumSidedness::two_sided_shifted}),
          "IStftProcessor::process Stable API 失败");

  const auto cuda = make_cuda_fft_backend();
  if (cuda) {
    const auto cuda_result = cuda.value()->execute({4U, FftDirection::forward}, impulse);
    if (cuda_result) {
      require_samples_close(cuda_result.value().bins, cpu_result.value().bins, 1e-10);
      require(cuda_result.value().provenance.version.find("CUDA Runtime") != std::string::npos &&
                  cuda_result.value().provenance.version.find("cuFFT") != std::string::npos,
              "CUDA/cuFFT 版本未由运行时查询记录");
      std::cout << "CUDA_PROVENANCE version=" << cuda_result.value().provenance.version
                << " device=" << cuda_result.value().provenance.device << '\n';
    } else {
      std::string reason;
      const auto automatic = make_auto_fft_backend(true, &reason);
      require(automatic, "cuFFT 实机执行失败后自动后端创建失败");
      const auto fallback = automatic.value()->execute({4U, FftDirection::forward}, impulse);
      require(fallback && fallback.value().provenance.degraded &&
                  fallback.value().provenance.requested == compute::BackendKind::cuda,
              "cuFFT 实机不兼容后未诚实降级 CPU");
    }
  } else {
    std::string reason;
    const auto automatic = make_auto_fft_backend(true, &reason);
    require(automatic && reason.find("降级") != std::string::npos, "CUDA 不可用时未显式降级 CPU");
    const auto fallback = automatic.value()->execute({4U, FftDirection::forward}, impulse);
    require(fallback && fallback.value().provenance.degraded &&
                fallback.value().provenance.requested == compute::BackendKind::cuda,
            "CPU 降级 provenance 未记录");
  }
#if defined(SIGNAL_STUDIO_HAVE_EXTERNAL_TEST_DATA)
  validate_external_recordings(*cpu);
#endif
  require(!cpu->execute({4U, FftDirection::forward}, std::span<const data::ComplexSample>{impulse}.first(3U)),
          "FFT 长度不一致必须失败");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3 || std::string_view(argv[1]) != "--case") {
      throw std::runtime_error("用法：signal_studio_dsp_tests --case <需求编号>");
    }
    const std::map<std::string_view, std::function<void()>> cases{
        {"FR-DSP-001", test_dsp_001},  {"FR-DSP-002", test_dsp_002},  {"FR-DSP-003", test_dsp_003},
        {"FR-DSP-004", test_dsp_004},  {"FR-DSP-005", test_dsp_005},  {"FR-DSP-006", test_dsp_006},
        {"FR-DSP-007", test_dsp_007},  {"FR-DSP-008", test_dsp_008},  {"FR-DSP-009", test_dsp_009},
        {"FR-DSP-010", test_dsp_010},  {"FR-DSP-011", test_dsp_011},  {"FR-DSP-012", test_dsp_012},
        {"NFR-NUM-001", test_num_001}, {"NFR-NUM-002", test_num_002}, {"NFR-NUM-003", test_num_003},
        {"NFR-NUM-004", test_num_004}, {"NFR-NUM-005", test_num_005}, {"FR-DSP-101", test_dsp_101},
    };
    const auto found = cases.find(argv[2]);
    if (found == cases.end()) {
      throw std::runtime_error("未知需求编号");
    }
    found->second();
    std::cout << argv[2] << " PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
