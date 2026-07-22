#include "signal_studio/plugin_sdk/plugin_abi_v1.h"

#include <cstddef>
#include <type_traits>

static_assert(std::is_standard_layout_v<signal_host_api_v1>);
static_assert(std::is_trivial_v<signal_host_api_v1>);
static_assert(std::is_standard_layout_v<signal_plugin_api_v1>);
static_assert(std::is_trivial_v<signal_plugin_api_v1>);
static_assert(sizeof(signal_plugin_handle_v1) == 8);
static_assert(offsetof(signal_host_api_v1, struct_size) == 0);
static_assert(offsetof(signal_plugin_api_v1, struct_size) == 0);

extern "C" signal_plugin_result_v1 SIGNAL_PLUGIN_CALL signal_plugin_validate_api_v1(
    const signal_host_api_v1* host, const signal_plugin_api_v1* plugin) SIGNAL_PLUGIN_NOEXCEPT {
  if (host == nullptr || plugin == nullptr) return SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1;
  if (host->struct_size < sizeof(signal_host_api_v1) || plugin->struct_size < sizeof(signal_plugin_api_v1)) {
    return SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1;
  }
  if (host->abi_version != SIGNAL_PLUGIN_ABI_V1 || plugin->abi_version != SIGNAL_PLUGIN_ABI_V1) {
    return SIGNAL_PLUGIN_RESULT_INCOMPATIBLE_ABI_V1;
  }
  if (plugin->plugin_id == nullptr || plugin->load == nullptr || plugin->activate == nullptr || plugin->unload == nullptr) {
    return SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1;
  }
  return SIGNAL_PLUGIN_RESULT_OK_V1;
}
