#pragma once
#include "signal_studio/core/module_descriptor.hpp"
namespace signal::task {
[[nodiscard]] const core::ModuleDescriptor& module_descriptor() noexcept;
}
