#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>
namespace signalstudio::narrowband_dsp {
inline void fft(std::vector<std::complex<double>>& data) {
  const auto n=data.size();
  for(std::size_t i=1,j=0;i<n;++i) {
    auto bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(data[i],data[j]);
  }
  for(std::size_t length=2;length<=n;length*=2) {
    const auto root=std::polar(1.0,-2*std::numbers::pi/length);
    for(std::size_t start=0;start<n;start+=length) {
      std::complex<double> phase=1;
      for(std::size_t i=0;i<length/2;++i) {
        const auto a=data[start+i],b=data[start+i+length/2]*phase;
        data[start+i]=a+b;data[start+i+length/2]=a-b;phase*=root;
      }
    }
  }
}
}  // namespace signalstudio::narrowband_dsp
