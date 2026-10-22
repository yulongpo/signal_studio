#pragma once
#include "infrastructure/narrowband_analysis/analysis_source.h"
namespace signalstudio {
NarrowbandAnalysisResult analyzeNarrowband(std::shared_ptr<const AnalysisSourceData>,
    const NarrowbandAnalysisSettings&,const AnalysisCancel& cancelled={});
}  // namespace signalstudio
