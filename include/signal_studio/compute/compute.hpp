#pragma once

#include "signal_studio/core/result.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::compute {

enum class BackendKind : std::uint8_t { cpu_scalar, cpu_simd, cpu_multithread, cuda };
enum class MemoryKind : std::uint8_t { host, pinned_host, device };

struct BackendCapabilities final {
  BackendKind kind{BackendKind::cpu_scalar};
  std::string backend_id;
  std::string version;
  std::string device;
  bool available{};
  bool supports_buffer_copy{};
  bool supports_fft{};
  bool supports_convolution{};
  bool supports_linear_solve{};
  std::uint32_t logical_threads{1};
  std::uint64_t memory_bytes{};
  bool compiled{true};
};

struct Workload final {
  std::string operation;
  std::uint64_t input_bytes{};
  std::uint64_t working_set_bytes{};
  std::uint64_t operation_count{};
  bool interactive{};
  bool require_cuda{};
  bool deterministic{true};
  std::optional<BackendKind> preferred_backend;
};

struct BackendProvenance final {
  BackendKind requested{BackendKind::cpu_scalar};
  BackendKind actual{BackendKind::cpu_scalar};
  std::string backend_id;
  std::string version;
  std::string device;
  std::string reason;
  bool degraded{};
  bool consistency_verified{};
  /// Numeric representation used by the executed operation, or "unspecified" when the operation has no such contract.
  std::string precision{"unspecified"};
  friend bool operator==(const BackendProvenance&, const BackendProvenance&) = default;
};

struct BufferCopyRequest final {
  std::span<const double> input;
  std::span<double> output;
};

struct ConsistencyMetrics final {
  double maximum_absolute_error{};
  double rms_error{};
  std::size_t value_count{};
  /// 仅当指标由被测结果与独立参考结果逐值比较得到时为 true。
  bool reference_verified{};
};

struct ComputeExecution final {
  BackendProvenance provenance;
  ConsistencyMetrics consistency;
};

class IComputeOperation {
public:
  virtual ~IComputeOperation() = default;
  [[nodiscard]] virtual std::string_view operation() const noexcept = 0;
  [[nodiscard]] virtual bool supports(BackendKind kind) const noexcept = 0;
  [[nodiscard]] virtual core::Status execute(BackendKind kind, std::uint32_t worker_threads) = 0;
  [[nodiscard]] virtual core::Result<ConsistencyMetrics> consistency(BackendKind kind) const = 0;
};

class IComputeBackend {
public:
  virtual ~IComputeBackend() = default;
  [[nodiscard]] virtual BackendCapabilities capabilities() const noexcept = 0;
  [[nodiscard]] virtual bool supports(const Workload& workload) const noexcept = 0;
  [[nodiscard]] virtual core::Status execute_buffer_copy(const BufferCopyRequest& request,
                                                         std::uint32_t worker_threads) const = 0;
};

[[nodiscard]] std::vector<std::shared_ptr<IComputeBackend>> discover_compute_backends();

class IBackendSelector {
public:
  virtual ~IBackendSelector() = default;
  [[nodiscard]] virtual core::Result<BackendProvenance> select(const Workload& workload) const = 0;
  [[nodiscard]] virtual core::Result<BackendProvenance> fallback(const Workload& workload, BackendKind failed_backend,
                                                                 std::string_view reason) const = 0;
};

class ComputeRuntime final : public IBackendSelector {
public:
  [[nodiscard]] static core::Result<std::shared_ptr<ComputeRuntime>>
  create(std::vector<BackendCapabilities> capabilities, std::optional<std::uint32_t> thread_limit = std::nullopt);
  [[nodiscard]] static core::Result<std::shared_ptr<ComputeRuntime>>
  create(std::vector<std::shared_ptr<IComputeBackend>> backends,
         std::optional<std::uint32_t> thread_limit = std::nullopt);
  [[nodiscard]] core::Result<BackendProvenance> select(const Workload& workload) const override;
  [[nodiscard]] core::Result<BackendProvenance> fallback(const Workload& workload, BackendKind failed_backend,
                                                         std::string_view reason) const override;
  [[nodiscard]] core::Result<ComputeExecution> execute_buffer_copy(const Workload& workload,
                                                                   const BufferCopyRequest& request,
                                                                   double maximum_error_tolerance = 0.0,
                                                                   double rms_error_tolerance = 0.0) const;
  [[nodiscard]] core::Result<ComputeExecution> execute_operation(const Workload& workload, IComputeOperation& operation,
                                                                 double maximum_error_tolerance,
                                                                 double rms_error_tolerance) const;
  [[nodiscard]] std::span<const BackendCapabilities> capabilities() const noexcept;
  [[nodiscard]] std::uint32_t worker_threads() const noexcept;

private:
  ComputeRuntime(std::vector<std::shared_ptr<IComputeBackend>> backends, std::vector<BackendCapabilities> capabilities,
                 std::uint32_t worker_threads);
  [[nodiscard]] const IComputeBackend* find_backend(BackendKind kind) const noexcept;
  std::vector<std::shared_ptr<IComputeBackend>> backends_;
  std::vector<BackendCapabilities> capabilities_;
  std::uint32_t worker_threads_{1};
};

struct BufferSpec final {
  std::uint64_t bytes{};
  std::uint64_t alignment{64};
  MemoryKind memory_kind{MemoryKind::host};
};

class IMemoryAllocator {
public:
  virtual ~IMemoryAllocator() = default;
  [[nodiscard]] virtual MemoryKind memory_kind() const noexcept = 0;
  [[nodiscard]] virtual core::Result<void*> allocate(std::uint64_t bytes, std::uint64_t alignment) = 0;
  virtual void release(void* pointer, std::uint64_t bytes, std::uint64_t alignment) noexcept = 0;
};

[[nodiscard]] core::Result<std::vector<std::shared_ptr<IMemoryAllocator>>> make_cuda_memory_allocators();
[[nodiscard]] std::vector<BackendCapabilities> discover_compute_capabilities();

class BufferLease final {
public:
  BufferLease() noexcept = default;
  BufferLease(BufferLease&&) noexcept;
  BufferLease& operator=(BufferLease&&) noexcept;
  BufferLease(const BufferLease&) = delete;
  BufferLease& operator=(const BufferLease&) = delete;
  ~BufferLease();
  [[nodiscard]] std::span<std::byte> bytes() noexcept;
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] MemoryKind memory_kind() const noexcept;
  [[nodiscard]] std::uint64_t size_bytes() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  struct State;
  friend class BudgetedBufferPool;
  explicit BufferLease(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

class BudgetedBufferPool final {
public:
  [[nodiscard]] static core::Result<std::shared_ptr<BudgetedBufferPool>>
  create(std::uint64_t budget_bytes, std::vector<std::shared_ptr<IMemoryAllocator>> allocators = {});
  [[nodiscard]] core::Result<BufferLease> acquire(const BufferSpec& spec);
  [[nodiscard]] std::uint64_t budget_bytes() const noexcept;
  [[nodiscard]] std::uint64_t used_bytes() const noexcept;

private:
  friend class BufferLease;
  struct SharedState;
  explicit BudgetedBufferPool(std::shared_ptr<SharedState> state);
  std::shared_ptr<SharedState> state_;
};

struct PerformanceSample final {
  std::chrono::microseconds latency{};
  std::uint64_t bytes{};
};

struct PerformanceSummary final {
  std::chrono::microseconds p50{};
  std::chrono::microseconds p95{};
  std::chrono::microseconds maximum{};
  double throughput_bytes_per_second{};
  std::size_t sample_count{};
};

[[nodiscard]] core::Result<PerformanceSummary> summarize_performance(std::span<const PerformanceSample> samples);

struct LargeFileReadPlan final {
  std::uint64_t file_bytes{};
  std::uint64_t frame_bytes{};
  std::uint64_t estimated_frames{};
  std::uint64_t initial_window_frames{};
  std::uint64_t bounded_working_set_bytes{};
  std::string estimate_source;
  bool navigation_ready{};
  bool sample_overview{};
};

[[nodiscard]] core::Result<LargeFileReadPlan>
make_large_file_read_plan(std::uint64_t file_bytes, std::uint64_t frame_bytes, std::uint64_t memory_budget_bytes,
                          std::optional<std::uint64_t> scanned_frames = std::nullopt);

class ViewActivityGate;

class ViewActivityLease final {
public:
  ViewActivityLease() noexcept = default;
  ViewActivityLease(ViewActivityLease&& other) noexcept;
  ViewActivityLease& operator=(ViewActivityLease&& other) noexcept;
  ViewActivityLease(const ViewActivityLease&) = delete;
  ViewActivityLease& operator=(const ViewActivityLease&) = delete;
  ~ViewActivityLease();
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool mark_activity() noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  struct State;
  friend class ViewActivityGate;
  ViewActivityLease(std::shared_ptr<State> state, std::uint64_t generation) noexcept;
  void release() noexcept;
  std::shared_ptr<State> state_;
  std::uint64_t generation_{};
  bool registered_{};
};

class ViewActivityGate final {
public:
  ViewActivityGate();
  ~ViewActivityGate();
  ViewActivityGate(const ViewActivityGate&) = delete;
  ViewActivityGate& operator=(const ViewActivityGate&) = delete;
  [[nodiscard]] std::optional<ViewActivityLease> acquire_view_activity() noexcept;
  void mark_shared_activity() noexcept;
  void mark_view_activity() noexcept;
  void set_visible(bool visible) noexcept;
  [[nodiscard]] bool wait_for_view_idle(std::chrono::milliseconds timeout) noexcept;
  [[nodiscard]] bool visible() const noexcept;
  [[nodiscard]] std::uint64_t shared_activity_count() const noexcept;
  [[nodiscard]] std::uint64_t view_activity_count() const noexcept;
  [[nodiscard]] bool accepts_view_activity() const noexcept;

private:
  std::shared_ptr<ViewActivityLease::State> state_;
};

[[nodiscard]] core::Result<ConsistencyMetrics> measure_consistency(std::span<const double> reference,
                                                                   std::span<const double> actual);
[[nodiscard]] core::Result<BackendProvenance> verify_consistency(BackendProvenance provenance, double maximum_error,
                                                                 double tolerance);
[[nodiscard]] core::Result<BackendProvenance> verify_consistency(BackendProvenance provenance,
                                                                 const ConsistencyMetrics& metrics,
                                                                 double maximum_error_tolerance,
                                                                 double rms_error_tolerance);
[[nodiscard]] std::string_view backend_kind_name(BackendKind kind) noexcept;

} // namespace signal::compute
