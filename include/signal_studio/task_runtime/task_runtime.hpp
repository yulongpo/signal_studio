#pragma once

#include "signal_studio/core/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::task {

struct TaskId final {
  std::string value;
  [[nodiscard]] static TaskId generate();
  friend bool operator==(const TaskId&, const TaskId&) = default;
  friend auto operator<=>(const TaskId&, const TaskId&) = default;
};

struct ProjectId final {
  std::string value;
  friend bool operator==(const ProjectId&, const ProjectId&) = default;
};

struct DataSourceVersionId final {
  std::string value;
  friend bool operator==(const DataSourceVersionId&, const DataSourceVersionId&) = default;
};

struct SourceObject final {
  std::string type;
  std::string id;
  friend bool operator==(const SourceObject&, const SourceObject&) = default;
};

struct TaskProvenance final {
  ProjectId project_id;
  DataSourceVersionId data_source_version_id;
  SourceObject source_object;
  friend bool operator==(const TaskProvenance&, const TaskProvenance&) = default;
};

enum class TaskState : std::uint8_t {
  queued,
  running,
  paused,
  canceling,
  canceled,
  completed,
  failed,
  dependency_failed,
  stale,
};

enum class TaskPriority : std::uint8_t { background, foreground, interactive };

enum class WorkClass : std::uint8_t { io, dsp, indexing, exporting, inference, other };

struct SchedulingRequirement final {
  bool must_schedule{};
  std::chrono::milliseconds expected_duration{};
  WorkClass work_class{WorkClass::other};
};

/// 校验可由公共 API 构造的任务状态枚举值。
[[nodiscard]] core::Status validate_task_state(TaskState state);
/// 校验可由公共 API 构造的任务优先级枚举值。
[[nodiscard]] core::Status validate_task_priority(TaskPriority priority);
/// 校验可由公共 API 构造的工作类别枚举值。
[[nodiscard]] core::Status validate_work_class(WorkClass work_class);
/// 判断预计工作是否必须进入任务运行时；未知工作类别会返回错误。
[[nodiscard]] core::Result<SchedulingRequirement> evaluate_scheduling(WorkClass work_class,
                                                                      std::chrono::milliseconds expected_duration);

struct ResourceProfile final {
  std::uint32_t cpu_units{1};
  std::uint32_t io_units{};
  std::uint32_t gpu_units{};
  std::uint32_t runtime_threads{1};
  friend bool operator==(const ResourceProfile&, const ResourceProfile&) = default;
};

struct ResourceBudget final {
  std::uint32_t cpu_units{1};
  std::uint32_t io_units{1};
  std::uint32_t gpu_units{};
  std::uint32_t runtime_threads{1};
  friend bool operator==(const ResourceBudget&, const ResourceBudget&) = default;
};

struct ViewRequestId final {
  std::string scope;
  std::uint64_t generation{};
  friend bool operator==(const ViewRequestId&, const ViewRequestId&) = default;
};

struct FailureInfo final {
  std::string error_code;
  std::string user_message;
  std::string technical_details;
  std::string suggested_action;
  bool retryable{};
  std::string retry_action;
  std::string log_link;
  friend bool operator==(const FailureInfo&, const FailureInfo&) = default;
};

[[nodiscard]] core::Status validate_failure_info(const FailureInfo& failure);

struct CommittedArtifact final {
  std::filesystem::path path;
  std::string sha256_digest;
  std::uint64_t size_bytes{};
  friend bool operator==(const CommittedArtifact&, const CommittedArtifact&) = default;
};

struct TaskSpec final {
  TaskId task_id;
  std::string task_type;
  TaskPriority priority{TaskPriority::foreground};
  ResourceProfile resources;
  std::vector<TaskId> dependencies;
  std::string idempotency_key;
  TaskProvenance provenance;
  std::optional<ViewRequestId> view_request;
  std::chrono::milliseconds timeout{};
  std::uint32_t max_attempts{1};
  bool persistent{true};
};

struct TaskStatus final {
  TaskId task_id;
  std::string task_type;
  TaskState state{TaskState::queued};
  TaskPriority priority{TaskPriority::foreground};
  ResourceProfile resources;
  TaskProvenance provenance;
  std::optional<ViewRequestId> view_request;
  double progress{};
  std::string status_text;
  std::uint32_t attempt{1};
  std::uint64_t revision{};
  std::optional<FailureInfo> failure;
  std::vector<CommittedArtifact> committed_artifacts;
  std::chrono::system_clock::time_point submitted_at{};
  std::optional<std::chrono::system_clock::time_point> started_at;
  std::optional<std::chrono::system_clock::time_point> finished_at;
  friend bool operator==(const TaskStatus&, const TaskStatus&) = default;
};

struct TaskEvent final {
  std::uint64_t sequence{};
  TaskStatus status;
  friend bool operator==(const TaskEvent&, const TaskEvent&) = default;
};

class ITaskObserver {
public:
  virtual ~ITaskObserver() noexcept = default;
  virtual void on_event(const TaskEvent& event) noexcept = 0;
};

namespace detail {
class RuntimeControl;
class TaskContextState;
class ViewCommitState;
} // namespace detail

class TaskContext final {
public:
  TaskContext(const TaskContext&) = delete;
  TaskContext& operator=(const TaskContext&) = delete;

  [[nodiscard]] bool cancellation_requested() const noexcept;
  [[nodiscard]] bool checkpoint();
  /// 发布有限且位于 [0,1] 的单调进度；非法数值不会进入事件或持久历史。
  [[nodiscard]] bool report_progress(double progress, std::string status_text = {});
  [[nodiscard]] core::Status commit_artifact(const std::filesystem::path& temporary_path,
                                             const std::filesystem::path& final_path);
  /// Atomically records immutable files and seals the task as completed. This must be the final
  /// operation performed by a successful work body; cancellation/staleness wins before the seal.
  [[nodiscard]] core::Status complete_with_existing_artifacts(std::span<const std::filesystem::path> artifact_paths);
  [[nodiscard]] TaskId task_id() const;
  [[nodiscard]] std::uint32_t attempt() const noexcept;

private:
  explicit TaskContext(std::shared_ptr<detail::TaskContextState> state);
  std::shared_ptr<detail::TaskContextState> state_;
  friend class detail::RuntimeControl;
};

struct TaskExecutionResult final {
  bool succeeded{true};
  std::optional<FailureInfo> failure;

  [[nodiscard]] static TaskExecutionResult completed() noexcept;
  [[nodiscard]] static TaskExecutionResult failed(FailureInfo failure);
};

using TaskWork = std::function<TaskExecutionResult(TaskContext&)>;

struct TaskDefinition final {
  TaskSpec spec;
  TaskWork work;
};

class TaskHandle final {
public:
  TaskHandle();
  ~TaskHandle();
  TaskHandle(const TaskHandle&);
  TaskHandle& operator=(const TaskHandle&);
  TaskHandle(TaskHandle&&) noexcept;
  TaskHandle& operator=(TaskHandle&&) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const TaskId& id() const noexcept;
  [[nodiscard]] core::Status pause() const;
  [[nodiscard]] core::Status resume() const;
  [[nodiscard]] core::Status cancel() const;
  [[nodiscard]] core::Result<TaskStatus> status() const;
  [[nodiscard]] core::Result<TaskStatus> wait() const;
  [[nodiscard]] std::optional<TaskStatus> wait_for(std::chrono::milliseconds timeout) const;

private:
  TaskHandle(std::shared_ptr<detail::RuntimeControl> control, TaskId id);
  std::shared_ptr<detail::RuntimeControl> control_;
  TaskId id_;
  friend class TaskRuntime;
  friend class detail::RuntimeControl;
};

class ViewCommitPermit final {
public:
  ~ViewCommitPermit();
  ViewCommitPermit(ViewCommitPermit&&) noexcept;
  ViewCommitPermit& operator=(ViewCommitPermit&&) noexcept;
  ViewCommitPermit(const ViewCommitPermit&) = delete;
  ViewCommitPermit& operator=(const ViewCommitPermit&) = delete;

private:
  explicit ViewCommitPermit(std::unique_ptr<detail::ViewCommitState> state);
  std::unique_ptr<detail::ViewCommitState> state_;
  friend class TaskRuntime;
  friend class detail::RuntimeControl;
};

struct RuntimeConfig final {
  std::size_t worker_count{1};
  ResourceBudget budget;
  std::filesystem::path history_file;
  std::chrono::milliseconds starvation_threshold{250};
  std::chrono::milliseconds monitor_interval{10};
};

class ITaskService {
public:
  virtual ~ITaskService() noexcept = default;
  [[nodiscard]] virtual core::Result<TaskHandle> submit(const TaskSpec& spec) noexcept = 0;
  [[nodiscard]] virtual core::Result<std::vector<TaskHandle>> submit_batch(std::vector<TaskDefinition> definitions) = 0;
  /// 重建目标及其未成功依赖链，并为每个新尝试分配新的 TaskId。
  [[nodiscard]] virtual core::Result<TaskHandle> retry(const TaskId& task_id) = 0;
  [[nodiscard]] virtual core::Status pause(const TaskId& task_id) = 0;
  [[nodiscard]] virtual core::Status resume(const TaskId& task_id) = 0;
  [[nodiscard]] virtual core::Status cancel(const TaskId& task_id) = 0;
  [[nodiscard]] virtual core::Result<TaskStatus> status(const TaskId& task_id) const = 0;
  [[nodiscard]] virtual std::vector<TaskStatus> history() const = 0;
};

class TaskRuntime final : public ITaskService {
public:
  explicit TaskRuntime(RuntimeConfig config);
  ~TaskRuntime() override;
  TaskRuntime(const TaskRuntime&) = delete;
  TaskRuntime& operator=(const TaskRuntime&) = delete;
  TaskRuntime(TaskRuntime&&) noexcept;
  TaskRuntime& operator=(TaskRuntime&&) noexcept;

  [[nodiscard]] core::Result<TaskHandle> submit(const TaskSpec& spec) noexcept override;
  [[nodiscard]] core::Result<TaskHandle> submit(TaskSpec spec, TaskWork work);
  [[nodiscard]] core::Status register_handler(std::string task_type, TaskWork work);
  [[nodiscard]] core::Status remove_handler(std::string_view task_type);
  [[nodiscard]] core::Result<std::vector<TaskHandle>> submit_batch(std::vector<TaskDefinition> definitions) override;
  [[nodiscard]] core::Result<TaskHandle> retry(const TaskId& task_id) override;
  [[nodiscard]] core::Status pause(const TaskId& task_id) override;
  [[nodiscard]] core::Status resume(const TaskId& task_id) override;
  [[nodiscard]] core::Status cancel(const TaskId& task_id) override;
  [[nodiscard]] core::Result<TaskStatus> status(const TaskId& task_id) const override;
  [[nodiscard]] std::vector<TaskStatus> history() const override;
  [[nodiscard]] std::vector<TaskStatus> find_by_provenance(const ProjectId& project_id,
                                                           const DataSourceVersionId& data_source_version_id,
                                                           const SourceObject& source_object) const;
  [[nodiscard]] std::uint64_t add_observer(std::shared_ptr<ITaskObserver> observer);
  void remove_observer(std::uint64_t subscription_id);
  [[nodiscard]] ViewRequestId issue_view_request(std::string scope);
  [[nodiscard]] std::optional<ViewCommitPermit> try_begin_view_commit(const ViewRequestId& request) const;
  [[nodiscard]] bool history_healthy() const noexcept;
  void shutdown() noexcept;

private:
  std::shared_ptr<detail::RuntimeControl> control_;
};

[[nodiscard]] bool is_terminal(TaskState state) noexcept;
[[nodiscard]] std::string_view to_string(TaskState state) noexcept;
[[nodiscard]] std::string_view to_string(TaskPriority priority) noexcept;

} // namespace signal::task
