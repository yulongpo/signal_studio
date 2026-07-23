#include "signal_studio/dsp/fft.hpp"

#include <cstring>

#if defined(__CUDACC__) || defined(SIGNAL_STUDIO_HAVE_CUDA)
#define SIGNAL_STUDIO_CUDA_ENABLED 1
#include <cuda_runtime_api.h>
#include <cufft.h>
#endif

namespace signal::dsp {

namespace {
constexpr bool cuda_enabled_build =
#if SIGNAL_STUDIO_CUDA_ENABLED
    true;
#else
    false;
#endif

core::Status dsp_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, reason}, std::move(message));
}
} // namespace

bool fft_backend_available(compute::ComputeDeviceType device) noexcept {
  if (device == compute::ComputeDeviceType::cuda) {
    return cuda_enabled_build;
  }
  return false; // oneMKL CPU backend not built in this configuration
}

#if SIGNAL_STUDIO_CUDA_ENABLED
namespace {
class CudaFftPlan final : public FftPlan {
public:
  CudaFftPlan(std::uint64_t n, FftDirection direction, compute::BackendProvenance provenance)
      : size_(n), direction_(direction), provenance_(std::move(provenance)) {
    cufftPlan1d(&plan_, static_cast<int>(n), CUFFT_Z2Z, 1);
    const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(cufftDoubleComplex);
    cudaMalloc(&d_in_, bytes);
    cudaMalloc(&d_out_, bytes);
  }
  ~CudaFftPlan() override {
    if (plan_)
      cufftDestroy(plan_);
    if (d_in_)
      cudaFree(d_in_);
    if (d_out_)
      cudaFree(d_out_);
  }
  CudaFftPlan(const CudaFftPlan&) = delete;
  CudaFftPlan& operator=(const CudaFftPlan&) = delete;

  std::uint64_t size() const noexcept override {
    return size_;
  }
  FftDirection direction() const noexcept override {
    return direction_;
  }
  compute::BackendProvenance provenance() const override {
    return provenance_;
  }

  core::Result<std::vector<data::ComplexSample>> execute(std::span<const data::ComplexSample> input) const override {
    if (input.size() != size_) {
      return dsp_failure(core::ErrorReason::invalid_argument, "fft input length does not match plan size");
    }
    if (!plan_ || !d_in_ || !d_out_) {
      return dsp_failure(core::ErrorReason::internal_failure, "cuFFT plan or device buffers not initialized");
    }
    const std::size_t bytes = static_cast<std::size_t>(size_) * sizeof(cufftDoubleComplex);
    if (cudaMemcpy(d_in_, input.data(), bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
      return dsp_failure(core::ErrorReason::internal_failure, "cudaMemcpy H2D failed");
    }
    const cufftResult res =
        cufftExecZ2Z(plan_, static_cast<cufftDoubleComplex*>(d_in_), static_cast<cufftDoubleComplex*>(d_out_),
                     direction_ == FftDirection::forward ? CUFFT_FORWARD : CUFFT_INVERSE);
    if (res != CUFFT_SUCCESS) {
      return dsp_failure(core::ErrorReason::internal_failure, "cufftExecZ2Z failed");
    }
    if (cudaDeviceSynchronize() != cudaSuccess) {
      return dsp_failure(core::ErrorReason::internal_failure, "cudaDeviceSynchronize failed");
    }
    std::vector<data::ComplexSample> output(static_cast<std::size_t>(size_));
    if (cudaMemcpy(output.data(), d_out_, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
      return dsp_failure(core::ErrorReason::internal_failure, "cudaMemcpy D2H failed");
    }
    return output;
  }

private:
  std::uint64_t size_;
  FftDirection direction_;
  compute::BackendProvenance provenance_;
  cufftHandle plan_{};
  void* d_in_{nullptr};
  void* d_out_{nullptr};
};

class CudaFftBackend final : public IFftBackend {
public:
  explicit CudaFftBackend(compute::BackendProvenance provenance) : provenance_(std::move(provenance)) {}
  compute::ComputeDeviceType device_type() const noexcept override {
    return compute::ComputeDeviceType::cuda;
  }
  compute::BackendProvenance provenance() const override {
    return provenance_;
  }
  core::Result<std::unique_ptr<FftPlan>> create_plan(const FftSpec& spec) override {
    if (spec.size == 0) {
      return dsp_failure(core::ErrorReason::invalid_argument, "fft size must be positive");
    }
    auto plan = std::make_unique<CudaFftPlan>(spec.size, spec.direction, provenance_);
    return std::unique_ptr<FftPlan>(std::move(plan));
  }

private:
  compute::BackendProvenance provenance_;
};
} // namespace
#endif // SIGNAL_STUDIO_CUDA_ENABLED

core::Result<std::unique_ptr<IFftBackend>> make_fft_backend(compute::ComputeDeviceType device) {
  if (device == compute::ComputeDeviceType::cuda) {
#if SIGNAL_STUDIO_CUDA_ENABLED
    auto cuda_backend = compute::make_cuda_compute_backend();
    if (!cuda_backend.ok()) {
      return dsp_failure(core::ErrorReason::unavailable,
                         "CUDA compute backend unavailable: " + std::string(cuda_backend.error().message()));
    }
    compute::BackendProvenance provenance = (*cuda_backend)->provenance();
    return std::unique_ptr<IFftBackend>(std::make_unique<CudaFftBackend>(std::move(provenance)));
#else
    return dsp_failure(core::ErrorReason::unavailable,
                       "cuFFT backend was not built into this configuration (oneMKL CPU also unavailable)");
#endif
  }
  // CPU path: oneMKL is the BL1.0-sanctioned CPU FFT backend (ADR-006). It is not installed in
  // this environment, so the CPU FFT adapter is unavailable rather than faked.
  return dsp_failure(core::ErrorReason::unavailable, "oneMKL CPU FFT backend is not installed in this environment");
}

core::Result<FftResult> fft(std::span<const data::ComplexSample> input, IFftBackend& backend) {
  FftSpec spec;
  spec.size = input.size();
  spec.direction = FftDirection::forward;
  auto plan_result = backend.create_plan(spec);
  if (!plan_result.ok()) {
    return core::Status(plan_result.error());
  }
  auto exec_result = (*plan_result)->execute(input);
  if (!exec_result.ok()) {
    return core::Status(exec_result.error());
  }
  FftResult result;
  result.bins = std::move(*exec_result);
  result.size = input.size();
  result.direction = FftDirection::forward;
  result.provenance = (*plan_result)->provenance();
  return result;
}

core::Result<FftResult> ifft(std::span<const data::ComplexSample> input, IFftBackend& backend) {
  FftSpec spec;
  spec.size = input.size();
  spec.direction = FftDirection::inverse;
  auto plan_result = backend.create_plan(spec);
  if (!plan_result.ok()) {
    return core::Status(plan_result.error());
  }
  auto exec_result = (*plan_result)->execute(input);
  if (!exec_result.ok()) {
    return core::Status(exec_result.error());
  }
  // cuFFT inverse is unscaled; divide by N to recover the original signal.
  const double scale = 1.0 / static_cast<double>(input.size());
  for (auto& bin : *exec_result) {
    bin.real *= scale;
    bin.imag *= scale;
  }
  FftResult result;
  result.bins = std::move(*exec_result);
  result.size = input.size();
  result.direction = FftDirection::inverse;
  result.provenance = (*plan_result)->provenance();
  return result;
}

} // namespace signal::dsp
