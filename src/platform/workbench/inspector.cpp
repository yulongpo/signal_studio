#include "signal_studio/workbench/inspector.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace signal::workbench {
namespace {

[[nodiscard]] core::Status failure(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::workbench, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] bool valid_text(std::string_view value) {
  return !value.empty() && value.find_first_of("\r\n\t") == std::string_view::npos;
}

[[nodiscard]] bool known_view(InspectorViewKind kind) noexcept {
  return kind >= InspectorViewKind::time_domain && kind <= InspectorViewKind::instantaneous_frequency;
}

[[nodiscard]] ViewApplicability applicable(InspectorViewKind kind, std::string range, std::string unit,
                                           std::string preprocessing) {
  return {kind, true, {}, std::move(range), std::move(unit), std::move(preprocessing)};
}

[[nodiscard]] ViewApplicability unavailable(InspectorViewKind kind, std::string reason) {
  return {kind, false, std::move(reason), {}, {}, {}};
}

} // namespace

std::string_view inspector_view_name(InspectorViewKind kind) noexcept {
  switch (kind) {
  case InspectorViewKind::time_domain:
    return "time-domain";
  case InspectorViewKind::spectrum:
    return "spectrum";
  case InspectorViewKind::stft:
    return "stft";
  case InspectorViewKind::constellation:
    return "constellation";
  case InspectorViewKind::eye_diagram:
    return "eye-diagram";
  case InspectorViewKind::amplitude_histogram:
    return "amplitude-histogram";
  case InspectorViewKind::phase_histogram:
    return "phase-histogram";
  case InspectorViewKind::instantaneous_frequency:
    return "instantaneous-frequency";
  }
  return "unknown";
}

core::Result<InspectorChannelState> make_inspector_channel_state(std::string channel_id, std::string channel_version,
                                                                 std::string data_source_version_id,
                                                                 const data::SignalDescriptor& descriptor,
                                                                 std::optional<double> symbol_rate_baud,
                                                                 std::string synchronization_source) {
  if (const auto status = descriptor.validate(); !status) {
    return status.with_context("Inspector 输入描述符");
  }
  InspectorChannelState state;
  state.channel_id = std::move(channel_id);
  state.channel_version = std::move(channel_version);
  state.data_source_version_id = std::move(data_source_version_id);
  state.signal_kind = descriptor.signal_kind;
  state.views.push_back(applicable(InspectorViewKind::time_domain, "当前通道时间视窗", "线性幅值", "通道处理链输出"));
  state.views.push_back(applicable(InspectorViewKind::spectrum, "通道有效带宽", "dB/Hz", "Hann 窗与 ENBW 归一化"));
  state.views.push_back(applicable(InspectorViewKind::stft, "通道时间×频率视窗", "dB/Hz", "Hann 窗分块 STFT"));
  state.views.push_back(
      descriptor.signal_kind == data::SignalKind::complex
          ? applicable(InspectorViewKind::constellation, "I/Q 平面", "归一化幅值", "有界抽样；不推断调制方式")
          : unavailable(InspectorViewKind::constellation, "仅复信号具有 I/Q 星座输入"));
  if (symbol_rate_baud && std::isfinite(*symbol_rate_baud) && *symbol_rate_baud > 0.0 &&
      valid_text(synchronization_source)) {
    state.eye_diagram.symbol_rate_baud = symbol_rate_baud;
    state.eye_diagram.synchronization_source = std::move(synchronization_source);
    state.views.push_back(applicable(InspectorViewKind::eye_diagram, "两个符号周期", "归一化幅值",
                                     "符号率与同步来源均由用户或算法结果明确提供"));
  } else {
    state.views.push_back(unavailable(InspectorViewKind::eye_diagram, "需要明确符号率和同步来源，不使用无依据默认值"));
  }
  state.views.push_back(applicable(InspectorViewKind::amplitude_histogram, "[最小幅值, 最大幅值]", "线性幅值",
                                   "通道处理链输出的有限样本"));
  state.views.push_back(
      descriptor.signal_kind == data::SignalKind::complex
          ? applicable(InspectorViewKind::phase_histogram, "[-π, π]", "rad", "atan2(Q, I)，排除非有限样本")
          : unavailable(InspectorViewKind::phase_histogram, "实信号没有独立 I/Q 相位"));
  state.views.push_back(
      descriptor.signal_kind == data::SignalKind::complex
          ? applicable(InspectorViewKind::instantaneous_frequency, "当前通道时间视窗", "Hz",
                       "展开相位的一阶差分；采样率来自描述符")
          : unavailable(InspectorViewKind::instantaneous_frequency, "实信号需先由批准处理链生成解析信号"));
  if (const auto status = validate_inspector_channel(state); !status) {
    return status;
  }
  return state;
}

core::Status validate_inspector_channel(const InspectorChannelState& state) {
  if (!valid_text(state.channel_id) || !valid_text(state.channel_version) ||
      !valid_text(state.data_source_version_id) || state.views.size() < 5U ||
      state.constellation.maximum_points == 0U || state.constellation.maximum_points > 10'000'000U) {
    return failure(core::ErrorReason::invalid_argument, "Inspector 通道身份、视图或星座抽样上限无效");
  }
  std::vector<InspectorViewKind> kinds;
  for (const auto& view : state.views) {
    if (!known_view(view.kind) ||
        (view.applicable && (view.range.empty() || view.unit.empty() || view.preprocessing.empty())) ||
        (!view.applicable && view.reason.empty()) || std::ranges::find(kinds, view.kind) != kinds.end()) {
      return failure(core::ErrorReason::invalid_argument, "Inspector 视图适用性、范围、单位或预处理无效");
    }
    kinds.push_back(view.kind);
  }
  const auto eye =
      std::ranges::find_if(state.views, [](const auto& view) { return view.kind == InspectorViewKind::eye_diagram; });
  if (eye == state.views.end() ||
      (eye->applicable && (!state.eye_diagram.symbol_rate_baud || *state.eye_diagram.symbol_rate_baud <= 0.0 ||
                           state.eye_diagram.synchronization_source.empty()))) {
    return failure(core::ErrorReason::invalid_argument, "眼图缺少明确符号率或同步来源");
  }
  for (const auto& result : state.results) {
    if (!valid_text(result.result_id) || !valid_text(result.channel_version) || !valid_text(result.parameter_version)) {
      return failure(core::ErrorReason::invalid_argument, "Inspector 结果绑定缺少版本");
    }
  }
  return core::Status::success();
}

bool inspector_result_is_current(const InspectorResultBinding& result, std::string_view current_channel_version,
                                 std::string_view current_parameter_version) noexcept {
  return result.channel_version == current_channel_version && result.parameter_version == current_parameter_version;
}

core::Result<RestoredInspectorLayout> restore_inspector_layout(const InspectorLayoutTemplate& layout,
                                                               std::span<const std::string> available_plugins) {
  if (!valid_text(layout.id) || layout.views.empty() ||
      std::ranges::any_of(layout.views, [](auto view) { return !known_view(view); })) {
    return failure(core::ErrorReason::invalid_argument, "Inspector 布局模板无效");
  }
  RestoredInspectorLayout restored{layout};
  for (const auto& plugin : layout.required_plugins) {
    if (!valid_text(plugin)) {
      return failure(core::ErrorReason::invalid_argument, "Inspector 布局模板包含无效插件 ID");
    }
    if (std::ranges::find(available_plugins, plugin) == available_plugins.end()) {
      restored.missing_plugins.push_back(plugin);
    }
  }
  restored.degraded = !restored.missing_plugins.empty();
  return restored;
}

core::Status InspectorStateStore::upsert(InspectorChannelState state) {
  if (const auto status = validate_inspector_channel(state); !status) {
    return status;
  }
  channels_.insert_or_assign(state.channel_id, std::move(state));
  return core::Status::success();
}

std::optional<InspectorChannelState> InspectorStateStore::find(std::string_view channel_id) const {
  const auto found = channels_.find(channel_id);
  return found == channels_.end() ? std::nullopt : std::optional<InspectorChannelState>{found->second};
}

std::vector<InspectorChannelState> InspectorStateStore::channels() const {
  std::vector<InspectorChannelState> result;
  result.reserve(channels_.size());
  for (const auto& [id, state] : channels_) {
    static_cast<void>(id);
    result.push_back(state);
  }
  return result;
}

core::Result<std::string> InspectorStateStore::serialize() const {
  std::ostringstream output;
  output << "signal-inspector-state/1.0\n" << channels_.size() << '\n';
  for (const auto& [id, state] : channels_) {
    static_cast<void>(id);
    output << std::quoted(state.channel_id) << ' ' << std::quoted(state.channel_version) << ' '
           << std::quoted(state.data_source_version_id) << ' ' << static_cast<unsigned>(state.signal_kind) << ' '
           << state.constellation.maximum_points << ' ' << state.constellation.density_mode << ' '
           << state.constellation.reference_grid << ' ' << state.constellation.compare_before_after << '\n';
    output << state.eye_diagram.symbol_rate_baud.has_value() << ' '
           << (state.eye_diagram.symbol_rate_baud ? *state.eye_diagram.symbol_rate_baud : 0.0) << ' '
           << std::quoted(state.eye_diagram.synchronization_source) << '\n';
    output << state.views.size() << '\n';
    for (const auto& view : state.views) {
      output << static_cast<unsigned>(view.kind) << ' ' << view.applicable << ' ' << std::quoted(view.reason) << ' '
             << std::quoted(view.range) << ' ' << std::quoted(view.unit) << ' ' << std::quoted(view.preprocessing)
             << '\n';
    }
    output << state.results.size() << '\n';
    for (const auto& result : state.results) {
      output << std::quoted(result.result_id) << ' ' << std::quoted(result.channel_version) << ' '
             << std::quoted(result.parameter_version) << '\n';
    }
    output << state.parameters.size() << '\n';
    for (const auto& [key, value] : state.parameters) {
      output << std::quoted(key) << ' ' << std::quoted(value) << '\n';
    }
  }
  return output.str();
}

core::Result<InspectorStateStore> InspectorStateStore::parse(std::string_view serialized) {
  std::istringstream input{std::string{serialized}};
  std::string signature;
  std::getline(input, signature);
  std::size_t channel_count{};
  if (signature != "signal-inspector-state/1.0" || !(input >> channel_count) || channel_count > 4096U) {
    return failure(core::ErrorReason::invalid_argument, "Inspector 状态版本或通道数量无效");
  }
  InspectorStateStore store;
  for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
    InspectorChannelState state;
    unsigned signal_kind{};
    if (!(input >> std::quoted(state.channel_id) >> std::quoted(state.channel_version) >>
          std::quoted(state.data_source_version_id) >> signal_kind >> state.constellation.maximum_points >>
          state.constellation.density_mode >> state.constellation.reference_grid >>
          state.constellation.compare_before_after) ||
        signal_kind > static_cast<unsigned>(data::SignalKind::complex)) {
      return failure(core::ErrorReason::invalid_argument, "Inspector 通道状态损坏");
    }
    state.signal_kind = static_cast<data::SignalKind>(signal_kind);
    bool has_symbol_rate{};
    double symbol_rate{};
    if (!(input >> has_symbol_rate >> symbol_rate >> std::quoted(state.eye_diagram.synchronization_source))) {
      return failure(core::ErrorReason::invalid_argument, "Inspector 眼图状态损坏");
    }
    if (has_symbol_rate) {
      state.eye_diagram.symbol_rate_baud = symbol_rate;
    }
    std::size_t view_count{};
    if (!(input >> view_count) || view_count > 64U) {
      return failure(core::ErrorReason::invalid_argument, "Inspector 视图数量无效");
    }
    for (std::size_t index = 0; index < view_count; ++index) {
      ViewApplicability view;
      unsigned kind{};
      if (!(input >> kind >> view.applicable >> std::quoted(view.reason) >> std::quoted(view.range) >>
            std::quoted(view.unit) >> std::quoted(view.preprocessing)) ||
          kind > static_cast<unsigned>(InspectorViewKind::instantaneous_frequency)) {
        return failure(core::ErrorReason::invalid_argument, "Inspector 视图状态损坏");
      }
      view.kind = static_cast<InspectorViewKind>(kind);
      state.views.push_back(std::move(view));
    }
    std::size_t result_count{};
    if (!(input >> result_count) || result_count > 100'000U) {
      return failure(core::ErrorReason::invalid_argument, "Inspector 结果数量无效");
    }
    for (std::size_t index = 0; index < result_count; ++index) {
      InspectorResultBinding result;
      if (!(input >> std::quoted(result.result_id) >> std::quoted(result.channel_version) >>
            std::quoted(result.parameter_version))) {
        return failure(core::ErrorReason::invalid_argument, "Inspector 结果状态损坏");
      }
      state.results.push_back(std::move(result));
    }
    std::size_t parameter_count{};
    if (!(input >> parameter_count) || parameter_count > 4096U) {
      return failure(core::ErrorReason::invalid_argument, "Inspector 参数数量无效");
    }
    for (std::size_t index = 0; index < parameter_count; ++index) {
      std::string key;
      std::string value;
      if (!(input >> std::quoted(key) >> std::quoted(value)) ||
          !state.parameters.emplace(std::move(key), std::move(value)).second) {
        return failure(core::ErrorReason::invalid_argument, "Inspector 参数状态损坏");
      }
    }
    if (const auto status = store.upsert(std::move(state)); !status) {
      return status;
    }
  }
  input >> std::ws;
  if (!input.eof()) {
    return failure(core::ErrorReason::invalid_argument, "Inspector 状态包含尾随数据");
  }
  return store;
}

} // namespace signal::workbench
