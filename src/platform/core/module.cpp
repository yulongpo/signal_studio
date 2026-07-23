#include "signal_studio/core/module_descriptor.hpp"

#include <array>

namespace signal::core {

const ModuleDescriptor& module_descriptor() noexcept {
  static constexpr std::array<std::string_view, 4> capabilities{"core.version", "core.status", "core.capabilities",
                                                                "core.module-descriptors"};
  static constexpr ModuleDescriptor descriptor{ModuleId::core, "SignalStudio::Core", "signal::core", {1, 0, 0}, {},
                                               capabilities};
  return descriptor;
}

} // namespace signal::core
