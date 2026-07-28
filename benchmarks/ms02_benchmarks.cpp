#include "mkl_benchmark_probe.hpp"
#include "ms02_benchmark_support.hpp"

#include "signal_studio/dsp/pipeline.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(SIGNAL_STUDIO_BENCHMARK_HAVE_CUDA)
#include <cuda_runtime_api.h>
#include <cufft.h>
#endif

#ifndef SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256
#define SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256 "unknown"
#endif

#ifndef SIGNAL_STUDIO_MS02_PERF_RECORDING
#define SIGNAL_STUDIO_MS02_PERF_RECORDING ""
#endif

#ifndef SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE
#define SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE 0
#endif

namespace {

using Clock = std::chrono::steady_clock;
using signal::data::ComplexSample;

constexpr int kSamples = 30;
#if defined(_DEBUG)
constexpr int kRecordingIndexSamples = 1;
#else
constexpr int kRecordingIndexSamples = 3;
#endif

class FailureTrackingConsoleReporter final : public benchmark::ConsoleReporter {
public:
  using benchmark::ConsoleReporter::ConsoleReporter;

  void ReportRuns(const std::vector<Run>& reports) override {
    for (const auto& report : reports) {
      if (report.skipped == benchmark::internal::SkippedWithError)
        error_occurred_ = true;
    }
    benchmark::ConsoleReporter::ReportRuns(reports);
  }

  [[nodiscard]] bool error_occurred() const noexcept {
    return error_occurred_;
  }

private:
  bool error_occurred_{};
};

double percentile(const std::vector<double>& values, double probability) {
  if (values.empty())
    return 0.0;
  auto ordered = values;
  std::ranges::sort(ordered);
  const auto rank = static_cast<std::size_t>(std::ceil(probability * static_cast<double>(ordered.size())));
  return ordered[std::min(ordered.size() - 1U, std::max<std::size_t>(1U, rank) - 1U)];
}

double mean(const std::vector<double>& values) {
  if (values.empty())
    return 0.0;
  return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double ci95_margin(const std::vector<double>& values) {
  if (values.size() < 2U)
    return 0.0;
  const auto average = mean(values);
  double squared_deviation{};
  for (const auto value : values) {
    const auto deviation = value - average;
    squared_deviation += deviation * deviation;
  }
  const auto variance = squared_deviation / static_cast<double>(values.size() - 1U);
  return 1.96 * std::sqrt(variance / static_cast<double>(values.size()));
}

benchmark::Benchmark* evidence(benchmark::Benchmark* test, int repetitions = kSamples) {
  return test->Iterations(1)
      ->Repetitions(repetitions)
      ->ReportAggregatesOnly(true)
      ->UseManualTime()
      ->Unit(benchmark::kMicrosecond)
      ->ComputeStatistics("p50", [](const std::vector<double>& values) { return percentile(values, 0.50); })
      ->ComputeStatistics("p95", [](const std::vector<double>& values) { return percentile(values, 0.95); })
      ->ComputeStatistics(
          "max",
          [](const std::vector<double>& values) { return values.empty() ? 0.0 : *std::ranges::max_element(values); })
      ->ComputeStatistics(
          "ci95_low",
          [](const std::vector<double>& values) { return std::max(0.0, mean(values) - ci95_margin(values)); })
      ->ComputeStatistics("ci95_high",
                          [](const std::vector<double>& values) { return mean(values) + ci95_margin(values); });
}

void set_elapsed(benchmark::State& state, Clock::time_point start) {
  state.SetIterationTime(std::chrono::duration<double>(Clock::now() - start).count());
}

std::vector<ComplexSample> fft_input(std::size_t length, bool real_input) {
  const auto tone = signal::benchmarks::ms02::complex_tone(length, 17U);
  std::vector<ComplexSample> values(tone.view().complex_values().begin(), tone.view().complex_values().end());
  if (real_input) {
    for (auto& value : values)
      value.imag = 0.0;
  }
  return values;
}

struct FftCase final {
  std::size_t length{};
  std::size_t batch{1U};
  bool real_input{};
  bool cold_plan{};
};

void fft_benchmark(benchmark::State& state, FftCase configuration) {
  auto input = fft_input(configuration.length, configuration.real_input);
  std::shared_ptr<signal::dsp::IFftBackend> hot_backend;
  std::shared_ptr<signal::dsp::IFftPlan> hot_plan;
  if (!configuration.cold_plan) {
    const auto backend = signal::dsp::make_cpu_fft_backend();
    if (!backend) {
      state.SkipWithError(std::string{backend.error().message()});
      return;
    }
    hot_backend = backend.value();
    const auto plan = hot_backend->create_plan({configuration.length, signal::dsp::FftDirection::forward});
    if (!plan) {
      state.SkipWithError(std::string{plan.error().message()});
      return;
    }
    const auto cached = hot_backend->create_plan({configuration.length, signal::dsp::FftDirection::forward});
    if (!cached) {
      state.SkipWithError(std::string{cached.error().message()});
      return;
    }
    const auto counters = signal::dsp::benchmark_internal::inspect_mkl_plan_cache(*hot_backend);
    if (!counters || counters.value().misses != 1U || counters.value().hits != 1U || plan.value() != cached.value()) {
      state.SkipWithError("hot FFT plan cache evidence is inconsistent");
      return;
    }
    hot_plan = cached.value();
    const auto warm = hot_plan->process(input);
    if (!warm) {
      state.SkipWithError(std::string{warm.error().message()});
      return;
    }
  }

  for (auto _ : state) {
    static_cast<void>(_);
    const auto start = Clock::now();
    for (std::size_t batch = 0; batch < configuration.batch; ++batch) {
      if (configuration.cold_plan) {
        // 冷态样本使用全新 backend，避免 oneMKL plan cache 污染。
        const auto backend = signal::dsp::make_cpu_fft_backend();
        if (!backend) {
          state.SkipWithError(std::string{backend.error().message()});
          return;
        }
        const auto plan = backend.value()->create_plan({configuration.length, signal::dsp::FftDirection::forward});
        if (!plan) {
          state.SkipWithError(std::string{plan.error().message()});
          return;
        }
        const auto counters = signal::dsp::benchmark_internal::inspect_mkl_plan_cache(*backend.value());
        if (!counters || counters.value().misses != 1U || counters.value().hits != 0U ||
            counters.value().entries != 1U) {
          state.SkipWithError("cold FFT plan was contaminated by a cache hit");
          return;
        }
        const auto output = plan.value()->process(input);
        if (!output) {
          state.SkipWithError(std::string{output.error().message()});
          return;
        }
        benchmark::DoNotOptimize(output.value().bins.data());
      } else {
        const auto output = hot_plan->process(input);
        if (!output) {
          state.SkipWithError(std::string{output.error().message()});
          return;
        }
        benchmark::DoNotOptimize(output.value().bins.data());
      }
    }
    set_elapsed(state, start);
  }
  state.SetBytesProcessed(
      static_cast<std::int64_t>(configuration.length * sizeof(ComplexSample) * configuration.batch));
  state.counters["samples"] = kSamples;
  state.counters["plan_creation"] = configuration.cold_plan ? 1.0 : 0.0;
  state.counters["cache_hit"] = configuration.cold_plan ? 0.0 : 1.0;
  state.SetLabel(std::string{"backend="} + (hot_backend ? std::string{hot_backend->backend_id()} : "oneMKL-fresh") +
                 ";input=" + (configuration.real_input ? "real-zero-imag" : "complex") +
                 ";plan_creation=" + (configuration.cold_plan ? "true" : "false") +
                 ";cache_hit=" + (configuration.cold_plan ? "false" : "true"));
}

std::string window_name(signal::dsp::WindowKind window) {
  switch (window) {
  case signal::dsp::WindowKind::rectangular:
    return "rectangular";
  case signal::dsp::WindowKind::hann:
    return "hann";
  case signal::dsp::WindowKind::hamming:
    return "hamming";
  case signal::dsp::WindowKind::blackman:
    return "blackman";
  }
  return "unknown";
}

struct StftCase final {
  std::uint64_t fft_length{};
  std::uint64_t hop_length{};
  signal::dsp::WindowKind window{signal::dsp::WindowKind::hann};
  bool cuda{};
};

void stft_benchmark(benchmark::State& state, StftCase configuration) {
  const auto backend = configuration.cuda ? signal::dsp::make_cuda_fft_backend() : signal::dsp::make_cpu_fft_backend();
  if (!backend) {
    state.SkipWithMessage(std::string{"SKIP: "} + std::string{backend.error().message()});
    return;
  }
  const auto input =
      signal::benchmarks::ms02::complex_tone(static_cast<std::size_t>(configuration.fft_length * 8U), 71U);
  const signal::dsp::StftRequest request{50'000'000.0,
                                         1'425'000'000.0,
                                         configuration.fft_length,
                                         configuration.hop_length,
                                         configuration.window,
                                         signal::dsp::SpectrumSidedness::two_sided_shifted};
  const auto warm = signal::dsp::calculate_stft(*backend.value(), input.view(), request);
  if (!warm) {
    state.SkipWithError(std::string{warm.error().message()});
    return;
  }
  for (auto _ : state) {
    static_cast<void>(_);
    const auto start = Clock::now();
    const auto output = signal::dsp::calculate_stft(*backend.value(), input.view(), request);
    set_elapsed(state, start);
    if (!output) {
      state.SkipWithError(std::string{output.error().message()});
      return;
    }
    benchmark::DoNotOptimize(output.value().db_per_hz.data());
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(input.size() * sizeof(ComplexSample)));
  state.SetLabel(std::string{"backend="} + std::string{backend.value()->backend_id()} +
                 ";window=" + window_name(configuration.window));
}

std::vector<double> lowpass_taps(std::size_t tap_count) {
  std::vector<double> taps(tap_count);
  const auto middle = static_cast<double>(tap_count - 1U) / 2.0;
  for (std::size_t index = 0; index < tap_count; ++index) {
    const auto offset = static_cast<double>(index) - middle;
    const auto sinc =
        std::abs(offset) < 1.0e-12 ? 0.25 : std::sin(0.25 * std::numbers::pi * offset) / (std::numbers::pi * offset);
    const auto window =
        0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(tap_count - 1U));
    taps[index] = sinc * window;
  }
  const auto total = std::accumulate(taps.begin(), taps.end(), 0.0);
  for (auto& tap : taps)
    tap /= total;
  return taps;
}

struct ResamplerCase final {
  std::uint32_t numerator{};
  std::uint32_t denominator{};
  std::size_t tap_count{};
  std::size_t block_size{};
};

void resampler_benchmark(benchmark::State& state, ResamplerCase configuration) {
  const auto backend = signal::dsp::make_cpu_signal_kernel_backend();
  if (!backend) {
    state.SkipWithError(std::string{backend.error().message()});
    return;
  }
  const auto resampler = signal::dsp::make_resampler(backend.value());
  if (!resampler) {
    state.SkipWithError(std::string{resampler.error().message()});
    return;
  }
  const auto input = signal::benchmarks::ms02::complex_tone(configuration.block_size, 29U);
  const auto taps = lowpass_taps(configuration.tap_count);
  for (auto _ : state) {
    static_cast<void>(_);
    signal::dsp::FilterState filter_state;
    const auto start = Clock::now();
    const auto output = resampler.value()->process({configuration.numerator, configuration.denominator},
                                                   input.view().complex_values(), taps, filter_state);
    set_elapsed(state, start);
    if (!output) {
      state.SkipWithError(std::string{output.error().message()});
      return;
    }
    benchmark::DoNotOptimize(output.value().data());
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(configuration.block_size * sizeof(ComplexSample)));
  state.SetLabel(std::string{"backend="} + std::string{backend.value()->backend_id()});
}

#if defined(SIGNAL_STUDIO_BENCHMARK_HAVE_CUDA)

bool cuda_ready(benchmark::State& state, int minimum_devices = 1) {
  int devices{};
  const auto status = cudaGetDeviceCount(&devices);
  if (status != cudaSuccess || devices < minimum_devices) {
    state.SkipWithMessage("SKIP: CUDA device/driver does not satisfy this benchmark");
    return false;
  }
  return true;
}

void gpu_transfer_benchmark(benchmark::State& state, bool host_to_device) {
  if (!cuda_ready(state))
    return;
  constexpr std::size_t bytes = 16U * 1024U * 1024U;
  std::vector<std::byte> host(bytes);
  void* device{};
  if (cudaMalloc(&device, bytes) != cudaSuccess) {
    state.SkipWithMessage("SKIP: CUDA device allocation unavailable");
    return;
  }
  const auto warm = cudaMemcpy(host_to_device ? device : host.data(), host_to_device ? host.data() : device, bytes,
                               host_to_device ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost);
  if (warm != cudaSuccess) {
    cudaFree(device);
    state.SkipWithError(cudaGetErrorString(warm));
    return;
  }
  for (auto _ : state) {
    static_cast<void>(_);
    const auto start = Clock::now();
    const auto copied = cudaMemcpy(host_to_device ? device : host.data(), host_to_device ? host.data() : device, bytes,
                                   host_to_device ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost);
    const auto synchronized = cudaDeviceSynchronize();
    set_elapsed(state, start);
    if (copied != cudaSuccess || synchronized != cudaSuccess) {
      cudaFree(device);
      state.SkipWithError("CUDA transfer failed");
      return;
    }
  }
  cudaFree(device);
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
}

void gpu_plan_benchmark(benchmark::State& state) {
  if (!cuda_ready(state))
    return;
  for (auto _ : state) {
    static_cast<void>(_);
    cufftHandle plan{};
    const auto start = Clock::now();
    const auto created = cufftPlan1d(&plan, 4096, CUFFT_Z2Z, 1);
    if (created == CUFFT_SUCCESS)
      cufftDestroy(plan);
    set_elapsed(state, start);
    if (created != CUFFT_SUCCESS) {
      state.SkipWithError("cuFFT plan creation failed");
      return;
    }
  }
}

void gpu_stream_benchmark(benchmark::State& state) {
  if (!cuda_ready(state))
    return;
  constexpr std::size_t bytes = 4U * 1024U * 1024U;
  std::vector<std::byte> host(bytes);
  void* device{};
  if (cudaMalloc(&device, bytes) != cudaSuccess) {
    state.SkipWithMessage("SKIP: CUDA stream buffer unavailable");
    return;
  }
  for (auto _ : state) {
    static_cast<void>(_);
    cudaStream_t stream{};
    const auto start = Clock::now();
    const auto created = cudaStreamCreate(&stream);
    const auto copied =
        created == cudaSuccess ? cudaMemcpyAsync(device, host.data(), bytes, cudaMemcpyHostToDevice, stream) : created;
    const auto synchronized = copied == cudaSuccess ? cudaStreamSynchronize(stream) : copied;
    if (created == cudaSuccess)
      cudaStreamDestroy(stream);
    set_elapsed(state, start);
    if (created != cudaSuccess || copied != cudaSuccess || synchronized != cudaSuccess) {
      cudaFree(device);
      state.SkipWithError("CUDA stream operation failed");
      return;
    }
  }
  cudaFree(device);
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
}

void gpu_oom_benchmark(benchmark::State& state) {
  if (!cuda_ready(state))
    return;
  for (auto _ : state) {
    static_cast<void>(_);
    void* impossible{};
    const auto start = Clock::now();
    const auto allocated = cudaMalloc(&impossible, std::numeric_limits<std::size_t>::max() / 2U);
    set_elapsed(state, start);
    if (allocated == cudaSuccess) {
      cudaFree(impossible);
      state.SkipWithError("CUDA OOM audit unexpectedly allocated impossible size");
      return;
    }
    cudaGetLastError();
  }
  state.SetLabel("expected OOM recovered without terminating process");
}

void gpu_device_switch_benchmark(benchmark::State& state) {
  if (!cuda_ready(state, 2))
    return;
  int original{};
  cudaGetDevice(&original);
  const auto alternate = original == 0 ? 1 : 0;
  for (auto _ : state) {
    static_cast<void>(_);
    const auto start = Clock::now();
    const auto switched = cudaSetDevice(alternate);
    const auto restored = switched == cudaSuccess ? cudaSetDevice(original) : switched;
    set_elapsed(state, start);
    if (switched != cudaSuccess || restored != cudaSuccess) {
      state.SkipWithError("CUDA device switch failed");
      return;
    }
  }
}

#else

void gpu_unavailable(benchmark::State& state) {
  state.SkipWithMessage("SKIP: binary was built without the optional CUDA 12.4 adapter");
}

#endif

void register_fft_matrix() {
  constexpr std::array<std::size_t, 5U> lengths{1009U, 1024U, 4096U, 65'536U, 1'048'576U};
  for (const auto length : lengths) {
    evidence(benchmark::RegisterBenchmark(
        ("fft/complex/hot/" + std::to_string(length)).c_str(),
        [length](benchmark::State& state) { fft_benchmark(state, {length, 1U, false, false}); }));
    evidence(benchmark::RegisterBenchmark(
        ("fft/real_input/hot/" + std::to_string(length)).c_str(),
        [length](benchmark::State& state) { fft_benchmark(state, {length, 1U, true, false}); }));
  }
  for (const auto length : {1009U, 4096U, 1'048'576U}) {
    evidence(benchmark::RegisterBenchmark(
        ("fft/complex/cold_isolated/" + std::to_string(length)).c_str(),
        [length](benchmark::State& state) { fft_benchmark(state, {length, 1U, false, true}); }));
  }
  for (const auto length : {4096U, 65'536U}) {
    evidence(benchmark::RegisterBenchmark(
        ("fft/complex/batch8/hot/" + std::to_string(length)).c_str(),
        [length](benchmark::State& state) { fft_benchmark(state, {length, 8U, false, false}); }));
  }
}

void register_stft_matrix() {
  constexpr std::array<std::uint64_t, 3U> fft_lengths{1024U, 2048U, 4096U};
  constexpr std::array<signal::dsp::WindowKind, 3U> windows{
      signal::dsp::WindowKind::rectangular, signal::dsp::WindowKind::hann, signal::dsp::WindowKind::blackman};
  for (const auto fft_length : fft_lengths) {
    for (const auto overlap_percent : {0U, 50U, 75U}) {
      const auto hop = fft_length * static_cast<std::uint64_t>(100U - overlap_percent) / 100U;
      for (const auto window : windows) {
        const auto name = "stft/cpu/fft" + std::to_string(fft_length) + "/overlap" + std::to_string(overlap_percent) +
                          "/" + window_name(window);
        evidence(benchmark::RegisterBenchmark(name.c_str(), [fft_length, hop, window](benchmark::State& state) {
          stft_benchmark(state, {fft_length, hop, window, false});
        }));
      }
    }
  }
  for (const auto fft_length : fft_lengths) {
#if defined(SIGNAL_STUDIO_BENCHMARK_HAVE_CUDA)
    constexpr int cuda_repetitions = kSamples;
#else
    constexpr int cuda_repetitions = 1;
#endif
    evidence(benchmark::RegisterBenchmark(("stft/cuda/fft" + std::to_string(fft_length) + "/overlap50/hann").c_str(),
                                          [fft_length](benchmark::State& state) {
                                            stft_benchmark(state, {fft_length, fft_length / 2U,
                                                                   signal::dsp::WindowKind::hann, true});
                                          }),
             cuda_repetitions);
  }
}

void register_resampler_matrix() {
  constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 3U> ratios{std::pair{1U, 2U}, std::pair{2U, 1U},
                                                                           std::pair{3U, 2U}};
  for (const auto [numerator, denominator] : ratios) {
    for (const auto taps : {15U, 31U, 63U}) {
      for (const auto block : {1024U, 16'384U, 65'536U}) {
        const auto name = "resampler/cpu/" + std::to_string(numerator) + "over" + std::to_string(denominator) +
                          "/taps" + std::to_string(taps) + "/block" + std::to_string(block);
        evidence(benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
          resampler_benchmark(state, {numerator, denominator, taps, block});
        }));
      }
    }
  }
}

void full_x310_index_benchmark(benchmark::State& state) {
#if SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE
  signal::data::SignalDescriptor descriptor;
  descriptor.signal_kind = signal::data::SignalKind::complex;
  descriptor.scalar_type = signal::data::ScalarType::int16;
  descriptor.component_layout = signal::data::ComponentLayout::interleaved;
  descriptor.component_order = signal::data::ComponentOrder::iq;
  descriptor.endianness = signal::data::Endianness::little;
  descriptor.sample_rate_hz = 50'000'000.0;
  descriptor.center_frequency_hz = 1'425'000'000.0;
  constexpr std::uint64_t expected_bytes = 4'004'031'888ULL;
  constexpr std::uint64_t expected_frames = expected_bytes / 4U;
  for (auto _ : state) {
    static_cast<void>(_);
    const auto start = Clock::now();
    const auto indexed = signal::data::build_full_sc16_index(std::filesystem::path{SIGNAL_STUDIO_MS02_PERF_RECORDING},
                                                             descriptor, 1U << 20U, 64U * 1024U * 1024U);
    if (!indexed) {
      state.SkipWithError(std::string{indexed.error().message()});
      return;
    }
    if (indexed.value().bytes_read != expected_bytes || indexed.value().frames_indexed != expected_frames ||
        indexed.value().bins.size() != 955U) {
      state.SkipWithError("real X310 full production index did not cover the complete recording");
      return;
    }
    set_elapsed(state, start);
    state.SetBytesProcessed(static_cast<std::int64_t>(indexed.value().bytes_read));
    state.counters["frames"] = static_cast<double>(indexed.value().frames_indexed);
    state.counters["index_bins"] = static_cast<double>(indexed.value().bins.size());
  }
#else
  state.SkipWithMessage("SKIP: exact real X310 recording is unavailable");
#endif
}

void register_gpu_matrix() {
#if defined(SIGNAL_STUDIO_BENCHMARK_HAVE_CUDA)
  evidence(benchmark::RegisterBenchmark("gpu/h2d/16MiB",
                                        [](benchmark::State& state) { gpu_transfer_benchmark(state, true); }));
  evidence(benchmark::RegisterBenchmark("gpu/d2h/16MiB",
                                        [](benchmark::State& state) { gpu_transfer_benchmark(state, false); }));
  evidence(benchmark::RegisterBenchmark("gpu/cufft_plan/4096", gpu_plan_benchmark));
  evidence(benchmark::RegisterBenchmark("gpu/stream/h2d/4MiB", gpu_stream_benchmark));
  evidence(benchmark::RegisterBenchmark("gpu/oom/recovery", gpu_oom_benchmark));
  evidence(benchmark::RegisterBenchmark("gpu/device_switch", gpu_device_switch_benchmark));
#else
  for (const auto* name : {"gpu/h2d/16MiB", "gpu/d2h/16MiB", "gpu/cufft_plan/4096", "gpu/stream/h2d/4MiB",
                           "gpu/oom/recovery", "gpu/device_switch"}) {
    evidence(benchmark::RegisterBenchmark(name, gpu_unavailable), 1);
  }
#endif
}

std::string cuda_context() {
#if defined(SIGNAL_STUDIO_BENCHMARK_HAVE_CUDA)
  int count{};
  int driver{};
  int runtime{};
  cudaGetDeviceCount(&count);
  cudaDriverGetVersion(&driver);
  cudaRuntimeGetVersion(&runtime);
  if (count <= 0)
    return "CUDA 12.4 adapter compiled; device unavailable";
  cudaDeviceProp properties{};
  cudaGetDeviceProperties(&properties, 0);
  return std::string{properties.name} + ";driver=" + std::to_string(driver) + ";runtime=" + std::to_string(runtime) +
         ";devices=" + std::to_string(count);
#else
  return "SKIP: optional CUDA adapter not compiled";
#endif
}

} // namespace

int main(int argc, char** argv) {
  register_fft_matrix();
  register_stft_matrix();
  register_resampler_matrix();
  evidence(benchmark::RegisterBenchmark("recording/x310/full_production_index", full_x310_index_benchmark),
           kRecordingIndexSamples);
  register_gpu_matrix();

  const auto cpu = signal::dsp::make_cpu_fft_backend();
  benchmark::AddCustomContext(
      "method", "Google Benchmark 1.9.5; algorithm matrix: 30 independent repetitions; full real recording index: "
                "Release=3 independent repetitions, Debug=1 structural run; P50/P95/max/normal-approximation 95% CI");
  benchmark::AddCustomContext("dependency_lock", "vcpkg 82b6bc886d7b0f8342e34babc2e0b8943f79b0e1; oneMKL "
                                                 "2025.2.0#1; benchmark 1.9.5; libsamplerate 0.2.2#1");
  benchmark::AddCustomContext("recording_sha256", SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256);
  benchmark::AddCustomContext("recording_scope", "real X310 recording + logical repetition mapping to exactly "
                                                 "100,000,000,000 bytes; no physical 100 GB throughput claim");
  benchmark::AddCustomContext("cpu_backend",
                              cpu ? std::string{cpu.value()->backend_id()} : std::string{cpu.error().message()});
  benchmark::AddCustomContext("cuda_device_driver", cuda_context());
  benchmark::AddCustomContext("background_load", "CTest RUN_SERIAL; no synthetic background load; ordinary host "
                                                 "processes unchanged");
  benchmark::AddCustomContext("host_threads", std::to_string(std::thread::hardware_concurrency()));

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv))
    return EXIT_FAILURE;
  FailureTrackingConsoleReporter reporter;
  const auto matched = benchmark::RunSpecifiedBenchmarks(&reporter);
  benchmark::Shutdown();
  return matched == 0U || reporter.error_occurred() ? EXIT_FAILURE : EXIT_SUCCESS;
}
