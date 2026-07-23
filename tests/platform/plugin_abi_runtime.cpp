#include "signal_studio/plugin_sdk/plugin_abi_v1.h"

#include <iostream>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
#if defined(_WIN32)
  const HMODULE library = LoadLibraryA(argv[1]);
  if (library == nullptr)
    return 3;
  const auto query = reinterpret_cast<signal_plugin_query_v1_fn>(GetProcAddress(library, "signal_plugin_query_v1"));
#else
  void* library = dlopen(argv[1], RTLD_NOW);
  if (library == nullptr)
    return 3;
  const auto query = reinterpret_cast<signal_plugin_query_v1_fn>(dlsym(library, "signal_plugin_query_v1"));
#endif
  if (query == nullptr)
    return 4;
  signal_host_api_v1 host{sizeof(host), SIGNAL_PLUGIN_ABI_V1, nullptr, nullptr};
  signal_plugin_api_v1 plugin{};
  plugin.struct_size = sizeof(plugin);
  if (query(&host, &plugin) != SIGNAL_PLUGIN_RESULT_OK_V1)
    return 5;
  if (signal_plugin_validate_api_v1(&host, &plugin) != SIGNAL_PLUGIN_RESULT_OK_V1)
    return 6;
  signal_plugin_handle_v1 handle = SIGNAL_PLUGIN_NULL_HANDLE;
  if (plugin.load(&handle) != SIGNAL_PLUGIN_RESULT_OK_V1 || plugin.activate(handle) != SIGNAL_PLUGIN_RESULT_OK_V1)
    return 7;
  plugin.unload(handle);
  host.abi_version += 1u;
  plugin.struct_size = sizeof(plugin);
  if (query(&host, &plugin) != SIGNAL_PLUGIN_RESULT_INCOMPATIBLE_ABI_V1)
    return 8;
  std::cout << "signal_plugin_query_v1 symbol and ABI rejection verified";
  return 0;
}
