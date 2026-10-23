#pragma once
#include <QJsonObject>
#include "domain/narrowband_analysis.h"
namespace signalstudio {
inline QJsonObject narrowbandAnalysisEvidence(const NarrowbandAnalysisResult* r) {
    if (!r) return {{"ready",false}};
    return {{"ready",r->status==AnalysisStatus::Ready},{"channelId",QString::fromStdString(r->identity.channelId)},
        {"realSource",r->source && r->source->status==AnalysisStatus::Ready},
        {"continuousIqSamples",static_cast<qint64>(r->source?r->source->samples.size():0)},
        {"outputRateHz",static_cast<qint64>(r->source?r->source->outputRateHz:0)},
        {"pointCount",static_cast<qint64>(r->constellation.size())},{"eyeTraces",static_cast<qint64>(r->eyes.size())},
        {"constellationStage",static_cast<int>(r->constellationStage)},{"eyeStage",static_cast<int>(r->eyeStage)},
        {"symbolRateHz",r->symbolRateHz},{"samplesPerSymbol",r->samplesPerSymbol},{"cfoHz",r->estimatedCfoHz},
        {"timingLocked",r->timingLocked},{"carrierLocked",r->carrierLocked},{"conditionalEvm",r->conditionalEvm},
        {"diagnostic",QString::fromStdString(r->syncMessage)}};
}
}
