#include "domain/detection.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {
bool finiteBetween(double value, double lower, double upper) {
  return std::isfinite(value) && value >= lower && value <= upper;
}
bool validRange(ViewRange range, const SourceIdentity& source,
                bool requireAvailable = true) {
  const double half = source.sampleRateHz / 2;
  const double lower = source.sampleFormat.structure == SampleStructure::Real
                           ? source.centerFrequencyHz
                           : source.centerFrequencyHz - half;
  return range.time.begin < range.time.end &&
         range.time.end <= (requireAvailable ? source.availableSamples
                                             : source.physicalSamples) &&
         std::isfinite(range.frequency.lowerHz) &&
         std::isfinite(range.frequency.upperHz) &&
         range.frequency.lowerHz < range.frequency.upperHz &&
         range.frequency.lowerHz >= lower &&
         range.frequency.upperHz <= source.centerFrequencyHz + half;
}
}  // namespace

const std::vector<DetectionProfile>& detectionProfiles() {
  static const std::vector<DetectionProfile> profiles{
      {"ffscn-gui-iq-v1", 1, "FFSCN GUI IQ", "主线候选 / 需资产预检",
       DetectionBackendKind::TensorRt, 65536, 10, 5, .001, false, false},
      {"scn-max-psd-v1", 1, "SCN 频率谱", "频率对照", DetectionBackendKind::Scn,
       32768, 16, 5, .001, true, false},
      {"ffscn-17-experimental-v1", 1, "FFSCN 17阶 · 10帧", "实验 / 未正式验证",
       DetectionBackendKind::Experimental17, 131072, 10, 5, .001, false, false},
      {"pipeline-fixture-v1", 1, "系统链路夹具 · 不是真实模型", "仅工程测试",
       DetectionBackendKind::Fixture, 65536, 10, 5, .001, false, false}};
  return profiles;
}
const DetectionProfile* findDetectionProfile(const std::string& id) {
  const auto& profiles = detectionProfiles();
  const auto found = std::find_if(
      profiles.begin(), profiles.end(),
      [&](const DetectionProfile& profile) { return profile.id == id; });
  return found == profiles.end() ? nullptr : &*found;
}
const char* detectionLifecycleName(DetectionLifecycle state) {
  switch (state) {
    case DetectionLifecycle::Queued:
      return "Queued";
    case DetectionLifecycle::Validating:
      return "Validating";
    case DetectionLifecycle::Running:
      return "Running";
    case DetectionLifecycle::Cancelling:
      return "Cancelling";
    case DetectionLifecycle::Completed:
      return "Completed";
    case DetectionLifecycle::Cancelled:
      return "Cancelled";
    case DetectionLifecycle::Failed:
      return "Failed";
    case DetectionLifecycle::Interrupted:
      return "Interrupted";
  }
  return "Failed";
}
const char* detectionSourceValidityName(DetectionSourceValidity state) {
  switch (state) {
    case DetectionSourceValidity::Current:
      return "Current";
    case DetectionSourceValidity::Missing:
      return "Missing";
    case DetectionSourceValidity::Changed:
      return "Changed";
    case DetectionSourceValidity::Unsupported:
      return "Unsupported";
  }
  return "Unsupported";
}
const char* detectionCoverageKindName(DetectionCoverageKind kind) {
  switch (kind) {
    case DetectionCoverageKind::Read:
      return "read";
    case DetectionCoverageKind::Eligible:
      return "eligible";
    case DetectionCoverageKind::Completed:
      return "completed";
    case DetectionCoverageKind::Excluded:
      return "excluded";
    case DetectionCoverageKind::Unread:
      return "unread";
  }
  return "unread";
}
const char* detectionTimeStrategyName(DetectionTimeStrategy strategy) {
  switch (strategy) {
    case DetectionTimeStrategy::Sample:
      return "sample";
    case DetectionTimeStrategy::Roi:
      return "roi";
    case DetectionTimeStrategy::Coarse:
      return "coarse";
    case DetectionTimeStrategy::None:
      return "none";
  }
  return "none";
}
bool detectionIsTerminal(DetectionLifecycle state) {
  return state == DetectionLifecycle::Completed ||
         state == DetectionLifecycle::Cancelled ||
         state == DetectionLifecycle::Failed ||
         state == DetectionLifecycle::Interrupted;
}
bool detectionTransitionAllowed(DetectionLifecycle from,
                                DetectionLifecycle to) {
  if (from == to) return true;
  if (detectionIsTerminal(from)) return false;
  if (to == DetectionLifecycle::Failed || to == DetectionLifecycle::Interrupted)
    return true;
  if (to == DetectionLifecycle::Cancelling) return true;
  if (from == DetectionLifecycle::Queued)
    return to == DetectionLifecycle::Validating ||
           to == DetectionLifecycle::Cancelled;
  if (from == DetectionLifecycle::Validating)
    return to == DetectionLifecycle::Running ||
           to == DetectionLifecycle::Cancelled;
  if (from == DetectionLifecycle::Running)
    return to == DetectionLifecycle::Completed ||
           to == DetectionLifecycle::Cancelled;
  return from == DetectionLifecycle::Cancelling &&
         to == DetectionLifecycle::Cancelled;
}
DetectionValidation validateDetectionRequest(const DetectionRequest& request) {
  if(static_cast<int>(request.sampleEnergyMode)<0 || static_cast<int>(request.sampleEnergyMode)>2 ||
      request.sampleScratchBudgetBytes>32ULL*1024*1024*1024)
    return {false,"Invalid sample energy mode or bounded temporary storage quota"};
  const auto* profile = findDetectionProfile(request.profile.id);
  if (!profile || request.profile.version != profile->version ||
      request.profile.backend != profile->backend ||
      request.profile.maximumPsdOnly != profile->maximumPsdOnly ||
      request.profile.supportsReal != profile->supportsReal)
    return {false, "Detection profile identity/shape is not registered"};
  if (request.inputAdaptationVersion == 1 &&
      (request.profile.fftSize != profile->fftSize ||
       request.profile.modelFrames != profile->modelFrames ||
       request.profile.modelHopFrames != profile->modelHopFrames ||
       request.profile.hopSeconds != profile->hopSeconds ||
       request.spectrumMode != DetectionSpectrumMode::ModelDefault))
    return {false, "Historical detection grid identity changed"};
  const auto& grid = request.profile;
  if ((request.inputAdaptationVersion != 1 &&
       request.inputAdaptationVersion != 2) ||
      static_cast<int>(request.spectrumMode) < 0 ||
      static_cast<int>(request.spectrumMode) > 4 || grid.fftSize < 32 ||
      grid.fftSize > 262144 || !grid.modelFrames || !grid.modelHopFrames ||
      !std::isfinite(grid.hopSeconds) || grid.hopSeconds <= 0)
    return {false, "Detection analysis grid is invalid"};
  const auto& source = request.sourceIdentity;
  if (request.runId.empty() || request.runId.size() > 128 ||
      source.fileId.empty() || source.path.empty() ||
      !finiteBetween(source.sampleRateHz, 1, 1e12) ||
      !finiteBetween(source.centerFrequencyHz, -1e12, 1e12) ||
      source.availableSamples > source.physicalSamples ||
      !source.availableSamples ||
      source.selectedSampleChannel != source.sampleFormat.selectedChannel)
    return {false, "Detection source identity is incomplete"};
  if (!profile->supportsReal &&
      source.sampleFormat.structure == SampleStructure::Real)
    return {false,
            "This profile requires complex IQ; labels/manual annotation remain "
            "available"};
  if (!validRange(request.requestedRange, source, false))
    return {false,
            "Requested detection range exceeds available samples or sampled "
            "frequencies"};
  if (!validRange(request.contextRange, source))
    return {false, "Detection context range exceeds its source snapshot"};
  const auto& parameters = request.parameters;
  if (!finiteBetween(parameters.scoreThreshold, 0, 1) ||
      !finiteBetween(parameters.nmsIou, 0, 1) ||       !parameters.topK ||
      parameters.topK > 512 ||
      !isDetectionExecutionBatchTier(parameters.batchSize) || !parameters.minHits ||
      parameters.minHits > 10000 || parameters.maxMiss > 10000 ||
      !finiteBetween(parameters.mergeFrequencyHz, 0, source.sampleRateHz) ||
      !finiteBetween(parameters.bandwidthMarginDb, 0, 100) ||
      !finiteBetween(parameters.sampleEdgeRatio, 0, 1) ||
      !finiteBetween(parameters.minOnSeconds, 0, 60) ||
      !finiteBetween(parameters.minOffSeconds, 0, 60) ||
      !finiteBetween(parameters.smoothSeconds, 0, 60) ||
      !finiteBetween(parameters.coarseSplitGapSeconds, 0, 60) ||
      !finiteBetween(parameters.coarseMinBurstSeconds, 0, 60))
    return {false, "Detection parameters are invalid"};
  if (request.workingSetBudgetBytes < 16 * 1024 * 1024 ||
      request.workingSetBudgetBytes > 1024ULL * 1024 * 1024 ||
      request.evidenceBudgetBytes > 256 * 1024 * 1024 ||
      request.rawHeadBudgetBytes > 2ULL * 1024 * 1024 * 1024)
    return {false, "Detection resource budget is invalid"};
  return {};
}
std::uint32_t detectionModelMinimumBins(const DetectionRequest& request) {
  if (request.profile.backend == DetectionBackendKind::Fixture) return 1;
  // FFSCN GUI and SCN use down_nums=11. The separate experimental FFSCN-17
  // architecture declares thirteen stages in its frozen model contract.
  const auto downNums =
      request.profile.backend == DetectionBackendKind::Experimental17 ? 13 : 11;
  return 1U << downNums;
}
std::uint32_t detectionModelInputBins(const DetectionRequest& request) {
  if (request.inputAdaptationVersion == 1 ||
      request.profile.backend == DetectionBackendKind::Fixture)
    return request.profile.fftSize;
  // The installed SCN artifact has one fixed tensor grid. This is a deployment
  // specification, separate from the architecture's 2048-bin minimum.
  if (request.profile.backend == DetectionBackendKind::Scn) return 32768;
  return std::bit_ceil(
      std::max(request.profile.fftSize, detectionModelMinimumBins(request)));
}
double detectionModelBinScale(const DetectionRequest& request) {
  return static_cast<double>(request.profile.fftSize) /
         detectionModelInputBins(request);
}
std::uint32_t detectionModelTensorFrames(const DetectionRequest& request,
                                         std::uint32_t actualFrames) {
  return request.profile.backend == DetectionBackendKind::Scn ||
                 request.spectrumMode != DetectionSpectrumMode::ModelDefault
             ? 1
             : actualFrames;
}
bool selectDetectionSpectrum(std::span<const float> values, std::size_t bins,
                             DetectionSpectrumMode mode,
                             std::vector<float>& target, std::string& error) {
  if (!bins || values.empty() || values.size() % bins ||
      static_cast<int>(mode) < 0 || static_cast<int>(mode) > 4 ||
      !std::all_of(values.begin(), values.end(),
                   [](float value) { return std::isfinite(value); })) {
    error = "invalid-detection-spectrum-selection";
    return false;
  }
  const auto frames = values.size() / bins;
  std::vector<float> selected(bins);
  for (std::size_t bin = 0; bin < bins; ++bin) {
    float maximum = values[bin], minimum = maximum;
    for (std::size_t frame = 1; frame < frames; ++frame) {
      maximum = std::max(maximum, values[frame * bins + bin]);
      minimum = std::min(minimum, values[frame * bins + bin]);
    }
    if (mode == DetectionSpectrumMode::Mean) {
      double power = 0;
      for (std::size_t frame = 0; frame < frames; ++frame)
        power +=
            std::pow(10.0, (values[frame * bins + bin] - double(maximum)) / 10);
      selected[bin] =
          static_cast<float>(maximum + 10 * std::log10(power / frames));
    } else if (mode == DetectionSpectrumMode::Minimum)
      selected[bin] = minimum;
    else if (mode == DetectionSpectrumMode::Current)
      selected[bin] = values[(frames - 1) * bins + bin];
    else
      selected[bin] = maximum;
  }
  target = std::move(selected);
  error.clear();
  return true;
}
DetectionValidation validateDetectionRun(const DetectionRun& run) {
  if (const auto valid = validateDetectionRequest(run.request); !valid)
    return valid;
  if (run.performanceJson.size() > 65536)
    return {false, "Detection performance diagnostics exceed 64 KiB"};
  if (run.completedWindows > run.eligibleWindows ||
      run.refinedCandidates > run.totalRefinementCandidates ||
      run.totalRefinementCandidates > annotationSegmentBudget ||
      run.chunks.size() > 100000 || run.coverage.size() > 100000 ||
      run.resultCount > 100000)
    return {false, "Detection run counts exceed their coverage/budget"};
  if (run.computationPhase != "none" && run.computationPhase != "reading" &&
      run.computationPhase != "inference" &&
      run.computationPhase != "decoding" &&
      run.computationPhase != "storing-heads" &&
      run.computationPhase != "tracking" &&
      run.computationPhase != "checkpoint" &&
      run.computationPhase != "frequency" && run.computationPhase != "refine" &&
      run.computationPhase != "publishing" &&
      run.computationPhase != "completed")
    return {false, "Unknown detection computation phase"};
  std::unordered_set<std::uint64_t> sequences;
  for (const auto& chunk : run.chunks)
    if (!sequences.insert(chunk.sequence).second || chunk.resultCount > 4096 ||
        chunk.sha256.size() != 64 || chunk.relativePath.empty())
      return {false, "Invalid or duplicate detection result chunk"};
  return {};
}
}  // namespace signalstudio
