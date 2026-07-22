#include "signal_studio/compute/module.hpp"

#include <array>

namespace signal::compute {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static constexpr std::array dependencies{core::ModuleId::core};
  static constexpr std::array<std::string_view, 3> capabilities{
      "compute.backend-discovery", "compute.cpu-fallback", "compute.resource-description"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::compute, "SignalStudio::Compute", "signal::compute", {1, 0, 0}, dependencies, capabilities};
  return descriptor;
}

}  // namespace signal::compute
