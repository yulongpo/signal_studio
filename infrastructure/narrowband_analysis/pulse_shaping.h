#pragma once
#include <span>
#include "infrastructure/narrowband_analysis/analysis_source.h"
namespace signalstudio {
bool interpolateBandlimited(std::span<const std::complex<float>>,double position,std::complex<double>&);
bool matchedRrc(std::span<const std::complex<float>>,double samplesPerSymbol,double alpha,int spanSymbols,
                std::vector<std::complex<float>>&,std::size_t& discardedEachSide,
                std::string& error,const AnalysisCancel& cancelled={});
}  // namespace signalstudio
