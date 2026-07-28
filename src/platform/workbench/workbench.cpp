#include "signal_studio/workbench/workbench.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

namespace signal::workbench {
namespace {

[[nodiscard]] core::Status failure(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::workbench, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] bool valid_token(std::string_view value) {
  return !value.empty() && value.find_first_of("\t\r\n") == std::string_view::npos;
}

[[nodiscard]] core::Result<PanelArea> parse_panel_area(std::string_view text) {
  std::uint32_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value > static_cast<std::uint32_t>(PanelArea::center)) {
    return failure(core::ErrorReason::invalid_argument, "面板区域无效");
  }
  return static_cast<PanelArea>(value);
}

} // namespace

core::Status ServiceRegistry::add(ServiceEntry entry) {
  if (!valid_token(entry.id) || !valid_token(entry.contract) || !entry.instance) {
    return failure(core::ErrorReason::invalid_argument, "服务注册需要唯一 ID、契约名称和有效实例");
  }
  if (services_.contains(entry.id)) {
    return failure(core::ErrorReason::invalid_argument, "服务 ID 已注册");
  }
  services_.emplace(entry.id, std::move(entry));
  return core::Status::success();
}

std::shared_ptr<void> ServiceRegistry::resolve(std::string_view id) const {
  const auto entry = services_.find(id);
  return entry == services_.end() ? std::shared_ptr<void>{} : entry->second.instance;
}

std::vector<ServiceId> ServiceRegistry::ids() const {
  std::vector<ServiceId> result;
  result.reserve(services_.size());
  for (const auto& [id, entry] : services_) {
    static_cast<void>(entry);
    result.push_back(id);
  }
  return result;
}

core::Status CommandRegistry::add(Command command) {
  if (!valid_token(command.id) || !valid_token(command.label) || !valid_token(command.description) ||
      !command.can_execute || !command.execute) {
    return failure(core::ErrorReason::invalid_argument, "命令需要 ID、标签、说明、CanExecute 和执行函数");
  }
  for (const auto& [id, existing] : commands_) {
    static_cast<void>(id);
    if (!command.shortcut.empty() && existing.shortcut == command.shortcut) {
      return failure(core::ErrorReason::invalid_argument, "命令快捷键冲突");
    }
  }
  if (commands_.contains(command.id)) {
    return failure(core::ErrorReason::invalid_argument, "命令 ID 已注册");
  }
  commands_.emplace(command.id, std::move(command));
  return core::Status::success();
}

core::Status CommandRegistry::remove(std::string_view id) {
  if (commands_.erase(std::string(id)) == 0) {
    return failure(core::ErrorReason::invalid_argument, "命令不存在");
  }
  return core::Status::success();
}

core::Result<bool> CommandRegistry::can_execute(std::string_view id) const {
  const auto command = commands_.find(id);
  if (command == commands_.end()) {
    return failure(core::ErrorReason::invalid_argument, "命令不存在");
  }
  return command->second.can_execute();
}

core::Status CommandRegistry::execute(std::string_view id) const {
  const auto command = commands_.find(id);
  if (command == commands_.end()) {
    return failure(core::ErrorReason::invalid_argument, "命令不存在");
  }
  if (!command->second.can_execute()) {
    return failure(core::ErrorReason::unavailable, "命令当前不可执行");
  }
  return command->second.execute();
}

std::vector<Command> CommandRegistry::commands() const {
  std::vector<Command> result;
  result.reserve(commands_.size());
  for (const auto& [id, command] : commands_) {
    static_cast<void>(id);
    result.push_back(command);
  }
  return result;
}

core::Status PanelRegistry::add(PanelDescriptor descriptor) {
  if (!valid_token(descriptor.id) || !valid_token(descriptor.title) ||
      !valid_token(descriptor.accessible_description)) {
    return failure(core::ErrorReason::invalid_argument, "面板需要 ID、标题和可访问说明");
  }
  if (panels_.contains(descriptor.id)) {
    return failure(core::ErrorReason::invalid_argument, "面板 ID 已注册");
  }
  panels_.emplace(descriptor.id, std::move(descriptor));
  return core::Status::success();
}

std::optional<PanelDescriptor> PanelRegistry::find(std::string_view id) const {
  const auto panel = panels_.find(id);
  return panel == panels_.end() ? std::nullopt : std::optional<PanelDescriptor>{panel->second};
}

std::vector<PanelDescriptor> PanelRegistry::panels() const {
  std::vector<PanelDescriptor> result;
  result.reserve(panels_.size());
  for (const auto& [id, panel] : panels_) {
    static_cast<void>(id);
    result.push_back(panel);
  }
  return result;
}

core::Status validate_theme(const ThemeTokens& tokens) {
  const auto is_color = [](std::string_view value) {
    return value.size() == 7 && value.front() == '#' &&
           value.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string_view::npos;
  };
  if (!is_color(tokens.canvas) || !is_color(tokens.panel) || !is_color(tokens.primary_text) ||
      !is_color(tokens.muted_text) || !is_color(tokens.accent) || !is_color(tokens.error) || tokens.ui_font.empty() ||
      tokens.numeric_font.empty() || tokens.focus_ring_width < 2 || tokens.pointer_target < 28) {
    return failure(core::ErrorReason::invalid_argument, "主题令牌必须满足颜色、字体、2 px 焦点环和 28 px 命中区约束");
  }
  return core::Status::success();
}

core::Status validate_parameter(const ParameterDescriptor& parameter) {
  if (!valid_token(parameter.id) || !valid_token(parameter.label) || !valid_token(parameter.unit) ||
      !std::isfinite(parameter.minimum) || !std::isfinite(parameter.maximum) ||
      !std::isfinite(parameter.default_value) || !std::isfinite(parameter.value) ||
      parameter.minimum >= parameter.maximum || parameter.default_value < parameter.minimum ||
      parameter.default_value > parameter.maximum || parameter.value < parameter.minimum ||
      parameter.value > parameter.maximum) {
    return failure(core::ErrorReason::invalid_argument, "参数必须显示单位、范围、合法默认值并通过即时校验");
  }
  return core::Status::success();
}

core::Result<std::string> serialize_layout(const WorkbenchLayout& layout) {
  std::ostringstream output;
  for (const auto& [id, area] : layout.areas) {
    if (!valid_token(id)) {
      return failure(core::ErrorReason::invalid_argument, "布局面板 ID 无效");
    }
    output << "A\t" << id << '\t' << static_cast<std::uint32_t>(area) << '\n';
  }
  for (const auto& id : layout.visible_panels) {
    if (!valid_token(id)) {
      return failure(core::ErrorReason::invalid_argument, "可见面板 ID 无效");
    }
    output << "V\t" << id << '\n';
  }
  output << "S\t";
  constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : layout.native_state) {
    output << hex[(byte >> 4U) & 0x0FU] << hex[byte & 0x0FU];
  }
  output << '\n';
  return output.str();
}

core::Result<WorkbenchLayout> parse_layout(std::string_view serialized) {
  WorkbenchLayout layout;
  std::istringstream input{std::string(serialized)};
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    if (line.starts_with("A\t")) {
      const auto separator = line.find('\t', 2);
      if (separator == std::string::npos) {
        return failure(core::ErrorReason::invalid_argument, "布局区域行损坏");
      }
      const auto id = line.substr(2, separator - 2);
      const auto area = parse_panel_area(std::string_view(line).substr(separator + 1));
      if (!valid_token(id) || !area) {
        return failure(core::ErrorReason::invalid_argument, "布局区域行无效");
      }
      layout.areas.emplace(id, area.value());
    } else if (line.starts_with("V\t")) {
      const auto id = line.substr(2);
      if (!valid_token(id)) {
        return failure(core::ErrorReason::invalid_argument, "布局可见面板无效");
      }
      layout.visible_panels.push_back(id);
    } else if (line.starts_with("S\t")) {
      const auto hex = std::string_view(line).substr(2);
      if (hex.size() % 2 != 0) {
        return failure(core::ErrorReason::invalid_argument, "布局原生状态长度无效");
      }
      for (std::size_t index = 0; index < hex.size(); index += 2) {
        std::uint32_t byte{};
        const auto result = std::from_chars(hex.data() + index, hex.data() + index + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != hex.data() + index + 2) {
          return failure(core::ErrorReason::invalid_argument, "布局原生状态不是十六进制");
        }
        layout.native_state.push_back(static_cast<std::uint8_t>(byte));
      }
    } else {
      return failure(core::ErrorReason::invalid_argument, "布局包含未知记录");
    }
  }
  return layout;
}

core::Status handle_escape(ModalBehavior& behavior) {
  if (!behavior.focus_trapped || !behavior.escape_cancels) {
    return failure(core::ErrorReason::unavailable, "模态对话框必须锁定焦点并统一 Escape 取消语义");
  }
  behavior.cancellation_requested = true;
  behavior.live_region_text = "已请求取消";
  return core::Status::success();
}

} // namespace signal::workbench
