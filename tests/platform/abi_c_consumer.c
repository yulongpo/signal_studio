#include "signal_studio/plugin_sdk/plugin_abi_v1.h"

#include <stddef.h>

_Static_assert(sizeof(signal_plugin_handle_v1) == 8, "ABI handle must be fixed-width");
_Static_assert(offsetof(signal_host_api_v1, struct_size) == 0, "host prefix changed");
_Static_assert(offsetof(signal_plugin_api_v1, struct_size) == 0, "plugin prefix changed");
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(signal_host_api_v1) == SIGNAL_PLUGIN_HOST_API_V1_SIZE_X64, "x64 host layout changed");
_Static_assert(sizeof(signal_plugin_api_v1) == SIGNAL_PLUGIN_API_V1_SIZE_X64, "x64 plugin layout changed");
_Static_assert(offsetof(signal_plugin_api_v1, load) == 32, "x64 function-table layout changed");
#endif

static signal_plugin_result_v1 SIGNAL_PLUGIN_CALL load(signal_plugin_handle_v1* out) {
  *out = 1u;
  return SIGNAL_PLUGIN_RESULT_OK_V1;
}
static signal_plugin_result_v1 SIGNAL_PLUGIN_CALL activate(signal_plugin_handle_v1 handle) {
  return handle == 1u ? SIGNAL_PLUGIN_RESULT_OK_V1 : SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1;
}
static void SIGNAL_PLUGIN_CALL unload(signal_plugin_handle_v1 handle) {
  (void)handle;
}

int main(void) {
  signal_host_api_v1 host = {(uint32_t)sizeof(signal_host_api_v1), SIGNAL_PLUGIN_ABI_V1, NULL, NULL};
  signal_plugin_api_v1 plugin = {(uint32_t)sizeof(signal_plugin_api_v1),
                                 SIGNAL_PLUGIN_ABI_V1,
                                 "c-consumer",
                                 1u,
                                 0u,
                                 0u,
                                 0u,
                                 load,
                                 activate,
                                 unload};
  if (signal_plugin_validate_api_v1(&host, &plugin) != SIGNAL_PLUGIN_RESULT_OK_V1)
    return 1;
  host.abi_version += 1u;
  return signal_plugin_validate_api_v1(&host, &plugin) == SIGNAL_PLUGIN_RESULT_INCOMPATIBLE_ABI_V1 ? 0 : 2;
}
