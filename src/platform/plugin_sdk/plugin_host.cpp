#include "signal_studio/plugin_sdk/plugin_host.hpp"

#include "signal_studio/plugin_sdk/plugin_exception_boundary.hpp"

#include <windows.h>

#include <cstring>
#include <utility>

namespace signal::plugin {

namespace {
core::Status plugin_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::plugin_sdk, reason}, std::move(message));
}

std::string narrow_string(const std::filesystem::path& p) {
  return p.generic_string();
}
} // namespace

PluginHandle::~PluginHandle() {
  unload();
}

PluginHandle::PluginHandle(PluginHandle&& other) noexcept
    : module_(other.module_), api_(other.api_), plugin_handle_(other.plugin_handle_), info_(std::move(other.info_)) {
  other.module_ = nullptr;
  other.api_ = {};
  other.plugin_handle_ = SIGNAL_PLUGIN_NULL_HANDLE;
}

PluginHandle& PluginHandle::operator=(PluginHandle&& other) noexcept {
  if (this == &other)
    return *this;
  unload();
  module_ = other.module_;
  api_ = other.api_;
  plugin_handle_ = other.plugin_handle_;
  info_ = std::move(other.info_);
  other.module_ = nullptr;
  other.api_ = {};
  other.plugin_handle_ = SIGNAL_PLUGIN_NULL_HANDLE;
  return *this;
}

core::Status PluginHandle::activate() const {
  if (!valid()) {
    return plugin_failure(core::ErrorReason::invalid_argument, "cannot activate an invalid plugin handle");
  }
  if (!api_.activate) {
    return plugin_failure(core::ErrorReason::internal_failure, "plugin has no activate function");
  }
  const signal_plugin_handle_v1 handle = plugin_handle_;
  const auto result = signal::plugin::abi_v1::invoke_result([&] { return api_.activate(handle); });
  if (result != SIGNAL_PLUGIN_RESULT_OK_V1) {
    return plugin_failure(core::ErrorReason::internal_failure, "plugin activate returned " + std::to_string(result));
  }
  return core::Status::success();
}

void PluginHandle::unload() noexcept {
  if (module_ == nullptr)
    return;
  if (api_.unload) {
    signal::plugin::abi_v1::invoke_void([&] { api_.unload(plugin_handle_); });
  }
  api_ = {};
  plugin_handle_ = SIGNAL_PLUGIN_NULL_HANDLE;
  FreeLibrary(static_cast<HMODULE>(module_));
  module_ = nullptr;
}

core::Result<PluginHandle> PluginHost::load(const std::filesystem::path& library_path) {
  if (library_path.empty()) {
    return plugin_failure(core::ErrorReason::invalid_argument, "plugin library path is empty");
  }
  const std::wstring native = library_path.wstring();
  HMODULE module = LoadLibraryW(native.c_str());
  if (module == nullptr) {
    return plugin_failure(core::ErrorReason::unavailable, "LoadLibraryW failed for " + narrow_string(library_path));
  }
  using query_fn_t = signal_plugin_result_v1(SIGNAL_PLUGIN_CALL*)(const signal_host_api_v1*, signal_plugin_api_v1*)
      SIGNAL_PLUGIN_NOEXCEPT;
  auto query_fn = reinterpret_cast<query_fn_t>(GetProcAddress(module, "signal_plugin_query_v1"));
  if (query_fn == nullptr) {
    FreeLibrary(module);
    return plugin_failure(core::ErrorReason::unavailable,
                          "plugin does not export signal_plugin_query_v1: " + narrow_string(library_path));
  }
  signal_host_api_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_version = SIGNAL_PLUGIN_ABI_V1;
  host.context = nullptr;
  host.log = nullptr;
  signal_plugin_api_v1 api{};
  api.struct_size = sizeof(api);
  const auto qresult = signal::plugin::abi_v1::invoke_result([&] { return query_fn(&host, &api); });
  if (qresult != SIGNAL_PLUGIN_RESULT_OK_V1) {
    FreeLibrary(module);
    return plugin_failure(core::ErrorReason::internal_failure,
                          "signal_plugin_query_v1 returned " + std::to_string(qresult));
  }
  const auto vresult = signal_plugin_validate_api_v1(&host, &api);
  if (vresult != SIGNAL_PLUGIN_RESULT_OK_V1) {
    FreeLibrary(module);
    return plugin_failure(core::ErrorReason::invalid_argument,
                          "plugin ABI validation failed (" + std::to_string(vresult) + ")");
  }
  // Invoke the plugin's load callback to obtain its private handle (used for activate/unload).
  signal_plugin_handle_v1 plugin_handle = SIGNAL_PLUGIN_NULL_HANDLE;
  const auto lresult = signal::plugin::abi_v1::invoke_result([&] { return api.load(&plugin_handle); });
  if (lresult != SIGNAL_PLUGIN_RESULT_OK_V1 || plugin_handle == SIGNAL_PLUGIN_NULL_HANDLE) {
    FreeLibrary(module);
    return plugin_failure(core::ErrorReason::internal_failure, "plugin load returned " + std::to_string(lresult));
  }
  PluginInfo info;
  info.plugin_id = api.plugin_id ? api.plugin_id : "";
  info.version_major = api.plugin_version_major;
  info.version_minor = api.plugin_version_minor;
  info.version_patch = api.plugin_version_patch;
  info.source_path = library_path;
  return PluginHandle(static_cast<void*>(module), api, plugin_handle, std::move(info));
}

core::Result<std::vector<PluginHandle>> PluginHost::discover(const std::filesystem::path& directory) {
  if (directory.empty()) {
    return plugin_failure(core::ErrorReason::invalid_argument, "discover directory is empty");
  }
  std::vector<PluginHandle> loaded;
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    return plugin_failure(core::ErrorReason::unavailable, "not a directory: " + narrow_string(directory));
  }
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".dll")
      continue;
    auto handle = load(entry.path());
    if (handle.ok()) {
      loaded.push_back(std::move(*handle));
    }
    // Plugins that fail to load are skipped silently; discovery is best-effort.
  }
  return loaded;
}

} // namespace signal::plugin
