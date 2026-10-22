#pragma once
#include "infrastructure/narrowband_analysis/pulse_shaping.h"
namespace signalstudio {
void buildEye(NarrowbandAnalysisResult&,const NarrowbandAnalysisSettings&,const AnalysisCancel& cancelled={});
}  // namespace signalstudio
