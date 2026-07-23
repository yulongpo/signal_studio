#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/plugin_sdk/plugin_abi_v1.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace signal::plugin {

/// Loaded plugin descriptor captured at query time.
struct PluginInfo final {
  std::string plugin_id;
  std::uint32_t version_major{};
  std::uint32_t version_minor{};
  std::uint32_t version_patch{};
  std::filesystem::path source_path;
  friend bool operator==(const PluginInfo&, const PluginInfo&) = default;
};

/// RAII handle to a loaded plugin. Unloads on destruction. Move-only.
class PluginHandle final {
 public:
  PluginHandle() = default;
  ~PluginHandle();
  PluginHandle(const PluginHandle&) = delete;
  PluginHandle& operator=(const PluginHandle&) = delete;
  PluginHandle(PluginHandle&&) noexcept;
  PluginHandle& operator=(PluginHandle&&) noexcept;

  [[nodiscard]] bool valid() const noexcept { return module_ != nullptr; }
  [[nodiscard]] const PluginInfo& info() const noexcept { return info_; }
  [[nodiscard]] core::Status activate() const;
  void unload() noexcept;

 private:
  friend class PluginHost;
  PluginHandle(void* module, signal_plugin_api_v1 api, signal_plugin_handle_v1 plugin_handle, PluginInfo info)
      : module_(module), api_(api), plugin_handle_(plugin_handle), info_(std::move(info)) {}
  void* module_{nullptr};
  signal_plugin_api_v1 api_{};
  signal_plugin_handle_v1 plugin_handle_{SIGNAL_PLUGIN_NULL_HANDLE};
  PluginInfo info_;
};

/// Plugin host: discovers and loads ABI-v1 plugins from dynamic libraries. The query entry point
/// is invoked through a C exception boundary; a plugin that throws or crashes is rejected without
/// taking down the host (API-PLG-001, error isolation).
class PluginHost final {
 public:
  PluginHost() = default;
  /// Load a single plugin library. Returns failure if the library cannot be opened, the query
  /// symbol is missing, the ABI is incompatible, or validation rejects the descriptor.
  [[nodiscard]] core::Result<PluginHandle> load(const std::filesystem::path& library_path);
  /// Scan a directory for candidate plugin libraries and load each that queries successfully.
  [[nodiscard]] core::Result<std::vector<PluginHandle>> discover(const std::filesystem::path& directory);
};

}  // namespace signal::plugin
