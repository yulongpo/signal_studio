#pragma once

#include <functional>
#include "domain/narrowband_analysis.h"
#include "infrastructure/channel_processor.h"

namespace signalstudio {
using AnalysisCancel=std::function<bool()>;
std::string analysisSourceKey(const FileMetadata&,const Channel&);
bool mapAnalysisPosition(const AnalysisSourceData&,SamplePosition output,SamplePosition& source);
bool analysisIdentityCurrent(const AnalysisIdentity&,const FileMetadata&,const Channel&,
                             std::uint64_t projectGeneration,std::uint64_t requestGeneration);
AnalysisSourceData readAnalysisSource(ChannelSampleCache&,const FileMetadata&,const Channel&,
                                      const AnalysisRequest&,const AnalysisCancel& cancelled={});
}  // namespace signalstudio
