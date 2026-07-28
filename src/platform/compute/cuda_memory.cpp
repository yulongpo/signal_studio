#include "signal_studio/compute/compute.hpp"

#if defined(SIGNAL_STUDIO_HAVE_CUDA_MEMORY)
#include <cuda_runtime_api.h>
#endif

namespace signal::compute {
namespace {

[[nodiscard]] core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::compute, reason}, std::move(message), std::move(diagnostic));
}

#if defined(SIGNAL_STUDIO_HAVE_CUDA_MEMORY)

class CudaDeviceScope final {
public:
  explicit CudaDeviceScope(int target_device) noexcept {
    status_ = cudaGetDevice(&previous_device_);
    if (status_ == cudaSuccess && previous_device_ != target_device) {
      status_ = cudaSetDevice(target_device);
      changed_ = status_ == cudaSuccess;
    }
  }

  CudaDeviceScope(const CudaDeviceScope&) = delete;
  CudaDeviceScope& operator=(const CudaDeviceScope&) = delete;

  ~CudaDeviceScope() {
    static_cast<void>(restore());
  }

  [[nodiscard]] cudaError_t status() const noexcept {
    return status_;
  }

  [[nodiscard]] cudaError_t restore() noexcept {
    if (!changed_) {
      return cudaSuccess;
    }
    const auto restore_status = cudaSetDevice(previous_device_);
    if (restore_status == cudaSuccess) {
      changed_ = false;
    }
    return restore_status;
  }

private:
  int previous_device_{};
  cudaError_t status_{cudaSuccess};
  bool changed_{};
};

class CudaMemoryAllocator final : public IMemoryAllocator {
public:
  CudaMemoryAllocator(MemoryKind kind, int device) : kind_(kind), device_(device) {}

  [[nodiscard]] MemoryKind memory_kind() const noexcept override {
    return kind_;
  }

  [[nodiscard]] core::Result<void*> allocate(std::uint64_t bytes, std::uint64_t alignment) override {
    CudaDeviceScope device_scope(device_);
    if (device_scope.status() != cudaSuccess) {
      return error(core::ErrorReason::unavailable, "CUDA 分配器无法选择所属设备",
                   cudaGetErrorString(device_scope.status()));
    }
    void* pointer{};
    const auto status = kind_ == MemoryKind::pinned_host
                            ? cudaHostAlloc(&pointer, static_cast<std::size_t>(bytes), cudaHostAllocDefault)
                            : cudaMalloc(&pointer, static_cast<std::size_t>(bytes));
    if (status != cudaSuccess) {
      return error(core::ErrorReason::unavailable, "CUDA 预算内存池分配失败", cudaGetErrorString(status));
    }
    if (reinterpret_cast<std::uintptr_t>(pointer) % alignment != 0U) {
      if (kind_ == MemoryKind::pinned_host) {
        static_cast<void>(cudaFreeHost(pointer));
      } else {
        static_cast<void>(cudaFree(pointer));
      }
      return error(core::ErrorReason::unavailable, "CUDA 分配结果未满足请求的对齐");
    }
    const auto restore_status = device_scope.restore();
    if (restore_status != cudaSuccess) {
      if (kind_ == MemoryKind::pinned_host) {
        static_cast<void>(cudaFreeHost(pointer));
      } else {
        static_cast<void>(cudaFree(pointer));
      }
      return error(core::ErrorReason::internal_failure, "CUDA 分配后无法恢复调用线程设备",
                   cudaGetErrorString(restore_status));
    }
    return pointer;
  }

  void release(void* pointer, std::uint64_t, std::uint64_t) noexcept override {
    CudaDeviceScope device_scope(device_);
    if (device_scope.status() != cudaSuccess) {
      return;
    }
    if (kind_ == MemoryKind::pinned_host) {
      static_cast<void>(cudaFreeHost(pointer));
    } else {
      static_cast<void>(cudaFree(pointer));
    }
  }

private:
  MemoryKind kind_;
  int device_{};
};

#endif

} // namespace

BackendCapabilities discover_cuda_capability() noexcept {
#if defined(SIGNAL_STUDIO_HAVE_CUDA_MEMORY)
  int runtime_version{};
  int count{};
  const auto count_status = cudaGetDeviceCount(&count);
  const auto version_status = cudaRuntimeGetVersion(&runtime_version);
  BackendCapabilities capability{
      BackendKind::cuda,
      "cuda-runtime",
      version_status == cudaSuccess ? std::to_string(runtime_version) : "runtime-query-failed",
      count_status == cudaSuccess ? "CUDA 设备数量为零" : std::string(cudaGetErrorString(count_status)),
      false,
      true,
      true,
      false,
      false,
      1U,
      0U,
      true};
  if (count_status == cudaSuccess && version_status == cudaSuccess && count > 0) {
    CudaDeviceScope device_scope(0);
    if (device_scope.status() != cudaSuccess) {
      capability.device = std::string{"CUDA 设备 0 上下文选择失败："} + cudaGetErrorString(device_scope.status());
    } else {
      cudaDeviceProp properties{};
      const auto property_status = cudaGetDeviceProperties(&properties, 0);
      if (property_status == cudaSuccess) {
        void* probe{};
        const auto allocation_status = cudaMalloc(&probe, 1U);
        const auto release_status = allocation_status == cudaSuccess ? cudaFree(probe) : allocation_status;
        capability.available = allocation_status == cudaSuccess && release_status == cudaSuccess;
        capability.device = properties.name;
        capability.memory_bytes = properties.totalGlobalMem;
        if (!capability.available) {
          capability.device +=
              std::string{"（设备上下文探测失败："} +
              cudaGetErrorString(allocation_status != cudaSuccess ? allocation_status : release_status) + "）";
        }
      } else {
        capability.device = std::string{"CUDA 设备属性探测失败："} + cudaGetErrorString(property_status);
      }
    }
    const auto restore_status = device_scope.restore();
    if (restore_status != cudaSuccess) {
      capability.available = false;
      capability.device += std::string{"（调用线程 CUDA 设备恢复失败："} + cudaGetErrorString(restore_status) + "）";
    }
  }
  return capability;
#else
  return {BackendKind::cuda,
          "cuda-runtime",
          "not-compiled",
          "CUDA 未编译",
          false,
          false,
          false,
          false,
          false,
          1U,
          0U,
          false};
#endif
}

core::Status execute_cuda_buffer_copy(const BufferCopyRequest& request) noexcept {
#if defined(SIGNAL_STUDIO_HAVE_CUDA_MEMORY)
  if (request.input.empty() || request.input.size() != request.output.size()) {
    return error(core::ErrorReason::invalid_argument, "CUDA 缓冲区复制参数无效");
  }
  void* input_device{};
  void* output_device{};
  const auto bytes = request.input.size_bytes();
  auto status = cudaMalloc(&input_device, bytes);
  if (status != cudaSuccess) {
    return error(core::ErrorReason::unavailable, "CUDA 输入设备内存分配失败", cudaGetErrorString(status));
  }
  status = cudaMalloc(&output_device, bytes);
  if (status != cudaSuccess) {
    static_cast<void>(cudaFree(input_device));
    return error(core::ErrorReason::unavailable, "CUDA 输出设备内存分配失败", cudaGetErrorString(status));
  }

  status = cudaMemcpy(input_device, request.input.data(), bytes, cudaMemcpyHostToDevice);
  if (status == cudaSuccess) {
    status = cudaMemcpy(output_device, input_device, bytes, cudaMemcpyDeviceToDevice);
  }
  if (status == cudaSuccess) {
    status = cudaMemcpy(request.output.data(), output_device, bytes, cudaMemcpyDeviceToHost);
  }
  const auto output_release = cudaFree(output_device);
  const auto input_release = cudaFree(input_device);
  if (status != cudaSuccess) {
    return error(core::ErrorReason::unavailable, "CUDA 实际设备缓冲区复制失败", cudaGetErrorString(status));
  }
  if (output_release != cudaSuccess || input_release != cudaSuccess) {
    const auto release_status = output_release != cudaSuccess ? output_release : input_release;
    return error(core::ErrorReason::internal_failure, "CUDA 设备内存释放失败", cudaGetErrorString(release_status));
  }
  return core::Status::success();
#else
  static_cast<void>(request);
  return error(core::ErrorReason::unavailable, "CUDA 缓冲区复制后端未构建");
#endif
}

core::Result<std::vector<std::shared_ptr<IMemoryAllocator>>> make_cuda_memory_allocators() {
#if defined(SIGNAL_STUDIO_HAVE_CUDA_MEMORY)
  int count{};
  auto status = cudaGetDeviceCount(&count);
  if (status != cudaSuccess || count <= 0) {
    return error(core::ErrorReason::unavailable, "CUDA 内存分配器不可运行",
                 status == cudaSuccess ? "设备数量为零" : cudaGetErrorString(status));
  }
  int device{};
  status = cudaGetDevice(&device);
  if (status != cudaSuccess || device < 0 || device >= count) {
    return error(core::ErrorReason::unavailable, "CUDA 内存分配器无法绑定当前实际设备",
                 status == cudaSuccess ? "当前设备编号超出探测范围" : cudaGetErrorString(status));
  }
  std::vector<std::shared_ptr<IMemoryAllocator>> allocators;
  allocators.push_back(std::make_shared<CudaMemoryAllocator>(MemoryKind::pinned_host, device));
  allocators.push_back(std::make_shared<CudaMemoryAllocator>(MemoryKind::device, device));
  return allocators;
#else
  return error(core::ErrorReason::unavailable, "CUDA 内存分配器未构建");
#endif
}

} // namespace signal::compute
