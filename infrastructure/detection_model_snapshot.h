#pragma once

#include <functional>

#include <QString>

#include "domain/detection.h"

namespace signalstudio {

// Configuration check only. The worker verifies engine/runtime hashes and the
// actual CUDA device before it accepts the immutable request. cancel is polled
// at each snapshot checkpoint. A true result leaves the request unchanged.
// The default empty callback preserves existing callers.
bool freezeDetectionModelSnapshot(DetectionRequest&, QString& error,
                                  const std::function<bool()>& cancel = {});

}  // namespace signalstudio
