#include "infrastructure/narrowband_analysis/pulse_shaping.h"
#include <array>
#include <cmath>
#include <numbers>

namespace signalstudio {
namespace {
constexpr int phases=1024,tapCount=32;
const auto& interpolationTable() {
  static const auto table=[] {
    std::array<std::array<double,tapCount>,phases+1> values{};
    const double normalization=std::cyl_bessel_i(0.0,7.0);
    for(int phase=0;phase<=phases;++phase) {
      const double fraction=static_cast<double>(phase)/phases;double sum=0;
      for(int tap=0;tap<tapCount;++tap) {
        const double x=tap-15-fraction;
        const double sinc=std::abs(x)<1e-12?1:std::sin(std::numbers::pi*x)/(std::numbers::pi*x);
        const double window=std::abs(x)>16?0:std::cyl_bessel_i(0.0,7*std::sqrt(std::max(0.0,1-x*x/256)))/normalization;
        values[phase][tap]=sinc*window;sum+=values[phase][tap];
      }
      for(auto& value:values[phase])value/=sum;
    }
    return values;
  }();
  return table;
}
double rrc(double t,double a) {
  if(std::abs(t)<1e-10)return 1+a*(4/std::numbers::pi-1);
  if(std::abs(std::abs(t)-1/(4*a))<1e-8)
    return a/std::sqrt(2.0)*((1+2/std::numbers::pi)*std::sin(std::numbers::pi/(4*a))+(1-2/std::numbers::pi)*std::cos(std::numbers::pi/(4*a)));
  return (std::sin(std::numbers::pi*t*(1-a))+4*a*t*std::cos(std::numbers::pi*t*(1+a)))/(std::numbers::pi*t*(1-16*a*a*t*t));
}
}
bool interpolateBandlimited(std::span<const std::complex<float>> values,double position,std::complex<double>& value) {
  if(!std::isfinite(position) || position<15 || position+16>=static_cast<double>(values.size()))return false;
  const auto integer=static_cast<std::size_t>(std::floor(position));
  const double phase=(position-static_cast<double>(integer))*phases;
  const auto p=static_cast<int>(std::floor(phase));const double mix=phase-p;
  const auto& table=interpolationTable();value={};
  for(int i=0;i<tapCount;++i) {
    const double coefficient=table[p][i]*(1-mix)+table[p+1][i]*mix;
    value+=std::complex<double>(values[integer-15+static_cast<std::size_t>(i)])*coefficient;
  }
  return std::isfinite(value.real()) && std::isfinite(value.imag());
}
bool matchedRrc(std::span<const std::complex<float>> input,double sps,double alpha,int span,
                std::vector<std::complex<float>>& output,std::size_t& discarded,
                std::string& error,const AnalysisCancel& cancelled) {
  output.clear();discarded=0;
  if(!std::isfinite(sps) || sps<=1 || !std::isfinite(alpha) || alpha<.05 || alpha>1 || span<2 || span>32) {error="RRC 的 SPS、滚降或跨度无效。";return false;}
  if(sps+1e-9<1+alpha){error="实际 Fs 不足以覆盖此 RRC 滚降假设的带宽。";return false;}
  const double length=std::ceil(sps*span);
  if(length>4095 || length*input.size()>256e6) {error="RRC 运算预算超限，请缩小分析时窗或降低滤波跨度。";return false;}
  const auto taps=static_cast<std::size_t>(length)|std::size_t{1};discarded=taps/2;
  if(input.size()<=2*discarded+32) {error="有效样本不足以排除 RRC 滤波瞬态。";return false;}
  std::vector<double> coefficients(taps);double energy=0;
  for(std::size_t i=0;i<taps;++i) {coefficients[i]=rrc((static_cast<double>(i)-discarded)/sps,alpha);energy+=coefficients[i]*coefficients[i];}
  for(auto& coefficient:coefficients)coefficient/=energy;
  output.resize(input.size()-2*discarded);
  for(std::size_t i=0;i<output.size();++i) {
    if((i&255U)==0 && cancelled && cancelled()) {output.clear();error="分析已取消。";return false;}
    std::complex<double> sum{};
    for(std::size_t j=0;j<taps;++j)sum+=std::complex<double>(input[i+j])*coefficients[j];
    output[i]=static_cast<std::complex<float>>(sum);
  }
  error.clear();return true;
}
}  // namespace signalstudio
