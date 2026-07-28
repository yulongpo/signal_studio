#pragma once

#include "signal_studio/data/preview.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace signal::data {

struct TimeSummaryBin final {
  SampleRange range;
  double minimum{};
  double maximum{};
  double rms{};
  std::uint64_t finite_count{};
};

class TimeSummaryPyramid final {
public:
  [[nodiscard]] static core::Result<TimeSummaryPyramid> build(const SignalSlice& samples,
                                                              const SampleRange& loaded_range);
  [[nodiscard]] core::Result<std::vector<TimeSummaryBin>> viewport(const SampleRange& viewport_range,
                                                                   std::uint32_t pixel_width) const;
  [[nodiscard]] std::size_t level_count() const noexcept {
    return levels_.size();
  }

private:
  SampleRange loaded_range_;
  std::vector<std::vector<TimeSummaryBin>> levels_;
};

struct FullRawIndexResult final {
  std::vector<TimeSummaryBin> bins;
  std::uint64_t frames_indexed{};
  std::uint64_t bytes_read{};
  std::uint64_t checksum{};
};

/// 以有界块扫描完整 little-endian interleaved SC16 录制并构建正式时域索引。
/// 该入口不把文件整体载入内存，取消回调在块内安全点被轮询。
[[nodiscard]] core::Result<FullRawIndexResult> build_full_sc16_index(const std::filesystem::path& path,
                                                                     const SignalDescriptor& descriptor,
                                                                     std::uint64_t frames_per_bin,
                                                                     std::uint64_t maximum_chunk_bytes,
                                                                     std::function<bool()> cancellation_requested = {});

enum class ProgressiveIndexState : std::uint8_t {
  time_frame_ready,
  sample_overview,
  building,
  complete,
  degraded,
};

class ProgressiveIndexStatus final {
public:
  explicit ProgressiveIndexStatus(std::uint64_t loaded_end_sample);
  [[nodiscard]] core::Status transition(ProgressiveIndexState next, double coverage, std::string reason = {});
  [[nodiscard]] ProgressiveIndexState state() const noexcept {
    return state_;
  }
  [[nodiscard]] double coverage() const noexcept {
    return coverage_;
  }
  [[nodiscard]] std::uint64_t index_upper_bound() const noexcept {
    return loaded_end_sample_;
  }
  [[nodiscard]] const std::string& reason() const noexcept {
    return reason_;
  }

private:
  std::uint64_t loaded_end_sample_{};
  ProgressiveIndexState state_{ProgressiveIndexState::time_frame_ready};
  double coverage_{};
  std::string reason_;
};

enum class TileKind : std::uint8_t { spectrum_summary, stft };

struct CacheKey final {
  TileKind tile_kind{TileKind::spectrum_summary};
  std::string source_fingerprint;
  std::string data_source_version_id;
  SampleRange loaded_range;
  std::string descriptor_digest;
  std::string algorithm_version;
  std::string dependency_version;
  std::string parameter_digest;
  SampleRange time_viewport;
  std::int64_t frequency_begin_hz{};
  std::int64_t frequency_end_hz{};
  std::uint32_t pixel_width{};
  std::uint32_t pixel_height{};
  std::string quality;

  [[nodiscard]] core::Status validate() const;
  [[nodiscard]] std::string canonical() const;
};

struct TileRequest final {
  TileKind kind{TileKind::spectrum_summary};
  CacheKey key;
  bool prefetch{};
  [[nodiscard]] core::Status validate() const;
};

struct Tile final {
  TileKind kind{TileKind::spectrum_summary};
  SampleRange time_range;
  std::int64_t frequency_begin_hz{};
  std::int64_t frequency_end_hz{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<float> values;
};

class ITileProducer {
public:
  virtual ~ITileProducer() = default;
  /// 生产单个有界瓦片；预计超过 50 ms 的索引/DFT 工作必须由 ITaskService 调度。
  [[nodiscard]] virtual core::Result<Tile> produce(const TileRequest& request,
                                                   const CancellationToken& cancellation) = 0;
};

class DirectDftTileProducer final : public ITileProducer {
public:
  DirectDftTileProducer(SignalSlice samples, SampleRange loaded_range, double sample_rate_hz,
                        std::uint64_t maximum_input_samples = 4096U,
                        std::uint64_t maximum_operations = 4U * 1024U * 1024U);
  [[nodiscard]] core::Result<Tile> produce(const TileRequest& request, const CancellationToken& cancellation) override;

private:
  SignalSlice samples_;
  SampleRange loaded_range_;
  double sample_rate_hz_{};
  std::uint64_t maximum_input_samples_{};
  std::uint64_t maximum_operations_{};
};

[[nodiscard]] core::Status validate_range_extension(const SampleRange& previous_range,
                                                    std::string_view previous_data_source_version_id,
                                                    const SampleRange& next_range,
                                                    std::string_view next_data_source_version_id);

struct CacheDiagnostics final {
  std::uint64_t memory_hits{};
  std::uint64_t disk_hits{};
  std::uint64_t misses{};
  std::uint64_t corruptions{};
  std::uint64_t evictions{};
  std::uint64_t disk_bytes{};
  double index_progress{};
  double coverage{};
  std::string active_level;
  std::string invalidation_reason;
  bool degraded{};
};

class MemoryTileCache final {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<MemoryTileCache>> create(std::uint64_t physical_memory_bytes,
                                                                             std::uint32_t budget_percent = 25);
  [[nodiscard]] std::shared_ptr<const Tile> get(const CacheKey& key);
  [[nodiscard]] core::Status put(const CacheKey& key, std::shared_ptr<const Tile> tile);
  [[nodiscard]] core::Status pin(const CacheKey& key);
  void unpin(const CacheKey& key) noexcept;
  [[nodiscard]] std::uint64_t budget_bytes() const noexcept {
    return budget_bytes_;
  }
  [[nodiscard]] std::uint64_t used_bytes() const noexcept;
  [[nodiscard]] CacheDiagnostics diagnostics() const;

private:
  struct Entry final {
    std::shared_ptr<const Tile> tile;
    std::uint64_t bytes{};
    std::uint64_t access{};
    std::uint64_t pins{};
  };
  explicit MemoryTileCache(std::uint64_t budget_bytes) : budget_bytes_(budget_bytes) {}
  void evict_to_fit(std::uint64_t incoming_bytes);

  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  std::uint64_t budget_bytes_{};
  std::uint64_t used_bytes_{};
  std::uint64_t clock_{};
  CacheDiagnostics diagnostics_;
};

class DiskTileStore final {
public:
  DiskTileStore(std::filesystem::path directory, std::uint64_t capacity_bytes);
  [[nodiscard]] core::Status recover();
  [[nodiscard]] core::Result<std::shared_ptr<const Tile>> get(const CacheKey& key);
  [[nodiscard]] core::Status put(const CacheKey& key, const Tile& tile);
  [[nodiscard]] core::Status pin(const CacheKey& key);
  void unpin(const CacheKey& key) noexcept;
  [[nodiscard]] std::uint64_t disk_bytes() const;

private:
  [[nodiscard]] core::Result<std::filesystem::path> path_for(const CacheKey& key) const;
  void enforce_capacity();
  std::filesystem::path directory_;
  std::uint64_t capacity_bytes_{};
  mutable std::mutex mutex_;
  std::set<std::string> pinned_;
};

class IMultiResolutionStore {
public:
  virtual ~IMultiResolutionStore() = default;
  [[nodiscard]] virtual core::Result<std::shared_ptr<const Tile>> get(const TileRequest& request) = 0;
  [[nodiscard]] virtual core::Status put(const TileRequest& request, std::shared_ptr<const Tile> tile) = 0;
  [[nodiscard]] virtual core::Status pin(const TileRequest& request) = 0;
  virtual void unpin(const TileRequest& request) noexcept = 0;
  [[nodiscard]] virtual CacheDiagnostics diagnostics() const = 0;
};

class MultiResolutionTileStore final : public IMultiResolutionStore {
public:
  MultiResolutionTileStore(std::unique_ptr<MemoryTileCache> memory, std::unique_ptr<DiskTileStore> disk);
  [[nodiscard]] core::Result<std::shared_ptr<const Tile>> get(const TileRequest& request) override;
  [[nodiscard]] core::Status put(const TileRequest& request, std::shared_ptr<const Tile> tile) override;
  [[nodiscard]] core::Status pin(const TileRequest& request) override;
  void unpin(const TileRequest& request) noexcept override;
  [[nodiscard]] core::Result<std::shared_ptr<const Tile>>
  get_or_produce(const TileRequest& request, ITileProducer& producer, const CancellationToken& cancellation);
  void update_index_status(const ProgressiveIndexStatus& status);
  [[nodiscard]] CacheDiagnostics diagnostics() const override;

private:
  std::unique_ptr<MemoryTileCache> memory_;
  std::unique_ptr<DiskTileStore> disk_;
  mutable std::mutex mutex_;
  CacheDiagnostics diagnostics_;
};

class PrefetchQueue final {
public:
  void enqueue(TileRequest request);
  void cancel_pending() noexcept;
  [[nodiscard]] core::Status run_next(MultiResolutionTileStore& store, ITileProducer& producer);
  [[nodiscard]] core::Result<std::shared_ptr<const Tile>> run_interactive(MultiResolutionTileStore& store,
                                                                          ITileProducer& producer, TileRequest request,
                                                                          const CancellationToken& cancellation);
  [[nodiscard]] std::size_t pending() const noexcept;

private:
  struct Pending final {
    TileRequest request;
    CancellationToken cancellation;
  };
  mutable std::mutex mutex_;
  std::deque<Pending> pending_;
  std::optional<CancellationToken> active_;
};

} // namespace signal::data
