#pragma once

#include "domain/project.h"
#include "infrastructure/spectral_analysis.h"
#include "infrastructure/source_loader.h"
#include "infrastructure/raw_sample_reader.h"

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
    bool open(const FileMetadata& metadata, QString& error) {
        raw_.reset();if(mapped_&&ownedBytes_.isEmpty())file_.unmap(mapped_);mapped_=nullptr;
        ownedBytes_.clear();file_.close();sampleCount_=0;
        const auto path=QString::fromStdString(metadata.path);
        if(metadata.availability.status==LoadStatus::Loading||metadata.availability.status==LoadStatus::Failed){error="来源未读入或已失效";return false;}
        if(!metadata.availability.fingerprint.empty()&&iqSourceFingerprint(path).toStdString()!=metadata.availability.fingerprint){error="源文件指纹已改变，请重新读入";return false;}
        if(path.startsWith(":/")){
            if(metadata.sampleFormat!=SampleFormat{}){error="内置 IQ 资源只支持固定的 CI16/LE/IQ 单通道格式";return false;}
            if(!open(path,error))return false;
        }
        else {
            raw_=std::make_unique<RawSampleReader>();
            if(!raw_->open(path,metadata.sampleFormat,error)){raw_.reset();return false;}
            sampleCount_=raw_->sampleCount();format_=metadata.sampleFormat;
        }
        limitTo(availableSamples(metadata));return true;
    }
    bool isOpen() const { return mapped_ != nullptr || (raw_ && raw_->isOpen()); }
    std::uint64_t sampleCount() const { return sampleCount_; }
    void limitTo(SampleIndex count) { sampleCount_ = std::min(sampleCount_, count); }
    std::complex<double> sampleAt(std::uint64_t index) const;
    SpectralSource spectralSource() const;

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
    double powerDensityDb(const std::complex<double>& value, double normalization) const;

    QFile file_;
    std::unique_ptr<RawSampleReader> raw_;
    SampleFormat format_;
    QByteArray ownedBytes_;
    uchar* mapped_ = nullptr;
    std::uint64_t sampleCount_ = 0;
};

} // namespace signalstudio
