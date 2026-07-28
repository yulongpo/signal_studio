#include "signal_studio/compute/module.hpp"
#include "signal_studio/core/module_descriptor.hpp"
#include "signal_studio/core/services.hpp"
#include "signal_studio/core/version.hpp"
#include "signal_studio/data/module.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/dataset/module.hpp"
#include "signal_studio/dsp/module.hpp"
#include "signal_studio/model_runtime/module.hpp"
#include "signal_studio/plugin_sdk/module.hpp"
#include "signal_studio/task_runtime/module.hpp"
#include "signal_studio/task_runtime/task_runtime.hpp"
#include "signal_studio/visualization/module.hpp"
#include "signal_studio/workbench/module.hpp"

#include "../consumer_ms02_smoke.hpp"

#include <array>
#include <functional>
#include <iostream>

int main() {
  const std::array descriptors{
      std::cref(signal::core::module_descriptor()),   std::cref(signal::compute::module_descriptor()),
      std::cref(signal::data::module_descriptor()),   std::cref(signal::task::module_descriptor()),
      std::cref(signal::dsp::module_descriptor()),    std::cref(signal::visualization::module_descriptor()),
      std::cref(signal::model::module_descriptor()),  std::cref(signal::dataset::module_descriptor()),
      std::cref(signal::plugin::module_descriptor()), std::cref(signal::workbench::module_descriptor()),
  };
  for (const auto& descriptor : descriptors) {
    if (!signal::core::validate_module_descriptor(descriptor.get())) {
      return 1;
    }
  }
  signal::core::WorkspaceStore workspaces;
  auto workspace = workspaces.create("installed-ui-consumer");
  auto samples = signal::data::SignalBuffer::from_complex({{1.0, -1.0}, {0.5, 0.25}});
  const auto scheduling =
      signal::task::evaluate_scheduling(signal::task::WorkClass::indexing, std::chrono::milliseconds{51});
  if (!workspace || samples.view().size() != 2U || !scheduling || !scheduling.value().must_schedule) {
    return 2;
  }
  signal::task::TaskRuntime runtime{
      {1U, {1U, 1U, 0U, 1U}, {}, std::chrono::milliseconds{250}, std::chrono::milliseconds{10}}};
  signal::task::TaskSpec task;
  task.task_id = signal::task::TaskId::generate();
  task.task_type = "installed-ui-consumer";
  task.idempotency_key = "installed-ui-consumer-1";
  task.provenance = {{"installed-ui-consumer"}, {"version-1"}, {"consumer", "smoke"}};
  auto handle = runtime.submit(
      std::move(task), [](signal::task::TaskContext&) { return signal::task::TaskExecutionResult::completed(); });
  if (!handle || handle.value().wait().value().state != signal::task::TaskState::completed)
    return 3;
  if (!signal_studio_consumer::verify_ms02_public_api())
    return 4;
  std::cout << signal::core::build_info().product << ' ' << signal::core::build_info().version.to_string()
            << ": consumed " << descriptors.size() << " installed targets and executed MS-02 APIs";
  return descriptors.size() == 10 ? 0 : 5;
}
