#include "signal_studio/plugin_sdk/module.hpp"

#include <array>

namespace signal::plugin {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static constexpr std::array dependencies{core::ModuleId::data, core::ModuleId::task_runtime, core::ModuleId::core};
  static constexpr std::array<std::string_view, 4> capabilities{
      "plugin.abi-v1", "plugin.query-v1", "plugin.service-boundaries", "plugin.capability-declaration"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::plugin_sdk, "SignalStudio::PluginSDK", "signal::plugin", {1, 0, 0}, dependencies, capabilities};
  return descriptor;
}

} // namespace signal::plugin
