#pragma once

#include "signal_studio/compute/compute.hpp"
#include "signal_studio/dsp/analysis.hpp"
#include "signal_studio/dsp/pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace signal_studio_consumer {

inline bool verify_ms02_public_api() {
  auto backends = signal::compute::discover_compute_backends();
  auto runtime = signal::compute::ComputeRuntime::create(std::move(backends));
  if (!runtime)
    return false;

  std::vector<double> reference(512U);
  for (std::size_t index = 0; index < reference.size(); ++index)
    reference[index] = std::sin(static_cast<double>(index) * 0.03125);
  std::vector<double> copied(reference.size());
  signal::compute::Workload copy_workload;
  copy_workload.operation = "buffer-copy";
  copy_workload.input_bytes = reference.size() * sizeof(double);
  copy_workload.working_set_bytes = copy_workload.input_bytes * 2U;
  copy_workload.operation_count = reference.size();
  copy_workload.deterministic = true;
  const auto execution = runtime.value()->execute_buffer_copy(copy_workload, {reference, copied}, 0.0, 0.0);
  if (!execution || !execution.value().provenance.consistency_verified ||
      execution.value().consistency.value_count != reference.size() ||
      execution.value().consistency.maximum_absolute_error != 0.0 || execution.value().consistency.rms_error != 0.0 ||
      copied != reference) {
    return false;
  }

  std::vector<signal::data::ComplexSample> tone;
  tone.reserve(512U);
  for (std::size_t index = 0; index < 512U; ++index) {
    const auto phase = 2.0 * std::numbers::pi * 19.0 * static_cast<double>(index) / 512.0;
    tone.push_back({std::cos(phase), std::sin(phase)});
  }
  const auto samples = signal::data::SignalBuffer::from_complex(tone);
  const auto fft = signal::dsp::make_cpu_fft_backend();
  if (!fft)
    return false;
  const auto plan = fft.value()->create_plan({512U, signal::dsp::FftDirection::forward});
  if (!plan)
    return false;
  const auto transformed = plan.value()->process(samples.view().complex_values());
  if (!transformed || transformed.value().bins.size() != 512U)
    return false;

  const auto psd = signal::dsp::make_psd_estimator(fft.value());
  const auto stft = signal::dsp::make_stft_processor(fft.value());
  if (!psd || !stft)
    return false;
  const auto density = psd.value()->process(samples.view(), {48'000.0, 0.0, signal::dsp::WindowKind::hann,
                                                             signal::dsp::SpectrumSidedness::two_sided_shifted});
  const auto time_frequency =
      stft.value()->process(samples.view(), {48'000.0, 0.0, 128U, 64U, signal::dsp::WindowKind::hann,
                                             signal::dsp::SpectrumSidedness::two_sided_shifted});
  if (!density || density.value().db_per_hz.size() != 512U || !time_frequency || time_frequency.value().rows != 7U ||
      time_frequency.value().columns != 128U) {
    return false;
  }

  const auto kernel = signal::dsp::make_cpu_signal_kernel_backend();
  if (!kernel)
    return false;
  const auto filter = signal::dsp::make_filter(kernel.value());
  const auto resampler = signal::dsp::make_resampler(kernel.value());
  if (!filter || !resampler)
    return false;

  signal::dsp::NodeSpec filter_spec;
  filter_spec.id = "installed-consumer-fir";
  filter_spec.kind = signal::dsp::NodeKind::fir_filter;
  filter_spec.filter_shape = signal::dsp::FilterShape::custom;
  filter_spec.numerator = {0.25, 0.5, 0.25};
  signal::dsp::FilterState filter_state;
  const auto filtered = filter.value()->process(filter_spec, 48'000.0, samples.view().complex_values(), filter_state,
                                                signal::dsp::BoundaryPolicy::preserve_state);
  signal::dsp::FilterState resample_state;
  constexpr std::array identity_filter{1.0};
  const auto resampled =
      resampler.value()->process({1U, 1U}, samples.view().complex_values(), identity_filter, resample_state);
  return filtered && filtered.value().size() == samples.view().size() && resampled &&
         resampled.value().size() == samples.view().size() &&
         std::ranges::all_of(filtered.value(),
                             [](const auto& value) { return std::isfinite(value.real) && std::isfinite(value.imag); });
}

} // namespace signal_studio_consumer
