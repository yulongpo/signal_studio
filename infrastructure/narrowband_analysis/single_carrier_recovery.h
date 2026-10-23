#pragma once
#include "infrastructure/narrowband_analysis/analysis_source.h"
namespace signalstudio {
void recoverSingleCarrier(NarrowbandAnalysisResult&,const NarrowbandAnalysisSettings&,const AnalysisCancel& cancelled={});
const ContinuousAnalysisStage* findAnalysisStage(const NarrowbandAnalysisResult&,AnalysisStage);
}  // namespace signalstudio
