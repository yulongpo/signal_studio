#include "ms02_benchmark_support.hpp"
#include "signal_studio/dsp/browse_performance.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>

#include <Psapi.h>
#endif

namespace {
using namespace signal;
using namespace signal::benchmarks::ms02;
using namespace signal::compute;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename T> void require(const core::Result<T>& result, std::string_view message) {
  require(static_cast<bool>(result), message);
}

void require(const core::Status& status, std::string_view message) {
  require(static_cast<bool>(status), message);
}

std::shared_ptr<dsp::IFftBackend> fft_backend() {
  const auto backend = dsp::make_cpu_fft_backend();
  require(backend, "性能验收要求 oneMKL 生产 FFT 后端可用");
  return backend.value();
}

class FailingAllocator final : public IMemoryAllocator {
public:
  [[nodiscard]] MemoryKind memory_kind() const noexcept override {
    return MemoryKind::pinned_host;
  }

  [[nodiscard]] core::Result<void*> allocate(std::uint64_t, std::uint64_t) override {
    return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::unavailable}, "注入可恢复的分配失败");
  }

  void release(void*, std::uint64_t, std::uint64_t) noexcept override {}
};

class CorruptingBackend final : public IComputeBackend {
public:
  explicit CorruptingBackend(std::shared_ptr<IComputeBackend> delegate) : delegate_(std::move(delegate)) {}

  [[nodiscard]] BackendCapabilities capabilities() const noexcept override {
    return delegate_->capabilities();
  }

  [[nodiscard]] bool supports(const Workload& workload) const noexcept override {
    return delegate_->supports(workload);
  }

  [[nodiscard]] core::Status execute_buffer_copy(const BufferCopyRequest& request,
                                                 std::uint32_t worker_threads) const override {
    const auto status = delegate_->execute_buffer_copy(request, worker_threads);
    if (status && !request.output.empty()) {
      request.output.front() += 0.5;
    }
    return status;
  }

private:
  std::shared_ptr<IComputeBackend> delegate_;
};

class RecordingDspOperation final : public IComputeOperation {
public:
  explicit RecordingDspOperation(std::string operation, bool fail_cuda = false)
      : operation_(std::move(operation)), fail_cuda_(fail_cuda) {}

  [[nodiscard]] std::string_view operation() const noexcept override {
    return operation_;
  }

  [[nodiscard]] bool supports(BackendKind kind) const noexcept override {
    return kind == BackendKind::cpu_scalar || kind == BackendKind::cuda;
  }

  [[nodiscard]] core::Status execute(BackendKind kind, std::uint32_t) override {
    executions_.push_back(kind);
    if (kind == BackendKind::cuda && fail_cuda_) {
      return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::unavailable},
                                   "注入 DSP CUDA 执行失败");
    }
    return core::Status::success();
  }

  [[nodiscard]] core::Result<ConsistencyMetrics> consistency(BackendKind) const override {
    return ConsistencyMetrics{0.0, 0.0, 16U, false};
  }

  [[nodiscard]] std::span<const BackendKind> executions() const noexcept {
    return executions_;
  }

private:
  std::string operation_;
  bool fail_cuda_{};
  std::vector<BackendKind> executions_;
};

Workload copy_workload(std::size_t value_count, bool interactive = false, bool require_cuda = false) {
  return {"buffer-copy",
          static_cast<std::uint64_t>(value_count * sizeof(double)),
          static_cast<std::uint64_t>(value_count * sizeof(double) * 2U),
          static_cast<std::uint64_t>(value_count),
          interactive,
          require_cuda,
          true};
}

std::vector<double> reference_values(std::size_t value_count) {
  std::vector<double> values(value_count);
  for (std::size_t index = 0; index < values.size(); ++index) {
    values[index] = std::sin(static_cast<double>(index) * 0.003) + 0.25 * std::cos(static_cast<double>(index) * 0.011);
  }
  return values;
}

data::SignalDescriptor performance_descriptor() {
  data::SignalDescriptor descriptor;
  descriptor.signal_kind = data::SignalKind::complex;
  descriptor.scalar_type = data::ScalarType::int16;
  descriptor.component_layout = data::ComponentLayout::interleaved;
  descriptor.component_order = data::ComponentOrder::iq;
  descriptor.endianness = data::Endianness::little;
  descriptor.sample_rate_hz = 50'000'000.0;
  descriptor.center_frequency_hz = 1'425'000'000.0;
  descriptor.requested_sample_range = range(4096U);
  return descriptor;
}

std::shared_ptr<dsp::LogicalRecordingSource> recording_source(std::uint64_t logical_bytes = 100'000'000'000ULL) {
  const auto source = dsp::LogicalRecordingSource::open(
      std::filesystem::path{SIGNAL_STUDIO_MS02_PERF_RECORDING}, performance_descriptor(),
      "x310-1425MHz-50MSps-sha256-" SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256, logical_bytes,
      256ULL * 1024ULL * 1024ULL);
  require(source, "无法打开用户指定的真实 X310 SC16 录制");
  return source.value();
}

dsp::BrowseViewport initial_viewport(const dsp::LogicalRecordingSource& source) {
  const auto logical_range =
      data::SampleRange::from_count(0U, std::min<std::uint64_t>(16'384U, source.plan().logical_frames));
  require(logical_range, "无法建立初始逻辑视窗");
  return {logical_range.value(), 512U, 32U};
}

dsp::BrowseViewport distributed_viewport(const dsp::LogicalRecordingSource& source, std::uint64_t numerator,
                                         std::uint64_t denominator) {
  constexpr std::uint64_t window_frames = 16'384U;
  require(denominator != 0U && numerator <= denominator && source.plan().logical_frames >= window_frames,
          "分布式视窗参数无效");
  const auto maximum_begin = source.plan().logical_frames - window_frames;
  const auto quotient = maximum_begin / denominator;
  const auto remainder = maximum_begin % denominator;
  const auto begin = quotient * numerator + remainder * numerator / denominator;
  const auto logical_range = data::SampleRange::from_count(begin, window_frames);
  require(logical_range, "无法建立分布式逻辑视窗");
  return {logical_range.value(), 512U, 32U};
}

bool same_viewport(const dsp::BrowseViewport& left, const dsp::BrowseViewport& right) {
  return left.logical_range == right.logical_range && left.pixel_width == right.pixel_width &&
         left.spectrogram_rows == right.spectrogram_rows;
}

std::filesystem::path temporary_cache(std::string_view name) {
  return std::filesystem::temp_directory_path() / (std::string{"signal-studio-ms02-"} + std::string{name} + "-" +
                                                   std::to_string(Clock::now().time_since_epoch().count()));
}

struct ProcessMemorySnapshot final {
  std::uint64_t current_working_set_bytes{};
  std::uint64_t peak_working_set_bytes{};
};

ProcessMemorySnapshot process_memory_snapshot() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters)) != 0,
          "无法读取真实进程 Working Set");
  return {static_cast<std::uint64_t>(counters.WorkingSetSize), static_cast<std::uint64_t>(counters.PeakWorkingSetSize)};
#else
  return {};
#endif
}

void test_perf_001() {
  dsp::BrowseInteractionSequencer interaction;
  const auto summary = measure(60U, 4096U, [&](std::size_t index) {
    const auto logical_range = data::SampleRange::from_count(index * 16U, 4096U);
    if (!logical_range)
      return false;
    const auto feedback = interaction.issue({logical_range.value(), 512U, 32U});
    return feedback.visible && feedback.status_text == "正在细化" && feedback.generation == index + 1U;
  });
  require(summary && summary.value().p95 <= std::chrono::milliseconds{50}, "真实浏览交互反馈代际发布 P95 超过 50 ms");
  std::array<PerformanceSample, 29U> too_few{};
  require(!summarize_performance(too_few), "少于 30 个样本必须拒绝");
  std::cout << "HEADLESS_PLATFORM_ONLY: 可见 QWidget 反馈由 performance.qt.NFR-PERF-001 专项验收\n";
}

void test_perf_002() {
  dsp::BrowseInteractionSequencer interaction;
  for (std::uint64_t frame = 0; frame < 120U; ++frame) {
    const auto logical_range = data::SampleRange::from_count(frame * 32U, 4096U);
    require(logical_range, "连续交互视窗无效");
    const auto feedback = interaction.issue({logical_range.value(), 512U, 32U});
    auto product_frame = std::make_shared<dsp::AtomicBrowseFrame>();
    product_frame->generation = feedback.generation;
    product_frame->viewport = feedback.viewport;
    product_frame->time_envelope.assign(512U, static_cast<float>(frame));
    product_frame->spectrum_db_per_hz.assign(512U, -100.0F);
    product_frame->spectrogram_db_per_hz.assign(512U * 8U, -110.0F);
    product_frame->spectrogram_columns = 512U;
    product_frame->spectrogram_rows = 8U;
    product_frame->source_scope = "无界面平台交互路径";
    product_frame->complete = true;
    require(interaction.publish(std::move(product_frame)), "连续交互未原子发布当前代际");
  }
  require(interaction.frame() && interaction.frame()->generation == 120U, "连续交互最终代际不一致");
  std::cout << "HEADLESS_PLATFORM_ONLY: 实际 QWidget 图谱 paintEvent FPS 由 performance.qt.NFR-PERF-002 专项验收\n";
}

void test_perf_003() {
  const auto source = recording_source();
  const auto cache = temporary_cache("high-resolution");
  auto session = dsp::BrowsePerformanceSession::create(source, fft_backend(), cache);
  require(session, "真实浏览缓存会话创建失败");
  const auto viewport = initial_viewport(*source);
  require(session.value()->build_frame(viewport, 1U), "真实录制高分辨率三图准备失败");
  const auto summary = measure(60U, 512U * sizeof(float), [&](std::size_t index) {
    const auto restored = session.value()->restore_cached_frame(viewport, index + 2U);
    return restored && restored.value()->cache_hit && restored.value()->complete;
  });
  std::error_code remove_error;
  std::filesystem::remove_all(cache, remove_error);
  require(summary && summary.value().p95 <= std::chrono::milliseconds{500}, "真实高分辨率三图缓存替换 P95 超过 500 ms");
}

void test_perf_004() {
  const auto start = Clock::now();
  const auto source = recording_source();
  const auto viewport = initial_viewport(*source);
  const auto initial =
      source->read_logical(viewport.logical_range, viewport.logical_range.size() * source->plan().frame_bytes);
  dsp::BrowseInteractionSequencer interaction;
  const auto feedback = interaction.issue(viewport);
  const auto elapsed = Clock::now() - start;
  require(initial && source->plan().navigation_ready && source->plan().real_recording_input &&
              source->plan().logical_repeat_mapping && source->plan().logical_file_bytes == 100'000'000'000ULL &&
              initial.value().samples.size() == viewport.logical_range.size() && feedback.visible &&
              source->plan().estimate_source.find("100 GB（100,000,000,000 字节）逻辑重复映射") != std::string::npos,
          "真实录制输入 + 100 GB（100,000,000,000 字节）逻辑映射的读取计划、导航或初始视窗不完整");
  require(elapsed < std::chrono::seconds{1},
          "100 GB（100,000,000,000 字节）逻辑映射确认导入后的读取计划/导航/初始视窗超过 1 秒");

  constexpr std::uint64_t boundary_frames = 16U;
  const auto physical_boundary =
      data::SampleRange::from_count(source->plan().physical_frames - boundary_frames / 2U, boundary_frames);
  const auto physical_tail =
      data::SampleRange::from_count(source->plan().physical_frames - boundary_frames / 2U, boundary_frames / 2U);
  const auto physical_head = data::SampleRange::from_count(0U, boundary_frames / 2U);
  require(physical_boundary && physical_tail && physical_head, "无法建立物理重复边界视窗");
  const auto crossed = source->read_logical(physical_boundary.value(), boundary_frames * source->plan().frame_bytes);
  const auto tail = source->read_logical(physical_tail.value(), boundary_frames / 2U * source->plan().frame_bytes);
  const auto head = source->read_logical(physical_head.value(), boundary_frames / 2U * source->plan().frame_bytes);
  require(crossed && tail && head, "逻辑重复映射无法跨越物理文件尾");
  const auto crossed_values = crossed.value().samples.view().complex_values();
  const auto tail_values = tail.value().samples.view().complex_values();
  const auto head_values = head.value().samples.view().complex_values();
  require(std::equal(tail_values.begin(), tail_values.end(), crossed_values.begin()) &&
              std::equal(head_values.begin(), head_values.end(),
                         crossed_values.begin() + static_cast<std::ptrdiff_t>(tail_values.size())),
          "逻辑重复映射在物理文件尾拼接了错误样本");

  const auto final_frame = data::SampleRange::from_count(source->plan().logical_frames - 1U, 1U);
  const auto beyond_eof = data::SampleRange::from_count(source->plan().logical_frames, 1U);
  require(final_frame && beyond_eof, "无法建立逻辑 EOF 边界视窗");
  require(source->read_logical(final_frame.value(), source->plan().frame_bytes), "精确逻辑最后一帧读取失败");
  require(!source->read_logical(beyond_eof.value(), source->plan().frame_bytes),
          "逻辑读取不得越过 100,000,000,000 字节 EOF");
  require(!source->read_logical(physical_boundary.value(),
                                boundary_frames * source->plan().frame_bytes - source->plan().frame_bytes),
          "逻辑读取不得超过显式最大读取字节上限");
}

void test_perf_005() {
  const auto directory = temporary_cache("recovery");
  const auto source = recording_source();
  const auto viewport = initial_viewport(*source);
  {
    auto session = dsp::BrowsePerformanceSession::create(source, fft_backend(), directory);
    require(session && session.value()->build_frame(viewport, 1U), "真实录制首屏三图缓存准备失败");
  }
  const auto summary = measure(30U, viewport.pixel_width * sizeof(float), [&](std::size_t index) {
    auto recovered = dsp::BrowsePerformanceSession::create(source, fft_backend(), directory);
    if (!recovered)
      return false;
    const auto frame = recovered.value()->restore_cached_frame(viewport, index + 2U);
    return frame && frame.value()->cache_hit && frame.value()->complete &&
           frame.value()->source_scope == "真实录制输入 + 100 GB（100,000,000,000 字节）逻辑重复映射";
  });
  std::error_code remove_error;
  std::filesystem::remove_all(directory, remove_error);
  require(summary && summary.value().p95 <= std::chrono::seconds{3},
          "有效缓存的 100 GB（100,000,000,000 字节）逻辑映射项目恢复真实首屏 P95 超过 3 秒");
}

void test_perf_006() {
  const auto before = process_memory_snapshot().current_working_set_bytes;
  const auto exercise_browse = [&](std::uint64_t logical_bytes, std::string_view cache_name) {
    auto source = recording_source(logical_bytes);
    const auto cache = temporary_cache(cache_name);
    auto session = dsp::BrowsePerformanceSession::create(source, fft_backend(), cache);
    require(session, "标准三图浏览内存专项无法创建会话");
    dsp::BrowseInteractionSequencer interaction;
    auto peak_rss = process_memory_snapshot().peak_working_set_bytes;
    constexpr std::uint64_t samples = 48U;
    for (std::uint64_t sample = 0U; sample < samples; ++sample) {
      const auto view = distributed_viewport(*source, sample + 1U, samples + 1U);
      const auto feedback = interaction.issue(view);
      const auto frame = session.value()->build_frame(view, feedback.generation);
      require(frame && interaction.publish(frame.value()) && interaction.frame() &&
                  same_viewport(interaction.frame()->viewport, feedback.viewport),
              "标准三图浏览工作负载未发布当前视窗");
      // Windows 的 PeakWorkingSetSize 覆盖同步 build_frame() 内部临时读取、
      // PSD 与 STFT 缓冲区的生命周期，不会因函数返回后释放而漏掉瞬时峰值。
      peak_rss = std::max(peak_rss, process_memory_snapshot().peak_working_set_bytes);
    }
    std::error_code remove_error;
    std::filesystem::remove_all(cache, remove_error);
    return std::pair{peak_rss, source->plan().bounded_working_set_bytes};
  };

  // 用户录制本身约 4.004 GB，因此较小逻辑映射仍覆盖完整物理源；两轮均跨越
  // 48 个远距离视窗，生成时域、频谱、STFT 三图并写入内存/磁盘缓存。
  const auto [small_peak_rss, small_bound] = exercise_browse(8'000'000'000ULL, "rss-small");
  const auto [large_peak_rss, large_bound] = exercise_browse(100'000'000'000ULL, "rss-large");
  const auto peak_growth = large_peak_rss > small_peak_rss ? large_peak_rss - small_peak_rss : 0U;
  require(std::max(small_peak_rss, large_peak_rss) < 4ULL * 1024ULL * 1024ULL * 1024ULL &&
              peak_growth < 256ULL * 1024ULL * 1024ULL && large_bound == small_bound &&
              std::max(small_peak_rss, large_peak_rss) >= before,
          "标准三图持续浏览采样峰值 Working Set 超过 4 GiB 或随逻辑文件大小线性增长");
  std::cout << "NFR-PERF-006 sampled_peak_small_bytes=" << small_peak_rss
            << " sampled_peak_100GB_bytes=" << large_peak_rss << " growth_bytes=" << peak_growth << '\n';
}

void test_perf_007() {
  const auto discovered = discover_compute_backends();
  require(!discovered.empty(), "生产 IComputeBackend 探测为空");
  const auto runtime = ComputeRuntime::create(discovered);
  require(runtime, "默认计算运行时创建失败");
  const auto hardware = std::max(1U, std::thread::hardware_concurrency());
  require(runtime.value()->worker_threads() >= 1U &&
              (hardware == 1U || runtime.value()->worker_threads() <= hardware - 1U),
          "默认线程未为 UI/系统保留逻辑核");
  require(!ComputeRuntime::create(discover_compute_backends(), 0U), "零线程限制必须拒绝");
}

void test_perf_008() {
  const auto source = recording_source();
  const auto cache = temporary_cache("overview");
  auto session = dsp::BrowsePerformanceSession::create(source, fft_backend(), cache);
  require(session, "真实录制采样概览会话创建失败");
  const auto start = Clock::now();
  const auto overview = session.value()->build_sampling_overview();
  const auto elapsed = Clock::now() - start;
  std::error_code remove_error;
  std::filesystem::remove_all(cache, remove_error);
  require(overview && overview.value().complete && overview.value().label == "采样概览" &&
              overview.value().source_scope == "真实录制输入 + 100 GB（100,000,000,000 字节）逻辑重复映射" &&
              overview.value().physical_samples_read > 0U &&
              overview.value().logical_samples_represented == source->plan().logical_frames,
          "真实 X310 数据采样概览未生成、未标记或状态不完整");
  require(elapsed < std::chrono::seconds{5}, "真实 X310 数据采样概览超过 5 秒");
}

void test_perf_009() {
#if defined(_DEBUG)
  auto descriptor = performance_descriptor();
  descriptor.requested_sample_range = {};
  const auto result = data::build_full_sc16_index(std::filesystem::path{SIGNAL_STUDIO_MS02_PERF_RECORDING}, descriptor,
                                                  1U << 20U, 64U * 1024U * 1024U);
  if (result) {
    std::cout << "NFR-PERF-009 Debug full-index bytes=" << result.value().bytes_read
              << " frames=" << result.value().frames_indexed << " bins=" << result.value().bins.size() << '\n';
  } else {
    std::cerr << "NFR-PERF-009 Debug error=" << result.error().message()
              << " diagnostic=" << result.error().diagnostic() << '\n';
  }
  require(result && result.value().bytes_read == 4'004'031'888ULL &&
              result.value().frames_indexed == 1'001'007'972ULL && result.value().bins.size() == 955U,
          "Debug 全文件索引结构验证未覆盖真实 X310 全部帧");
#else
  const auto result = measure_same_disk_throughput(std::filesystem::path{SIGNAL_STUDIO_MS02_PERF_RECORDING},
                                                   performance_descriptor(), 1U << 20U, 64U * 1024U * 1024U, 3U);
  if (result) {
    std::cout << "NFR-PERF-009 baseline_p95_Bps=" << result.value().baseline_p95_bytes_per_second
              << " indexed_p95_Bps=" << result.value().indexed_p95_bytes_per_second << " ratio=" << result.value().ratio
              << " rounds=" << result.value().sample_count << " frames=" << result.value().indexed_sample_count
              << " bins=" << result.value().index_bin_count << '\n';
  } else {
    std::cerr << "NFR-PERF-009 error=" << result.error().message() << " diagnostic=" << result.error().diagnostic()
              << '\n';
  }
  require(result && result.value().sample_count == 3U && result.value().indexed_sample_count == 1'001'007'972ULL &&
              result.value().index_bin_count == 955U && result.value().ratio >= 0.60,
          "真实 X310 完整生产索引未覆盖全文件，或 Release 同盘交替预热 P95 吞吐低于顺序读取基线 60%");
#endif
}

void test_perf_010() {
  const auto source = recording_source();
  const auto cache = temporary_cache("atomic-three-view");
  auto session = dsp::BrowsePerformanceSession::create(source, fft_backend(), cache);
  require(session, "真实三图会话创建失败");
  std::array<dsp::BrowseViewport, 60U> hot_viewports;
  for (std::size_t index = 0U; index < hot_viewports.size(); ++index) {
    hot_viewports[index] = distributed_viewport(*source, index + 1U, hot_viewports.size() + 1U);
    const auto prepared = session.value()->build_frame(hot_viewports[index], index + 1U);
    require(prepared && prepared.value()->complete, "真实三图多视窗缓存准备失败");
  }
  dsp::BrowseInteractionSequencer interaction;
  const auto stale_feedback = interaction.issue(hot_viewports[0]);
  const auto stale_frame = session.value()->restore_cached_frame(hot_viewports[0], stale_feedback.generation);
  require(stale_frame, "无法准备旧代际三图结果");
  const auto current_feedback = interaction.issue(hot_viewports[1]);
  require(!interaction.publish(stale_frame.value()) &&
              same_viewport(interaction.feedback().viewport, current_feedback.viewport),
          "视窗变化后必须拒绝旧代际三图结果");

  const auto hot = measure(60U, 3U * 512U * sizeof(float), [&](std::size_t index) {
    const auto& viewport = hot_viewports[index];
    const auto feedback = interaction.issue(viewport);
    const auto restored = session.value()->restore_cached_frame(viewport, feedback.generation);
    return restored && interaction.publish(restored.value()) && interaction.frame() &&
           interaction.frame()->generation == feedback.generation &&
           same_viewport(interaction.frame()->viewport, feedback.viewport) && interaction.frame()->complete;
  });
  const auto cold =
      measure(30U, initial_viewport(*source).logical_range.size() * source->plan().frame_bytes, [&](std::size_t index) {
        const auto viewport = distributed_viewport(*source, index * 2U + 1U, 60U);
        const auto feedback = interaction.issue(viewport);
        const auto frame = session.value()->build_frame(viewport, feedback.generation, false);
        return frame && interaction.publish(frame.value()) && interaction.frame() &&
               interaction.frame()->generation == feedback.generation &&
               same_viewport(interaction.frame()->viewport, feedback.viewport) && interaction.frame()->complete;
      });
  std::error_code remove_error;
  std::filesystem::remove_all(cache, remove_error);
  require(hot && cold && hot.value().p95 <= std::chrono::milliseconds{150} &&
              cold.value().p95 <= std::chrono::seconds{1},
          "真实录制视窗变化后的三图同代际原子结果延迟超限");
}

void test_perf_011() {
  ViewActivityGate gate;
  const auto view_backend = fft_backend();
  const auto shared_backend = fft_backend();
  const auto tone = complex_tone(65'536U);
  std::atomic_bool failed{};
  std::atomic_uint64_t actual_view_fft_count{};
  std::atomic_bool shared_finished{};

  std::thread view_producer([&] {
    auto lease = gate.acquire_view_activity();
    if (!lease) {
      failed.store(true);
      return;
    }
    while (lease->active()) {
      if (!view_backend->execute({65'536U, dsp::FftDirection::forward}, tone.view().complex_values())) {
        failed.store(true);
        break;
      }
      if (!lease->mark_activity()) {
        break;
      }
      actual_view_fft_count.fetch_add(1U);
    }
  });
  std::thread shared_producer([&] {
    for (std::size_t iteration = 0; iteration < 40U; ++iteration) {
      if (!shared_backend->execute({65'536U, dsp::FftDirection::forward}, tone.view().complex_values())) {
        failed.store(true);
        break;
      }
      gate.mark_shared_activity();
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    shared_finished.store(true);
  });

  const auto producer_deadline = Clock::now() + std::chrono::seconds{2};
  while (actual_view_fft_count.load() < 2U && Clock::now() < producer_deadline) {
    std::this_thread::yield();
  }
  if (actual_view_fft_count.load() < 2U) {
    failed.store(true);
  }
  const auto hide_start = Clock::now();
  gate.set_visible(false);
  const auto hide_latency = Clock::now() - hide_start;
  const auto drained = gate.wait_for_view_idle(std::chrono::milliseconds{500});
  const auto count_after_hide = actual_view_fft_count.load();
  std::this_thread::sleep_for(std::chrono::milliseconds{550});
  view_producer.join();
  shared_producer.join();

  require(!failed.load() && hide_latency < std::chrono::milliseconds{50} && drained &&
              actual_view_fft_count.load() == count_after_hide && gate.view_activity_count() == count_after_hide &&
              shared_finished.load() && gate.shared_activity_count() == 40U && !gate.acquire_view_activity(),
          "隐藏视图后仍有实际 FFT/观察器活动，或共享 FFT 任务被误伤");
  gate.set_visible(true);
  auto resumed = gate.acquire_view_activity();
  require(resumed && resumed->active(), "视图重新显示后未恢复新的生命周期");
  const auto same_thread_hide_start = Clock::now();
  gate.set_visible(false);
  require(Clock::now() - same_thread_hide_start < std::chrono::milliseconds{50} && !resumed->active() &&
              !gate.wait_for_view_idle(std::chrono::milliseconds{1}),
          "持有租约的同线程隐藏调用发生阻塞或代际取消未生效");
  resumed.reset();
  require(gate.wait_for_view_idle(std::chrono::milliseconds{10}), "租约释放后视图生产者未排空");
}

void verify_real_backend_execution(const std::shared_ptr<IComputeBackend>& backend, std::span<const double> input) {
  const auto capability = backend->capabilities();
  if (!capability.available) {
    return;
  }
  require(capability.supports_buffer_copy, "可用生产计算后端未声明真实缓冲区操作能力");
  const auto workload = copy_workload(input.size(), capability.kind == BackendKind::cpu_scalar);
  require(backend->supports(workload), "生产后端能力匹配拒绝其真实操作");
  std::vector<double> output(input.size(), std::numeric_limits<double>::quiet_NaN());
  const auto execution = backend->execute_buffer_copy({input, output}, std::max(1U, capability.logical_threads));
  require(execution, "生产计算后端真实执行失败");
  const auto metrics = measure_consistency(input, output);
  require(metrics && metrics.value().maximum_absolute_error == 0.0 && metrics.value().rms_error == 0.0 &&
              metrics.value().value_count == input.size(),
          "生产计算后端输出与标量参考不一致");
}

#if defined(_WIN32)
class CudaRuntimeDeviceApi final {
public:
  using GetDeviceCount = int(__cdecl*)(int*);
  using GetDevice = int(__cdecl*)(int*);
  using SetDevice = int(__cdecl*)(int);

  CudaRuntimeDeviceApi() {
    module_ = GetModuleHandleW(L"cudart64_12.dll");
    if (module_ == nullptr) {
      module_ = LoadLibraryW(L"cudart64_12.dll");
      owns_module_ = module_ != nullptr;
    }
    if (module_ != nullptr) {
      get_device_count_ = reinterpret_cast<GetDeviceCount>(GetProcAddress(module_, "cudaGetDeviceCount"));
      get_device_ = reinterpret_cast<GetDevice>(GetProcAddress(module_, "cudaGetDevice"));
      set_device_ = reinterpret_cast<SetDevice>(GetProcAddress(module_, "cudaSetDevice"));
    }
  }

  CudaRuntimeDeviceApi(const CudaRuntimeDeviceApi&) = delete;
  CudaRuntimeDeviceApi& operator=(const CudaRuntimeDeviceApi&) = delete;

  ~CudaRuntimeDeviceApi() {
    if (owns_module_) {
      FreeLibrary(module_);
    }
  }

  [[nodiscard]] bool ready() const noexcept {
    return get_device_count_ != nullptr && get_device_ != nullptr && set_device_ != nullptr;
  }

  [[nodiscard]] int get_device_count(int* count) const noexcept {
    return get_device_count_(count);
  }

  [[nodiscard]] int get_device(int* device) const noexcept {
    return get_device_(device);
  }

  [[nodiscard]] int set_device(int device) const noexcept {
    return set_device_(device);
  }

private:
  HMODULE module_{};
  bool owns_module_{};
  GetDeviceCount get_device_count_{};
  GetDevice get_device_{};
  SetDevice set_device_{};
};

class CudaCallerDeviceRestore final {
public:
  CudaCallerDeviceRestore(const CudaRuntimeDeviceApi& api, int device) noexcept : api_(api), device_(device) {}
  CudaCallerDeviceRestore(const CudaCallerDeviceRestore&) = delete;
  CudaCallerDeviceRestore& operator=(const CudaCallerDeviceRestore&) = delete;
  ~CudaCallerDeviceRestore() {
    static_cast<void>(api_.set_device(device_));
  }

private:
  const CudaRuntimeDeviceApi& api_;
  int device_{};
};

void verify_cuda_caller_device_restoration() {
  if (!dsp::cuda_fft_available()) {
    return;
  }
  CudaRuntimeDeviceApi api;
  require(api.ready(), "CUDA 12.4 运行时缺少设备上下文 API");
  int device_count{};
  require(api.get_device_count(&device_count) == 0, "CUDA 设备数量查询失败");

  int original_device{};
  require(api.get_device(&original_device) == 0, "CUDA 调用线程原设备查询失败");
  const auto caller_device = device_count > 1 && original_device == 0 ? 1 : original_device;
  require(api.set_device(caller_device) == 0, "CUDA 多设备回归无法选择调用线程设备");
  const CudaCallerDeviceRestore restore(api, original_device);

  const auto allocators = make_cuda_memory_allocators();
  require(allocators && allocators.value().size() == 2U, "CUDA 实际设备分配器创建失败");
  const auto observer_device = device_count > 1 ? (caller_device == 0 ? 1 : 0) : caller_device;
  require(api.set_device(observer_device) == 0, "CUDA 分配器回归无法选择观察设备");
  for (const auto& allocator : allocators.value()) {
    const auto alignment = allocator->memory_kind() == MemoryKind::device ? 256U : 64U;
    const auto allocation = allocator->allocate(4096U, alignment);
    require(allocation, "CUDA 绑定设备分配器无法完成真实分配");
    int allocator_current_device{};
    require(api.get_device(&allocator_current_device) == 0 && allocator_current_device == observer_device,
            "CUDA 分配器改变了调用线程当前设备");
    allocator->release(allocation.value(), 4096U, alignment);
    require(api.get_device(&allocator_current_device) == 0 && allocator_current_device == observer_device,
            "CUDA 分配器释放改变了调用线程当前设备");
  }
  require(api.set_device(caller_device) == 0, "CUDA 回归无法恢复调用方测试设备");

  const auto discovered = discover_compute_backends();
  require(!discovered.empty(), "CUDA 多设备回归的计算后端探测为空");
  int current_device{};
  require(api.get_device(&current_device) == 0 && current_device == caller_device,
          "CUDA 能力探测改变了调用线程当前设备");

  {
    const auto backend = dsp::make_cuda_fft_backend();
    require(backend, "CUDA 多设备回归无法创建 cuFFT 后端");
    const std::array<data::ComplexSample, 8U> input{data::ComplexSample{1.0, 0.0},   data::ComplexSample{0.0, 1.0},
                                                    data::ComplexSample{-1.0, 0.0},  data::ComplexSample{0.0, -1.0},
                                                    data::ComplexSample{0.5, 0.25},  data::ComplexSample{-0.5, -0.25},
                                                    data::ComplexSample{0.25, -0.5}, data::ComplexSample{-0.25, 0.5}};
    const auto execution = backend.value()->execute({input.size(), dsp::FftDirection::forward}, input);
    require(execution, "cuFFT 计划未在所属设备创建并完成执行");
  }
  require(api.get_device(&current_device) == 0 && current_device == caller_device,
          "cuFFT 计划创建、执行或销毁改变了调用线程当前设备");
}
#endif

void verify_cpu_working_set_capacity() {
  const std::vector<BackendCapabilities> constrained_capabilities{
      {BackendKind::cpu_multithread, "constrained-threaded", "1.0", "host-cpu", true, true, false, false, false, 4U,
       1024U, true},
      {BackendKind::cpu_simd, "constrained-simd", "1.0", "host-cpu", true, true, false, false, false, 1U, 1024U, true},
      {BackendKind::cpu_scalar, "adequate-scalar", "1.0", "host-cpu", true, true, false, false, false, 1U, 4096U, true},
  };
  const auto constrained_runtime = ComputeRuntime::create(constrained_capabilities, 1U);
  require(constrained_runtime, "受限内存计算运行时创建失败");
  const Workload constrained_workload{"buffer-copy", 1024U, 2048U, 128'000U, false, false, true};
  const auto constrained_selection = constrained_runtime.value()->select(constrained_workload);
  require(constrained_selection && constrained_selection.value().actual == BackendKind::cpu_scalar,
          "CPU 自动选择未跳过 Working Set 容量不足的多线程/SIMD 后端");
  const auto constrained_fallback =
      constrained_runtime.value()->fallback(constrained_workload, BackendKind::cuda, "注入 CUDA 内存不足");
  require(constrained_fallback && constrained_fallback.value().actual == BackendKind::cpu_scalar,
          "CPU 自动降级未跳过 Working Set 容量不足的多线程/SIMD 后端");

  auto insufficient_capabilities = constrained_capabilities;
  insufficient_capabilities.back().memory_bytes = 1024U;
  const auto insufficient_runtime = ComputeRuntime::create(std::move(insufficient_capabilities), 1U);
  require(insufficient_runtime, "全容量不足计算运行时创建失败");
  require(!insufficient_runtime.value()->select(constrained_workload),
          "CPU 自动选择不得接受 Working Set 容量不足的后端");
  require(!insufficient_runtime.value()->fallback(constrained_workload, BackendKind::cuda, "注入 CUDA 内存不足"),
          "CPU 自动降级不得接受 Working Set 容量不足的后端");
}

void verify_cuda_device_context() {
#if defined(_WIN32)
  verify_cuda_caller_device_restoration();
#endif
}

void test_compute_101() {
  const auto backends = discover_compute_backends();
  require(backends.size() >= 4U, "生产后端集合不完整");
  const auto input = reference_values(1U << 20U);
  bool cuda_available{};
  bool scalar_available{};
  bool simd_available{};
  bool multithread_available{};
  for (const auto& backend : backends) {
    const auto capability = backend->capabilities();
    cuda_available = cuda_available || (capability.kind == BackendKind::cuda && capability.available);
    scalar_available = scalar_available || (capability.kind == BackendKind::cpu_scalar && capability.available);
    simd_available = simd_available || (capability.kind == BackendKind::cpu_simd && capability.available);
    multithread_available =
        multithread_available || (capability.kind == BackendKind::cpu_multithread && capability.available);
    verify_real_backend_execution(backend, input);
  }
  require(scalar_available && simd_available, "CPU 标量/SIMD 生产后端探测不完整");
  if (std::thread::hardware_concurrency() > 1U) {
    require(multithread_available, "多核主机未发现真实多线程后端");
  }

  const auto runtime = ComputeRuntime::create(backends);
  require(runtime, "统一计算运行时创建失败");
  const auto workload = copy_workload(input.size());
  std::vector<double> output(input.size(), std::numeric_limits<double>::quiet_NaN());
  const auto executed = runtime.value()->execute_buffer_copy(workload, {input, output});
  require(executed && executed.value().provenance.consistency_verified &&
              executed.value().consistency.maximum_absolute_error == 0.0 &&
              executed.value().consistency.rms_error == 0.0 && executed.value().provenance.backend_id.size() > 0U &&
              output == input,
          "统一运行时未完成真实选择、执行、参考比较和 provenance");

  const auto simd = std::ranges::find_if(backends, [](const auto& backend) {
    const auto capability = backend->capabilities();
    return capability.kind == BackendKind::cpu_simd && capability.available;
  });
  const auto scalar = std::ranges::find_if(backends, [](const auto& backend) {
    const auto capability = backend->capabilities();
    return capability.kind == BackendKind::cpu_scalar && capability.available;
  });
  require(simd != backends.end() && scalar != backends.end(), "自动降级测试缺少真实 SIMD/标量后端");
  const auto fallback_runtime = ComputeRuntime::create({std::make_shared<CorruptingBackend>(*simd), *scalar}, 1U);
  require(fallback_runtime, "自动降级运行时创建失败");
  std::vector<double> fallback_output(input.size(), std::numeric_limits<double>::quiet_NaN());
  const auto fallback_execution = fallback_runtime.value()->execute_buffer_copy(copy_workload(input.size(), true),
                                                                                {input, fallback_output}, 0.0, 0.0);
  require(fallback_execution && fallback_output == input && fallback_execution.value().provenance.degraded &&
              fallback_execution.value().provenance.requested == BackendKind::cpu_simd &&
              fallback_execution.value().provenance.actual == BackendKind::cpu_scalar &&
              fallback_execution.value().provenance.consistency_verified &&
              fallback_execution.value().consistency.maximum_absolute_error == 0.0 &&
              fallback_execution.value().consistency.rms_error == 0.0 &&
              fallback_execution.value().provenance.reason.find("一致性") != std::string::npos,
          "错误输出未触发真实自动降级、重新执行和一致性 provenance");
  require(!runtime.value()->select({"unknown-operation", 8U, 16U, 1U, false, false, true}),
          "后端选择器不得忽略未知 operation");
  verify_cpu_working_set_capacity();

  if (cuda_available) {
    const auto fallback = runtime.value()->fallback(workload, BackendKind::cuda, "设备内存不足");
    require(fallback && fallback.value().degraded && fallback.value().actual != BackendKind::cuda &&
                fallback.value().reason.find("设备内存不足") != std::string::npos,
            "CUDA 显式降级未记录实际 CPU 后端与失败原因");
  }

  auto altered = input;
  altered[17] += 0.25;
  const auto mismatch = measure_consistency(input, altered);
  require(mismatch && std::abs(mismatch.value().maximum_absolute_error - 0.25) <= 1e-12 &&
              mismatch.value().rms_error > 0.0,
          "最大绝对误差/RMS 未从真实输出计算");
  require(!verify_consistency(executed.value().provenance, mismatch.value(), 0.1, 0.01),
          "真实输出超阈值时一致性验证必须失败");
  require(!verify_consistency(executed.value().provenance, ConsistencyMetrics{0.0, 0.0, 16U, false}, 0.0, 0.0),
          "没有独立参考结果的固定零指标不得通过一致性验证");

  const std::vector<BackendCapabilities> dsp_capabilities{
      {BackendKind::cpu_scalar, "oneMKL-adapter", "2025.2", "host-cpu", true, false, true, true, true, 1U, 0U, true},
      {BackendKind::cpu_multithread, "unsupported-threaded-adapter", "1.0", "host-cpu", true, false, true, true, true,
       4U, 0U, true},
      {BackendKind::cuda, "cuFFT-adapter", "CUDA 12.4", "test-gpu", true, false, true, true, false, 1U,
       8U * 1024U * 1024U, true},
  };
  const auto dsp_runtime = ComputeRuntime::create(dsp_capabilities, 1U);
  require(dsp_runtime, "DSP 统一计算运行时创建失败");
  for (const auto operation_name : {"fft", "filter", "resample"}) {
    RecordingDspOperation operation(operation_name);
    const Workload dsp_workload{operation_name, 1U * 1024U * 1024U, 2U * 1024U * 1024U, 4'000'000U, false, false, true};
    const auto dsp_execution = dsp_runtime.value()->execute_operation(dsp_workload, operation, 0.0, 0.0);
    require(dsp_execution && dsp_execution.value().provenance.actual == BackendKind::cuda &&
                !dsp_execution.value().provenance.consistency_verified &&
                dsp_execution.value().provenance.reason.find("独立参考结果") != std::string::npos &&
                operation.executions().size() == 1U,
            "统一运行时未实际承载 FFT/滤波/重采样操作，或把无独立参考的固定零指标误报为已验证");
  }
  RecordingDspOperation failing_fft("fft", true);
  const auto dsp_fallback = dsp_runtime.value()->execute_operation(
      {"fft", 1U * 1024U * 1024U, 2U * 1024U * 1024U, 4'000'000U, false, false, true}, failing_fft, 0.0, 0.0);
  require(dsp_fallback && dsp_fallback.value().provenance.degraded &&
              dsp_fallback.value().provenance.requested == BackendKind::cuda &&
              dsp_fallback.value().provenance.actual == BackendKind::cpu_scalar &&
              !dsp_fallback.value().provenance.consistency_verified &&
              dsp_fallback.value().provenance.reason.find("独立参考结果") != std::string::npos &&
              failing_fft.executions().size() == 2U,
          "DSP 操作失败未跳过适配器不支持的能力声明并显式降级到可执行 CPU 后端");

  const auto failing_pool = BudgetedBufferPool::create(1024U, {std::make_shared<FailingAllocator>()});
  require(failing_pool, "异常安全内存池创建失败");
  require(!failing_pool.value()->acquire({512U, 64U, MemoryKind::pinned_host}) &&
              failing_pool.value()->used_bytes() == 0U,
          "分配器失败后预算预留未回滚");
  const auto host_pool = BudgetedBufferPool::create(1024U);
  require(host_pool, "主机预算内存池创建失败");
  {
    const auto aligned = host_pool.value()->acquire({512U, 256U, MemoryKind::host});
    require(aligned && reinterpret_cast<std::uintptr_t>(aligned.value().bytes().data()) % 256U == 0U &&
                host_pool.value()->used_bytes() == 512U,
            "主机租约未满足对齐或预算计数");
    require(!host_pool.value()->acquire({513U, 64U, MemoryKind::host}), "超预算申请必须拒绝");
  }
  require(host_pool.value()->used_bytes() == 0U, "租约释放后预算未归还");

  auto allocators = make_cuda_memory_allocators();
  if (allocators) {
    const auto pool = BudgetedBufferPool::create(2U * 1024U * 1024U, std::move(allocators.value()));
    require(pool, "CUDA 统一预算内存池创建失败");
    const auto pinned = pool.value()->acquire({64U * 1024U, 64U, MemoryKind::pinned_host});
    const auto device = pool.value()->acquire({64U * 1024U, 256U, MemoryKind::device});
    require(pinned && device && pinned.value().bytes().size() == 64U * 1024U && device.value().bytes().empty() &&
                device.value().size_bytes() == 64U * 1024U,
            "CUDA 页锁定/设备内存池契约失败");
  } else {
    require(!cuda_available, "CUDA 已探测可用但真实内存分配器不可用");
  }
#if defined(_WIN32)
  verify_cuda_caller_device_restoration();
#endif
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3 || std::string_view(argv[1]) != "--case") {
      throw std::runtime_error("用法：signal_studio_compute_tests --case <需求编号>");
    }
    const std::map<std::string_view, std::function<void()>> cases{
        {"NFR-PERF-001", test_perf_001},
        {"NFR-PERF-002", test_perf_002},
        {"NFR-PERF-003", test_perf_003},
        {"NFR-PERF-004", test_perf_004},
        {"NFR-PERF-005", test_perf_005},
        {"NFR-PERF-006", test_perf_006},
        {"NFR-PERF-007", test_perf_007},
        {"NFR-PERF-008", test_perf_008},
        {"NFR-PERF-009", test_perf_009},
        {"NFR-PERF-010", test_perf_010},
        {"NFR-PERF-011", test_perf_011},
        {"FR-COMPUTE-101", test_compute_101},
        {"REG-COMPUTE-CAPACITY", verify_cpu_working_set_capacity},
        {"REG-CUDA-DEVICE-CONTEXT", verify_cuda_device_context},
    };
    const auto found = cases.find(argv[2]);
    if (found == cases.end()) {
      throw std::runtime_error("未知需求编号");
    }
    found->second();
    std::cout << argv[2] << " PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
