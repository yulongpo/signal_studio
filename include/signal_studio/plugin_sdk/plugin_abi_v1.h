#ifndef SIGNAL_STUDIO_PLUGIN_ABI_V1_H
#define SIGNAL_STUDIO_PLUGIN_ABI_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define SIGNAL_PLUGIN_NOEXCEPT noexcept
extern "C" {
#else
#define SIGNAL_PLUGIN_NOEXCEPT
#endif

#if defined(_WIN32)
#define SIGNAL_PLUGIN_CALL __cdecl
#if defined(SIGNAL_PLUGIN_IMPLEMENTATION)
#define SIGNAL_PLUGIN_EXPORT __declspec(dllexport)
#else
#define SIGNAL_PLUGIN_EXPORT
#endif
#else
#define SIGNAL_PLUGIN_CALL
#if defined(SIGNAL_PLUGIN_IMPLEMENTATION)
#define SIGNAL_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define SIGNAL_PLUGIN_EXPORT
#endif
#endif

#define SIGNAL_PLUGIN_ABI_V1 0x00010000u
#define SIGNAL_PLUGIN_NULL_HANDLE UINT64_C(0)
#define SIGNAL_PLUGIN_HOST_API_V1_SIZE_X64 UINT32_C(24)
#define SIGNAL_PLUGIN_API_V1_SIZE_X64 UINT32_C(56)

typedef uint64_t signal_plugin_handle_v1;

typedef int32_t signal_plugin_result_v1;
enum {
  SIGNAL_PLUGIN_RESULT_OK_V1 = 0,
  SIGNAL_PLUGIN_RESULT_INVALID_ARGUMENT_V1 = -1,
  SIGNAL_PLUGIN_RESULT_INCOMPATIBLE_ABI_V1 = -2,
  SIGNAL_PLUGIN_RESULT_UNAVAILABLE_V1 = -3,
  SIGNAL_PLUGIN_RESULT_INTERNAL_FAILURE_V1 = -4
};

typedef void(SIGNAL_PLUGIN_CALL* signal_plugin_log_fn_v1)(void* context, int32_t severity, const char* stable_code,
                                                          const char* message) SIGNAL_PLUGIN_NOEXCEPT;
typedef signal_plugin_result_v1(SIGNAL_PLUGIN_CALL* signal_plugin_load_fn_v1)(signal_plugin_handle_v1* out_handle)
    SIGNAL_PLUGIN_NOEXCEPT;
typedef signal_plugin_result_v1(SIGNAL_PLUGIN_CALL* signal_plugin_activate_fn_v1)(signal_plugin_handle_v1 handle)
    SIGNAL_PLUGIN_NOEXCEPT;
typedef void(SIGNAL_PLUGIN_CALL* signal_plugin_unload_fn_v1)(signal_plugin_handle_v1 handle) SIGNAL_PLUGIN_NOEXCEPT;

typedef struct signal_host_api_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  void* context;
  signal_plugin_log_fn_v1 log;
} signal_host_api_v1;

typedef struct signal_plugin_api_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* plugin_id;
  uint32_t plugin_version_major;
  uint32_t plugin_version_minor;
  uint32_t plugin_version_patch;
  uint32_t reserved;
  signal_plugin_load_fn_v1 load;
  signal_plugin_activate_fn_v1 activate;
  signal_plugin_unload_fn_v1 unload;
} signal_plugin_api_v1;

typedef signal_plugin_result_v1(SIGNAL_PLUGIN_CALL* signal_plugin_query_v1_fn)(
    const signal_host_api_v1* host, signal_plugin_api_v1* out_plugin) SIGNAL_PLUGIN_NOEXCEPT;

SIGNAL_PLUGIN_EXPORT signal_plugin_result_v1 SIGNAL_PLUGIN_CALL
signal_plugin_query_v1(const signal_host_api_v1* host, signal_plugin_api_v1* out_plugin) SIGNAL_PLUGIN_NOEXCEPT;

/* Linkable SDK-side structural/version check. It never invokes plug-in callbacks. */
signal_plugin_result_v1 SIGNAL_PLUGIN_CALL signal_plugin_validate_api_v1(
    const signal_host_api_v1* host, const signal_plugin_api_v1* plugin) SIGNAL_PLUGIN_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif
