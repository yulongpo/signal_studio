#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>

#include "application/annotation_service.h"
#include "domain/scn_detection_postprocess.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_model_snapshot.h"
#include "infrastructure/detection_projection.h"
#include "infrastructure/detection_source.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/detection_task.h"
#include "infrastructure/detection_worker_client.h"
#include "infrastructure/source_loader.h"
#include "infrastructure/wav_metadata.h"

using namespace signalstudio;
namespace {
void check(bool ok, const QString& error) {
  if (!ok) throw std::runtime_error(error.toStdString());
}
QString workerPath() {
  return QCoreApplication::applicationDirPath() +
#ifdef Q_OS_WIN
         "/SignalStudioDetectionWorker.exe";
#else
         "/SignalStudioDetectionWorker";
#endif
}
void writeFile(const QString& path, const QByteArray& bytes) {
  QSaveFile file(path);
  check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
            file.commit(),
        "Cannot publish pipeline evidence " + path);
}
QByteArray floatBytes(const std::vector<float>& values) {
  QByteArray bytes(static_cast<qsizetype>(values.size() * sizeof(float)), '\0');
  for (std::size_t i = 0; i < values.size(); ++i)
    qToLittleEndian<quint32>(std::bit_cast<quint32>(values[i]),
                             bytes.data() + i * sizeof(float));
  return bytes;
}
void projectionRoundtrip() {
  DetectionRequest request;
  request.runId = "projection-scope";
  request.sourceIdentity.fileId = "file-a";
  request.sourceIdentity.sampleRateHz = 65536;
  request.sourceIdentity.centerFrequencyHz = 100000;
  request.sourceIdentity.availableSamples = 1000;
  request.sourceIdentity.physicalSamples = 1000;
  request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  request.parameters.mergeFrequencyHz = 0;
  request.requestedRange = {{0, 1000}, {67232, 132768}};
  AnnotationSourceSet set;
  set.id = "source-set";
  set.fileId = "file-a";
  DetectionResultProjector projector;
  QString error;
  check(projector.reset(request, 3, 32 * 1024 * 1024, error), error);
  QJsonArray results, mappings;
  for (int index = 0; index < 3; ++index) {
    results.append(QJsonObject{
        {"windowId", QString::number(10 + index)},
        {"candidates", QJsonArray{QJsonObject{{"lowerBin", 30000.0},
                                              {"upperBin", 31000.0},
                                              {"score", .8 + .05 * index},
                                              {"peakIndex", "7600"}},
                                  QJsonObject{{"lowerBin", 50000.0},
                                              {"upperBin", 51000.0},
                                              {"score", .9},
                                              {"peakIndex", "12600"}}}}});
    mappings.append(
        QJsonObject{{"windowId", QString::number(10 + index)},
                    {"startSample", QString::number(index * 100)},
                    {"endSample", QString::number(index * 100 + 200)},
                    {"contextStartSample", QString::number(index * 100)},
                    {"contextEndSample", QString::number(index * 100 + 300)},
                    {"frequencyFirstBinHz", 67232.0},
                    {"frequencyStepHz", 1.0}});
  }
  check(projector.consume(results, mappings, error), error);
  std::vector<FrequencyTrack> tracks;
  check(projector.finish(tracks, error), error);
  check(tracks.size() == 2 && tracks[0].hits == 3 &&
            tracks[0].windowDenominator == 3,
        "Association must count unique window evidence");
  auto wrongGrid = mappings;
  auto badMapping = wrongGrid[0].toObject();
  badMapping["frequencyStepHz"] = 2.0;
  wrongGrid[0] = badMapping;
  DetectionResultProjector malformed;
  check(malformed.reset(request, 3, 32 * 1024 * 1024, error) &&
            !malformed.consume(results, wrongGrid, error),
        "A recovered FFT grid must match the immutable physical source");
  DetectionPublication publication;
  check(projectFrequencyTracks(request, set, tracks, 32 * 1024 * 1024,
                               publication, error),
        error);
  check(publication.annotations.size() == 2 &&
            publication.annotations[0].segments.empty() &&
            !publication.annotations[0].classLabel,
        "A frequency model cannot invent activity intervals or a class");
  const auto records = encodeDetectionPublication(publication);
  DetectionPublication restored;
  check(decodeDetectionPublication(records, request, set, 32 * 1024 * 1024,
                                   restored, error),
        error);
  check(encodeDetectionPublication(restored) == records,
        "Immutable frequency evidence must roundtrip exactly");
  auto foreign = records;
  auto record = foreign[0].toObject();
  auto annotation = record["annotation"].toObject();
  annotation["fileId"] = "file-b";
  record["annotation"] = annotation;
  foreign[0] = record;
  check(!decodeDetectionPublication(foreign, request, set, 32 * 1024 * 1024,
                                    restored, error) &&
            encodeDetectionPublication(restored) == records,
        "Foreign publication must be rejected without replacing prior data");
}

void scnOrderAndOccupancy() {
  std::vector<float> values(16 * 64, -20);
  for (std::size_t row = 0; row < 16; ++row) {
    for (std::size_t bin = 30; bin < 34; ++bin) values[row * 64 + bin] = 0;
    values[row * 64 + 37] = row ? -52 : -20;
  }
  const std::vector<DecodedCandidate> raw{{25.2, 37.4, .9, 8},
                                          {37.5, 41.5, .8, 10}};
  ScnWindowPostprocess result;
  std::string error;
  check(postprocessScnWindow(values, 16, 64, raw, 0, 1, result, error),
        QString::fromStdString(error));
  check(result.measurements.size() == 2 && result.measurements[0].accepted &&
            !result.measurements[1].accepted &&
            result.unfilteredFused.size() == 2 &&
            result.filteredFused.size() == 1 &&
            result.filteredFused[0].lowerBin == 25 &&
            result.filteredFused[0].upperBin == 37 &&
            result.candidates.size() == 1 &&
            result.candidates[0].lowerBin == 30 &&
            result.candidates[0].upperBin == 34 &&
            result.refinements[0].noiseFloorDb == -20 &&
            result.refinements[0].noiseSampleCount == 1,
        "SCN integer CNR boundaries and rejected-candidate occupancy must "
        "precede minimum-noise correction");
  const auto prior = result.candidates;
  check(!postprocessScnWindow(values, 16, 64, raw, 0, 1, result, error,
                              [] { return true; }) &&
            result.candidates == prior,
        "Cancelled SCN postprocessing must retain prior complete output");
}

void publicationByteBudgetRoundtrip() {
  QTemporaryDir directory;
  FileMetadata metadata;
  metadata.id = "large-publication-boundary";
  metadata.path = directory.filePath("synthetic-boundary.iq").toStdString();
  metadata.sampleRateHz = 1e6;
  metadata.centerFrequencyHz = 100e6;
  metadata.sampleCount = 20000;
  metadata.demo = false;
  writeFile(QString::fromStdString(metadata.path), QByteArray(80000, '\0'));
  metadata.availability.fingerprint =
      iqSourceFingerprint(QString::fromStdString(metadata.path)).toStdString();
  DetectionRun run;
  run.request.runId = "synthetic-large-final-publication-not-model-inference";
  run.request.sourceIdentity = annotationSourceIdentity(metadata, 991);
  run.request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  run.request.requestedRange = {{0, 20000}, {99.5e6, 100.5e6}};
  run.request.contextRange = run.request.requestedRange;
  run.lifecycle = DetectionLifecycle::Running;
  run.eligibleWindows = run.completedWindows = 3;
  QString error;
  DetectionPublication empty;
  check(recoverDetectionPublication(directory.path(), run, empty, error),
        error);
  std::vector<FrequencyTrack> tracks;
  for (std::size_t i = 0; i < 80; ++i) {
    FrequencyTrack track;
    track.seedId = "boundary-" + std::to_string(i);
    track.runId = run.request.runId;
    track.fileId = metadata.id;
    track.frequency = track.originalFrequency = {100e6 + i * 1000.0,
                                                 100e6 + i * 1000.0 + 500};
    track.nominalSupport = track.actualSupport = {0, 20000};
    track.scoreMean = track.scoreMax = .9;
    track.windowDenominator = 3;
    tracks.push_back(std::move(track));
  }
  DetectionPublication publication;
  check(projectFrequencyTracks(run.request, empty.sourceSet, tracks,
                               64 * 1024 * 1024, publication, error),
        error);
  const std::string padding(120 * 1024, 'x');
  for (auto& annotation : publication.annotations) {
    auto origin = std::make_shared<OriginEvidence>(*annotation.originEvidence);
    origin->originalFieldsJson =
        "{\"syntheticBudgetFixture\":\"" + padding + "\"}";
    annotation.originEvidence = std::move(origin);
  }
  check(
      validateDetectionPublicationBudget(publication, 64 * 1024 * 1024, error),
      error);
  const auto records = encodeDetectionPublication(publication);
  check(QJsonDocument(records).toJson(QJsonDocument::Compact).size() >
            8 * 1024 * 1024,
        "The recovery regression must exercise a final collection larger than "
        "8 MiB");
  for (qsizetype first = 0; first < records.size(); first += 12) {
    QJsonArray shard;
    for (qsizetype i = first; i < std::min(first + 12, records.size()); ++i)
      shard.append(records[i]);
    check(appendDetectionChunk(directory.path(), run, shard, {}, error), error);
  }
  check(appendDetectionChunk(
            directory.path(), run,
            QJsonArray{QJsonObject{
                {"kind", "frequency-publication-end-v1"},
                {"count", QString::number(publication.annotations.size())}}},
            {}, error),
        error);
  run.lifecycle = DetectionLifecycle::Completed;
  check(writeDetectionRun(directory.path(), run, error), error);
  DetectionPublication restored;
  check(recoverDetectionPublication(directory.path(), run, restored, error),
        error);
  check(encodeDetectionPublication(restored) == records,
        "Every legal multi-shard final publication must recover beyond the "
        "former 8 MiB aggregate limit");
}

void livePipeline(const QString& wavPath, const QString& output) {
  QString error;
  WavDescriptor descriptor;
  check(readWavDescriptor(wavPath, descriptor, error), error);
  FileMetadata metadata;
  metadata.id = "m3-real-generator-wav";
  metadata.name = QFileInfo(wavPath).fileName().toStdString();
  metadata.path = QFileInfo(wavPath).absoluteFilePath().toStdString();
  metadata.sampleFormat = descriptor.format;
  metadata.sampleRateHz = descriptor.sampleRateHz;
  metadata.sampleCount = descriptor.sampleCount;
  metadata.centerFrequencyHz = 2.5e9;
  metadata.demo = false;
  metadata.availability.fingerprint =
      iqSourceFingerprint(QString::fromStdString(metadata.path)).toStdString();
  QDir root(output);
  check(root.mkpath("."), "Cannot create pipeline report directory");
  QJsonArray profiles;
  for (const auto& profileId : {"ffscn-gui-iq-v1", "scn-max-psd-v1"}) {
    DetectionRequest request;
    request.runId = std::string("m3-live-") + profileId;
    request.sourceIdentity = annotationSourceIdentity(metadata, 901);
    request.profile = *findDetectionProfile(profileId);
    // This retained M3 oracle isolates frequency/L1/L2. M4 event accuracy is
    // exercised by the separate full-file time pipeline and paired evaluator.
    request.parameters.timeStrategy = DetectionTimeStrategy::None;
    request.requestedRange = {
        {0, std::min(metadata.sampleCount,
                     static_cast<SampleIndex>(descriptor.sampleRateHz / 50))},
        {2.5e9 - descriptor.sampleRateHz / 2.0,
         2.5e9 + descriptor.sampleRateHz / 2.0}};
    request.contextRange = {{0, metadata.sampleCount},
                            request.requestedRange.frequency};
    request.rangeOrigin = "live-model-20ms-validation";
    check(freezeDetectionModelSnapshot(request, error), error);
    const auto directory = root.filePath(profileId);
    check(root.mkpath(profileId), "Cannot create profile evidence directory");
    FixedDetectionSource source;
    DetectionSourceConfig config;
    config.profileId = profileId;
    config.fftSize = request.profile.fftSize;
    config.modelFrames = request.profile.modelFrames;
    config.modelHopFrames = request.profile.modelHopFrames;
    check(source.open(metadata, request.requestedRange, config, error, 901),
          error);
    DetectionWorkerClient worker;
    check(worker.start(workerPath(), request, error, [] { return false; }),
          error);
    DetectionModelWindow first;
    check(source.readModelWindow(0, first, error), error);
    const auto& input = request.profile.maximumPsdOnly ? first.densityDb
                                                       : first.normalizedTensor;
    writeFile(QDir(directory).filePath("first-input.f32"), floatBytes(input));
    writeFile(QDir(directory).filePath("first-density.f32"),
              floatBytes(first.densityDb));
    DetectionWorkerPacket batch{
        workerEnvelope(
            "batch", "live-head-check", 0,
            {{"windows", QJsonArray{QJsonObject{{"windowId", "0"}}}}},
            QString::fromStdString(request.runId), 901,
            QString::fromStdString(metadata.id)),
        {}};
    attachFloat32Payload(batch, input,
                         {1, 1, static_cast<int>(config.modelFrames),
                          static_cast<int>(config.fftSize)});
    DetectionWorkerPacket response;
    check(worker.exchange(std::move(batch), response, error,
                          [] { return false; }),
          error);
    check(response.envelope["payload"].toObject()["candidateEncoding"] ==
              "raw-heads-v1",
          "Live worker must return original finite model heads");
    writeFile(QDir(directory).filePath("first-heads.f32"), response.binary);
    writeFile(QDir(directory).filePath("first-response.json"),
              QJsonDocument(response.envelope).toJson());
    worker.stop();
    DetectionTaskManager tasks(workerPath());
    check(tasks.enqueue(directory, metadata, request, error), error);
    QElapsedTimer timeout;
    timeout.start();
    DetectionRun run;
    bool terminal = false;
    while (timeout.elapsed() < 120000 && !terminal) {
      tasks.pump();
      for (const auto& snapshot : tasks.snapshots())
        if (snapshot.request.runId == request.runId &&
            detectionIsTerminal(snapshot.lifecycle)) {
          run = snapshot;
          terminal = true;
        }
      QCoreApplication::processEvents();
      QThread::msleep(5);
    }
    check(terminal && run.lifecycle == DetectionLifecycle::Completed,
          QString::fromStdString(run.error));
    check(
        run.completedWindows == run.eligibleWindows && run.completedWindows > 0,
        "Actual model coverage must complete every eligible observation");
    DetectionPublication publication;
    check(tasks.takePublication(request.runId, publication),
          "Completed production run must expose a durable source set");
    check(!publication.annotations.empty(),
          "Frozen real IQ should yield model observations");
    for (const auto& annotation : publication.annotations)
      check(annotation.segments.empty() && annotation.originEvidence &&
                !annotation.classLabel &&
                annotation.originEvidence->windowCount == run.eligibleWindows,
            "Actual frequency predictions retain unknown event time/class and "
            "eligible denominator");
    DetectionRun loaded;
    check(loadDetectionRun(directory, QString::fromStdString(request.runId),
                           loaded, error),
          error);
    DetectionPublication restored;
    check(recoverDetectionPublication(directory, loaded, restored, error),
          error);
    check(encodeDetectionPublication(restored) ==
              encodeDetectionPublication(publication),
          "Real model publication must recover exact corrected frequency and "
          "origin evidence");
    writeFile(QDir(directory).filePath("annotations.json"),
              QJsonDocument(encodeDetectionPublication(publication)).toJson());
    bool productionPrefixVerified = false;
    if (request.profile.backend == DetectionBackendKind::TensorRt) {
      auto partialRequest = request;
      partialRequest.runId += "-cancel-prefix";
      partialRequest.requestedRange.time.end = metadata.sampleCount;
      check(tasks.enqueue(directory, metadata, partialRequest, error), error);
      timeout.restart();
      bool cancellationAccepted = false;
      DetectionRun partialRun;
      while (timeout.elapsed() < 120000) {
        tasks.pump();
        for (const auto& snapshot : tasks.snapshots()) {
          if (snapshot.request.runId != partialRequest.runId) continue;
          if (!cancellationAccepted && snapshot.completedWindows >= 2 &&
              !detectionIsTerminal(snapshot.lifecycle))
            cancellationAccepted = tasks.cancel(partialRequest.runId);
          if (detectionIsTerminal(snapshot.lifecycle)) partialRun = snapshot;
        }
        if (!partialRun.request.runId.empty()) break;
        QThread::msleep(5);
      }
      check(cancellationAccepted &&
                partialRun.lifecycle == DetectionLifecycle::Cancelled &&
                partialRun.completedWindows >= 2 &&
                partialRun.completedWindows < partialRun.eligibleWindows,
            "Production cancellation must retain an incomplete but durable "
            "model prefix");
      DetectionPublication partialPublication;
      check(tasks.takePublication(partialRequest.runId, partialPublication) &&
                !partialPublication.annotations.empty(),
            "The UI publication path must expose completed production prefix "
            "observations");
      check(
          recoverDetectionPublication(directory, partialRun, restored, error) &&
              encodeDetectionPublication(restored) ==
                  encodeDetectionPublication(partialPublication),
          error);
      for (const auto& annotation : partialPublication.annotations)
        check(annotation.segments.empty() &&
                  annotation.originEvidence->windowCount ==
                      partialRun.eligibleWindows,
              "Cancelled prefix cannot invent event time or reduce eligible "
              "denominator");
      writeFile(QDir(directory).filePath("cancelled-prefix-run.json"),
                QJsonDocument(encodeDetectionRun(partialRun)).toJson());
      writeFile(QDir(directory).filePath("cancelled-prefix-annotations.json"),
                QJsonDocument(encodeDetectionPublication(partialPublication))
                    .toJson());
      productionPrefixVerified = true;
    }
    profiles.append(QJsonObject{
        {"profileId", profileId},
        {"run", encodeDetectionRun(run)},
        {"annotations", static_cast<int>(publication.annotations.size())},
        {"workerEvidence", worker.evidence()},
        {"firstFrameIndex", QString::number(first.firstFrameIndex)},
        {"sourceLeadingPadding",
         QString::number(source.plan().sourceLeadingPadding)},
        {"exactRecovery", true}});
    auto profileReport = profiles.last().toObject();
    profileReport["productionCancelledPrefixVerified"] =
        productionPrefixVerified;
    profiles[profiles.size() - 1] = profileReport;
  }
  writeFile(root.filePath("pipeline-report.json"),
            QJsonDocument(
                QJsonObject{{"schema", "signalstudio-live-model-pipeline-v1"},
                            {"wav", wavPath},
                            {"profiles", profiles},
                            {"timeEventsVerified", false},
                            {"passed", true}})
                .toJson());
}
}  // namespace
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    projectionRoundtrip();
    scnOrderAndOccupancy();
    publicationByteBudgetRoundtrip();
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--live")
      livePipeline(QString::fromLocal8Bit(argv[2]),
                   QString::fromLocal8Bit(argv[3]));
    else
      check(argc == 1,
            "Usage: detection-pipeline-tests [--live WAV report-directory]");
    std::cout << "Detection projection/pipeline checks passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
