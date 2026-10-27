#pragma once

#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <functional>
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
bool validateDetectionModelPackage(const DetectionRequest& request,
                                   const QString& modelRoot,
                                   QJsonObject& manifest, QString& error);

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
