#include "signal_studio/task_runtime/task_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using signal::task::DataSourceVersionId;
using signal::task::FailureInfo;
using signal::task::ProjectId;
using signal::task::ResourceBudget;
using signal::task::RuntimeConfig;
using signal::task::SourceObject;
using signal::task::TaskContext;
using signal::task::TaskDefinition;
using signal::task::TaskExecutionResult;
using signal::task::TaskHandle;
using signal::task::TaskId;
using signal::task::TaskPriority;
using signal::task::TaskRuntime;
using signal::task::TaskSpec;
using signal::task::TaskState;
using signal::task::TaskStatus;
using signal::task::WorkClass;

void require(bool condition, std::string message) {
  if (!condition) {
    throw std::runtime_error(std::move(message));
  }
}

template <typename Predicate>
void wait_until(Predicate predicate, std::chrono::milliseconds timeout, std::string message) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return;
    }
    std::this_thread::sleep_for(1ms);
  }
  require(predicate(), std::move(message));
}

TaskSpec make_spec(std::string key, TaskPriority priority = TaskPriority::foreground) {
  TaskSpec spec;
  spec.task_type = "test." + key;
  spec.priority = priority;
  spec.idempotency_key = std::move(key);
  spec.provenance = {ProjectId{"project-a"}, DataSourceVersionId{"data-v1"}, SourceObject{"selection", "object-a"}};
  spec.max_attempts = 3U;
  return spec;
}

RuntimeConfig make_config(std::size_t workers = 1U) {
  RuntimeConfig config;
  config.worker_count = workers;
  config.budget = ResourceBudget{static_cast<std::uint32_t>(workers), static_cast<std::uint32_t>(workers),
                                 static_cast<std::uint32_t>(workers), static_cast<std::uint32_t>(workers)};
  config.starvation_threshold = 20ms;
  config.monitor_interval = 2ms;
  return config;
}

TaskStatus require_status(const TaskHandle& handle) {
  auto result = handle.status();
  require(result.ok(), "无法读取任务状态");
  return result.value();
}

void wait_state(const TaskHandle& handle, TaskState state, std::chrono::milliseconds timeout = 1s) {
  wait_until([&] { return require_status(handle).state == state; }, timeout,
             "等待任务状态超时: " + std::string{signal::task::to_string(state)});
}

TaskExecutionResult cooperative_wait(TaskContext& context, std::atomic_bool& release) {
  while (!release.load()) {
    if (!context.checkpoint()) {
      return TaskExecutionResult::completed();
    }
    std::this_thread::sleep_for(1ms);
  }
  return TaskExecutionResult::completed();
}

void test_fr_tsk_001_schedule() {
  for (const auto work_class :
       {WorkClass::io, WorkClass::dsp, WorkClass::indexing, WorkClass::exporting, WorkClass::inference}) {
    require(signal::task::evaluate_scheduling(work_class, 51ms).must_schedule, "超过 50 ms 的规定工作必须调度");
    require(!signal::task::evaluate_scheduling(work_class, 50ms).must_schedule, "50 ms 边界不应误判");
  }
  require(!signal::task::evaluate_scheduling(WorkClass::other, 1s).must_schedule, "未分类工作不应被该契约强制调度");
}

void test_fr_tsk_002_states() {
  const std::vector expected{TaskState::queued,    TaskState::running,           TaskState::paused,
                             TaskState::canceling, TaskState::canceled,          TaskState::completed,
                             TaskState::failed,    TaskState::dependency_failed, TaskState::stale};
  for (const auto state : expected) {
    require(signal::task::to_string(state) != "unknown", "任务状态缺少稳定文本");
  }

  TaskRuntime runtime{make_config()};
  std::atomic_bool release{};
  auto submitted = runtime.submit(make_spec("state-pause"), [&](TaskContext& context) {
    while (!release.load()) {
      if (!context.report_progress(0.25, "四分之一")) {
        break;
      }
      if (!context.checkpoint()) {
        break;
      }
      std::this_thread::sleep_for(1ms);
    }
    return TaskExecutionResult::completed();
  });
  require(submitted.ok(), "暂停测试提交失败");
  auto handle = submitted.value();
  wait_state(handle, TaskState::running);
  require(handle.pause().ok(), "暂停请求失败");
  wait_state(handle, TaskState::paused);
  require(std::abs(require_status(handle).progress - 0.25) < 1e-9, "进度未保存");
  require(handle.resume().ok(), "恢复请求失败");
  wait_state(handle, TaskState::running);
  release.store(true);
  auto completed = handle.wait();
  require(completed.ok() && completed.value().state == TaskState::completed, "暂停恢复任务未完成");
}

void test_fr_tsk_003_priority() {
  TaskRuntime runtime{make_config()};
  std::atomic_bool release_blocker{};
  auto blocker = runtime.submit(make_spec("priority-blocker"),
                                [&](TaskContext& context) { return cooperative_wait(context, release_blocker); });
  require(blocker.ok(), "阻塞任务提交失败");
  wait_state(blocker.value(), TaskState::running);

  std::mutex order_mutex;
  std::vector<TaskPriority> order;
  const auto enqueue = [&](std::string key, TaskPriority priority) {
    return runtime.submit(make_spec(std::move(key), priority), [&, priority](TaskContext&) {
      std::lock_guard lock(order_mutex);
      order.push_back(priority);
      return TaskExecutionResult::completed();
    });
  };
  auto background = enqueue("priority-background", TaskPriority::background);
  auto foreground = enqueue("priority-foreground", TaskPriority::foreground);
  auto interactive = enqueue("priority-interactive", TaskPriority::interactive);
  require(background && foreground && interactive, "优先级任务提交失败");
  release_blocker.store(true);
  require(interactive.value().wait().ok() && foreground.value().wait().ok() && background.value().wait().ok(),
          "优先级任务未完成");
  require(order == std::vector{TaskPriority::interactive, TaskPriority::foreground, TaskPriority::background},
          "交互、前台、后台优先级顺序错误");

  std::atomic_bool second_release{};
  auto second_blocker = runtime.submit(make_spec("starvation-blocker"),
                                       [&](TaskContext& context) { return cooperative_wait(context, second_release); });
  wait_state(second_blocker.value(), TaskState::running);
  std::vector<TaskPriority> fairness_order;
  auto aged = runtime.submit(make_spec("aged-background", TaskPriority::background), [&](TaskContext&) {
    std::lock_guard lock(order_mutex);
    fairness_order.push_back(TaskPriority::background);
    return TaskExecutionResult::completed();
  });
  std::this_thread::sleep_for(70ms);
  auto fresh = runtime.submit(make_spec("fresh-interactive", TaskPriority::interactive), [&](TaskContext&) {
    std::lock_guard lock(order_mutex);
    fairness_order.push_back(TaskPriority::interactive);
    return TaskExecutionResult::completed();
  });
  second_release.store(true);
  require(aged.value().wait().ok() && fresh.value().wait().ok(), "饥饿控制任务未完成");
  require(fairness_order.front() == TaskPriority::background, "后台任务发生饥饿");
}

void test_fr_tsk_004_stale() {
  TaskRuntime runtime{make_config()};
  const auto first = runtime.issue_view_request("spectrum");
  auto spec = make_spec("stale-old");
  spec.view_request = first;
  std::atomic_bool release{};
  auto old = runtime.submit(std::move(spec), [&](TaskContext& context) { return cooperative_wait(context, release); });
  require(old.ok(), "旧视图任务提交失败");
  wait_state(old.value(), TaskState::running);
  const auto current = runtime.issue_view_request("spectrum");
  wait_state(old.value(), TaskState::stale);
  require(!runtime.try_begin_view_commit(first).has_value(), "旧视图仍获得提交许可");
  auto permit = runtime.try_begin_view_commit(current);
  require(permit.has_value(), "当前视图未获得提交许可");
  permit.reset();
  release.store(true);
}

void test_fr_tsk_005_cancel() {
  constexpr std::size_t task_count = 20U;
  TaskRuntime runtime{make_config(task_count)};
  std::vector<TaskHandle> handles;
  handles.reserve(task_count);
  for (std::size_t index = 0; index < task_count; ++index) {
    auto submitted = runtime.submit(make_spec("cancel-" + std::to_string(index)), [](TaskContext& context) {
      while (context.checkpoint()) {
        std::this_thread::sleep_for(1ms);
      }
      return TaskExecutionResult::completed();
    });
    require(submitted.ok(), "取消时延任务提交失败");
    handles.push_back(submitted.value());
  }
  for (const auto& handle : handles) {
    wait_state(handle, TaskState::running, 2s);
  }
  std::vector<double> acknowledgements;
  for (const auto& handle : handles) {
    const auto begin = std::chrono::steady_clock::now();
    require(handle.cancel().ok(), "取消请求失败");
    const auto state = require_status(handle).state;
    require(state == TaskState::canceling || state == TaskState::canceled, "取消未立即进入确认状态");
    acknowledgements.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
  }
  std::sort(acknowledgements.begin(), acknowledgements.end());
  const auto p95_index = static_cast<std::size_t>(std::ceil(static_cast<double>(acknowledgements.size()) * 0.95) - 1.0);
  require(acknowledgements[p95_index] <= 200.0, "取消确认 P95 超过 200 ms");
  const auto stop_begin = std::chrono::steady_clock::now();
  for (const auto& handle : handles) {
    const auto status = handle.wait();
    require(status.ok() && status.value().state == TaskState::canceled, "取消任务未停止");
  }
  require(std::chrono::steady_clock::now() - stop_begin <= 2s, "可中断计算未在 2 s 内停止");
}

void test_fr_tsk_006_failure() {
  TaskRuntime runtime{make_config()};
  FailureInfo failure{"TASK.INPUT_INVALID", "输入无效", "头部字段长度不匹配", "重新选择数据文件", true, "retry",
                      "logs://task/input"};
  auto submitted = runtime.submit(make_spec("structured-failure"),
                                  [failure](TaskContext&) { return TaskExecutionResult::failed(failure); });
  auto result = submitted.value().wait();
  require(result.ok() && result.value().state == TaskState::failed && result.value().failure, "结构化失败未保存");
  const auto& actual = *result.value().failure;
  require(!actual.error_code.empty() && !actual.user_message.empty() && !actual.technical_details.empty() &&
              !actual.suggested_action.empty() && actual.retryable && !actual.retry_action.empty() &&
              !actual.log_link.empty(),
          "失败信息字段不完整");

  std::vector<FailureInfo> invalid_failures;
  auto invalid = failure;
  invalid.error_code.clear();
  invalid_failures.push_back(invalid);
  invalid = failure;
  invalid.user_message.clear();
  invalid_failures.push_back(invalid);
  invalid = failure;
  invalid.technical_details.clear();
  invalid_failures.push_back(invalid);
  invalid = failure;
  invalid.suggested_action.clear();
  invalid_failures.push_back(invalid);
  invalid = failure;
  invalid.log_link.clear();
  invalid_failures.push_back(invalid);
  invalid = failure;
  invalid.retry_action.clear();
  invalid_failures.push_back(invalid);
  for (const auto& invalid_failure : invalid_failures) {
    require(!signal::task::validate_failure_info(invalid_failure).ok(), "不完整失败信息通过了契约校验");
  }
  auto invalid_result =
      runtime.submit(make_spec("invalid-failure"), [invalid = invalid_failures.front()](TaskContext&) {
        return TaskExecutionResult{false, invalid};
      });
  const auto normalized = invalid_result.value().wait();
  require(normalized.ok() && normalized.value().failure &&
              normalized.value().failure->error_code == "TASK.INVALID_FAILURE_INFO" &&
              signal::task::validate_failure_info(*normalized.value().failure).ok(),
          "无效失败信息未转换为明确的结构化内部错误");
}

void test_fr_tsk_007_resources() {
  auto config = make_config(2U);
  config.budget = ResourceBudget{2U, 1U, 1U, 2U};
  TaskRuntime runtime{config};
  std::atomic_int active{};
  std::atomic_int maximum{};
  std::atomic_bool release{};
  auto work = [&](TaskContext& context) {
    const auto count = active.fetch_add(1) + 1;
    maximum.store(std::max(maximum.load(), count));
    cooperative_wait(context, release);
    active.fetch_sub(1);
    return TaskExecutionResult::completed();
  };
  auto first_spec = make_spec("resource-first");
  first_spec.resources = {2U, 1U, 1U, 1U};
  auto second_spec = make_spec("resource-second");
  second_spec.resources = first_spec.resources;
  auto first = runtime.submit(std::move(first_spec), work);
  auto second = runtime.submit(std::move(second_spec), work);
  require(first && second, "资源预算内任务提交失败");
  wait_state(first.value(), TaskState::running);
  std::this_thread::sleep_for(20ms);
  require(require_status(second.value()).state == TaskState::queued, "资源预算未限制并发");
  release.store(true);
  require(first.value().wait().ok() && second.value().wait().ok(), "资源任务未完成");
  require(maximum.load() == 1, "资源预算发生超额准入");
  auto excessive = make_spec("resource-excessive");
  excessive.resources = {3U, 0U, 0U, 1U};
  require(!runtime.submit(std::move(excessive), [](TaskContext&) { return TaskExecutionResult::completed(); }),
          "超过预算的任务被准入");
}

void test_fr_tsk_008_dag() {
  TaskRuntime runtime{make_config()};
  auto cyclic_a = make_spec("cycle-a");
  auto cyclic_b = make_spec("cycle-b");
  cyclic_a.task_id = TaskId{"cycle-a-id"};
  cyclic_b.task_id = TaskId{"cycle-b-id"};
  cyclic_a.dependencies = {cyclic_b.task_id};
  cyclic_b.dependencies = {cyclic_a.task_id};
  std::vector<TaskDefinition> cycle;
  cycle.push_back({cyclic_a, [](TaskContext&) { return TaskExecutionResult::completed(); }});
  cycle.push_back({cyclic_b, [](TaskContext&) { return TaskExecutionResult::completed(); }});
  require(!runtime.submit_batch(std::move(cycle)), "DAG 环未被拒绝");

  std::mutex order_mutex;
  std::vector<std::string> order;
  auto parent_spec = make_spec("dag-parent");
  auto child_spec = make_spec("dag-child");
  parent_spec.task_id = TaskId{"dag-parent-id"};
  child_spec.task_id = TaskId{"dag-child-id"};
  child_spec.dependencies = {parent_spec.task_id};
  std::vector<TaskDefinition> valid;
  valid.push_back({child_spec, [&](TaskContext&) {
                     std::lock_guard lock(order_mutex);
                     order.push_back("child");
                     return TaskExecutionResult::completed();
                   }});
  valid.push_back({parent_spec, [&](TaskContext&) {
                     std::lock_guard lock(order_mutex);
                     order.push_back("parent");
                     return TaskExecutionResult::completed();
                   }});
  auto submitted = runtime.submit_batch(std::move(valid));
  require(submitted.ok(), "有效 DAG 提交失败");
  for (const auto& handle : submitted.value()) {
    require(handle.wait().ok(), "DAG 任务未完成");
  }
  require(order == std::vector<std::string>{"parent", "child"}, "DAG 依赖完成顺序错误");

  auto failed_parent = make_spec("failed-parent");
  failed_parent.task_id = TaskId{"failed-parent-id"};
  auto failed_child = make_spec("failed-child");
  failed_child.task_id = TaskId{"failed-child-id"};
  failed_child.dependencies = {failed_parent.task_id};
  std::vector<TaskDefinition> failing;
  failing.push_back({failed_child, [](TaskContext&) { return TaskExecutionResult::completed(); }});
  failing.push_back({failed_parent, [](TaskContext&) {
                       return TaskExecutionResult::failed(
                           {"TASK.TEST_FAILURE", "测试失败", "依赖传播", "重试", true, "retry", "logs://dag"});
                     }});
  auto failure_result = runtime.submit_batch(std::move(failing));
  require(failure_result.ok(), "失败 DAG 提交失败");
  for (const auto& handle : failure_result.value()) {
    const auto status = handle.wait();
    require(status.ok(), "失败 DAG 状态不可读");
  }
  require(runtime.status(TaskId{"failed-child-id"}).value().state == TaskState::dependency_failed, "依赖失败未传播");
  auto failed_retry = runtime.retry(TaskId{"failed-child-id"});
  require(failed_retry.ok() && require_status(failed_retry.value()).state == TaskState::dependency_failed,
          "失败依赖的重试任务永久停留在排队状态");

  std::atomic_bool release{};
  auto cancel_parent = make_spec("cancel-parent");
  cancel_parent.task_id = TaskId{"cancel-parent-id"};
  auto cancel_child = make_spec("cancel-child");
  cancel_child.task_id = TaskId{"cancel-child-id"};
  cancel_child.dependencies = {cancel_parent.task_id};
  std::vector<TaskDefinition> canceling;
  canceling.push_back({cancel_child, [](TaskContext&) { return TaskExecutionResult::completed(); }});
  canceling.push_back({cancel_parent, [&](TaskContext& context) { return cooperative_wait(context, release); }});
  auto cancel_result = runtime.submit_batch(std::move(canceling));
  require(cancel_result.ok(), "取消 DAG 提交失败");
  wait_until([&] { return runtime.status(TaskId{"cancel-parent-id"}).value().state == TaskState::running; }, 1s,
             "取消父任务未运行");
  require(runtime.cancel(TaskId{"cancel-parent-id"}).ok(), "取消父任务失败");
  require(runtime.status(TaskId{"cancel-child-id"}).value().state == TaskState::dependency_failed,
          "取消未向依赖任务传播");
  auto canceled_retry = runtime.retry(TaskId{"cancel-child-id"});
  require(canceled_retry.ok() && require_status(canceled_retry.value()).state == TaskState::dependency_failed,
          "取消依赖的重试任务永久停留在排队状态");
  release.store(true);

  const auto stale_request = runtime.issue_view_request("dag-stale");
  auto stale_parent = make_spec("stale-parent");
  stale_parent.task_id = TaskId{"stale-parent-id"};
  stale_parent.view_request = stale_request;
  auto stale_child = make_spec("stale-child");
  stale_child.task_id = TaskId{"stale-child-id"};
  stale_child.dependencies = {stale_parent.task_id};
  std::atomic_bool stale_release{};
  std::vector<TaskDefinition> stale_definitions;
  stale_definitions.push_back({stale_child, [](TaskContext&) { return TaskExecutionResult::completed(); }});
  stale_definitions.push_back(
      {stale_parent, [&](TaskContext& context) { return cooperative_wait(context, stale_release); }});
  auto stale_tasks = runtime.submit_batch(std::move(stale_definitions));
  require(stale_tasks.ok(), "过期 DAG 提交失败");
  wait_until([&] { return runtime.status(TaskId{"stale-parent-id"}).value().state == TaskState::running; }, 1s,
             "过期父任务未运行");
  static_cast<void>(runtime.issue_view_request("dag-stale"));
  auto stale_retry = runtime.retry(TaskId{"stale-child-id"});
  require(stale_retry.ok() && require_status(stale_retry.value()).state == TaskState::dependency_failed,
          "过期依赖的重试任务永久停留在排队状态");
  stale_release.store(true);
}

void test_fr_tsk_009_idempotency() {
  TaskRuntime runtime{make_config()};
  std::atomic_int executions{};
  auto spec = make_spec("idempotent-action");
  spec.max_attempts = 2U;
  const auto work = [&](TaskContext& context) {
    executions.fetch_add(1);
    require(context.attempt() >= 1U, "尝试次数未传给工作体");
    return TaskExecutionResult::failed(
        {"TASK.RETRYABLE", "可重试失败", "测试重试", "重试", true, "retry", "logs://retry"});
  };
  auto first = runtime.submit(spec, work);
  auto duplicate = runtime.submit(spec, work);
  require(first && duplicate && first.value().id() == duplicate.value().id(), "幂等提交未去重");
  require(first.value().wait().value().state == TaskState::failed && executions.load() == 1,
          "初次幂等任务执行次数错误");
  auto retry = runtime.retry(first.value().id());
  auto duplicate_retry = runtime.retry(first.value().id());
  require(retry && duplicate_retry && retry.value().id() == duplicate_retry.value().id(), "重试尝试未去重");
  const auto retry_status = retry.value().wait();
  require(retry_status.ok() && retry_status.value().attempt == 2U && executions.load() == 2, "重试尝试语义错误");
}

void test_fr_tsk_010_provenance() {
  TaskRuntime runtime{make_config()};
  auto wanted = make_spec("provenance-wanted");
  wanted.provenance = {ProjectId{"project-wanted"}, DataSourceVersionId{"version-wanted"},
                       SourceObject{"region", "region-wanted"}};
  auto other = make_spec("provenance-other");
  require(runtime.submit(wanted, [](TaskContext&) { return TaskExecutionResult::completed(); }).ok() &&
              runtime.submit(other, [](TaskContext&) { return TaskExecutionResult::completed(); }).ok(),
          "来源链任务提交失败");
  wait_until([&] { return runtime.history().size() == 2U; }, 1s, "任务历史数量错误");
  const auto matches = runtime.find_by_provenance(ProjectId{"project-wanted"}, DataSourceVersionId{"version-wanted"},
                                                  SourceObject{"region", "region-wanted"});
  require(matches.size() == 1U && matches.front().task_type == wanted.task_type, "来源链过滤不准确");
  auto invalid = make_spec("provenance-invalid");
  invalid.provenance.project_id.value.clear();
  require(!runtime.submit(std::move(invalid), [](TaskContext&) { return TaskExecutionResult::completed(); }),
          "缺少 ProjectId 的任务被接受");
}

class ReentrantObserver final : public signal::task::ITaskObserver {
public:
  TaskRuntime* runtime{};
  std::atomic_uint64_t count{};

  void on_event(const signal::task::TaskEvent& event) noexcept override {
    if (runtime != nullptr) {
      static_cast<void>(runtime->status(event.status.task_id));
    }
    count.fetch_add(1U);
  }
};

void test_fr_task_101_lifecycle() {
  static_assert(std::is_base_of_v<signal::task::ITaskService, TaskRuntime>);
  static_assert(noexcept(std::declval<signal::task::ITaskService&>().submit(std::declval<const TaskSpec&>())));
  {
    TaskRuntime runtime{make_config()};
    require(
        runtime.register_handler("test.public-service", [](TaskContext&) { return TaskExecutionResult::completed(); })
            .ok(),
        "公共服务处理器注册失败");
    auto spec = make_spec("public-service");
    spec.task_type = "test.public-service";
    signal::task::ITaskService& service = runtime;
    auto submitted = service.submit(spec);
    require(submitted.ok() && submitted.value().wait().value().state == TaskState::completed,
            "ITaskService 稳定提交入口不可用");
    require(runtime.remove_handler("test.public-service").ok(), "公共服务处理器移除失败");
  }
  const auto base = std::filesystem::temp_directory_path() / ("signal-task-runtime-" + TaskId::generate().value);
  const auto journal = base / "history.journal";
  const auto crash_journal = base / "crash.journal";
  const auto temporary_recovery_journal = base / "temporary-recovery.journal";
  const auto temporary_artifact = base / "artifact.partial";
  const auto final_artifact = base / "artifact.bin";
  std::filesystem::create_directories(base);

  {
    auto config = make_config();
    config.history_file = journal;
    TaskRuntime runtime{config};
    auto observer = std::make_shared<ReentrantObserver>();
    observer->runtime = &runtime;
    const auto subscription = runtime.add_observer(observer);
    require(subscription != 0U, "观察者注册失败");

    std::atomic_bool artifact_committed{};
    std::atomic_bool release{};
    auto spec = make_spec("crash-artifact");
    auto submitted = runtime.submit(std::move(spec), [&](TaskContext& context) {
      {
        std::ofstream stream(temporary_artifact, std::ios::binary);
        stream << "abc";
      }
      const auto committed = context.commit_artifact(temporary_artifact, final_artifact);
      require(committed.ok(), "原子制品提交失败");
      artifact_committed.store(true);
      return cooperative_wait(context, release);
    });
    require(submitted.ok(), "崩溃恢复任务提交失败");
    wait_until([&] { return artifact_committed.load(); }, 1s, "制品未提交");
    wait_until([&] { return observer->count.load() >= 2U; }, 1s, "观察者未收到事件");
    std::filesystem::copy_file(journal, crash_journal, std::filesystem::copy_options::overwrite_existing);
    release.store(true);
    const auto completed = submitted.value().wait();
    require(completed.value().state == TaskState::completed && completed.value().committed_artifacts.size() == 1U &&
                completed.value().committed_artifacts.front().size_bytes == 3U &&
                completed.value().committed_artifacts.front().sha256_digest ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "原任务制品元数据错误");
    runtime.remove_observer(subscription);
  }

  {
    auto config = make_config();
    config.history_file = journal;
    TaskRuntime recovered{config};
    const auto history = recovered.history();
    require(history.size() == 1U && history.front().state == TaskState::completed, "完成任务历史未恢复");
  }

  {
    const auto temporary = std::filesystem::path{temporary_recovery_journal.string() + ".tmp"};
    std::filesystem::copy_file(journal, temporary, std::filesystem::copy_options::overwrite_existing);
    auto config = make_config();
    config.history_file = temporary_recovery_journal;
    TaskRuntime recovered{config};
    require(recovered.history_healthy() && recovered.history().size() == 1U &&
                recovered.history().front().state == TaskState::completed &&
                std::filesystem::exists(temporary_recovery_journal) && !std::filesystem::exists(temporary),
            "主历史缺失时未从唯一完整临时副本恢复");
  }
  {
    const auto temporary = std::filesystem::path{temporary_recovery_journal.string() + ".tmp"};
    {
      std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      stream << "stale temporary copy";
    }
    auto config = make_config();
    config.history_file = temporary_recovery_journal;
    TaskRuntime recovered{config};
    require(recovered.history_healthy() && recovered.history().size() == 1U && !std::filesystem::exists(temporary),
            "主历史存在时未清理临时副本");
  }

  {
    std::ofstream stream(final_artifact, std::ios::binary | std::ios::trunc);
    stream << "abd";
  }
  {
    auto config = make_config();
    config.history_file = journal;
    TaskRuntime recovered{config};
    const auto history = recovered.history();
    require(history.size() == 1U && history.front().state == TaskState::failed && history.front().failure &&
                history.front().failure->error_code == "TASK.ARTIFACT_INVALID" &&
                history.front().committed_artifacts.empty(),
            "内容损坏制品在恢复时仍被发布");
  }

  {
    auto config = make_config();
    config.history_file = crash_journal;
    TaskRuntime recovered{config};
    const auto history = recovered.history();
    require(history.size() == 1U && history.front().state == TaskState::failed && history.front().failure &&
                history.front().failure->error_code == "TASK.CRASH_RECOVERED" &&
                history.front().committed_artifacts.empty(),
            "崩溃恢复错误地发布了未完成结果");
  }

  {
    std::ofstream stream(crash_journal, std::ios::binary | std::ios::app);
    stream << "corrupted-history-record\n";
  }
  {
    auto config = make_config();
    config.history_file = crash_journal;
    TaskRuntime corrupted{config};
    require(!corrupted.history_healthy(), "历史损坏未被检测");
    require(!corrupted.history().empty(), "损坏前的有效历史未恢复");
  }

  {
    TaskRuntime runtime{make_config()};
    auto spec = make_spec("timeout");
    spec.timeout = 30ms;
    auto timed = runtime.submit(std::move(spec), [](TaskContext& context) {
      while (context.checkpoint()) {
        std::this_thread::sleep_for(1ms);
      }
      return TaskExecutionResult::completed();
    });
    const auto status = timed.value().wait();
    require(status.ok() && status.value().state == TaskState::failed && status.value().failure &&
                status.value().failure->error_code == "TASK.TIMEOUT",
            "超时任务状态错误");
  }

  const auto shutdown_begin = std::chrono::steady_clock::now();
  {
    TaskRuntime runtime{make_config(2U)};
    for (int index = 0; index < 2; ++index) {
      auto handle = runtime.submit(make_spec("shutdown-" + std::to_string(index)), [](TaskContext& context) {
        while (context.checkpoint()) {
          std::this_thread::sleep_for(1ms);
        }
        return TaskExecutionResult::completed();
      });
      require(handle.ok(), "关闭测试任务提交失败");
    }
  }
  require(std::chrono::steady_clock::now() - shutdown_begin < 2s, "协作式关闭不确定");

  std::error_code error;
  std::filesystem::remove_all(base, error);
}

using Test = std::function<void()>;

const std::map<std::string, Test> tests{
    {"FR-TSK-001", test_fr_tsk_001_schedule},    {"FR-TSK-002", test_fr_tsk_002_states},
    {"FR-TSK-003", test_fr_tsk_003_priority},    {"FR-TSK-004", test_fr_tsk_004_stale},
    {"FR-TSK-005", test_fr_tsk_005_cancel},      {"FR-TSK-006", test_fr_tsk_006_failure},
    {"FR-TSK-007", test_fr_tsk_007_resources},   {"FR-TSK-008", test_fr_tsk_008_dag},
    {"FR-TSK-009", test_fr_tsk_009_idempotency}, {"FR-TSK-010", test_fr_tsk_010_provenance},
    {"FR-TASK-101", test_fr_task_101_lifecycle},
};

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view{argv[1]} == "--case") {
      const auto iterator = tests.find(argv[2]);
      require(iterator != tests.end(), "未知测试用例");
      iterator->second();
      std::cout << "PASS " << iterator->first << '\n';
      return 0;
    }
    for (const auto& [name, test] : tests) {
      test();
      std::cout << "PASS " << name << '\n';
    }
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "FAIL " << exception.what() << '\n';
    return 1;
  }
}
