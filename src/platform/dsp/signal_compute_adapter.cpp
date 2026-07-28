#include "signal_studio/compute/compute.hpp"
#include "signal_studio/dsp/pipeline.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <thread>

namespace signal::dsp {
namespace {

[[nodiscard]] core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::dsp, reason}, std::move(message), std::move(diagnostic));
}

class KernelOperation final : public compute::IComputeOperation {
public:
  using Executor = std::function<core::Result<std::vector<data::ComplexSample>>(compute::BackendKind)>;

  KernelOperation(std::string operation_name, Executor executor)
      : operation_name_(std::move(operation_name)), executor_(std::move(executor)) {}

  [[nodiscard]] std::string_view operation() const noexcept override {
    return operation_name_;
  }

  [[nodiscard]] bool supports(compute::BackendKind kind) const noexcept override {
    return kind == compute::BackendKind::cpu_simd;
  }

  [[nodiscard]] core::Status execute(compute::BackendKind kind, std::uint32_t) override {
    if (!supports(kind)) {
      return error(core::ErrorReason::unavailable, "成熟信号内核适配器不支持所选计算后端",
                   std::string(compute::backend_kind_name(kind)));
    }
    auto result = executor_(kind);
    if (!result) {
      return result.error();
    }
    result_ = std::move(result.value());
    return core::Status::success();
  }

  [[nodiscard]] core::Result<compute::ConsistencyMetrics> consistency(compute::BackendKind) const override {
    if (!result_.has_value()) {
      return error(core::ErrorReason::internal_failure, "信号内核尚未生成可验证结果");
    }
    return compute::ConsistencyMetrics{0.0, 0.0, result_->size() * 2U, false};
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>> take_result() {
    if (!result_.has_value()) {
      return error(core::ErrorReason::internal_failure, "信号内核没有可发布结果");
    }
    return std::move(result_.value());
  }

private:
  std::string operation_name_;
  Executor executor_;
  std::optional<std::vector<data::ComplexSample>> result_;
};

class ComputeSignalKernelBackend final : public ISignalKernelBackend {
public:
  ComputeSignalKernelBackend(std::shared_ptr<ISignalKernelBackend> mature_backend,
                             std::shared_ptr<compute::ComputeRuntime> runtime)
      : mature_backend_(std::move(mature_backend)), runtime_(std::move(runtime)),
        backend_id_("SignalCompute[" + std::string(mature_backend_->backend_id()) + "]") {}

  [[nodiscard]] std::string_view backend_id() const noexcept override {
    return backend_id_;
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>> analytic_signal(std::span<const double> input) override {
    KernelOperation operation("fft",
                              [this, input](compute::BackendKind) { return mature_backend_->analytic_signal(input); });
    return execute(operation, input.size_bytes(), input.size() * 4U);
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>> convolve(std::span<const data::ComplexSample> input,
                                                                        std::span<const double> coefficients,
                                                                        FilterState& state,
                                                                        BoundaryPolicy boundary) override {
    KernelOperation operation("filter", [this, input, coefficients, &state, boundary](compute::BackendKind) {
      return mature_backend_->convolve(input, coefficients, state, boundary);
    });
    return execute(operation, input.size_bytes(), input.size() * coefficients.size());
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>>
  solve_iir(std::span<const data::ComplexSample> input, std::span<const double> numerator,
            std::span<const double> denominator, FilterState& state, BoundaryPolicy boundary) override {
    KernelOperation operation("filter", [this, input, numerator, denominator, &state, boundary](compute::BackendKind) {
      return mature_backend_->solve_iir(input, numerator, denominator, state, boundary);
    });
    return execute(operation, input.size_bytes(), input.size() * (numerator.size() + denominator.size()));
  }

  [[nodiscard]] core::Result<std::vector<data::ComplexSample>>
  resample(std::span<const data::ComplexSample> input, std::uint32_t numerator, std::uint32_t denominator,
           std::span<const double> anti_alias_coefficients, FilterState& state, bool end_of_input) override {
    KernelOperation operation("resample", [this, input, numerator, denominator, anti_alias_coefficients, &state,
                                           end_of_input](compute::BackendKind) {
      return mature_backend_->resample(input, numerator, denominator, anti_alias_coefficients, state, end_of_input);
    });
    const auto operation_count = input.size() > std::numeric_limits<std::uint64_t>::max() / 64U
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : input.size() * 64U;
    return execute(operation, input.size_bytes(), operation_count);
  }

private:
  [[nodiscard]] core::Result<std::vector<data::ComplexSample>>
  execute(KernelOperation& operation, std::uint64_t input_bytes, std::uint64_t operation_count) {
    const auto working_set_bytes = input_bytes > std::numeric_limits<std::uint64_t>::max() / 2U
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : std::max<std::uint64_t>(1U, input_bytes * 2U);
    const compute::Workload workload{std::string(operation.operation()),
                                     std::max<std::uint64_t>(1U, input_bytes),
                                     working_set_bytes,
                                     std::max<std::uint64_t>(1U, operation_count),
                                     false,
                                     false,
                                     true,
                                     compute::BackendKind::cpu_simd};
    auto execution = runtime_->execute_operation(workload, operation, 0.0, 0.0);
    if (!execution) {
      return execution.error();
    }
    return operation.take_result();
  }

  std::shared_ptr<ISignalKernelBackend> mature_backend_;
  std::shared_ptr<compute::ComputeRuntime> runtime_;
  std::string backend_id_;
};

} // namespace

core::Result<std::shared_ptr<ISignalKernelBackend>> make_auto_signal_kernel_backend() {
  auto mature_backend = make_cpu_signal_kernel_backend();
  if (!mature_backend) {
    return mature_backend.error();
  }
  const auto threads = std::max(1U, std::thread::hardware_concurrency());
  std::vector<compute::BackendCapabilities> capabilities{
      {compute::BackendKind::cpu_simd, std::string(mature_backend.value()->backend_id()),
       "oneMKL 2025.2 + libsamplerate 0.2.2", "host-cpu", true, false, true, true, true, threads, 0U, true},
  };
  auto runtime = compute::ComputeRuntime::create(std::move(capabilities));
  if (!runtime) {
    return runtime.error();
  }
  return std::shared_ptr<ISignalKernelBackend>(
      std::make_shared<ComputeSignalKernelBackend>(mature_backend.value(), runtime.value()));
}

} // namespace signal::dsp
