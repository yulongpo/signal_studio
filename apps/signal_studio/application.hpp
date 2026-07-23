#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/dsp/fft.hpp"
#include "signal_studio/dsp/spectrum.hpp"
#include "signal_studio/dsp/statistics.hpp"
#include "signal_studio/dsp/stft.hpp"
#include "signal_studio/task_runtime/task_runtime.hpp"
#include "wideband_narrowband.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace signal::studio {

/// Parsed filename hint for SC16 captures (e.g. x310_capture_cf1245MHz_sr50MSps_...sc16).
struct FilenameHint final {
  double center_frequency_hz{};
  double sample_rate_hz{};
  bool had_center_frequency{false};
  bool had_sample_rate{false};
  friend bool operator==(const FilenameHint&, const FilenameHint&) = default;
};

[[nodiscard]] FilenameHint parse_capture_filename(const std::filesystem::path& path);

/// Outcome of a successful import: data source + descriptor + version id + readable facts.
struct ImportResult final {
  std::shared_ptr<data::FileDataSource> source;
  data::SignalDescriptor descriptor;
  std::string data_source_version_id;
  std::uint64_t total_samples{};
  friend bool operator==(const ImportResult&, const ImportResult&) = default;
};

/// Composition root for the Signal Studio application. Owns the task runtime, FFT backend and
/// adapter registry; provides import/read/analyze operations used by both the GUI and the
/// headless self-test. Qt-free so it can be tested without a QApplication.
class Application final {
public:
  Application();
  ~Application();
  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  [[nodiscard]] task::TaskRuntime& task_runtime() noexcept;
  [[nodiscard]] bool fft_available() const noexcept;

  /// WAV auto-detect import (stereo IQ assumed).
  [[nodiscard]] core::Result<ImportResult> import_wav(const std::filesystem::path& path);
  /// RAW import with an explicit descriptor.
  [[nodiscard]] core::Result<ImportResult> import_raw(const std::filesystem::path& path,
                                                      data::SignalDescriptor descriptor);
  /// SC16 import: builds an int16 interleaved-IQ descriptor from filename hints (cf/sr) and
  /// optional overrides.
  [[nodiscard]] core::Result<ImportResult> import_sc16(const std::filesystem::path& path,
                                                       std::optional<double> override_sample_rate_hz = {},
                                                       std::optional<double> override_center_frequency_hz = {});

  /// Read a bounded window of samples [begin, begin+count).
  [[nodiscard]] core::Result<data::SignalSlice> read_samples(const data::FileDataSource& source, std::uint64_t begin,
                                                             std::uint64_t count, std::uint64_t maximum_read_bytes,
                                                             std::function<bool()> cancel = {});

  /// Welch PSD of a slice. Requires an FFT backend (CUDA in this environment).
  [[nodiscard]] core::Result<dsp::PsdResult> analyze_psd(const data::SignalSlice& slice, double sample_rate_hz,
                                                         std::uint64_t nfft, std::uint64_t overlap_samples);
  /// STFT of a slice. Requires an FFT backend.
  [[nodiscard]] core::Result<dsp::StftResult> analyze_stft(const data::SignalSlice& slice, double sample_rate_hz,
                                                           std::uint64_t nfft, std::uint64_t hop_samples);

  /// Extract a narrowband channel from a wideband complex slice (digital down-conversion:
  /// frequency shift, low-pass filter, resample). No FFT backend required.
  [[nodiscard]] core::Result<NarrowbandChannel> extract_narrowband(const data::SignalSlice& wideband,
                                                                   double sample_rate_hz,
                                                                   const NarrowbandChannelSpec& spec,
                                                                   std::uint64_t source_start_sample = 0);

private:
  std::unique_ptr<task::TaskRuntime> runtime_;
  std::unique_ptr<dsp::IFftBackend> fft_;
  std::unique_ptr<dsp::WelchPsdEstimator> psd_;
  std::unique_ptr<dsp::StftProcessor> stft_;
};

} // namespace signal::studio
