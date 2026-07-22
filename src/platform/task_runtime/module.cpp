#include "signal_studio/task_runtime/module.hpp"

#include <array>

namespace signal::task {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static constexpr std::array dependencies{core::ModuleId::compute, core::ModuleId::core};
  static constexpr std::array<std::string_view, 3> capabilities{
      "task.scheduling", "task.cancellation", "task.progress"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::task_runtime,
      "SignalStudio::TaskRuntime",
      "signal::task",
      {1, 0, 0},
      dependencies,
      capabilities};
  return descriptor;
}

}  // namespace signal::task
