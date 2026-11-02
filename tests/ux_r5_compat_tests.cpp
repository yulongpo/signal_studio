#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include "application/session.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/annotation_migration.h"
#include "infrastructure/project_file.h"

using namespace signalstudio;
namespace {
Project fixture(AnnotationSource kind, int objects = 1) {
  Session session;
  session.addDemoFile("compat-iq", 1e6, 100e6, .1);
  auto& file = *session.activeFile();
  AnnotationSourceSet set;
  set.id = "legacy-source";
  set.fileId = file.metadata.id;
  set.sourceKind = kind;
  set.formatId = kind == AnnotationSource::ImportedLabel ? "generator-txt-v1" : "detection-v1";
  set.originalText = "原始完整证据";
  set.supplementalOriginalText = "supplemental retained evidence";
  set.supplementalHash = "recorded-product-source-hash";
  set.sourceIdentity = annotationSourceIdentity(file.metadata);
  std::vector<Annotation> annotations;
  for (int i = 0; i < objects; ++i) {
    Annotation a;
    a.id = "legacy-annotation-" + std::to_string(i);
    a.name = "legacy name";
    a.fileId = file.metadata.id;
    a.sourceKind = kind;
    a.sourceSetId = set.id;
    a.originRecordId = "record-" + std::to_string(i);
    a.classLabel = "QPSK";
    a.segments.push_back({"original-segment-" + std::to_string(i), {{101, 501}, {99.95e6, 100.05e6}}, TimeQuality::SampleLabel, FrequencyQuality::NominalLabel});
    auto origin = std::make_shared<OriginEvidence>();
    origin->formatId = set.formatId;
    origin->originalSegments = a.segments;
    origin->originalFieldsJson = "{\"original\":true}";
    a.originEvidence = origin;
    annotations.push_back(std::move(a));
  }
  if (!session.annotations().publishSourceSet(file, set, std::move(annotations))) return {};
  return session.project();
}
}
class UxR5CompatTests : public QObject {
  Q_OBJECT
 private slots:
  void conversionRoundtripAndSourceRemoval() {
    auto project = fixture(AnnotationSource::ImportedLabel, 100);
    QVERIFY(!project.files.empty());
    auto& file = project.files.front();
    std::vector<std::string> ids;
    for (const auto& a : file.marks) ids.push_back(a.id);
    const auto sourceBytes = QJsonDocument(encodeAnnotation(file.marks.front())).toJson();
    AnnotationService service;
    std::vector<std::string> created;
    QVERIFY(service.convertToManual(file, ids, {}, created));
    QCOMPARE(created.size(), std::size_t(100));
    QCOMPARE(QJsonDocument(encodeAnnotation(*findAnnotation(file, ids.front()))).toJson(), sourceBytes);
    const auto snapshot = findAnnotation(file, created.front())->conversionSnapshot;
    QVERIFY(snapshot);
    std::erase_if(file.marks, [](const auto& a) { return a.sourceKind == AnnotationSource::ImportedLabel; });
    std::erase_if(file.sourceSets, [](const auto& s) { return s.sourceKind == AnnotationSource::ImportedLabel; });
    QTemporaryDir directory;
    QString error;
    const auto path = directory.filePath("converted.ssproj");
    QVERIFY2(ProjectFileCodec::save(path, project, error), qPrintable(error));
    Project reopened;
    QVERIFY2(ProjectFileCodec::load(path, reopened, error), qPrintable(error));
    QCOMPARE(reopened.files.front().marks.size(), std::size_t(100));
    const auto* copy = findAnnotation(reopened.files.front(), created.front());
    QVERIFY(copy && copy->conversionSnapshot);
    QVERIFY(*copy->conversionSnapshot == *snapshot);
    QVERIFY(service.rename(reopened.files.front(), copy->id, "independent", "edited after source removed"));
    QVERIFY2(ProjectFileCodec::save(path, reopened, error), qPrintable(error));
    QVERIFY2(ProjectFileCodec::load(path, project, error), qPrintable(error));
    QCOMPARE(findAnnotation(project.files.front(), created.front())->name, std::string("independent"));
  }
  void historicalEdits_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("imported") << 0;
    QTest::newRow("algorithm") << 2;
  }
  void historicalEdits() {
    QFETCH(int, kind);
    auto project = fixture(static_cast<AnnotationSource>(kind));
    QVERIFY(!project.files.empty());
    auto& file = project.files.front();
    auto& a = file.marks.front();
    const auto sourceId = a.id;
    const auto originalEvidence = *a.originEvidence;
    AnnotationRevision edit;
    edit.id = "legacy-user-edit";
    edit.parentRevisionId = a.revisionId;
    edit.action = "geometry";
    edit.actor = "user";
    edit.nameBefore = a.name;
    edit.nameAfter = "旧人工修订";
    a.name = *edit.nameAfter;
    a.segments.front().range.time = {151, 651};
    a.revisions.push_back(edit);
    a.revisionId = edit.id;
    a.reviewRevisionId = edit.id;
    refreshAnnotationProjection(a);
    QString error;
    QVERIFY2(migrateRevisedSourceAnnotations(project, error), qPrintable(error));
    const auto& migrated = project.files.front();
    QCOMPARE(migrated.marks.size(), std::size_t(2));
    const auto* original = findAnnotation(migrated, sourceId);
    QCOMPARE(original->segments, originalEvidence.originalSegments);
    QVERIFY(*original->originEvidence == originalEvidence);
    const Annotation* manual = nullptr;
    for (const auto& value : migrated.marks) if (value.sourceKind == AnnotationSource::Manual) manual = &value;
    QVERIFY(manual && manual->conversionSnapshot);
    QCOMPARE(manual->name, std::string("旧人工修订"));
    QCOMPARE(manual->segments.front().range.time, (TimeRange{151,651}));
    QVERIFY(QString::fromStdString(manual->originEvidence->originalFieldsJson).contains("legacy-user-edit"));
    QVERIFY2(migrateRevisedSourceAnnotations(project, error), qPrintable(error));
    QCOMPARE(project.files.front().marks.size(), std::size_t(2));
  }
  void malformedSnapshotRejectsAtomically() {
    auto project = fixture(AnnotationSource::ImportedLabel);
    auto& file = project.files.front();
    AnnotationService service;
    std::vector<std::string> created;
    QVERIFY(service.convertToManual(file, {file.marks.front().id}, {}, created));
    const auto* manual = findAnnotation(file, created.front());
    auto json = encodeAnnotation(*manual);
    auto snapshot = json["conversionSnapshot"].toObject();
    snapshot["sourceKind"] = "manual";
    json["conversionSnapshot"] = snapshot;
    auto target = *manual;
    const auto before = encodeAnnotation(target);
    QString error;
    QVERIFY(!decodeAnnotation(json, file.metadata, target, error));
    QCOMPARE(encodeAnnotation(target), before);
  }
  void algorithmPlotHiddenOmittedUnlessTrue() {
    auto project = fixture(AnnotationSource::Algorithm);
    QVERIFY(!project.files.empty());
    auto& file = project.files.front();
    auto* annotation = &file.marks.front();
    QVERIFY(!annotation->plotHidden);
    auto encoded = encodeAnnotation(*annotation);
    QVERIFY(!encoded.contains("plotHidden"));
    annotation->plotHidden = true;
    encoded = encodeAnnotation(*annotation);
    QCOMPARE(encoded.value("plotHidden").toBool(), true);
    Annotation decoded;
    QString error;
    QVERIFY2(decodeAnnotation(encoded, file.metadata, decoded, error),
             qPrintable(error));
    QVERIFY(decoded.plotHidden);
    encoded.remove("plotHidden");
    QVERIFY2(decodeAnnotation(encoded, file.metadata, decoded, error),
             qPrintable(error));
    QVERIFY(!decoded.plotHidden);
    encoded.insert("plotHidden", false);
    QVERIFY2(decodeAnnotation(encoded, file.metadata, decoded, error),
             qPrintable(error));
    QVERIFY(!decoded.plotHidden);
    QVERIFY(!encodeAnnotation(decoded).contains("plotHidden"));
    const auto preserved = encodeAnnotation(decoded);
    encoded.insert("plotHidden", QString("yes"));
    QVERIFY(!decodeAnnotation(encoded, file.metadata, decoded, error));
    QCOMPARE(encodeAnnotation(decoded), preserved);
  }
};
QTEST_GUILESS_MAIN(UxR5CompatTests)
#include "ux_r5_compat_tests.moc"
