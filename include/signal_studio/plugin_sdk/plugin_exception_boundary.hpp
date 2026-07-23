#pragma once

#include "signal_studio/plugin_sdk/plugin_abi_v1.h"

#include <functional>
#include <type_traits>
#include <utility>

namespace signal::plugin::abi_v1 {

template <typename Callable, typename... Arguments>
  requires std::is_invocable_r_v<signal_plugin_result_v1, Callable, Arguments...>
[[nodiscard]] signal_plugin_result_v1 invoke_result(Callable&& callable, Arguments&&... arguments) noexcept {
  try {
    return static_cast<signal_plugin_result_v1>(
        std::invoke(std::forward<Callable>(callable), std::forward<Arguments>(arguments)...));
  } catch (...) {
    return SIGNAL_PLUGIN_RESULT_INTERNAL_FAILURE_V1;
  }
}

template <typename Callable, typename... Arguments>
  requires std::is_invocable_r_v<void, Callable, Arguments...>
void invoke_void(Callable&& callable, Arguments&&... arguments) noexcept {
  try {
    std::invoke(std::forward<Callable>(callable), std::forward<Arguments>(arguments)...);
  } catch (...) {
    // Void ABI callbacks cannot report failure. Containment is still mandatory.
  }
}

} // namespace signal::plugin::abi_v1
