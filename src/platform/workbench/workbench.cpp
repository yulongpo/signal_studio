#include "signal_studio/workbench/workbench.hpp"

#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace signal::workbench {

namespace {
core::Status wb_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::workbench, reason}, std::move(message));
}

class ServiceRegistry final : public IServiceRegistry {
 public:
  core::Status register_service(ServiceId id, std::shared_ptr<void> service) override {
    if (id.value.empty()) {
      return wb_failure(core::ErrorReason::invalid_argument, "service id must be non-empty");
    }
    services_[std::move(id.value)] = std::move(service);
    return core::Status::success();
  }
  std::shared_ptr<void> resolve(const ServiceId& id) const noexcept override {
    auto it = services_.find(id.value);
    return it == services_.end() ? nullptr : it->second;
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<void>> services_;
};

class CommandRegistry final : public ICommandRegistry {
 public:
  core::Status register_command(std::string command_id, CommandAction action) override {
    if (command_id.empty()) {
      return wb_failure(core::ErrorReason::invalid_argument, "command id must be non-empty");
    }
    commands_[std::move(command_id)] = std::move(action);
    return core::Status::success();
  }
  core::Status invoke(std::string_view command_id) const override {
    auto it = commands_.find(std::string(command_id));
    if (it == commands_.end()) {
      return wb_failure(core::ErrorReason::unavailable, "unknown command: " + std::string(command_id));
    }
    if (!it->second) {
      return wb_failure(core::ErrorReason::internal_failure, "command has no action: " + std::string(command_id));
    }
    return it->second();
  }
  std::vector<std::string> registered_command_ids() const override {
    std::vector<std::string> ids;
    ids.reserve(commands_.size());
    for (const auto& [id, _] : commands_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
  }

 private:
  std::unordered_map<std::string, CommandAction> commands_;
};
}  // namespace

core::Status PanelFactory::register_creator(std::string panel_id, PanelCreator creator) {
  if (panel_id.empty()) {
    return wb_failure(core::ErrorReason::invalid_argument, "panel id must be non-empty");
  }
  creators_[std::move(panel_id)] = std::move(creator);
  return core::Status::success();
}

core::Result<std::unique_ptr<IPanel>> PanelFactory::create(const PanelContext& context) {
  auto it = creators_.find(context.panel_id);
  if (it == creators_.end()) {
    return wb_failure(core::ErrorReason::unavailable, "no creator registered for panel: " + context.panel_id);
  }
  if (!it->second) {
    return wb_failure(core::ErrorReason::internal_failure, "panel creator is empty: " + context.panel_id);
  }
  return it->second(context);
}

std::vector<std::string> PanelFactory::registered_panel_ids() const {
  std::vector<std::string> ids;
  ids.reserve(creators_.size());
  for (const auto& [id, _] : creators_) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  return ids;
}

void ConfigurableDiagnosticsProvider::set_compute_info(std::string backend, std::string cuda_device) {
  compute_backend_ = std::move(backend);
  cuda_device_ = std::move(cuda_device);
}
void ConfigurableDiagnosticsProvider::set_active_panels(std::vector<std::string> panels) {
  active_panels_ = std::move(panels);
}
void ConfigurableDiagnosticsProvider::add_note(std::string note) {
  notes_.push_back(std::move(note));
}
DiagnosticsSnapshot ConfigurableDiagnosticsProvider::snapshot() const {
  DiagnosticsSnapshot s;
  s.platform = "windows-msvc-x64";
  s.qt_version = qVersion();
  s.compute_backend = compute_backend_.empty() ? "unspecified" : compute_backend_;
  s.cuda_device = cuda_device_.empty() ? "none" : cuda_device_;
  s.active_panels = active_panels_;
  s.notes = notes_;
  return s;
}

std::unique_ptr<IServiceRegistry> make_service_registry() {
  return std::make_unique<ServiceRegistry>();
}
std::unique_ptr<ICommandRegistry> make_command_registry() {
  return std::make_unique<CommandRegistry>();
}
std::unique_ptr<IDiagnosticsProvider> make_diagnostics_provider() {
  return std::make_unique<ConfigurableDiagnosticsProvider>();
}

}  // namespace signal::workbench
