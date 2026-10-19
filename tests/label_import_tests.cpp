#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <limits>

#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/annotation_store.h"
#include "infrastructure/label_import.h"
#include "infrastructure/wav_metadata.h"

using namespace signalstudio;
namespace {
QDir root() {
#ifdef SS_SOURCE_DIR
  return QDir(QString::fromUtf8(SS_SOURCE_DIR));
#else
  return QDir(QDir::currentPath());
#endif
}
QByteArray fixture(const QString& name) {
  QFile f(root().filePath("tests/fixtures/wideband/" + name));
  if (!f.open(QIODevice::ReadOnly)) qFatal("fixture unavailable");
  return f.readAll();
}
void write(const QString& path, const QByteArray& raw) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(raw) != raw.size())
    qFatal("fixture write failed");
}
void sizeFile(const QString& path, quint64 size) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      !file.resize(static_cast<qint64>(size)))
    qFatal("sparse sample fixture failed");
}
FileMetadata target(const QString& path, double fs = 60000000,
                    quint64 count = 600000, double fc = 0) {
  FileMetadata m;
  m.id = "target-file";
  m.path = path.toUtf8().toStdString();
  m.name = QFileInfo(path).fileName().toUtf8().toStdString();
  m.demo = false;
  m.sampleRateHz = fs;
  m.centerFrequencyHz = fc;
  m.sampleCount = count;
  m.effectiveBandwidthHz = fs;
  return m;
}
FileMetadata torchTarget(QTemporaryDir& dir) {
  const auto base = dir.filePath("0000000");
  write(base + ".sigmf-meta", fixture("torchsig-0000000.sigmf-meta"));
  write(base + ".json", fixture("torchsig-0000000.json"));
  sizeFile(base + ".sigmf-data", 3200000);
  auto m = target(base + ".sigmf-data", 20000000, 400000, 2500000000);
  m.sampleFormat.componentEncoding = ComponentEncoding::Float32;
  m.sampleFormat.normalizeIntegerAdc = false;
  return m;
}
QJsonObject readJson(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) qFatal("JSON fixture unavailable");
  return QJsonDocument::fromJson(file.readAll()).object();
}
QString baseOf(const FileMetadata& m) {
  return QString::fromStdString(m.path).chopped(11);
}
}  // namespace
class LabelImportTests : public QObject {
  Q_OBJECT
 private slots:
  void generatorFixtureAndPhysicalBinding() {
    QTemporaryDir dir;
    const auto path = dir.filePath("generator.txt"),
               data = dir.filePath("samples.raw");
    const auto raw = fixture("generator-60m-10ms.txt");
    write(path, raw);
    sizeFile(data, 2400000);
    const auto m = target(data);
    const auto imported = importGeneratorLabels(path, m);
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    QCOMPARE(imported.annotations.size(), std::size_t{12});
    std::size_t count = 0;
    for (const auto& a : imported.annotations) {
      count += a.segments.size();
      QCOMPARE(a.reviewStatus, ReviewStatus::Pending);
      QCOMPARE(a.sourceKind, AnnotationSource::ImportedLabel);
      QVERIFY(a.originEvidence && !a.originEvidence->scoreMean &&
              !a.originEvidence->scoreMax);
      QVERIFY(validateAnnotation(a, annotationBounds(m)).ok);
    }
    QCOMPARE(count, std::size_t{17});
    const auto& a = imported.annotations.front();
    QCOMPARE(a.segments.size(), std::size_t{2});
    QCOMPARE(a.segments[0].range.time.begin, quint64{180000});
    QCOMPARE(a.segments[0].range.time.end, quint64{300000});
    QCOMPARE(a.segments[1].range.time.begin, quint64{420000});
    QCOMPARE(a.segments[1].range.time.end, quint64{600000});
    QCOMPARE(a.segments[0].uncertaintySamples, quint64{301});
    QCOMPARE(a.segments[0].range.frequency.lowerHz,
             801339.0 - 30000000 - 229890.0 / 2);
    QCOMPARE(imported.sourceSet.originalText, raw.toStdString());
    const auto second = importGeneratorLabels(path, m, 123);
    QCOMPARE(second.sourceSet.id, imported.sourceSet.id);
    QCOMPARE(second.annotations.front().id, a.id);
    auto changed = m;
    changed.centerFrequencyHz = 100000000;
    const auto shifted = importGeneratorLabels(path, changed);
    QVERIFY(shifted.ok());
    QVERIFY(shifted.sourceSet.id != imported.sourceSet.id);
    QCOMPARE(shifted.annotations[0].segments[0].range.frequency.lowerHz,
             a.segments[0].range.frequency.lowerHz + 100000000);
    changed = m;
    changed.sampleCount--;
    QVERIFY(!importGeneratorLabels(path, changed).ok());
    changed = m;
    changed.sampleRateHz /= 2;
    QVERIFY(!importGeneratorLabels(path, changed).ok());
    auto partial = m;
    partial.availability.status = LoadStatus::Partial;
    partial.availability.availableSamples = 200000;
    const auto tail = importGeneratorLabels(path, partial);
    QVERIFY(tail.ok());
    QCOMPARE(tail.annotations[0].segments[1].range.time.end, quint64{600000});
    QVERIFY(std::any_of(
        tail.diagnostics.begin(), tail.diagnostics.end(),
        [](const auto& d) { return d.code == "outside-available-prefix"; }));
  }
  void generatorRoundingAndRejectWholeBatch() {
    QTemporaryDir dir;
    const auto path = dir.filePath("labels.txt"),
               data = dir.filePath("samples.raw");
    sizeFile(data, 1234567 * 4);
    const auto m = target(data, 1234567, 1234567);
    const QByteArray good =
        "1: sig_len:1234567 time_end:1.00000 time_start:0.00000\r\n 1): SNR:2 "
        "CNR:3 SPS:4 FD:5 LEN:1234567 BW:100000 FC:617283 sys:BPSK mod:BPSK "
        "TIME:0.00001,0.00002 0.50000,1.00000 \r\n";
    write(path, good);
    const auto result = importGeneratorLabels(path, m);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.annotations[0].segments[0].range.time.begin, quint64{12});
    QCOMPARE(result.annotations[0].segments[0].range.time.end, quint64{25});
    QCOMPARE(result.annotations[0].segments[0].uncertaintySamples, quint64{7});
    QCOMPARE(result.sourceSet.originalText, good.toStdString());
    const auto fields =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(
                result.annotations[0].originEvidence->originalFieldsJson))
            .object();
    QCOMPARE(fields["textQuantizationSeconds"].toDouble(), 1e-5);
    QCOMPARE(fields["fields"].toObject()["CNR"].toString(), QString("3"));
    QCOMPARE(fields["fields"].toObject()["SNR"].toString(), QString("2"));
    for (const auto& replacement :
         std::vector<std::pair<QByteArray, QByteArray>>{
             {"mod:BPSK", "mod:"},
             {"FC:617283", "FC:9999999"},
             {"BW:100000", "BW:9999999"},
             {"SNR:2", "SNR:nan"},
             {"LEN:1234567", "LEN:1234566"},
             {"TIME:0.00001,0.00002", "TIME:0.00001 0.00002"},
             {"0.50000,1.00000", "0.50000,1.00001"},
             {"SPS:4", "SPS:4 SPS:5"},
             {"sig_len:1234567", "sig_len:18446744073709551616"}}) {
      auto bad = good;
      bad.replace(replacement.first, replacement.second);
      write(path, bad);
      const auto rejected = importGeneratorLabels(path, m);
      QVERIFY2(!rejected.ok(), replacement.first.constData());
      QVERIFY(rejected.annotations.empty());
    }
    write(path, good + good.mid(good.indexOf(" 1):")));
    const auto duplicate = importGeneratorLabels(path, m);
    QVERIFY(!duplicate.ok() && duplicate.annotations.empty());
  }
  void torchSigFixtureAndOrderIndependentMatching() {
    QTemporaryDir dir;
    auto m = torchTarget(dir);
    const auto base = baseOf(m);
    auto result = importTorchSigLabels(base + ".sigmf-meta", m);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.annotations.size(), std::size_t{4});
    const auto& segment = result.annotations[0].segments[0];
    QCOMPARE(segment.range.time.begin, quint64{51107});
    QCOMPARE(segment.range.time.end, quint64{343277});
    QCOMPARE(segment.range.frequency.lowerHz, 2502021605.9327126);
    QCOMPARE(segment.range.frequency.upperHz, 2502282009.1227803);
    QCOMPARE(*result.annotations[0].classLabel, std::string("64qam"));
    QCOMPARE(segment.timeQuality, TimeQuality::SampleLabel);
    QCOMPARE(segment.uncertaintySamples, quint64{0});
    auto json = readJson(base + ".json");
    const auto events = json["events"].toArray();
    QJsonArray reverse;
    for (qsizetype i = events.size(); i > 0; --i) reverse.append(events[i - 1]);
    json["events"] = reverse;
    json["unknown_optional"] = QJsonObject{
        {"nullable", QJsonValue(QJsonValue::Null)}, {"detail", "kept"}};
    write(base + ".json", QJsonDocument(json).toJson());
    result = importTorchSigLabels(base + ".json", m);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.annotations.size(), std::size_t{4});
    const auto raw =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(
                result.annotations[0].originEvidence->originalFieldsJson))
            .object();
    QCOMPARE(raw["rfbenchEvent"].toObject()["event_id"].toInteger(), qint64{0});
    QCOMPARE(raw["rfbenchHeaderReference"].toObject()["sourceHash"].toString(),
             QString::fromStdString(result.sourceSet.supplementalHash));
    QVERIFY(
        QJsonDocument::fromJson(QByteArray::fromStdString(
                                    result.sourceSet.supplementalOriginalText))
            .object()
            .contains("unknown_optional"));
    QCOMPARE(importLabelSource(base + ".sigmf-data", m).annotations.size(),
             std::size_t{4});
    m.availability.status = LoadStatus::Partial;
    m.availability.availableSamples = 100000;
    const auto partial = importTorchSigLabels(base + ".sigmf-meta", m);
    QVERIFY(partial.ok());
    QCOMPARE(partial.annotations[0].segments[0].range.time.end,
             quint64{343277});
    QFile::remove(base + ".json");
    const auto metaOnly = importTorchSigLabels(base + ".sigmf-meta", m);
    QVERIFY(metaOnly.ok());
    QVERIFY(metaOnly.sourceSet.supplementalHash.empty());
  }
  void torchSigConflictsAndUnsupported() {
    QTemporaryDir dir;
    const auto m = torchTarget(dir);
    const auto base = baseOf(m);
    const auto validMeta = fixture("torchsig-0000000.sigmf-meta"),
               validJson = fixture("torchsig-0000000.json");
    const auto rejectMeta = [&](const QJsonObject& meta) {
      write(base + ".sigmf-meta", QJsonDocument(meta).toJson());
      const auto result = importTorchSigLabels(base + ".sigmf-meta", m);
      QVERIFY2(!result.ok(), qPrintable(result.error));
      QVERIFY(result.annotations.empty());
      write(base + ".sigmf-meta", validMeta);
    };
    for (const auto& key : QStringList{"core:label", "core:freq_upper_edge",
                                       "core:sample_count"}) {
      auto meta = QJsonDocument::fromJson(validMeta).object();
      auto records = meta["annotations"].toArray();
      auto item = records[0].toObject();
      item.remove(key);
      records[0] = item;
      meta["annotations"] = records;
      rejectMeta(meta);
    }
    {
      auto meta = QJsonDocument::fromJson(validMeta).object();
      auto records = meta["annotations"].toArray();
      auto item = records[0].toObject();
      item["core:label"] = 42;
      records[0] = item;
      meta["annotations"] = records;
      rejectMeta(meta);
      item["core:label"] = "bpsk";
      item["core:sample_start"] = 1e30;
      records[0] = item;
      meta["annotations"] = records;
      rejectMeta(meta);
    }
    for (const auto& key : QStringList{"core:offset", "core:num_channels",
                                       "core:datatype", "core:dataset"}) {
      auto meta = QJsonDocument::fromJson(validMeta).object();
      auto global = meta["global"].toObject();
      global[key] = key == "core:datatype" ? QJsonValue("ci16_le")
                    : key == "core:dataset"
                        ? QJsonValue("../outside.sigmf-data")
                        : QJsonValue(2);
      meta["global"] = global;
      rejectMeta(meta);
    }
    {
      auto meta = QJsonDocument::fromJson(validMeta).object();
      auto captures = meta["captures"].toArray();
      captures.append(captures[0]);
      meta["captures"] = captures;
      rejectMeta(meta);
    }
    {
      auto json = QJsonDocument::fromJson(validJson).object();
      auto events = json["events"].toArray();
      auto item = events[0].toObject();
      item["freq_upper_hz"] = item["freq_upper_hz"].toDouble() + .001;
      events[0] = item;
      json["events"] = events;
      write(base + ".json", QJsonDocument(json).toJson());
      const auto result = importTorchSigLabels(base + ".sigmf-meta", m);
      QVERIFY(!result.ok() && result.annotations.empty());
      const auto explicitMeta =
          importTorchSigLabels(base + ".sigmf-meta", m, 0, true);
      QVERIFY(explicitMeta.ok());
      QVERIFY(!explicitMeta.sourceSet.supplementalHash.empty());
      write(base + ".json", validJson);
    }
    {
      auto json = QJsonDocument::fromJson(validJson).object();
      auto events = json["events"].toArray();
      events[1] = events[0];
      json["events"] = events;
      write(base + ".json", QJsonDocument(json).toJson());
      QVERIFY(!importTorchSigLabels(base + ".sigmf-meta", m).ok());
      write(base + ".json", validJson);
    }
    sizeFile(QString::fromStdString(m.path), 3199999);
    QVERIFY(!importTorchSigLabels(base + ".sigmf-meta", m).ok());
    QVERIFY(!importLabelSource(base + ".yolo", m).ok());
  }
  void codecFullRoundtripAndExports() {
    QTemporaryDir dir;
    auto m = torchTarget(dir);
    const auto imported = importTorchSigLabels(baseOf(m) + ".sigmf-meta", m);
    QVERIFY(imported.ok());
    auto a = imported.annotations[0];
    const auto before = a.segments[0];
    auto after = before;
    after.range.time.begin++;
    after.timeQuality = TimeQuality::Manual;
    after.frequencyQuality = FrequencyQuality::Manual;
    a.segments[0] = after;
    const auto old = a.revisionId;
    a.revisionId = a.id + ":r1";
    a.reviewRevisionId = a.revisionId;
    AnnotationRevision revision{
        a.revisionId, old, "geometry", "user", "2026-10-13T00:00:00Z", "note"};
    revision.segmentChanges.push_back({before.id, before, after});
    revision.nameBefore = a.name;
    revision.nameAfter = "edited";
    revision.noteBefore = a.note;
    revision.noteAfter = "note";
    a.revisions.push_back(revision);
    a.reviewHistory.push_back({ReviewStatus::Accepted, old, "user",
                               "2026-10-13T00:00:00Z",
                               "accepted old revision"});
    a.name = "edited";
    a.note = "note\nsecond line";
    a.derivedFrom = {"candidate-1"};
    refreshAnnotationProjection(a);
    Annotation decoded;
    QString error;
    const auto encoded = encodeAnnotation(a);
    QVERIFY2(decodeAnnotation(encoded, m, decoded, error), qPrintable(error));
    QCOMPARE(encodeAnnotation(decoded), encoded);
    QCOMPARE(*decoded.originEvidence, *a.originEvidence);
    QVERIFY(decoded.originEvidence->originalSegments[0].range.time.begin !=
            decoded.segments[0].range.time.begin);
    AnnotationSourceSet set;
    QVERIFY2(
        decodeSourceSet(encodeSourceSet(imported.sourceSet), m, set, error),
        qPrintable(error));
    QCOMPARE(set, imported.sourceSet);
    SourceIdentity identity;
    QVERIFY(decodeSourceIdentity(encodeSourceIdentity(set.sourceIdentity),
                                 identity, error));
    QCOMPARE(identity, set.sourceIdentity);
    AnnotationChannelSnapshot snapshot{a.id,
                                       a.segments[0].id,
                                       old,
                                       "original",
                                       before.range,
                                       AnnotationSource::ImportedLabel,
                                       ReviewStatus::Accepted,
                                       set.sourceIdentity},
        snapshotDecoded;
    QVERIFY2(decodeAnnotationChannelSnapshot(
                 encodeAnnotationChannelSnapshot(snapshot), m, snapshotDecoded,
                 error),
             qPrintable(error));
    QCOMPARE(snapshot, snapshotDecoded);
    FileState file;
    file.metadata = m;
    file.sourceSets = {set};
    file.marks = {a};
    const auto json = exportAnnotationsJson(file),
               csv = exportAnnotationsCsv(file);
    QVERIFY(QJsonDocument::fromJson(json).isObject());
    QVERIFY(json.endsWith('\n'));
    QVERIFY(!json.contains('\r'));
    QVERIFY(csv.contains("51108,343277"));
    QVERIFY(csv.endsWith('\n'));
    QCOMPARE(csv.count('\n'), 2);
    auto big = identity;
    big.projectGeneration = std::numeric_limits<quint64>::max();
    big.physicalSamples = (quint64{1} << 53) + 73;
    big.availableSamples = big.physicalSamples;
    big.hashedRange = TimeRange{(quint64{1} << 53) + 1, big.physicalSamples};
    QVERIFY(decodeSourceIdentity(encodeSourceIdentity(big), identity, error));
    QCOMPARE(identity, big);
    auto bad = encoded;
    auto segments = bad["segments"].toArray();
    auto segment = segments[0].toObject();
    auto range = segment["range"].toObject();
    for (const auto value : QJsonArray{51108, "051108", "18446744073709551616",
                                       "51108\n", "51108\r\n"}) {
      range["start"] = value;
      segment["range"] = range;
      segments[0] = segment;
      bad["segments"] = segments;
      QVERIFY(!decodeAnnotation(bad, m, decoded, error));
      QCOMPARE(encodeAnnotation(decoded), encoded);
    }
    bad = encoded;
    bad["reviewRevisionId"] = "missing";
    QVERIFY(!decodeAnnotation(bad, m, decoded, error));
    bad = encoded;
    bad["sourceKind"] = "unknown";
    QVERIFY(!decodeAnnotation(bad, m, decoded, error));
    bad = encoded;
    auto evidence = bad["originEvidence"].toObject();
    evidence["scoreMean"] = .9;
    bad["originEvidence"] = evidence;
    QVERIFY(!decodeAnnotation(bad, m, decoded, error));
    bad = encoded;
    auto revisions = bad["revisions"].toArray();
    auto item = revisions[0].toObject();
    item["parentRevisionId"] = item["id"];
    revisions[0] = item;
    bad["revisions"] = revisions;
    QVERIFY(!decodeAnnotation(bad, m, decoded, error));
  }
  void largeSharedAndRecordEvidenceRemainLossless() {
    QTemporaryDir directory;
    const auto metadata = torchTarget(directory);
    const auto base = baseOf(metadata);
    const auto originalMeta =
        QJsonDocument::fromJson(fixture("torchsig-0000000.sigmf-meta"))
            .object();
    const auto originalSupplemental =
        QJsonDocument::fromJson(fixture("torchsig-0000000.json")).object();
    const QString payload(15 * 1024 * 1024, QLatin1Char('L'));
    for (const auto& kind : QStringList{"global", "sigmfRecord",
                                        "rfbenchHeader", "rfbenchEvent"}) {
      auto primary = originalMeta;
      auto supplemental = originalSupplemental;
      if (kind == "global") {
        auto global = primary["global"].toObject();
        global["unknown_optional"] = payload;
        primary["global"] = global;
      } else if (kind == "sigmfRecord") {
        auto records = primary["annotations"].toArray();
        auto record = records[0].toObject();
        record["unknown_optional"] = payload;
        records[0] = record;
        primary["annotations"] = records;
      } else if (kind == "rfbenchHeader") {
        supplemental["unknown_optional"] = payload;
      } else {
        auto events = supplemental["events"].toArray();
        auto event = events[0].toObject();
        event["unknown_optional"] = payload;
        events[0] = event;
        QJsonArray reverse;
        for (qsizetype i = events.size(); i > 0; --i)
          reverse.append(events[i - 1]);
        supplemental["events"] = reverse;
      }
      const auto primaryBytes = QJsonDocument(primary).toJson();
      const auto supplementalBytes = QJsonDocument(supplemental).toJson();
      write(base + ".sigmf-meta", primaryBytes);
      write(base + ".json", supplementalBytes);
      const auto imported =
          importTorchSigLabels(base + ".sigmf-meta", metadata);
      QVERIFY2(imported.ok(), qPrintable(imported.error));
      QCOMPARE(imported.annotations.size(), std::size_t{4});
      for (const auto& annotation : imported.annotations) {
        QVERIFY(annotation.originEvidence->originalFieldsJson.size() < 4096);
        const auto origin =
            QJsonDocument::fromJson(
                QByteArray::fromStdString(
                    annotation.originEvidence->originalFieldsJson))
                .object();
        QVERIFY(!origin.contains("global") && !origin.contains("capture") &&
                !origin.contains("rfbenchHeader"));
        QCOMPARE(origin["globalReference"].toObject()["sourceHash"].toString(),
                 QString::fromStdString(imported.sourceSet.sourceHash));
      }
      const auto origin =
          QJsonDocument::fromJson(
              QByteArray::fromStdString(
                  imported.annotations[0].originEvidence->originalFieldsJson))
              .object();
      if (kind == "sigmfRecord") {
        const auto reference = origin["sigmfAnnotation"]
                                   .toObject()["snapshotReference"]
                                   .toObject();
        QCOMPARE(reference["jsonPointer"].toString(),
                 QString("/annotations/0"));
        const auto record = primary["annotations"].toArray()[0].toObject();
        QCOMPARE(reference["canonicalObjectSha256"].toString(),
                 QString::fromLatin1(
                     QCryptographicHash::hash(
                         QJsonDocument(record).toJson(QJsonDocument::Compact),
                         QCryptographicHash::Sha256)
                         .toHex()));
      } else if (kind == "rfbenchEvent") {
        const auto reference =
            origin["rfbenchEvent"].toObject()["snapshotReference"].toObject();
        QCOMPARE(reference["jsonPointer"].toString(), QString("/events/3"));
        QCOMPARE(reference["sourceHash"].toString(),
                 QString::fromStdString(imported.sourceSet.supplementalHash));
      }
      FileState file;
      file.metadata = metadata;
      file.sourceSets = {imported.sourceSet};
      file.marks = imported.annotations;
      const auto exported =
          QJsonDocument::fromJson(exportAnnotationsJson(file)).object();
      const auto source = exported["sourceSets"].toArray()[0].toObject();
      QVERIFY(source["originalText"].toString().toUtf8() == primaryBytes);
      QVERIFY(source["supplementalOriginalText"].toString().toUtf8() ==
              supplementalBytes);
      QString error;
      QJsonObject index;
      const auto project = directory.filePath(kind + ".json");
      QVERIFY2(writeAnnotationStore(project, file, index, error),
               qPrintable(error));
      FileState restored;
      restored.metadata = metadata;
      QVERIFY2(readAnnotationStore(project, index, restored, error),
               qPrintable(error));
      QCOMPARE(restored.marks.size(), std::size_t{4});
      QVERIFY(restored.sourceSets[0].originalText ==
              primaryBytes.toStdString());
      QVERIFY(restored.sourceSets[0].supplementalOriginalText ==
              supplementalBytes.toStdString());
    }
  }

  void normalizedBatchBudgetRejectsAtomically() {
    QTemporaryDir directory;
    const auto metadata = torchTarget(directory);
    const auto base = baseOf(metadata);
    auto primary =
        QJsonDocument::fromJson(fixture("torchsig-0000000.sigmf-meta"))
            .object();
    const QJsonObject record{
        {"core:sample_start", 0},
        {"core:sample_count", 1},
        {"core:freq_lower_edge", 2499999900.0},
        {"core:freq_upper_edge", 2500000100.0},
        {"core:label", "bpsk"},
        {"unknown_optional", QString(4096, QLatin1Char('R'))}};
    QJsonArray records;
    for (int i = 0; i < 4000; ++i) records.append(record);
    primary["annotations"] = records;
    const auto raw = QJsonDocument(primary).toJson(QJsonDocument::Compact);
    QVERIFY(raw.size() < 32 * 1024 * 1024);
    write(base + ".sigmf-meta", raw);
    QVERIFY(QFile::remove(base + ".json"));
    const auto rejected = importTorchSigLabels(base + ".sigmf-meta", metadata);
    QVERIFY(!rejected.ok());
    QVERIFY(rejected.annotations.empty());
    QVERIFY(rejected.error.contains("累计预算"));
  }

  void largeGeneratorRecordUsesSourceLineReference() {
    QTemporaryDir directory;
    const auto data = directory.filePath("source.raw");
    const auto path = directory.filePath("large.txt");
    sizeFile(data, 4000000);
    const auto metadata = target(data, 1000000, 1000000);
    const auto raw =
        QByteArray(
            "1: time_start:0.00000 time_end:1.00000 sig_len:1000000\n"
            "1): mod:BPSK sys:BPSK FC:500000 BW:100000 FD:1 LEN:1000000 SPS:1 "
            "CNR:1 SNR:1 TIME:0.10000,0.20000 OPT:") +
        QByteArray(128 * 1024, 'T') + "\n";
    write(path, raw);
    const auto imported = importGeneratorLabels(path, metadata);
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    const auto origin =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(
                imported.annotations[0].originEvidence->originalFieldsJson))
            .object();
    QVERIFY(origin.contains("recordReference"));
    QCOMPARE(origin["recordReference"].toObject()["textLineNumber"].toString(),
             QString("2"));
    QVERIFY(!origin.contains("rawLine") && !origin.contains("fields"));
    QVERIFY(imported.sourceSet.originalText == raw.toStdString());
  }

  void actualTwoSourceAssets() {
    QFile assets(root().filePath(
        "docs/implementation/wideband-detection/M0/assets.json"));
    QVERIFY(assets.open(QIODevice::ReadOnly));
    const auto all = QJsonDocument::fromJson(assets.readAll()).object();
    const auto pair = all["datasets"]
                          .toObject()["matlabGenerator"]
                          .toObject()["goldenPair"]
                          .toObject();
    const auto wavPath = pair["wav"].toObject()["path"].toString(),
               txtPath = pair["txtLabel"].toObject()["path"].toString();
    if (!QFileInfo::exists(wavPath))
      QSKIP("External generator M0 golden unavailable");
    WavDescriptor wav;
    QString error;
    QVERIFY(readWavDescriptor(wavPath, wav, error));
    auto m = target(wavPath, wav.sampleRateHz, wav.sampleCount, 0);
    m.sampleFormat = wav.format;
    const auto generator = importGeneratorLabels(txtPath, m);
    QVERIFY2(generator.ok(), qPrintable(generator.error));
    QCOMPARE(generator.annotations.size(), std::size_t{10});
    std::size_t segments = 0;
    for (const auto& a : generator.annotations) segments += a.segments.size();
    qInfo() << "Actual generator annotations" << generator.annotations.size()
            << "segments" << segments << "source SHA256"
            << QString::fromStdString(generator.sourceSet.sourceHash);
    const QString data =
        "W:/wbSig53_rfbench20m/train/baseline/0000000.sigmf-data";
    if (!QFileInfo::exists(data))
      QSKIP("External TorchSig M0 golden unavailable");
    m = target(data, 20000000, 400000, 2500000000);
    m.sampleFormat.componentEncoding = ComponentEncoding::Float32;
    m.sampleFormat.normalizeIntegerAdc = false;
    const auto torch = importTorchSigLabels(data, m);
    QVERIFY2(torch.ok(), qPrintable(torch.error));
    QCOMPARE(torch.annotations.size(), std::size_t{4});
    qInfo() << "Actual TorchSig annotations" << torch.annotations.size()
            << "source SHA256"
            << QString::fromStdString(torch.sourceSet.sourceHash);
  }
};
QTEST_GUILESS_MAIN(LabelImportTests)
#include "label_import_tests.moc"
