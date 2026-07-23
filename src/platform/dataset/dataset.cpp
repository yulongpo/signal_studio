#include "signal_studio/dataset/dataset.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace signal::dataset {

namespace {
core::Status dataset_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::dataset, reason}, std::move(message));
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

// Minimal JSON array-of-objects serializer for SampleRecord. Hand-written to avoid any third-party
// JSON dependency (consistent with the data module's sidecar serializer).
std::string serialize_manifest(const std::vector<SampleRecord>& records) {
  std::ostringstream out;
  out << "{\"schema\":\"signal-studio.dataset/1.0\",\"records\":[";
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& r = records[i];
    if (i) out << ',';
    out << "{\"sampleId\":\"" << json_escape(r.sample_id) << "\",\"dataPath\":\""
        << json_escape(r.data_path.generic_string()) << "\",\"label\":\"" << json_escape(r.label)
        << "\",\"sourceFormat\":\"" << json_escape(r.source_format) << "\",\"sampleCount\":"
        << r.sample_count << ",\"sampleRateHz\":" << r.sample_rate_hz << ",\"sha256\":\""
        << json_escape(r.sha256_digest) << "\"}";
  }
  out << "]}";
  return out.str();
}

// Minimal lenient JSON parser for the manifest schema above. Reads only the fields we write.
std::vector<SampleRecord> parse_manifest(std::string_view text) {
  std::vector<SampleRecord> records;
  auto find_field = [&](std::string_view obj, std::string_view key) -> std::string {
    std::string needle = "\"" + std::string(key) + "\":";
    auto pos = obj.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    if (pos >= obj.size()) return {};
    if (obj[pos] == '"') {
      auto end = obj.find('"', pos + 1);
      if (end == std::string_view::npos) return {};
      return std::string(obj.substr(pos + 1, end - pos - 1));
    }
    auto end = obj.find_first_of(",}", pos);
    if (end == std::string_view::npos) end = obj.size();
    return std::string(obj.substr(pos, end - pos));
  };
  // Split top-level records array into objects by brace matching.
  auto arr = text.find("\"records\":[");
  if (arr == std::string_view::npos) return records;
  std::size_t i = arr + std::string("\"records\":[").size();
  while (i < text.size()) {
    auto obj_start = text.find('{', i);
    if (obj_start == std::string_view::npos) break;
    int depth = 0;
    std::size_t j = obj_start;
    for (; j < text.size(); ++j) {
      if (text[j] == '{') ++depth;
      else if (text[j] == '}') {
        --depth;
        if (depth == 0) break;
      }
    }
    if (depth != 0) break;
    std::string_view obj = text.substr(obj_start, j - obj_start + 1);
    SampleRecord r;
    r.sample_id = find_field(obj, "sampleId");
    r.data_path = find_field(obj, "dataPath");
    r.label = find_field(obj, "label");
    r.source_format = find_field(obj, "sourceFormat");
    try {
      r.sample_count = std::stoull(find_field(obj, "sampleCount"));
      r.sample_rate_hz = std::stod(find_field(obj, "sampleRateHz"));
    } catch (...) {
      // Malformed numeric field: leave defaults.
    }
    r.sha256_digest = find_field(obj, "sha256");
    records.push_back(std::move(r));
    i = j + 1;
    if (i < text.size() && text[i] == ']') break;
  }
  return records;
}
}  // namespace

JsonFileDataset::JsonFileDataset(std::filesystem::path manifest_path) : manifest_path_(std::move(manifest_path)) {
  if (std::error_code ec; std::filesystem::exists(manifest_path_, ec)) {
    std::ifstream in(manifest_path_);
    if (in) {
      std::ostringstream ss;
      ss << in.rdbuf();
      records_ = parse_manifest(ss.str());
    }
  }
}

std::string JsonFileDataset::id() const {
  return "dataset.json:" + manifest_path_.filename().generic_string();
}

std::vector<SampleRecord> JsonFileDataset::query(const SampleQuery& q) const {
  std::vector<SampleRecord> out;
  for (const auto& r : records_) {
    if (q.label && r.label != *q.label) continue;
    if (q.source_format && r.source_format != *q.source_format) continue;
    out.push_back(r);
    if (q.limit && out.size() >= *q.limit) break;
  }
  return out;
}

std::uint64_t JsonFileDataset::size() const noexcept {
  return records_.size();
}

core::Status JsonFileDataset::append(SampleRecord record) {
  if (record.sample_id.empty()) {
    return dataset_failure(core::ErrorReason::invalid_argument, "sample id must be non-empty");
  }
  records_.push_back(std::move(record));
  return core::Status::success();
}

core::Status JsonFileDataset::commit() {
  if (manifest_path_.empty()) {
    return dataset_failure(core::ErrorReason::invalid_argument, "manifest path is empty");
  }
  std::error_code ec;
  std::filesystem::create_directories(manifest_path_.parent_path(), ec);
  std::ofstream out(manifest_path_, std::ios::binary | std::ios::trunc);
  if (!out) {
    return dataset_failure(core::ErrorReason::internal_failure, "cannot open manifest for writing");
  }
  out << serialize_manifest(records_);
  if (!out) {
    return dataset_failure(core::ErrorReason::internal_failure, "manifest write failed");
  }
  return core::Status::success();
}

}  // namespace signal::dataset
