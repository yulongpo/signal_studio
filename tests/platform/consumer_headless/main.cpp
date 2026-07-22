#include "signal_studio/compute/module.hpp"
#include "signal_studio/core/version.hpp"
#include "signal_studio/data/module.hpp"
#include "signal_studio/dataset/module.hpp"
#include "signal_studio/dsp/module.hpp"
#include "signal_studio/model_runtime/module.hpp"
#include "signal_studio/plugin_sdk/module.hpp"
#include "signal_studio/plugin_sdk/plugin_abi_v1.h"
#include "signal_studio/task_runtime/module.hpp"

#include <array>
#include <functional>
#include <iostream>

int main() {
  signal_host_api_v1 host{sizeof(signal_host_api_v1), SIGNAL_PLUGIN_ABI_V1, nullptr, nullptr};
  signal_plugin_api_v1 plugin{};
  plugin.struct_size = sizeof(plugin);
  plugin.abi_version = SIGNAL_PLUGIN_ABI_V1 + 1u;
  if (signal_plugin_validate_api_v1(&host, &plugin) != SIGNAL_PLUGIN_RESULT_INCOMPATIBLE_ABI_V1) return 1;
  const std::array descriptors{
      std::cref(signal::core::module_descriptor()), std::cref(signal::data::module_descriptor()),
      std::cref(signal::dsp::module_descriptor()), std::cref(signal::compute::module_descriptor()),
      std::cref(signal::task::module_descriptor()), std::cref(signal::plugin::module_descriptor()),
      std::cref(signal::model::module_descriptor()), std::cref(signal::dataset::module_descriptor()),
  };
  for (const auto& descriptor : descriptors) {
    if (!signal::core::validate_module_descriptor(descriptor.get())) return 2;
  }
  std::cout << signal::core::build_info().product << " headless consumer: " << descriptors.size()
            << " installed targets";
  return descriptors.size() == 8 ? 0 : 3;
}
