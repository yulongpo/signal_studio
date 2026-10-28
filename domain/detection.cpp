#include "domain/detection.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

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
std::filesystem::path utf8Path(const std::string& text) {
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
}  // namespace

std::uint64_t detectionPhysicalMemoryBytes() {
#ifdef _WIN32
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status) || status.ullTotalPhys == 0)
    return kDetectionWorkingSetTargetBytes;
  return status.ullTotalPhys;
#else
  const auto pages = sysconf(_SC_PHYS_PAGES);
  const auto page = sysconf(_SC_PAGE_SIZE);
  if (pages <= 0 || page <= 0) return kDetectionWorkingSetTargetBytes;
  return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
#endif
}
std::optional<std::uint64_t> detectionVolumeFreeBytes(const std::string& path) {
  std::error_code error;
  std::filesystem::path candidate =
      path.empty() ? std::filesystem::current_path(error) : utf8Path(path);
  if (error) return std::nullopt;
  auto absolute = std::filesystem::absolute(candidate, error);
  if (!error) candidate = std::move(absolute);
  for (int hop = 0; hop < 64; ++hop) {
    std::error_code existsError;
    if (std::filesystem::exists(candidate, existsError) && !existsError) break;
    const auto parent = candidate.parent_path();
    if (parent.empty() || parent == candidate) break;
    candidate = parent;
  }
#ifdef _WIN32
  ULARGE_INTEGER available;
  if (!GetDiskFreeSpaceExW(candidate.c_str(), &available, nullptr, nullptr))
    return std::nullopt;
  return static_cast<std::uint64_t>(available.QuadPart);
#else
  struct statvfs status;
  if (statvfs(candidate.c_str(), &status) != 0) return std::nullopt;
  if (status.f_frsize == 0) return std::nullopt;
  return static_cast<std::uint64_t>(status.f_bavail) *
         static_cast<std::uint64_t>(status.f_frsize);
#endif
}
std::uint64_t detectionWorkingSetLimitBytes() {
  const auto memory = detectionPhysicalMemoryBytes();
  return memory == 0 ? kDetectionWorkingSetTargetBytes : memory;
}
std::uint64_t detectionWorkingSetDefaultBytes() {
  return std::min(kDetectionWorkingSetTargetBytes, detectionWorkingSetLimitBytes());
}
std::uint64_t detectionDiskBudgetLimitBytes(const std::string& path) {
  const auto free = detectionVolumeFreeBytes(path);
  return free.value_or(kDetectionDiskBudgetTargetBytes);
}
std::uint64_t detectionDiskBudgetDefaultBytes(const std::string& path) {
  return std::min(kDetectionDiskBudgetTargetBytes,
                  detectionDiskBudgetLimitBytes(path));
}

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
DetectionValidation validateDetectionRequest(const DetectionRequest& request,
                                              const std::string& storagePath) {
  const auto diskLimit = storagePath.empty()
                             ? std::optional<std::uint64_t>{}
                             : detectionVolumeFreeBytes(storagePath);
  const bool scratchOverDisk =
      diskLimit && request.sampleScratchBudgetBytes > *diskLimit;
  if (static_cast<int>(request.sampleEnergyMode) < 0 ||
      static_cast<int>(request.sampleEnergyMode) > 2 || scratchOverDisk)
    return {false, "Invalid sample energy mode or bounded temporary storage quota"};
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
  const bool explicitRawHeadOverCap =
      !request.retainRawModelOutput &&
      request.rawHeadBudgetBytes > kDetectionExplicitRawHeadMaxBytes;
  const bool retainedRawHeadOverDisk =
      diskLimit && request.retainRawModelOutput &&
      request.rawHeadBudgetBytes > *diskLimit;
  const bool evidenceOverDisk =
      diskLimit && request.evidenceBudgetBytes > *diskLimit;
  if (request.workingSetBudgetBytes < kDetectionWorkingSetMinBytes ||
      request.workingSetBudgetBytes > detectionWorkingSetLimitBytes() ||
      evidenceOverDisk || explicitRawHeadOverCap || retainedRawHeadOverDisk)
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
DetectionValidation validateDetectionRun(const DetectionRun& run,
                                          const std::string& storagePath) {
  if (const auto valid = validateDetectionRequest(run.request, storagePath); !valid)
    return valid;
  if (run.performanceJson.size() > 65536)
    return {false, "Detection performance diagnostics exceed 64 KiB"};
  if (run.completedWindows > run.eligibleWindows ||
      run.refinedCandidates > run.totalRefinementCandidates ||
      run.totalRefinementCandidates > annotationSegmentBudget ||
      run.chunks.size() > 100000 || run.coverage.size() > 100000 ||
      run.resultCount > 100000)
    return {false, "Detection run counts exceed their coverage/budget"};
  if (run.computationPhase != "none" && run.computationPhase != "preflight" &&
      run.computationPhase != "reading" &&
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

namespace {
std::string formatDecisionMessage(const char* phase, const char* resource,
                                  const char* unit, std::uint64_t needed,
                                  std::uint64_t available, std::uint64_t reserve,
                                  std::uint64_t granted,
                                  const std::string& fallback) {
  return std::string("phase=") + phase + " resource=" + resource +
         " unit=" + unit + " needed=" + std::to_string(needed) +
         " available=" + std::to_string(available) +
         " reserve=" + std::to_string(reserve) +
         " granted=" + std::to_string(granted) + " fallback=" + fallback;
}
}  // namespace

bool checkedMulU64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    out = 0;
    return false;
  }
  out = a * b;
  return true;
}

ResourceDecision decideSamplePhaseBudget(std::uint64_t authorizedRamBytes,
                                         std::uint64_t estimatedChunkResidentBytes,
                                         std::size_t plannedCells) {
  constexpr const char* kPhase = "time-refine";
  ResourceDecision decision;
  decision.phase = kPhase;
  decision.reserve = 0;
  if (authorizedRamBytes == 0) {
    decision.kind = ResourceDecisionKind::Rejected;
    decision.errorCode = "sample.invalid-context";
    decision.resource = "ram";
    decision.requested = estimatedChunkResidentBytes;
    decision.available = 0;
    decision.granted = 0;
    decision.message = formatDecisionMessage(
        kPhase, "ram", "bytes", estimatedChunkResidentBytes, 0, 0, 0,
        "reject sample.invalid-context");
    return decision;
  }
  if (plannedCells > kSampleWorkUnitCellCeiling) {
    const auto requested = static_cast<std::uint64_t>(plannedCells);
    const auto ceiling = static_cast<std::uint64_t>(kSampleWorkUnitCellCeiling);
    decision.kind = ResourceDecisionKind::Rejected;
    decision.errorCode = "sample.work-unit-ceiling";
    decision.resource = "work-unit";
    decision.requested = requested;
    decision.available = ceiling;
    decision.granted = 0;
    decision.message = formatDecisionMessage(
        kPhase, "work-unit", "cells", requested, ceiling, 0, 0,
        "reject sample.work-unit-ceiling");
    return decision;
  }
  if (estimatedChunkResidentBytes > authorizedRamBytes) {
    decision.kind = ResourceDecisionKind::Rejected;
    decision.errorCode = "sample.chunk-exceeds-grant";
    decision.resource = "ram";
    decision.requested = estimatedChunkResidentBytes;
    decision.available = authorizedRamBytes;
    decision.granted = 0;
    decision.message = formatDecisionMessage(
        kPhase, "ram", "bytes", estimatedChunkResidentBytes, authorizedRamBytes,
        0, 0, "reject sample.chunk-exceeds-grant");
    return decision;
  }
  if (estimatedChunkResidentBytes > kSampleContiguousAllocationCapBytes) {
    decision.kind = ResourceDecisionKind::Adapted;
    decision.errorCode = "sample.grant-exceeds-contiguous-cap";
    decision.resource = "ram";
    decision.requested = estimatedChunkResidentBytes;
    decision.available = authorizedRamBytes;
    decision.granted = kSampleContiguousAllocationCapBytes;
    decision.message = formatDecisionMessage(
        kPhase, "ram", "bytes", estimatedChunkResidentBytes, authorizedRamBytes,
        0, kSampleContiguousAllocationCapBytes,
        "planner must chunk; not a hard failure of the whole job");
    return decision;
  }
  decision.kind = ResourceDecisionKind::Accepted;
  decision.errorCode.clear();
  decision.resource = "ram";
  decision.requested = estimatedChunkResidentBytes;
  decision.available = authorizedRamBytes;
  decision.granted = estimatedChunkResidentBytes;
  decision.message = formatDecisionMessage(
      kPhase, "ram", "bytes", estimatedChunkResidentBytes, authorizedRamBytes, 0,
      estimatedChunkResidentBytes,
      "none; a larger authorization is a ceiling, not a single allocation");
  return decision;
}
}  // namespace signalstudio
