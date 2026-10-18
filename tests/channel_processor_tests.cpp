#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "infrastructure/channel_processor.h"
#include "infrastructure/int16_iq_file.h"
#include "infrastructure/source_loader.h"
#include "infrastructure/wav_metadata.h"

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

void testActuallyStoppedPrefixAndDdcBoundaries() {
  QTemporaryDir directory;
  const auto path = directory.filePath("stopped-prefix.iq");
  constexpr SampleIndex total = 4 * 1024 * 1024;
  QByteArray bytes(static_cast<qsizetype>(total * 4), '\0');
  for (SampleIndex index = 0; index < total; ++index)
    qToLittleEndian<qint16>(10000, bytes.data() + index * 4);
  QFile file(path);
  check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
        "Could not write actual multi-block IQ source");
  file.close();
  SourceLoader loader(path, total);
  SourceLoadSnapshot snapshot;
  bool stopRequested = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  do {
    snapshot = loader.snapshot();
    if (!stopRequested && snapshot.loaded > 0 && snapshot.loaded < total) {
      loader.stop();
      stopRequested = true;
    }
    if (snapshot.finished) break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  check(stopRequested && snapshot.finished && snapshot.error.isEmpty() &&
            snapshot.loaded >= 1024 * 1024 && snapshot.loaded < total,
        "Stopping an actual 4 MiB block loader must publish a nonempty Partial "
        "prefix");

  FileMetadata source;
  source.id = "actually-stopped-source";
  source.path = path.toStdString();
  source.sampleRateHz = 1e6;
  source.centerFrequencyHz = 100e6;
  source.sampleCount = total;
  source.demo = false;
  source.availability = {snapshot.loaded,
                         LoadStatus::Partial,
                         0,
                         snapshot.fingerprint.toStdString(),
                         {}};
  Channel channel;
  channel.id = "partial-tail";
  channel.centerFrequencyHz = 100e6;
  channel.bandwidthHz = 100000;
  channel.outputSampleRateHz = 1e6;
  channel.sourceTime = {snapshot.loaded - 4096, snapshot.loaded};
  ChannelDspPlan plan;
  QString error, field;
  check(makeChannelDspPlan(source, channel, plan, error, &field) &&
            field.isEmpty(),
        "A valid channel at the tail of an actually stopped prefix must plan");
  ChannelSampleData tail;
  check(processChannelSamples(source, channel, plan, {3000, 4096}, tail) &&
            tail.samples.size() == 1096 && tail.rightPadded &&
            tail.samples[100].real() > .25 && tail.samples[100].real() < .35,
        "Real prefix-tail DDC must read actual IQ and preserve its right "
        "padding");
  ChannelSampleCache cache;
  check(cache.process(source, channel, plan, {3000, 4096}, tail),
        "The actual Partial tail must be cacheable");
  auto beyond = channel;
  ++beyond.sourceTime.end;
  ChannelDspPlan invalidPlan;
  check(
      !makeChannelDspPlan(source, beyond, invalidPlan, error, &field) &&
          field == "sourceTime" && !error.isEmpty(),
      "The first unread sample must fail planning with its time-field reason");
  check(!processChannelSamples(source, beyond, plan, {0, 1}, tail) &&
            !cache.process(source, beyond, plan, {0, 1}, tail),
        "Neither direct processing nor a populated cache may reuse a plan "
        "beyond the readable prefix");
  source.availability.status = LoadStatus::Loading;
  const bool loadingPlanReady =
      makeChannelDspPlan(source, channel, invalidPlan, error, &field);
  check(loadingPlanReady,
        qPrintable("Loading prefix planning failed: " + error));
  Int16IqFile prefixReader;
  const bool loadingReaderReady = prefixReader.open(source, error);
  check(loadingReaderReady,
        qPrintable("Loading prefix reader failed: " + error));
  check(prefixReader.sampleCount() == snapshot.loaded &&
            prefixReader.sampleAt(snapshot.loaded - 1).real() > .25 &&
            prefixReader.sampleAt(snapshot.loaded) == std::complex<double>{},
        "A Loading reader must expose the last available sample and refuse "
        "the first unread physical sample");
  std::vector<std::complex<float>> boundary;
  const auto readableSource = prefixReader.spectralSource();
  check(readableSource.read({snapshot.loaded - 1, snapshot.loaded}, boundary,
                            {}) &&
            boundary.size() == 1 && boundary.front().real() > .25 &&
            !readableSource.read({snapshot.loaded, snapshot.loaded + 1},
                                 boundary, {}),
        "Loading spectral reads must enforce the same exact prefix boundary");
  check(
      processChannelSamples(source, channel, invalidPlan, {3000, 4096}, tail) &&
          tail.samples.size() == 1096 && tail.rightPadded &&
          tail.samples[100].real() > .25 && tail.samples[100].real() < .35,
      "Loading status alone must not disable an already readable frozen "
      "prefix");
  source.availability.availableSamples = 0;
  check(!prefixReader.open(source, error) && error.contains("前缀") &&
            !processChannelSamples(source, channel, plan, {0, 1}, tail),
        "Permitting a readable Loading prefix must not permit an empty one");
  source.availability.availableSamples = snapshot.loaded;
  source.availability.status = LoadStatus::Failed;
  source.availability.error = "fixture storage read failure";
  check(!prefixReader.open(source, error) &&
            error.contains("fixture storage read failure"),
        "The sample reader must retain the concrete failed-source reason");
  check(!makeChannelDspPlan(source, channel, invalidPlan, error, &field) &&
            field == "source" &&
            error.contains("fixture storage read failure") &&
            !processChannelSamples(source, channel, plan, {0, 1}, tail),
        "Failed sources must retain their concrete I/O cause and stop "
        "processing");
}

void testWavContainerAndRateFieldDiagnostics() {
  QTemporaryDir directory;
  const auto path = directory.filePath("source.wav");
  constexpr int frames = 4096;
  QByteArray bytes(44 + frames * 4, '\0');
  const auto putText = [&](int offset, const char* text) {
    std::copy_n(text, 4, bytes.data() + offset);
  };
  putText(0, "RIFF");
  qToLittleEndian<quint32>(bytes.size() - 8, bytes.data() + 4);
  putText(8, "WAVE");
  putText(12, "fmt ");
  qToLittleEndian<quint32>(16, bytes.data() + 16);
  qToLittleEndian<quint16>(1, bytes.data() + 20);
  qToLittleEndian<quint16>(2, bytes.data() + 22);
  qToLittleEndian<quint32>(1'000'000, bytes.data() + 24);
  qToLittleEndian<quint32>(4'000'000, bytes.data() + 28);
  qToLittleEndian<quint16>(4, bytes.data() + 32);
  qToLittleEndian<quint16>(16, bytes.data() + 34);
  putText(36, "data");
  qToLittleEndian<quint32>(frames * 4, bytes.data() + 40);
  QFile file(path);
  check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
        "Could not write actual WAV diagnostic fixture");
  file.close();
  WavDescriptor descriptor;
  QString error, field;
  check(readWavDescriptor(path, descriptor, error), "WAV fixture must parse");
  FileMetadata source;
  source.id = "wav-source";
  source.path = path.toStdString();
  source.sampleRateHz = descriptor.sampleRateHz;
  source.centerFrequencyHz = 100e6;
  source.sampleCount = descriptor.sampleCount;
  source.sampleFormat = descriptor.format;
  source.demo = false;
  Channel channel;
  channel.centerFrequencyHz = source.centerFrequencyHz;
  channel.bandwidthHz = 100000;
  channel.outputSampleRateHz = 1e6;
  channel.sourceTime = {0, frames};
  ChannelDspPlan plan;
  check(makeChannelDspPlan(source, channel, plan, error, &field),
        "Container-derived metadata must produce a real WAV DSP plan");
  const auto correctSource = source;
  source.sampleRateHz = 2e6;
  check(!makeChannelDspPlan(source, channel, plan, error, &field) &&
            field == "source" && error.contains("WAV") &&
            error.contains("2000000") && error.contains("1000000") &&
            source.sampleRateHz == 2e6,
        "Legacy conflicting Fs must report both values without silently "
        "repairing coordinates");
  source = correctSource;
  source.sampleFormat.headerBytes = 0;
  check(!makeChannelDspPlan(source, channel, plan, error, &field) &&
            field == "source" && error.contains("格式"),
        "Conflicting WAV format/data boundaries must identify the physical "
        "source");
  source = correctSource;
  channel.outputSampleRateHz = 1'000'003;
  check(
      !makeChannelDspPlan(source, channel, plan, error, &field) &&
          field == "outputSampleRateHz" && error.contains("4096"),
      "Unsupported real rational-resampling ratios must identify output rate");
  channel.outputSampleRateHz = 1e6;
  channel.centerFrequencyHz = 100.45e6;
  check(!makeChannelDspPlan(source, channel, plan, error, &field) &&
            field == "frequency" && error.contains("过渡带"),
        "FIR protection-band overflow must identify frequency");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    try {
        testDspPlanAndRealIqChain();
        testActuallyStoppedPrefixAndDdcBoundaries();
        testWavContainerAndRateFieldDiagnostics();
        std::cout << "PASS DDC/FIR/resampling, source mapping, chunk invariance, PSD and STFT\n";
    } catch (const std::exception& exception) {
        std::cerr << "FAIL " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
