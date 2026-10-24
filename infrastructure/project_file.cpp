#include "infrastructure/project_file.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <stdexcept>
#include <algorithm>

#include "infrastructure/detection_store.h"
#include "infrastructure/project_store.h"
#include "infrastructure/raw_sample_reader.h"
#include "infrastructure/sample_format_json.h"
#include "infrastructure/source_loader.h"

namespace signalstudio {
namespace {
thread_local std::function<bool()> cancellation;
struct CancellationScope {
  std::function<bool()> previous;
  explicit CancellationScope(std::function<bool()> value) : previous(cancellation) {
    if (value) cancellation = std::move(value);
  }
  ~CancellationScope() { cancellation = std::move(previous); }
};
QString q(const std::string& s) { return QString::fromStdString(s); }
void require(bool value, const QString& why) {
  if (cancellation && cancellation()) throw std::runtime_error("用户已取消工程 IO；当前会话未替换，已完成发布的转换文件和新伴随资产会保留");
  if (!value) throw std::runtime_error(why.toUtf8().constData());
}
QString hash(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString fileHash(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "资产无法读取：" + path);
  QCryptographicHash digest(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    const auto block = file.read(1024 * 1024);
    require(!block.isEmpty(), "资产校验读取失败：" + path);
    digest.addData(block);
  }
  require(file.error() == QFileDevice::NoError, "资产校验失败：" + path);
  return QString::fromLatin1(digest.result().toHex());
}
QJsonObject readObject(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly) && file.size() <= 32 * 1024 * 1024,
          "工程无法读取或超过 32 MiB：" + path);
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  require(file.error() == QFileDevice::NoError && error.error == QJsonParseError::NoError && document.isObject(),
          "工程 JSON 无效：" + error.errorString());
  return document.object();
}
bool inside(const QString& path, const QString& root) {
  const auto canonical = QFileInfo(path).canonicalFilePath();
  const auto base = QFileInfo(root).canonicalFilePath();
  return !canonical.isEmpty() && !base.isEmpty() &&
         canonical.startsWith(base + '/', Qt::CaseInsensitive);
}
QString safeAsset(const QDir& root, const QString& relative) {
  require(!relative.isEmpty() && !QDir::isAbsolutePath(relative) &&
              !relative.contains('\\') && !relative.split('/').contains("..") &&
              (relative.startsWith("annotations/") || relative.startsWith("detections/")),
          "资产相对路径越界");
  const auto path = root.filePath(relative);
  require(!QFileInfo(path).isSymLink() && inside(path, root.absolutePath()), "资产缺失或链接越界：" + relative);
  return path;
}
QString runManifest(const QString& id) {
  return "detections/" + hash(id.toUtf8()) + "/manifest.json";
}
QString requestHash(const DetectionRun& run) {
  return hash(QJsonDocument(encodeDetectionRequest(run.request)).toJson(QJsonDocument::Compact));
}
QJsonArray manifest(const QDir& root, const Project& project) {
  QJsonArray result;
  quint64 budget = 0;
  QMap<QString, const DetectionRun*> runs;
  QSet<QString> runDirectories;
  for (const auto& file : project.files) for (const auto& run : file.detectionRuns) {
    const auto path = runManifest(q(run.request.runId));
    require(!runs.contains(path), "重复的检测运行身份");
    runs.insert(path, &run);
    runDirectories.insert(path.section('/', 0, 1));
  }
  for (const auto& folder : {QString("annotations"), QString("detections")}) {
    QDirIterator it(root.filePath(folder), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const auto path = it.next();
      const auto relative = root.relativeFilePath(path);
      if (folder == "detections") {
        if (!runDirectories.contains(relative.section('/', 0, 1))) continue;
        const auto name = relative.section('/', 2);
        // Runtime scratch files are not published evidence. Only the store's
        // atomic index and content-addressed chunks/heads are project assets.
        if (!QRegularExpression("\\A(?:manifest\\.json|chunk-[0-9a-f]{64}\\.json|heads-[0-9a-f]{64}\\.f32)\\z").match(name).hasMatch()) continue;
      } else if (!relative.endsWith(".json")) continue;
      safeAsset(root, relative);
      budget += static_cast<quint64>(it.fileInfo().size());
      require(result.size() < 20000 && budget <= 2ull * 1024 * 1024 * 1024,
              "伴随资产超过 20000 文件或 2 GiB 校验预算");
      if (runs.contains(relative)) {
        DetectionRun current;
        QString error;
        require(decodeDetectionRun(readObject(path), current, error), error);
        require(encodeDetectionRequest(current.request) == encodeDetectionRequest(runs[relative]->request),
                "检测运行的不可变请求与工程不一致");
        if (!detectionIsTerminal(current.lifecycle)) {
          DetectionRun checked;
          require(loadDetectionRun(root.absolutePath(), q(current.request.runId), checked, error), error);
          require(requestHash(checked) == requestHash(current), "检测请求在保存期间发生变化");
          result.append(QJsonObject{{"assetId", hash(relative.toUtf8())}, {"path", relative},
              {"integrity", "detection-live-v1"}, {"runId", q(current.request.runId)},
              {"requestSha256", requestHash(checked)},
              {"minimumChunks", encodeDetectionRun(checked)["chunks"]},
              {"minimumCompletedWindows", QString::number(checked.completedWindows)},
              {"minimumReadFrames", QString::number(checked.readFrames)},
              {"minimumResultCount", QString::number(checked.resultCount)}});
          continue;
        }
      }
      result.append(QJsonObject{{"assetId", hash(relative.toUtf8())}, {"path", relative},
          {"bytes", QString::number(it.fileInfo().size())}, {"sha256", fileHash(path)}});
    }
  }
  return result;
}
QSet<QString> verifyManifest(const QDir& root, const QJsonValue& value) {
  require(value.isArray() && value.toArray().size() <= 20000, "资产清单无效或超过预算");
  QSet<QString> seen;
  QSet<QString> accounted;
  QJsonArray live;
  quint64 budget = 0;
  const auto account = [&](const QString& relative, quint64 bytes) {
    if (accounted.contains(relative)) return;
    require(bytes <= 2ull * 1024 * 1024 * 1024 && accounted.size() < 20000, "伴随资产超过文件数/大小预算");
    budget += bytes;
    require(budget <= 2ull * 1024 * 1024 * 1024, "资产超过读取预算");
    accounted.insert(relative);
  };
  for (const auto& item : value.toArray()) {
    require(item.isObject(), "资产清单项必须为对象");
    const auto entry = item.toObject();
    const auto relative = entry["path"].toString();
    require(!seen.contains(relative) && entry["assetId"].toString() == hash(relative.toUtf8()), "资产身份无效或重复");
    seen.insert(relative);
    const auto path = safeAsset(root, relative);
    if (entry["integrity"] == "detection-live-v1") {
      require(entry["runId"].isString() && relative == runManifest(entry["runId"].toString()) &&
                  entry["minimumChunks"].isArray() && entry["minimumChunks"].toArray().size() <= 20000 &&
                  QRegularExpression("\\A[0-9a-f]{64}\\z").match(entry["requestSha256"].toString()).hasMatch() &&
                  QFileInfo(path).isFile(), "可变检测索引身份/前缀无效");
      account(relative, static_cast<quint64>(QFileInfo(path).size()));
      live.append(entry);
      continue;
    }
    require(!entry.contains("integrity"), "未知资产完整性规则");
    bool ok = false;
    const auto bytes = entry["bytes"].toString().toULongLong(&ok);
    require(ok, "资产大小无效");
    account(relative, bytes);
    require(static_cast<quint64>(QFileInfo(path).size()) == bytes && fileHash(path) == entry["sha256"].toString(), "资产长度或 SHA-256 不符：" + relative);
  }
  for (const auto& value : live) {
    const auto entry = value.toObject();
    DetectionRun run;
    QString error;
    require(loadDetectionRun(root.absolutePath(), entry["runId"].toString(), run, error), error);
    require(requestHash(run) == entry["requestSha256"].toString(), "检测运行的不可变请求已改变");
    const auto before = entry["minimumChunks"].toArray();
    const auto after = encodeDetectionRun(run)["chunks"].toArray();
    require(after.size() >= before.size(), "已保存的检测前缀被删除");
    for (qsizetype i = 0; i < before.size(); ++i)
      require(before[i].isObject() && before[i] == after[i], "已保存的检测分片被替换或重排");
    for (const auto& field : {QString("CompletedWindows"), QString("ReadFrames"), QString("ResultCount")}) {
      bool ok = false;
      const auto minimum = entry["minimum" + field].toString().toULongLong(&ok);
      const auto current = field == "CompletedWindows" ? run.completedWindows : field == "ReadFrames" ? run.readFrames : run.resultCount;
      require(ok && current >= minimum, "检测持久进度低于已保存前缀");
    }
    const auto accountCurrent = [&](const QString& relative) {
      const auto path = safeAsset(root, relative);
      require(relative.startsWith(runManifest(q(run.request.runId)).section('/', 0, 1) + '/') && QFileInfo(path).isFile(), "检测分片属于其它运行");
      account(relative, static_cast<quint64>(QFileInfo(path).size()));
    };
    for (const auto& chunk : run.chunks) accountCurrent(q(chunk.relativePath));
    require(visitDetectionChunks(root.absolutePath(), run, [&](const QJsonObject& chunk, QString&) {
      for (const auto& item : chunk["mappings"].toArray()) {
        const auto mapping = item.toObject();
        if (mapping.contains("headEvidence")) accountCurrent(mapping["headEvidence"].toObject()["path"].toString());
      }
      return true;
    }, error), error);
  }
  return seen;
}
void copyDetectionAssets(const Project& project, const QString& from, const QDir& to) {
  if (QDir(from).absolutePath().compare(to.absolutePath(), Qt::CaseInsensitive) == 0) return;
  for (const auto& file : project.files) for (const auto& run : file.detectionRuns) {
    QString error;
    DetectionRun checked;
    require(loadDetectionRun(from, q(run.request.runId), checked, error), error);
    const auto relative = "detections/" + hash(QByteArray::fromStdString(run.request.runId));
    QDirIterator it(QDir(from).filePath(relative), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    int count = 0;
    quint64 bytes = 0;
    while (it.hasNext()) {
      const auto source = it.next();
      const auto name = QDir(from).relativeFilePath(source);
      safeAsset(QDir(from), name);
      bytes += static_cast<quint64>(it.fileInfo().size());
      require(++count <= 20000 && bytes <= 2ull * 1024 * 1024 * 1024, "检测资产超过复制预算");
      const auto destination = to.filePath(name);
      require(QDir().mkpath(QFileInfo(destination).absolutePath()), "无法创建目标资产目录");
      if (!QFileInfo::exists(destination)) {
        QFile input(source);
        QSaveFile output(destination);
        require(input.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly), "检测资产复制失败");
        while (!input.atEnd()) {
          const auto block = input.read(1024 * 1024);
          require(!block.isEmpty() && output.write(block) == block.size(), "检测资产复制中断");
        }
        require(input.error() == QFileDevice::NoError && output.commit(), "检测资产复制发布失败");
      }
      require(fileHash(destination) == fileHash(source), "复制目标资产冲突或损坏");
    }
  }
}
}

QString ProjectFileCodec::signature(const QString& path, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return {}; }
  const auto size = file.size();
  QCryptographicHash digest(QCryptographicHash::Sha256);
  digest.addData(QByteArray::number(size));
  digest.addData(file.read(65536));
  if (size > 65536) {
    if (!file.seek(std::max<qint64>(65536, size - 65536))) { error = "源文件签名 seek 失败"; return {}; }
    digest.addData(file.read(65536));
  }
  if (file.error() != QFileDevice::NoError || file.size() != size) { error = "源文件签名读取失败或文件已变化"; return {}; }
  error.clear();
  return QString::fromLatin1(digest.result().toHex());
}

QString ProjectFileCodec::assetDirectory(const QString& path, const Project& project) {
  if (!project.assetDirectory.empty()) return q(project.assetDirectory);
  return QFileInfo(path).absoluteFilePath() + ".assets/" + q(project.projectId);
}

bool ProjectFileCodec::save(const QString& path, Project& project, QString& error, AssetSaveMode mode, std::function<bool()> cancelled) {
  CancellationScope cancellationScope(std::move(cancelled));
  error.clear();
  try {
    require(path.endsWith(".ssproj", Qt::CaseInsensitive), "请保存为 .ssproj 工程文件；旧 JSON 仅允许显式导入转换");
    require(QFileInfo(path).absoluteDir().exists(), "工程目标目录不存在");
    Project candidate = project;
    if (candidate.projectId.empty()) candidate.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const bool existing = QFileInfo::exists(path);
    QJsonObject previous;
    if (existing) {
      previous = readObject(path);
      require(previous["magic"] == "SignalStudioProject" && previous["schemaVersion"].toDouble() == 1 &&
                  previous["projectId"].toString() == q(candidate.projectId),
              "目标属于其它工程或不是有效工程；请选择新文件名，未覆盖原文件");
    }
    const auto previousAssets = q(candidate.assetDirectory);
    const auto projectDir = QFileInfo(path).absoluteDir();
    auto effectiveMode = mode;
    QString preservedReference, assets;
    if (mode == AssetSaveMode::Preserve && existing) {
      const auto previousMode = previous["assetMode"].toString();
      require(previousMode == "copy" || previousMode == "reference", "现有工程资产模式无效");
      preservedReference = previous["assetDirectory"].toString();
      require(!preservedReference.isEmpty(), "现有工程缺少资产位置");
      assets = QDir::isAbsolutePath(preservedReference) ? preservedReference : projectDir.filePath(preservedReference);
      require(QDir(assets).exists() && (previousAssets.isEmpty() ||
                  QDir(assets).absolutePath().compare(QDir(previousAssets).absolutePath(), Qt::CaseInsensitive) == 0),
              "普通保存不能切换或重建资产根；请使用明确的复制/引用另存");
      effectiveMode = previousMode == "reference" ? AssetSaveMode::Reference : AssetSaveMode::Copy;
    } else {
      require(mode != AssetSaveMode::Preserve || previousAssets.isEmpty(),
              "原工程文件缺失或目标已改变；请明确选择复制/引用另存");
      effectiveMode = mode == AssetSaveMode::Preserve ? AssetSaveMode::Copy : mode;
      if (effectiveMode == AssetSaveMode::Reference) {
        require(!previousAssets.isEmpty() && QDir(previousAssets).exists(), "没有可引用的现有资产目录");
        assets = previousAssets;
      } else assets = QFileInfo(path).absoluteFilePath() + ".assets/" + q(candidate.projectId);
    }
    require(QDir().mkpath(assets) && !QFileInfo(assets).isSymLink(), "无法创建工程伴随资产目录");
    const QDir root(assets);
    if (!previousAssets.isEmpty()) copyDetectionAssets(candidate, previousAssets, root);
    QJsonArray references;
    for (auto& file : candidate.files) {
      auto& metadata = file.metadata;
      if (metadata.demo || q(metadata.path).startsWith(":/")) continue;
      const auto source = QFileInfo(q(metadata.path)).absoluteFilePath();
      QString why;
      auto digest = signature(source, why);
      if (digest.isEmpty()) {
        require(!metadata.referenceSignature.empty(), "源文件缺失且没有可恢复身份：" + source);
        digest = q(metadata.referenceSignature);
      } else {
        require(metadata.referenceSignature.empty() || (digest == q(metadata.referenceSignature) && metadata.referenceBytes == static_cast<quint64>(QFileInfo(source).size())), "源文件已改变，请重新导入而非覆盖既有标记：" + source);
        metadata.referenceBytes = static_cast<quint64>(QFileInfo(source).size());
        metadata.referenceSignature = digest.toStdString();
      }
      const auto relativeCandidate = projectDir.relativeFilePath(source);
      // Windows cannot express a path relative to another drive. Keep that
      // absence explicit and retain the verified absolute reference.
      const auto relative = QDir::isAbsolutePath(relativeCandidate) ? QString{} : relativeCandidate;
      references.append(QJsonObject{{"fileId", q(metadata.id)}, {"absolutePath", source},
          {"relativePath", relative}, {"bytes", QString::number(metadata.referenceBytes)},
          {"signature", digest}, {"signatureMethod", "size+first-last-64KiB-sha256-v1"},
          {"sampleFormat", encodeSampleFormat(metadata.sampleFormat)}, {"sampleRateHz", metadata.sampleRateHz},
          {"centerFrequencyHz", metadata.centerFrequencyHz}, {"sampleCount", QString::number(metadata.sampleCount)}});
    }
    const auto statePath = root.filePath("state-v6.json");
    QJsonObject payload;
    require(ProjectStore::encodeDocument(statePath, candidate, payload, error), error);
    const auto assetsRef = !preservedReference.isEmpty() ? preservedReference :
        effectiveMode == AssetSaveMode::Reference ? root.absolutePath() : projectDir.relativeFilePath(root.absolutePath());
    const QJsonObject container{{"magic", "SignalStudioProject"}, {"schemaVersion", 1},
        {"projectId", q(candidate.projectId)}, {"assetDirectory", assetsRef},
        {"assetMode", effectiveMode == AssetSaveMode::Reference ? "reference" : "copy"},
        {"assets", manifest(root, candidate)}, {"sourceReferences", references}, {"state", payload}};
    const auto bytes = QJsonDocument(container).toJson(QJsonDocument::Indented);
    require(bytes.size() <= 32 * 1024 * 1024, "工程超过 32 MiB");
    QSaveFile output(path);
    require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size(), "工程写入失败：" + output.errorString());
    // The last cancellation check precedes the atomic publication point.
    require(true, {});
    if (!output.commit()) throw std::runtime_error(("工程原子保存失败：" + output.errorString()).toUtf8().constData());
    candidate.assetDirectory = root.absolutePath().toStdString();
    project = std::move(candidate);
    return true;
  } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
}

bool ProjectFileCodec::load(const QString& path, Project& target, QString& error, std::function<bool()> cancelled) {
  CancellationScope cancellationScope(std::move(cancelled));
  error.clear();
  try {
    const auto container = readObject(path);
    require(container["magic"] == "SignalStudioProject" && container["schemaVersion"].isDouble() && container["schemaVersion"].toDouble() == 1,
            "不是受支持的 .ssproj；旧 v6 JSON 请使用显式导入转换");
    const auto id = container["projectId"].toString();
    require(!QUuid(id).isNull(), "工程 projectId 无效");
    const auto assetRef = container["assetDirectory"].toString();
    require(!assetRef.isEmpty() && (container["assetMode"] == "copy" || container["assetMode"] == "reference"), "缺少伴随资产位置或模式无效");
    const auto projectDir = QFileInfo(path).absoluteDir();
    const QDir root(QDir::isAbsolutePath(assetRef) ? assetRef : projectDir.filePath(assetRef));
    require(root.exists() && !QFileInfo(root.absolutePath()).isSymLink(), "伴随资产缺失或目录链接无效");
    const auto declaredAssets = verifyManifest(root, container["assets"]);
    Project candidate;
    require(container["state"].isObject() && ProjectStore::decodeDocument(container["state"].toObject(), root.filePath("state-v6.json"), candidate, error), error.isEmpty() ? "缺少工程状态" : error);
    candidate.projectId = id.toStdString();
    candidate.assetDirectory = root.absolutePath().toStdString();
    for (const auto& file : candidate.files) for (const auto& run : file.detectionRuns)
      require(declaredAssets.contains(runManifest(q(run.request.runId))), "检测运行缺少资产清单锚点");
    require(container["sourceReferences"].isArray() && container["sourceReferences"].toArray().size() <= 150, "源引用清单无效");
    QSet<QString> seen;
    for (const auto& value : container["sourceReferences"].toArray()) {
      require(value.isObject(), "源引用必须为对象");
      const auto reference = value.toObject();
      const auto fileId = reference["fileId"].toString();
      require(!seen.contains(fileId), "重复源引用");
      seen.insert(fileId);
      auto it = std::find_if(candidate.files.begin(), candidate.files.end(), [&](const auto& file) { return q(file.metadata.id) == fileId; });
      require(it != candidate.files.end() && !it->metadata.demo, "源引用文件身份无效");
      auto& source = it->metadata;
      bool ok = false;
      source.referenceBytes = reference["bytes"].toString().toULongLong(&ok);
      source.referenceSignature = reference["signature"].toString().toStdString();
      require(ok && reference["signatureMethod"] == "size+first-last-64KiB-sha256-v1" && QRegularExpression("\\A[0-9a-f]{64}\\z").match(q(source.referenceSignature)).hasMatch() &&
              reference["sampleFormat"].toObject() == encodeSampleFormat(source.sampleFormat) && reference["sampleRateHz"].toDouble() == source.sampleRateHz && reference["centerFrequencyHz"].toDouble() == source.centerFrequencyHz && reference["sampleCount"].toString() == QString::number(source.sampleCount), "源引用物理格式与工程状态不一致");
      const auto relative = reference["relativePath"].toString();
      const auto absolute = reference["absolutePath"].toString();
      require((relative.isEmpty() || !QDir::isAbsolutePath(relative)) && QDir::isAbsolutePath(absolute), "源引用路径无效");
      QString why;
      bool resolved = false;
      const QStringList choices = relative.isEmpty() ? QStringList{absolute}
                                                     : QStringList{projectDir.filePath(relative), absolute};
      for (const auto& choice : choices) {
        if (QFileInfo::exists(choice) && relocate(source, choice, why)) { resolved = true; break; }
      }
      if (!resolved) {
        source.path = absolute.toStdString();
        source.availability.status = LoadStatus::Failed;
        source.availability.availableSamples = 0;
        source.availability.error = "源文件缺失或身份不符；请右键重新定位：" + why.toStdString();
      }
    }
    for (const auto& file : candidate.files)
      require(file.metadata.demo || q(file.metadata.path).startsWith(":/") || seen.contains(q(file.metadata.id)), "真实源文件缺少引用身份");
    target = std::move(candidate);
    return true;
  } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
}

bool ProjectFileCodec::relocate(FileMetadata& source, const QString& path, QString& error) {
  const auto digest = signature(path, error);
  if (digest.isEmpty()) return false;
  if (source.referenceSignature.empty() || digest != q(source.referenceSignature) || static_cast<quint64>(QFileInfo(path).size()) != source.referenceBytes) {
    error = "长度或内容签名不符，未更改标记绑定"; return false;
  }
  RawSampleReader reader;
  if (!reader.open(path, source.sampleFormat, error) || reader.sampleCount() != source.sampleCount) {
    error = "样本格式或物理长度不符"; return false;
  }
  source.path = QFileInfo(path).absoluteFilePath().toStdString();
  source.availability.fingerprint = iqSourceFingerprint(path).toStdString();
  error.clear();
  return true;
}

bool ProjectFileCodec::importLegacy(const QString& legacy, const QString& destination, Project& target, QString& error,
                                   std::function<bool()> cancelled) {
  CancellationScope cancellationScope(std::move(cancelled));
  try {
  require(true, {});
  if (QFileInfo::exists(destination)) { error = "转换目标已存在；请选择新的 .ssproj 文件"; return false; }
  Project candidate;
  if (!ProjectStore::load(legacy, candidate, error)) return false;
  candidate.assetDirectory = QFileInfo(legacy).absolutePath().toStdString();
  for (auto& file : candidate.files) if (!file.metadata.demo && QDir::isRelativePath(q(file.metadata.path)) && !q(file.metadata.path).startsWith(":/"))
    file.metadata.path = QFileInfo(legacy).absoluteDir().absoluteFilePath(q(file.metadata.path)).toStdString();
  if (!save(destination, candidate, error, AssetSaveMode::Copy)) return false;
  Project verified;
  if (!load(destination, verified, error)) return false;
  target = std::move(verified);
  return true;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    return false;
  }
}
}  // namespace signalstudio
