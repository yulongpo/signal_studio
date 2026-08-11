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
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <sstream>
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

class CancellingFftPlan final : public IFftPlan {
public:
  CancellingFftPlan(std::shared_ptr<IFftPlan> delegate, std::shared_ptr<std::atomic_bool> cancellation)
      : delegate_(std::move(delegate)), cancellation_(std::move(cancellation)) {}

  [[nodiscard]] FftSpec spec() const noexcept override {
    return delegate_->spec();
  }

  [[nodiscard]] core::Result<FftResult> process(std::span<const data::ComplexSample> input) override {
    auto result = delegate_->process(input);
    cancellation_->store(true, std::memory_order_release);
    return result;
  }

private:
  std::shared_ptr<IFftPlan> delegate_;
  std::shared_ptr<std::atomic_bool> cancellation_;
};

class CancellingFftBackend final : public IFftBackend {
public:
  CancellingFftBackend(std::shared_ptr<IFftBackend> delegate, std::shared_ptr<std::atomic_bool> cancellation)
      : delegate_(std::move(delegate)), cancellation_(std::move(cancellation)) {}

  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return delegate_->backend_id();
  }

  [[nodiscard]] core::Status validate(const FftSpec& spec) const override {
    return delegate_->validate(spec);
  }

  [[nodiscard]] core::Result<std::shared_ptr<IFftPlan>> create_plan(const FftSpec& spec) override {
    auto plan = delegate_->create_plan(spec);
    if (!plan) {
      return plan.error();
    }
    return std::shared_ptr<IFftPlan>(std::make_shared<CancellingFftPlan>(plan.value(), cancellation_));
  }

private:
  std::shared_ptr<IFftBackend> delegate_;
  std::shared_ptr<std::atomic_bool> cancellation_;
};

struct FftCallCounts final {
  std::uint64_t plans{};
  std::uint64_t executions{};
};

class CountingFftPlan final : public IFftPlan {
public:
  CountingFftPlan(std::shared_ptr<IFftPlan> delegate, std::shared_ptr<FftCallCounts> counts)
      : delegate_(std::move(delegate)), counts_(std::move(counts)) {}

  [[nodiscard]] FftSpec spec() const noexcept override {
    return delegate_->spec();
  }

  [[nodiscard]] core::Result<FftResult> process(std::span<const data::ComplexSample> input) override {
    ++counts_->executions;
    return delegate_->process(input);
  }

private:
  std::shared_ptr<IFftPlan> delegate_;
  std::shared_ptr<FftCallCounts> counts_;
};

class CountingFftBackend final : public IFftBackend {
public:
  CountingFftBackend(std::shared_ptr<IFftBackend> delegate, std::shared_ptr<FftCallCounts> counts)
      : delegate_(std::move(delegate)), counts_(std::move(counts)) {}

  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return delegate_->backend_id();
  }

  [[nodiscard]] core::Status validate(const FftSpec& spec) const override {
    return delegate_->validate(spec);
  }

  [[nodiscard]] core::Result<std::shared_ptr<IFftPlan>> create_plan(const FftSpec& spec) override {
    auto plan = delegate_->create_plan(spec);
    if (!plan) {
      return plan.error();
    }
    ++counts_->plans;
    return std::shared_ptr<IFftPlan>(std::make_shared<CountingFftPlan>(plan.value(), counts_));
  }

private:
  std::shared_ptr<IFftBackend> delegate_;
  std::shared_ptr<FftCallCounts> counts_;
};

enum class ProvenanceMutation : std::uint8_t { backend, device, precision, fallback };

struct ProvenanceSwitchState final {
  std::uint64_t executions{};
  std::uint64_t switch_execution{};
  ProvenanceMutation mutation{ProvenanceMutation::backend};
};

class SwitchingProvenanceFftPlan final : public IFftPlan {
public:
  SwitchingProvenanceFftPlan(std::shared_ptr<IFftPlan> delegate, std::shared_ptr<ProvenanceSwitchState> state)
      : delegate_(std::move(delegate)), state_(std::move(state)) {}

  [[nodiscard]] FftSpec spec() const noexcept override {
    return delegate_->spec();
  }

  [[nodiscard]] core::Result<FftResult> process(std::span<const data::ComplexSample> input) override {
    auto result = delegate_->process(input);
    if (!result) {
      return result.error();
    }
    ++state_->executions;
    if (state_->executions >= state_->switch_execution) {
      switch (state_->mutation) {
      case ProvenanceMutation::backend:
        result.value().provenance.actual = compute::BackendKind::cuda;
        result.value().provenance.backend_id = "test-switched-backend";
        break;
      case ProvenanceMutation::device:
        result.value().provenance.device = "test-switched-device";
        break;
      case ProvenanceMutation::precision:
        result.value().provenance.precision = "complex-float32";
        break;
      case ProvenanceMutation::fallback:
        result.value().provenance.requested = compute::BackendKind::cuda;
        result.value().provenance.degraded = true;
        result.value().provenance.reason = "test deterministic fallback";
        break;
      }
    }
    return result;
  }

private:
  std::shared_ptr<IFftPlan> delegate_;
  std::shared_ptr<ProvenanceSwitchState> state_;
};

class SwitchingProvenanceFftBackend final : public IFftBackend {
public:
  SwitchingProvenanceFftBackend(std::shared_ptr<IFftBackend> delegate, std::shared_ptr<ProvenanceSwitchState> state)
      : delegate_(std::move(delegate)), state_(std::move(state)) {}

  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return delegate_->backend_id();
  }

  [[nodiscard]] core::Status validate(const FftSpec& spec) const override {
    return delegate_->validate(spec);
  }

  [[nodiscard]] core::Result<std::shared_ptr<IFftPlan>> create_plan(const FftSpec& spec) override {
    auto plan = delegate_->create_plan(spec);
    if (!plan) {
      return plan.error();
    }
    return std::shared_ptr<IFftPlan>(std::make_shared<SwitchingProvenanceFftPlan>(plan.value(), state_));
  }

private:
  std::shared_ptr<IFftBackend> delegate_;
  std::shared_ptr<ProvenanceSwitchState> state_;
};

std::size_t peak_index(std::span<const double> values) {
  return static_cast<std::size_t>(std::distance(values.begin(), std::max_element(values.begin(), values.end())));
}

std::size_t frequency_index(std::span<const double> frequencies, double expected_hz) {
  return static_cast<std::size_t>(
      std::distance(frequencies.begin(),
                    std::min_element(frequencies.begin(), frequencies.end(), [expected_hz](double left, double right) {
                      return std::abs(left - expected_hz) < std::abs(right - expected_hz);
                    })));
}

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

void test_ms45_contracts() {
  const auto catalog = window_catalog();
  require(catalog.size() == 8U, "MS-4.5 窗函数目录必须完整包含八种窗");
  std::array<bool, 8U> seen{};
  for (const auto& descriptor : catalog) {
    const auto index = static_cast<std::size_t>(descriptor.kind);
    require(index < seen.size() && !seen[index], "窗函数目录 kind 重复或越界");
    seen[index] = true;
    require(!descriptor.english_name.empty() && !descriptor.chinese_name.empty() &&
                !descriptor.recommended_use.empty() && !descriptor.amplitude_characteristics.empty() &&
                !descriptor.leakage_characteristics.empty() && descriptor.reference_coherent_gain > 0.0 &&
                descriptor.reference_enbw_bins > 0.0 &&
                descriptor.parameter_minimum <= descriptor.recommended_parameter &&
                descriptor.recommended_parameter <= descriptor.parameter_maximum,
            "窗函数目录缺少名称、参数、CG、ENBW、用途或幅度/泄漏元数据");
    if (descriptor.kind == WindowKind::kaiser || descriptor.kind == WindowKind::tukey) {
      require(!descriptor.parameter_name.empty() && descriptor.parameter_maximum > descriptor.parameter_minimum,
              "参数化窗缺少有效参数范围");
    } else {
      require(descriptor.parameter_name.empty() && descriptor.parameter_minimum == 0.0 &&
                  descriptor.parameter_maximum == 0.0,
              "无参数窗错误声明了参数范围");
    }
  }
  const auto rectangular = make_window(WindowSpecification{WindowKind::rectangular, 0.0}, 5U);
  const auto hann = make_window(WindowSpecification{WindowKind::hann, 0.0}, 5U);
  const auto blackman_harris = make_window(WindowSpecification{WindowKind::blackman_harris, 0.0}, 5U);
  const auto flat_top = make_window(WindowSpecification{WindowKind::flat_top, 0.0}, 5U);
  const auto kaiser_zero = make_window(WindowSpecification{WindowKind::kaiser, 0.0}, 5U);
  const auto kaiser = make_window(WindowSpecification{WindowKind::kaiser, 8.6}, 5U);
  const auto tukey_zero = make_window(WindowSpecification{WindowKind::tukey, 0.0}, 5U);
  const auto tukey_one = make_window(WindowSpecification{WindowKind::tukey, 1.0}, 5U);
  require(rectangular && hann && blackman_harris && flat_top && kaiser_zero && kaiser && tukey_zero && tukey_one,
          "MS-4.5 窗函数创建失败");
  for (const auto coefficient : rectangular.value().coefficients) {
    require_close(coefficient, 1.0, 1e-15, "Rectangular 系数错误");
  }
  require_close(rectangular.value().coherent_gain, 1.0, 1e-15, "Rectangular coherent gain 错误");
  require_close(rectangular.value().equivalent_noise_bandwidth_bins, 1.0, 1e-15, "Rectangular ENBW 错误");
  const std::array<double, 5U> expected_hann{0.0, 0.5, 1.0, 0.5, 0.0};
  for (std::size_t index = 0; index < expected_hann.size(); ++index) {
    require_close(hann.value().coefficients[index], expected_hann[index], 1e-14, "Hann 系数错误");
    require_close(tukey_one.value().coefficients[index], expected_hann[index], 1e-14, "Tukey alpha=1 未退化为 Hann");
  }
  require_close(hann.value().coherent_gain, 0.4, 1e-14, "Hann coherent gain 错误");
  require_close(hann.value().equivalent_noise_bandwidth_bins, 1.875, 1e-14, "Hann ENBW 错误");
  require_close(blackman_harris.value().coefficients.front(), 0.00006, 1e-12, "Blackman-Harris 端点系数错误");
  require_close(blackman_harris.value().coefficients[2U], 1.0, 1e-12, "Blackman-Harris 中心系数错误");
  require_close(flat_top.value().coefficients[2U], 1.0, 5e-9, "Flat Top 中心系数错误");
  const auto flat_top_descriptor = std::ranges::find(catalog, WindowKind::flat_top, &WindowDescriptor::kind);
  const auto long_flat_top = make_window(WindowSpecification{WindowKind::flat_top, 0.0}, 4096U);
  require(flat_top_descriptor != catalog.end() && long_flat_top, "Flat Top 目录或长窗参考创建失败");
  require_close(flat_top_descriptor->reference_coherent_gain, long_flat_top.value().coherent_gain, 1e-3,
                "Flat Top 目录 coherent gain 与实现不一致");
  for (const auto coefficient : kaiser_zero.value().coefficients) {
    require_close(coefficient, 1.0, 1e-14, "Kaiser beta=0 未退化为 Rectangular");
  }
  require(kaiser.value().coefficients.front() < 0.002 && kaiser.value().coefficients[2U] > 0.999999,
          "Kaiser beta 未真实改变窗系数");
  for (const auto coefficient : tukey_zero.value().coefficients) {
    require_close(coefficient, 1.0, 1e-14, "Tukey alpha=0 未退化为 Rectangular");
  }
  require(!make_window(WindowSpecification{WindowKind::kaiser, -0.1}, 16U), "负 Kaiser beta 必须拒绝");
  require(!make_window(WindowSpecification{WindowKind::tukey, 1.1}, 16U), "超范围 Tukey alpha 必须拒绝");

  AnalysisSettingsSnapshot settings;
  settings.algorithm_version = "signal.dsp.analysis/1.0";
  settings.spectrum.analysis_range_policy = AnalysisRangePolicy::all_complete_frames;
  settings.spectrum.frame_length = 256U;
  settings.spectrum.fft_length = 512U;
  settings.spectrum.zero_padding_policy = ZeroPaddingPolicy::enabled;
  settings.spectrum.window = {WindowKind::kaiser, 8.6};
  settings.spectrum.sidedness = SpectrumSidedness::two_sided_shifted;
  settings.spectrum.output_quantity = SpectrumOutputQuantity::linear_power_density;
  settings.spectrum.normalization = SpectrumNormalization::window_power;
  settings.spectrum.estimator = {PsdEstimatorKind::welch, 0.5, 4U};
  settings.spectrum.accumulation = {SpectrumAccumulationMode::exponential_average, 0U, 0.25, 0U};
  settings.spectrum.smoothing = {SpectrumSmoothingKind::savitzky_golay, 7U, 0.0, 3U};
  settings.spectrum.measurement_source = MeasurementSource::raw;
  settings.spectrogram.frame_length = 128U;
  settings.spectrogram.fft_length = 256U;
  settings.spectrogram.hop_length = 64U;
  settings.spectrogram.padding_policy = ZeroPaddingPolicy::enabled;
  settings.spectrogram.window = {WindowKind::tukey, 0.4};
  settings.spectrogram.smoothing = {SpectrogramFrequencySmoothingKind::gaussian, 5U, 1.0,
                                    SpectrogramTimeSmoothingKind::exponential, 0.4};
  settings.prefilter.enabled = true;
  settings.prefilter.boundary = BoundaryPolicy::zero_pad;
  auto prefilter = node("analysis-lowpass", NodeKind::fir_filter);
  prefilter.filter_shape = FilterShape::custom;
  prefilter.numerator = {0.25, 0.5, 0.25};
  prefilter.implementation_id = "signal.dsp.onemkl/1.0";
  settings.prefilter.chain.nodes.push_back(prefilter);

  const auto serialized = serialize_analysis_settings(settings);
  require(serialized, "MS-4.5 参数规范化序列化失败");
  const auto parsed = parse_analysis_settings(serialized.value());
  require(parsed, "MS-4.5 参数解析失败");
  const auto reserialized = serialize_analysis_settings(parsed.value());
  require(reserialized && reserialized.value() == serialized.value(), "MS-4.5 参数序列化未确定性往返");
  const auto hash = hash_analysis_settings(settings);
  const auto restored_hash = hash_analysis_settings(parsed.value());
  require(hash && restored_hash && hash.value() == restored_hash.value() && hash.value().hex.size() == 64U,
          "MS-4.5 参数哈希不稳定或不是 SHA-256");
  auto smoothing_changed = settings;
  smoothing_changed.spectrum.smoothing.polynomial_order = 2U;
  const auto smoothing_hash = hash_analysis_settings(smoothing_changed);
  require(smoothing_hash && smoothing_hash.value() != hash.value(), "数值参数变化未进入参数哈希");
  auto display_axis_changed = settings;
  display_axis_changed.spectrum.frequency_reference = data::FrequencyReference::absolute;
  const auto display_axis_hash = hash_analysis_settings(display_axis_changed);
  require(display_axis_hash && display_axis_hash.value() != hash.value(), "公共 DSP 频率参考变化未进入参数哈希");
  auto future_minor = serialized.value();
  future_minor += "future.optional-field=ignored\n";
  require(parse_analysis_settings(future_minor), "同主版本新增字段未安全忽略");
  auto incompatible = serialized.value();
  const auto schema_position = incompatible.find("signal.analysis-settings/1.0");
  require(schema_position != std::string::npos, "MS-4.5 schema 夹具缺失");
  incompatible.replace(schema_position, std::string_view("signal.analysis-settings/1.0").size(),
                       "signal.analysis-settings/2.0");
  require(!parse_analysis_settings(incompatible), "未来主版本必须明确拒绝");
  const auto corrupt_field = [](std::string text, std::string_view key, std::string_view value) {
    const auto begin = text.find(key);
    require(begin != std::string::npos, "参数损坏夹具字段缺失");
    const auto value_begin = begin + key.size();
    const auto end = text.find('\n', value_begin);
    text.replace(value_begin, end - value_begin, value);
    return text;
  };
  require(!parse_analysis_settings(corrupt_field(serialized.value(), "spectrum.normalization=", "255")),
          "同主版本损坏的归一化枚举未拒绝");
  require(!parse_analysis_settings(corrupt_field(serialized.value(), "spectrum.fft_length=", "128")),
          "同主版本 FFT 长度小于帧长未拒绝");
  require(!parse_analysis_settings(corrupt_field(serialized.value(), "prefilter.boundary=", "255")),
          "同主版本损坏的预滤波边界枚举未拒绝");

  const auto estimate = estimate_analysis_cost(settings, 4096U, 48'000.0);
  require(estimate && estimate.value().spectrum_output_bins == 512U && estimate.value().spectrogram_rows == 63U &&
              estimate.value().spectrogram_columns == 256U && estimate.value().fft_execution_count >= 67U &&
              estimate.value().host_memory_bytes > 0U && estimate.value().estimated_operations > 0.0,
          "MS-4.5 资源估计未反映 FFT/Welch/STFT 参数");
  require_close(estimate.value().spectrum_bin_spacing_hz, 93.75, 1e-12, "资源估计频点间隔错误");
  require_close(estimate.value().spectrogram_time_step_seconds, 64.0 / 48'000.0, 1e-15, "资源估计 STFT 时间步长错误");

  auto changed = settings;
  changed.spectrum.smoothing.polynomial_order = 2U;
  auto invalidation = classify_analysis_change(settings, changed);
  require(has_invalidation(invalidation, AnalysisInvalidation::spectrum_smoothing) &&
              !has_invalidation(invalidation, AnalysisInvalidation::spectrum_transform),
          "仅频谱平滑变化未执行最小失效");
  changed = settings;
  changed.spectrum.fft_length = 1024U;
  invalidation = classify_analysis_change(settings, changed);
  require(has_invalidation(invalidation, AnalysisInvalidation::spectrum_transform) &&
              !has_invalidation(invalidation, AnalysisInvalidation::spectrogram_transform),
          "频谱 FFT 变化错误失效 STFT");
  changed = settings;
  changed.spectrogram.smoothing.time_exponential_alpha = 0.6;
  invalidation = classify_analysis_change(settings, changed);
  require(has_invalidation(invalidation, AnalysisInvalidation::spectrogram_smoothing) &&
              !has_invalidation(invalidation, AnalysisInvalidation::spectrogram_transform),
          "仅 STFT 平滑变化未执行最小失效");
  changed = settings;
  changed.spectrum.measurement_source = MeasurementSource::smoothed;
  invalidation = classify_analysis_change(settings, changed);
  require(invalidation == AnalysisInvalidation::none, "仅测量来源变化错误重算 FFT");
  changed = settings;
  changed.spectrum.frequency_reference = data::FrequencyReference::absolute;
  invalidation = classify_analysis_change(settings, changed);
  require(invalidation == AnalysisInvalidation::spectrum_transform, "公共 DSP 频率参考变化未失效频谱变换");
  changed = settings;
  changed.prefilter.chain.nodes.front().numerator.front() = 0.2;
  invalidation = classify_analysis_change(settings, changed);
  require(has_invalidation(invalidation, AnalysisInvalidation::prefilter) &&
              has_invalidation(invalidation, AnalysisInvalidation::spectrum_transform) &&
              has_invalidation(invalidation, AnalysisInvalidation::spectrogram_transform),
          "预滤波变化未失效全部下游分析");
}

void test_ms45_spectrum_psd() {
  auto backend = fft_backend();
  constexpr double sample_rate = 48'000.0;
  const auto tone = complex_tone(512U, 32U);
  SpectrumAnalysisSettings spectrum_settings;
  spectrum_settings.frame_length = 256U;
  spectrum_settings.fft_length = 512U;
  spectrum_settings.zero_padding_policy = ZeroPaddingPolicy::enabled;
  spectrum_settings.window = {WindowKind::rectangular, 0.0};
  spectrum_settings.sidedness = SpectrumSidedness::two_sided_shifted;
  spectrum_settings.output_quantity = SpectrumOutputQuantity::magnitude_dbfs;
  const auto spectrum = calculate_spectrum(*backend, tone.view(), sample_rate, 0.0, spectrum_settings, nullptr);
  require(spectrum && spectrum.value().frequency_hz.size() == 512U && spectrum.value().raw_values.size() == 512U &&
              spectrum.value().values.size() == 512U && spectrum.value().frame_length == 256U &&
              spectrum.value().fft_length == 512U,
          "参数化频谱未真实使用帧长和 FFT 长度");
  const auto spectrum_peak = peak_index(spectrum.value().values);
  require_close(spectrum.value().frequency_hz[spectrum_peak], 3'000.0, 1e-12, "补零频谱峰值频率错误");
  require_close(spectrum.value().values[spectrum_peak], 0.0, 1e-10, "补零频谱 coherent gain 幅值错误");
  require_close(spectrum.value().bin_spacing_hz, sample_rate / 512.0, 1e-15, "补零频点间隔错误");

  auto no_padding = spectrum_settings;
  no_padding.fft_length = 256U;
  no_padding.zero_padding_policy = ZeroPaddingPolicy::forbidden;
  const auto compact = calculate_spectrum(*backend, tone.view(), sample_rate, 0.0, no_padding, nullptr);
  require(compact && compact.value().frequency_hz.size() == 256U &&
              compact.value().bin_spacing_hz == sample_rate / 256.0,
          "FFT 长度变化未改变 bin 数和频点间隔");
  auto invalid_padding = spectrum_settings;
  invalid_padding.zero_padding_policy = ZeroPaddingPolicy::forbidden;
  require(!calculate_spectrum(*backend, tone.view(), sample_rate, 0.0, invalid_padding, nullptr),
          "禁止补零时仍接受 FFT 长度大于帧长");

  const auto real = real_tone(256U, 16U, 0.5);
  SpectrumAnalysisSettings one_sided_amplitude;
  one_sided_amplitude.frame_length = 256U;
  one_sided_amplitude.fft_length = 256U;
  one_sided_amplitude.window = {WindowKind::rectangular, 0.0};
  one_sided_amplitude.sidedness = SpectrumSidedness::one_sided;
  one_sided_amplitude.output_quantity = SpectrumOutputQuantity::linear_amplitude;
  one_sided_amplitude.normalization = SpectrumNormalization::coherent_gain;
  auto invalid_amplitude_normalization = one_sided_amplitude;
  invalid_amplitude_normalization.normalization = SpectrumNormalization::window_power;
  require(!calculate_spectrum(*backend, real.view(), sample_rate, 0.0, invalid_amplitude_normalization, nullptr),
          "幅度/dBFS 错误接受 Window-power 归一化");
  const auto amplitude_result =
      calculate_spectrum(*backend, real.view(), sample_rate, 0.0, one_sided_amplitude, nullptr);
  auto absolute_amplitude = one_sided_amplitude;
  absolute_amplitude.frequency_reference = data::FrequencyReference::absolute;
  constexpr double center_frequency = 10.0e6;
  const auto absolute_amplitude_result =
      calculate_spectrum(*backend, real.view(), sample_rate, center_frequency, absolute_amplitude, nullptr);
  auto unnormalized_amplitude = one_sided_amplitude;
  unnormalized_amplitude.normalization = SpectrumNormalization::none;
  const auto unnormalized_amplitude_result =
      calculate_spectrum(*backend, real.view(), sample_rate, 0.0, unnormalized_amplitude, nullptr);
  auto one_sided_power = one_sided_amplitude;
  one_sided_power.output_quantity = SpectrumOutputQuantity::linear_power;
  const auto power_result = calculate_spectrum(*backend, real.view(), sample_rate, 0.0, one_sided_power, nullptr);
  require(amplitude_result && power_result && amplitude_result.value().values.size() == 129U &&
              power_result.value().values.size() == amplitude_result.value().values.size(),
          "实信号单边频谱尺寸错误");
  const auto real_peak = frequency_index(amplitude_result.value().frequency_hz, 3'000.0);
  require_close(amplitude_result.value().values[real_peak], 0.5 / std::sqrt(2.0), 1e-12, "单边 RMS 幅度缩放错误");
  require(absolute_amplitude_result && absolute_amplitude_result.value().values == amplitude_result.value().values,
          "公共 DSP absolute 频率参考错误改变了频谱数值");
  require_close(absolute_amplitude_result.value().frequency_hz[real_peak], center_frequency + 3'000.0, 1e-9,
                "公共 DSP absolute 频率参考未加入中心频率");
  require(unnormalized_amplitude_result, "幅度输出未接受 None 归一化");
  require(unnormalized_amplitude_result.value().normalization == SpectrumNormalization::none,
          "频谱结果未携带实际 None 归一化语义");
  require_close(unnormalized_amplitude_result.value().values[real_peak],
                amplitude_result.value().values[real_peak] * 256.0, 1e-9, "None 归一化未真实保留原始 FFT 幅度尺度");
  auto calibrated_log = one_sided_amplitude;
  calibrated_log.output_quantity = SpectrumOutputQuantity::magnitude_dbfs;
  auto raw_log = calibrated_log;
  raw_log.normalization = SpectrumNormalization::none;
  const auto calibrated_log_result =
      calculate_spectrum(*backend, real.view(), sample_rate, 0.0, calibrated_log, nullptr);
  const auto raw_log_result = calculate_spectrum(*backend, real.view(), sample_rate, 0.0, raw_log, nullptr);
  require(calibrated_log_result && raw_log_result, "None 对数频谱验证未产生结果");
  require_close(raw_log_result.value().values[real_peak] - calibrated_log_result.value().values[real_peak],
                20.0 * std::log10(256.0), 1e-9, "None 对数频谱未保留未归一化 FFT 尺度");
  require(spectrum_output_unit(SpectrumOutputQuantity::magnitude_dbfs, SpectrumNormalization::coherent_gain) ==
                  "dBFS" &&
              spectrum_output_unit(SpectrumOutputQuantity::magnitude_dbfs, SpectrumNormalization::none) ==
                  "dB(re 1 raw FFT amplitude unit)" &&
              spectrum_output_unit(SpectrumOutputQuantity::power_dbfs, SpectrumNormalization::none) ==
                  "dB(re 1 raw FFT power unit)" &&
              spectrum_output_unit(SpectrumOutputQuantity::linear_amplitude, SpectrumNormalization::none) ==
                  "raw FFT amplitude unit" &&
              spectrum_output_unit(SpectrumOutputQuantity::linear_power, SpectrumNormalization::none) ==
                  "raw FFT power unit",
          "None 的线性/对数谱单位仍伪装为 full-scale 校准单位");
  require_close(power_result.value().values[real_peak], 0.125, 1e-12, "单边 Parseval 功率缩放错误");
  require_close(power_result.value().values[real_peak],
                amplitude_result.value().values[real_peak] * amplitude_result.value().values[real_peak], 1e-14,
                "线性功率不等于线性幅度平方");

  std::vector<data::ComplexSample> piecewise(512U);
  for (std::size_t index = 0; index < piecewise.size(); ++index) {
    const auto local = index % 256U;
    const auto amplitude = index < 256U ? 1.0 : 0.5;
    const auto phase = 2.0 * std::numbers::pi * 16.0 * static_cast<double>(local) / 256.0;
    piecewise[index] = {amplitude * std::cos(phase), amplitude * std::sin(phase)};
  }
  const auto input = data::SignalBuffer::from_complex(std::move(piecewise));
  SpectrumAnalysisSettings psd_settings;
  psd_settings.analysis_range_policy = AnalysisRangePolicy::all_complete_frames;
  psd_settings.frame_length = 256U;
  psd_settings.fft_length = 256U;
  psd_settings.window = {WindowKind::rectangular, 0.0};
  psd_settings.sidedness = SpectrumSidedness::two_sided_shifted;
  psd_settings.output_quantity = SpectrumOutputQuantity::linear_power_density;
  psd_settings.normalization = SpectrumNormalization::window_power;
  psd_settings.estimator = {PsdEstimatorKind::welch, 0.0, 2U};
  auto invalid_density_normalization = psd_settings;
  invalid_density_normalization.normalization = SpectrumNormalization::coherent_gain;
  require(!calculate_psd(*backend, input.view(), sample_rate, 0.0, invalid_density_normalization, nullptr),
          "功率密度错误接受 coherent-gain 归一化");
  const auto call_counts = std::make_shared<FftCallCounts>();
  CountingFftBackend counting_backend{backend, call_counts};
  const auto shared = calculate_spectrum_psd(counting_backend, input.view(), sample_rate, 0.0, psd_settings, nullptr);
  require(shared && call_counts->plans == 1U && call_counts->executions == 2U &&
              shared.value().spectrum.provenance.backend_id == shared.value().psd.provenance.backend_id &&
              shared.value().spectrum.raw_density_linear == shared.value().psd.raw_density_linear,
          "Spectrum/PSD 未共享同一 FFT 计划和帧变换");
  auto unnormalized_density = psd_settings;
  unnormalized_density.normalization = SpectrumNormalization::none;
  const auto unnormalized_density_result =
      calculate_psd(*backend, input.view(), sample_rate, 0.0, unnormalized_density, nullptr);
  require(unnormalized_density_result, "PSD 输出未接受 None 归一化");
  require(unnormalized_density_result.value().normalization == SpectrumNormalization::none,
          "PSD 结果未携带实际 None 归一化语义");
  const auto density_peak = frequency_index(shared.value().psd.frequency_hz, 3'000.0);
  require_close(unnormalized_density_result.value().raw_density_linear[density_peak],
                shared.value().psd.raw_density_linear[density_peak] * 256.0, 1e-9,
                "None 归一化未真实保留原始 FFT 功率密度尺度");
  require(spectrum_output_unit(SpectrumOutputQuantity::psd_dbfs_per_hz, SpectrumNormalization::window_power) ==
                  "dBFS/Hz" &&
              spectrum_output_unit(SpectrumOutputQuantity::psd_dbfs_per_hz, SpectrumNormalization::none) ==
                  "dB(re 1 raw FFT power unit/Hz)" &&
              spectrum_output_unit(SpectrumOutputQuantity::linear_power_density, SpectrumNormalization::none) ==
                  "raw FFT power unit/Hz",
          "None 的 PSD/线性功率密度单位仍伪装为校准单位");

  const auto expected_scale = 256.0 / sample_rate;
  const auto peak_frequency = 3'000.0;
  const auto verify_accumulation = [&](SpectrumAccumulationMode mode, double alpha, double expected_factor) {
    auto configured = psd_settings;
    configured.accumulation = {mode, 2U, alpha, 0U};
    const auto result = calculate_psd(*backend, input.view(), sample_rate, 0.0, configured, nullptr);
    require(result && result.value().segment_count == 2U && result.value().raw_values == result.value().values,
            "Welch 累积返回值或 raw 保留错误");
    const auto bin = frequency_index(result.value().frequency_hz, peak_frequency);
    require_close(result.value().values[bin], expected_factor * expected_scale, 1e-12,
                  "Welch/平均/保持的解析功率密度错误");
  };
  verify_accumulation(SpectrumAccumulationMode::none, 1.0, 0.625);
  verify_accumulation(SpectrumAccumulationMode::linear_average, 1.0, 0.625);
  verify_accumulation(SpectrumAccumulationMode::exponential_average, 0.25, 0.8125);
  verify_accumulation(SpectrumAccumulationMode::maximum_hold, 1.0, 1.0);

  auto periodogram = psd_settings;
  periodogram.analysis_range_policy = AnalysisRangePolicy::first_frame;
  periodogram.estimator = {PsdEstimatorKind::periodogram, 0.0, 1U};
  periodogram.accumulation = {};
  const auto periodogram_result = calculate_psd(*backend, input.view(), sample_rate, 0.0, periodogram, nullptr);
  require(periodogram_result && periodogram_result.value().segment_count == 1U, "Periodogram 未使用单段");
  const auto periodogram_bin = frequency_index(periodogram_result.value().frequency_hz, peak_frequency);
  require_close(periodogram_result.value().values[periodogram_bin], expected_scale, 1e-12,
                "Periodogram 解析功率密度错误");

  const std::array<double, 7U> impulse{0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0};
  const auto moving = smooth_spectrum(impulse, {SpectrumSmoothingKind::moving_average, 3U, 0.0, 0U}, nullptr);
  require(moving, "滑动平均失败");
  const std::array<double, 7U> expected_moving{0.0, 0.0, 1.0, 1.0, 1.0, 0.0, 0.0};
  for (std::size_t index = 0; index < expected_moving.size(); ++index) {
    require_close(moving.value()[index], expected_moving[index], 1e-14, "滑动平均数值错误");
  }
  const auto gaussian = smooth_spectrum(impulse, {SpectrumSmoothingKind::gaussian, 5U, 1.0, 0U}, nullptr);
  require(gaussian && gaussian.value()[3U] < 3.0 && gaussian.value()[3U] > gaussian.value()[2U] &&
              gaussian.value()[2U] > gaussian.value()[1U] && gaussian.value()[1U] > 0.0,
          "高斯平滑核形状错误");
  std::array<double, 9U> quadratic{};
  for (std::size_t index = 0; index < quadratic.size(); ++index) {
    const auto x = static_cast<double>(index) - 4.0;
    quadratic[index] = 1.0 + 2.0 * x + 0.5 * x * x;
  }
  const auto savitzky = smooth_spectrum(quadratic, {SpectrumSmoothingKind::savitzky_golay, 5U, 0.0, 2U}, nullptr);
  require(savitzky, "Savitzky-Golay 平滑失败");
  for (std::size_t index = 2U; index + 2U < quadratic.size(); ++index) {
    require_close(savitzky.value()[index], quadratic[index], 1e-10, "Savitzky-Golay 未保持二次多项式");
  }

  auto smoothed_psd_settings = periodogram;
  smoothed_psd_settings.smoothing = {SpectrumSmoothingKind::moving_average, 3U, 0.0, 0U};
  const auto smoothed_psd = calculate_psd(*backend, input.view(), sample_rate, 0.0, smoothed_psd_settings, nullptr);
  require(smoothed_psd && smoothed_psd.value().raw_values != smoothed_psd.value().values &&
              smoothed_psd.value().raw_values[periodogram_bin] > smoothed_psd.value().values[periodogram_bin],
          "频谱平滑未影响显示值或未保留 raw");
  const auto reused_psd = resmooth_psd(periodogram_result.value(), smoothed_psd_settings, nullptr);
  require(reused_psd && reused_psd.value().raw_linear_values == periodogram_result.value().raw_linear_values &&
              reused_psd.value().raw_values == periodogram_result.value().raw_values,
          "平滑最小失效未复用未平滑 PSD 线性结果");
  for (std::size_t index = 0; index < smoothed_psd.value().values.size(); ++index) {
    require_close(reused_psd.value().values[index], smoothed_psd.value().values[index], 1e-15,
                  "复用 raw 的 PSD 平滑与完整重算不一致");
    require_close(reused_psd.value().db_per_hz[index], smoothed_psd.value().db_per_hz[index], 1e-12,
                  "复用 raw 的 PSD 密度平滑与完整重算不一致");
  }

  auto moving_spectrum_settings = spectrum_settings;
  moving_spectrum_settings.smoothing = {SpectrumSmoothingKind::moving_average, 3U, 0.0, 0U};
  const auto full_smoothed_spectrum =
      calculate_spectrum(*backend, tone.view(), sample_rate, 0.0, moving_spectrum_settings, nullptr);
  const auto reused_spectrum = resmooth_spectrum(spectrum.value(), moving_spectrum_settings, nullptr);
  require(full_smoothed_spectrum && reused_spectrum &&
              reused_spectrum.value().raw_linear_values == spectrum.value().raw_linear_values,
          "平滑最小失效未保留频谱 FFT 的线性 raw");
  for (std::size_t index = 0; index < full_smoothed_spectrum.value().values.size(); ++index) {
    require_close(reused_spectrum.value().values[index], full_smoothed_spectrum.value().values[index], 1e-15,
                  "复用 raw 的频谱平滑与完整重算不一致");
  }

  const auto cancellation = std::make_shared<std::atomic_bool>(true);
  require(!calculate_spectrum(*backend, tone.view(), sample_rate, 0.0, spectrum_settings, cancellation),
          "开始前取消仍发布频谱");
  cancellation->store(false, std::memory_order_release);
  CancellingFftBackend cancelling_backend(backend, cancellation);
  const auto cancelled =
      calculate_spectrum(cancelling_backend, tone.view(), sample_rate, 0.0, spectrum_settings, cancellation);
  require(!cancelled && cancelled.error().code().reason == core::ErrorReason::cancelled,
          "FFT 内核完成后取消仍发布结果");
}

void test_ms45_stft_prefilter() {
  auto fft = fft_backend();
  auto kernels = kernel_backend();
  constexpr double sample_rate = 48'000.0;
  constexpr std::size_t frame_length = 64U;
  std::vector<data::ComplexSample> values(frame_length * 3U);
  const std::array<double, 3U> amplitudes{1.0, 0.5, 0.25};
  for (std::size_t frame = 0; frame < amplitudes.size(); ++frame) {
    for (std::size_t index = 0; index < frame_length; ++index) {
      const auto phase = 2.0 * std::numbers::pi * 8.0 * static_cast<double>(index) / static_cast<double>(frame_length);
      values[frame * frame_length + index] = {amplitudes[frame] * std::cos(phase), amplitudes[frame] * std::sin(phase)};
    }
  }
  const auto input = data::SignalBuffer::from_complex(std::move(values));
  SpectrogramAnalysisSettings settings;
  settings.frame_length = frame_length;
  settings.fft_length = frame_length;
  settings.hop_length = frame_length;
  settings.window = {WindowKind::rectangular, 0.0};
  settings.sidedness = SpectrumSidedness::two_sided_shifted;
  settings.output_quantity = SpectrumOutputQuantity::linear_power_density;
  settings.normalization = SpectrumNormalization::window_power;
  settings.smoothing.time_mode = SpectrogramTimeSmoothingKind::exponential;
  settings.smoothing.time_exponential_alpha = 0.5;
  const auto stft = calculate_stft(*fft, input.view(), sample_rate, 0.0, settings, nullptr);
  require(stft && stft.value().rows == 3U && stft.value().columns == frame_length &&
              stft.value().raw_values.size() == frame_length * 3U && stft.value().values.size() == frame_length * 3U,
          "参数化 STFT 尺寸或 raw 保留错误");
  const auto bin = frequency_index(stft.value().frequency_hz, 6'000.0);
  const auto scale = static_cast<double>(frame_length) / sample_rate;
  require_close(stft.value().raw_values[bin], scale, 1e-9, "STFT 第一帧原始功率密度错误");
  require_close(stft.value().raw_values[frame_length + bin], 0.25 * scale, 1e-9, "STFT 第二帧原始功率密度错误");
  require_close(stft.value().values[frame_length + bin], 0.625 * scale, 1e-9, "STFT 时间指数平滑错误");
  require_close(stft.value().values[2U * frame_length + bin], 0.34375 * scale, 1e-9, "STFT 时间指数平滑递推错误");
  require_close(stft.value().time_seconds.front(), 31.5 / sample_rate, 1e-15, "STFT 帧中心时间戳错误");
  auto unnormalized_settings = settings;
  unnormalized_settings.normalization = SpectrumNormalization::none;
  const auto unnormalized_stft = calculate_stft(*fft, input.view(), sample_rate, 0.0, unnormalized_settings, nullptr);
  require(unnormalized_stft, "STFT 输出未接受 None 归一化");
  require(unnormalized_stft.value().normalization == SpectrumNormalization::none,
          "STFT 结果未携带实际 None 归一化语义");
  require_close(unnormalized_stft.value().raw_density_linear[bin],
                stft.value().raw_density_linear[bin] * static_cast<double>(frame_length), 1e-9,
                "STFT None 归一化未真实保留原始 FFT 功率密度尺度");
  const auto stft_counts = std::make_shared<FftCallCounts>();
  CountingFftBackend stft_counting_backend{fft, stft_counts};
  constexpr std::uint64_t source_offset = 48'000U;
  const auto offset_stft =
      calculate_stft(stft_counting_backend, input.view(), sample_rate, 0.0, settings, nullptr, source_offset);
  require(offset_stft && stft_counts->plans == 1U && stft_counts->executions == 3U,
          "参数化 STFT 未在全部帧间复用一个 FFT 计划");
  require_close(offset_stft.value().time_seconds.front(), 1.0 + 31.5 / sample_rate, 1e-15,
                "非零导入范围的 STFT 时间轴未叠加源样本起点");
  AnalysisSettingsSnapshot offset_snapshot;
  offset_snapshot.spectrum.frame_length = frame_length;
  offset_snapshot.spectrum.fft_length = frame_length;
  offset_snapshot.spectrum.window = {WindowKind::rectangular, 0.0};
  offset_snapshot.spectrum.sidedness = SpectrumSidedness::two_sided_shifted;
  offset_snapshot.spectrum.output_quantity = SpectrumOutputQuantity::linear_power_density;
  offset_snapshot.spectrum.normalization = SpectrumNormalization::window_power;
  offset_snapshot.spectrogram = settings;
  auto offset_descriptor = descriptor(data::SignalKind::complex, input.view().size(), sample_rate);
  offset_descriptor.requested_sample_range = data::SampleRange::from_count(source_offset, input.view().size()).value();
  const auto descriptor_offset_stft =
      calculate_stft(*fft, *kernels, input.view(), offset_descriptor, offset_snapshot, nullptr);
  require(descriptor_offset_stft, "SignalDescriptor STFT 重载未完成非零范围分析");
  require_close(descriptor_offset_stft.value().time_seconds.front(), 1.0 + 31.5 / sample_rate, 1e-15,
                "SignalDescriptor STFT 重载忽略 requested_sample_range 起点");
  require_close(spectrogram_overlap_ratio(settings), 0.0, 1e-15, "STFT overlap 派生错误");
  auto unsmoothed_settings = settings;
  unsmoothed_settings.smoothing = {};
  const auto unsmoothed = calculate_stft(*fft, input.view(), sample_rate, 0.0, unsmoothed_settings, nullptr);
  const auto reused_time_smoothing =
      unsmoothed ? resmooth_stft(unsmoothed.value(), settings, nullptr) : core::Result<StftResult>{unsmoothed.error()};
  require(unsmoothed && reused_time_smoothing &&
              reused_time_smoothing.value().raw_linear_values == unsmoothed.value().raw_linear_values,
          "STFT 平滑最小失效未复用原始线性矩阵");
  for (std::size_t index = 0; index < stft.value().values.size(); ++index) {
    require_close(reused_time_smoothing.value().values[index], stft.value().values[index], 1e-9,
                  "复用 raw 的 STFT 时间平滑与完整重算不一致");
  }

  auto frequency_smoothed = settings;
  frequency_smoothed.smoothing.time_mode = SpectrogramTimeSmoothingKind::none;
  frequency_smoothed.smoothing.frequency_mode = SpectrogramFrequencySmoothingKind::gaussian;
  frequency_smoothed.smoothing.frequency_kernel_length = 5U;
  frequency_smoothed.smoothing.frequency_sigma = 1.0;
  const auto gaussian = calculate_stft(*fft, input.view(), sample_rate, 0.0, frequency_smoothed, nullptr);
  require(gaussian && gaussian.value().raw_values[bin] > gaussian.value().values[bin] &&
              gaussian.value().values[bin - 1U] > gaussian.value().raw_values[bin - 1U],
          "STFT 频率高斯平滑未生效或 raw 被覆盖");

  constexpr std::size_t long_count = 4096U;
  std::vector<data::ComplexSample> two_tone(long_count);
  for (std::size_t index = 0; index < long_count; ++index) {
    const auto low_phase = 2.0 * std::numbers::pi * 128.0 * static_cast<double>(index) / long_count;
    const auto high_phase = 2.0 * std::numbers::pi * 1024.0 * static_cast<double>(index) / long_count;
    two_tone[index] = {std::cos(low_phase) + std::cos(high_phase), std::sin(low_phase) + std::sin(high_phase)};
  }
  const auto prefilter_input = data::SignalBuffer::from_complex(std::move(two_tone));
  AnalysisSettingsSnapshot snapshot;
  snapshot.spectrum.frame_length = long_count;
  snapshot.spectrum.fft_length = long_count;
  snapshot.spectrum.window = {WindowKind::rectangular, 0.0};
  snapshot.spectrum.sidedness = SpectrumSidedness::two_sided_shifted;
  snapshot.spectrum.output_quantity = SpectrumOutputQuantity::linear_amplitude;
  snapshot.prefilter.enabled = true;
  snapshot.prefilter.boundary = BoundaryPolicy::zero_pad;
  auto filter = node("analysis-prefilter", NodeKind::fir_filter);
  filter.filter_shape = FilterShape::custom;
  filter.numerator = lowpass(127U, 0.125);
  snapshot.prefilter.chain.nodes.push_back(filter);
  const auto filtered =
      calculate_spectrum(*fft, *kernels, prefilter_input.view(),
                         descriptor(data::SignalKind::complex, long_count, sample_rate), snapshot, nullptr);
  require(filtered && filtered.value().prefilter_applied && !filtered.value().settings_hash.hex.empty(),
          "分析前滤波未接入 process_chain 或结果缺少参数哈希");
  const auto low_bin = frequency_index(filtered.value().frequency_hz, 1'500.0);
  const auto high_bin = frequency_index(filtered.value().frequency_hz, 12'000.0);
  require(20.0 * std::log10(
                     std::max(filtered.value().raw_values[high_bin] / filtered.value().raw_values[low_bin], 1e-300)) <
              -30.0,
          "分析前 FIR 未真实抑制高频分量");

  auto forbidden = snapshot;
  forbidden.prefilter.chain.nodes.front().kind = NodeKind::resample;
  require(!calculate_spectrum(*fft, *kernels, prefilter_input.view(),
                              descriptor(data::SignalKind::complex, long_count, sample_rate), forbidden, nullptr),
          "分析前滤波错误接受 MS-05 重采样/通道业务");
}

void test_ms45_backend_consistency() {
  auto cpu = fft_backend();
  const auto signal = complex_tone(2048U, 137U, 0.75);
  SpectrumAnalysisSettings settings;
  settings.frame_length = 512U;
  settings.fft_length = 1024U;
  settings.zero_padding_policy = ZeroPaddingPolicy::enabled;
  settings.window = {WindowKind::blackman_harris, 0.0};
  settings.sidedness = SpectrumSidedness::two_sided_shifted;
  settings.output_quantity = SpectrumOutputQuantity::linear_power_density;
  settings.normalization = SpectrumNormalization::window_power;
  settings.estimator = {PsdEstimatorKind::welch, 0.5, 4U};
  settings.accumulation = {SpectrumAccumulationMode::linear_average, 4U, 1.0, 0U};
  settings.smoothing = {SpectrumSmoothingKind::gaussian, 5U, 1.0, 0U};
  const auto cpu_psd = calculate_psd(*cpu, signal.view(), 48'000.0, 0.0, settings, nullptr);
  require(cpu_psd && cpu_psd.value().provenance.precision == "complex-float64",
          "MS-4.5 CPU Welch/高斯 PSD 失败或未记录实际 FFT 精度");

  SpectrogramAnalysisSettings stft_settings;
  stft_settings.frame_length = 256U;
  stft_settings.fft_length = 512U;
  stft_settings.hop_length = 128U;
  stft_settings.padding_policy = ZeroPaddingPolicy::enabled;
  stft_settings.window = {WindowKind::kaiser, 7.5};
  stft_settings.sidedness = SpectrumSidedness::two_sided_shifted;
  stft_settings.smoothing = {SpectrogramFrequencySmoothingKind::gaussian, 5U, 1.0,
                             SpectrogramTimeSmoothingKind::exponential, 0.35};
  const auto cpu_stft = calculate_stft(*cpu, signal.view(), 48'000.0, 0.0, stft_settings, nullptr);
  require(cpu_stft && cpu_stft.value().provenance.precision == "complex-float64",
          "MS-4.5 CPU 参数化 STFT 失败或未记录实际 FFT 精度");

  const auto require_mixed_welch_rejected = [&](ProvenanceMutation mutation) {
    auto state = std::make_shared<ProvenanceSwitchState>(ProvenanceSwitchState{0U, 2U, mutation});
    SwitchingProvenanceFftBackend switching_backend{cpu, state};
    const auto mixed = calculate_psd(switching_backend, signal.view(), 48'000.0, 0.0, settings, nullptr);
    require(!mixed && state->executions == 2U, "Welch/平均未在第 N 帧拒绝混合 FFT provenance");
  };
  require_mixed_welch_rejected(ProvenanceMutation::backend);
  require_mixed_welch_rejected(ProvenanceMutation::device);
  require_mixed_welch_rejected(ProvenanceMutation::precision);
  require_mixed_welch_rejected(ProvenanceMutation::fallback);

  auto stft_switch =
      std::make_shared<ProvenanceSwitchState>(ProvenanceSwitchState{0U, 3U, ProvenanceMutation::backend});
  SwitchingProvenanceFftBackend switching_stft_backend{cpu, stft_switch};
  const auto mixed_stft = calculate_stft(switching_stft_backend, signal.view(), 48'000.0, 0.0, stft_settings, nullptr);
  require(!mixed_stft && stft_switch->executions == 3U, "STFT 未在第 N 帧拒绝混合 FFT provenance");

  const auto cuda = make_cuda_fft_backend();
  if (!cuda) {
#if defined(SIGNAL_STUDIO_TEST_CUDA_REQUIRED)
    require(false, std::string{"CUDA 构建未能创建必需的 cuFFT 后端: "} + std::string{cuda.error().message()});
#else
    std::cout << "MS45_CUDA unavailable: " << cuda.error().message() << '\n';
    return;
#endif
  }
  const auto cuda_psd = calculate_psd(*cuda.value(), signal.view(), 48'000.0, 0.0, settings, nullptr);
  const auto cuda_stft = calculate_stft(*cuda.value(), signal.view(), 48'000.0, 0.0, stft_settings, nullptr);
  if (!cuda_psd) {
    require(false, std::string{"CUDA backend was created but PSD execution failed: "} +
                       std::string{cuda_psd.error().message()});
  }
  if (!cuda_stft) {
    require(false, std::string{"CUDA backend was created but STFT execution failed: "} +
                       std::string{cuda_stft.error().message()});
  }
  require(cuda_psd.value().frequency_hz == cpu_psd.value().frequency_hz &&
              cuda_psd.value().provenance.precision == "complex-float64" &&
              cuda_psd.value().values.size() == cpu_psd.value().values.size() &&
              cuda_psd.value().raw_values.size() == cpu_psd.value().raw_values.size() &&
              cuda_psd.value().db_per_hz.size() == cpu_psd.value().db_per_hz.size(),
          "CPU/CUDA PSD 轴或尺寸不一致");
  for (std::size_t index = 0; index < cpu_psd.value().values.size(); ++index) {
    require_close(cuda_psd.value().values[index], cpu_psd.value().values[index],
                  std::max(1e-11, std::abs(cpu_psd.value().values[index]) * 1e-9),
                  "CPU/CUDA Welch/平滑 PSD 数值不一致");
    require_close(cuda_psd.value().raw_values[index], cpu_psd.value().raw_values[index],
                  std::max(1e-11, std::abs(cpu_psd.value().raw_values[index]) * 1e-9),
                  "CPU/CUDA Welch 原始 PSD 数值不一致");
    require_close(cuda_psd.value().db_per_hz[index], cpu_psd.value().db_per_hz[index], 1e-7,
                  "CPU/CUDA PSD dB/Hz 数值不一致");
  }
  require(cuda_stft.value().time_seconds == cpu_stft.value().time_seconds &&
              cuda_stft.value().provenance.precision == "complex-float64" &&
              cuda_stft.value().frequency_hz == cpu_stft.value().frequency_hz &&
              cuda_stft.value().values.size() == cpu_stft.value().values.size() &&
              cuda_stft.value().raw_values.size() == cpu_stft.value().raw_values.size() &&
              cuda_stft.value().db_per_hz.size() == cpu_stft.value().db_per_hz.size(),
          "CPU/CUDA STFT 轴或尺寸不一致");
  for (std::size_t index = 0; index < cpu_stft.value().values.size(); ++index) {
    require_close(cuda_stft.value().values[index], cpu_stft.value().values[index],
                  std::max(1e-6, std::abs(static_cast<double>(cpu_stft.value().values[index])) * 1e-5),
                  "CPU/CUDA STFT/平滑数值不一致");
    require_close(cuda_stft.value().raw_values[index], cpu_stft.value().raw_values[index],
                  std::max(1e-6, std::abs(static_cast<double>(cpu_stft.value().raw_values[index])) * 1e-5),
                  "CPU/CUDA STFT 原始数值不一致");
    require_close(cuda_stft.value().db_per_hz[index], cpu_stft.value().db_per_hz[index], 1e-5,
                  "CPU/CUDA STFT dB/Hz 数值不一致");
  }

  auto kernels = kernel_backend();
  AnalysisSettingsSnapshot filtered_settings;
  filtered_settings.spectrum = settings;
  filtered_settings.prefilter.enabled = true;
  filtered_settings.prefilter.boundary = BoundaryPolicy::zero_pad;
  auto filter = node("backend-prefilter", NodeKind::fir_filter);
  filter.filter_shape = FilterShape::custom;
  filter.numerator = lowpass(31U, 0.18);
  filtered_settings.prefilter.chain.nodes.push_back(std::move(filter));
  const auto filtered_descriptor = descriptor(data::SignalKind::complex, signal.view().size(), 48'000.0);
  const auto cpu_filtered =
      calculate_psd(*cpu, *kernels, signal.view(), filtered_descriptor, filtered_settings, nullptr);
  const auto cuda_filtered =
      calculate_psd(*cuda.value(), *kernels, signal.view(), filtered_descriptor, filtered_settings, nullptr);
  require(cpu_filtered && cuda_filtered && cpu_filtered.value().prefilter_applied &&
              cuda_filtered.value().prefilter_applied &&
              cpu_filtered.value().values.size() == cuda_filtered.value().values.size(),
          "CPU/CUDA 分析前滤波 PSD 执行失败");
  for (std::size_t index = 0; index < cpu_filtered.value().values.size(); ++index) {
    require_close(cuda_filtered.value().values[index], cpu_filtered.value().values[index],
                  std::max(1e-11, std::abs(cpu_filtered.value().values[index]) * 1e-9),
                  "CPU/CUDA 分析前滤波 PSD 数值不一致");
  }
  std::cout << "MS45_CUDA backend=cuFFT PSD/STFT/prefilter consistency passed\n";
}

void test_ms45_scipy_reference() {
  constexpr double sample_rate = 8'000.0;
  constexpr std::size_t frame_length = 64U;
  constexpr std::size_t fft_length = 128U;
  constexpr std::size_t hop_length = 32U;
  constexpr std::size_t sample_count = 192U;
  std::vector<double> samples(sample_count);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const auto phase5 = 2.0 * std::numbers::pi * 5.0 * static_cast<double>(index) / static_cast<double>(frame_length);
    const auto phase11 = 2.0 * std::numbers::pi * 11.0 * static_cast<double>(index) / static_cast<double>(frame_length);
    const auto dither = 0.01 * static_cast<double>(static_cast<int>(index % 7U) - 3);
    samples[index] = 0.35 * std::cos(phase5) + 0.20 * std::sin(phase11) + dither;
  }
  const auto input = data::SignalBuffer::from_real(std::move(samples));
  auto backend = fft_backend();

  SpectrumAnalysisSettings periodogram_settings;
  periodogram_settings.analysis_range_policy = AnalysisRangePolicy::first_frame;
  periodogram_settings.frame_length = frame_length;
  periodogram_settings.fft_length = fft_length;
  periodogram_settings.zero_padding_policy = ZeroPaddingPolicy::enabled;
  periodogram_settings.window = {WindowKind::hann, 0.0};
  periodogram_settings.sidedness = SpectrumSidedness::one_sided;
  periodogram_settings.output_quantity = SpectrumOutputQuantity::linear_power_density;
  periodogram_settings.normalization = SpectrumNormalization::window_power;
  periodogram_settings.detrend_policy = DetrendPolicy::none;
  periodogram_settings.estimator = {PsdEstimatorKind::periodogram, 0.0, 1U};
  const auto periodogram = calculate_psd(*backend, input.view(), sample_rate, 0.0, periodogram_settings, nullptr);

  auto welch_settings = periodogram_settings;
  welch_settings.analysis_range_policy = AnalysisRangePolicy::all_complete_frames;
  welch_settings.estimator = {PsdEstimatorKind::welch, 0.5, 5U};
  const auto welch = calculate_psd(*backend, input.view(), sample_rate, 0.0, welch_settings, nullptr);

  SpectrogramAnalysisSettings stft_settings;
  stft_settings.frame_length = frame_length;
  stft_settings.fft_length = fft_length;
  stft_settings.hop_length = hop_length;
  stft_settings.padding_policy = ZeroPaddingPolicy::enabled;
  stft_settings.window = {WindowKind::hann, 0.0};
  stft_settings.sidedness = SpectrumSidedness::one_sided;
  stft_settings.output_quantity = SpectrumOutputQuantity::linear_power_density;
  stft_settings.normalization = SpectrumNormalization::window_power;
  stft_settings.detrend_policy = DetrendPolicy::none;
  const auto stft = calculate_stft(*backend, input.view(), sample_rate, 0.0, stft_settings, nullptr);
  require(periodogram && welch && stft && periodogram.value().segment_count == 1U &&
              welch.value().segment_count == 5U && stft.value().rows == 5U && stft.value().columns == 65U,
          "独立 SciPy 参考参数未产生预期尺寸");

  std::ifstream reference{SIGNAL_STUDIO_MS45_REFERENCE_FILE};
  require(reference.good(), "无法读取 MS-4.5 NumPy/SciPy 参考数据");
  std::string line;
  std::size_t checked{};
  while (std::getline(reference, line)) {
    if (line.empty() || line.front() == '#' || line.starts_with("case,")) {
      continue;
    }
    std::array<std::string, 6U> fields;
    std::stringstream row{line};
    for (auto& field : fields) {
      require(static_cast<bool>(std::getline(row, field, ',')), "NumPy/SciPy 参考行字段不足");
    }
    const auto row_index = std::stoi(fields[1]);
    const auto bin = static_cast<std::size_t>(std::stoull(fields[2]));
    const auto expected_frequency = std::stod(fields[3]);
    const auto expected_time = std::stod(fields[4]);
    const auto expected_value = std::stod(fields[5]);
    const std::vector<double>* frequency{};
    double actual_time{-1.0};
    double actual_value{};
    if (fields[0] == "periodogram") {
      frequency = &periodogram.value().frequency_hz;
      actual_value = periodogram.value().raw_density_linear.at(bin);
    } else if (fields[0] == "welch") {
      frequency = &welch.value().frequency_hz;
      actual_value = welch.value().raw_density_linear.at(bin);
    } else if (fields[0] == "stft") {
      require(row_index >= 0, "SciPy STFT 参考行索引无效");
      frequency = &stft.value().frequency_hz;
      actual_time = stft.value().time_seconds.at(static_cast<std::size_t>(row_index));
      actual_value =
          stft.value().raw_density_linear.at(static_cast<std::size_t>(row_index) * stft.value().columns + bin);
    } else {
      require(false, "NumPy/SciPy 参考包含未知用例");
    }
    require_close(frequency->at(bin), expected_frequency, 1e-12, "NumPy/SciPy 参考频率轴不一致");
    if (row_index >= 0) {
      require_close(actual_time, expected_time, 1e-15, "NumPy/SciPy 参考 STFT 帧中心时间不一致");
    }
    require_close(actual_value, expected_value, std::max(1e-14, std::abs(expected_value) * 2e-10),
                  "NumPy/SciPy 参考功率密度不一致");
    ++checked;
  }
  require(checked == 49U, "NumPy/SciPy 参考检查点数量不完整");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3 || std::string_view(argv[1]) != "--case") {
      throw std::runtime_error("用法：signal_studio_dsp_tests --case <需求编号>");
    }
    const std::map<std::string_view, std::function<void()>> cases{
        {"FR-DSP-001", test_dsp_001},
        {"FR-DSP-002", test_dsp_002},
        {"FR-DSP-003", test_dsp_003},
        {"FR-DSP-004", test_dsp_004},
        {"FR-DSP-005", test_dsp_005},
        {"FR-DSP-006", test_dsp_006},
        {"FR-DSP-007", test_dsp_007},
        {"FR-DSP-008", test_dsp_008},
        {"FR-DSP-009", test_dsp_009},
        {"FR-DSP-010", test_dsp_010},
        {"FR-DSP-011", test_dsp_011},
        {"FR-DSP-012", test_dsp_012},
        {"NFR-NUM-001", test_num_001},
        {"NFR-NUM-002", test_num_002},
        {"NFR-NUM-003", test_num_003},
        {"NFR-NUM-004", test_num_004},
        {"NFR-NUM-005", test_num_005},
        {"FR-DSP-101", test_dsp_101},
        {"MS-4.5-CONTRACTS", test_ms45_contracts},
        {"MS-4.5-SPECTRUM-PSD", test_ms45_spectrum_psd},
        {"MS-4.5-STFT-PREFILTER", test_ms45_stft_prefilter},
        {"MS-4.5-BACKENDS", test_ms45_backend_consistency},
        {"MS-4.5-SCIPY-REFERENCE", test_ms45_scipy_reference},
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
