#include "domain/detection.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
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

// Same walk as the historical free-space query. False only when the current
// directory itself cannot be read. A missing leaf still returns its parent.
bool resolveVolumeCandidate(const std::string& path,
                            std::filesystem::path& candidate) {
  std::error_code error;
  candidate = path.empty() ? std::filesystem::current_path(error) : utf8Path(path);
  if (error) return false;
  auto absolute = std::filesystem::absolute(candidate, error);
  if (!error) candidate = std::move(absolute);
  for (int hop = 0; hop < 64; ++hop) {
    std::error_code existsError;
    if (std::filesystem::exists(candidate, existsError) && !existsError)
      return true;
    const auto parent = candidate.parent_path();
    if (parent.empty() || parent == candidate) return true;
    candidate = parent;
  }
  return true;
}

std::string asciiUpperDrive(std::string key) {
  if (!key.empty() && key[0] >= 'a' && key[0] <= 'z')
    key[0] = static_cast<char>(key[0] - 'a' + 'A');
  return key;
}

#ifdef _WIN32
std::string wideToUtf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                        static_cast<int>(wide.size()), nullptr,
                                        0, nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      out.data(), bytes, nullptr, nullptr);
  return out;
}
#endif

struct VolumeProbeRaw {
  bool ok = false;
  std::uint64_t available = 0;
  std::uint64_t total = 0;
  std::string volumeKey;
  std::string detail;
};

VolumeProbeRaw probeVolume(const std::string& path) {
  VolumeProbeRaw probed;
  std::filesystem::path candidate;
  if (!resolveVolumeCandidate(path, candidate)) {
    probed.detail = "volume-path-unavailable";
    return probed;
  }
#ifdef _WIN32
  ULARGE_INTEGER available;
  ULARGE_INTEGER total;
  if (!GetDiskFreeSpaceExW(candidate.c_str(), &available, &total, nullptr)) {
    probed.detail = "volume-free-space-unavailable";
    return probed;
  }
  probed.ok = true;
  probed.available = static_cast<std::uint64_t>(available.QuadPart);
  probed.total = static_cast<std::uint64_t>(total.QuadPart);
  probed.detail = "GetDiskFreeSpaceExW";
  wchar_t mount[1024];
  if (GetVolumePathNameW(candidate.c_str(), mount, 1024))
    probed.volumeKey = asciiUpperDrive(wideToUtf8(mount));
  if (probed.volumeKey.empty()) {
    try {
      probed.volumeKey = asciiUpperDrive(candidate.root_path().string());
    } catch (const std::exception&) {
      probed.volumeKey.clear();
    }
  }
#else
  struct statvfs status;
  if (statvfs(candidate.c_str(), &status) != 0 || status.f_frsize == 0) {
    probed.detail = "volume-free-space-unavailable";
    return probed;
  }
  probed.ok = true;
  probed.available = static_cast<std::uint64_t>(status.f_bavail) *
                     static_cast<std::uint64_t>(status.f_frsize);
  probed.total = static_cast<std::uint64_t>(status.f_blocks) *
                 static_cast<std::uint64_t>(status.f_frsize);
  probed.detail = "statvfs";
  struct stat identity;
  if (stat(candidate.c_str(), &identity) == 0)
    probed.volumeKey = std::to_string(
        static_cast<unsigned long long>(identity.st_dev));
#endif
  return probed;
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
  const auto probed = probeVolume(path);
  if (!probed.ok) return std::nullopt;
  return probed.available;
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

bool checkedAddU64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    out = 0;
    return false;
  }
  out = a + b;
  return true;
}

const char* resourceDecisionUnit(const ResourceDecision& decision) {
  return decision.resource == "work-unit" ? "cells" : "bytes";
}

namespace {
ResourceDecision byteDecision(const char* phase, const char* resource,
                              ResourceDecisionKind kind, const char* errorCode,
                              std::uint64_t needed, std::uint64_t available,
                              std::uint64_t reserve, std::uint64_t granted,
                              const std::string& fallback) {
  ResourceDecision decision;
  decision.kind = kind;
  decision.errorCode = errorCode == nullptr ? "" : errorCode;
  decision.phase = phase == nullptr ? "" : phase;
  decision.resource = resource == nullptr ? "" : resource;
  decision.requested = needed;
  decision.available = available;
  decision.reserve = reserve;
  decision.granted = granted;
  decision.message = formatDecisionMessage(
      decision.phase.c_str(), decision.resource.c_str(),
      resourceDecisionUnit(decision), needed, available, reserve, granted,
      fallback);
  return decision;
}

std::uint64_t steadyNanoseconds() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

int phaseRank(const std::string& phase) {
  if (phase == "checkpoint") return 0;
  if (phase == "refine") return 1;
  if (phase == "publishing") return 2;
  return 3;
}

struct LedgerGroup {
  std::string volumeKey;
  std::string phase;
  std::uint64_t rawHead = 0;
  std::uint64_t scratch = 0;
  std::uint64_t shards = 0;
  std::uint64_t checkpoint = 0;
  std::uint64_t projectAtomic = 0;
  std::uint64_t total = 0;
};

bool addResident(LedgerGroup& group, DiskLedgerKind kind,
                 std::uint64_t resident) {
  std::uint64_t* slot = &group.shards;
  switch (kind) {
    case DiskLedgerKind::RawHead:
      slot = &group.rawHead;
      break;
    case DiskLedgerKind::Scratch:
      slot = &group.scratch;
      break;
    case DiskLedgerKind::Shards:
      slot = &group.shards;
      break;
    case DiskLedgerKind::Checkpoint:
      slot = &group.checkpoint;
      break;
    case DiskLedgerKind::ProjectAtomic:
      slot = &group.projectAtomic;
      break;
  }
  std::uint64_t slotNext = 0;
  std::uint64_t totalNext = 0;
  if (!checkedAddU64(*slot, resident, slotNext) ||
      !checkedAddU64(group.total, resident, totalNext))
    return false;
  *slot = slotNext;
  group.total = totalNext;
  return true;
}

std::string breakdown(const LedgerGroup& group) {
  return "rawHead=" + std::to_string(group.rawHead) +
         " scratch=" + std::to_string(group.scratch) +
         " shards=" + std::to_string(group.shards) +
         " checkpoint=" + std::to_string(group.checkpoint) +
         " projectAtomic=" + std::to_string(group.projectAtomic);
}

}  // namespace

RuntimeResourceSnapshot captureRuntimeResourceSnapshot(
    const std::string& volumePath, const VramProbeInput* vram) {
  RuntimeResourceSnapshot snapshot;
  snapshot.capturedAtSteadyNs = steadyNanoseconds();
#ifdef _WIN32
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status) || status.ullTotalPhys == 0) {
    snapshot.ram.status = ResourceProbeStatus::Unavailable;
    snapshot.ram.detail = "GlobalMemoryStatusEx failed";
  } else {
    snapshot.ram.status = ResourceProbeStatus::Probed;
    snapshot.ram.totalBytes = status.ullTotalPhys;
    snapshot.ram.availableBytes = status.ullAvailPhys;
    snapshot.ram.detail = "GlobalMemoryStatusEx";
    if (snapshot.ram.availableBytes > snapshot.ram.totalBytes) {
      snapshot.ram = {};
      snapshot.ram.status = ResourceProbeStatus::Unavailable;
      snapshot.ram.detail = "ram-probe-inconsistent";
    }
  }
#else
  const auto pages = sysconf(_SC_PHYS_PAGES);
  const auto page = sysconf(_SC_PAGE_SIZE);
  std::uint64_t total = 0;
  if (pages > 0 && page > 0)
    total = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
  std::uint64_t available = 0;
  bool availableKnown = false;
  if (FILE* meminfo = std::fopen("/proc/meminfo", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), meminfo) != nullptr) {
      unsigned long long kibibytes = 0;
      if (std::sscanf(line, "MemAvailable: %llu kB", &kibibytes) == 1) {
        available = static_cast<std::uint64_t>(kibibytes) * 1024ULL;
        availableKnown = true;
        break;
      }
    }
    std::fclose(meminfo);
  }
  if (total == 0 || !availableKnown || available > total) {
    snapshot.ram.status = ResourceProbeStatus::Unavailable;
    snapshot.ram.totalBytes = 0;
    snapshot.ram.availableBytes = 0;
    snapshot.ram.detail = total == 0 ? "physical-memory-unavailable"
                                     : "MemAvailable unavailable";
  } else {
    snapshot.ram.status = ResourceProbeStatus::Probed;
    snapshot.ram.totalBytes = total;
    snapshot.ram.availableBytes = available;
    snapshot.ram.detail = "sysconf+MemAvailable";
  }
#endif
  if (vram == nullptr) {
    snapshot.vram.status = ResourceProbeStatus::NotRequested;
    snapshot.vram.detail = "not-requested";
  } else if (!vram->probed || vram->totalBytes == 0 ||
             vram->availableBytes > vram->totalBytes) {
    snapshot.vram.status = ResourceProbeStatus::Unavailable;
    if (vram->probed && vram->totalBytes != 0 &&
        vram->availableBytes > vram->totalBytes)
      snapshot.vram.detail = "vram-probe-inconsistent";
    else
      snapshot.vram.detail =
          vram->detail.empty() ? "vram-probe-failed" : vram->detail;
  } else {
    snapshot.vram.status = ResourceProbeStatus::Probed;
    snapshot.vram.totalBytes = vram->totalBytes;
    snapshot.vram.availableBytes = vram->availableBytes;
    if (!vram->deviceIdentity.empty())
      snapshot.vram.detail = vram->deviceIdentity;
    else if (!vram->detail.empty())
      snapshot.vram.detail = vram->detail;
    else
      snapshot.vram.detail = "caller-probe";
  }
  const auto volume = probeVolume(volumePath);
  snapshot.volume.path = volumePath;
  snapshot.volume.volumeKey = volume.volumeKey;
  snapshot.volume.detail = volume.detail;
  if (!volume.ok) {
    snapshot.volume.status = ResourceProbeStatus::Unavailable;
  } else {
    snapshot.volume.status = ResourceProbeStatus::Probed;
    snapshot.volume.availableBytes = volume.available;
    snapshot.volume.totalBytes = volume.total;
  }
  return snapshot;
}

ResourceDecision decideResidentAgainstProbe(const char* phase,
                                            const char* resource,
                                            std::uint64_t requestedBytes,
                                            const ResourceByteProbe& probe,
                                            std::uint64_t reserveBytes) {
  const bool ram = resource != nullptr && std::strcmp(resource, "ram") == 0;
  const bool vram = resource != nullptr && std::strcmp(resource, "vram") == 0;
  if (!ram && !vram) {
    return byteDecision(
        phase, "invalid", ResourceDecisionKind::Rejected,
        "resource.invalid-resource", requestedBytes, 0, reserveBytes, 0,
        "use decideSamplePhaseBudget for sample cells; bytes and cells are "
        "not interchangeable");
  }
  if (probe.status != ResourceProbeStatus::Probed) {
    return byteDecision(phase, resource, ResourceDecisionKind::Rejected,
                        "resource.probe-unavailable", requestedBytes, 0,
                        reserveBytes, 0,
                        probe.detail.empty() ? "probe unavailable"
                                             : probe.detail);
  }
  if (reserveBytes > probe.availableBytes) {
    return byteDecision(phase, resource, ResourceDecisionKind::Rejected,
                        "resource.reserve-exceeds-available", requestedBytes,
                        probe.availableBytes, reserveBytes, 0,
                        "reserve exceeds the probed available bytes");
  }
  const auto usable = probe.availableBytes - reserveBytes;
  if (requestedBytes > usable) {
    return byteDecision(phase, resource, ResourceDecisionKind::Rejected,
                        "resource.resident-shortfall", requestedBytes,
                        probe.availableBytes, reserveBytes, 0,
                        "requested resident bytes exceed the probed budget; "
                        "the request is not reduced");
  }
  return byteDecision(phase, resource, ResourceDecisionKind::Accepted, "",
                      requestedBytes, probe.availableBytes, reserveBytes,
                      requestedBytes, "none");
}

RawHeadDiskAssessment assessRawHeadDisk(const RawHeadDiskInput& input) {
  RawHeadDiskAssessment assessment;
  constexpr const char* kPhase = "preflight";
  if (input.legacyCombined) {
    assessment.applies = false;
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Rejected,
        "resource.legacy-combined-unchanged", 0, 0, 0, 0,
        "legacy combined evidence stays on the existing preflight branch");
    return assessment;
  }
  assessment.applies = true;
  const auto windowsText = " windows=" + std::to_string(input.windows) +
                           " perWindow=" + std::to_string(input.bytesPerWindow);
  if (!input.retainRawModelOutput && input.explicitQuotaBytes == 0) {
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Accepted, "", 0,
        input.volumeKnown ? input.volumeAvailableBytes : 0, 0, 0,
        "raw-head not retained" + windowsText);
    return assessment;
  }
  if (!input.retainRawModelOutput &&
      input.explicitQuotaBytes > kDetectionExplicitRawHeadMaxBytes) {
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Rejected,
        "resource.explicit-raw-head-cap", input.explicitQuotaBytes,
        kDetectionExplicitRawHeadMaxBytes, 0, 0,
        "explicit raw-head quota exceeds 2 GiB while retention is off" +
            windowsText);
    return assessment;
  }
  if (input.windows == 0 || input.bytesPerWindow == 0) {
    const auto available = input.retainRawModelOutput
                               ? (input.volumeKnown ? input.volumeAvailableBytes
                                                    : 0)
                               : input.explicitQuotaBytes;
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Accepted, "", 0, available, 0, 0,
        "no raw-head bytes to store" + windowsText);
    return assessment;
  }
  std::uint64_t required = 0;
  if (!checkedMulU64(input.windows, input.bytesPerWindow, required)) {
    assessment.overflow = true;
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Rejected,
        "resource.arithmetic-overflow", 0, 0, 0, 0,
        "windows x per-window raw-head bytes overflow uint64" + windowsText);
    return assessment;
  }
  assessment.requiredBytes = required;
  if (!input.retainRawModelOutput) {
    if (required > input.explicitQuotaBytes) {
      assessment.decision = byteDecision(
          kPhase, "disk", ResourceDecisionKind::Rejected,
          "resource.explicit-raw-head-quota", required,
          input.explicitQuotaBytes, 0, 0,
          "explicit raw-head quota exceeded" + windowsText);
      return assessment;
    }
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Accepted, "", required,
        input.explicitQuotaBytes, 0, required,
        "explicit raw-head quota holds the product" + windowsText);
    return assessment;
  }
  if (input.explicitQuotaBytes != 0 && required > input.explicitQuotaBytes) {
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Rejected,
        "resource.raw-head-shortfall", required, input.explicitQuotaBytes, 0,
        0, "retained raw-head exceeds the explicit quota" + windowsText);
    return assessment;
  }
  if (!input.volumeKnown) {
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Rejected,
        "resource.raw-head-shortfall", required, 0, 0, 0,
        "remaining disk bytes are unavailable" + windowsText);
    return assessment;
  }
  if (required > input.volumeAvailableBytes) {
    assessment.decision = byteDecision(
        kPhase, "disk", ResourceDecisionKind::Rejected,
        "resource.raw-head-shortfall", required, input.volumeAvailableBytes, 0,
        0, "retained raw-head exceeds the remaining disk bytes" + windowsText);
    return assessment;
  }
  assessment.decision = byteDecision(
      kPhase, "disk", ResourceDecisionKind::Accepted, "", required,
      input.volumeAvailableBytes, 0, required,
      "retained raw-head fits" + windowsText);
  return assessment;
}

bool applyAcceptedRawHeadToUsage(const RawHeadDiskAssessment& assessment,
                                 DetectionDiskUsage& usage) {
  if (!assessment.applies ||
      assessment.decision.kind != ResourceDecisionKind::Accepted)
    return false;
  if (assessment.decision.granted == 0) {
    usage.storeRawHead = false;
    usage.rawHeadBytes = 0;
    return true;
  }
  usage.storeRawHead = true;
  usage.rawHeadBytes = assessment.requiredBytes;
  return true;
}

void appendPhaseOverlapDiskLines(const DetectionDiskUsage& usage,
                                 std::vector<DiskLedgerLine>& lines) {
  const std::string& durable = usage.durableVolumeKey;
  const std::string& scratchVolume = usage.scratchVolumeKey.empty()
                                         ? usage.durableVolumeKey
                                         : usage.scratchVolumeKey;
  const auto push = [&](DiskLedgerKind kind, const char* phase,
                        const std::string& volume, std::uint64_t payload,
                        std::uint64_t existing, bool atomic,
                        std::uint64_t largest) {
    if (payload == 0) return;
    DiskLedgerLine line;
    line.kind = kind;
    line.phase = phase;
    line.volumeKey = volume;
    line.payloadBytes = payload;
    line.existingBytes = atomic ? existing : 0;
    line.atomicReplace = atomic;
    line.largestUnitBytes = largest;
    lines.push_back(std::move(line));
  };
  const auto committedCheckpoint = usage.checkpointBytes;
  if (usage.storeRawHead) {
    push(DiskLedgerKind::RawHead, "checkpoint", durable, usage.rawHeadBytes, 0,
         false, 0);
    push(DiskLedgerKind::RawHead, "refine", durable, usage.rawHeadBytes, 0,
         false, 0);
    push(DiskLedgerKind::RawHead, "publishing", durable, usage.rawHeadBytes, 0,
         false, 0);
  }
  push(DiskLedgerKind::Shards, "checkpoint", durable, usage.resultShardBytes, 0,
       false, usage.largestResultShardBytes);
  push(DiskLedgerKind::Shards, "refine", durable, usage.resultShardBytes, 0,
       false, usage.largestResultShardBytes);
  push(DiskLedgerKind::Shards, "publishing", durable, usage.resultShardBytes, 0,
       false, usage.largestResultShardBytes);
  push(DiskLedgerKind::Shards, "publishing", durable,
       usage.publicationShardBytes, 0, false,
       usage.largestPublicationShardBytes);
  push(DiskLedgerKind::Checkpoint, "checkpoint", durable, usage.checkpointBytes,
       usage.checkpointExistingBytes, true, 0);
  push(DiskLedgerKind::Checkpoint, "refine", durable, committedCheckpoint, 0,
       false, 0);
  push(DiskLedgerKind::Checkpoint, "publishing", durable, committedCheckpoint, 0,
       false, 0);
  push(DiskLedgerKind::Scratch, "refine", scratchVolume, usage.scratchBytes, 0,
       false, 0);
  push(DiskLedgerKind::ProjectAtomic, "publishing", durable, usage.projectBytes,
       usage.projectExistingBytes, true, 0);
}

DiskVolumeBudget diskBudgetFromVolumeProbe(const VolumeResourceProbe& probe) {
  DiskVolumeBudget budget;
  budget.volumeKey = probe.volumeKey;
  budget.known = probe.status == ResourceProbeStatus::Probed;
  budget.availableBytes = budget.known ? probe.availableBytes : 0;
  return budget;
}

DiskLedgerEvaluation evaluatePhaseOverlapDiskLedger(
    std::span<const DiskLedgerLine> lines,
    std::span<const DiskVolumeBudget> volumes, std::uint64_t reserveBytes) {
  DiskLedgerEvaluation evaluation;
  for (const auto& line : lines) {
    if (line.kind != DiskLedgerKind::Shards) continue;
    const bool missingSplit =
        line.largestUnitBytes == 0 &&
        line.payloadBytes > kDetectionShardFileCapBytes;
    const bool overCap = line.largestUnitBytes > kDetectionShardFileCapBytes;
    if (!missingSplit && !overCap) continue;
    const auto requested =
        overCap ? line.largestUnitBytes : line.payloadBytes;
    evaluation.decision = byteDecision(
        line.phase.c_str(), "format", ResourceDecisionKind::Rejected,
        "resource.shard-boundary", requested, kDetectionShardFileCapBytes, 0,
        0,
        missingSplit
            ? "shard total exceeds 8 MiB and the largest shard was not provided"
            : "one shard file exceeds 8 MiB");
    evaluation.peakPhase = line.phase;
    evaluation.peakVolumeKey = line.volumeKey;
    evaluation.peakBytes = requested;
    return evaluation;
  }

  std::vector<LedgerGroup> groups;
  for (const auto& line : lines) {
    std::uint64_t resident = line.payloadBytes;
    if (line.atomicReplace &&
        !checkedAddU64(line.payloadBytes, line.existingBytes, resident)) {
      evaluation.decision = byteDecision(
          line.phase.c_str(), "disk", ResourceDecisionKind::Rejected,
          "resource.arithmetic-overflow", 0, 0, reserveBytes, 0,
          "atomic file pair overflows uint64");
      evaluation.peakPhase = line.phase;
      evaluation.peakVolumeKey = line.volumeKey;
      return evaluation;
    }
    if (resident == 0) continue;
    LedgerGroup* group = nullptr;
    for (auto& candidate : groups) {
      if (candidate.volumeKey == line.volumeKey &&
          candidate.phase == line.phase) {
        group = &candidate;
        break;
      }
    }
    if (group == nullptr) {
      groups.push_back(LedgerGroup{line.volumeKey, line.phase});
      group = &groups.back();
    }
    if (!addResident(*group, line.kind, resident)) {
      evaluation.decision = byteDecision(
          line.phase.c_str(), "disk", ResourceDecisionKind::Rejected,
          "resource.arithmetic-overflow", 0, 0, reserveBytes, 0,
          "phase overlap sum overflows uint64");
      evaluation.peakPhase = line.phase;
      evaluation.peakVolumeKey = line.volumeKey;
      return evaluation;
    }
  }

  struct VolumePeak {
    LedgerGroup group;
    bool known = false;
    bool matched = false;
    std::uint64_t available = 0;
  };
  std::vector<VolumePeak> peaks;
  for (const auto& group : groups) {
    if (group.total == 0) continue;
    VolumePeak* peak = nullptr;
    for (auto& candidate : peaks) {
      if (candidate.group.volumeKey == group.volumeKey) {
        peak = &candidate;
        break;
      }
    }
    if (peak == nullptr) {
      VolumePeak created;
      created.group.volumeKey = group.volumeKey;
      for (const auto& budget : volumes) {
        if (budget.volumeKey != group.volumeKey || created.matched) continue;
        created.matched = true;
        created.known = budget.known;
        created.available = budget.known ? budget.availableBytes : 0;
      }
      peaks.push_back(std::move(created));
      peak = &peaks.back();
    }
    const bool higher = group.total > peak->group.total;
    const bool tie =
        group.total == peak->group.total &&
        (phaseRank(group.phase) < phaseRank(peak->group.phase) ||
         (phaseRank(group.phase) == phaseRank(peak->group.phase) &&
          group.phase < peak->group.phase));
    if (peak->group.phase.empty() || higher || tie) peak->group = group;
  }

  if (peaks.empty()) {
    evaluation.decision =
        byteDecision("", "disk", ResourceDecisionKind::Accepted, "", 0, 0,
                     reserveBytes, 0, "empty disk ledger");
    return evaluation;
  }

  auto decideVolume = [&](const VolumePeak& peak) {
    const auto& group = peak.group;
    if (!peak.matched || !peak.known) {
      return byteDecision(group.phase.c_str(), "disk",
                          ResourceDecisionKind::Rejected,
                          "resource.volume-unavailable", group.total, 0,
                          reserveBytes, 0,
                          std::string("volume free space is unavailable ") +
                              breakdown(group));
    }
    if (reserveBytes > peak.available) {
      return byteDecision(group.phase.c_str(), "disk",
                          ResourceDecisionKind::Rejected,
                          "resource.reserve-exceeds-available", group.total,
                          peak.available, reserveBytes, 0,
                          std::string("reserve exceeds the volume ") +
                              breakdown(group));
    }
    const auto usable = peak.available - reserveBytes;
    if (group.total > usable) {
      const bool rawHeadItself = group.rawHead > usable;
      return byteDecision(
          group.phase.c_str(), "disk", ResourceDecisionKind::Rejected,
          rawHeadItself ? "resource.raw-head-shortfall"
                        : "resource.disk-shortfall",
          group.total, peak.available, reserveBytes, 0,
          (rawHeadItself ? std::string("retained raw-head exceeds usable space ")
                         : std::string("phase overlap exceeds usable space ")) +
              breakdown(group));
    }
    return byteDecision(group.phase.c_str(), "disk",
                        ResourceDecisionKind::Accepted, "", group.total,
                        peak.available, reserveBytes, group.total,
                        std::string("none ") + breakdown(group));
  };

  std::vector<ResourceDecision> decisions;
  decisions.reserve(peaks.size());
  for (const auto& peak : peaks) decisions.push_back(decideVolume(peak));

  std::size_t selected = 0;
  bool anyRejected = false;
  for (std::size_t i = 0; i < decisions.size(); ++i) {
    if (decisions[i].kind == ResourceDecisionKind::Rejected) anyRejected = true;
  }
  for (std::size_t i = 1; i < decisions.size(); ++i) {
    const bool candidateRejected =
        decisions[i].kind == ResourceDecisionKind::Rejected;
    const bool selectedRejected =
        decisions[selected].kind == ResourceDecisionKind::Rejected;
    if (anyRejected && candidateRejected != selectedRejected) {
      if (candidateRejected) selected = i;
      continue;
    }
    if (decisions[i].requested > decisions[selected].requested ||
        (decisions[i].requested == decisions[selected].requested &&
         peaks[i].group.volumeKey < peaks[selected].group.volumeKey))
      selected = i;
  }
  evaluation.decision = decisions[selected];
  evaluation.perVolume = std::move(decisions);
  evaluation.peakPhase = peaks[selected].group.phase;
  evaluation.peakVolumeKey = peaks[selected].group.volumeKey;
  evaluation.peakBytes = peaks[selected].group.total;
  return evaluation;
}

}  // namespace signalstudio
