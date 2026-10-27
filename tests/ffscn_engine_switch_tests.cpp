#include "infrastructure/detection_model_backend.h"
#include "infrastructure/ffscn_engine_package.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace signalstudio;

namespace {
constexpr auto kEpoch1200 =
    "acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0";

QByteArray readBytes(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QString sha256(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject head(const char* name) {
  return {{"semantic", name},
          {"name", name},
          {"dtype", "float32"},
          {"shape", QJsonArray{0, 1, 1, 0}},
          {"stride", 4}};
}

struct BuiltPackage {
  QString directory;
  QString manifestPath;
  QString enginePath;
  QString engineSha;
  QString checkpoint;
  QByteArray manifestBytes;
  QByteArray engineBytes;
};

bool buildPackage(const QString& directory, const QByteArray& engineBytes,
                  const QString& checkpoint, BuiltPackage& built) {
  if (!QDir().mkpath(directory)) return false;
  const auto canonical = QFileInfo(directory).canonicalFilePath();
  if (canonical.isEmpty()) return false;
  const auto engineName = QStringLiteral("ffscn_custom_fp32.engine");
  const auto enginePath = QDir(canonical).filePath(engineName);
  const auto manifestPath = QDir(canonical).filePath("manifest.json");
  const auto engineSha = sha256(engineBytes);
  const QJsonObject manifest{
      {"schema", "signalstudio-detection-model-package-v2"},
      {"profileId", "ffscn-gui-iq-v1"},
      {"profileVersion", 1},
      {"adaptationVersion", 2},
      {"precision", "FP32"},
      {"checkpointSha256", checkpoint},
      {"engine", QJsonObject{{"path", engineName},
                             {"bytes", static_cast<int>(engineBytes.size())},
                             {"sha256", engineSha}}},
      {"runtime", QJsonObject{{"tensorRt", "10.11.0.33"},
                              {"cudaRuntime", 12090},
                              {"gpu", "NVIDIA GeForce RTX 5060 Laptop GPU"},
                              {"computeCapability", QJsonArray{12, 0}},
                              {"hardwareCompatibility", "none"},
                              {"versionCompatible", false}}},
      {"input", QJsonObject{{"name", "spectrum"},
                            {"dtype", "float32"},
                            {"layout", "contiguous-NCHW"},
                            {"minShape", QJsonArray{1, 1, 1, 2048}},
                            {"maxShape", QJsonArray{2, 1, 10, 65536}},
                            {"downNums", 11},
                            {"frequencyPowerOfTwo", true}}},
      {"heads", QJsonArray{head("hm"), head("bw"), head("off")}}};
  const auto manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
  if (!writeBytes(enginePath, engineBytes) ||
      !writeBytes(manifestPath, manifestBytes))
    return false;
  built.directory = canonical;
  built.manifestPath = manifestPath;
  built.enginePath = enginePath;
  built.engineSha = engineSha;
  built.checkpoint = checkpoint;
  built.manifestBytes = manifestBytes;
  built.engineBytes = engineBytes;
  return true;
}

DetectionRequest ffscnRequest(const QString& engineSha) {
  DetectionRequest request;
  request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  request.parameters.batchSize = 2;
  request.modelSha256 = engineSha.toStdString();
  return request;
}
}  // namespace

class FfscnEngineSwitchTests : public QObject {
  Q_OBJECT
 private slots:
  void validSelectionUpdatesActivePath();
  void invalidManifestKeepsPreviousPackage();
};

void FfscnEngineSwitchTests::validSelectionUpdatesActivePath() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto modelRoot = QDir(root.path()).filePath("detection-models");
  const QByteArray previousManifest("previous-manifest-keep");
  const QByteArray previousEngine("previous-engine-keep");
  const auto previousManifestPath =
      QDir(modelRoot).filePath("ffscn-gui-iq-v1/manifest.json");
  const auto previousEnginePath =
      QDir(modelRoot).filePath("ffscn-gui-iq-v1/legacy.engine");
  QVERIFY(writeBytes(previousManifestPath, previousManifest));
  QVERIFY(writeBytes(previousEnginePath, previousEngine));

  const auto checkpoint =
      QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  QVERIFY(checkpoint != QLatin1String(kEpoch1200));
  QVERIFY(checkpoint.size() == 64);
  BuiltPackage package;
  QVERIFY(buildPackage(QDir(root.path()).filePath("packages/custom"),
                       "replacement-engine", checkpoint, package));

  FfscnActivePackage activated;
  QString error;
  QVERIFY2(activateFfscnEnginePackage(package.directory, modelRoot, activated,
                                      error),
           qPrintable(error));
  QCOMPARE(activated.packageDirectory, package.directory);
  QCOMPARE(activated.engineSha256, package.engineSha);
  QCOMPARE(activated.checkpointSha256, checkpoint);
  QVERIFY(activated.explicitSelection);
  QVERIFY(!activated.manifestSha256.isEmpty());

  FfscnActivePackage recorded;
  QVERIFY(readFfscnActivePackage(modelRoot, recorded, error));
  QCOMPARE(recorded.packageDirectory, package.directory);
  QCOMPARE(recorded.engineSha256, package.engineSha);
  QCOMPARE(recorded.checkpointSha256, checkpoint);
  QVERIFY(recorded.explicitSelection);

  QString resolved;
  QVERIFY(resolveDetectionPackageDirectory(modelRoot, "ffscn-gui-iq-v1",
                                           resolved, error));
  QCOMPARE(resolved, package.directory);
  QVERIFY(resolveDetectionPackageDirectory(modelRoot, "scn-max-psd-v1",
                                           resolved, error));
  QCOMPARE(QFileInfo(resolved).absoluteFilePath(),
           QFileInfo(QDir(modelRoot).filePath("scn-max-psd-v1")).absoluteFilePath());

  QJsonObject checked;
  auto request = ffscnRequest(package.engineSha);
  QVERIFY2(validateDetectionModelPackage(request, modelRoot, checked, error),
           qPrintable(error));
  QCOMPARE(checked["loadedEngineSha256"].toString(), package.engineSha);
  QCOMPARE(checked["loadedCheckpointSha256"].toString(), checkpoint);
  QCOMPARE(QFileInfo(checked["engineAbsolutePath"].toString()).canonicalPath(),
           package.directory);
  QCOMPARE(readBytes(previousManifestPath), previousManifest);
  QCOMPARE(readBytes(previousEnginePath), previousEngine);
  QCOMPARE(readBytes(package.manifestPath), package.manifestBytes);
  QCOMPARE(readBytes(package.enginePath), package.engineBytes);
}

void FfscnEngineSwitchTests::invalidManifestKeepsPreviousPackage() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto modelRoot = QDir(root.path()).filePath("detection-models");
  const QByteArray previousManifest("previous-manifest-keep");
  const QByteArray previousEngine("previous-engine-keep");
  const auto previousManifestPath =
      QDir(modelRoot).filePath("ffscn-gui-iq-v1/manifest.json");
  const auto previousEnginePath =
      QDir(modelRoot).filePath("ffscn-gui-iq-v1/legacy.engine");
  QVERIFY(writeBytes(previousManifestPath, previousManifest));
  QVERIFY(writeBytes(previousEnginePath, previousEngine));

  FfscnActivePackage rejected;
  QString error;
  const auto missing = QDir(root.path()).filePath("missing-package");
  QVERIFY(!activateFfscnEnginePackage(missing, modelRoot, rejected, error));
  QVERIFY(error.contains("找不到引擎目录"));
  QVERIFY(!QFileInfo::exists(ffscnActivePackageRecordPath(modelRoot)));

  const auto empty = QDir(root.path()).filePath("empty-package");
  QVERIFY(QDir().mkpath(empty));
  QVERIFY(!activateFfscnEnginePackage(empty, modelRoot, rejected, error));
  QVERIFY(error.contains("manifest.json"));
  QVERIFY(!QFileInfo::exists(ffscnActivePackageRecordPath(modelRoot)));

  BuiltPackage current;
  const auto checkpoint =
      QStringLiteral("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");
  QVERIFY(buildPackage(QDir(root.path()).filePath("packages/current"),
                       "current-engine", checkpoint, current));
  QVERIFY2(activateFfscnEnginePackage(current.directory, modelRoot, rejected,
                                      error),
           qPrintable(error));
  const auto pointer = readBytes(ffscnActivePackageRecordPath(modelRoot));
  QVERIFY(!pointer.isEmpty());

  const auto invalid = QDir(root.path()).filePath("packages/invalid");
  QVERIFY(writeBytes(QDir(invalid).filePath("manifest.json"),
                     "{not-json"));
  QVERIFY(!activateFfscnEnginePackage(invalid, modelRoot, rejected, error));
  QVERIFY(error.contains("JSON") || error.contains("生产校验"));

  const auto scn = QDir(root.path()).filePath("packages/scn");
  QVERIFY(writeBytes(
      QDir(scn).filePath("manifest.json"),
      QJsonDocument(QJsonObject{{"profileId", "scn-max-psd-v1"},
                                {"precision", "FP32"}})
          .toJson()));
  QVERIFY(!activateFfscnEnginePackage(scn, modelRoot, rejected, error));
  QVERIFY(error.contains("SCN"));

  BuiltPackage fp16;
  QVERIFY(buildPackage(QDir(root.path()).filePath("packages/fp16"),
                       "fp16-engine", checkpoint, fp16));
  QJsonObject fp16Manifest =
      QJsonDocument::fromJson(fp16.manifestBytes).object();
  fp16Manifest["precision"] = "FP16";
  QVERIFY(writeBytes(fp16.manifestPath,
                     QJsonDocument(fp16Manifest).toJson(QJsonDocument::Compact)));
  QVERIFY(!activateFfscnEnginePackage(fp16.directory, modelRoot, rejected, error));
  QVERIFY(error.contains("FP16"));
  QVERIFY(error.contains("FP32"));

  const auto tool = QDir(root.path()).filePath("packages/tool");
  QVERIFY(writeBytes(
      QDir(tool).filePath("tool-manifest.json"),
      QJsonDocument(QJsonObject{
                        {"schema", "signalstudio-ffscn-engine-tool-v1"},
                        {"precision", "FP16"}})
          .toJson()));
  QVERIFY(!activateFfscnEnginePackage(tool, modelRoot, rejected, error));
  QVERIFY(error.contains("FP16"));

  const auto toolFp32 = QDir(root.path()).filePath("packages/tool-fp32");
  QVERIFY(writeBytes(
      QDir(toolFp32).filePath("tool-manifest.json"),
      QJsonDocument(QJsonObject{
                        {"schema", "signalstudio-ffscn-engine-tool-v1"},
                        {"precision", "FP32"}})
          .toJson()));
  QVERIFY(!activateFfscnEnginePackage(toolFp32, modelRoot, rejected, error));
  QVERIFY(error.contains("tool-manifest.json"));

  QCOMPARE(readBytes(ffscnActivePackageRecordPath(modelRoot)), pointer);
  QCOMPARE(readBytes(previousManifestPath), previousManifest);
  QCOMPARE(readBytes(previousEnginePath), previousEngine);
  QCOMPARE(readBytes(current.manifestPath), current.manifestBytes);
  QCOMPARE(readBytes(current.enginePath), current.engineBytes);
  FfscnActivePackage still;
  QVERIFY(readFfscnActivePackage(modelRoot, still, error));
  QCOMPARE(still.packageDirectory, current.directory);
  QCOMPARE(still.engineSha256, current.engineSha);
  QCOMPARE(still.checkpointSha256, checkpoint);
}
QTEST_GUILESS_MAIN(FfscnEngineSwitchTests)
#include "ffscn_engine_switch_tests.moc"
