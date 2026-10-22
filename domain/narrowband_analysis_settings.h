#pragma once
#include <cstddef>
namespace signalstudio {
enum class AnalysisStage { RawIq, CfoCorrected, MatchedFiltered, TimingAligned, CarrierRecovered };
enum class SymbolRateMode { Unknown, Manual, Estimated };
enum class ModulationHypothesis { Unknown, Bpsk, Qpsk, Qam16, Fsk, Ofdm, Gmsk };
enum class CfoMode { Off, Automatic, Manual };
struct NarrowbandAnalysisSettings {
  int pointCount=4096;
  bool removeDc=false, normalizePower=false, constellationAutoScale=true;
  std::size_t sampleBudget=1U*1024U*1024U;
  SymbolRateMode symbolRateMode=SymbolRateMode::Unknown;
  double symbolRateHz=0;
  ModulationHypothesis hypothesis=ModulationHypothesis::Unknown;
  AnalysisStage constellationStage=AnalysisStage::RawIq, eyeStage=AnalysisStage::RawIq;
  int eyePeriods=2,eyeTraces=64,eyeComponent=0,eyePointsPerUi=16;
  double eyeTimingOffsetUi=0;
  bool matchedFilter=false,timingRecovery=false,carrierRecovery=false;
  double rolloff=.35;
  int filterSpanSymbols=10;
  CfoMode cfoMode=CfoMode::Off;
  double manualCfoHz=0,manualPhaseRadians=0;
  double timingLoopBandwidth=.002,carrierLoopBandwidth=.01;
  bool operator==(const NarrowbandAnalysisSettings&) const = default;
};
}  // namespace signalstudio
