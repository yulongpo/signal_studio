#include "infrastructure/annotation_store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <stdexcept>
#include <vector>

#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"

namespace signalstudio {
namespace {
constexpr qsizetype shardBytes = 8 * 1024 * 1024, shardRecords = 4096;
constexpr qint64 totalBytes = 256 * 1024 * 1024;
QString digest(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString fileKey(const FileState& file) {
  return digest(QByteArray::fromStdString(file.metadata.id)).left(24);
}
void require(bool b, const QString& message) {
  if (!b) throw std::runtime_error(message.toUtf8().constData());
}
quint64 index(const QJsonValue& v) {
  require(v.isString(), "索引必须为uint64字符串");
  const auto s = v.toString();
  require(QRegularExpression("^(0|[1-9][0-9]{0,19})$").match(s).hasMatch() &&
              !s.contains('\n') && !s.contains('\r'),
          "索引非规范");
  bool ok = false;
  const auto n = s.toULongLong(&ok);
  require(ok, "索引溢出");
  return n;
}
bool inside(const QString& path, const QString& root) {
  const auto normalizedPath = QDir::fromNativeSeparators(path);
  const auto prefix = QDir::fromNativeSeparators(root) + '/';
#ifdef Q_OS_WIN
  return normalizedPath.startsWith(prefix, Qt::CaseInsensitive);
#else
  return normalizedPath.startsWith(prefix);
#endif
}
void validateImportedSnapshot(const AnnotationSourceSet& source) {
  if (source.sourceKind != AnnotationSource::ImportedLabel) return;
  require(digest(QByteArray::fromStdString(source.originalText)) ==
              QString::fromStdString(source.sourceHash),
          "导入来源原文与原始 SHA-256 不一致");
  require(source.supplementalOriginalText.empty()
              ? source.supplementalHash.empty()
              : digest(QByteArray::fromStdString(
                    source.supplementalOriginalText)) ==
                    QString::fromStdString(source.supplementalHash),
          "配套 JSON 原文与原始 SHA-256 不一致");
}
QJsonObject writeSourceSnapshot(const QDir& root, const QString& relative,
                                const FileState& file,
                                const AnnotationSourceSet& source,
                                qint64& written) {
  validateImportedSnapshot(source);
  const auto bytes =
      QJsonDocument(
          QJsonObject{{"schema", "signal-studio-annotation-source"},
                      {"version", 1},
                      {"fileId", QString::fromStdString(file.metadata.id)},
                      {"sourceSet", encodeSourceSet(source)}})
          .toJson(QJsonDocument::Compact);
  require(bytes.size() <= 32 * 1024 * 1024, "单个来源快照超过 32 MiB");
  written += bytes.size();
  require(written <= totalBytes, "标记与来源快照超过 256 MiB 总预算");
  const auto hash = digest(bytes);
  const auto name = relative + "/source-" + hash + ".json";
  const auto path = root.filePath(name);
  QFile previous(path);
  if (previous.exists()) {
    require(previous.open(QIODevice::ReadOnly) &&
                previous.size() == bytes.size() &&
                digest(previous.readAll()) == hash,
            "已有来源快照损坏，请先修复该工程");
  } else {
    QSaveFile output(path);
    require(output.open(QIODevice::WriteOnly) &&
                output.write(bytes) == bytes.size() && output.commit(),
            "来源快照写入失败");
  }
  return {{"path", name},
          {"sha256", hash},
          {"bytes", QString::number(bytes.size())}};
}
QByteArray compactDocument(const QJsonDocument& document) {
  auto json = document.toJson(QJsonDocument::Compact);
  while (!json.isEmpty() && (json.endsWith('\n') || json.endsWith('\r')))
    json.chop(1);
  return json;
}
QJsonObject readSourceSnapshot(const QDir& root, const QString& canonicalRoot,
                               const FileState& file, const QJsonObject& ref,
                               QSet<QString>& paths, qint64& bytesRead) {
  const auto hash = ref["sha256"].toString();
  const auto name = ref["path"].toString();
  require(QRegularExpression("\\A[0-9a-f]{64}\\z").match(hash).hasMatch() &&
              name == "annotations/" + fileKey(file) + "/source-" + hash +
                          ".json" &&
              !paths.contains(name),
          "来源快照路径越界、重复或 hash 无效");
  paths.insert(name);
  const auto size = index(ref["bytes"]);
  require(size <= 32 * 1024 * 1024, "来源快照超过 32 MiB");
  bytesRead += static_cast<qint64>(size);
  require(bytesRead <= totalBytes, "来源与标记读取超过 256 MiB 总预算");
  const auto path = root.filePath(name);
  require(inside(QFileInfo(path).canonicalFilePath(), canonicalRoot),
          "来源快照缺失或路径越界");
  QFile input(path);
  require(input.open(QIODevice::ReadOnly) &&
              static_cast<quint64>(input.size()) == size,
          "来源快照字节数不符或不可读取");
  const auto data = input.readAll();
  require(input.error() == QFileDevice::NoError && digest(data) == hash,
          "来源快照内容 hash 不符");
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(data, &parseError);
  require(parseError.error == QJsonParseError::NoError && document.isObject(),
          "来源快照 JSON 无效");
  const auto object = document.object();
  require(object["schema"] == "signal-studio-annotation-source" &&
              object["version"] == 1 &&
              object["fileId"].toString() ==
                  QString::fromStdString(file.metadata.id) &&
              object["sourceSet"].isObject(),
          "来源快照身份不符");
  return object["sourceSet"].toObject();
}
}  // namespace
bool writeAnnotationStore(const QString& projectPath, const FileState& file,
                          QJsonObject& result, QString& error) {
  try {
    const auto valid = validateFileAnnotations(file);
    require(valid.ok, QString::fromStdString(valid.error));
    const QDir root = QFileInfo(projectPath).absoluteDir();
    require(root.exists(), "工程目标目录不存在，请先创建工程目录");
    const auto relative = "annotations/" + fileKey(file);
    require(root.mkpath(relative), "无法创建标记分片目录");
    require(inside(QFileInfo(root.filePath(relative)).canonicalFilePath(),
                   QFileInfo(root.absolutePath()).canonicalFilePath()),
            "标记目录越过工程范围");
    QJsonArray refs, sources;
    std::vector<QByteArray> records;
    qsizetype estimated = 2;
    quint64 count = 0, segments = 0;
    qint64 written = 0;
    auto flush = [&] {
      if (records.empty()) return;
      // Qt writes the wrapper. The annotation objects spliced into it are the
      // compact bytes already counted for the shard budget.
      const auto wrapper = compactDocument(QJsonDocument(QJsonObject{
          {"schema", "signal-studio-annotations"},
          {"version", 1},
          {"fileId", QString::fromStdString(file.metadata.id)},
          {"annotations", QJsonArray{}}}));
      const QByteArray placeholder("\"annotations\":[]");
      const auto at = wrapper.indexOf(placeholder);
      require(at >= 0, "标记分片封装失败");
      QByteArray bytes;
      bytes.reserve(wrapper.size() + estimated);
      bytes += wrapper.left(at);
      bytes += "\"annotations\":[";
      for (std::size_t index = 0; index < records.size(); ++index) {
        if (index) bytes += ',';
        bytes += records[index];
      }
      bytes += ']';
      bytes += wrapper.mid(at + placeholder.size());
      require(bytes.size() <= shardBytes, "单个标记分片超过8MiB");
      written += bytes.size();
      require(written <= totalBytes, "标记分片超过256MiB预算");
      const auto hash = digest(bytes), name = relative + "/" + hash + ".json",
                 path = root.filePath(name);
      QFile previous(path);
      if (previous.exists()) {
        require(previous.open(QIODevice::ReadOnly) &&
                    previous.size() == bytes.size() &&
                    digest(previous.readAll()) == hash,
                "已有标记分片损坏，请先修复该工程");
      } else {
        QSaveFile out(path);
        require(out.open(QIODevice::WriteOnly) &&
                    out.write(bytes) == bytes.size() && out.commit(),
                "标记分片写入失败");
      }
      refs.append(QJsonObject{{"path", name},
                              {"sha256", hash},
                              {"bytes", QString::number(bytes.size())},
                              {"count", QString::number(records.size())}});
      records.clear();
      estimated = 2;
    };
    for (const auto& a : file.marks) {
      const auto object = compactDocument(QJsonDocument(encodeAnnotation(a)));
      const auto size = object.size() + 1;
      require(size + 512 < shardBytes, "单个标记超过分片预算");
      if (static_cast<qsizetype>(records.size()) >= shardRecords ||
          estimated + size + 512 > shardBytes)
        flush();
      records.push_back(object);
      estimated += size;
      ++count;
      segments += a.segments.size();
      require(count <= annotationSegmentBudget &&
                  segments <= annotationSegmentBudget,
              "标记/活动段超过100000预算");
    }
    flush();
    for (const auto& source : file.sourceSets)
      sources.append(
          writeSourceSnapshot(root, relative, file, source, written));
    result = {{"schema", "signal-studio-annotation-index"},
              {"version", 2},
              {"annotationCount", QString::number(count)},
              {"segmentCount", QString::number(segments)},
              {"shards", refs},
              {"sourceSets", sources}};
    return true;
  } catch (const std::exception& e) {
    error = QString::fromUtf8(e.what());
    return false;
  }
}
bool readAnnotationStore(const QString& projectPath,
                         const QJsonObject& manifest, FileState& target,
                         QString& error) {
  try {
    require(manifest["schema"] == "signal-studio-annotation-index" &&
                (manifest["version"] == 1 || manifest["version"] == 2) &&
                manifest["shards"].isArray() &&
                manifest["sourceSets"].isArray(),
            "标记索引格式无效");
    const auto planned = index(manifest["annotationCount"]),
               plannedSegments = index(manifest["segmentCount"]);
    require(planned <= annotationSegmentBudget &&
                plannedSegments <= annotationSegmentBudget,
            "标记索引超过数量预算");
    FileState candidate = target;
    candidate.marks.clear();
    candidate.sourceSets.clear();
    const QDir root = QFileInfo(projectPath).absoluteDir();
    const auto canonicalRoot =
        QFileInfo(root.absolutePath()).canonicalFilePath();
    require(!canonicalRoot.isEmpty(), "工程目录不存在");
    QSet<QString> ids, sourceIds, paths;
    qint64 bytesRead = 0;
    quint64 segments = 0;
    const auto sources = manifest["sourceSets"].toArray();
    require(sources.size() <= 4096, "来源集合超过预算");
    for (const auto& value : sources) {
      require(value.isObject(), "来源集合非对象");
      AnnotationSourceSet source;
      QString why;
      const auto sourceObject =
          manifest["version"] == 2
              ? readSourceSnapshot(root, canonicalRoot, candidate,
                                   value.toObject(), paths, bytesRead)
              : value.toObject();
      const bool decoded =
          decodeSourceSet(sourceObject, candidate.metadata, source, why);
      require(decoded, why);
      validateImportedSnapshot(source);
      require(!sourceIds.contains(QString::fromStdString(source.id)),
              "来源集合ID重复");
      sourceIds.insert(QString::fromStdString(source.id));
      candidate.sourceSets.push_back(std::move(source));
    }
    const auto refs = manifest["shards"].toArray();
    require(refs.size() <= 4096, "标记分片过多");
    for (const auto& value : refs) {
      require(value.isObject(), "分片引用非对象");
      const auto ref = value.toObject();
      const auto hash = ref["sha256"].toString(), name = ref["path"].toString();
      require(QRegularExpression("^[0-9a-f]{64}$").match(hash).hasMatch(),
              "分片hash格式无效");
      require(
          name == "annotations/" + fileKey(candidate) + "/" + hash + ".json" &&
              !paths.contains(name),
          "拒绝跨目录或重复分片路径");
      paths.insert(name);
      const auto size = index(ref["bytes"]), n = index(ref["count"]);
      require(size <= static_cast<quint64>(shardBytes) &&
                  n <= static_cast<quint64>(shardRecords),
              "分片大小/数量超预算");
      bytesRead += static_cast<qint64>(size);
      require(bytesRead <= totalBytes, "标记读取超过256MiB预算");
      const auto path = root.filePath(name);
      require(inside(QFileInfo(path).canonicalFilePath(), canonicalRoot),
              "分片缺失或路径越界");
      QFile in(path);
      require(in.open(QIODevice::ReadOnly) &&
                  static_cast<quint64>(in.size()) == size,
              "分片不存在/字节数不符");
      const auto data = in.readAll();
      require(in.error() == QFileDevice::NoError && digest(data) == hash,
              "分片内容hash不符");
      QJsonParseError pe;
      const auto doc = QJsonDocument::fromJson(data, &pe);
      require(pe.error == QJsonParseError::NoError && doc.isObject(),
              "分片JSON无效");
      const auto object = doc.object();
      require(object["schema"] == "signal-studio-annotations" &&
                  object["version"] == 1 &&
                  object["fileId"].toString() ==
                      QString::fromStdString(candidate.metadata.id) &&
                  object["annotations"].isArray(),
              "分片身份不符");
      const auto records = object["annotations"].toArray();
      require(static_cast<quint64>(records.size()) == n, "分片条数不符");
      for (const auto& entry : records) {
        require(entry.isObject(), "标记非对象");
        Annotation a;
        QString why;
        const bool decoded =
            decodeAnnotation(entry.toObject(), candidate.metadata, a, why);
        require(decoded, why);
        require(!ids.contains(QString::fromStdString(a.id)), "标记ID重复");
        ids.insert(QString::fromStdString(a.id));
        segments += a.segments.size();
        candidate.marks.push_back(std::move(a));
        require(candidate.marks.size() <= annotationSegmentBudget &&
                    segments <= annotationSegmentBudget,
                "标记预算超过100000");
      }
    }
    require(candidate.marks.size() == planned && segments == plannedSegments,
            "索引与实际标记/段数不一致");
    const auto valid = validateFileAnnotations(candidate);
    require(valid.ok, QString::fromStdString(valid.error));
    target = std::move(candidate);
    return true;
  } catch (const std::exception& e) {
    error = QString::fromUtf8(e.what());
    return false;
  }
}
}  // namespace signalstudio
