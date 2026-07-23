#pragma once

#include "signal_studio/core/error.hpp"
#include "signal_studio/core/version.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace signal::core {

enum class ModuleId : std::uint8_t {
  core = 1,
  compute = 2,
  data = 3,
  task_runtime = 4,
  dsp = 5,
  visualization = 6,
  model_runtime = 7,
  dataset = 8,
  plugin_sdk = 9,
  workbench = 10,
};

[[nodiscard]] bool is_known_module_id(ModuleId id) noexcept;

struct ModuleDescriptor final {
  ModuleId id;
  std::string_view cmake_target;
  std::string_view public_namespace;
  SemanticVersion api_version;
  std::span<const ModuleId> dependencies;
  std::span<const std::string_view> capabilities;
};

[[nodiscard]] std::string_view module_id_name(ModuleId id) noexcept;
[[nodiscard]] Status validate_module_descriptor(const ModuleDescriptor& descriptor);
[[nodiscard]] const ModuleDescriptor& module_descriptor() noexcept;

} // namespace signal::core
