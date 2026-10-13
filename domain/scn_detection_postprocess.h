#pragma once

#include <span>
#include <string>
#include <vector>

#include "domain/detection_postprocess.h"

namespace signalstudio {
struct ScnCandidateEvidence {
  DecodedCandidate candidate;
  double signalLevelDb = 0, neighborLevelDb = 0, cnrDb = 0;
  bool accepted = false;
};
struct ScnWindowPostprocess {
  std::vector<ScnCandidateEvidence> measurements;
  std::vector<DecodedCandidate> unfilteredFused;
  std::vector<DecodedCandidate> filteredFused;
  std::vector<ScnFrequencyRefinement> refinements;
  std::vector<DecodedCandidate> candidates;
};

// The frozen SCN comparison order is CNR -> frequency fusion -> inward
// bandwidth correction -> tracking. Every unfiltered fused band masks noise.
bool postprocessScnWindow(std::span<const float> physicalDbFrames,
                          std::size_t frames, std::size_t bins,
                          std::span<const DecodedCandidate> candidates,
                          double firstBinHz, double binWidthHz,
                          ScnWindowPostprocess&, std::string& error,
                          const PostprocessCancellation& cancelled = {});
}  // namespace signalstudio
