#pragma once

#include "signal_studio/compute/compute.hpp"
#include "signal_studio/data/index.hpp"
#include "signal_studio/dsp/analysis.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace signal::benchmarks::ms02 {

using Clock = std::chrono::steady_clock;

inline data::SampleRange range(std::uint64_t count) {
  return data::SampleRange::from_count(0U, count).value();
}

inline data::SignalBuffer complex_tone(std::size_t count, std::size_t bin = 17U) {
  std::vector<data::ComplexSample> values;
  values.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto phase = 2.0 * std::numbers::pi * static_cast<double>(bin * index) / static_cast<double>(count);
    values.push_back({std::cos(phase), std::sin(phase)});
  }
  return data::SignalBuffer::from_complex(std::move(values));
}

inline data::CacheKey cache_key(std::uint64_t count = 4096U, data::TileKind kind = data::TileKind::spectrum_summary) {
  data::CacheKey key;
  key.tile_kind = kind;
  key.source_fingerprint = "ms02-benchmark-source";
  key.data_source_version_id = "v1";
  key.loaded_range = range(count);
  key.descriptor_digest = "complex-f64-48k";
  key.algorithm_version = "ms02-1";
  key.dependency_version = "oneMKL-runtime";
  key.parameter_digest = "default";
  key.time_viewport = range(count);
  key.frequency_begin_hz = -24'000;
  key.frequency_end_hz = 24'000;
  key.pixel_width = 512U;
  key.pixel_height = kind == data::TileKind::stft ? 32U : 1U;
  key.quality = "full";
  return key;
}

inline data::Tile tile_for(const data::CacheKey& key, float value = 1.0F) {
  return {key.tile_kind,
          key.time_viewport,
          key.frequency_begin_hz,
          key.frequency_end_hz,
          key.pixel_width,
          key.pixel_height,
          std::vector<float>(static_cast<std::size_t>(key.pixel_width) * key.pixel_height, value)};
}

template <typename Operation>
inline core::Result<compute::PerformanceSummary> measure(std::size_t rounds, std::uint64_t bytes,
                                                         Operation&& operation) {
  std::vector<compute::PerformanceSample> samples;
  samples.reserve(rounds);
  for (std::size_t round = 0; round < rounds; ++round) {
    const auto start = Clock::now();
    if (!operation(round)) {
      return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::internal_failure}, "基准操作失败");
    }
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    if (latency.count() == 0) {
      latency = std::chrono::microseconds{1};
    }
    samples.push_back({latency, bytes});
  }
  return compute::summarize_performance(samples);
}

struct DetailedPerformance final {
  compute::PerformanceSummary summary;
  double mean_microseconds{};
  double ci95_lower_microseconds{};
  double ci95_upper_microseconds{};
};

template <typename Operation>
inline core::Result<DetailedPerformance> measure_detailed(std::size_t rounds, std::uint64_t bytes,
                                                          Operation&& operation) {
  if (rounds < 30U) {
    return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::invalid_argument},
                                 "性能证据至少需要 30 个样本");
  }
  std::vector<compute::PerformanceSample> samples;
  samples.reserve(rounds);
  std::vector<double> microseconds;
  microseconds.reserve(rounds);
  for (std::size_t round = 0; round < rounds; ++round) {
    const auto start = Clock::now();
    if (!operation(round)) {
      return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::internal_failure}, "基准操作失败");
    }
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    if (latency.count() == 0) {
      latency = std::chrono::microseconds{1};
    }
    samples.push_back({latency, bytes});
    microseconds.push_back(static_cast<double>(latency.count()));
  }
  const auto summary = compute::summarize_performance(samples);
  if (!summary)
    return summary.error();
  const auto mean =
      std::accumulate(microseconds.begin(), microseconds.end(), 0.0) / static_cast<double>(microseconds.size());
  double squared_deviation{};
  for (const auto value : microseconds) {
    const auto deviation = value - mean;
    squared_deviation += deviation * deviation;
  }
  const auto variance = squared_deviation / static_cast<double>(microseconds.size() - 1U);
  const auto margin = 1.96 * std::sqrt(variance / static_cast<double>(microseconds.size()));
  return DetailedPerformance{summary.value(), mean, std::max(0.0, mean - margin), mean + margin};
}

inline core::Result<compute::PerformanceSummary> measure_fft(dsp::IFftBackend& backend, std::size_t rounds,
                                                             std::size_t length) {
  const auto tone = complex_tone(length);
  return measure(rounds, length * sizeof(data::ComplexSample), [&](std::size_t) {
    return static_cast<bool>(backend.execute({length, dsp::FftDirection::forward}, tone.view().complex_values()));
  });
}

struct ThroughputComparison final {
  double baseline_p95_bytes_per_second{};
  double indexed_p95_bytes_per_second{};
  double ratio{};
  std::size_t sample_count{};
  std::uint64_t indexed_sample_count{};
  std::uint64_t index_bin_count{};
};

inline core::Result<ThroughputComparison>
measure_same_disk_throughput(const std::filesystem::path& path, data::SignalDescriptor descriptor,
                             std::uint64_t frames_per_bin = 1U << 20U,
                             std::uint64_t maximum_chunk_bytes = 64U * 1024U * 1024U, std::size_t rounds = 3U) {
  std::error_code size_error;
  const auto bytes = std::filesystem::file_size(path, size_error);
  if (size_error || bytes < 4U || rounds < 3U || frames_per_bin == 0U || maximum_chunk_bytes < 4U) {
    return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::invalid_argument},
                                 "完整索引基准要求真实、对齐的 SC16 文件以及至少 3 轮成对测量");
  }
  descriptor.requested_sample_range = {};

  struct ReadObservation final {
    double seconds{};
    std::uint64_t bytes_read{};
    std::uint64_t frame_count{};
    std::uint64_t bin_count{};
    std::uint64_t checksum{};
  };
  const auto read_baseline_once = [&]() -> core::Result<ReadObservation> {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::internal_failure},
                                   "无法打开同盘吞吐基准文件");
    }
    const auto block_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(maximum_chunk_bytes, bytes));
    std::vector<unsigned char> block(block_bytes);
    std::uint64_t checksum{};
    std::uint64_t observed_bytes{};
    const auto start = Clock::now();
    while (input.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size())) ||
           input.gcount() > 0) {
      const auto count = static_cast<std::size_t>(input.gcount());
      // 文件流读取及 observed_bytes 已形成可观测副作用；这里只抽样每块首尾字节，
      // 防止后续维护误删实际读取，不把逐字节 CPU 校验混入纯顺序读取基线。
      if (count != 0U) {
        checksum ^= static_cast<std::uint64_t>(block.front());
        checksum = std::rotl(checksum, 7) ^ static_cast<std::uint64_t>(block[count - 1U]);
      }
      observed_bytes += count;
    }
    if (!input.eof()) {
      return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::internal_failure},
                                   "同盘吞吐基准读取失败");
    }
    return ReadObservation{std::chrono::duration<double>(Clock::now() - start).count(), observed_bytes,
                           observed_bytes / 4U, 0U, checksum};
  };
  const auto read_index_once = [&]() -> core::Result<ReadObservation> {
    const auto start = Clock::now();
    const auto indexed = data::build_full_sc16_index(path, descriptor, frames_per_bin, maximum_chunk_bytes);
    if (!indexed)
      return indexed.error();
    return ReadObservation{std::chrono::duration<double>(Clock::now() - start).count(), indexed.value().bytes_read,
                           indexed.value().frames_indexed, static_cast<std::uint64_t>(indexed.value().bins.size()),
                           indexed.value().checksum};
  };

  const auto warm_baseline = read_baseline_once();
  const auto warm_index = read_index_once();
  if (!warm_baseline || !warm_index) {
    return !warm_baseline ? warm_baseline.error() : warm_index.error();
  }
  std::vector<double> baseline_rates;
  std::vector<double> indexed_rates;
  std::uint64_t observed_samples{};
  std::uint64_t observed_bins{};
  std::uint64_t checksum_sink{};
  const auto record = [&](const core::Result<ReadObservation>& baseline,
                          const core::Result<ReadObservation>& indexed) -> core::Status {
    if (!baseline || !indexed || baseline.value().bytes_read != bytes || indexed.value().bytes_read != bytes ||
        baseline.value().frame_count != bytes / 4U || indexed.value().frame_count != bytes / 4U ||
        indexed.value().bin_count == 0U) {
      return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::internal_failure},
                                   "完整索引未消费全部输入样本");
    }
    baseline_rates.push_back(static_cast<double>(bytes) / std::max(baseline.value().seconds, 1e-9));
    indexed_rates.push_back(static_cast<double>(bytes) / std::max(indexed.value().seconds, 1e-9));
    observed_samples = indexed.value().frame_count;
    observed_bins = indexed.value().bin_count;
    checksum_sink ^= baseline.value().checksum ^ indexed.value().checksum;
    return core::Status::success();
  };
  for (std::size_t round = 0; round < rounds; ++round) {
    core::Status recorded = core::Status::success();
    if (round % 2U == 0U) {
      const auto baseline = read_baseline_once();
      const auto indexed = read_index_once();
      recorded = record(baseline, indexed);
    } else {
      const auto indexed = read_index_once();
      const auto baseline = read_baseline_once();
      recorded = record(baseline, indexed);
    }
    if (!recorded)
      return recorded;
  }
  if (checksum_sink == std::numeric_limits<std::uint64_t>::max())
    return core::Status::failure({core::ErrorDomain::compute, core::ErrorReason::internal_failure},
                                 "完整索引校验和无效");
  std::ranges::sort(baseline_rates);
  std::ranges::sort(indexed_rates);
  // P95 延迟对应吞吐分布的慢侧 P05；两个路径必须使用相同分位。
  const auto p95_latency_index = std::min(baseline_rates.size() - 1U, baseline_rates.size() / 20U);
  const auto baseline = baseline_rates[p95_latency_index];
  const auto indexed = indexed_rates[p95_latency_index];
  return ThroughputComparison{baseline, indexed,          indexed / std::max(baseline, 1e-9),
                              rounds,   observed_samples, observed_bins};
}

} // namespace signal::benchmarks::ms02
