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
#include "infrastructure/source_loader.h"
#include <QTemporaryDir>
#include <QtEndian>
#include <thread>

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
    check(!insufficient->frames.empty() && insufficient->frames.front()->paddedSamples > 0,
        "Insufficient duration must zero-pad without changing N or borrowing samples");
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
void testPaddingStatisticsAndMethods() {
    SpectralSource source;source.sampleRateHz=1024;
    source.read=[](TimeRange r,auto& out,const auto&){check(r.begin>=100&&r.end<=103,"Padding escaped real samples");out.assign(r.end-r.begin,{.5f,0});return true;};
    SpectrogramSettings settings;settings.parameters.window=SpectralWindow::Rectangular;
    const auto shortData=analyzeSpectrogram(source,{100,103},{-512,512},32,240,settings,10);
    check(shortData->frames.size()==10&&shortData->sourceView==TimeRange{100,103}&&shortData->adjustedOverlap&&shortData->repeatedObservation,
        "Short STFT must preserve the original time axis and show ten padded cells");
    check(std::abs(shortData->effectiveHop-.3)<1e-12&&std::abs(shortData->effectiveOverlap-(1-.3/32))<1e-12,"Reported hop must match rational display centers including repeated observations");
    for(const auto& frame:shortData->frames)check(frame->linearPower.size()==32&&frame->paddedSamples>0&&frame->sourceSamples.end<=103,
        "Padded frames must retain N and their bounded real sample ranges");
    Session session;session.installSpectrogram("short",shortData);const auto* last=shortData->frameAtFraction(.99);
    session.pinCursor("short",102,0,true,last->id);check(session.selectedSpectralFrame("short")==last,"Repeated source centers need explicit display frame IDs");
    PsdSettings psd;psd.parameters.window=SpectralWindow::Rectangular;std::string error;
    const auto padded=analyzeSpectrum(source,{100,103},{-512,512},32,psd,error);
    check(padded&&error.empty()&&padded->paddedSamples>0,"Short PSD must zero-pad");
    double integrated=0;for(auto p:padded->linearPower)integrated+=p*32;
    check(std::abs(integrated-.25)<1e-6,"Zero extension must not dilute the observed power");
    SampleIndex highest=0;source.read=[&](TimeRange r,auto& out,const auto&){highest=std::max(highest,r.end);out.resize(r.end-r.begin);
        for(std::size_t n=0;n<out.size();++n){const auto sample=r.begin+n;const double amplitude=sample>=6400?.8:.1;out[n]=static_cast<std::complex<float>>(std::polar(amplitude,2*std::numbers::pi*64*sample/1024));}return true;};
    auto mean=analyzeSpectrum(source,{0,6503},{-512,512},32,psd,error);check(mean&&highest==6503,"Final PSD must cover all data including the last incomplete segment");
    psd.statistic=SpectrumStatistic::Maximum;auto maximum=analyzeSpectrum(source,{0,6503},{-512,512},32,psd,error);
    psd.statistic=SpectrumStatistic::Minimum;auto minimum=analyzeSpectrum(source,{0,6503},{-512,512},32,psd,error);
    for(int k=0;k<32;++k)check(maximum->linearPower[k]+1e-12>=mean->linearPower[k]&&mean->linearPower[k]+1e-12>=minimum->linearPower[k],"Power statistics must be evaluated in linear power");
    psd.statistic=SpectrumStatistic::Mean;
    for(auto method:{SpectralMethod::Periodogram,SpectralMethod::Bartlett,SpectralMethod::Welch,SpectralMethod::Multitaper,SpectralMethod::Burg}){
        psd.parameters.method=method;auto result=analyzeSpectrum(source,{0,256},{-512,512},32,psd,error);
        check(result&&error.empty()&&result->linearPower.size()==32,"Every enabled estimator must produce a real two-sided N-bin PSD");
        check(std::all_of(result->linearPower.begin(),result->linearPower.end(),[](float x){return std::isfinite(x)&&x>=0;}),"PSD must contain finite nonnegative powers");
    }
    const auto windows=dpssWindows(128,3.5,6);check(windows.size()==6,"DPSS must calculate all configured tapers");
    for(std::size_t i=0;i<windows.size();++i)for(std::size_t j=0;j<windows.size();++j){double dot=0;for(int n=0;n<128;++n)dot+=windows[i][n]*windows[j][n];check(std::abs(dot-(i==j?1.0:0.0))<1e-8,"DPSS must retain orthonormal tapers");}
}
void testSourceLoading() {
    QTemporaryDir directory;check(directory.isValid(),"Loader temp directory missing");const auto path=directory.filePath("load.iq");
    QFile file(path);check(file.open(QIODevice::WriteOnly),"Could not create loader fixture");QByteArray data(4*1'048'700,0);
    qToLittleEndian<qint16>(32767,reinterpret_cast<uchar*>(data.data()+4*1'048'699));file.write(data);file.close();
    SourceLoader loader(path,1'048'700);SourceLoadSnapshot state;
    do{state=loader.snapshot();std::this_thread::sleep_for(std::chrono::milliseconds(2));}while(!state.finished);
    check(state.error.isEmpty()&&state.loaded==1'048'700&&state.envelope.size()<=2048,"Loader must scan actual samples and bound the envelope point count");
    const auto peak=std::max_element(state.envelope.begin(),state.envelope.end(),[](auto a,auto b){return a.peak<b.peak;});
    check(peak->peakSample==1'048'699&&peak->peak==32767,"Navigator must preserve a single final-sample impulse");
    SourceLoader partial(path,123);do{state=partial.snapshot();std::this_thread::sleep_for(std::chrono::milliseconds(2));}while(!state.finished);
    check(state.loaded==123&&state.envelope.back().end==123,"Saved prefixes must not read the uncommitted source tail");
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
    SpectralSource reference;reference.sampleRateHz=1024;
    QJsonArray real,imag;
    std::vector<std::complex<float>> iq(79);
    for(int n=0;n<79;++n){iq[n]=static_cast<std::complex<float>>(.3*std::polar(1.0,2*std::numbers::pi*112*n/1024)+.07*std::polar(1.0,2*std::numbers::pi*233*n/1024)+std::complex<double>(.03*std::sin(n*19.7),.02*std::cos(n*7.3)));real.append(iq[n].real());imag.append(iq[n].imag());}
    reference.read=[&](TimeRange r,auto& out,const auto&){out.assign(iq.begin()+r.begin,iq.begin()+r.end);return true;};
    QJsonArray cases;
    for(auto method:{SpectralMethod::Periodogram,SpectralMethod::Bartlett,SpectralMethod::Welch,SpectralMethod::Multitaper,SpectralMethod::Burg})for(int window=0;window<7;++window){
        PsdSettings settings;settings.parameters.method=method;settings.parameters.window=static_cast<SpectralWindow>(window);settings.parameters.burgOrder=4;
        std::string error;auto frame=analyzeSpectrum(reference,{0,79},{-512,512},128,settings,error);check(frame!=nullptr,"Reference estimator missing");
        QJsonArray power,coefficients;for(auto x:frame->linearPower)power.append(x);for(auto x:spectralWindow(settings.parameters.window,79))coefficients.append(x);
        cases.append(QJsonObject{{"method",static_cast<int>(method)},{"window",window},{"linearPower",power},{"coefficients",coefficients}});
    }
    QJsonArray tapers;for(auto& taper:dpssWindows(79,3.5,6)){QJsonArray values;for(auto x:taper)values.append(x);tapers.append(values);}
    QJsonArray streamReal,streamImag,statistics;
    std::vector<std::complex<float>> stream(2257);for(int n=0;n<2257;++n){stream[n]=static_cast<std::complex<float>>(std::polar(n>=2240||n==1300?.8:.1,2*std::numbers::pi*64*n/1024));streamReal.append(stream[n].real());streamImag.append(stream[n].imag());}
    reference.read=[&](TimeRange r,auto& out,const auto&){out.assign(stream.begin()+r.begin,stream.begin()+r.end);return true;};
    for(int type=0;type<3;++type){PsdSettings settings;settings.statistic=static_cast<SpectrumStatistic>(type);std::string error;auto estimate=analyzeSpectrum(reference,{0,2257},{-512,512},32,settings,error);check(estimate!=nullptr,"Stream reference failed");QJsonArray power;for(auto x:estimate->linearPower)power.append(x);statistics.append(power);}
    report["streamStatistics"]=QJsonObject{{"real",streamReal},{"imag",streamImag},{"powers",statistics},{"points",32},{"sampleRateHz",1024}};
    report["estimators"]=QJsonObject{{"real",real},{"imag",imag},{"sampleRateHz",1024},{"points",128},{"cases",cases},{"dpss",tapers}};
    QFile file(path); check(file.open(QIODevice::WriteOnly), "Could not write SciPy comparison fixture");
    file.write(QJsonDocument(report).toJson());
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        testDirectReference(); testZoomResolutionAndBounds(); testMappingAndLinkedFrame(); testDensityAndInvalidPlans();
        testPaddingStatisticsAndMethods();testSourceLoading();
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--dump-reference") dumpReference(QString::fromLocal8Bit(argv[2]));
        std::cout << "PASS zoom CZT, actual resolution, visible bounds, cancellation, integer mapping and exact linked frames\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
