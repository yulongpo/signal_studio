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

enum class DataSourceState : std::uint8_t { unavailable, loading, partial_read_available, complete };

/// 导入领域错误；任务状态、进度和日志事件由 TaskRuntime 单独持有。
struct ImportErrorDetail final {
  core::Status status;
  bool retryable{true};
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

/// 已通过完整帧校验、可安全发布的数据范围。
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
  DataSourceState source_state{DataSourceState::unavailable};
  SampleRange requested_range;
  std::uint64_t next_sample{};
  std::uint64_t bytes_read{};
  bool has_remaining_chunks{};
  std::optional<ImportErrorDetail> error;
  std::shared_ptr<const LoadedDataRange> published_range;
};

/// 有界、单次尝试的导入会话；长时间循环必须由 ITaskService 处理器驱动。
class IncrementalLoader final {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<IncrementalLoader>>
  create(std::string data_source_version_id, std::filesystem::path path, SignalDescriptor descriptor, LoadPlan plan);
  /// 读取至多一个 frame-aligned chunk，供任务处理器在安全点之间调用。
  [[nodiscard]] core::Status process_next();
  /// 取消当前领域尝试；若已读取完整帧，则发布从请求起点到最后完整帧的只读部分范围。
  [[nodiscard]] core::Status cancel_import();
  /// 基于相同参数创建全新的可执行尝试；失败对象本身不会被重启。
  [[nodiscard]] core::Result<std::unique_ptr<IncrementalLoader>> retry() const;
  /// 应用修订后的来源、描述符和计划，并创建全新的可执行尝试。
  [[nodiscard]] core::Result<std::unique_ptr<IncrementalLoader>> edit_parameters(std::string data_source_version_id,
                                                                                 std::filesystem::path path,
                                                                                 SignalDescriptor descriptor,
                                                                                 LoadPlan plan) const;
  /// 返回失败尝试的稳定日志 URI，供 TaskRuntime::FailureInfo 引用。
  [[nodiscard]] core::Result<std::string> view_log() const;
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
  DataSourceState source_state_{DataSourceState::unavailable};
  std::uint64_t next_sample_{};
  std::uint64_t bytes_read_{};
  std::vector<double> real_samples_;
  std::vector<ComplexSample> complex_samples_;
  std::optional<ImportErrorDetail> error_;
  std::shared_ptr<const LoadedDataRange> published_range_;
  bool publication_allowed_{true};
  mutable std::mutex mutex_;
};

} // namespace signal::data
