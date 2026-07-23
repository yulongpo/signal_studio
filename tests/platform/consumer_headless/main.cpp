#include "signal_studio/compute/module.hpp"
#include "signal_studio/core/services.hpp"
#include "signal_studio/core/version.hpp"
#include "signal_studio/data/module.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/dataset/module.hpp"
#include "signal_studio/dsp/module.hpp"
#include "signal_studio/model_runtime/module.hpp"
#include "signal_studio/plugin_sdk/module.hpp"
#include "signal_studio/plugin_sdk/plugin_abi_v1.h"
#include "signal_studio/task_runtime/module.hpp"
#include "signal_studio/task_runtime/task_runtime.hpp"

#include <array>
#include <functional>
#include <iostream>

int main() {
  signal_host_api_v1 host{sizeof(signal_host_api_v1), SIGNAL_PLUGIN_ABI_V1, nullptr, nullptr};
  signal_plugin_api_v1 plugin{};
  plugin.struct_size = sizeof(plugin);
  plugin.abi_version = SIGNAL_PLUGIN_ABI_V1 + 1u;
  if (signal_plugin_validate_api_v1(&host, &plugin) != SIGNAL_PLUGIN_RESULT_INCOMPATIBLE_ABI_V1)
    return 1;
  const std::array descriptors{
      std::cref(signal::core::module_descriptor()),  std::cref(signal::data::module_descriptor()),
      std::cref(signal::dsp::module_descriptor()),   std::cref(signal::compute::module_descriptor()),
      std::cref(signal::task::module_descriptor()),  std::cref(signal::plugin::module_descriptor()),
      std::cref(signal::model::module_descriptor()), std::cref(signal::dataset::module_descriptor()),
  };
  for (const auto& descriptor : descriptors) {
    if (!signal::core::validate_module_descriptor(descriptor.get()))
      return 2;
  }
  signal::core::WorkspaceStore workspaces;
  auto workspace = workspaces.create("installed-consumer");
  auto samples = signal::data::SignalBuffer::from_real({1.0, -1.0, 0.5});
  if (!workspace || samples.view().size() != 3U ||
      !signal::task::evaluate_scheduling(signal::task::WorkClass::dsp, std::chrono::milliseconds{51}).must_schedule) {
    return 3;
  }
  signal::task::TaskRuntime runtime{
      {1U, {1U, 1U, 0U, 1U}, {}, std::chrono::milliseconds{250}, std::chrono::milliseconds{10}}};
  signal::task::TaskSpec task;
  task.task_id = signal::task::TaskId::generate();
  task.task_type = "installed-consumer";
  task.idempotency_key = "installed-consumer-1";
  task.provenance = {{"installed-consumer"}, {"version-1"}, {"consumer", "smoke"}};
  auto handle = runtime.submit(
      std::move(task), [](signal::task::TaskContext&) { return signal::task::TaskExecutionResult::completed(); });
  if (!handle || handle.value().wait().value().state != signal::task::TaskState::completed)
    return 4;
  std::cout << signal::core::build_info().product << " headless consumer: " << descriptors.size()
            << " installed targets";
  return descriptors.size() == 8 ? 0 : 5;
}
