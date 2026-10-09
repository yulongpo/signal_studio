#pragma once

#include "domain/project.h"

#include <QByteArray>
#include <QFile>
#include <QSize>

#include <atomic>
#include <complex>
#include <functional>
#include <optional>
#include <vector>

namespace signalstudio {

struct Int16IqDescriptor {
    FileMetadata metadata;
    double declaredBandwidthHz = 0;
    std::uint64_t byteSize = 0;
};

// Reads the common little-endian interleaved signed-int16 format: I0,Q0,I1,Q1...
std::optional<Int16IqDescriptor> describeInt16IqFile(const QString& path, QString& error);

class Int16IqFile {
public:
    Int16IqFile() = default;
    ~Int16IqFile();
    Int16IqFile(const Int16IqFile&) = delete;
    Int16IqFile& operator=(const Int16IqFile&) = delete;

    bool open(const QString& path, QString& error);
    bool isOpen() const { return mapped_ != nullptr; }
    std::uint64_t sampleCount() const { return sampleCount_; }
    std::complex<double> sampleAt(std::uint64_t index) const;

    bool waveform(const TimeRange& range, int points, WaveformMode mode, std::vector<float>& output,
                  const std::function<bool()>& cancelled = {}) const;
    bool psd(const TimeRange& range, const FrequencyRange& frequencies, double sampleRateHz,
             double centerFrequencyHz, int fftSize, int points, std::vector<float>& output,
             const std::function<bool()>& cancelled = {}) const;
    bool spectrogram(const FileMetadata& metadata, const ViewRange& view, MainMode mode,
                     QSize pixels, int fftSize, std::vector<float>& output,
                     const std::function<bool()>& cancelled = {}) const;

private:
    std::complex<double> sample(std::uint64_t index) const;
    void spectrum(std::uint64_t first, int fftSize, std::vector<std::complex<double>>& output) const;
    double powerDensityDb(const std::complex<double>& value, double normalization) const;

    QFile file_;
    QByteArray ownedBytes_;
    uchar* mapped_ = nullptr;
    std::uint64_t sampleCount_ = 0;
};

} // namespace signalstudio
