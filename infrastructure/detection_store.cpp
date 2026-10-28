#include "infrastructure/detection_store.h"
#include "infrastructure/runtime_log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

#include "infrastructure/annotation_json.h"
#include "infrastructure/project_file.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace signalstudio {
namespace {
constexpr qint64 kChunkBytes = 8 * 1024 * 1024;
constexpr quint64 kStoreBytes = 256 * 1024 * 1024;
quint64 jsonStoreLimit(const DetectionRequest& request) {
  // Legacy combined manifests keep the historical chunk-file ceiling. A newer
  // evidence budget can raise that ceiling; each shard stays at 8 MiB.
  if (request.legacyCombinedEvidence) return kStoreBytes;
  return std::max(kStoreBytes, request.evidenceBudgetBytes);
}
constexpr qsizetype kChunkRecords = 4096;
constexpr qsizetype kMaximumRuns = 10000;

QString text(const std::string& value) { return QString::fromStdString(value); }
QString decimal(std::uint64_t value) { return QString::number(value); }
QString digest(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
void require(bool condition, const QString& message) {
  if (!condition) throw std::runtime_error(message.toUtf8().toStdString());
}
bool hashValid(const QString& value) {
  return QRegularExpression("\\A[0-9a-f]{64}\\z").match(value).hasMatch();
}
void onlyKeys(const QJsonObject& object, const QSet<QString>& keys) {
  for (auto it = object.begin(); it != object.end(); ++it)
    require(keys.contains(it.key()), "Unknown detection field: " + it.key());
}
std::string string(const QJsonObject& object, const char* key,
                   qsizetype maximum = 1024, bool nonempty = false) {
  const auto value = object[key];
  require(value.isString(), QString(key) + " must be a string");
  const auto result = value.toString();
  require(result.toUtf8().size() <= maximum && !result.contains(QChar(0)) &&
              (!nonempty || !result.isEmpty()),
          QString(key) + " exceeds its text budget or is empty");
  return result.toStdString();
}
quint64 integer(const QJsonValue& value, const QString& key) {
  require(value.isString() && QRegularExpression("\\A(?:0|[1-9][0-9]{0,19})\\z")
                                  .match(value.toString())
                                  .hasMatch(),
          key + " requires a canonical uint64 decimal string");
  bool ok = false;
  const auto result = value.toString().toULongLong(&ok);
  require(ok, key + " overflows uint64");
  return result;
}
double number(const QJsonObject& object, const char* key) {
  require(object[key].isDouble() && std::isfinite(object[key].toDouble()),
          QString(key) + " requires a finite number");
  return object[key].toDouble();
}
std::uint32_t smallInteger(const QJsonObject& object, const char* key,
                           std::uint32_t maximum) {
  const auto value = number(object, key);
  require(value >= 0 && value <= maximum && std::floor(value) == value,
          QString(key) + " is outside its integer budget");
  return static_cast<std::uint32_t>(value);
}
bool boolean(const QJsonObject& object, const char* key) {
  require(object[key].isBool(), QString(key) + " requires a boolean");
  return object[key].toBool();
}
QJsonObject objectValue(const QJsonValue& value, const QString& key) {
  require(value.isObject(), key + " requires an object");
  return value.toObject();
}
QJsonArray arrayValue(const QJsonObject& object, const char* key,
                      qsizetype maximum) {
  require(object[key].isArray(), QString(key) + " requires an array");
  const auto result = object[key].toArray();
  require(result.size() <= maximum,
          QString(key) + " exceeds its record budget");
  return result;
}
void finiteJson(const QJsonValue& value, int depth = 0) {
  require(depth <= 64, "Detection JSON exceeds its nesting budget");
  if (value.isDouble())
    require(std::isfinite(value.toDouble()),
            "Detection JSON contains nonfinite numbers");
  else if (value.isObject()) {
    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
      finiteJson(it.value(), depth + 1);
  } else if (value.isArray()) {
    for (const auto& child : value.toArray()) finiteJson(child, depth + 1);
  }
}
template <typename Enum>
Enum enumeration(const QJsonObject& object, const char* key,
                 const QStringList& values) {
  const auto value = string(object, key, 64, true);
  const auto index = values.indexOf(text(value));
  require(index >= 0, QString(key) + " has an unknown enum value");
  return static_cast<Enum>(index);
}
QString enumText(int value, const QStringList& values) {
  require(value >= 0 && value < values.size(), "Invalid detection enum");
  return values[value];
}
const QStringList kLifecycles{"Queued",     "Validating", "Running",
                              "Cancelling", "Completed",  "Cancelled",
                              "Failed",     "Interrupted"};
const QStringList kValidity{"Current", "Missing", "Changed", "Unsupported"};
const QStringList kCoverage{"read", "eligible", "completed", "excluded",
                            "unread"};
const QStringList kBackend{"TensorRt", "Scn", "Experimental17", "Fixture"};
const QStringList kStrategy{"sample", "roi", "coarse", "none"};

QJsonObject rangeJson(const ViewRange& range) {
  return {{"startSample", decimal(range.time.begin)},
          {"endSample", decimal(range.time.end)},
          {"frequencyLowerHz", range.frequency.lowerHz},
          {"frequencyUpperHz", range.frequency.upperHz},
          {"frequencyFrame", "RF"}};
}
ViewRange rangeValue(const QJsonValue& value) {
  const auto object = objectValue(value, "range");
  onlyKeys(object, {"startSample", "endSample", "frequencyLowerHz",
                    "frequencyUpperHz", "frequencyFrame"});
  require(object["frequencyFrame"] == "RF",
          "Detection storage requires the RF coordinate frame");
  return {
      {integer(object["startSample"], "startSample"),
       integer(object["endSample"], "endSample")},
      {number(object, "frequencyLowerHz"), number(object, "frequencyUpperHz")}};
}
bool emptyRange(const ViewRange& range) { return range == ViewRange{}; }
bool contained(const ViewRange& inner, const ViewRange& outer) {
  return inner.time.begin < inner.time.end &&
         inner.time.begin >= outer.time.begin &&
         inner.time.end <= outer.time.end &&
         std::isfinite(inner.frequency.lowerHz) &&
         std::isfinite(inner.frequency.upperHz) &&
         inner.frequency.lowerHz < inner.frequency.upperHz &&
         inner.frequency.lowerHz >= outer.frequency.lowerHz &&
         inner.frequency.upperHz <= outer.frequency.upperHz;
}
QJsonObject parametersJson(const DetectionParameters& p) {
  return {
      {"scoreThreshold", p.scoreThreshold},
      {"nmsIou", p.nmsIou},
      {"topK", static_cast<int>(p.topK)},
      {"batchSize", static_cast<int>(p.batchSize)},
      {"minHits", static_cast<int>(p.minHits)},
      {"maxMiss", static_cast<int>(p.maxMiss)},
      {"mergeFrequencyHz", p.mergeFrequencyHz},
      {"bandwidthMarginDb", p.bandwidthMarginDb},
      {"timeStrategy", enumText(static_cast<int>(p.timeStrategy), kStrategy)},
      {"splitBursts", p.splitBursts},
      {"sampleEdgeRatio", p.sampleEdgeRatio},
      {"minOnSeconds", p.minOnSeconds},
      {"minOffSeconds", p.minOffSeconds},
      {"smoothSeconds", p.smoothSeconds},
      {"coarseSplitGapSeconds", p.coarseSplitGapSeconds},
      {"coarseMinBurstSeconds", p.coarseMinBurstSeconds}};
}
DetectionParameters parametersValue(const QJsonValue& value) {
  const auto o = objectValue(value, "parameters");
  onlyKeys(
      o, {"scoreThreshold", "nmsIou", "topK", "batchSize", "minHits", "maxMiss",
          "mergeFrequencyHz", "bandwidthMarginDb", "timeStrategy",
          "splitBursts", "sampleEdgeRatio", "minOnSeconds", "minOffSeconds",
          "smoothSeconds", "coarseSplitGapSeconds", "coarseMinBurstSeconds"});
  DetectionParameters p;
  p.scoreThreshold = number(o, "scoreThreshold");
  p.nmsIou = number(o, "nmsIou");
  p.topK = smallInteger(o, "topK", 512);
  p.batchSize = smallInteger(o, "batchSize", 16);
  p.minHits = smallInteger(o, "minHits", 10000);
  p.maxMiss = smallInteger(o, "maxMiss", 10000);
  p.mergeFrequencyHz = number(o, "mergeFrequencyHz");
  p.bandwidthMarginDb = number(o, "bandwidthMarginDb");
  p.timeStrategy =
      enumeration<DetectionTimeStrategy>(o, "timeStrategy", kStrategy);
  p.splitBursts = boolean(o, "splitBursts");
  p.sampleEdgeRatio = number(o, "sampleEdgeRatio");
  p.minOnSeconds = number(o, "minOnSeconds");
  p.minOffSeconds = number(o, "minOffSeconds");
  p.smoothSeconds = number(o, "smoothSeconds");
  p.coarseSplitGapSeconds = number(o, "coarseSplitGapSeconds");
  p.coarseMinBurstSeconds = number(o, "coarseMinBurstSeconds");
  return p;
}
QJsonObject profileJson(const DetectionProfile& p) {
  return {{"id", text(p.id)},
          {"version", static_cast<int>(p.version)},
          {"name", text(p.name)},
          {"status", text(p.status)},
          {"backend", enumText(static_cast<int>(p.backend), kBackend)},
          {"fftSize", static_cast<int>(p.fftSize)},
          {"modelFrames", static_cast<double>(p.modelFrames)},
          {"modelHopFrames", static_cast<double>(p.modelHopFrames)},
          {"hopSeconds", p.hopSeconds},
          {"maximumPsdOnly", p.maximumPsdOnly},
          {"supportsReal", p.supportsReal}};
}
DetectionProfile profileValue(const QJsonValue& value) {
  const auto o = objectValue(value, "profile");
  onlyKeys(o, {"id", "version", "name", "status", "backend", "fftSize",
               "modelFrames", "modelHopFrames", "hopSeconds", "maximumPsdOnly",
               "supportsReal"});
  DetectionProfile p;
  p.id = string(o, "id", 128, true);
  p.version = smallInteger(o, "version", 1000000);
  p.name = string(o, "name", 1024);
  p.status = string(o, "status", 1024);
  p.backend = enumeration<DetectionBackendKind>(o, "backend", kBackend);
  p.fftSize = smallInteger(o, "fftSize", 262144);
  p.modelFrames =
      smallInteger(o, "modelFrames", std::numeric_limits<std::uint32_t>::max());
  p.modelHopFrames = smallInteger(o, "modelHopFrames",
                                  std::numeric_limits<std::uint32_t>::max());
  p.hopSeconds = number(o, "hopSeconds");
  p.maximumPsdOnly = boolean(o, "maximumPsdOnly");
  p.supportsReal = boolean(o, "supportsReal");
  return p;
}
QString runDirectory(const std::string& id) {
  return "detections/" + digest(QByteArray::fromStdString(id));
}
QString manifestPath(const DetectionRequest& request) {
  return runDirectory(request.runId) + "/manifest.json";
}
QString chunkPath(const DetectionRequest& request, const QString& hash) {
  return runDirectory(request.runId) + "/chunk-" + hash + ".json";
}
void validateRequest(const DetectionRequest& request,
                     const QString& storagePath = {}) {
  const auto valid =
      validateDetectionRequest(request, storagePath.toStdString());
  require(valid.ok, text(valid.error));
  QString error;
  SourceIdentity source;
  const bool sourceDecoded = decodeSourceIdentity(
      encodeSourceIdentity(request.sourceIdentity), source, error);
  require(sourceDecoded, error);
  require(!source.quickFingerprint.empty() || !source.contentHash.empty(),
          "Detection source requires an immutable identity fingerprint");
  require(source.contentHash.empty() || hashValid(text(source.contentHash)),
          "Invalid detection source content hash");
  require(!text(source.path).contains(QChar(0)),
          "Detection source path contains a null character");
  const auto* profile = findDetectionProfile(request.profile.id);
  require(
      profile &&
          (request.inputAdaptationVersion != 1 ||
           request.profile.hopSeconds == profile->hopSeconds) &&
          request.profile.maximumPsdOnly == profile->maximumPsdOnly &&
          request.profile.supportsReal == profile->supportsReal,
      "Detection profile preprocessing differs from the registered version");
  const FrequencyRange physicalFrequency{
      source.centerFrequencyHz - source.sampleRateHz / 2,
      source.centerFrequencyHz + source.sampleRateHz / 2};
  const ViewRange sourceRange{{0, source.availableSamples}, physicalFrequency};
  const ViewRange physicalRange{{0, source.physicalSamples}, physicalFrequency};
  auto availableRequest = request.requestedRange;
  availableRequest.time.end =
      std::min(availableRequest.time.end, source.availableSamples);
  if (!emptyRange(request.contextRange)) {
    require(contained(request.contextRange, sourceRange) &&
                (availableRequest.time.begin >= availableRequest.time.end ||
                 contained(availableRequest, request.contextRange)) &&
                request.contextRange.frequency == physicalFrequency,
            "Detection context must contain the available request within the "
            "source prefix");
  }
  require(contained(request.requestedRange, physicalRange),
          "Requested range exceeds the physical source bandwidth");
  require(request.modelSha256.empty() || hashValid(text(request.modelSha256)),
          "Invalid detection model hash");
  require(
      request.sourceAnnotationId.empty() == request.sourceRevisionId.empty(),
      "Detection annotation ROI requires both source and revision identities");
}
void validateRun(const DetectionRun& run, const QString& storagePath = {}) {
  validateRequest(run.request, storagePath);
  const auto valid = validateDetectionRun(run, storagePath.toStdString());
  require(valid.ok, text(valid.error));
  enumText(static_cast<int>(run.lifecycle), kLifecycles);
  enumText(static_cast<int>(run.sourceValidity), kValidity);
  require(run.manifestRelativePath.empty() ||
              text(run.manifestRelativePath) == manifestPath(run.request),
          "Detection manifest path is not contained in its run directory");
  quint64 total = 0;
  for (std::size_t i = 0; i < run.chunks.size(); ++i) {
    const auto& chunk = run.chunks[i];
    require(chunk.sequence == i && hashValid(text(chunk.sha256)) &&
                text(chunk.relativePath) ==
                    chunkPath(run.request, text(chunk.sha256)),
            "Detection chunk path/hash/sequence is invalid");
    total += chunk.resultCount;
  }
  require(total == run.resultCount,
          "Detection result count disagrees with immutable chunks");
  auto availableRequest = run.request.requestedRange;
  availableRequest.time.end = std::min(
      availableRequest.time.end, run.request.sourceIdentity.availableSamples);
  for (const auto& coverage : run.coverage) {
    enumText(static_cast<int>(coverage.kind), kCoverage);
    const auto bounds = coverage.kind == DetectionCoverageKind::Read
                            ? run.request.contextRange
                        : coverage.kind == DetectionCoverageKind::Completed ||
                                coverage.kind == DetectionCoverageKind::Eligible
                            ? availableRequest
                            : run.request.requestedRange;
    require(contained(coverage.range, bounds),
            "Detection coverage exceeds its physical request or available "
            "analysis range");
    require(coverage.reason.size() <= 1024,
            "Detection coverage reason exceeds text budget");
  }
  require(!run.finishedAtMilliseconds ||
              run.finishedAtMilliseconds >= run.startedAtMilliseconds,
          "Detection finish time precedes its start time");
}
template <typename Target, typename Decode>
bool decodeSafely(const QJsonObject& object, Target& target, QString& error,
                  Decode decode) {
  try {
    finiteJson(object);
    require(QJsonDocument(object).toJson(QJsonDocument::Compact).size() <=
                kChunkBytes,
            "Detection manifest exceeds 8 MiB");
    auto candidate = decode();
    target = std::move(candidate);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    return false;
  }
}
bool inside(const QString& path, const QString& root) {
  const auto prefix = QDir::fromNativeSeparators(root) + '/';
#ifdef Q_OS_WIN
  return QDir::fromNativeSeparators(path).startsWith(prefix,
                                                     Qt::CaseInsensitive);
#else
  return QDir::fromNativeSeparators(path).startsWith(prefix);
#endif
}
QDir projectRoot(const QString& directory) {
  const auto canonical = QFileInfo(directory).canonicalFilePath();
  require(!canonical.isEmpty() && QFileInfo(canonical).isDir(),
          "Detection project directory is missing");
  return QDir(canonical);
}
void prepareDirectory(const QDir& root, const QString& relative) {
  QDir current(root);
  for (const auto& component : relative.split('/')) {
    const auto path = current.filePath(component);
    if (!QFileInfo::exists(path))
      require(current.mkdir(component),
              "Cannot create detection store directory");
    const QFileInfo info(path);
    require(info.isDir() && !info.isSymLink() &&
                inside(info.canonicalFilePath(), root.path()),
            "Detection store directory escapes its project");
    current = QDir(info.canonicalFilePath());
  }
}
QByteArray readBounded(const QDir& root, const QString& relative) {
  const QFileInfo info(root.filePath(relative));
  require(info.isFile() && !info.isSymLink() &&
              inside(info.canonicalFilePath(), root.path()),
          "Detection file is missing or escapes its project");
  QFile file(info.canonicalFilePath());
  require(file.open(QIODevice::ReadOnly) && file.size() > 0 &&
              file.size() <= kChunkBytes,
          "Detection file is unreadable or exceeds 8 MiB");
  const auto bytes = file.read(kChunkBytes + 1);
  require(file.error() == QFileDevice::NoError && bytes.size() == file.size(),
          "Detection file could not be read completely");
  return bytes;
}
QJsonObject parseObject(const QByteArray& bytes) {
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  require(error.error == QJsonParseError::NoError && document.isObject(),
          "Malformed detection JSON");
  finiteJson(document.object());
  return document.object();
}
void atomicWrite(const QDir& root, const QString& relative,
                 const QByteArray& bytes) {
  require(!bytes.isEmpty() && bytes.size() <= kChunkBytes,
          "Detection write exceeds 8 MiB");
  const QFileInfo info(root.filePath(relative));
  require(!info.isSymLink() &&
              (!info.exists() || inside(info.canonicalFilePath(), root.path())),
          "Detection write path escapes its project");
  QString lastFailure;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    QSaveFile file(root.filePath(relative));
    file.setDirectWriteFallback(false);
    const auto failureMessage = [&](const char* operation) {
      return QString(
                 "Cannot atomically publish detection data (%1): %2: %3 "
                 "[Qt error=%4, attempt=%5/3]")
          .arg(QString::fromLatin1(operation), relative,
               file.errorString().left(1024))
          .arg(static_cast<int>(file.error()))
          .arg(attempt);
    };
    const bool opened = file.open(QIODevice::WriteOnly);
    require(opened, failureMessage("open"));
    const auto written = file.write(bytes);
    require(written == bytes.size(), failureMessage("write"));
    if (file.commit()) return;
    // A temporary reader can deny Windows replacement. Retry the same bytes
    // through a fresh atomic file; never overwrite the previous manifest.
    lastFailure = failureMessage("commit");
    if (attempt < 3) QThread::msleep(20 * attempt);
  }
  require(false, lastFailure);
}
void validateRecords(const QJsonArray& values, const DetectionRequest& request,
                     bool mappings) {
  require(values.size() <= kChunkRecords,
          "Detection chunk exceeds 4096 records");
  for (const auto& value : values) {
    const auto object = objectValue(value, mappings ? "mapping" : "result");
    finiteJson(object);
    for (const auto& key : {"windowId", "firstFrameIndex", "frameIndex",
                            "validSamples", "paddedSamples"})
      if (object.contains(key)) integer(object[key], key);
    if (mappings) {
      const auto samples = [&](const char* startKey, const char* endKey,
                               bool report) {
        if (!object.contains(startKey) && !object.contains(endKey)) return;
        const auto start = integer(object[startKey], startKey);
        const auto end = integer(object[endKey], endKey);
        require(start < end && end <= request.sourceIdentity.availableSamples,
                "Detection mapping exceeds available source samples");
        if (report)
          require(start >= request.requestedRange.time.begin &&
                      end <= request.requestedRange.time.end,
                  "Detection report mapping exceeds the requested range");
      };
      samples("startSample", "endSample", true);
      samples("contextStartSample", "contextEndSample", false);
      for (const auto& key : {"tensorHash", "inputHash"})
        if (object.contains(key))
          require(object[key].isString() && hashValid(object[key].toString()),
                  QString(key) + " requires a SHA-256 hash");
      if (object.contains("normalizationStdDb"))
        require(number(object, "normalizationStdDb") >= 0,
                "Negative normalization deviation");
      if (object.contains("frequencyStepHz"))
        require(number(object, "frequencyStepHz") > 0,
                "Invalid detection frequency step");
      if (object.contains("epsilonGuardApplied"))
        boolean(object, "epsilonGuardApplied");
      if (object.contains("frames")) {
        const auto frames = arrayValue(object, "frames", kChunkBytes / 128);
        for (const auto& frame : frames) {
          const auto f = objectValue(frame, "frame");
          finiteJson(f);
          for (const auto& key : {"frameIndex", "startSample", "endSample",
                                  "validSamples", "paddedSamples"})
            integer(f[key], key);
          const auto start = integer(f["startSample"], "frame.startSample");
          const auto end = integer(f["endSample"], "frame.endSample");
          require(start < end &&
                      end <= request.sourceIdentity.availableSamples &&
                      f["inputHash"].isString() &&
                      hashValid(f["inputHash"].toString()),
                  "Detection frame mapping/hash exceeds its source support");
        }
      }
    }
  }
}
QString sourceHash(const DetectionRequest& request) {
  return digest(QJsonDocument(encodeSourceIdentity(request.sourceIdentity))
                    .toJson(QJsonDocument::Compact));
}
quint64 verifyHeadEvidence(const QDir& root, const DetectionRequest& request,
                           const QJsonArray& mappings) {
  quint64 bytes = 0;
  for (const auto value : mappings) {
    const auto mapping = value.toObject();
    if (!mapping.contains("headEvidence")) continue;
    const auto reference = mapping["headEvidence"].toObject();
    onlyKeys(reference, {"path", "sha256", "bytes", "shape", "headOrder"});
    const auto hash = string(reference, "sha256", 64, true);
    const auto path = string(reference, "path", 32768, true);
    const auto size = integer(reference["bytes"], "headEvidence.bytes");
    const auto shape = reference["shape"].toArray();
    require(
        hashValid(text(hash)) &&
            text(path) ==
                runDirectory(request.runId) + "/heads-" + text(hash) + ".f32" &&
            shape == QJsonArray{1, 3, 1,
                                static_cast<int>(
                                    detectionModelInputBins(request) / 4)} &&
            reference["headOrder"] == QJsonArray{"hm", "bw", "off"} &&
            size == 3ULL * detectionModelInputBins(request) / 4 * sizeof(float),
        "Stored model head reference has invalid scope/layout");
    const auto raw = readBounded(root, text(path));
    require(
        static_cast<quint64>(raw.size()) == size && digest(raw) == text(hash),
        "Stored raw model heads are missing or changed");
    bytes += size;
  }
  return bytes;
}
void verifyEvidenceBudget(const DetectionRequest& request, quint64 mappingBytes,
                          quint64 rawHeadBytes) {
  if (request.legacyCombinedEvidence) {
    require(rawHeadBytes <= request.evidenceBudgetBytes &&
                mappingBytes <= request.evidenceBudgetBytes - rawHeadBytes,
            QString("Detection mappings exceed evidence budget (legacy combined: JSON %1 + raw heads %2; limit %3)")
                .arg(mappingBytes).arg(rawHeadBytes).arg(request.evidenceBudgetBytes));
    return;
  }
  require(mappingBytes <= request.evidenceBudgetBytes,
          QString("Detection mappings exceed JSON evidence budget (required %1; limit %2)")
              .arg(mappingBytes).arg(request.evidenceBudgetBytes));
  if (request.rawHeadBudgetBytes == 0) {
    require(rawHeadBytes == 0,
            QString("Per-window raw model output is not retained (found %1 bytes)")
                .arg(rawHeadBytes));
    return;
  }
  require(rawHeadBytes <= request.rawHeadBudgetBytes,
          QString("Raw model heads exceed independent disk evidence budget (required %1; limit %2)")
              .arg(rawHeadBytes).arg(request.rawHeadBudgetBytes));
}
QJsonObject verifyChunk(const QDir& root, const DetectionRun& run,
                        const DetectionChunkReference& reference,
                        quint64& totalBytes, quint64& evidenceBytes,
                        quint64& rawHeadBytes) {
  const auto bytes = readBounded(root, text(reference.relativePath));
  require(static_cast<std::uint64_t>(bytes.size()) <=
              (run.request.workingSetBudgetBytes - 65536) / 8,
          "Detection shard parsing exceeds working set budget");
  require(digest(bytes) == text(reference.sha256),
          "Detection chunk content hash mismatch");
  totalBytes += static_cast<quint64>(bytes.size());
  require(totalBytes <= jsonStoreLimit(run.request),
          "Detection result store exceeds its evidence budget");
  const auto object = parseObject(bytes);
  onlyKeys(object, {"schema", "version", "runId", "projectGeneration", "fileId",
                    "sourceSha256", "sequence", "results", "mappings"});
  require(object["schema"] == "signal-studio-detection-chunk" &&
              object["version"] == 1 &&
              object["runId"] == text(run.request.runId) &&
              object["fileId"] == text(run.request.sourceIdentity.fileId) &&
              integer(object["projectGeneration"], "projectGeneration") ==
                  run.request.sourceIdentity.projectGeneration &&
              integer(object["sequence"], "sequence") == reference.sequence &&
              object["sourceSha256"] == sourceHash(run.request),
          "Detection chunk source/run identity mismatch");
  const auto results = arrayValue(object, "results", kChunkRecords);
  const auto mappings = arrayValue(object, "mappings", kChunkRecords);
  require(results.size() == reference.resultCount,
          "Detection chunk result count mismatch");
  validateRecords(results, run.request, false);
  validateRecords(mappings, run.request, true);
  evidenceBytes += static_cast<quint64>(
      QJsonDocument(mappings).toJson(QJsonDocument::Compact).size() - 2);
  rawHeadBytes += verifyHeadEvidence(root, run.request, mappings);
  verifyEvidenceBudget(run.request, evidenceBytes, rawHeadBytes);
  return object;
}
void verifyChunks(const QDir& root, const DetectionRun& run) {
  quint64 bytes = 0, evidence = 0, rawHeads = 0;
  for (const auto& chunk : run.chunks)
    verifyChunk(root, run, chunk, bytes, evidence, rawHeads);
}
}  // namespace

QJsonObject encodeDetectionRequest(const DetectionRequest& r) {
  QJsonObject value{
      {"schemaVersion", static_cast<int>(r.inputAdaptationVersion)},
      {"runId", text(r.runId)},
      {"parentRunId", r.parentRunId ? QJsonValue(text(*r.parentRunId))
                                    : QJsonValue(QJsonValue::Null)},
      {"sourceIdentity", encodeSourceIdentity(r.sourceIdentity)},
      {"profile", profileJson(r.profile)},
      {"parameters", parametersJson(r.parameters)},
      {"requestedRange", rangeJson(r.requestedRange)},
      {"contextRange", emptyRange(r.contextRange)
                           ? QJsonValue(QJsonValue::Null)
                           : QJsonValue(rangeJson(r.contextRange))},
      {"rangeOrigin", text(r.rangeOrigin)},
      {"sourceAnnotationId", text(r.sourceAnnotationId)},
      {"sourceRevisionId", text(r.sourceRevisionId)},
      {"modelManifestPath", text(r.modelManifestPath)},
      {"modelSha256", text(r.modelSha256)},
      {"workingSetBudgetBytes", decimal(r.workingSetBudgetBytes)},
      {"evidenceBudgetBytes", decimal(r.evidenceBudgetBytes)}};
  if (r.inputAdaptationVersion >= 2)
    value["spectrumMode"] = static_cast<int>(r.spectrumMode);
  if (!r.legacyCombinedEvidence) {
    value["retainRawModelOutput"] = r.retainRawModelOutput;
    if (r.rawHeadBudgetBytes)
      value["rawHeadBudgetBytes"] = decimal(r.rawHeadBudgetBytes);
  }
  if (r.gpuDspPreferred) value["gpuDspPreferred"] = true;
  if(r.sampleEnergyMode!=DetectionSampleEnergyMode::LegacyExact ||
      r.sampleScratchBudgetBytes!=kDetectionLegacySampleScratchBytes) {
    value["sampleEnergyMode"]=static_cast<int>(r.sampleEnergyMode);
    value["sampleScratchBudgetBytes"]=decimal(r.sampleScratchBudgetBytes);
  }
  return value;
}
bool decodeDetectionRequest(const QJsonObject& o, DetectionRequest& target,
                            QString& error, const QString& storagePath) {
  return decodeSafely(o, target, error, [&] {
    onlyKeys(o, {"schemaVersion", "runId", "parentRunId", "sourceIdentity",
                 "profile", "parameters", "requestedRange", "contextRange",
                 "rangeOrigin", "sourceAnnotationId", "sourceRevisionId",
                 "modelManifestPath", "modelSha256", "workingSetBudgetBytes",
                 "evidenceBudgetBytes", "spectrumMode", "rawHeadBudgetBytes",
                 "retainRawModelOutput", "sampleEnergyMode",
                 "sampleScratchBudgetBytes", "gpuDspPreferred"});
    require(o["schemaVersion"] == 1 || o["schemaVersion"] == 2,
            "Unsupported detection request version");
    DetectionRequest r;
    r.inputAdaptationVersion = o["schemaVersion"].toInt();
    if (o.contains("spectrumMode"))
      r.spectrumMode = static_cast<DetectionSpectrumMode>(
          smallInteger(o, "spectrumMode", 4));
    r.runId = string(o, "runId", 128, true);
    require(!o["parentRunId"].isUndefined(),
            "Detection parentRunId is missing");
    if (!o["parentRunId"].isNull())
      r.parentRunId = string(o, "parentRunId", 128, true);
    QString sourceError;
    const auto source = objectValue(o["sourceIdentity"], "sourceIdentity");
    onlyKeys(source, {"projectGeneration", "formatRevision", "fileId", "path",
                      "quickFingerprint", "contentHash",
                      "selectedSampleChannel", "sampleFormat", "sampleRateHz",
                      "centerFrequencyHz", "effectiveFrequency",
                      "physicalSamples", "availableSamples", "hashedRange"});
    onlyKeys(objectValue(source["sampleFormat"], "sampleFormat"),
             {"structure", "encoding", "byteOrder", "iqLayout", "channels",
              "selectedChannel", "channelLayout", "headerBytes", "trailerBytes",
              "normalizeIntegerAdc"});
    onlyKeys(objectValue(source["effectiveFrequency"], "effectiveFrequency"),
             {"lowerHz", "upperHz"});
    if (!source["hashedRange"].isNull())
      onlyKeys(objectValue(source["hashedRange"], "hashedRange"),
               {"start", "end"});
    const bool sourceDecoded =
        decodeSourceIdentity(source, r.sourceIdentity, sourceError);
    require(sourceDecoded, sourceError);
    r.profile = profileValue(o["profile"]);
    r.parameters = parametersValue(o["parameters"]);
    r.requestedRange = rangeValue(o["requestedRange"]);
    require(!o["contextRange"].isUndefined(),
            "Detection contextRange is missing");
    if (!o["contextRange"].isNull())
      r.contextRange = rangeValue(o["contextRange"]);
    r.rangeOrigin = string(o, "rangeOrigin", 128);
    r.sourceAnnotationId = string(o, "sourceAnnotationId", 128);
    r.sourceRevisionId = string(o, "sourceRevisionId", 128);
    r.modelManifestPath = string(o, "modelManifestPath", 32768);
    r.modelSha256 = string(o, "modelSha256", 64);
    r.workingSetBudgetBytes =
        integer(o["workingSetBudgetBytes"], "workingSetBudgetBytes");
    r.evidenceBudgetBytes =
        integer(o["evidenceBudgetBytes"], "evidenceBudgetBytes");
    if (o.contains("retainRawModelOutput")) {
      require(o["retainRawModelOutput"].isBool(),
              "retainRawModelOutput must be a boolean");
      r.retainRawModelOutput = o["retainRawModelOutput"].toBool();
      r.legacyCombinedEvidence = false;
      r.rawHeadBudgetBytes = o.contains("rawHeadBudgetBytes")
          ? integer(o["rawHeadBudgetBytes"], "rawHeadBudgetBytes") : 0;
    } else if (o.contains("rawHeadBudgetBytes")) {
      r.rawHeadBudgetBytes =
          integer(o["rawHeadBudgetBytes"], "rawHeadBudgetBytes");
      r.legacyCombinedEvidence = false;
      r.retainRawModelOutput = r.rawHeadBudgetBytes > 0;
    } else {
      r.rawHeadBudgetBytes = 0;
      r.retainRawModelOutput = false;
      r.legacyCombinedEvidence = true;
    }
    r.sampleEnergyMode=o.contains("sampleEnergyMode")
        ? static_cast<DetectionSampleEnergyMode>(smallInteger(o,"sampleEnergyMode",2))
        : DetectionSampleEnergyMode::LegacyExact;
    require(!o.contains("gpuDspPreferred") || o["gpuDspPreferred"].isBool(),
            "Invalid GPU DSP preference");
    r.gpuDspPreferred = o["gpuDspPreferred"].toBool(false);
    r.sampleScratchBudgetBytes=o.contains("sampleScratchBudgetBytes")
        ? integer(o["sampleScratchBudgetBytes"],"sampleScratchBudgetBytes")
        : kDetectionLegacySampleScratchBytes;
    validateRequest(r, storagePath);
    return r;
  });
}
QJsonObject detectionTensorRtInferenceTimings(const QJsonObject& performance) {
  const auto named = performance.value(QStringLiteral("tensorRtInference"));
  const auto events = named.isObject() ? named.toObject()
                                       : performance.value(QStringLiteral("gpu")).toObject();
  QJsonObject timings;
  for (const auto* key : {"h2dMs", "gpuStreamMs", "d2hMs", "hostEnqueueMs",
                          "inputAdaptationMs", "physicalEnqueueCalls",
                          "cudaEventIntervals"})
    if (events.contains(key)) timings.insert(key, events.value(key));
  return timings;
}
QJsonObject encodeDetectionRun(const DetectionRun& run) {
  QJsonArray coverage, chunks;
  for (const auto& part : run.coverage)
    coverage.append(
        QJsonObject{{"kind", enumText(static_cast<int>(part.kind), kCoverage)},
                    {"range", rangeJson(part.range)},
                    {"reason", text(part.reason)}});
  for (const auto& chunk : run.chunks)
    chunks.append(
        QJsonObject{{"sequence", decimal(chunk.sequence)},
                    {"relativePath", text(chunk.relativePath)},
                    {"sha256", text(chunk.sha256)},
                    {"resultCount", static_cast<int>(chunk.resultCount)}});
  QJsonObject encoded{{"schema", "signal-studio-detection-run"},
          {"version", 2},
          {"request", encodeDetectionRequest(run.request)},
          {"lifecycle", enumText(static_cast<int>(run.lifecycle), kLifecycles)},
          {"sourceValidity",
           enumText(static_cast<int>(run.sourceValidity), kValidity)},
          {"coverage", coverage},
          {"chunks", chunks},
          {"readFrames", decimal(run.readFrames)},
          {"eligibleWindows", decimal(run.eligibleWindows)},
          {"completedWindows", decimal(run.completedWindows)},
          {"resultCount", decimal(run.resultCount)},
          {"accountedChunkBytes", decimal(run.accountedChunkBytes)},
          {"accountedEvidenceBytes", decimal(run.accountedEvidenceBytes)},
          {"accountedRawHeadBytes", decimal(run.accountedRawHeadBytes)},
          {"computationPhase", text(run.computationPhase)},
          {"refinedCandidates", decimal(run.refinedCandidates)},
          {"totalRefinementCandidates", decimal(run.totalRefinementCandidates)},
          {"startedAtMilliseconds", decimal(run.startedAtMilliseconds)},
          {"finishedAtMilliseconds", decimal(run.finishedAtMilliseconds)},
          {"error", text(run.error)},
          {"device", text(run.device)},
          {"runtime", text(run.runtime)},
          {"manifestRelativePath", manifestPath(run.request)},
          {"recoverable", run.recoverable}};
  if (!run.performanceJson.empty()) {
    const auto document=QJsonDocument::fromJson(QByteArray::fromStdString(run.performanceJson));
    require(run.performanceJson.size()<=65536 && document.isObject() &&
        document.object()["schema"]=="signal-studio-detection-profile-v1",
        "Invalid bounded detection performance diagnostics");
    encoded["performance"]=document.object();
  }
  return encoded;
}
bool decodeDetectionRun(const QJsonObject& o, DetectionRun& target,
                        QString& error, const QString& storagePath) {
  return decodeSafely(o, target, error, [&] {
    onlyKeys(o, {"schema",
                 "version",
                 "request",
                 "lifecycle",
                 "sourceValidity",
                 "coverage",
                 "chunks",
                 "readFrames",
                 "eligibleWindows",
                 "completedWindows",
                 "resultCount",
                 "accountedChunkBytes",
                 "accountedEvidenceBytes",
                 "accountedRawHeadBytes",
                 "startedAtMilliseconds",
                 "finishedAtMilliseconds",
                 "error",
                 "device",
                 "runtime",
                 "manifestRelativePath",
                 "recoverable",
                 "computationPhase",
                 "refinedCandidates",
                 "totalRefinementCandidates", "performance"});
    require(o["schema"] == "signal-studio-detection-run" &&
                (o["version"] == 1 || o["version"] == 2),
            "Unsupported detection run manifest version");
    DetectionRun run;
    QString requestError;
    const bool requestDecoded = decodeDetectionRequest(
        objectValue(o["request"], "request"), run.request, requestError,
        storagePath);
    require(requestDecoded, requestError);
    run.lifecycle =
        enumeration<DetectionLifecycle>(o, "lifecycle", kLifecycles);
    run.sourceValidity =
        enumeration<DetectionSourceValidity>(o, "sourceValidity", kValidity);
    for (const auto& value : arrayValue(o, "coverage", 100000)) {
      const auto part = objectValue(value, "coverage");
      onlyKeys(part, {"kind", "range", "reason"});
      run.coverage.push_back(
          {enumeration<DetectionCoverageKind>(part, "kind", kCoverage),
           rangeValue(part["range"]), string(part, "reason", 1024)});
    }
    for (const auto& value : arrayValue(o, "chunks", 100000)) {
      const auto chunk = objectValue(value, "chunk");
      onlyKeys(chunk, {"sequence", "relativePath", "sha256", "resultCount"});
      run.chunks.push_back({integer(chunk["sequence"], "sequence"),
                            string(chunk, "relativePath", 1024, true),
                            string(chunk, "sha256", 64, true),
                            smallInteger(chunk, "resultCount", 4096)});
    }
    run.readFrames = integer(o["readFrames"], "readFrames");
    run.eligibleWindows = integer(o["eligibleWindows"], "eligibleWindows");
    run.completedWindows = integer(o["completedWindows"], "completedWindows");
    run.resultCount = integer(o["resultCount"], "resultCount");
    if (o["version"] == 2) {
      run.computationPhase = string(o, "computationPhase", 32, true);
      require(QStringList{"none", "preflight", "reading", "inference", "decoding",
                          "storing-heads", "tracking", "checkpoint", "frequency", "refine",
                          "publishing", "completed"}
                  .contains(text(run.computationPhase)),
              "Unknown detection computation phase");
      run.accountedChunkBytes = o.contains("accountedChunkBytes")
          ? integer(o["accountedChunkBytes"], "accountedChunkBytes") : 0;
      run.accountedEvidenceBytes = o.contains("accountedEvidenceBytes")
          ? integer(o["accountedEvidenceBytes"], "accountedEvidenceBytes") : 0;
      run.accountedRawHeadBytes = o.contains("accountedRawHeadBytes")
          ? integer(o["accountedRawHeadBytes"], "accountedRawHeadBytes") : 0;
      run.refinedCandidates =
          integer(o["refinedCandidates"], "refinedCandidates");
      run.totalRefinementCandidates =
          integer(o["totalRefinementCandidates"], "totalRefinementCandidates");
      require(run.refinedCandidates <= run.totalRefinementCandidates &&
                  run.totalRefinementCandidates <= annotationSegmentBudget,
              "Detection refinement counters exceed their frozen total");
    } else {
      require(!o.contains("computationPhase") &&
                  !o.contains("refinedCandidates") &&
                  !o.contains("totalRefinementCandidates") && !o.contains("performance") &&
                  !o.contains("accountedChunkBytes") &&
                  !o.contains("accountedEvidenceBytes") &&
                  !o.contains("accountedRawHeadBytes"),
              "Version 1 cannot contain version 2 progress fields");
    }
    run.startedAtMilliseconds =
        integer(o["startedAtMilliseconds"], "startedAtMilliseconds");
    run.finishedAtMilliseconds =
        integer(o["finishedAtMilliseconds"], "finishedAtMilliseconds");
    run.error = string(o, "error", 4096);
    run.device = string(o, "device", 1024);
    run.runtime = string(o, "runtime", 1024);
    run.manifestRelativePath = string(o, "manifestRelativePath", 1024, true);
    run.recoverable = boolean(o, "recoverable");
    if(o.contains("performance")) {
      const auto performance=objectValue(o["performance"],"performance");
      const auto bytes=QJsonDocument(performance).toJson(QJsonDocument::Compact);
      require(bytes.size()<=65536 && performance["schema"]=="signal-studio-detection-profile-v1",
          "Invalid bounded detection performance diagnostics");
      if (performance.contains("tensorRtInference"))
        require(performance.value("tensorRtInference").isObject(),
                "tensorRtInference timings must be an object");
      // Legacy alias for the same TensorRT CUDA-event object. The stored
      // document keeps whichever key the file used.
      if (performance.contains("gpu"))
        require(performance.value("gpu").isObject(),
                "Legacy gpu timings alias must be an object");
      run.performanceJson=bytes.toStdString();
    }
    validateRun(run, storagePath);
    return run;
  });
}
bool writeDetectionRun(const QString& directory, const DetectionRun& run,
                       QString& error) {
  try {
    validateRun(run);
    const auto root = projectRoot(directory);
    prepareDirectory(root, runDirectory(run.request.runId));
    if (QFileInfo::exists(root.filePath(manifestPath(run.request)))) {
      DetectionRun previous;
      const bool previousDecoded = decodeDetectionRun(
          parseObject(readBounded(root, manifestPath(run.request))), previous,
          error);
      require(previousDecoded, error);
      require(encodeDetectionRequest(previous.request) ==
                  encodeDetectionRequest(run.request),
              "A detection run cannot replace its immutable request snapshot");
      require(detectionTransitionAllowed(previous.lifecycle, run.lifecycle),
              "Detection manifest lifecycle transition is invalid");
      require(previous.chunks.size() <= run.chunks.size(),
              "Published detection chunks cannot be dropped");
      for (std::size_t i = 0; i < previous.chunks.size(); ++i) {
        const auto& before = previous.chunks[i];
        const auto& after = run.chunks[i];
        require(before.sequence == after.sequence &&
                    before.relativePath == after.relativePath &&
                    before.sha256 == after.sha256 &&
                    before.resultCount == after.resultCount,
                "Published detection chunks cannot be replaced or reordered");
      }
    }
    // Append already checked the new shard. Historical hashes, including raw
    // heads, are audited again when the run reaches a terminal lifecycle.
    if (detectionIsTerminal(run.lifecycle)) verifyChunks(root, run);
    DetectionRun decoded;
    QString decodeError;
    const auto object = encodeDetectionRun(run);
    const bool runDecoded = decodeDetectionRun(object, decoded, decodeError);
    require(runDecoded, decodeError);
    atomicWrite(root, manifestPath(run.request),
                QJsonDocument(object).toJson(QJsonDocument::Compact));
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    return false;
  }
}
bool appendDetectionChunk(const QString& directory, DetectionRun& run,
                          const QJsonArray& results, const QJsonArray& mappings,
                          QString& error) {
  try {
    validateRun(run);
    require(!detectionIsTerminal(run.lifecycle),
            "Cannot append results to a terminal detection run");
    require(!results.isEmpty() || !mappings.isEmpty(),
            "Empty detection chunks are not published");
    require(run.resultCount + static_cast<quint64>(results.size()) <= 100000,
            "Detection run exceeds 100000 results");
    validateRecords(results, run.request, false);
    validateRecords(mappings, run.request, true);
    require(run.chunks.empty() || run.accountedChunkBytes > 0,
            "Detection append is missing its cumulative chunk byte ledger");
    const auto root = projectRoot(directory);
    prepareDirectory(root, runDirectory(run.request.runId));
    quint64 totalBytes = run.accountedChunkBytes;
    quint64 evidenceBytes = run.accountedEvidenceBytes;
    quint64 rawHeadBytes = run.accountedRawHeadBytes;
    const auto object =
        QJsonObject{{"schema", "signal-studio-detection-chunk"},
                    {"version", 1},
                    {"runId", text(run.request.runId)},
                    {"projectGeneration",
                     decimal(run.request.sourceIdentity.projectGeneration)},
                    {"fileId", text(run.request.sourceIdentity.fileId)},
                    {"sourceSha256", sourceHash(run.request)},
                    {"sequence", decimal(run.chunks.size())},
                    {"results", results},
                    {"mappings", mappings}};
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    require(bytes.size() <= kChunkBytes &&
                totalBytes + static_cast<quint64>(bytes.size()) <=
                    jsonStoreLimit(run.request),
            "Detection chunk/store exceeds its byte budget");
    evidenceBytes += static_cast<quint64>(
        QJsonDocument(mappings).toJson(QJsonDocument::Compact).size() - 2);
    rawHeadBytes += verifyHeadEvidence(root, run.request, mappings);
    verifyEvidenceBudget(run.request, evidenceBytes, rawHeadBytes);
    const auto hash = digest(bytes);
    const auto relative = chunkPath(run.request, hash);
    if (QFileInfo::exists(root.filePath(relative)))
      require(readBounded(root, relative) == bytes,
              "Existing immutable detection chunk is damaged");
    else
      atomicWrite(root, relative, bytes);
    auto candidate = run;
    candidate.accountedChunkBytes =
        totalBytes + static_cast<quint64>(bytes.size());
    candidate.accountedEvidenceBytes = evidenceBytes;
    candidate.accountedRawHeadBytes = rawHeadBytes;
    candidate.chunks.push_back(
        {static_cast<std::uint64_t>(candidate.chunks.size()),
         relative.toStdString(), hash.toStdString(),
         static_cast<std::uint32_t>(results.size())});
    candidate.resultCount += static_cast<std::uint64_t>(results.size());
    candidate.manifestRelativePath =
        manifestPath(candidate.request).toStdString();
    const bool published = writeDetectionRun(directory, candidate, error);
    require(published, error);
    run = std::move(candidate);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    return false;
  }
}

bool storeDetectionHeadEvidence(const QString& directory,
                                const DetectionRequest& request,
                                const DetectionWorkerPacket& packet,
                                QJsonArray& target, QString& error) {
  try {
    const auto shape = packet.envelope["binary"].toObject()["shape"].toArray();
    const auto count = target.size();
    const auto width = detectionModelInputBins(request) / 4;
    const auto payload = packet.envelope["payload"].toObject();
    require(count >= 1 && count <= 2 &&
                shape == QJsonArray{static_cast<int>(count), 3, 1,
                                    static_cast<int>(width)} &&
                payload["headOrder"] == QJsonArray{"hm", "bw", "off"},
            "Production worker returned an invalid raw head layout");
    std::vector<float> heads;
    const bool decoded = decodeFloat32Payload(packet, heads, error);
    require(decoded, error);
    const auto root = projectRoot(directory);
    prepareDirectory(root, runDirectory(request.runId));
    auto mappings = target;
    const auto memberBytes =
        static_cast<qsizetype>(3ULL * width * sizeof(float));
    require(packet.binary.size() == count * memberBytes,
            "Production model head byte count mismatch");
    for (qsizetype index = 0; index < count; ++index) {
      const auto bytes = packet.binary.mid(index * memberBytes, memberBytes);
      const auto hash = digest(bytes);
      const auto path = runDirectory(request.runId) + "/heads-" + hash + ".f32";
      if (QFileInfo::exists(root.filePath(path)))
        require(readBounded(root, path) == bytes,
                "Existing immutable model heads are damaged");
      else
        atomicWrite(root, path, bytes);
      auto mapped = mappings[index].toObject();
      mapped["headEvidence"] =
          QJsonObject{{"path", path},
                      {"sha256", hash},
                      {"bytes", QString::number(memberBytes)},
                      {"shape", QJsonArray{1, 3, 1, static_cast<int>(width)}},
                      {"headOrder", QJsonArray{"hm", "bw", "off"}}};
      mapped["modelEvidence"] = payload["modelEvidence"];
      const auto summaries = payload["decodeSummary"].toArray();
      if (index < summaries.size()) mapped["decodeSummary"] = summaries[index];
      mappings[index] = mapped;
    }
    target = std::move(mappings);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    return false;
  }
}
bool loadDetectionRun(const QString& directory, const QString& id,
                      DetectionRun& target, QString& error, bool verify) {
  try {
    require(
        !id.isEmpty() && id.toUtf8().size() <= 128 && !id.contains(QChar(0)),
        "Invalid detection run ID");
    const auto root = projectRoot(directory);
    DetectionRequest key;
    key.runId = id.toStdString();
    DetectionRun candidate;
    const bool runDecoded = decodeDetectionRun(
        parseObject(readBounded(root, manifestPath(key))), candidate, error);
    require(runDecoded, error);
    require(text(candidate.request.runId) == id,
            "Detection manifest belongs to another run");
    if (verify) verifyChunks(root, candidate);
    if (!detectionIsTerminal(candidate.lifecycle)) {
      candidate.lifecycle = DetectionLifecycle::Interrupted;
      candidate.recoverable = true;
      candidate.error =
          "Interrupted execution; stored results can be reviewed. Restart "
          "creates a new run.";
      RuntimeLog::write(
          QStringLiteral("detection"),
          QStringList{"run=" + QString::fromStdString(candidate.request.runId),
                      "phase=" + QString::fromStdString(candidate.computationPhase),
                      "windows=" + QString::number(candidate.completedWindows) + "/" +
                          QString::number(candidate.eligibleWindows),
                      "batch=0", "event=interrupted",
                      "error=" + QString::fromStdString(candidate.error)}
              .join(' '));
    }
    target = std::move(candidate);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    return false;
  }
}
std::vector<DetectionRun> discoverDetectionRuns(const QString& directory,
                                                QString& error) {
  try {
    const auto root = projectRoot(directory);
    const auto path = root.filePath("detections");
    if (!QFileInfo::exists(path)) {
      error.clear();
      return {};
    }
    require(inside(QFileInfo(path).canonicalFilePath(), root.path()),
            "Detection discovery path escapes project");
    const QDir runs(path);
    const auto entries =
        runs.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    require(entries.size() <= kMaximumRuns,
            "Detection discovery exceeds 10000 runs");
    std::vector<DetectionRun> results;
    for (const auto& entry : entries) {
      require(hashValid(entry), "Invalid detection run directory key");
      const auto relative = "detections/" + entry + "/manifest.json";
      if (!QFileInfo::exists(root.filePath(relative)))
        continue;  // Unpublished crash leftovers.
      DetectionRun candidate;
      const bool runDecoded = decodeDetectionRun(
          parseObject(readBounded(root, relative)), candidate, error);
      require(runDecoded, error);
      require(manifestPath(candidate.request) == relative,
              "Detection manifest directory/run identity mismatch");
      const bool loaded = loadDetectionRun(
          directory, text(candidate.request.runId), candidate, error);
      require(loaded, error);
      results.push_back(std::move(candidate));
    }
    error.clear();
    return results;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    return {};
  }
}
bool readDetectionChunks(const QString& directory, const DetectionRun& run,
                         QJsonArray& target, QString& error) {
  QJsonArray candidate;
  std::uint64_t residentBytes = 65536;
  // Preflight before parsing any shard, while previous JSON nodes are resident.
  for (const auto& shard : run.chunks) {
    const auto size =
        QFileInfo(QDir(directory).filePath(text(shard.relativePath))).size();
    if (size <= 0) continue;  // The visitor diagnoses a missing/invalid path.
    const auto reserve = 8 * static_cast<std::uint64_t>(size) + 65536;
    if (reserve > run.request.workingSetBudgetBytes ||
        residentBytes > run.request.workingSetBudgetBytes - reserve) {
      error = "Reading accumulated detection shards exceeds working set budget";
      return false;
    }
    residentBytes += reserve;
  }
  residentBytes = 65536;
  const bool visited = visitDetectionChunks(
      directory, run,
      [&](const QJsonObject& object, QString& reason) {
        const auto bytes = static_cast<std::uint64_t>(
            QJsonDocument(object).toJson(QJsonDocument::Compact).size());
        // JSON nodes, encoded temporaries and the next parsed shard coexist.
        const auto reserve = 8 * bytes + 65536;
        if (reserve > run.request.workingSetBudgetBytes ||
            residentBytes > run.request.workingSetBudgetBytes - reserve) {
          reason =
              "Reading accumulated detection shards exceeds working set budget";
          return false;
        }
        residentBytes += reserve;
        candidate.append(object);
        return true;
      },
      error);
  if (!visited) return false;
  target = std::move(candidate);
  return true;
}
bool visitDetectionChunks(
    const QString& directory, const DetectionRun& run,
    const std::function<bool(const QJsonObject&, QString&)>& visitor,
    QString& error) {
  try {
    validateRun(run);
    const auto root = projectRoot(directory);
    quint64 totalBytes = 0, evidenceBytes = 0, rawHeadBytes = 0;
    for (const auto& reference : run.chunks) {
      const auto size =
          QFileInfo(root.filePath(text(reference.relativePath))).size();
      require(size > 0 && static_cast<std::uint64_t>(size) <=
                              (run.request.workingSetBudgetBytes - 65536) / 8,
              "Detection shard parsing exceeds working set budget");
      const auto object =
          verifyChunk(root, run, reference, totalBytes, evidenceBytes, rawHeadBytes);
      if (!visitor(object, error)) return false;
    }
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    return false;
  }
}

std::vector<DetectionRun> discoverDetectionRuns(const QString& directory,
                                               const Project& project,
                                               QString& error) {
  std::vector<DetectionRun> result;
  QSet<QString> ids;
  for (const auto& file : project.files) {
    if (!file.currentDetectionSlots) {
      if (file.detectionRuns.empty() && std::none_of(file.sourceSets.begin(), file.sourceSets.end(),
          [](const auto& set) { return set.sourceKind == AnnotationSource::Algorithm; })) continue;
      error = "旧工程必须先完成检测 slot 裁决，再恢复任务";
      return {};
    }
    for (const auto& slot : *file.currentDetectionSlots) {
      for (const auto& id : {slot.committedRunId, slot.pendingRunId}) {
        if (id.empty() || ids.contains(text(id))) continue;
        if (std::find(slot.retiredRunIds.begin(), slot.retiredRunIds.end(), id) !=
            slot.retiredRunIds.end()) {
          error = "权威检测索引同时包含退役运行"; return {};
        }
        DetectionRun run;
        if (!loadDetectionRun(directory, text(id), run, error)) return {};
        if (run.request.sourceIdentity.fileId != file.metadata.id ||
            run.request.sourceIdentity.selectedSampleChannel != slot.sampleChannel) {
          error = "恢复运行与权威 slot 作用域不同"; return {};
        }
        ids.insert(text(id));
        result.push_back(std::move(run));
      }
    }
  }
  error.clear();
  return result;
}

bool detectionRunAssetPaths(const QString& directory, const DetectionRun& run,
                            QStringList& target, QString& error) {
  QStringList paths{manifestPath(run.request)};
  QSet<QString> seen;
  for (const auto& chunk : run.chunks) {
    paths.push_back(text(chunk.relativePath));
    seen.insert(text(chunk.relativePath));
  }
  const bool valid = visitDetectionChunks(directory, run,
      [&](const QJsonObject& chunk, QString&) {
        for (const auto& value : chunk["mappings"].toArray()) {
          const auto reference = value.toObject()["headEvidence"].toObject();
          const auto path = reference["path"].toString();
          if (!path.isEmpty() && !seen.contains(path)) {
            paths.push_back(path);
            seen.insert(path);
          }
        }
        return true;
      }, error);
  if (!valid) return false;
  target = std::move(paths);
  return true;
}

namespace {
constexpr auto ownerFile = ".detection-owner.json";
constexpr auto sharedFile = ".detection-shared.json";
bool linkInPath(QString path) {
  path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  for (;;) {
    const QFileInfo info(path);
    if (info.isSymLink()) return true;
#ifdef Q_OS_WIN
    const auto attributes = GetFileAttributesW(
        reinterpret_cast<const wchar_t*>(path.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    const auto parent = info.absolutePath();
    if (parent == path) break;
    path = parent;
  }
  return false;
}
QString canonicalProject(const QString& path) {
  require(!linkInPath(path), "工程或资产路径含链接/reparse point；禁止 GC");
  const QFileInfo info(path);
  const auto parent = info.absoluteDir().canonicalPath();
  require(!parent.isEmpty(), "工程父目录不存在");
  return QDir(parent).filePath(info.fileName());
}
QJsonObject readOwner(const QString& root) {
  const auto path = QDir(root).filePath(ownerFile);
  require(!linkInPath(path), "资产所有权文件不得为链接");
  QFile input(path);
  require(input.open(QIODevice::ReadOnly) && input.size() <= 16384,
          "资产所有权文件不可用；延后 GC");
  const auto object = parseObject(input.readAll());
  require(input.error() == QFileDevice::NoError &&
              object["schema"] == "signal-studio-private-detection-root-v1" &&
              object["shared"].isBool(), "资产所有权文件无效");
  return object;
}
void writeOwner(const QString& root, const QJsonObject& object) {
  QSaveFile output(QDir(root).filePath(ownerFile));
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() &&
              output.commit(), "资产所有权记录无法原子保存");
}
bool sharedRoot(const QString& root) {
  const auto path = QDir(root).filePath(sharedFile);
  if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) return false;
  require(!linkInPath(path), "资产共享声明不得为链接");
  QFile input(path);
  require(input.open(QIODevice::ReadOnly) && input.size() <= 16384,
          "资产共享声明无法读取；禁止 GC");
  const auto object = parseObject(input.readAll());
  require(input.error() == QFileDevice::NoError &&
              object["schema"] == "signal-studio-shared-detection-root-v1" &&
              object["shared"].isBool() && object["shared"].toBool(),
          "资产共享声明无效；禁止 GC");
  return true;
}
void declareSharedRoot(const QString& root) {
  if (sharedRoot(root)) return;
  // Append one immutable safety sidecar. Never rewrite owner/container/state
  // bytes when another project begins to reference this root.
  const auto bytes = QJsonDocument(QJsonObject{
      {"schema", "signal-studio-shared-detection-root-v1"}, {"shared", true}})
      .toJson(QJsonDocument::Compact);
  QSaveFile output(QDir(root).filePath(sharedFile));
  output.setDirectWriteFallback(false);
  require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() &&
              output.commit(), "资产共享声明无法原子发布");
}
}  // namespace

bool recordDetectionAssetOwnership(const QString& path, const QString& id,
                                   const QString& assets, bool newlyCreated,
                                   bool reference, QString& error,
                                   bool rootLockHeld) {
  try {
    // An unproven pre-existing root stays unowned. Reference makes even an
    // unowned root permanently shared before the referencing project commits.
    const auto marker = QDir(assets).filePath(ownerFile);
    if (!reference && (!newlyCreated || QFileInfo::exists(marker))) {
      error.clear(); return true;
    }
    require(!linkInPath(assets), "资产根含链接/reparse point；无法声明所有权");
    std::unique_ptr<QLockFile> lock;
    if (!rootLockHeld) {
      lock = std::make_unique<QLockFile>(QDir(assets).filePath(".detection-gc.lock"));
      lock->setStaleLockTime(0);
      require(lock->tryLock(0), "资产根正在清理；稍后重试保存");
    }
    if (reference) {
      declareSharedRoot(assets);
      error.clear(); return true;
    }
    QJsonObject owner;
    if (QFileInfo::exists(marker)) owner = readOwner(assets);
    else owner = {{"schema", "signal-studio-private-detection-root-v1"},
                  {"projectId", id}, {"ownerPath", canonicalProject(path)},
                  {"shared", false}};
    {
      const auto expected = canonicalProject(path) + ".assets/" + id;
      require(QFileInfo(assets).canonicalFilePath().compare(
                  QFileInfo(expected).canonicalFilePath(), Qt::CaseInsensitive) == 0,
              "私有资产根不在本工程 canonical 目录");
    }
    writeOwner(assets, owner);
    error.clear(); return true;
  } catch (const std::exception& e) {
    error = QString::fromUtf8(e.what()); return false;
  }
}

bool collectRetiredDetectionAssets(const QString& path, DetectionGcReport& report,
                                   QString& error, std::uint32_t maximumRuns,
                                   std::uint32_t maximumFiles) {
  report = {};
  try {
    require(maximumRuns > 0 && maximumRuns <= 64 && maximumFiles > 0 &&
                maximumFiles <= 20000, "GC 预算无效");
    const auto ownerPath = canonicalProject(path);
    QFile input(ownerPath);
    require(input.open(QIODevice::ReadOnly) && input.size() <= 32 * 1024 * 1024,
            "GC 需要已提交的真实 .ssproj");
    const auto container = parseObject(input.readAll());
    require(container["magic"] == "SignalStudioProject" && container["schemaVersion"] == 1,
            "GC 工程身份无效");
    if (container["assetMode"] != "copy") { error.clear(); return true; }
    const auto id = container["projectId"].toString();
    const auto ref = container["assetDirectory"].toString();
    const auto root = QDir::isAbsolutePath(ref) ? ref : QFileInfo(ownerPath).absoluteDir().filePath(ref);
    const auto expected = ownerPath + ".assets/" + id;
    require(!id.isEmpty() && !ref.isEmpty() && !linkInPath(root) &&
                QFileInfo(root).canonicalFilePath() == QFileInfo(expected).canonicalFilePath() &&
                !QFileInfo(root).canonicalFilePath().isEmpty(),
            "GC 仅允许本工程自有 canonical 资产根");
    if (!QFileInfo::exists(QDir(root).filePath(ownerFile))) { error.clear(); return true; }
    QLockFile lock(QDir(root).filePath(".detection-gc.lock"));
    lock.setStaleLockTime(0);
    require(lock.tryLock(0), "GC 资产根正被引用/清理；稍后重试");
    if (sharedRoot(root)) { error.clear(); return true; }
    const auto owner = readOwner(root);
    if (owner["shared"].toBool() || owner["projectId"] != id ||
        owner["ownerPath"].toString() != ownerPath) { error.clear(); return true; }
    Project project;
    require(ProjectFileCodec::load(ownerPath, project, error), error);
    // Detect a concurrent project/root switch before touching any old file.
    require(project.projectId == id.toStdString() &&
                QFileInfo(text(project.assetDirectory)).canonicalFilePath() == QFileInfo(root).canonicalFilePath(),
            "GC 期间工程资产根已改变");
    report.privateRoot = true;
    QSet<QString> referenced, retired, declaredStorePaths;
    for (const auto& value : container["assets"].toArray())
      declaredStorePaths.insert(value.toObject()["path"].toString());
    for (const auto& file : project.files) {
      if (!file.currentDetectionSlots) {
        require(file.detectionRuns.empty() && std::none_of(file.sourceSets.begin(), file.sourceSets.end(),
            [](const auto& set) { return set.sourceKind == AnnotationSource::Algorithm; }),
            "旧工程尚未提交 slot 迁移；延后 GC");
        continue;
      }
      for (const auto& run : file.detectionRuns) referenced.insert(text(run.request.runId));
      for (const auto& set : file.sourceSets)
        if (!set.runId.empty()) referenced.insert(text(set.runId));
      for (const auto& slot : *file.currentDetectionSlots) {
        if (!slot.committedRunId.empty()) referenced.insert(text(slot.committedRunId));
        if (!slot.pendingRunId.empty()) referenced.insert(text(slot.pendingRunId));
        for (const auto& run : slot.retiredRunIds) retired.insert(text(run));
      }
    }
    std::uint32_t visited = 0;
    for (const auto& runId : retired) {
      if (referenced.contains(runId)) { ++report.deferredRuns; continue; }
      DetectionRequest key; key.runId = runId.toStdString();
      const auto relative = manifestPath(key);
      // A read-only legacy migration is not a durable retirement. The raw
      // committed container may still anchor that run even if the decoded
      // candidate has already selected a newer slot. Wait for an actual save.
      if (declaredStorePaths.contains(relative)) { ++report.deferredRuns; continue; }
      const auto manifest = QDir(root).filePath(relative);
      if (!QFileInfo::exists(manifest)) continue;
      if (visited++ >= maximumRuns) { ++report.deferredRuns; continue; }
      require(!linkInPath(manifest), "GC run 路径含链接/reparse point");
      DetectionRun run;
      const auto store = projectRoot(root);
      require(decodeDetectionRun(parseObject(readBounded(store, relative)), run, error), error);
      if (run.request.runId != runId.toStdString() ||
          (run.lifecycle != DetectionLifecycle::Completed &&
           run.lifecycle != DetectionLifecycle::Failed &&
           run.lifecycle != DetectionLifecycle::Cancelled) ||
          run.chunks.size() >= maximumFiles) { ++report.deferredRuns; continue; }
      QStringList paths;
      if (!detectionRunAssetPaths(root, run, paths, error)) {
        ++report.deferredRuns; continue;  // Already interrupted GC: keep garbage safely.
      }
      if (paths.size() > maximumFiles - report.removedFiles) { ++report.deferredRuns; continue; }
      for (const auto& asset : paths) {
        require(asset.startsWith(runDirectory(run.request.runId) + '/') &&
                    !linkInPath(QDir(root).filePath(asset)), "GC 资产路径越界或含链接");
      }
      // The manifest is last. Only enumerated, validated store files are
      // removed; IQ/TXT/model/scratch files are never deletion candidates.
      for (qsizetype i = paths.size() - 1; i >= 0; --i) {
        require(QFile::remove(QDir(root).filePath(paths[i])), "GC 文件删除失败；新工程仍已提交");
        ++report.removedFiles;
      }
      QDir(root).rmdir(runDirectory(run.request.runId));
      ++report.removedRuns;
    }
    error.clear(); return true;
  } catch (const std::exception& e) {
    error = QString::fromUtf8(e.what()); return false;
  }
}
}  // namespace signalstudio
