#include "signal_studio/core/artifact.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace signal::core {
namespace {

[[nodiscard]] Status failure(ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return Status::failure({ErrorDomain::core, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] bool valid_token(std::string_view value) {
  return !value.empty() && value.size() <= 128U && std::ranges::all_of(value, [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
  });
}

[[nodiscard]] bool known_kind(ArtifactKind kind) noexcept {
  return kind >= ArtifactKind::measurement && kind <= ArtifactKind::plugin_defined;
}

[[nodiscard]] bool known_format(ArtifactFormat format) noexcept {
  return format >= ArtifactFormat::json && format <= ArtifactFormat::plugin_defined;
}

[[nodiscard]] std::string json_quote(std::string_view value) {
  std::ostringstream output;
  output << '"';
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20U) {
        output << "\\u00" << hex[character >> 4U] << hex[character & 0x0FU];
      } else {
        output << static_cast<char>(character);
      }
    }
  }
  output << '"';
  return output.str();
}

[[nodiscard]] std::string csv_quote(std::string_view value) {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string{value};
  }
  std::string result{"\""};
  for (const char character : value) {
    if (character == '"') {
      result += "\"\"";
    } else {
      result.push_back(character);
    }
  }
  result.push_back('"');
  return result;
}

[[nodiscard]] std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> result(text.size());
  if (!text.empty()) {
    std::memcpy(result.data(), text.data(), text.size());
  }
  return result;
}

[[nodiscard]] std::string payload_extension(ArtifactFormat format) {
  switch (format) {
  case ArtifactFormat::json:
    return "json";
  case ArtifactFormat::csv:
    return "csv";
  case ArtifactFormat::png:
    return "png";
  case ArtifactFormat::raw:
    return "raw";
  case ArtifactFormat::wav:
    return "wav";
  case ArtifactFormat::plugin_defined:
    return "bin";
  }
  return "bin";
}

[[nodiscard]] bool payload_has_text(std::span<const std::byte> payload, std::string_view needle) {
  if (payload.size() > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    return false;
  }
  const auto text = std::string_view{reinterpret_cast<const char*>(payload.data()), payload.size()};
  return text.find(needle) != std::string_view::npos;
}

[[nodiscard]] Status validate_payload(const ArtifactDescriptor& descriptor, std::span<const std::byte> payload) {
  if (payload.empty()) {
    return failure(ErrorReason::invalid_argument, "结果制品载荷不能为空");
  }
  switch (descriptor.format) {
  case ArtifactFormat::json: {
    const auto text = std::string_view{reinterpret_cast<const char*>(payload.data()), payload.size()};
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos || (text[first] != '{' && text[first] != '[') ||
        !payload_has_text(payload, "\"schema\"") || !payload_has_text(payload, "\"provenance\"") ||
        !payload_has_text(payload, "\"units\"")) {
      return failure(ErrorReason::invalid_argument, "JSON 结果必须包含 schema、provenance 和 units");
    }
    break;
  }
  case ArtifactFormat::csv:
    if (!payload_has_text(payload, "# schema,") || !payload_has_text(payload, "# provenance.") ||
        !payload_has_text(payload, "# unit.")) {
      return failure(ErrorReason::invalid_argument, "CSV 结果必须包含 schema、provenance 和 unit 头");
    }
    break;
  case ArtifactFormat::png: {
    constexpr std::array<std::uint8_t, 8> signature{0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    if (payload.size() < signature.size() ||
        !std::equal(signature.begin(), signature.end(), reinterpret_cast<const std::uint8_t*>(payload.data()))) {
      return failure(ErrorReason::invalid_argument, "PNG 结果缺少有效文件签名");
    }
    break;
  }
  case ArtifactFormat::raw:
    if (!descriptor.metadata.contains("signalDescriptor")) {
      return failure(ErrorReason::invalid_argument, "RAW 结果必须携带可重新导入的 SignalDescriptor");
    }
    break;
  case ArtifactFormat::wav:
    if (payload.size() < 12U || std::memcmp(payload.data(), "RIFF", 4U) != 0 ||
        std::memcmp(payload.data() + 8U, "WAVE", 4U) != 0 || !descriptor.metadata.contains("signalDescriptor") ||
        !descriptor.metadata.contains("bitDepth") || !descriptor.metadata.contains("sampleRateHz") ||
        !descriptor.metadata.contains("normalizationPolicy")) {
      return failure(ErrorReason::invalid_argument, "WAV 结果必须包含有效 RIFF/WAVE 与位深、采样率、归一化策略");
    }
    break;
  case ArtifactFormat::plugin_defined:
    if (descriptor.plugin_format.empty()) {
      return failure(ErrorReason::invalid_argument, "插件结果必须声明格式");
    }
    break;
  }
  return Status::success();
}

[[nodiscard]] std::string serialize_manifest(const ArtifactDescriptor& descriptor, const Hash256& digest,
                                             std::uint64_t size, std::string_view payload_name) {
  const auto map_json = [](const auto& values) {
    std::string text{"{"};
    bool first = true;
    for (const auto& [key, value] : values) {
      if (!first) {
        text += ',';
      }
      first = false;
      text += json_quote(key) + ':' + json_quote(value);
    }
    return text + '}';
  };
  const auto& provenance = descriptor.provenance;
  std::ostringstream output;
  output << "{\"schema\":" << json_quote(descriptor.schema) << ",\"id\":" << json_quote(descriptor.id)
         << ",\"kind\":" << json_quote(artifact_kind_name(descriptor.kind))
         << ",\"format\":" << json_quote(artifact_format_name(descriptor.format))
         << ",\"pluginFormat\":" << json_quote(descriptor.plugin_format) << ",\"provenance\":{"
         << "\"projectId\":" << json_quote(provenance.project_id)
         << ",\"dataSourceVersionId\":" << json_quote(provenance.data_source_version_id)
         << ",\"selectionId\":" << json_quote(provenance.selection_id)
         << ",\"channelId\":" << json_quote(provenance.channel_id)
         << ",\"channelVersion\":" << json_quote(provenance.channel_version)
         << ",\"taskId\":" << json_quote(provenance.task_id)
         << ",\"algorithmId\":" << json_quote(provenance.algorithm_id)
         << ",\"algorithmVersion\":" << json_quote(provenance.algorithm_version)
         << ",\"parameterVersion\":" << json_quote(provenance.parameter_version)
         << "},\"units\":" << map_json(descriptor.units) << ",\"metadata\":" << map_json(descriptor.metadata)
         << ",\"payload\":{"
         << "\"file\":" << json_quote(payload_name) << ",\"sha256\":" << json_quote(digest.hex())
         << ",\"bytes\":" << size << "}}\n";
  return output.str();
}

[[nodiscard]] std::string serialize_index(const ArtifactRecord& record) {
  const auto& descriptor = record.descriptor;
  const auto& provenance = descriptor.provenance;
  std::ostringstream output;
  output << "signal-artifact-index/1.0\n";
  output << std::quoted(descriptor.id) << ' ' << static_cast<unsigned>(descriptor.kind) << ' '
         << static_cast<unsigned>(descriptor.format) << ' ' << std::quoted(descriptor.schema) << ' '
         << std::quoted(descriptor.plugin_format) << '\n';
  output << std::quoted(provenance.project_id) << ' ' << std::quoted(provenance.data_source_version_id) << ' '
         << std::quoted(provenance.selection_id) << ' ' << std::quoted(provenance.channel_id) << ' '
         << std::quoted(provenance.channel_version) << ' ' << std::quoted(provenance.task_id) << ' '
         << std::quoted(provenance.algorithm_id) << ' ' << std::quoted(provenance.algorithm_version) << ' '
         << std::quoted(provenance.parameter_version) << '\n';
  output << descriptor.units.size() << '\n';
  for (const auto& [key, value] : descriptor.units) {
    output << std::quoted(key) << ' ' << std::quoted(value) << '\n';
  }
  output << descriptor.metadata.size() << '\n';
  for (const auto& [key, value] : descriptor.metadata) {
    output << std::quoted(key) << ' ' << std::quoted(value) << '\n';
  }
  output << std::quoted(record.payload_path.filename().string()) << ' ' << record.payload_digest.hex() << ' '
         << record.payload_bytes << '\n';
  return output.str();
}

[[nodiscard]] Result<ArtifactRecord> parse_index(const std::filesystem::path& package) {
  std::ifstream input(package / ".artifact-index", std::ios::binary);
  if (!input) {
    return failure(ErrorReason::unavailable, "结果制品内部索引不存在", package.string());
  }
  std::string signature;
  std::getline(input, signature);
  if (signature != "signal-artifact-index/1.0") {
    return failure(ErrorReason::invalid_argument, "结果制品内部索引版本不兼容", package.string());
  }
  ArtifactRecord record;
  unsigned kind{};
  unsigned format{};
  if (!(input >> std::quoted(record.descriptor.id) >> kind >> format >> std::quoted(record.descriptor.schema) >>
        std::quoted(record.descriptor.plugin_format)) ||
      kind > static_cast<unsigned>(ArtifactKind::plugin_defined) ||
      format > static_cast<unsigned>(ArtifactFormat::plugin_defined)) {
    return failure(ErrorReason::invalid_argument, "结果制品内部索引头损坏", package.string());
  }
  record.descriptor.kind = static_cast<ArtifactKind>(kind);
  record.descriptor.format = static_cast<ArtifactFormat>(format);
  auto& provenance = record.descriptor.provenance;
  if (!(input >> std::quoted(provenance.project_id) >> std::quoted(provenance.data_source_version_id) >>
        std::quoted(provenance.selection_id) >> std::quoted(provenance.channel_id) >>
        std::quoted(provenance.channel_version) >> std::quoted(provenance.task_id) >>
        std::quoted(provenance.algorithm_id) >> std::quoted(provenance.algorithm_version) >>
        std::quoted(provenance.parameter_version))) {
    return failure(ErrorReason::invalid_argument, "结果制品来源索引损坏", package.string());
  }
  const auto read_map = [&input, &package](auto& values) -> Status {
    std::size_t count{};
    if (!(input >> count) || count > 4096U) {
      return failure(ErrorReason::invalid_argument, "结果制品键值数量无效", package.string());
    }
    for (std::size_t index = 0; index < count; ++index) {
      std::string key;
      std::string value;
      if (!(input >> std::quoted(key) >> std::quoted(value)) ||
          !values.emplace(std::move(key), std::move(value)).second) {
        return failure(ErrorReason::invalid_argument, "结果制品键值索引损坏", package.string());
      }
    }
    return Status::success();
  };
  if (const auto status = read_map(record.descriptor.units); !status) {
    return status;
  }
  if (const auto status = read_map(record.descriptor.metadata); !status) {
    return status;
  }
  std::string payload_name;
  std::string digest;
  if (!(input >> std::quoted(payload_name) >> digest >> record.payload_bytes) || digest.size() != 64U) {
    return failure(ErrorReason::invalid_argument, "结果制品载荷索引损坏", package.string());
  }
  record.package_path = package;
  record.payload_path = package / payload_name;
  for (std::size_t index = 0; index < record.payload_digest.bytes.size(); ++index) {
    unsigned value{};
    const auto parsed = std::from_chars(digest.data() + index * 2U, digest.data() + index * 2U + 2U, value, 16);
    if (parsed.ec != std::errc{}) {
      return failure(ErrorReason::invalid_argument, "结果制品摘要索引损坏", package.string());
    }
    record.payload_digest.bytes[index] = static_cast<std::uint8_t>(value);
  }
  return record;
}

[[nodiscard]] bool matches(const ArtifactRecord& record, const ArtifactFilter& filter) {
  const auto& descriptor = record.descriptor;
  const auto& provenance = descriptor.provenance;
  return (!filter.project_id || provenance.project_id == *filter.project_id) &&
         (!filter.data_source_version_id || provenance.data_source_version_id == *filter.data_source_version_id) &&
         (!filter.selection_id || provenance.selection_id == *filter.selection_id) &&
         (!filter.channel_id || provenance.channel_id == *filter.channel_id) &&
         (!filter.kind || descriptor.kind == *filter.kind);
}

[[nodiscard]] std::filesystem::path staging_path(const std::filesystem::path& destination) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return destination.parent_path() / (destination.filename().string() + ".staging-" + std::to_string(stamp));
}

[[nodiscard]] Status commit_directory(const std::filesystem::path& staging, const std::filesystem::path& destination) {
  std::error_code error;
  if (std::filesystem::exists(destination, error)) {
    return failure(ErrorReason::unavailable, "目标已存在，结果不会被静默覆盖", destination.string());
  }
  std::filesystem::rename(staging, destination, error);
  return error ? failure(ErrorReason::internal_failure, "结果目录原子提交失败", error.message()) : Status::success();
}

} // namespace

std::string_view artifact_kind_name(ArtifactKind kind) noexcept {
  switch (kind) {
  case ArtifactKind::measurement:
    return "measurement";
  case ArtifactKind::estimation:
    return "estimation";
  case ArtifactKind::identification:
    return "identification";
  case ArtifactKind::demodulation:
    return "demodulation";
  case ArtifactKind::spectrum:
    return "spectrum";
  case ArtifactKind::sampled_data:
    return "sampled-data";
  case ArtifactKind::audio:
    return "audio";
  case ArtifactKind::export_record:
    return "export-record";
  case ArtifactKind::plugin_defined:
    return "plugin-defined";
  }
  return "unknown";
}

std::string_view artifact_format_name(ArtifactFormat format) noexcept {
  switch (format) {
  case ArtifactFormat::json:
    return "json";
  case ArtifactFormat::csv:
    return "csv";
  case ArtifactFormat::png:
    return "png";
  case ArtifactFormat::raw:
    return "raw";
  case ArtifactFormat::wav:
    return "wav";
  case ArtifactFormat::plugin_defined:
    return "plugin-defined";
  }
  return "unknown";
}

Status validate_artifact_descriptor(const ArtifactDescriptor& descriptor) {
  const auto& provenance = descriptor.provenance;
  if (descriptor.schema != "signal.artifact/1.0" || !valid_token(descriptor.id) || !known_kind(descriptor.kind) ||
      !known_format(descriptor.format) || provenance.project_id.empty() || provenance.data_source_version_id.empty() ||
      provenance.algorithm_id.empty() || provenance.algorithm_version.empty() || provenance.parameter_version.empty()) {
    return failure(ErrorReason::invalid_argument, "结果制品描述符缺少稳定身份、来源或版本");
  }
  if (descriptor.kind == ArtifactKind::plugin_defined && descriptor.plugin_format.empty()) {
    return failure(ErrorReason::invalid_argument, "插件结果类型必须声明格式");
  }
  for (const auto& [key, value] : descriptor.units) {
    if (!valid_token(key) || value.empty()) {
      return failure(ErrorReason::invalid_argument, "结果制品单位表包含无效项");
    }
  }
  return Status::success();
}

bool artifact_is_current(const ArtifactRecord& artifact, std::string_view current_data_source_version_id,
                         std::string_view current_channel_version,
                         std::string_view current_parameter_version) noexcept {
  const auto& provenance = artifact.descriptor.provenance;
  return provenance.data_source_version_id == current_data_source_version_id &&
         (provenance.channel_version.empty() || provenance.channel_version == current_channel_version) &&
         provenance.parameter_version == current_parameter_version;
}

Result<std::vector<std::byte>> make_artifact_csv(std::string_view schema, const ArtifactProvenance& provenance,
                                                 const std::map<std::string, std::string, std::less<>>& units,
                                                 std::span<const std::string> columns,
                                                 std::span<const std::vector<std::string>> rows) {
  if (schema.empty() || provenance.project_id.empty() || provenance.data_source_version_id.empty() || units.empty() ||
      columns.empty() ||
      std::ranges::any_of(rows, [columns](const auto& row) { return row.size() != columns.size(); })) {
    return failure(ErrorReason::invalid_argument, "CSV 结果的 schema、来源、单位或列数无效");
  }
  std::ostringstream output;
  output << "# schema," << csv_quote(schema) << '\n';
  output << "# provenance.projectId," << csv_quote(provenance.project_id) << '\n';
  output << "# provenance.dataSourceVersionId," << csv_quote(provenance.data_source_version_id) << '\n';
  output << "# provenance.selectionId," << csv_quote(provenance.selection_id) << '\n';
  output << "# provenance.channelId," << csv_quote(provenance.channel_id) << '\n';
  output << "# provenance.channelVersion," << csv_quote(provenance.channel_version) << '\n';
  output << "# provenance.taskId," << csv_quote(provenance.task_id) << '\n';
  output << "# provenance.algorithm," << csv_quote(provenance.algorithm_id + "@" + provenance.algorithm_version)
         << '\n';
  output << "# provenance.parameterVersion," << csv_quote(provenance.parameter_version) << '\n';
  for (const auto& [key, value] : units) {
    output << "# unit." << csv_quote(key) << ',' << csv_quote(value) << '\n';
  }
  for (std::size_t index = 0; index < columns.size(); ++index) {
    output << (index == 0U ? "" : ",") << csv_quote(columns[index]);
  }
  output << '\n';
  for (const auto& row : rows) {
    for (std::size_t index = 0; index < row.size(); ++index) {
      output << (index == 0U ? "" : ",") << csv_quote(row[index]);
    }
    output << '\n';
  }
  return bytes(output.str());
}

Result<std::vector<std::byte>> make_artifact_json(std::string_view schema, const ArtifactProvenance& provenance,
                                                  const std::map<std::string, std::string, std::less<>>& units,
                                                  std::string_view data_json) {
  if (schema.empty() || provenance.project_id.empty() || provenance.data_source_version_id.empty() || units.empty() ||
      data_json.empty()) {
    return failure(ErrorReason::invalid_argument, "JSON 结果的 schema、来源、单位或数据无效");
  }
  const auto first = data_json.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos || (data_json[first] != '{' && data_json[first] != '[')) {
    return failure(ErrorReason::invalid_argument, "JSON 结果数据必须是对象或数组");
  }
  std::ostringstream output;
  output << "{\"schema\":" << json_quote(schema) << ",\"provenance\":{"
         << "\"projectId\":" << json_quote(provenance.project_id)
         << ",\"dataSourceVersionId\":" << json_quote(provenance.data_source_version_id)
         << ",\"selectionId\":" << json_quote(provenance.selection_id)
         << ",\"channelId\":" << json_quote(provenance.channel_id)
         << ",\"channelVersion\":" << json_quote(provenance.channel_version)
         << ",\"taskId\":" << json_quote(provenance.task_id)
         << ",\"algorithmId\":" << json_quote(provenance.algorithm_id)
         << ",\"algorithmVersion\":" << json_quote(provenance.algorithm_version)
         << ",\"parameterVersion\":" << json_quote(provenance.parameter_version) << "},\"units\":{";
  bool first_unit = true;
  for (const auto& [key, value] : units) {
    if (!first_unit) {
      output << ',';
    }
    first_unit = false;
    output << json_quote(key) << ':' << json_quote(value);
  }
  output << "},\"data\":" << data_json << "}\n";
  return bytes(output.str());
}

ArtifactStore::ArtifactStore(std::filesystem::path root) : root_(std::move(root)) {}

Status ArtifactStore::recover() {
  std::error_code error;
  std::filesystem::create_directories(root_, error);
  if (error) {
    return failure(ErrorReason::unavailable, "结果存储目录不可创建", error.message());
  }
  for (const auto& entry : std::filesystem::directory_iterator(root_, error)) {
    if (error) {
      return failure(ErrorReason::internal_failure, "结果存储目录不可枚举", error.message());
    }
    if (entry.is_directory() && entry.path().filename().string().find(".staging-") != std::string::npos) {
      std::filesystem::remove_all(entry.path(), error);
      if (error) {
        return failure(ErrorReason::internal_failure, "失败的结果暂存目录不可清理", error.message());
      }
    }
  }
  return Status::success();
}

Result<ArtifactRecord> ArtifactStore::commit(const ArtifactDescriptor& descriptor, std::span<const std::byte> payload) {
  if (const auto status = validate_artifact_descriptor(descriptor); !status) {
    return status;
  }
  if (const auto status = validate_payload(descriptor, payload); !status) {
    return status;
  }
  if (const auto status = recover(); !status) {
    return status;
  }
  const auto destination = root_ / descriptor.id;
  std::error_code error;
  if (std::filesystem::exists(destination, error)) {
    return failure(ErrorReason::unavailable, "结果 ID 已存在，禁止静默覆盖", descriptor.id);
  }
  const auto staging = staging_path(destination);
  std::filesystem::create_directories(staging, error);
  if (error) {
    return failure(ErrorReason::unavailable, "结果暂存目录不可创建", error.message());
  }
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
  };
  const auto payload_name = "payload." + payload_extension(descriptor.format);
  const auto payload_path = staging / payload_name;
  if (const auto status = AtomicFileStore{}.write(payload_path, payload); !status) {
    cleanup();
    return status.with_context("写入结果载荷");
  }
  auto digest = hash_file(payload_path);
  if (!digest) {
    cleanup();
    return digest.error();
  }
  ArtifactRecord staged{descriptor, staging, payload_path, digest.value(), static_cast<std::uint64_t>(payload.size())};
  const auto manifest = serialize_manifest(descriptor, staged.payload_digest, staged.payload_bytes, payload_name);
  const auto index = serialize_index(staged);
  if (const auto status = AtomicFileStore{}.write(staging / "manifest.json", bytes(manifest)); !status) {
    cleanup();
    return status.with_context("写入结果清单");
  }
  if (const auto status = AtomicFileStore{}.write(staging / ".artifact-index", bytes(index)); !status) {
    cleanup();
    return status.with_context("写入结果内部索引");
  }
  if (const auto status = commit_directory(staging, destination); !status) {
    cleanup();
    return status;
  }
  staged.package_path = destination;
  staged.payload_path = destination / payload_name;
  return staged;
}

Result<std::vector<ArtifactRecord>> ArtifactStore::query(const ArtifactFilter& filter) const {
  std::vector<ArtifactRecord> records;
  std::error_code error;
  if (!std::filesystem::exists(root_, error)) {
    return records;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root_, error)) {
    if (error) {
      return failure(ErrorReason::internal_failure, "结果存储目录不可枚举", error.message());
    }
    if (!entry.is_directory() || entry.path().filename().string().find(".staging-") != std::string::npos) {
      continue;
    }
    auto record = parse_index(entry.path());
    if (!record) {
      return record.error();
    }
    if (const auto status = verify(record.value()); !status) {
      return status;
    }
    if (matches(record.value(), filter)) {
      records.push_back(std::move(record).value());
    }
  }
  std::ranges::sort(records,
                    [](const auto& left, const auto& right) { return left.descriptor.id < right.descriptor.id; });
  return records;
}

Status ArtifactStore::verify(const ArtifactRecord& record) const {
  std::error_code error;
  const auto size = std::filesystem::file_size(record.payload_path, error);
  if (error || size != record.payload_bytes) {
    return failure(ErrorReason::internal_failure, "结果载荷大小校验失败", record.payload_path.string());
  }
  auto digest = hash_file(record.payload_path);
  if (!digest || digest.value() != record.payload_digest) {
    return failure(ErrorReason::internal_failure, "结果载荷 SHA-256 完整性校验失败", record.payload_path.string());
  }
  return Status::success();
}

Result<std::filesystem::path> ArtifactStore::export_package(const ArtifactRecord& record,
                                                            const std::filesystem::path& destination) const {
  if (const auto status = verify(record); !status) {
    return status;
  }
  if (destination.empty()) {
    return failure(ErrorReason::invalid_argument, "导出目标不能为空");
  }
  std::error_code error;
  if (std::filesystem::exists(destination, error)) {
    return failure(ErrorReason::unavailable, "导出目标已存在，禁止静默覆盖", destination.string());
  }
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    return failure(ErrorReason::unavailable, "导出父目录不可创建", error.message());
  }
  const auto staging = staging_path(destination);
  std::filesystem::copy(record.package_path, staging, std::filesystem::copy_options::recursive, error);
  if (error) {
    std::filesystem::remove_all(staging, error);
    return failure(ErrorReason::internal_failure, "结果包复制失败", error.message());
  }
  if (const auto status = commit_directory(staging, destination); !status) {
    std::filesystem::remove_all(staging, error);
    return status;
  }
  return destination;
}

Result<std::filesystem::path> ArtifactStore::export_batch(std::span<const ArtifactRecord> records,
                                                          const ArtifactExportTemplate& export_template,
                                                          const std::filesystem::path& destination) const {
  if (!valid_token(export_template.id) || records.empty() || destination.empty()) {
    return failure(ErrorReason::invalid_argument, "批量导出模板、结果或目标无效");
  }
  std::error_code error;
  if (std::filesystem::exists(destination, error)) {
    return failure(ErrorReason::unavailable, "批量导出目标已存在，禁止静默覆盖", destination.string());
  }
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    return failure(ErrorReason::unavailable, "批量导出父目录不可创建", error.message());
  }
  const auto staging = staging_path(destination);
  std::filesystem::create_directories(staging, error);
  if (error) {
    return failure(ErrorReason::unavailable, "批量导出暂存目录不可创建", error.message());
  }
  std::vector<std::string> exported_ids;
  for (const auto& record : records) {
    if (!export_template.formats.empty() &&
        std::ranges::find(export_template.formats, record.descriptor.format) == export_template.formats.end()) {
      continue;
    }
    if (const auto status = verify(record); !status) {
      std::filesystem::remove_all(staging, error);
      return status;
    }
    std::filesystem::copy(record.package_path, staging / record.descriptor.id, std::filesystem::copy_options::recursive,
                          error);
    if (error) {
      std::filesystem::remove_all(staging, error);
      return failure(ErrorReason::internal_failure, "批量导出结果复制失败", error.message());
    }
    exported_ids.push_back(record.descriptor.id);
  }
  if (exported_ids.empty()) {
    std::filesystem::remove_all(staging, error);
    return failure(ErrorReason::unavailable, "批量导出模板未匹配任何结果");
  }
  if (export_template.include_batch_manifest) {
    std::ostringstream manifest;
    manifest << "{\"schema\":\"signal.artifact-batch/1.0\",\"template\":" << json_quote(export_template.id)
             << ",\"items\":[";
    for (std::size_t index = 0; index < exported_ids.size(); ++index) {
      manifest << (index == 0U ? "" : ",") << json_quote(exported_ids[index]);
    }
    manifest << "]}\n";
    if (const auto status = AtomicFileStore{}.write(staging / "batch-manifest.json", bytes(manifest.str())); !status) {
      std::filesystem::remove_all(staging, error);
      return status;
    }
  }
  if (const auto status = commit_directory(staging, destination); !status) {
    std::filesystem::remove_all(staging, error);
    return status;
  }
  return destination;
}

} // namespace signal::core
