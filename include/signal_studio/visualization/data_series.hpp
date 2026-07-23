#pragma once

#include "signal_studio/core/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::visualization {

/// Logical kind of a data series, driving default rendering and axis selection.
enum class SeriesKind : std::uint8_t {
  time_waveform = 0,
  spectrum = 1,
  spectrogram = 2,
  constellation = 3,
  eye_diagram = 4,
  scalar_trace = 5,
};

/// Read-only data series bound to a chart view (API-VIS-001). Implementations are Qt-free value
/// types so the public Visualization API exposes no QWidget.
class IDataSeries {
public:
  virtual ~IDataSeries() = default;
  [[nodiscard]] virtual SeriesKind kind() const noexcept = 0;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t point_count() const noexcept = 0;
};

/// Real-valued time-domain trace (samples + sample rate).
class RealSeries final : public IDataSeries {
public:
  RealSeries(std::string name, std::vector<double> samples, double sample_rate_hz);
  SeriesKind kind() const noexcept override {
    return SeriesKind::time_waveform;
  }
  std::string_view name() const noexcept override {
    return name_;
  }
  std::uint64_t point_count() const noexcept override {
    return samples_.size();
  }
  [[nodiscard]] std::span<const double> samples() const noexcept {
    return samples_;
  }
  [[nodiscard]] double sample_rate_hz() const noexcept {
    return sample_rate_hz_;
  }

private:
  std::string name_;
  std::vector<double> samples_;
  double sample_rate_hz_;
};

/// One-sided or two-sided power spectrum (frequency axis + power, typically dB/Hz).
class SpectrumSeries final : public IDataSeries {
public:
  SpectrumSeries(std::string name, std::vector<double> frequencies_hz, std::vector<double> power_db,
                 double sample_rate_hz);
  SeriesKind kind() const noexcept override {
    return SeriesKind::spectrum;
  }
  std::string_view name() const noexcept override {
    return name_;
  }
  std::uint64_t point_count() const noexcept override {
    return frequencies_.size();
  }
  [[nodiscard]] std::span<const double> frequencies_hz() const noexcept {
    return frequencies_;
  }
  [[nodiscard]] std::span<const double> power_db() const noexcept {
    return power_db_;
  }
  [[nodiscard]] double sample_rate_hz() const noexcept {
    return sample_rate_hz_;
  }

private:
  std::string name_;
  std::vector<double> frequencies_;
  std::vector<double> power_db_;
  double sample_rate_hz_;
};

/// STFT/spectrogram matrix: frames x freq_bins, row-major magnitudes (dB).
class SpectrogramSeries final : public IDataSeries {
public:
  SpectrogramSeries(std::string name, std::vector<double> time_bins, std::vector<double> freq_bins,
                    std::vector<double> magnitudes_db, std::uint64_t frame_count, std::uint64_t freq_count);
  SeriesKind kind() const noexcept override {
    return SeriesKind::spectrogram;
  }
  std::string_view name() const noexcept override {
    return name_;
  }
  std::uint64_t point_count() const noexcept override {
    return magnitudes_.size();
  }
  [[nodiscard]] std::span<const double> time_bins() const noexcept {
    return time_bins_;
  }
  [[nodiscard]] std::span<const double> freq_bins() const noexcept {
    return freq_bins_;
  }
  [[nodiscard]] std::span<const double> magnitudes_db() const noexcept {
    return magnitudes_;
  }
  [[nodiscard]] std::uint64_t frame_count() const noexcept {
    return frame_count_;
  }
  [[nodiscard]] std::uint64_t freq_count() const noexcept {
    return freq_count_;
  }

private:
  std::string name_;
  std::vector<double> time_bins_;
  std::vector<double> freq_bins_;
  std::vector<double> magnitudes_;
  std::uint64_t frame_count_;
  std::uint64_t freq_count_;
};

/// Complex IQ samples for constellation/eye-diagram views.
class ComplexSeries final : public IDataSeries {
public:
  ComplexSeries(std::string name, std::vector<double> real, std::vector<double> imag, double sample_rate_hz);
  SeriesKind kind() const noexcept override {
    return SeriesKind::constellation;
  }
  std::string_view name() const noexcept override {
    return name_;
  }
  std::uint64_t point_count() const noexcept override {
    return real_.size();
  }
  [[nodiscard]] std::span<const double> real() const noexcept {
    return real_;
  }
  [[nodiscard]] std::span<const double> imag() const noexcept {
    return imag_;
  }
  [[nodiscard]] double sample_rate_hz() const noexcept {
    return sample_rate_hz_;
  }

private:
  std::string name_;
  std::vector<double> real_;
  std::vector<double> imag_;
  double sample_rate_hz_;
};

} // namespace signal::visualization
