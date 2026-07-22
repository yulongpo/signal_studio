#pragma once
#include "signal_studio/core/module_descriptor.hpp"
namespace signal::plugin {
[[nodiscard]] const core::ModuleDescriptor& module_descriptor() noexcept;
}
