#pragma once

#include "signal_studio/core/result.hpp"

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace signal::workbench {

/// Opaque service identifier (API-WB-001).
struct ServiceId final {
  std::string value;
  friend bool operator==(const ServiceId&, const ServiceId&) = default;
};

/// A panel hosted in a dock/center/inspector region (API-WB-002). Qt-free interface; the host
/// embeds the underlying widget via native_widget().
enum class PanelRegion : std::uint8_t { center = 0, left_dock = 1, right_dock = 2, bottom_dock = 3 };

struct PanelContext final {
  std::string panel_id;
  PanelRegion region{PanelRegion::center};
  friend bool operator==(const PanelContext&, const PanelContext&) = default;
};

class IPanel {
public:
  virtual ~IPanel() = default;
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual PanelRegion region() const noexcept = 0;
  [[nodiscard]] virtual void* native_widget() noexcept = 0;
};

/// Service registry (API-WB-001). Stores arbitrary service objects by id for host discovery.
class IServiceRegistry {
public:
  virtual ~IServiceRegistry() = default;
  [[nodiscard]] virtual core::Status register_service(ServiceId id, std::shared_ptr<void> service) = 0;
  [[nodiscard]] virtual std::shared_ptr<void> resolve(const ServiceId& id) const noexcept = 0;
};

/// Panel factory (API-WB-002).
class IPanelFactory {
public:
  virtual ~IPanelFactory() = default;
  [[nodiscard]] virtual core::Result<std::unique_ptr<IPanel>> create(const PanelContext& context) = 0;
  [[nodiscard]] virtual std::vector<std::string> registered_panel_ids() const = 0;
};

/// Command registry (API-WB-003). Commands back menus, shortcuts and automation.
using CommandAction = std::function<core::Status()>;

class ICommandRegistry {
public:
  virtual ~ICommandRegistry() = default;
  [[nodiscard]] virtual core::Status register_command(std::string command_id, CommandAction action) = 0;
  [[nodiscard]] virtual core::Status invoke(std::string_view command_id) const = 0;
  [[nodiscard]] virtual std::vector<std::string> registered_command_ids() const = 0;
};

/// Diagnostics snapshot (API-WB-004): real environment and backend state, never fabricated.
struct DiagnosticsSnapshot final {
  std::string platform;
  std::string qt_version;
  std::string compute_backend;
  std::string cuda_device;
  std::vector<std::string> active_panels;
  std::vector<std::string> notes;
  friend bool operator==(const DiagnosticsSnapshot&, const DiagnosticsSnapshot&) = default;
};

class IDiagnosticsProvider {
public:
  virtual ~IDiagnosticsProvider() = default;
  [[nodiscard]] virtual DiagnosticsSnapshot snapshot() const = 0;
};

/// Concrete panel factory (API-WB-002). Hosts register creators per panel id; create() dispatches.
using PanelCreator = std::function<core::Result<std::unique_ptr<IPanel>>(const PanelContext&)>;

class PanelFactory final : public IPanelFactory {
public:
  [[nodiscard]] core::Status register_creator(std::string panel_id, PanelCreator creator);
  [[nodiscard]] core::Result<std::unique_ptr<IPanel>> create(const PanelContext& context) override;
  [[nodiscard]] std::vector<std::string> registered_panel_ids() const override;

private:
  std::unordered_map<std::string, PanelCreator> creators_;
};

/// Diagnostics provider that the host seeds with compute/backend facts it alone can see
/// (Workbench cannot depend on Compute/Data per the approved DAG).
class ConfigurableDiagnosticsProvider final : public IDiagnosticsProvider {
public:
  void set_compute_info(std::string backend, std::string cuda_device);
  void set_active_panels(std::vector<std::string> panels);
  void add_note(std::string note);
  [[nodiscard]] DiagnosticsSnapshot snapshot() const override;

private:
  std::string compute_backend_;
  std::string cuda_device_;
  std::vector<std::string> active_panels_;
  std::vector<std::string> notes_;
};

[[nodiscard]] std::unique_ptr<IServiceRegistry> make_service_registry();
[[nodiscard]] std::unique_ptr<ICommandRegistry> make_command_registry();
[[nodiscard]] std::unique_ptr<IDiagnosticsProvider> make_diagnostics_provider();

} // namespace signal::workbench
