#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "domain/detection.h"

namespace signalstudio {

struct DetectionModelOutput {
  std::uint32_t batch = 0;
  std::uint32_t width = 0;
  std::uint32_t inputBins = 0, actualFrames = 0;
  std::vector<float> hm;
  std::vector<float> bw;
  std::vector<float> off;
};

struct DetectionModelResourceLimits {
  // Only 1, 2, 4, 8 and 16 are accepted. The installed engine may use fewer.
  std::uint32_t maximumExecutionBatch = 2;
  std::uint64_t maximumEngineBlobBytes = 128ULL * 1024 * 1024;
  std::uint64_t maximumDeviceBytes = 4ULL * 1024 * 1024 * 1024;
};
struct DetectionModelMemoryUsage {
  std::uint64_t hostWorkingSetBytes = 0;
  std::uint64_t engineBlobBytes = 0;
  std::uint64_t runtimeDeviceBytes = 0;
  std::uint64_t allocationDeviceBytes = 0;
  std::uint64_t freeDeviceBytes = 0;
  std::uint64_t totalDeviceBytes = 0;
};
struct DetectionModelRuntimeHooks {
  // Optional fault injection, used only by native runtime boundary tests.
  // Production leaves every hook empty. An empty failDeviceAllocation never
  // runs and never replaces a real CUDA allocation failure.
  std::function<bool(std::uint32_t)> failAfterQueuedOutput;
  // Return true to reject this physical batch before cudaMalloc. The failure
  // is labelled injected. Legal tiers above 1 downshift and retry the same
  // windows; batch 1 rejects the load.
  std::function<bool(std::uint32_t physicalBatch, std::uint64_t neededBytes)>
      failDeviceAllocation;
  // Return true to stop load or infer, release device resources, and leave
  // every window unattempted by this call. Not a batch downshift.
  std::function<bool()> cancelled;
};

// CPU intermediate buffers, bounded model blobs and device allocations have
// independent limits. Device allocations must also fit actual available VRAM.
bool validateDetectionModelMemory(const DetectionModelMemoryUsage& usage,
                                  std::uint64_t hostWorkingSetBudgetBytes,
                                  const DetectionModelResourceLimits& limits,
                                  QString& error);

// These checks do not load a model/runtime. All spectrum statistics are valid
// inputs. ModelDefault retains the historical preprocessing; an explicit mode
// selects the actual statistic before backend normalization and interpolation.
bool prepareDetectionModelInput(const DetectionRequest& request,
                                const std::vector<float>& sourceFrames,
                                std::uint32_t batch, std::vector<float>& input,
                                QString& error, std::uint32_t actualFrames = 0);
// packageDirectory is the profile directory itself (the folder that contains
// manifest.json), not the detection-models root.
bool validateDetectionModelPackageDirectory(const DetectionRequest& request,
                                            const QString& packageDirectory,
                                            QJsonObject& manifest,
                                            QString& error);
// FFSCN uses the active-package record under modelRoot when one has been
// published. Other profiles stay in modelRoot/<profileId>.
bool validateDetectionModelPackage(const DetectionRequest& request,
                                   const QString& modelRoot,
                                   QJsonObject& manifest, QString& error);

// Legal results are 1, 2, 4, 8, and 16. Returns 0 when profileMaximum is 0,
// when no legal tier is at or below min(requested, workerLimit, profileMaximum),
// or when known VRAM cannot hold batch 1. The caller rejects a 0 result and
// does not skip windows. freeDeviceBytes == 0 means VRAM is unknown, so the
// result is that greatest legal tier and is not shrunk for memory. When VRAM
// is known, 16 is returned only if 16 is within all three ceilings and its
// checked cost fits.
inline std::uint32_t selectDetectionExecutionBatch(
    std::uint32_t requested, std::uint32_t workerLimit,
    std::uint32_t profileMaximum, std::uint64_t bytesPerUnitBatch,
    std::uint64_t freeDeviceBytes, std::uint64_t reserveBytes) {
  if (profileMaximum == 0) return 0;
  auto ceiling = requested < workerLimit ? requested : workerLimit;
  if (profileMaximum < ceiling) ceiling = profileMaximum;
  constexpr std::uint32_t kTiers[] = {16, 8, 4, 2, 1};
  std::uint32_t tierCeiling = 0;
  for (const auto tier : kTiers) {
    if (tier <= ceiling && isDetectionExecutionBatchTier(tier)) {
      tierCeiling = tier;
      break;
    }
  }
  if (tierCeiling == 0 || freeDeviceBytes == 0) return tierCeiling;
  for (const auto tier : kTiers) {
    if (tier > tierCeiling || !isDetectionExecutionBatchTier(tier)) continue;
    const auto width = static_cast<std::uint64_t>(tier);
    if (bytesPerUnitBatch != 0 &&
        width > std::numeric_limits<std::uint64_t>::max() / bytesPerUnitBatch)
      continue;
    const auto tierCost = bytesPerUnitBatch * width;
    if (reserveBytes > std::numeric_limits<std::uint64_t>::max() - tierCost)
      continue;
    if (tierCost + reserveBytes <= freeDeviceBytes) return tier;
  }
  return 0;
}

// Worker-limit for the next load attempt. Zero means the failed attempt is
// terminal: the caller rejects the load and does not skip windows.
inline std::uint32_t nextDetectionWorkerLimitAfterResourceFailure(
    std::uint32_t attemptedBatch, bool retryableResourceFailure) {
  if (!retryableResourceFailure || attemptedBatch <= 1) return 0;
  return attemptedBatch / 2;
}

inline bool detectionExecutionNeededBytes(std::uint32_t batch,
                                         std::uint64_t bytesPerUnitBatch,
                                         std::uint64_t reserveBytes,
                                         std::uint64_t& neededBytes) {
  neededBytes = 0;
  if (batch == 0) return false;
  const auto width = static_cast<std::uint64_t>(batch);
  if (bytesPerUnitBatch != 0 &&
      width > std::numeric_limits<std::uint64_t>::max() / bytesPerUnitBatch)
    return false;
  const auto tierCost = bytesPerUnitBatch * width;
  if (reserveBytes > std::numeric_limits<std::uint64_t>::max() - tierCost)
    return false;
  neededBytes = tierCost + reserveBytes;
  return true;
}

// SCN keeps the enqueue count. FFSCN reports the physical tier it will run.
inline std::uint32_t detectionReportedLogicalBatch(bool physicalBatchFixed,
                                                  std::uint32_t enqueueBatch,
                                                  std::uint32_t physicalBatch) {
  return physicalBatchFixed ? enqueueBatch : physicalBatch;
}

inline QString detectionExecutionCancelledMessage() {
  return QStringLiteral(
      "Cancelled during model execution. Device resources are released and no "
      "window is skipped.");
}

inline QString detectionBatchOneUnfitMessage(bool overflow,
                                            std::uint64_t neededBytes,
                                            std::uint64_t freeBytes) {
  if (overflow)
    return QString(
               "Execution batch 1 does not fit available VRAM: needed bytes "
               "overflow, free %1 bytes. The load is rejected and no window "
               "is skipped.")
        .arg(QString::number(freeBytes));
  return QString(
             "Execution batch 1 does not fit available VRAM: needed %1 bytes, "
             "free %2 bytes. The load is rejected and no window is skipped.")
      .arg(QString::number(neededBytes))
      .arg(QString::number(freeBytes));
}

inline QString detectionBatchFitMessage(const char* label,
                                       std::uint32_t attemptedBatch,
                                       std::uint64_t neededBytes,
                                       std::uint64_t freeBytes,
                                       std::uint32_t nextWorkerLimit) {
  if (nextWorkerLimit == 0)
    return QString(
               "%1 at physical batch %2; needed %3 bytes, free %4 bytes. No "
               "smaller legal batch remains. The load is rejected and no "
               "window is skipped.")
        .arg(QString::fromLatin1(label))
        .arg(attemptedBatch)
        .arg(QString::number(neededBytes))
        .arg(QString::number(freeBytes));
  return QString(
             "%1 at physical batch %2; needed %3 bytes, free %4 bytes. "
             "Retrying legal batch %5 for the same windows.")
      .arg(QString::fromLatin1(label))
      .arg(attemptedBatch)
      .arg(QString::number(neededBytes))
      .arg(QString::number(freeBytes))
      .arg(nextWorkerLimit);
}

inline bool detectionAdaptationWasInjected(const QString& reason) {
  return reason.contains(QStringLiteral("Injected device allocation failure"));
}

// Raw optimization-profile dims from the engine, rank 4, not reordered.
struct DetectionProfileExtent {
  int profileIndex = -1;
  bool known = false;
  std::int64_t minimum[4]{};
  std::int64_t optimum[4]{};
  std::int64_t maximum[4]{};
};

inline bool copyDetectionProfileDims(int rank, const std::int64_t* values,
                                    std::int64_t destination[4]) {
  if (destination == nullptr || values == nullptr || rank != 4) return false;
  for (int index = 0; index < 4; ++index) destination[index] = values[index];
  return true;
}

struct DetectionDeviceAccount {
  int cudaDeviceIndex = -1;
  std::uint32_t requestedBatch = 0;
  std::uint32_t logicalBatch = 0;
  std::uint32_t physicalBatch = 0;
  std::uint64_t freeDeviceBytes = 0;
  std::uint64_t totalDeviceBytes = 0;
  std::uint64_t reservedDeviceBytes = 0;
  std::uint64_t neededDeviceBytes = 0;
  bool injected = false;
};

inline QJsonArray detectionProfileDimArray(const std::int64_t values[4]) {
  QJsonArray array;
  for (int index = 0; index < 4; ++index)
    array.append(static_cast<qint64>(values[index]));
  return array;
}

// Byte counts stay decimal strings so values above 2^53 do not lose precision.
inline void writeDetectionDeviceEvidence(QJsonObject& evidence,
                                        const DetectionDeviceAccount& account,
                                        const DetectionProfileExtent& profile) {
  evidence.insert(QStringLiteral("cudaDeviceIndex"), account.cudaDeviceIndex);
  evidence.insert(QStringLiteral("deviceIndex"), account.cudaDeviceIndex);
  evidence.insert(QStringLiteral("requestedBatch"),
                  static_cast<int>(account.requestedBatch));
  evidence.insert(QStringLiteral("requestedMaximumBatch"),
                  static_cast<int>(account.requestedBatch));
  evidence.insert(QStringLiteral("logicalBatch"),
                  static_cast<int>(account.logicalBatch));
  evidence.insert(QStringLiteral("logicalMaximumBatch"),
                  static_cast<int>(account.logicalBatch));
  evidence.insert(QStringLiteral("physicalBatch"),
                  static_cast<int>(account.physicalBatch));
  evidence.insert(QStringLiteral("freeDeviceBytes"),
                  QString::number(account.freeDeviceBytes));
  evidence.insert(QStringLiteral("totalDeviceBytes"),
                  QString::number(account.totalDeviceBytes));
  evidence.insert(QStringLiteral("reservedDeviceBytes"),
                  QString::number(account.reservedDeviceBytes));
  evidence.insert(QStringLiteral("neededDeviceBytes"),
                  QString::number(account.neededDeviceBytes));
  if (profile.known) {
    evidence.insert(QStringLiteral("optimizationProfile"), profile.profileIndex);
    evidence.insert(QStringLiteral("profileMin"),
                    detectionProfileDimArray(profile.minimum));
    evidence.insert(QStringLiteral("profileOpt"),
                    detectionProfileDimArray(profile.optimum));
    evidence.insert(QStringLiteral("profileMax"),
                    detectionProfileDimArray(profile.maximum));
  }
  if (account.injected)
    evidence.insert(QStringLiteral("allocationFailureKind"),
                    QStringLiteral("injected"));
}

struct DetectionDevicePlan {
  DetectionDeviceAccount account;
  DetectionProfileExtent profile;
  bool ok = false;
  QString error;
  QString adaptationReason;
  std::vector<std::uint32_t> attemptedBatches;
};

// Pure model of DetectionModelBackend::load plus selectDetectionExecutionBatch.
// rejectAllocation is the test allocator. Empty means the selected tier is
// accepted. freeDeviceBytes == 0 still means VRAM is unknown. This function
// does not call CUDA and must not be described as a device measurement.
inline DetectionDevicePlan planDetectionDeviceExecution(
    std::uint32_t requested, std::uint32_t workerLimit,
    std::uint32_t profileMaximum, std::uint64_t bytesPerUnitBatch,
    std::uint64_t freeDeviceBytes, std::uint64_t totalDeviceBytes,
    std::uint64_t reserveBytes, int cudaDeviceIndex,
    const DetectionProfileExtent& profile,
    const std::function<bool(std::uint32_t, std::uint64_t)>& rejectAllocation,
    const std::function<bool()>& cancelled) {
  DetectionDevicePlan plan;
  plan.profile = profile;
  plan.account.cudaDeviceIndex = cudaDeviceIndex;
  plan.account.requestedBatch = requested;
  plan.account.freeDeviceBytes = freeDeviceBytes;
  plan.account.totalDeviceBytes = totalDeviceBytes;
  plan.account.reservedDeviceBytes = reserveBytes;
  if (!isDetectionExecutionBatchTier(requested) ||
      !isDetectionExecutionBatchTier(workerLimit)) {
    plan.error = QStringLiteral("Execution batch must be 1, 2, 4, 8 or 16");
    return plan;
  }
  QString reason;
  for (auto batch = requested < workerLimit ? requested : workerLimit;
       batch > 0; batch = batch == 1 ? 0 : batch / 2) {
    if (cancelled && cancelled()) {
      plan.account.injected = false;
      plan.account.physicalBatch = 0;
      plan.account.logicalBatch = 0;
      plan.error = detectionExecutionCancelledMessage();
      plan.adaptationReason = reason;
      return plan;
    }
    const auto chosen = selectDetectionExecutionBatch(
        requested, batch, profileMaximum, bytesPerUnitBatch, freeDeviceBytes,
        reserveBytes);
    if (chosen == 0) {
      std::uint64_t needed = 0;
      const bool fits =
          detectionExecutionNeededBytes(1, bytesPerUnitBatch, reserveBytes,
                                        needed);
      plan.account.neededDeviceBytes = fits ? needed : 0;
      plan.error = detectionBatchOneUnfitMessage(!fits, needed, freeDeviceBytes);
      plan.adaptationReason = reason;
      return plan;
    }
    std::uint64_t needed = 0;
    const bool fits = detectionExecutionNeededBytes(
        chosen, bytesPerUnitBatch, reserveBytes, needed);
    plan.attemptedBatches.push_back(chosen);
    plan.account.neededDeviceBytes = fits ? needed : 0;
    const bool injected =
        fits && rejectAllocation && rejectAllocation(chosen, needed);
    if (fits && !injected) {
      plan.ok = true;
      plan.account.physicalBatch = chosen;
      plan.account.logicalBatch =
          detectionReportedLogicalBatch(false, chosen, chosen);
      plan.account.neededDeviceBytes = needed;
      plan.error.clear();
      plan.adaptationReason = reason;
      return plan;
    }
    const auto next =
        nextDetectionWorkerLimitAfterResourceFailure(chosen, chosen > 1);
    const auto message = detectionBatchFitMessage(
        injected ? "Injected device allocation failure"
                 : "Execution batch does not fit available VRAM",
        chosen, plan.account.neededDeviceBytes, freeDeviceBytes, next);
    if (!reason.isEmpty()) reason += QStringLiteral("; ");
    reason += message;
    plan.account.injected = injected;
    if (next == 0) {
      plan.account.physicalBatch = 0;
      plan.account.logicalBatch = 0;
      plan.error = reason;
      plan.adaptationReason = reason;
      return plan;
    }
    batch = chosen;
  }
  plan.error = QStringLiteral(
      "No execution batch fits the installed model resources. The load is "
      "rejected and no window is skipped.");
  plan.adaptationReason = reason;
  return plan;
}

// Models and DLLs are loaded lazily from the standalone package. Missing model
// dependencies therefore cannot prevent the explicit fixture worker starting.
class DetectionModelBackend {
 public:
  explicit DetectionModelBackend(QString modelRoot = {},
                                 DetectionModelResourceLimits limits = {},
                                 DetectionModelRuntimeHooks hooks = {});
  ~DetectionModelBackend();
  DetectionModelBackend(const DetectionModelBackend&) = delete;
  DetectionModelBackend& operator=(const DetectionModelBackend&) = delete;

  bool load(const DetectionRequest& request, QString& error);
  bool infer(const std::vector<float>& sourceFrames, std::uint32_t batch,
             DetectionModelOutput& output, QString& error,
             std::uint32_t actualFrames = 0);
  QJsonObject evidence() const;
  void reset();

 private:
  class Impl;
  QString modelRoot_;
  DetectionModelResourceLimits limits_;
  DetectionModelRuntimeHooks hooks_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace signalstudio
