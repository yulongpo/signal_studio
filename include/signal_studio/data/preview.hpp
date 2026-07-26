#pragma once

#include "signal_studio/data/signal.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace signal::data {

class CancellationToken final {
public:
  CancellationToken();
  void cancel() noexcept;
  [[nodiscard]] bool cancelled() const noexcept;

private:
  std::shared_ptr<std::atomic_bool> state_;
};

enum class QualityWarning : std::uint8_t {
  nan_present,
  infinity_present,
  all_zero,
  clipping,
  dc_offset,
  unusual_amplitude,
};

struct PreviewStatistics final {
  std::uint64_t sample_count{};
  std::uint64_t finite_component_count{};
  std::uint64_t nan_component_count{};
  std::uint64_t infinity_component_count{};
  double minimum{};
  double maximum{};
  double mean{};
  double rms{};
};

struct QuickSpectrum final {
  std::vector<double> magnitude;
  std::uint64_t analyzed_samples{};
  bool limited{true};
};

struct PreviewOptions final {
  std::uint64_t maximum_samples{4096};
  std::uint64_t maximum_read_bytes{16U * 1024U * 1024U};
  std::uint32_t spectrum_bins{256};
  double clipping_level{1.0};
  double dc_warning_ratio{0.1};
  double unusual_amplitude_level{1.0e6};
};

struct PreviewResult final {
  SampleRange analyzed_range;
  PreviewStatistics statistics;
  std::vector<QualityWarning> warnings;
  QuickSpectrum spectrum;
  bool whole_file{false};
  std::string scope_label{"bounded-preview"};
};

[[nodiscard]] core::Result<PreviewResult> create_bounded_preview(const std::filesystem::path& path,
                                                                 const SignalDescriptor& descriptor,
                                                                 const PreviewOptions& options,
                                                                 const CancellationToken& cancellation);

class PreviewCoordinator final {
public:
  [[nodiscard]] CancellationToken begin_request();
  void cancel_current() noexcept;

private:
  CancellationToken current_;
};

} // namespace signal::data
