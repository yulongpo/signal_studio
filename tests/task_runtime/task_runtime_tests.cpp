#include "signal_studio/task_runtime/task_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
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

std::string journal_checksum(std::string_view payload) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto character : payload) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 1099511628211ULL;
  }
  std::ostringstream output;
  output << std::hex << std::setw(16) << std::setfill('0') << hash;
  return output.str();
}

void write_unknown_state_journal(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::ifstream input{source, std::ios::binary};
  std::string line;
  std::getline(input, line);
  require(static_cast<bool>(input) || input.eof(), "无法读取任务历史夹具");
  const auto checksum_separator = line.rfind('\t');
  require(checksum_separator != std::string::npos, "任务历史夹具缺少校验和");
  auto payload = line.substr(0U, checksum_separator);
  std::size_t field_begin = 0U;
  for (int field = 0; field < 4; ++field) {
    field_begin = payload.find('\t', field_begin);
    require(field_begin != std::string::npos, "任务历史夹具字段不足");
    ++field_begin;
  }
  const auto field_end = payload.find('\t', field_begin);
  require(field_end != std::string::npos, "任务历史夹具状态字段缺失");
  payload.replace(field_begin, field_end - field_begin, "255");
  std::ofstream output{destination, std::ios::binary | std::ios::trunc};
  output << payload << '\t' << journal_checksum(payload) << '\n';
  require(static_cast<bool>(output), "无法写入未知状态任务历史夹具");
}

void write_sstj2_journal(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::ifstream input{source, std::ios::binary};
  std::ofstream output{destination, std::ios::binary | std::ios::trunc};
  std::string line;
  while (std::getline(input, line)) {
    const auto checksum_separator = line.rfind('\t');
    require(checksum_separator != std::string::npos, "SSTJ3 历史夹具缺少校验和");
    auto payload = line.substr(0U, checksum_separator);
    const auto timeout_separator = payload.rfind('\t');
    require(timeout_separator != std::string::npos, "SSTJ3 历史夹具缺少 timeout");
    payload.erase(timeout_separator);
    const auto dependencies_separator = payload.rfind('\t');
    require(dependencies_separator != std::string::npos, "SSTJ3 历史夹具缺少 dependencies");
    payload.erase(dependencies_separator);
    require(payload.starts_with("SSTJ3"), "历史夹具不是 SSTJ3");
    payload.replace(0U, 5U, "SSTJ2");
    output << payload << '\t' << journal_checksum(payload) << '\n';
  }
  require((input.eof() || static_cast<bool>(input)) && static_cast<bool>(output), "无法生成 SSTJ2 兼容夹具");
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
    const auto scheduled = signal::task::evaluate_scheduling(work_class, 51ms);
    const auto boundary = signal::task::evaluate_scheduling(work_class, 50ms);
    require(scheduled && scheduled.value().must_schedule, "超过 50 ms 的规定工作必须调度");
    require(boundary && !boundary.value().must_schedule, "50 ms 边界不应误判");
  }
  const auto other = signal::task::evaluate_scheduling(WorkClass::other, 1s);
  require(other && !other.value().must_schedule, "未分类工作不应被该契约强制调度");
  require(!signal::task::evaluate_scheduling(static_cast<WorkClass>(255U), 1ms) &&
              !signal::task::validate_work_class(static_cast<WorkClass>(255U)),
          "未知 WorkClass 必须被公共调度入口拒绝");
}

void test_fr_tsk_002_states() {
  const std::vector expected{TaskState::queued,    TaskState::running,           TaskState::paused,
                             TaskState::canceling, TaskState::canceled,          TaskState::completed,
                             TaskState::failed,    TaskState::dependency_failed, TaskState::stale};
  for (const auto state : expected) {
    require(signal::task::validate_task_state(state).ok() && signal::task::to_string(state) != "unknown",
            "任务状态缺少稳定文本");
  }
  require(!signal::task::validate_task_state(static_cast<TaskState>(255U)) &&
              !signal::task::validate_task_priority(static_cast<TaskPriority>(255U)),
          "未知任务状态或优先级必须被拒绝");

  TaskRuntime runtime{make_config()};
  std::atomic_bool release{};
  std::atomic_bool rejected_nonfinite{};
  auto submitted = runtime.submit(make_spec("state-pause"), [&](TaskContext& context) {
    rejected_nonfinite.store(!context.report_progress(std::numeric_limits<double>::quiet_NaN(), "NaN") &&
                             !context.report_progress(std::numeric_limits<double>::infinity(), "+Inf") &&
                             !context.report_progress(-std::numeric_limits<double>::infinity(), "-Inf"));
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
  require(rejected_nonfinite.load() && std::isfinite(require_status(handle).progress) &&
              std::abs(require_status(handle).progress - 0.25) < 1e-9,
          "非有限进度未被拒绝或有限进度未保存");
  require(handle.resume().ok(), "恢复请求失败");
  wait_state(handle, TaskState::running);
  release.store(true);
  auto completed = handle.wait();
  require(completed.ok() && completed.value().state == TaskState::completed, "暂停恢复任务未完成");
  auto invalid_priority = make_spec("invalid-priority", static_cast<TaskPriority>(255U));
  require(!runtime.submit(std::move(invalid_priority), [](TaskContext&) { return TaskExecutionResult::completed(); }),
          "未知 TaskPriority 被任务提交入口接受");
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

  const auto artifact_root = std::filesystem::temp_directory_path() / ("signal-task-view-" + TaskId::generate().value);
  std::filesystem::create_directories(artifact_root);
  const auto stale_temporary = artifact_root / "stale.partial";
  const auto stale_final = artifact_root / "stale.bin";
  {
    std::ofstream stream{stale_temporary, std::ios::binary};
    stream << "stale";
  }
  const auto stale_view = runtime.issue_view_request("artifact-stale");
  auto stale_spec = make_spec("artifact-stale");
  stale_spec.view_request = stale_view;
  std::atomic_bool stale_entered{};
  std::atomic_bool allow_stale_commit{};
  std::atomic_bool stale_commit_finished{};
  std::atomic_bool stale_commit_succeeded{};
  auto stale_artifact = runtime.submit(std::move(stale_spec), [&](TaskContext& context) {
    stale_entered.store(true);
    while (!allow_stale_commit.load()) {
      std::this_thread::yield();
    }
    stale_commit_succeeded.store(context.commit_artifact(stale_temporary, stale_final).ok());
    stale_commit_finished.store(true);
    return TaskExecutionResult::completed();
  });
  wait_until([&] { return stale_entered.load(); }, 1s, "旧视图制品任务未进入提交边界");
  static_cast<void>(runtime.issue_view_request("artifact-stale"));
  allow_stale_commit.store(true);
  wait_until([&] { return stale_commit_finished.load(); }, 1s, "旧视图制品提交未返回");
  require(!stale_commit_succeeded.load() && !std::filesystem::exists(stale_final) &&
              stale_artifact.value().wait().value().state == TaskState::stale,
          "新视图发布后旧视图制品仍完成提交");

  const auto preparing_temporary = artifact_root / "preparing.partial";
  const auto preparing_final = artifact_root / "preparing.bin";
  {
    std::ofstream stream{preparing_temporary, std::ios::binary};
    const std::string block(1024U * 1024U, 'x');
    for (int index = 0; index < 64; ++index) {
      stream.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
  }
  const auto preparing_view = runtime.issue_view_request("artifact-preparing");
  auto preparing_spec = make_spec("artifact-preparing");
  preparing_spec.view_request = preparing_view;
  std::atomic_bool preparing_entered{};
  std::atomic_bool preparing_commit_finished{};
  std::atomic_bool preparing_commit_succeeded{};
  auto preparing_artifact = runtime.submit(std::move(preparing_spec), [&](TaskContext& context) {
    preparing_entered.store(true);
    preparing_commit_succeeded.store(context.commit_artifact(preparing_temporary, preparing_final).ok());
    preparing_commit_finished.store(true);
    return TaskExecutionResult::completed();
  });
  wait_until([&] { return preparing_entered.load(); }, 1s, "大制品任务未进入提交准备");
  std::this_thread::sleep_for(20ms);
  const auto view_begin = std::chrono::steady_clock::now();
  static_cast<void>(runtime.issue_view_request("artifact-preparing"));
  const auto view_latency = std::chrono::steady_clock::now() - view_begin;
  const bool preparation_finished_when_new_view_returned = preparing_commit_finished.load();
  require(view_latency < 50ms && !preparation_finished_when_new_view_returned, "大制品同步或摘要计算阻塞了新视图请求");
  wait_until([&] { return preparing_commit_finished.load(); }, 5s, "大制品准备阶段提交未返回");
  require(!preparing_commit_succeeded.load() && !std::filesystem::exists(preparing_final) &&
              preparing_artifact.value().wait().value().state == TaskState::stale,
          "新视图在大制品准备阶段生效后仍发布了旧制品");

  const auto racing_temporary = artifact_root / "racing.partial";
  const auto racing_final = artifact_root / "racing.bin";
  {
    std::ofstream stream{racing_temporary, std::ios::binary};
    const std::string block(1024U * 1024U, 'y');
    for (int index = 0; index < 16; ++index) {
      stream.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
  }
  const auto racing_view = runtime.issue_view_request("artifact-race");
  auto racing_spec = make_spec("artifact-race");
  racing_spec.view_request = racing_view;
  std::atomic_bool racing_entered{};
  std::atomic_bool racing_commit_finished{};
  std::atomic_bool racing_commit_succeeded{};
  auto racing_artifact = runtime.submit(std::move(racing_spec), [&](TaskContext& context) {
    racing_entered.store(true);
    racing_commit_succeeded.store(context.commit_artifact(racing_temporary, racing_final).ok());
    racing_commit_finished.store(true);
    return TaskExecutionResult::completed();
  });
  wait_until([&] { return racing_entered.load(); }, 1s, "并发视图制品任务未进入提交");
  static_cast<void>(runtime.issue_view_request("artifact-race"));
  const auto status_when_new_view_returned = runtime.status(racing_artifact.value().id());
  const bool registered_when_new_view_returned =
      status_when_new_view_returned && !status_when_new_view_returned.value().committed_artifacts.empty();
  wait_until([&] { return racing_commit_finished.load(); }, 2s, "并发视图制品提交未返回");
  const auto racing_status = racing_artifact.value().wait();
  require(
      (!racing_commit_succeeded.load() && !std::filesystem::exists(racing_final) &&
       racing_status.value().state == TaskState::stale) ||
          (racing_commit_succeeded.load() && registered_when_new_view_returned &&
           std::filesystem::exists(racing_final) &&
           (racing_status.value().state == TaskState::completed || racing_status.value().state == TaskState::stale)),
      "视图更新越过了 rename、目录同步或制品登记最终临界区");
  std::error_code cleanup_error;
  std::filesystem::remove_all(artifact_root, cleanup_error);
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
  auto zero_threads = make_spec("resource-zero-threads");
  zero_threads.resources = {1U, 0U, 0U, 0U};
  require(!runtime.submit(std::move(zero_threads), [](TaskContext&) { return TaskExecutionResult::completed(); }),
          "零运行时线程声明绕过了统一资源预算");
  auto zero_cpu = make_spec("resource-zero-cpu");
  zero_cpu.resources = {0U, 0U, 0U, 1U};
  require(!runtime.submit(std::move(zero_cpu), [](TaskContext&) { return TaskExecutionResult::completed(); }),
          "零 CPU 声明绕过了统一资源预算");
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
  failing.push_back({failed_parent, [](TaskContext& context) {
                       if (context.attempt() > 1U) {
                         return TaskExecutionResult::completed();
                       }
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
  auto failed_retry_duplicate = runtime.retry(TaskId{"failed-child-id"});
  require(failed_retry.ok() && failed_retry_duplicate.ok() &&
              failed_retry.value().id() == failed_retry_duplicate.value().id() &&
              failed_retry.value().wait().value().state == TaskState::completed,
          "失败依赖链未以幂等的新 TaskId 重建并完成");

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
  release.store(true);
  wait_until([&] { return runtime.status(TaskId{"cancel-parent-id"}).value().state == TaskState::canceled; }, 1s,
             "取消父任务未进入终态");
  auto canceled_retry = runtime.retry(TaskId{"cancel-child-id"});
  require(canceled_retry.ok() && canceled_retry.value().wait().value().state == TaskState::completed,
          "取消依赖链未重建并完成");

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
  stale_release.store(true);
  auto stale_retry = runtime.retry(TaskId{"stale-child-id"});
  require(stale_retry.ok() && stale_retry.value().wait().value().state == TaskState::completed,
          "过期依赖链未绑定最新视图请求并完成");

  auto exhausted_parent = make_spec("exhausted-parent");
  exhausted_parent.task_id = TaskId{"exhausted-parent-id"};
  exhausted_parent.max_attempts = 1U;
  auto exhausted_child = make_spec("exhausted-child");
  exhausted_child.task_id = TaskId{"exhausted-child-id"};
  exhausted_child.dependencies = {exhausted_parent.task_id};
  std::vector<TaskDefinition> exhausted;
  exhausted.push_back({exhausted_child, [](TaskContext&) { return TaskExecutionResult::completed(); }});
  exhausted.push_back({exhausted_parent, [](TaskContext&) {
                         return TaskExecutionResult::failed(
                             {"TASK.EXHAUSTED", "尝试耗尽", "依赖无法重试", "修改参数", false, {}, "logs://exhausted"});
                       }});
  auto exhausted_tasks = runtime.submit_batch(std::move(exhausted));
  require(exhausted_tasks && exhausted_tasks.value()[0].wait().value().state == TaskState::dependency_failed &&
              !runtime.retry(TaskId{"exhausted-child-id"}),
          "依赖达到 max_attempts 后仍被不完整重建");
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

class OrderedObserver final : public signal::task::ITaskObserver {
public:
  void on_event(const signal::task::TaskEvent& event) noexcept override {
    std::lock_guard lock{mutex_};
    events_.push_back(event);
  }

  [[nodiscard]] std::vector<signal::task::TaskEvent> snapshot() const {
    std::lock_guard lock{mutex_};
    return events_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<signal::task::TaskEvent> events_;
};

class ReentrantCancelObserver final : public signal::task::ITaskObserver {
public:
  TaskRuntime* runtime{};
  std::string task_type;
  std::atomic_bool triggered{};

  void on_event(const signal::task::TaskEvent& event) noexcept override {
    if (runtime != nullptr && event.status.task_type == task_type && event.status.state == TaskState::queued &&
        !triggered.exchange(true)) {
      static_cast<void>(runtime->cancel(event.status.task_id));
    }
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
  const auto progress_journal = base / "progress.journal";
  const auto legacy_journal = base / "legacy-sstj2.journal";
  const auto ordering_journal = base / "ordering.journal";
  const auto dag_live_journal = base / "dag-live.journal";
  const auto dag_crash_journal = base / "dag-crash.journal";
  const auto unknown_state_journal = base / "unknown-state.journal";
  const auto temporary_recovery_journal = base / "temporary-recovery.journal";
  const auto temporary_artifact = base / "artifact.partial";
  const auto final_artifact = base / "artifact.bin";
  std::filesystem::create_directories(base);

  {
    auto config = make_config(8U);
    config.history_file = ordering_journal;
    TaskRuntime runtime{config};
    auto reentrant = std::make_shared<ReentrantCancelObserver>();
    reentrant->runtime = &runtime;
    reentrant->task_type = "test.reentrant-order";
    auto ordered = std::make_shared<OrderedObserver>();
    const auto reentrant_subscription = runtime.add_observer(reentrant);
    const auto ordered_subscription = runtime.add_observer(ordered);

    std::atomic_bool release_blockers{};
    std::vector<TaskHandle> blockers;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
      auto blocker = runtime.submit(make_spec("ordering-blocker-" + std::to_string(index)),
                                    [&](TaskContext& context) { return cooperative_wait(context, release_blockers); });
      require(blocker.ok(), "事件顺序阻塞任务提交失败");
      blockers.push_back(std::move(blocker.value()));
    }
    for (const auto& blocker : blockers) {
      wait_state(blocker, TaskState::running);
    }

    auto canceled_spec = make_spec("reentrant-order");
    auto canceled =
        runtime.submit(std::move(canceled_spec), [](TaskContext&) { return TaskExecutionResult::completed(); });
    require(canceled && canceled.value().wait().value().state == TaskState::canceled && reentrant->triggered.load(),
            "观察者回调中的可重入取消失败");
    release_blockers.store(true);
    for (const auto& blocker : blockers) {
      require(blocker.wait().value().state == TaskState::completed, "事件顺序阻塞任务未完成");
    }

    std::vector<TaskHandle> concurrent;
    for (std::uint32_t index = 0U; index < 32U; ++index) {
      auto submitted = runtime.submit(make_spec("ordered-" + std::to_string(index)),
                                      [](TaskContext&) { return TaskExecutionResult::completed(); });
      require(submitted.ok(), "并发事件顺序任务提交失败");
      concurrent.push_back(std::move(submitted.value()));
    }
    for (const auto& handle : concurrent) {
      require(handle.wait().value().state == TaskState::completed, "并发事件顺序任务未完成");
    }
    const auto events = ordered->snapshot();
    require(!events.empty() && events.front().sequence == 1U, "全局事件序号未从 1 开始");
    std::map<std::string, std::uint64_t> last_revision;
    for (std::size_t index = 0U; index < events.size(); ++index) {
      if (index != 0U) {
        require(events[index].sequence == events[index - 1U].sequence + 1U, "观察者收到乱序或跳号事件");
      }
      auto& revision = last_revision[events[index].status.task_id.value];
      require(events[index].status.revision > revision, "同一任务状态在观察者中发生倒退");
      revision = events[index].status.revision;
    }
    runtime.remove_observer(reentrant_subscription);
    runtime.remove_observer(ordered_subscription);
  }

  {
    auto config = make_config();
    config.history_file = progress_journal;
    TaskRuntime runtime{config};
    std::atomic_bool rejected_nonfinite{};
    auto submitted = runtime.submit(make_spec("finite-history"), [&](TaskContext& context) {
      rejected_nonfinite.store(!context.report_progress(std::numeric_limits<double>::quiet_NaN(), "NaN") &&
                               !context.report_progress(std::numeric_limits<double>::infinity(), "+Inf") &&
                               !context.report_progress(-std::numeric_limits<double>::infinity(), "-Inf"));
      static_cast<void>(context.report_progress(0.5, "有限进度"));
      return TaskExecutionResult::completed();
    });
    require(submitted && submitted.value().wait().value().state == TaskState::completed && rejected_nonfinite.load() &&
                runtime.history_healthy(),
            "非有限进度污染了任务状态或持久历史");
  }
  {
    auto config = make_config();
    config.history_file = progress_journal;
    TaskRuntime recovered{config};
    const auto history = recovered.history();
    require(recovered.history_healthy() && history.size() == 1U && std::isfinite(history.front().progress) &&
                history.front().state == TaskState::completed,
            "有限进度历史未健康恢复");
  }
  write_sstj2_journal(progress_journal, legacy_journal);
  {
    auto config = make_config();
    config.history_file = legacy_journal;
    TaskRuntime recovered{config};
    require(recovered.history_healthy() && recovered.history().size() == 1U &&
                recovered.history().front().state == TaskState::completed,
            "SSTJ2 历史兼容恢复失败");
  }
  write_unknown_state_journal(progress_journal, unknown_state_journal);
  {
    auto config = make_config();
    config.history_file = unknown_state_journal;
    TaskRuntime recovered{config};
    require(!recovered.history_healthy() && recovered.history().empty(), "反序列化接受了未知 TaskState");
  }

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
    auto parent_spec = make_spec("recovered-dag-parent");
    parent_spec.task_id = {"recovered-dag-parent-id"};
    parent_spec.timeout = 5s;
    auto child_spec = make_spec("recovered-dag-child");
    child_spec.task_id = {"recovered-dag-child-id"};
    child_spec.dependencies = {parent_spec.task_id};
    child_spec.timeout = 7s;
    {
      auto config = make_config();
      config.history_file = dag_live_journal;
      TaskRuntime runtime{config};
      std::atomic_bool parent_started{};
      std::atomic_bool release_parent{};
      std::vector<TaskDefinition> definitions;
      definitions.push_back({child_spec, [](TaskContext&) { return TaskExecutionResult::completed(); }});
      definitions.push_back({parent_spec, [&](TaskContext& context) {
                               parent_started.store(true);
                               return cooperative_wait(context, release_parent);
                             }});
      auto submitted = runtime.submit_batch(std::move(definitions));
      require(submitted.ok(), "带依赖 DAG 提交失败");
      wait_until([&] { return parent_started.load(); }, 1s, "恢复 DAG 父任务未启动");
      std::filesystem::copy_file(dag_live_journal, dag_crash_journal,
                                 std::filesystem::copy_options::overwrite_existing);
      release_parent.store(true);
      for (const auto& handle : submitted.value()) {
        require(handle.wait().value().state == TaskState::completed, "原始 DAG 未完成");
      }
    }
    {
      auto config = make_config();
      config.history_file = dag_crash_journal;
      TaskRuntime recovered{config};
      const auto history = recovered.history();
      require(history.size() == 2U && std::all_of(history.begin(), history.end(),
                                                  [](const TaskStatus& status) {
                                                    return status.state == TaskState::failed && status.failure &&
                                                           status.failure->error_code == "TASK.CRASH_RECOVERED";
                                                  }),
              "带依赖 DAG 未完整恢复为崩溃失败状态");
      std::atomic_uint32_t execution_order{};
      require(recovered
                      .register_handler(parent_spec.task_type,
                                        [&](TaskContext&) {
                                          require(execution_order.fetch_add(1U) == 0U, "恢复 DAG 父任务执行顺序错误");
                                          return TaskExecutionResult::completed();
                                        })
                      .ok() &&
                  recovered
                      .register_handler(child_spec.task_type,
                                        [&](TaskContext&) {
                                          require(execution_order.fetch_add(1U) == 1U, "恢复 DAG 子任务先于父任务执行");
                                          return TaskExecutionResult::completed();
                                        })
                      .ok(),
              "恢复 DAG 处理器注册失败");
      signal::task::ITaskService& service = recovered;
      auto same_parent = service.submit(parent_spec);
      auto same_child = service.submit(child_spec);
      require(same_parent && same_parent.value().id() == parent_spec.task_id && same_child &&
                  same_child.value().id() == child_spec.task_id,
              "恢复后的 dependencies/timeout 未保持同幂等提交");
      auto retried = recovered.retry(child_spec.task_id);
      require(retried && retried.value().id() != child_spec.task_id &&
                  retried.value().wait().value().state == TaskState::completed && execution_order.load() == 2U,
              "恢复 DAG 未递归创建新 TaskId 并按依赖完成");
    }
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
    const auto recovered_status = history.front();
    require(!recovered.retry(recovered_status.task_id), "未注册 task_type 处理器时恢复任务重试未诚实失败");
    std::atomic_uint32_t handler_invocations{};
    std::atomic_uint32_t recovered_attempt{};
    require(recovered
                .register_handler(recovered_status.task_type,
                                  [&](TaskContext& context) {
                                    handler_invocations.fetch_add(1U);
                                    recovered_attempt.store(context.attempt());
                                    return TaskExecutionResult::completed();
                                  })
                .ok(),
            "恢复任务对应 task_type 处理器注册失败");
    auto retried = recovered.retry(recovered_status.task_id);
    require(retried && retried.value().id() != recovered_status.task_id, "恢复任务重试未创建新 TaskId");
    const auto retry_status = retried.value().wait();
    require(retry_status && retry_status.value().state == TaskState::completed &&
                retry_status.value().attempt == recovered_status.attempt + 1U && handler_invocations.load() == 1U &&
                recovered_attempt.load() == retry_status.value().attempt,
            "恢复→注册处理器→新 TaskId 重试完成闭环失败");
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
