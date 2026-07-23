#include "signal_studio/dataset/module.hpp"

#include <array>

namespace signal::dataset {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static constexpr std::array dependencies{core::ModuleId::data, core::ModuleId::task_runtime, core::ModuleId::core};
  static constexpr std::array<std::string_view, 3> capabilities{"dataset.schema", "dataset.provenance",
                                                                "dataset.versioning"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::dataset, "SignalStudio::Dataset", "signal::dataset", {1, 0, 0}, dependencies, capabilities};
  return descriptor;
}

} // namespace signal::dataset
