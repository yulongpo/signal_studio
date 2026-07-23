#include "signal_studio/core/module_descriptor.hpp"

#include <array>
#include <unordered_set>

namespace signal::core {

bool is_known_module_id(ModuleId id) noexcept {
  switch (id) {
  case ModuleId::core:
  case ModuleId::compute:
  case ModuleId::data:
  case ModuleId::task_runtime:
  case ModuleId::dsp:
  case ModuleId::visualization:
  case ModuleId::model_runtime:
  case ModuleId::dataset:
  case ModuleId::plugin_sdk:
  case ModuleId::workbench:
    return true;
  }
  return false;
}

std::string_view module_id_name(ModuleId id) noexcept {
  switch (id) {
  case ModuleId::core:
    return "Core";
  case ModuleId::compute:
    return "Compute";
  case ModuleId::data:
    return "Data";
  case ModuleId::task_runtime:
    return "TaskRuntime";
  case ModuleId::dsp:
    return "DSP";
  case ModuleId::visualization:
    return "Visualization";
  case ModuleId::model_runtime:
    return "ModelRuntime";
  case ModuleId::dataset:
    return "Dataset";
  case ModuleId::plugin_sdk:
    return "PluginSDK";
  case ModuleId::workbench:
    return "Workbench";
  }
  return "Unknown";
}

Status validate_module_descriptor(const ModuleDescriptor& descriptor) {
  if (!is_known_module_id(descriptor.id)) {
    return Status::failure({ErrorDomain::core, ErrorReason::invalid_argument},
                           "Module id is outside the public contract");
  }
  if (descriptor.cmake_target.empty() || descriptor.public_namespace.empty()) {
    return Status::failure({ErrorDomain::core, ErrorReason::invalid_argument},
                           "Module target and public namespace are required");
  }
  if (descriptor.api_version.major == 0) {
    return Status::failure({ErrorDomain::core, ErrorReason::invalid_argument},
                           "Module API major version must be non-zero");
  }
  std::unordered_set<std::uint8_t> dependencies;
  for (const auto dependency : descriptor.dependencies) {
    if (!is_known_module_id(dependency)) {
      return Status::failure({ErrorDomain::core, ErrorReason::invalid_argument},
                             "Module dependency id is outside the public contract");
    }
    if (dependency == descriptor.id || !dependencies.insert(static_cast<std::uint8_t>(dependency)).second) {
      return Status::failure({ErrorDomain::core, ErrorReason::invalid_argument},
                             "Module dependencies must be unique and cannot include the module itself");
    }
  }
  std::unordered_set<std::string_view> capabilities;
  for (const auto capability : descriptor.capabilities) {
    if (capability.empty() || !capabilities.insert(capability).second) {
      return Status::failure({ErrorDomain::core, ErrorReason::invalid_argument},
                             "Module capabilities must be non-empty and unique");
    }
  }
  return Status::success();
}

} // namespace signal::core
