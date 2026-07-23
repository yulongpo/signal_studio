#include "signal_studio/compute/module.hpp"
#include "signal_studio/core/module_descriptor.hpp"
#include "signal_studio/data/module.hpp"
#include "signal_studio/dataset/module.hpp"
#include "signal_studio/dsp/module.hpp"
#include "signal_studio/model_runtime/module.hpp"
#include "signal_studio/plugin_sdk/module.hpp"
#include "signal_studio/task_runtime/module.hpp"
#if SIGNAL_STUDIO_TEST_HAS_UI
#include "signal_studio/visualization/module.hpp"
#include "signal_studio/workbench/module.hpp"
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {
using Provider = const signal::core::ModuleDescriptor& (*)() noexcept;

struct ModuleCase final {
  std::string_view name;
  signal::core::ModuleId id;
  Provider provider;
};

constexpr std::array headless_cases{
    ModuleCase{"core", signal::core::ModuleId::core, signal::core::module_descriptor},
    ModuleCase{"data", signal::core::ModuleId::data, signal::data::module_descriptor},
    ModuleCase{"dsp", signal::core::ModuleId::dsp, signal::dsp::module_descriptor},
    ModuleCase{"compute", signal::core::ModuleId::compute, signal::compute::module_descriptor},
    ModuleCase{"task-runtime", signal::core::ModuleId::task_runtime, signal::task::module_descriptor},
    ModuleCase{"plugin-sdk", signal::core::ModuleId::plugin_sdk, signal::plugin::module_descriptor},
    ModuleCase{"model-runtime", signal::core::ModuleId::model_runtime, signal::model::module_descriptor},
    ModuleCase{"dataset", signal::core::ModuleId::dataset, signal::dataset::module_descriptor},
};

const ModuleCase* find_case(std::string_view name) noexcept {
  for (const auto& item : headless_cases) {
    if (item.name == name)
      return &item;
  }
#if SIGNAL_STUDIO_TEST_HAS_UI
  static constexpr std::array ui_cases{
      ModuleCase{"visualization", signal::core::ModuleId::visualization, signal::visualization::module_descriptor},
      ModuleCase{"workbench", signal::core::ModuleId::workbench, signal::workbench::module_descriptor},
  };
  for (const auto& item : ui_cases) {
    if (item.name == name)
      return &item;
  }
#endif
  return nullptr;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--module")
    return 2;
  const auto* module = find_case(argv[2]);
  if (module == nullptr)
    return 3;

  using Clock = std::chrono::steady_clock;
  constexpr std::uint32_t iterations = 100'000;
  constexpr auto threshold = std::chrono::seconds{5};
  std::uint64_t checksum = 0;
  bool valid = true;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto& descriptor = module->provider();
    checksum += static_cast<std::uint8_t>(descriptor.id) + descriptor.capabilities.size() +
                descriptor.dependencies.size() + descriptor.api_version.major;
    if ((index & 0x3ffU) == 0U)
      valid = valid && signal::core::validate_module_descriptor(descriptor).ok();
  }
  const auto elapsed = Clock::now() - start;
  const auto elapsed_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  std::cout << "module_performance_smoke module=" << module->name << " iterations=" << iterations
            << " elapsed_us=" << elapsed_microseconds << " threshold_ms=5000"
            << " classification=non-release-regression-guard\n";
  return valid && module->provider().id == module->id && checksum != 0 && elapsed < threshold ? 0 : 4;
}
