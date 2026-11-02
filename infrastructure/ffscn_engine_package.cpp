#include "infrastructure/ffscn_engine_package.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <array>

#include "domain/detection.h"
#include "infrastructure/detection_model_backend.h"

namespace signalstudio {
namespace {
constexpr qint64 kMaximumEngineBytes = 128 * 1024 * 1024;
constexpr auto kStaticFfscnSha =
    "d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81";
constexpr auto kStaticScnSha =
    "ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627";
constexpr auto kRecordSchema = "signalstudio-ffscn-active-package-v1";
constexpr auto kRecordName = "ffscn-active-package.json";
constexpr auto kFfscnProfile = "ffscn-gui-iq-v1";
constexpr auto kScnProfile = "scn-max-psd-v1";
constexpr auto kToolSchema = "signalstudio-ffscn-engine-tool-v1";

bool fail(QString& error, const QString& message) {
  error = message;
  return false;
}

QString fileHash(const QString& path, qint64 maximum, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > maximum) {
    fail(error, "Model package file is missing, empty or exceeds its budget: " +
                    path);
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

QString deployedFfscnDirectory(const QString& modelRoot) {
  return QDir(modelRoot).filePath(kFfscnProfile);
}

QJsonObject readJsonFile(const QString& path, qint64 maximum, QString& error) {
  QFile file(path);
  if (!file.exists()) return {};
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > maximum) {
    fail(error, "无法读取清单：" + path);
    return {};
  }
  const auto document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject()) {
    fail(error, "清单不是 JSON 对象：" + path);
    return {};
  }
  error.clear();
  return document.object();
}

QString toolManifestReason(const QJsonObject& manifest) {
  if (manifest["precision"].toString() == QLatin1String("FP16"))
    return QStringLiteral(
        "该目录是转换工具产物，精度为 FP16。当前检测只接受 FP32，"
        "生产校验没有放宽精度，因此不能切换。");
  return QStringLiteral(
      "该目录只有转换工具清单 tool-manifest.json，不是可加载的 FFSCN "
      "检测包。需要 manifest.json，并且满足 FP32、down_nums=11、"
      "频率长度为不小于 2048 的 2 的幂，以及 TensorRT/CUDA/SM 契约。"
      "转换不会自动替换活动引擎。");
}

bool readActiveObject(const QString& modelRoot, QJsonObject& object,
                      bool& present, QString& error) {
  present = false;
  const auto path = ffscnActivePackageRecordPath(modelRoot);
  if (!QFileInfo::exists(path)) {
    error.clear();
    return true;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > 1024 * 1024) {
    return fail(error, "FFSCN 活动引擎记录无法读取：" + path);
  }
  const auto document = QJsonDocument::fromJson(file.readAll());
  object = document.object();
  const auto packageDirectory = object["packageDirectory"].toString();
  const QFileInfo directory(packageDirectory);
  if (!document.isObject() || object["schema"].toString() != kRecordSchema ||
      object["profileId"].toString() != kFfscnProfile ||
      !directory.exists() || !directory.isDir() ||
      directory.canonicalFilePath().isEmpty()) {
    return fail(error, "FFSCN 活动引擎记录无效或目录已不存在：" +
                           packageDirectory);
  }
  present = true;
  error.clear();
  return true;
}

bool publishActiveRecord(const QString& modelRoot,
                         const FfscnActivePackage& package, QString& error) {
  if (!QDir().mkpath(modelRoot))
    return fail(error, "无法创建模型目录：" + modelRoot);
  const QJsonObject record{
      {"schema", kRecordSchema},
      {"profileId", kFfscnProfile},
      {"packageDirectory", package.packageDirectory},
      {"manifestPath", package.manifestPath},
      {"enginePath", package.enginePath},
      {"engineSha256", package.engineSha256},
      {"checkpointSha256", package.checkpointSha256},
      {"manifestSha256", package.manifestSha256}};
  const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Indented);
  QSaveFile output(ffscnActivePackageRecordPath(modelRoot));
  if (!output.open(QIODevice::WriteOnly))
    return fail(error, "无法写入 FFSCN 活动引擎记录：" + output.errorString());
  if (output.write(bytes) != bytes.size()) {
    output.cancelWriting();
    return fail(error, "无法写入 FFSCN 活动引擎记录：" + output.errorString());
  }
  if (!output.commit()) {
    output.cancelWriting();
    return fail(error,
                "无法原子发布 FFSCN 活动引擎记录：" + output.errorString());
  }
  return true;
}
}  // namespace

bool validateDetectionModelPackageDirectory(const DetectionRequest& request,
                                            const QString& packageDirectory,
                                            QJsonObject& manifest,
                                            QString& error) {
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
      request.profile.maximumPsdOnly != registered->maximumPsdOnly ||
      !isDetectionExecutionBatchTier(request.parameters.batchSize))
    return fail(error, "Production model request shape is not registered");
  const auto path = QDir(packageDirectory).filePath("manifest.json");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > 1024 * 1024)
    return fail(error, "Model manifest is missing or exceeds 1 MiB: " + path);
  const auto bytes = file.readAll();
  const auto document = QJsonDocument::fromJson(bytes);
  if (!document.isObject()) return fail(error, "Model manifest is not JSON");
  const auto object = document.object();
  const bool scn = request.profile.id == "scn-max-psd-v1";
  const auto precision = object["precision"].toString();
  if (precision == QLatin1String("FP16"))
    return fail(error,
                "FP16 engines cannot be loaded: production validation accepts "
                "FP32 only and was not extended for this precision");
  const auto engine = object["engine"].toObject();
  const auto runtime = object["runtime"].toObject();
  const auto input = object["input"].toObject();
  const auto heads = object["heads"].toArray();
  const auto capability = runtime["computeCapability"].toArray();
  const std::array<int, 4> minimum = scn ? std::array<int, 4>{1, 32768, 1, 1}
                                         : std::array<int, 4>{1, 1, 10, 65536};
  auto maximum = minimum;
  maximum[0] = scn ? 1 : 2;
  // Checkpoint SHA identifies the packaged weights. It is recorded and is not
  // pinned to epoch-1200. The engine file must still match the manifest, and
  // the FP32, shape, and runtime checks stay in force.
  const auto checkpointSha = object["checkpointSha256"].toString();
  const bool dynamicContract =
      !scn && object["schema"] == "signalstudio-detection-model-package-v2" &&
      object["adaptationVersion"] == 2;
  if (dynamicContract && !isLowercaseSha256(checkpointSha))
    return fail(error,
                "FFSCN checkpoint SHA-256 must be 64 lowercase hex digits");
  const bool dynamic = dynamicContract && isLowercaseSha256(checkpointSha);
  const auto declaredMin = input["minShape"].toArray();
  const auto declaredMax = input["maxShape"].toArray();
  const bool dynamicShape =
      dynamic && declaredMin == QJsonArray{1, 1, 1, 2048} &&
      input["downNums"] == 11 && input["frequencyPowerOfTwo"] == true &&
      declaredMax.size() == 4 && declaredMax[0].toInt() >= 1 &&
      declaredMax[1] == 1 && declaredMax[2].toInt() >= 1 &&
      declaredMax[3].toInt() >= 2048;
  if ((!dynamic &&
       object["schema"] != "signalstudio-detection-model-package-v1") ||
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
      (dynamic ? !dynamicShape
               : (!sameShape(declaredMin, minimum) ||
                  !sameShape(declaredMax, maximum))) ||
      heads.size() != 3 || !plainFileName(engine["path"].toString()) ||
      (!dynamic && engine["sha256"] != (scn ? kStaticScnSha : kStaticFfscnSha)))
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
        (dynamic
             ? (head["shape"] != QJsonArray{0, 1, 1, 0} || head["stride"] != 4)
             : !sameShape(head["shape"].toArray(), shape)))
      return fail(error, "Model output head contract is invalid");
  }
  if (request.modelSha256.empty() ||
      request.modelSha256 != engine["sha256"].toString().toStdString())
    return fail(error, "Immutable request model SHA differs from its package");
  const auto enginePath =
      QDir(packageDirectory).filePath(engine["path"].toString());
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
  checked["loadedEngineSha256"] = hash;
  checked["loadedCheckpointSha256"] = checkpointSha;
  manifest = std::move(checked);
  return true;
}

bool validateDetectionModelPackage(const DetectionRequest& request,
                                   const QString& modelRoot,
                                   QJsonObject& manifest, QString& error) {
  QString directory;
  if (!resolveDetectionPackageDirectory(
          modelRoot, QString::fromStdString(request.profile.id), directory,
          error))
    return false;
  return validateDetectionModelPackageDirectory(request, directory, manifest,
                                                error);
}

QString detectionModelsRoot() {
  return QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("detection-models"));
}

QString ffscnActivePackageRecordPath(const QString& modelRoot) {
  return QDir(modelRoot).filePath(kRecordName);
}

bool resolveDetectionPackageDirectory(const QString& modelRoot,
                                      const QString& profileId,
                                      QString& directory, QString& error) {
  if (profileId != QLatin1String(kFfscnProfile)) {
    directory = QDir(modelRoot).filePath(profileId);
    error.clear();
    return true;
  }
  QJsonObject record;
  bool present = false;
  if (!readActiveObject(modelRoot, record, present, error)) return false;
  if (!present) {
    directory = deployedFfscnDirectory(modelRoot);
    return true;
  }
  directory = QFileInfo(record["packageDirectory"].toString()).canonicalFilePath();
  return true;
}

bool readFfscnActivePackage(const QString& modelRoot, FfscnActivePackage& active,
                            QString& error) {
  QJsonObject record;
  bool present = false;
  if (!readActiveObject(modelRoot, record, present, error)) return false;
  active = {};
  if (!present) {
    active.packageDirectory =
        QFileInfo(deployedFfscnDirectory(modelRoot)).absoluteFilePath();
    active.explicitSelection = false;
    return true;
  }
  const auto directory =
      QFileInfo(record["packageDirectory"].toString()).canonicalFilePath();
  active.packageDirectory = directory;
  active.manifestPath = record["manifestPath"].toString();
  active.enginePath = record["enginePath"].toString();
  active.engineSha256 = record["engineSha256"].toString();
  active.checkpointSha256 = record["checkpointSha256"].toString();
  active.manifestSha256 = record["manifestSha256"].toString();
  active.explicitSelection = true;
  return true;
}

bool inspectFfscnEnginePackage(const QString& packageDirectory,
                               FfscnActivePackage& inspected, QString& error) {
  const QFileInfo directoryInfo(packageDirectory);
  if (packageDirectory.trimmed().isEmpty() || !directoryInfo.exists() ||
      !directoryInfo.isDir())
    return fail(error, "找不到引擎目录。");
  const auto directory = directoryInfo.canonicalFilePath();
  const auto manifestPath = QDir(directory).filePath(QStringLiteral("manifest.json"));
  if (!QFileInfo::exists(manifestPath)) {
    const auto toolPath =
        QDir(directory).filePath(QStringLiteral("tool-manifest.json"));
    if (QFileInfo::exists(toolPath)) {
      QString toolError;
      const auto tool = readJsonFile(toolPath, 1024 * 1024, toolError);
      if (!toolError.isEmpty() && tool.isEmpty()) return fail(error, toolError);
      return fail(error, toolManifestReason(tool));
    }
    return fail(error,
                "该目录没有 manifest.json，不能切换 FFSCN 引擎。"
                "原来的活动包没有改动。");
  }
  QString manifestError;
  const auto manifest = readJsonFile(manifestPath, 1024 * 1024, manifestError);
  if (!manifestError.isEmpty()) return fail(error, manifestError);
  if (manifest["schema"].toString() == QLatin1String(kToolSchema))
    return fail(error, toolManifestReason(manifest));
  const auto profileId = manifest["profileId"].toString();
  if (profileId == QLatin1String(kScnProfile))
    return fail(error, "不能把 SCN 包切换为 FFSCN 引擎。");
  if (profileId != QLatin1String(kFfscnProfile))
    return fail(error,
                "该清单的 profileId 不是 ffscn-gui-iq-v1，不能作为 FFSCN 引擎。");
  const auto precision = manifest["precision"].toString();
  if (precision == QLatin1String("FP16"))
    return fail(error,
                "该引擎精度是 FP16。当前生产校验只接受 FP32，"
                "这里不会放宽精度检查，因此不能切换。");
  if (precision != QLatin1String("FP32"))
    return fail(error, "该引擎精度不是 FP32，不能切换。");
  const auto engineSha = manifest["engine"].toObject()["sha256"].toString();
  if (!isLowercaseSha256(engineSha))
    return fail(error, "清单中的引擎 SHA-256 无效。");
  const auto* profile = findDetectionProfile(kFfscnProfile);
  if (!profile) return fail(error, "未注册 FFSCN 检测方案。");
  DetectionRequest request;
  request.profile = *profile;
  request.parameters.batchSize = 2;
  request.modelSha256 = engineSha.toStdString();
  QJsonObject checked;
  QString validationError;
  if (!validateDetectionModelPackageDirectory(request, directory, checked,
                                              validationError))
    return fail(error, "FFSCN 包未通过生产校验：" + validationError);
  inspected = {};
  inspected.packageDirectory = directory;
  inspected.manifestPath = QFileInfo(manifestPath).absoluteFilePath();
  inspected.enginePath = checked["engineAbsolutePath"].toString();
  inspected.engineSha256 = checked["loadedEngineSha256"].toString();
  inspected.checkpointSha256 = checked["loadedCheckpointSha256"].toString();
  inspected.manifestSha256 = checked["manifestSha256"].toString();
  inspected.explicitSelection = false;
  error.clear();
  return true;
}

bool activateFfscnEnginePackage(const QString& packageDirectory,
                                const QString& modelRoot,
                                FfscnActivePackage& activated, QString& error) {
  // Publish the pointer only after the package contract succeeds. Failure
  // leaves the previous record and every package file untouched.
  FfscnActivePackage inspected;
  if (!inspectFfscnEnginePackage(packageDirectory, inspected, error))
    return false;
  if (modelRoot.trimmed().isEmpty())
    return fail(error, "模型根目录无效，活动引擎没有切换。");
  if (!publishActiveRecord(modelRoot, inspected, error)) return false;
  activated = inspected;
  activated.explicitSelection = true;
  error.clear();
  return true;
}
}  // namespace signalstudio
