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
    return ok && std::isfinite(value) && value > 0 ? std::optional<double>{value} : std::nullopt;
}

bool validRange(const TimeRange& range, std::uint64_t samples) {
    return range.begin < range.end && range.end <= samples;
}
std::uint64_t scaledOffset(std::uint64_t span, std::uint64_t numerator, std::uint64_t denominator) {
    if (denominator == 0 || numerator == 0 || span == 0) return 0;
    numerator = std::min(numerator, denominator);
    return (span / denominator) * numerator + ((span % denominator) * numerator) / denominator;
}

void fft(std::vector<std::complex<double>>& data) {
    const auto size = data.size();
    for (std::size_t i = 1, j = 0; i < size; ++i) {
        std::size_t bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const auto angle = -2.0 * std::numbers::pi / static_cast<double>(length);
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (std::size_t base = 0; base < size; base += length) {
            std::complex<double> twiddle(1.0, 0.0);
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const auto even = data[base + offset];
                const auto odd = data[base + offset + length / 2] * twiddle;
                data[base + offset] = even + odd;
                data[base + offset + length / 2] = even - odd;
                twiddle *= root;
            }
        }
    }
}

double hannPowerSum(int size) {
    double sum = 0;
    for (int i = 0; i < size; ++i) {
        const double window = .5 - .5 * std::cos(2 * std::numbers::pi * i / (size - 1));
        sum += window * window;
    }
    return sum;
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
    if (mapped_) file_.unmap(mapped_);
}

bool Int16IqFile::open(const QString& path, QString& error) {
    if (mapped_) { file_.unmap(mapped_); mapped_ = nullptr; }
    file_.setFileName(path);
    if (!file_.open(QIODevice::ReadOnly)) { error = file_.errorString(); return false; }
    const auto bytes = file_.size();
    if (bytes <= 0 || bytes % 4 != 0) { error = QStringLiteral("IQ 文件长度无效"); file_.close(); return false; }
    mapped_ = file_.map(0, bytes);
    if (!mapped_) { error = QStringLiteral("无法将 IQ 文件映射到 64 位地址空间：%1").arg(file_.errorString()); file_.close(); return false; }
    sampleCount_ = static_cast<std::uint64_t>(bytes / 4);
    error.clear();
    return true;
}

std::complex<double> Int16IqFile::sample(std::uint64_t index) const {
    const auto offset = static_cast<qsizetype>(index * 4);
    const auto i = qFromLittleEndian<qint16>(mapped_ + offset);
    const auto q = qFromLittleEndian<qint16>(mapped_ + offset + 2);
    return {static_cast<double>(i) / 32768.0, static_cast<double>(q) / 32768.0};
}

void Int16IqFile::spectrum(std::uint64_t first, int fftSize, std::vector<std::complex<double>>& output) const {
    output.resize(static_cast<std::size_t>(fftSize));
    for (int i = 0; i < fftSize; ++i) {
        const double hann = .5 - .5 * std::cos(2 * std::numbers::pi * i / (fftSize - 1));
        const auto index = first + static_cast<std::uint64_t>(i);
        output[static_cast<std::size_t>(i)] = index < sampleCount_ ? sample(index) * hann : std::complex<double>{};
    }
    fft(output);
}

double Int16IqFile::powerDensityDb(const std::complex<double>& value, double normalization) const {
    return 10.0 * std::log10(std::max(std::norm(value) / normalization, 1e-20));
}

bool Int16IqFile::waveform(const TimeRange& range, int points, WaveformMode mode, std::vector<float>& output,
                           const std::function<bool()>& cancelled) const {
    if (!mapped_ || !validRange(range, sampleCount_) || points < 1) return false;
    output.resize(static_cast<std::size_t>(points));
    const auto span = range.end - range.begin;
    for (int x = 0; x < points; ++x) {
        if (cancelled && cancelled()) return false;
        const auto first = range.begin + scaledOffset(span, static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(points));
        const auto last = range.begin + scaledOffset(span, static_cast<std::uint64_t>(x + 1), static_cast<std::uint64_t>(points));
        if (mode == WaveformMode::I || mode == WaveformMode::Q) {
            const auto index = std::min(range.end - 1, first + (last - first) / 2);
            const auto value = sample(index);
            output[static_cast<std::size_t>(x)] = static_cast<float>(mode == WaveformMode::I ? value.real() : value.imag());
            continue;
        }
        const auto bucket = std::max<std::uint64_t>(1, last - first);
        // A short contiguous block per display point gives a stable IQ envelope
        // while touching only a few mapped pages, even for multi-gigabyte files.
        const auto count = std::min<std::uint64_t>(bucket, 1024);
        const auto start = first + (bucket - count) / 2;
        double power = 0;
        for (std::uint64_t n = 0; n < count; ++n) power += std::norm(sample(start + n));
        power /= static_cast<double>(count);
        output[static_cast<std::size_t>(x)] = static_cast<float>(10 * std::log10(std::max(power, 1e-12)));
    }
    return true;
}

bool Int16IqFile::psd(const TimeRange& range, const FrequencyRange& frequencies, double sampleRateHz,
                      double centerFrequencyHz, int fftSize, int points, std::vector<float>& output,
                      const std::function<bool()>& cancelled) const {
    if (!mapped_ || !validRange(range, sampleCount_) || fftSize < 16 || (fftSize & (fftSize - 1)) || points < 1 || sampleRateHz <= 0) return false;
    const auto span = range.end - range.begin;
    while (fftSize > static_cast<std::uint64_t>(span) && fftSize > 16) fftSize >>= 1;
    if (fftSize > span) return false;
    constexpr int maximumWindows = 64;
    const auto maximumStart = span - static_cast<std::uint64_t>(fftSize);
    const auto hop = static_cast<std::uint64_t>(std::max(1, fftSize / 2));
    const auto availableWindows = 1 + maximumStart / hop;
    const int windows = static_cast<int>(std::min<std::uint64_t>(maximumWindows, availableWindows));
    const double normalization = sampleRateHz * hannPowerSum(fftSize);
    std::vector<double> average(static_cast<std::size_t>(fftSize));
    std::vector<std::complex<double>> bins;
    for (int frame = 0; frame < windows; ++frame) {
        if (cancelled && cancelled()) return false;
        const auto offset = windows == 1 ? maximumStart / 2 :
            scaledOffset(maximumStart, static_cast<std::uint64_t>(frame), static_cast<std::uint64_t>(windows - 1));
        spectrum(range.begin + offset, fftSize, bins);
        for (int k = 0; k < fftSize; ++k) {
            const int shifted = (k + fftSize / 2) % fftSize;
            average[static_cast<std::size_t>(shifted)] += std::norm(bins[static_cast<std::size_t>(k)]) / windows;
        }
    }
    output.resize(static_cast<std::size_t>(points));
    const double firstHz = centerFrequencyHz - sampleRateHz / 2;
    for (int x = 0; x < points; ++x) {
        const double u = points == 1 ? .5 : static_cast<double>(x) / (points - 1);
        const double hz = frequencies.lowerHz + u * (frequencies.upperHz - frequencies.lowerHz);
        const auto index = static_cast<int>(std::lround((hz - firstHz) * fftSize / sampleRateHz));
        const auto clamped = std::clamp(index, 0, fftSize - 1);
        output[static_cast<std::size_t>(x)] = static_cast<float>(powerDensityDb({std::sqrt(average[static_cast<std::size_t>(clamped)]), 0}, normalization));
    }
    return true;
}

bool Int16IqFile::spectrogram(const FileMetadata& metadata, const ViewRange& view, MainMode mode,
                              QSize pixels, int fftSize, std::vector<float>& output,
                              const std::function<bool()>& cancelled) const {
    const int width = pixels.width(), height = pixels.height();
    if (!mapped_ || !validRange(view.time, sampleCount_) || width < 2 || height < 2 || fftSize < 16 ||
        (fftSize & (fftSize - 1)) || metadata.sampleRateHz <= 0) return false;
    const auto timeSpan = view.time.end - view.time.begin;
    while (fftSize > static_cast<std::uint64_t>(timeSpan) && fftSize > 16) fftSize >>= 1;
    if (fftSize > timeSpan) return false;
    output.assign(static_cast<std::size_t>(width) * height, -140.0f);
    const bool waterfall = mode == MainMode::Waterfall;
    const int lineCount = std::min(waterfall ? height : width, std::max(1, 8'000'000 / fftSize));
    const double binHz = metadata.sampleRateHz / fftSize;
    const double normalization = metadata.sampleRateHz * hannPowerSum(fftSize);
    const double lower = metadata.centerFrequencyHz - metadata.sampleRateHz / 2;
    const auto maximumStart = timeSpan - static_cast<std::uint64_t>(fftSize);
    std::vector<std::complex<double>> bins;
    for (int line = 0; line < lineCount; ++line) {
        if (cancelled && cancelled()) return false;
        const auto offset = lineCount == 1 ? maximumStart / 2 :
            scaledOffset(maximumStart, static_cast<std::uint64_t>(line), static_cast<std::uint64_t>(lineCount - 1));
        const auto first = view.time.begin + offset;
        spectrum(first, fftSize, bins);
        const int rows = waterfall ? width : height;
        for (int row = 0; row < rows; ++row) {
            const double v = rows == 1 ? .5 : static_cast<double>(row) / (rows - 1);
            const double frequency = waterfall ? view.frequency.lowerHz + v * (view.frequency.upperHz - view.frequency.lowerHz) :
                view.frequency.upperHz - v * (view.frequency.upperHz - view.frequency.lowerHz);
            const double nextFrequency = waterfall ? view.frequency.lowerHz + std::min(1.0, v + 1.0 / (rows - 1)) * (view.frequency.upperHz - view.frequency.lowerHz) :
                view.frequency.upperHz - std::min(1.0, v + 1.0 / (rows - 1)) * (view.frequency.upperHz - view.frequency.lowerHz);
            int firstBin = static_cast<int>(std::floor((std::min(frequency, nextFrequency) - lower) / binHz));
            int lastBin = static_cast<int>(std::ceil((std::max(frequency, nextFrequency) - lower) / binHz));
            firstBin = std::clamp(firstBin, 0, fftSize - 1); lastBin = std::clamp(lastBin, firstBin + 1, fftSize);
            double peak = 0;
            for (int k = firstBin; k < lastBin; ++k) {
                const int fftIndex = (k + fftSize / 2) % fftSize;
                peak = std::max(peak, std::norm(bins[static_cast<std::size_t>(fftIndex)]));
            }
            const float db = static_cast<float>(powerDensityDb({std::sqrt(peak), 0}, normalization));
            if (waterfall) {
                const int y = std::min(height - 1, static_cast<int>((line + .5) * height / lineCount));
                output[static_cast<std::size_t>(y) * width + row] = db;
            } else {
                const int x = std::min(width - 1, static_cast<int>((line + .5) * width / lineCount));
                output[static_cast<std::size_t>(row) * width + x] = db;
            }
        }
    }
    // At large FFT sizes fewer independent columns are computed; stretch those
    // columns across the requested raster without repeating the FFT work.
    if (lineCount < (waterfall ? height : width)) {
        const auto original = output;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int sourceLine = std::min(lineCount - 1, static_cast<int>((waterfall ? y + .5 : x + .5) * lineCount / (waterfall ? height : width)));
            const int sourceX = waterfall ? x : std::min(width - 1, static_cast<int>((sourceLine + .5) * width / lineCount));
            const int sourceY = waterfall ? std::min(height - 1, static_cast<int>((sourceLine + .5) * height / lineCount)) : y;
            output[static_cast<std::size_t>(y) * width + x] = original[static_cast<std::size_t>(sourceY) * width + sourceX];
        }
    }
    return true;
}

} // namespace signalstudio
