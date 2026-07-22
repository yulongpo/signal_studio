#include "signal_studio/plugin_sdk/plugin_exception_boundary.hpp"

#include <stdexcept>
#include <type_traits>

namespace {
signal_plugin_result_v1 throwing_load_impl(signal_plugin_handle_v1*) {
  throw std::runtime_error("deliberate plug-in failure");
}

signal_plugin_result_v1 SIGNAL_PLUGIN_CALL guarded_throwing_load(
    signal_plugin_handle_v1* out_handle) SIGNAL_PLUGIN_NOEXCEPT {
  return signal::plugin::abi_v1::invoke_result(throwing_load_impl, out_handle);
}

void throwing_unload_impl(signal_plugin_handle_v1) {
  throw std::runtime_error("deliberate void callback failure");
}

void SIGNAL_PLUGIN_CALL guarded_throwing_unload(signal_plugin_handle_v1 handle) SIGNAL_PLUGIN_NOEXCEPT {
  signal::plugin::abi_v1::invoke_void(throwing_unload_impl, handle);
}
}  // namespace

static_assert(noexcept(guarded_throwing_load(nullptr)));
static_assert(noexcept(guarded_throwing_unload(SIGNAL_PLUGIN_NULL_HANDLE)));
static_assert(std::is_nothrow_invocable_r_v<signal_plugin_result_v1, signal_plugin_load_fn_v1,
                                            signal_plugin_handle_v1*>);

int main() {
  signal_plugin_load_fn_v1 load = guarded_throwing_load;
  signal_plugin_unload_fn_v1 unload = guarded_throwing_unload;
  signal_plugin_handle_v1 handle = SIGNAL_PLUGIN_NULL_HANDLE;
  if (load(&handle) != SIGNAL_PLUGIN_RESULT_INTERNAL_FAILURE_V1) return 1;
  unload(handle);
  return 0;
}
