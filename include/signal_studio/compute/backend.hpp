#pragma once

#include "signal_studio/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace signal::compute {

/// Physical execution device category. Mirrors ADR-009 (CPU/SIMD/CUDA via SignalCompute).
enum class ComputeDeviceType : std::uint8_t { cpu = 0, cuda = 1 };

/// Coarse workload classification used by the backend selector. Maps to the task
/// runtime WorkClass families that drive scheduling decisions.
enum class WorkloadClass : std::uint8_t { io_bound, dsp_bound, indexing, inference, other };

/// Capability snapshot reported by a backend. Public API (API-COMPUTE-001): no third-party
/// types leak; CUDA device names are copied into std::string.
struct ComputeCapabilities final {
  ComputeDeviceType device{ComputeDeviceType::cpu};
  bool available{true};
  bool simd{true};
  std::uint32_t compute_units{};
  std::uint64_t memory_bytes{};
  std::string device_name;
  std::string runtime_version;
  friend bool operator==(const ComputeCapabilities&, const ComputeCapabilities&) = default;
};

/// Provenance recorded alongside every compute result so downstream caches and reports can
/// audit which backend actually produced a value (ADR-009).
struct BackendProvenance final {
  ComputeDeviceType device{ComputeDeviceType::cpu};
  std::string backend_id;
  std::string runtime_version;
  std::string device_name;
  friend bool operator==(const BackendProvenance&, const BackendProvenance&) = default;
};

class IComputeBackend {
 public:
  virtual ~IComputeBackend() = default;
  [[nodiscard]] virtual ComputeDeviceType device_type() const noexcept = 0;
  [[nodiscard]] virtual ComputeCapabilities capabilities() const = 0;
  [[nodiscard]] virtual BackendProvenance provenance() const = 0;
};

/// CPU backend: always available; reports the host SIMD/runtime detected at configure time.
class CpuComputeBackend final : public IComputeBackend {
 public:
  CpuComputeBackend() = default;
  [[nodiscard]] ComputeDeviceType device_type() const noexcept override;
  [[nodiscard]] ComputeCapabilities capabilities() const override;
  [[nodiscard]] BackendProvenance provenance() const override;
};

/// CUDA backend: probes the CUDA runtime at construction. When CUDA is not available at
/// build time this backend is not registered; when built but no device/driver is present,
/// construction returns a failure status instead of throwing.
[[nodiscard]] core::Result<std::unique_ptr<IComputeBackend>> make_cuda_compute_backend();

/// Buffer acquisition request (API-COMPUTE-002). Memory is budget-constrained: a pool rejects
/// acquisitions that would exceed its configured byte budget rather than unbounded growth.
struct BufferSpec final {
  std::size_t element_count{};
  std::size_t element_bytes{};
  ComputeDeviceType device{ComputeDeviceType::cpu};
  [[nodiscard]] std::size_t total_bytes() const noexcept;
  friend bool operator==(const BufferSpec&, const BufferSpec&) = default;
};

/// Owning handle for a pooled buffer. Memory is host-accessible for CPU specs; CUDA specs are
/// device-resident and only accessed through the pool that issued them.
class BufferHandle final {
 public:
  BufferHandle() = default;
  BufferHandle(std::size_t bytes, ComputeDeviceType device);
  ~BufferHandle();
  BufferHandle(const BufferHandle&) = delete;
  BufferHandle& operator=(const BufferHandle&) = delete;
  BufferHandle(BufferHandle&&) noexcept;
  BufferHandle& operator=(BufferHandle&&) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::size_t size_bytes() const noexcept;
  [[nodiscard]] ComputeDeviceType device() const noexcept;
  [[nodiscard]] void* data() noexcept;
  [[nodiscard]] const void* data() const noexcept;

 private:
  std::size_t bytes_{};
  ComputeDeviceType device_{ComputeDeviceType::cpu};
  void* storage_{nullptr};
};

class IBufferPool {
 public:
  virtual ~IBufferPool() = default;
  [[nodiscard]] virtual core::Result<BufferHandle> acquire(const BufferSpec& spec) = 0;
  [[nodiscard]] virtual std::uint64_t acquired_bytes() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t budget_bytes() const noexcept = 0;
};

/// Bounded host buffer pool. Tracks live bytes and rejects acquisitions exceeding the budget,
/// giving callers a deterministic back-pressure signal instead of OOM.
class BoundedBufferPool final : public IBufferPool {
 public:
  explicit BoundedBufferPool(std::uint64_t budget_bytes);
  [[nodiscard]] core::Result<BufferHandle> acquire(const BufferSpec& spec) override;
  [[nodiscard]] std::uint64_t acquired_bytes() const noexcept override;
  [[nodiscard]] std::uint64_t budget_bytes() const noexcept override;

 private:
  std::uint64_t budget_bytes_;
  std::uint64_t acquired_bytes_{};
};

/// Workload hint consumed by the selector (API-COMPUTE-003).
struct Workload final {
  WorkloadClass work_class{WorkloadClass::dsp_bound};
  std::uint64_t element_count{};
  bool benefits_from_gpu{true};
  bool requires_gpu{false};
  friend bool operator==(const Workload&, const Workload&) = default;
};

class IBackendSelector {
 public:
  virtual ~IBackendSelector() = default;
  /// Returns the selected device and the provenance of the backend that will execute. When the
  /// requested device is unavailable, selection falls back to CPU and records the degradation.
  [[nodiscard]] virtual core::Result<std::pair<ComputeDeviceType, BackendProvenance>>
  select(const Workload& workload) const = 0;
};

/// Auto selector: prefers CUDA for GPU-friendly DSP workloads when a CUDA backend is registered,
/// otherwise CPU. CPU fallback is always available (ADR-009 explicit degradation).
class AutoBackendSelector final : public IBackendSelector {
 public:
  AutoBackendSelector();
  /// Registers a backend. The selector does not take ownership of CUDA device memory; it only
  /// probes capabilities. Passing nullptr clears the CUDA path.
  void register_backend(std::unique_ptr<IComputeBackend> backend);
  [[nodiscard]] bool has_cuda() const noexcept;
  [[nodiscard]] core::Result<std::pair<ComputeDeviceType, BackendProvenance>>
  select(const Workload& workload) const override;

 private:
  std::unique_ptr<IComputeBackend> cpu_backend_;
  std::unique_ptr<IComputeBackend> cuda_backend_;
};

[[nodiscard]] std::string_view to_string(ComputeDeviceType device) noexcept;
[[nodiscard]] std::string_view to_string(WorkloadClass work_class) noexcept;

}  // namespace signal::compute
