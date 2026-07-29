#pragma once

#include "signal_studio/core/services.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::core {

enum class ArtifactKind : std::uint8_t {
  measurement,
  estimation,
  identification,
  demodulation,
  spectrum,
  sampled_data,
  audio,
  export_record,
  plugin_defined,
};

enum class ArtifactFormat : std::uint8_t {
  json,
  csv,
  png,
  raw,
  wav,
  plugin_defined,
};

struct ArtifactProvenance final {
  ProjectId project_id;
  DataSourceVersionId data_source_version_id;
  std::string selection_id;
  std::string channel_id;
  std::string channel_version;
  std::string task_id;
  std::string algorithm_id;
  std::string algorithm_version;
  std::string parameter_version;
  friend bool operator==(const ArtifactProvenance&, const ArtifactProvenance&) = default;
};

struct ArtifactDescriptor final {
  std::string schema{"signal.artifact/1.0"};
  std::string id;
  ArtifactKind kind{ArtifactKind::measurement};
  ArtifactFormat format{ArtifactFormat::json};
  ArtifactProvenance provenance;
  std::map<std::string, std::string, std::less<>> units;
  std::map<std::string, std::string, std::less<>> metadata;
  std::string plugin_format;
  friend bool operator==(const ArtifactDescriptor&, const ArtifactDescriptor&) = default;
};

struct ArtifactRecord final {
  ArtifactDescriptor descriptor;
  std::filesystem::path package_path;
  std::filesystem::path payload_path;
  Hash256 payload_digest;
  std::uint64_t payload_bytes{};
  friend bool operator==(const ArtifactRecord&, const ArtifactRecord&) = default;
};

struct ArtifactFilter final {
  std::optional<ProjectId> project_id;
  std::optional<DataSourceVersionId> data_source_version_id;
  std::optional<std::string> selection_id;
  std::optional<std::string> channel_id;
  std::optional<ArtifactKind> kind;
};

struct ArtifactExportTemplate final {
  std::string id;
  std::vector<ArtifactFormat> formats;
  bool include_batch_manifest{true};
};

[[nodiscard]] Status validate_artifact_descriptor(const ArtifactDescriptor& descriptor);
[[nodiscard]] std::string_view artifact_kind_name(ArtifactKind kind) noexcept;
[[nodiscard]] std::string_view artifact_format_name(ArtifactFormat format) noexcept;
[[nodiscard]] bool artifact_is_current(const ArtifactRecord& artifact, std::string_view current_data_source_version_id,
                                       std::string_view current_channel_version,
                                       std::string_view current_parameter_version) noexcept;

/// 将表格结果编码为带 schema、单位和来源头的 UTF-8 CSV。
[[nodiscard]] Result<std::vector<std::byte>>
make_artifact_csv(std::string_view schema, const ArtifactProvenance& provenance,
                  const std::map<std::string, std::string, std::less<>>& units, std::span<const std::string> columns,
                  std::span<const std::vector<std::string>> rows);

/// 将任意 JSON 数据值包装为带 schema、单位和来源的结果文档。
[[nodiscard]] Result<std::vector<std::byte>>
make_artifact_json(std::string_view schema, const ArtifactProvenance& provenance,
                   const std::map<std::string, std::string, std::less<>>& units, std::string_view data_json);

class IArtifactStore {
public:
  virtual ~IArtifactStore() = default;
  [[nodiscard]] virtual Result<ArtifactRecord> commit(const ArtifactDescriptor& descriptor,
                                                      std::span<const std::byte> payload) = 0;
  [[nodiscard]] virtual Result<std::vector<ArtifactRecord>> query(const ArtifactFilter& filter = {}) const = 0;
  [[nodiscard]] virtual Status verify(const ArtifactRecord& record) const = 0;
  [[nodiscard]] virtual Result<std::filesystem::path>
  export_package(const ArtifactRecord& record, const std::filesystem::path& destination) const = 0;
  [[nodiscard]] virtual Result<std::filesystem::path> export_batch(std::span<const ArtifactRecord> records,
                                                                   const ArtifactExportTemplate& export_template,
                                                                   const std::filesystem::path& destination) const = 0;
};

/// 目录级原子提交的不可变制品存储。每项制品包含 payload、manifest.json 和内部索引。
class ArtifactStore final : public IArtifactStore {
public:
  explicit ArtifactStore(std::filesystem::path root);
  [[nodiscard]] Status recover();
  [[nodiscard]] Result<ArtifactRecord> commit(const ArtifactDescriptor& descriptor,
                                              std::span<const std::byte> payload) override;
  [[nodiscard]] Result<std::vector<ArtifactRecord>> query(const ArtifactFilter& filter = {}) const override;
  [[nodiscard]] Status verify(const ArtifactRecord& record) const override;
  [[nodiscard]] Result<std::filesystem::path> export_package(const ArtifactRecord& record,
                                                             const std::filesystem::path& destination) const override;
  [[nodiscard]] Result<std::filesystem::path> export_batch(std::span<const ArtifactRecord> records,
                                                           const ArtifactExportTemplate& export_template,
                                                           const std::filesystem::path& destination) const override;
  [[nodiscard]] const std::filesystem::path& root() const noexcept {
    return root_;
  }

private:
  std::filesystem::path root_;
};

} // namespace signal::core
