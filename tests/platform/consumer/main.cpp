#include "signal_studio/compute/module.hpp"
#include "signal_studio/core/module_descriptor.hpp"
#include "signal_studio/core/version.hpp"
#include "signal_studio/data/module.hpp"
#include "signal_studio/dataset/module.hpp"
#include "signal_studio/dsp/module.hpp"
#include "signal_studio/model_runtime/module.hpp"
#include "signal_studio/plugin_sdk/module.hpp"
#include "signal_studio/task_runtime/module.hpp"
#include "signal_studio/visualization/module.hpp"
#include "signal_studio/workbench/module.hpp"

#include <array>
#include <functional>
#include <iostream>

int main() {
  const std::array descriptors{
      std::cref(signal::core::module_descriptor()),
      std::cref(signal::compute::module_descriptor()),
      std::cref(signal::data::module_descriptor()),
      std::cref(signal::task::module_descriptor()),
      std::cref(signal::dsp::module_descriptor()),
      std::cref(signal::visualization::module_descriptor()),
      std::cref(signal::model::module_descriptor()),
      std::cref(signal::dataset::module_descriptor()),
      std::cref(signal::plugin::module_descriptor()),
      std::cref(signal::workbench::module_descriptor()),
  };
  for (const auto& descriptor : descriptors) {
    if (!signal::core::validate_module_descriptor(descriptor.get())) {
      return 1;
    }
  }
  std::cout << signal::core::build_info().product << ' '
            << signal::core::build_info().version.to_string() << ": consumed " << descriptors.size()
            << " installed targets";
  return descriptors.size() == 10 ? 0 : 2;
}
