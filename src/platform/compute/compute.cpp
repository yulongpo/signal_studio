#include "signal_studio/compute/compute.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <numeric>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <emmintrin.h>
#include <intrin.h>
#endif

namespace signal::compute {
[[nodiscard]] BackendCapabilities discover_cuda_capability() noexcept;
[[nodiscard]] core::Status execute_cuda_buffer_copy(const BufferCopyRequest& request) noexcept;

namespace {

[[nodiscard]] core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::compute, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] const BackendCapabilities* find_available(std::span<const BackendCapabilities> capabilities,
                                                        BackendKind kind) {
  const auto found = std::ranges::find_if(capabilities, [kind](const BackendCapabilities& capability) {
    return capability.kind == kind && capability.available;
  });
  return found == capabilities.end() ? nullptr : &*found;
}

[[nodiscard]] bool supports_operation(const BackendCapabilities& capability, std::string_view operation) noexcept {
  if (operation == "buffer-copy" || operation == "command-feedback") {
    return capability.supports_buffer_copy;
  }
  if (operation == "fft") {
    return capability.supports_fft;
  }
  if (operation == "convolution") {
    return capability.supports_convolution;
  }
  if (operation == "filter" || operation == "resample") {
    return capability.supports_convolution;
  }
  if (operation == "linear-solve") {
    return capability.supports_linear_solve;
  }
  return false;
}

[[nodiscard]] bool has_working_set_capacity(const BackendCapabilities& capability, const Workload& workload) noexcept {
  return capability.memory_bytes == 0U || capability.memory_bytes >= workload.working_set_bytes;
}

[[nodiscard]] const BackendCapabilities* find_operation_fallback(std::span<const BackendCapabilities> capabilities,
                                                                 const Workload& workload,
                                                                 const IComputeOperation& operation,
                                                                 std::span<const BackendKind> excluded) {
  for (const auto kind : {BackendKind::cpu_multithread, BackendKind::cpu_simd, BackendKind::cpu_scalar}) {
    if (std::ranges::find(excluded, kind) != excluded.end()) {
      continue;
    }
    const auto* candidate = find_available(capabilities, kind);
    if (candidate != nullptr && supports_operation(*candidate, workload.operation) && operation.supports(kind) &&
        has_working_set_capacity(*candidate, workload)) {
      return candidate;
    }
  }
  return nullptr;
}

[[nodiscard]] BackendProvenance provenance(const BackendCapabilities& capability, BackendKind requested,
                                           std::string reason, bool degraded) {
  return {requested,
          capability.kind,
          capability.backend_id,
          capability.version,
          capability.device,
          std::move(reason),
          degraded,
          false};
}

class DiscoveredComputeBackend final : public IComputeBackend {
public:
  explicit DiscoveredComputeBackend(BackendCapabilities capabilities) : capabilities_(std::move(capabilities)) {}

  [[nodiscard]] BackendCapabilities capabilities() const noexcept override {
    return capabilities_;
  }

  [[nodiscard]] bool supports(const Workload& workload) const noexcept override {
    return capabilities_.available && supports_operation(capabilities_, workload.operation);
  }

  [[nodiscard]] core::Status execute_buffer_copy(const BufferCopyRequest& request,
                                                 std::uint32_t worker_threads) const override {
    if (!capabilities_.available || !capabilities_.supports_buffer_copy) {
      return error(core::ErrorReason::unavailable, "计算后端不支持缓冲区复制", capabilities_.backend_id);
    }
    if (request.input.empty() || request.input.size() != request.output.size()) {
      return error(core::ErrorReason::invalid_argument, "缓冲区复制要求非空且输入输出长度一致");
    }
    if (request.input.data() == request.output.data()) {
      return core::Status::success();
    }

    switch (capabilities_.kind) {
    case BackendKind::cpu_scalar:
      for (std::size_t index = 0; index < request.input.size(); ++index) {
        request.output[index] = request.input[index];
      }
      return core::Status::success();
    case BackendKind::cpu_simd:
#if defined(_WIN32)
    {
      std::size_t index{};
      for (; index + 2U <= request.input.size(); index += 2U) {
        const auto values = _mm_loadu_pd(request.input.data() + index);
        _mm_storeu_pd(request.output.data() + index, values);
      }
      for (; index < request.input.size(); ++index) {
        request.output[index] = request.input[index];
      }
      return core::Status::success();
    }
#else
      std::copy(request.input.begin(), request.input.end(), request.output.begin());
      return core::Status::success();
#endif
    case BackendKind::cpu_multithread: {
      const auto threads = std::min<std::size_t>(std::max<std::uint32_t>(1U, worker_threads), request.input.size());
      const auto chunk = (request.input.size() + threads - 1U) / threads;
      try {
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for (std::size_t worker = 0; worker < threads; ++worker) {
          const auto begin = worker * chunk;
          const auto end = std::min(request.input.size(), begin + chunk);
          if (begin == end) {
            break;
          }
          workers.emplace_back([input = request.input, output = request.output, begin, end] {
            std::copy(input.begin() + static_cast<std::ptrdiff_t>(begin),
                      input.begin() + static_cast<std::ptrdiff_t>(end),
                      output.begin() + static_cast<std::ptrdiff_t>(begin));
          });
        }
      } catch (const std::exception& exception) {
        return error(core::ErrorReason::internal_failure, "CPU 多线程后端执行失败", exception.what());
      }
      return core::Status::success();
    }
    case BackendKind::cuda:
      return execute_cuda_buffer_copy(request);
    }
    return error(core::ErrorReason::internal_failure, "未知计算后端");
  }

private:
  BackendCapabilities capabilities_;
};

[[nodiscard]] std::vector<std::shared_ptr<IComputeBackend>>
make_backends(std::vector<BackendCapabilities> capabilities) {
  std::vector<std::shared_ptr<IComputeBackend>> backends;
  backends.reserve(capabilities.size());
  for (auto& capability : capabilities) {
    backends.push_back(std::make_shared<DiscoveredComputeBackend>(std::move(capability)));
  }
  return backends;
}

[[nodiscard]] bool ranges_overlap(const BufferCopyRequest& request) noexcept {
  if (request.input.data() == request.output.data()) {
    return false;
  }
  const auto input_begin = reinterpret_cast<std::uintptr_t>(request.input.data());
  const auto output_begin = reinterpret_cast<std::uintptr_t>(request.output.data());
  const auto bytes = request.input.size_bytes();
  if (input_begin > std::numeric_limits<std::uintptr_t>::max() - bytes ||
      output_begin > std::numeric_limits<std::uintptr_t>::max() - bytes) {
    return true;
  }
  return input_begin < output_begin + bytes && output_begin < input_begin + bytes;
}

} // namespace

std::vector<BackendCapabilities> discover_compute_capabilities() {
  const auto threads = std::max(1U, std::thread::hardware_concurrency());
  std::uint64_t memory_bytes{};
#if defined(_WIN32)
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory) != 0) {
    memory_bytes = memory.ullTotalPhys;
  }
  int registers[4]{};
  __cpuid(registers, 1);
  const bool simd = (registers[3] & (1 << 26)) != 0;
#else
#if defined(__SSE2__)
  const bool simd = true;
#else
  const bool simd = false;
#endif
#endif
  std::vector<BackendCapabilities> capabilities;
  capabilities.push_back({BackendKind::cpu_scalar, "host-cpu-scalar", "C++20", "host-cpu", true, true, false, false,
                          false, 1U, memory_bytes, true});
  capabilities.push_back({BackendKind::cpu_simd, "host-cpu-simd", "SSE2", "host-cpu", simd, true, false, false, false,
                          1U, memory_bytes, true});
  capabilities.push_back({BackendKind::cpu_multithread, "host-cpu-threads", "std::thread", "host-cpu", threads > 1U,
                          true, false, false, false, threads, memory_bytes, true});
  capabilities.push_back(discover_cuda_capability());
  return capabilities;
}

std::vector<std::shared_ptr<IComputeBackend>> discover_compute_backends() {
  return make_backends(discover_compute_capabilities());
}

core::Result<std::shared_ptr<ComputeRuntime>> ComputeRuntime::create(std::vector<BackendCapabilities> capabilities,
                                                                     std::optional<std::uint32_t> thread_limit) {
  return create(make_backends(std::move(capabilities)), thread_limit);
}

core::Result<std::shared_ptr<ComputeRuntime>>
ComputeRuntime::create(std::vector<std::shared_ptr<IComputeBackend>> backends,
                       std::optional<std::uint32_t> thread_limit) {
  if (backends.empty()) {
    return error(core::ErrorReason::invalid_argument, "至少需要声明一个计算后端");
  }
  if (std::ranges::any_of(backends, [](const auto& backend) { return backend == nullptr; })) {
    return error(core::ErrorReason::invalid_argument, "计算后端实例不得为空");
  }
  std::vector<BackendCapabilities> capabilities;
  capabilities.reserve(backends.size());
  for (const auto& backend : backends) {
    capabilities.push_back(backend->capabilities());
  }
  const auto available =
      std::ranges::count_if(capabilities, [](const BackendCapabilities& capability) { return capability.available; });
  if (available == 0) {
    return error(core::ErrorReason::unavailable, "没有可用计算后端");
  }
  const auto hardware_threads = std::max(1U, std::thread::hardware_concurrency());
  const auto reserved_default = hardware_threads > 1U ? hardware_threads - 1U : 1U;
  const auto workers = thread_limit.value_or(reserved_default);
  if (workers == 0 || workers > hardware_threads) {
    return error(core::ErrorReason::invalid_argument, "线程限制必须位于一至逻辑核数量之间");
  }
  return std::shared_ptr<ComputeRuntime>(new ComputeRuntime(std::move(backends), std::move(capabilities), workers));
}

ComputeRuntime::ComputeRuntime(std::vector<std::shared_ptr<IComputeBackend>> backends,
                               std::vector<BackendCapabilities> capabilities, std::uint32_t worker_threads)
    : backends_(std::move(backends)), capabilities_(std::move(capabilities)), worker_threads_(worker_threads) {}

core::Result<BackendProvenance> ComputeRuntime::select(const Workload& workload) const {
  if (workload.operation.empty() || workload.input_bytes == 0 || workload.working_set_bytes == 0) {
    return error(core::ErrorReason::invalid_argument, "工作负载必须包含操作名和非零数据规模");
  }
  const auto* cuda = find_available(capabilities_, BackendKind::cuda);
  if (workload.require_cuda && workload.preferred_backend.has_value() &&
      workload.preferred_backend.value() != BackendKind::cuda) {
    return error(core::ErrorReason::invalid_argument, "强制 CUDA 与首选非 CUDA 后端互相冲突");
  }
  if (workload.require_cuda) {
    if (cuda == nullptr || !supports_operation(*cuda, workload.operation)) {
      return error(core::ErrorReason::unavailable, "请求的 CUDA 后端不可用", "未执行静默降级");
    }
    return provenance(*cuda, BackendKind::cuda, "调用方明确要求 CUDA", false);
  }
  if (workload.preferred_backend.has_value()) {
    if (const auto* preferred = find_available(capabilities_, workload.preferred_backend.value());
        preferred != nullptr && supports_operation(*preferred, workload.operation) &&
        has_working_set_capacity(*preferred, workload)) {
      return provenance(*preferred, workload.preferred_backend.value(), "调用方声明可降级的首选后端", false);
    }
  }
  const auto requested = workload.preferred_backend.value_or(BackendKind::cpu_scalar);
  const auto preferred_unavailable = workload.preferred_backend.has_value();
  const auto selected_provenance = [&](const BackendCapabilities& capability, std::string reason) {
    if (preferred_unavailable) {
      reason = "首选后端不可用，显式降级：" + std::move(reason);
    }
    return provenance(capability, preferred_unavailable ? requested : capability.kind, std::move(reason),
                      preferred_unavailable);
  };
  const bool cuda_worthwhile = !workload.interactive && workload.operation_count >= 4'000'000U &&
                               workload.working_set_bytes >= 1U * 1024U * 1024U;
  if (cuda != nullptr && supports_operation(*cuda, workload.operation) && cuda_worthwhile &&
      has_working_set_capacity(*cuda, workload)) {
    return selected_provenance(*cuda, "规模超过 GPU 端到端传输阈值");
  }
  if (const auto* threaded = find_available(capabilities_, BackendKind::cpu_multithread);
      threaded != nullptr && supports_operation(*threaded, workload.operation) && worker_threads_ > 1U &&
      workload.operation_count >= 64'000U && has_working_set_capacity(*threaded, workload)) {
    return selected_provenance(*threaded, "CPU 多线程满足当前规模并保留系统逻辑核");
  }
  if (const auto* simd = find_available(capabilities_, BackendKind::cpu_simd);
      simd != nullptr && supports_operation(*simd, workload.operation) && has_working_set_capacity(*simd, workload)) {
    return selected_provenance(*simd, "交互或小规模工作负载优先低传输开销 SIMD");
  }
  if (const auto* scalar = find_available(capabilities_, BackendKind::cpu_scalar);
      scalar != nullptr && supports_operation(*scalar, workload.operation) &&
      has_working_set_capacity(*scalar, workload)) {
    return selected_provenance(*scalar, "使用可用 CPU 标量后端");
  }
  return error(core::ErrorReason::unavailable, "没有满足工作负载操作能力的计算后端", workload.operation);
}

core::Result<BackendProvenance> ComputeRuntime::fallback(const Workload& workload, BackendKind failed_backend,
                                                         std::string_view reason) const {
  if (reason.empty()) {
    return error(core::ErrorReason::invalid_argument, "降级必须记录失败原因");
  }
  if (workload.require_cuda) {
    return error(core::ErrorReason::unavailable, "强制 CUDA 工作负载禁止自动降级", std::string(reason));
  }
  const BackendCapabilities* fallback_backend{};
  for (const auto kind : {BackendKind::cpu_multithread, BackendKind::cpu_simd, BackendKind::cpu_scalar}) {
    const auto* candidate = find_available(capabilities_, kind);
    if (candidate != nullptr && candidate->kind != failed_backend &&
        supports_operation(*candidate, workload.operation) && has_working_set_capacity(*candidate, workload)) {
      fallback_backend = candidate;
      break;
    }
  }
  if (fallback_backend == nullptr) {
    return error(core::ErrorReason::unavailable, "没有可用的独立 CPU 降级后端", std::string(reason));
  }
  return provenance(*fallback_backend, failed_backend, "后端失败后显式降级：" + std::string(reason), true);
}

const IComputeBackend* ComputeRuntime::find_backend(BackendKind kind) const noexcept {
  const auto found = std::ranges::find_if(backends_, [kind](const auto& backend) {
    const auto capability = backend->capabilities();
    return capability.kind == kind && capability.available;
  });
  return found == backends_.end() ? nullptr : found->get();
}

core::Result<ComputeExecution> ComputeRuntime::execute_buffer_copy(const Workload& workload,
                                                                   const BufferCopyRequest& request,
                                                                   double maximum_error_tolerance,
                                                                   double rms_error_tolerance) const {
  if (workload.operation != "buffer-copy") {
    return error(core::ErrorReason::invalid_argument, "缓冲区复制执行要求 operation=buffer-copy");
  }
  if (request.input.empty() || request.input.size() != request.output.size() ||
      request.input.size_bytes() != workload.input_bytes) {
    return error(core::ErrorReason::invalid_argument, "缓冲区复制请求与工作负载数据规模不一致");
  }
  if (ranges_overlap(request)) {
    return error(core::ErrorReason::invalid_argument, "缓冲区复制输入输出不得部分重叠");
  }
  if (std::ranges::any_of(request.input, [](double value) { return !std::isfinite(value); })) {
    return error(core::ErrorReason::invalid_argument, "缓冲区复制一致性验证拒绝非有限输入");
  }
  if (!std::isfinite(maximum_error_tolerance) || !std::isfinite(rms_error_tolerance) || maximum_error_tolerance < 0.0 ||
      rms_error_tolerance < 0.0) {
    return error(core::ErrorReason::invalid_argument, "一致性阈值必须为有限非负数");
  }

  auto selected = select(workload);
  if (!selected) {
    return selected.error();
  }

  auto run = [&](BackendProvenance selected_provenance) -> core::Result<ComputeExecution> {
    const auto* backend = find_backend(selected_provenance.actual);
    if (backend == nullptr || !backend->supports(workload)) {
      return error(core::ErrorReason::unavailable, "已选后端在执行时不可用",
                   std::string(backend_kind_name(selected_provenance.actual)));
    }
    const auto execution = backend->execute_buffer_copy(request, worker_threads_);
    if (!execution) {
      return execution;
    }
    auto metrics = measure_consistency(request.input, request.output);
    if (!metrics) {
      return metrics.error();
    }
    auto verified = verify_consistency(std::move(selected_provenance), metrics.value(), maximum_error_tolerance,
                                       rms_error_tolerance);
    if (!verified) {
      return verified.error();
    }
    return ComputeExecution{std::move(verified.value()), metrics.value()};
  };

  auto result = run(selected.value());
  if (result || workload.require_cuda) {
    return result;
  }
  const auto fallback_provenance =
      fallback(workload, selected.value().actual,
               std::string(result.error().message()) + "；" + std::string(result.error().diagnostic()));
  if (!fallback_provenance) {
    return error(core::ErrorReason::unavailable, "计算后端执行失败且无可用降级",
                 std::string(result.error().message()) + "；" + std::string(result.error().diagnostic()));
  }
  return run(fallback_provenance.value());
}

core::Result<ComputeExecution> ComputeRuntime::execute_operation(const Workload& workload, IComputeOperation& operation,
                                                                 double maximum_error_tolerance,
                                                                 double rms_error_tolerance) const {
  if (workload.operation.empty() || workload.operation != operation.operation()) {
    return error(core::ErrorReason::invalid_argument, "计算操作名必须与工作负载 operation 一致");
  }
  if (!std::isfinite(maximum_error_tolerance) || !std::isfinite(rms_error_tolerance) || maximum_error_tolerance < 0.0 ||
      rms_error_tolerance < 0.0) {
    return error(core::ErrorReason::invalid_argument, "计算操作一致性阈值必须为有限非负数");
  }

  auto selected = select(workload);
  if (!selected) {
    return selected.error();
  }
  auto run = [&](BackendProvenance selected_provenance) -> core::Result<ComputeExecution> {
    if (!operation.supports(selected_provenance.actual)) {
      return error(core::ErrorReason::unavailable, "计算操作适配器不支持已选后端",
                   std::string(backend_kind_name(selected_provenance.actual)));
    }
    const auto execution = operation.execute(selected_provenance.actual, worker_threads_);
    if (!execution) {
      return execution;
    }
    auto metrics = operation.consistency(selected_provenance.actual);
    if (!metrics) {
      return metrics.error();
    }
    if (!metrics.value().reference_verified) {
      selected_provenance.consistency_verified = false;
      selected_provenance.reason += "；操作未提供独立参考结果，一致性未验证";
      return ComputeExecution{std::move(selected_provenance), metrics.value()};
    }
    auto verified = verify_consistency(std::move(selected_provenance), metrics.value(), maximum_error_tolerance,
                                       rms_error_tolerance);
    if (!verified) {
      return verified.error();
    }
    return ComputeExecution{std::move(verified.value()), metrics.value()};
  };

  auto result = run(selected.value());
  if (result || workload.require_cuda) {
    return result;
  }

  const auto originally_requested = selected.value().actual;
  std::vector<BackendKind> excluded{originally_requested};
  std::string failure_reason = std::string(result.error().message()) + "；" + std::string(result.error().diagnostic());
  while (const auto* candidate =
             find_operation_fallback(capabilities_, workload, operation, std::span<const BackendKind>{excluded})) {
    auto fallback_provenance =
        provenance(*candidate, originally_requested, "后端失败后显式降级：" + failure_reason, true);
    result = run(std::move(fallback_provenance));
    if (result) {
      return result;
    }
    excluded.push_back(candidate->kind);
    failure_reason += "；" + std::string(result.error().message()) + "；" + std::string(result.error().diagnostic());
  }
  return error(core::ErrorReason::unavailable, "计算操作执行失败且无可用降级", std::move(failure_reason));
}

std::span<const BackendCapabilities> ComputeRuntime::capabilities() const noexcept {
  return capabilities_;
}

std::uint32_t ComputeRuntime::worker_threads() const noexcept {
  return worker_threads_;
}

struct BudgetedBufferPool::SharedState final {
  SharedState(std::uint64_t budget, std::vector<std::shared_ptr<IMemoryAllocator>> memory_allocators)
      : budget_bytes(budget), allocators(std::move(memory_allocators)) {}
  mutable std::mutex mutex;
  std::uint64_t budget_bytes{};
  std::uint64_t used_bytes{};
  std::vector<std::shared_ptr<IMemoryAllocator>> allocators;
};

struct BufferLease::State final {
  State(std::shared_ptr<BudgetedBufferPool::SharedState> owner_state, std::shared_ptr<IMemoryAllocator> allocator_value,
        void* pointer_value, std::uint64_t size_value, std::uint64_t alignment_value)
      : owner(std::move(owner_state)), allocator(std::move(allocator_value)), pointer(pointer_value), size(size_value),
        alignment(alignment_value), kind(allocator->memory_kind()) {}
  ~State() {
    allocator->release(pointer, size, alignment);
    std::scoped_lock lock(owner->mutex);
    owner->used_bytes -= size;
  }
  std::shared_ptr<BudgetedBufferPool::SharedState> owner;
  std::shared_ptr<IMemoryAllocator> allocator;
  void* pointer{};
  std::uint64_t size{};
  std::uint64_t alignment{};
  MemoryKind kind{MemoryKind::host};
};

class HostMemoryAllocator final : public IMemoryAllocator {
public:
  [[nodiscard]] MemoryKind memory_kind() const noexcept override {
    return MemoryKind::host;
  }
  [[nodiscard]] core::Result<void*> allocate(std::uint64_t bytes, std::uint64_t alignment) override {
    try {
      return ::operator new(static_cast<std::size_t>(bytes), std::align_val_t{static_cast<std::size_t>(alignment)});
    } catch (const std::bad_alloc&) {
      return error(core::ErrorReason::unavailable, "主机内存分配失败");
    }
  }
  void release(void* pointer, std::uint64_t, std::uint64_t alignment) noexcept override {
    ::operator delete(pointer, std::align_val_t{static_cast<std::size_t>(alignment)});
  }
};

BufferLease::BufferLease(std::shared_ptr<State> state) : state_(std::move(state)) {}
BufferLease::BufferLease(BufferLease&&) noexcept = default;
BufferLease& BufferLease::operator=(BufferLease&&) noexcept = default;
BufferLease::~BufferLease() = default;

std::span<std::byte> BufferLease::bytes() noexcept {
  return state_ == nullptr || state_->kind == MemoryKind::device
             ? std::span<std::byte>{}
             : std::span<std::byte>{static_cast<std::byte*>(state_->pointer), static_cast<std::size_t>(state_->size)};
}

std::span<const std::byte> BufferLease::bytes() const noexcept {
  return state_ == nullptr || state_->kind == MemoryKind::device
             ? std::span<const std::byte>{}
             : std::span<const std::byte>{static_cast<const std::byte*>(state_->pointer),
                                          static_cast<std::size_t>(state_->size)};
}

BufferLease::operator bool() const noexcept {
  return state_ != nullptr;
}

MemoryKind BufferLease::memory_kind() const noexcept {
  return state_ == nullptr ? MemoryKind::host : state_->kind;
}

std::uint64_t BufferLease::size_bytes() const noexcept {
  return state_ == nullptr ? 0U : state_->size;
}

core::Result<std::shared_ptr<BudgetedBufferPool>>
BudgetedBufferPool::create(std::uint64_t budget_bytes, std::vector<std::shared_ptr<IMemoryAllocator>> allocators) {
  if (budget_bytes == 0 || budget_bytes > 4ULL * 1024ULL * 1024ULL * 1024ULL) {
    return error(core::ErrorReason::invalid_argument, "内存池预算必须位于 1 字节至 4 GiB");
  }
  if (std::ranges::any_of(allocators, [](const auto& allocator) { return allocator == nullptr; })) {
    return error(core::ErrorReason::invalid_argument, "内存分配器不得为空");
  }
  if (std::ranges::none_of(allocators,
                           [](const auto& allocator) { return allocator->memory_kind() == MemoryKind::host; })) {
    allocators.push_back(std::make_shared<HostMemoryAllocator>());
  }
  std::ranges::sort(allocators, {}, [](const auto& allocator) { return allocator->memory_kind(); });
  if (std::ranges::adjacent_find(allocators, [](const auto& left, const auto& right) {
        return left->memory_kind() == right->memory_kind();
      }) != allocators.end()) {
    return error(core::ErrorReason::invalid_argument, "同一内存类型只能注册一个分配器");
  }
  return std::shared_ptr<BudgetedBufferPool>(
      new BudgetedBufferPool(std::make_shared<SharedState>(budget_bytes, std::move(allocators))));
}

BudgetedBufferPool::BudgetedBufferPool(std::shared_ptr<SharedState> state) : state_(std::move(state)) {}

core::Result<BufferLease> BudgetedBufferPool::acquire(const BufferSpec& spec) {
  if (spec.bytes == 0 || spec.alignment < alignof(std::max_align_t) || (spec.alignment & (spec.alignment - 1U)) != 0U ||
      spec.alignment > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return error(core::ErrorReason::invalid_argument, "缓冲区大小必须非零，对齐须为不小于 max_align_t 的二次幂");
  }
  const auto allocator = std::ranges::find_if(
      state_->allocators, [&spec](const auto& candidate) { return candidate->memory_kind() == spec.memory_kind; });
  if (allocator == state_->allocators.end()) {
    return error(core::ErrorReason::unavailable, "请求的内存类型没有注册后端分配器");
  }
  if (spec.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return error(core::ErrorReason::unavailable, "缓冲区大小超过进程地址空间");
  }
  {
    std::scoped_lock lock(state_->mutex);
    if (state_->used_bytes > state_->budget_bytes || spec.bytes > state_->budget_bytes - state_->used_bytes) {
      return error(core::ErrorReason::unavailable, "缓冲区申请超过预算");
    }
    state_->used_bytes += spec.bytes;
  }

  auto allocation = (*allocator)->allocate(spec.bytes, spec.alignment);
  if (!allocation) {
    std::scoped_lock lock(state_->mutex);
    state_->used_bytes -= spec.bytes;
    return allocation.error();
  }
  try {
    return BufferLease(
        std::make_shared<BufferLease::State>(state_, *allocator, allocation.value(), spec.bytes, spec.alignment));
  } catch (const std::bad_alloc&) {
    (*allocator)->release(allocation.value(), spec.bytes, spec.alignment);
    std::scoped_lock lock(state_->mutex);
    state_->used_bytes -= spec.bytes;
    return error(core::ErrorReason::unavailable, "缓冲区租约元数据分配失败");
  } catch (...) {
    (*allocator)->release(allocation.value(), spec.bytes, spec.alignment);
    std::scoped_lock lock(state_->mutex);
    state_->used_bytes -= spec.bytes;
    return error(core::ErrorReason::internal_failure, "缓冲区租约创建失败");
  }
}

std::uint64_t BudgetedBufferPool::budget_bytes() const noexcept {
  return state_->budget_bytes;
}

std::uint64_t BudgetedBufferPool::used_bytes() const noexcept {
  std::scoped_lock lock(state_->mutex);
  return state_->used_bytes;
}

core::Result<PerformanceSummary> summarize_performance(std::span<const PerformanceSample> samples) {
  if (samples.size() < 30U) {
    return error(core::ErrorReason::invalid_argument, "性能汇总至少需要 30 个样本");
  }
  std::vector<std::chrono::microseconds> latencies;
  latencies.reserve(samples.size());
  std::uint64_t bytes{};
  std::chrono::microseconds elapsed{};
  for (const auto& sample : samples) {
    if (sample.latency.count() <= 0) {
      return error(core::ErrorReason::invalid_argument, "性能样本延迟必须为正");
    }
    latencies.push_back(sample.latency);
    if (sample.bytes > std::numeric_limits<std::uint64_t>::max() - bytes ||
        sample.latency.count() > std::numeric_limits<std::int64_t>::max() - elapsed.count()) {
      return error(core::ErrorReason::invalid_argument, "性能样本汇总溢出");
    }
    bytes += sample.bytes;
    elapsed += sample.latency;
  }
  std::ranges::sort(latencies);
  const auto index = [count = latencies.size()](double percentile) {
    return std::min(count - 1U, static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(count))) - 1U);
  };
  const auto seconds = static_cast<double>(elapsed.count()) / 1'000'000.0;
  return PerformanceSummary{latencies[index(0.50)], latencies[index(0.95)], latencies.back(),
                            seconds > 0.0 ? static_cast<double>(bytes) / seconds : 0.0, samples.size()};
}

core::Result<LargeFileReadPlan> make_large_file_read_plan(std::uint64_t file_bytes, std::uint64_t frame_bytes,
                                                          std::uint64_t memory_budget_bytes,
                                                          std::optional<std::uint64_t> scanned_frames) {
  if (file_bytes == 0 || frame_bytes == 0 || memory_budget_bytes < frame_bytes) {
    return error(core::ErrorReason::invalid_argument, "读取计划参数无效");
  }
  const auto estimated = scanned_frames.value_or(file_bytes / frame_bytes);
  const auto working_set = std::min(memory_budget_bytes, 64ULL * 1024ULL * 1024ULL);
  return LargeFileReadPlan{file_bytes,  frame_bytes,
                           estimated,   std::max<std::uint64_t>(1U, working_set / frame_bytes),
                           working_set, scanned_frames.has_value() ? "完整索引扫描" : "文件字节数除以帧字节数",
                           true,        true};
}

struct ViewActivityLease::State final {
  mutable std::mutex mutex;
  std::condition_variable drained;
  bool visible{true};
  std::uint64_t generation{};
  std::uint64_t active_leases{};
  std::uint64_t shared_activity_count{};
  std::uint64_t view_activity_count{};
};

ViewActivityLease::ViewActivityLease(std::shared_ptr<State> state, std::uint64_t generation) noexcept
    : state_(std::move(state)), generation_(generation), registered_(true) {}

ViewActivityLease::ViewActivityLease(ViewActivityLease&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_), registered_(other.registered_) {
  other.registered_ = false;
}

ViewActivityLease& ViewActivityLease::operator=(ViewActivityLease&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
    generation_ = other.generation_;
    registered_ = other.registered_;
    other.registered_ = false;
  }
  return *this;
}

ViewActivityLease::~ViewActivityLease() {
  release();
}

void ViewActivityLease::release() noexcept {
  if (!registered_ || state_ == nullptr) {
    return;
  }
  {
    std::scoped_lock lock(state_->mutex);
    if (state_->active_leases > 0U) {
      --state_->active_leases;
    }
    registered_ = false;
  }
  state_->drained.notify_all();
}

bool ViewActivityLease::active() const noexcept {
  if (!registered_ || state_ == nullptr) {
    return false;
  }
  std::scoped_lock lock(state_->mutex);
  return state_->visible && state_->generation == generation_;
}

bool ViewActivityLease::mark_activity() noexcept {
  if (!registered_ || state_ == nullptr) {
    return false;
  }
  std::scoped_lock lock(state_->mutex);
  if (!state_->visible || state_->generation != generation_) {
    return false;
  }
  ++state_->view_activity_count;
  return true;
}

ViewActivityLease::operator bool() const noexcept {
  return registered_ && state_ != nullptr;
}

ViewActivityGate::ViewActivityGate() : state_(std::make_shared<ViewActivityLease::State>()) {}
ViewActivityGate::~ViewActivityGate() = default;

std::optional<ViewActivityLease> ViewActivityGate::acquire_view_activity() noexcept {
  try {
    std::scoped_lock lock(state_->mutex);
    if (!state_->visible) {
      return std::nullopt;
    }
    ++state_->active_leases;
    return ViewActivityLease{state_, state_->generation};
  } catch (...) {
    return std::nullopt;
  }
}

void ViewActivityGate::mark_shared_activity() noexcept {
  std::scoped_lock lock(state_->mutex);
  ++state_->shared_activity_count;
}

void ViewActivityGate::mark_view_activity() noexcept {
  std::scoped_lock lock(state_->mutex);
  if (state_->visible) {
    ++state_->view_activity_count;
  }
}

void ViewActivityGate::set_visible(bool visible_value) noexcept {
  std::scoped_lock lock(state_->mutex);
  if (state_->visible == visible_value) {
    return;
  }
  state_->visible = visible_value;
  ++state_->generation;
}

bool ViewActivityGate::wait_for_view_idle(std::chrono::milliseconds timeout) noexcept {
  if (timeout.count() < 0) {
    return false;
  }
  std::unique_lock lock(state_->mutex);
  return state_->drained.wait_for(lock, timeout, [this] { return state_->active_leases == 0U; });
}

bool ViewActivityGate::visible() const noexcept {
  std::scoped_lock lock(state_->mutex);
  return state_->visible;
}

std::uint64_t ViewActivityGate::shared_activity_count() const noexcept {
  std::scoped_lock lock(state_->mutex);
  return state_->shared_activity_count;
}

std::uint64_t ViewActivityGate::view_activity_count() const noexcept {
  std::scoped_lock lock(state_->mutex);
  return state_->view_activity_count;
}

bool ViewActivityGate::accepts_view_activity() const noexcept {
  return visible();
}

core::Result<ConsistencyMetrics> measure_consistency(std::span<const double> reference,
                                                     std::span<const double> actual) {
  if (reference.empty() || reference.size() != actual.size()) {
    return error(core::ErrorReason::invalid_argument, "一致性比较要求非空且长度相同");
  }
  double maximum{};
  long double squared_error_sum{};
  for (std::size_t index = 0; index < reference.size(); ++index) {
    if (!std::isfinite(reference[index]) || !std::isfinite(actual[index])) {
      return error(core::ErrorReason::invalid_argument, "一致性比较拒绝非有限数值");
    }
    const auto difference = std::abs(reference[index] - actual[index]);
    maximum = std::max(maximum, difference);
    squared_error_sum += static_cast<long double>(difference) * static_cast<long double>(difference);
  }
  const auto rms = std::sqrt(static_cast<double>(squared_error_sum / static_cast<long double>(reference.size())));
  return ConsistencyMetrics{maximum, rms, reference.size(), true};
}

core::Result<BackendProvenance> verify_consistency(BackendProvenance provenance_value, double maximum_error,
                                                   double tolerance) {
  return verify_consistency(std::move(provenance_value), ConsistencyMetrics{maximum_error, maximum_error, 1U, true},
                            tolerance, tolerance);
}

core::Result<BackendProvenance> verify_consistency(BackendProvenance provenance_value,
                                                   const ConsistencyMetrics& metrics, double maximum_error_tolerance,
                                                   double rms_error_tolerance) {
  if (metrics.value_count == 0U || !std::isfinite(metrics.maximum_absolute_error) ||
      !std::isfinite(metrics.rms_error) || !std::isfinite(maximum_error_tolerance) ||
      !std::isfinite(rms_error_tolerance) || metrics.maximum_absolute_error < 0.0 || metrics.rms_error < 0.0 ||
      maximum_error_tolerance < 0.0 || rms_error_tolerance < 0.0) {
    return error(core::ErrorReason::invalid_argument, "一致性误差、样本数与阈值无效");
  }
  if (!metrics.reference_verified) {
    return error(core::ErrorReason::invalid_argument, "一致性指标没有独立参考结果，不能标记为已验证");
  }
  if (metrics.maximum_absolute_error > maximum_error_tolerance || metrics.rms_error > rms_error_tolerance) {
    return error(core::ErrorReason::internal_failure, "计算后端结果一致性校验失败",
                 "最大绝对误差=" + std::to_string(metrics.maximum_absolute_error) + "，最大阈值=" +
                     std::to_string(maximum_error_tolerance) + "，RMS 误差=" + std::to_string(metrics.rms_error) +
                     "，RMS 阈值=" + std::to_string(rms_error_tolerance));
  }
  provenance_value.consistency_verified = true;
  provenance_value.reason += "；结果一致性已按最大绝对误差和 RMS 误差验证";
  return provenance_value;
}

std::string_view backend_kind_name(BackendKind kind) noexcept {
  switch (kind) {
  case BackendKind::cpu_scalar:
    return "cpu-scalar";
  case BackendKind::cpu_simd:
    return "cpu-simd";
  case BackendKind::cpu_multithread:
    return "cpu-multithread";
  case BackendKind::cuda:
    return "cuda";
  }
  return "unknown";
}

} // namespace signal::compute
