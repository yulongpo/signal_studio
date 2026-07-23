#include "signal_studio/compute/backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>

#if defined(__CUDACC__) || defined(SIGNAL_STUDIO_HAVE_CUDA)
#define SIGNAL_STUDIO_CUDA_ENABLED 1
#endif

#if SIGNAL_STUDIO_CUDA_ENABLED
#include <cuda_runtime_api.h>
#endif

namespace signal::compute {

std::size_t BufferSpec::total_bytes() const noexcept {
  return element_count * element_bytes;
}

ComputeDeviceType CpuComputeBackend::device_type() const noexcept {
  return ComputeDeviceType::cpu;
}

ComputeCapabilities CpuComputeBackend::capabilities() const {
  ComputeCapabilities caps;
  caps.device = ComputeDeviceType::cpu;
  caps.available = true;
  caps.simd = true;
  caps.compute_units = static_cast<std::uint32_t>(std::max(1u, std::thread::hardware_concurrency()));
  caps.device_name = "host-cpu";
  caps.runtime_version = "msvc-host";
  return caps;
}

BackendProvenance CpuComputeBackend::provenance() const {
  BackendProvenance p;
  p.device = ComputeDeviceType::cpu;
  p.backend_id = "compute.cpu";
  p.runtime_version = "msvc-host";
  p.device_name = "host-cpu";
  return p;
}

BufferHandle::BufferHandle(std::size_t bytes, ComputeDeviceType device) : bytes_(bytes), device_(device) {
  if (bytes_ == 0) {
    return;
  }
#if SIGNAL_STUDIO_CUDA_ENABLED
  if (device_ == ComputeDeviceType::cuda) {
    void* ptr = nullptr;
    if (cudaMalloc(&ptr, bytes_) == cudaSuccess) {
      storage_ = ptr;
      return;
    }
    storage_ = nullptr;
    return;
  }
#else
  (void)device_;
#endif
  storage_ = std::malloc(bytes_);
}

BufferHandle::~BufferHandle() {
  if (!storage_) {
    return;
  }
#if SIGNAL_STUDIO_CUDA_ENABLED
  if (device_ == ComputeDeviceType::cuda) {
    cudaFree(storage_);
    storage_ = nullptr;
    return;
  }
#endif
  std::free(storage_);
  storage_ = nullptr;
}

BufferHandle::BufferHandle(BufferHandle&& other) noexcept
    : bytes_(other.bytes_), device_(other.device_), storage_(other.storage_) {
  other.bytes_ = 0;
  other.storage_ = nullptr;
}

BufferHandle& BufferHandle::operator=(BufferHandle&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  this->~BufferHandle();
  bytes_ = other.bytes_;
  device_ = other.device_;
  storage_ = other.storage_;
  other.bytes_ = 0;
  other.storage_ = nullptr;
  return *this;
}

bool BufferHandle::valid() const noexcept {
  return storage_ != nullptr;
}

std::size_t BufferHandle::size_bytes() const noexcept {
  return bytes_;
}

ComputeDeviceType BufferHandle::device() const noexcept {
  return device_;
}

void* BufferHandle::data() noexcept {
  return storage_;
}

const void* BufferHandle::data() const noexcept {
  return storage_;
}

BoundedBufferPool::BoundedBufferPool(std::uint64_t budget_bytes) : budget_bytes_(budget_bytes) {}

core::Result<BufferHandle> BoundedBufferPool::acquire(const BufferSpec& spec) {
  if (spec.element_count == 0 || spec.element_bytes == 0) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::compute, core::ErrorReason::invalid_argument},
                                 "buffer spec must request at least one element");
  }
  const std::size_t bytes = spec.total_bytes();
  if (bytes > budget_bytes_) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::compute, core::ErrorReason::unavailable},
                                 "requested buffer exceeds pool budget");
  }
  if (acquired_bytes_ + bytes > budget_bytes_) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::compute, core::ErrorReason::unavailable},
                                 "buffer pool budget exhausted");
  }
  BufferHandle handle(bytes, spec.device);
  if (!handle.valid()) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::compute, core::ErrorReason::internal_failure},
                                 "failed to allocate buffer");
  }
  acquired_bytes_ += bytes;
  return handle;
}

std::uint64_t BoundedBufferPool::acquired_bytes() const noexcept {
  return acquired_bytes_;
}

std::uint64_t BoundedBufferPool::budget_bytes() const noexcept {
  return budget_bytes_;
}

AutoBackendSelector::AutoBackendSelector() : cpu_backend_(std::make_unique<CpuComputeBackend>()) {}

void AutoBackendSelector::register_backend(std::unique_ptr<IComputeBackend> backend) {
  if (!backend || backend->device_type() != ComputeDeviceType::cuda) {
    return;
  }
  cuda_backend_ = std::move(backend);
}

bool AutoBackendSelector::has_cuda() const noexcept {
  return cuda_backend_ != nullptr;
}

core::Result<std::pair<ComputeDeviceType, BackendProvenance>>
AutoBackendSelector::select(const Workload& workload) const {
  if (workload.requires_gpu && !has_cuda()) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::compute, core::ErrorReason::unavailable},
                                 "GPU required but no CUDA backend is registered");
  }
  if (has_cuda() && workload.benefits_from_gpu && workload.work_class != WorkloadClass::io_bound) {
    return std::make_pair(ComputeDeviceType::cuda, cuda_backend_->provenance());
  }
  return std::make_pair(ComputeDeviceType::cpu, cpu_backend_->provenance());
}

core::Result<std::unique_ptr<IComputeBackend>> make_cuda_compute_backend() {
#if SIGNAL_STUDIO_CUDA_ENABLED
  int device_count = 0;
  cudaError_t err = cudaGetDeviceCount(&device_count);
  if (err != cudaSuccess || device_count <= 0) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::compute, core::ErrorReason::unavailable},
                                 "no CUDA-capable device available");
  }
  // Validate that a context can be created on device 0.
  cudaDeviceProp prop{};
  if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::compute, core::ErrorReason::unavailable},
                                 "failed to query CUDA device properties");
  }
  int runtime_version = 0;
  cudaRuntimeGetVersion(&runtime_version);
  class CudaBackend final : public IComputeBackend {
  public:
    explicit CudaBackend(cudaDeviceProp prop, int runtime_version) : prop_(prop), runtime_version_(runtime_version) {}
    ComputeDeviceType device_type() const noexcept override {
      return ComputeDeviceType::cuda;
    }
    ComputeCapabilities capabilities() const override {
      ComputeCapabilities caps;
      caps.device = ComputeDeviceType::cuda;
      caps.available = true;
      caps.simd = false;
      caps.compute_units = static_cast<std::uint32_t>(prop_.multiProcessorCount);
      caps.memory_bytes = static_cast<std::uint64_t>(prop_.totalGlobalMem);
      caps.device_name = prop_.name;
      caps.runtime_version = "cuda-runtime-" + std::to_string(runtime_version_);
      return caps;
    }
    BackendProvenance provenance() const override {
      BackendProvenance p;
      p.device = ComputeDeviceType::cuda;
      p.backend_id = "compute.cuda";
      p.runtime_version = "cuda-runtime-" + std::to_string(runtime_version_);
      p.device_name = prop_.name;
      return p;
    }

  private:
    cudaDeviceProp prop_{};
    int runtime_version_{};
  };
  return std::unique_ptr<IComputeBackend>(std::make_unique<CudaBackend>(prop, runtime_version));
#else
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::compute, core::ErrorReason::unavailable},
                               "CUDA backend was not built into this configuration");
#endif
}

std::string_view to_string(ComputeDeviceType device) noexcept {
  switch (device) {
  case ComputeDeviceType::cpu:
    return "cpu";
  case ComputeDeviceType::cuda:
    return "cuda";
  }
  return "unknown";
}

std::string_view to_string(WorkloadClass work_class) noexcept {
  switch (work_class) {
  case WorkloadClass::io_bound:
    return "io-bound";
  case WorkloadClass::dsp_bound:
    return "dsp-bound";
  case WorkloadClass::indexing:
    return "indexing";
  case WorkloadClass::inference:
    return "inference";
  case WorkloadClass::other:
    return "other";
  }
  return "unknown";
}

} // namespace signal::compute
