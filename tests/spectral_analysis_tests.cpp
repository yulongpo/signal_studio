#include "infrastructure/spectral_analysis.h"
#include "infrastructure/channel_processor.h"
#include "application/session.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace signalstudio;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
SpectralSource toneSource(double rate, SampleIndex origin = 0) {
    SpectralSource source; source.sampleRateHz = rate;
    source.read = [rate, origin](TimeRange range, std::vector<std::complex<float>>& values, const SpectralCancel& stop) {
        values.resize(static_cast<std::size_t>(range.end - range.begin));
        for (std::size_t n = 0; n < values.size(); ++n) {
            if ((n & 255) == 0 && stop && stop()) return false;
            const double index = static_cast<double>(range.begin - origin + n);
            const auto a = std::polar(.3, 2 * std::numbers::pi * 8040 * index / rate);
            const auto b = std::polar(.2, 2 * std::numbers::pi * 8080 * index / rate);
            const auto outside = std::polar(.1, 2 * std::numbers::pi * 26000 * index / rate);
            values[n] = std::complex<float>(a + b + outside);
        }
        return true;
    };
    return source;
}
void testDirectReference() {
    auto source = toneSource(65536);
    const auto data = analyzeSpectrogram(source, {100, 1200}, {-24000, 24000}, 256, 1);
    check(data->frames.size() == 1 && data->plan.decimationStages == 0, "Direct zoom frame must be available");
    const auto& frame = *data->frames.front();
    std::vector<std::complex<float>> samples;
    source.read(frame.providerSamples, samples, {});
    double windowPower = 0;
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const double window = .5 - .5 * std::cos(2 * std::numbers::pi * n / (samples.size() - 1));
        windowPower += window * window;
    }
    for (int bin = 0; bin < 256; ++bin) {
        std::complex<double> reference;
        for (std::size_t n = 0; n < samples.size(); ++n) {
            const double window = .5 - .5 * std::cos(2 * std::numbers::pi * n / (samples.size() - 1));
            reference += std::complex<double>(samples[n]) * window * std::polar(1.0, -2 * std::numbers::pi * frame.frequencyAt(bin) * n / source.sampleRateHz);
        }
        const double expected = std::norm(reference) / (source.sampleRateHz * windowPower);
        check(std::abs(frame.linearPower[bin] - expected) <= 1e-9 + expected * 1e-5, "CZT power must match direct DFT at every visible frequency bin");
    }
}
void testZoomResolutionAndBounds() {
    auto source = toneSource(65536);
    const auto broad = analyzeSpectrogram(source, {0, 65536}, {-32768, 32768}, 256, 3);
    const auto zoom = analyzeSpectrogram(source, {0, 65536}, {7800, 8200}, 256, 3);
    check(!broad->frames.empty() && !zoom->frames.empty(), "Broad and zoomed spectra must calculate");
    check(zoom->plan.inputSamples > broad->plan.inputSamples && zoom->plan.binHz < broad->plan.binHz,
        "Frequency zoom must increase real acquisition duration and retain N bins");
    for (const auto& frame : zoom->frames) {
        check(frame->linearPower.size() == 256 && frame->providerSamples.end <= 65536, "Every zoom frame must contain N bins inside the visible time window");
        const auto a = frame->binAt(8040), b = frame->binAt(8080), middle = frame->binAt(8060);
        check(frame->dbAt(a) > frame->dbAt(middle) + 15 && frame->dbAt(b) > frame->dbAt(middle) + 15,
            "Longer zoom frames must resolve the two nearby tones");
    }
    const auto insufficient = analyzeSpectrogram(source, {0, 1000}, {7800, 8200}, 256, 3);
    check(insufficient->frames.empty() && insufficient->error.find("时间窗不足") != std::string::npos,
        "Insufficient duration must be explicit without changing N or borrowing samples");
    check(insufficient->providerView == TimeRange{0, 1000} && insufficient->plan.points == 256, "Insufficient request must retain the user's view and N");
    auto bounded = source;
    bounded.read = [source](TimeRange range, auto& samples, const auto& cancel) {
        if (range.begin < 1000 || range.end > 65000) throw std::runtime_error("Analysis read escaped the visible time window");
        return source.read(range, samples, cancel);
    };
    check(!analyzeSpectrogram(bounded, {1000, 65000}, {7800, 8200}, 256, 2)->frames.empty(), "FIR neighborhoods must respect visible bounds");
    int checks = 0;
    const auto cancelled = analyzeSpectrogram(source, {0, 65536}, {7800, 8200}, 256, 10, [&checks] { return ++checks > 10; });
    check(cancelled->frames.empty(), "Cancelled analyses must not publish partial frames as complete");
}
void testMappingAndLinkedFrame() {
    constexpr SampleIndex origin = (SampleIndex{1} << 53) + 57;
    auto source = toneSource(65536, origin);
    const auto data = analyzeSpectrogram(source, {origin, origin + 65536}, {7800, 8200}, 256, 4);
    check(!data->frames.empty() && data->sourceView.begin == origin, "Large integer origins must stay exact");
    Session session;
    session.installSpectrogram("file", data);
    session.pinCursor("file", data->frames.back()->sourceCenter, 8040, true);
    check(session.selectedSpectralFrame("file") == data->frames.back().get(), "Pinned PSD must reuse the exact actual STFT frame");
    check(session.linkedCursor("file").framePsd && !session.linkedCursor("other").pinned, "Cursor contexts must stay isolated");
    const auto db = spectrumDb(*session.selectedSpectralFrame("file"));
    for (std::size_t n = 0; n < db.size(); ++n) check(db[n] == data->frames.back()->dbAt(n), "Frame PSD must be bin-for-bin identical");
    const auto timeRaster = spectralRaster(*data, 12, 10, MainMode::TimeFrequency);
    const auto waterfall = spectralRaster(*data, 10, 12, MainMode::Waterfall);
    for (int t = 0; t < 12; ++t) for (int f = 0; f < 10; ++f)
        check(timeRaster[(9 - f) * 12 + t] == waterfall[t * 10 + f], "Waterfall and STFT must map the same frame/bin cells");
    session.clearCursor("file"); check(!session.linkedCursor("file").framePsd, "Clearing cursor must restore averaged PSD");
    session.pinCursor("file", origin, 8000, true); session.newProject();
    check(!session.linkedCursor("file").pinned && !session.spectrogram("file"), "Closing a project must release cursor and analysis state");
}
void testDensityAndInvalidPlans() {
    constexpr double rate = 65536;
    SpectralSource source; source.sampleRateHz = rate;
    source.read = [](TimeRange range, auto& output, const auto&) {
        output.resize(range.end - range.begin);
        for (std::size_t n = 0; n < output.size(); ++n) {
            auto bits = static_cast<std::uint32_t>(range.begin + n + 1);
            auto random = [&] { bits ^= bits << 13; bits ^= bits >> 17; bits ^= bits << 5; return (bits / 4294967296.0 - .5) * .6; };
            const auto real = random(), imag = random(); output[n] = {static_cast<float>(real), static_cast<float>(imag)};
        }
        return true;
    };
    const auto noise = analyzeSpectrogram(source, {0, 131072}, {-rate / 2, rate / 2}, 1024, 80);
    const auto average = averageSpectrum(*noise);
    double density = 0;
    for (const auto power : average->linearPower) density += power * average->binHz;
    check(std::abs(density - .06) < .003, "Integrated complex noise PSD must retain dBFS/Hz power normalization");
    source.read = [](TimeRange range, auto& output, const auto&) {
        output.resize(range.end - range.begin);
        for (std::size_t n = 0; n < output.size(); ++n)
            output[n] = static_cast<std::complex<float>>(.3 * std::polar(1.0, -2 * std::numbers::pi * 8050 * (range.begin + n) / rate));
        return true;
    };
    const auto negative = analyzeSpectrogram(source, {0, 65536}, {-8200, -7800}, 256, 2);
    check(!negative->frames.empty(), "Negative-center zoom must be valid");
    const auto& frame = *negative->frames.front();
    const auto peak = std::max_element(frame.linearPower.begin(), frame.linearPower.end()) - frame.linearPower.begin();
    check(std::abs(frame.frequencyAt(peak) + 8050) <= frame.binHz, "Negative NCO/zoom peak must stay within one bin");
    check(!makeSpectralAnalysisPlan(1e30, {0, 1e-30}, 65536).valid, "Extreme analysis duration must reject resource/integer overflow");
    check(!makeSpectralAnalysisPlan(rate, {-40000, 100}, 256).valid, "Frequency ranges outside Nyquist must be rejected");
}
void dumpReference(const QString& path) {
    auto source = toneSource(65536);
    const auto data = analyzeSpectrogram(source, {0, 65536}, {7800, 8200}, 256, 1);
    const auto& frame = *data->frames.front(); const auto& plan = data->plan;
    std::vector<double> taps; designChannelLowpass(1, .20, .25 - 1e-8, 60, taps);
    QJsonArray coefficients, powers;
    for (const auto tap : taps) coefficients.append(tap);
    for (const auto power : frame.linearPower) powers.append(power);
    QJsonObject report{{"sampleRateHz", source.sampleRateHz}, {"frequencyLowerHz", plan.frequencies.lowerHz},
        {"frequencyUpperHz", plan.frequencies.upperHz}, {"points", plan.points}, {"analysisSamples", plan.analysisSamples},
        {"stages", plan.decimationStages}, {"decimation", static_cast<double>(plan.decimation)},
        {"first", static_cast<double>(frame.providerSamples.begin)}, {"last", static_cast<double>(frame.providerSamples.end)},
        {"viewBegin", 0}, {"viewEnd", 65536}, {"taps", coefficients}, {"linearPower", powers}};
    QFile file(path); check(file.open(QIODevice::WriteOnly), "Could not write SciPy comparison fixture");
    file.write(QJsonDocument(report).toJson());
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        testDirectReference(); testZoomResolutionAndBounds(); testMappingAndLinkedFrame(); testDensityAndInvalidPlans();
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--dump-reference") dumpReference(QString::fromLocal8Bit(argv[2]));
        std::cout << "PASS zoom CZT, actual resolution, visible bounds, cancellation, integer mapping and exact linked frames\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
