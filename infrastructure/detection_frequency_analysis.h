#pragma once

#include <QString>
#include <span>
#include <string>
#include <vector>

#include "domain/detection.h"
#include "domain/detection_postprocess.h"
#include "infrastructure/detection_source.h"

namespace signalstudio {

struct DetectionFrequencyEvidence {
  std::string seedId, method, skippedReason;
  FrequencyRange originalFrequency, frequency;
  bool applied = false, shrunk = false, hasNoiseFloor = false;
  bool hasMeasuredBins = false, hasPeak = false;
  double noiseFloorDb = 0, thresholdDb = 0, peakDb = 0;
  std::uint64_t firstFrame = 0, lastFrameExclusive = 0;
  std::uint64_t frameCount = 0, globalFrameCount = 0;
  std::uint64_t globalFirstFrame = 0, globalLastFrameExclusive = 0;
  std::uint64_t percentileSampleCount = 0, noiseSampleCount = 0;
  std::size_t originalFirstBin = 0, originalLastBin = 0;
  std::size_t measuredFirstBin = 0, measuredLastBin = 0;
  std::string meanSpectrumSha256, maximumSpectrumSha256;
  std::string numericalPolicy =
      "unique-real-frames; float64-dB-mean; "
      "numpy-2.4-scalar-float32-linear-percentile";
  bool operator==(const DetectionFrequencyEvidence&) const = default;
};

// Pure GUI frequency measurement, independent of time-event refinement.
// Invalid input/cancellation leaves output unchanged. The caller supplies the
// unnormalized physical dB spectrum and the global 10th-percentile floor.
bool refineGuiFrequencySpectrum(std::span<const double> meanDb,
                                FrequencyRange original, double firstBinHz,
                                double binWidthHz, double noiseFloorDb,
                                double marginDb,
                                DetectionFrequencyEvidence& output,
                                QString& error,
                                const DetectionSourceCancel& cancel = {});

// Re-reads real DSP frames; model-window overlap never multiplies a frame's
// weight. GUI global percentile uses a disk spool capped at
// min(64 * workingSetBudgetBytes, 16 GiB), with 4 MiB radix-read blocks.
// A successful non-SCN spool stays on the source until close or that analysis
// fails. Zero margin and SCN do not create one.
// Frequency updates and evidence publish atomically after all tracks succeed.
// nominalSupport remains observation context, never transmission timing.
bool applyDetectionFrequencyAnalysis(
    FixedDetectionSource& source, const DetectionRequest& request,
    std::vector<FrequencyTrack>& tracks,
    std::vector<DetectionFrequencyEvidence>& output, QString& error,
    const DetectionSourceCancel& cancel = {});

}  // namespace signalstudio
