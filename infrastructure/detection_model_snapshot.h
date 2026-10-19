#pragma once

#include <QString>

#include "domain/detection.h"

namespace signalstudio {

// Configuration check only. The worker verifies engine/runtime hashes and the
// actual CUDA device before it accepts the immutable request.
bool freezeDetectionModelSnapshot(DetectionRequest&, QString& error);

}  // namespace signalstudio
