#pragma once

#include "signal_studio/data/signal.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace signal::data {

enum class LoadProgressState : std::uint8_t {
  not_started,
  reading,
  cancellation_requested,
  cancelled,
  complete,
  failed
};
enum class DataSourceState : std::uint8_t { unavailable, loading, partial_read_available, complete };
enum class ImportRecoveryAction : std::uint8_t { retry, edit_parameters, cancel, view_log };

struct LoadLogEntry final {
  std::uint64_t sequence{};
  std::string message;
};

struct ImportFailure final {
  core::Status status;
  std::vector<ImportRecoveryAction> recovery_actions;
  std::string log_uri;
};

struct LoadPlan final {
  SampleRange requested_range;
  std::uint64_t target_bytes{};
  std::uint64_t frame_bytes{};
  std::uint64_t chunk_bytes{};
};

[[nodiscard]] core::Result<LoadPlan> make_initial_load_plan(const std::filesystem::path& path,
                                                            const SignalDescriptor& descriptor,
                                                            std::uint64_t start_sample,
                                                            std::uint64_t configured_initial_bytes,
                                                            std::uint64_t configured_chunk_bytes);

class LoadedDataRange final {
public:
  LoadedDataRange(SampleRange range, SignalBuffer samples, std::string data_source_version_id);
  [[nodiscard]] const SampleRange& range() const noexcept {
    return range_;
  }
  [[nodiscard]] SignalSlice samples() const noexcept {
    return samples_.view();
  }
  [[nodiscard]] const std::string& data_source_version_id() const noexcept {
    return data_source_version_id_;
  }

private:
  SampleRange range_;
  SignalBuffer samples_;
  std::string data_source_version_id_;
};

struct LoadSnapshot final {
  LoadProgressState progress_state{LoadProgressState::not_started};
  DataSourceState source_state{DataSourceState::unavailable};
  SampleRange requested_range;
  std::uint64_t next_sample{};
  std::uint64_t bytes_read{};
  double progress{};
  std::optional<ImportFailure> failure;
  std::vector<LoadLogEntry> logs;
  std::shared_ptr<const LoadedDataRange> published_range;
};

class IncrementalLoader final {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<IncrementalLoader>>
  create(std::string data_source_version_id, std::filesystem::path path, SignalDescriptor descriptor, LoadPlan plan);
  [[nodiscard]] core::Status start();
  [[nodiscard]] core::Status process_next();
  [[nodiscard]] core::Status cancel();
  [[nodiscard]] LoadSnapshot snapshot() const;

private:
  IncrementalLoader(std::string data_source_version_id, std::filesystem::path path, SignalDescriptor descriptor,
                    LoadPlan plan);
  void publish(bool complete);
  void fail(core::Status error);

  std::string data_source_version_id_;
  std::filesystem::path path_;
  SignalDescriptor descriptor_;
  LoadPlan plan_;
  LoadProgressState progress_state_{LoadProgressState::not_started};
  DataSourceState source_state_{DataSourceState::unavailable};
  std::uint64_t next_sample_{};
  std::uint64_t bytes_read_{};
  std::vector<double> real_samples_;
  std::vector<ComplexSample> complex_samples_;
  std::optional<ImportFailure> failure_;
  std::vector<LoadLogEntry> logs_;
  std::shared_ptr<const LoadedDataRange> published_range_;
  mutable std::mutex mutex_;
};

} // namespace signal::data
