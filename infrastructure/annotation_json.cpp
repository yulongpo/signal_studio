#include "infrastructure/annotation_json.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

#include "application/annotation_service.h"
#include "infrastructure/sample_format_json.h"

namespace signalstudio {
namespace {
constexpr qsizetype metadataBudget = 32 * 1024 * 1024;
QString q(const std::string& s) {
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
QString u(std::uint64_t n) { return QString::number(n); }
[[noreturn]] void bad(const QString& s) {
  throw std::runtime_error(s.toUtf8().toStdString());
}
void require(bool condition, const QString& message) {
  if (!condition) bad(message);
}
std::string string(const QJsonObject& o, const char* key,
                   qsizetype limit = 1024) {
  const auto v = o[key];
  require(v.isString(), QString(key) + " 必须为字符串");
  const auto b = v.toString().toUtf8();
  require(b.size() <= limit, QString(key) + " 超过文本预算");
  return b.toStdString();
}
bool boolean(const QJsonObject& o, const char* key) {
  require(o[key].isBool(), QString(key) + " 必须为布尔值");
  return o[key].toBool();
}
double number(const QJsonValue& v, const QString& key) {
  require(v.isDouble() && std::isfinite(v.toDouble()), key + " 必须为有限数");
  return v.toDouble();
}
double number(const QJsonObject& o, const char* key) {
  return number(o[key], key);
}
std::uint64_t integer(const QJsonValue& v, const QString& key) {
  require(v.isString() && QRegularExpression("\\A(0|[1-9][0-9]{0,19})\\z")
                              .match(v.toString())
                              .hasMatch(),
          key + " 必须为规范十进制 uint64 字符串");
  bool ok = false;
  const auto n = v.toString().toULongLong(&ok);
  require(ok, key + " 超出 uint64");
  return n;
}
std::uint64_t integer(const QJsonObject& o, const char* key) {
  return integer(o[key], key);
}
QJsonObject object(const QJsonValue& v, const QString& key) {
  require(v.isObject(), key + " 必须为对象");
  return v.toObject();
}
QJsonArray array(const QJsonObject& o, const char* key,
                 qsizetype limit = annotationSegmentBudget) {
  require(o[key].isArray(), QString(key) + " 必须为数组");
  const auto a = o[key].toArray();
  require(a.size() <= limit, QString(key) + " 超过数量预算");
  return a;
}
int choice(const QJsonObject& o, const char* key, const QStringList& names) {
  require(o[key].isString(), QString(key) + " 枚举缺失");
  const auto n = names.indexOf(o[key].toString());
  require(n >= 0, QString(key) + " 枚举无效");
  return n;
}
QString enumText(int n, const QStringList& names) {
  return n >= 0 && n < names.size() ? names[n] : QString("invalid");
}
const QStringList sourceNames{"ImportedLabel", "Manual", "Algorithm"},
    reviewNames{"Pending", "Accepted", "Rejected"},
    frameNames{"RF", "Baseband", "RealOneSided"};
const QStringList timeNames{"SampleLabel",        "RoundedLabel",
                            "AlgorithmEstimated", "Manual",
                            "WindowObservation",  "Unknown"},
    frequencyNames{"NominalLabel", "ModelEstimated", "Refined", "Manual",
                   "Unknown"};
const QStringList methodNames{"Coarse", "Sample", "Roi", "None", "Manual"},
    statusNames{"Applied", "Fallback", "Skipped", "Unknown"},
    severityNames{"Info", "Warning", "Error"};
QJsonObject rangeJson(ViewRange r) {
  return {{"start", u(r.time.begin)},
          {"end", u(r.time.end)},
          {"lowerHz", r.frequency.lowerHz},
          {"upperHz", r.frequency.upperHz}};
}
ViewRange rangeValue(const QJsonValue& value) {
  const auto o = object(value, "range");
  return {{integer(o, "start"), integer(o, "end")},
          {number(o, "lowerHz"), number(o, "upperHz")}};
}
QJsonValue optionalRange(const std::optional<ViewRange>& r) {
  return r ? QJsonValue(rangeJson(*r)) : QJsonValue(QJsonValue::Null);
}
std::optional<ViewRange> optionalRangeValue(const QJsonValue& v) {
  require(!v.isUndefined(), "可空 range 缺失");
  return v.isNull() ? std::optional<ViewRange>{}
                    : std::optional<ViewRange>{rangeValue(v)};
}
QJsonValue optionalNumber(const std::optional<double>& n) {
  return n ? QJsonValue(*n) : QJsonValue(QJsonValue::Null);
}
std::optional<double> optionalNumberValue(const QJsonValue& v,
                                          const QString& key) {
  require(!v.isUndefined(), key + " 缺失");
  return v.isNull() ? std::optional<double>{}
                    : std::optional<double>{number(v, key)};
}
QJsonValue optionalString(const std::optional<std::string>& v) {
  return v ? QJsonValue(q(*v)) : QJsonValue(QJsonValue::Null);
}
std::optional<std::string> optionalStringValue(const QJsonValue& v,
                                               qsizetype limit) {
  require(!v.isUndefined(), "可空文本缺失");
  if (v.isNull()) return {};
  require(v.isString() && v.toString().toUtf8().size() <= limit,
          "可空文本无效或超过预算");
  return v.toString().toUtf8().toStdString();
}
QJsonArray stringsJson(const std::vector<std::string>& strings) {
  QJsonArray a;
  for (const auto& s : strings) a.append(q(s));
  return a;
}
std::vector<std::string> stringsValue(const QJsonObject& o, const char* key,
                                      qsizetype count = 128,
                                      qsizetype length = 1024) {
  std::vector<std::string> out;
  for (const auto v : array(o, key, count)) {
    require(v.isString() && v.toString().toUtf8().size() <= length,
            QString(key) + " 文本无效");
    out.push_back(v.toString().toUtf8().toStdString());
  }
  return out;
}
QJsonObject segmentJson(const Segment& s) {
  return {{"id", q(s.id)},
          {"range", rangeJson(s.range)},
          {"timeQuality", enumText(static_cast<int>(s.timeQuality), timeNames)},
          {"frequencyQuality",
           enumText(static_cast<int>(s.frequencyQuality), frequencyNames)},
          {"confirmed", s.confirmed},
          {"uncertaintySamples", u(s.uncertaintySamples)},
          {"frequencyFrame",
           enumText(static_cast<int>(s.frequencyFrame), frameNames)},
          {"clippedAtRoi", s.clippedAtRoi}};
}
Segment segmentValue(const QJsonValue& v, const AnnotationBounds& bounds) {
  const auto o = object(v, "segment");
  Segment s;
  s.id = string(o, "id", 128);
  s.range = rangeValue(o["range"]);
  s.timeQuality = static_cast<TimeQuality>(choice(o, "timeQuality", timeNames));
  s.frequencyQuality = static_cast<FrequencyQuality>(
      choice(o, "frequencyQuality", frequencyNames));
  s.confirmed = boolean(o, "confirmed");
  s.uncertaintySamples = integer(o, "uncertaintySamples");
  s.frequencyFrame =
      static_cast<FrequencyFrame>(choice(o, "frequencyFrame", frameNames));
  s.clippedAtRoi = boolean(o, "clippedAtRoi");
  const auto check = validateSegment(s, bounds, false);
  require(check.ok, q(check.error));
  return s;
}
QJsonArray segmentsJson(const std::vector<Segment>& segments) {
  QJsonArray a;
  for (const auto& s : segments) a.append(segmentJson(s));
  return a;
}
std::vector<Segment> segmentsValue(const QJsonObject& o, const char* key,
                                   const AnnotationBounds& bounds) {
  std::vector<Segment> out;
  for (const auto v : array(o, key)) out.push_back(segmentValue(v, bounds));
  return out;
}
QJsonObject diagnosticJson(const AnnotationDiagnostic& d) {
  return {{"severity", enumText(static_cast<int>(d.severity), severityNames)},
          {"code", q(d.code)},
          {"message", q(d.message)},
          {"originRecordId", q(d.originRecordId)},
          {"line", u(d.line)}};
}
AnnotationDiagnostic diagnosticValue(const QJsonValue& v) {
  const auto o = object(v, "diagnostic");
  return {static_cast<DiagnosticSeverity>(choice(o, "severity", severityNames)),
          string(o, "code", 128), string(o, "message", 4096),
          string(o, "originRecordId", 128), integer(o, "line")};
}
QJsonArray diagnosticsJson(
    const std::vector<AnnotationDiagnostic>& diagnostics) {
  QJsonArray a;
  for (const auto& d : diagnostics) a.append(diagnosticJson(d));
  return a;
}
SourceIdentity identityValue(const QJsonObject& o) {
  SourceIdentity s;
  s.projectGeneration = integer(o, "projectGeneration");
  s.formatRevision = integer(o, "formatRevision");
  s.fileId = string(o, "fileId", 128);
  s.path = string(o, "path", 32768);
  s.quickFingerprint = string(o, "quickFingerprint", 4096);
  s.contentHash = string(o, "contentHash", 256);
  const auto channel = integer(o, "selectedSampleChannel");
  require(channel <= 127, "样本通道超出范围");
  s.selectedSampleChannel = static_cast<std::uint32_t>(channel);
  const auto format = object(o["sampleFormat"], "sampleFormat");
  integer(format, "headerBytes");
  integer(format, "trailerBytes");
  s.sampleFormat = decodeSampleFormat(format);
  require(channel == s.sampleFormat.selectedChannel, "通道身份与格式不一致");
  s.sampleRateHz = number(o, "sampleRateHz");
  s.centerFrequencyHz = number(o, "centerFrequencyHz");
  const auto band = object(o["effectiveFrequency"], "effectiveFrequency");
  s.effectiveFrequency = {number(band, "lowerHz"), number(band, "upperHz")};
  s.physicalSamples = integer(o, "physicalSamples");
  s.availableSamples = integer(o, "availableSamples");
  require(s.formatRevision > 0 && !s.fileId.empty() && s.sampleRateHz > 0 &&
              s.sampleRateHz <= 1e12 && s.centerFrequencyHz >= 0 &&
              s.centerFrequencyHz <= 1e12 && s.physicalSamples > 0 &&
              s.availableSamples <= s.physicalSamples,
          "来源身份采样或版本无效");
  const auto lower = s.sampleFormat.structure == SampleStructure::Real
                         ? 0
                         : s.centerFrequencyHz - s.sampleRateHz / 2;
  const auto upper = s.sampleFormat.structure == SampleStructure::Real
                         ? s.sampleRateHz / 2
                         : s.centerFrequencyHz + s.sampleRateHz / 2;
  require(s.effectiveFrequency.lowerHz < s.effectiveFrequency.upperHz &&
              s.effectiveFrequency.lowerHz >= lower &&
              s.effectiveFrequency.upperHz <= upper,
          "来源身份有效频段超出 Nyquist");
  require(!o["hashedRange"].isUndefined(), "hashedRange 缺失");
  if (!o["hashedRange"].isNull()) {
    const auto range = object(o["hashedRange"], "hashedRange");
    s.hashedRange = TimeRange{integer(range, "start"), integer(range, "end")};
    require(s.hashedRange->begin < s.hashedRange->end &&
                s.hashedRange->end <= s.physicalSamples,
            "摘要范围越界");
  }
  return s;
}
void validateObservation(const std::optional<ViewRange>& range,
                         FrequencyFrame frame, const AnnotationBounds& bounds) {
  if (!range) return;
  Segment temporary{"observation",
                    *range,
                    TimeQuality::SampleLabel,
                    FrequencyQuality::NominalLabel,
                    false,
                    0,
                    frame};
  const auto valid = validateSegment(temporary, bounds, false);
  require(valid.ok, q(valid.error));
}
OriginEvidence originValue(const QJsonObject& o,
                           const AnnotationBounds& bounds) {
  OriginEvidence e;
  e.formatId = string(o, "formatId", 128);
  e.originRef = string(o, "originRef", 1024);
  e.originalFieldsJson = string(o, "originalFieldsJson", metadataBudget);
  require(
      QJsonDocument::fromJson(QByteArray::fromStdString(e.originalFieldsJson))
          .isObject(),
      "原始字段快照不是 JSON 对象");
  e.coordinateInterpretation = string(o, "coordinateInterpretation", 4096);
  e.runId = string(o, "runId", 128);
  e.parentCandidateId = string(o, "parentCandidateId", 128);
  e.originalSegments = segmentsValue(o, "originalSegments", bounds);
  e.originalObservation = optionalRangeValue(o["originalObservation"]);
  e.observationFrequencyFrame = static_cast<FrequencyFrame>(
      choice(o, "observationFrequencyFrame", frameNames));
  validateObservation(e.originalObservation, e.observationFrequencyFrame,
                      bounds);
  e.refineMethod =
      static_cast<RefineMethod>(choice(o, "refineMethod", methodNames));
  e.refineStatus =
      static_cast<RefineStatus>(choice(o, "refineStatus", statusNames));
  e.fallbackReason = string(o, "fallbackReason", 1024);
  e.scoreMean = optionalNumberValue(o["scoreMean"], "scoreMean");
  e.scoreMax = optionalNumberValue(o["scoreMax"], "scoreMax");
  e.estimatedCnrDb = optionalNumberValue(o["estimatedCnrDb"], "estimatedCnrDb");
  for (const auto value : array(o, "windowIds"))
    e.windowIds.push_back(integer(value, "windowId"));
  e.windowCount = integer(o, "windowCount");
  e.evidenceRefs = stringsValue(o, "evidenceRefs", annotationSegmentBudget);
  return e;
}
QJsonObject originJson(const OriginEvidence& e) {
  QJsonArray windows;
  for (const auto id : e.windowIds) windows.append(u(id));
  return {
      {"formatId", q(e.formatId)},
      {"originRef", q(e.originRef)},
      {"originalFieldsJson", q(e.originalFieldsJson)},
      {"coordinateInterpretation", q(e.coordinateInterpretation)},
      {"runId", q(e.runId)},
      {"parentCandidateId", q(e.parentCandidateId)},
      {"originalSegments", segmentsJson(e.originalSegments)},
      {"originalObservation", optionalRange(e.originalObservation)},
      {"observationFrequencyFrame",
       enumText(static_cast<int>(e.observationFrequencyFrame), frameNames)},
      {"refineMethod", enumText(static_cast<int>(e.refineMethod), methodNames)},
      {"refineStatus", enumText(static_cast<int>(e.refineStatus), statusNames)},
      {"fallbackReason", q(e.fallbackReason)},
      {"scoreMean", optionalNumber(e.scoreMean)},
      {"scoreMax", optionalNumber(e.scoreMax)},
      {"estimatedCnrDb", optionalNumber(e.estimatedCnrDb)},
      {"windowIds", windows},
      {"windowCount", u(e.windowCount)},
      {"evidenceRefs", stringsJson(e.evidenceRefs)}};
}
QJsonObject revisionJson(const AnnotationRevision& r) {
  QJsonArray changes;
  for (const auto& c : r.segmentChanges)
    changes.append(
        QJsonObject{{"id", q(c.id)},
                    {"before", c.before ? QJsonValue(segmentJson(*c.before))
                                        : QJsonValue(QJsonValue::Null)},
                    {"after", c.after ? QJsonValue(segmentJson(*c.after))
                                      : QJsonValue(QJsonValue::Null)}});
  return {{"id", q(r.id)},
          {"parentRevisionId", q(r.parentRevisionId)},
          {"action", q(r.action)},
          {"actor", q(r.actor)},
          {"timestamp", q(r.timestamp)},
          {"note", q(r.note)},
          {"segmentChanges", changes},
          {"nameBefore", optionalString(r.nameBefore)},
          {"nameAfter", optionalString(r.nameAfter)},
          {"noteBefore", optionalString(r.noteBefore)},
          {"noteAfter", optionalString(r.noteAfter)},
          {"observationChanged", r.observationChanged},
          {"observationBefore", optionalRange(r.observationBefore)},
          {"observationAfter", optionalRange(r.observationAfter)}};
}
AnnotationRevision revisionValue(const QJsonValue& v,
                                 const AnnotationBounds& bounds,
                                 FrequencyFrame frame) {
  const auto o = object(v, "revision");
  AnnotationRevision r;
  r.id = string(o, "id", 128);
  r.parentRevisionId = string(o, "parentRevisionId", 128);
  r.action = string(o, "action", 128);
  r.actor = string(o, "actor", 256);
  r.timestamp = string(o, "timestamp", 128);
  r.note = string(o, "note", 4096);
  std::unordered_set<std::string> ids;
  for (const auto value : array(o, "segmentChanges")) {
    const auto item = object(value, "segmentChange");
    SegmentChange c;
    c.id = string(item, "id", 128);
    require(!c.id.empty() && ids.insert(c.id).second,
            "重复或空 SegmentChange ID");
    require(!item["before"].isUndefined() && !item["after"].isUndefined(),
            "SegmentChange 快照缺失");
    if (!item["before"].isNull())
      c.before = segmentValue(item["before"], bounds);
    if (!item["after"].isNull()) c.after = segmentValue(item["after"], bounds);
    require((c.before || c.after) && (!c.before || c.before->id == c.id) &&
                (!c.after || c.after->id == c.id),
            "SegmentChange 身份不一致");
    r.segmentChanges.push_back(std::move(c));
  }
  r.nameBefore = optionalStringValue(o["nameBefore"], 256);
  r.nameAfter = optionalStringValue(o["nameAfter"], 256);
  r.noteBefore = optionalStringValue(o["noteBefore"], 4096);
  r.noteAfter = optionalStringValue(o["noteAfter"], 4096);
  r.observationChanged = boolean(o, "observationChanged");
  r.observationBefore = optionalRangeValue(o["observationBefore"]);
  r.observationAfter = optionalRangeValue(o["observationAfter"]);
  validateObservation(r.observationBefore, frame, bounds);
  validateObservation(r.observationAfter, frame, bounds);
  return r;
}
template <class T, class F>
bool decodeSafely(const QJsonObject& o, T& target, QString& error, F fn) {
  try {
    require(QJsonDocument(o).toJson(QJsonDocument::Compact).size() <=
                metadataBudget,
            "标记元数据超过 32 MiB 预算");
    auto result = fn();
    target = std::move(result);
    error.clear();
    return true;
  } catch (const std::exception& e) {
    error = QString::fromUtf8(e.what());
    return false;
  }
}
QString csv(const QString& text) {
  auto out = text;
  out.replace('"', "\"\"");
  return '"' + out + '"';
}
}  // namespace

QJsonObject encodeSourceIdentity(const SourceIdentity& s) {
  QJsonValue hashed(QJsonValue::Null);
  if (s.hashedRange)
    hashed = QJsonObject{{"start", u(s.hashedRange->begin)},
                         {"end", u(s.hashedRange->end)}};
  return {{"projectGeneration", u(s.projectGeneration)},
          {"formatRevision", u(s.formatRevision)},
          {"fileId", q(s.fileId)},
          {"path", q(s.path)},
          {"quickFingerprint", q(s.quickFingerprint)},
          {"contentHash", q(s.contentHash)},
          {"selectedSampleChannel", u(s.selectedSampleChannel)},
          {"sampleFormat", encodeSampleFormat(s.sampleFormat)},
          {"sampleRateHz", s.sampleRateHz},
          {"centerFrequencyHz", s.centerFrequencyHz},
          {"effectiveFrequency",
           QJsonObject{{"lowerHz", s.effectiveFrequency.lowerHz},
                       {"upperHz", s.effectiveFrequency.upperHz}}},
          {"physicalSamples", u(s.physicalSamples)},
          {"availableSamples", u(s.availableSamples)},
          {"hashedRange", hashed}};
}
bool decodeSourceIdentity(const QJsonObject& o, SourceIdentity& target,
                          QString& error) {
  return decodeSafely(o, target, error, [&] { return identityValue(o); });
}
QJsonObject encodeSourceSet(const AnnotationSourceSet& s) {
  return {{"id", q(s.id)},
          {"fileId", q(s.fileId)},
          {"sampleChannel", u(s.sampleChannel)},
          {"sourceKind", enumText(static_cast<int>(s.sourceKind), sourceNames)},
          {"formatId", q(s.formatId)},
          {"parserId", q(s.parserId)},
          {"parserVersion", q(s.parserVersion)},
          {"profileId", q(s.profileId)},
          {"profileVersion", q(s.profileVersion)},
          {"originalPath", q(s.originalPath)},
          {"originalText", q(s.originalText)},
          {"sourceHash", q(s.sourceHash)},
          {"supplementalOriginalText", q(s.supplementalOriginalText)},
          {"supplementalHash", q(s.supplementalHash)},
          {"coordinateInterpretation", q(s.coordinateInterpretation)},
          {"runId", q(s.runId)},
          {"sourceIdentity", encodeSourceIdentity(s.sourceIdentity)},
          {"diagnostics", diagnosticsJson(s.diagnostics)},
          {"deleted", s.deleted}};
}
bool decodeSourceSet(const QJsonObject& o, const FileMetadata& metadata,
                     AnnotationSourceSet& target, QString& error) {
  return decodeSafely(o, target, error, [&] {
    AnnotationSourceSet s;
    s.id = string(o, "id", 128);
    s.fileId = string(o, "fileId", 128);
    const auto channel = integer(o, "sampleChannel");
    require(channel < metadata.sampleFormat.channels, "来源集合通道越界");
    s.sampleChannel = static_cast<std::uint32_t>(channel);
    s.sourceKind =
        static_cast<AnnotationSource>(choice(o, "sourceKind", sourceNames));
    for (auto [key, field] :
         std::initializer_list<std::pair<const char*, std::string*>>{
             {"formatId", &s.formatId},
             {"parserId", &s.parserId},
             {"parserVersion", &s.parserVersion},
             {"profileId", &s.profileId},
             {"profileVersion", &s.profileVersion},
             {"sourceHash", &s.sourceHash},
             {"supplementalHash", &s.supplementalHash},
             {"runId", &s.runId}})
      *field = string(o, key, 256);
    s.originalPath = string(o, "originalPath", 32768);
    s.originalText = string(o, "originalText", metadataBudget);
    s.supplementalOriginalText =
        string(o, "supplementalOriginalText", metadataBudget);
    s.coordinateInterpretation = string(o, "coordinateInterpretation", 4096);
    s.sourceIdentity =
        identityValue(object(o["sourceIdentity"], "sourceIdentity"));
    for (const auto v : array(o, "diagnostics"))
      s.diagnostics.push_back(diagnosticValue(v));
    s.deleted = boolean(o, "deleted");
    require(!s.id.empty() && s.fileId == metadata.id &&
                s.sourceIdentity.fileId == s.fileId &&
                s.sourceIdentity.selectedSampleChannel == s.sampleChannel,
            "来源集合身份与目标不一致");
    require(s.originalText.size() + s.supplementalOriginalText.size() <=
                metadataBudget,
            "来源文本总预算超出 32 MiB");
    return s;
  });
}
QJsonObject encodeAnnotation(const Annotation& a) {
  QJsonArray revisions, reviews;
  for (const auto& r : a.revisions) revisions.append(revisionJson(r));
  for (const auto& r : a.reviewHistory)
    reviews.append(QJsonObject{
        {"status", enumText(static_cast<int>(r.status), reviewNames)},
        {"revisionId", q(r.revisionId)},
        {"actor", q(r.actor)},
        {"timestamp", q(r.timestamp)},
        {"note", q(r.note)}});
  return {
      {"id", q(a.id)},
      {"name", q(a.name)},
      {"fileId", q(a.fileId)},
      {"sourceSetId", q(a.sourceSetId)},
      {"originRecordId", q(a.originRecordId)},
      {"sourceKind", enumText(static_cast<int>(a.sourceKind), sourceNames)},
      {"classLabel", optionalString(a.classLabel)},
      {"note", q(a.note)},
      {"activeSegmentId", q(a.activeSegmentId)},
      {"revisionId", q(a.revisionId)},
      {"segments", segmentsJson(a.segments)},
      {"observation", optionalRange(a.observation)},
      {"observationFrequencyFrame",
       enumText(static_cast<int>(a.observationFrequencyFrame), frameNames)},
      {"observationTimeQuality",
       enumText(static_cast<int>(a.observationTimeQuality), timeNames)},
      {"originEvidence", a.originEvidence
                             ? QJsonValue(originJson(*a.originEvidence))
                             : QJsonValue(QJsonValue::Null)},
      {"reviewStatus", enumText(static_cast<int>(a.reviewStatus), reviewNames)},
      {"reviewRevisionId", q(a.reviewRevisionId)},
      {"reviewHistory", reviews},
      {"revisions", revisions},
      {"derivedFrom", stringsJson(a.derivedFrom)},
      {"deleted", a.deleted}};
}
bool decodeAnnotation(const QJsonObject& o, const FileMetadata& metadata,
                      Annotation& target, QString& error) {
  return decodeSafely(o, target, error, [&] {
    Annotation a;
    const auto bounds = annotationBounds(metadata);
    for (auto [key, field] :
         std::initializer_list<std::pair<const char*, std::string*>>{
             {"id", &a.id},
             {"fileId", &a.fileId},
             {"sourceSetId", &a.sourceSetId},
             {"originRecordId", &a.originRecordId},
             {"activeSegmentId", &a.activeSegmentId},
             {"revisionId", &a.revisionId},
             {"reviewRevisionId", &a.reviewRevisionId}})
      *field = string(o, key, 128);
    a.name = string(o, "name", 256);
    a.note = string(o, "note", 4096);
    a.classLabel = optionalStringValue(o["classLabel"], 256);
    a.sourceKind =
        static_cast<AnnotationSource>(choice(o, "sourceKind", sourceNames));
    a.segments = segmentsValue(o, "segments", bounds);
    a.observation = optionalRangeValue(o["observation"]);
    a.observationFrequencyFrame = static_cast<FrequencyFrame>(
        choice(o, "observationFrequencyFrame", frameNames));
    a.observationTimeQuality = static_cast<TimeQuality>(
        choice(o, "observationTimeQuality", timeNames));
    a.originEvidence = std::make_shared<const OriginEvidence>(
        originValue(object(o["originEvidence"], "originEvidence"), bounds));
    a.reviewStatus =
        static_cast<ReviewStatus>(choice(o, "reviewStatus", reviewNames));
    a.deleted = boolean(o, "deleted");
    std::unordered_set<std::string> revisionIds;
    for (const auto v : array(o, "revisions")) {
      auto r = revisionValue(v, bounds, a.observationFrequencyFrame);
      require(r.parentRevisionId.empty() ||
                  revisionIds.contains(r.parentRevisionId),
              "修订父节点必须已经存在");
      require(revisionIds.insert(r.id).second, "修订 ID 重复");
      a.revisions.push_back(std::move(r));
    }
    for (const auto v : array(o, "reviewHistory")) {
      const auto r = object(v, "review");
      a.reviewHistory.push_back(
          {static_cast<ReviewStatus>(choice(r, "status", reviewNames)),
           string(r, "revisionId", 128), string(r, "actor", 256),
           string(r, "timestamp", 128), string(r, "note", 4096)});
    }
    a.derivedFrom = stringsValue(o, "derivedFrom");
    require(a.fileId == metadata.id, "标记目标文件身份不一致");
    const auto result = validateAnnotation(a, bounds);
    require(result.ok, q(result.error));
    refreshAnnotationProjection(a);
    return a;
  });
}
QJsonObject encodeAnnotationChannelSnapshot(
    const AnnotationChannelSnapshot& s) {
  return {
      {"annotationId", q(s.annotationId)},
      {"segmentId", q(s.segmentId)},
      {"revisionId", q(s.revisionId)},
      {"name", q(s.name)},
      {"range", rangeJson(s.range)},
      {"bindingKind", s.bindingKind == ChannelBindingKind::LocatedSegment
                          ? "LocatedSegment"
                          : "UserRange"},
      {"boundRange", rangeJson(s.boundRange.value_or(s.range))},
      {"sourceKind", enumText(static_cast<int>(s.sourceKind), sourceNames)},
      {"reviewStatus", enumText(static_cast<int>(s.reviewStatus), reviewNames)},
      {"sourceIdentity", encodeSourceIdentity(s.sourceIdentity)}};
}
bool decodeAnnotationChannelSnapshot(const QJsonObject& o,
                                     const FileMetadata& metadata,
                                     AnnotationChannelSnapshot& target,
                                     QString& error, int projectVersion) {
  return decodeSafely(o, target, error, [&] {
    AnnotationChannelSnapshot s;
    s.annotationId = string(o, "annotationId", 128);
    s.segmentId = string(o, "segmentId", 128);
    s.revisionId = string(o, "revisionId", 128);
    s.name = string(o, "name", 256);
    s.range = rangeValue(o["range"]);
    if (projectVersion >= 6) {
      const auto kind = string(o, "bindingKind", 32);
      require(kind == "LocatedSegment" || kind == "UserRange",
              "通道绑定类型无效");
      s.bindingKind = kind == "UserRange" ? ChannelBindingKind::UserRange
                                          : ChannelBindingKind::LocatedSegment;
      s.boundRange = rangeValue(o["boundRange"]);
    } else {
      s.boundRange = s.range;
    }
    s.sourceKind =
        static_cast<AnnotationSource>(choice(o, "sourceKind", sourceNames));
    s.reviewStatus =
        static_cast<ReviewStatus>(choice(o, "reviewStatus", reviewNames));
    s.sourceIdentity =
        identityValue(object(o["sourceIdentity"], "sourceIdentity"));
    require(!s.annotationId.empty() &&
                (s.bindingKind == ChannelBindingKind::LocatedSegment
                     ? !s.segmentId.empty()
                     : s.segmentId.empty()) &&
                !s.revisionId.empty() && s.sourceIdentity.fileId == metadata.id,
            "通道快照身份无效");
    FileMetadata original = metadata;
    original.sampleFormat = s.sourceIdentity.sampleFormat;
    original.sampleRateHz = s.sourceIdentity.sampleRateHz;
    original.centerFrequencyHz = s.sourceIdentity.centerFrequencyHz;
    original.sampleCount = s.sourceIdentity.physicalSamples;
    original.effectiveBandwidthHz =
        s.sourceIdentity.effectiveFrequency.upperHz -
        s.sourceIdentity.effectiveFrequency.lowerHz;
    original.availability.availableSamples = s.sourceIdentity.availableSamples;
    original.availability.status = LoadStatus::Partial;
    const auto bounds = annotationBounds(original);
    Segment segment{"snapshot",
                    s.range,
                    TimeQuality::SampleLabel,
                    FrequencyQuality::NominalLabel,
                    false,
                    0,
                    original.sampleFormat.structure == SampleStructure::Real
                        ? FrequencyFrame::RealOneSided
                        : FrequencyFrame::RF};
    const auto checked = validateSegment(segment, bounds, false);
    require(checked.ok, q(checked.error));
    segment.range = *s.boundRange;
    const auto bindingChecked = validateSegment(segment, bounds, false);
    require(bindingChecked.ok, q(bindingChecked.error));
    if (s.bindingKind == ChannelBindingKind::LocatedSegment)
      require(rangeInside(s.range, *s.boundRange),
              "通道快照的提取时间或频带超出创建时绑定段");
    return s;
  });
}
QByteArray exportAnnotationsJson(const FileState& file) {
  QJsonArray sets, annotations;
  for (const auto& set : file.sourceSets) sets.append(encodeSourceSet(set));
  for (const auto& a : file.marks) annotations.append(encodeAnnotation(a));
  return QJsonDocument(QJsonObject{{"schema", "signalstudio-annotations-v1"},
                                   {"fileId", q(file.metadata.id)},
                                   {"sourceSets", sets},
                                   {"annotations", annotations}})
      .toJson(QJsonDocument::Indented);
}
QByteArray exportAnnotationsCsv(const FileState& file) {
  QByteArray out =
      "annotation_id,segment_id,source_set_id,source_kind,review_status,"
      "revision_id,class_label,name,start_sample,end_sample,lower_hz,upper_hz,"
      "frequency_frame,time_quality,frequency_quality,confirmed,uncertainty_"
      "samples,deleted\n";
  for (const auto& a : file.marks)
    for (const auto& s : a.segments) {
      QStringList fields{
          csv(q(a.id)),
          csv(q(s.id)),
          csv(q(a.sourceSetId)),
          csv(enumText(static_cast<int>(a.sourceKind), sourceNames)),
          csv(enumText(static_cast<int>(a.reviewStatus), reviewNames)),
          csv(q(a.revisionId)),
          csv(a.classLabel ? q(*a.classLabel) : QString()),
          csv(q(a.name)),
          u(s.range.time.begin),
          u(s.range.time.end),
          QString::number(s.range.frequency.lowerHz, 'g', 17),
          QString::number(s.range.frequency.upperHz, 'g', 17),
          csv(enumText(static_cast<int>(s.frequencyFrame), frameNames)),
          csv(enumText(static_cast<int>(s.timeQuality), timeNames)),
          csv(enumText(static_cast<int>(s.frequencyQuality), frequencyNames)),
          s.confirmed ? "true" : "false",
          u(s.uncertaintySamples),
          a.deleted ? "true" : "false"};
      out += fields.join(',').toUtf8() + '\n';
    }
  return out;
}
}  // namespace signalstudio
