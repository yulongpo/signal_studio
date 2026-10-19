#pragma once

#include "infrastructure/detection_projection.h"
#include "infrastructure/detection_source.h"

namespace signalstudio {

// Reads physical source evidence and refines a completed frequency publication
// atomically. Cancellation leaves publication unchanged. A resource-limited
// candidate remains an explicitly labelled fallback observation/coarse segment.
bool applyDetectionTimeAnalysis(
    FixedDetectionSource&, const FileMetadata&, const DetectionRequest&,
    const std::vector<FrequencyTrack>&, DetectionPublication&, QString& error,
    const DetectionSourceCancel& cancelled = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {});

}  // namespace signalstudio
