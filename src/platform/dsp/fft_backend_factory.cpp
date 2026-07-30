#include "signal_studio/dsp/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace signal::dsp {
namespace {

[[nodiscard]] core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::dsp, reason}, std::move(message), std::move(diagnostic));
}

class FftOperation final : public compute::IComputeOperation {
public:
  FftOperation(std::map<compute::BackendKind, std::shared_ptr<IFftPlan>> plans,
               std::span<const data::ComplexSample> input)
      : plans_(std::move(plans)), input_(input.begin(), input.end()) {}

  [[nodiscard]] std::string_view operation() const noexcept override {
    return "fft";
  }

  [[nodiscard]] bool supports(compute::BackendKind kind) const noexcept override {
    return plans_.contains(kind);
  }

  [[nodiscard]] core::Status execute(compute::BackendKind kind, std::uint32_t) override {
    const auto plan = plans_.find(kind);
    if (plan == plans_.end()) {
      return error(core::ErrorReason::unavailable, "FFT 适配器不支持所选计算后端",
                   std::string(compute::backend_kind_name(kind)));
    }
    auto transformed = plan->second->process(input_);
    if (!transformed) {
      return transformed.error();
    }
    result_ = std::move(transformed.value());
    actual_kind_ = kind;
    return core::Status::success();
  }

  [[nodiscard]] core::Result<compute::ConsistencyMetrics> consistency(compute::BackendKind kind) const override {
    if (!result_.has_value() || kind != actual_kind_) {
      return error(core::ErrorReason::internal_failure, "FFT 操作尚未生成可验证结果");
    }
    // CPU/CUDA numerical equivalence is covered by the dedicated backend test matrix.
    // Production analysis must not synchronously repeat every cuFFT on the CPU.
    return compute::ConsistencyMetrics{0.0, 0.0, result_->bins.size() * 2U, false};
  }

  [[nodiscard]] core::Result<FftResult> take_result(const compute::ComputeExecution& execution) {
    if (!result_.has_value()) {
      return error(core::ErrorReason::internal_failure, "FFT 运行时没有可发布结果");
    }
    auto native = std::move(result_.value());
    auto provenance = execution.provenance;
    provenance.backend_id = native.provenance.backend_id;
    provenance.version = native.provenance.version;
    provenance.device = native.provenance.device;
    provenance.reason += "；" + native.provenance.reason;
    native.provenance = std::move(provenance);
    return native;
  }

private:
  std::map<compute::BackendKind, std::shared_ptr<IFftPlan>> plans_;
  std::vector<data::ComplexSample> input_;
  mutable std::optional<FftResult> result_;
  compute::BackendKind actual_kind_{compute::BackendKind::cpu_simd};
};

class ComputeFftPlan final : public IFftPlan {
public:
  ComputeFftPlan(FftSpec specification, std::shared_ptr<compute::ComputeRuntime> runtime,
                 std::map<compute::BackendKind, std::shared_ptr<IFftPlan>> plans, bool prefer_cuda)
      : specification_(specification), runtime_(std::move(runtime)), plans_(std::move(plans)),
        prefer_cuda_(prefer_cuda) {}

  [[nodiscard]] FftSpec spec() const noexcept override {
    return specification_;
  }

  [[nodiscard]] core::Result<FftResult> process(std::span<const data::ComplexSample> input) override {
    if (input.size() != specification_.length) {
      return error(core::ErrorReason::invalid_argument, "SignalCompute FFT 输入长度与计划不一致");
    }
    const auto input_bytes = static_cast<std::uint64_t>(input.size_bytes());
    const auto working_set = input_bytes > std::numeric_limits<std::uint64_t>::max() / 3U
                                 ? std::numeric_limits<std::uint64_t>::max()
                                 : input_bytes * 3U;
    const auto operation_count = static_cast<std::uint64_t>(std::ceil(
        static_cast<double>(input.size()) * std::log2(static_cast<double>(std::max<std::size_t>(2U, input.size())))));
    compute::Workload workload{
        "fft", input_bytes, working_set, operation_count,
        false, false,       true,        prefer_cuda_ ? compute::BackendKind::cuda : compute::BackendKind::cpu_simd};
    FftOperation operation(plans_, input);
    const auto tolerance = std::max(1.0e-10, 1.0e-9 * std::sqrt(static_cast<double>(input.size())));
    auto execution = runtime_->execute_operation(workload, operation, tolerance, tolerance);
    if (!execution) {
      return execution.error();
    }
    return operation.take_result(execution.value());
  }

private:
  FftSpec specification_;
  std::shared_ptr<compute::ComputeRuntime> runtime_;
  std::map<compute::BackendKind, std::shared_ptr<IFftPlan>> plans_;
  bool prefer_cuda_{};
};

class ComputeFftBackend final : public IFftBackend {
public:
  ComputeFftBackend(std::shared_ptr<compute::ComputeRuntime> runtime, std::shared_ptr<IFftBackend> cpu,
                    std::shared_ptr<IFftBackend> cuda, bool prefer_cuda)
      : runtime_(std::move(runtime)), cpu_(std::move(cpu)), cuda_(std::move(cuda)), prefer_cuda_(prefer_cuda) {}

  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return prefer_cuda_ && cuda_ ? cuda_->backend_id() : cpu_->backend_id();
  }

  [[nodiscard]] core::Status validate(const FftSpec& spec) const override {
    return cpu_->validate(spec);
  }

  [[nodiscard]] core::Result<std::shared_ptr<IFftPlan>> create_plan(const FftSpec& spec) override {
    if (const auto status = validate(spec); !status) {
      return status;
    }
    auto cpu_plan = cpu_->create_plan(spec);
    if (!cpu_plan) {
      return cpu_plan.error();
    }
    std::map<compute::BackendKind, std::shared_ptr<IFftPlan>> plans{
        {compute::BackendKind::cpu_simd, cpu_plan.value()},
    };
    if (cuda_) {
      auto cuda_plan = cuda_->create_plan(spec);
      if (cuda_plan) {
        plans.emplace(compute::BackendKind::cuda, cuda_plan.value());
      }
    }
    return std::shared_ptr<IFftPlan>(std::make_shared<ComputeFftPlan>(spec, runtime_, std::move(plans), prefer_cuda_));
  }

private:
  std::shared_ptr<compute::ComputeRuntime> runtime_;
  std::shared_ptr<IFftBackend> cpu_;
  std::shared_ptr<IFftBackend> cuda_;
  bool prefer_cuda_{};
};

} // namespace

core::Result<std::shared_ptr<IFftBackend>> make_auto_fft_backend(bool prefer_cuda, std::string* selection_reason) {
  auto cpu = make_cpu_fft_backend();
  if (!cpu) {
    return cpu.error();
  }
  std::shared_ptr<IFftBackend> cuda;
  if (prefer_cuda) {
    auto detected = make_cuda_fft_backend();
    if (detected) {
      cuda = detected.value();
    }
  }

  std::vector<compute::BackendCapabilities> capabilities{
      {compute::BackendKind::cpu_simd, "oneMKL-DFTI", "oneMKL 2025.2", "host-cpu", true, false, true, false, false,
       std::max(1U, std::thread::hardware_concurrency()), 0U, true},
  };
  if (cuda) {
    auto discovered = compute::discover_compute_capabilities();
    const auto found = std::ranges::find_if(discovered, [](const auto& capability) {
      return capability.kind == compute::BackendKind::cuda && capability.available;
    });
    if (found != discovered.end()) {
      found->supports_fft = true;
      capabilities.push_back(*found);
    }
  }
  auto runtime = compute::ComputeRuntime::create(std::move(capabilities));
  if (!runtime) {
    return runtime.error();
  }
  if (selection_reason != nullptr) {
    if (!prefer_cuda) {
      *selection_reason = "SignalCompute 选择批准的 oneMKL CPU FFT 适配器";
    } else if (cuda) {
      *selection_reason = "SignalCompute 首选 CUDA 12.4 cuFFT，并以 oneMKL 作为显式降级";
    } else {
      *selection_reason = "CUDA 不可运行，SignalCompute 显式降级 oneMKL CPU FFT";
    }
  }
  return std::shared_ptr<IFftBackend>(
      std::make_shared<ComputeFftBackend>(runtime.value(), cpu.value(), std::move(cuda), prefer_cuda));
}

} // namespace signal::dsp
