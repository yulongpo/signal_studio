#include "infrastructure/narrowband_analysis/symbol_rate_estimator.h"
#include "infrastructure/narrowband_analysis/fft.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace signalstudio {
namespace {
double median(std::vector<double> values,std::size_t first,std::size_t last) {
  if(last<=first)return 1e-30;
  auto begin=values.begin()+static_cast<std::ptrdiff_t>(first),end=values.begin()+static_cast<std::ptrdiff_t>(last);
  auto mid=begin+(end-begin)/2;std::nth_element(begin,mid,end);return std::max(*mid,1e-30);
}
}
SymbolRateEstimate estimateSymbolRate(std::span<const std::complex<float>> samples,double fs,const AnalysisCancel& cancelled) {
  SymbolRateEstimate result;
  if(samples.size()<2048 || !std::isfinite(fs) || fs<=0) {result.message="有效观测不足，不能可靠估计 Rs。";return result;}
  double peak=0,mean=0,second=0;
  for(const auto value:samples){const auto power=static_cast<double>(std::norm(value));peak=std::max(peak,power);mean+=power;second+=power*power;}
  mean/=samples.size();second/=samples.size();
  std::size_t active=0;for(const auto value:samples)if(std::norm(value)>peak*.02)++active;
  if(mean<1e-20 || active<1024) {result.message="真实有效突发过短或没有可用信号能量。";return result;}
  if(std::sqrt(std::max(0.0,second-mean*mean))/mean<.01) {result.message="近恒包络；当前观测没有可辨识的符号率循环特征。";return result;}
  std::size_t length=512;while(length*2<=std::min<std::size_t>(16384,samples.size()/3))length*=2;
  const auto blocks=std::min<std::size_t>(3,samples.size()/length);
  if(blocks<2){result.message="独立时间子窗不足，不能判断 Rs 候选稳定性。";return result;}
  result.observedSamples=length*blocks;
  const std::size_t low=8,high=length/2-2;
  std::vector<double> aggregate(length/2+1,0);
  std::vector<std::vector<double>> windowPower(blocks,std::vector<double>(length/2+1,0));
  for(std::size_t block=0;block<blocks;++block) {
    const auto origin=block*(samples.size()-length)/(blocks-1);
    for(int method=0;method<4;++method) {
      if(cancelled && cancelled()){result.message="符号率估计已取消。";return result;}
      std::vector<std::complex<double>> signal(length);std::complex<double> dc{};
      for(std::size_t i=0;i<length;++i) {
        const auto value=std::complex<double>(samples[origin+i]);
        if(method==0)signal[i]=std::norm(value);
        else if(method==3)signal[i]=std::norm(value)*std::norm(value);
        else signal[i]=value*std::conj(std::complex<double>(samples[origin+(i>=static_cast<std::size_t>(method)?i-method:0)]));
        dc+=signal[i];
      }
      dc/=static_cast<double>(length);
      for(std::size_t i=0;i<length;++i)signal[i]=(signal[i]-dc)*(.5-.5*std::cos(2*std::numbers::pi*i/(length-1)));
      narrowband_dsp::fft(signal);
      std::vector<double> power(length/2+1);
      for(std::size_t k=low;k<high;++k)power[k]=std::norm(signal[k])+std::norm(signal[length-k]);
      const double floor=median(power,low,high),weight=method==3?.5:1;
      for(std::size_t k=low;k<high;++k)windowPower[block][k]+=weight*power[k]/floor;
    }
    for(std::size_t k=low;k<high;++k)aggregate[k]+=windowPower[block][k]/blocks;
  }
  const double baseline=median(aggregate,low,high);
  std::vector<double> windowBaselines;for(const auto& powers:windowPower)windowBaselines.push_back(median(powers,low,high));
  std::vector<SymbolRateCandidate> candidates;
  for(std::size_t k=low+1;k+1<high;++k) {
    if(aggregate[k]<=aggregate[k-1] || aggregate[k]<aggregate[k+1])continue;
    const double prominence=10*std::log10(std::max(aggregate[k]/baseline,1e-30));
    if(prominence<6)continue;
    double weakest=1000;
    for(std::size_t block=0;block<windowPower.size();++block) {
      const auto& powers=windowPower[block];
      const double local=std::max({powers[k-1],powers[k],powers[k+1]});
      weakest=std::min(weakest,10*std::log10(local/windowBaselines[block]));
    }
    const double a=std::log(std::max(aggregate[k-1],1e-30)),b=std::log(std::max(aggregate[k],1e-30)),c=std::log(std::max(aggregate[k+1],1e-30));
    const double correction=std::abs(a-2*b+c)>1e-12?std::clamp(.5*(a-c)/(a-2*b+c),-.5,.5):0;
    const double rate=(k+correction)*fs/length;
    const double score=std::clamp((prominence-5)/15,0.0,1.0)*std::clamp((weakest-3)/8,0.0,1.0);
    if(score<=0)continue;
    candidates.push_back({rate,score,prominence,"包络二/四阶与延迟循环谱；三个真实时间子窗"});
  }
  std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.score!=b.score?a.score>b.score:a.prominenceDb>b.prominenceDb;});
  const auto add=[&](SymbolRateCandidate candidate) {
    if(candidate.rateHz<=0 || candidate.rateHz>=fs)return;
    for(const auto& old:result.candidates)if(std::abs(old.rateHz-candidate.rateHz)<std::max(fs/length*2,candidate.rateHz*.005))return;
    result.candidates.push_back(std::move(candidate));
  };
  for(const auto& candidate:candidates){add(candidate);if(result.candidates.size()==5)break;}
  if(!result.candidates.empty()) {
    const auto leading=result.candidates.front();
    if(result.candidates.size()<5)add({fs-leading.rateHz,leading.score*.55,leading.prominenceDb,"循环频率混叠镜像；需人工确认"});
    std::sort(result.candidates.begin(),result.candidates.end(),[](const auto& a,const auto& b){return a.score>b.score;});
    result.reliable=leading.score>=.8 && result.observedSamples*leading.rateHz/fs>=256 && leading.rateHz<=fs/2;
  }
  result.message=result.reliable?"检测到稳定 Rs 候选；评分不是概率，也不代表调制识别。":"Rs 候选置信度不足；可选择候选作为手工假设或直接输入。";
  return result;
}
}  // namespace signalstudio
