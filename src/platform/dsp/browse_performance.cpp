#include "signal_studio/dsp/browse_performance.hpp"

#include "signal_studio/compute/compute.hpp"
#include "signal_studio/core/services.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace signal::dsp {
namespace {

core::Status error(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::dsp, reason}, std::move(message), std::move(diagnostic));
}

core::Result<data::SampleRange> range_from_count(std::uint64_t begin, std::uint64_t count) {
  return data::SampleRange::from_count(begin, count);
}

std::uint64_t ceil_div(std::uint64_t numerator, std::uint64_t denominator) {
  return numerator / denominator + (numerator % denominator == 0U ? 0U : 1U);
}

std::vector<float> envelope(std::span<const data::ComplexSample> samples, std::uint32_t width) {
  const auto bins = std::max<std::size_t>(1U, std::min<std::size_t>(width, samples.size()));
  std::vector<float> values(bins);
  for (std::size_t bin = 0; bin < bins; ++bin) {
    const auto begin = bin * samples.size() / bins;
    const auto end = std::max(begin + 1U, (bin + 1U) * samples.size() / bins);
    double peak{};
    for (std::size_t index = begin; index < std::min(end, samples.size()); ++index) {
      peak = std::max(peak, std::hypot(samples[index].real, samples[index].imag));
    }
    values[bin] = static_cast<float>(peak);
  }
  return values;
}

std::vector<float> to_float(std::span<const double> values) {
  std::vector<float> result;
  result.reserve(values.size());
  for (const auto value : values)
    result.push_back(static_cast<float>(value));
  return result;
}

std::string source_scope(const LogicalRecordingSource::Plan& plan) {
  if (!plan.logical_repeat_mapping) {
    return "真实录制输入（" + std::to_string(plan.physical_file_bytes) + " 字节）";
  }
  if (plan.logical_file_bytes == 100'000'000'000ULL) {
    return "真实录制输入 + 100 GB（100,000,000,000 字节）逻辑重复映射";
  }
  return "真实录制输入 + " + std::to_string(plan.logical_file_bytes) + " 字节逻辑重复映射";
}

} // namespace

core::Result<std::shared_ptr<LogicalRecordingSource>>
LogicalRecordingSource::open(std::filesystem::path physical_path, data::SignalDescriptor descriptor,
                             std::string data_source_version_id, std::uint64_t logical_file_bytes,
                             std::uint64_t memory_budget_bytes, std::uint64_t maximum_window_frames) {
  if (data_source_version_id.empty() || memory_budget_bytes == 0U || maximum_window_frames == 0U) {
    return error(core::ErrorReason::invalid_argument, "逻辑录制源需要版本、内存预算和非零视窗上界");
  }
  const auto descriptor_status = descriptor.validate();
  if (!descriptor_status)
    return descriptor_status;
  const auto frame_bytes = descriptor.frame_bytes();
  if (!frame_bytes)
    return frame_bytes.error();
  std::error_code file_error;
  const auto physical_bytes = std::filesystem::file_size(physical_path, file_error);
  if (file_error || physical_bytes == 0U || physical_bytes % frame_bytes.value() != 0U) {
    return error(core::ErrorReason::unavailable, "真实录制文件不可用或尾部不是完整帧", file_error.message());
  }
  if (logical_file_bytes < physical_bytes || logical_file_bytes % frame_bytes.value() != 0U) {
    return error(core::ErrorReason::invalid_argument, "逻辑字节数必须不小于真实录制并按帧对齐");
  }
  const auto fingerprint = core::fingerprint_source(physical_path);
  if (!fingerprint) {
    return fingerprint.error();
  }
  const auto serialized_descriptor = data::serialize_sidecar(descriptor);
  if (!serialized_descriptor) {
    return serialized_descriptor.error();
  }
  const auto descriptor_hash = core::hash_bytes(
      std::as_bytes(std::span{serialized_descriptor.value().data(), serialized_descriptor.value().size()}));
  if (!descriptor_hash) {
    return descriptor_hash.error();
  }
  data_source_version_id += ":" + fingerprint.value().version_id;
  const auto physical_frames = physical_bytes / frame_bytes.value();
  const auto logical_frames = logical_file_bytes / frame_bytes.value();
  const auto initial_frames = std::min(
      {maximum_window_frames, physical_frames, std::max<std::uint64_t>(2U, memory_budget_bytes / frame_bytes.value())});
  auto physical_descriptor = descriptor;
  const auto physical_range = range_from_count(0U, physical_frames);
  if (!physical_range)
    return physical_range.error();
  physical_descriptor.requested_sample_range = physical_range.value();
  auto physical_source =
      data::FileDataSource::open_raw(physical_path, physical_descriptor, data_source_version_id + "-physical");
  if (!physical_source)
    return physical_source.error();

  Plan plan;
  plan.physical_path = std::move(physical_path);
  plan.physical_file_bytes = physical_bytes;
  plan.logical_file_bytes = logical_file_bytes;
  plan.frame_bytes = frame_bytes.value();
  plan.physical_frames = physical_frames;
  plan.logical_frames = logical_frames;
  plan.initial_window_frames = initial_frames;
  plan.bounded_working_set_bytes = initial_frames * frame_bytes.value();
  plan.logical_repetition_count = ceil_div(logical_file_bytes, physical_bytes);
  plan.source_fingerprint = fingerprint.value().version_id;
  plan.descriptor_digest = descriptor_hash.value().hex();
  plan.navigation_ready = true;
  plan.real_recording_input = true;
  plan.logical_repeat_mapping = logical_file_bytes != physical_bytes;
  plan.estimate_source = source_scope(plan) + "；总时长由逻辑字节数除以帧字节数和采样率估计";
  return std::shared_ptr<LogicalRecordingSource>(
      new LogicalRecordingSource(std::move(physical_source.value()), std::move(descriptor),
                                 std::move(data_source_version_id), std::move(plan), maximum_window_frames));
}

LogicalRecordingSource::LogicalRecordingSource(std::shared_ptr<data::FileDataSource> physical_source,
                                               data::SignalDescriptor descriptor, std::string data_source_version_id,
                                               Plan plan, std::uint64_t maximum_window_frames)
    : physical_source_(std::move(physical_source)), descriptor_(std::move(descriptor)),
      data_source_version_id_(std::move(data_source_version_id)), plan_(std::move(plan)),
      maximum_window_frames_(maximum_window_frames) {}

const LogicalRecordingSource::Plan& LogicalRecordingSource::plan() const noexcept {
  return plan_;
}

const data::SignalDescriptor& LogicalRecordingSource::descriptor() const noexcept {
  return descriptor_;
}

std::string_view LogicalRecordingSource::data_source_version_id() const noexcept {
  return data_source_version_id_;
}

core::Result<data::RawReadResult> LogicalRecordingSource::read_logical(const data::SampleRange& logical_range,
                                                                       std::uint64_t maximum_read_bytes) const {
  if (logical_range.empty() || logical_range.end() > plan_.logical_frames ||
      logical_range.size() > maximum_window_frames_ || logical_range.size() > maximum_read_bytes / plan_.frame_bytes) {
    return error(core::ErrorReason::invalid_argument, "逻辑视窗越界或超过显式有界读取上限");
  }
  std::vector<data::ComplexSample> complex;
  std::vector<double> real;
  if (descriptor_.signal_kind == data::SignalKind::complex)
    complex.reserve(static_cast<std::size_t>(logical_range.size()));
  else
    real.reserve(static_cast<std::size_t>(logical_range.size()));

  std::uint64_t cursor = logical_range.begin();
  while (cursor < logical_range.end()) {
    const auto physical_begin = cursor % plan_.physical_frames;
    const auto count = std::min(logical_range.end() - cursor, plan_.physical_frames - physical_begin);
    const auto physical_range = range_from_count(physical_begin, count);
    if (!physical_range)
      return physical_range.error();
    const auto read = physical_source_->read({physical_range.value(), count * plan_.frame_bytes, {}});
    if (!read || read.value().range.size() != count || read.value().bytes_read != count * plan_.frame_bytes)
      return read ? error(core::ErrorReason::internal_failure, "真实录制逻辑映射返回不完整帧") : read.error();
    if (descriptor_.signal_kind == data::SignalKind::complex) {
      const auto values = read.value().samples.view().complex_values();
      complex.insert(complex.end(), values.begin(), values.end());
    } else {
      const auto values = read.value().samples.view().real_values();
      real.insert(real.end(), values.begin(), values.end());
    }
    cursor += count;
  }
  data::RawReadResult result;
  result.samples = descriptor_.signal_kind == data::SignalKind::complex
                       ? data::SignalBuffer::from_complex(std::move(complex))
                       : data::SignalBuffer::from_real(std::move(real));
  result.range = logical_range;
  result.bytes_read = logical_range.size() * plan_.frame_bytes;
  return result;
}

BrowseCommandFeedback BrowseInteractionSequencer::issue(BrowseViewport viewport) {
  std::lock_guard lock{mutex_};
  ++generation_;
  feedback_ = {generation_, std::move(viewport), "正在细化", true};
  return feedback_;
}

bool BrowseInteractionSequencer::publish(std::shared_ptr<const AtomicBrowseFrame> frame) {
  if (!frame || !frame->complete)
    return false;
  std::lock_guard lock{mutex_};
  if (frame->generation != generation_)
    return false;
  frame_ = std::move(frame);
  feedback_.status_text = frame_->cache_hit ? "高分辨率缓存已恢复" : "三图一致结果已更新";
  feedback_.visible = true;
  return true;
}

BrowseCommandFeedback BrowseInteractionSequencer::feedback() const {
  std::lock_guard lock{mutex_};
  return feedback_;
}

std::shared_ptr<const AtomicBrowseFrame> BrowseInteractionSequencer::frame() const {
  std::lock_guard lock{mutex_};
  return frame_;
}

core::Result<std::unique_ptr<BrowsePerformanceSession>>
BrowsePerformanceSession::create(std::shared_ptr<LogicalRecordingSource> source,
                                 std::shared_ptr<IFftBackend> fft_backend, std::filesystem::path cache_directory,
                                 std::uint64_t memory_cache_bytes, std::uint64_t disk_cache_bytes) {
  if (!source || !fft_backend || memory_cache_bytes == 0U || disk_cache_bytes == 0U) {
    return error(core::ErrorReason::invalid_argument, "浏览会话需要真实录制源、FFT 后端和非零缓存预算");
  }
  if (memory_cache_bytes > std::numeric_limits<std::uint64_t>::max() / 2U) {
    return error(core::ErrorReason::invalid_argument, "浏览内存缓存预算溢出");
  }
  auto memory = data::MemoryTileCache::create(memory_cache_bytes * 2U, 50U);
  if (!memory)
    return memory.error();
  auto disk = std::make_unique<data::DiskTileStore>(std::move(cache_directory), disk_cache_bytes);
  if (const auto recovered = disk->recover(); !recovered)
    return recovered;
  return std::unique_ptr<BrowsePerformanceSession>(new BrowsePerformanceSession(
      std::move(source), std::move(fft_backend), std::move(memory.value()), std::move(disk)));
}

BrowsePerformanceSession::BrowsePerformanceSession(std::shared_ptr<LogicalRecordingSource> source,
                                                   std::shared_ptr<IFftBackend> fft_backend,
                                                   std::unique_ptr<data::MemoryTileCache> memory_cache,
                                                   std::unique_ptr<data::DiskTileStore> disk_cache)
    : source_(std::move(source)), fft_backend_(std::move(fft_backend)), memory_cache_(std::move(memory_cache)),
      disk_cache_(std::move(disk_cache)) {}

data::CacheKey BrowsePerformanceSession::make_key(const BrowseViewport& viewport, std::string parameter,
                                                  data::TileKind kind) const {
  data::CacheKey key;
  key.tile_kind = kind;
  key.source_fingerprint =
      source_->plan().source_fingerprint + ":logical:" + std::to_string(source_->plan().logical_file_bytes);
  key.data_source_version_id = std::string{source_->data_source_version_id()};
  key.loaded_range = data::SampleRange::from_count(0U, source_->plan().logical_frames).value();
  key.descriptor_digest = source_->plan().descriptor_digest;
  key.algorithm_version = "browse-performance/1.0";
  key.dependency_version = std::string{fft_backend_->backend_id()};
  key.parameter_digest = std::move(parameter);
  key.time_viewport = viewport.logical_range;
  const auto center = static_cast<std::int64_t>(source_->descriptor().center_frequency_hz.value_or(0.0));
  const auto half = static_cast<std::int64_t>(source_->descriptor().sample_rate_hz / 2.0);
  key.frequency_begin_hz = center - half;
  key.frequency_end_hz = center + half;
  key.pixel_width = viewport.pixel_width;
  if (kind == data::TileKind::stft) {
    const auto fft_length = static_cast<std::uint64_t>(viewport.pixel_width);
    const auto hop = std::max<std::uint64_t>(1U, fft_length / 2U);
    key.pixel_height = viewport.logical_range.size() < fft_length
                           ? 0U
                           : static_cast<std::uint32_t>(1U + (viewport.logical_range.size() - fft_length) / hop);
  } else {
    key.pixel_height = 1U;
  }
  key.quality = "high-resolution";
  return key;
}

core::Result<std::shared_ptr<const data::Tile>> BrowsePerformanceSession::restore_tile(const data::CacheKey& key) {
  if (auto memory = memory_cache_->get(key))
    return memory;
  auto disk = disk_cache_->get(key);
  if (!disk)
    return disk.error();
  if (const auto stored = memory_cache_->put(key, disk.value()); !stored)
    return stored;
  return disk.value();
}

core::Status BrowsePerformanceSession::store_tile(const data::CacheKey& key, const data::Tile& tile) {
  auto shared = std::make_shared<const data::Tile>(tile);
  if (const auto memory = memory_cache_->put(key, shared); !memory)
    return memory;
  return disk_cache_->put(key, tile);
}

core::Result<std::shared_ptr<const AtomicBrowseFrame>>
BrowsePerformanceSession::build_frame(const BrowseViewport& viewport, std::uint64_t generation, bool persist_cache) {
  if (generation == 0U || viewport.logical_range.empty() || viewport.pixel_width == 0U ||
      viewport.spectrogram_rows == 0U) {
    return error(core::ErrorReason::invalid_argument, "浏览视窗、代际和像素尺寸必须有效");
  }
  const auto read_bytes = viewport.logical_range.size() * source_->plan().frame_bytes;
  auto read = source_->read_logical(viewport.logical_range, read_bytes);
  if (!read)
    return read.error();
  if (read.value().samples.kind() != data::SignalKind::complex)
    return error(core::ErrorReason::invalid_argument, "MS-02 浏览性能路径要求复数 IQ 输入");
  const auto sample_count = read.value().samples.size();
  const auto fft_length = static_cast<std::uint64_t>(viewport.pixel_width);
  if (fft_length < 256U || fft_length > 4096U || fft_length > sample_count)
    return error(core::ErrorReason::invalid_argument, "真实视窗不足以形成三图结果");
  auto fft_slice = read.value().samples.view().slice(0U, fft_length);
  if (!fft_slice)
    return fft_slice.error();
  const SpectrumRequest spectrum_request{source_->descriptor().sample_rate_hz,
                                         source_->descriptor().center_frequency_hz.value_or(0.0), WindowKind::hann,
                                         SpectrumSidedness::two_sided_shifted};
  auto psd = calculate_psd(*fft_backend_, fft_slice.value(), spectrum_request);
  if (!psd)
    return psd.error();
  const auto hop = std::max<std::uint64_t>(1U, fft_length / 2U);
  auto stft =
      calculate_stft(*fft_backend_, read.value().samples.view(),
                     {source_->descriptor().sample_rate_hz, source_->descriptor().center_frequency_hz.value_or(0.0),
                      fft_length, hop, WindowKind::hann, SpectrumSidedness::two_sided_shifted});
  if (!stft)
    return stft.error();

  auto frame = std::make_shared<AtomicBrowseFrame>();
  frame->generation = generation;
  frame->viewport = viewport;
  frame->time_envelope = envelope(read.value().samples.view().complex_values(), viewport.pixel_width);
  frame->spectrum_db_per_hz = to_float(psd.value().db_per_hz);
  frame->spectrogram_db_per_hz = stft.value().db_per_hz;
  frame->spectrogram_columns = static_cast<std::uint32_t>(stft.value().columns);
  frame->spectrogram_rows = static_cast<std::uint32_t>(stft.value().rows);
  frame->source_scope = source_scope(source_->plan());
  frame->complete =
      !frame->time_envelope.empty() && !frame->spectrum_db_per_hz.empty() && !frame->spectrogram_db_per_hz.empty();
  if (!frame->complete)
    return error(core::ErrorReason::internal_failure, "三图原子结果不完整");

  if (persist_cache) {
    const auto time_key = make_key(viewport, "time-envelope", data::TileKind::spectrum_summary);
    const auto spectrum_key = make_key(viewport, "psd-db-per-hz", data::TileKind::spectrum_summary);
    const auto stft_key = make_key(viewport, "stft-db-per-hz", data::TileKind::stft);
    const data::Tile time_tile{time_key.tile_kind,
                               time_key.time_viewport,
                               time_key.frequency_begin_hz,
                               time_key.frequency_end_hz,
                               static_cast<std::uint32_t>(frame->time_envelope.size()),
                               1U,
                               frame->time_envelope};
    const data::Tile spectrum_tile{spectrum_key.tile_kind,
                                   spectrum_key.time_viewport,
                                   spectrum_key.frequency_begin_hz,
                                   spectrum_key.frequency_end_hz,
                                   static_cast<std::uint32_t>(frame->spectrum_db_per_hz.size()),
                                   1U,
                                   frame->spectrum_db_per_hz};
    const data::Tile stft_tile{stft_key.tile_kind,          stft_key.time_viewport,     stft_key.frequency_begin_hz,
                               stft_key.frequency_end_hz,   frame->spectrogram_columns, frame->spectrogram_rows,
                               frame->spectrogram_db_per_hz};
    if (const auto status = store_tile(time_key, time_tile); !status)
      return status;
    if (const auto status = store_tile(spectrum_key, spectrum_tile); !status)
      return status;
    if (const auto status = store_tile(stft_key, stft_tile); !status)
      return status;
  }
  return std::shared_ptr<const AtomicBrowseFrame>(std::move(frame));
}

core::Result<std::shared_ptr<const AtomicBrowseFrame>>
BrowsePerformanceSession::restore_cached_frame(const BrowseViewport& viewport, std::uint64_t generation) {
  if (generation == 0U)
    return error(core::ErrorReason::invalid_argument, "缓存恢复代际必须非零");
  auto time = restore_tile(make_key(viewport, "time-envelope", data::TileKind::spectrum_summary));
  auto spectrum = restore_tile(make_key(viewport, "psd-db-per-hz", data::TileKind::spectrum_summary));
  auto stft = restore_tile(make_key(viewport, "stft-db-per-hz", data::TileKind::stft));
  if (!time || !spectrum || !stft)
    return !time ? time.error() : (!spectrum ? spectrum.error() : stft.error());
  auto frame = std::make_shared<AtomicBrowseFrame>();
  frame->generation = generation;
  frame->viewport = viewport;
  frame->time_envelope = time.value()->values;
  frame->spectrum_db_per_hz = spectrum.value()->values;
  frame->spectrogram_db_per_hz = stft.value()->values;
  frame->spectrogram_columns = stft.value()->width;
  frame->spectrogram_rows = stft.value()->height;
  frame->source_scope = source_scope(source_->plan());
  frame->cache_hit = true;
  frame->complete = !frame->time_envelope.empty() && !frame->spectrum_db_per_hz.empty() &&
                    frame->spectrogram_columns > 0U && frame->spectrogram_rows > 0U &&
                    frame->spectrogram_db_per_hz.size() ==
                        static_cast<std::size_t>(frame->spectrogram_columns) * frame->spectrogram_rows;
  if (!frame->complete)
    return error(core::ErrorReason::internal_failure, "缓存中的三图结果不完整，拒绝原子发布");
  return std::shared_ptr<const AtomicBrowseFrame>(std::move(frame));
}

core::Result<SamplingOverview> BrowsePerformanceSession::build_sampling_overview(std::uint64_t maximum_samples,
                                                                                 std::uint32_t output_bins) {
  if (maximum_samples < output_bins || output_bins == 0U)
    return error(core::ErrorReason::invalid_argument, "采样概览样本数必须覆盖输出 bin");
  const auto windows = std::min<std::uint64_t>(64U, output_bins);
  const auto per_window = std::max<std::uint64_t>(1U, maximum_samples / windows);
  std::vector<data::ComplexSample> sampled;
  sampled.reserve(static_cast<std::size_t>(per_window * windows));
  for (std::uint64_t window = 0; window < windows; ++window) {
    const auto maximum_begin = source_->plan().logical_frames - per_window;
    const auto begin = windows == 1U ? 0U : window * maximum_begin / (windows - 1U);
    const auto logical_range = range_from_count(begin, per_window);
    if (!logical_range)
      return logical_range.error();
    auto read = source_->read_logical(logical_range.value(), per_window * source_->plan().frame_bytes);
    if (!read)
      return read.error();
    const auto values = read.value().samples.view().complex_values();
    sampled.insert(sampled.end(), values.begin(), values.end());
  }
  auto buffer = data::SignalBuffer::from_complex(std::move(sampled));
  auto sampled_range = range_from_count(0U, buffer.size());
  if (!sampled_range)
    return sampled_range.error();
  auto pyramid = data::TimeSummaryPyramid::build(buffer.view(), sampled_range.value());
  if (!pyramid)
    return pyramid.error();
  auto bins = pyramid.value().viewport(sampled_range.value(), output_bins);
  if (!bins)
    return bins.error();
  SamplingOverview result;
  result.source_scope = source_scope(source_->plan());
  result.bins = std::move(bins.value());
  result.physical_samples_read = buffer.size();
  result.logical_samples_represented = source_->plan().logical_frames;
  result.complete = !result.bins.empty();
  return result;
}

const LogicalRecordingSource::Plan& BrowsePerformanceSession::plan() const noexcept {
  return source_->plan();
}

} // namespace signal::dsp
