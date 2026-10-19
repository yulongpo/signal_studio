#pragma once

#include "infrastructure/detection_projection.h"
#include "infrastructure/detection_source.h"

namespace signalstudio {
bool fuseDetectionPublication(const DetectionRequest&, DetectionPublication&,
                              QString& error,
                              const DetectionSourceCancel& cancelled = {});
}  // namespace signalstudio
