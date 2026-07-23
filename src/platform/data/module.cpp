#include "signal_studio/data/module.hpp"

#include <array>

namespace signal::data {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static constexpr std::array dependencies{core::ModuleId::core};
  static constexpr std::array<std::string_view, 3> capabilities{"data.descriptors", "data.sample-ranges",
                                                                "data.format-adapters"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::data, "SignalStudio::Data", "signal::data", {1, 0, 0}, dependencies, capabilities};
  return descriptor;
}

} // namespace signal::data
