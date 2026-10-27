#include "infrastructure/detection_frequency_analysis.h"
#include "infrastructure/detection_spectrum_spool.h"

#include <memory>

#include <QCryptographicHash>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <set>

#include "infrastructure/source_loader.h"

namespace signalstudio {
namespace {
constexpr std::uint64_t kReadBlockBytes = 4 * 1024 * 1024;
constexpr std::uint64_t kMaximumScratchBytes = 16 * 1024 * 1024;
constexpr std::uint64_t kMaximumSpoolBytes = 16ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaximumAccumulatedValues = 64ULL * 1024 * 1024 * 1024;

bool fail(QString& error, const QString& reason) {
  error = reason;
  return false;
}
bool stopped(const DetectionSourceCancel& cancel, QString& error) {
  if (!cancel || !cancel()) return false;
  error = "frequency-analysis-cancelled";
  return true;
}
bool validFrequency(FrequencyRange range) {
  return std::isfinite(range.lowerHz) && std::isfinite(range.upperHz) &&
         range.lowerHz < range.upperHz;
}
std::uint64_t ceilDivide(std::uint64_t value, std::uint64_t divisor) {
  return value / divisor + (value % divisor != 0);
}
std::size_t searchBin(double frequency, double first, double step,
                      std::size_t count) {
  // Equivalent to searchsorted on ascending FFT-bin centres, clipped to the
  // final real bin. Keep the exact comparisons at floating-point boundaries.
  std::size_t left = 0, right = count;
  while (left < right) {
    const auto middle = left + (right - left) / 2;
    if (first + static_cast<double>(middle) * step < frequency)
      left = middle + 1;
    else
      right = middle;
  }
  return std::min(left, count - 1);
}
FrequencyRange binEdges(std::size_t first, std::size_t last, double origin,
                        double step) {
  return {origin + static_cast<double>(first) * step - .5 * step,
          origin + static_cast<double>(last) * step + .5 * step};
}
std::string spectrumHash(std::span<const double> values) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  for (const auto value : values) {
    char bytes[8];
    qToLittleEndian(std::bit_cast<std::uint64_t>(value), bytes);
    hash.addData(QByteArrayView(bytes, 8));
  }
  return hash.result().toHex().toStdString();
}

struct FrameInterval {
  std::uint64_t first = 0, end = 0;
};
FrameInterval analysisFrames(const DetectionSourcePlan& plan) {
  if (!plan.eligibleWindowCount) return {};
  const auto first = plan.eligibleRegularWindows
                         ? plan.firstRegularWindow * plan.config.modelHopFrames
                         : plan.realFrameCount - plan.config.modelFrames;
  const auto last =
      plan.eligibleTailWindow
          ? plan.realFrameCount - plan.config.modelFrames
          : (plan.firstRegularWindow + plan.eligibleRegularWindows - 1) *
                plan.config.modelHopFrames;
  return {first, last + plan.config.modelFrames};
}
FrameInterval trackFrames(const FrequencyTrack& track,
                          const DetectionSourcePlan& plan,
                          FrameInterval available) {
  auto first = ceilDivide(track.nominalSupport.begin, plan.hopSamples);
  auto end = ceilDivide(track.nominalSupport.end, plan.hopSamples);
  first = std::clamp(first, available.first, available.end - 1);
  end = std::clamp(end, available.first + 1, available.end);
  if (end <= first) {
    // The reference clips searchsorted indices then exchanges reversed bounds.
    // Retain that inclusive-index fallback without inventing emission timing.
    const auto last = end - 1;
    end = first + 1;
    first = last;
  }
  return {first, end};
}

bool validateRequest(const DetectionSourcePlan& plan,
                     const DetectionRequest& request,
                     const std::vector<FrequencyTrack>& tracks,
                     QString& error) {
  const auto& a = plan.sourceIdentity;
  const auto& b = request.sourceIdentity;
  if (a.fileId != b.fileId || a.path != b.path ||
      a.projectGeneration != b.projectGeneration ||
      a.formatRevision != b.formatRevision ||
      a.selectedSampleChannel != b.selectedSampleChannel ||
      (!b.quickFingerprint.empty() &&
       a.quickFingerprint != b.quickFingerprint) ||
      a.sampleFormat != b.sampleFormat || a.sampleRateHz != b.sampleRateHz ||
      a.centerFrequencyHz != b.centerFrequencyHz ||
      a.physicalSamples != b.physicalSamples ||
      a.availableSamples != b.availableSamples ||
      plan.requestedRange != request.requestedRange ||
      plan.config.fftSize != request.profile.fftSize ||
      plan.config.modelFrames != request.profile.modelFrames ||
      plan.config.modelHopFrames != request.profile.modelHopFrames ||
      plan.config.modelHopFrames > plan.config.modelFrames ||
      !plan.hopSamples || (!plan.eligibleWindowCount && !tracks.empty()) ||
      !std::isfinite(request.parameters.bandwidthMarginDb) ||
      request.parameters.bandwidthMarginDb < 0 || tracks.size() > 32768)
    return fail(error, "frequency-analysis-request-source-mismatch");
  // Extra scratch is in addition to the source's existing bounded estimate.
  const auto scratch = kReadBlockBytes +
                       static_cast<std::uint64_t>(plan.config.fftSize) *
                           (4 * sizeof(double) + 3 * sizeof(float)) +
                       tracks.size() * (sizeof(DetectionFrequencyEvidence) +
                                        sizeof(FrequencyRange) + 1024);
  if (scratch > kMaximumScratchBytes ||
      scratch > request.workingSetBudgetBytes ||
      plan.estimatedPeakBytes > request.workingSetBudgetBytes - scratch ||
      tracks.size() * (sizeof(DetectionFrequencyEvidence) + 1024) >
          request.evidenceBudgetBytes)
    return fail(error, "frequency-analysis-resident-budget-exceeded");
  std::set<std::string> ids;
  for (const auto& track : tracks) {
    if (track.seedId.empty() || track.seedId.size() > 256 ||
        track.runId != request.runId || track.fileId != b.fileId ||
        !validFrequency(track.frequency) ||
        track.nominalSupport.begin >= track.nominalSupport.end ||
        track.nominalSupport.end > b.physicalSamples ||
        !ids.insert(track.seedId).second)
      return fail(error, "frequency-analysis-invalid-track-scope-or-support");
  }
  return true;
}
}  // namespace

bool refineGuiFrequencySpectrum(std::span<const double> meanDb,
                                FrequencyRange original, double firstBinHz,
                                double binWidthHz, double noiseFloorDb,
                                double marginDb,
                                DetectionFrequencyEvidence& output,
                                QString& error,
                                const DetectionSourceCancel& cancel) {
  if (meanDb.empty() || meanDb.size() > 262144 || !validFrequency(original) ||
      !std::isfinite(firstBinHz) || !std::isfinite(binWidthHz) ||
      binWidthHz <= 0 || !std::isfinite(noiseFloorDb) ||
      !std::isfinite(marginDb) || marginDb < 0 ||
      !std::isfinite(noiseFloorDb + marginDb))
    return fail(error, "invalid-gui-frequency-refinement-input");
  if (stopped(cancel, error)) return false;
  DetectionFrequencyEvidence result;
  result.method = "gui-global-percentile-10-mean-db-v1";
  result.originalFrequency = original;
  if (marginDb == 0) {
    result.frequency = original;
    result.skippedReason = "nonpositive-margin-disables-gui-refinement";
    output = std::move(result);
    error.clear();
    return true;
  }
  result.hasNoiseFloor = true;
  result.hasMeasuredBins = result.hasPeak = true;
  result.noiseFloorDb = noiseFloorDb;
  result.thresholdDb = noiseFloorDb + marginDb;
  result.originalFirstBin =
      searchBin(original.lowerHz, firstBinHz, binWidthHz, meanDb.size());
  result.originalLastBin =
      searchBin(original.upperHz, firstBinHz, binWidthHz, meanDb.size());
  result.measuredFirstBin = result.originalFirstBin;
  result.measuredLastBin = result.originalLastBin;
  auto left = result.originalFirstBin, right = result.originalLastBin;
  auto peak = left;
  for (auto bin = left; bin <= right; ++bin) {
    if (stopped(cancel, error)) return false;
    if (!std::isfinite(meanDb[bin]))
      return fail(error, "gui-frequency-spectrum-nonfinite");
    if (meanDb[bin] > meanDb[peak]) peak = bin;
  }
  result.peakDb = meanDb[peak];
  if (result.peakDb <= result.thresholdDb) {
    result.skippedReason = "peak-does-not-strictly-exceed-threshold";
  } else {
    auto measuredLeft = peak, measuredRight = peak;
    while (measuredLeft > left && meanDb[measuredLeft - 1] > result.thresholdDb)
      --measuredLeft;
    while (measuredRight < right &&
           meanDb[measuredRight + 1] > result.thresholdDb)
      ++measuredRight;
    result.measuredFirstBin = measuredLeft;
    result.measuredLastBin = measuredRight;
    const auto width = right - left + 1;
    const auto measuredWidth = measuredRight - measuredLeft + 1;
    if (static_cast<double>(measuredWidth) < .5 * width) {
      const auto conservativeWidth = std::max(
          static_cast<std::size_t>(std::ceil(.1 * width)), measuredWidth);
      const double center = .5 * (measuredLeft + measuredRight);
      const double proposedLeft =
          std::floor(center - .5 * (conservativeWidth - 1));
      left = proposedLeft <= static_cast<double>(left)
                 ? left
                 : static_cast<std::size_t>(proposedLeft);
      right = std::min(right, left + conservativeWidth - 1);
      left = std::max(result.originalFirstBin, right - conservativeWidth + 1);
      result.shrunk = true;
    } else {
      result.skippedReason = "measured-width-at-least-half-original";
    }
  }
  result.frequency = binEdges(left, right, firstBinHz, binWidthHz);
  result.applied = result.frequency != original;
  if (stopped(cancel, error)) return false;
  output = std::move(result);
  error.clear();
  return true;
}

bool applyDetectionFrequencyAnalysis(
    FixedDetectionSource& source, const DetectionRequest& request,
    std::vector<FrequencyTrack>& tracks,
    std::vector<DetectionFrequencyEvidence>& output, QString& error,
    const DetectionSourceCancel& cancel) {
  try {
    if (stopped(cancel, error)) return false;
    const auto& plan = source.plan();
    if (!validateRequest(plan, request, tracks, error)) return false;
    if (tracks.empty()) {
      output.clear();
      error.clear();
      return true;
    }
    const auto globalFrames = analysisFrames(plan);
    std::vector<DetectionFrequencyEvidence> results;
    results.reserve(tracks.size());
    std::uint64_t accumulatedValues = 0;
    const bool scn = request.profile.backend == DetectionBackendKind::Scn;
    if (!scn && request.parameters.bandwidthMarginDb == 0) {
      source.clearFrequencySpool();
      for (const auto& track : tracks) {
        if (stopped(cancel, error)) return false;
        DetectionFrequencyEvidence result;
        result.seedId = track.seedId;
        result.method = "gui-global-percentile-10-mean-db-v1";
        result.originalFrequency = result.frequency = track.frequency;
        result.skippedReason = "nonpositive-margin-disables-gui-refinement";
        results.push_back(std::move(result));
      }
      if (stopped(cancel, error)) return false;
      output = std::move(results);
      error.clear();
      return true;
    }
    std::unique_ptr<SpectrumSpool> spool;
    double globalFloor = 0;
    if (!scn) {
      // Drop any spool from an unfinished attempt before publishing a new one.
      source.clearFrequencySpool();
      const auto budget =
          std::min(kMaximumSpoolBytes,
                   request.workingSetBudgetBytes > kMaximumSpoolBytes / 64
                       ? kMaximumSpoolBytes
                       : request.workingSetBudgetBytes * 64);
      spool = std::make_unique<SpectrumSpool>();
      if (!spool->create(source, globalFrames.first, globalFrames.end, budget,
                         error, cancel) ||
          !spool->percentile(globalFloor, error, cancel))
        return false;
    } else {
      source.clearFrequencySpool();
    }
    std::vector<FrequencyRange> occupancy;
    if (scn) {
      occupancy.reserve(tracks.size());
      for (const auto& track : tracks) occupancy.push_back(track.frequency);
    }
    for (const auto& track : tracks) {
      if (stopped(cancel, error)) return false;
      const auto frames = trackFrames(track, plan, globalFrames);
      const auto frameCount = frames.end - frames.first;
      if (frameCount >
          (kMaximumAccumulatedValues - accumulatedValues) / plan.config.fftSize)
        return fail(error, "frequency-analysis-accumulation-work-limit");
      const auto values = frameCount * plan.config.fftSize;
      accumulatedValues += values;
      DetectionFrequencyEvidence result;
      std::vector<double> meanDb;
      if (!scn) {
        if (!spool->mean(frames.first, frames.end, meanDb, error, cancel))
          return false;
        if (frameCount >= 3) {
          if (!refineGuiFrequencySpectrum(
                  meanDb, track.frequency, plan.frequencyFirstBinHz,
                  plan.frequencyStepHz, globalFloor,
                  request.parameters.bandwidthMarginDb, result, error, cancel))
            return false;
        } else {
          result.method = "gui-global-percentile-10-mean-db-v1";
          result.originalFrequency = result.frequency = track.frequency;
          result.hasNoiseFloor = true;
          result.noiseFloorDb = globalFloor;
          result.thresholdDb =
              globalFloor + request.parameters.bandwidthMarginDb;
          result.skippedReason = "fewer-than-three-real-context-frames";
        }
        result.percentileSampleCount =
            (globalFrames.end - globalFrames.first) * plan.config.fftSize;
      } else {
        meanDb.assign(plan.config.fftSize, 0);
        std::vector<float> maximumDb(plan.config.fftSize,
                                     -std::numeric_limits<float>::infinity());
        for (auto frameIndex = frames.first; frameIndex < frames.end;
             ++frameIndex) {
          if (stopped(cancel, error)) return false;
          DetectionSpectrumFrame frame;
          if (!source.readFrame(frameIndex, frame, error, cancel)) return false;
          if (frame.densityDb.size() != meanDb.size())
            return fail(error, "scn-frequency-frame-shape-mismatch");
          for (std::size_t bin = 0; bin < meanDb.size(); ++bin) {
            if (!std::isfinite(frame.densityDb[bin]))
              return fail(error, "scn-frequency-spectrum-nonfinite");
            meanDb[bin] += frame.densityDb[bin];
            maximumDb[bin] = std::max(maximumDb[bin], frame.densityDb[bin]);
          }
        }
        std::vector<float> averageDb(meanDb.size());
        for (std::size_t bin = 0; bin < meanDb.size(); ++bin) {
          meanDb[bin] /= frameCount;
          averageDb[bin] = static_cast<float>(meanDb[bin]);
        }
        std::vector<ScnFrequencyRefinement> refined;
        std::string domainError;
        const std::array band{track.frequency};
        if (!refineScnFrequencyBands(
                band, occupancy, averageDb, maximumDb, plan.frequencyFirstBinHz,
                plan.frequencyStepHz, request.parameters.bandwidthMarginDb,
                refined, domainError, cancel))
          return fail(error, QString::fromStdString(domainError));
        result.method = "scn-neighbor-minimum-mean-noise-maximum-gate-v1";
        result.originalFrequency = refined[0].originalFrequency;
        result.frequency = refined[0].frequency;
        result.applied = result.shrunk = refined[0].applied;
        result.skippedReason = refined[0].skippedReason;
        result.noiseSampleCount = refined[0].noiseSampleCount;
        result.hasNoiseFloor = result.noiseSampleCount > 0;
        result.noiseFloorDb = refined[0].noiseFloorDb;
        result.thresholdDb = refined[0].thresholdDb;
        std::vector<double> maximumAsDouble(maximumDb.begin(), maximumDb.end());
        result.maximumSpectrumSha256 = spectrumHash(maximumAsDouble);
      }
      result.seedId = track.seedId;
      result.firstFrame = frames.first;
      result.lastFrameExclusive = frames.end;
      result.frameCount = frameCount;
      result.globalFrameCount = globalFrames.end - globalFrames.first;
      result.globalFirstFrame = globalFrames.first;
      result.globalLastFrameExclusive = globalFrames.end;
      result.meanSpectrumSha256 = spectrumHash(meanDb);
      results.push_back(std::move(result));
    }
    if (stopped(cancel, error)) return false;
    if (iqSourceFingerprint(QString::fromStdString(plan.sourceIdentity.path))
            .toStdString() != plan.sourceIdentity.quickFingerprint)
      return fail(error,
                  "frequency-analysis-source-changed-before-publication");
    for (std::size_t i = 0; i < tracks.size(); ++i)
      tracks[i].frequency = results[i].frequency;
    output = std::move(results);
    if (spool) source.adoptFrequencySpool(std::move(spool));
    error.clear();
    return true;
  } catch (const std::bad_alloc&) {
    return fail(error, "frequency-analysis-allocation-failed");
  }
}
}  // namespace signalstudio
