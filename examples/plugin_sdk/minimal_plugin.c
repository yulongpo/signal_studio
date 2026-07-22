#define SIGNAL_PLUGIN_IMPLEMENTATION
#include "signal_studio/plugin_sdk/plugin_abi_v1.h"

static signal_plugin_result_v1 SIGNAL_PLUGIN_CALL minimal_load(
    signal_plugin_handle_v1* out_handle) SIGNAL_PLUGIN_NOEXCEPT {
  if (out_handle == NULL) return SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1;
  *out_handle = UINT64_C(1);
  return SIGNAL_PLUGIN_RESULT_OK_V1;
}

static signal_plugin_result_v1 SIGNAL_PLUGIN_CALL minimal_activate(
    signal_plugin_handle_v1 handle) SIGNAL_PLUGIN_NOEXCEPT {
  return handle == UINT64_C(1) ? SIGNAL_PLUGIN_RESULT_OK_V1 : SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1;
}

static void SIGNAL_PLUGIN_CALL minimal_unload(signal_plugin_handle_v1 handle) SIGNAL_PLUGIN_NOEXCEPT { (void)handle; }

SIGNAL_PLUGIN_EXPORT signal_plugin_result_v1 SIGNAL_PLUGIN_CALL signal_plugin_query_v1(
    const signal_host_api_v1* host, signal_plugin_api_v1* out_plugin) SIGNAL_PLUGIN_NOEXCEPT {
  if (host == NULL || out_plugin == NULL || host->struct_size < sizeof(signal_host_api_v1) ||
      out_plugin->struct_size < sizeof(signal_plugin_api_v1)) {
    return SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1;
  }
  if (host->abi_version != SIGNAL_PLUGIN_ABI_V1) return SIGNAL_PLUGIN_RESULT_INCOMPATIBLE_ABI_V1;
  out_plugin->struct_size = (uint32_t)sizeof(signal_plugin_api_v1);
  out_plugin->abi_version = SIGNAL_PLUGIN_ABI_V1;
  out_plugin->plugin_id = "org.signalstudio.example.minimal";
  out_plugin->plugin_version_major = 1u;
  out_plugin->plugin_version_minor = 0u;
  out_plugin->plugin_version_patch = 0u;
  out_plugin->reserved = 0u;
  out_plugin->load = minimal_load;
  out_plugin->activate = minimal_activate;
  out_plugin->unload = minimal_unload;
  if (host->log != NULL) host->log(host->context, 1, "SS-PLG-E001", "minimal plug-in queried");
  return SIGNAL_PLUGIN_RESULT_OK_V1;
}
