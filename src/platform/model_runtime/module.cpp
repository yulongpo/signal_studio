#include "signal_studio/model_runtime/module.hpp"

#include <array>

namespace signal::model {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static constexpr std::array dependencies{
      core::ModuleId::data, core::ModuleId::compute, core::ModuleId::task_runtime, core::ModuleId::core};
  static constexpr std::array<std::string_view, 3> capabilities{
      "model.provider-selection", "model.preprocess-contracts", "model.artifact-contracts"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::model_runtime,
      "SignalStudio::ModelRuntime",
      "signal::model",
      {1, 0, 0},
      dependencies,
      capabilities};
  return descriptor;
}

}  // namespace signal::model
