#pragma once
#include "signal_studio/core/module_descriptor.hpp"
namespace signal::compute {
[[nodiscard]] const core::ModuleDescriptor& module_descriptor() noexcept;
}
