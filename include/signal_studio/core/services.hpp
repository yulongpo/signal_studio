#pragma once

#include "signal_studio/core/capability.hpp"
#include "signal_studio/core/result.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::core {

using ProjectId = std::string;
using DataSourceVersionId = std::string;
using ViewRequestId = std::uint64_t;

template <typename Unit, typename Rep = double> struct Quantity final {
  Rep value{};
  friend constexpr bool operator==(const Quantity&, const Quantity&) = default;
};
struct SecondsUnit final {};
struct HertzUnit final {};
struct SamplesPerSecondUnit final {};
using Seconds = Quantity<SecondsUnit>;
using Hertz = Quantity<HertzUnit>;
using SamplesPerSecond = Quantity<SamplesPerSecondUnit>;

struct Hash256 final {
  std::array<std::uint8_t, 32> bytes{};
  [[nodiscard]] std::string hex() const;
  friend bool operator==(const Hash256&, const Hash256&) = default;
};

[[nodiscard]] Result<Hash256> hash_bytes(std::span<const std::byte> bytes);
[[nodiscard]] Result<Hash256> hash_file(const std::filesystem::path& path,
                                        std::size_t chunk_bytes = 4U * 1024U * 1024U);

enum class LogLevel : std::uint8_t { trace, debug, info, warning, error, critical };
struct LogEvent final {
  std::chrono::system_clock::time_point timestamp{};
  LogLevel level{LogLevel::info};
  std::string category;
  std::string message;
  std::map<std::string, std::string> fields;
};
class ILogger {
public:
  virtual ~ILogger() = default;
  virtual void write(const LogEvent& event) noexcept = 0;
};
class MemoryLogger final : public ILogger {
public:
  void write(const LogEvent& event) noexcept override;
  [[nodiscard]] std::vector<LogEvent> snapshot() const;

private:
  mutable std::mutex mutex_;
  std::vector<LogEvent> events_;
};

class Configuration final {
public:
  [[nodiscard]] Status set(std::string key, std::string value);
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
  [[nodiscard]] Result<std::uint64_t> get_uint64(std::string_view key, std::uint64_t minimum,
                                                 std::uint64_t maximum) const;
  [[nodiscard]] std::map<std::string, std::string> snapshot() const;

private:
  mutable std::shared_mutex mutex_;
  std::map<std::string, std::string> values_;
};

[[nodiscard]] Status validate_relative_resource_path(const std::filesystem::path& path);
[[nodiscard]] Result<std::filesystem::path> resolve_relative_resource(const std::filesystem::path& project_file,
                                                                      const std::filesystem::path& resource);

class AtomicFileStore final {
public:
  [[nodiscard]] Status write(const std::filesystem::path& destination, std::span<const std::byte> content) const;
  [[nodiscard]] Result<std::vector<std::byte>> read(const std::filesystem::path& source,
                                                    std::uint64_t maximum_bytes) const;
  [[nodiscard]] Status recover(const std::filesystem::path& destination) const;
};

struct SourceFingerprint final {
  std::string canonical_path;
  std::uint64_t size_bytes{};
  std::int64_t modified_ticks{};
  Hash256 sampled_hash;
  DataSourceVersionId version_id;
  friend bool operator==(const SourceFingerprint&, const SourceFingerprint&) = default;
};
[[nodiscard]] Result<SourceFingerprint> fingerprint_source(const std::filesystem::path& path,
                                                           std::size_t sample_bytes = 64U * 1024U);

struct SchemaVersion final {
  std::uint32_t major{1};
  std::uint32_t minor{0};
  friend bool operator==(const SchemaVersion&, const SchemaVersion&) = default;
};
struct WorkspaceDataSource final {
  std::string id;
  DataSourceVersionId version_id;
  std::string relative_uri;
  std::string descriptor_json;
  std::uint64_t loaded_begin{};
  std::uint64_t loaded_end{};
  bool read_only{true};
  friend bool operator==(const WorkspaceDataSource&, const WorkspaceDataSource&) = default;
};
struct WorkspaceObject final {
  std::string id;
  std::string kind;
  DataSourceVersionId data_source_version_id;
  std::map<std::string, std::string> attributes;
  std::vector<std::string> relations;
  friend bool operator==(const WorkspaceObject&, const WorkspaceObject&) = default;
};
struct Workspace final {
  SchemaVersion schema_version{};
  SchemaVersion loaded_schema_version{};
  ProjectId project_id;
  std::vector<WorkspaceDataSource> data_sources;
  std::vector<WorkspaceObject> objects;
  std::vector<WorkspaceObject> tasks;
  std::vector<WorkspaceObject> results;
  std::map<std::string, std::string> extensions;
  bool read_only{};
  friend bool operator==(const Workspace&, const Workspace&) = default;
};

class IWorkspaceStore {
public:
  virtual ~IWorkspaceStore() = default;
  [[nodiscard]] virtual Status save(const std::filesystem::path& path, const Workspace& workspace) const = 0;
  [[nodiscard]] virtual Result<Workspace> load(const std::filesystem::path& path, bool read_only = false) const = 0;
};

class WorkspaceStore final : public IWorkspaceStore {
public:
  static constexpr SchemaVersion supported_schema{1, 0};
  [[nodiscard]] Result<Workspace> create(ProjectId project_id) const;
  [[nodiscard]] Status save(const std::filesystem::path& path, const Workspace& workspace) const override;
  [[nodiscard]] Result<Workspace> load(const std::filesystem::path& path, bool read_only = false) const override;
  [[nodiscard]] Status autosave(const std::filesystem::path& project_path, const Workspace& workspace) const;
  [[nodiscard]] Result<Workspace> recover_autosave(const std::filesystem::path& project_path) const;
  [[nodiscard]] Status close(Workspace& workspace) const noexcept;
  [[nodiscard]] Status relocate(Workspace& workspace, const std::filesystem::path& old_root,
                                const std::filesystem::path& new_root) const;
};

class RecentProjectStore final {
public:
  explicit RecentProjectStore(std::filesystem::path storage_path);
  [[nodiscard]] Status record(const std::filesystem::path& project_path);
  [[nodiscard]] Result<std::vector<std::filesystem::path>> load() const;

private:
  std::filesystem::path storage_path_;
};

struct CurrentContext final {
  ProjectId project_id;
  std::string data_source_id;
  DataSourceVersionId data_source_version_id;
  std::uint64_t generation{};
  friend bool operator==(const CurrentContext&, const CurrentContext&) = default;
};
class CurrentContextStore final {
public:
  [[nodiscard]] Status switch_to(CurrentContext context);
  [[nodiscard]] CurrentContext snapshot() const;
  [[nodiscard]] Status validate_consistency(std::span<const CurrentContext> consumers) const;
  [[nodiscard]] Result<std::vector<WorkspaceObject>> select_current_objects(const Workspace& workspace,
                                                                            std::string_view kind = {}) const;

private:
  mutable std::shared_mutex mutex_;
  CurrentContext current_;
};

} // namespace signal::core
