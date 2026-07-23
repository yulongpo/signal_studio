#pragma once

#include "signal_studio/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace signal::dataset {

/// A single sample record in a dataset.
struct SampleRecord final {
  std::string sample_id;
  std::filesystem::path data_path;
  std::string label;
  std::string source_format;
  std::uint64_t sample_count{};
  double sample_rate_hz{};
  std::string sha256_digest;
  friend bool operator==(const SampleRecord&, const SampleRecord&) = default;
};

/// Query for dataset records (API-DSET-001).
struct SampleQuery final {
  std::optional<std::string> label;
  std::optional<std::string> source_format;
  std::optional<std::uint64_t> limit;
  friend bool operator==(const SampleQuery&, const SampleQuery&) = default;
};

class IDataset {
 public:
  virtual ~IDataset() = default;
  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual std::vector<SampleRecord> query(const SampleQuery& q) const = 0;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
};

class IDatasetWriter {
 public:
  virtual ~IDatasetWriter() = default;
  [[nodiscard]] virtual core::Status append(SampleRecord record) = 0;
  [[nodiscard]] virtual core::Status commit() = 0;
};

/// JSON-manifest dataset. A real, dependency-free implementation: records are stored in a JSON
/// file on disk and queried by label/format/limit. HDF5 is the BL1.0 reference format but is not
/// installed in this environment; the JSON adapter preserves the same public contract so callers
/// can switch to HDF5 later without API changes.
class JsonFileDataset final : public IDataset, public IDatasetWriter {
 public:
  JsonFileDataset() = default;
  explicit JsonFileDataset(std::filesystem::path manifest_path);

  [[nodiscard]] std::string id() const override;
  [[nodiscard]] std::vector<SampleRecord> query(const SampleQuery& q) const override;
  [[nodiscard]] std::uint64_t size() const noexcept override;

  [[nodiscard]] core::Status append(SampleRecord record) override;
  [[nodiscard]] core::Status commit() override;

  [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept { return manifest_path_; }
  [[nodiscard]] const std::vector<SampleRecord>& records() const noexcept { return records_; }

 private:
  std::filesystem::path manifest_path_;
  std::vector<SampleRecord> records_;
};

}  // namespace signal::dataset
