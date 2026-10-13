#include "infrastructure/detection_model_backend.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
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
constexpr auto kFfscnSha =
    "d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81";
constexpr auto kScnSha =
    "ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627";
constexpr auto kTrtSha =
    "d100ec93e5abf56f8d2dafb69cde536c4f9fbb4748a5746a74b7009f6894ce67";
constexpr auto kCudaSha =
    "760c38928bbe5759f7b31ed6692599eb7ec83cedd5702e84c2b72028a89837e1";

bool fail(QString& error, const QString& message) {
  error = message;
  return false;
}

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

bool plainFileName(const QString& name) {
  return !name.isEmpty() && name != "." && name != ".." &&
         !name.contains('/') && !name.contains('\\') && !name.contains(':');
}

bool sameShape(const QJsonArray& array, const std::array<int, 4>& shape) {
  if (array.size() != 4) return false;
  for (int index = 0; index < 4; ++index)
    if (!array[index].isDouble() || array[index].toDouble() != shape[index])
      return false;
  return true;
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
                                QString& error) {
  const bool scn = request.profile.backend == DetectionBackendKind::Scn;
  if ((request.profile.id != "ffscn-gui-iq-v1" &&
       request.profile.id != "scn-max-psd-v1") ||
      batch == 0 || batch > 2 || batch > request.parameters.batchSize)
    return fail(error, "Model profile or batch is unsupported");
  const auto* registered = findDetectionProfile(request.profile.id);
  if (!registered || request.profile.version != registered->version ||
      request.profile.backend != registered->backend ||
      request.profile.fftSize != registered->fftSize ||
      request.profile.modelFrames != registered->modelFrames ||
      request.profile.maximumPsdOnly != registered->maximumPsdOnly)
    return fail(error,
                "Model tensor profile differs from the registered shape");
  const std::size_t bins = request.profile.fftSize;
  const std::size_t frames = request.profile.modelFrames;
  const auto expected = static_cast<std::size_t>(batch) * bins * frames;
  const auto outputElements = scn ? batch * bins : expected;
  if (sourceFrames.size() != expected ||
      (expected + outputElements) * sizeof(float) >
          request.workingSetBudgetBytes)
    return fail(error, "Model input shape or working-set budget is invalid");
  if (!std::all_of(sourceFrames.begin(), sourceFrames.end(),
                   [](float value) { return std::isfinite(value); }))
    return fail(error, "Model input contains a non-finite spectrum value");
  std::vector<float> prepared(outputElements);
  if (!scn) {
    prepared = sourceFrames;
  } else {
    for (std::size_t item = 0; item < batch; ++item) {
      const auto source = item * frames * bins;
      const auto target = item * bins;
      for (std::size_t bin = 0; bin < bins; ++bin) {
        float maximum = sourceFrames[source + bin];
        for (std::size_t frame = 1; frame < frames; ++frame)
          maximum =
              std::max(maximum, sourceFrames[source + frame * bins + bin]);
        prepared[target + bin] = maximum;
      }
      const auto begin = prepared.begin() + target;
      const auto [lower, upper] = std::minmax_element(begin, begin + bins);
      const float minimum = *lower;
      const float span =
          std::max(*upper - minimum, std::numeric_limits<float>::epsilon());
      if (!std::isfinite(span))
        return fail(error, "SCN maximum-spectrum minmax span is not finite");
      for (std::size_t bin = 0; bin < bins; ++bin)
        prepared[target + bin] = (prepared[target + bin] - minimum) / span;
    }
  }
  input = std::move(prepared);
  return true;
}

bool validateDetectionModelPackage(const DetectionRequest& request,
                                   const QString& modelRoot,
                                   QJsonObject& manifest, QString& error) {
  if (request.profile.backend == DetectionBackendKind::Experimental17)
    return fail(error,
                "FFSCN 17-order 131072-bin numerical parity is unvalidated; "
                "this experimental profile cannot start production inference");
  if (request.profile.id != "ffscn-gui-iq-v1" &&
      request.profile.id != "scn-max-psd-v1")
    return fail(error, "No production model package exists for this profile");
  const auto* registered = findDetectionProfile(request.profile.id);
  if (!registered || request.profile.version != registered->version ||
      request.profile.backend != registered->backend ||
      request.profile.fftSize != registered->fftSize ||
      request.profile.modelFrames != registered->modelFrames ||
      request.profile.maximumPsdOnly != registered->maximumPsdOnly ||
      request.parameters.batchSize < 1 || request.parameters.batchSize > 2)
    return fail(error, "Production model request shape is not registered");
  const auto directory =
      QDir(modelRoot).filePath(QString::fromStdString(request.profile.id));
  const auto path = QDir(directory).filePath("manifest.json");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > 1024 * 1024)
    return fail(error, "Model manifest is missing or exceeds 1 MiB: " + path);
  const auto bytes = file.readAll();
  const auto document = QJsonDocument::fromJson(bytes);
  if (!document.isObject()) return fail(error, "Model manifest is not JSON");
  const auto object = document.object();
  const bool scn = request.profile.id == "scn-max-psd-v1";
  const auto engine = object["engine"].toObject();
  const auto runtime = object["runtime"].toObject();
  const auto input = object["input"].toObject();
  const auto heads = object["heads"].toArray();
  const auto capability = runtime["computeCapability"].toArray();
  const std::array<int, 4> minimum = scn ? std::array<int, 4>{1, 32768, 1, 1}
                                         : std::array<int, 4>{1, 1, 10, 65536};
  auto maximum = minimum;
  maximum[0] = scn ? 1 : 2;
  if (object["schema"] != "signalstudio-detection-model-package-v1" ||
      object["profileId"] != QString::fromStdString(request.profile.id) ||
      object["profileVersion"].toDouble() != request.profile.version ||
      object["precision"] != "FP32" || runtime["tensorRt"] != "10.11.0.33" ||
      runtime["cudaRuntime"].toInt() != 12090 ||
      runtime["hardwareCompatibility"] != "none" ||
      runtime["versionCompatible"] != false ||
      capability != QJsonArray{12, 0} ||
      runtime["gpu"] != "NVIDIA GeForce RTX 5060 Laptop GPU" ||
      input["dtype"] != "float32" || input["layout"] != "contiguous-NCHW" ||
      input["name"] != (scn ? "resnet_32_input:0" : "spectrum") ||
      !sameShape(input["minShape"].toArray(), minimum) ||
      !sameShape(input["maxShape"].toArray(), maximum) || heads.size() != 3 ||
      !plainFileName(engine["path"].toString()) ||
      engine["sha256"] != (scn ? kScnSha : kFfscnSha))
    return fail(error,
                "Model manifest identity/runtime/precision/shape differs "
                "from the frozen production contract");
  const QStringList names =
      scn ? QStringList{"Identity_1:0", "Identity:0", "Identity_2:0"}
          : QStringList{"hm", "bw", "off"};
  for (int index = 0; index < 3; ++index) {
    const auto head = heads[index].toObject();
    const auto shape = scn ? std::array<int, 4>{0, 8192, 1, 1}
                           : std::array<int, 4>{0, 1, 1, 16384};
    if (head["semantic"] != QStringList{"hm", "bw", "off"}[index] ||
        head["name"] != names[index] || head["dtype"] != "float32" ||
        !sameShape(head["shape"].toArray(), shape))
      return fail(error, "Model output head contract is invalid");
  }
  if (request.modelSha256.empty() ||
      request.modelSha256 != engine["sha256"].toString().toStdString())
    return fail(error, "Immutable request model SHA differs from its package");
  const auto enginePath = QDir(directory).filePath(engine["path"].toString());
  const auto hash = fileHash(enginePath, kMaximumEngineBytes, error);
  if (hash.isEmpty() || hash != engine["sha256"].toString())
    return fail(error,
                "Model engine SHA-256 does not match its frozen identity");
  if (engine["bytes"].toDouble() != QFileInfo(enginePath).size())
    return fail(error, "Model engine byte length differs from its manifest");
  QJsonObject checked = object;
  checked["manifestSha256"] = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  checked["engineAbsolutePath"] = QFileInfo(enginePath).absoluteFilePath();
  manifest = std::move(checked);
  return true;
}

class DetectionModelBackend::Impl {
 public:
  DetectionRequest request;
  QJsonObject manifest;
  QJsonObject evidence;
  DetectionModelResourceLimits limits;
  DetectionModelRuntimeHooks hooks;
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
  std::unique_ptr<nvinfer1::IRuntime> runtime;
  std::unique_ptr<nvinfer1::ICudaEngine> engine;
  std::unique_ptr<nvinfer1::IExecutionContext> context;
  cudaStream_t stream = nullptr;
  std::array<void*, 5> memory{};
  std::array<QString, 3> headNames;
  QString inputName;
  std::uint32_t maximumBatch = 2;
  std::size_t workspaceBytes = 0;
  std::uint64_t streamDrainCount = 0;

  class StreamDrain {
   public:
    explicit StreamDrain(Impl& owner) : owner_(owner) {}
    ~StreamDrain() {
      // This scope is destroyed before any host buffers used by asynchronous
      // transfers. Preserve the original failure; cleanup cannot overwrite it.
      if (!completed_) {
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

  ~Impl() {
    if (stream && synchronize) synchronize(stream);
    context.reset();
    engine.reset();
    runtime.reset();
    for (void* value : memory)
      if (value && release) release(value);
    if (stream && destroyStream) destroyStream(stream);
  }

  bool checkCuda(cudaError_t status, const char* operation, QString& error) {
    if (status == cudaSuccess) return true;
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
    const bool scn = request.profile.backend == DetectionBackendKind::Scn;
    const auto physicalBatch = scn ? 1 : request.parameters.batchSize;
    const auto minimumDeviceBytes =
        static_cast<std::uint64_t>(physicalBatch) *
        (request.profile.fftSize * (scn ? 1 : request.profile.modelFrames) +
         3 * (request.profile.fftSize / 4)) *
        sizeof(float);
    if (minimumDeviceBytes > limits.maximumDeviceBytes)
      return fail(error,
                  "Model GPU input/output exceeds its independent device "
                  "budget before engine loading");
    std::size_t initialFreeBytes = 0;
    std::size_t initialTotalBytes = 0;
    if (!checkCuda(memoryInfo(&initialFreeBytes, &initialTotalBytes),
                   "cudaMemGetInfo before model", error))
      return false;
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
    maximumBatch = scn ? 1 : request.parameters.batchSize;
    const auto shape = scn ? nvinfer1::Dims4(maximumBatch, 32768, 1, 1)
                           : nvinfer1::Dims4(maximumBatch, 1, 10, 65536);
    if (!context->setInputShape(inputUtf8.constData(), shape))
      return fail(error,
                  "Engine does not accept the frozen maximum batch shape");
    const auto workspace = context->updateDeviceMemorySizeForShapes();
    if (!contiguousTensor(inputUtf8, shape, nvinfer1::TensorIOMode::kINPUT,
                          error))
      return false;
    workspaceBytes = workspace;
    const std::size_t inputBytes = static_cast<std::size_t>(maximumBatch) *
                                   request.profile.fftSize * (scn ? 1 : 10) *
                                   sizeof(float);
    const std::size_t headBytes = static_cast<std::size_t>(maximumBatch) *
                                  (request.profile.fftSize / 4) * sizeof(float);
    const std::size_t allocationBytes = inputBytes + 3 * headBytes + workspace;
    const auto logicalBatch =
        static_cast<std::size_t>(request.parameters.batchSize);
    const auto sourceBytes = logicalBatch * request.profile.fftSize *
                             request.profile.modelFrames * sizeof(float);
    const auto preparedBytes = logicalBatch * request.profile.fftSize *
                               (scn ? 1 : request.profile.modelFrames) *
                               sizeof(float);
    const auto logicalHeadBytes =
        logicalBatch * (request.profile.fftSize / 4) * sizeof(float);
    // Source, prepared input and four complete hm/bw/off host copies cover
    // output, packed heads, binary payload and framed response in the worker.
    const auto hostWorkingSet =
        sourceBytes + preparedBytes + 12 * logicalHeadBytes;
    std::size_t freeBytes = 0;
    std::size_t totalBytes = 0;
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
                                      limits, error))
      return false;
    const std::array<std::size_t, 5> sizes{inputBytes, headBytes, headBytes,
                                           headBytes,
                                           static_cast<std::size_t>(workspace)};
    for (std::size_t index = 0; index < memory.size(); ++index)
      if (sizes[index] && !checkCuda(allocate(&memory[index], sizes[index]),
                                     "cudaMalloc", error))
        return false;
    context->setDeviceMemoryV2(memory[4], static_cast<int64_t>(workspace));
    if (!context->setTensorAddress(inputUtf8.constData(), memory[0]))
      return fail(error, "Cannot bind the bounded model input/workspace");
    const auto heads = manifest["heads"].toArray();
    for (int index = 0; index < 3; ++index) {
      headNames[index] = heads[index].toObject()["name"].toString();
      const auto name = headNames[index].toUtf8();
      const auto expected = scn ? nvinfer1::Dims4(maximumBatch, 8192, 1, 1)
                                : nvinfer1::Dims4(maximumBatch, 1, 1, 16384);
      if (!contiguousTensor(name, expected, nvinfer1::TensorIOMode::kOUTPUT,
                            error))
        return false;
      if (!context->setTensorAddress(name.constData(), memory[index + 1]))
        return fail(error, "Cannot bind model output head");
    }
    if (!checkCuda(createStream(&stream), "cudaStreamCreate", error))
      return false;
    evidence = {
        {"backend", scn ? "scn-maximum-tensorrt" : "ffscn-tensorrt"},
        {"modelSha256", manifest["engine"].toObject()["sha256"]},
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
        {"logicalMaximumBatch", static_cast<int>(request.parameters.batchSize)},
        {"modelInputKind",
         scn ? "maximum-of-16-psd-minmax" : "10-psd-global-zscore"},
        {"meanSpectrumInferenceCalls", 0},
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
    return true;
  }

  bool forward(const std::vector<float>& sourceFrames, std::uint32_t batch,
               DetectionModelOutput& result, QString& error) {
    std::vector<float> input;
    if (!prepareDetectionModelInput(request, sourceFrames, batch, input, error))
      return false;
    const bool scn = request.profile.backend == DetectionBackendKind::Scn;
    const auto shape = scn ? nvinfer1::Dims4(1, 32768, 1, 1)
                           : nvinfer1::Dims4(batch, 1, 10, 65536);
    if (!context->setInputShape(inputName.toUtf8().constData(), shape))
      return fail(error, "Cannot prepare the requested model batch shape");
    if (context->updateDeviceMemorySizeForShapes() > workspaceBytes)
      return fail(error,
                  "Model batch workspace exceeds the bounded allocation");
    DetectionModelOutput output;
    output.batch = batch;
    output.width = request.profile.fftSize / 4;
    const auto elements = static_cast<std::size_t>(batch) * output.width;
    output.hm.resize(elements);
    output.bw.resize(elements);
    output.off.resize(elements);
    StreamDrain drain(*this);
    const std::uint32_t calls = scn ? batch : 1;
    const auto inputElements = scn ? request.profile.fftSize : input.size();
    const auto outputElements = scn ? output.width : elements;
    for (std::uint32_t call = 0; call < calls; ++call) {
      if (!checkCuda(copy(memory[0], input.data() + call * inputElements,
                          inputElements * sizeof(float), cudaMemcpyHostToDevice,
                          stream),
                     "cudaMemcpyAsync input", error))
        return false;
      if (!context->enqueueV3(stream))
        return fail(error,
                    "TensorRT model enqueue failed: " + logger.lastError());
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
    }
    if (!drain.complete(error)) return false;
    for (const auto* head : {&output.hm, &output.bw, &output.off})
      if (!std::all_of(head->begin(), head->end(),
                       [](float value) { return std::isfinite(value); }))
        return fail(error, "Model returned a non-finite output head");
    result = std::move(output);
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
  auto candidate = std::make_unique<Impl>();
  candidate->request = request;
  candidate->limits = limits_;
  candidate->hooks = hooks_;
  if (!validateDetectionModelPackage(request, modelRoot_, candidate->manifest,
                                     error))
    return false;
#ifdef SS_DETECTION_TENSORRT
  if (!candidate->initialize(QFileInfo(modelRoot_).absolutePath(), error))
    return false;
  impl_ = std::move(candidate);
  return true;
#else
  return fail(error,
              "This worker was built without the exact TensorRT 10.11 SDK; "
              "production inference is unavailable");
#endif
}
bool DetectionModelBackend::infer(const std::vector<float>& sourceFrames,
                                  std::uint32_t batch,
                                  DetectionModelOutput& output,
                                  QString& error) {
  if (!impl_) return fail(error, "Production model has not passed preflight");
#ifdef SS_DETECTION_TENSORRT
  return impl_->forward(sourceFrames, batch, output, error);
#else
  Q_UNUSED(sourceFrames);
  Q_UNUSED(batch);
  Q_UNUSED(output);
  return fail(error, "TensorRT production backend is unavailable");
#endif
}
}  // namespace signalstudio
