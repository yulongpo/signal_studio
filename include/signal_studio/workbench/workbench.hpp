#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/visualization/visualization.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace signal::workbench {

using ServiceId = std::string;
using CommandId = std::string;

struct ServiceEntry final {
  ServiceId id;
  std::string contract;
  std::shared_ptr<void> instance;
};

class ServiceRegistry final {
public:
  [[nodiscard]] core::Status add(ServiceEntry entry);
  [[nodiscard]] std::shared_ptr<void> resolve(std::string_view id) const;
  [[nodiscard]] std::vector<ServiceId> ids() const;

private:
  std::map<ServiceId, ServiceEntry, std::less<>> services_;
};

struct Command final {
  CommandId id;
  std::string label;
  std::string description;
  std::string shortcut;
  std::string parameter_schema;
  std::function<bool()> can_execute;
  std::function<core::Status()> execute;
};

class CommandRegistry final {
public:
  [[nodiscard]] core::Status add(Command command);
  [[nodiscard]] core::Status remove(std::string_view id);
  [[nodiscard]] core::Result<bool> can_execute(std::string_view id) const;
  [[nodiscard]] core::Status execute(std::string_view id) const;
  [[nodiscard]] std::vector<Command> commands() const;

private:
  std::map<CommandId, Command, std::less<>> commands_;
};

enum class PanelArea : std::uint8_t { left, right, bottom, center };

struct PanelDescriptor final {
  std::string id;
  std::string title;
  PanelArea default_area{PanelArea::left};
  bool closable{true};
  bool movable{true};
  std::string accessible_description;
  friend bool operator==(const PanelDescriptor&, const PanelDescriptor&) = default;
};

class PanelRegistry final {
public:
  [[nodiscard]] core::Status add(PanelDescriptor descriptor);
  [[nodiscard]] std::optional<PanelDescriptor> find(std::string_view id) const;
  [[nodiscard]] std::vector<PanelDescriptor> panels() const;

private:
  std::map<std::string, PanelDescriptor, std::less<>> panels_;
};

struct DiagnosticItem final {
  std::string key;
  std::string value;
  std::string state_text;
  std::string action;
  friend bool operator==(const DiagnosticItem&, const DiagnosticItem&) = default;
};

struct DiagnosticsSnapshot final {
  std::string generated_at_utc;
  std::vector<DiagnosticItem> items;
};

class IDiagnosticsProvider {
public:
  virtual ~IDiagnosticsProvider() noexcept = default;
  [[nodiscard]] virtual DiagnosticsSnapshot snapshot() const = 0;
};

struct ThemeTokens final {
  std::string canvas{"#08111F"};
  std::string panel{"#0E1B2D"};
  std::string primary_text{"#E5F1FF"};
  std::string muted_text{"#8FA8C2"};
  std::string accent{"#20D3EE"};
  std::string error{"#FF6B7A"};
  std::string ui_font{"Microsoft YaHei UI"};
  std::string numeric_font{"Cascadia Mono"};
  std::uint32_t focus_ring_width{2};
  std::uint32_t pointer_target{28};
  friend bool operator==(const ThemeTokens&, const ThemeTokens&) = default;
};

[[nodiscard]] core::Status validate_theme(const ThemeTokens& tokens);

struct ParameterDescriptor final {
  std::string id;
  std::string label;
  std::string unit;
  double minimum{};
  double maximum{};
  double default_value{};
  double value{};
  friend bool operator==(const ParameterDescriptor&, const ParameterDescriptor&) = default;
};

[[nodiscard]] core::Status validate_parameter(const ParameterDescriptor& parameter);

struct WorkbenchLayout final {
  std::map<std::string, PanelArea, std::less<>> areas;
  std::vector<std::string> visible_panels;
  std::vector<std::uint8_t> native_state;
  friend bool operator==(const WorkbenchLayout&, const WorkbenchLayout&) = default;
};

[[nodiscard]] core::Result<std::string> serialize_layout(const WorkbenchLayout& layout);
[[nodiscard]] core::Result<WorkbenchLayout> parse_layout(std::string_view serialized);

struct ModalBehavior final {
  bool focus_trapped{true};
  bool escape_cancels{true};
  bool cancellation_requested{};
  std::string live_region_text;
};

[[nodiscard]] core::Status handle_escape(ModalBehavior& behavior);

struct InspectorEntry final {
  std::string label;
  std::string value;
  friend bool operator==(const InspectorEntry&, const InspectorEntry&) = default;
};

struct TaskCenterEntry final {
  std::string task;
  std::string state_text;
  std::string progress_text;
  std::string backend;
  std::string source;
  friend bool operator==(const TaskCenterEntry&, const TaskCenterEntry&) = default;
};

struct ResultCenterEntry final {
  std::string result;
  std::string validity_text;
  std::string data_source_version_id;
  std::string location_action;
  friend bool operator==(const ResultCenterEntry&, const ResultCenterEntry&) = default;
};

struct NavigationEntry final {
  std::string label;
  std::string badge;
  std::uint32_t depth{};
  bool current{};
  friend bool operator==(const NavigationEntry&, const NavigationEntry&) = default;
};

/// 工作台只呈现宿主注入的真实状态；默认值明确表示当前没有外部快照。
struct WorkbenchContent final {
  std::string status_text{"● 就绪"};
  std::string resource_text{"资源快照未提供"};
  std::string project_name;
  std::string source_summary;
  std::vector<NavigationEntry> navigation;
  std::vector<InspectorEntry> inspector;
  std::vector<TaskCenterEntry> tasks;
  std::vector<ResultCenterEntry> results;
  friend bool operator==(const WorkbenchContent&, const WorkbenchContent&) = default;
};

struct WorkbenchConfiguration final {
  std::string application_name{"Signal Platform"};
  std::string window_title{"可视化工作台"};
  std::uint32_t minimum_width{1280};
  std::uint32_t minimum_height{720};
  ThemeTokens theme;
  WorkbenchContent content;
};

class IWorkbenchWindow {
public:
  virtual ~IWorkbenchWindow() noexcept = default;
  [[nodiscard]] virtual void* native_handle() noexcept = 0;
  virtual void show() = 0;
  [[nodiscard]] virtual core::Status restore_layout(const WorkbenchLayout& layout) = 0;
  [[nodiscard]] virtual WorkbenchLayout save_layout() const = 0;
  [[nodiscard]] virtual std::vector<std::string> visible_panels() const = 0;
  [[nodiscard]] virtual std::string accessibility_summary() const = 0;
};

/// 创建可复用 Qt Widgets 工作台；Qt 只存在于私有实现和不透明原生句柄中。
[[nodiscard]] std::unique_ptr<IWorkbenchWindow>
make_workbench_window(WorkbenchConfiguration configuration,
                      std::unique_ptr<visualization::IAnalysisWorkspace> center_workspace,
                      std::shared_ptr<CommandRegistry> commands, std::shared_ptr<PanelRegistry> panels,
                      std::shared_ptr<IDiagnosticsProvider> diagnostics = {});

} // namespace signal::workbench
