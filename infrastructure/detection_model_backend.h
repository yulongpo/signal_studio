#pragma once

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
  std::function<bool(std::uint32_t)> failAfterQueuedOutput;
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
