#include "signal_studio/plugin_sdk/plugin_abi_v1.h"

#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout_v<signal_host_api_v1> && std::is_trivial_v<signal_host_api_v1>);
static_assert(std::is_standard_layout_v<signal_plugin_api_v1> && std::is_trivial_v<signal_plugin_api_v1>);
static_assert(sizeof(signal_plugin_handle_v1) == 8);
static_assert(std::is_nothrow_invocable_r_v<void, signal_plugin_log_fn_v1, void*, int32_t, const char*, const char*>);
static_assert(
    std::is_nothrow_invocable_r_v<signal_plugin_result_v1, signal_plugin_load_fn_v1, signal_plugin_handle_v1*>);
static_assert(
    std::is_nothrow_invocable_r_v<signal_plugin_result_v1, signal_plugin_activate_fn_v1, signal_plugin_handle_v1>);
static_assert(std::is_nothrow_invocable_r_v<void, signal_plugin_unload_fn_v1, signal_plugin_handle_v1>);
static_assert(std::is_nothrow_invocable_r_v<signal_plugin_result_v1, signal_plugin_query_v1_fn,
                                            const signal_host_api_v1*, signal_plugin_api_v1*>);
static_assert(noexcept(signal_plugin_query_v1(nullptr, nullptr)));
static_assert(noexcept(signal_plugin_validate_api_v1(nullptr, nullptr)));
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(signal_host_api_v1) == SIGNAL_PLUGIN_HOST_API_V1_SIZE_X64);
static_assert(sizeof(signal_plugin_api_v1) == SIGNAL_PLUGIN_API_V1_SIZE_X64);
static_assert(offsetof(signal_plugin_api_v1, load) == 32);
#endif

int main() {
  signal_host_api_v1 host{sizeof(signal_host_api_v1), SIGNAL_PLUGIN_ABI_V1, nullptr, nullptr};
  signal_plugin_api_v1 plugin{};
  plugin.struct_size = sizeof(plugin);
  plugin.abi_version = SIGNAL_PLUGIN_ABI_V1;
  return signal_plugin_validate_api_v1(&host, &plugin) == SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1 ? 0 : 1;
}
