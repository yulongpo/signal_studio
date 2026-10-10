#include "infrastructure/int16_iq_file.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace signalstudio {
namespace {

double unitScale(QString unit) {
    unit = unit.toLower();
    if (unit == "ghz" || unit == "gsps") return 1e9;
    if (unit == "mhz" || unit == "msps") return 1e6;
    if (unit == "khz" || unit == "ksps") return 1e3;
    return 1.0;
}

std::optional<double> parseFilenameValue(const QString& name, const QString& field, const QString& units) {
    const QRegularExpression expression(QStringLiteral("(?:^|_)") + field +
        QStringLiteral("([0-9]+(?:\\.[0-9]+)?)(") + units + QStringLiteral(")"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = expression.match(name);
    if (!match.hasMatch()) return std::nullopt;
    bool ok = false;
    const double value = match.captured(1).toDouble(&ok) * unitScale(match.captured(2));
    return ok && std::isfinite(value) && value >= 0 ? std::optional<double>{value} : std::nullopt;
}

bool validRange(const TimeRange& range, std::uint64_t samples) {
    return range.begin < range.end && range.end <= samples;
}
std::uint64_t scaledOffset(std::uint64_t span, std::uint64_t numerator, std::uint64_t denominator) {
    if (denominator == 0 || numerator == 0 || span == 0) return 0;
    numerator = std::min(numerator, denominator);
    return (span / denominator) * numerator + ((span % denominator) * numerator) / denominator;
}

} // namespace

std::optional<Int16IqDescriptor> describeInt16IqFile(const QString& path, QString& error) {
    QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        error = QStringLiteral("文件不存在或不可读：%1").arg(path);
        return std::nullopt;
    }
    const auto bytes = info.size();
    if (bytes <= 0 || bytes % 4 != 0) {
        error = QStringLiteral("int16 IQ 文件大小必须为正数且能被 4 整除（每个复采样点占 4 字节）");
        return std::nullopt;
    }

    const auto name = info.fileName();
    const QRegularExpression fsExpression(QStringLiteral("(?:^|_)FS([0-9]+(?:\\.[0-9]+)?)([kMGT]?sps)"),
                                          QRegularExpression::CaseInsensitiveOption);
    const auto fsMatch = fsExpression.match(name);
    if (!fsMatch.hasMatch()) {
        error = QStringLiteral("文件名未包含 FS 参数（例如 FS102.4Msps），无法确定采样率");
        return std::nullopt;
    }
    bool fsOk = false;
    const double sampleRate = fsMatch.captured(1).toDouble(&fsOk) * unitScale(fsMatch.captured(2));
    const auto center = parseFilenameValue(name, QStringLiteral("FC"), QStringLiteral("GHz|MHz|kHz|Hz"));
    const auto bandwidth = parseFilenameValue(name, QStringLiteral("BW"), QStringLiteral("GHz|MHz|kHz|Hz"));
    if (!fsOk || !std::isfinite(sampleRate) || sampleRate <= 0 || !center) {
        error = QStringLiteral("文件名必须包含有效的 FS 与 FC 参数，例如 FS102.4Msps_FC830MHz");
        return std::nullopt;
    }

    Int16IqDescriptor result;
    result.metadata.name = name.toUtf8().toStdString();
    result.metadata.path = info.absoluteFilePath().toUtf8().toStdString();
    result.metadata.sampleRateHz = sampleRate;
    result.metadata.centerFrequencyHz = *center;
    result.metadata.sampleCount = static_cast<SampleIndex>(bytes / 4);
    result.metadata.demo = false;
    result.metadata.demoSeed = 1;
    result.declaredBandwidthHz = bandwidth.value_or(0.0);
    result.byteSize = static_cast<std::uint64_t>(bytes);
    error.clear();
    return result;
}

Int16IqFile::~Int16IqFile() {
    if (mapped_ && ownedBytes_.isEmpty()) file_.unmap(mapped_);
}

bool Int16IqFile::open(const QString& path, QString& error) {
    raw_.reset();format_=SampleFormat{};
    if (mapped_ && ownedBytes_.isEmpty()) file_.unmap(mapped_);
    file_.close();
    mapped_ = nullptr; ownedBytes_.clear(); sampleCount_ = 0;
    file_.setFileName(path);
    if (!file_.open(QIODevice::ReadOnly)) { error = file_.errorString(); return false; }
    const auto bytes = file_.size();
    if (bytes <= 0 || bytes % 4 != 0) { error = QStringLiteral("IQ 文件长度无效"); file_.close(); return false; }
    if (path.startsWith(QStringLiteral(":/"))) {
        ownedBytes_ = file_.readAll();
        file_.close();
        if (ownedBytes_.size() != bytes) { error = QStringLiteral("内置 IQ 资源读取不完整"); ownedBytes_.clear(); return false; }
        mapped_ = reinterpret_cast<uchar*>(ownedBytes_.data());
    } else {
        mapped_ = file_.map(0, bytes);
        if (!mapped_) { error = QStringLiteral("无法将 IQ 文件映射到 64 位地址空间：%1").arg(file_.errorString()); file_.close(); return false; }
    }
    sampleCount_ = static_cast<std::uint64_t>(bytes / 4);
    error.clear();
    return true;
}

std::complex<double> Int16IqFile::sample(std::uint64_t index) const {
    if(raw_){std::complex<double> value;raw_->sampleAt(index,value);return value;}
    const auto offset = static_cast<qsizetype>(index * 4);
    const auto i = qFromLittleEndian<qint16>(mapped_ + offset);
    const auto q = qFromLittleEndian<qint16>(mapped_ + offset + 2);
    return {static_cast<double>(i) / 32768.0, static_cast<double>(q) / 32768.0};
}

std::complex<double> Int16IqFile::sampleAt(std::uint64_t index) const {
    return isOpen() && index < sampleCount_ ? sample(index) : std::complex<double>{};
}

SpectralSource Int16IqFile::spectralSource() const {
    SpectralSource source;
    source.real = format_.structure==SampleStructure::Real;
    source.read = [this](TimeRange range, std::vector<std::complex<float>>& output, const SpectralCancel& cancel) {
        if (!isOpen() || !validRange(range, sampleCount_)) return false;
        if(raw_)return raw_->read(range,output,cancel);
        output.resize(static_cast<std::size_t>(range.end - range.begin));
        for (std::size_t n = 0; n < output.size(); ++n) {
            if ((n & 1023) == 0 && cancel && cancel()) return false;
            output[n] = std::complex<float>(sample(range.begin + n));
        }
        return true;
    };
    return source;
}

bool Int16IqFile::waveform(const TimeRange& range, int points, WaveformMode mode, std::vector<float>& output,
                           const std::function<bool()>& cancelled) const {
    if (!isOpen() || !validRange(range, sampleCount_) || points < 1) return false;
    output.resize(static_cast<std::size_t>(points));
    const auto span = range.end - range.begin;
    for (int x = 0; x < points; ++x) {
        if (cancelled && cancelled()) return false;
        const auto first = range.begin + scaledOffset(span, static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(points));
        const auto last = range.begin + scaledOffset(span, static_cast<std::uint64_t>(x + 1), static_cast<std::uint64_t>(points));
        if (mode == WaveformMode::I || mode == WaveformMode::Q) {
            const auto index = std::min(range.end - 1, first + (last - first) / 2);
            const auto value = sample(index);
            output[static_cast<std::size_t>(x)] = static_cast<float>((mode == WaveformMode::I ? value.real() : value.imag()) * adcScale(format_));
            continue;
        }
        const auto bucket = std::max<std::uint64_t>(1, last - first);
        // Envelope scans every sample to retain isolated pulses. RMS overview
        // uses evenly distributed probes with a bounded cost.
        const auto count = mode == WaveformMode::Envelope ? bucket : std::min<std::uint64_t>(bucket, 2048);
        double power = 0;
        double peak = 0;
        for (std::uint64_t n = 0; n < count; ++n) {
            if ((n & 1023) == 0 && cancelled && cancelled()) return false;
            const auto index = first + (count == bucket ? n : n * bucket / count);
            const double magnitudeSquared = std::norm(sample(index));
            power += magnitudeSquared;
            peak = std::max(peak, magnitudeSquared);
        }
        const double magnitude = mode == WaveformMode::Envelope ? std::sqrt(peak) : std::sqrt(power / count);
        output[static_cast<std::size_t>(x)] = static_cast<float>(magnitude * adcScale(format_));
    }
    return true;
}

bool Int16IqFile::psd(const TimeRange& range, const FrequencyRange& frequencies, double sampleRateHz,
                      double centerFrequencyHz, int fftSize, int points, std::vector<float>& output,
                      const std::function<bool()>& cancelled) const {
    if (!isOpen() || points < 1) return false;
    auto source = spectralSource(); source.sampleRateHz = sampleRateHz;
    if(format_.structure==SampleStructure::Real)centerFrequencyHz=0;
    const auto data = analyzeSpectrogram(source, range,
        {frequencies.lowerHz - centerFrequencyHz, frequencies.upperHz - centerFrequencyHz}, fftSize, 64, cancelled);
    const auto average = averageSpectrum(*data);
    if (!average) return false;
    output.resize(points);
    for (int x = 0; x < points; ++x) {
        const auto bin = std::min(average->linearPower.size() - 1,
            static_cast<std::size_t>(x) * average->linearPower.size() / points);
        output[x] = average->dbAt(bin);
    }
    return true;
}

bool Int16IqFile::spectrogram(const FileMetadata& metadata, const ViewRange& view, MainMode mode,
                              QSize pixels, int fftSize, std::vector<float>& output,
                              const std::function<bool()>& cancelled) const {
    auto source = spectralSource(); source.sampleRateHz = metadata.sampleRateHz;
    const auto data = analyzeSpectrogram(source, view.time,
        {view.frequency.lowerHz - analysisFrequencyOffset(metadata), view.frequency.upperHz - analysisFrequencyOffset(metadata)},
        fftSize, mode == MainMode::Waterfall ? pixels.height() : pixels.width(), cancelled);
    output = spectralRaster(*data, pixels.width(), pixels.height(), mode);
    return !output.empty();
}

} // namespace signalstudio
