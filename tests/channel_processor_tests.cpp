#include "infrastructure/channel_processor.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace signalstudio;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFixture(const QString& path, int sampleCount, double sampleRateHz) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "Could not open deterministic IQ fixture");
    QByteArray bytes(sampleCount * 4, Qt::Uninitialized);
    auto* destination = reinterpret_cast<uchar*>(bytes.data());
    constexpr double pi = 3.14159265358979323846;
    for (int sample = 0; sample < sampleCount; ++sample) {
        const double phaseA = 2 * pi * 1.04e6 * sample / sampleRateHz;
        const double phaseB = 2 * pi * 1.25e6 * sample / sampleRateHz;
        const double i = .32 * std::cos(phaseA) + .32 * std::cos(phaseB);
        const double q = .32 * std::sin(phaseA) + .32 * std::sin(phaseB);
        qToLittleEndian<qint16>(static_cast<qint16>(std::lround(i * 32767)), destination + sample * 4);
        qToLittleEndian<qint16>(static_cast<qint16>(std::lround(q * 32767)), destination + sample * 4 + 2);
    }
    check(file.write(bytes) == bytes.size(), "Could not write complete deterministic IQ fixture");
}

float peakNear(const std::vector<float>& psd, double frequencyHz, double sampleRateHz) {
    const double exact = (frequencyHz + sampleRateHz / 2) * psd.size() / sampleRateHz;
    const auto center = static_cast<int>(std::lround(exact));
    const int begin = std::max(0, center - 5), end = std::min(static_cast<int>(psd.size()), center + 6);
    return *std::max_element(psd.begin() + begin, psd.begin() + end);
}

void testDspPlanAndRealIqChain() {
    QTemporaryDir directory;
    check(directory.isValid(), "Temporary IQ directory must be available");
    constexpr int sourceSamples = 65'536;
    constexpr double sourceRate = 10'000'000;
    const auto path = directory.filePath(QStringLiteral("channel_processor_fixture.iq"));
    writeFixture(path, sourceSamples, sourceRate);

    FileMetadata source;
    source.id = "source";
    source.name = "deterministic two-tone IQ";
    source.path = path.toStdString();
    source.sampleRateHz = sourceRate;
    source.centerFrequencyHz = 10'000'000;
    source.sampleCount = sourceSamples;
    source.demo = false;
    Channel channel;
    channel.id = "channel";
    channel.name = "one megahertz offset";
    channel.sourceMarkId = "mark";
    channel.centerFrequencyHz = 11'000'000;
    channel.bandwidthHz = 200'000;
    channel.sourceTime = {0, sourceSamples};
    channel.outputSampleRateHz = 1'000'000;
    channel.filter = ChannelFilter::Standard;

    ChannelDspPlan plan;
    QString error;
    check(makeChannelDspPlan(source, channel, plan, error), "Valid DDC/FIR/resampling plan must be accepted");
    check(plan.transitionHz == 30'000 && plan.stopbandAttenuationDb == 60 &&
          plan.sourceDecimationStages > 0 && plan.interpolation <= 4096 && plan.decimation <= 4096,
          "DSP plan must expose the requested transition, attenuation, and bounded rational ratio");

    ChannelSampleData full;
    const TimeRange outputRange{0, 6'554};
    check(processChannelSamples(source, channel, plan, outputRange, full),
          "Real channel processing must produce the requested DDC/FIR/resampled block");
    check(full.samples.size() == outputRange.end && full.outputSamples == outputRange &&
          full.sourceSamples == TimeRange{0, sourceSamples} && full.leftPadded && full.rightPadded,
          "Output sample count and source-time mapping must use the exact sample-rate ratio");

    ChannelSampleData first, second, third;
    check(processChannelSamples(source, channel, plan, {0, 2'000}, first) &&
          processChannelSamples(source, channel, plan, {2'000, 4'000}, second) &&
          processChannelSamples(source, channel, plan, {4'000, 6'554}, third),
          "Adjacent and random-window requests must be independently processable");
    for (std::size_t i = 0; i < first.samples.size(); ++i)
        check(std::abs(first.samples[i] - full.samples[i]) < 1e-6f,
              "First chunk must match one-shot DSP output");
    for (std::size_t i = 0; i < second.samples.size(); ++i)
        check(std::abs(second.samples[i] - full.samples[i + first.samples.size()]) < 1e-6f,
              "Middle chunk must match one-shot DSP output");
    for (std::size_t i = 0; i < third.samples.size(); ++i)
        check(std::abs(third.samples[i] - full.samples[i + first.samples.size() + second.samples.size()]) < 1e-6f,
              "Final chunk must match one-shot DSP output");

    auto cacheChannel = channel;
    cacheChannel.outputSampleRateHz = 3'000'000;
    ChannelDspPlan cachePlan;
    check(makeChannelDspPlan(source, cacheChannel, cachePlan, error),
          "The multi-block cache fixture must have a valid rational-resampling plan");
    const TimeRange cacheRange{12'000, 19'000};
    ChannelSampleData uncached, cached, cacheHit;
    check(processChannelSamples(source, cacheChannel, cachePlan, cacheRange, uncached),
          "The multi-block cache fixture must have a direct reference result");
    ChannelSampleCache cache;
    check(cache.process(source, cacheChannel, cachePlan, cacheRange, cached) &&
          cached.outputSamples == uncached.outputSamples && cached.sourceSamples == uncached.sourceSamples &&
          cached.leftPadded == uncached.leftPadded && cached.rightPadded == uncached.rightPadded &&
          cached.samples.size() == uncached.samples.size(),
          "The aligned 16,384-sample block cache must preserve mapping and boundaries");
    for (std::size_t i = 0; i < uncached.samples.size(); ++i)
        check(std::abs(cached.samples[i] - uncached.samples[i]) < 1e-6f,
              "Cached output across a block boundary must match direct DSP output");
    const auto firstStats = cache.stats();
    check(firstStats.misses == 2 && firstStats.hits == 0 && firstStats.entries == 2 && firstStats.bytes > 0,
          "A cross-block request must populate exactly two bounded IQ blocks");
    check(cache.process(source, cacheChannel, cachePlan, cacheRange, cacheHit),
          "Repeated cached output requests must complete");
    const auto hitStats = cache.stats();
    check(hitStats.hits == 2 && hitStats.misses == 2,
          "Repeated requests must reuse both processed IQ blocks");
    ++cacheChannel.configVersion;
    ChannelSampleData reconfigured;
    check(cache.process(source, cacheChannel, cachePlan, cacheRange, reconfigured) &&
          cache.stats().misses == 4,
          "A new channel configuration version must not reuse stale IQ blocks");
    cache.clear();
    check(cache.stats().entries == 0 && cache.stats().bytes == 0,
          "Closing the active project must be able to release every cached IQ block");

    ChannelSampleData steady;
    steady.outputSamples = {256, 6'144};
    steady.samples.assign(full.samples.begin() + 256, full.samples.begin() + 6'144);
    std::vector<float> psd;
    check(channelPsd(steady, channel.outputSampleRateHz, {-500'000, 500'000}, 4096, 4096, psd),
          "Welch PSD must accept a complete visible FFT window");
    const auto inBand = peakNear(psd, 40'000, channel.outputSampleRateHz);
    const auto rejected = peakNear(psd, 250'000, channel.outputSampleRateHz);
    check(inBand > -100 && inBand - rejected >= 45,
          "The configured FIR must preserve the in-band tone and reject the out-of-band tone");

    const std::array<std::pair<ChannelFilter, double>, 3> filterTargets{{
        {ChannelFilter::FastPreview, 40}, {ChannelFilter::Standard, 60}, {ChannelFilter::HighRejection, 80}}};
    for (const auto& [filter, targetDb] : filterTargets) {
        auto configured = channel; configured.filter = filter;
        ChannelDspPlan configuredPlan;
        check(makeChannelDspPlan(source, configured, configuredPlan, error), "All three FIR presets must produce a DSP plan");
        ChannelSampleData filtered;
        check(processChannelSamples(source, configured, configuredPlan, outputRange, filtered),
              "All three FIR presets must process the deterministic IQ fixture");
        ChannelSampleData passband;
        passband.outputSamples = steady.outputSamples;
        passband.samples.assign(filtered.samples.begin() + 256, filtered.samples.begin() + 6'144);
        std::vector<float> response;
        check(channelPsd(passband, configured.outputSampleRateHz, {-500'000, 500'000}, 4096, 4096, response),
              "The preset stopband response must be measurable with the reference PSD");
        const auto passPower = peakNear(response, 40'000, configured.outputSampleRateHz);
        const auto stopPower = peakNear(response, 250'000, configured.outputSampleRateHz);
        check(passPower > -100 && passPower - stopPower >= targetDb - 3,
              "Each Kaiser FIR preset must meet its configured stopband target within 3 dB");
    }

    std::vector<float> i, q, waveform;
    check(channelWaveformIQ(steady, 128, i, q) && i.size() == 128 && q.size() == 128,
          "The real channel must expose ADC-count-equivalent I/Q traces");
    check(*std::max_element(i.begin(), i.end()) > 1.0f || *std::min_element(i.begin(), i.end()) < -1.0f,
          "I/Q traces must not be normalized to full-scale floating-point values");
    check(channelWaveform(steady, 128, NarrowbandWaveform::Magnitude, waveform) && waveform.size() == 128 &&
          *std::max_element(waveform.begin(), waveform.end()) > 1.0f,
          "Magnitude mode must return linear ADC-count-equivalent RMS amplitude");
    ChannelSampleData pulse;
    pulse.samples.assign(32, {});
    pulse.samples[17] = {0.2f, 0.3f};
    std::vector<float> rms, envelope;
    check(channelWaveform(pulse, 4, NarrowbandWaveform::Magnitude, rms) &&
          channelWaveform(pulse, 4, NarrowbandWaveform::Envelope, envelope) &&
          envelope[2] > rms[2] * 2.8f &&
          std::abs(envelope[2] - std::sqrt(0.13f) * 32768.0f) < 1.0f,
          "Envelope mode must preserve the instantaneous IQ magnitude peak in each display bin");
    check(channelWaveform(steady, 128, NarrowbandWaveform::Phase, waveform) &&
          std::all_of(waveform.begin(), waveform.end(), [](float value) { return std::abs(value) <= 3.142f; }),
          "Phase mode must be expressed in radians");
    std::vector<float> spectrogram;
    const bool stftReady = channelSpectrogram(steady, channel.outputSampleRateHz, {-500'000, 500'000}, QSize(96, 48), 1024, spectrogram);
    const auto stftMaxIt = spectrogram.empty() ? spectrogram.end() : std::max_element(spectrogram.begin(), spectrogram.end());
    const float stftMaximum = stftMaxIt == spectrogram.end() ? -999 : *stftMaxIt;
    check(stftReady && spectrogram.size() == 96U * 48U && stftMaximum > -100,
          "The real channel must provide a bounded PSD-normalized STFT raster");

    ChannelSampleData canceled;
    check(!processChannelSamples(source, channel, plan, {0, 128}, canceled, [] { return true; }),
          "A canceled DSP request must not return a successful frame");
    check(!processChannelSamples(source, channel, plan, {0, 6'555}, canceled),
          "Requests beyond the channel sample count must be rejected");
    Channel invalid = channel;
    invalid.outputSampleRateHz = 200'000;
    check(!makeChannelDspPlan(source, invalid, plan, error) && !error.isEmpty(),
          "A rate below bandwidth plus both transition bands must be rejected with a reason");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    try {
        testDspPlanAndRealIqChain();
        std::cout << "PASS DDC/FIR/resampling, source mapping, chunk invariance, PSD and STFT\n";
    } catch (const std::exception& exception) {
        std::cerr << "FAIL " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
