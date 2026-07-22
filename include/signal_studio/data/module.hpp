#pragma once
#include "signal_studio/core/module_descriptor.hpp"
namespace signal::data {
[[nodiscard]] const core::ModuleDescriptor& module_descriptor() noexcept;
}
