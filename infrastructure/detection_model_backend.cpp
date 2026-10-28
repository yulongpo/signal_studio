#include "infrastructure/detection_model_backend.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#ifdef SS_DETECTION_TENSORRT
#include <NvInfer.h>
#include <cuda_runtime_api.h>
static_assert(NV_TENSORRT_MAJOR == 10 && NV_TENSORRT_MINOR == 11 &&
              NV_TENSORRT_PATCH == 0 && NV_TENSORRT_BUILD == 33);
#endif

namespace signalstudio {
namespace {
constexpr qint64 kMaximumEngineBytes = 128 * 1024 * 1024;
constexpr auto kTrtSha =
    "d100ec93e5abf56f8d2dafb69cde536c4f9fbb4748a5746a74b7009f6894ce67";
constexpr auto kCudaSha =
    "760c38928bbe5759f7b31ed6692599eb7ec83cedd5702e84c2b72028a89837e1";

bool fail(QString& error, const QString& message) {
  error = message;
  return false;
}

#ifdef SS_DETECTION_TENSORRT
// Device bytes for one physical batch: input tensor plus three float heads.
// False means the product does not fit in uint64.
bool unitDeviceBytes(std::uint32_t bins, std::uint32_t frames,
                     std::uint64_t& bytes) {
  if (bins != 0 && static_cast<std::uint64_t>(frames) >
                       std::numeric_limits<std::uint64_t>::max() / bins)
    return false;
  const auto inputFloats =
      static_cast<std::uint64_t>(bins) * static_cast<std::uint64_t>(frames);
  const auto headBins = static_cast<std::uint64_t>(bins / 4);
  if (headBins > std::numeric_limits<std::uint64_t>::max() / 3) return false;
  const auto headFloats = headBins * 3;
  if (inputFloats > std::numeric_limits<std::uint64_t>::max() - headFloats)
    return false;
  const auto floats = inputFloats + headFloats;
  if (floats > std::numeric_limits<std::uint64_t>::max() / sizeof(float))
    return false;
  bytes = floats * sizeof(float);
  return true;
}
#endif

QString fileHash(const QString& path, qint64 maximum, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > maximum) {
    fail(error,
         "Model package file is missing, empty or exceeds its budget: " + path);
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) {
    fail(error, "Cannot hash model package file: " + path);
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

void interpolateFrequency(const float* source, std::size_t bins, float* target,
                          std::size_t modelBins) {
  if (bins == modelBins) {
    std::copy_n(source, bins, target);
    return;
  }
  const double scale = static_cast<double>(bins) / modelBins;
  for (std::size_t bin = 0; bin < modelBins; ++bin) {
    const double position = std::min(bin * scale, double(bins - 1));
    const auto left = static_cast<std::size_t>(position);
    const auto right = std::min(left + 1, bins - 1);
    target[bin] = static_cast<float>(source[left] +
                                     (source[right] - double(source[left])) *
                                         (position - left));
  }
}
}  // namespace

bool validateDetectionModelMemory(const DetectionModelMemoryUsage& usage,
                                  std::uint64_t hostWorkingSetBudgetBytes,
                                  const DetectionModelResourceLimits& limits,
                                  QString& error) {
  if (!hostWorkingSetBudgetBytes ||
      usage.hostWorkingSetBytes > hostWorkingSetBudgetBytes)
    return fail(error,
                "CPU model intermediate working set exceeds its budget: " +
                    QString::number(usage.hostWorkingSetBytes) + "/" +
                    QString::number(hostWorkingSetBudgetBytes));
  if (!limits.maximumEngineBlobBytes ||
      usage.engineBlobBytes > limits.maximumEngineBlobBytes)
    return fail(error,
                "Model engine blob exceeds its independent asset budget");
  if (!usage.totalDeviceBytes || usage.freeDeviceBytes > usage.totalDeviceBytes)
    return fail(error, "CUDA reported invalid available VRAM");
  if (usage.allocationDeviceBytes > usage.freeDeviceBytes)
    return fail(error, "Model CUDA allocations exceed actual available VRAM: " +
                           QString::number(usage.allocationDeviceBytes) + "/" +
                           QString::number(usage.freeDeviceBytes));
  if (!limits.maximumDeviceBytes ||
      usage.runtimeDeviceBytes > limits.maximumDeviceBytes ||
      usage.allocationDeviceBytes >
          limits.maximumDeviceBytes - usage.runtimeDeviceBytes)
    return fail(error,
                "Model GPU memory exceeds its independent device budget: " +
                    QString::number(usage.runtimeDeviceBytes) + "+" +
                    QString::number(usage.allocationDeviceBytes) + "/" +
                    QString::number(limits.maximumDeviceBytes));
  return true;
}

bool prepareDetectionModelInput(const DetectionRequest& request,
                                const std::vector<float>& sourceFrames,
                                std::uint32_t batch, std::vector<float>& input,
                                QString& error, std::uint32_t actualFrames) {
  const bool scn = request.profile.backend == DetectionBackendKind::Scn;
  if ((request.profile.id != "ffscn-gui-iq-v1" &&
       request.profile.id != "scn-max-psd-v1") ||
      batch == 0 || batch > 16 || batch > request.parameters.batchSize)
    return fail(error, "Model profile or batch is unsupported");
  const auto* registered = findDetectionProfile(request.profile.id);
  if (!registered || request.profile.version != registered->version ||
      request.profile.backend != registered->backend ||
      request.profile.maximumPsdOnly != registered->maximumPsdOnly)
    return fail(error,
                "Model tensor profile differs from the registered shape");
  const std::size_t bins = request.profile.fftSize;
  const std::size_t frames =
      actualFrames ? actualFrames : request.profile.modelFrames;
  const std::size_t modelBins = detectionModelInputBins(request);
  if (!bins || bins > 262144 || !frames ||
      frames > request.profile.modelFrames || modelBins > 262144 ||
      modelBins < detectionModelMinimumBins(request) ||
      (modelBins & (modelBins - 1)) != 0)
    return fail(error, "Analysis input dimensions are invalid");
  const auto expected = static_cast<std::size_t>(batch) * bins * frames;
  const auto outputElements = static_cast<std::size_t>(batch) * modelBins *
                              detectionModelTensorFrames(request, frames);
  const auto temporaryElements =
      scn || request.spectrumMode != DetectionSpectrumMode::ModelDefault ? bins
                                                                         : 0;
  if (sourceFrames.size() != expected ||
      (expected + outputElements + temporaryElements) * sizeof(float) >
          request.workingSetBudgetBytes)
    return fail(error, "Model input shape or working-set budget is invalid");
  if (!std::all_of(sourceFrames.begin(), sourceFrames.end(),
                   [](float value) { return std::isfinite(value); }))
    return fail(error, "Model input contains a non-finite spectrum value");
  std::vector<float> prepared(outputElements, 0);
  if (!scn && request.spectrumMode == DetectionSpectrumMode::ModelDefault) {
    // Interpolation covers the original physical band. It does not add real
    // frequency resolution or fabricate temporal observations.
    for (std::size_t row = 0; row < batch * frames; ++row)
      interpolateFrequency(sourceFrames.data() + row * bins, bins,
                           prepared.data() + row * modelBins, modelBins);
  } else {
    for (std::size_t item = 0; item < batch; ++item) {
      const auto source = item * frames * bins;
      const auto target = item * modelBins;
      std::vector<float> selected;
      std::string issue;
      if (!selectDetectionSpectrum(std::span<const float>(sourceFrames)
                                       .subspan(source, frames * bins),
                                   bins, request.spectrumMode, selected, issue))
        return fail(error, QString::fromStdString(issue));
      if (!scn) {
        double mean = 0, square = 0;
        std::size_t count = 0;
        for (const auto value : selected) {
          const double delta = value - mean;
          mean += delta / ++count;
          square += delta * (value - mean);
        }
        const double deviation =
            std::max(std::sqrt(std::max(
                         0.0, square / std::max<std::size_t>(1, count - 1))),
                     double(std::numeric_limits<float>::epsilon()));
        for (auto& value : selected)
          value = static_cast<float>((value - mean) / deviation);
        interpolateFrequency(selected.data(), bins, prepared.data() + target,
                             modelBins);
        continue;
      }
      if (modelBins == bins) {
        std::copy(selected.begin(), selected.end(), prepared.begin() + target);
      } else {
        // Integrate linear power over each destination cell. Subtracting a
        // common dB reference prevents overflow without changing normalization.
        const double reference =
            *std::max_element(selected.begin(), selected.end());
        const double scale = static_cast<double>(bins) / modelBins;
        for (std::size_t bin = 0; bin < modelBins; ++bin) {
          const double left = bin * scale, right = (bin + 1) * scale;
          double power = 0;
          for (auto index = static_cast<std::size_t>(left);
               index <
               std::min(bins, static_cast<std::size_t>(std::ceil(right)));
               ++index) {
            const double overlap =
                std::max(0.0, std::min(right, double(index + 1)) -
                                  std::max(left, double(index)));
            power +=
                overlap * std::pow(10.0, (selected[index] - reference) / 10);
          }
          prepared[target + bin] = static_cast<float>(
              reference + 10 * std::log10(std::max(power / scale, 1e-300)));
        }
      }
      const auto begin = prepared.begin() + target;
      const auto [lower, upper] = std::minmax_element(begin, begin + modelBins);
      const float minimum = *lower;
      const float span =
          std::max(*upper - minimum, std::numeric_limits<float>::epsilon());
      if (!std::isfinite(span))
        return fail(error, "SCN selected-spectrum minmax span is not finite");
      for (std::size_t bin = 0; bin < modelBins; ++bin)
        prepared[target + bin] = (prepared[target + bin] - minimum) / span;
    }
  }
  input = std::move(prepared);
  return true;
}

class DetectionModelBackend::Impl {
 public:
  DetectionRequest request;
  QJsonObject manifest;
  QJsonObject evidence;
  DetectionModelResourceLimits limits;
  DetectionModelRuntimeHooks hooks;
  bool retryableResourceFailure = false;
  bool injectedAllocationFailure = false;
  std::uint32_t attemptedExecutionBatch = 0;
  std::uint64_t neededDeviceBytes = 0;
  std::uint64_t accountedFreeDeviceBytes = 0;
  std::uint64_t accountedTotalDeviceBytes = 0;
  std::uint64_t accountedReservedDeviceBytes = 0;

  bool stopIfCancelled(QString& error) {
    if (!hooks.cancelled || !hooks.cancelled()) return false;
    retryableResourceFailure = false;
    injectedAllocationFailure = false;
    return fail(error, detectionExecutionCancelledMessage());
  }
#ifdef SS_DETECTION_TENSORRT
  class Logger final : public nvinfer1::ILogger {
   public:
    void log(Severity severity, const char* message) noexcept override {
      if (severity <= Severity::kWARNING) {
        const auto diagnostic =
            QString::fromUtf8(message, qstrnlen(message, 4096));
        const std::lock_guard lock(mutex_);
        lastError_ = diagnostic;
      }
    }
    QString lastError() const {
      const std::lock_guard lock(mutex_);
      return lastError_;
    }

   private:
    mutable std::mutex mutex_;
    QString lastError_;
  } logger;
  QLibrary cuda;
  QLibrary trt;
  decltype(&cudaGetDeviceCount) getDeviceCount = nullptr;
  decltype(&cudaSetDevice) setDevice = nullptr;
  decltype(&cudaGetDeviceProperties) getDeviceProperties = nullptr;
  decltype(&cudaRuntimeGetVersion) runtimeVersion = nullptr;
  decltype(&cudaDriverGetVersion) driverVersion = nullptr;
  decltype(&cudaMalloc) allocate = nullptr;
  decltype(&cudaFree) release = nullptr;
  decltype(&cudaMemcpyAsync) copy = nullptr;
  decltype(&cudaStreamCreate) createStream = nullptr;
  decltype(&cudaStreamDestroy) destroyStream = nullptr;
  decltype(&cudaStreamSynchronize) synchronize = nullptr;
  decltype(&cudaMemGetInfo) memoryInfo = nullptr;
  decltype(&cudaGetErrorString) errorString = nullptr;
  decltype(&cudaGetLastError) lastError = nullptr;
  int boundDevice = -1;
  bool deviceBound = false;
  decltype(&cudaEventCreate) createEvent = nullptr;
  decltype(&cudaEventDestroy) destroyEvent = nullptr;
  decltype(&cudaEventRecord) recordEvent = nullptr;
  decltype(&cudaEventElapsedTime) elapsedEvent = nullptr;
  std::unique_ptr<nvinfer1::IRuntime> runtime;
  std::unique_ptr<nvinfer1::ICudaEngine> engine;
  std::unique_ptr<nvinfer1::IExecutionContext> context;
  cudaStream_t stream = nullptr;
  std::array<void*, 5> memory{};
  std::array<QString, 3> headNames;
  QString inputName;
  std::uint32_t maximumBatch = 2;
  std::uint32_t logicalMaximumBatch = 2;
  std::uint32_t modelBins = 0, maximumFrames = 0;
  std::size_t workspaceBytes = 0;
  std::uint64_t streamDrainCount = 0;
  std::array<cudaEvent_t,64> timingEvents{};
  std::uint64_t physicalEnqueues = 0;

  class StreamDrain {
   public:
    explicit StreamDrain(Impl& owner) : owner_(owner) {}
    ~StreamDrain() {
      // This scope is destroyed before any host buffers used by asynchronous
      // transfers. Preserve the original failure; cleanup cannot overwrite it.
      // A sticky CUDA error would make the drain return without waiting.
      if (!completed_) {
        if (owner_.lastError) owner_.lastError();
        if (owner_.synchronize && owner_.stream)
          owner_.synchronize(owner_.stream);
        ++owner_.streamDrainCount;
      }
    }
    bool complete(QString& error) {
      const auto status = owner_.synchronize(owner_.stream);
      ++owner_.streamDrainCount;
      completed_ = status == cudaSuccess;
      return owner_.checkCuda(status, "cudaStreamSynchronize", error);
    }

   private:
    Impl& owner_;
    bool completed_ = false;
  };

  void releaseCudaResources() {
    // Clear a sticky allocation error first. Otherwise cudaFree/synchronize
    // can return that error and leave the earlier buffers allocated. Bind the
    // device that owns the buffers, destroy the context that aliases the
    // workspace, then free every pointer. Idempotent.
    if (lastError) lastError();
    if (deviceBound && setDevice && boundDevice >= 0) {
      setDevice(boundDevice);
      if (lastError) lastError();
    }
    if (stream && synchronize) {
      synchronize(stream);
      if (lastError) lastError();
    }
    for (auto& event : timingEvents) {
      if (event && destroyEvent) destroyEvent(event);
      event = nullptr;
    }
    context.reset();
    engine.reset();
    runtime.reset();
    if (release) {
      for (auto& value : memory) {
        if (!value) continue;
        release(value);
        value = nullptr;
        if (lastError) lastError();
      }
    }
    if (stream && destroyStream) {
      destroyStream(stream);
      stream = nullptr;
      if (lastError) lastError();
    }
  }

  ~Impl() { releaseCudaResources(); }

  bool checkCuda(cudaError_t status, const char* operation, QString& error) {
    if (status == cudaSuccess) return true;
    if (status == cudaErrorMemoryAllocation) retryableResourceFailure = true;
    if (lastError) lastError();
    return fail(error, QString::fromLatin1(operation) + ": " +
                           QString::fromUtf8(errorString(status)));
  }

  bool contiguousTensor(const QByteArray& name, const nvinfer1::Dims& expected,
                        nvinfer1::TensorIOMode mode, QString& error) {
    const auto shape = context->getTensorShape(name.constData());
    const auto strides = context->getTensorStrides(name.constData());
    if (shape.nbDims != 4 || strides.nbDims != 4 ||
        engine->getTensorIOMode(name.constData()) != mode ||
        engine->getTensorDataType(name.constData()) !=
            nvinfer1::DataType::kFLOAT ||
        engine->getTensorFormat(name.constData()) !=
            nvinfer1::TensorFormat::kLINEAR ||
        engine->getTensorLocation(name.constData()) !=
            nvinfer1::TensorLocation::kDEVICE)
      return fail(error,
                  "Engine tensor type/layout/location is invalid: " + name);
    std::int64_t stride = 1;
    for (int dimension = 3; dimension >= 0; --dimension) {
      if (shape.d[dimension] != expected.d[dimension] ||
          strides.d[dimension] != stride)
        return fail(error,
                    "Engine tensor shape/strides are not contiguous: " + name);
      stride *= expected.d[dimension];
    }
    return true;
  }

  template <typename Function>
  bool resolveCuda(Function& destination, const char* name, QString& error) {
    destination = reinterpret_cast<Function>(cuda.resolve(name));
    return destination ||
           fail(error, "Required CUDA runtime entry point is unavailable: " +
                           QString::fromLatin1(name));
  }

  bool initialize(const QString& runtimeRoot, QString& error) {
    for (const auto& item : {std::pair{"cudart64_12.dll", kCudaSha},
                             std::pair{"nvinfer_10.dll", kTrtSha}}) {
      const auto hash = fileHash(QDir(runtimeRoot).filePath(item.first),
                                 512 * 1024 * 1024, error);
      if (hash.isEmpty() || hash != item.second)
        return fail(error,
                    "Packaged CUDA/TensorRT DLL is absent or modified: " +
                        QString::fromLatin1(item.first));
    }
    cuda.setFileName(QDir(runtimeRoot).filePath("cudart64_12.dll"));
    trt.setFileName(QDir(runtimeRoot).filePath("nvinfer_10.dll"));
    if (!cuda.load() || !trt.load())
      return fail(error, "Cannot load packaged CUDA/TensorRT runtime: " +
                             cuda.errorString() + "; " + trt.errorString());
#define SS_RESOLVE_CUDA(member, name) \
  if (!resolveCuda(member, name, error)) return false
    SS_RESOLVE_CUDA(getDeviceCount, "cudaGetDeviceCount");
    SS_RESOLVE_CUDA(setDevice, "cudaSetDevice");
    SS_RESOLVE_CUDA(getDeviceProperties, "cudaGetDeviceProperties_v2");
    SS_RESOLVE_CUDA(runtimeVersion, "cudaRuntimeGetVersion");
    SS_RESOLVE_CUDA(driverVersion, "cudaDriverGetVersion");
    SS_RESOLVE_CUDA(allocate, "cudaMalloc");
    SS_RESOLVE_CUDA(release, "cudaFree");
    SS_RESOLVE_CUDA(copy, "cudaMemcpyAsync");
    SS_RESOLVE_CUDA(createStream, "cudaStreamCreate");
    SS_RESOLVE_CUDA(destroyStream, "cudaStreamDestroy");
    SS_RESOLVE_CUDA(synchronize, "cudaStreamSynchronize");
    SS_RESOLVE_CUDA(memoryInfo, "cudaMemGetInfo");
    SS_RESOLVE_CUDA(errorString, "cudaGetErrorString");
    SS_RESOLVE_CUDA(lastError, "cudaGetLastError");
    SS_RESOLVE_CUDA(createEvent, "cudaEventCreate");
    SS_RESOLVE_CUDA(destroyEvent, "cudaEventDestroy");
    SS_RESOLVE_CUDA(recordEvent, "cudaEventRecord");
    SS_RESOLVE_CUDA(elapsedEvent, "cudaEventElapsedTime");
#undef SS_RESOLVE_CUDA
    using RuntimeFactory = void* (*)(void*, int32_t);
    using VersionFunction = int32_t (*)();
    const auto factory = reinterpret_cast<RuntimeFactory>(
        trt.resolve("createInferRuntime_INTERNAL"));
    const auto version =
        reinterpret_cast<VersionFunction>(trt.resolve("getInferLibVersion"));
    if (!factory || !version || version() != NV_TENSORRT_VERSION)
      return fail(error, "TensorRT runtime differs from compiled 10.11.0.33");
    int count = 0;
    int cudaVersion = 0;
    int cudaDriver = 0;
    if (!checkCuda(getDeviceCount(&count), "cudaGetDeviceCount", error) ||
        count < 1 ||
        !checkCuda(runtimeVersion(&cudaVersion), "cudaRuntimeGetVersion",
                   error) ||
        !checkCuda(driverVersion(&cudaDriver), "cudaDriverGetVersion", error) ||
        cudaVersion != 12090 || cudaDriver < 12090)
      return fail(error,
                  "Required CUDA 12.9 runtime/driver/device is unavailable");
    int selected = -1;
    cudaDeviceProp properties{};
    for (int index = 0; index < count; ++index) {
      cudaDeviceProp candidate{};
      if (!checkCuda(getDeviceProperties(&candidate, index),
                     "cudaGetDeviceProperties", error))
        return false;
      if (candidate.major == 12 && candidate.minor == 0 &&
          QString::fromUtf8(candidate.name) ==
              manifest["runtime"].toObject()["gpu"].toString()) {
        selected = index;
        properties = candidate;
        break;
      }
    }
    if (selected < 0)
      return fail(error,
                  "Engine requires NVIDIA GeForce RTX 5060 Laptop GPU "
                  "SM 12.0; no automatic model/runtime fallback exists");
    if (!checkCuda(setDevice(selected), "cudaSetDevice", error)) return false;
    boundDevice = selected;
    deviceBound = true;
    const bool scn = request.profile.backend == DetectionBackendKind::Scn;
    logicalMaximumBatch =
        std::min(request.parameters.batchSize, limits.maximumExecutionBatch);
    attemptedExecutionBatch = logicalMaximumBatch;
    const auto physicalBatch = scn ? 1 : logicalMaximumBatch;
    modelBins = detectionModelInputBins(request);
    const auto hop =
        std::max(1.0, std::floor(request.sourceIdentity.sampleRateHz *
                                 request.profile.hopSeconds));
    const auto availableFrames = static_cast<std::uint64_t>(
        std::ceil(request.sourceIdentity.availableSamples / hop));
    maximumFrames = request.sourceIdentity.availableSamples &&
                            request.sourceIdentity.sampleRateHz > 0
                        ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                              request.profile.modelFrames,
                              std::max<std::uint64_t>(1, availableFrames)))
                        : request.profile.modelFrames;
    const auto minimumDeviceBytes =
        static_cast<std::uint64_t>(physicalBatch) *
        (static_cast<std::uint64_t>(modelBins) *
             detectionModelTensorFrames(request, maximumFrames) +
         3ULL * (modelBins / 4)) *
        sizeof(float);
    if (minimumDeviceBytes > limits.maximumDeviceBytes) {
      retryableResourceFailure = true;
      return fail(error,
                  "Model GPU input/output exceeds its independent device "
                  "budget before engine loading");
    }
    std::size_t initialFreeBytes = 0;
    std::size_t initialTotalBytes = 0;
    if (!checkCuda(memoryInfo(&initialFreeBytes, &initialTotalBytes),
                   "cudaMemGetInfo before model", error))
      return false;
    if (stopIfCancelled(error)) return false;
    const auto enginePath = manifest["engineAbsolutePath"].toString();
    QFile file(enginePath);
    if (!file.open(QIODevice::ReadOnly))
      return fail(error, "Cannot read the verified model engine");
    if (static_cast<std::uint64_t>(file.size()) > limits.maximumEngineBlobBytes)
      return fail(error,
                  "Model engine blob exceeds its independent asset budget");
    const auto engineBytes = file.readAll();
    if (QString::fromLatin1(
            QCryptographicHash::hash(engineBytes, QCryptographicHash::Sha256)
                .toHex()) != manifest["engine"].toObject()["sha256"].toString())
      return fail(error, "Model engine changed between validation and loading");
    runtime.reset(static_cast<nvinfer1::IRuntime*>(
        factory(&logger, NV_TENSORRT_VERSION)));
    if (!runtime) return fail(error, "Cannot create TensorRT runtime");
    engine.reset(runtime->deserializeCudaEngine(engineBytes.constData(),
                                                engineBytes.size()));
    if (!engine)
      return fail(error, "Cannot deserialize the verified engine: " +
                             logger.lastError());
    if (engine->getNbIOTensors() != 4)
      return fail(error,
                  "Model engine must expose exactly one input and three heads");
    context.reset(engine->createExecutionContext(
        nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED));
    if (!context) return fail(error, "Cannot create model execution context");
    inputName = manifest["input"].toObject()["name"].toString();
    const auto inputUtf8 = inputName.toUtf8();
    if (engine->getTensorIOMode(inputUtf8.constData()) !=
            nvinfer1::TensorIOMode::kINPUT ||
        engine->getTensorDataType(inputUtf8.constData()) !=
            nvinfer1::DataType::kFLOAT ||
        engine->getTensorFormat(inputUtf8.constData()) !=
            nvinfer1::TensorFormat::kLINEAR)
      return fail(error,
                  "Engine input is not the frozen contiguous float32 tensor");
    const auto fitsTier = [&](int tier) {
      const auto candidate = scn
                                 ? nvinfer1::Dims4(1, 32768, 1, 1)
                                 : nvinfer1::Dims4(tier, 1,
                                                   detectionModelTensorFrames(
                                                       request, maximumFrames),
                                                   modelBins);
      for (int profile = 0; profile < engine->getNbOptimizationProfiles();
           ++profile) {
        const auto minimum = engine->getProfileShape(
            inputUtf8.constData(), profile, nvinfer1::OptProfileSelector::kMIN);
        const auto maximum = engine->getProfileShape(
            inputUtf8.constData(), profile, nvinfer1::OptProfileSelector::kMAX);
        bool fits = minimum.nbDims == 4 && maximum.nbDims == 4;
        for (int dimension = 0; fits && dimension < 4; ++dimension)
          fits = candidate.d[dimension] >= minimum.d[dimension] &&
                 candidate.d[dimension] <= maximum.d[dimension];
        if (fits) return profile;
      }
      return -1;
    };
    QJsonArray supportedTiers;
    int profileMaximumBatch = 0;
    if (scn) {
      if (fitsTier(1) >= 0) {
        supportedTiers.append(1);
        profileMaximumBatch = 1;
      }
    } else {
      for (const int tier : {1, 2, 4, 8, 16}) {
        if (fitsTier(tier) >= 0) {
          supportedTiers.append(tier);
          profileMaximumBatch = tier;
        }
      }
    }
    const int workerLimit = static_cast<int>(logicalMaximumBatch);
    const int userRequested = static_cast<int>(request.parameters.batchSize);
    QString profileReason;
    if (!profileMaximumBatch)
      return fail(error,
                  "Installed model package has no optimization profile for "
                  "execution batch 1, 2, 4, 8 or 16 at this tensor shape");
    if (scn && userRequested > 1) {
      profileReason =
          QString("SCN physical batch is 1; user batch %1 is %2 separate "
                  "enqueueV3 calls, not one physical batch")
              .arg(userRequested)
              .arg(workerLimit);
    } else if (!scn && workerLimit > profileMaximumBatch) {
      logicalMaximumBatch = static_cast<std::uint32_t>(profileMaximumBatch);
      attemptedExecutionBatch = logicalMaximumBatch;
      profileReason =
          QString("Engine optimization profile maximum batch is %1; user batch "
                  "%2 with worker limit %3 selected physical batch %1. The "
                  "installed engine was not rebuilt.")
              .arg(profileMaximumBatch)
              .arg(userRequested)
              .arg(workerLimit);
    }
    maximumBatch = scn ? 1 : logicalMaximumBatch;
    const auto shape = scn ? nvinfer1::Dims4(maximumBatch, 32768, 1, 1)
                           : nvinfer1::Dims4(maximumBatch, 1,
                                             detectionModelTensorFrames(
                                                 request, maximumFrames),
                                             modelBins);
    const int selectedProfile = fitsTier(static_cast<int>(maximumBatch));
    if (selectedProfile < 0)
      return fail(error,
                  QString("Installed model package cannot execute the actual "
                          "tensor [%1,%2,%3,%4]")
                      .arg(shape.d[0])
                      .arg(shape.d[1])
                      .arg(shape.d[2])
                      .arg(shape.d[3]));
    if (!checkCuda(createStream(&stream), "cudaStreamCreate", error) ||
        !context->setOptimizationProfileAsync(selectedProfile, stream) ||
        !checkCuda(synchronize(stream), "profile initialization", error))
      return fail(error,
                  "Cannot select the actual model resource profile: " + error);
    if (!context->setInputShape(inputUtf8.constData(), shape))
      return fail(error,
                  "Engine does not accept the frozen maximum batch shape");
    std::int64_t workspace = context->updateDeviceMemorySizeForShapes();
    if (workspace < 0) return fail(error, "Cannot size actual model workspace");
    if (!contiguousTensor(inputUtf8, shape, nvinfer1::TensorIOMode::kINPUT,
                          error))
      return false;
    int activeProfile = selectedProfile;
    std::uint32_t boundBatch = maximumBatch;
    auto bindPhysicalBatch = [&](std::uint32_t batch,
                                 std::int64_t& workspaceOut) {
      const auto bound =
          scn ? nvinfer1::Dims4(batch, 32768, 1, 1)
              : nvinfer1::Dims4(batch, 1,
                                detectionModelTensorFrames(request,
                                                           maximumFrames),
                                modelBins);
      const int profile = fitsTier(static_cast<int>(batch));
      if (profile < 0)
        return fail(error,
                    QString("Installed model package cannot execute the actual "
                            "tensor [%1,%2,%3,%4]")
                        .arg(bound.d[0])
                        .arg(bound.d[1])
                        .arg(bound.d[2])
                        .arg(bound.d[3]));
      if (profile != activeProfile &&
          (!context->setOptimizationProfileAsync(profile, stream) ||
           !checkCuda(synchronize(stream), "profile initialization", error)))
        return fail(error,
                    "Cannot select the actual model resource profile: " +
                        error);
      if (profile != activeProfile) activeProfile = profile;
      if (!context->setInputShape(inputUtf8.constData(), bound))
        return fail(error,
                    "Engine does not accept the frozen maximum batch shape");
      workspaceOut = context->updateDeviceMemorySizeForShapes();
      if (workspaceOut < 0)
        return fail(error, "Cannot size actual model workspace");
      return contiguousTensor(inputUtf8, bound,
                              nvinfer1::TensorIOMode::kINPUT, error);
    };
    std::size_t freeBytes = 0;
    std::size_t totalBytes = 0;
    if (!checkCuda(memoryInfo(&freeBytes, &totalBytes), "cudaMemGetInfo",
                   error))
      return false;
    std::uint64_t bytesPerUnitBatch = 0;
    if (!unitDeviceBytes(modelBins,
                         detectionModelTensorFrames(request, maximumFrames),
                         bytesPerUnitBatch)) {
      retryableResourceFailure = false;
      return fail(error, detectionBatchOneUnfitMessage(true, 0, freeBytes));
    }
    const auto positiveBatch = [](int value) -> std::uint32_t {
      return value > 0 ? static_cast<std::uint32_t>(value) : 0;
    };
    const auto selectBatch = [&](std::uint64_t reserve) {
      return selectDetectionExecutionBatch(
          positiveBatch(userRequested), positiveBatch(workerLimit),
          positiveBatch(profileMaximumBatch), bytesPerUnitBatch,
          static_cast<std::uint64_t>(freeBytes), reserve);
    };
    const auto failUnfit = [&](std::uint64_t reserve) {
      std::uint64_t needed = 0;
      const bool fits = detectionExecutionNeededBytes(
          1, bytesPerUnitBatch, reserve, needed);
      return fail(error, detectionBatchOneUnfitMessage(!fits, needed,
                                                       freeBytes));
    };
    // Profile maximum is known, and this cudaMemGetInfo call succeeded.
    // Workspace of the profile-clamped shape is the fixed reserve. A smaller
    // tier is measured on its own shape before batch 1 is rejected.
    std::uint64_t reserveBytes = static_cast<std::uint64_t>(workspace);
    auto vramTier = selectBatch(reserveBytes);
    if (vramTier == 0 && maximumBatch > 1) {
      std::int64_t oneWorkspace = 0;
      if (!bindPhysicalBatch(1, oneWorkspace)) return false;
      boundBatch = 1;
      workspace = oneWorkspace;
      reserveBytes = static_cast<std::uint64_t>(oneWorkspace);
      vramTier = selectBatch(reserveBytes);
    }
    if (vramTier == 0) {
      // Batch 1 does not fit. A smaller worker limit cannot make a legal tier
      // below 1, so this is a hard load failure rather than another downgrade.
      retryableResourceFailure = false;
      return failUnfit(reserveBytes);
    }
    std::uint32_t chosen = 0;
    for (int index = supportedTiers.size() - 1; index >= 0; --index) {
      const auto tier =
          static_cast<std::uint32_t>(supportedTiers.at(index).toInt());
      if (tier <= vramTier && isDetectionExecutionBatchTier(tier)) {
        chosen = tier;
        break;
      }
    }
    if (chosen == 0) {
      retryableResourceFailure = false;
      const auto smallest = static_cast<std::uint64_t>(
          std::max(1, supportedTiers.first().toInt()));
      const bool fits =
          (bytesPerUnitBatch == 0 ||
           smallest <= std::numeric_limits<std::uint64_t>::max() /
                           bytesPerUnitBatch) &&
          bytesPerUnitBatch * smallest <=
              std::numeric_limits<std::uint64_t>::max() - reserveBytes;
      return fail(error,
                  QString("No profile-supported execution batch fits available "
                          "VRAM: needed %1 bytes, free %2 bytes. The load is "
                          "rejected and no window is skipped.")
                      .arg(fits ? QString::number(bytesPerUnitBatch * smallest +
                                                  reserveBytes)
                                : QString("overflow"))
                      .arg(QString::number(
                          static_cast<qulonglong>(freeBytes))));
    }
    const auto previousLogical = logicalMaximumBatch;
    logicalMaximumBatch = detectionReportedLogicalBatch(
        scn, logicalMaximumBatch, chosen);
    if (!scn && logicalMaximumBatch != previousLogical) {
      if (!profileReason.isEmpty()) profileReason += "; ";
      profileReason +=
          QString("Available VRAM selected physical batch %1 (profile maximum "
                  "%2, free %3 bytes, %4 bytes/batch, reserve %5 bytes)")
              .arg(chosen)
              .arg(profileMaximumBatch)
              .arg(QString::number(static_cast<qulonglong>(freeBytes)))
              .arg(QString::number(bytesPerUnitBatch))
              .arg(QString::number(reserveBytes));
    }
    if (!scn) attemptedExecutionBatch = chosen;
    if (chosen != boundBatch && !bindPhysicalBatch(chosen, workspace))
      return false;
    maximumBatch = chosen;
    workspaceBytes = static_cast<std::size_t>(workspace);
    accountedFreeDeviceBytes = static_cast<std::uint64_t>(freeBytes);
    accountedTotalDeviceBytes = static_cast<std::uint64_t>(totalBytes);
    accountedReservedDeviceBytes = static_cast<std::uint64_t>(workspace);
    bool neededFits = false;
    {
      std::uint64_t needed = 0;
      neededFits = detectionExecutionNeededBytes(
          maximumBatch, bytesPerUnitBatch, accountedReservedDeviceBytes,
          needed);
      neededDeviceBytes = neededFits ? needed : 0;
    }
    DetectionProfileExtent profileExtent;
    profileExtent.profileIndex = activeProfile;
    {
      const auto capture = [&](nvinfer1::OptProfileSelector selector,
                               std::int64_t* destination) {
        const auto dims = engine->getProfileShape(
            inputUtf8.constData(), activeProfile, selector);
        if (dims.nbDims != 4)
          return fail(error, "Engine optimization profile shape is not rank 4");
        std::int64_t raw[4]{};
        for (int index = 0; index < 4; ++index)
          raw[index] = static_cast<std::int64_t>(dims.d[index]);
        if (!copyDetectionProfileDims(4, raw, destination))
          return fail(error, "Engine optimization profile shape is not rank 4");
        return true;
      };
      if (!capture(nvinfer1::OptProfileSelector::kMIN, profileExtent.minimum) ||
          !capture(nvinfer1::OptProfileSelector::kOPT, profileExtent.optimum) ||
          !capture(nvinfer1::OptProfileSelector::kMAX, profileExtent.maximum))
        return false;
      profileExtent.known = true;
    }
    if (stopIfCancelled(error)) return false;
    if (accountedFreeDeviceBytes != 0 &&
        (!neededFits || neededDeviceBytes > accountedFreeDeviceBytes)) {
      // Final workspace can differ from the reserve used to pick the tier.
      // Reject this shape before cudaMalloc. Batch 1 ends the load.
      attemptedExecutionBatch = maximumBatch;
      retryableResourceFailure = maximumBatch > 1;
      injectedAllocationFailure = false;
      const auto next = nextDetectionWorkerLimitAfterResourceFailure(
          maximumBatch, retryableResourceFailure);
      return fail(error, detectionBatchFitMessage(
                             "Execution batch does not fit available VRAM",
                             maximumBatch, neededDeviceBytes,
                             accountedFreeDeviceBytes, next));
    }
    if (hooks.failDeviceAllocation &&
        hooks.failDeviceAllocation(maximumBatch, neededDeviceBytes)) {
      attemptedExecutionBatch = maximumBatch;
      injectedAllocationFailure = true;
      retryableResourceFailure = true;
      const auto next = nextDetectionWorkerLimitAfterResourceFailure(
          maximumBatch, maximumBatch > 1);
      return fail(error,
                  detectionBatchFitMessage("Injected device allocation failure",
                                           maximumBatch, neededDeviceBytes,
                                           accountedFreeDeviceBytes, next));
    }
    const std::size_t inputBytes =
        static_cast<std::size_t>(maximumBatch) * modelBins *
        detectionModelTensorFrames(request, maximumFrames) * sizeof(float);
    const std::size_t headBytes = static_cast<std::size_t>(maximumBatch) *
                                  (modelBins / 4) * sizeof(float);
    const std::size_t allocationBytes = inputBytes + 3 * headBytes +
                                        static_cast<std::size_t>(workspace);
    const auto logicalBatch = static_cast<std::size_t>(logicalMaximumBatch);
    const auto sourceBytes =
        logicalBatch * request.profile.fftSize * maximumFrames * sizeof(float);
    const auto preparedBytes =
        logicalBatch * modelBins *
        detectionModelTensorFrames(request, maximumFrames) * sizeof(float);
    const auto logicalHeadBytes =
        logicalBatch * (modelBins / 4) * sizeof(float);
    // Source, prepared input and four complete hm/bw/off host copies cover
    // output, packed heads, binary payload and framed response in the worker.
    const auto temporaryBytes =
        scn || request.spectrumMode != DetectionSpectrumMode::ModelDefault
            ? request.profile.fftSize * sizeof(float)
            : 0;
    const auto hostWorkingSet =
        sourceBytes + preparedBytes +
        std::max<std::size_t>(12 * logicalHeadBytes, temporaryBytes);
    if (!checkCuda(memoryInfo(&freeBytes, &totalBytes), "cudaMemGetInfo",
                   error))
      return false;
    const auto runtimeDeviceBytes =
        initialFreeBytes > freeBytes ? initialFreeBytes - freeBytes : 0;
    const DetectionModelMemoryUsage usage{
        hostWorkingSet,     static_cast<std::uint64_t>(engineBytes.size()),
        runtimeDeviceBytes, allocationBytes,
        freeBytes,          totalBytes};
    if (!validateDetectionModelMemory(usage, request.workingSetBudgetBytes,
                                      limits, error)) {
      retryableResourceFailure =
          usage.totalDeviceBytes &&
          usage.freeDeviceBytes <= usage.totalDeviceBytes &&
          (hostWorkingSet > request.workingSetBudgetBytes ||
           allocationBytes > freeBytes ||
           runtimeDeviceBytes > limits.maximumDeviceBytes ||
           allocationBytes >
               limits.maximumDeviceBytes -
                   std::min<std::uint64_t>(runtimeDeviceBytes,
                                           limits.maximumDeviceBytes));
      return false;
    }
    const std::array<std::size_t, 5> sizes{inputBytes, headBytes, headBytes,
                                           headBytes,
                                           static_cast<std::size_t>(workspace)};
    for (std::size_t index = 0; index < memory.size(); ++index) {
      if (!sizes[index]) continue;
      void* allocated = nullptr;
      const auto status = allocate(&allocated, sizes[index]);
      if (!checkCuda(status, "cudaMalloc", error)) {
        // Ignore a pointer written by a failed cudaMalloc. The physical tier
        // is what the next legal downshift must shrink.
        memory[index] = nullptr;
        attemptedExecutionBatch = maximumBatch;
        return false;
      }
      memory[index] = allocated;
    }
    context->setDeviceMemoryV2(memory[4], static_cast<int64_t>(workspace));
    if (!context->setTensorAddress(inputUtf8.constData(), memory[0]))
      return fail(error, "Cannot bind the bounded model input/workspace");
    const auto heads = manifest["heads"].toArray();
    for (int index = 0; index < 3; ++index) {
      headNames[index] = heads[index].toObject()["name"].toString();
      const auto name = headNames[index].toUtf8();
      const auto expected =
          scn ? nvinfer1::Dims4(maximumBatch, 8192, 1, 1)
              : nvinfer1::Dims4(maximumBatch, 1, 1, modelBins / 4);
      if (!contiguousTensor(name, expected, nvinfer1::TensorIOMode::kOUTPUT,
                            error))
        return false;
      if (!context->setTensorAddress(name.constData(), memory[index + 1]))
        return fail(error, "Cannot bind model output head");
    }
    evidence = {
        {"backend", scn ? "scn-tensorrt" : "ffscn-tensorrt"},
        {"modelSha256", manifest["loadedEngineSha256"]},
        {"checkpointSha256", manifest["loadedCheckpointSha256"]},
        {"engineAbsolutePath", manifest["engineAbsolutePath"]},
        {"manifestSha256", manifest["manifestSha256"]},
        {"profileId", QString::fromStdString(request.profile.id)},
        {"profileVersion", static_cast<int>(request.profile.version)},
        {"tensorRt", "10.11.0.33"},
        {"cudaRuntime", cudaVersion},
        {"cudaDriver", cudaDriver},
        {"precision", "FP32"},
        {"device", QString::fromUtf8(properties.name)},
        {"deviceIndex", selected},
        {"computeCapability", QJsonArray{12, 0}},
        {"engineMaximumBatch", static_cast<int>(maximumBatch)},
        {"optimizationProfile", activeProfile},
        {"logicalMaximumBatch", static_cast<int>(logicalMaximumBatch)},
        {"physicalBatch", static_cast<int>(maximumBatch)},
        {"profileMaximumBatch", profileMaximumBatch},
        {"profileSupportedTiers", supportedTiers},
        {"batchProfileReason", profileReason},
        {"requestedMaximumBatch",
         static_cast<int>(request.parameters.batchSize)},
        {"modelInputKind",
         scn ? "selected-psd-minmax" : "selected-psd-global-zscore"},
        {"inputAdaptationVersion",
         static_cast<int>(request.inputAdaptationVersion)},
        {"analysisBins", static_cast<int>(request.profile.fftSize)},
        {"modelBins", static_cast<int>(modelBins)},
        {"minimumModelBins",
         static_cast<int>(detectionModelMinimumBins(request))},
        {"frequencyBinScale", detectionModelBinScale(request)},
        {"frequencyAdaptation", request.profile.fftSize == modelBins
                                    ? "identity"
                                    : (scn ? "linear-power-cell-rebin"
                                           : "linear-frequency-interpolation")},
        {"actualMaximumFrames", static_cast<int>(maximumFrames)},
        {"meanSpectrumInferenceCalls", 0},
        {"spectrumMode", static_cast<int>(request.spectrumMode)},
        {"boundedDeviceBytes", QString::number(allocationBytes)},
        {"deviceWorkspaceBytes", QString::number(workspace)},
        {"deviceInputBytes", QString::number(inputBytes)},
        {"deviceHeadBytes", QString::number(3 * headBytes)},
        {"deviceBudgetBytes", QString::number(limits.maximumDeviceBytes)},
        {"deviceFreeBytesBeforeModel", QString::number(initialFreeBytes)},
        {"deviceFreeBytesBeforeAllocation", QString::number(freeBytes)},
        {"deviceTotalBytes", QString::number(totalBytes)},
        {"runtimeDeviceBytes", QString::number(runtimeDeviceBytes)},
        {"hostWorkingSetBytes", QString::number(hostWorkingSet)},
        {"hostWorkingSetBudgetBytes",
         QString::number(request.workingSetBudgetBytes)},
        {"engineBlobBytes", QString::number(engineBytes.size())},
        {"engineBlobBudgetBytes",
         QString::number(limits.maximumEngineBlobBytes)}};
    DetectionDeviceAccount account;
    account.cudaDeviceIndex = selected;
    account.requestedBatch = request.parameters.batchSize;
    account.logicalBatch = logicalMaximumBatch;
    account.physicalBatch = maximumBatch;
    account.freeDeviceBytes = accountedFreeDeviceBytes;
    account.totalDeviceBytes = accountedTotalDeviceBytes;
    account.reservedDeviceBytes = accountedReservedDeviceBytes;
    account.neededDeviceBytes = neededDeviceBytes;
    writeDetectionDeviceEvidence(evidence, account, profileExtent);
    for(auto& event:timingEvents)
      if(!checkCuda(createEvent(&event),"cudaEventCreate timing",error)) return false;
    evidence["cudaEventTiming"]=true;
    return true;
  }

  bool forward(const std::vector<float>& sourceFrames, std::uint32_t batch,
               DetectionModelOutput& result, QString& error,
               std::uint32_t actualFrames) {
    if (stopIfCancelled(error)) return false;
    const auto frames =
        actualFrames ? actualFrames : request.profile.modelFrames;
    if (!batch || batch > logicalMaximumBatch || !frames ||
        frames > maximumFrames)
      return fail(error, "Actual model window exceeds its planned allocation");
    QElapsedTimer adaptationTimer;adaptationTimer.start();
    std::vector<float> input;
    if (!prepareDetectionModelInput(request, sourceFrames, batch, input, error,
                                    frames))
      return false;
    const double adaptationMs=adaptationTimer.nsecsElapsed()/1e6;
    const bool scn = request.profile.backend == DetectionBackendKind::Scn;
    const auto shape =
        scn ? nvinfer1::Dims4(1, 32768, 1, 1)
            : nvinfer1::Dims4(batch, 1,
                              detectionModelTensorFrames(request, frames),
                              modelBins);
    if (!context->setInputShape(inputName.toUtf8().constData(), shape))
      return fail(error, "Cannot prepare the requested model batch shape");
    if (context->updateDeviceMemorySizeForShapes() > workspaceBytes)
      return fail(error,
                  "Model batch workspace exceeds the bounded allocation");
    DetectionModelOutput output;
    output.batch = batch;
    output.inputBins = modelBins;
    output.actualFrames = frames;
    output.width = modelBins / 4;
    const auto elements = static_cast<std::size_t>(batch) * output.width;
    output.hm.resize(elements);
    output.bw.resize(elements);
    output.off.resize(elements);
    StreamDrain drain(*this);
    const std::uint32_t calls = scn ? batch : 1;
    const auto inputElements = scn ? modelBins : input.size();
    const auto outputElements = scn ? output.width : elements;
    double enqueueHostMs=0;
    for (std::uint32_t call = 0; call < calls; ++call) {
      if (stopIfCancelled(error)) return false;
      if(call*4+3>=timingEvents.size()) return fail(error,"CUDA timing call capacity exceeded");
      if(!checkCuda(recordEvent(timingEvents[call*4],stream),"cudaEventRecord input",error)) return false;
      if (!checkCuda(copy(memory[0], input.data() + call * inputElements,
                          inputElements * sizeof(float), cudaMemcpyHostToDevice,
                          stream),
                     "cudaMemcpyAsync input", error))
        return false;
      if(!checkCuda(recordEvent(timingEvents[call*4+1],stream),"cudaEventRecord enqueue start",error)) return false;
      QElapsedTimer enqueueTimer;enqueueTimer.start();
      if (!context->enqueueV3(stream))
        return fail(error,
                    "TensorRT model enqueue failed: " + logger.lastError());
      enqueueHostMs+=enqueueTimer.nsecsElapsed()/1e6;
      ++physicalEnqueues;
      if(!checkCuda(recordEvent(timingEvents[call*4+2],stream),"cudaEventRecord enqueue end",error)) return false;
      const auto offset = call * outputElements;
      const std::array<float*, 3> destinations{output.hm.data() + offset,
                                               output.bw.data() + offset,
                                               output.off.data() + offset};
      for (int index = 0; index < 3; ++index) {
        if (!checkCuda(copy(destinations[index], memory[index + 1],
                            outputElements * sizeof(float),
                            cudaMemcpyDeviceToHost, stream),
                       "cudaMemcpyAsync output", error))
          return false;
        if (hooks.failAfterQueuedOutput && hooks.failAfterQueuedOutput(index))
          return fail(error, "Injected model failure after queued output copy");
      }
      if(!checkCuda(recordEvent(timingEvents[call*4+3],stream),"cudaEventRecord output end",error)) return false;
    }
    if (!drain.complete(error)) return false;
    std::array<double,3> gpuMs{};
    for(std::uint32_t call=0;call<calls;++call)
      for(std::size_t part=0;part<gpuMs.size();++part) {
        float elapsed=0;
        if(!checkCuda(elapsedEvent(&elapsed,timingEvents[call*4+part],timingEvents[call*4+part+1]),
            "cudaEventElapsedTime",error)) return false;
        if(!std::isfinite(elapsed) || elapsed<0) return fail(error,"CUDA event interval is not finite/nonnegative");
        gpuMs[part]+=elapsed;
      }
    for (const auto* head : {&output.hm, &output.bw, &output.off})
      if (!std::all_of(head->begin(), head->end(),
                       [](float value) { return std::isfinite(value); }))
        return fail(error, "Model returned a non-finite output head");
    result = std::move(output);
    evidence["lastInference"]=QJsonObject{{"cudaEventIntervals",true},
        {"h2dMs",gpuMs[0]},{"gpuStreamMs",gpuMs[1]},{"d2hMs",gpuMs[2]},
        {"hostEnqueueMs",enqueueHostMs},{"inputAdaptationMs",adaptationMs},
        {"physicalEnqueueCalls",int(calls)},{"logicalBatch",int(batch)}};
    evidence["physicalEnqueueCount"]=QString::number(physicalEnqueues);
    evidence["lastActualFrames"] = static_cast<int>(frames);
    evidence["lastTensorFrames"] =
        static_cast<int>(detectionModelTensorFrames(request, frames));
    if (request.spectrumMode == DetectionSpectrumMode::Mean)
      evidence["meanSpectrumInferenceCalls"] =
          evidence["meanSpectrumInferenceCalls"].toInt() +
          static_cast<int>(calls);
    return true;
  }
#endif
};

DetectionModelBackend::DetectionModelBackend(
    QString modelRoot, DetectionModelResourceLimits limits,
    DetectionModelRuntimeHooks hooks)
    : modelRoot_(modelRoot.isEmpty()
                     ? QDir(QCoreApplication::applicationDirPath())
                           .filePath("detection-models")
                     : std::move(modelRoot)),
      limits_(limits),
      hooks_(std::move(hooks)) {}
DetectionModelBackend::~DetectionModelBackend() = default;
void DetectionModelBackend::reset() { impl_.reset(); }
QJsonObject DetectionModelBackend::evidence() const {
  if (!impl_) return {};
  auto result = impl_->evidence;
#ifdef SS_DETECTION_TENSORRT
  result["streamDrainCount"] = QString::number(impl_->streamDrainCount);
#endif
  return result;
}
bool DetectionModelBackend::load(const DetectionRequest& request,
                                 QString& error) {
  reset();
  if (!isDetectionExecutionBatchTier(limits_.maximumExecutionBatch) ||
      !isDetectionExecutionBatchTier(request.parameters.batchSize))
    return fail(error, "Execution batch must be 1, 2, 4, 8 or 16");
  QString batchFallbackReason;
  for (auto batch = std::min(request.parameters.batchSize,
                             limits_.maximumExecutionBatch);
       batch > 0; batch = batch == 1 ? 0 : batch / 2) {
    auto candidate = std::make_unique<Impl>();
    candidate->request = request;
    candidate->limits = limits_;
    candidate->limits.maximumExecutionBatch = batch;
    candidate->hooks = hooks_;
    if (!validateDetectionModelPackage(request, modelRoot_, candidate->manifest,
                                       error))
      return false;
#ifdef SS_DETECTION_TENSORRT
    if (!candidate->initialize(QFileInfo(modelRoot_).absolutePath(), error)) {
      const auto attempted = candidate->attemptedExecutionBatch
                                 ? candidate->attemptedExecutionBatch
                                 : batch;
      const auto next = nextDetectionWorkerLimitAfterResourceFailure(
          attempted, candidate->retryableResourceFailure);
      if (next != 0) {
        batchFallbackReason = error;
        // The for-loop halves this value. next is attempted / 2.
        batch = attempted;
        continue;
      }
      return false;
    }
    if (!batchFallbackReason.isEmpty()) {
      candidate->evidence["batchAdaptationReason"] = batchFallbackReason;
      if (detectionAdaptationWasInjected(batchFallbackReason))
        candidate->evidence["allocationFailureKind"] =
            QStringLiteral("injected");
    }
    impl_ = std::move(candidate);
    error.clear();
    return true;
#else
    return fail(error,
                "This worker was built without the exact TensorRT 10.11 SDK; "
                "production inference is unavailable");
#endif
  }
  return fail(error,
              "No execution batch fits the installed model resources. The load "
              "is rejected and no window is skipped.");
}
bool DetectionModelBackend::infer(const std::vector<float>& sourceFrames,
                                  std::uint32_t batch,
                                  DetectionModelOutput& output, QString& error,
                                  std::uint32_t actualFrames) {
  if (!impl_) return fail(error, "Production model has not passed preflight");
#ifdef SS_DETECTION_TENSORRT
  return impl_->forward(sourceFrames, batch, output, error, actualFrames);
#else
  Q_UNUSED(sourceFrames);
  Q_UNUSED(batch);
  Q_UNUSED(output);
  Q_UNUSED(actualFrames);
  return fail(error, "TensorRT production backend is unavailable");
#endif
}
}  // namespace signalstudio
