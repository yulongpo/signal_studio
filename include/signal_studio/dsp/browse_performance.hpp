#pragma once

#include "signal_studio/data/index.hpp"
#include "signal_studio/dsp/analysis.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace signal::dsp {

/// 以一个真实录制文件为物理后端、以重复映射为逻辑地址空间的有界浏览源。
///
/// 该类型不会创建逻辑大小对应的稀疏文件，也不会把整个物理文件读入内存。
class LogicalRecordingSource final {
public:
  struct Plan final {
    std::filesystem::path physical_path;
    std::uint64_t physical_file_bytes{};
    std::uint64_t logical_file_bytes{};
    std::uint64_t frame_bytes{};
    std::uint64_t physical_frames{};
    std::uint64_t logical_frames{};
    std::uint64_t initial_window_frames{};
    std::uint64_t bounded_working_set_bytes{};
    std::uint64_t logical_repetition_count{};
    std::string source_fingerprint;
    std::string descriptor_digest;
    std::string estimate_source;
    bool navigation_ready{};
    bool real_recording_input{};
    bool logical_repeat_mapping{};
  };

  [[nodiscard]] static core::Result<std::shared_ptr<LogicalRecordingSource>>
  open(std::filesystem::path physical_path, data::SignalDescriptor descriptor, std::string data_source_version_id,
       std::uint64_t logical_file_bytes, std::uint64_t memory_budget_bytes,
       std::uint64_t maximum_window_frames = 262'144U);

  [[nodiscard]] const Plan& plan() const noexcept;
  [[nodiscard]] const data::SignalDescriptor& descriptor() const noexcept;
  [[nodiscard]] std::string_view data_source_version_id() const noexcept;
  [[nodiscard]] core::Result<data::RawReadResult> read_logical(const data::SampleRange& logical_range,
                                                               std::uint64_t maximum_read_bytes) const;

private:
  LogicalRecordingSource(std::shared_ptr<data::FileDataSource> physical_source, data::SignalDescriptor descriptor,
                         std::string data_source_version_id, Plan plan, std::uint64_t maximum_window_frames);

  std::shared_ptr<data::FileDataSource> physical_source_;
  data::SignalDescriptor descriptor_;
  std::string data_source_version_id_;
  Plan plan_;
  std::uint64_t maximum_window_frames_{};
};

struct BrowseViewport final {
  data::SampleRange logical_range;
  std::uint32_t pixel_width{1024U};
  std::uint32_t spectrogram_rows{32U};
};

struct BrowseCommandFeedback final {
  std::uint64_t generation{};
  BrowseViewport viewport;
  std::string status_text{"正在细化"};
  bool visible{};
};

struct AtomicBrowseFrame final {
  std::uint64_t generation{};
  BrowseViewport viewport;
  std::vector<float> time_envelope;
  std::vector<float> spectrum_db_per_hz;
  std::vector<float> spectrogram_db_per_hz;
  std::uint32_t spectrogram_columns{};
  std::uint32_t spectrogram_rows{};
  std::string source_scope;
  bool cache_hit{};
  bool complete{};
};

struct SamplingOverview final {
  std::string label{"采样概览"};
  std::string source_scope;
  std::vector<data::TimeSummaryBin> bins;
  std::uint64_t physical_samples_read{};
  std::uint64_t logical_samples_represented{};
  bool complete{};
};

/// UI 可直接消费的交互代际与三图原子发布器。
class BrowseInteractionSequencer final {
public:
  [[nodiscard]] BrowseCommandFeedback issue(BrowseViewport viewport);
  [[nodiscard]] bool publish(std::shared_ptr<const AtomicBrowseFrame> frame);
  [[nodiscard]] BrowseCommandFeedback feedback() const;
  [[nodiscard]] std::shared_ptr<const AtomicBrowseFrame> frame() const;

private:
  mutable std::mutex mutex_;
  std::uint64_t generation_{};
  BrowseCommandFeedback feedback_;
  std::shared_ptr<const AtomicBrowseFrame> frame_;
};

/// 基于真实 SignalData 文件读取、SignalDSP 和多级瓦片缓存的浏览性能路径。
class BrowsePerformanceSession final {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<BrowsePerformanceSession>>
  create(std::shared_ptr<LogicalRecordingSource> source, std::shared_ptr<IFftBackend> fft_backend,
         std::filesystem::path cache_directory, std::uint64_t memory_cache_bytes = 256U * 1024U * 1024U,
         std::uint64_t disk_cache_bytes = 256U * 1024U * 1024U);

  [[nodiscard]] core::Result<std::shared_ptr<const AtomicBrowseFrame>>
  build_frame(const BrowseViewport& viewport, std::uint64_t generation, bool persist_cache = true);
  [[nodiscard]] core::Result<std::shared_ptr<const AtomicBrowseFrame>>
  restore_cached_frame(const BrowseViewport& viewport, std::uint64_t generation);
  [[nodiscard]] core::Result<SamplingOverview> build_sampling_overview(std::uint64_t maximum_samples = 262'144U,
                                                                       std::uint32_t output_bins = 1024U);
  [[nodiscard]] const LogicalRecordingSource::Plan& plan() const noexcept;

private:
  BrowsePerformanceSession(std::shared_ptr<LogicalRecordingSource> source, std::shared_ptr<IFftBackend> fft_backend,
                           std::unique_ptr<data::MemoryTileCache> memory_cache,
                           std::unique_ptr<data::DiskTileStore> disk_cache);
  [[nodiscard]] data::CacheKey make_key(const BrowseViewport& viewport, std::string parameter,
                                        data::TileKind kind) const;
  [[nodiscard]] core::Result<std::shared_ptr<const data::Tile>> restore_tile(const data::CacheKey& key);
  [[nodiscard]] core::Status store_tile(const data::CacheKey& key, const data::Tile& tile);

  std::shared_ptr<LogicalRecordingSource> source_;
  std::shared_ptr<IFftBackend> fft_backend_;
  std::unique_ptr<data::MemoryTileCache> memory_cache_;
  std::unique_ptr<data::DiskTileStore> disk_cache_;
};

} // namespace signal::dsp
