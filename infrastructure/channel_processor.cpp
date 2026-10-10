#include "infrastructure/channel_processor.h"

#include "infrastructure/int16_iq_file.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace signalstudio {
namespace {
using SignedIndex = std::int64_t;

SignedIndex floorDiv(SignedIndex value, SignedIndex divisor) {
    const SignedIndex quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}

SignedIndex ceilDiv(SignedIndex value, SignedIndex divisor) {
    return -floorDiv(-value, divisor);
}

std::uint64_t gcd(std::uint64_t left, std::uint64_t right) {
    while (right) { const auto remainder = left % right; left = right; right = remainder; }
    return left;
}

bool multiplyDivideFloor(std::uint64_t value, std::uint64_t numerator,
                         std::uint64_t denominator, std::uint64_t& output) {
    if (!denominator) return false;
    const auto quotient = value / denominator;
    const auto remainder = value % denominator;
    if (numerator && quotient > std::numeric_limits<std::uint64_t>::max() / numerator) return false;
    const auto whole = quotient * numerator;
    if (numerator && remainder > std::numeric_limits<std::uint64_t>::max() / numerator) return false;
    const auto fraction = remainder * numerator / denominator;
    if (fraction > std::numeric_limits<std::uint64_t>::max() - whole) return false;
    output = whole + fraction;
    return output >= whole;
}

std::uint64_t roundHz(double value) {
    if (!std::isfinite(value) || value <= 0 || value > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) return 0;
    return static_cast<std::uint64_t>(std::llround(value));
}

double kaiserBeta(double attenuation) {
    if (attenuation > 50) return .1102 * (attenuation - 8.7);
    if (attenuation >= 21) return .5842 * std::pow(attenuation - 21, .4) + .07886 * (attenuation - 21);
    return 0;
}

bool lowpass(double sampleRate, double passEdge, double stopEdge, double attenuation,
             std::vector<double>& taps) {
    if (!(sampleRate > 0) || !(passEdge > 0) || !(stopEdge > passEdge) || stopEdge >= sampleRate / 2)
        return false;
    const double transition = (stopEdge - passEdge) / sampleRate;
    const double estimated = std::ceil((attenuation - 8.0) / (2.285 * 2 * std::numbers::pi * transition)) + 1;
    if (!std::isfinite(estimated) || estimated < 3 || estimated > 131071) return false;
    int count = static_cast<int>(estimated);
    if ((count & 1) == 0) ++count;
    const int half = count / 2;
    const double beta = kaiserBeta(attenuation);
    const double cutoff = (passEdge + stopEdge) / 2;
    const double norm = std::cyl_bessel_i(0.0, beta);
    taps.resize(static_cast<std::size_t>(count));
    double sum = 0;
    for (int index = 0; index < count; ++index) {
        const int offset = index - half;
        const double ideal = offset == 0 ? 2 * cutoff / sampleRate :
            std::sin(2 * std::numbers::pi * cutoff * offset / sampleRate) /
                (std::numbers::pi * offset);
        const double ratio = static_cast<double>(offset) / half;
        const double window = std::cyl_bessel_i(0.0, beta * std::sqrt(std::max(0.0, 1 - ratio * ratio))) / norm;
        taps[static_cast<std::size_t>(index)] = ideal * window;
        sum += taps[static_cast<std::size_t>(index)];
    }
    if (!(sum > 0) || !std::isfinite(sum)) return false;
    for (auto& tap : taps) tap /= sum;
    return true;
}

void transform(std::vector<std::complex<double>>& data) {
    const auto size = data.size();
    for (std::size_t index = 1, reverse = 0; index < size; ++index) {
        std::size_t bit = size >> 1;
        for (; reverse & bit; bit >>= 1) reverse ^= bit;
        reverse ^= bit;
        if (index < reverse) std::swap(data[index], data[reverse]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const double angle = -2 * std::numbers::pi / static_cast<double>(length);
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < size; start += length) {
            std::complex<double> twiddle(1, 0);
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const auto even = data[start + offset];
                const auto odd = data[start + offset + length / 2] * twiddle;
                data[start + offset] = even + odd;
                data[start + offset + length / 2] = even - odd;
                twiddle *= root;
            }
        }
    }
}

double hann(int index, int size) {
    return .5 - .5 * std::cos(2 * std::numbers::pi * index / (size - 1));
}

bool validFft(int size) { return size >= 16 && (size & (size - 1)) == 0; }

std::complex<double> phaseFor(std::uint64_t increment, SignedIndex relativeSample) {
    const auto phase = increment * static_cast<std::uint64_t>(relativeSample);
    const double turns = std::ldexp(static_cast<double>(phase >> 32), -32) +
                         std::ldexp(static_cast<double>(static_cast<std::uint32_t>(phase)), -64);
    const double angle = turns * 2 * std::numbers::pi;
    return {std::cos(angle), std::sin(angle)};
}

bool outputGrid(std::uint64_t outputIndex, const ChannelDspPlan& plan,
                SignedIndex& integer, std::uint32_t& remainder) {
    if (!plan.interpolation || !plan.decimation ||
        outputIndex > static_cast<std::uint64_t>(std::numeric_limits<SignedIndex>::max())) return false;
    const auto quotient = outputIndex / plan.interpolation;
    const auto fraction = outputIndex % plan.interpolation;
    if (plan.decimation && quotient > static_cast<std::uint64_t>(std::numeric_limits<SignedIndex>::max()) / plan.decimation)
        return false;
    const auto whole = quotient * plan.decimation;
    const auto small = fraction * plan.decimation;
    integer = static_cast<SignedIndex>(whole + small / plan.interpolation);
    remainder = static_cast<std::uint32_t>(small % plan.interpolation);
    return true;
}

bool sourceCountFor(const FileMetadata& source, const Channel& channel,
                    const ChannelDspPlan& plan, std::uint64_t& outputCount) {
    const auto inputSamples = channel.sourceTime.end - channel.sourceTime.begin;
    if (channel.sourceTime.end > source.sampleCount || !plan.sourceRateHz || !plan.outputRateHz ||
        !multiplyDivideFloor(inputSamples, plan.outputRateHz, plan.sourceRateHz, outputCount)) return false;
    const auto remainder = (inputSamples % plan.sourceRateHz) * plan.outputRateHz % plan.sourceRateHz;
    if (remainder) {
        if (outputCount == std::numeric_limits<std::uint64_t>::max()) return false;
        ++outputCount;
    }
    return true;
}

} // namespace

bool designChannelLowpass(double sampleRateHz, double passEdgeHz, double stopEdgeHz,
                          double attenuationDb, std::vector<double>& coefficients) {
    return lowpass(sampleRateHz, passEdgeHz, stopEdgeHz, attenuationDb, coefficients);
}

bool makeChannelDspPlan(const FileMetadata& source, const Channel& channel,
                        ChannelDspPlan& plan, QString& error) {
    error.clear();
    plan = {};
    if(source.sampleFormat.structure==SampleStructure::Real){error="实数 ADC 尚未实现解析信号转换，不能创建复数 DDC 通道";return false;}
    if(!source.availability.fingerprint.empty()&&iqSourceFingerprint(QString::fromStdString(source.path)).toStdString()!=source.availability.fingerprint){error="通道源文件指纹已改变，请重新读入";return false;}
    if(source.availability.status==LoadStatus::Loading||channel.sourceTime.end>availableSamples(source)){error="通道来源未读入";return false;}
    const auto sourceRate = roundHz(source.sampleRateHz);
    const auto outputRate = roundHz(channel.outputSampleRateHz);
    if (!sourceRate || !outputRate || sourceRate > 4'000'000'000ULL || outputRate > 4'000'000'000ULL ||
        std::abs(source.sampleRateHz - sourceRate) > .01 ||
        std::abs(channel.outputSampleRateHz - outputRate) > .01) {
        error = QStringLiteral("当前采样率必须可精确表示到 0.01 Hz");
        return false;
    }
    if (channel.sourceTime.begin >= channel.sourceTime.end ||
        channel.sourceTime.end > source.sampleCount || !(channel.bandwidthHz > 0) ||
        !std::isfinite(channel.centerFrequencyHz) || !std::isfinite(channel.bandwidthHz)) {
        error = QStringLiteral("通道的源时段或频率参数无效");
        return false;
    }
    const double transition = channel.bandwidthHz * .15;
    const double passEdge = channel.bandwidthHz / 2;
    const double stopEdge = passEdge + transition;
    const auto sourceBand = fullRange(source).frequency;
    if (channel.centerFrequencyHz - stopEdge < sourceBand.lowerHz ||
        channel.centerFrequencyHz + stopEdge > sourceBand.upperHz) {
        error = QStringLiteral("通道滤波过渡带超出源文件有效频带");
        return false;
    }
    if (outputRate < stopEdge * 2 || outputRate > sourceRate * 4096.0) {
        error = QStringLiteral("输出采样率必须覆盖通带与抗混叠过渡带");
        return false;
    }

    const double targetDb = channel.filter == ChannelFilter::FastPreview ? 40 :
        channel.filter == ChannelFilter::HighRejection ? 80 : 60;
    std::uint64_t stageRate = sourceRate;
    std::vector<std::vector<double>> stages;
    const auto minimumIntermediateRate = static_cast<std::uint64_t>(std::ceil(stopEdge * 4));
    while (stageRate / 2 >= minimumIntermediateRate && stageRate / 2 >= 2) {
        const auto nextRate = stageRate / 2;
        const double aliasStop = static_cast<double>(nextRate) - stopEdge;
        std::vector<double> coefficients;
        const double stageAttenuation = targetDb + 6 + 20 * std::log10(static_cast<double>(stages.size() + 2));
        if (!lowpass(static_cast<double>(stageRate), passEdge, aliasStop,
                     stageAttenuation, coefficients)) break;
        stages.push_back(std::move(coefficients));
        stageRate = nextRate;
    }
    const auto common = gcd(outputRate, stageRate);
    const auto interpolation = outputRate / common;
    const auto decimation = stageRate / common;
    if (!interpolation || !decimation || interpolation > 4096 || decimation > 4096) {
        error = QStringLiteral("有理重采样比例超出 L、M ≤ 4096 的支持范围");
        return false;
    }
    const double expandedRate = static_cast<double>(stageRate) * interpolation;
    const double rationalStop = std::min(expandedRate / 2,
        passEdge + transition);
    std::vector<double> finalCoefficients;
    const double finalAttenuation = targetDb + 6 + 20 * std::log10(static_cast<double>(stages.size() + 1));
    if (!lowpass(expandedRate, passEdge, rationalStop, finalAttenuation, finalCoefficients)) {
        error = QStringLiteral("FIR 阶数超出当前资源预算；请增加过渡带或更换输出采样率");
        return false;
    }
    stages.push_back(std::move(finalCoefficients));
    plan.sourceRateHz = sourceRate;
    plan.outputRateHz = outputRate;
    plan.interpolation = static_cast<std::uint32_t>(interpolation);
    plan.decimation = static_cast<std::uint32_t>(decimation);
    plan.sourceDecimationStages = static_cast<int>(stages.size()) - 1;
    plan.transitionHz = transition;
    plan.cutoffHz = passEdge + transition / 2;
    plan.stopbandAttenuationDb = targetDb;
    plan.stageCoefficients = std::move(stages);
    for (const auto& coefficients : plan.stageCoefficients)
        plan.estimatedWorkingBytes += coefficients.size() * sizeof(double);
    error.clear();
    return true;
}

bool processChannelSamples(const FileMetadata& source, const Channel& channel,
                           const ChannelDspPlan& plan, TimeRange outputSamples,
                           ChannelSampleData& result,
                           const std::function<bool()>& cancelled,
                           const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    result = {};
    if (plan.stageCoefficients.empty() || outputSamples.begin >= outputSamples.end ||
        outputSamples.end - outputSamples.begin > 8'000'000) return false;
    std::uint64_t outputCount = 0;
    if (!sourceCountFor(source, channel, plan, outputCount) || outputSamples.end > outputCount) return false;

    const auto& finalTaps = plan.stageCoefficients.back();
    const auto finalMargin = static_cast<SignedIndex>(finalTaps.size() / (2 * plan.interpolation)) + 2;
    SignedIndex firstGrid = 0, lastGrid = 0;
    std::uint32_t phase = 0, finalPhase = 0;
    if (!outputGrid(outputSamples.begin, plan, firstGrid, phase) ||
        !outputGrid(outputSamples.end - 1, plan, lastGrid, finalPhase)) return false;
    SignedIndex first = firstGrid - finalMargin;
    SignedIndex last = lastGrid + finalMargin;
    for (std::size_t reverse = plan.stageCoefficients.size() - 1; reverse > 0; --reverse) {
        const auto half = static_cast<SignedIndex>(plan.stageCoefficients[reverse - 1].size() / 2);
        first = 2 * first - half;
        last = 2 * last + half;
    }
    const auto decimation = SignedIndex{1} << plan.sourceDecimationStages;
    first = floorDiv(first, decimation) * decimation;
    last = (ceilDiv(last + 1, decimation) * decimation) - 1;
    if (last < first || static_cast<std::uint64_t>(last - first + 1) > 32'000'000) return false;

    Int16IqFile iq;
    QString error;
    const auto sourcePath = QString::fromUtf8(source.path.data(), static_cast<qsizetype>(source.path.size()));
    if (!iq.open(sourcePath, error) || iq.sampleCount() != source.sampleCount) return false;
    const auto count = static_cast<std::size_t>(last - first + 1);
    std::vector<std::complex<float>> current(count);

    // A source tone at (channel center - source center) must rotate down to DC.
    const double delta = source.centerFrequencyHz - channel.centerFrequencyHz;
    double turns = std::fmod(delta, static_cast<double>(plan.sourceRateHz)) / plan.sourceRateHz;
    if (turns < 0) turns += 1;
    const double highScaled = std::ldexp(turns, 32);
    const auto high = static_cast<std::uint64_t>(std::floor(highScaled));
    const auto low = static_cast<std::uint64_t>(std::floor(std::ldexp(highScaled - high, 32)));
    const std::uint64_t increment = (high << 32) | (low & 0xffffffffULL);
    std::complex<double> oscillator = phaseFor(increment, first);
    const double stepTurns = std::ldexp(static_cast<double>(increment >> 32), -32) +
                             std::ldexp(static_cast<double>(static_cast<std::uint32_t>(increment)), -64);
    const double stepAngle = stepTurns * 2 * std::numbers::pi;
    const std::complex<double> step(std::cos(stepAngle), std::sin(stepAngle));
    for (std::size_t index = 0; index < count; ++index) {
        if ((index & 4095U) == 0 && cancelled && cancelled()) return false;
        const SignedIndex relative = first + static_cast<SignedIndex>(index);
        std::complex<double> input{};
        if (relative >= 0 && static_cast<std::uint64_t>(relative) < channel.sourceTime.end - channel.sourceTime.begin) {
            const auto sourceIndex = channel.sourceTime.begin + static_cast<std::uint64_t>(relative);
            if (sourceIndex < source.sampleCount) input = iq.sampleAt(sourceIndex);
        }
        const auto mixed = input * oscillator;
        current[index] = {static_cast<float>(mixed.real()), static_cast<float>(mixed.imag())};
        oscillator *= step;
        if ((index & 4095U) == 4095U) {
            const auto exactRelative = first + static_cast<SignedIndex>(index + 1);
            oscillator = phaseFor(increment, exactRelative);
        }
    }
    SignedIndex origin = first;
    const std::uint64_t decimationFactor = static_cast<std::uint64_t>(decimation);
    for (std::size_t stage = 0; stage < plan.sourceDecimationStages; ++stage) {
        if (cancelled && cancelled()) return false;
        const auto& taps = plan.stageCoefficients[stage];
        const auto half = static_cast<SignedIndex>(taps.size() / 2);
        const SignedIndex firstOutput = ceilDiv(origin + half, 2);
        const SignedIndex lastInput = origin + static_cast<SignedIndex>(current.size()) - 1;
        const SignedIndex lastOutput = floorDiv(lastInput - half, 2);
        if (lastOutput < firstOutput || static_cast<std::uint64_t>(lastOutput - firstOutput + 1) > 32'000'000) return false;
        std::vector<std::complex<float>> next(static_cast<std::size_t>(lastOutput - firstOutput + 1));
        for (SignedIndex output = firstOutput; output <= lastOutput; ++output) {
            if (cancelled && (output & 1023) == 0 && cancelled()) return false;
            std::complex<double> sum{};
            const auto center = 2 * output;
            for (std::size_t tap = 0; tap < taps.size(); ++tap) {
                const auto absolute = center + static_cast<SignedIndex>(tap) - half;
                const auto sampleIndex = static_cast<std::size_t>(absolute - origin);
                sum += std::complex<double>(current[sampleIndex].real(), current[sampleIndex].imag()) * taps[tap];
            }
            next[static_cast<std::size_t>(output - firstOutput)] = {
                static_cast<float>(sum.real()), static_cast<float>(sum.imag())};
        }
        origin = firstOutput;
        current = std::move(next);
        if (progress) progress(static_cast<std::uint64_t>(stage + 1), decimationFactor);
    }

    const auto& taps = plan.stageCoefficients.back();
    const SignedIndex half = static_cast<SignedIndex>(taps.size() / 2);
    result.outputSamples = outputSamples;
    result.samples.resize(static_cast<std::size_t>(outputSamples.end - outputSamples.begin));
    const double angularPerHz = 0; Q_UNUSED(angularPerHz);
    for (std::uint64_t outputIndex = outputSamples.begin; outputIndex < outputSamples.end; ++outputIndex) {
        if (((outputIndex - outputSamples.begin) & 255U) == 0 && cancelled && cancelled()) return false;
        SignedIndex center = 0;
        std::uint32_t fraction = 0;
        if (!outputGrid(outputIndex, plan, center, fraction)) return false;
        const SignedIndex numerator = center * static_cast<SignedIndex>(plan.interpolation) + fraction;
        const SignedIndex firstTap = std::max<SignedIndex>(0, ceilDiv(numerator - half, plan.interpolation));
        const SignedIndex lastTap = floorDiv(numerator + half, plan.interpolation);
        std::complex<double> sum{};
        for (SignedIndex inputIndex = firstTap; inputIndex <= lastTap; ++inputIndex) {
            const SignedIndex tapNumerator = half + numerator - inputIndex * plan.interpolation;
            if (tapNumerator < 0 || static_cast<std::uint64_t>(tapNumerator) >= taps.size()) continue;
            const SignedIndex local = inputIndex - origin;
            if (local < 0 || static_cast<std::uint64_t>(local) >= current.size()) continue;
            const auto sample = current[static_cast<std::size_t>(local)];
            sum += std::complex<double>(sample.real(), sample.imag()) *
                   (taps[static_cast<std::size_t>(tapNumerator)] * plan.interpolation);
        }
        result.samples[static_cast<std::size_t>(outputIndex - outputSamples.begin)] = {
            static_cast<float>(sum.real()), static_cast<float>(sum.imag())};
    }
    std::uint64_t sourceBeginOffset = 0, sourceEndOffset = 0;
    if (!multiplyDivideFloor(outputSamples.begin, plan.sourceRateHz, plan.outputRateHz, sourceBeginOffset) ||
        !multiplyDivideFloor(outputSamples.end, plan.sourceRateHz, plan.outputRateHz, sourceEndOffset)) return false;
    sourceBeginOffset = std::min(sourceBeginOffset, channel.sourceTime.end - channel.sourceTime.begin);
    sourceEndOffset = std::min(sourceEndOffset, channel.sourceTime.end - channel.sourceTime.begin);
    result.sourceSamples = {channel.sourceTime.begin + sourceBeginOffset,
        std::max(channel.sourceTime.begin + sourceBeginOffset, channel.sourceTime.begin + sourceEndOffset)};
    result.leftPadded = outputSamples.begin == 0;
    result.rightPadded = outputSamples.end == outputCount;
    return true;
}

ChannelSampleCache::ChannelSampleCache(std::size_t budgetBytes) : budgetBytes_(budgetBytes) {}

bool ChannelSampleCache::process(const FileMetadata& source, const Channel& channel,
                                 const ChannelDspPlan& plan, TimeRange outputSamples,
                                 ChannelSampleData& result,
                                 const std::function<bool()>& cancelled) {
    std::lock_guard lock(mutex_);
    result = {};
    if (outputSamples.begin >= outputSamples.end || outputSamples.end - outputSamples.begin > 8'000'000)
        return false;
    std::uint64_t outputCount = 0;
    if (!sourceCountFor(source, channel, plan, outputCount) || outputSamples.end > outputCount) return false;

    const auto sourcePath = QString::fromUtf8(source.path.data(), static_cast<qsizetype>(source.path.size()));
    const QFileInfo sourceInfo(sourcePath);
    QByteArray identity("signal-studio-channel-iq-v1\n");
    auto append = [&identity](const QString& value) { identity += value.toUtf8(); identity += '\n'; };
    append(sourcePath);
    append(QString::fromStdString(sampleFormatKey(source.sampleFormat)));
    append(QString::number(availableSamples(source)));
    append(QString::number(source.availability.generation));
    append(QString::number(source.sampleCount));
    append(QString::number(source.sampleRateHz, 'g', 17));
    append(QString::number(source.centerFrequencyHz, 'g', 17));
    append(QString::number(sourceInfo.size()));
    append(QString::number(sourceInfo.lastModified().toMSecsSinceEpoch()));
    append(QString::fromStdString(channel.id));
    append(QString::number(channel.configVersion));
    append(QString::number(channel.sourceTime.begin));
    append(QString::number(channel.sourceTime.end));
    append(QString::number(channel.centerFrequencyHz, 'g', 17));
    append(QString::number(channel.bandwidthHz, 'g', 17));
    append(QString::number(channel.outputSampleRateHz, 'g', 17));
    append(QString::number(static_cast<int>(channel.filter)));
    append(QString::number(plan.interpolation));
    append(QString::number(plan.decimation));
    append(QString::number(plan.stopbandAttenuationDb, 'g', 17));
    const auto fingerprint = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().toStdString();

    result.outputSamples = outputSamples;
    result.samples.resize(static_cast<std::size_t>(outputSamples.end - outputSamples.begin));
    std::uint64_t position = outputSamples.begin;
    while (position < outputSamples.end) {
        if (cancelled && cancelled()) { result = {}; return false; }
        const auto blockBegin = position / blockSamples * blockSamples;
        const auto blockEnd = blockBegin + std::min(blockSamples, outputCount - blockBegin);
        const auto blockKey = fingerprint + ':' + std::to_string(blockBegin);
        std::shared_ptr<const ChannelSampleData> block;
        const auto found = entries_.find(blockKey);
        if (found != entries_.end()) {
            block = found->second.data;
            lru_.splice(lru_.begin(), lru_, found->second.lru);
            found->second.lru = lru_.begin();
            ++hits_;
        } else {
            ++misses_;
            auto computed = std::make_shared<ChannelSampleData>();
            if (!processChannelSamples(source, channel, plan, {blockBegin, blockEnd}, *computed, cancelled)) {
                result = {};
                return false;
            }
            block = computed;
            const auto bytes = computed->samples.size() * sizeof(std::complex<float>);
            if (bytes <= budgetBytes_ && bytes_ <= std::numeric_limits<std::size_t>::max() - bytes) {
                lru_.push_front(blockKey);
                entries_.emplace(blockKey, Entry{block, lru_.begin(), bytes});
                bytes_ += bytes;
                while (bytes_ > budgetBytes_ && !lru_.empty()) {
                    const auto oldestKey = lru_.back();
                    const auto oldest = entries_.find(oldestKey);
                    if (oldest != entries_.end()) {
                        bytes_ -= oldest->second.bytes;
                        entries_.erase(oldest);
                    }
                    lru_.pop_back();
                }
            }
        }
        const auto copyBegin = std::max(position, blockBegin);
        const auto copyEnd = std::min(outputSamples.end, blockEnd);
        const auto count = static_cast<std::size_t>(copyEnd - copyBegin);
        const auto sourceOffset = static_cast<std::size_t>(copyBegin - blockBegin);
        const auto destinationOffset = static_cast<std::size_t>(copyBegin - outputSamples.begin);
        if (sourceOffset > block->samples.size() || count > block->samples.size() - sourceOffset) {
            result = {};
            return false;
        }
        std::copy_n(block->samples.begin() + static_cast<std::ptrdiff_t>(sourceOffset), count,
                    result.samples.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
        position = copyEnd;
    }

    const auto sourceLength = channel.sourceTime.end - channel.sourceTime.begin;
    std::uint64_t sourceBeginOffset = 0, sourceEndOffset = 0;
    if (!multiplyDivideFloor(outputSamples.begin, plan.sourceRateHz, plan.outputRateHz, sourceBeginOffset) ||
        !multiplyDivideFloor(outputSamples.end, plan.sourceRateHz, plan.outputRateHz, sourceEndOffset)) {
        result = {};
        return false;
    }
    sourceBeginOffset = std::min(sourceBeginOffset, sourceLength);
    sourceEndOffset = std::min(sourceEndOffset, sourceLength);
    result.sourceSamples = {channel.sourceTime.begin + sourceBeginOffset,
        std::max(channel.sourceTime.begin + sourceBeginOffset, channel.sourceTime.begin + sourceEndOffset)};
    result.leftPadded = outputSamples.begin == 0;
    result.rightPadded = outputSamples.end == outputCount;
    return true;
}

void ChannelSampleCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    lru_.clear();
    bytes_ = 0;
}

ChannelSampleCacheStats ChannelSampleCache::stats() const {
    std::lock_guard lock(mutex_);
    return {entries_.size(), bytes_, hits_, misses_};
}

bool channelWaveform(const ChannelSampleData& data, int points, NarrowbandWaveform mode,
                     std::vector<float>& output) {
    if (points < 1 || data.samples.empty()) return false;
    constexpr double adcCountScale = 32768.0;
    output.resize(static_cast<std::size_t>(points));
    for (int point = 0; point < points; ++point) {
        const auto first = static_cast<std::size_t>(data.samples.size()) * point / points;
        const auto last = static_cast<std::size_t>(data.samples.size()) * (point + 1) / points;
        const auto start = std::min(first, data.samples.size() - 1);
        const auto end = std::max(start + 1, std::min(last, data.samples.size()));
        if (mode == NarrowbandWaveform::Phase) {
            const auto sample = data.samples[start + (end - start) / 2];
            output[static_cast<std::size_t>(point)] = std::atan2(sample.imag(), sample.real());
        } else {
            double average = 0;
            double peakMagnitude = 0;
            for (auto index = start; index < end; ++index) {
                const double magnitude = std::abs(std::complex<double>(data.samples[index].real(), data.samples[index].imag()));
                average += magnitude * magnitude;
                peakMagnitude = std::max(peakMagnitude, magnitude);
            }
            average /= end - start;
            if (mode == NarrowbandWaveform::Magnitude)
                output[static_cast<std::size_t>(point)] = static_cast<float>(std::sqrt(average) * adcCountScale);
            else if (mode == NarrowbandWaveform::Envelope)
                output[static_cast<std::size_t>(point)] = static_cast<float>(peakMagnitude * adcCountScale);
            else
                output[static_cast<std::size_t>(point)] = static_cast<float>(std::atan2(data.samples[start + (end - start) / 2].imag(),
                                                                                       data.samples[start + (end - start) / 2].real()));
        }
    }
    return true;
}

bool channelWaveformIQ(const ChannelSampleData& data, int points,
                       std::vector<float>& i, std::vector<float>& q) {
    if (points < 1 || data.samples.empty()) return false;
    constexpr float adcCountScale = 32768.0f;
    i.resize(static_cast<std::size_t>(points)); q.resize(static_cast<std::size_t>(points));
    for (int point = 0; point < points; ++point) {
        const auto first = static_cast<std::size_t>(data.samples.size()) * point / points;
        const auto last = static_cast<std::size_t>(data.samples.size()) * (point + 1) / points;
        const auto start = std::min(first, data.samples.size() - 1);
        const auto end = std::max(start + 1, std::min(last, data.samples.size()));
        const auto sample = data.samples[start + (end - start) / 2];
        i[static_cast<std::size_t>(point)] = sample.real() * adcCountScale;
        q[static_cast<std::size_t>(point)] = sample.imag() * adcCountScale;
    }
    return true;
}

bool channelPsd(const ChannelSampleData& data, double sampleRateHz,
                FrequencyRange frequencies, int fftSize, int points,
                std::vector<float>& output, const std::function<bool()>& cancelled) {
    if (fftSize < 16 || !validFft(fftSize) || points < 1 || sampleRateHz <= 0 ||
        data.samples.size() < static_cast<std::size_t>(fftSize)) return false;
    const auto hop = static_cast<std::size_t>(std::max(1, fftSize / 2));
    const auto available = 1 + (data.samples.size() - fftSize) / hop;
    const auto windows = std::min<std::size_t>(64, available);
    std::vector<double> power(static_cast<std::size_t>(fftSize));
    std::vector<std::complex<double>> frame(static_cast<std::size_t>(fftSize));
    double windowPower = 0;
    for (int index = 0; index < fftSize; ++index) windowPower += hann(index, fftSize) * hann(index, fftSize);
    for (std::size_t window = 0; window < windows; ++window) {
        if (cancelled && cancelled()) return false;
        const auto offset = windows == 1 ? available / 2 * hop :
            (available - 1) * hop * window / std::max<std::size_t>(1, windows - 1);
        for (int index = 0; index < fftSize; ++index) {
            const auto sample = data.samples[offset + static_cast<std::size_t>(index)];
            frame[static_cast<std::size_t>(index)] = std::complex<double>(sample.real(), sample.imag()) * hann(index, fftSize);
        }
        transform(frame);
        for (int bin = 0; bin < fftSize; ++bin) {
            const auto shifted = static_cast<std::size_t>((bin + fftSize / 2) % fftSize);
            power[shifted] += std::norm(frame[static_cast<std::size_t>(bin)]) /
                              (sampleRateHz * windowPower * windows);
        }
    }
    output.resize(static_cast<std::size_t>(points));
    for (int x = 0; x < points; ++x) {
        const double u = points == 1 ? .5 : static_cast<double>(x) / (points - 1);
        const double frequency = frequencies.lowerHz + u * (frequencies.upperHz - frequencies.lowerHz);
        const int bin = std::clamp(static_cast<int>(std::lround((frequency + sampleRateHz / 2) * fftSize / sampleRateHz)), 0, fftSize - 1);
        output[static_cast<std::size_t>(x)] = static_cast<float>(10 * std::log10(std::max(power[static_cast<std::size_t>(bin)], 1e-20)));
    }
    return true;
}

bool channelSpectrogram(const ChannelSampleData& data, double sampleRateHz,
                        FrequencyRange frequencies, QSize pixels, int fftSize,
                        std::vector<float>& output, const std::function<bool()>& cancelled) {
    const int width = pixels.width(), height = pixels.height();
    if (width < 2 || height < 2 || fftSize < 16 || !validFft(fftSize) || sampleRateHz <= 0 ||
        data.samples.size() < static_cast<std::size_t>(fftSize)) return false;
    const auto hop = static_cast<std::size_t>(std::max(1, fftSize / 2));
    const auto available = 1 + (data.samples.size() - fftSize) / hop;
    const int columns = static_cast<int>(std::min<std::size_t>({static_cast<std::size_t>(width), available,
        std::max<std::size_t>(1, 8'000'000 / static_cast<std::size_t>(fftSize))}));
    if (columns < 1) return false;
    output.assign(static_cast<std::size_t>(width) * height, -140.0f);
    std::vector<std::complex<double>> frame(static_cast<std::size_t>(fftSize));
    std::vector<double> bins(static_cast<std::size_t>(fftSize));
    double windowPower = 0;
    for (int index = 0; index < fftSize; ++index) windowPower += hann(index, fftSize) * hann(index, fftSize);
    for (int column = 0; column < columns; ++column) {
        if (cancelled && cancelled()) return false;
        const auto start = columns == 1 ? (available - 1) * hop / 2 :
            (available - 1) * hop * static_cast<std::size_t>(column) / static_cast<std::size_t>(columns - 1);
        for (int index = 0; index < fftSize; ++index) {
            const auto sample = data.samples[start + static_cast<std::size_t>(index)];
            frame[static_cast<std::size_t>(index)] = std::complex<double>(sample.real(), sample.imag()) * hann(index, fftSize);
        }
        transform(frame);
        for (int bin = 0; bin < fftSize; ++bin)
            bins[static_cast<std::size_t>((bin + fftSize / 2) % fftSize)] =
                std::norm(frame[static_cast<std::size_t>(bin)]) / (sampleRateHz * windowPower);
        const int x = std::min(width - 1, static_cast<int>((column + .5) * width / columns));
        for (int y = 0; y < height; ++y) {
            const double firstFraction = static_cast<double>(y) / height;
            const double lastFraction = static_cast<double>(y + 1) / height;
            const double firstFrequency = frequencies.upperHz - firstFraction * (frequencies.upperHz - frequencies.lowerHz);
            const double lastFrequency = frequencies.upperHz - lastFraction * (frequencies.upperHz - frequencies.lowerHz);
            const auto firstBin = static_cast<int>(std::floor((lastFrequency + sampleRateHz / 2) * fftSize / sampleRateHz));
            const auto lastBin = static_cast<int>(std::ceil((firstFrequency + sampleRateHz / 2) * fftSize / sampleRateHz));
            const int lower = std::clamp(std::min(firstBin, lastBin), 0, fftSize - 1);
            const int upper = std::clamp(std::max(firstBin, lastBin), lower, fftSize - 1);
            double peak = 0;
            for (int bin = lower; bin <= upper; ++bin)
                peak = std::max(peak, bins[static_cast<std::size_t>(bin)]);
            output[static_cast<std::size_t>(y) * width + x] =
                static_cast<float>(10 * std::log10(std::max(peak, 1e-20)));
        }
    }
    if (columns < width) {
        const auto original = output;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int column = std::min(columns - 1, static_cast<int>((x + .5) * columns / width));
            const int sourceX = std::min(width - 1, static_cast<int>((column + .5) * width / columns));
            output[static_cast<std::size_t>(y) * width + x] = original[static_cast<std::size_t>(y) * width + sourceX];
        }
    }
    return true;
}

} // namespace signalstudio
