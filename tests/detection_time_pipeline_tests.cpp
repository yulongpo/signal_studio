#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/annotation_store.h"
#include "infrastructure/detection_model_snapshot.h"
#include "infrastructure/detection_projection.h"
#include "infrastructure/detection_refined_publication.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/detection_task.h"
#include "infrastructure/detection_time_analysis.h"
#include "infrastructure/source_loader.h"
#include "infrastructure/source_metadata.h"
#include "infrastructure/wav_metadata.h"

using namespace signalstudio;
namespace {
void check(bool ok, const QString& error) {
  if (!ok) throw std::runtime_error(error.toStdString());
}
void save(const QString& path, const QJsonObject& object) {
  QSaveFile file(path);
  check(file.open(QIODevice::WriteOnly), file.errorString());
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
  check(file.write(bytes) == bytes.size() && file.commit(), file.errorString());
}
void refinementPublicationRoundtrip() {
  FileMetadata metadata;
  metadata.id = "time-roundtrip";
  metadata.path = "unused-source.dat";
  metadata.sampleRateHz = 20e6;
  metadata.centerFrequencyHz = 2.5e9;
  metadata.sampleCount = 400000;
  metadata.demo = false;
  metadata.availability.fingerprint = "synthetic-roundtrip-identity-only";
  DetectionRequest request;
  request.runId = "time-roundtrip-run";
  request.sourceIdentity = annotationSourceIdentity(metadata, 901);
  request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  request.requestedRange =
      request.contextRange = {{0, 400000}, {2.49e9, 2.51e9}};
  QTemporaryDir directory;
  DetectionRun run;
  run.request = request;
  run.eligibleWindows = 3;
  DetectionPublication empty;
  QString error;
  check(recoverDetectionPublication(directory.path(), run, empty, error),
        error);
  FrequencyTrack track;
  track.seedId = std::string(210, 's') + "-sample-track";
  track.runId = request.runId;
  track.fileId = metadata.id;
  track.frequency = track.originalFrequency = {2.499e9, 2.501e9};
  track.nominalSupport = track.actualSupport = {0, 400000};
  track.windowDenominator = 3;
  DetectionPublication publication;
  check(projectFrequencyTracks(request, empty.sourceSet, {track},
                               8 * 1024 * 1024, publication, error),
        error);
  auto& annotation = publication.annotations.front();
  const auto rawFields = QJsonDocument::fromJson(
                             QByteArray::fromStdString(
                                 annotation.originEvidence->originalFieldsJson))
                             .object();
  check(annotation.originRecordId.size() <= 128 &&
            rawFields["seedId"].toString().toStdString() == track.seedId &&
            detectionTrackRecordId(track.seedId + "-other") !=
                annotation.originRecordId &&
            detectionTrackRecordId("short-seed") == "short-seed",
        "Long source identities must retain full provenance and distinct "
        "bounded record IDs; existing short IDs must remain stable");
  auto origin = std::make_shared<OriginEvidence>(*annotation.originEvidence);
  origin->refineMethod = RefineMethod::Sample;
  origin->refineStatus = RefineStatus::Applied;
  Segment first, second;
  first.id = annotation.id + "/activity/0";
  first.range = {{20000, 100000}, track.frequency};
  first.timeQuality = TimeQuality::AlgorithmEstimated;
  first.frequencyQuality = FrequencyQuality::ModelEstimated;
  second = first;
  second.id = annotation.id + "/activity/1";
  second.range.time = {200000, 300000};
  annotation.segments = {first, second};
  origin->originalSegments = annotation.segments;
  annotation.originEvidence = origin;
  annotation.activeSegmentId = first.id;
  for (const auto& segment : annotation.segments)
    annotation.revisions.front().segmentChanges.push_back(
        {segment.id, std::nullopt, segment});
  refreshAnnotationProjection(annotation);
  const auto records = encodeDetectionPublication(publication);
  check(records.at(0).toObject()["kind"] == "refined-annotation-v1",
        "Time events require their own durable record identity");
  check(appendDetectionChunk(directory.path(), run, records, {}, error), error);
  check(appendDetectionChunk(
            directory.path(), run,
            QJsonArray{QJsonObject{{"kind", "frequency-publication-end-v1"},
                                   {"count", "1"}}},
            {}, error),
        error);
  DetectionPublication restored;
  check(recoverDetectionPublication(directory.path(), run, restored, error),
        error);
  check(encodeDetectionPublication(restored) == records,
        "Multi-burst activity/origin/revision must recover exactly");
  auto outside = publication;
  outside.annotations.front().segments.front().range.time.end = 500000;
  DetectionPublication rejected;
  check(!decodeDetectionPublication(encodeDetectionPublication(outside),
                                    request, empty.sourceSet, 8 * 1024 * 1024,
                                    rejected, error),
        "Out-of-ROI activity must be rejected");
  auto changedOrigin = publication;
  auto differentOrigin = std::make_shared<OriginEvidence>(
      *changedOrigin.annotations.front().originEvidence);
  differentOrigin->originalSegments.front().range.time.end -= 1;
  changedOrigin.annotations.front().originEvidence = std::move(differentOrigin);
  check(!decodeDetectionPublication(encodeDetectionPublication(changedOrigin),
                                    request, empty.sourceSet, 8 * 1024 * 1024,
                                    rejected, error),
        "Raw output cannot differ from immutable original activity");
  auto humanRevision = publication;
  auto& revised = humanRevision.annotations.front();
  AnnotationRevision next;
  next.id = revised.id + ":r1";
  next.parentRevisionId = revised.revisionId;
  next.action = "begin-revision";
  next.actor = "user";
  revised.revisions.push_back(next);
  revised.revisionId = next.id;
  revised.reviewRevisionId = next.id;
  revised.reviewHistory.push_back(
      {ReviewStatus::Pending, next.id, "user", {}, {}});
  check(!decodeDetectionPublication(encodeDetectionPublication(humanRevision),
                                    request, empty.sourceSet, 8 * 1024 * 1024,
                                    rejected, error),
        "Durable algorithm origin cannot contain a human revision");
  for (const bool observationEdit : {false, true}) {
    auto humanR0 = publication;
    auto& revision = humanR0.annotations.front().revisions.front();
    if (observationEdit)
      revision.observationChanged = true;
    else
      revision.actor = "user";
    check(!decodeDetectionPublication(encodeDetectionPublication(humanR0),
                                      request, empty.sourceSet, 8 * 1024 * 1024,
                                      rejected, error),
          "Raw r0 must not contain human edit metadata");
  }
  auto manualFrequency = publication;
  auto& manual = manualFrequency.annotations.front();
  manual.segments.front().frequencyQuality = FrequencyQuality::Manual;
  auto manualOrigin = std::make_shared<OriginEvidence>(*manual.originEvidence);
  manualOrigin->originalSegments = manual.segments;
  manual.originEvidence = manualOrigin;
  manual.revisions.front().segmentChanges.front().after =
      manual.segments.front();
  check(!decodeDetectionPublication(encodeDetectionPublication(manualFrequency),
                                    request, empty.sourceSet, 8 * 1024 * 1024,
                                    rejected, error),
        "Raw activity cannot claim manually edited frequency as model output");
  auto fusionPublication = publication;
  auto other = publication.annotations.front();
  other.id += "-other";
  other.originRecordId += "-other";
  other.segments.resize(1);
  other.segments.front().range.time = {80000, 140000};
  auto prepareParent = [&](Annotation& parent, const QJsonArray& windows) {
    for (std::size_t i = 0; i < parent.segments.size(); ++i)
      parent.segments[i].id = parent.id + "/activity/" + std::to_string(i);
    parent.activeSegmentId = parent.segments.front().id;
    parent.revisionId = parent.id + ":r0";
    parent.reviewRevisionId = parent.revisionId;
    parent.revisions = {
        {parent.revisionId, {}, "algorithm-origin", "model", {}, {}}};
    parent.reviewHistory = {
        {ReviewStatus::Pending, parent.revisionId, "model", {}, {}}};
    for (const auto& segment : parent.segments)
      parent.revisions.front().segmentChanges.push_back(
          {segment.id, std::nullopt, segment});
    auto evidence = std::make_shared<OriginEvidence>(*parent.originEvidence);
    evidence->originalSegments = parent.segments;
    auto fields = QJsonDocument::fromJson(
                      QByteArray::fromStdString(evidence->originalFieldsJson))
                      .object();
    fields["windows"] = windows;
    evidence->originalFieldsJson =
        QJsonDocument(fields).toJson(QJsonDocument::Compact).toStdString();
    parent.originEvidence = std::move(evidence);
    refreshAnnotationProjection(parent);
  };
  prepareParent(fusionPublication.annotations.front(),
                QJsonArray{QJsonObject{{"windowId", "0"}, {"score", .8}}});
  prepareParent(other,
                QJsonArray{QJsonObject{{"windowId", "0"}, {"score", .9}},
                           QJsonObject{{"windowId", "1"}, {"score", .7}}});
  fusionPublication.annotations.push_back(other);
  check(fuseDetectionPublication(request, fusionPublication, error), error);
  check(
      fusionPublication.annotations.size() == 1 &&
          fusionPublication.annotations.front().segments.size() == 2 &&
          fusionPublication.annotations.front().segments.front().range.time ==
              TimeRange{20000, 140000} &&
          fusionPublication.annotations.front().derivedFrom.size() == 2 &&
          fusionPublication.annotations.front().originEvidence->windowIds ==
              std::vector<std::uint64_t>{0, 1} &&
          std::abs(
              *fusionPublication.annotations.front().originEvidence->scoreMean -
              .8) < 1e-12,
      "Refined publication must preserve both parents and unique-window max "
      "scores");
  const auto fusedFields =
      QJsonDocument::fromJson(
          QByteArray::fromStdString(fusionPublication.annotations.front()
                                        .originEvidence->originalFieldsJson))
          .object();
  check(
      !fusedFields.contains("timeAnalysis") &&
          !fusedFields.contains("seedId") && fusedFields["uniqueHits"] == "2" &&
          fusedFields["refinedFusion"].toObject()["parents"].toArray().size() ==
              2,
      "Fusion summary must not present one parent's measurements as its own");
  check(decodeDetectionPublication(
            encodeDetectionPublication(fusionPublication), request,
            empty.sourceSet, 8 * 1024 * 1024, restored, error),
        error);
  QTemporaryDir fusedDirectory;
  DetectionRun fusedRun;
  fusedRun.request = request;
  fusedRun.eligibleWindows = 3;
  const auto fusedRecords = encodeDetectionPublication(fusionPublication);
  check(appendDetectionChunk(fusedDirectory.path(), fusedRun, fusedRecords, {},
                             error),
        error);
  check(appendDetectionChunk(
            fusedDirectory.path(), fusedRun,
            QJsonArray{QJsonObject{{"kind", "frequency-publication-end-v1"},
                                   {"count", "1"}}},
            {}, error),
        error);
  check(recoverDetectionPublication(fusedDirectory.path(), fusedRun, restored,
                                    error) &&
            encodeDetectionPublication(restored) == fusedRecords,
        error);
  const auto parents =
      fusedFields["refinedFusion"].toObject()["parents"].toArray();
  for (const auto parent : parents) {
    const auto raw = parent.toObject()["originEvidence"]
                         .toObject()["originalFieldsJson"]
                         .toString();
    check(QJsonDocument::fromJson(raw.toUtf8())
                  .object()["seedId"]
                  .toString()
                  .toStdString() == track.seedId,
          "Fusion durable snapshots must preserve the complete long seed");
  }
}

void measuredFallbackKeepsBranchParameterScope() {
  QTemporaryDir directory;
  const auto path = directory.filePath("branch-scope.raw");
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), file.errorString());
  QByteArray bytes;
  for (int i = 0; i < 400000; ++i) {
    const bool active =
        (i >= 80000 && i < 100000) || (i >= 240000 && i < 260000);
    char sample[4];
    qToLittleEndian<qint16>(active ? 12000 : 0, sample);
    qToLittleEndian<qint16>(0, sample + 2);
    bytes.append(sample, 4);
  }
  check(file.write(bytes) == bytes.size(), file.errorString());
  file.close();
  FileMetadata metadata;
  metadata.id = "measured-branch-scope";
  metadata.path = path.toStdString();
  metadata.sampleRateHz = 20e6;
  metadata.centerFrequencyHz = 2.5e9;
  metadata.sampleCount = 400000;
  metadata.demo = false;
  metadata.availability.fingerprint = iqSourceFingerprint(path).toStdString();
  DetectionRequest request;
  request.runId = "measured-branch-scope-run";
  request.sourceIdentity = annotationSourceIdentity(metadata, 941);
  request.profile = *findDetectionProfile("pipeline-fixture-v1");
  request.requestedRange =
      request.contextRange = {{0, 400000}, {2.49e9, 2.51e9}};
  DetectionSourceConfig config;
  config.profileId = "fixed-grid-fixture-v1";
  FixedDetectionSource source;
  QString error;
  check(source.open(metadata, request.requestedRange, config, error, 941),
        error);
  DetectionRun run;
  run.request = request;
  DetectionPublication empty;
  check(recoverDetectionPublication(directory.path(), run, empty, error),
        error);
  FrequencyTrack track;
  track.seedId = std::string(210, 'f') + "-physical-branch-scope";
  track.runId = request.runId;
  track.fileId = metadata.id;
  track.frequency = track.originalFrequency = {2.499e9, 2.501e9};
  track.nominalSupport = track.actualSupport = {0, 400000};
  track.windowCoverage = .1;
  const auto apply = [&](DetectionTimeStrategy strategy, double minimum) {
    request.parameters.timeStrategy = strategy;
    request.parameters.minOnSeconds = .050;
    request.parameters.coarseMinBurstSeconds = minimum;
    request.parameters.coarseSplitGapSeconds = .010;
    const auto stored = request.parameters;
    DetectionPublication publication;
    check(projectFrequencyTracks(request, empty.sourceSet, {track},
                                 8 * 1024 * 1024, publication, error),
          error);
    check(applyDetectionTimeAnalysis(source, metadata, request, {track},
                                     publication, error),
          error);
    check(request.parameters == stored,
          "Physical fallback cannot alter stored parameters");
    return publication.annotations.front();
  };
  const auto rejected = apply(DetectionTimeStrategy::Coarse, .050);
  check(rejected.segments.empty() &&
            rejected.originEvidence->refineStatus == RefineStatus::Fallback,
        "Direct Coarse rejection cannot be rerun with weaker defaults");
  const auto measured = apply(DetectionTimeStrategy::Coarse, .010);
  check(!measured.segments.empty(),
        "Physical fixture must contain a measurable coarse activity");
  for (const auto strategy :
       {DetectionTimeStrategy::Sample, DetectionTimeStrategy::Roi}) {
    const auto fine = apply(strategy, .050);
    const auto otherHistory = apply(strategy, .010);
    check(!fine.segments.empty() && fine.segments == otherHistory.segments &&
              fine.originEvidence->refineStatus == RefineStatus::Fallback,
          "Physical Fine fallback must ignore inactive Coarse history");
    const auto fields =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(fine.originEvidence->originalFieldsJson))
            .object();
    const auto fallback =
        fields["timeAnalysis"].toObject()["fallbackCoarseEvidence"].toObject();
    if (strategy == DetectionTimeStrategy::Roi) {
      const auto time = fields["timeAnalysis"].toObject();
      check(time["roiThresholdDomain"] == "positive-db-residual-mean-v1" &&
                time["roiTimeThreshold"].isDouble() &&
                time["roiFrequencyThreshold"].isDouble(),
            "Saved physical ROI evidence must contain both actual gates");
    }
    check(fallback["effectiveCoarseMinBurstSeconds"].toDouble() == .010 &&
              fallback["effectiveCoarseSplitGapSeconds"].toDouble() == .020 &&
              fallback["parameterPolicy"].toString().contains("fixed-default"),
          "Saved fallback evidence must name the actual fixed policy");
  }

  // A silent physical source must preserve both failed measurements without
  // publishing the nominal model support as an activity segment.
  const auto silentPath = directory.filePath("silent-branch-scope.raw");
  QFile silentFile(silentPath);
  check(silentFile.open(QIODevice::WriteOnly), silentFile.errorString());
  const QByteArray silence(400000 * 4, '\0');
  check(silentFile.write(silence) == silence.size(), silentFile.errorString());
  silentFile.close();
  metadata.path = silentPath.toStdString();
  metadata.availability.fingerprint =
      iqSourceFingerprint(silentPath).toStdString();
  request.runId += "-silent";
  request.sourceIdentity = annotationSourceIdentity(metadata, 942);
  track.runId = request.runId;
  check(source.open(metadata, request.requestedRange, config, error, 942),
        error);
  run.request = request;
  QTemporaryDir silentPublication;
  check(
      recoverDetectionPublication(silentPublication.path(), run, empty, error),
      error);
  for (const auto strategy :
       {DetectionTimeStrategy::Sample, DetectionTimeStrategy::Roi}) {
    const auto fine = apply(strategy, .050);
    check(fine.segments.empty() && fine.observation &&
              fine.observationTimeQuality == TimeQuality::WindowObservation &&
              fine.originEvidence->originalSegments.empty() &&
              fine.originEvidence->refineStatus == RefineStatus::Fallback,
          "Silent Fine and Coarse attempts cannot invent an activity");
    const auto fields =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(fine.originEvidence->originalFieldsJson))
            .object();
    const auto fallback =
        fields["timeAnalysis"].toObject()["fallbackCoarseEvidence"].toObject();
    check(fallback["status"] == "fallback" &&
              fallback["fallbackReason"] == "coarse-no-supported-on-run" &&
              fallback["thresholdDomain"] == "spectrum-density-db-v1" &&
              fallback["noise"].isDouble() &&
              fallback["threshold"].isDouble() &&
              fallback["parameterPolicy"].toString().contains("fixed-default"),
          "Silent fallback must save the real failed Coarse measurement");
    check(!fine.originEvidence->fallbackReason.empty() &&
              fine.originEvidence->fallbackReason.find(
                  "measured-coarse-fallback") == std::string::npos,
          "A failed Coarse attempt cannot claim measured fallback activity");
  }
}

void runFile(const QString& wav, const QString& output, const QString& strategy,
             bool shortProfile) {
  const auto started = std::chrono::steady_clock::now();
  QString error;
  WavDescriptor descriptor;
  check(readWavDescriptor(wav, descriptor, error), error);
  FileMetadata metadata;
  metadata.id = "m4-source-" + QFileInfo(wav).completeBaseName().toStdString();
  metadata.path = QFileInfo(wav).absoluteFilePath().toStdString();
  metadata.name = QFileInfo(wav).fileName().toStdString();
  metadata.sampleFormat = descriptor.format;
  metadata.sampleRateHz = descriptor.sampleRateHz;
  metadata.sampleCount = descriptor.sampleCount;
  metadata.demo = false;
  metadata.centerFrequencyHz = 2.5e9;
  metadata.availability.fingerprint = iqSourceFingerprint(wav).toStdString();
  DetectionRequest request;
  request.runId = "m4-" + strategy.toStdString() + "-" +
                  (shortProfile ? "short-" : "default-") + metadata.id;
  request.sourceIdentity = annotationSourceIdentity(metadata, 940);
  request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  request.requestedRange = request.contextRange = {
      {0, metadata.sampleCount},
      {2.5e9 - metadata.sampleRateHz / 2, 2.5e9 + metadata.sampleRateHz / 2}};
  request.rangeOrigin = "m4-frozen-complete-file-validation";
  request.evidenceBudgetBytes = 128 * 1024 * 1024;
  if (strategy == "sample" || strategy == "no-split")
    request.parameters.timeStrategy = DetectionTimeStrategy::Sample;
  else if (strategy == "roi" || strategy == "roi-no-split")
    request.parameters.timeStrategy = DetectionTimeStrategy::Roi;
  else if (strategy == "coarse" || strategy == "coarse-no-split")
    request.parameters.timeStrategy = DetectionTimeStrategy::Coarse;
  else if (strategy == "none")
    request.parameters.timeStrategy = DetectionTimeStrategy::None;
  else
    check(false, "Unknown time strategy");
  request.parameters.splitBursts = !strategy.endsWith("no-split");
  if (shortProfile) {
    request.parameters.minOnSeconds = .0005;
    request.parameters.minOffSeconds = .001;
    request.parameters.smoothSeconds = .0001;
  }
  check(freezeDetectionModelSnapshot(request, error), error);
  check(QDir().mkpath(output), "Cannot create M4 evidence directory");
  DetectionTaskManager manager(QCoreApplication::applicationDirPath() +
                               "/SignalStudioDetectionWorker.exe");
  check(manager.enqueue(output, metadata, request, error), error);
  const auto deadline = started + std::chrono::minutes(20);
  DetectionRun terminal;
  bool finished = false;
  std::string previousPhase;
  std::uint64_t previousRefined = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    manager.pump();
    const auto runs = manager.snapshots();
    if (!runs.empty() && (runs.front().computationPhase != previousPhase ||
                          runs.front().refinedCandidates != previousRefined)) {
      previousPhase = runs.front().computationPhase;
      previousRefined = runs.front().refinedCandidates;
      std::cout << "phase=" << previousPhase << " refined=" << previousRefined
                << "/" << runs.front().totalRefinementCandidates
                << " elapsedSeconds="
                << std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - started)
                       .count()
                << std::endl;
    }
    if (!runs.empty() && detectionIsTerminal(runs.front().lifecycle)) {
      terminal = runs.front();
      finished = true;
      break;
    }
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  check(finished, "M4 actual pipeline exceeded its validation deadline");
  DetectionPublication publication;
  const bool published = manager.takePublication(request.runId, publication);
  QJsonArray records;
  if (published) records = encodeDetectionPublication(publication);
  bool recoveredExactly = false;
  if (published) {
    DetectionPublication recovered;
    check(recoverDetectionPublication(output, terminal, recovered, error),
          error);
    recoveredExactly = encodeDetectionPublication(recovered) == records;
    check(recoveredExactly, "Actual time publication did not recover exactly");
  }
  QJsonArray states;
  std::size_t segments = 0;
  for (const auto& annotation : publication.annotations) {
    segments += annotation.segments.size();
    states.append(QJsonObject{
        {"id", QString::fromStdString(annotation.id)},
        {"segments", static_cast<int>(annotation.segments.size())},
        {"method", static_cast<int>(annotation.originEvidence->refineMethod)},
        {"status", static_cast<int>(annotation.originEvidence->refineStatus)},
        {"reason",
         QString::fromStdString(annotation.originEvidence->fallbackReason)}});
  }
  save(QDir(output).filePath("result.json"),
       {{"schemaVersion", 1},
        {"pass", terminal.lifecycle == DetectionLifecycle::Completed &&
                     published && recoveredExactly},
        {"sourcePath", wav},
        {"sourceFingerprint",
         QString::fromStdString(request.sourceIdentity.quickFingerprint)},
        {"sampleRateHz", metadata.sampleRateHz},
        {"centerFrequencyHz", metadata.centerFrequencyHz},
        {"physicalSamples", QString::number(metadata.sampleCount)},
        {"strategy", strategy},
        {"shortProfile", shortProfile},
        {"run", encodeDetectionRun(terminal)},
        {"annotations", records},
        {"activitySegments", static_cast<int>(segments)},
        {"refinementStates", states},
        {"exactRecovery", recoveredExactly},
        {"elapsedSeconds", std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count()}});
  check(terminal.lifecycle == DetectionLifecycle::Completed,
        QString::fromStdString(terminal.error));
}
}  // namespace
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == "--file")
      runFile(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]),
              QString::fromLocal8Bit(argv[4]),
              argc >= 6 && QString::fromLocal8Bit(argv[5]) == "--short");
    else {
      refinementPublicationRoundtrip();
      measuredFallbackKeepsBranchParameterScope();
    }
    std::cout << "Time pipeline checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
