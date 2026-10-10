#include "infrastructure/spectral_analysis.h"
#include "infrastructure/channel_processor.h"
#include <array>
#include <limits>
#include <numbers>

namespace signalstudio {
namespace {
constexpr double tau = 2 * std::numbers::pi;
bool stopped(const SpectralCancel& cancel) { return cancel && cancel(); }
void fft(std::vector<std::complex<double>>& data, bool inverse = false) {
    const auto count = data.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        auto bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1) {
        const auto root = std::polar(1.0, (inverse ? tau : -tau) / length);
        for (std::size_t start = 0; start < count; start += length) {
            std::complex<double> phase(1, 0);
            for (std::size_t n = 0; n < length / 2; ++n) {
                const auto even = data[start + n], odd = phase * data[start + n + length / 2];
                data[start + n] = even + odd; data[start + n + length / 2] = even - odd; phase *= root;
            }
        }
    }
    if (inverse) for (auto& value : data) value /= static_cast<double>(count);
}
std::complex<double> phase(long double radians) {
    const auto angle = static_cast<double>(std::remainder(radians, static_cast<long double>(tau)));
    return {std::cos(angle), std::sin(angle)};
}
class ZoomTransform {
public:
    explicit ZoomTransform(const SpectralAnalysisPlan& plan) : plan_(plan) {
        std::size_t size = 1;
        while (size < static_cast<std::size_t>(plan.analysisSamples + plan.points - 1)) size <<= 1;
        kernel_.assign(size, {});
        const long double step = std::numbers::pi_v<long double> * plan.binHz / plan.analysisRateHz;
        for (int k = 0; k < plan.points; ++k) kernel_[k] = phase(step * k * k);
        for (int k = 1; k < plan.analysisSamples; ++k) kernel_[size - k] = phase(step * k * k);
        fft(kernel_);
        pre_.resize(plan.analysisSamples); post_.resize(plan.points);
        const double lower = plan.frequencies.lowerHz -
            (plan.decimationStages ? (plan.frequencies.lowerHz + plan.frequencies.upperHz) / 2 : 0);
        for (int n = 0; n < plan.analysisSamples; ++n) {
            const double window = .5 - .5 * std::cos(tau * n / (plan.analysisSamples - 1));
            pre_[n] = window * phase(-static_cast<long double>(tau) * lower * n / plan.analysisRateHz - step * n * n);
            windowPower_ += window * window;
        }
        for (int k = 0; k < plan.points; ++k) post_[k] = phase(-step * k * k);
    }
    std::vector<float> power(const std::vector<std::complex<double>>& samples) const {
        std::vector<std::complex<double>> work(kernel_.size());
        for (std::size_t n = 0; n < samples.size(); ++n) work[n] = samples[n] * pre_[n];
        fft(work);
        for (std::size_t n = 0; n < work.size(); ++n) work[n] *= kernel_[n];
        fft(work, true);
        std::vector<float> output(plan_.points);
        const double normalization = plan_.analysisRateHz * windowPower_;
        for (int k = 0; k < plan_.points; ++k) output[k] = static_cast<float>(std::norm(work[k] * post_[k]) / normalization);
        return output;
    }
private:
    SpectralAnalysisPlan plan_;
    std::vector<std::complex<double>> kernel_, pre_, post_;
    double windowPower_ = 0;
};
struct FirDecimator {
    std::vector<double> taps;
    std::vector<std::complex<double>> ring;
    std::uint64_t count = 0;
    std::size_t write = 0;
    explicit FirDecimator(const std::vector<double>& coefficients) : taps(coefficients), ring(taps.size()) {}
    bool push(std::complex<double> input, std::complex<double>& result) {
        ring[write] = input; write = (write + 1) % ring.size();
        const auto index = count++;
        const auto delay = taps.size() / 2;
        if (index < delay || ((index - delay) & 1)) return false;
        result = {};
        auto read = write;
        for (std::size_t n = 0; n < taps.size(); ++n) {
            read = read ? read - 1 : ring.size() - 1;
            result += ring[read] * taps[n];
        }
        return true;
    }
};
bool readFrame(const SpectralSource& source, const SpectralAnalysisPlan& plan, TimeRange bounds,
    SampleIndex first, std::vector<std::complex<double>>& samples, const SpectralCancel& cancel) {
    samples.clear(); samples.reserve(plan.analysisSamples);
    std::vector<double> taps;
    if (plan.decimationStages && !designChannelLowpass(1.0, .20, .25 - 1e-8, 60, taps)) return false;
    std::vector<FirDecimator> stages;
    std::uint64_t halo = 0, scale = 1;
    for (int stage = 0; stage < plan.decimationStages; ++stage) {
        stages.emplace_back(taps); halo += (taps.size() / 2) * scale; scale *= 2;
    }
    if (halo > std::numeric_limits<std::uint64_t>::max() - plan.decimation) return false;
    halo = (halo / plan.decimation + (halo % plan.decimation != 0)) * plan.decimation;
    if (halo > std::numeric_limits<std::uint64_t>::max() / 2 || plan.inputSamples > std::numeric_limits<std::uint64_t>::max() - 2 * halo) return false;
    const auto total = plan.inputSamples + 2 * halo;
    std::uint64_t outputIndex = 0;
    // A phase increment modulo 2^64 retains precise phase at large source indices.
    const double center = (plan.frequencies.lowerHz + plan.frequencies.upperHz) / 2;
    const auto increment = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(
        -static_cast<long double>(center) / source.sampleRateHz * std::ldexp(1.0L, 63)))) * 2;
    constexpr std::uint64_t block = 16'384;
    std::vector<std::complex<float>> raw;
    for (std::uint64_t offset = 0; offset < total; ) {
        if (stopped(cancel)) return false;
        const auto count = std::min(block, total - offset);
        const long double relative = static_cast<long double>(first - bounds.begin) + offset - halo;
        const auto validFirst = static_cast<std::uint64_t>(std::clamp(relative, 0.0L, static_cast<long double>(bounds.end - bounds.begin)));
        const auto validLast = static_cast<std::uint64_t>(std::clamp(relative + count, 0.0L, static_cast<long double>(bounds.end - bounds.begin)));
        raw.clear();
        if (validLast > validFirst && (!source.read({bounds.begin + validFirst, bounds.begin + validLast}, raw, cancel) || raw.size() != validLast - validFirst)) return false;
        for (std::uint64_t n = 0; n < count; ++n) {
            const auto position = relative + n;
            std::complex<double> value;
            if (position >= 0 && position < bounds.end - bounds.begin)
                value = std::complex<double>(raw[static_cast<std::size_t>(position - validFirst)]);
            if (plan.decimationStages) {
                const auto moduloIndex = first + offset + n - halo;
                const auto turns = increment * moduloIndex;
                const double angle = tau * (std::ldexp(static_cast<double>(turns >> 32), -32) +
                    std::ldexp(static_cast<double>(static_cast<std::uint32_t>(turns)), -64));
                value *= std::complex<double>(std::cos(angle), std::sin(angle));
            }
            bool available = true;
            for (auto& stage : stages) {
                std::complex<double> filtered;
                if (!stage.push(value, filtered)) { available = false; break; }
                value = filtered;
            }
            if (available) {
                if (outputIndex >= halo / plan.decimation && samples.size() < static_cast<std::size_t>(plan.analysisSamples)) samples.push_back(value);
                ++outputIndex;
            }
        }
        offset += count;
    }
    return samples.size() == static_cast<std::size_t>(plan.analysisSamples);
}
std::uint64_t scaled(std::uint64_t span, std::uint64_t numerator, std::uint64_t denominator) {
    return denominator ? (span / denominator) * numerator + (span % denominator) * numerator / denominator : 0;
}
}

SpectralAnalysisPlan makeSpectralAnalysisPlan(double rate, FrequencyRange frequency, int points) {
    SpectralAnalysisPlan result; result.frequencies = frequency; result.inputRateHz = rate; result.points = points;
    const double bandwidth = frequency.upperHz - frequency.lowerHz;
    if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(bandwidth) || bandwidth <= 0 ||
        points < 16 || points > 65536 || (points & (points - 1)) ||
        frequency.lowerHz < -rate / 2 - rate * 1e-12 || frequency.upperHz > rate / 2 + rate * 1e-12) {
        result.reason = "无效的频率范围或分析点数"; return result;
    }
    result.analysisRateHz = rate;
    while (result.analysisRateHz / 2 >= 2 * bandwidth && result.decimationStages < 40) {
        result.analysisRateHz /= 2; result.decimation *= 2; ++result.decimationStages;
    }
    result.binHz = bandwidth / points;
    const double sampleCount = std::ceil(result.analysisRateHz / result.binHz);
    if (!std::isfinite(sampleCount) || sampleCount > 262144 || sampleCount < 16) {
        result.reason = "频段过窄，超出单帧分析资源预算；请扩大频段"; return result;
    }
    result.analysisSamples = static_cast<int>(sampleCount);
    if (static_cast<std::uint64_t>(result.analysisSamples) > std::numeric_limits<std::uint64_t>::max() / result.decimation) {
        result.reason = "所需分析样本数超出整数映射范围"; return result;
    }
    result.inputSamples = static_cast<std::uint64_t>(result.analysisSamples) * result.decimation;
    result.requiredSeconds = static_cast<double>(result.inputSamples) / rate;
    double sum = 0, squares = 0;
    for (int n = 0; n < result.analysisSamples; ++n) {
        const double window = .5 - .5 * std::cos(tau * n / (result.analysisSamples - 1));
        sum += window; squares += window * window;
    }
    result.noiseBandwidthHz = result.analysisRateHz * squares / (sum * sum);
    result.valid = true; return result;
}
std::shared_ptr<SpectrogramData> analyzeSpectrogram(const SpectralSource& source, TimeRange view,
    FrequencyRange frequency, int points, int maximumFrames, const SpectralCancel& cancel) {
    auto output = std::make_shared<SpectrogramData>();
    output->plan = makeSpectralAnalysisPlan(source.sampleRateHz, frequency, points);
    output->providerView = view; output->sourceView = source.sourceRange ? source.sourceRange(view) : view;
    const auto& plan = output->plan;
    if (!plan.valid) { output->error = plan.reason; return output; }
    if (view.end <= view.begin || view.end - view.begin < plan.inputSamples) {
        output->error = "时间窗不足；所需分析时长 " + std::to_string(plan.requiredSeconds * 1000) + " ms";
        return output;
    }
    const auto hop = std::max<std::uint64_t>(1, plan.inputSamples / 2);
    const auto lastStart = view.end - view.begin - plan.inputSamples;
    const auto available = 1 + lastStart / hop;
    const auto budget = std::max(1, std::min(8'000'000 / plan.analysisSamples, 8'000'000 / points));
    const auto frames = std::min<std::uint64_t>(available, static_cast<std::uint64_t>(std::clamp(maximumFrames, 1, budget)));
    ZoomTransform transform(plan);
    std::vector<std::complex<double>> samples;
    for (std::uint64_t index = 0; index < frames; ++index) {
        if (stopped(cancel)) { output->frames.clear(); output->error = "已取消"; return output; }
        const auto grid = frames == 1 ? (available - 1) / 2 : scaled(available - 1, index, frames - 1);
        const auto first = view.begin + grid * hop;
        if (!readFrame(source, plan, view, first, samples, cancel)) { output->frames.clear(); output->error = "IQ 读取或频段滤波失败"; return output; }
        auto frame = std::make_shared<SpectralFrame>(); frame->id = first;
        frame->providerSamples = {first, first + plan.inputSamples};
        frame->sourceSamples = source.sourceRange ? source.sourceRange(frame->providerSamples) : frame->providerSamples;
        frame->sourceCenter = frame->sourceSamples.begin + (frame->sourceSamples.end - frame->sourceSamples.begin) / 2;
        frame->frequencies = frequency; frame->binHz = plan.binHz; frame->linearPower = transform.power(samples);
        output->frames.push_back(std::move(frame));
    }
    return output;
}
std::shared_ptr<SpectralFrame> averageSpectrum(const SpectrogramData& data) {
    if (data.frames.empty()) return {};
    auto output = std::make_shared<SpectralFrame>(*data.frames.front());
    output->sourceSamples = data.sourceView; output->providerSamples = data.providerView;
    std::fill(output->linearPower.begin(), output->linearPower.end(), 0);
    for (const auto& frame : data.frames) for (std::size_t bin = 0; bin < frame->linearPower.size(); ++bin)
        output->linearPower[bin] += frame->linearPower[bin] / static_cast<float>(data.frames.size());
    return output;
}
std::vector<float> spectrumDb(const SpectralFrame& frame) {
    std::vector<float> values(frame.linearPower.size());
    for (std::size_t bin = 0; bin < values.size(); ++bin) values[bin] = frame.dbAt(bin);
    return values;
}
std::vector<float> spectralRaster(const SpectrogramData& data, int width, int height, MainMode mode) {
    if (data.frames.empty() || width < 1 || height < 1) return {};
    std::vector<float> raster(static_cast<std::size_t>(width) * height);
    const bool waterfall = mode == MainMode::Waterfall;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const double t = (waterfall ? y + .5 : x + .5) / (waterfall ? height : width);
        const auto sample = data.sourceView.begin + static_cast<SampleIndex>(t * (data.sourceView.end - data.sourceView.begin));
        const auto* frame = data.frameAt(std::min(sample, data.sourceView.end - 1));
        const int axis = waterfall ? x : height - y - 1, extent = waterfall ? width : height;
        const auto first = static_cast<std::size_t>(axis) * frame->linearPower.size() / extent;
        const auto last = std::min(frame->linearPower.size(), std::max(first + 1,
            static_cast<std::size_t>(axis + 1) * frame->linearPower.size() / extent));
        const auto begin = std::min(first, frame->linearPower.size() - 1);
        float power = 0;
        for (auto bin = begin; bin < last; ++bin) power = std::max(power, frame->linearPower[bin]);
        raster[static_cast<std::size_t>(y) * width + x] = static_cast<float>(10 * std::log10(std::max(power, 1e-20f)));
    }
    return raster;
}
} // namespace signalstudio
