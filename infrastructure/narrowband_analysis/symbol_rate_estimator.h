#pragma once
#include <span>
#include "infrastructure/narrowband_analysis/analysis_source.h"
namespace signalstudio {
struct SymbolRateEstimate {
  std::vector<SymbolRateCandidate> candidates;
  bool reliable=false;
  std::size_t observedSamples=0;
  std::string message;
};
SymbolRateEstimate estimateSymbolRate(std::span<const std::complex<float>>,double sampleRateHz,const AnalysisCancel& cancelled={});
}  // namespace signalstudio
