#include "signal_studio/task_runtime/task_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace signal::task {
namespace {

using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] core::Status task_error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::task_runtime, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] FailureInfo dependency_failure(const TaskId& dependency) {
  return {"TASK.DEPENDENCY_FAILED",
          "任务依赖未成功完成",
          "依赖任务 " + dependency.value + " 已取消、失败或过期",
          "检查依赖任务后重试",
          true,
          "retry",
          "task://" + dependency.value + "/log"};
}

[[nodiscard]] FailureInfo timeout_failure() {
  return {"TASK.TIMEOUT",
          "任务执行超时",
          "任务超过声明的超时时间并收到协作式取消请求",
          "调整超时或减小工作单元后重试",
          true,
          "retry",
          "task://current/log"};
}

[[nodiscard]] FailureInfo crash_failure() {
  return {"TASK.CRASH_RECOVERED",
          "任务在上次进程退出时未完成",
          "恢复时检测到非终态任务；运行时不会假定任务或制品已经成功",
          "确认输入和资源后重试",
          true,
          "retry",
          "history://recovery"};
}

[[nodiscard]] FailureInfo artifact_failure() {
  return {"TASK.ARTIFACT_INVALID",
          "已完成任务的制品校验失败",
          "历史记录中的至少一个已提交制品缺失、大小变化或 SHA-256 不一致；该结果不会重新发布",
          "重新运行任务生成制品",
          true,
          "retry",
          "history://artifact-validation"};
}

[[nodiscard]] FailureInfo invalid_failure_info() {
  return {"TASK.INVALID_FAILURE_INFO",
          "任务返回了无效的失败信息",
          "失败信息缺少错误码、用户说明、技术详情、建议、日志入口或可重试动作",
          "修正任务适配器的失败契约",
          false,
          {},
          "runtime://failure-validation"};
}

[[nodiscard]] FailureInfo normalized_failure(std::optional<FailureInfo> failure) {
  if (!failure || !validate_failure_info(*failure).ok()) {
    return invalid_failure_info();
  }
  return std::move(*failure);
}

[[nodiscard]] std::int64_t epoch_milliseconds(SystemClock::time_point value) noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

[[nodiscard]] SystemClock::time_point from_epoch_milliseconds(std::int64_t value) noexcept {
  return SystemClock::time_point{std::chrono::milliseconds{value}};
}

[[nodiscard]] std::string hex_encode(std::string_view input) {
  constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string output;
  output.reserve(input.size() * 2U);
  for (const auto character : input) {
    const auto value = static_cast<unsigned char>(character);
    output.push_back(digits[value >> 4U]);
    output.push_back(digits[value & 0x0FU]);
  }
  return output;
}

[[nodiscard]] std::optional<std::string> hex_decode(std::string_view input) {
  if ((input.size() % 2U) != 0U) {
    return std::nullopt;
  }
  const auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') {
      return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
      return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
      return value - 'A' + 10;
    }
    return -1;
  };
  std::string output;
  output.reserve(input.size() / 2U);
  for (std::size_t index = 0; index < input.size(); index += 2U) {
    const auto high = nibble(input[index]);
    const auto low = nibble(input[index + 1U]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    output.push_back(static_cast<char>((high << 4) | low));
  }
  return output;
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view input) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto character : input) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] std::string checksum(std::string_view input) {
  std::ostringstream stream;
  stream << std::hex << std::setw(16) << std::setfill('0') << fnv1a(input);
  return stream.str();
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view input, char separator) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (begin <= input.size()) {
    const auto end = input.find(separator, begin);
    if (end == std::string_view::npos) {
      parts.push_back(input.substr(begin));
      break;
    }
    parts.push_back(input.substr(begin, end - begin));
    begin = end + 1U;
  }
  return parts;
}

template <typename Integer> [[nodiscard]] bool parse_integer(std::string_view text, Integer& output) noexcept {
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, output);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& target) noexcept {
#ifdef _WIN32
  return ::MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
  std::error_code error;
  std::filesystem::rename(source, target, error);
  return !error;
#endif
}

[[nodiscard]] bool sync_file(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
  const auto handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  const auto flushed = ::FlushFileBuffers(handle) != FALSE;
  static_cast<void>(::CloseHandle(handle));
  return flushed;
#else
  const auto descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return false;
  }
  const auto flushed = ::fsync(descriptor) == 0;
  static_cast<void>(::close(descriptor));
  return flushed;
#endif
}

[[nodiscard]] bool sync_parent_directory(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
  static_cast<void>(path);
  return true;
#else
  const auto parent = path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
  const auto descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) {
    return false;
  }
  const auto flushed = ::fsync(descriptor) == 0;
  static_cast<void>(::close(descriptor));
  return flushed;
#endif
}

class Sha256 final {
public:
  void update(const unsigned char* data, std::size_t size) noexcept {
    total_bytes_ += static_cast<std::uint64_t>(size);
    while (size > 0U) {
      const auto copied = std::min(size, block_.size() - block_size_);
      std::memcpy(block_.data() + block_size_, data, copied);
      block_size_ += copied;
      data += copied;
      size -= copied;
      if (block_size_ == block_.size()) {
        transform(block_.data());
        block_size_ = 0U;
      }
    }
  }

  [[nodiscard]] std::string finish() noexcept {
    const auto total_bits = total_bytes_ * 8U;
    block_[block_size_++] = 0x80U;
    if (block_size_ > 56U) {
      std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), static_cast<unsigned char>(0));
      transform(block_.data());
      block_size_ = 0U;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56,
              static_cast<unsigned char>(0));
    for (std::size_t index = 0; index < 8U; ++index) {
      block_[63U - index] = static_cast<unsigned char>(total_bits >> (index * 8U));
    }
    transform(block_.data());
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : state_) {
      output << std::setw(8) << value;
    }
    return output.str();
  }

private:
  [[nodiscard]] static constexpr std::uint32_t rotate_right(std::uint32_t value, std::uint32_t count) noexcept {
    return (value >> count) | (value << (32U - count));
  }

  void transform(const unsigned char* data) noexcept {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      const auto offset = index * 4U;
      words[index] =
          (static_cast<std::uint32_t>(data[offset]) << 24U) | (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
          (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) | static_cast<std::uint32_t>(data[offset + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const auto first =
          rotate_right(words[index - 15U], 7U) ^ rotate_right(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
      const auto second =
          rotate_right(words[index - 2U], 17U) ^ rotate_right(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + first + words[index - 7U] + second;
    }
    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sigma_one = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temporary_one = h + sigma_one + choose + constants[index] + words[index];
      const auto sigma_zero = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary_two = sigma_zero + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary_one;
      d = c;
      c = b;
      b = a;
      a = temporary_one + temporary_two;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<unsigned char, 64> block_{};
  std::size_t block_size_{};
  std::uint64_t total_bytes_{};
};

struct FileDigest final {
  std::string sha256;
  std::uint64_t size_bytes{};
};

[[nodiscard]] std::optional<FileDigest> digest_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }
  Sha256 hasher;
  std::array<char, 64U * 1024U> buffer{};
  std::uint64_t size = 0U;
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0) {
      hasher.update(reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<std::size_t>(count));
      size += static_cast<std::uint64_t>(count);
    }
  }
  if (!stream.eof()) {
    return std::nullopt;
  }
  return FileDigest{hasher.finish(), size};
}

[[nodiscard]] bool terminal_state(TaskState state) noexcept {
  return state == TaskState::canceled || state == TaskState::completed || state == TaskState::failed ||
         state == TaskState::dependency_failed || state == TaskState::stale;
}

[[nodiscard]] bool failed_dependency_state(TaskState state) noexcept {
  return state == TaskState::canceling || state == TaskState::canceled || state == TaskState::failed ||
         state == TaskState::dependency_failed || state == TaskState::stale;
}

[[nodiscard]] bool resources_fit(const ResourceProfile& required, const ResourceBudget& available) {
  return required.cpu_units <= available.cpu_units && required.io_units <= available.io_units &&
         required.gpu_units <= available.gpu_units && required.runtime_threads <= available.runtime_threads;
}

void subtract_resources(ResourceBudget& available, const ResourceProfile& required) {
  available.cpu_units -= required.cpu_units;
  available.io_units -= required.io_units;
  available.gpu_units -= required.gpu_units;
  available.runtime_threads -= required.runtime_threads;
}

void add_resources(ResourceBudget& available, const ResourceProfile& released) {
  available.cpu_units += released.cpu_units;
  available.io_units += released.io_units;
  available.gpu_units += released.gpu_units;
  available.runtime_threads += released.runtime_threads;
}

[[nodiscard]] bool equivalent_submission(const TaskSpec& left, const TaskSpec& right) {
  return left.task_type == right.task_type && left.priority == right.priority && left.resources == right.resources &&
         left.dependencies == right.dependencies && left.provenance == right.provenance &&
         left.view_request == right.view_request && left.timeout == right.timeout &&
         left.max_attempts == right.max_attempts;
}

} // namespace

namespace detail {

struct Record final {
  TaskSpec spec;
  TaskWork work;
  TaskStatus status;
  std::string base_idempotency_key;
  SteadyClock::time_point submitted_steady{SteadyClock::now()};
  SteadyClock::time_point deadline{};
  std::atomic_bool cancel_requested{};
  std::atomic_bool pause_requested{};
  std::atomic_bool timed_out{};
  std::atomic_bool stale_requested{};
  bool resources_held{};
  std::condition_variable state_changed;
};

struct Emission final {
  TaskEvent event;
  std::string idempotency_key;
  std::string base_idempotency_key;
  std::vector<TaskId> dependencies;
  std::chrono::milliseconds timeout{};
  std::uint32_t max_attempts{1};
  bool persistent{true};
};

class HistoryStore final {
public:
  explicit HistoryStore(std::filesystem::path path) : path_(std::move(path)) {
    load_file();
  }

  struct Recovered final {
    TaskStatus status;
    std::string idempotency_key;
    std::string base_idempotency_key;
    std::vector<TaskId> dependencies;
    std::chrono::milliseconds timeout{};
    std::uint32_t max_attempts{1};
    bool persistent{true};
  };

  [[nodiscard]] std::vector<Recovered> recover() {
    std::lock_guard lock(mutex_);
    std::map<std::string, Recovered> latest;
    for (const auto& record : recovered_) {
      const auto iterator = latest.find(record.status.task_id.value);
      if (iterator == latest.end() || iterator->second.status.revision < record.status.revision) {
        latest[record.status.task_id.value] = record;
      }
    }
    std::vector<Recovered> output;
    output.reserve(latest.size());
    for (auto& [id, record] : latest) {
      static_cast<void>(id);
      output.push_back(std::move(record));
    }
    return output;
  }

  [[nodiscard]] bool append(const Emission& emission) {
    if (path_.empty() || !emission.persistent) {
      return true;
    }
    if (!std::isfinite(emission.event.status.progress) || !validate_task_state(emission.event.status.state).ok() ||
        !validate_task_priority(emission.event.status.priority).ok() ||
        emission.event.status.resources.cpu_units == 0U || emission.event.status.resources.runtime_threads == 0U) {
      healthy_.store(false);
      return false;
    }
    const auto payload = serialize(emission);
    const auto line = payload + '\t' + checksum(payload) + '\n';
    std::lock_guard lock(mutex_);
    std::error_code error;
    if (!path_.parent_path().empty()) {
      std::filesystem::create_directories(path_.parent_path(), error);
      if (error) {
        healthy_.store(false);
        return false;
      }
    }
    const auto temporary = std::filesystem::path{path_.string() + ".tmp"};
    {
      std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      if (!stream) {
        healthy_.store(false);
        return false;
      }
      stream.write(content_.data(), static_cast<std::streamsize>(content_.size()));
      stream.write(line.data(), static_cast<std::streamsize>(line.size()));
      stream.flush();
      if (!stream) {
        healthy_.store(false);
        return false;
      }
    }
    if (!sync_file(temporary)) {
      healthy_.store(false);
      return false;
    }
    if (!atomic_replace(temporary, path_)) {
      healthy_.store(false);
      return false;
    }
    content_ += line;
    if (!sync_parent_directory(path_)) {
      healthy_.store(false);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool healthy() const noexcept {
    return healthy_.load();
  }

private:
  [[nodiscard]] static std::string serialize(const Emission& emission) {
    const auto& status = emission.event.status;
    const auto optional_time = [](const std::optional<SystemClock::time_point>& value) {
      return value ? epoch_milliseconds(*value) : std::int64_t{-1};
    };
    std::ostringstream artifacts;
    for (std::size_t index = 0; index < status.committed_artifacts.size(); ++index) {
      if (index != 0U) {
        artifacts << ',';
      }
      const auto& artifact = status.committed_artifacts[index];
      artifacts << hex_encode(artifact.path.string()) << ':' << artifact.sha256_digest << ':' << artifact.size_bytes;
    }
    std::ostringstream dependencies;
    for (std::size_t index = 0; index < emission.dependencies.size(); ++index) {
      if (index != 0U) {
        dependencies << ',';
      }
      dependencies << hex_encode(emission.dependencies[index].value);
    }
    const auto failure = status.failure.value_or(FailureInfo{});
    const auto view_scope = status.view_request ? status.view_request->scope : std::string{};
    const auto view_generation = status.view_request ? status.view_request->generation : 0U;
    const auto progress = static_cast<std::int64_t>(status.progress * 1'000'000.0);
    std::ostringstream stream;
    stream << "SSTJ3" << '\t' << status.revision << '\t' << hex_encode(status.task_id.value) << '\t'
           << hex_encode(status.task_type) << '\t' << static_cast<unsigned>(status.state) << '\t'
           << static_cast<unsigned>(status.priority) << '\t' << status.resources.cpu_units << '\t'
           << status.resources.io_units << '\t' << status.resources.gpu_units << '\t'
           << status.resources.runtime_threads << '\t' << hex_encode(status.provenance.project_id.value) << '\t'
           << hex_encode(status.provenance.data_source_version_id.value) << '\t'
           << hex_encode(status.provenance.source_object.type) << '\t' << hex_encode(status.provenance.source_object.id)
           << '\t' << hex_encode(view_scope) << '\t' << view_generation << '\t' << progress << '\t'
           << hex_encode(status.status_text) << '\t' << status.attempt << '\t' << (status.failure ? 1 : 0) << '\t'
           << hex_encode(failure.error_code) << '\t' << hex_encode(failure.user_message) << '\t'
           << hex_encode(failure.technical_details) << '\t' << hex_encode(failure.suggested_action) << '\t'
           << (failure.retryable ? 1 : 0) << '\t' << hex_encode(failure.retry_action) << '\t'
           << hex_encode(failure.log_link) << '\t' << artifacts.str() << '\t' << epoch_milliseconds(status.submitted_at)
           << '\t' << optional_time(status.started_at) << '\t' << optional_time(status.finished_at) << '\t'
           << hex_encode(emission.idempotency_key) << '\t' << hex_encode(emission.base_idempotency_key) << '\t'
           << emission.max_attempts << '\t' << (emission.persistent ? 1 : 0) << '\t' << dependencies.str() << '\t'
           << emission.timeout.count();
    return stream.str();
  }

  [[nodiscard]] static std::optional<Recovered> parse(std::string_view payload) {
    const auto fields = split(payload, '\t');
    const bool version_two = fields.size() == 35U && fields[0] == "SSTJ2";
    const bool version_three = fields.size() == 37U && fields[0] == "SSTJ3";
    if (!version_two && !version_three) {
      return std::nullopt;
    }
    Recovered output;
    unsigned state = 0;
    unsigned priority = 0;
    std::int64_t progress = 0;
    int has_failure = 0;
    int retryable = 0;
    std::int64_t submitted = 0;
    std::int64_t started = 0;
    std::int64_t finished = 0;
    std::int64_t timeout = 0;
    int persistent = 0;
    if (!parse_integer(fields[1], output.status.revision) || !parse_integer(fields[4], state) ||
        !parse_integer(fields[5], priority) || !parse_integer(fields[6], output.status.resources.cpu_units) ||
        !parse_integer(fields[7], output.status.resources.io_units) ||
        !parse_integer(fields[8], output.status.resources.gpu_units) ||
        !parse_integer(fields[9], output.status.resources.runtime_threads) ||
        !parse_integer(fields[15], output.status.view_request.emplace().generation) ||
        !parse_integer(fields[16], progress) || !parse_integer(fields[18], output.status.attempt) ||
        !parse_integer(fields[19], has_failure) || !parse_integer(fields[24], retryable) ||
        !parse_integer(fields[28], submitted) || !parse_integer(fields[29], started) ||
        !parse_integer(fields[30], finished) || !parse_integer(fields[33], output.max_attempts) ||
        !parse_integer(fields[34], persistent) || state > static_cast<unsigned>(TaskState::stale) ||
        priority > static_cast<unsigned>(TaskPriority::interactive) || progress < 0 || progress > 1'000'000 ||
        (has_failure != 0 && has_failure != 1) || (retryable != 0 && retryable != 1) ||
        (persistent != 0 && persistent != 1) || output.status.resources.cpu_units == 0U ||
        output.status.resources.runtime_threads == 0U || output.status.attempt == 0U || output.max_attempts == 0U ||
        (version_three && (!parse_integer(fields[36], timeout) || timeout < 0))) {
      return std::nullopt;
    }
    const auto task_id = hex_decode(fields[2]);
    const auto task_type = hex_decode(fields[3]);
    const auto project = hex_decode(fields[10]);
    const auto data_version = hex_decode(fields[11]);
    const auto source_type = hex_decode(fields[12]);
    const auto source_id = hex_decode(fields[13]);
    const auto view_scope = hex_decode(fields[14]);
    const auto status_text = hex_decode(fields[17]);
    const auto error_code = hex_decode(fields[20]);
    const auto user_message = hex_decode(fields[21]);
    const auto technical = hex_decode(fields[22]);
    const auto suggestion = hex_decode(fields[23]);
    const auto retry_action = hex_decode(fields[25]);
    const auto log_link = hex_decode(fields[26]);
    const auto idempotency = hex_decode(fields[31]);
    const auto base_idempotency = hex_decode(fields[32]);
    if (!task_id || !task_type || !project || !data_version || !source_type || !source_id || !view_scope ||
        !status_text || !error_code || !user_message || !technical || !suggestion || !retry_action || !log_link ||
        !idempotency || !base_idempotency) {
      return std::nullopt;
    }
    output.status.task_id.value = *task_id;
    output.status.task_type = *task_type;
    output.status.state = static_cast<TaskState>(state);
    output.status.priority = static_cast<TaskPriority>(priority);
    output.status.provenance = {{*project}, {*data_version}, {*source_type, *source_id}};
    if (view_scope->empty()) {
      output.status.view_request.reset();
    } else {
      output.status.view_request->scope = *view_scope;
    }
    output.status.progress = static_cast<double>(progress) / 1'000'000.0;
    output.status.status_text = *status_text;
    if (has_failure != 0) {
      output.status.failure =
          FailureInfo{*error_code, *user_message, *technical, *suggestion, retryable != 0, *retry_action, *log_link};
      if (!validate_failure_info(*output.status.failure).ok()) {
        return std::nullopt;
      }
    } else if (!error_code->empty() || !user_message->empty() || !technical->empty() || !suggestion->empty() ||
               retryable != 0 || !retry_action->empty() || !log_link->empty()) {
      return std::nullopt;
    }
    if (!fields[27].empty()) {
      for (const auto encoded : split(fields[27], ',')) {
        const auto artifact_fields = split(encoded, ':');
        std::uint64_t artifact_size = 0U;
        if (artifact_fields.size() != 3U || artifact_fields[1].size() != 64U ||
            !std::all_of(artifact_fields[1].begin(), artifact_fields[1].end(),
                         [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }) ||
            !parse_integer(artifact_fields[2], artifact_size)) {
          return std::nullopt;
        }
        const auto decoded = hex_decode(artifact_fields[0]);
        if (!decoded) {
          return std::nullopt;
        }
        output.status.committed_artifacts.push_back(
            {std::filesystem::path{*decoded}, std::string{artifact_fields[1]}, artifact_size});
      }
    }
    output.status.submitted_at = from_epoch_milliseconds(submitted);
    if (started >= 0) {
      output.status.started_at = from_epoch_milliseconds(started);
    }
    if (finished >= 0) {
      output.status.finished_at = from_epoch_milliseconds(finished);
    }
    output.idempotency_key = *idempotency;
    output.base_idempotency_key = *base_idempotency;
    if (version_three && !fields[35].empty()) {
      std::map<std::string, bool> unique_dependencies;
      for (const auto encoded : split(fields[35], ',')) {
        const auto dependency = hex_decode(encoded);
        if (!dependency || dependency->empty() || *dependency == output.status.task_id.value ||
            !unique_dependencies.emplace(*dependency, true).second) {
          return std::nullopt;
        }
        output.dependencies.push_back({*dependency});
      }
    }
    output.timeout = std::chrono::milliseconds{timeout};
    output.persistent = persistent != 0;
    return output;
  }

  [[nodiscard]] static bool read_history_file(const std::filesystem::path& path, std::string& content,
                                              std::vector<Recovered>& recovered) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return false;
    }
    content.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
    if (stream.bad() || (!content.empty() && content.back() != '\n')) {
      return false;
    }
    std::size_t begin = 0;
    while (begin < content.size()) {
      const auto end = content.find('\n', begin);
      if (end == std::string::npos) {
        return false;
      }
      const std::string_view line{content.data() + begin, end - begin};
      const auto checksum_separator = line.rfind('\t');
      if (checksum_separator == std::string_view::npos) {
        return false;
      }
      const auto payload = line.substr(0, checksum_separator);
      if (checksum(payload) != line.substr(checksum_separator + 1U)) {
        return false;
      }
      auto parsed = parse(payload);
      if (!parsed) {
        return false;
      }
      recovered.push_back(std::move(*parsed));
      begin = end + 1U;
    }
    return true;
  }

  void load_file() {
    if (path_.empty()) {
      return;
    }
    const auto temporary = std::filesystem::path{path_.string() + ".tmp"};
    std::error_code error;
    const auto main_exists = std::filesystem::exists(path_, error);
    if (error) {
      healthy_.store(false);
      return;
    }
    if (main_exists) {
      if (std::filesystem::exists(temporary, error)) {
        error.clear();
        std::filesystem::remove(temporary, error);
        if (error) {
          healthy_.store(false);
        }
      }
      if (!read_history_file(path_, content_, recovered_)) {
        healthy_.store(false);
      }
      return;
    }
    error.clear();
    if (!std::filesystem::exists(temporary, error) || error) {
      if (error) {
        healthy_.store(false);
      }
      return;
    }
    std::string recovered_content;
    std::vector<Recovered> recovered_records;
    if (!read_history_file(temporary, recovered_content, recovered_records)) {
      healthy_.store(false);
      return;
    }
    if (!atomic_replace(temporary, path_) || !sync_parent_directory(path_)) {
      healthy_.store(false);
      return;
    }
    content_ = std::move(recovered_content);
    recovered_ = std::move(recovered_records);
  }

  std::filesystem::path path_;
  mutable std::mutex mutex_;
  std::string content_;
  std::vector<Recovered> recovered_;
  std::atomic_bool healthy_{true};
};

class RuntimeControl;

class TaskContextState final {
public:
  std::weak_ptr<RuntimeControl> control;
  TaskId task_id;
  std::uint32_t attempt{1};
};

class ViewCommitState final {
public:
  explicit ViewCommitState(std::shared_lock<std::shared_mutex> lock) : lock_(std::move(lock)) {}

private:
  std::shared_lock<std::shared_mutex> lock_;
};

class RuntimeControl final : public std::enable_shared_from_this<RuntimeControl> {
public:
  explicit RuntimeControl(RuntimeConfig config)
      : config_(std::move(config)), available_(config_.budget), history_store_(config_.history_file) {
    if (config_.worker_count == 0U || config_.starvation_threshold <= std::chrono::milliseconds::zero() ||
        config_.monitor_interval <= std::chrono::milliseconds::zero() || config_.budget.cpu_units == 0U ||
        config_.budget.runtime_threads == 0U) {
      throw std::invalid_argument("invalid task runtime configuration");
    }
    recover_history();
  }

  ~RuntimeControl() {
    shutdown();
  }

  void start() {
    std::vector<Emission> recovery_events;
    {
      std::lock_guard lock(tasks_mutex_);
      for (auto& [id, record] : tasks_) {
        static_cast<void>(id);
        if (!terminal_state(record->status.state)) {
          transition_locked(*record, TaskState::failed, "崩溃恢复失败", crash_failure());
          record->status.committed_artifacts.clear();
          recovery_events.push_back(emission_locked(*record));
        } else if (record->status.state == TaskState::completed &&
                   !artifacts_valid(record->status.committed_artifacts)) {
          transition_locked(*record, TaskState::failed, "制品恢复校验失败", artifact_failure());
          record->status.committed_artifacts.clear();
          recovery_events.push_back(emission_locked(*record));
        }
      }
    }
    publish_all(std::move(recovery_events));
    workers_.reserve(config_.worker_count);
    for (std::size_t index = 0; index < config_.worker_count; ++index) {
      workers_.emplace_back([this](std::stop_token token) { worker_loop(token); });
    }
    monitor_ = std::jthread([this](std::stop_token token) { monitor_loop(token); });
  }

  [[nodiscard]] core::Status register_handler(std::string task_type, TaskWork work) {
    if (task_type.empty() || !work) {
      return task_error(core::ErrorReason::invalid_argument, "任务类型或处理器为空");
    }
    std::lock_guard lock(handlers_mutex_);
    if (!handlers_.emplace(std::move(task_type), std::move(work)).second) {
      return task_error(core::ErrorReason::invalid_argument, "任务类型处理器已注册");
    }
    return core::Status::success();
  }

  [[nodiscard]] core::Status remove_handler(std::string_view task_type) {
    std::lock_guard lock(handlers_mutex_);
    if (handlers_.erase(std::string{task_type}) == 0U) {
      return task_error(core::ErrorReason::invalid_argument, "任务类型处理器不存在");
    }
    return core::Status::success();
  }

  [[nodiscard]] core::Result<TaskHandle> submit_registered(const TaskSpec& spec) {
    TaskWork work;
    {
      std::lock_guard lock(handlers_mutex_);
      const auto iterator = handlers_.find(spec.task_type);
      if (iterator == handlers_.end()) {
        return task_error(core::ErrorReason::unavailable, "任务类型没有已注册处理器", spec.task_type);
      }
      work = iterator->second;
    }
    std::vector<TaskDefinition> definitions;
    definitions.push_back({spec, std::move(work)});
    auto result = submit_batch(std::move(definitions));
    if (!result) {
      return result.error();
    }
    return std::move(result.value().front());
  }

  [[nodiscard]] core::Result<std::vector<TaskHandle>> submit_batch(std::vector<TaskDefinition> definitions) {
    if (definitions.empty()) {
      return task_error(core::ErrorReason::invalid_argument, "任务批次不能为空");
    }
    for (auto& definition : definitions) {
      if (definition.spec.task_id.value.empty()) {
        definition.spec.task_id = TaskId::generate();
      }
      if (const auto validation = validate_definition(definition); !validation.ok()) {
        return validation;
      }
    }

    std::vector<Emission> emissions;
    std::vector<TaskHandle> handles;
    {
      std::lock_guard lock(tasks_mutex_);
      if (stopping_) {
        return task_error(core::ErrorReason::unavailable, "任务运行时正在关闭");
      }
      if (definitions.size() == 1U) {
        const auto existing = idempotency_index_.find(definitions.front().spec.idempotency_key);
        if (existing != idempotency_index_.end()) {
          const auto record = tasks_.at(existing->second.value);
          if (!equivalent_submission(record->spec, definitions.front().spec)) {
            return task_error(core::ErrorReason::invalid_argument, "幂等键与已有任务定义冲突",
                              definitions.front().spec.idempotency_key);
          }
          handles.push_back(TaskHandle{shared_from_this(), record->status.task_id});
          return handles;
        }
      } else {
        for (const auto& definition : definitions) {
          if (idempotency_index_.contains(definition.spec.idempotency_key)) {
            return task_error(core::ErrorReason::invalid_argument, "批量提交不能混合已有幂等任务与新任务");
          }
        }
      }

      std::map<std::string, const TaskSpec*> planned;
      std::map<std::string, std::string> planned_keys;
      for (const auto& definition : definitions) {
        if (tasks_.contains(definition.spec.task_id.value) ||
            !planned.emplace(definition.spec.task_id.value, &definition.spec).second) {
          return task_error(core::ErrorReason::invalid_argument, "任务标识重复", definition.spec.task_id.value);
        }
        if (!planned_keys.emplace(definition.spec.idempotency_key, definition.spec.task_id.value).second) {
          return task_error(core::ErrorReason::invalid_argument, "批次内幂等键重复", definition.spec.idempotency_key);
        }
      }
      for (const auto& definition : definitions) {
        for (const auto& dependency : definition.spec.dependencies) {
          if (!tasks_.contains(dependency.value) && !planned.contains(dependency.value)) {
            return task_error(core::ErrorReason::invalid_argument, "依赖任务不存在", dependency.value);
          }
        }
      }
      if (has_cycle_locked(planned)) {
        return task_error(core::ErrorReason::invalid_argument, "任务依赖图包含环");
      }

      handles.reserve(definitions.size());
      emissions.reserve(definitions.size());
      for (auto& definition : definitions) {
        auto record = make_record(std::move(definition.spec), std::move(definition.work), 1U, {});
        record->base_idempotency_key = record->spec.idempotency_key;
        const auto id = record->status.task_id.value;
        idempotency_index_[record->spec.idempotency_key] = record->status.task_id;
        handles.push_back(TaskHandle{shared_from_this(), record->status.task_id});
        tasks_.emplace(id, record);
      }
      for (auto& handle : handles) {
        auto record = tasks_.at(handle.id().value);
        const auto failed_dependency = first_failed_dependency_locked(*record);
        if (failed_dependency) {
          transition_locked(*record, TaskState::dependency_failed, "依赖任务失败",
                            dependency_failure(*failed_dependency));
        }
        emissions.push_back(emission_locked(*record));
      }
    }
    publish_all(std::move(emissions));
    scheduler_changed_.notify_all();
    return handles;
  }

  [[nodiscard]] core::Result<TaskHandle> retry(const TaskId& task_id) {
    std::map<std::string, TaskWork> registered_handlers;
    {
      std::lock_guard lock(handlers_mutex_);
      registered_handlers = handlers_;
    }
    std::vector<Emission> emissions;
    TaskHandle handle;
    {
      std::shared_lock view_lock(view_mutex_);
      std::lock_guard lock(tasks_mutex_);
      const auto iterator = tasks_.find(task_id.value);
      if (iterator == tasks_.end()) {
        return task_error(core::ErrorReason::invalid_argument, "任务不存在", task_id.value);
      }
      const auto source = iterator->second;
      if (!terminal_state(source->status.state) || source->status.state == TaskState::completed) {
        return task_error(core::ErrorReason::invalid_argument, "当前任务状态不允许重试");
      }
      RetryPlan plan;
      std::map<std::string, std::uint8_t> marks;
      auto rebuilt = plan_retry_locked(source, true, registered_handlers, plan, marks);
      if (!rebuilt) {
        return rebuilt.error();
      }
      if (plan.entries.empty()) {
        return TaskHandle{shared_from_this(), rebuilt.value()};
      }
      emissions.reserve(plan.entries.size());
      for (auto& entry : plan.entries) {
        auto record = make_record(std::move(entry.spec), std::move(entry.work), entry.attempt,
                                  entry.source->base_idempotency_key);
        const auto id = record->status.task_id.value;
        idempotency_index_[record->spec.idempotency_key] = record->status.task_id;
        tasks_.emplace(id, record);
        emissions.push_back(emission_locked(*record));
      }
      handle = TaskHandle{shared_from_this(), rebuilt.value()};
    }
    publish_all(std::move(emissions));
    scheduler_changed_.notify_all();
    return handle;
  }

  [[nodiscard]] core::Status pause(const TaskId& task_id) {
    std::optional<Emission> emission;
    {
      std::lock_guard lock(tasks_mutex_);
      const auto record = find_locked(task_id);
      if (!record) {
        return task_error(core::ErrorReason::invalid_argument, "任务不存在", task_id.value);
      }
      if (terminal_state(record->status.state) || record->status.state == TaskState::canceling) {
        return task_error(core::ErrorReason::invalid_argument, "终态或取消中的任务不能暂停");
      }
      if (record->pause_requested.exchange(true)) {
        return core::Status::success();
      }
      if (record->status.state == TaskState::queued) {
        transition_locked(*record, TaskState::paused, "排队任务已暂停", std::nullopt);
        emission = emission_locked(*record);
      }
    }
    if (emission) {
      publish(std::move(*emission));
    }
    scheduler_changed_.notify_all();
    return core::Status::success();
  }

  [[nodiscard]] core::Status resume(const TaskId& task_id) {
    std::optional<Emission> emission;
    std::shared_ptr<Record> record;
    {
      std::lock_guard lock(tasks_mutex_);
      record = find_locked(task_id);
      if (!record) {
        return task_error(core::ErrorReason::invalid_argument, "任务不存在", task_id.value);
      }
      if (terminal_state(record->status.state) || record->status.state == TaskState::canceling) {
        return task_error(core::ErrorReason::invalid_argument, "终态或取消中的任务不能恢复");
      }
      if (!record->pause_requested.exchange(false)) {
        return core::Status::success();
      }
      if (record->status.state == TaskState::paused) {
        const auto next_state = record->resources_held ? TaskState::running : TaskState::queued;
        transition_locked(*record, next_state, next_state == TaskState::running ? "任务继续运行" : "任务重新排队",
                          std::nullopt);
        emission = emission_locked(*record);
      }
    }
    record->state_changed.notify_all();
    scheduler_changed_.notify_all();
    if (emission) {
      publish(std::move(*emission));
    }
    return core::Status::success();
  }

  [[nodiscard]] core::Status cancel(const TaskId& task_id) {
    std::vector<Emission> emissions;
    std::shared_ptr<Record> record;
    {
      std::lock_guard lock(tasks_mutex_);
      record = find_locked(task_id);
      if (!record) {
        return task_error(core::ErrorReason::invalid_argument, "任务不存在", task_id.value);
      }
      if (terminal_state(record->status.state) || record->status.state == TaskState::canceling) {
        return core::Status::success();
      }
      record->cancel_requested.store(true);
      record->pause_requested.store(false);
      if (record->status.state == TaskState::running ||
          (record->status.state == TaskState::paused && record->resources_held)) {
        transition_locked(*record, TaskState::canceling, "正在取消", std::nullopt);
      } else {
        transition_locked(*record, TaskState::canceled, "已取消", std::nullopt);
      }
      emissions.push_back(emission_locked(*record));
      propagate_dependency_failure_locked(record->status.task_id, emissions);
    }
    record->state_changed.notify_all();
    scheduler_changed_.notify_all();
    publish_all(std::move(emissions));
    return core::Status::success();
  }

  [[nodiscard]] core::Result<TaskStatus> status(const TaskId& task_id) const {
    std::lock_guard lock(tasks_mutex_);
    const auto record = find_locked(task_id);
    if (!record) {
      return task_error(core::ErrorReason::invalid_argument, "任务不存在", task_id.value);
    }
    return record->status;
  }

  [[nodiscard]] core::Result<TaskStatus> wait(const TaskId& task_id) const {
    std::shared_ptr<Record> record;
    std::unique_lock lock(tasks_mutex_);
    record = find_locked(task_id);
    if (!record) {
      return task_error(core::ErrorReason::invalid_argument, "任务不存在", task_id.value);
    }
    record->state_changed.wait(lock, [&record] { return terminal_state(record->status.state); });
    return record->status;
  }

  [[nodiscard]] std::optional<TaskStatus> wait_for(const TaskId& task_id, std::chrono::milliseconds timeout) const {
    std::shared_ptr<Record> record;
    std::unique_lock lock(tasks_mutex_);
    record = find_locked(task_id);
    if (!record) {
      return std::nullopt;
    }
    if (!record->state_changed.wait_for(lock, timeout, [&record] { return terminal_state(record->status.state); })) {
      return std::nullopt;
    }
    return record->status;
  }

  [[nodiscard]] std::vector<TaskStatus> history() const {
    std::lock_guard lock(tasks_mutex_);
    std::vector<TaskStatus> output;
    output.reserve(tasks_.size());
    for (const auto& [id, record] : tasks_) {
      static_cast<void>(id);
      output.push_back(record->status);
    }
    std::sort(output.begin(), output.end(),
              [](const TaskStatus& left, const TaskStatus& right) { return left.submitted_at < right.submitted_at; });
    return output;
  }

  [[nodiscard]] std::vector<TaskStatus> find_by_provenance(const ProjectId& project_id,
                                                           const DataSourceVersionId& data_source_version_id,
                                                           const SourceObject& source_object) const {
    std::lock_guard lock(tasks_mutex_);
    std::vector<TaskStatus> output;
    for (const auto& [id, record] : tasks_) {
      static_cast<void>(id);
      if (record->status.provenance.project_id == project_id &&
          record->status.provenance.data_source_version_id == data_source_version_id &&
          record->status.provenance.source_object == source_object) {
        output.push_back(record->status);
      }
    }
    return output;
  }

  [[nodiscard]] std::uint64_t add_observer(std::shared_ptr<ITaskObserver> observer) {
    if (!observer) {
      return 0U;
    }
    const auto id = next_observer_id_.fetch_add(1U);
    std::lock_guard lock(observers_mutex_);
    observers_[id] = std::move(observer);
    return id;
  }

  void remove_observer(std::uint64_t id) {
    std::lock_guard lock(observers_mutex_);
    observers_.erase(id);
  }

  [[nodiscard]] ViewRequestId issue_view_request(std::string scope) {
    if (scope.empty()) {
      throw std::invalid_argument("view request scope cannot be empty");
    }
    const auto generation = next_view_generation_.fetch_add(1U);
    ViewRequestId request{std::move(scope), generation};
    {
      std::unique_lock lock(view_mutex_);
      auto& current = current_views_[request.scope];
      current = std::max(current, request.generation);
    }

    std::vector<Emission> emissions;
    std::vector<std::shared_ptr<Record>> wake;
    {
      std::lock_guard lock(tasks_mutex_);
      for (auto& [id, record] : tasks_) {
        static_cast<void>(id);
        if (!record->status.view_request || record->status.view_request->scope != request.scope ||
            record->status.view_request->generation >= request.generation || terminal_state(record->status.state)) {
          continue;
        }
        record->stale_requested.store(true);
        record->cancel_requested.store(true);
        record->pause_requested.store(false);
        transition_locked(*record, TaskState::stale, "视图请求已过期", std::nullopt);
        emissions.push_back(emission_locked(*record));
        propagate_dependency_failure_locked(record->status.task_id, emissions);
        wake.push_back(record);
      }
    }
    for (const auto& record : wake) {
      record->state_changed.notify_all();
    }
    scheduler_changed_.notify_all();
    publish_all(std::move(emissions));
    return request;
  }

  [[nodiscard]] std::optional<ViewCommitPermit> try_begin_view_commit(const ViewRequestId& request) const {
    std::shared_lock lock(view_mutex_);
    const auto iterator = current_views_.find(request.scope);
    if (iterator == current_views_.end() || iterator->second != request.generation) {
      return std::nullopt;
    }
    return ViewCommitPermit{std::make_unique<ViewCommitState>(std::move(lock))};
  }

  [[nodiscard]] bool history_healthy() const noexcept {
    return history_store_.healthy();
  }

  [[nodiscard]] bool cancellation_requested(const TaskId& task_id) const noexcept {
    std::lock_guard lock(tasks_mutex_);
    const auto record = find_locked(task_id);
    return !record || record->cancel_requested.load() || record->stale_requested.load() || stopping_;
  }

  [[nodiscard]] bool checkpoint(const TaskId& task_id) {
    std::optional<Emission> paused;
    std::optional<Emission> resumed;
    std::shared_ptr<Record> record;
    std::unique_lock lock(tasks_mutex_);
    record = find_locked(task_id);
    if (!record || record->cancel_requested.load() || record->stale_requested.load() || stopping_) {
      return false;
    }
    if (record->pause_requested.load()) {
      if (record->status.state == TaskState::running) {
        transition_locked(*record, TaskState::paused, "任务已在安全点暂停", std::nullopt);
        paused = emission_locked(*record);
      }
      lock.unlock();
      if (paused) {
        publish(std::move(*paused));
      }
      lock.lock();
      record->state_changed.wait(lock, [this, &record] {
        return !record->pause_requested.load() || record->cancel_requested.load() || record->stale_requested.load() ||
               stopping_;
      });
      if (record->cancel_requested.load() || record->stale_requested.load() || stopping_) {
        return false;
      }
      if (record->status.state == TaskState::paused) {
        transition_locked(*record, TaskState::running, "任务继续运行", std::nullopt);
        resumed = emission_locked(*record);
      }
    }
    lock.unlock();
    if (resumed) {
      publish(std::move(*resumed));
    }
    return true;
  }

  [[nodiscard]] bool report_progress(const TaskId& task_id, double progress, std::string status_text) {
    if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
      return false;
    }
    std::optional<Emission> emission;
    {
      std::lock_guard lock(tasks_mutex_);
      const auto record = find_locked(task_id);
      if (!record || terminal_state(record->status.state) || progress < record->status.progress) {
        return false;
      }
      record->status.progress = progress;
      record->status.status_text = std::move(status_text);
      ++record->status.revision;
      emission = emission_locked(*record);
    }
    publish(std::move(*emission));
    return true;
  }

  [[nodiscard]] core::Status commit_artifact(const TaskId& task_id, const std::filesystem::path& temporary,
                                             const std::filesystem::path& final_path) {
    std::optional<Emission> emission;
    std::error_code error;
    if (temporary.empty() || final_path.empty()) {
      return task_error(core::ErrorReason::invalid_argument, "制品临时路径或目标路径为空");
    }
    if (!std::filesystem::is_regular_file(temporary, error) || error) {
      return task_error(core::ErrorReason::invalid_argument, "制品临时路径无效");
    }
    error.clear();
    if (std::filesystem::exists(final_path, error) || error) {
      return task_error(core::ErrorReason::invalid_argument, "制品目标已存在或无法检查");
    }
    if (!final_path.parent_path().empty()) {
      std::filesystem::create_directories(final_path.parent_path(), error);
      if (error) {
        return task_error(core::ErrorReason::unavailable, "无法创建制品目录", error.message());
      }
    }
    if (!sync_file(temporary)) {
      return task_error(core::ErrorReason::unavailable, "无法同步制品临时文件");
    }
    const auto digest = digest_file(temporary);
    if (!digest) {
      return task_error(core::ErrorReason::unavailable, "无法计算制品 SHA-256");
    }

    {
      std::shared_lock view_lock(view_mutex_);
      {
        std::lock_guard task_lock(tasks_mutex_);
        const auto record = find_locked(task_id);
        if (!record || record->cancel_requested.load() || record->stale_requested.load() ||
            terminal_state(record->status.state)) {
          return task_error(core::ErrorReason::cancelled, "任务已取消或过期，制品未提交");
        }
        if (record->status.view_request) {
          const auto iterator = current_views_.find(record->status.view_request->scope);
          if (iterator == current_views_.end() || iterator->second != record->status.view_request->generation) {
            return task_error(core::ErrorReason::cancelled, "视图请求已过期，制品未提交");
          }
        }
      }
      error.clear();
      std::filesystem::rename(temporary, final_path, error);
      if (error) {
        return task_error(core::ErrorReason::unavailable, "无法原子提交制品", error.message());
      }
      if (!sync_parent_directory(final_path)) {
        std::filesystem::remove(final_path, error);
        return task_error(core::ErrorReason::unavailable, "无法同步制品目录");
      }
      {
        std::lock_guard lock(tasks_mutex_);
        const auto record = find_locked(task_id);
        if (!record || record->cancel_requested.load() || record->stale_requested.load() ||
            terminal_state(record->status.state)) {
          std::filesystem::remove(final_path, error);
          static_cast<void>(sync_parent_directory(final_path));
          return task_error(core::ErrorReason::cancelled, "提交期间任务已取消，制品已撤回");
        }
        record->status.committed_artifacts.push_back({final_path, digest->sha256, digest->size_bytes});
        ++record->status.revision;
        emission = emission_locked(*record);
      }
    }
    publish(std::move(*emission));
    return core::Status::success();
  }

  [[nodiscard]] core::Status complete_with_existing_artifacts(const TaskId& task_id,
                                                              std::span<const std::filesystem::path> artifact_paths) {
    if (artifact_paths.empty()) {
      return task_error(core::ErrorReason::invalid_argument, "完成正式任务至少需要一个不可变制品文件");
    }
    std::shared_lock view_lock(view_mutex_);
    std::vector<CommittedArtifact> artifacts;
    artifacts.reserve(artifact_paths.size());
    std::map<std::filesystem::path, bool> unique_paths;
    for (const auto& path : artifact_paths) {
      std::error_code error;
      const auto normalized = path.lexically_normal();
      if (path.empty() || !unique_paths.emplace(normalized, true).second ||
          !std::filesystem::is_regular_file(normalized, error) || error) {
        return task_error(core::ErrorReason::invalid_argument, "正式任务制品路径无效或重复", path.string());
      }
      const auto digest = digest_file(normalized);
      if (!digest) {
        return task_error(core::ErrorReason::unavailable, "无法计算正式任务制品 SHA-256", normalized.string());
      }
      artifacts.push_back({normalized, digest->sha256, digest->size_bytes});
    }

    std::optional<Emission> emission;
    std::shared_ptr<Record> record;
    {
      std::lock_guard task_lock(tasks_mutex_);
      record = find_locked(task_id);
      if (!record || record->cancel_requested.load() || record->stale_requested.load() || record->timed_out.load() ||
          terminal_state(record->status.state) || record->status.state != TaskState::running ||
          !record->status.committed_artifacts.empty()) {
        return task_error(core::ErrorReason::cancelled, "任务已取消、过期或不再允许完成正式制品提交");
      }
      if (record->status.view_request) {
        const auto current = current_views_.find(record->status.view_request->scope);
        if (current == current_views_.end() || current->second != record->status.view_request->generation) {
          return task_error(core::ErrorReason::cancelled, "视图请求已过期，正式制品未登记");
        }
      }
      record->status.committed_artifacts = std::move(artifacts);
      record->status.progress = 1.0;
      transition_locked(*record, TaskState::completed, "正式制品已提交，任务完成", std::nullopt);
      emission = emission_locked(*record);
    }
    record->state_changed.notify_all();
    publish(std::move(*emission));
    return core::Status::success();
  }

  void shutdown() noexcept {
    std::vector<Emission> emissions;
    std::vector<std::shared_ptr<Record>> wake;
    {
      std::lock_guard lock(tasks_mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
      for (auto& [id, record] : tasks_) {
        static_cast<void>(id);
        if (terminal_state(record->status.state)) {
          continue;
        }
        record->cancel_requested.store(true);
        record->pause_requested.store(false);
        if (record->resources_held) {
          transition_locked(*record, TaskState::canceling, "运行时关闭，正在取消", std::nullopt);
        } else {
          transition_locked(*record, TaskState::canceled, "运行时关闭，任务已取消", std::nullopt);
        }
        emissions.push_back(emission_locked(*record));
        wake.push_back(record);
      }
    }
    publish_all(std::move(emissions));
    for (const auto& record : wake) {
      record->state_changed.notify_all();
    }
    scheduler_changed_.notify_all();
    monitor_.request_stop();
    if (monitor_.joinable()) {
      monitor_.join();
    }
    for (auto& worker : workers_) {
      worker.request_stop();
    }
    scheduler_changed_.notify_all();
    workers_.clear();
  }

private:
  struct RetryPlanEntry final {
    std::shared_ptr<Record> source;
    TaskSpec spec;
    TaskWork work;
    std::uint32_t attempt{};
  };

  struct RetryPlan final {
    std::vector<RetryPlanEntry> entries;
    std::map<std::string, TaskId> replacements;
  };

  [[nodiscard]] core::Result<TaskId> plan_retry_locked(const std::shared_ptr<Record>& source, bool root,
                                                       const std::map<std::string, TaskWork>& registered_handlers,
                                                       RetryPlan& plan,
                                                       std::map<std::string, std::uint8_t>& marks) const {
    if (!source) {
      return task_error(core::ErrorReason::invalid_argument, "重试依赖任务不存在");
    }
    if (source->status.state == TaskState::completed) {
      return source->status.task_id;
    }
    if (!terminal_state(source->status.state)) {
      return task_error(core::ErrorReason::unavailable, "重试依赖尚未进入终态", source->status.task_id.value);
    }
    if (const auto replacement = plan.replacements.find(source->status.task_id.value);
        replacement != plan.replacements.end()) {
      return replacement->second;
    }
    auto& mark = marks[source->status.task_id.value];
    if (mark == 1U) {
      return task_error(core::ErrorReason::invalid_argument, "重试依赖图包含环", source->status.task_id.value);
    }
    if (source->status.attempt >= source->spec.max_attempts) {
      return task_error(core::ErrorReason::unavailable, "重试依赖已达到最大尝试次数", source->status.task_id.value);
    }
    auto work = source->work;
    if (!work) {
      const auto handler = registered_handlers.find(source->status.task_type);
      if (handler == registered_handlers.end()) {
        return task_error(core::ErrorReason::unavailable, "恢复的任务类型没有已注册处理器", source->status.task_type);
      }
      work = handler->second;
    }

    const auto next_attempt = source->status.attempt + 1U;
    const auto retry_key = source->base_idempotency_key + "#attempt=" + std::to_string(next_attempt);
    if (const auto existing = idempotency_index_.find(retry_key); existing != idempotency_index_.end()) {
      const auto existing_record = find_locked(existing->second);
      if (!existing_record || existing_record->base_idempotency_key != source->base_idempotency_key ||
          existing_record->status.attempt != next_attempt ||
          existing_record->status.task_type != source->status.task_type) {
        return task_error(core::ErrorReason::invalid_argument, "重试幂等键与已有任务冲突", retry_key);
      }
      if (root || existing_record->status.state == TaskState::completed ||
          !terminal_state(existing_record->status.state)) {
        plan.replacements[source->status.task_id.value] = existing_record->status.task_id;
        return existing_record->status.task_id;
      }
      auto advanced = plan_retry_locked(existing_record, false, registered_handlers, plan, marks);
      if (advanced) {
        plan.replacements[source->status.task_id.value] = advanced.value();
      }
      return advanced;
    }

    mark = 1U;
    auto spec = source->spec;
    spec.task_id = TaskId::generate();
    spec.idempotency_key = retry_key;
    for (auto& dependency : spec.dependencies) {
      const auto dependency_record = find_locked(dependency);
      if (!dependency_record) {
        mark = 0U;
        return task_error(core::ErrorReason::invalid_argument, "重试依赖任务不存在", dependency.value);
      }
      auto replacement = plan_retry_locked(dependency_record, false, registered_handlers, plan, marks);
      if (!replacement) {
        mark = 0U;
        return replacement.error();
      }
      dependency = replacement.value();
    }
    if (spec.view_request) {
      const auto current = current_views_.find(spec.view_request->scope);
      if (current == current_views_.end()) {
        mark = 0U;
        return task_error(core::ErrorReason::unavailable, "重试视图范围已不存在", spec.view_request->scope);
      }
      spec.view_request->generation = current->second;
    }
    mark = 2U;
    plan.replacements[source->status.task_id.value] = spec.task_id;
    plan.entries.push_back({source, std::move(spec), std::move(work), next_attempt});
    return plan.replacements[source->status.task_id.value];
  }

  [[nodiscard]] core::Status validate_definition(const TaskDefinition& definition) const {
    const auto& spec = definition.spec;
    if (!definition.work || !validate_task_priority(spec.priority).ok() || spec.task_type.empty() ||
        spec.idempotency_key.empty() || spec.provenance.project_id.value.empty() ||
        spec.provenance.data_source_version_id.value.empty() || spec.provenance.source_object.type.empty() ||
        spec.provenance.source_object.id.empty() || spec.resources.cpu_units == 0U ||
        spec.resources.runtime_threads == 0U || spec.max_attempts == 0U ||
        spec.timeout < std::chrono::milliseconds::zero()) {
      return task_error(core::ErrorReason::invalid_argument,
                        "任务定义缺少工作体、幂等键、完整来源链或最低 CPU/运行时线程资源");
    }
    if (!resources_fit(spec.resources, config_.budget)) {
      return task_error(core::ErrorReason::unavailable, "任务资源申请超过运行时预算");
    }
    std::map<std::string, bool> unique;
    for (const auto& dependency : spec.dependencies) {
      if (dependency.value.empty() || dependency == spec.task_id || !unique.emplace(dependency.value, true).second) {
        return task_error(core::ErrorReason::invalid_argument, "任务依赖无效或重复");
      }
    }
    return core::Status::success();
  }

  [[nodiscard]] bool has_cycle_locked(const std::map<std::string, const TaskSpec*>& planned) const {
    std::map<std::string, std::uint8_t> marks;
    const auto dependencies = [this, &planned](const std::string& id) -> const std::vector<TaskId>* {
      const auto planned_iterator = planned.find(id);
      if (planned_iterator != planned.end()) {
        return &planned_iterator->second->dependencies;
      }
      const auto existing = tasks_.find(id);
      return existing == tasks_.end() ? nullptr : &existing->second->spec.dependencies;
    };
    std::function<bool(const std::string&)> visit = [&](const std::string& id) {
      auto& mark = marks[id];
      if (mark == 1U) {
        return true;
      }
      if (mark == 2U) {
        return false;
      }
      mark = 1U;
      if (const auto* edges = dependencies(id)) {
        for (const auto& dependency : *edges) {
          if (visit(dependency.value)) {
            return true;
          }
        }
      }
      mark = 2U;
      return false;
    };
    for (const auto& [id, spec] : planned) {
      static_cast<void>(spec);
      if (visit(id)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::shared_ptr<Record> make_record(TaskSpec spec, TaskWork work, std::uint32_t attempt,
                                                    std::string base_idempotency_key) const {
    auto record = std::make_shared<Record>();
    record->spec = std::move(spec);
    record->work = std::move(work);
    record->base_idempotency_key =
        base_idempotency_key.empty() ? record->spec.idempotency_key : std::move(base_idempotency_key);
    record->status.task_id = record->spec.task_id;
    record->status.task_type = record->spec.task_type;
    record->status.state = TaskState::queued;
    record->status.priority = record->spec.priority;
    record->status.resources = record->spec.resources;
    record->status.provenance = record->spec.provenance;
    record->status.view_request = record->spec.view_request;
    record->status.status_text = "任务已排队";
    record->status.attempt = attempt;
    record->status.revision = 1U;
    record->status.submitted_at = SystemClock::now();
    record->submitted_steady = SteadyClock::now();
    return record;
  }

  [[nodiscard]] std::shared_ptr<Record> find_locked(const TaskId& task_id) const {
    const auto iterator = tasks_.find(task_id.value);
    return iterator == tasks_.end() ? nullptr : iterator->second;
  }

  [[nodiscard]] std::optional<TaskId> first_failed_dependency_locked(const Record& record) const {
    for (const auto& dependency : record.spec.dependencies) {
      const auto iterator = tasks_.find(dependency.value);
      if (iterator != tasks_.end() && failed_dependency_state(iterator->second->status.state)) {
        return dependency;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool dependencies_completed_locked(const Record& record) const {
    return std::all_of(record.spec.dependencies.begin(), record.spec.dependencies.end(),
                       [this](const TaskId& dependency) {
                         const auto iterator = tasks_.find(dependency.value);
                         return iterator != tasks_.end() && iterator->second->status.state == TaskState::completed;
                       });
  }

  void transition_locked(Record& record, TaskState state, std::string text, std::optional<FailureInfo> failure) const {
    record.status.state = state;
    record.status.status_text = std::move(text);
    record.status.failure = std::move(failure);
    ++record.status.revision;
    if (state == TaskState::running && !record.status.started_at) {
      record.status.started_at = SystemClock::now();
    }
    if (terminal_state(state)) {
      record.status.finished_at = SystemClock::now();
    }
  }

  [[nodiscard]] Emission emission_locked(const Record& record) {
    return {{next_event_sequence_.fetch_add(1U), record.status},
            record.spec.idempotency_key,
            record.base_idempotency_key,
            record.spec.dependencies,
            record.spec.timeout,
            record.spec.max_attempts,
            record.spec.persistent};
  }

  void propagate_dependency_failure_locked(const TaskId& source, std::vector<Emission>& emissions) {
    std::deque<TaskId> pending{source};
    while (!pending.empty()) {
      const auto failed = std::move(pending.front());
      pending.pop_front();
      for (auto& [id, record] : tasks_) {
        static_cast<void>(id);
        if (terminal_state(record->status.state) || record->status.state == TaskState::running ||
            record->status.state == TaskState::canceling) {
          continue;
        }
        const auto depends = std::find(record->spec.dependencies.begin(), record->spec.dependencies.end(), failed) !=
                             record->spec.dependencies.end();
        if (!depends) {
          continue;
        }
        record->pause_requested.store(false);
        transition_locked(*record, TaskState::dependency_failed, "依赖任务失败", dependency_failure(failed));
        emissions.push_back(emission_locked(*record));
        pending.push_back(record->status.task_id);
        record->state_changed.notify_all();
      }
    }
  }

  [[nodiscard]] std::shared_ptr<Record> select_task_locked() {
    std::shared_ptr<Record> selected;
    std::uint64_t selected_score = 0U;
    const auto now = SteadyClock::now();
    for (auto& [id, record] : tasks_) {
      static_cast<void>(id);
      if (record->status.state != TaskState::queued || record->pause_requested.load() ||
          !dependencies_completed_locked(*record) || !resources_fit(record->spec.resources, available_)) {
        continue;
      }
      const auto base = static_cast<std::uint64_t>(record->spec.priority);
      const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - record->submitted_steady);
      const auto age = static_cast<std::uint64_t>(waited.count() / config_.starvation_threshold.count());
      const auto score = base + age;
      if (!selected || score > selected_score ||
          (score == selected_score && record->submitted_steady < selected->submitted_steady)) {
        selected = record;
        selected_score = score;
      }
    }
    if (selected) {
      subtract_resources(available_, selected->spec.resources);
      selected->resources_held = true;
      if (selected->spec.timeout > std::chrono::milliseconds::zero()) {
        selected->deadline = SteadyClock::now() + selected->spec.timeout;
      }
      transition_locked(*selected, TaskState::running, "任务正在运行", std::nullopt);
    }
    return selected;
  }

  void worker_loop(std::stop_token token) {
    while (!token.stop_requested()) {
      std::shared_ptr<Record> record;
      Emission started;
      {
        std::unique_lock lock(tasks_mutex_);
        scheduler_changed_.wait(lock, token, [this] {
          if (stopping_) {
            return true;
          }
          for (const auto& [id, candidate] : tasks_) {
            static_cast<void>(id);
            if (candidate->status.state == TaskState::queued && !candidate->pause_requested.load() &&
                dependencies_completed_locked(*candidate) && resources_fit(candidate->spec.resources, available_)) {
              return true;
            }
          }
          return false;
        });
        if (stopping_ || token.stop_requested()) {
          break;
        }
        record = select_task_locked();
        if (!record) {
          continue;
        }
        started = emission_locked(*record);
      }
      publish(std::move(started));

      auto context_state = std::make_shared<TaskContextState>();
      context_state->control = shared_from_this();
      context_state->task_id = record->status.task_id;
      context_state->attempt = record->status.attempt;
      TaskContext context{std::move(context_state)};
      TaskExecutionResult result;
      try {
        result = record->work(context);
      } catch (const std::exception& exception) {
        result =
            TaskExecutionResult::failed({"TASK.UNHANDLED_EXCEPTION", "任务执行失败", exception.what(), "查看日志并重试",
                                         true, "retry", "task://" + record->status.task_id.value + "/log"});
      } catch (...) {
        result = TaskExecutionResult::failed({"TASK.UNKNOWN_EXCEPTION", "任务执行失败", "工作体抛出未知异常",
                                              "查看日志并重试", true, "retry",
                                              "task://" + record->status.task_id.value + "/log"});
      }

      std::vector<Emission> emissions;
      {
        std::lock_guard lock(tasks_mutex_);
        if (record->resources_held) {
          add_resources(available_, record->spec.resources);
          record->resources_held = false;
        }
        if (!terminal_state(record->status.state)) {
          if (record->timed_out.load()) {
            transition_locked(*record, TaskState::failed, "任务超时", timeout_failure());
          } else if (record->cancel_requested.load()) {
            transition_locked(*record, TaskState::canceled, "任务已取消", std::nullopt);
          } else if (!result.succeeded) {
            auto failure = normalized_failure(std::move(result.failure));
            transition_locked(*record, TaskState::failed, "任务失败", std::move(failure));
          } else {
            record->status.progress = 1.0;
            transition_locked(*record, TaskState::completed, "任务已完成", std::nullopt);
          }
          emissions.push_back(emission_locked(*record));
          if (record->status.state != TaskState::completed) {
            propagate_dependency_failure_locked(record->status.task_id, emissions);
          }
        }
        record->state_changed.notify_all();
      }
      scheduler_changed_.notify_all();
      publish_all(std::move(emissions));
    }
  }

  void monitor_loop(std::stop_token token) {
    while (!token.stop_requested()) {
      std::vector<Emission> emissions;
      std::vector<std::shared_ptr<Record>> wake;
      {
        std::lock_guard lock(tasks_mutex_);
        const auto now = SteadyClock::now();
        for (auto& [id, record] : tasks_) {
          static_cast<void>(id);
          if (record->resources_held && record->spec.timeout > std::chrono::milliseconds::zero() &&
              now >= record->deadline && !record->timed_out.exchange(true)) {
            record->cancel_requested.store(true);
            record->pause_requested.store(false);
            if (record->status.state != TaskState::stale) {
              transition_locked(*record, TaskState::canceling, "任务超时，正在停止", timeout_failure());
              emissions.push_back(emission_locked(*record));
              propagate_dependency_failure_locked(record->status.task_id, emissions);
            }
            wake.push_back(record);
          }
        }
      }
      for (const auto& record : wake) {
        record->state_changed.notify_all();
      }
      publish_all(std::move(emissions));
      std::unique_lock lock(monitor_mutex_);
      monitor_changed_.wait_for(lock, token, config_.monitor_interval, [] { return false; });
    }
  }

  void publish(Emission emission) {
    std::unique_lock publication_lock(publication_mutex_);
    pending_publications_.emplace(emission.event.sequence, std::move(emission));
    if (publication_active_) {
      return;
    }
    publication_active_ = true;
    while (true) {
      const auto next = pending_publications_.find(next_publication_sequence_);
      if (next == pending_publications_.end()) {
        publication_active_ = false;
        return;
      }
      auto current = std::move(next->second);
      pending_publications_.erase(next);
      ++next_publication_sequence_;
      publication_lock.unlock();

      static_cast<void>(history_store_.append(current));
      std::vector<std::shared_ptr<ITaskObserver>> observers;
      {
        std::lock_guard lock(observers_mutex_);
        observers.reserve(observers_.size());
        for (const auto& [id, observer] : observers_) {
          static_cast<void>(id);
          observers.push_back(observer);
        }
      }
      for (const auto& observer : observers) {
        observer->on_event(current.event);
      }
      publication_lock.lock();
    }
  }

  void publish_all(std::vector<Emission> emissions) {
    for (auto& emission : emissions) {
      publish(std::move(emission));
    }
  }

  void recover_history() {
    auto recovered = history_store_.recover();
    std::lock_guard lock(tasks_mutex_);
    for (auto& persisted : recovered) {
      if (persisted.status.task_id.value.empty() || tasks_.contains(persisted.status.task_id.value)) {
        continue;
      }
      auto record = std::make_shared<Record>();
      record->status = std::move(persisted.status);
      record->spec.task_id = record->status.task_id;
      record->spec.task_type = record->status.task_type;
      record->spec.priority = record->status.priority;
      record->spec.resources = record->status.resources;
      record->spec.idempotency_key = std::move(persisted.idempotency_key);
      record->spec.provenance = record->status.provenance;
      record->spec.view_request = record->status.view_request;
      record->spec.dependencies = std::move(persisted.dependencies);
      record->spec.timeout = persisted.timeout;
      record->spec.max_attempts = persisted.max_attempts;
      record->spec.persistent = persisted.persistent;
      record->base_idempotency_key = std::move(persisted.base_idempotency_key);
      record->submitted_steady = SteadyClock::now();
      if (record->status.view_request) {
        auto& generation = current_views_[record->status.view_request->scope];
        generation = std::max(generation, record->status.view_request->generation);
        next_view_generation_.store(std::max(next_view_generation_.load(), generation + 1U));
      }
      idempotency_index_[record->spec.idempotency_key] = record->status.task_id;
      tasks_[record->status.task_id.value] = std::move(record);
    }
  }

  [[nodiscard]] static bool artifacts_valid(const std::vector<CommittedArtifact>& artifacts) {
    return std::all_of(artifacts.begin(), artifacts.end(), [](const CommittedArtifact& artifact) {
      std::error_code error;
      if (!std::filesystem::is_regular_file(artifact.path, error) || error ||
          std::filesystem::file_size(artifact.path, error) != artifact.size_bytes || error) {
        return false;
      }
      const auto digest = digest_file(artifact.path);
      return digest && digest->size_bytes == artifact.size_bytes && digest->sha256 == artifact.sha256_digest;
    });
  }

  RuntimeConfig config_;
  mutable std::mutex tasks_mutex_;
  mutable std::condition_variable_any scheduler_changed_;
  std::map<std::string, std::shared_ptr<Record>> tasks_;
  std::map<std::string, TaskId> idempotency_index_;
  ResourceBudget available_;
  bool stopping_{};
  std::vector<std::jthread> workers_;
  std::jthread monitor_;
  std::mutex monitor_mutex_;
  std::condition_variable_any monitor_changed_;

  mutable std::shared_mutex view_mutex_;
  std::map<std::string, std::uint64_t> current_views_;
  std::atomic_uint64_t next_view_generation_{1U};

  std::mutex observers_mutex_;
  std::map<std::uint64_t, std::shared_ptr<ITaskObserver>> observers_;
  std::atomic_uint64_t next_observer_id_{1U};
  std::atomic_uint64_t next_event_sequence_{1U};
  std::mutex publication_mutex_;
  std::map<std::uint64_t, Emission> pending_publications_;
  std::uint64_t next_publication_sequence_{1U};
  bool publication_active_{};
  std::mutex handlers_mutex_;
  std::map<std::string, TaskWork> handlers_;
  HistoryStore history_store_;
};

} // namespace detail

TaskId TaskId::generate() {
  static std::atomic_uint64_t counter{1U};
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(SystemClock::now().time_since_epoch()).count());
  const auto sequence = counter.fetch_add(1U);
  std::ostringstream stream;
  stream << std::hex << std::setw(16) << std::setfill('0') << ticks << std::setw(16) << std::setfill('0') << sequence;
  return {stream.str()};
}

core::Status validate_task_state(TaskState state) {
  switch (state) {
  case TaskState::queued:
  case TaskState::running:
  case TaskState::paused:
  case TaskState::canceling:
  case TaskState::canceled:
  case TaskState::completed:
  case TaskState::failed:
  case TaskState::dependency_failed:
  case TaskState::stale:
    return core::Status::success();
  }
  return task_error(core::ErrorReason::invalid_argument, "未知任务状态枚举值");
}

core::Status validate_task_priority(TaskPriority priority) {
  switch (priority) {
  case TaskPriority::background:
  case TaskPriority::foreground:
  case TaskPriority::interactive:
    return core::Status::success();
  }
  return task_error(core::ErrorReason::invalid_argument, "未知任务优先级枚举值");
}

core::Status validate_work_class(WorkClass work_class) {
  switch (work_class) {
  case WorkClass::io:
  case WorkClass::dsp:
  case WorkClass::indexing:
  case WorkClass::exporting:
  case WorkClass::inference:
  case WorkClass::other:
    return core::Status::success();
  }
  return task_error(core::ErrorReason::invalid_argument, "未知工作类别枚举值");
}

core::Result<SchedulingRequirement> evaluate_scheduling(WorkClass work_class,
                                                        std::chrono::milliseconds expected_duration) {
  if (const auto validation = validate_work_class(work_class); !validation) {
    return validation;
  }
  if (expected_duration < std::chrono::milliseconds::zero()) {
    return task_error(core::ErrorReason::invalid_argument, "预计工作时长不能为负数");
  }
  const auto task_class = work_class == WorkClass::io || work_class == WorkClass::dsp ||
                          work_class == WorkClass::indexing || work_class == WorkClass::exporting ||
                          work_class == WorkClass::inference;
  return SchedulingRequirement{task_class && expected_duration > std::chrono::milliseconds{50}, expected_duration,
                               work_class};
}

core::Status validate_failure_info(const FailureInfo& failure) {
  if (failure.error_code.empty() || failure.user_message.empty() || failure.technical_details.empty() ||
      failure.suggested_action.empty() || failure.log_link.empty() ||
      (failure.retryable && failure.retry_action.empty())) {
    return task_error(core::ErrorReason::invalid_argument, "任务失败信息不完整");
  }
  return core::Status::success();
}

bool is_terminal(TaskState state) noexcept {
  return terminal_state(state);
}

std::string_view to_string(TaskState state) noexcept {
  switch (state) {
  case TaskState::queued:
    return "queued";
  case TaskState::running:
    return "running";
  case TaskState::paused:
    return "paused";
  case TaskState::canceling:
    return "canceling";
  case TaskState::canceled:
    return "canceled";
  case TaskState::completed:
    return "completed";
  case TaskState::failed:
    return "failed";
  case TaskState::dependency_failed:
    return "dependency_failed";
  case TaskState::stale:
    return "stale";
  }
  return "unknown";
}

std::string_view to_string(TaskPriority priority) noexcept {
  switch (priority) {
  case TaskPriority::background:
    return "background";
  case TaskPriority::foreground:
    return "foreground";
  case TaskPriority::interactive:
    return "interactive";
  }
  return "unknown";
}

TaskContext::TaskContext(std::shared_ptr<detail::TaskContextState> state) : state_(std::move(state)) {}

bool TaskContext::cancellation_requested() const noexcept {
  const auto control = state_ ? state_->control.lock() : nullptr;
  return !control || control->cancellation_requested(state_->task_id);
}

bool TaskContext::checkpoint() {
  const auto control = state_ ? state_->control.lock() : nullptr;
  return control && control->checkpoint(state_->task_id);
}

bool TaskContext::report_progress(double progress, std::string status_text) {
  const auto control = state_ ? state_->control.lock() : nullptr;
  return control && control->report_progress(state_->task_id, progress, std::move(status_text));
}

core::Status TaskContext::commit_artifact(const std::filesystem::path& temporary_path,
                                          const std::filesystem::path& final_path) {
  const auto control = state_ ? state_->control.lock() : nullptr;
  if (!control) {
    return task_error(core::ErrorReason::unavailable, "任务运行时不可用");
  }
  return control->commit_artifact(state_->task_id, temporary_path, final_path);
}

core::Status TaskContext::complete_with_existing_artifacts(std::span<const std::filesystem::path> artifact_paths) {
  const auto control = state_ ? state_->control.lock() : nullptr;
  if (!control) {
    return task_error(core::ErrorReason::unavailable, "任务运行时不可用");
  }
  return control->complete_with_existing_artifacts(state_->task_id, artifact_paths);
}

TaskId TaskContext::task_id() const {
  return state_ ? state_->task_id : TaskId{};
}

std::uint32_t TaskContext::attempt() const noexcept {
  return state_ ? state_->attempt : 0U;
}

TaskExecutionResult TaskExecutionResult::completed() noexcept {
  return {};
}

TaskExecutionResult TaskExecutionResult::failed(FailureInfo failure) {
  return {false, normalized_failure(std::move(failure))};
}

TaskHandle::TaskHandle() = default;
TaskHandle::~TaskHandle() = default;
TaskHandle::TaskHandle(const TaskHandle&) = default;
TaskHandle& TaskHandle::operator=(const TaskHandle&) = default;
TaskHandle::TaskHandle(TaskHandle&&) noexcept = default;
TaskHandle& TaskHandle::operator=(TaskHandle&&) noexcept = default;

TaskHandle::TaskHandle(std::shared_ptr<detail::RuntimeControl> control, TaskId id)
    : control_(std::move(control)), id_(std::move(id)) {}

bool TaskHandle::valid() const noexcept {
  return control_ && !id_.value.empty();
}
const TaskId& TaskHandle::id() const noexcept {
  return id_;
}

core::Status TaskHandle::pause() const {
  return control_ ? control_->pause(id_) : task_error(core::ErrorReason::unavailable, "任务句柄无效");
}

core::Status TaskHandle::resume() const {
  return control_ ? control_->resume(id_) : task_error(core::ErrorReason::unavailable, "任务句柄无效");
}

core::Status TaskHandle::cancel() const {
  return control_ ? control_->cancel(id_) : task_error(core::ErrorReason::unavailable, "任务句柄无效");
}

core::Result<TaskStatus> TaskHandle::status() const {
  return control_ ? control_->status(id_)
                  : core::Result<TaskStatus>{task_error(core::ErrorReason::unavailable, "任务句柄无效")};
}

core::Result<TaskStatus> TaskHandle::wait() const {
  return control_ ? control_->wait(id_)
                  : core::Result<TaskStatus>{task_error(core::ErrorReason::unavailable, "任务句柄无效")};
}

std::optional<TaskStatus> TaskHandle::wait_for(std::chrono::milliseconds timeout) const {
  return control_ ? control_->wait_for(id_, timeout) : std::nullopt;
}

ViewCommitPermit::ViewCommitPermit(std::unique_ptr<detail::ViewCommitState> state) : state_(std::move(state)) {}
ViewCommitPermit::~ViewCommitPermit() = default;
ViewCommitPermit::ViewCommitPermit(ViewCommitPermit&&) noexcept = default;
ViewCommitPermit& ViewCommitPermit::operator=(ViewCommitPermit&&) noexcept = default;

TaskRuntime::TaskRuntime(RuntimeConfig config) : control_(std::make_shared<detail::RuntimeControl>(std::move(config))) {
  control_->start();
}

TaskRuntime::~TaskRuntime() {
  shutdown();
}
TaskRuntime::TaskRuntime(TaskRuntime&&) noexcept = default;
TaskRuntime& TaskRuntime::operator=(TaskRuntime&&) noexcept = default;

core::Result<TaskHandle> TaskRuntime::submit(const TaskSpec& spec) noexcept {
  try {
    return control_->submit_registered(spec);
  } catch (const std::exception& exception) {
    return task_error(core::ErrorReason::internal_failure, "任务提交发生内部异常", exception.what());
  } catch (...) {
    return task_error(core::ErrorReason::internal_failure, "任务提交发生未知内部异常");
  }
}

core::Result<TaskHandle> TaskRuntime::submit(TaskSpec spec, TaskWork work) {
  std::vector<TaskDefinition> definitions;
  definitions.push_back({std::move(spec), std::move(work)});
  auto result = control_->submit_batch(std::move(definitions));
  if (!result) {
    return result.error();
  }
  return std::move(result.value().front());
}

core::Status TaskRuntime::register_handler(std::string task_type, TaskWork work) {
  return control_->register_handler(std::move(task_type), std::move(work));
}

core::Status TaskRuntime::remove_handler(std::string_view task_type) {
  return control_->remove_handler(task_type);
}

core::Result<std::vector<TaskHandle>> TaskRuntime::submit_batch(std::vector<TaskDefinition> definitions) {
  return control_->submit_batch(std::move(definitions));
}

core::Result<TaskHandle> TaskRuntime::retry(const TaskId& task_id) {
  return control_->retry(task_id);
}
core::Status TaskRuntime::pause(const TaskId& task_id) {
  return control_->pause(task_id);
}
core::Status TaskRuntime::resume(const TaskId& task_id) {
  return control_->resume(task_id);
}
core::Status TaskRuntime::cancel(const TaskId& task_id) {
  return control_->cancel(task_id);
}

core::Result<TaskStatus> TaskRuntime::status(const TaskId& task_id) const {
  return control_->status(task_id);
}

std::vector<TaskStatus> TaskRuntime::history() const {
  return control_->history();
}

std::vector<TaskStatus> TaskRuntime::find_by_provenance(const ProjectId& project_id,
                                                        const DataSourceVersionId& data_source_version_id,
                                                        const SourceObject& source_object) const {
  return control_->find_by_provenance(project_id, data_source_version_id, source_object);
}

std::uint64_t TaskRuntime::add_observer(std::shared_ptr<ITaskObserver> observer) {
  return control_->add_observer(std::move(observer));
}

void TaskRuntime::remove_observer(std::uint64_t subscription_id) {
  control_->remove_observer(subscription_id);
}

ViewRequestId TaskRuntime::issue_view_request(std::string scope) {
  return control_->issue_view_request(std::move(scope));
}

std::optional<ViewCommitPermit> TaskRuntime::try_begin_view_commit(const ViewRequestId& request) const {
  return control_->try_begin_view_commit(request);
}

bool TaskRuntime::history_healthy() const noexcept {
  return control_ && control_->history_healthy();
}

void TaskRuntime::shutdown() noexcept {
  if (control_) {
    control_->shutdown();
  }
}

} // namespace signal::task
