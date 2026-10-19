#include "infrastructure/detection_model_snapshot.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace signalstudio {
bool freezeDetectionModelSnapshot(DetectionRequest& request, QString& error) {
  if (request.profile.backend == DetectionBackendKind::Fixture) return true;
  if (request.profile.backend == DetectionBackendKind::Experimental17) {
    error = "实验配置的 131072 输入尚未通过原张量对照，不能作为正式检测运行";
    return false;
  }
  const QDir root(QCoreApplication::applicationDirPath() + "/detection-models");
  const auto path = root.filePath(QString::fromStdString(request.profile.id) +
                                  "/manifest.json");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > 65536) {
    error = "模型包未就绪：缺少对应方案的有效资产清单";
    return false;
  }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
  const auto manifest = document.object();
  const auto engine = manifest["engine"].toObject();
  const auto hash = engine["sha256"].toString();
  const auto engineName = engine["path"].toString();
  if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
      (manifest["schema"] != "signalstudio-detection-model-package-v1" &&
       manifest["schema"] != "signalstudio-detection-model-package-v2") ||
      manifest["profileId"] != QString::fromStdString(request.profile.id) ||
      manifest["profileVersion"].toInt() !=
          static_cast<int>(request.profile.version) ||
      manifest["precision"] != "FP32" ||
      !QRegularExpression("\\A[0-9a-f]{64}\\z").match(hash).hasMatch() ||
      engineName.isEmpty() || QFileInfo(engineName).fileName() != engineName ||
      engineName.contains('/') || engineName.contains('\\') ||
      !QFileInfo(QFileInfo(path).dir().filePath(engineName)).isFile()) {
    error = "模型包清单与当前已验证方案不符";
    return false;
  }
  request.modelManifestPath = QFileInfo(path).absoluteFilePath().toStdString();
  request.modelSha256 = hash.toStdString();
  error.clear();
  return true;
}
}  // namespace signalstudio
