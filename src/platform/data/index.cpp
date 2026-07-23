#include "signal_studio/data/index.hpp"

#include "signal_studio/core/services.hpp"
#include "signal_studio/data/io.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace signal::data {
namespace {

core::Status data_error(core::ErrorReason reason, std::string message, std::string detail = {}) {
  return core::Status::failure({core::ErrorDomain::data, reason}, std::move(message), std::move(detail));
}

double scalar_value(const SignalSlice& samples, std::size_t index) {
  if (samples.kind() == SignalKind::real)
    return samples.real_values()[index];
  const auto value = samples.complex_values()[index];
  return std::hypot(value.real, value.imag);
}

std::string key_part(std::string_view value) {
  return std::to_string(value.size()) + ":" + std::string{value};
}

template <typename T> void append_unsigned(std::vector<std::byte>& output, T value) {
  static_assert(std::is_unsigned_v<T>);
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    output.push_back(static_cast<std::byte>((value >> (index * 8U)) & static_cast<T>(0xffU)));
  }
}

template <typename T> bool take_unsigned(std::span<const std::byte> bytes, std::size_t& cursor, T& value) {
  static_assert(std::is_unsigned_v<T>);
  if (cursor > bytes.size() || sizeof(T) > bytes.size() - cursor)
    return false;
  value = 0U;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    value |= static_cast<T>(std::to_integer<std::uint8_t>(bytes[cursor + index])) << (index * 8U);
  }
  cursor += sizeof(T);
  return true;
}

void append_signed64(std::vector<std::byte>& output, std::int64_t value) {
  append_unsigned(output, std::bit_cast<std::uint64_t>(value));
}

bool take_signed64(std::span<const std::byte> bytes, std::size_t& cursor, std::int64_t& value) {
  std::uint64_t bits{};
  if (!take_unsigned(bytes, cursor, bits))
    return false;
  value = std::bit_cast<std::int64_t>(bits);
  return true;
}

core::Result<std::string> cache_id(const CacheKey& key) {
  const auto canonical = key.canonical();
  const auto span = std::as_bytes(std::span<const char>{canonical.data(), canonical.size()});
  auto digest = core::hash_bytes(span);
  if (!digest)
    return digest.error();
  return digest.value().hex();
}

core::Result<std::vector<std::byte>> encode_tile(const CacheKey& key, const Tile& tile) {
  const auto validation = key.validate();
  if (!validation)
    return validation;
  if (tile.values.size() > std::numeric_limits<std::uint64_t>::max()) {
    return data_error(core::ErrorReason::invalid_argument, "Tile payload exceeds the portable format");
  }
  const std::array<std::byte, 8> magic{std::byte{'S'}, std::byte{'S'}, std::byte{'T'}, std::byte{'I'},
                                       std::byte{'L'}, std::byte{'E'}, std::byte{'1'}, std::byte{0}};
  std::vector<std::byte> output(magic.begin(), magic.end());
  append_unsigned(output, std::uint32_t{1});
  append_unsigned(output, static_cast<std::uint8_t>(tile.kind));
  append_unsigned(output, tile.time_range.begin());
  append_unsigned(output, tile.time_range.end());
  append_signed64(output, tile.frequency_begin_hz);
  append_signed64(output, tile.frequency_end_hz);
  append_unsigned(output, tile.width);
  append_unsigned(output, tile.height);
  append_unsigned(output, static_cast<std::uint64_t>(tile.values.size()));
  const auto canonical = key.canonical();
  append_unsigned(output, static_cast<std::uint64_t>(canonical.size()));
  output.insert(output.end(), reinterpret_cast<const std::byte*>(canonical.data()),
                reinterpret_cast<const std::byte*>(canonical.data() + canonical.size()));
  for (const float value : tile.values)
    append_unsigned(output, std::bit_cast<std::uint32_t>(value));
  auto digest = core::hash_bytes(output);
  if (!digest)
    return digest.error();
  output.insert(output.end(), reinterpret_cast<const std::byte*>(digest.value().bytes.data()),
                reinterpret_cast<const std::byte*>(digest.value().bytes.data() + digest.value().bytes.size()));
  return output;
}

core::Result<Tile> decode_tile(const CacheKey& key, std::span<const std::byte> bytes) {
  constexpr std::size_t digest_bytes = 32U;
  constexpr std::size_t minimum_bytes = 8U + 4U + 1U + 8U * 4U + 4U * 2U + 8U * 2U + digest_bytes;
  if (bytes.size() < minimum_bytes) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact is truncated");
  }
  const std::array<std::byte, 8> magic{std::byte{'S'}, std::byte{'S'}, std::byte{'T'}, std::byte{'I'},
                                       std::byte{'L'}, std::byte{'E'}, std::byte{'1'}, std::byte{0}};
  if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact magic is invalid");
  }
  auto digest = core::hash_bytes(bytes.first(bytes.size() - digest_bytes));
  if (!digest)
    return digest.error();
  if (!std::equal(digest.value().bytes.begin(), digest.value().bytes.end(),
                  reinterpret_cast<const std::uint8_t*>(bytes.data() + bytes.size() - digest_bytes))) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact digest is invalid");
  }
  std::size_t cursor = 8U;
  std::uint32_t version{};
  std::uint8_t kind{};
  std::uint64_t begin{};
  std::uint64_t end{};
  std::int64_t frequency_begin{};
  std::int64_t frequency_end{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint64_t value_count{};
  std::uint64_t key_size{};
  if (!take_unsigned(bytes, cursor, version) || !take_unsigned(bytes, cursor, kind) ||
      !take_unsigned(bytes, cursor, begin) || !take_unsigned(bytes, cursor, end) ||
      !take_signed64(bytes, cursor, frequency_begin) || !take_signed64(bytes, cursor, frequency_end) ||
      !take_unsigned(bytes, cursor, width) || !take_unsigned(bytes, cursor, height) ||
      !take_unsigned(bytes, cursor, value_count) || !take_unsigned(bytes, cursor, key_size) || version != 1U ||
      kind > static_cast<std::uint8_t>(TileKind::stft)) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact metadata is invalid");
  }
  if (key_size > bytes.size() - digest_bytes - cursor ||
      key_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact key length is invalid");
  }
  const std::string stored_key{reinterpret_cast<const char*>(bytes.data() + cursor),
                               static_cast<std::size_t>(key_size)};
  cursor += static_cast<std::size_t>(key_size);
  if (stored_key != key.canonical()) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact provenance key is incompatible");
  }
  if (value_count > (bytes.size() - digest_bytes - cursor) / sizeof(std::uint32_t) ||
      value_count * sizeof(std::uint32_t) != bytes.size() - digest_bytes - cursor ||
      value_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact payload length is invalid");
  }
  const auto range = SampleRange::make(begin, end);
  if (!range)
    return range.error();
  Tile tile;
  tile.kind = static_cast<TileKind>(kind);
  if (tile.kind != key.tile_kind) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact kind is incompatible");
  }
  tile.time_range = range.value();
  tile.frequency_begin_hz = frequency_begin;
  tile.frequency_end_hz = frequency_end;
  tile.width = width;
  tile.height = height;
  tile.values.reserve(static_cast<std::size_t>(value_count));
  for (std::uint64_t index = 0; index < value_count; ++index) {
    std::uint32_t bits{};
    if (!take_unsigned(bytes, cursor, bits)) {
      return data_error(core::ErrorReason::internal_failure, "Tile artifact payload is truncated");
    }
    tile.values.push_back(std::bit_cast<float>(bits));
  }
  return tile;
}

std::uint64_t tile_bytes(const Tile& tile) {
  if (tile.values.size() > (std::numeric_limits<std::uint64_t>::max() - sizeof(Tile)) / sizeof(float)) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return sizeof(Tile) + static_cast<std::uint64_t>(tile.values.size()) * sizeof(float);
}

bool corrupt_error(const core::Status& status) {
  const std::string message{status.message()};
  return message.find("artifact") != std::string::npos || message.find("digest") != std::string::npos;
}

} // namespace

core::Result<TimeSummaryPyramid> TimeSummaryPyramid::build(const SignalSlice& samples,
                                                           const SampleRange& loaded_range) {
  if (samples.size() != loaded_range.size()) {
    return data_error(core::ErrorReason::invalid_argument, "Pyramid samples do not match LoadedDataRange");
  }
  TimeSummaryPyramid result;
  result.loaded_range_ = loaded_range;
  std::vector<TimeSummaryBin> base;
  base.reserve(static_cast<std::size_t>(samples.size()));
  for (std::uint64_t index = 0; index < samples.size(); ++index) {
    const auto range = SampleRange::from_count(loaded_range.begin() + index, 1U);
    if (!range)
      return range.error();
    const double value = scalar_value(samples, static_cast<std::size_t>(index));
    if (std::isfinite(value))
      base.push_back({range.value(), value, value, std::abs(value), 1U});
    else {
      const auto nan = std::numeric_limits<double>::quiet_NaN();
      base.push_back({range.value(), nan, nan, nan, 0U});
    }
  }
  result.levels_.push_back(std::move(base));
  while (result.levels_.back().size() > 1U) {
    const auto& previous = result.levels_.back();
    std::vector<TimeSummaryBin> level;
    level.reserve((previous.size() + 1U) / 2U);
    for (std::size_t index = 0; index < previous.size(); index += 2U) {
      if (index + 1U == previous.size()) {
        level.push_back(previous[index]);
        continue;
      }
      const auto& left = previous[index];
      const auto& right = previous[index + 1U];
      const auto range = SampleRange::make(left.range.begin(), right.range.end());
      if (!range)
        return range.error();
      const auto count = left.finite_count + right.finite_count;
      if (count == 0U) {
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        level.push_back({range.value(), nan, nan, nan, 0U});
      } else {
        const double minimum = left.finite_count == 0U
                                   ? right.minimum
                                   : (right.finite_count == 0U ? left.minimum : std::min(left.minimum, right.minimum));
        const double maximum = left.finite_count == 0U
                                   ? right.maximum
                                   : (right.finite_count == 0U ? left.maximum : std::max(left.maximum, right.maximum));
        const long double squares = static_cast<long double>(left.rms) * left.rms * left.finite_count +
                                    static_cast<long double>(right.rms) * right.rms * right.finite_count;
        level.push_back({range.value(), minimum, maximum, std::sqrt(static_cast<double>(squares / count)), count});
      }
    }
    result.levels_.push_back(std::move(level));
  }
  return result;
}

core::Result<std::vector<TimeSummaryBin>> TimeSummaryPyramid::viewport(const SampleRange& viewport_range,
                                                                       std::uint32_t pixel_width) const {
  if (pixel_width == 0U || !loaded_range_.contains(viewport_range)) {
    return data_error(core::ErrorReason::invalid_argument, "Viewport is outside indexed samples or has zero pixels");
  }
  if (viewport_range.empty())
    return std::vector<TimeSummaryBin>{};
  const auto samples_per_pixel =
      std::max<std::uint64_t>(1U, (viewport_range.size() + static_cast<std::uint64_t>(pixel_width) - 1U) / pixel_width);
  std::size_t level_index{};
  std::uint64_t level_width{1U};
  while (level_index + 1U < levels_.size() && level_width <= samples_per_pixel / 2U) {
    ++level_index;
    level_width *= 2U;
  }
  std::vector<TimeSummaryBin> output;
  for (const auto& bin : levels_[level_index]) {
    if (bin.range.end() <= viewport_range.begin())
      continue;
    if (bin.range.begin() >= viewport_range.end())
      break;
    output.push_back(bin);
  }
  return output;
}

ProgressiveIndexStatus::ProgressiveIndexStatus(std::uint64_t loaded_end_sample)
    : loaded_end_sample_(loaded_end_sample) {}

core::Status ProgressiveIndexStatus::transition(ProgressiveIndexState next, double coverage, std::string reason) {
  if (!std::isfinite(coverage) || coverage < 0.0 || coverage > 1.0 ||
      (next == ProgressiveIndexState::complete && coverage != 1.0) ||
      ((next == ProgressiveIndexState::sample_overview || next == ProgressiveIndexState::degraded) && reason.empty())) {
    return data_error(core::ErrorReason::invalid_argument, "Progressive index state metadata is invalid");
  }
  const bool allowed = (state_ == ProgressiveIndexState::time_frame_ready &&
                        (next == ProgressiveIndexState::sample_overview || next == ProgressiveIndexState::degraded)) ||
                       (state_ == ProgressiveIndexState::sample_overview &&
                        (next == ProgressiveIndexState::building || next == ProgressiveIndexState::degraded)) ||
                       (state_ == ProgressiveIndexState::building &&
                        (next == ProgressiveIndexState::building || next == ProgressiveIndexState::complete ||
                         next == ProgressiveIndexState::degraded)) ||
                       (state_ == ProgressiveIndexState::complete &&
                        (next == ProgressiveIndexState::complete || next == ProgressiveIndexState::degraded)) ||
                       (state_ == ProgressiveIndexState::degraded && next == ProgressiveIndexState::degraded);
  if (!allowed || (next != ProgressiveIndexState::degraded && coverage < coverage_)) {
    return data_error(core::ErrorReason::invalid_argument, "Progressive index transition is invalid");
  }
  state_ = next;
  coverage_ = coverage;
  reason_ = std::move(reason);
  return core::Status::success();
}

core::Status CacheKey::validate() const {
  if (static_cast<std::uint8_t>(tile_kind) > static_cast<std::uint8_t>(TileKind::stft) || source_fingerprint.empty() ||
      data_source_version_id.empty() || descriptor_digest.empty() || algorithm_version.empty() ||
      dependency_version.empty() || parameter_digest.empty() || quality.empty() ||
      !loaded_range.contains(time_viewport) || time_viewport.empty() || frequency_end_hz <= frequency_begin_hz ||
      pixel_width == 0U || pixel_height == 0U) {
    return data_error(core::ErrorReason::invalid_argument, "Cache key omits provenance or viewport identity");
  }
  return core::Status::success();
}

std::string CacheKey::canonical() const {
  std::ostringstream output;
  output << static_cast<unsigned>(tile_kind) << '|' << key_part(source_fingerprint) << '|'
         << key_part(data_source_version_id) << '|' << loaded_range.begin() << ':' << loaded_range.end() << '|'
         << key_part(descriptor_digest) << '|' << key_part(algorithm_version) << '|' << key_part(dependency_version)
         << '|' << key_part(parameter_digest) << '|' << time_viewport.begin() << ':' << time_viewport.end() << '|'
         << frequency_begin_hz << ':' << frequency_end_hz << '|' << pixel_width << ':' << pixel_height << '|'
         << key_part(quality);
  return output.str();
}

core::Status TileRequest::validate() const {
  const auto validation = key.validate();
  if (!validation)
    return validation;
  if (kind != key.tile_kind) {
    return data_error(core::ErrorReason::invalid_argument, "Tile request kind differs from its cache identity");
  }
  return core::Status::success();
}

DirectDftTileProducer::DirectDftTileProducer(SignalSlice samples, SampleRange loaded_range, double sample_rate_hz,
                                             std::uint64_t maximum_input_samples, std::uint64_t maximum_operations)
    : samples_(std::move(samples)), loaded_range_(loaded_range), sample_rate_hz_(sample_rate_hz),
      maximum_input_samples_(maximum_input_samples), maximum_operations_(maximum_operations) {}

core::Result<Tile> DirectDftTileProducer::produce(const TileRequest& request, const CancellationToken& cancellation) {
  const auto validation = request.validate();
  if (!validation)
    return validation;
  if (!std::isfinite(sample_rate_hz_) || sample_rate_hz_ <= 0.0 || samples_.size() != loaded_range_.size() ||
      !loaded_range_.contains(request.key.time_viewport) || maximum_input_samples_ == 0U || maximum_operations_ == 0U) {
    return data_error(core::ErrorReason::invalid_argument, "Direct DFT producer configuration is invalid");
  }
  const auto input_count = request.key.time_viewport.size();
  if (input_count == 0U || input_count > maximum_input_samples_ ||
      request.key.pixel_width > maximum_operations_ / input_count) {
    return data_error(core::ErrorReason::unavailable, "Direct DFT request exceeds its deterministic work bound");
  }
  const std::uint32_t rows = request.kind == TileKind::stft ? request.key.pixel_height : 1U;
  if (rows == 0U)
    return data_error(core::ErrorReason::invalid_argument, "DFT tile requires at least one row");
  Tile tile;
  tile.kind = request.kind;
  tile.time_range = request.key.time_viewport;
  tile.frequency_begin_hz = request.key.frequency_begin_hz;
  tile.frequency_end_hz = request.key.frequency_end_hz;
  tile.width = request.key.pixel_width;
  tile.height = rows;
  tile.values.reserve(static_cast<std::size_t>(tile.width) * tile.height);
  const auto source_offset = request.key.time_viewport.begin() - loaded_range_.begin();
  const auto value_at = [&](std::uint64_t index) {
    if (samples_.kind() == SignalKind::real) {
      return std::complex<long double>{samples_.real_values()[static_cast<std::size_t>(source_offset + index)], 0.0L};
    }
    const auto value = samples_.complex_values()[static_cast<std::size_t>(source_offset + index)];
    return std::complex<long double>{value.real, value.imag};
  };
  for (std::uint32_t row = 0; row < rows; ++row) {
    const auto frame_begin = input_count * row / rows;
    auto frame_end = input_count * (static_cast<std::uint64_t>(row) + 1U) / rows;
    if (frame_end <= frame_begin)
      frame_end = std::min(input_count, frame_begin + 1U);
    const auto frame_count = frame_end - frame_begin;
    for (std::uint32_t column = 0; column < tile.width; ++column) {
      if (cancellation.cancelled()) {
        return data_error(core::ErrorReason::cancelled, "Direct DFT tile production was cancelled");
      }
      const long double fraction =
          tile.width == 1U ? 0.5L : static_cast<long double>(column) / static_cast<long double>(tile.width - 1U);
      const long double frequency =
          static_cast<long double>(tile.frequency_begin_hz) +
          fraction * static_cast<long double>(tile.frequency_end_hz - tile.frequency_begin_hz);
      std::complex<long double> accumulator{};
      for (std::uint64_t index = frame_begin; index < frame_end; ++index) {
        const long double phase = -2.0L * std::numbers::pi_v<long double> * frequency *
                                  static_cast<long double>(index - frame_begin) /
                                  static_cast<long double>(sample_rate_hz_);
        accumulator += value_at(index) * std::complex<long double>{std::cos(phase), std::sin(phase)};
      }
      tile.values.push_back(
          frame_count == 0U ? 0.0F : static_cast<float>(std::abs(accumulator) / static_cast<long double>(frame_count)));
    }
  }
  return tile;
}

core::Status validate_range_extension(const SampleRange& previous_range,
                                      std::string_view previous_data_source_version_id, const SampleRange& next_range,
                                      std::string_view next_data_source_version_id) {
  if (previous_data_source_version_id.empty() || next_data_source_version_id.empty() ||
      next_range.begin() != previous_range.begin() || next_range.end() < previous_range.end()) {
    return data_error(core::ErrorReason::invalid_argument, "Range extension metadata is invalid");
  }
  if (next_range.end() > previous_range.end() && previous_data_source_version_id == next_data_source_version_id) {
    return data_error(core::ErrorReason::invalid_argument,
                      "Expanding LoadedDataRange requires a new data source version");
  }
  return core::Status::success();
}

core::Result<std::unique_ptr<MemoryTileCache>> MemoryTileCache::create(std::uint64_t physical_memory_bytes,
                                                                       std::uint32_t budget_percent) {
  if (physical_memory_bytes == 0U || budget_percent < 10U || budget_percent > 60U) {
    return data_error(core::ErrorReason::invalid_argument, "Memory cache budget must be 10 through 60 percent");
  }
  const auto quotient = physical_memory_bytes / 100U;
  const auto remainder = physical_memory_bytes % 100U;
  const auto budget = quotient * budget_percent + remainder * budget_percent / 100U;
  return std::unique_ptr<MemoryTileCache>{new MemoryTileCache{budget}};
}

std::shared_ptr<const Tile> MemoryTileCache::get(const CacheKey& key) {
  std::lock_guard lock{mutex_};
  const auto iterator = entries_.find(key.canonical());
  if (iterator == entries_.end()) {
    ++diagnostics_.misses;
    return nullptr;
  }
  iterator->second.access = ++clock_;
  if (!iterator->second.tile || iterator->second.tile->kind != key.tile_kind) {
    used_bytes_ -= iterator->second.bytes;
    entries_.erase(iterator);
    ++diagnostics_.corruptions;
    ++diagnostics_.misses;
    return nullptr;
  }
  ++diagnostics_.memory_hits;
  diagnostics_.active_level = "L0-memory";
  return iterator->second.tile;
}

void MemoryTileCache::evict_to_fit(std::uint64_t incoming_bytes) {
  while (incoming_bytes <= budget_bytes_ && used_bytes_ > budget_bytes_ - incoming_bytes) {
    auto victim = entries_.end();
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
      if (iterator->second.pins == 0U && (victim == entries_.end() || iterator->second.access < victim->second.access))
        victim = iterator;
    }
    if (victim == entries_.end())
      break;
    used_bytes_ -= victim->second.bytes;
    entries_.erase(victim);
    ++diagnostics_.evictions;
  }
}

core::Status MemoryTileCache::put(const CacheKey& key, std::shared_ptr<const Tile> tile) {
  const auto validation = key.validate();
  if (!validation)
    return validation;
  if (!tile)
    return data_error(core::ErrorReason::invalid_argument, "Cannot cache a null tile");
  const auto bytes = tile_bytes(*tile);
  std::lock_guard lock{mutex_};
  const auto canonical = key.canonical();
  if (auto existing = entries_.find(canonical); existing != entries_.end()) {
    used_bytes_ -= existing->second.bytes;
    entries_.erase(existing);
  }
  if (bytes > budget_bytes_)
    return core::Status::success();
  evict_to_fit(bytes);
  if (used_bytes_ > budget_bytes_ - bytes) {
    return data_error(core::ErrorReason::unavailable, "Pinned cache entries leave insufficient memory budget");
  }
  entries_.emplace(canonical, Entry{std::move(tile), bytes, ++clock_, 0U});
  used_bytes_ += bytes;
  return core::Status::success();
}

core::Status MemoryTileCache::pin(const CacheKey& key) {
  std::lock_guard lock{mutex_};
  const auto iterator = entries_.find(key.canonical());
  if (iterator == entries_.end())
    return data_error(core::ErrorReason::unavailable, "Cache entry does not exist");
  ++iterator->second.pins;
  return core::Status::success();
}

void MemoryTileCache::unpin(const CacheKey& key) noexcept {
  std::lock_guard lock{mutex_};
  const auto iterator = entries_.find(key.canonical());
  if (iterator != entries_.end() && iterator->second.pins != 0U)
    --iterator->second.pins;
}

std::uint64_t MemoryTileCache::used_bytes() const noexcept {
  std::lock_guard lock{mutex_};
  return used_bytes_;
}

CacheDiagnostics MemoryTileCache::diagnostics() const {
  std::lock_guard lock{mutex_};
  return diagnostics_;
}

DiskTileStore::DiskTileStore(std::filesystem::path directory, std::uint64_t capacity_bytes)
    : directory_(std::move(directory)), capacity_bytes_(capacity_bytes) {}

core::Result<std::filesystem::path> DiskTileStore::path_for(const CacheKey& key) const {
  const auto validation = key.validate();
  if (!validation)
    return validation;
  auto id = cache_id(key);
  if (!id)
    return id.error();
  return directory_ / (id.value() + ".tile");
}

core::Status DiskTileStore::recover() {
  std::lock_guard lock{mutex_};
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error)
    return data_error(core::ErrorReason::unavailable, "Cannot create tile cache directory", error.message());
  for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
    if (error)
      return data_error(core::ErrorReason::unavailable, "Cannot enumerate tile cache", error.message());
    const auto extension = entry.path().extension().string();
    if (extension == ".tmp") {
      std::filesystem::remove(entry.path(), error);
      error.clear();
    } else if (extension == ".bak") {
      auto destination = entry.path();
      destination.replace_extension();
      if (std::filesystem::exists(destination, error))
        std::filesystem::remove(entry.path(), error);
      else
        std::filesystem::rename(entry.path(), destination, error);
      if (error)
        return data_error(core::ErrorReason::unavailable, "Cannot recover tile backup", error.message());
    }
  }
  enforce_capacity();
  return core::Status::success();
}

core::Result<std::shared_ptr<const Tile>> DiskTileStore::get(const CacheKey& key) {
  std::lock_guard lock{mutex_};
  auto path = path_for(key);
  if (!path)
    return path.error();
  std::error_code error;
  if (!std::filesystem::is_regular_file(path.value(), error)) {
    return data_error(core::ErrorReason::unavailable, "Tile artifact is not present");
  }
  const auto size = std::filesystem::file_size(path.value(), error);
  if (error || size > capacity_bytes_ || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    return data_error(core::ErrorReason::internal_failure, "Tile artifact size is invalid", error.message());
  }
  BoundedFileReader reader{path.value(), static_cast<std::uint64_t>(size)};
  auto bytes = reader.read(0U, static_cast<std::uint64_t>(size));
  if (!bytes)
    return bytes.error();
  auto tile = decode_tile(key, bytes.value());
  if (!tile)
    return tile.error();
  std::filesystem::last_write_time(path.value(), std::filesystem::file_time_type::clock::now(), error);
  return std::make_shared<const Tile>(std::move(tile.value()));
}

core::Status DiskTileStore::put(const CacheKey& key, const Tile& tile) {
  std::lock_guard lock{mutex_};
  if (capacity_bytes_ == 0U)
    return data_error(core::ErrorReason::unavailable, "Disk tile capacity is zero");
  auto payload = encode_tile(key, tile);
  if (!payload)
    return payload.error();
  if (payload.value().size() > capacity_bytes_) {
    return data_error(core::ErrorReason::unavailable, "Tile exceeds disk cache capacity");
  }
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error)
    return data_error(core::ErrorReason::unavailable, "Cannot create tile cache directory", error.message());
  auto destination = path_for(key);
  if (!destination)
    return destination.error();
  auto temporary = destination.value();
  temporary += ".tmp";
  auto backup = destination.value();
  backup += ".bak";
  {
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    if (!output)
      return data_error(core::ErrorReason::unavailable, "Cannot create temporary tile artifact");
    output.write(reinterpret_cast<const char*>(payload.value().data()),
                 static_cast<std::streamsize>(payload.value().size()));
    output.flush();
    if (!output)
      return data_error(core::ErrorReason::unavailable, "Cannot flush temporary tile artifact");
  }
  const bool had_destination = std::filesystem::exists(destination.value(), error);
  error.clear();
  if (had_destination) {
    std::filesystem::remove(backup, error);
    error.clear();
    std::filesystem::rename(destination.value(), backup, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      return data_error(core::ErrorReason::unavailable, "Cannot preserve previous tile artifact", error.message());
    }
  }
  std::filesystem::rename(temporary, destination.value(), error);
  if (error) {
    const auto rename_error = error.message();
    error.clear();
    if (had_destination)
      std::filesystem::rename(backup, destination.value(), error);
    std::filesystem::remove(temporary, error);
    return data_error(core::ErrorReason::unavailable, "Cannot atomically commit tile artifact", rename_error);
  }
  if (had_destination)
    std::filesystem::remove(backup, error);
  enforce_capacity();
  return core::Status::success();
}

core::Status DiskTileStore::pin(const CacheKey& key) {
  auto path = path_for(key);
  if (!path)
    return path.error();
  std::lock_guard lock{mutex_};
  pinned_.insert(path.value().filename().string());
  return core::Status::success();
}

void DiskTileStore::unpin(const CacheKey& key) noexcept {
  auto path = path_for(key);
  if (!path)
    return;
  std::lock_guard lock{mutex_};
  pinned_.erase(path.value().filename().string());
}

std::uint64_t DiskTileStore::disk_bytes() const {
  std::lock_guard lock{mutex_};
  std::uint64_t total{};
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
    if (error)
      break;
    if (entry.path().extension() != ".tile")
      continue;
    const auto size = entry.file_size(error);
    if (error || size > std::numeric_limits<std::uint64_t>::max() - total)
      break;
    total += static_cast<std::uint64_t>(size);
  }
  return total;
}

void DiskTileStore::enforce_capacity() {
  struct Candidate final {
    std::filesystem::path path;
    std::filesystem::file_time_type time;
    std::uint64_t bytes{};
  };
  std::vector<Candidate> candidates;
  std::uint64_t total{};
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
    if (error)
      return;
    if (entry.path().extension() != ".tile")
      continue;
    const auto bytes = static_cast<std::uint64_t>(entry.file_size(error));
    if (error)
      return;
    total += bytes;
    if (!pinned_.contains(entry.path().filename().string())) {
      candidates.push_back({entry.path(), entry.last_write_time(error), bytes});
      if (error)
        return;
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) { return left.time < right.time; });
  for (const auto& candidate : candidates) {
    if (total <= capacity_bytes_)
      break;
    std::filesystem::remove(candidate.path, error);
    if (!error)
      total -= candidate.bytes;
    error.clear();
  }
}

MultiResolutionTileStore::MultiResolutionTileStore(std::unique_ptr<MemoryTileCache> memory,
                                                   std::unique_ptr<DiskTileStore> disk)
    : memory_(std::move(memory)), disk_(std::move(disk)) {
  if (!memory_)
    throw std::invalid_argument("MultiResolutionTileStore requires a memory cache");
}

core::Result<std::shared_ptr<const Tile>> MultiResolutionTileStore::get(const TileRequest& request) {
  const auto validation = request.validate();
  if (!validation)
    return validation;
  if (auto tile = memory_->get(request.key)) {
    if (tile->kind != request.kind) {
      std::lock_guard lock{mutex_};
      ++diagnostics_.corruptions;
      diagnostics_.invalidation_reason = "Memory tile kind mismatch";
    } else {
      std::lock_guard lock{mutex_};
      diagnostics_.active_level = "L0-memory";
      return tile;
    }
  }
  if (disk_) {
    auto tile = disk_->get(request.key);
    if (tile && tile.value()->kind == request.kind) {
      static_cast<void>(memory_->put(request.key, tile.value()));
      const auto bytes = disk_->disk_bytes();
      std::lock_guard lock{mutex_};
      ++diagnostics_.disk_hits;
      diagnostics_.active_level = "L1-disk";
      diagnostics_.disk_bytes = bytes;
      return tile.value();
    }
    std::lock_guard lock{mutex_};
    if (!tile || tile.value()->kind != request.kind) {
      if (!tile && corrupt_error(tile.error()))
        ++diagnostics_.corruptions;
      diagnostics_.invalidation_reason = tile ? "Disk tile kind mismatch" : std::string{tile.error().message()};
    }
  }
  std::lock_guard lock{mutex_};
  ++diagnostics_.misses;
  return data_error(core::ErrorReason::unavailable, "Compatible tile is not cached");
}

core::Status MultiResolutionTileStore::put(const TileRequest& request, std::shared_ptr<const Tile> tile) {
  const auto validation = request.validate();
  if (!validation)
    return validation;
  if (!tile || tile->kind != request.kind || tile->time_range != request.key.time_viewport ||
      tile->frequency_begin_hz != request.key.frequency_begin_hz ||
      tile->frequency_end_hz != request.key.frequency_end_hz ||
      static_cast<std::uint64_t>(tile->width) * tile->height != tile->values.size()) {
    return data_error(core::ErrorReason::invalid_argument, "Tile is incompatible with the cache request");
  }
  const auto memory_status = memory_->put(request.key, tile);
  if (!memory_status)
    return memory_status;
  if (disk_) {
    const auto disk_status = disk_->put(request.key, *tile);
    const auto bytes = disk_->disk_bytes();
    std::lock_guard lock{mutex_};
    diagnostics_.disk_bytes = bytes;
    if (!disk_status) {
      diagnostics_.degraded = true;
      diagnostics_.invalidation_reason = std::string{disk_status.message()};
      return disk_status;
    }
  }
  return core::Status::success();
}

core::Status MultiResolutionTileStore::pin(const TileRequest& request) {
  const auto validation = request.validate();
  if (!validation)
    return validation;
  const auto memory_status = memory_->pin(request.key);
  if (!memory_status)
    return memory_status;
  if (disk_) {
    const auto disk_status = disk_->pin(request.key);
    if (!disk_status) {
      memory_->unpin(request.key);
      return disk_status;
    }
  }
  return core::Status::success();
}

void MultiResolutionTileStore::unpin(const TileRequest& request) noexcept {
  memory_->unpin(request.key);
  if (disk_)
    disk_->unpin(request.key);
}

core::Result<std::shared_ptr<const Tile>>
MultiResolutionTileStore::get_or_produce(const TileRequest& request, ITileProducer& producer,
                                         const CancellationToken& cancellation) {
  const auto validation = request.validate();
  if (!validation)
    return validation;
  if (cancellation.cancelled())
    return data_error(core::ErrorReason::cancelled, "Tile request was cancelled");
  auto cached = get(request);
  if (cached)
    return cached.value();
  auto tile = producer.produce(request, cancellation);
  if (!tile)
    return tile.error();
  if (cancellation.cancelled())
    return data_error(core::ErrorReason::cancelled, "Tile request was cancelled");
  const auto expected_count = static_cast<std::uint64_t>(tile.value().width) * tile.value().height;
  if (tile.value().kind != request.kind || tile.value().time_range != request.key.time_viewport ||
      tile.value().frequency_begin_hz != request.key.frequency_begin_hz ||
      tile.value().frequency_end_hz != request.key.frequency_end_hz || expected_count != tile.value().values.size()) {
    return data_error(core::ErrorReason::internal_failure, "Tile producer returned incompatible output");
  }
  auto shared = std::make_shared<const Tile>(std::move(tile.value()));
  const auto cache_status = put(request, shared);
  if (!cache_status) {
    std::lock_guard lock{mutex_};
    diagnostics_.degraded = true;
    diagnostics_.invalidation_reason = std::string{cache_status.message()};
  }
  return shared;
}

void MultiResolutionTileStore::update_index_status(const ProgressiveIndexStatus& status) {
  std::string state;
  switch (status.state()) {
  case ProgressiveIndexState::time_frame_ready:
    state = "time-frame-ready";
    break;
  case ProgressiveIndexState::sample_overview:
    state = "sample-overview";
    break;
  case ProgressiveIndexState::building:
    state = "building";
    break;
  case ProgressiveIndexState::complete:
    state = "complete";
    break;
  case ProgressiveIndexState::degraded:
    state = "degraded";
    break;
  }
  std::lock_guard lock{mutex_};
  diagnostics_.index_progress = status.coverage();
  diagnostics_.coverage = status.coverage();
  if (diagnostics_.active_level.empty())
    diagnostics_.active_level = std::move(state);
  diagnostics_.degraded = status.state() == ProgressiveIndexState::degraded;
  diagnostics_.invalidation_reason = status.reason();
}

CacheDiagnostics MultiResolutionTileStore::diagnostics() const {
  const auto memory = memory_->diagnostics();
  std::lock_guard lock{mutex_};
  CacheDiagnostics result = diagnostics_;
  result.memory_hits = memory.memory_hits;
  result.evictions = memory.evictions;
  return result;
}

void PrefetchQueue::enqueue(TileRequest request) {
  request.prefetch = true;
  std::lock_guard lock{mutex_};
  pending_.push_back({std::move(request), CancellationToken{}});
}

void PrefetchQueue::cancel_pending() noexcept {
  std::lock_guard lock{mutex_};
  if (active_)
    active_->cancel();
  for (auto& pending : pending_)
    pending.cancellation.cancel();
  pending_.clear();
}

core::Status PrefetchQueue::run_next(MultiResolutionTileStore& store, ITileProducer& producer) {
  std::optional<Pending> next;
  {
    std::lock_guard lock{mutex_};
    if (pending_.empty())
      return core::Status::success();
    next = std::move(pending_.front());
    pending_.pop_front();
    active_ = next->cancellation;
  }
  auto result = store.get_or_produce(next->request, producer, next->cancellation);
  {
    std::lock_guard lock{mutex_};
    active_.reset();
  }
  return result ? core::Status::success() : result.error();
}

core::Result<std::shared_ptr<const Tile>> PrefetchQueue::run_interactive(MultiResolutionTileStore& store,
                                                                         ITileProducer& producer, TileRequest request,
                                                                         const CancellationToken& cancellation) {
  cancel_pending();
  request.prefetch = false;
  return store.get_or_produce(request, producer, cancellation);
}

std::size_t PrefetchQueue::pending() const noexcept {
  std::lock_guard lock{mutex_};
  return pending_.size();
}

} // namespace signal::data
