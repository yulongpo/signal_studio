#include "signal_studio/dsp/pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>

namespace signal::dsp {
namespace {

[[nodiscard]] core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::dsp, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] auto find_node(std::vector<NodeSpec>& nodes, std::string_view id) {
  return std::ranges::find_if(nodes, [id](const NodeSpec& node) { return node.id == id; });
}

[[nodiscard]] auto find_node(const std::vector<NodeSpec>& nodes, std::string_view id) {
  return std::ranges::find_if(nodes, [id](const NodeSpec& node) { return node.id == id; });
}

[[nodiscard]] core::Result<std::vector<data::ComplexSample>>
to_complex(const data::SignalSlice& input, const std::shared_ptr<const std::atomic_bool>& cancellation = nullptr) {
  std::vector<data::ComplexSample> output;
  output.reserve(static_cast<std::size_t>(input.size()));
  constexpr std::size_t cancellation_check_interval = 4'096U;
  if (input.kind() == data::SignalKind::real) {
    const auto values = input.real_values();
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index % cancellation_check_interval == 0U && cancellation != nullptr &&
          cancellation->load(std::memory_order_relaxed)) {
        return error(core::ErrorReason::cancelled, "输入样本转换已取消，未发布部分结果");
      }
      output.push_back({values[index], 0.0});
    }
  } else {
    const auto values = input.complex_values();
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index % cancellation_check_interval == 0U && cancellation != nullptr &&
          cancellation->load(std::memory_order_relaxed)) {
        return error(core::ErrorReason::cancelled, "输入样本转换已取消，未发布部分结果");
      }
      output.push_back(values[index]);
    }
  }
  return output;
}

[[nodiscard]] core::Result<data::SignalBuffer>
to_buffer(std::vector<data::ComplexSample> samples, data::SignalKind kind,
          const std::shared_ptr<const std::atomic_bool>& cancellation = nullptr) {
  if (kind == data::SignalKind::complex) {
    if (cancellation != nullptr && cancellation->load(std::memory_order_relaxed)) {
      return error(core::ErrorReason::cancelled, "复数输出提交前已取消，未发布部分结果");
    }
    return data::SignalBuffer::from_complex(std::move(samples));
  }
  std::vector<double> real;
  real.reserve(samples.size());
  constexpr std::size_t cancellation_check_interval = 4'096U;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    if (index % cancellation_check_interval == 0U && cancellation != nullptr &&
        cancellation->load(std::memory_order_relaxed)) {
      return error(core::ErrorReason::cancelled, "实数输出转换已取消，未发布部分结果");
    }
    real.push_back(samples[index].real);
  }
  if (cancellation != nullptr && cancellation->load(std::memory_order_relaxed)) {
    return error(core::ErrorReason::cancelled, "实数输出提交前已取消，未发布部分结果");
  }
  return data::SignalBuffer::from_real(std::move(real));
}

[[nodiscard]] bool cancelled(const ProcessRequest& request) noexcept {
  return request.cancellation != nullptr && request.cancellation->load(std::memory_order_relaxed);
}

[[nodiscard]] core::Result<double> required_parameter(const NodeSpec& node, std::string_view key) {
  const auto found = node.parameters.find(key);
  if (found == node.parameters.end() || !std::isfinite(found->second)) {
    return error(core::ErrorReason::invalid_argument, "滤波器缺少有效设计参数", std::string(key));
  }
  return found->second;
}

[[nodiscard]] double normalized_sinc(double value) noexcept {
  return std::abs(value) < 1e-14 ? 1.0 : std::sin(std::numbers::pi * value) / (std::numbers::pi * value);
}

[[nodiscard]] std::vector<double> ideal_lowpass(std::size_t tap_count, double normalized_cutoff) {
  std::vector<double> result(tap_count);
  const auto center = static_cast<double>(tap_count - 1U) / 2.0;
  for (std::size_t index = 0; index < tap_count; ++index) {
    const auto offset = static_cast<double>(index) - center;
    const auto ideal = 2.0 * normalized_cutoff * normalized_sinc(2.0 * normalized_cutoff * offset);
    const auto window = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) /
                                               static_cast<double>(tap_count - 1U));
    result[index] = ideal * window;
  }
  return result;
}

[[nodiscard]] std::complex<double> frequency_response(std::span<const double> numerator, double normalized_frequency) {
  std::complex<double> response{};
  for (std::size_t index = 0; index < numerator.size(); ++index) {
    const auto phase = -2.0 * std::numbers::pi * normalized_frequency * static_cast<double>(index);
    response += numerator[index] * std::complex<double>{std::cos(phase), std::sin(phase)};
  }
  return response;
}

[[nodiscard]] core::Status normalize_fir(std::vector<double>& coefficients, double normalized_frequency) {
  const auto response = frequency_response(coefficients, normalized_frequency);
  const auto magnitude = std::abs(response);
  if (!(magnitude > 1e-12) || !std::isfinite(magnitude)) {
    return error(core::ErrorReason::invalid_argument, "FIR 设计在归一化频点响应为零");
  }
  for (auto& coefficient : coefficients) {
    coefficient /= magnitude;
  }
  return core::Status::success();
}

[[nodiscard]] bool stable_denominator(std::span<const double> denominator) {
  if (denominator.empty() || denominator.front() == 0.0 ||
      std::ranges::any_of(denominator, [](double value) { return !std::isfinite(value); })) {
    return false;
  }
  if (denominator.size() == 1U) {
    return true;
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

[[nodiscard]] core::Result<FilterCoefficients> design_fir(const NodeSpec& node, double sample_rate_hz) {
  const auto order_parameter = required_parameter(node, "order");
  if (!order_parameter || order_parameter.value() < 2.0 || order_parameter.value() > 4096.0 ||
      std::floor(order_parameter.value()) != order_parameter.value()) {
    return error(core::ErrorReason::invalid_argument, "FIR order 必须是 2..4096 的整数");
  }
  auto order = static_cast<std::size_t>(order_parameter.value());
  if (order % 2U != 0U) {
    ++order;
  }
  const auto taps = order + 1U;
  std::vector<double> coefficients;
  double normalization_frequency{};
  if (node.filter_shape == FilterShape::lowpass || node.filter_shape == FilterShape::highpass) {
    const auto cutoff = required_parameter(node, "cutoff_hz");
    if (!cutoff || !(cutoff.value() > 0.0) || !(cutoff.value() < sample_rate_hz / 2.0)) {
      return error(core::ErrorReason::invalid_argument, "低通/高通截止频率必须位于 Nyquist 内");
    }
    coefficients = ideal_lowpass(taps, cutoff.value() / sample_rate_hz);
    if (node.filter_shape == FilterShape::highpass) {
      for (auto& coefficient : coefficients) {
        coefficient = -coefficient;
      }
      coefficients[order / 2U] += 1.0;
      normalization_frequency = 0.5;
    }
  } else {
    const auto low = required_parameter(node, "low_cutoff_hz");
    const auto high = required_parameter(node, "high_cutoff_hz");
    if (!low || !high || !(low.value() > 0.0) || !(low.value() < high.value()) ||
        !(high.value() < sample_rate_hz / 2.0)) {
      return error(core::ErrorReason::invalid_argument, "带通/带阻边界必须递增且位于 Nyquist 内");
    }
    const auto lowpass_low = ideal_lowpass(taps, low.value() / sample_rate_hz);
    const auto lowpass_high = ideal_lowpass(taps, high.value() / sample_rate_hz);
    coefficients.resize(taps);
    for (std::size_t index = 0; index < taps; ++index) {
      coefficients[index] = lowpass_high[index] - lowpass_low[index];
    }
    normalization_frequency = (low.value() + high.value()) / (2.0 * sample_rate_hz);
    if (node.filter_shape == FilterShape::bandstop) {
      for (auto& coefficient : coefficients) {
        coefficient = -coefficient;
      }
      coefficients[order / 2U] += 1.0;
      normalization_frequency = 0.0;
    }
  }
  if (const auto status = normalize_fir(coefficients, normalization_frequency); !status) {
    return status;
  }
  return FilterCoefficients{std::move(coefficients), {}};
}

[[nodiscard]] core::Result<FilterCoefficients> design_iir(const NodeSpec& node, double sample_rate_hz) {
  const auto order = required_parameter(node, "order");
  if (!order || order.value() != 2.0) {
    return error(core::ErrorReason::invalid_argument, "内置 IIR 形态当前使用可验证的二阶节，order 必须为 2");
  }
  double center_frequency{};
  double quality = std::numbers::sqrt2 / 2.0;
  if (node.filter_shape == FilterShape::lowpass || node.filter_shape == FilterShape::highpass) {
    const auto cutoff = required_parameter(node, "cutoff_hz");
    if (!cutoff || !(cutoff.value() > 0.0) || !(cutoff.value() < sample_rate_hz / 2.0)) {
      return error(core::ErrorReason::invalid_argument, "IIR 截止频率必须位于 Nyquist 内");
    }
    center_frequency = cutoff.value();
    if (const auto found = node.parameters.find("q"); found != node.parameters.end()) {
      quality = found->second;
    }
  } else {
    const auto low = required_parameter(node, "low_cutoff_hz");
    const auto high = required_parameter(node, "high_cutoff_hz");
    if (!low || !high || !(low.value() > 0.0) || !(low.value() < high.value()) ||
        !(high.value() < sample_rate_hz / 2.0)) {
      return error(core::ErrorReason::invalid_argument, "IIR 带通/带阻边界无效");
    }
    center_frequency = std::sqrt(low.value() * high.value());
    quality = center_frequency / (high.value() - low.value());
  }
  if (!(quality > 0.0) || !std::isfinite(quality)) {
    return error(core::ErrorReason::invalid_argument, "IIR Q 值必须为有限正数");
  }
  const auto omega = 2.0 * std::numbers::pi * center_frequency / sample_rate_hz;
  const auto cosine = std::cos(omega);
  const auto alpha = std::sin(omega) / (2.0 * quality);
  std::array<double, 3U> numerator{};
  if (node.filter_shape == FilterShape::lowpass) {
    numerator = {(1.0 - cosine) / 2.0, 1.0 - cosine, (1.0 - cosine) / 2.0};
  } else if (node.filter_shape == FilterShape::highpass) {
    numerator = {(1.0 + cosine) / 2.0, -(1.0 + cosine), (1.0 + cosine) / 2.0};
  } else if (node.filter_shape == FilterShape::bandpass) {
    numerator = {alpha, 0.0, -alpha};
  } else {
    numerator = {1.0, -2.0 * cosine, 1.0};
  }
  const std::array<double, 3U> denominator{1.0 + alpha, -2.0 * cosine, 1.0 - alpha};
  FilterCoefficients result;
  result.numerator.assign(numerator.begin(), numerator.end());
  result.denominator.assign(denominator.begin(), denominator.end());
  const auto a0 = denominator.front();
  for (auto& value : result.numerator) {
    value /= a0;
  }
  for (auto& value : result.denominator) {
    value /= a0;
  }
  if (!stable_denominator(result.denominator)) {
    return error(core::ErrorReason::internal_failure, "内置 IIR 设计未通过根稳定性校验");
  }
  return result;
}

class FilterAdapter final : public IFilter {
public:
  explicit FilterAdapter(std::shared_ptr<ISignalKernelBackend> backend) : backend_(std::move(backend)) {}

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>>
  process(const NodeSpec& specification, double sample_rate_hz, std::span<const data::ComplexSample> input,
          FilterState& state, BoundaryPolicy boundary) override {
    auto coefficients = resolve_filter_coefficients(specification, sample_rate_hz);
    if (!coefficients) {
      return coefficients.error();
    }
    if (specification.kind == NodeKind::fir_filter) {
      return backend_->convolve(input, coefficients.value().numerator, state, boundary);
    }
    if (specification.kind == NodeKind::iir_filter) {
      return backend_->solve_iir(input, coefficients.value().numerator, coefficients.value().denominator, state,
                                 boundary);
    }
    return error(core::ErrorReason::invalid_argument, "IFilter 只接受 FIR/IIR 节点");
  }

private:
  std::shared_ptr<ISignalKernelBackend> backend_;
};

class ResamplerAdapter final : public IResampler {
public:
  explicit ResamplerAdapter(std::shared_ptr<ISignalKernelBackend> backend) : backend_(std::move(backend)) {}

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>> process(ResampleRatio ratio,
                                                                       std::span<const data::ComplexSample> input,
                                                                       std::span<const double> anti_alias_coefficients,
                                                                       FilterState& state, bool end_of_input) override {
    return backend_->resample(input, ratio.numerator, ratio.denominator, anti_alias_coefficients, state, end_of_input);
  }

private:
  std::shared_ptr<ISignalKernelBackend> backend_;
};

} // namespace

core::Result<std::shared_ptr<IFilter>> make_filter(std::shared_ptr<ISignalKernelBackend> backend) {
  if (!backend) {
    return error(core::ErrorReason::invalid_argument, "IFilter 要求有效信号核后端");
  }
  return std::shared_ptr<IFilter>(std::make_shared<FilterAdapter>(std::move(backend)));
}

core::Result<std::shared_ptr<IResampler>> make_resampler(std::shared_ptr<ISignalKernelBackend> backend) {
  if (!backend) {
    return error(core::ErrorReason::invalid_argument, "IResampler 要求有效信号核后端");
  }
  return std::shared_ptr<IResampler>(std::make_shared<ResamplerAdapter>(std::move(backend)));
}

core::Result<FilterCoefficients> resolve_filter_coefficients(const NodeSpec& node, double sample_rate_hz) {
  if (!(sample_rate_hz > 0.0) || !std::isfinite(sample_rate_hz) ||
      (node.kind != NodeKind::fir_filter && node.kind != NodeKind::iir_filter)) {
    return error(core::ErrorReason::invalid_argument, "滤波器类型或采样率无效");
  }
  if (node.filter_shape == FilterShape::custom) {
    if (node.numerator.empty() ||
        std::ranges::any_of(node.numerator, [](double value) { return !std::isfinite(value); })) {
      return error(core::ErrorReason::invalid_argument, "自定义滤波器分子/FIR 系数无效");
    }
    if (node.kind == NodeKind::fir_filter) {
      return FilterCoefficients{node.numerator, {}};
    }
    if (node.denominator.empty() || !stable_denominator(node.denominator)) {
      return error(core::ErrorReason::invalid_argument, "自定义 IIR 分母的根不全部位于单位圆内");
    }
    return FilterCoefficients{node.numerator, node.denominator};
  }
  return node.kind == NodeKind::fir_filter ? design_fir(node, sample_rate_hz) : design_iir(node, sample_rate_hz);
}

core::Status validate_anti_alias_filter(std::span<const double> coefficients, double input_sample_rate_hz,
                                        ResampleRatio ratio, double cutoff_hz, double required_stopband_db) {
  if (coefficients.size() < 3U || ratio.numerator == 0U || ratio.denominator == 0U || !(input_sample_rate_hz > 0.0) ||
      !std::isfinite(input_sample_rate_hz) || !(cutoff_hz > 0.0) || !std::isfinite(cutoff_hz) ||
      !(required_stopband_db >= 40.0) || !std::isfinite(required_stopband_db) ||
      std::ranges::any_of(coefficients, [](double value) { return !std::isfinite(value); })) {
    return error(core::ErrorReason::invalid_argument, "抗混叠 FIR 参数或系数无效");
  }
  const auto intermediate_rate = input_sample_rate_hz * static_cast<double>(ratio.numerator);
  const auto output_rate =
      input_sample_rate_hz * static_cast<double>(ratio.numerator) / static_cast<double>(ratio.denominator);
  if (!std::isfinite(intermediate_rate) || !std::isfinite(output_rate)) {
    return error(core::ErrorReason::invalid_argument, "重采样率计算溢出");
  }
  const auto alias_edge_hz = std::min(input_sample_rate_hz, output_rate) / 2.0;
  if (!(cutoff_hz < alias_edge_hz)) {
    return error(core::ErrorReason::invalid_argument, "抗混叠截止频率必须低于输入/输出较小 Nyquist");
  }
  const auto dc_gain = std::abs(frequency_response(coefficients, 0.0));
  if (!(dc_gain > 0.9 && dc_gain < 1.1)) {
    return error(core::ErrorReason::invalid_argument, "抗混叠 FIR 直流增益必须在 0.9..1.1");
  }
  const auto cutoff_gain = std::abs(frequency_response(coefficients, cutoff_hz / intermediate_rate)) / dc_gain;
  if (!(cutoff_gain >= std::pow(10.0, -6.25 / 20.0))) {
    return error(core::ErrorReason::invalid_argument, "抗混叠 FIR 在声明截止频率前已严重衰减");
  }
  double worst_stopband_gain{};
  constexpr std::size_t probe_count = 2048U;
  for (std::size_t probe = 0; probe <= probe_count; ++probe) {
    const auto frequency = alias_edge_hz + (intermediate_rate / 2.0 - alias_edge_hz) * static_cast<double>(probe) /
                                               static_cast<double>(probe_count);
    worst_stopband_gain = std::max(worst_stopband_gain,
                                   std::abs(frequency_response(coefficients, frequency / intermediate_rate)) / dc_gain);
  }
  const auto measured_stopband_db =
      -20.0 * std::log10(std::max(worst_stopband_gain, std::numeric_limits<double>::min()));
  if (measured_stopband_db + 0.25 < required_stopband_db) {
    std::ostringstream diagnostic;
    diagnostic << "声明 " << required_stopband_db << " dB，实测最差 " << measured_stopband_db << " dB";
    return error(core::ErrorReason::invalid_argument, "抗混叠 FIR 未达到声明阻带衰减", diagnostic.str());
  }
  return core::Status::success();
}

core::Status ProcessingChain::append(NodeSpec node) {
  if (node.id.empty() || find_node(nodes_, node.id) != nodes_.end()) {
    return error(core::ErrorReason::invalid_argument, "节点 ID 为空或重复");
  }
  nodes_.push_back(std::move(node));
  return core::Status::success();
}

core::Status ProcessingChain::set_enabled(std::string_view id, bool enabled) {
  const auto node = find_node(nodes_, id);
  if (node == nodes_.end()) {
    return error(core::ErrorReason::invalid_argument, "节点不存在");
  }
  node->enabled = enabled;
  return core::Status::success();
}

core::Status ProcessingChain::move(std::string_view id, std::size_t destination) {
  const auto node = find_node(nodes_, id);
  if (node == nodes_.end() || destination >= nodes_.size()) {
    return error(core::ErrorReason::invalid_argument, "节点或目标位置无效");
  }
  NodeSpec moved = std::move(*node);
  const auto source_index = static_cast<std::size_t>(std::distance(nodes_.begin(), node));
  nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(source_index));
  nodes_.insert(nodes_.begin() + static_cast<std::ptrdiff_t>(destination), std::move(moved));
  return core::Status::success();
}

core::Status ProcessingChain::duplicate(std::string_view id, std::string duplicate_id) {
  const auto node = find_node(nodes_, id);
  if (node == nodes_.end() || duplicate_id.empty() || find_node(nodes_, duplicate_id) != nodes_.end()) {
    return error(core::ErrorReason::invalid_argument, "源节点不存在或副本 ID 无效");
  }
  NodeSpec copy = *node;
  copy.id = std::move(duplicate_id);
  nodes_.insert(std::next(node), std::move(copy));
  return core::Status::success();
}

core::Status ProcessingChain::erase(std::string_view id) {
  const auto node = find_node(nodes_, id);
  if (node == nodes_.end()) {
    return error(core::ErrorReason::invalid_argument, "节点不存在");
  }
  nodes_.erase(node);
  return core::Status::success();
}

core::Status ProcessingChain::apply_preset(std::string_view id, std::map<std::string, double, std::less<>> values) {
  const auto node = find_node(nodes_, id);
  if (node == nodes_.end() || values.empty()) {
    return error(core::ErrorReason::invalid_argument, "节点不存在或预设为空");
  }
  if (std::ranges::any_of(values, [](const auto& entry) { return !std::isfinite(entry.second); })) {
    return error(core::ErrorReason::invalid_argument, "预设参数必须为有限数");
  }
  for (const auto& [key, value] : values) {
    if (key == "gain") {
      node->gain = value;
    } else if (key == "additive_offset") {
      node->additive_offset = value;
    } else if (key == "iq_gain_balance") {
      node->iq_gain_balance = value;
    } else if (key == "iq_phase_radians") {
      node->iq_phase_radians = value;
    } else if (key == "frequency_shift_hz") {
      node->frequency_shift_hz = value;
    } else if (key == "resample_numerator" && value >= 1.0 && std::floor(value) == value &&
               value <= std::numeric_limits<std::uint32_t>::max()) {
      node->resample_numerator = static_cast<std::uint32_t>(value);
    } else if (key == "resample_denominator" && value >= 1.0 && std::floor(value) == value &&
               value <= std::numeric_limits<std::uint32_t>::max()) {
      node->resample_denominator = static_cast<std::uint32_t>(value);
    } else if (key == "anti_alias_cutoff_hz") {
      node->anti_alias_cutoff_hz = value;
    } else if (key == "anti_alias_stopband_db") {
      node->anti_alias_stopband_db = value;
    } else {
      return error(core::ErrorReason::invalid_argument, "预设包含未知或类型无效的参数", key);
    }
  }
  node->parameters = std::move(values);
  return core::Status::success();
}

ChainSnapshot ProcessingChain::snapshot() const {
  return {"signal-processing-chain/1.0", "1.0.0", nodes_};
}

std::vector<std::string> ProcessingChain::invalidate_downstream(std::string_view changed_id) const {
  const auto node = find_node(nodes_, changed_id);
  if (node == nodes_.end()) {
    return {};
  }
  std::vector<std::string> result;
  result.reserve(static_cast<std::size_t>(std::distance(node, nodes_.end())));
  for (auto current = node; current != nodes_.end(); ++current) {
    result.push_back(current->id);
  }
  return result;
}

core::Status validate_node(const NodeSpec& node, const data::SignalDescriptor& input) {
  if (node.id.empty() || node.implementation_id.empty()) {
    return error(core::ErrorReason::invalid_argument, "节点 ID 和实现 ID 不得为空");
  }
  if (static_cast<unsigned>(node.kind) > static_cast<unsigned>(NodeKind::resample) ||
      static_cast<unsigned>(node.filter_shape) > static_cast<unsigned>(FilterShape::custom) ||
      static_cast<unsigned>(node.real_to_complex) > static_cast<unsigned>(RealToComplexMode::quadrature_mixer) ||
      !std::isfinite(node.gain) || !std::isfinite(node.additive_offset) || !std::isfinite(node.iq_gain_balance) ||
      node.iq_gain_balance == 0.0 || !std::isfinite(node.iq_phase_radians) || !std::isfinite(node.frequency_shift_hz) ||
      std::ranges::any_of(node.numerator, [](double value) { return !std::isfinite(value); }) ||
      std::ranges::any_of(node.denominator, [](double value) { return !std::isfinite(value); }) ||
      std::ranges::any_of(node.parameters, [](const auto& entry) { return !std::isfinite(entry.second); })) {
    return error(core::ErrorReason::invalid_argument, "节点枚举或数值参数无效");
  }
  if (const auto descriptor_status = input.validate(); !descriptor_status) {
    return descriptor_status.with_context("节点输入描述符");
  }
  const bool is_complex = input.signal_kind == data::SignalKind::complex;
  if ((is_complex && !node.contract.accepts_complex) || (!is_complex && !node.contract.accepts_real)) {
    return error(core::ErrorReason::invalid_argument, "输入实复类型不满足节点契约");
  }
  if (!(node.contract.sample_rate_numerator > 0.0) || !(node.contract.sample_rate_denominator > 0.0) ||
      node.contract.input_unit.empty() || node.contract.output_unit.empty()) {
    return error(core::ErrorReason::invalid_argument, "节点采样率关系或单位契约无效");
  }
  if (input.amplitude_mode != node.contract.input_unit) {
    return error(core::ErrorReason::invalid_argument, "节点输入单位与描述符不一致",
                 input.amplitude_mode + " != " + node.contract.input_unit);
  }
  const auto expected_rate_ratio = node.kind == NodeKind::resample ? static_cast<double>(node.resample_numerator) /
                                                                         static_cast<double>(node.resample_denominator)
                                                                   : 1.0;
  const auto declared_rate_ratio = node.contract.sample_rate_numerator / node.contract.sample_rate_denominator;
  if (!std::isfinite(expected_rate_ratio) || !std::isfinite(declared_rate_ratio) ||
      std::abs(expected_rate_ratio - declared_rate_ratio) >
          1e-12 * std::max({1.0, std::abs(expected_rate_ratio), std::abs(declared_rate_ratio)})) {
    return error(core::ErrorReason::invalid_argument, "节点声明的采样率关系与实际执行语义不一致");
  }
  if (node.kind == NodeKind::iq_correction && !is_complex) {
    return error(core::ErrorReason::invalid_argument, "实信号不能执行 IQ 校正：缺少独立 I/Q 分量");
  }
  if (node.kind == NodeKind::frequency_shift && !is_complex && node.real_to_complex == RealToComplexMode::forbidden) {
    return error(core::ErrorReason::invalid_argument, "实信号频移到复基带必须显式选择解析信号或正交混频");
  }
  if (node.kind == NodeKind::frequency_shift && std::abs(node.frequency_shift_hz) > input.sample_rate_hz / 2.0) {
    return error(core::ErrorReason::invalid_argument, "频移量超出 Nyquist 范围");
  }
  const bool converts_real_to_complex =
      node.kind == NodeKind::frequency_shift && !is_complex && node.real_to_complex != RealToComplexMode::forbidden;
  const bool actual_output_is_complex = is_complex || converts_real_to_complex;
  if (node.contract.produces_complex != actual_output_is_complex) {
    return error(core::ErrorReason::invalid_argument, "produces_complex 与节点实际输出类型不一致");
  }
  if (node.kind == NodeKind::fir_filter || node.kind == NodeKind::iir_filter) {
    if (const auto coefficients = resolve_filter_coefficients(node, input.sample_rate_hz); !coefficients) {
      return coefficients.error();
    }
  }
  if (node.kind == NodeKind::resample) {
    const auto output_nyquist = input.sample_rate_hz * static_cast<double>(node.resample_numerator) /
                                static_cast<double>(node.resample_denominator) / 2.0;
    if (node.resample_numerator == 0U || node.resample_denominator == 0U || !(node.anti_alias_cutoff_hz > 0.0) ||
        node.anti_alias_cutoff_hz >= output_nyquist || !(node.anti_alias_stopband_db >= 40.0)) {
      return error(core::ErrorReason::invalid_argument,
                   "重采样必须提供有效比例、抗混叠 FIR、截止频率和至少 40 dB 阻带元数据");
    }
    if (const auto filter_status = validate_anti_alias_filter(node.numerator, input.sample_rate_hz,
                                                              {node.resample_numerator, node.resample_denominator},
                                                              node.anti_alias_cutoff_hz, node.anti_alias_stopband_db);
        !filter_status) {
      return filter_status.with_context(node.id);
    }
  }
  return core::Status::success();
}

core::Result<ProcessResult> process_chain(ISignalKernelBackend& backend, const ProcessRequest& request,
                                          std::span<const FilterState> initial_states) {
  if (request.samples.size() == 0U || request.chain.schema != "signal-processing-chain/1.0") {
    return error(core::ErrorReason::invalid_argument, "处理请求样本为空或处理链架构不兼容");
  }
  if (!initial_states.empty() && initial_states.size() != request.chain.nodes.size()) {
    return error(core::ErrorReason::invalid_argument, "跨块状态数量必须与节点数量一致");
  }
  if (const auto descriptor_status = request.descriptor.validate(); !descriptor_status) {
    return descriptor_status.with_context("处理请求输入描述符");
  }
  if (request.descriptor.signal_kind != request.samples.kind() ||
      request.descriptor.requested_sample_range.size() != request.samples.size()) {
    return error(core::ErrorReason::invalid_argument, "处理请求描述符与实际样本类型或数量不一致");
  }
  if (cancelled(request)) {
    return error(core::ErrorReason::cancelled, "处理链已取消，未开始转换输入样本");
  }
  auto converted = to_complex(request.samples, request.cancellation);
  if (!converted) {
    return converted.error();
  }
  auto current = std::move(converted.value());
  if (cancelled(request)) {
    return error(core::ErrorReason::cancelled, "输入样本转换期间已取消，未发布部分结果");
  }
  auto kind = request.samples.kind();
  auto descriptor = request.descriptor;
  std::vector<FilterState> states(request.chain.nodes.size());
  if (!initial_states.empty()) {
    std::ranges::copy(initial_states, states.begin());
  }
  std::vector<std::string> applied;
  constexpr std::size_t maximum_kernel_chunk = 65'536U;
  constexpr std::size_t cancellation_check_interval = 4'096U;
  for (std::size_t node_index = 0; node_index < request.chain.nodes.size(); ++node_index) {
    if (cancelled(request)) {
      return error(core::ErrorReason::cancelled, "处理链已取消，未发布部分结果");
    }
    const auto& node = request.chain.nodes[node_index];
    if (!node.enabled) {
      continue;
    }
    const auto validation = validate_node(node, descriptor);
    if (!validation) {
      return validation.with_context(node.id);
    }
    switch (node.kind) {
    case NodeKind::remove_dc: {
      data::ComplexSample mean{};
      for (std::size_t index = 0; index < current.size(); ++index) {
        if (index % cancellation_check_interval == 0U && cancelled(request)) {
          return error(core::ErrorReason::cancelled, "去直流统计已取消，未发布部分结果");
        }
        const auto& sample = current[index];
        mean.real += sample.real;
        mean.imag += sample.imag;
      }
      mean.real /= static_cast<double>(current.size());
      mean.imag /= static_cast<double>(current.size());
      for (std::size_t index = 0; index < current.size(); ++index) {
        if (index % cancellation_check_interval == 0U && cancelled(request)) {
          return error(core::ErrorReason::cancelled, "去直流应用已取消，未发布部分结果");
        }
        auto& sample = current[index];
        sample.real -= mean.real;
        sample.imag -= mean.imag;
      }
      break;
    }
    case NodeKind::gain:
      for (std::size_t index = 0; index < current.size(); ++index) {
        if (index % cancellation_check_interval == 0U && cancelled(request)) {
          return error(core::ErrorReason::cancelled, "增益/标定已取消，未发布部分结果");
        }
        auto& sample = current[index];
        sample.real = sample.real * node.gain + node.additive_offset;
        sample.imag *= node.gain;
      }
      break;
    case NodeKind::iq_correction: {
      const auto cosine = std::cos(node.iq_phase_radians);
      const auto sine = std::sin(node.iq_phase_radians);
      for (std::size_t index = 0; index < current.size(); ++index) {
        if (index % cancellation_check_interval == 0U && cancelled(request)) {
          return error(core::ErrorReason::cancelled, "IQ 校正已取消，未发布部分结果");
        }
        auto& sample = current[index];
        const auto corrected_i = sample.real / node.iq_gain_balance;
        const auto corrected_q = sample.imag * node.iq_gain_balance;
        sample = {corrected_i * cosine - corrected_q * sine, corrected_i * sine + corrected_q * cosine};
      }
      break;
    }
    case NodeKind::frequency_shift: {
      if (kind == data::SignalKind::real) {
        if (node.real_to_complex == RealToComplexMode::analytic_signal) {
          std::vector<double> real;
          real.reserve(current.size());
          for (std::size_t index = 0; index < current.size(); ++index) {
            if (index % cancellation_check_interval == 0U && cancelled(request)) {
              return error(core::ErrorReason::cancelled, "解析信号输入准备已取消，未发布部分结果");
            }
            real.push_back(current[index].real);
          }
          auto analytic = backend.analytic_signal(real);
          if (!analytic) {
            return analytic.error().with_context(node.id);
          }
          if (cancelled(request)) {
            return error(core::ErrorReason::cancelled, "解析信号内核完成后已取消，未发布部分结果");
          }
          current = std::move(analytic.value());
        }
        kind = data::SignalKind::complex;
        descriptor.signal_kind = data::SignalKind::complex;
        descriptor.component_layout = data::ComponentLayout::interleaved;
        descriptor.component_order = data::ComponentOrder::iq;
      }
      auto phase = states[node_index].oscillator_phase_radians;
      const auto step = 2.0 * std::numbers::pi * node.frequency_shift_hz / descriptor.sample_rate_hz;
      for (std::size_t index = 0; index < current.size(); ++index) {
        if (index % cancellation_check_interval == 0U && cancelled(request)) {
          return error(core::ErrorReason::cancelled, "频移已取消，未发布部分结果");
        }
        auto& sample = current[index];
        const auto cosine = std::cos(phase);
        const auto sine = std::sin(phase);
        sample = {sample.real * cosine - sample.imag * sine, sample.real * sine + sample.imag * cosine};
        phase = std::remainder(phase + step, 2.0 * std::numbers::pi);
      }
      states[node_index].oscillator_phase_radians = phase;
      states[node_index].processed_samples += current.size();
      break;
    }
    case NodeKind::fir_filter: {
      const auto coefficients = resolve_filter_coefficients(node, descriptor.sample_rate_hz);
      if (!coefficients) {
        return coefficients.error().with_context(node.id);
      }
      std::vector<data::ComplexSample> filtered_all;
      filtered_all.reserve(current.size());
      for (std::size_t begin = 0; begin < current.size(); begin += maximum_kernel_chunk) {
        if (cancelled(request)) {
          return error(core::ErrorReason::cancelled, "FIR 分块处理已取消，未发布部分结果");
        }
        const auto count = std::min(maximum_kernel_chunk, current.size() - begin);
        auto filtered = backend.convolve(std::span<const data::ComplexSample>{current}.subspan(begin, count),
                                         coefficients.value().numerator, states[node_index],
                                         begin == 0U ? request.boundary : BoundaryPolicy::preserve_state);
        if (!filtered) {
          return filtered.error().with_context(node.id);
        }
        if (cancelled(request)) {
          return error(core::ErrorReason::cancelled, "FIR 内核完成后已取消，未发布部分结果");
        }
        filtered_all.insert(filtered_all.end(), filtered.value().begin(), filtered.value().end());
      }
      current = std::move(filtered_all);
      break;
    }
    case NodeKind::iir_filter: {
      const auto coefficients = resolve_filter_coefficients(node, descriptor.sample_rate_hz);
      if (!coefficients) {
        return coefficients.error().with_context(node.id);
      }
      std::vector<data::ComplexSample> filtered_all;
      filtered_all.reserve(current.size());
      for (std::size_t begin = 0; begin < current.size(); begin += maximum_kernel_chunk) {
        if (cancelled(request)) {
          return error(core::ErrorReason::cancelled, "IIR 分块处理已取消，未发布部分结果");
        }
        const auto count = std::min(maximum_kernel_chunk, current.size() - begin);
        auto filtered =
            backend.solve_iir(std::span<const data::ComplexSample>{current}.subspan(begin, count),
                              coefficients.value().numerator, coefficients.value().denominator, states[node_index],
                              begin == 0U ? request.boundary : BoundaryPolicy::preserve_state);
        if (!filtered) {
          return filtered.error().with_context(node.id);
        }
        if (cancelled(request)) {
          return error(core::ErrorReason::cancelled, "IIR 内核完成后已取消，未发布部分结果");
        }
        filtered_all.insert(filtered_all.end(), filtered.value().begin(), filtered.value().end());
      }
      current = std::move(filtered_all);
      break;
    }
    case NodeKind::resample: {
      std::vector<data::ComplexSample> resampled_all;
      for (std::size_t begin = 0; begin < current.size(); begin += maximum_kernel_chunk) {
        if (cancelled(request)) {
          return error(core::ErrorReason::cancelled, "重采样分块处理已取消，未发布部分结果");
        }
        const auto count = std::min(maximum_kernel_chunk, current.size() - begin);
        auto resampled = backend.resample(std::span<const data::ComplexSample>{current}.subspan(begin, count),
                                          node.resample_numerator, node.resample_denominator, node.numerator,
                                          states[node_index], begin + count == current.size());
        if (!resampled) {
          return resampled.error().with_context(node.id);
        }
        if (cancelled(request)) {
          return error(core::ErrorReason::cancelled, "重采样内核完成后已取消，未发布部分结果");
        }
        resampled_all.insert(resampled_all.end(), resampled.value().begin(), resampled.value().end());
      }
      current = std::move(resampled_all);
      descriptor.sample_rate_hz = descriptor.sample_rate_hz * static_cast<double>(node.resample_numerator) /
                                  static_cast<double>(node.resample_denominator);
      break;
    }
    }
    if (current.empty() && node_index + 1U < request.chain.nodes.size()) {
      return error(core::ErrorReason::invalid_argument,
                   "节点输出为空，无法继续执行下游节点；请扩大输入块或调整重采样比例");
    }
    descriptor.amplitude_mode = node.contract.output_unit;
    applied.push_back(node.id);
  }
  if (cancelled(request)) {
    return error(core::ErrorReason::cancelled, "处理链发布前已取消，未发布部分结果");
  }
  const auto output_range = data::SampleRange::from_count(0U, static_cast<std::uint64_t>(current.size()));
  if (!output_range) {
    return output_range.error();
  }
  descriptor.requested_sample_range = output_range.value();
  auto output = to_buffer(std::move(current), kind, request.cancellation);
  if (!output) {
    return output.error();
  }
  if (cancelled(request)) {
    return error(core::ErrorReason::cancelled, "处理链提交结果前已取消，未发布部分结果");
  }
  return ProcessResult{std::move(output.value()), descriptor, std::move(states), std::move(applied),
                       std::string(backend.backend_id())};
}

core::Result<NodePreview> preview_node(ISignalKernelBackend& backend, const data::SignalSlice& samples,
                                       const data::SignalDescriptor& descriptor, const NodeSpec& node) {
  ProcessRequest request{
      samples, descriptor, {"signal-processing-chain/1.0", "1.0.0", {node}}, BoundaryPolicy::zero_pad, nullptr};
  auto processed = process_chain(backend, request, {});
  if (!processed) {
    return processed.error();
  }
  auto before = to_complex(samples);
  if (!before) {
    return before.error();
  }
  auto before_buffer = to_buffer(std::move(before.value()), samples.kind());
  if (!before_buffer) {
    return before_buffer.error();
  }
  NodePreview preview{std::move(before_buffer.value()), processed.value().samples, {}, {}, 0.0};
  FilterCoefficients response_coefficients{node.numerator, node.denominator};
  if (node.kind == NodeKind::fir_filter || node.kind == NodeKind::iir_filter) {
    auto resolved = resolve_filter_coefficients(node, descriptor.sample_rate_hz);
    if (!resolved) {
      return resolved.error();
    }
    response_coefficients = std::move(resolved.value());
  }
  const auto& numerator = response_coefficients.numerator;
  const auto& denominator = response_coefficients.denominator;
  if (!numerator.empty()) {
    constexpr std::size_t point_count = 129U;
    std::vector<double> response_phase;
    response_phase.reserve(point_count);
    preview.response_frequency_hz.reserve(point_count);
    preview.response_magnitude_db.reserve(point_count);
    for (std::size_t point = 0; point < point_count; ++point) {
      const auto omega = std::numbers::pi * static_cast<double>(point) / static_cast<double>(point_count - 1U);
      data::ComplexSample numerator_response{};
      data::ComplexSample denominator_response{1.0, 0.0};
      for (std::size_t index = 0; index < numerator.size(); ++index) {
        numerator_response.real += numerator[index] * std::cos(omega * static_cast<double>(index));
        numerator_response.imag -= numerator[index] * std::sin(omega * static_cast<double>(index));
      }
      if (node.kind == NodeKind::iir_filter) {
        denominator_response = {};
        for (std::size_t index = 0; index < denominator.size(); ++index) {
          denominator_response.real += denominator[index] * std::cos(omega * static_cast<double>(index));
          denominator_response.imag -= denominator[index] * std::sin(omega * static_cast<double>(index));
        }
      }
      const auto numerator_power =
          numerator_response.real * numerator_response.real + numerator_response.imag * numerator_response.imag;
      const auto denominator_power =
          denominator_response.real * denominator_response.real + denominator_response.imag * denominator_response.imag;
      preview.response_frequency_hz.push_back(static_cast<double>(point) * descriptor.sample_rate_hz /
                                              (2.0 * static_cast<double>(point_count - 1U)));
      preview.response_magnitude_db.push_back(
          10.0 * std::log10(std::max(numerator_power / denominator_power, std::numeric_limits<double>::min())));
      auto phase = std::atan2(numerator_response.imag, numerator_response.real) -
                   std::atan2(denominator_response.imag, denominator_response.real);
      if (!response_phase.empty()) {
        while (phase - response_phase.back() > std::numbers::pi) {
          phase -= 2.0 * std::numbers::pi;
        }
        while (phase - response_phase.back() < -std::numbers::pi) {
          phase += 2.0 * std::numbers::pi;
        }
      }
      response_phase.push_back(phase);
    }
    if (node.kind == NodeKind::fir_filter) {
      preview.group_delay_samples = static_cast<double>(numerator.size() - 1U) / 2.0;
    } else {
      const auto omega_step = std::numbers::pi / static_cast<double>(point_count - 1U);
      double delay_sum{};
      constexpr std::size_t low_frequency_intervals = 8U;
      for (std::size_t index = 0; index < low_frequency_intervals; ++index) {
        delay_sum -= (response_phase[index + 1U] - response_phase[index]) / omega_step;
      }
      preview.group_delay_samples = delay_sum / static_cast<double>(low_frequency_intervals);
    }
  }
  return preview;
}

ProcessingProvenance::ProcessingProvenance(std::string source_fingerprint, data::SampleRange source_range,
                                           std::string data_source_version_id, ChainSnapshot chain,
                                           std::string backend_id, SignalSummary input_summary,
                                           SignalSummary output_summary)
    : source_fingerprint_(std::move(source_fingerprint)), source_range_(source_range),
      data_source_version_id_(std::move(data_source_version_id)), chain_(std::move(chain)),
      backend_id_(std::move(backend_id)), input_summary_(std::move(input_summary)),
      output_summary_(std::move(output_summary)) {}

std::string_view ProcessingProvenance::source_fingerprint() const noexcept {
  return source_fingerprint_;
}

const data::SampleRange& ProcessingProvenance::source_range() const noexcept {
  return source_range_;
}

std::string_view ProcessingProvenance::data_source_version_id() const noexcept {
  return data_source_version_id_;
}

const ChainSnapshot& ProcessingProvenance::chain() const noexcept {
  return chain_;
}

std::string_view ProcessingProvenance::backend_id() const noexcept {
  return backend_id_;
}

const SignalSummary& ProcessingProvenance::input_summary() const noexcept {
  return input_summary_;
}

const SignalSummary& ProcessingProvenance::output_summary() const noexcept {
  return output_summary_;
}

core::Result<ProcessingProvenance>
make_processing_provenance(std::string source_fingerprint, data::SampleRange source_range,
                           std::string data_source_version_id, const ChainSnapshot& chain, std::string backend_id,
                           SignalSummary input_summary, SignalSummary output_summary) {
  const auto valid_summary = [](const SignalSummary& summary) {
    return summary.sample_count > 0U && summary.sample_rate_hz > 0.0 && std::isfinite(summary.sample_rate_hz) &&
           !summary.unit.empty() &&
           static_cast<unsigned>(summary.kind) <= static_cast<unsigned>(data::SignalKind::complex);
  };
  if (source_fingerprint.empty() || source_range.empty() || data_source_version_id.empty() || backend_id.empty() ||
      chain.schema != "signal-processing-chain/1.0" || chain.version.empty() || !valid_summary(input_summary) ||
      !valid_summary(output_summary)) {
    return error(core::ErrorReason::invalid_argument, "处理 provenance 源、链、后端或输入输出摘要不完整");
  }
  if (const auto serialized_chain = export_chain_template(chain); !serialized_chain) {
    return serialized_chain.error().with_context("处理 provenance 链快照");
  }
  return ProcessingProvenance{std::move(source_fingerprint),
                              source_range,
                              std::move(data_source_version_id),
                              chain,
                              std::move(backend_id),
                              std::move(input_summary),
                              std::move(output_summary)};
}

core::Result<std::string> serialize_processing_provenance(const ProcessingProvenance& provenance) {
  const auto serialized_chain = export_chain_template(provenance.chain());
  if (!serialized_chain) {
    return serialized_chain.error().with_context("处理 provenance 链快照");
  }
  const auto write_summary = [](std::ostringstream& output, std::string_view label, const SignalSummary& summary) {
    output << label << ' ' << static_cast<unsigned>(summary.kind) << ' ' << summary.sample_count << ' '
           << std::setprecision(17) << summary.sample_rate_hz << ' ' << std::quoted(summary.unit) << '\n';
  };
  std::ostringstream output;
  output << "SSPROV2\n"
         << "SOURCE " << std::quoted(std::string(provenance.source_fingerprint())) << ' '
         << provenance.source_range().begin() << ' ' << provenance.source_range().end() << ' '
         << std::quoted(std::string(provenance.data_source_version_id())) << '\n'
         << "BACKEND " << std::quoted(std::string(provenance.backend_id())) << '\n';
  write_summary(output, "INPUT", provenance.input_summary());
  write_summary(output, "OUTPUT", provenance.output_summary());
  output << "CHAIN_BYTES " << serialized_chain.value().size() << '\n' << serialized_chain.value();
  return output.str();
}

core::Status ProcessingCache::put(std::string node_id, data::SignalBuffer output) {
  if (node_id.empty() || output.size() == 0U) {
    return error(core::ErrorReason::invalid_argument, "节点缓存 ID 或输出为空");
  }
  entries_.insert_or_assign(std::move(node_id), std::move(output));
  return core::Status::success();
}

bool ProcessingCache::contains(std::string_view node_id) const {
  return entries_.find(node_id) != entries_.end();
}

std::size_t ProcessingCache::size() const noexcept {
  return entries_.size();
}

std::vector<std::string> ProcessingCache::invalidate_downstream(const ChainSnapshot& chain,
                                                                std::string_view changed_id) {
  const auto changed = find_node(chain.nodes, changed_id);
  if (changed == chain.nodes.end()) {
    return {};
  }
  std::vector<std::string> invalidated;
  for (auto current = changed; current != chain.nodes.end(); ++current) {
    if (entries_.erase(current->id) > 0U) {
      invalidated.push_back(current->id);
    }
  }
  return invalidated;
}

core::Result<std::string> export_chain_template(const ChainSnapshot& chain) {
  if (chain.schema != "signal-processing-chain/1.0" || chain.version.empty()) {
    return error(core::ErrorReason::invalid_argument, "处理链模板架构或版本无效");
  }
  std::ostringstream output;
  output << "SSCHAIN1 " << std::quoted(chain.version) << ' ' << chain.nodes.size() << '\n';
  for (const auto& node : chain.nodes) {
    if (node.id.empty() || node.implementation_id.empty()) {
      return error(core::ErrorReason::invalid_argument, "处理链模板包含无效节点");
    }
    output << std::quoted(node.id) << ' ' << static_cast<unsigned>(node.kind) << ' '
           << std::quoted(node.implementation_id) << ' ' << node.enabled << ' ' << node.contract.accepts_real << ' '
           << node.contract.accepts_complex << ' ' << node.contract.produces_complex << ' ' << std::setprecision(17)
           << node.contract.sample_rate_numerator << ' ' << node.contract.sample_rate_denominator << ' '
           << std::quoted(node.contract.input_unit) << ' ' << std::quoted(node.contract.output_unit) << ' '
           << static_cast<unsigned>(node.filter_shape) << ' ' << static_cast<unsigned>(node.real_to_complex) << ' '
           << node.gain << ' ' << node.additive_offset << ' ' << node.iq_gain_balance << ' ' << node.iq_phase_radians
           << ' ' << node.frequency_shift_hz << ' ' << node.resample_numerator << ' ' << node.resample_denominator
           << ' ' << node.anti_alias_cutoff_hz << ' ' << node.anti_alias_stopband_db << ' ' << node.numerator.size();
    for (const auto value : node.numerator) {
      output << ' ' << std::setprecision(17) << value;
    }
    output << ' ' << node.denominator.size();
    for (const auto value : node.denominator) {
      output << ' ' << std::setprecision(17) << value;
    }
    output << ' ' << node.parameters.size();
    for (const auto& [key, value] : node.parameters) {
      output << ' ' << std::quoted(key) << ' ' << std::setprecision(17) << value;
    }
    output << '\n';
  }
  return output.str();
}

core::Result<ChainSnapshot> import_chain_template(std::string_view text,
                                                  std::span<const std::string> available_implementations) {
  std::istringstream input{std::string(text)};
  std::string magic;
  ChainSnapshot chain;
  std::size_t count{};
  if (!(input >> magic >> std::quoted(chain.version) >> count) || magic != "SSCHAIN1" || count > 1024U) {
    return error(core::ErrorReason::invalid_argument, "处理链模板头无效或节点过多");
  }
  if (chain.version != "1.0.0") {
    return error(core::ErrorReason::unavailable, "处理链模板版本不兼容", chain.version);
  }
  std::vector<std::string> ids;
  chain.nodes.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    NodeSpec node;
    unsigned kind{};
    unsigned filter_shape{};
    unsigned real_to_complex{};
    std::size_t numerator_count{};
    std::size_t denominator_count{};
    std::size_t parameter_count{};
    if (!(input >> std::quoted(node.id) >> kind >> std::quoted(node.implementation_id) >> node.enabled >>
          node.contract.accepts_real >> node.contract.accepts_complex >> node.contract.produces_complex >>
          node.contract.sample_rate_numerator >> node.contract.sample_rate_denominator >>
          std::quoted(node.contract.input_unit) >> std::quoted(node.contract.output_unit) >> filter_shape >>
          real_to_complex >> node.gain >> node.additive_offset >> node.iq_gain_balance >> node.iq_phase_radians >>
          node.frequency_shift_hz >> node.resample_numerator >> node.resample_denominator >>
          node.anti_alias_cutoff_hz >> node.anti_alias_stopband_db >> numerator_count) ||
        node.id.empty() || std::ranges::find(ids, node.id) != ids.end() ||
        kind > static_cast<unsigned>(NodeKind::resample) || filter_shape > static_cast<unsigned>(FilterShape::custom) ||
        real_to_complex > static_cast<unsigned>(RealToComplexMode::quadrature_mixer) || numerator_count > 65'536U) {
      return error(core::ErrorReason::invalid_argument, "处理链模板节点无效");
    }
    if (std::ranges::find(available_implementations, node.implementation_id) == available_implementations.end()) {
      return error(core::ErrorReason::unavailable, "缺少处理链依赖实现", node.implementation_id);
    }
    node.kind = static_cast<NodeKind>(kind);
    node.filter_shape = static_cast<FilterShape>(filter_shape);
    node.real_to_complex = static_cast<RealToComplexMode>(real_to_complex);
    node.numerator.resize(numerator_count);
    for (auto& value : node.numerator) {
      if (!(input >> value) || !std::isfinite(value)) {
        return error(core::ErrorReason::invalid_argument, "处理链模板分子/FIR 系数无效");
      }
    }
    if (!(input >> denominator_count) || denominator_count > 65'536U) {
      return error(core::ErrorReason::invalid_argument, "处理链模板分母系数数量无效");
    }
    node.denominator.resize(denominator_count);
    for (auto& value : node.denominator) {
      if (!(input >> value) || !std::isfinite(value)) {
        return error(core::ErrorReason::invalid_argument, "处理链模板分母系数无效");
      }
    }
    if (!(input >> parameter_count) || parameter_count > 256U) {
      return error(core::ErrorReason::invalid_argument, "处理链模板参数数量无效");
    }
    for (std::size_t parameter_index = 0; parameter_index < parameter_count; ++parameter_index) {
      std::string key;
      double value{};
      if (!(input >> std::quoted(key) >> value) || key.empty() || !std::isfinite(value)) {
        return error(core::ErrorReason::invalid_argument, "处理链模板参数无效");
      }
      node.parameters.emplace(std::move(key), value);
    }
    ids.push_back(node.id);
    chain.nodes.push_back(std::move(node));
  }
  std::string trailing;
  if (input >> trailing) {
    return error(core::ErrorReason::invalid_argument, "处理链模板含多余内容");
  }
  return chain;
}

core::Result<std::vector<std::byte>> export_bit_exact_bypass(std::span<const std::byte> original_bytes,
                                                             std::uint64_t frame_bytes,
                                                             const data::SampleRange& range) {
  if (frame_bytes == 0 || range.begin() > std::numeric_limits<std::uint64_t>::max() / frame_bytes ||
      range.end() > std::numeric_limits<std::uint64_t>::max() / frame_bytes) {
    return error(core::ErrorReason::invalid_argument, "位精确导出边界或帧大小无效");
  }
  const auto begin = range.begin() * frame_bytes;
  const auto end = range.end() * frame_bytes;
  if (end > original_bytes.size() || begin > end) {
    return error(core::ErrorReason::invalid_argument, "位精确导出范围超出原始字节");
  }
  return std::vector<std::byte>(original_bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                                original_bytes.begin() + static_cast<std::ptrdiff_t>(end));
}

} // namespace signal::dsp
