#include "infrastructure/spectral_analysis.h"
#include "infrastructure/channel_processor.h"
#include <array>
#include <limits>
#include <numbers>
#include <sstream>
#include <iomanip>

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
            pre_[n] = phase(-static_cast<long double>(tau) * lower * n / plan.analysisRateHz - step * n * n);
        }
        for (int k = 0; k < plan.points; ++k) post_[k] = phase(-step * k * k);
    }
    std::vector<float> power(const std::vector<std::complex<double>>& samples,
        const std::vector<double>& window, int validFirst, int validLast) const {
        std::vector<std::complex<double>> work(kernel_.size());
        double energy = 0;
        for (int n = validFirst; n < validLast; ++n) { work[n] = samples[n] * pre_[n] * window[n]; energy += window[n] * window[n]; }
        fft(work);
        for (std::size_t n = 0; n < work.size(); ++n) work[n] *= kernel_[n];
        fft(work, true);
        std::vector<float> output(plan_.points);
        const double normalization = plan_.analysisRateHz * std::max(energy, 1e-30);
        for (int k = 0; k < plan_.points; ++k) output[k] = static_cast<float>(std::norm(work[k] * post_[k]) / normalization);
        return output;
    }
private:
    SpectralAnalysisPlan plan_;
    std::vector<std::complex<double>> kernel_, pre_, post_;
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
    SampleIndex first, std::vector<std::complex<double>>& samples, const SpectralCancel& cancel,
    std::uint64_t leadingPad = 0) {
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
        const long double relative = static_cast<long double>(first - bounds.begin) + offset - halo - leadingPad;
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
                const auto moduloIndex = first + offset + n - halo - leadingPad;
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
    return makeSpectralAnalysisPlan(rate, frequency, points, SpectralParameters{});
}
SpectralAnalysisPlan makeSpectralAnalysisPlan(double rate, FrequencyRange frequency, int points, const SpectralParameters& parameters) {
    SpectralAnalysisPlan result; result.frequencies=frequency; result.inputRateHz=rate; result.points=points; result.parameters=parameters;
    const double bandwidth=frequency.upperHz-frequency.lowerHz;
    if(!validSpectralParameters(parameters)||!std::isfinite(rate)||rate<=0||!std::isfinite(bandwidth)||bandwidth<=0||
       points<32||points>65536||(points&(points-1))||frequency.lowerHz < -rate/2-rate*1e-12||frequency.upperHz > rate/2+rate*1e-12) {
        result.reason="无效的频率范围、分析参数或点数（支持 32–65536）";return result;
    }
    result.analysisRateHz=rate;
    while(result.analysisRateHz/2>=2*bandwidth&&result.decimationStages<40) {result.analysisRateHz/=2;result.decimation*=2;++result.decimationStages;}
    result.binHz=bandwidth/points;
    const double count=std::ceil(parameters.segmentMilliseconds>0 ? result.analysisRateHz*parameters.segmentMilliseconds/1000 : result.analysisRateHz/result.binHz);
    if(!std::isfinite(count)||count>262144||count<1) {result.reason="单帧超过 64 MiB 工作预算；请缩短分段时长或扩大频段";return result;}
    result.analysisSamples=static_cast<int>(count);
    std::size_t transformSize=1;while(transformSize<static_cast<std::size_t>(result.analysisSamples+points-1))transformSize*=2;
    const std::size_t working=transformSize*48+static_cast<std::size_t>(result.analysisSamples)*32+
        (parameters.method==SpectralMethod::Multitaper?static_cast<std::size_t>(result.analysisSamples)*parameters.tapers*8:0);
    if(working>64U*1024U*1024U){result.reason="分析工作缓冲超过 64 MiB；请减少窗数或缩短分段";return result;}
    if(static_cast<std::uint64_t>(result.analysisSamples)>std::numeric_limits<std::uint64_t>::max()/result.decimation) {result.reason="所需样本数溢出";return result;}
    result.inputSamples=static_cast<std::uint64_t>(result.analysisSamples)*result.decimation;
    result.requiredSeconds=static_cast<double>(result.inputSamples)/rate;
    const auto window=spectralWindow(parameters.window,result.analysisSamples,parameters.kaiserBeta);
    double sum=0,squares=0;for(double x:window){sum+=x;squares+=x*x;}
    result.noiseBandwidthHz=sum?result.analysisRateHz*squares/(sum*sum):result.analysisRateHz;
    result.valid=true;return result;
}
namespace {
std::vector<float> burgPower(const std::vector<std::complex<double>>& samples, int begin, int end,
    const SpectralAnalysisPlan& plan, std::string& error) {
    const int count=end-begin, order=plan.parameters.burgOrder;
    if(count<=order){error="Burg 阶数必须小于真实有效样本数；补零不用于模型拟合";return {};}
    std::vector<std::complex<double>> forward(samples.begin()+begin,samples.begin()+end),backward=forward,a(1,1);
    double variance=0;for(auto x:forward)variance+=std::norm(x);variance/=count;
    if(variance<1e-30)return std::vector<float>(plan.points,0);
    for(int m=1;m<=order;++m) {
        std::complex<double> cross{};double denominator=0;
        for(int n=m;n<count;++n){cross+=forward[n]*std::conj(backward[n-1]);denominator+=std::norm(forward[n])+std::norm(backward[n-1]);}
        if(denominator<1e-30)break;
        auto reflection=-2.0*cross/denominator;
        if(std::norm(reflection)>=1)reflection*=std::sqrt((1-1e-12)/std::norm(reflection));
        auto next=a;next.resize(m+1);for(int k=1;k<m;++k)next[k]=a[k]+reflection*std::conj(a[m-k]);next[m]=reflection;a=std::move(next);
        for(int n=count-1;n>=m;--n){const auto old=forward[n];forward[n]=old+reflection*backward[n-1];backward[n-1]=backward[n-1]+std::conj(reflection)*old;}
        // Shift backward errors into the next order's alignment.
        for(int n=count-1;n>=m;--n)backward[n]=backward[n-1];
        variance*=std::max(0.0,1-std::norm(reflection));
    }
    std::vector<float> power(plan.points);
    const double center=plan.decimationStages?(plan.frequencies.lowerHz+plan.frequencies.upperHz)/2:0;
    for(int k=0;k<plan.points;++k){std::complex<double> response{};const auto root=std::polar(1.0,-tau*(plan.frequencies.lowerHz+k*plan.binHz-center)/plan.analysisRateHz);std::complex<double> z=1;for(auto coefficient:a){response+=coefficient*z;z*=root;}power[k]=static_cast<float>(variance/(plan.analysisRateHz*std::max(1e-30,std::norm(response))));}
    return power;
}
class FrameEstimator {
public:
    explicit FrameEstimator(const SpectralAnalysisPlan& plan): plan_(plan),transform_(plan),window_(spectralWindow(plan.parameters.window,plan.analysisSamples,plan.parameters.kaiserBeta)) {}
    std::shared_ptr<SpectralFrame> compute(const SpectralSource& source,TimeRange bounds,SampleIndex first,
        std::uint64_t leading,std::uint64_t id,long double fraction,const SpectralCancel& cancel,std::string& error) {
        std::vector<std::complex<double>> values;
        if(!readFrame(source,plan_,bounds,first,values,cancel,leading)){error=stopped(cancel)?"已取消":"IQ 读取或滤波失败";return {};}
        const auto available=bounds.end-first;
        const int begin=static_cast<int>(std::min<std::uint64_t>(plan_.analysisSamples,(leading+plan_.decimation-1)/plan_.decimation));
        const int end=static_cast<int>(std::min<std::uint64_t>(plan_.analysisSamples,(available+leading+plan_.decimation-1)/plan_.decimation));
        if(begin>=end){error="没有有效分析样本";return {};}
        auto frame=std::make_shared<SpectralFrame>(); frame->id=id;frame->displayFraction=fraction;
        frame->providerSamples={first,first+std::min(available,plan_.inputSamples-leading)};
        frame->sourceSamples=source.sourceRange?source.sourceRange(frame->providerSamples):frame->providerSamples;
        frame->sourceCenter=frame->sourceSamples.begin+(frame->sourceSamples.end-frame->sourceSamples.begin)/2;
        frame->validSamples=end-begin;frame->paddedSamples=plan_.analysisSamples-frame->validSamples;
        frame->frequencies=plan_.frequencies;frame->binHz=plan_.binHz;frame->observedSeconds=static_cast<double>(frame->validSamples)/plan_.analysisRateHz;
        if(plan_.parameters.removeMean){std::complex<double> mean{};for(int n=begin;n<end;++n)mean+=values[n];mean/=end-begin;for(int n=begin;n<end;++n)values[n]-=mean;}
        if(plan_.parameters.method==SpectralMethod::Burg){frame->linearPower=burgPower(values,begin,end,plan_,error);frame->windowEnergy=frame->validSamples;}
        else if(plan_.parameters.method==SpectralMethod::Multitaper) {
            const int length=end-begin;
            if(plan_.parameters.timeBandwidth>=length*.5||plan_.parameters.tapers>length){error="DPSS NW/K 超出真实有效样本长度";return {};}
            if(taperLength_!=length){tapers_=dpssWindows(length,plan_.parameters.timeBandwidth,plan_.parameters.tapers,cancel);taperLength_=length;}
            if(tapers_.empty()){error="DPSS 计算未收敛";return {};}
            frame->linearPower.assign(plan_.points,0);
            for(const auto& taper:tapers_){if(stopped(cancel)){error="已取消";return {};}
                std::vector<double> w(plan_.analysisSamples);std::copy(taper.begin(),taper.end(),w.begin()+begin);
                const auto power=transform_.power(values,w,begin,end);
                for(int k=0;k<plan_.points;++k)frame->linearPower[k]+=power[k]/static_cast<float>(tapers_.size());
            }
            frame->windowEnergy=static_cast<double>(length)/plan_.analysisSamples;
            frame->noiseBandwidthHz=2*plan_.parameters.timeBandwidth*plan_.analysisRateHz/length;
        } else {
            // Windows are defined on the observed data before zero extension.
            auto observed=spectralWindow(plan_.parameters.window,end-begin,plan_.parameters.kaiserBeta);
            if(std::all_of(observed.begin(),observed.end(),[](double x){return std::abs(x)<1e-15;}))std::fill(observed.begin(),observed.end(),1);
            std::vector<double> w(plan_.analysisSamples);std::copy(observed.begin(),observed.end(),w.begin()+begin);
            frame->linearPower=transform_.power(values,w,begin,end);
            double fullEnergy=0;for(double x:window_)fullEnergy+=x*x;
            double coherentGain=0;for(double x:observed){frame->windowEnergy+=x*x;coherentGain+=x;}
            frame->noiseBandwidthHz=coherentGain?plan_.analysisRateHz*frame->windowEnergy/(coherentGain*coherentGain):plan_.analysisRateHz;
            frame->windowEnergy/=std::max(1e-30,fullEnergy);
        }
        if(frame->linearPower.empty())return {};
        if(source.real)for(std::size_t k=0;k<frame->linearPower.size();++k){
            const double f=frame->frequencyAt(k);
            if(f>0 && f<source.sampleRateHz/2)frame->linearPower[k]*=2;
        }
        return frame;
    }
private:
    SpectralAnalysisPlan plan_;ZoomTransform transform_;std::vector<double> window_;
    int taperLength_=0;std::vector<std::vector<double>> tapers_;
};
}
std::shared_ptr<SpectrogramData> analyzeSpectrogram(const SpectralSource& source,TimeRange view,
    FrequencyRange frequency,int points,int maximumFrames,const SpectralCancel& cancel) {
    return analyzeSpectrogram(source,view,frequency,points,maximumFrames,SpectrogramSettings{},1,cancel);
}
std::shared_ptr<SpectrogramData> analyzeSpectrogram(const SpectralSource& source,TimeRange view,
    FrequencyRange frequency,int points,int maximumFrames,const SpectrogramSettings& settings,int minimumFrames,const SpectralCancel& cancel) {
    auto output=std::make_shared<SpectrogramData>();output->plan=makeSpectralAnalysisPlan(source.sampleRateHz,frequency,points,settings.parameters);
    output->providerView=view;output->sourceView=source.sourceRange?source.sourceRange(view):view;
    const auto& plan=output->plan;
    if(!plan.valid){output->error=plan.reason;return output;}
    if(view.end<=view.begin){output->error="没有有效分析数据";return output;}
    const auto span=view.end-view.begin;
    auto hop=std::max<std::uint64_t>(1,static_cast<std::uint64_t>(plan.inputSamples*(1-settings.parameters.overlap)));
    const auto complete=span>=plan.inputSamples?1+(span-plan.inputSamples)/hop:0;
    const auto budget=std::max(10,std::min(8'000'000/plan.analysisSamples,8'000'000/points));
    const auto count=std::min<std::uint64_t>(std::max<std::uint64_t>(complete,std::max(1,minimumFrames)),std::clamp(maximumFrames,std::max(1,minimumFrames),budget));
    output->adjustedOverlap=complete<static_cast<std::uint64_t>(minimumFrames);
    output->effectiveHop=output->adjustedOverlap?static_cast<double>(span)/count:count>1?static_cast<double>((complete-1)*hop)/(count-1):static_cast<double>(hop);
    output->requestedOverlap=settings.parameters.overlap;output->effectiveOverlap=1-std::min(output->effectiveHop,static_cast<double>(plan.inputSamples))/plan.inputSamples;
    output->repeatedObservation=span<count;output->totalFrames=count;
    FrameEstimator estimator(plan);
    for(std::uint64_t index=0;index<count;++index) {
        if(stopped(cancel)){output->frames.clear();output->error="已取消";return output;}
        SampleIndex first;std::uint64_t leading=0;long double fraction;
        if(complete>=static_cast<std::uint64_t>(minimumFrames)&&complete>0) {
            const auto grid=count==1?(complete-1)/2:scaled(complete-1,index,count-1);
            first=view.begin+grid*hop;fraction=(static_cast<long double>(first-view.begin)+plan.inputSamples*.5L)/span;
        } else {
            // Rational display positions retain ten cells even when real sample centers repeat.
            fraction=(index+.5L)/count;
            const auto centerOffset=scaled(span,2*index+1,2*count);
            if(centerOffset<plan.inputSamples/2){leading=plan.inputSamples/2-centerOffset;first=view.begin;}
            else first=view.begin+centerOffset-plan.inputSamples/2;
        }
        auto frame=estimator.compute(source,view,first,leading,index+1,fraction,cancel,output->error);
        if(!frame){output->frames.clear();return output;}
        frame->sourceCenter=output->sourceView.begin+scaled(output->sourceView.end-output->sourceView.begin,2*index+1,2*count);
        if(!output->adjustedOverlap&&complete>0)frame->sourceCenter=frame->sourceSamples.begin+(frame->sourceSamples.end-frame->sourceSamples.begin)/2;
        if(frame->paddedSamples)++output->paddedFrames;
        output->frames.push_back(std::move(frame));
    }
    return output;
}
std::shared_ptr<SpectralFrame> analyzeSpectrum(const SpectralSource& source,TimeRange range,FrequencyRange frequency,
    int points,const PsdSettings& settings,std::string& error,const SpectralCancel& cancel,const SpectrumProgress& progress) {
    auto p=settings.parameters;auto plan=makeSpectralAnalysisPlan(source.sampleRateHz,frequency,points,p);
    if(!plan.valid){error=plan.reason;return {};}
    if(range.end<=range.begin){error="没有有效统计数据";return {};}
    const auto span=range.end-range.begin;
    if(p.method==SpectralMethod::Periodogram){
        const auto size=span/plan.decimation+(span%plan.decimation!=0);
        if(size>262144){error="整段周期图超过 64 MiB 工作预算；请缩短统计范围或选择 Welch";return {};}
        plan.analysisSamples=std::max(plan.analysisSamples,static_cast<int>(size));
        plan.inputSamples=static_cast<std::uint64_t>(plan.analysisSamples)*plan.decimation;
    }
    const auto hop=p.method==SpectralMethod::Bartlett?plan.inputSamples:std::max<std::uint64_t>(1,static_cast<std::uint64_t>(plan.inputSamples*(1-p.overlap)));
    const auto count=p.method==SpectralMethod::Periodogram?1:(span<=plan.inputSamples?1:1+(span-plan.inputSamples)/hop+((span-plan.inputSamples)%hop!=0));
    FrameEstimator estimator(plan);std::shared_ptr<SpectralFrame> output;std::vector<double> sum(points);double weight=0;std::uint64_t totalPadding=0;
    for(std::uint64_t index=0;index<count;++index){
        if(stopped(cancel)){error="已取消";return {};}
        const auto first=range.begin+index*hop;
        auto frame=estimator.compute(source,range,first,0,index+1,.5L,cancel,error);if(!frame)return {};
        if(!output){output=std::make_shared<SpectralFrame>(*frame);std::fill(output->linearPower.begin(),output->linearPower.end(),settings.statistic==SpectrumStatistic::Minimum?std::numeric_limits<float>::infinity():0);}
        totalPadding+=frame->paddedSamples;const double w=std::max(1e-30,frame->windowEnergy);weight+=w;
        for(int k=0;k<points;++k){if(settings.statistic==SpectrumStatistic::Mean)sum[k]+=frame->linearPower[k]*w;
            else if(settings.statistic==SpectrumStatistic::Maximum)output->linearPower[k]=std::max(output->linearPower[k],frame->linearPower[k]);
            else output->linearPower[k]=std::min(output->linearPower[k],frame->linearPower[k]);}
        if(progress&&(index%16==0||index+1==count)){
            auto partial=std::make_shared<SpectralFrame>(*output);if(settings.statistic==SpectrumStatistic::Mean)for(int k=0;k<points;++k)partial->linearPower[k]=static_cast<float>(sum[k]/weight);
            progress(index+1,count,std::move(partial));
        }
    }
    if(settings.statistic==SpectrumStatistic::Mean)for(int k=0;k<points;++k)output->linearPower[k]=static_cast<float>(sum[k]/weight);
    output->providerSamples=range;output->sourceSamples=source.sourceRange?source.sourceRange(range):range;
    output->processedFrames=count;output->paddedSamples=totalPadding;output->observedSeconds=static_cast<double>(span)/source.sampleRateHz;
    error.clear();return output;
}
std::shared_ptr<SpectralFrame> averageSpectrum(const SpectrogramData& data) {
    if(data.frames.empty())return {};
    auto output=std::make_shared<SpectralFrame>(*data.frames.front());output->sourceSamples=data.sourceView;output->providerSamples=data.providerView;
    std::fill(output->linearPower.begin(),output->linearPower.end(),0);double weight=0;
    for(const auto& frame:data.frames)weight+=std::max(1e-30,frame->windowEnergy);
    for(const auto& frame:data.frames)for(std::size_t bin=0;bin<frame->linearPower.size();++bin)output->linearPower[bin]+=static_cast<float>(frame->linearPower[bin]*std::max(1e-30,frame->windowEnergy)/weight);
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
        const auto* frame = data.frameAtFraction(t);
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
