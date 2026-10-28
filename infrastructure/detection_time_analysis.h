#pragma once

#include "domain/detection_refinement.h"
#include "infrastructure/detection_projection.h"
#include "infrastructure/detection_source.h"

namespace signalstudio {

// Reads physical source evidence and refines a completed frequency publication
// atomically. Cancellation leaves publication unchanged. A sample resource
// limit can keep a measured coarse observation. A working-cell rejection
// fails the call and does not publish activity.

// Reads one track's real-frame spectrum. When the frequency spool covers the
// frame interval, densityDb is read back from that file and time slots still
// come from the source frame grid. A failed file read uses readFrame.
bool readSpectrum(FixedDetectionSource& source, const FrequencyTrack& track,
                  DetectionSpectrumRoi& spectrum, std::uint64_t budget,
                  std::uint64_t& consumedCells, std::size_t& cellBudget,
                  QString& error, const DetectionSourceCancel& cancel = {},
                  DetectionFailureCode* failure = nullptr);

bool applyDetectionTimeAnalysis(
    FixedDetectionSource&, const FileMetadata&, const DetectionRequest&,
    const std::vector<FrequencyTrack>&, DetectionPublication&, QString& error,
    const DetectionSourceCancel& cancelled = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {},
    const QString& privateScratchRoot = {});

}  // namespace signalstudio
