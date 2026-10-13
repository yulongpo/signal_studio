#pragma once
#include <QString>
#include <optional>

#include "domain/project.h"

namespace signalstudio {
struct SourceSuggestions {
  QString dataPath, provenance, error, conflicts, warnings;
  std::optional<double> sampleRateHz, centerFrequencyHz, bandwidthHz;
  std::optional<SampleFormat> format;
  std::optional<std::uint64_t> sampleCount;
  bool formatLocked = false, sampleRateLocked = false;
};
SourceSuggestions suggestSourceMetadata(const QString& path);
SourceSuggestions readSigmfMetadata(const QString& path);
SourceSuggestions readWavMetadata(const QString& path);
}  // namespace signalstudio
