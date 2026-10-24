#include "infrastructure/label_import.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringDecoder>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_set>

#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/source_metadata.h"
#include "infrastructure/wav_metadata.h"

namespace signalstudio {
namespace {
constexpr qint64 metadataBudget = 32 * 1024 * 1024;
QString q(const std::string& s) {
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
[[noreturn]] void bad(const QString& text) {
  throw std::runtime_error(text.toUtf8().toStdString());
}
void need(bool condition, const QString& text) {
  if (!condition) bad(text);
}
struct ImportAbort : std::runtime_error {
  LabelImportState state;
  ImportAbort(LabelImportState s, const char* message)
      : std::runtime_error(message), state(s) {}
};
struct ImportBudget {
  const LabelImportOptions& options;
  QElapsedTimer timer;
  explicit ImportBudget(const LabelImportOptions& o) : options(o) {
    need(o.maximumBytes > 0 && o.maximumBytes <= metadataBudget &&
             o.maximumRecords > 0 && o.maximumRecords <= annotationSegmentBudget &&
             o.maximumLines > 0 && o.maximumLines <= 1000000 &&
             o.maximumJsonDepth > 0 && o.maximumJsonDepth <= 128 && o.timeoutMs > 0,
         "标签解析限额无效");
    timer.start();
    check();
  }
  void check() const {
    if (options.cancelled && options.cancelled())
      throw ImportAbort(LabelImportState::Cancelled, "标签导入已取消");
    limit(timer.elapsed() <= options.timeoutMs, "标签解析超时");
  }
  static void limit(bool ok, const char* message) {
    if (!ok) throw ImportAbort(LabelImportState::LimitExceeded, message);
  }
};
QByteArray readText(const QString& path, qint64& remaining, ImportBudget& budget) {
  budget.check();
  QFile file(path);
  need(file.open(QIODevice::ReadOnly), "标签文件不可读取：" + path);
  ImportBudget::limit(file.size() >= 0 && file.size() <= remaining,
                      "标签元数据总量超过读取预算");
  QByteArray raw;
  while (!file.atEnd()) {
    budget.check();
    const auto block = file.read(std::min<qint64>(65536, remaining + 1 - raw.size()));
    need(!block.isEmpty(), "标签文件读取失败或已变化");
    raw += block;
    ImportBudget::limit(raw.size() <= remaining, "标签文件超过读取预算");
  }
  need(raw.size() == file.size() && raw.size() <= remaining,
       "标签文件截断或超过预算");
  remaining -= raw.size();
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  const QString decoded = decoder(raw);
  Q_UNUSED(decoded);
  need(!decoder.hasError(), "标签不是有效 UTF-8 文本");
  // Check nesting before Qt's JSON parser allocates nested objects. Strings
  // (including escaped quotes) do not contribute to structural depth.
  if (path.endsWith(".json", Qt::CaseInsensitive) ||
      path.endsWith(".sigmf-meta", Qt::CaseInsensitive)) {
    int depth = 0;
    bool quoted = false, escaped = false;
    for (qsizetype i = 0; i < raw.size(); ++i) {
      if (i % 65536 == 0) budget.check();
      const char c = raw[i];
      if (quoted) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') quoted = false;
      } else if (c == '"') quoted = true;
      else if (c == '{' || c == '[') {
        ImportBudget::limit(++depth <= budget.options.maximumJsonDepth,
                            "标签 JSON 嵌套超过深度预算");
      } else if (c == '}' || c == ']') --depth;
    }
  }
  budget.check();
  return raw;
}
QString digest(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
std::uint64_t unsignedText(const QString& text, const QString& field) {
  need(QRegularExpression("^(0|[1-9][0-9]{0,19})$").match(text).hasMatch(),
       field + " 必须为非负整数字面量");
  bool ok = false;
  const auto value = text.toULongLong(&ok);
  need(ok, field + " 超出 uint64");
  return value;
}
std::uint64_t jsonUnsigned(const QJsonValue& value, const QString& field) {
  need(value.isDouble(), field + " 必须为安全 JSON 整数");
  const auto n = value.toDouble(-1);
  need(std::isfinite(n) && n >= 0 && n <= 9007199254740991.0 &&
           std::floor(n) == n,
       field + " 必须为安全 JSON 整数");
  return static_cast<std::uint64_t>(n);
}
double realText(const QString& text, const QString& field) {
  need(QRegularExpression(
           "^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$")
           .match(text)
           .hasMatch(),
       field + " 数值无效");
  bool ok = false;
  const auto n = text.toDouble(&ok);
  need(ok && std::isfinite(n), field + " 必须为有限数");
  return n;
}
double jsonReal(const QJsonValue& value, const QString& field) {
  need(value.isDouble() && std::isfinite(value.toDouble()),
       field + " 必须为有限数");
  return value.toDouble();
}
QString className(const QJsonValue& value, const QString& field) {
  need(value.isString() && !value.toString().trimmed().isEmpty() &&
           value.toString().toUtf8().size() <= 256 &&
           !value.toString().contains(QRegularExpression("[\\x00-\\x1f]")),
       field + " 类别必须为非空文本");
  return value.toString();
}
QJsonObject jsonObject(const QByteArray& raw, const QString& name) {
  QJsonParseError error;
  const auto doc = QJsonDocument::fromJson(raw, &error);
  need(error.error == QJsonParseError::NoError && doc.isObject(),
       name + " JSON 无效：" + error.errorString());
  return doc.object();
}
QString canonicalPath(const QString& path) {
  const QFileInfo info(path);
  return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                            : info.canonicalFilePath();
}
bool samePath(const QString& a, const QString& b) {
  return canonicalPath(a) == canonicalPath(b);
}
void validateTarget(const FileMetadata& target) {
  need(!target.id.empty() && target.sampleCount > 0 &&
           target.sampleFormat.structure == SampleStructure::ComplexIQ,
       "标签 profile 需要已绑定的复数 IQ 数据源及物理样本数");
  std::string error;
  std::uint64_t count = 0;
  const QFileInfo info(q(target.path));
  need(info.isFile() && info.isReadable(), "目标样本文件不可读取");
  const bool described = sampleCountForBytes(
      target.sampleFormat, static_cast<std::uint64_t>(info.size()), count,
      error);
  need(described, q(error));
  need(count == target.sampleCount, "目标元数据样本数与物理文件不一致");
  need(std::isfinite(target.sampleRateHz) && target.sampleRateHz > 0 &&
           target.sampleRateHz <= 1e12 &&
           std::isfinite(target.centerFrequencyHz) &&
           target.centerFrequencyHz >= 0 && target.centerFrequencyHz <= 1e12,
       "目标 Fs/Fc 无效");
  if (q(target.path).endsWith(".wav", Qt::CaseInsensitive)) {
    WavDescriptor wav;
    QString message;
    const bool parsed = readWavDescriptor(q(target.path), wav, message);
    need(parsed, message);
    need(wav.format == target.sampleFormat &&
             wav.sampleRateHz == target.sampleRateHz,
         "目标 WAV 格式或 Fs 与容器冲突");
  }
}
AnnotationSourceSet source(const QString& path, const QByteArray& raw,
                           const QByteArray& supplemental,
                           const FileMetadata& target, std::uint64_t generation,
                           const QString& format, const QString& coordinates) {
  AnnotationSourceSet set;
  set.fileId = target.id;
  set.sampleChannel = target.sampleFormat.selectedChannel;
  set.sourceKind = AnnotationSource::ImportedLabel;
  set.formatId = format.toStdString();
  set.parserId = "signalstudio-native-label-import";
  set.parserVersion = "1";
  set.profileId = format.toStdString();
  set.profileVersion = "1";
  set.originalPath = QFileInfo(path).absoluteFilePath().toUtf8().toStdString();
  set.originalText = raw.toStdString();
  set.sourceHash = digest(raw).toStdString();
  set.supplementalOriginalText = supplemental.toStdString();
  if (!supplemental.isEmpty())
    set.supplementalHash = digest(supplemental).toStdString();
  set.coordinateInterpretation = coordinates.toStdString();
  set.sourceIdentity = annotationSourceIdentity(target, generation);
  auto identity = encodeSourceIdentity(set.sourceIdentity);
  identity.remove("projectGeneration");
  identity.remove("availableSamples");
  identity.remove("quickFingerprint");
  identity.remove("hashedRange");
  identity.remove("contentHash");
  const auto binding =
      QJsonDocument(QJsonObject{{"primarySha256", q(set.sourceHash)},
                                {"supplementalSha256", q(set.supplementalHash)},
                                {"profile", format},
                                {"coordinates", coordinates},
                                {"target", identity}})
          .toJson(QJsonDocument::Compact);
  set.id = "source-" + digest(binding).toStdString();
  return set;
}
Annotation annotation(const AnnotationSourceSet& set, const QString& record,
                      const QString& name) {
  Annotation a;
  a.id = "ann-" + set.id.substr(7) + ":" + record.toStdString();
  a.name = name.toUtf8().toStdString();
  a.fileId = set.fileId;
  a.sourceSetId = set.id;
  a.originRecordId = record.toStdString();
  a.sourceKind = AnnotationSource::ImportedLabel;
  a.classLabel = a.name;
  a.revisionId = a.id + ":r0";
  a.reviewRevisionId = a.revisionId;
  a.revisions.push_back({a.revisionId,
                         {},
                         "import",
                         "native-parser",
                         {},
                         "Original source publication"});
  return a;
}
void finishAnnotation(Annotation& a, const QJsonObject& fields,
                      const AnnotationSourceSet& set,
                      const FileMetadata& target) {
  auto origin = std::make_shared<OriginEvidence>();
  origin->formatId = set.formatId;
  origin->originRef = set.originalPath + "#" + a.originRecordId;
  origin->originalFieldsJson =
      QJsonDocument(fields).toJson(QJsonDocument::Compact).toStdString();
  origin->coordinateInterpretation = set.coordinateInterpretation;
  origin->originalSegments = a.segments;
  a.originEvidence = std::move(origin);
  refreshAnnotationProjection(a);
  const auto valid = validateAnnotation(a, annotationBounds(target));
  need(valid.ok, q(valid.error));
}
QJsonObject sharedSourceReference(const AnnotationSourceSet& set,
                                  const QString& pointer, bool supplemental) {
  return {
      {"sourceSetId", q(set.id)},
      {"sourceText",
       supplemental ? "supplementalOriginalText" : "originalText"},
      {"sourceHash", q(supplemental ? set.supplementalHash : set.sourceHash)},
      {"jsonPointer", pointer},
      {"snapshotPolicy", "immutable-source-set-original-text"}};
}
QJsonObject boundedRecordSnapshot(const QJsonObject& record,
                                  const AnnotationSourceSet& set,
                                  const QString& pointer, bool supplemental) {
  const auto encoded = QJsonDocument(record).toJson(QJsonDocument::Compact);
  if (encoded.size() <= 64 * 1024) return record;
  auto reference = sharedSourceReference(set, pointer, supplemental);
  reference["canonicalObjectSha256"] = digest(encoded);
  reference["canonicalObjectEncoding"] = "Qt JSON compact UTF-8";
  return {{"snapshotReference", reference}};
}
qint64 normalizedSourceBytes(const AnnotationSourceSet& sourceSet) {
  const auto size = QJsonDocument(encodeSourceSet(sourceSet))
                        .toJson(QJsonDocument::Compact)
                        .size();
  need(size <= metadataBudget, "规范化来源集合超过 32 MiB 预算");
  return size;
}
void appendImportedAnnotation(LabelImportResult& result, Annotation annotation,
                              qint64& remaining) {
  const auto size = QJsonDocument(encodeAnnotation(annotation))
                        .toJson(QJsonDocument::Compact)
                        .size();
  need(size + 512 < 8 * 1024 * 1024,
       "单个标记超过 8 MiB 持久化分片预算；整批未导入");
  need(size <= remaining, "规范化标记批次超过 32 MiB 累计预算；整批未导入");
  remaining -= size;
  result.annotations.push_back(std::move(annotation));
}
void validateNormalizedBatch(const LabelImportResult& result,
                             qint64 normalizedAnnotationBytes) {
  need(normalizedSourceBytes(result.sourceSet) <=
           metadataBudget - normalizedAnnotationBytes,
       "规范化来源和标记批次超过 32 MiB 累计预算；整批未导入");
}
void addPartialDiagnostic(LabelImportResult& result,
                          const FileMetadata& target) {
  const auto available = availableSamples(target);
  bool tail = false;
  for (const auto& a : result.annotations)
    for (const auto& s : a.segments) tail |= s.range.time.end > available;
  if (tail)
    result.diagnostics.push_back(
        {DiagnosticSeverity::Warning,
         "outside-available-prefix",
         "Original label segments beyond the available prefix are retained "
         "within the physical file; they cannot create a channel until "
         "available.",
         {},
         0});
  result.sourceSet.diagnostics = result.diagnostics;
}
LabelImportResult failure(LabelImportResult result, const std::exception& e) {
  result.annotations.clear();
  result.state = LabelImportState::Error;
  if (const auto* abort = dynamic_cast<const ImportAbort*>(&e))
    result.state = abort->state;
  result.error = QString::fromUtf8(e.what());
  if (result.error.isEmpty()) result.error = "标签校验失败";
  result.diagnostics.push_back({DiagnosticSeverity::Error,
                                "label-import-failed",
                                result.error.toUtf8().toStdString(),
                                {},
                                0});
  result.sourceSet.diagnostics = result.diagnostics;
  return result;
}
using Fields = std::map<QString, QString>;
Fields parseFields(const QString& line) {
  const QRegularExpression pattern("([A-Za-z_][A-Za-z0-9_]*)\\s*:");
  auto matches = pattern.globalMatch(line);
  std::vector<QRegularExpressionMatch> positions;
  while (matches.hasNext()) positions.push_back(matches.next());
  need(!positions.empty(), "TXT 行没有带键字段");
  Fields fields;
  for (std::size_t i = 0; i < positions.size(); ++i) {
    const auto key = positions[i].captured(1).toUpper();
    need(key.size() <= 256 && fields.size() < 256,
         "TXT 单行字段名称或数量超过预算");
    const auto begin = positions[i].capturedEnd();
    const auto end = i + 1 < positions.size() ? positions[i + 1].capturedStart()
                                              : line.size();
    need(fields.emplace(key, line.mid(begin, end - begin).trimmed()).second,
         "TXT 重复关键键：" + key);
  }
  return fields;
}
QString required(const Fields& fields, const QString& key) {
  const auto found = fields.find(key);
  need(found != fields.end() && !found->second.isEmpty(),
       "TXT 缺少关键字段：" + key);
  return found->second;
}
QJsonObject fieldsJson(const Fields& fields) {
  QJsonObject out;
  for (const auto& [key, value] : fields) out[key] = value;
  return out;
}
SampleIndex rounded(double seconds, double fs) {
  const long double value =
      std::floor(static_cast<long double>(seconds) * fs + .5L);
  need(value >= 0 && value < 18446744073709551616.0L, "时间映射样本索引溢出");
  return static_cast<SampleIndex>(value);
}
double tolerance(double value) {
  return std::max(
      1e-6, 4 * std::abs(std::nextafter(
                             value, std::numeric_limits<double>::infinity()) -
                         value));
}
void close(double before, double after, const QString& field, double epsilon) {
  need(std::abs(before - after) <= epsilon, field + " 与已绑定数据不一致");
}
QString intervalKey(std::uint64_t begin, std::uint64_t end,
                    const QString& label) {
  return QString::fromUtf8(
      QJsonDocument(
          QJsonArray{QString::number(begin), QString::number(end), label})
          .toJson(QJsonDocument::Compact));
}
QString emitterToken(const QJsonValue& value) {
  return value.isUndefined() || value.isNull()
             ? QString()
             : QString::fromUtf8(QJsonDocument(QJsonArray{value})
                                     .toJson(QJsonDocument::Compact));
}
struct SupplementalEvent {
  QJsonObject raw;
  std::uint64_t begin = 0, end = 0;
  double lowerRf = 0, upperRf = 0;
  QString label, emitter;
};
struct SupplementalMatch {
  QJsonObject record;
  qsizetype arrayIndex = 0;
};

void validateGeneratorHeader(const QString& line, const Fields& fields,
                             const FileMetadata& target) {
  need(QRegularExpression("^[0-9]+\\s*:").match(line).hasMatch(),
       "TXT 首行必须为生成器记录头");
  const double start = realText(required(fields, "TIME_START"), "time_start");
  const double end = realText(required(fields, "TIME_END"), "time_end");
  const auto count = unsignedText(required(fields, "SIG_LEN"), "sig_len");
  need(start == 0 && end > 0 && count == target.sampleCount,
       "TXT 记录起点或 sig_len 与绑定物理文件不一致");
  close(end, static_cast<double>(count) / target.sampleRateHz, "time_end / Fs",
        5e-6 + .5 / target.sampleRateHz);
}

QJsonArray appendGeneratorSegments(Annotation& annotation,
                                   const QString& timeText, FrequencyRange band,
                                   const FileMetadata& target,
                                   std::size_t& segmentCount) {
  const QRegularExpression pair(
      "([+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?)\\s*,"
      "\\s*([+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?)");
  auto times = pair.globalMatch(timeText);
  qsizetype consumed = 0;
  QJsonArray conversion;
  while (times.hasNext()) {
    const auto match = times.next();
    need(timeText.mid(consumed, match.capturedStart() - consumed)
             .trimmed()
             .isEmpty(),
         "TXT TIME 含无效间隔或坏时间对");
    consumed = match.capturedEnd();
    const double begin = realText(match.captured(1), "TIME start");
    const double end = realText(match.captured(2), "TIME end");
    need(begin >= 0 && begin < end, "TXT TIME 区间必须非负且递增");
    need(++segmentCount <= annotationSegmentBudget,
         "标签活动段超过 100000 段预算");
    const auto first = rounded(begin, target.sampleRateHz);
    const auto last = rounded(end, target.sampleRateHz);
    need(first < last && last <= target.sampleCount,
         "TXT TIME 舍入后超出物理文件或为空");
    const auto uncertainty =
        static_cast<SampleIndex>(std::ceil(target.sampleRateHz * 5e-6 + .5));
    annotation.segments.push_back(
        {annotation.id + ":s" + std::to_string(annotation.segments.size()),
         {{first, last}, band},
         TimeQuality::RoundedLabel,
         FrequencyQuality::NominalLabel,
         false,
         uncertainty,
         FrequencyFrame::RF});
    conversion.append(
        QJsonObject{{"sourceStartSeconds", begin},
                    {"sourceEndSeconds", end},
                    {"startSample", QString::number(first)},
                    {"endSample", QString::number(last)},
                    {"startRoundingSamples",
                     static_cast<double>(first) - begin * target.sampleRateHz},
                    {"endRoundingSamples",
                     static_cast<double>(last) - end * target.sampleRateHz}});
  }
  need(consumed > 0 && timeText.mid(consumed).trimmed().isEmpty(),
       "TXT TIME 缺失或尾部格式错误");
  return conversion;
}

Annotation parseGeneratorRecord(const QString& rawLine, qsizetype lineNumber,
                                const Fields& fields,
                                const AnnotationSourceSet& sourceSet,
                                const FileMetadata& target,
                                std::unordered_set<std::uint64_t>& records,
                                std::size_t& segmentCount) {
  const auto match =
      QRegularExpression("^([0-9]+)\\)\\s*:").match(rawLine.trimmed());
  need(match.hasMatch(), "TXT 载波编号无效");
  const auto record = unsignedText(match.captured(1), "record number");
  need(record > 0 && records.insert(record).second, "TXT 载波编号重复或为 0");
  const auto label = className(required(fields, "MOD"), "mod");
  className(required(fields, "SYS"), "sys");
  const double centerGenerated = realText(required(fields, "FC"), "FC");
  const double bandwidth = realText(required(fields, "BW"), "BW");
  need(centerGenerated >= 0 && centerGenerated < target.sampleRateHz &&
           bandwidth > 0,
       "TXT FC/BW 超出生成坐标范围");
  need(unsignedText(required(fields, "LEN"), "LEN") == target.sampleCount,
       "TXT LEN 与记录头/物理文件不一致");
  for (const auto& fieldName : QStringList{"FD", "SPS", "CNR", "SNR"}) {
    const double value = realText(required(fields, fieldName), fieldName);
    if (fieldName == "FD" || fieldName == "SPS")
      need(value > 0, fieldName + " 必须大于 0");
  }
  auto imported = annotation(sourceSet, QString::number(record), label);
  const double center =
      centerGenerated - target.sampleRateHz / 2 + target.centerFrequencyHz;
  const auto rounding = appendGeneratorSegments(
      imported, required(fields, "TIME"),
      {center - bandwidth / 2, center + bandwidth / 2}, target, segmentCount);
  QJsonObject original{
      {"lineNumber", QString::number(lineNumber)},
      {"recordNumber", QString::number(record)},
      {"textQuantizationSeconds", 1e-5},
      {"halfStepSeconds", 5e-6},
      {"sampleRoundingBound", .5},
      {"roundingRule", "nearest sample; floor(seconds*boundFs+0.5)"},
      {"boundSampleRateHz", target.sampleRateHz},
      {"boundCenterFrequencyHz", target.centerFrequencyHz}};
  const auto rawLineBytes = rawLine.toUtf8();
  if (rawLineBytes.size() <= 64 * 1024) {
    original["rawLine"] = rawLine;
    original["fields"] = fieldsJson(fields);
  } else {
    auto reference = sharedSourceReference(sourceSet, "", false);
    reference.remove("jsonPointer");
    reference["textLineNumber"] = QString::number(lineNumber);
    reference["rawLineSha256"] = digest(rawLineBytes);
    original["recordReference"] = reference;
  }
  if (QJsonDocument(rounding).toJson(QJsonDocument::Compact).size() <=
      64 * 1024)
    original["rounding"] = rounding;
  else
    original["roundingDerivedFromOriginalTimePairs"] = true;
  finishAnnotation(imported, original, sourceSet, target);
  return imported;
}

void validateTorchSigGlobal(const QJsonObject& global, const QString& dataPath,
                            const FileMetadata& target) {
  need(global["core:datatype"].toString() == "cf32_le",
       "TorchSig profile 仅支持 cf32_le");
  need(QRegularExpression("^1\\.[0-9]+\\.[0-9]+$")
           .match(global["core:version"].toString())
           .hasMatch(),
       "SigMF 版本须为支持的 1.x");
  if (global.contains("core:dataset"))
    need(
        global["core:dataset"].isString() &&
            global["core:dataset"].toString() == QFileInfo(dataPath).fileName(),
        "SigMF dataset 必须为同名配对数据，不允许跨目录");
  SampleFormat expectedFormat;
  expectedFormat.componentEncoding = ComponentEncoding::Float32;
  expectedFormat.normalizeIntegerAdc = false;
  need(expectedFormat == target.sampleFormat,
       "TorchSig profile 需要零偏移单通道复数 cf32");
  need(global["core:offset"].isUndefined() ||
           jsonUnsigned(global["core:offset"], "core:offset") == 0,
       "TorchSig profile 暂不支持非零外部 offset");
  need(global["core:num_channels"].isUndefined() ||
           jsonUnsigned(global["core:num_channels"], "core:num_channels") == 1,
       "TorchSig profile 暂不支持多通道");
  need(global["core:trailing_bytes"].isUndefined() ||
           jsonUnsigned(global["core:trailing_bytes"], "core:trailing_bytes") ==
               0,
       "TorchSig profile 暂不支持非零 trailer");
  if (!global.contains("core:extensions")) return;
  need(global["core:extensions"].isArray(), "core:extensions 必须为数组");
  for (const auto value : global["core:extensions"].toArray())
    need(value.isObject() && value.toObject()["optional"].isBool() &&
             value.toObject()["optional"].toBool(),
         "未知必需 SigMF 扩展不支持");
}

QJsonObject validateTorchSigCapture(const QJsonObject& meta,
                                    const QJsonObject& global,
                                    const FileMetadata& target) {
  const auto captures = meta["captures"].toArray();
  need(captures.size() == 1 && captures[0].isObject(),
       "TorchSig profile 仅支持从 0 开始的单个固定 capture");
  const auto capture = captures[0].toObject();
  need(jsonUnsigned(capture["core:sample_start"], "capture sample_start") == 0,
       "capture 必须从样本 0 开始");
  close(jsonReal(global["core:sample_rate"], "sample_rate"),
        target.sampleRateHz, "Fs", 0);
  close(jsonReal(capture["core:frequency"], "frequency"),
        target.centerFrequencyHz, "Fc", 0);
  need(capture["core:header_bytes"].isUndefined() ||
           jsonUnsigned(capture["core:header_bytes"], "header_bytes") == 0,
       "TorchSig profile 暂不支持 capture header");
  return capture;
}

QJsonArray validateRfbenchHeader(const QJsonObject& extra,
                                 const FileMetadata& target) {
  need(extra["schema"].toString() == "rfbench-v1" && extra["events"].isArray(),
       "补充 JSON 必须为 rfbench-v1 events");
  close(jsonReal(extra["sampling_rate_hz"], "sampling_rate_hz"),
        target.sampleRateHz, "JSON Fs", 0);
  close(jsonReal(extra["capture_center_hz"], "capture_center_hz"),
        target.centerFrequencyHz, "JSON Fc", 0);
  need(jsonUnsigned(extra["capture_samples"], "capture_samples") ==
           target.sampleCount,
       "JSON capture_samples 与物理文件不一致");
  close(jsonReal(extra["capture_duration_s"], "capture_duration_s"),
        static_cast<double>(target.sampleCount) / target.sampleRateHz,
        "JSON duration", 1e-12);
  need(extra["frequency_axis"].toString() ==
               "offset Hz from capture center; range [-Fs/2, Fs/2)" &&
           extra["time_axis"].toString() == "[start, end) in IQ sample indices",
       "rfbench 时间/频率轴声明不支持");
  const auto events = extra["events"].toArray();
  need(jsonUnsigned(extra["n_events"], "n_events") ==
           static_cast<std::uint64_t>(events.size()),
       "JSON n_events 与 events 不一致");
  return events;
}

void validateEventAttributes(const QJsonObject& event, TimeRange time,
                             const FileMetadata& target) {
  const double fs = target.sampleRateHz;
  for (auto [fieldName, expectedValue] :
       std::initializer_list<std::pair<const char*, double>>{
           {"t_start_s", static_cast<double>(time.begin) / fs},
           {"t_end_s", static_cast<double>(time.end) / fs},
           {"duration_s", static_cast<double>(time.end - time.begin) / fs}})
    if (event.contains(fieldName))
      close(jsonReal(event[fieldName], fieldName), expectedValue, fieldName,
            1e-12);
  if (event.contains("nominal_bandwidth_hz"))
    need(jsonReal(event["nominal_bandwidth_hz"], "nominal_bandwidth_hz") > 0,
         "名义 BW 必须为正数");
  if (event.contains("snr_inband_db"))
    jsonReal(event["snr_inband_db"], "snr_inband_db");
}

class SupplementalMatcher {
 public:
  SupplementalMatcher(const QJsonArray& events, const FileMetadata& target)
      : target_(target), used_(static_cast<std::size_t>(events.size()), false) {
    for (qsizetype index = 0; index < events.size(); ++index) {
      need(events[index].isObject(), "rfbench event 必须为对象");
      SupplementalEvent event;
      event.raw = events[index].toObject();
      event.begin =
          jsonUnsigned(event.raw["sample_start"], "event sample_start");
      event.end = jsonUnsigned(event.raw["sample_end"], "event sample_end");
      need(event.begin < event.end && event.end <= target.sampleCount,
           "rfbench event 时间越界");
      const double lower =
          jsonReal(event.raw["freq_lower_hz"], "event freq_lower_hz");
      const double upper =
          jsonReal(event.raw["freq_upper_hz"], "event freq_upper_hz");
      need(lower < upper && lower >= -target.sampleRateHz / 2 &&
               upper <= target.sampleRateHz / 2,
           "rfbench event 频率越界");
      event.lowerRf = lower + target.centerFrequencyHz;
      event.upperRf = upper + target.centerFrequencyHz;
      event.label = className(event.raw["modulation"], "event modulation");
      event.emitter = emitterToken(event.raw["emitter_id"]);
      const auto key = intervalKey(event.begin, event.end, event.label);
      broadIndex_[key].emplace(event.lowerRf, index);
      emitterIndex_[key + event.emitter].emplace(event.lowerRf, index);
      events_.push_back(std::move(event));
    }
  }

  SupplementalMatch match(const QJsonObject& record, const Segment& segment,
                          const QString& label) {
    std::vector<qsizetype> matches;
    const auto key =
        intervalKey(segment.range.time.begin, segment.range.time.end, label);
    const auto emitter = emitterToken(record["rfbench:emitter_id"]);
    if (emitter.isEmpty()) {
      if (const auto it = broadIndex_.find(key); it != broadIndex_.end())
        findMatches(it->second, segment.range.frequency, matches);
    } else {
      if (const auto it = emitterIndex_.find(key + emitter);
          it != emitterIndex_.end())
        findMatches(it->second, segment.range.frequency, matches);
      if (const auto it = emitterIndex_.find(key); it != emitterIndex_.end())
        findMatches(it->second, segment.range.frequency, matches);
    }
    need(matches.size() == 1,
         "rfbench event 与 SigMF 的样本/频率/类别/emitter 关联缺失或有歧义");
    const auto index = static_cast<std::size_t>(matches.front());
    need(!used_[index], "rfbench event 被多个 SigMF annotations 重复匹配");
    used_[index] = true;
    validateEventAttributes(events_[index].raw, segment.range.time, target_);
    return {events_[index].raw, static_cast<qsizetype>(index)};
  }

  void requireComplete() const {
    need(
        std::all_of(used_.begin(), used_.end(), [](bool used) { return used; }),
        "rfbench 存在未匹配 event");
  }

 private:
  void findMatches(const std::multimap<double, qsizetype>& indexed,
                   FrequencyRange band, std::vector<qsizetype>& matches) {
    const double epsilon = tolerance(band.lowerHz);
    for (auto it = indexed.lower_bound(band.lowerHz - epsilon);
         it != indexed.end() && it->first <= band.lowerHz + epsilon; ++it) {
      need(++matchingWork_ <= 4000000,
           "rfbench 关联工作超过预算；请减少单批事件");
      const auto& event = events_[static_cast<std::size_t>(it->second)];
      if (std::abs(event.upperRf - band.upperHz) <= tolerance(band.upperHz)) {
        matches.push_back(it->second);
        if (matches.size() > 1) break;
      }
    }
  }
  const FileMetadata& target_;
  std::vector<bool> used_;
  std::uint64_t matchingWork_ = 0;
  std::vector<SupplementalEvent> events_;
  std::map<QString, std::multimap<double, qsizetype>> broadIndex_,
      emitterIndex_;
};

Annotation parseTorchSigRecord(const QJsonObject& record, qsizetype index,
                               const AnnotationSourceSet& sourceSet,
                               const FileMetadata& target,
                               SupplementalMatcher* matcher) {
  const auto begin = jsonUnsigned(record["core:sample_start"], "sample_start");
  const auto count = jsonUnsigned(record["core:sample_count"], "sample_count");
  need(count > 0 && count <= std::numeric_limits<SampleIndex>::max() - begin,
       "SigMF 起止样本溢出或为空");
  const auto end = begin + count;
  need(end <= target.sampleCount, "SigMF annotation 超出物理文件");
  const double lower =
      jsonReal(record["core:freq_lower_edge"], "freq_lower_edge");
  const double upper =
      jsonReal(record["core:freq_upper_edge"], "freq_upper_edge");
  const auto label = className(record["core:label"], "core:label");
  auto imported = annotation(sourceSet, QString::number(index), label);
  imported.segments.push_back({imported.id + ":s0",
                               {{begin, end}, {lower, upper}},
                               TimeQuality::SampleLabel,
                               FrequencyQuality::NominalLabel,
                               false,
                               0,
                               FrequencyFrame::RF});
  QJsonObject original{
      {"sigmfAnnotation",
       boundedRecordSnapshot(record, sourceSet,
                             "/annotations/" + QString::number(index), false)},
      {"globalReference", sharedSourceReference(sourceSet, "/global", false)},
      {"captureReference",
       sharedSourceReference(sourceSet, "/captures/0", false)},
      {"arrayIndex", QString::number(index)}};
  if (record.contains("rfbench:snr_inband_db"))
    jsonReal(record["rfbench:snr_inband_db"], "rfbench:snr_inband_db");
  if (matcher) {
    const auto matched =
        matcher->match(record, imported.segments.front(), label);
    original["rfbenchEvent"] = boundedRecordSnapshot(
        matched.record, sourceSet,
        "/events/" + QString::number(matched.arrayIndex), true);
    original["rfbenchHeaderReference"] =
        sharedSourceReference(sourceSet, "", true);
  }
  finishAnnotation(imported, original, sourceSet, target);
  return imported;
}
}  // namespace

LabelImportResult importGeneratorLabels(const QString& path,
                                        const FileMetadata& target,
                                        std::uint64_t generation,
                                        const LabelImportOptions& options) {
  LabelImportResult result;
  try {
    ImportBudget control(options);
    validateTarget(target);
    need(path.endsWith(".txt", Qt::CaseInsensitive),
         "仅支持指定生成器带键 TXT");
    qint64 budget = options.maximumBytes;
    const auto raw = readText(path, budget, control);
    const auto document = QString::fromUtf8(raw);
    result.sourceSet =
        source(path, raw, {}, target, generation, "generator-txt-v1",
               "TIME seconds -> nearest sample [start,end); RF Hz = generator "
               "FC - Fs/2 + bound Fc; nominal BW; 10 us text quantization");
    const qint64 sourceBytes = normalizedSourceBytes(result.sourceSet);
    qint64 normalizedRemaining = metadataBudget - sourceBytes;
    bool haveHeader = false;
    std::unordered_set<std::uint64_t> records;
    std::size_t segmentCount = 0;
    qsizetype position = 0, lineNumber = 0;
    while (position < document.size()) {
      control.check();
      auto end = document.indexOf('\n', position);
      if (end < 0) end = document.size();
      const auto rawLine = document.mid(position, end - position);
      position = end + 1;
      ImportBudget::limit(++lineNumber <= static_cast<qsizetype>(options.maximumLines),
                          "TXT 行数超过预算");
      const auto line = rawLine.trimmed();
      if (line.isEmpty()) continue;
      try {
        const auto fields = parseFields(line);
        if (!haveHeader) {
          validateGeneratorHeader(line, fields, target);
          haveHeader = true;
          continue;
        }
        appendImportedAnnotation(
            result,
            parseGeneratorRecord(rawLine, lineNumber, fields, result.sourceSet,
                                 target, records, segmentCount),
            normalizedRemaining);
        ImportBudget::limit(segmentCount <= options.maximumRecords,
                            "TXT 活动段超过预算");
      } catch (const ImportAbort&) {
        throw;
      } catch (const std::exception& e) {
        const auto message = QString("TXT 第 %1 行：%2")
                                 .arg(lineNumber)
                                 .arg(QString::fromUtf8(e.what()));
        const auto record =
            QRegularExpression("^([0-9]+)\\)").match(line).captured(1);
        result.diagnostics.push_back(
            {DiagnosticSeverity::Error, "invalid-generator-record",
             message.toUtf8().toStdString(), record.toStdString(),
             static_cast<std::uint64_t>(lineNumber)});
        result.error = message;
        need(result.diagnostics.size() < 4096,
             "TXT 错误诊断超过 4096 条预算；整批未导入");
        if (!haveHeader) throw;
      }
    }
    need(result.error.isEmpty(),
         "TXT 存在错误记录，整批未导入；请查看逐行诊断");
    need(haveHeader && !result.annotations.empty(), "TXT 没有可导入载波");
    result.diagnostics.push_back(
        {DiagnosticSeverity::Info,
         "rounded-label-time",
         "TIME uses 10 us text quantization and nearest-sample rounding; "
         "nominal BW is not a measured occupied bandwidth.",
         {},
         0});
    addPartialDiagnostic(result, target);
    validateNormalizedBatch(result,
                            metadataBudget - sourceBytes - normalizedRemaining);
    control.check();
    return result;
  } catch (const std::exception& e) {
    return failure(std::move(result), e);
  }
}

LabelImportResult importTorchSigLabels(const QString& path,
                                       const FileMetadata& target,
                                       std::uint64_t generation,
                                       bool ignoreSupplemental,
                                       const LabelImportOptions& options) {
  LabelImportResult result;
  try {
    ImportBudget control(options);
    validateTarget(target);
    QString base;
    if (path.endsWith(".sigmf-meta", Qt::CaseInsensitive) ||
        path.endsWith(".sigmf-data", Qt::CaseInsensitive))
      base = path.chopped(11);
    else if (path.endsWith(".json", Qt::CaseInsensitive))
      base = path.chopped(5);
    else
      bad("仅支持指定 TorchSig SigMF 配对或同名 rfbench JSON");
    const QString metaPath = base + ".sigmf-meta",
                  dataPath = base + ".sigmf-data", jsonPath = base + ".json";
    need(samePath(dataPath, q(target.path)),
         "SigMF 配对数据必须与目标数据源为同一物理文件");
    qint64 budget = options.maximumBytes;
    const auto raw = readText(metaPath, budget, control);
    QByteArray supplemental;
    if (QFileInfo::exists(jsonPath)) supplemental = readText(jsonPath, budget, control);
    const auto meta = jsonObject(raw, "SigMF");
    need(meta["global"].isObject() && meta["captures"].isArray() &&
             meta["annotations"].isArray(),
         "SigMF 需要 global/captures/annotations");
    const auto global = meta["global"].toObject();
    validateTorchSigGlobal(global, dataPath, target);
    validateTorchSigCapture(meta, global, target);
    const auto records = meta["annotations"].toArray();
    ImportBudget::limit(records.size() <= static_cast<qsizetype>(options.maximumRecords),
                        "SigMF annotations 超过段预算");
    const bool useExtra = !supplemental.isEmpty() && !ignoreSupplemental;
    QJsonObject extraHeader;
    std::optional<SupplementalMatcher> matcher;
    if (useExtra) {
      extraHeader = jsonObject(supplemental, "rfbench");
      const auto events = validateRfbenchHeader(extraHeader, target);
      need(events.size() == records.size(),
           "补充 JSON events 数量与 SigMF annotations 不一致");
      matcher.emplace(events, target);
      extraHeader.remove("events");
    }
    result.sourceSet = source(
        metaPath, raw, supplemental, target, generation,
        "torchsig-rfbench-sigmf-v1",
        useExtra
            ? "SigMF sample [start,end) and RF Hz are authoritative; rfbench "
              "offset Hz checked after adding capture Fc exactly once"
            : "SigMF sample [start,end) and RF Hz are authoritative; no Fc "
              "added to SigMF edges");
    const qint64 sourceBytes = normalizedSourceBytes(result.sourceSet);
    qint64 normalizedRemaining = metadataBudget - sourceBytes;
    if (ignoreSupplemental && !supplemental.isEmpty())
      result.diagnostics.push_back(
          {DiagnosticSeverity::Warning,
           "supplemental-explicitly-ignored",
           "User selected meta-only import; supplemental JSON text/hash "
           "retained without applying its fields.",
           {},
           0});
    for (qsizetype i = 0; i < records.size(); ++i) {
      control.check();
      need(records[i].isObject(), "SigMF annotation 必须为对象");
      appendImportedAnnotation(
          result,
          parseTorchSigRecord(records[i].toObject(), i, result.sourceSet,
                              target, matcher ? &*matcher : nullptr),
          normalizedRemaining);
    }
    if (matcher) matcher->requireComplete();
    addPartialDiagnostic(result, target);
    validateNormalizedBatch(result,
                            metadataBudget - sourceBytes - normalizedRemaining);
    if (records.isEmpty()) result.state = LabelImportState::Empty;
    control.check();
    return result;
  } catch (const std::exception& e) {
    return failure(std::move(result), e);
  }
}

LabelImportResult importLabelSource(const QString& path,
                                    const FileMetadata& target,
                                    std::uint64_t generation,
                                    bool ignoreSupplemental,
                                    const LabelImportOptions& options) {
  if (path.endsWith(".txt", Qt::CaseInsensitive)) {
    LabelImportResult result;
    QString preview;
    if (!previewLabelSource(path, preview, result.error, options, 16384, &result.state)) return result;
    if (hasGeneratorLabelHeader(preview))
      return importGeneratorLabels(path, target, generation, options);
    result.state = LabelImportState::NeedsMapping;
    result.error = "TXT 未识别为已知生成器格式；请明确配置字段映射，未导入任何标记";
    return result;
  }
  if (path.endsWith(".sigmf-meta", Qt::CaseInsensitive) ||
      path.endsWith(".sigmf-data", Qt::CaseInsensitive) ||
      path.endsWith(".json", Qt::CaseInsensitive))
    return importTorchSigLabels(path, target, generation, ignoreSupplemental, options);
  LabelImportResult result;
  result.error =
      "仅支持指定生成器 TXT 与 TorchSig SigMF/rfbench 配对，不提供通用标签导入";
  result.state = LabelImportState::Error;
  return result;
}

bool hasGeneratorLabelHeader(const QString& preview) {
  const auto lines = preview.left(65536).split('\n');
  for (const auto& line : lines) {
    const auto text = line.trimmed();
    if (text.isEmpty()) continue;
    return QRegularExpression("^[0-9]+\\s*:").match(text).hasMatch() &&
           QRegularExpression("\\btime_start\\s*:", QRegularExpression::CaseInsensitiveOption).match(text).hasMatch() &&
           QRegularExpression("\\btime_end\\s*:", QRegularExpression::CaseInsensitiveOption).match(text).hasMatch() &&
           QRegularExpression("\\bsig_len\\s*:", QRegularExpression::CaseInsensitiveOption).match(text).hasMatch();
  }
  return false;
}

bool previewLabelSource(const QString& path, QString& preview, QString& error,
                         const LabelImportOptions& options, qint64 maximumPreviewBytes,
                         LabelImportState* failureState) {
  preview.clear();
  error.clear();
  if (failureState) *failureState = LabelImportState::Valid;
  try {
    ImportBudget control(options);
    need(maximumPreviewBytes > 0 && maximumPreviewBytes <= 65536, "标签预览预算必须为 1–65536 字节");
    QFile file(path);
    need(QFileInfo(path).isFile() && file.open(QIODevice::ReadOnly), "标签预览文件不可读取：" + path);
    ImportBudget::limit(file.size() >= 0 && file.size() <= options.maximumBytes,
                        "标签文件超过读取预算");
    const auto raw = file.read(maximumPreviewBytes);
    need(file.error() == QFileDevice::NoError, "标签预览读取失败：" + path);
    QStringConverter::Flags flags;
    if (file.size() <= raw.size()) flags |= QStringConverter::Flag::Stateless;
    QStringDecoder decoder(QStringDecoder::Utf8, flags);
    const QString text = decoder(raw);
    need(!decoder.hasError(), "标签预览不是有效 UTF-8 文本");
    control.check();
    preview = text;
    return true;
  } catch (const std::exception& e) {
    error = QString::fromUtf8(e.what());
    if (failureState) {
      const auto* abort = dynamic_cast<const ImportAbort*>(&e);
      *failureState = abort ? abort->state : LabelImportState::Error;
    }
    return false;
  }
}
QJsonObject encodeLabelFieldMapping(const LabelFieldMapping& m) {
  return {{"schemaVersion", m.schemaVersion}, {"delimiter", static_cast<int>(m.delimiter)},
          {"skipLines", m.skipLines}, {"commentPrefix", m.commentPrefix},
          {"classColumn", m.classColumn}, {"startColumn", m.startColumn},
          {"endColumn", m.endColumn}, {"lowerColumn", m.lowerColumn},
          {"upperColumn", m.upperColumn}, {"timeUnit", static_cast<int>(m.timeUnit)},
          {"interval", static_cast<int>(m.interval)}, {"indexOrigin", m.indexOrigin},
          {"frameHopSamples", QString::number(m.frameHopSamples)},
          {"frameLengthSamples", QString::number(m.frameLengthSamples)},
          {"allowTimeRounding", m.allowTimeRounding},
          {"timeQuantizationSeconds", m.timeQuantizationSeconds},
          {"frequencyLayout", static_cast<int>(m.frequencyLayout)},
          {"frequencyUnit", static_cast<int>(m.frequencyUnit)},
          {"frequencyReference", static_cast<int>(m.frequencyReference)}};
}

bool decodeLabelFieldMapping(const QJsonObject& object, LabelFieldMapping& output,
                             QString& error) {
  try {
    const auto integer = [&](const char* key, int low, int high) {
      const auto v = object[key];
      need(v.isDouble() && std::floor(v.toDouble()) == v.toDouble() &&
               v.toDouble() >= low && v.toDouble() <= high,
           QString("映射字段无效：") + key);
      return v.toInt();
    };
    LabelFieldMapping m;
    m.schemaVersion = integer("schemaVersion", 1, 1);
    m.delimiter = static_cast<LabelDelimiter>(integer("delimiter", 0, 4));
    m.skipLines = integer("skipLines", 0, 1000000);
    need(object["commentPrefix"].isString() &&
             object["commentPrefix"].toString().size() <= 32,
         "映射注释前缀无效");
    m.commentPrefix = object["commentPrefix"].toString();
    m.classColumn = integer("classColumn", -1, 255);
    m.startColumn = integer("startColumn", -1, 255);
    m.endColumn = integer("endColumn", -1, 255);
    m.lowerColumn = integer("lowerColumn", -1, 255);
    m.upperColumn = integer("upperColumn", -1, 255);
    m.timeUnit = static_cast<LabelTimeUnit>(integer("timeUnit", 0, 4));
    m.interval = static_cast<LabelInterval>(integer("interval", 0, 3));
    m.indexOrigin = integer("indexOrigin", -1, 1);
    need(object["frameHopSamples"].isString() && object["frameLengthSamples"].isString(),
         "帧步长和长度必须为 uint64 字符串");
    m.frameHopSamples = unsignedText(object["frameHopSamples"].toString(), "frameHopSamples");
    m.frameLengthSamples = unsignedText(object["frameLengthSamples"].toString(), "frameLengthSamples");
    need(object["allowTimeRounding"].isBool(), "时间舍入选项缺失");
    m.allowTimeRounding = object["allowTimeRounding"].toBool();
    m.timeQuantizationSeconds = jsonReal(object["timeQuantizationSeconds"], "timeQuantizationSeconds");
    need(m.timeQuantizationSeconds >= 0 && m.timeQuantizationSeconds <= 1e6,
         "时间量化精度无效");
    m.frequencyLayout = static_cast<LabelFrequencyLayout>(integer("frequencyLayout", 0, 3));
    m.frequencyUnit = static_cast<LabelFrequencyUnit>(integer("frequencyUnit", 0, 4));
    m.frequencyReference = static_cast<LabelFrequencyReference>(integer("frequencyReference", 0, 2));
    output = m;
    error.clear();
    return true;
  } catch (const std::exception& e) {
    error = QString::fromUtf8(e.what());
    return false;
  }
}

bool restoreLabelFieldMapping(const AnnotationSourceSet& sourceSet,
                              LabelFieldMapping& mapping, QString& error) {
  if (sourceSet.formatId != "mapped-txt-v1" ||
      sourceSet.profileId != "user-confirmed-delimited-txt" ||
      sourceSet.coordinateInterpretation.size() > 65536) {
    error = "来源不是受支持的已确认 TXT 映射 profile";
    return false;
  }
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(
      QByteArray::fromStdString(sourceSet.coordinateInterpretation), &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) {
    error = "已保存映射 JSON 损坏";
    return false;
  }
  return decodeLabelFieldMapping(document.object(), mapping, error);
}

LabelImportResult importMappedTextLabels(const QString& path,
                                         const FileMetadata& target,
                                         const LabelFieldMapping& requested,
                                         std::uint64_t generation,
                                         const LabelImportOptions& options) {
  LabelImportResult result;
  try {
    ImportBudget control(options);
    validateTarget(target);
    LabelFieldMapping m;
    QString mappingError;
    const bool decoded = decodeLabelFieldMapping(encodeLabelFieldMapping(requested), m, mappingError);
    need(decoded, mappingError);
    need(m.delimiter != LabelDelimiter::Unspecified && m.classColumn >= 0 &&
             m.timeUnit != LabelTimeUnit::Unspecified &&
             m.frequencyLayout != LabelFrequencyLayout::Unspecified,
         "请明确分隔符、类别列、时间和频率解释；不能推断字段");
    const bool unknown = m.frequencyLayout == LabelFrequencyLayout::Unknown;
    if (m.timeUnit != LabelTimeUnit::Unknown) {
      need(m.startColumn >= 0 && m.endColumn >= 0 &&
               m.interval != LabelInterval::Unspecified,
           "时间列和区间规则必须明确");
      if (m.timeUnit == LabelTimeUnit::Seconds)
        need(m.interval != LabelInterval::InclusiveEnd && m.indexOrigin == 0,
             "秒坐标从 0 起，不支持无采样依据的闭合秒区间");
      else
        need(m.indexOrigin == 0 || m.indexOrigin == 1, "必须确认样本/帧索引从 0 或 1 起");
      if (m.timeUnit == LabelTimeUnit::Frames)
        need(m.frameHopSamples > 0 && m.frameLengthSamples > 0,
             "帧映射必须给出实际帧长和步长（样本）");
    }
    if (m.frequencyLayout != LabelFrequencyLayout::Unknown)
      need(m.lowerColumn >= 0 && m.upperColumn >= 0 &&
               m.frequencyUnit != LabelFrequencyUnit::Unspecified &&
               m.frequencyReference != LabelFrequencyReference::Unspecified,
           "频率列、单位及绝对 RF/基带偏移必须明确");
    std::unordered_set<int> columns;
    for (const auto c : {m.classColumn,
                        m.timeUnit == LabelTimeUnit::Unknown ? -1 : m.startColumn,
                        m.timeUnit == LabelTimeUnit::Unknown ? -1 : m.endColumn,
                        m.frequencyLayout == LabelFrequencyLayout::Unknown ? -1 : m.lowerColumn,
                        m.frequencyLayout == LabelFrequencyLayout::Unknown ? -1 : m.upperColumn})
      need(c < 0 || columns.insert(c).second, "不同物理字段不能映射到同一列");
    qint64 budget = options.maximumBytes;
    const auto raw = readText(path, budget, control);
    const auto mapping = encodeLabelFieldMapping(m);
    const auto coordinates = QString::fromUtf8(QJsonDocument(mapping).toJson(QJsonDocument::Compact));
    result.sourceSet = source(path, raw, {}, target, generation, "mapped-txt-v1", coordinates);
    result.sourceSet.profileId = "user-confirmed-delimited-txt";
    const auto sourceBytes = normalizedSourceBytes(result.sourceSet);
    qint64 remaining = metadataBudget - sourceBytes;
    const auto text = QString::fromUtf8(raw);
    qsizetype position = 0;
    std::size_t lineNumber = 0, recordCount = 0;
    const auto add = [](std::uint64_t a, std::uint64_t b) {
      need(a <= std::numeric_limits<std::uint64_t>::max() - b, "时间整数加法溢出");
      return a + b;
    };
    const auto multiply = [](std::uint64_t a, std::uint64_t b) {
      need(b == 0 || a <= std::numeric_limits<std::uint64_t>::max() / b,
           "帧索引乘法溢出");
      return a * b;
    };
    while (position < text.size()) {
      control.check();
      ImportBudget::limit(++lineNumber <= options.maximumLines, "TXT 行数超过预算");
      auto end = text.indexOf('\n', position);
      if (end < 0) end = text.size();
      auto originalLine = text.mid(position, end - position);
      if (originalLine.endsWith('\r')) originalLine.chop(1);
      const auto line = originalLine.trimmed();
      position = end + 1;
      if (lineNumber <= static_cast<std::size_t>(m.skipLines) || line.isEmpty() ||
          (!m.commentPrefix.isEmpty() && line.startsWith(m.commentPrefix))) continue;
      ImportBudget::limit(++recordCount <= options.maximumRecords, "TXT 记录数超过预算");
      need(!line.contains('"'), "带引号/转义的 TXT 需要专用 profile，不能按简单分隔解析");
      QStringList fields;
      if (m.delimiter == LabelDelimiter::Whitespace) {
        auto tokens = QRegularExpression("\\S+").globalMatch(line);
        int count = 0;
        while (tokens.hasNext()) {
          tokens.next();
          need(++count <= 256, "TXT 列数超过 256");
        }
        fields = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
      } else {
        const auto separator = m.delimiter == LabelDelimiter::Tab ? '\t' :
                               m.delimiter == LabelDelimiter::Comma ? ',' : ';';
        need(originalLine.count(separator) < 256, "TXT 列数超过 256");
        fields = originalLine.split(separator, Qt::KeepEmptyParts);
        for (auto& field : fields) field = field.trimmed();
      }
      need(fields.size() <= 256, "TXT 列数超过 256");
      const auto field = [&](int column) {
        need(column >= 0 && column < fields.size() && !fields[column].isEmpty(),
             QString("TXT 第 %1 行缺少映射列 %2").arg(lineNumber).arg(column + 1));
        return fields[column];
      };
      const auto label = className(field(m.classColumn), "class");
      if (unknown) {
        ++result.incompleteRecords;
        continue;
      }
      TimeRange time{0, target.sampleCount};
      TimeQuality quality = TimeQuality::SampleLabel;
      SampleIndex uncertainty = 0;
      QJsonObject conversion;
      if (m.timeUnit == LabelTimeUnit::Unknown) {
        quality = TimeQuality::Unknown;
      } else if (m.timeUnit == LabelTimeUnit::Seconds) {
        const double first = realText(field(m.startColumn), "start seconds");
        const double second = realText(field(m.endColumn), "end/count seconds");
        const double last = m.interval == LabelInterval::StartCount ? first + second : second;
        need(first >= 0 && second >= 0 && std::isfinite(last) && first < last &&
                 static_cast<long double>(last) * target.sampleRateHz <= target.sampleCount,
             "时间超出物理文件或为空");
        const long double startSamples = static_cast<long double>(first) * target.sampleRateHz;
        const long double endSamples = static_cast<long double>(last) * target.sampleRateHz;
        time = {rounded(first, target.sampleRateHz), rounded(last, target.sampleRateHz)};
        const bool fractional = std::abs(startSamples - time.begin) > 1e-7L ||
                                std::abs(endSamples - time.end) > 1e-7L;
        need(!fractional || m.allowTimeRounding, "秒坐标不落在样本边界，须明确允许最近样本舍入");
        if (fractional || m.timeQuantizationSeconds > 0) {
          quality = TimeQuality::RoundedLabel;
          const long double u = std::ceil(static_cast<long double>(target.sampleRateHz) *
                                           m.timeQuantizationSeconds / 2 + .5L);
          need(u < 18446744073709551616.0L, "量化不确定度溢出");
          uncertainty = static_cast<SampleIndex>(u);
        }
        conversion = {{"startRoundingSamples", static_cast<double>(time.begin - startSamples)},
                      {"endRoundingSamples", static_cast<double>(time.end - endSamples)}};
      } else {
        auto first = unsignedText(field(m.startColumn), "start index");
        auto last = unsignedText(field(m.endColumn), "end/count index");
        const auto origin = static_cast<std::uint64_t>(m.indexOrigin);
        need(first >= origin, "开始索引小于指定起点");
        first -= origin;
        if (m.interval == LabelInterval::StartCount) last = add(first, last);
        else {
          need(last >= origin, "结束索引小于指定起点");
          last -= origin;
          if (m.interval == LabelInterval::InclusiveEnd) last = add(last, 1);
        }
        need(first < last, "索引区间为空或倒置");
        time = m.timeUnit == LabelTimeUnit::Frames
                   ? TimeRange{multiply(first, m.frameHopSamples),
                               add(multiply(last - 1, m.frameHopSamples), m.frameLengthSamples)}
                   : TimeRange{first, last};
      }
      const double scale = m.frequencyUnit == LabelFrequencyUnit::Hz ? 1 :
                           m.frequencyUnit == LabelFrequencyUnit::KHz ? 1e3 :
                           m.frequencyUnit == LabelFrequencyUnit::MHz ? 1e6 : 1e9;
      const double f1 = realText(field(m.lowerColumn), "frequency first") * scale;
      const double f2 = realText(field(m.upperColumn), "frequency second") * scale;
      FrequencyRange band{f1, f2};
      if (m.frequencyLayout == LabelFrequencyLayout::CenterBandwidth) {
        need(f2 > 0, "带宽必须大于 0");
        band = {f1 - f2 / 2, f1 + f2 / 2};
      }
      if (m.frequencyReference == LabelFrequencyReference::Baseband) {
        band.lowerHz += target.centerFrequencyHz;
        band.upperHz += target.centerFrequencyHz;
      }
      auto a = annotation(result.sourceSet, QString::number(lineNumber), label);
      Segment segment{a.id + ":s0", {time, band},
                      quality == TimeQuality::Unknown ? TimeQuality::SampleLabel : quality,
                      FrequencyQuality::NominalLabel, false, uncertainty, FrequencyFrame::RF};
      const auto valid = validateSegment(segment, annotationBounds(target), false);
      need(valid.ok, q(valid.error));
      if (quality == TimeQuality::Unknown) {
        // This is file observation support, never an emission interval.
        a.observation = ViewRange{time, band};
        a.observationTimeQuality = TimeQuality::Unknown;
        a.observationFrequencyFrame = FrequencyFrame::RF;
      } else a.segments.push_back(segment);
      auto evidence = std::make_shared<OriginEvidence>();
      evidence->formatId = result.sourceSet.formatId;
      evidence->originRef = result.sourceSet.originalPath + "#" + a.originRecordId;
      QJsonArray originalFields;
      for (const auto& value : fields) originalFields.append(value);
      evidence->originalFieldsJson = QJsonDocument(QJsonObject{
          {"line", QString::number(lineNumber)}, {"fields", originalFields},
          {"mapping", mapping}, {"conversion", conversion}}).toJson(QJsonDocument::Compact).toStdString();
      evidence->coordinateInterpretation = result.sourceSet.coordinateInterpretation;
      evidence->originalSegments = a.segments;
      evidence->originalObservation = a.observation;
      a.originEvidence = std::move(evidence);
      refreshAnnotationProjection(a);
      appendImportedAnnotation(result, std::move(a), remaining);
    }
    if (result.incompleteRecords) {
      result.annotations.clear();
      result.state = LabelImportState::Incomplete;
      result.error = "频率未知；原文和类别保留，未生成二维标记；请补全字段映射";
      result.diagnostics.push_back({DiagnosticSeverity::Warning, "unknown-label-geometry",
                                    result.error.toStdString(), {}, 0});
      result.sourceSet.diagnostics = result.diagnostics;
    } else {
      if (recordCount == 0) result.state = LabelImportState::Empty;
      addPartialDiagnostic(result, target);
      validateNormalizedBatch(result, metadataBudget - sourceBytes - remaining);
    }
    control.check();
    return result;
  } catch (const std::exception& e) {
    return failure(std::move(result), e);
  }
}
}  // namespace signalstudio
