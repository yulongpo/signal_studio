#pragma once
#include "domain/project.h"
#include <QString>
#include <optional>

namespace signalstudio {
struct SourceSuggestions {
    QString dataPath,provenance,error;
    std::optional<double> sampleRateHz,centerFrequencyHz,bandwidthHz;
    std::optional<SampleFormat> format;
};
SourceSuggestions suggestSourceMetadata(const QString& path);
}
