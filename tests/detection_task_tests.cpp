#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/detection_task.h"
#include "infrastructure/detection_worker_client.h"
#include "infrastructure/project_store.h"
#include "infrastructure/source_loader.h"

using namespace signalstudio;
namespace {
void check(bool condition, const QString& error) {
  if (!condition) throw std::runtime_error(error.toStdString());
}
FileMetadata sourceFixture(const QTemporaryDir& directory) {
  FileMetadata metadata;
  metadata.id = "fixture-source";
  metadata.name = "tone.iq";
  metadata.path = directory.filePath("tone.iq").toStdString();
  metadata.sampleRateHz = 1e6;
  metadata.centerFrequencyHz = 100e6;
  metadata.sampleCount = 20000;
  metadata.demo = false;
  QByteArray samples(20000 * 4, '\0');
  for (int i = 0; i < 20000; ++i) {
    const auto angle = 2 * 3.14159265358979323846 * .125 * i;
    qToLittleEndian<qint16>(static_cast<qint16>(12000 * std::cos(angle)),
                            samples.data() + i * 4);
    qToLittleEndian<qint16>(static_cast<qint16>(12000 * std::sin(angle)),
                            samples.data() + i * 4 + 2);
  }
  QFile file(QString::fromStdString(metadata.path));
  check(
      file.open(QIODevice::WriteOnly) && file.write(samples) == samples.size(),
      "Write task IQ fixture");
  return metadata;
}
DetectionRequest requestFixture(const FileMetadata& metadata,
                                const std::string& id) {
  DetectionRequest request;
  request.runId = id;
  request.sourceIdentity = annotationSourceIdentity(metadata, 42);
  request.profile = *findDetectionProfile("pipeline-fixture-v1");
  request.requestedRange = {{0, metadata.sampleCount}, {99.5e6, 100.5e6}};
  request.contextRange = request.requestedRange;
  request.contextRange.time.end = availableSamples(metadata);
  request.rangeOrigin = "native-test-fixture";
  return request;
}
DetectionRun waitForRun(DetectionTaskManager& tasks, const std::string& id) {
  QElapsedTimer timeout;
  timeout.start();
  while (timeout.elapsed() < 30000) {
    tasks.pump();
    for (const auto& run : tasks.snapshots())
      if (run.request.runId == id && detectionIsTerminal(run.lifecycle))
        return run;
    QCoreApplication::processEvents();
    QThread::msleep(5);
  }
  throw std::runtime_error("Task did not reach a terminal state");
}
QString workerPath() {
#ifdef Q_OS_WIN
  return QCoreApplication::applicationDirPath() +
         "/SignalStudioDetectionWorker.exe";
#else
  return QCoreApplication::applicationDirPath() +
         "/SignalStudioDetectionWorker";
#endif
}
void testTaskRoundtrip() {
  QTemporaryDir directory;
  const auto metadata = sourceFixture(directory);
  const auto request = requestFixture(metadata, "task-roundtrip");
  DetectionTaskManager tasks(workerPath());
  QString error;
  check(tasks.enqueue(directory.path(), metadata, request, error), error);
  const auto run = waitForRun(tasks, request.runId);
  check(run.lifecycle == DetectionLifecycle::Completed,
        QString::fromStdString(run.error));
  check(run.completedWindows == 3 && run.readFrames == 20 &&
            run.chunks.size() == 2,
        "Backpressure batch size and unique read-frame facts must be retained");
  DetectionPublication publication;
  check(tasks.takePublication(request.runId, publication) &&
            !tasks.takePublication(request.runId, publication),
        "Publication must be delivered exactly once");
  check(publication.sourceSet.profileId == "pipeline-fixture-v1" &&
            publication.annotations.size() == 3,
        "Fixture provenance and bounded window results must be explicit");
  FileState file;
  file.metadata = metadata;
  file.view = request.requestedRange;
  AnnotationService annotations;
  const auto published = annotations.publishSourceSet(
      file, publication.sourceSet, publication.annotations);
  check(published.ok, QString::fromStdString(published.error));
  check(file.marks[0].segments.empty() && file.marks[0].observation &&
            !segmentCanCreateChannel(file.marks[0], {}),
        "Pipeline fixture cannot invent precise event time or a confirmed DDC "
        "segment");
  file.detectionRuns.push_back(run);
  DetectionPublication recovered;
  check(recoverDetectionPublication(directory.path(), run, recovered, error),
        error);
  check(recovered.annotations.size() == file.marks.size(),
        "Recovery must restore completed results");
  for (std::size_t i = 0; i < recovered.annotations.size(); ++i)
    check(
        encodeAnnotation(recovered.annotations[i]) ==
            encodeAnnotation(publication.annotations[i]),
        "Recovery must preserve canonical IDs, original evidence and geometry");
  const auto beforeRecoveredPublication = exportAnnotationsJson(file);
  const auto repeated = annotations.publishSourceSet(file, recovered.sourceSet,
                                                     recovered.annotations);
  check(repeated.ok && !repeated.changed &&
            exportAnnotationsJson(file) == beforeRecoveredPublication,
        "Recovering an existing source cannot overwrite committed review "
        "history");
  Project project;
  project.files.push_back(std::move(file));
  project.activeFileId = metadata.id;
  const auto projectPath = directory.filePath("project.json");
  check(ProjectStore::save(projectPath, project, error), error);
  Project loaded;
  check(ProjectStore::load(projectPath, loaded, error), error);
  check(loaded.files[0].detectionRuns.size() == 1 &&
            loaded.files[0].marks.size() == 3,
        "Project v5 must preserve run references and canonical annotations");
  QJsonArray chunks;
  check(readDetectionChunks(directory.path(), run, chunks, error), error);
  const auto mapping = chunks[0].toObject()["mappings"].toArray()[0].toObject();
  check(mapping["numericalVersion"].isString() &&
            mapping.contains("floorGuardApplied") &&
            mapping["frames"].toArray()[0].toObject().contains("fftSupports"),
        "Persisted preprocessing facts must distinguish finite guards and real "
        "FFT supports");
  const auto before = QFileInfo(projectPath).lastModified();
  auto queued = requestFixture(metadata, "cancel-before-start");
  check(tasks.enqueue(directory.path(), metadata, queued, error), error);
  check(tasks.cancel(queued.runId),
        "Queued cancellation must be accepted immediately");
  check(waitForRun(tasks, queued.runId).lifecycle ==
            DetectionLifecycle::Cancelled,
        "Queued cancellation cannot launch the worker");
  check(QFileInfo(projectPath).lastModified() == before,
        "Task persistence cannot auto-save unrelated project edits");
}
void testFailuresAndGeneration() {
  QTemporaryDir directory;
  const auto metadata = sourceFixture(directory);
  QString error;
  DetectionTaskManager missingWorker(directory.filePath("missing-worker.exe"));
  auto request = requestFixture(metadata, "missing-worker");
  check(missingWorker.enqueue(directory.path(), metadata, request, error),
        error);
  const auto failed = waitForRun(missingWorker, request.runId);
  check(
      failed.lifecycle == DetectionLifecycle::Failed &&
          failed.completedWindows == 0 && !failed.error.empty(),
      "Missing worker must preserve a failed run with zero completed coverage");
  auto changed = metadata;
  changed.sampleRateHz = 2e6;
  check(
      !missingWorker.enqueue(directory.path(), changed,
                             requestFixture(metadata, "changed-source"), error),
      "A stale metadata request must fail before enqueue");
  DetectionTaskManager tasks(workerPath());
  auto smallBudget = requestFixture(metadata, "insufficient-pipeline-budget");
  smallBudget.workingSetBudgetBytes = 16 * 1024 * 1024;
  check(!tasks.enqueue(directory.path(), metadata, smallBudget, error) &&
            error.contains("budget"),
        "Budget must account for batched transport, not just one DSP window");
  request = requestFixture(metadata, "old-generation");
  check(tasks.enqueue(directory.path(), metadata, request, error), error);
  tasks.cancelGeneration(42);
  check(waitForRun(tasks, request.runId).lifecycle ==
            DetectionLifecycle::Cancelled,
        "Replacing a project must cancel its queued generation");
  request = requestFixture(metadata, "missing-model");
  request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  check(tasks.enqueue(directory.path(), metadata, request, error), error);
  check(
      waitForRun(tasks, request.runId).lifecycle == DetectionLifecycle::Failed,
      "A production profile cannot silently use the fixture backend");
  auto begun = requestFixture(metadata, "worker-bound-identity");
  begun.sourceIdentity.quickFingerprint =
      iqSourceFingerprint(QString::fromStdString(metadata.path)).toStdString();
  DetectionWorkerClient worker;
  check(worker.start(workerPath(), begun, error, [] { return false; }), error);
  DetectionWorkerPacket foreign{
      workerEnvelope("batch", "cross-run", 0, {}, "other-run", 42,
                     QString::fromStdString(metadata.id)),
      {}};
  DetectionWorkerPacket response;
  check(!worker.exchange(foreign, response, error, [] { return false; }) &&
            error.contains("identity"),
        "Worker must reject a foreign run even when the client echoes its "
        "envelope");
}
void testPartialAndActiveCancellation() {
  QTemporaryDir directory;
  auto metadata = sourceFixture(directory);
  {
    QFile file(QString::fromStdString(metadata.path));
    check(file.open(QIODevice::ReadWrite) && file.resize(40000 * 4),
          "Extend physical IQ fixture");
  }
  metadata.sampleCount = 40000;
  metadata.availability.status = LoadStatus::Partial;
  metadata.availability.availableSamples = 20000;
  DetectionTaskManager tasks(workerPath());
  QString error;
  auto request = requestFixture(metadata, "partial-physical-roi");
  check(tasks.enqueue(directory.path(), metadata, request, error), error);
  const auto partial = waitForRun(tasks, request.runId);
  check(partial.lifecycle == DetectionLifecycle::Completed &&
            partial.completedWindows == 3,
        QString::fromStdString(partial.error));
  check(std::any_of(partial.coverage.begin(), partial.coverage.end(),
                    [](const auto& coverage) {
                      return coverage.kind == DetectionCoverageKind::Excluded &&
                             coverage.range.time.end == 40000;
                    }),
        "Partial completion must preserve physical unread-tail exclusion");
  metadata.availability.status = LoadStatus::Ready;
  request = requestFixture(metadata, "cancel-after-persisted-prefix");
  check(tasks.enqueue(directory.path(), metadata, request, error), error);
  tasks.pump();
  QElapsedTimer timeout;
  timeout.start();
  bool cancelled = false;
  while (timeout.elapsed() < 15000 && !cancelled) {
    for (const auto& run : tasks.snapshots())
      if (run.request.runId == request.runId && run.completedWindows > 0 &&
          !detectionIsTerminal(run.lifecycle))
        cancelled = tasks.cancel(request.runId);
    QThread::msleep(5);
  }
  check(cancelled, "Active task must expose a cancellable persisted prefix");
  const auto stopped = waitForRun(tasks, request.runId);
  check(
      stopped.lifecycle == DetectionLifecycle::Cancelled &&
          !stopped.chunks.empty() &&
          stopped.completedWindows < stopped.eligibleWindows,
      "Cancellation preserves complete chunks and cannot claim full coverage");
  DetectionRun loaded;
  check(loadDetectionRun(directory.path(),
                         QString::fromStdString(request.runId), loaded, error),
        error);
  check(loaded.chunks.size() == stopped.chunks.size() &&
            loaded.completedWindows == stopped.completedWindows,
        "Cancelled prefix must remain durable across reopening");
}
void testTerminalCommitAndSourceInvalidation() {
  QTemporaryDir directory;
  const auto metadata = sourceFixture(directory);
  std::atomic_bool blocked = false, release = false;
  std::atomic_int terminalWrites = 0;
  DetectionTaskManager tasks(
      workerPath(),
      [&](const QString& path, const DetectionRun& run, QString& error) {
        if (run.lifecycle == DetectionLifecycle::Completed &&
            ++terminalWrites == 1) {
          blocked = true;
          QElapsedTimer timeout;
          timeout.start();
          while (!release && timeout.elapsed() < 15000) QThread::msleep(1);
          if (!release) {
            error = "Timed out waiting for terminal persistence";
            return false;
          }
        }
        return writeDetectionRun(path, run, error);
      });
  const auto unblock = qScopeGuard([&] { release = true; });
  const auto request = requestFixture(metadata, "terminal-commit-race");
  QString error;
  check(tasks.enqueue(directory.path(), metadata, request, error), error);
  tasks.pump();
  QElapsedTimer timeout;
  timeout.start();
  while (!blocked && timeout.elapsed() < 15000) QThread::msleep(1);
  check(blocked, "Task did not reach terminal persistence boundary");
  check(!tasks.cancel(request.runId),
        "Cancellation after terminal commit point cannot be accepted");
  tasks.invalidateSource(request.runId, DetectionSourceValidity::Changed);
  tasks.invalidateSource(request.runId, DetectionSourceValidity::Missing);
  release = true;
  const auto terminal = waitForRun(tasks, request.runId);
  check(terminal.lifecycle == DetectionLifecycle::Completed &&
            terminal.sourceValidity == DetectionSourceValidity::Missing &&
            terminalWrites >= 2,
        "Terminal persistence must retain the latest concurrent invalidation");
  DetectionRun loaded;
  check(loadDetectionRun(directory.path(),
                         QString::fromStdString(request.runId), loaded, error),
        error);
  check(encodeDetectionRun(terminal) == encodeDetectionRun(loaded),
        "Visible terminal and durable source validity must be identical");
}
void testFailedTerminalPersistenceKeepsRecoveryPrefix() {
  QTemporaryDir directory;
  const auto metadata = sourceFixture(directory);
  DetectionTaskManager tasks(
      workerPath(),
      [](const QString& path, const DetectionRun& run, QString& error) {
        if (detectionIsTerminal(run.lifecycle)) {
          error = "Injected terminal manifest write failure";
          return false;
        }
        return writeDetectionRun(path, run, error);
      });
  const auto request = requestFixture(metadata, "failed-terminal-persistence");
  QString error;
  check(tasks.enqueue(directory.path(), metadata, request, error), error);
  const auto failed = waitForRun(tasks, request.runId);
  check(failed.lifecycle == DetectionLifecycle::Failed &&
            QString::fromStdString(failed.error).contains("write failure") &&
            !failed.chunks.empty(),
        "Terminal IO failure must expose failure and keep durable chunks");
  DetectionPublication publication;
  check(!tasks.takePublication(request.runId, publication),
        "An unpersisted terminal cannot automatically publish annotations");
  DetectionRun loaded;
  check(loadDetectionRun(directory.path(),
                         QString::fromStdString(request.runId), loaded, error),
        error);
  check(loaded.lifecycle == DetectionLifecycle::Interrupted &&
            loaded.recoverable && loaded.chunks.size() == failed.chunks.size(),
        "Last durable active manifest must retain its recoverable prefix");
  auto queued = requestFixture(metadata, "queued-terminal-io-failure");
  check(tasks.enqueue(directory.path(), metadata, queued, error), error);
  check(tasks.cancel(queued.runId), "Queued cancellation was not accepted");
  const auto cancelled = waitForRun(tasks, queued.runId);
  check(
      cancelled.lifecycle == DetectionLifecycle::Failed &&
          QString::fromStdString(cancelled.error).contains("write failure"),
      "Queued terminal persistence failure cannot claim durable cancellation");
}
void testAccumulatedRecoveryBudget() {
  QTemporaryDir directory;
  auto metadata = sourceFixture(directory);
  {
    QFile file(QString::fromStdString(metadata.path));
    check(file.open(QIODevice::ReadWrite) && file.resize(40000 * 4),
          "Extend budget fixture to seven eligible windows");
  }
  metadata.sampleCount = metadata.availability.availableSamples = 40000;
  metadata.availability.status = LoadStatus::Ready;
  DetectionRun run;
  run.request = requestFixture(metadata, "recovery-result-budget");
  run.request.sourceIdentity.quickFingerprint =
      iqSourceFingerprint(QString::fromStdString(metadata.path)).toStdString();
  run.request.workingSetBudgetBytes = 16 * 1024 * 1024;
  run.lifecycle = DetectionLifecycle::Running;
  run.eligibleWindows = run.completedWindows = 7;
  QJsonArray results, mappings;
  for (int window = 0; window < 7; ++window) {
    QJsonArray candidates;
    for (int index = 0; index < 512; ++index)
      candidates.append(QJsonObject{{"lowerBin", index * 2},
                                    {"upperBin", index * 2 + 1},
                                    {"score", .95}});
    results.append(QJsonObject{{"windowId", QString::number(window)},
                               {"candidates", candidates}});
    mappings.append(
        QJsonObject{{"windowId", QString::number(window)},
                    {"startSample", QString::number(window * 5000)},
                    {"endSample", QString::number(window * 5000 + 10000)},
                    {"frequencyFirstBinHz", 99.5e6},
                    {"frequencyStepHz", 1e6 / 65536}});
  }
  QString error;
  check(appendDetectionChunk(directory.path(), run, results, mappings, error),
        error);
  DetectionPublication target;
  target.sourceSet.id = "unchanged-target";
  const bool recovered =
      recoverDetectionPublication(directory.path(), run, target, error);
  check(!recovered && error.contains("budget") &&
            target.sourceSet.id == "unchanged-target" &&
            target.annotations.empty(),
        "Valid stored candidate accumulation must obey recovery budget "
        "atomically: " +
            error);
}
}  // namespace
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    testTaskRoundtrip();
    testFailuresAndGeneration();
    testPartialAndActiveCancellation();
    testTerminalCommitAndSourceInvalidation();
    testFailedTerminalPersistenceKeepsRecoveryPrefix();
    testAccumulatedRecoveryBudget();
    std::cout << "PASS isolated native worker, bounded task lifecycle, "
                 "provenance and persistence\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
