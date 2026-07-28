#include "signal_studio/dsp/analysis.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(SIGNAL_STUDIO_HAVE_CUFFT)
#include <cuda_runtime_api.h>
#include <cufft.h>
#endif

namespace signal::dsp {
namespace {

[[nodiscard]] core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::dsp, reason}, std::move(message), std::move(diagnostic));
}

#if defined(SIGNAL_STUDIO_HAVE_CUFFT)

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

class DeviceBuffer final {
public:
  explicit DeviceBuffer(std::size_t bytes) {
    status_ = cudaMalloc(&pointer_, bytes);
  }
  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;
  ~DeviceBuffer() {
    if (pointer_ != nullptr) {
      static_cast<void>(cudaFree(pointer_));
    }
  }
  [[nodiscard]] cudaError_t status() const noexcept {
    return status_;
  }
  [[nodiscard]] void* get() const noexcept {
    return pointer_;
  }

private:
  void* pointer_{};
  cudaError_t status_{cudaSuccess};
};

class CufftPlan final {
public:
  CufftPlan(int length, int device) : device_(device) {
    CudaDeviceScope device_scope(device_);
    cuda_status_ = device_scope.status();
    if (cuda_status_ == cudaSuccess) {
      status_ = cufftPlan1d(&plan_, length, CUFFT_Z2Z, 1);
    }
    const auto restore_status = device_scope.restore();
    if (cuda_status_ == cudaSuccess && restore_status != cudaSuccess) {
      cuda_status_ = restore_status;
    }
  }
  CufftPlan(const CufftPlan&) = delete;
  CufftPlan& operator=(const CufftPlan&) = delete;
  ~CufftPlan() {
    if (status_ == CUFFT_SUCCESS) {
      CudaDeviceScope device_scope(device_);
      if (device_scope.status() == cudaSuccess) {
        static_cast<void>(cufftDestroy(plan_));
      }
    }
  }
  [[nodiscard]] cudaError_t cuda_status() const noexcept {
    return cuda_status_;
  }
  [[nodiscard]] cufftResult status() const noexcept {
    return status_;
  }
  [[nodiscard]] cufftHandle get() const noexcept {
    return plan_;
  }

private:
  cufftHandle plan_{};
  int device_{};
  cudaError_t cuda_status_{cudaSuccess};
  cufftResult status_{CUFFT_INVALID_PLAN};
};

class CudaFftBackend final : public IFftBackend {
public:
  explicit CudaFftBackend(int device, cudaDeviceProp properties) : device_(device), properties_(properties) {
    int runtime_version{};
    int cufft_version{};
    static_cast<void>(cudaRuntimeGetVersion(&runtime_version));
    static_cast<void>(cufftGetVersion(&cufft_version));
    version_ = "CUDA Runtime " + std::to_string(runtime_version / 1000) + "." +
               std::to_string((runtime_version % 1000) / 10) + "; cuFFT " + std::to_string(cufft_version / 1000) + "." +
               std::to_string((cufft_version % 1000) / 100) + "." + std::to_string(cufft_version % 100);
  }

  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return "NVIDIA-cuFFT";
  }

  [[nodiscard]] core::Status validate(const FftSpec& spec) const override {
    if (spec.length < 2U || spec.length > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      return error(core::ErrorReason::invalid_argument, "cuFFT 长度无效");
    }
    return core::Status::success();
  }

  [[nodiscard]] core::Result<std::shared_ptr<IFftPlan>> create_plan(const FftSpec& spec) override {
    if (const auto status = validate(spec); !status) {
      return status;
    }
    auto plan = cached_plan(spec);
    if (!plan) {
      return plan.error();
    }
    return std::static_pointer_cast<IFftPlan>(plan.value());
  }

private:
  struct PlanState final : IFftPlan {
    PlanState(FftSpec plan_specification, int selected_device, cudaDeviceProp selected_properties,
              std::string backend_version)
        : specification(plan_specification), device(selected_device), properties(selected_properties),
          version(std::move(backend_version)), plan(static_cast<int>(plan_specification.length), selected_device) {}

    [[nodiscard]] FftSpec spec() const noexcept override {
      return specification;
    }

    [[nodiscard]] core::Result<FftResult> process(std::span<const data::ComplexSample> input) override {
      if (input.size() != specification.length) {
        return error(core::ErrorReason::invalid_argument, "cuFFT 输入长度与计划不一致");
      }
      std::vector<cufftDoubleComplex> host(input.size());
      for (std::size_t index = 0; index < input.size(); ++index) {
        if (!std::isfinite(input[index].real) || !std::isfinite(input[index].imag)) {
          return error(core::ErrorReason::invalid_argument, "cuFFT 输入包含 NaN 或 Inf");
        }
        host[index] = {input[index].real, input[index].imag};
      }
      CudaDeviceScope device_scope(device);
      if (device_scope.status() != cudaSuccess) {
        return error(core::ErrorReason::internal_failure, "CUDA 设备选择失败",
                     cudaGetErrorString(device_scope.status()));
      }
      auto execution = [&]() -> core::Result<FftResult> {
        DeviceBuffer device_buffer(host.size() * sizeof(cufftDoubleComplex));
        if (device_buffer.status() != cudaSuccess) {
          return error(core::ErrorReason::unavailable, "CUDA 设备内存申请失败",
                       cudaGetErrorString(device_buffer.status()));
        }
        auto cuda_status = cudaMemcpy(device_buffer.get(), host.data(), host.size() * sizeof(cufftDoubleComplex),
                                      cudaMemcpyHostToDevice);
        if (cuda_status != cudaSuccess) {
          return error(core::ErrorReason::internal_failure, "CUDA 输入传输失败", cudaGetErrorString(cuda_status));
        }
        std::scoped_lock plan_lock(mutex);
        const auto fft_status =
            cufftExecZ2Z(plan.get(), static_cast<cufftDoubleComplex*>(device_buffer.get()),
                         static_cast<cufftDoubleComplex*>(device_buffer.get()),
                         specification.direction == FftDirection::forward ? CUFFT_FORWARD : CUFFT_INVERSE);
        if (fft_status != CUFFT_SUCCESS) {
          return error(core::ErrorReason::internal_failure, "cuFFT 执行失败", std::to_string(fft_status));
        }
        cuda_status = cudaDeviceSynchronize();
        if (cuda_status != cudaSuccess) {
          return error(core::ErrorReason::internal_failure, "cuFFT 同步失败", cudaGetErrorString(cuda_status));
        }
        cuda_status = cudaMemcpy(host.data(), device_buffer.get(), host.size() * sizeof(cufftDoubleComplex),
                                 cudaMemcpyDeviceToHost);
        if (cuda_status != cudaSuccess) {
          return error(core::ErrorReason::internal_failure, "CUDA 输出传输失败", cudaGetErrorString(cuda_status));
        }
        FftResult result;
        result.bins.reserve(host.size());
        const auto scale =
            specification.direction == FftDirection::inverse ? 1.0 / static_cast<double>(specification.length) : 1.0;
        for (const auto& value : host) {
          result.bins.push_back({value.x * scale, value.y * scale});
        }
        result.provenance = {compute::BackendKind::cuda,
                             compute::BackendKind::cuda,
                             "NVIDIA-cuFFT",
                             version,
                             properties.name,
                             "实机 cudaMalloc、H2D、cuFFT、D2H 端到端执行",
                             false,
                             false};
        return result;
      }();
      const auto restore_status = device_scope.restore();
      if (restore_status != cudaSuccess) {
        return error(core::ErrorReason::internal_failure, "调用线程 CUDA 设备恢复失败",
                     cudaGetErrorString(restore_status));
      }
      return execution;
    }

    FftSpec specification;
    int device{};
    cudaDeviceProp properties{};
    std::string version;
    CufftPlan plan;
    std::mutex mutex;
  };

  [[nodiscard]] core::Result<std::shared_ptr<PlanState>> cached_plan(const FftSpec& spec) {
    std::scoped_lock lock(plan_cache_mutex_);
    const auto key = std::pair{spec.length, spec.direction};
    if (const auto found = plans_.find(key); found != plans_.end()) {
      return found->second;
    }
    auto created = std::make_shared<PlanState>(spec, device_, properties_, version_);
    if (created->plan.cuda_status() != cudaSuccess) {
      return error(core::ErrorReason::internal_failure, "cuFFT 计划设备上下文准备失败",
                   cudaGetErrorString(created->plan.cuda_status()));
    }
    if (created->plan.status() != CUFFT_SUCCESS) {
      return error(core::ErrorReason::internal_failure, "cuFFT 计划创建失败", std::to_string(created->plan.status()));
    }
    plans_.emplace(key, created);
    return created;
  }

  int device_{};
  cudaDeviceProp properties_{};
  std::string version_;
  std::mutex plan_cache_mutex_;
  std::map<std::pair<std::uint64_t, FftDirection>, std::shared_ptr<PlanState>> plans_;
};

#endif

} // namespace

core::Result<std::shared_ptr<IFftBackend>> make_cuda_fft_backend() {
#if defined(SIGNAL_STUDIO_HAVE_CUFFT)
  int count{};
  auto status = cudaGetDeviceCount(&count);
  if (status != cudaSuccess || count <= 0) {
    return error(core::ErrorReason::unavailable, "没有可运行的 CUDA 设备",
                 status == cudaSuccess ? "设备数量为零" : cudaGetErrorString(status));
  }
  cudaDeviceProp properties{};
  status = cudaGetDeviceProperties(&properties, 0);
  if (status != cudaSuccess) {
    return error(core::ErrorReason::internal_failure, "读取 CUDA 设备属性失败", cudaGetErrorString(status));
  }
  return std::shared_ptr<IFftBackend>(std::make_shared<CudaFftBackend>(0, properties));
#else
  return error(core::ErrorReason::unavailable, "cuFFT 适配器未构建",
               "需要 CUDA Toolkit 12.4 的 cudart 与 cuFFT 开发文件");
#endif
}

bool cuda_fft_available() noexcept {
#if defined(SIGNAL_STUDIO_HAVE_CUFFT)
  int count{};
  return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
#else
  return false;
#endif
}

} // namespace signal::dsp
