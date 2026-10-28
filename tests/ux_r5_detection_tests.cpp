// Standalone Qt Core runner, matching detection_store_tests.cpp. Controlled
// durable publications exercise real .ssproj/store IO, not model inference.
// Target: SignalStudioUxR5DetectionTests; link SignalStudioInfrastructure
// Qt6::Core; AUTOMOC OFF; CTest signal_studio_ux_r5_detection, TIMEOUT 120.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <QLockFile>

#include "application/detection_result_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_projection.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/runtime_log.h"
#include "infrastructure/project_file.h"
#include "infrastructure/project_store.h"
#include "infrastructure/source_loader.h"
#include "infrastructure/channel_processor.h"
#include "application/session.h"

using namespace signalstudio;
namespace {
constexpr std::uint64_t generation = 7;
QString q(const std::string& s) { return QString::fromStdString(s); }
void check(bool value, const QString& why) {
  if (!value) throw std::runtime_error(why.toUtf8().toStdString());
}
void write(const QString& path, const QByteArray& bytes) {
  QFile out(path);
  check(out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.flush(),
        "Fixture write failed: " + path);
}
QByteArray read(const QString& path) {
  QFile in(path);
  check(in.open(QIODevice::ReadOnly), "Fixture read failed: " + path);
  const auto bytes = in.readAll();
  check(in.error() == QFileDevice::NoError, "Fixture read error");
  return bytes;
}
QJsonObject object(const QString& path) {
  const auto doc = QJsonDocument::fromJson(read(path));
  check(doc.isObject(), "Fixture JSON is not an object");
  return doc.object();
}
std::size_t algorithmCount(const FileState& file, std::uint32_t channel = 0) {
  return std::count_if(file.marks.begin(), file.marks.end(), [&](const auto& a) {
    const auto* set = findSourceSet(file, a.sourceSetId);
    return a.sourceKind == AnnotationSource::Algorithm && set && set->sampleChannel == channel;
  });
}
QJsonArray nonAlgorithm(const FileState& file) {
  QJsonArray data;
  for (const auto& a : file.marks)
    if (a.sourceKind != AnnotationSource::Algorithm) data.push_back(encodeAnnotation(a));
  return data;
}
FileState& byId(Project& project, const std::string& id) {
  for (auto& file : project.files) if (file.metadata.id == id) return file;
  throw std::runtime_error("Missing fixture file");
}
struct Prepared {
  DetectionRun run;
  DetectionPublication publication;
  DetectionResultTicket ticket;
};
struct Harness {
  QTemporaryDir temp;
  QString path;
  Project project;
  AnnotationService annotations;
  std::uint64_t clock = 100;
  explicit Harness(std::uint32_t channels = 1) {
    check(temp.isValid(), "Temporary directory unavailable");
    path = temp.filePath("result.ssproj");
    addFile("file-a", channels);
    project.activeFileId = "file-a";
    save();
  }
  void addFile(const std::string& id, std::uint32_t channels = 1) {
    const auto source = temp.filePath(q(id) + ".iq");
    QByteArray bytes(32768 * 4 * static_cast<int>(channels), '\0');
    for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i * 13 + 7) & 0xff);
    write(source, bytes);
    FileState file;
    file.metadata = {id, id + ".iq", source.toStdString(), 1e6, 100e6, 32768, false};
    file.metadata.sampleFormat.channels = channels;
    file.metadata.availability = {32768, LoadStatus::Ready, 1,
                                  iqSourceFingerprint(source).toStdString(), {}};
    file.view = fullRange(file.metadata);
    file.currentDetectionSlots.emplace();
    project.files.push_back(std::move(file));
  }
  QString root() const { return q(project.assetDirectory); }
  void save(AssetSaveMode mode = AssetSaveMode::Preserve) {
    QString error;
    check(ProjectFileCodec::save(path, project, error, mode), "Save: " + error);
  }
  Project reopen() {
    Project loaded;
    QString error;
    check(ProjectFileCodec::load(path, loaded, error), "Reopen: " + error);
    return loaded;
  }
  DetectionRequest request(const std::string& id, const std::string& profile,
                           const std::string& fileId = "file-a", std::uint32_t channel = 0,
                           TimeRange time = {0, 32768}) {
    auto metadata = byId(project, fileId).metadata;
    metadata.sampleFormat.selectedChannel = channel;
    DetectionRequest request;
    request.runId = id;
    request.profile = *findDetectionProfile(profile);
    request.profile.fftSize = 2048;
    request.profile.modelFrames = request.profile.modelHopFrames = 1;
    request.parameters.minHits = 1;
    request.parameters.mergeFrequencyHz = 0;
    request.sourceIdentity = annotationSourceIdentity(metadata, generation);
    request.requestedRange = {time, {99.6e6, 100.4e6}};
    request.contextRange = fullRange(metadata);
    request.rangeOrigin = time == TimeRange{0, 32768} ? "whole-file" : "selection";
    return request;
  }
  // Durable, controlled SCN/FFSCN record fixture. No worker or GPU is run.
  Prepared durable(const std::string& id, const std::string& profile = "scn-max-psd-v1",
                    bool zero = false, const std::string& fileId = "file-a",
                    std::uint32_t channel = 0, TimeRange time = {0, 32768},
                    std::uint64_t startedAt = 0) {
    Prepared prepared;
    auto& run = prepared.run;
    run.request = request(id, profile, fileId, channel, time);
    run.lifecycle = DetectionLifecycle::Running;
    run.startedAtMilliseconds = startedAt ? startedAt : ++clock;
    clock = std::max(clock, run.startedAtMilliseconds);
    run.readFrames = run.eligibleWindows = run.completedWindows = 1;
    run.coverage = {{DetectionCoverageKind::Read, run.request.contextRange, "controlled fixture"},
                    {DetectionCoverageKind::Eligible, run.request.requestedRange, ""},
                    {DetectionCoverageKind::Completed, run.request.requestedRange, ""}};
    QString error;
    DetectionPublication empty;
    check(recoverDetectionPublication(root(), run, empty, error), "Empty projection: " + error);
    if (zero) prepared.publication = std::move(empty);
    else {
      FrequencyTrack track;
      track.seedId = "controlled-track";
      track.runId = id;
      track.fileId = fileId;
      track.frequency = track.originalFrequency = track.originalFrequencyEnvelope = {99.95e6, 100.05e6};
      track.nominalSupport = track.actualSupport = time;
      track.scoreMean = track.scoreMax = .9;
      track.hits = track.windowDenominator = 1;
      track.windowCoverage = 1;
      track.windows.push_back({"0", 0, .9, time, time,
                               {{track.frequency, 900, 1100, .9, 250}}});
      check(projectFrequencyTracks(run.request, empty.sourceSet, {track},
          16 * 1024 * 1024, prepared.publication, error), "Projection: " + error);
    }
    auto records = encodeDetectionPublication(prepared.publication);
    records.append(QJsonObject{{"kind", "frequency-publication-end-v1"},
        {"count", QString::number(prepared.publication.annotations.size())}});
    check(appendDetectionChunk(root(), run, records, {}, error), "Chunk: " + error);
    run.lifecycle = DetectionLifecycle::Completed;
    run.computationPhase = "completed";
    run.finishedAtMilliseconds = ++clock;
    check(writeDetectionRun(root(), run, error), "Complete manifest: " + error);
    // Use precisely the same canonical replay as production recovery.
    check(recoverDetectionPublication(root(), run, prepared.publication, error), "Replay: " + error);
    return prepared;
  }
  void pending(Prepared& prepared) {
    Project candidate = project;
    QString error;
    check(DetectionResultService::beginDetection(candidate, prepared.run.request,
        generation, prepared.ticket, error), "Begin: " + error);
    byId(candidate, prepared.ticket.fileId).detectionRuns.push_back(prepared.run);
    check(ProjectFileCodec::save(path, candidate, error), "Save pending: " + error);
    project = std::move(candidate);
  }
  DetectionCommitResult commit(Prepared& prepared, const DetectionTransactionOptions& options = {}) {
    return DetectionResultService::replaceCurrentDetection(project, annotations,
        prepared.ticket, generation, prepared.run, prepared.publication, path, options);
  }
  void publish(Prepared& prepared) {
    pending(prepared);
    const auto result = commit(prepared);
    check(result.committed, "Commit: " + result.error);
  }
};
void uniqueResult(const FileState& file, const std::string& id,
                   std::uint32_t channel = 0, std::size_t count = 1) {
  const auto* slot = currentDetectionSlot(file, channel);
  check(slot && slot->committedRunId == id && !slot->stale && slot->pendingRunId.empty(),
        "Incorrect committed slot");
  check(algorithmCount(file, channel) == count, "Old algorithm objects remain or zero failed to clear");
  std::size_t sets = 0, runs = 0;
  for (const auto& set : file.sourceSets)
    if (set.sourceKind == AnnotationSource::Algorithm && set.sampleChannel == channel) {
      ++sets;
      check(set.runId == id, "Old algorithm source remains indexed");
    }
  for (const auto& run : file.detectionRuns)
    if (run.request.sourceIdentity.selectedSampleChannel == channel) {
      ++runs;
      check(run.request.runId == id, "Old algorithm run remains indexed");
    }
  check(sets == 1 && runs == 1, "Scope does not have one run/source set");
  if (file.metadata.sampleFormat.selectedChannel == channel) {
    std::size_t visible = 0;
    for (const auto& a : file.marks)
      if (a.sourceKind == AnnotationSource::Algorithm && annotationInContext(file, a)) {
        ++visible;
        check(a.originEvidence && a.originEvidence->runId == id,
              "A pending/retired run is visible alongside the committed result");
      }
    check(visible == count, "Successful publication is not the complete visible result");
  }
}
void aBAZeroRoiAndManualUndo() {
  Harness h;
  auto& file = h.project.files[0];
  AnnotationSourceSet labels;
  labels.id = "controlled-label-set"; labels.fileId = file.metadata.id;
  labels.sourceKind = AnnotationSource::ImportedLabel;
  labels.sourceIdentity = annotationSourceIdentity(file.metadata, generation);
  labels.formatId = labels.parserId = "controlled-label-fixture";
  labels.parserVersion = "1";
  labels.originalText = "small controlled original label";
  labels.sourceHash = QCryptographicHash::hash(QByteArray::fromStdString(labels.originalText),
      QCryptographicHash::Sha256).toHex().toStdString();
  Annotation label("controlled-label", "keep original", {{400, 800}, {99.8e6, 99.9e6}});
  label.fileId = file.metadata.id; label.sourceSetId = labels.id;
  label.sourceKind = AnnotationSource::ImportedLabel;
  label.segments.front().confirmed = false;
  label.reviewStatus = ReviewStatus::Pending;
  label.revisions.clear(); label.reviewHistory.clear();
  auto origin = std::make_shared<OriginEvidence>(*label.originEvidence);
  origin->formatId = "generator-txt-v1";
  origin->originalSegments = label.segments;
  label.originEvidence = std::move(origin);
  check(h.annotations.publishSourceSet(file, labels, {label}).ok, "Imported fixture failed");
  check(h.annotations.createManual(file, "keep manual", {{100, 300}, {99.9e6, 100.1e6}}).ok,
        "Manual fixture failed");
  const auto preserved = nonAlgorithm(file);
  auto first = h.durable("A1"); h.publish(first);
  auto reopened = h.reopen(); uniqueResult(reopened.files[0], "A1");
  const auto id = first.publication.annotations[0].id;
  std::vector<std::string> copies;
  check(h.annotations.convertToManual(h.project.files[0], {id}, {}, copies).ok && copies.size() == 1,
        "Manual conversion fixture failed");
  const auto copy = encodeAnnotation(*findAnnotation(h.project.files[0], copies[0]));
  const auto history = h.annotations.historySize(h.project.files[0]);
  auto second = h.durable("B", "ffscn-gui-iq-v1"); h.publish(second);
  uniqueResult(h.project.files[0], "B");
  check(h.annotations.historySize(h.project.files[0]) == history, "Detection reset Manual undo history");
  check(encodeAnnotation(*findAnnotation(h.project.files[0], copies[0])) == copy,
        "Replacement changed Manual conversion snapshot");
  check(h.annotations.undo(h.project.files[0]).ok, "Manual conversion undo failed after replacement");
  uniqueResult(h.project.files[0], "B");
  check(h.annotations.redo(h.project.files[0]).ok, "Manual conversion redo failed after replacement");
  auto third = h.durable("A2"); h.publish(third);
  uniqueResult(h.project.files[0], "A2");
  auto roi = h.durable("ROI", "ffscn-gui-iq-v1", false, "file-a", 0, {8000, 12000}); h.publish(roi);
  uniqueResult(h.project.files[0], "ROI");
  check(h.project.files[0].marks.back().observation->time == TimeRange{8000, 12000}, "ROI was spliced with old results");
  auto zero = h.durable("zero", "scn-max-psd-v1", true); h.publish(zero);
  DetectionGcReport gcReport;
  QString gcError;
  check(collectRetiredDetectionAssets(h.path, gcReport, gcError), gcError);
  check(gcReport.removedRuns == 4 &&
        !QFileInfo::exists(QDir(h.root()).filePath(q(first.run.manifestRelativePath))),
        "Converted Manual fixture did not actually collect all old run stores");
  reopened = h.reopen(); uniqueResult(reopened.files[0], "zero", 0, 0);
  check(findAnnotation(reopened.files[0], copies[0]) &&
        encodeAnnotation(*findAnnotation(reopened.files[0], copies[0])) == copy,
        "Zero replacement broke independent Manual source");
  check(nonAlgorithm(reopened.files[0]).size() == preserved.size() + 1,
        "Replacement removed Manual objects");
  check(encodeAnnotation(*findAnnotation(reopened.files[0], "controlled-label")) == preserved[0].toObject(),
        "Replacement changed ImportedLabel geometry, revision or evidence");
  QString error;
  const auto found = discoverDetectionRuns(h.root(), reopened, error);
  check(error.isEmpty() && found.size() == 1 && found[0].request.runId == "zero",
        "Discovery resurrected old A/B/ROI runs");
}
void failedCancelledLateAndIncomplete() {
  Harness h;
  auto original = h.durable("original"); h.publish(original);
  const auto oldId = original.publication.annotations[0].id;
  const auto oldObject = encodeAnnotation(*findAnnotation(h.project.files[0], oldId));
  const auto oldSource = encodeSourceSet(*findSourceSet(h.project.files[0], original.publication.sourceSet.id));
  const auto verifyOld = [&](const FileState& file) {
    const auto* a = findAnnotation(file, oldId);
    const auto* slot = currentDetectionSlot(file, 0);
    check(slot && slot->committedRunId == "original" && algorithmCount(file) == 1 &&
          a && annotationInContext(file, *a) && annotationSelectable(file, *a) &&
          annotationEligibleForTab(file, *a, AnnotationSource::Algorithm) &&
          annotationOverlayVisible(file, *a), "Last committed result disappeared before a successful replacement");
    const auto* source = findSourceSet(file, a->sourceSetId);
    check(source && encodeAnnotation(*a) == oldObject &&
          encodeSourceSet(*source) == oldSource,
          "A configuration/pending/failed operation changed immutable committed evidence");
  };
  check(h.annotations.select(h.project.files[0], {oldId}, oldId).ok, "Select committed result");
  const auto committedGeneration = currentDetectionSlot(h.project.files[0], 0)->generation;
  check(DetectionResultService::markStale(h.project, "file-a", 0), "Configuration did not become stale");
  h.annotations.reconcileSelection(h.project.files[0]);
  check(h.project.files[0].selectedMarkIds == std::vector<std::string>{oldId} &&
        h.project.files[0].activeMarkId == oldId &&
        currentDetectionSlot(h.project.files[0], 0)->generation == committedGeneration,
        "Marking configuration stale changed selection or a committed-only generation");
  verifyOld(h.project.files[0]);
  h.save(); verifyOld(h.reopen().files[0]);
  auto next = h.durable("next"); h.pending(next);
  verifyOld(h.project.files[0]);
  verifyOld(h.reopen().files[0]);
  // Even if a pending or retired projection reaches a consumer, the slot
  // admits only the committed run; changing stale must not broaden authority.
  auto projection = h.project.files[0];
  projection.sourceSets.push_back(next.publication.sourceSet);
  check(!annotationInContext(projection, next.publication.annotations[0]),
        "Pending publication became eligible before commit");
  projection.currentDetectionSlots->front().retiredRunIds.push_back("retired-result");
  projection.sourceSets.back().runId = "retired-result";
  check(!annotationInContext(projection, next.publication.annotations[0]),
        "Retired publication became eligible through a stale slot");
  const auto disk = read(h.path);
  for (const auto state : {DetectionLifecycle::Running, DetectionLifecycle::Cancelled, DetectionLifecycle::Failed}) {
    auto wrong = next;
    wrong.run.lifecycle = state;
    check(!h.commit(wrong).committed && read(h.path) == disk, "Non-completed run changed the project");
    verifyOld(h.project.files[0]);
  }
  auto wrong = next;
  wrong.run.completedWindows = 0;
  check(!h.commit(wrong).committed, "Incomplete windows were published");
  wrong = next;
  wrong.run.coverage.erase(wrong.run.coverage.begin() + 2);
  check(!h.commit(wrong).committed, "Missing completed coverage was published");
  wrong = next; ++wrong.ticket.slotGeneration;
  check(!h.commit(wrong).committed, "Wrong slot generation was published");
  wrong = next; ++wrong.ticket.projectGeneration;
  check(!h.commit(wrong).committed, "Wrong project generation was published");
  wrong = next; wrong.run.sourceValidity = DetectionSourceValidity::Changed;
  check(!h.commit(wrong).committed, "Changed source was published");
  wrong = next; wrong.publication.annotations.clear();
  check(!h.commit(wrong).committed, "Partial publication differed from durable store but committed");
  verifyOld(h.project.files[0]);
  DetectionResultService::abandonDetection(h.project, next.ticket);
  check(!h.commit(next).committed, "Cancelled pending ticket was accepted late");
  verifyOld(h.project.files[0]);
  h.save();
  const auto loaded = h.reopen();
  const auto* slot = currentDetectionSlot(loaded.files[0], 0);
  check(slot && slot->committedRunId == "original" && slot->stale && algorithmCount(loaded.files[0]) == 1,
        "Cancellation destroyed the last committed result");
  verifyOld(loaded.files[0]);
  QString error;
  const auto runs = discoverDetectionRuns(h.root(), loaded, error);
  check(error.isEmpty() && runs.size() == 1 && runs[0].request.runId == "original", "Cancelled run was recoverable");
  // A parameter commit during a later run must still invalidate its ticket,
  // retain the old visible result and reject a late successful publication.
  auto superseded = h.durable("superseded"); h.pending(superseded);
  const auto pendingGeneration = currentDetectionSlot(h.project.files[0], 0)->generation;
  check(DetectionResultService::markStale(h.project, "file-a", 0), "Pending configuration commit was ignored");
  check(currentDetectionSlot(h.project.files[0], 0)->pendingRunId.empty() &&
        currentDetectionSlot(h.project.files[0], 0)->generation > pendingGeneration &&
        !h.commit(superseded).committed, "Stale configuration accepted a superseded pending ticket");
  verifyOld(h.project.files[0]);
  h.save(); verifyOld(h.reopen().files[0]);
}
void filesAndChannels() {
  Harness h(2); h.addFile("file-b"); h.save();
  auto a0 = h.durable("a0"); h.publish(a0);
  auto a1 = h.durable("a1", "ffscn-gui-iq-v1", false, "file-a", 1); h.publish(a1);
  auto b = h.durable("b", "scn-max-psd-v1", false, "file-b"); h.publish(b);
  const auto other = exportAnnotationsJson(byId(h.project, "file-b"));
  auto next = h.durable("a0-next"); h.publish(next);
  const auto loaded = h.reopen();
  uniqueResult(loaded.files[0], "a0-next"); uniqueResult(loaded.files[0], "a1", 1);
  uniqueResult(loaded.files[1], "b");
  check(exportAnnotationsJson(loaded.files[1]) == other, "Updating file A changed file B");
  auto stale = loaded;
  check(DetectionResultService::markStale(stale, "file-a", 0), "File/channel stale fixture failed");
  auto& file = byId(stale, "file-a");
  check(annotationInContext(file, *findAnnotation(file, next.publication.annotations[0].id)) &&
        !annotationInContext(file, *findAnnotation(file, a1.publication.annotations[0].id)),
        "Stale channel zero hid its result or exposed another IQ channel");
  file.metadata.sampleFormat.selectedChannel = 1;
  check(annotationInContext(file, *findAnnotation(file, a1.publication.annotations[0].id)) &&
        !annotationInContext(file, *findAnnotation(file, next.publication.annotations[0].id)) &&
        !currentDetectionSlot(file, 1)->stale,
        "Changing stale scope altered another channel's authority");
  check(annotationInContext(byId(stale, "file-b"), *findAnnotation(byId(stale, "file-b"), b.publication.annotations[0].id)) &&
        !annotationInContext(file, b.publication.annotations[0]), "Stale scope leaked across files");
}
void faultBoundaries() {
  for (const auto point : {DetectionTransactionPoint::Stage, DetectionTransactionPoint::Persist,
                           DetectionTransactionPoint::Commit, DetectionTransactionPoint::Gc}) {
    Harness h;
    auto a = h.durable("old"); h.publish(a);
    auto b = h.durable("new", "ffscn-gui-iq-v1"); h.pending(b);
    const auto disk = read(h.path);
    const auto before = ProjectStore::stateToken(h.project);
    const auto result = h.commit(b, {[point](auto at) { return at == point; }});
    if (point == DetectionTransactionPoint::Gc) {
      check(result.committed && result.gcDeferred, "GC interruption rolled back a committed project");
      uniqueResult(h.reopen().files[0], "new");
      check(QFileInfo::exists(QDir(h.root()).filePath(q(a.run.manifestRelativePath))), "Injected GC unexpectedly deleted old assets");
    } else {
      check(!result.committed && read(h.path) == disk && ProjectStore::stateToken(h.project) == before,
            "Stage/persist/commit fault modified the old project/session");
      auto loaded = h.reopen();
      const auto* slot = currentDetectionSlot(loaded.files[0], 0);
      check(slot && slot->committedRunId == "old" && slot->pendingRunId == "new" && algorithmCount(loaded.files[0]) == 1,
            "Fault reopening mixed formal results");
      check(annotationInContext(loaded.files[0], *findAnnotation(loaded.files[0], a.publication.annotations[0].id)),
            "Stage/persist/commit failure hid the old committed result after reopen");
      DetectionResultTicket recovery;
      QString error;
      check(DetectionResultService::prepareRecovery(loaded, b.run, generation + 1, recovery, error), error);
      const auto recovered = DetectionResultService::replaceCurrentDetection(loaded, h.annotations, recovery,
          generation + 1, b.run, b.publication, h.path);
      check(recovered.committed, "Crash recovery transaction: " + recovered.error);
      uniqueResult(h.reopen().files[0], "new");
    }
  }
  Harness stale;
  auto previous = stale.durable("previous");
  stale.publish(previous);
  const auto previousId = previous.publication.annotations[0].id;
  auto replacement = stale.durable("replacement");
  stale.pending(replacement);
  const auto disk = read(stale.path);
  const auto memory = ProjectStore::stateToken(stale.project);
  int checks = 0;
  DetectionTransactionOptions staleAtCommit;
  staleAtCommit.stillCurrent = [&] { return ++checks < 2; };
  const auto refused = stale.commit(replacement, staleAtCommit);
  check(!refused.committed && checks >= 2 && refused.error.contains("上次正式结果仍在") &&
            read(stale.path) == disk && ProjectStore::stateToken(stale.project) == memory,
        "Generation change at replacement commit wrote or installed the new result: " +
            refused.error);
  const auto reopened = stale.reopen();
  const auto* slot = currentDetectionSlot(reopened.files[0], 0);
  check(slot && slot->committedRunId == "previous" && slot->pendingRunId == "replacement" &&
            algorithmCount(reopened.files[0]) == 1 &&
            annotationInContext(reopened.files[0], *findAnnotation(reopened.files[0], previousId)),
        "Reopen after a stale replacement commit lost the previous formal result");
}
void runningSaveAndDurableRecovery() {
  Harness h;
  auto a = h.durable("old"); h.publish(a);
  Prepared pending;
  pending.run.request = h.request("running", "scn-max-psd-v1");
  pending.run.lifecycle = DetectionLifecycle::Running;
  pending.run.startedAtMilliseconds = 200;
  QString error;
  check(writeDetectionRun(h.root(), pending.run, error), error);
  h.pending(pending);
  auto loaded = h.reopen();
  check(byId(loaded, "file-a").detectionRuns.back().lifecycle == DetectionLifecycle::Interrupted,
        "Live save did not reopen as interrupted task evidence");
  check(algorithmCount(loaded.files[0]) == 1, "Live save published pending results");
  check(annotationInContext(loaded.files[0], *findAnnotation(loaded.files[0], a.publication.annotations[0].id)),
        "Interrupted pending task hid the last committed result after reopen");
  // Simulate the worker finishing after the live manifest was anchored.
  auto complete = h.durable("running", "scn-max-psd-v1", false, "file-a", 0,
                             {0, 32768}, pending.run.startedAtMilliseconds);
  loaded = h.reopen();
  check(currentDetectionSlot(loaded.files[0], 0)->pendingRunId == "running", "Live anchor lost pending ID");
  DetectionResultTicket recovery;
  check(DetectionResultService::prepareRecovery(loaded, complete.run, 99, recovery, error), error);
  const auto result = DetectionResultService::replaceCurrentDetection(loaded, h.annotations, recovery,
      99, complete.run, complete.publication, h.path);
  check(result.committed, result.error);
  uniqueResult(h.reopen().files[0], "running");
}
void safePrivateGcAndRetiredDiscovery() {
  Harness h;
  auto a = h.durable("old"); h.publish(a);
  const auto external = h.temp.filePath("external-model.txt"); write(external, "never delete");
  const auto runDir = QFileInfo(QDir(h.root()).filePath(q(a.run.manifestRelativePath))).absolutePath();
  const auto scratch = QDir(runDir).filePath("capture.iq"); write(scratch, "user file, not a store asset");
  auto b = h.durable("new"); h.publish(b);
  DetectionGcReport report;
  QString error;
  check(collectRetiredDetectionAssets(h.path, report, error), error);
  check(report.privateRoot && report.removedRuns == 1 && report.removedFiles >= 2,
        "Private GC did not collect unreferenced store manifest/chunk");
  check(read(external) == "never delete" && read(scratch) == "user file, not a store asset",
        "GC removed non-store IQ/TXT/model data");
  uniqueResult(h.reopen().files[0], "new");
  // A physically present corrupt retired manifest must not poison recovery.
  write(QDir(h.root()).filePath(q(a.run.manifestRelativePath)), "corrupt retired run");
  const auto found = discoverDetectionRuns(h.root(), h.project, error);
  check(error.isEmpty() && found.size() == 1 && found[0].request.runId == "new",
        "Authority discovery scanned/revived corrupt retired directories");
}
void copyReferencePreserve() {
  Harness h;
  auto a = h.durable("old"); h.publish(a);
  const auto reference = h.temp.filePath("reference.ssproj");
  auto shared = h.project;
  QString error;
  check(ProjectFileCodec::save(reference, shared, error, AssetSaveMode::Reference), error);
  auto b = h.durable("new"); h.publish(b);
  DetectionGcReport report;
  check(collectRetiredDetectionAssets(h.path, report, error), error);
  check(!report.privateRoot && report.removedFiles == 0,
        "Owner project GC deleted an externally referenced root");
  const auto oldManifest = QDir(h.root()).filePath(q(a.run.manifestRelativePath));
  check(QFileInfo::exists(oldManifest), "Reference old assets were deleted");
  Project referenceLoaded;
  check(ProjectFileCodec::load(reference, referenceLoaded, error), error);
  uniqueResult(referenceLoaded.files[0], "old");
  check(ProjectFileCodec::save(reference, referenceLoaded, error, AssetSaveMode::Preserve), error);
  check(object(reference)["assetMode"] == "reference", "Preserve changed Reference mode");
  check(collectRetiredDetectionAssets(reference, report, error) && report.removedFiles == 0, error);
  auto copied = h.project;
  const auto copy = h.temp.filePath("copy.ssproj");
  check(ProjectFileCodec::save(copy, copied, error, AssetSaveMode::Copy), error);
  check(!QFileInfo::exists(QDir(q(copied.assetDirectory)).filePath(q(a.run.manifestRelativePath))) &&
        QFileInfo::exists(QDir(q(copied.assetDirectory)).filePath(q(b.run.manifestRelativePath))),
        "Copy included unreachable retired runs or omitted current assets");
  check(ProjectFileCodec::load(copy, copied, error), error);
  uniqueResult(copied.files[0], "new");
  check(read(oldManifest).size() > 0 && QFileInfo::exists(q(h.project.files[0].metadata.path)),
        "Save modes removed external source/current Reference assets");
}
void legacyMultiRunAdjudicationAndAmbiguity() {
  Harness h;
  auto a = h.durable("legacy-A");
  auto b = h.durable("legacy-B", "ffscn-gui-iq-v1");
  h.project.files[0].currentDetectionSlots.reset();
  for (auto* prepared : {&a, &b}) {
    check(h.annotations.publishSourceSet(h.project.files[0], prepared->publication.sourceSet,
        prepared->publication.annotations).ok, "Legacy source fixture failed");
    h.project.files[0].detectionRuns.push_back(prepared->run);
  }
  h.save();
  const auto legacyBytes = read(h.path);
  auto loaded = h.reopen();
  uniqueResult(loaded.files[0], "legacy-B");
  check(read(h.path) == legacyBytes, "Legacy opening overwrote original project");
  const auto* slot = currentDetectionSlot(loaded.files[0], 0);
  check(std::find(slot->retiredRunIds.begin(), slot->retiredRunIds.end(), "legacy-A") != slot->retiredRunIds.end(),
        "Legacy loser has no retirement tombstone");
  QString error;
  check(ProjectFileCodec::save(h.path, loaded, error), error);
  uniqueResult(h.reopen().files[0], "legacy-B");
  // Recreate a legacy timestamp tie without changing immutable request/chunks.
  a.run.finishedAtMilliseconds = b.run.finishedAtMilliseconds;
  check(writeDetectionRun(h.root(), a.run, error), error);
  h.project.files[0].currentDetectionSlots.reset();
  h.save();
  const auto tiedBytes = read(h.path);
  Project sentinel; sentinel.name = "unchanged on ambiguity";
  check(!ProjectFileCodec::load(h.path, sentinel, error) && !error.isEmpty() &&
        sentinel.name == "unchanged on ambiguity" && read(h.path) == tiedBytes,
        "Legacy tied runs were guessed or source/target data changed");
}
void retiredChannelSnapshot() {
  Harness h(2);
  auto a = h.durable("old"); h.publish(a);
  const auto& annotation = *findAnnotation(h.project.files[0], a.publication.annotations[0].id);
  const auto annotationId = annotation.id;
  AnnotationChannelSnapshot snapshot;
  snapshot.annotationId = annotation.id;
  snapshot.revisionId = annotation.revisionId;
  snapshot.name = annotation.name;
  snapshot.range = {{100, 900}, annotation.observation->frequency};
  snapshot.boundRange = snapshot.range;
  snapshot.sourceKind = AnnotationSource::Algorithm;
  snapshot.sourceIdentity = a.run.request.sourceIdentity;
  snapshot.bindingKind = ChannelBindingKind::UserRange;
  Channel channel;
  channel.id = "legacy-channel"; channel.name = "retained legacy algorithm channel";
  channel.sourceMarkId = channel.annotationId = snapshot.annotationId;
  channel.revisionId = snapshot.revisionId;
  channel.sourceIdentity = snapshot.sourceIdentity;
  channel.sourceTime = snapshot.range.time;
  channel.centerFrequencyHz = 100e6; channel.bandwidthHz = 100000;
  channel.outputSampleRateHz = 200000;
  channel.processingState = ChannelProcessingState::Ready;
  channel.sourceSnapshot = std::make_shared<const AnnotationChannelSnapshot>(snapshot);
  h.project.files[0].channels.push_back(channel);
  ChannelDspPlan beforePlan;
  ChannelSampleData beforeSamples;
  QString dspError;
  check(makeChannelDspPlan(h.project.files[0].metadata, channel, beforePlan, dspError) &&
        processChannelSamples(h.project.files[0].metadata, channel, beforePlan, {0,16}, beforeSamples), dspError);
  h.save();
  auto b = h.durable("new"); h.publish(b);
  h.project.files[0].metadata.sampleFormat.selectedChannel = 1;
  h.save();
  const auto loaded = h.reopen();
  const auto& retained = loaded.files[0].channels[0];
  check(retained.sourceSnapshot && *retained.sourceSnapshot == snapshot &&
        retained.revisionId == channel.revisionId && retained.configVersion == channel.configVersion &&
        retained.outputSampleRateHz == channel.outputSampleRateHz && retained.sourceTime == channel.sourceTime &&
        retained.sourceIdentity == channel.sourceIdentity && retained.bandwidthHz == channel.bandwidthHz &&
        retained.sourceRelation == ChannelSourceRelation::Retired,
        "Replacement changed legacy channel snapshot, geometry or DSP configuration");
  check(currentDetectionSlot(loaded.files[0], 0)->retiredAnnotationIds == std::vector<std::string>{annotationId},
        "Retirement did not capture exactly the channel's needed annotation ID");
  DetectionGcReport report; QString error;
  check(collectRetiredDetectionAssets(h.path, report, error), error);
  check(h.reopen().files[0].channels[0].sourceRelation == ChannelSourceRelation::Retired,
        "GC broke the self-contained retired channel snapshot");
  auto boundMetadata = loaded.files[0].metadata;
  // DDC reads the channel's explicitly captured IQ sample channel. Changing
  // the wideband selection never rebinds this creation snapshot to channel 1.
  boundMetadata.sampleFormat.selectedChannel = snapshot.sourceIdentity.selectedSampleChannel;
  ChannelDspPlan afterPlan;
  ChannelSampleData afterSamples;
  check(makeChannelDspPlan(boundMetadata, retained, afterPlan, dspError) &&
        processChannelSamples(boundMetadata, retained, afterPlan, {0,16}, afterSamples) &&
        afterSamples.samples == beforeSamples.samples && afterSamples.samples.size() == 16,
        "Retired channel's real CPU DDC changed after replacement/reopen/GC: " + dspError);
  Session session;
  session.replaceProject(loaded);
  auto updated = session.project().files[0].channels[0];
  updated.name = "retired channel remains configurable";
  check(session.updateChannel(updated.id, updated), "Retired snapshot channel configuration was rejected");
  auto invalid = session.project().files[0].channels[0];
  invalid.annotationId = "unrelated-source";
  check(!session.updateChannel(invalid.id, invalid), "Channel update silently rebound its immutable source identity");
}
void saveGcLockCompetition() {
  Harness h;
  auto a = h.durable("old"); h.publish(a);
  auto b = h.durable("new"); h.publish(b);
  const auto disk = read(h.path);
  const auto memory = ProjectStore::stateToken(h.project);
  // A real save thread holds the production root lock at the final atomic
  // publication boundary. GC must fail immediately without deleting files.
  std::promise<void> entered, release;
  auto enteredFuture = entered.get_future();
  auto releaseFuture = release.get_future().share();
  auto candidate = h.project;
  candidate.name = "save committed after lock test";
  QString saveError;
  auto saved = std::async(std::launch::async, [&] {
    return ProjectFileCodec::save(h.path, candidate, saveError,
        AssetSaveMode::Preserve, {}, [&] {
          entered.set_value();
          return releaseFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        });
  });
  check(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "Save never reached its locked commit boundary");
  DetectionGcReport report;
  QString error;
  const bool gc = collectRetiredDetectionAssets(h.path, report, error);
  const bool oldIntact = read(h.path) == disk && report.removedFiles == 0;
  auto contending = h.project;
  const auto contenderBefore = ProjectStore::stateToken(contending);
  const bool secondSave = ProjectFileCodec::save(h.path, contending, error);
  const bool contenderIntact = ProjectStore::stateToken(contending) == contenderBefore;
  release.set_value();
  const bool savedResult = saved.get();
  check(!gc && !secondSave && oldIntact && contenderIntact &&
        ProjectStore::stateToken(h.project) == memory,
        "GC/another save passed the publication lock or changed old state on failure");
  check(savedResult, "Locked save failed: " + saveError);
  check(h.reopen().name == candidate.name, "Save did not publish the complete candidate");
  check(collectRetiredDetectionAssets(h.path, report, error) && report.removedRuns == 1,
        "GC did not resume safely after save released its lock: " + error);
  uniqueResult(h.reopen().files[0], "new");
}
void transactionBudgetRejection() {
  Harness h;
  auto a = h.durable("old"); h.publish(a);
  auto b = h.durable("over-budget"); h.pending(b);
  const auto disk = read(h.path);
  const auto memory = ProjectStore::stateToken(h.project);
  auto wrong = b;
  wrong.run.resultCount = annotationSegmentBudget + 1;
  const auto count = h.commit(wrong);
  check(!count.committed && count.error.contains("budget", Qt::CaseInsensitive),
        "Declared result count exceeded budget without a diagnostic");
  wrong = b;
  wrong.publication.annotations[0].segments.resize(annotationSegmentBudget + 1);
  const auto segments = h.commit(wrong);
  check(!segments.committed && segments.error.contains("segment budget"),
        "100001 segments were not rejected before constructing a candidate");
  wrong = b;
  wrong.run.request.workingSetBudgetBytes = 16 * 1024 * 1024;
  wrong.publication.annotations[0].note.assign(2 * 1024 * 1024, 'x');
  const auto metadata = h.commit(wrong);
  check(!metadata.committed && metadata.error.contains("budget", Qt::CaseInsensitive),
        "Publication metadata exceeded its small resident budget");
  check(read(h.path) == disk && ProjectStore::stateToken(h.project) == memory,
        "Budget rejection changed disk/session official result state");
  const auto reopened = h.reopen();
  check(currentDetectionSlot(reopened.files[0], 0)->committedRunId == "old" &&
        algorithmCount(reopened.files[0]) == 1, "Budget rejection lost last complete formal result");
}
void pendingKeepsFormalResultUntilSuccess() {
  Harness h;
  auto& file = h.project.files[0];
  AnnotationSourceSet labels;
  labels.id = "controlled-label-set";
  labels.fileId = file.metadata.id;
  labels.sourceKind = AnnotationSource::ImportedLabel;
  labels.sourceIdentity = annotationSourceIdentity(file.metadata, generation);
  labels.formatId = labels.parserId = "controlled-label-fixture";
  labels.parserVersion = "1";
  labels.originalText = "small controlled original label";
  labels.sourceHash = QCryptographicHash::hash(
      QByteArray::fromStdString(labels.originalText), QCryptographicHash::Sha256)
      .toHex().toStdString();
  Annotation label("controlled-label", "keep original", {{400, 800}, {99.8e6, 99.9e6}});
  label.fileId = file.metadata.id;
  label.sourceSetId = labels.id;
  label.sourceKind = AnnotationSource::ImportedLabel;
  label.segments.front().confirmed = false;
  label.reviewStatus = ReviewStatus::Pending;
  label.revisions.clear();
  label.reviewHistory.clear();
  auto origin = std::make_shared<OriginEvidence>(*label.originEvidence);
  origin->formatId = "generator-txt-v1";
  origin->originalSegments = label.segments;
  label.originEvidence = std::move(origin);
  check(h.annotations.publishSourceSet(file, labels, {label}).ok, "Imported fixture failed");
  check(h.annotations.createManual(file, "keep manual", {{100, 300}, {99.9e6, 100.1e6}}).ok,
        "Manual fixture failed");
  const auto preserved = nonAlgorithm(file);
  auto original = h.durable("original");
  h.publish(original);
  const auto oldId = original.publication.annotations[0].id;
  check(h.annotations.select(h.project.files[0], {oldId}, oldId).ok, "Select committed result");
  check(DetectionResultService::markStale(h.project, "file-a", 0), "Configuration did not become stale");
  check(algorithmCount(h.project.files[0]) == 1 &&
            h.project.files[0].activeMarkId == oldId &&
            annotationInContext(h.project.files[0], *findAnnotation(h.project.files[0], oldId)),
        "Parameter stale cleared or hid formal results");
  h.save();
  const auto staleDisk = read(h.path);
  const auto staleToken = ProjectStore::stateToken(h.project);
  auto next = h.durable("next");
  Project candidate = h.project;
  QString error;
  check(DetectionResultService::beginDetection(candidate, next.run.request, generation,
                                                next.ticket, error),
        "Begin: " + error);
  byId(candidate, next.ticket.fileId).detectionRuns.push_back(next.run);
  const auto* prepared = currentDetectionSlot(byId(candidate, "file-a"), 0);
  check(prepared && prepared->committedRunId == "original" && prepared->pendingRunId == "next" &&
            algorithmCount(byId(candidate, "file-a")) == 1 &&
            nonAlgorithm(byId(candidate, "file-a")) == preserved,
        "Preparing a pending run cleared the formal result or manual/raw labels");
  int generationChecks = 0;
  bool queuedDuringGenerationChange = false;
  check(!DetectionResultService::publishPendingStart(
            h.project, candidate, [&] { return ++generationChecks < 2; },
            [&](QString&) {
              queuedDuringGenerationChange = true;
              return true;
            },
            h.path, error) &&
            generationChecks >= 2 && !queuedDuringGenerationChange &&
            error.contains("上次正式结果仍在") && read(h.path) == staleDisk &&
            ProjectStore::stateToken(h.project) == staleToken,
        "Generation change during start save wrote or installed a new project: " + error);
  {
    const auto reopened = h.reopen();
    const auto* slot = currentDetectionSlot(reopened.files[0], 0);
    check(slot && slot->committedRunId == "original" && slot->pendingRunId.empty() &&
              algorithmCount(reopened.files[0]) == 1 &&
              annotationInContext(reopened.files[0], *findAnnotation(reopened.files[0], oldId)) &&
              nonAlgorithm(reopened.files[0]) == preserved,
          "Reopen after a generation change lost the previous formal result");
  }
  bool queueAttempted = false;
  check(!DetectionResultService::publishPendingStart(
            h.project, candidate, [] { return true; },
            [&](QString& queueError) {
              queueAttempted = true;
              queueError = "queue full";
              return false;
            },
            h.path, error) &&
            queueAttempted && error.contains("未能启动") && error.contains("上次正式结果仍在") &&
            read(h.path) == staleDisk && ProjectStore::stateToken(h.project) == staleToken,
        "Enqueue failure left a different slot or project file: " + error);
  {
    const auto* slot = currentDetectionSlot(h.project.files[0], 0);
    check(slot && slot->committedRunId == "original" && slot->pendingRunId.empty() &&
              algorithmCount(h.project.files[0]) == 1,
          "Enqueue failure changed the in-memory formal slot");
    const auto reopened = h.reopen();
    const auto* diskSlot = currentDetectionSlot(reopened.files[0], 0);
    check(diskSlot && diskSlot->committedRunId == "original" && diskSlot->pendingRunId.empty() &&
              algorithmCount(reopened.files[0]) == 1 &&
              nonAlgorithm(reopened.files[0]) == preserved,
          "Enqueue failure reopen is not the pre-start formal result");
  }
  check(DetectionResultService::publishPendingStart(
            h.project, candidate, [] { return true; }, [](QString&) { return true; }, h.path, error),
        "Pending start: " + error);
  {
    const auto* slot = currentDetectionSlot(h.project.files[0], 0);
    check(slot && slot->committedRunId == "original" && slot->pendingRunId == "next" &&
              algorithmCount(h.project.files[0]) == 1 &&
              annotationInContext(h.project.files[0], *findAnnotation(h.project.files[0], oldId)) &&
              h.project.files[0].activeMarkId == oldId &&
              nonAlgorithm(h.project.files[0]) == preserved,
          "Pending start removed the formal result, selection, or manual/raw labels");
    const auto reopened = h.reopen();
    const auto* diskSlot = currentDetectionSlot(reopened.files[0], 0);
    check(diskSlot && diskSlot->committedRunId == "original" && diskSlot->pendingRunId == "next" &&
              algorithmCount(reopened.files[0]) == 1 &&
              annotationInContext(reopened.files[0], *findAnnotation(reopened.files[0], oldId)),
          "Reopen during pending lost the formal result");
  }
  DetectionResultService::abandonDetection(h.project, next.ticket);
  {
    const auto* slot = currentDetectionSlot(h.project.files[0], 0);
    check(slot && slot->committedRunId == "original" && slot->pendingRunId.empty() &&
              algorithmCount(h.project.files[0]) == 1 &&
              annotationInContext(h.project.files[0], *findAnnotation(h.project.files[0], oldId)) &&
              nonAlgorithm(h.project.files[0]) == preserved,
          "Abandon changed the committed id or formal marks");
    h.save();
    const auto reopened = h.reopen();
    const auto* diskSlot = currentDetectionSlot(reopened.files[0], 0);
    check(diskSlot && diskSlot->committedRunId == "original" && diskSlot->pendingRunId.empty() &&
              algorithmCount(reopened.files[0]) == 1,
          "Reopen after abandon lost the committed result");
  }
  auto zero = h.durable("zero", "scn-max-psd-v1", true);
  h.publish(zero);
  check(algorithmCount(h.project.files[0]) == 0 && nonAlgorithm(h.project.files[0]) == preserved,
        "A successful zero result did not replace the formal set or changed manual/raw labels");
  uniqueResult(h.reopen().files[0], "zero", 0, 0);
  DetectionPreviewSnapshot raw;
  raw.runId = "preview-run";
  raw.dropped = 4;
  raw.events.push_back({});
  raw.events.back().trackId = "preview-track";
  raw.events.back().sequence = 1;
  raw.events.push_back(raw.events.back());
  raw.events.back().sequence = 2;
  const auto shown = presentDetectionPreview(raw, false, true);
  check(shown.visible.events.size() == 2 && shown.visible.dropped == 4 &&
            shown.hiddenTrackIds.empty(),
        "In-progress preview was hidden while the option was on");
  const auto hidden = presentDetectionPreview(raw, false, false);
  check(hidden.visible.events.empty() && hidden.visible.dropped == 0 &&
            hidden.hiddenTrackIds.size() == 1 &&
            hidden.hiddenTrackIds.front() == "preview-track",
        "Unchecked intermediate option still exposed a dashed preview");
  for (const auto state : {DetectionLifecycle::Completed, DetectionLifecycle::Failed,
                           DetectionLifecycle::Cancelled, DetectionLifecycle::Interrupted}) {
    check(detectionIsTerminal(state), "Terminal detection state was not recognized");
    const auto done = presentDetectionPreview(raw, true, true);
    check(done.visible.events.empty() && done.visible.dropped == 0 && done.visible.sealed &&
              done.hiddenTrackIds.size() == 1,
          "Terminal state kept a dashed preview");
  }
  check(!detectionIsTerminal(DetectionLifecycle::Running) &&
            !detectionIsTerminal(DetectionLifecycle::Cancelling),
        "A running detection was treated as terminal");
  check(nonAlgorithm(h.project.files[0]) == preserved,
        "Preview presentation wrote into file marks");
}
const DetectionRun* indexedRun(const FileState& file, const std::string& id) {
  for (const auto& run : file.detectionRuns)
    if (run.request.runId == id) return &run;
  return nullptr;
}
bool retiredRun(const CurrentDetectionSlot& slot, const std::string& id) {
  return std::find(slot.retiredRunIds.begin(), slot.retiredRunIds.end(), id) !=
         slot.retiredRunIds.end();
}
void unpublishedTerminalStaysIndexed() {
  for (const auto state : {DetectionLifecycle::Cancelled, DetectionLifecycle::Failed,
                           DetectionLifecycle::Interrupted}) {
    const auto label = QString("lifecycle %1").arg(static_cast<int>(state));
    Harness h;
    auto& file = h.project.files[0];
    AnnotationSourceSet labels;
    labels.id = "controlled-label-set";
    labels.fileId = file.metadata.id;
    labels.sourceKind = AnnotationSource::ImportedLabel;
    labels.sourceIdentity = annotationSourceIdentity(file.metadata, generation);
    labels.formatId = labels.parserId = "controlled-label-fixture";
    labels.parserVersion = "1";
    labels.originalText = "small controlled original label";
    labels.sourceHash = QCryptographicHash::hash(
        QByteArray::fromStdString(labels.originalText), QCryptographicHash::Sha256)
        .toHex().toStdString();
    Annotation labelMark("controlled-label", "keep original", {{400, 800}, {99.8e6, 99.9e6}});
    labelMark.fileId = file.metadata.id;
    labelMark.sourceSetId = labels.id;
    labelMark.sourceKind = AnnotationSource::ImportedLabel;
    labelMark.segments.front().confirmed = false;
    labelMark.reviewStatus = ReviewStatus::Pending;
    labelMark.revisions.clear();
    labelMark.reviewHistory.clear();
    auto origin = std::make_shared<OriginEvidence>(*labelMark.originEvidence);
    origin->formatId = "generator-txt-v1";
    origin->originalSegments = labelMark.segments;
    labelMark.originEvidence = std::move(origin);
    check(h.annotations.publishSourceSet(file, labels, {labelMark}).ok,
          label + " imported fixture failed");
    check(h.annotations.createManual(file, "keep manual", {{100, 300}, {99.9e6, 100.1e6}}).ok,
          label + " manual fixture failed");
    auto original = h.durable("original");
    h.publish(original);
    const auto preserved = nonAlgorithm(h.project.files[0]);
    const auto formalId = original.publication.annotations[0].id;
    const auto formalObject = encodeAnnotation(*findAnnotation(h.project.files[0], formalId));
    Prepared stopped;
    stopped.run.request = h.request("stopped", "scn-max-psd-v1");
    stopped.run.lifecycle = DetectionLifecycle::Running;
    stopped.run.startedAtMilliseconds = 200;
    QString error;
    check(writeDetectionRun(h.root(), stopped.run, error), label + " running manifest: " + error);
    h.pending(stopped);
    stopped.run.lifecycle = state;
    stopped.run.finishedAtMilliseconds = 250;
    stopped.run.error = "controlled unpublished terminal";
    check(writeDetectionRun(h.root(), stopped.run, error), label + " terminal manifest: " + error);
    DetectionResultService::retainUnpublishedDetection(h.project, stopped.ticket, stopped.run);
    {
      const auto& current = h.project.files[0];
      const auto* slot = currentDetectionSlot(current, 0);
      const auto* stoppedRun = indexedRun(current, "stopped");
      const auto valid = validateCurrentDetectionSlots(current);
      check(valid.ok && slot && slot->committedRunId == "original" && slot->pendingRunId.empty() &&
                slot->stale && stoppedRun && stoppedRun->lifecycle == state &&
                !retiredRun(*slot, "stopped") && !retiredRun(*slot, "original") &&
                indexedRun(current, "original") && algorithmCount(current) == 1 &&
                encodeAnnotation(*findAnnotation(current, formalId)) == formalObject &&
                nonAlgorithm(current) == preserved,
            label + " terminal task retired the index or replaced the formal result: " +
                QString::fromStdString(valid.error));
      check(!h.commit(stopped).committed, label + " unpublished terminal replaced the formal result");
      check(currentDetectionSlot(h.project.files[0], 0)->committedRunId == "original",
            label + " rejected publication changed the committed id");
      check(ProjectFileCodec::save(h.path, h.project, error) &&
                !error.contains("Duplicate or retired detection run remains indexed"),
            label + " save after cancel/fail/interrupt: " + error);
      const auto loaded = h.reopen();
      const auto* loadedSlot = currentDetectionSlot(loaded.files[0], 0);
      const auto* loadedStopped = indexedRun(loaded.files[0], "stopped");
      check(loadedSlot && loadedSlot->committedRunId == "original" &&
                loadedSlot->pendingRunId.empty() && loadedStopped &&
                loadedStopped->lifecycle == state && !retiredRun(*loadedSlot, "stopped") &&
                validateCurrentDetectionSlots(loaded.files[0]).ok &&
                nonAlgorithm(loaded.files[0]) == preserved,
            label + " reopened terminal task was retired or lost the formal id");
    }
    Prepared again;
    again.run.request = h.request("again", "ffscn-gui-iq-v1");
    again.run.lifecycle = DetectionLifecycle::Running;
    again.run.startedAtMilliseconds = 300;
    check(writeDetectionRun(h.root(), again.run, error), label + " restart manifest: " + error);
    h.pending(again);
    {
      const auto& current = h.project.files[0];
      const auto* slot = currentDetectionSlot(current, 0);
      const auto* stoppedRun = indexedRun(current, "stopped");
      const auto* pendingRun = indexedRun(current, "again");
      const auto valid = validateCurrentDetectionSlots(current);
      check(valid.ok && slot && slot->committedRunId == "original" && slot->pendingRunId == "again" &&
                stoppedRun && stoppedRun->lifecycle == state && pendingRun &&
                pendingRun->lifecycle == DetectionLifecycle::Running &&
                !retiredRun(*slot, "stopped") && !retiredRun(*slot, "again") &&
                !retiredRun(*slot, "original") && algorithmCount(current) == 1 &&
                encodeAnnotation(*findAnnotation(current, formalId)) == formalObject &&
                nonAlgorithm(current) == preserved,
            label + " restart left an illegal detection index: " +
                QString::fromStdString(valid.error));
      check(ProjectFileCodec::save(h.path, h.project, error) &&
                !error.contains("Duplicate or retired detection run remains indexed"),
            label + " save after restart: " + error);
      const auto loaded = h.reopen();
      const auto* loadedSlot = currentDetectionSlot(loaded.files[0], 0);
      check(loadedSlot && loadedSlot->committedRunId == "original" &&
                loadedSlot->pendingRunId == "again" &&
                indexedRun(loaded.files[0], "stopped") &&
                indexedRun(loaded.files[0], "stopped")->lifecycle == state &&
                indexedRun(loaded.files[0], "again") &&
                indexedRun(loaded.files[0], "original") &&
                indexedRun(loaded.files[0], "original")->lifecycle == DetectionLifecycle::Completed &&
                !retiredRun(*loadedSlot, "stopped") &&
                validateCurrentDetectionSlots(loaded.files[0]).ok &&
                encodeAnnotation(*findAnnotation(loaded.files[0], formalId)) == formalObject &&
                nonAlgorithm(loaded.files[0]) == preserved,
            label + " reopened restart retired the cancelled run or dropped the formal result");
    }
  }
}
void latestSequencePerTrackWithoutUi() {
  std::vector<PreviewTrackEvent> events(4);
  events[0].trackId = "a";
  events[0].sequence = 1;
  events[0].frequency = {1, 2};
  events[0].observedSupport = {0, 10};
  events[1].trackId = "b";
  events[1].sequence = 4;
  events[1].frequency = {3, 4};
  events[1].observedSupport = {10, 20};
  events[2].trackId = "a";
  events[2].sequence = 5;
  events[2].frequency = {5, 6};
  events[2].observedSupport = {0, 30};
  events[3].trackId = "a";
  events[3].sequence = 2;
  events[3].frequency = {9, 9};
  events[3].observedSupport = {0, 1};
  const auto latest = latestPreviewEvents(events);
  check(latest.size() == 2, "Each trackId keeps one preview event");
  check(latest[0].trackId == "a" && latest[0].sequence == 5 &&
            latest[0].frequency == FrequencyRange{5, 6} &&
            latest[0].observedSupport == TimeRange{0, 30},
        "A track separated by other events must keep its greatest sequence");
  check(latest[1].trackId == "b" && latest[1].sequence == 4 &&
            latest[1].frequency == FrequencyRange{3, 4},
        "An intervening track must remain when the latest geometry is chosen");
}
void unpublishedPreviewFormalDisplayAndRuntimeLog() {
  DetectionPreviewSnapshot raw;
  raw.runId = "preview-run";
  raw.dropped = 2;
  raw.events.push_back({});
  raw.events.back().trackId = "preview-track";
  raw.events.back().sequence = 3;
  for (const auto state : {DetectionLifecycle::Failed, DetectionLifecycle::Cancelled,
                           DetectionLifecycle::Interrupted}) {
    check(detectionIsTerminal(state), "Terminal state");
    const auto kept = presentDetectionPreview(raw, true, true, true);
    check(!kept.visible.events.empty() && kept.visible.events.front().trackId == "preview-track" &&
              kept.visible.unpublished && kept.visible.sealed && kept.hiddenTrackIds.empty() &&
              kept.visible.dropped == 2,
          "Failure, cancellation, and interruption keep the unpublished preview");
  }
  const auto published = presentDetectionPreview(raw, true, true, false);
  check(published.visible.events.empty() && published.visible.sealed &&
            !published.visible.unpublished && published.hiddenTrackIds.size() == 1,
        "Successful completion still clears the dashed preview");
  FileState file;
  file.metadata.id = "file";
  file.metadata.sampleFormat.selectedChannel = 2;
  file.suppressCommittedDetectionChannel = 2;
  Annotation algorithm("algo", "detect", {{1, 2}, {3, 4}});
  algorithm.sourceKind = AnnotationSource::Algorithm;
  algorithm.fileId = file.metadata.id;
  check(!annotationEligibleForTab(file, algorithm, AnnotationSource::Algorithm) &&
            annotationVisible(file, algorithm),
        "Point detection hides the result table without changing annotationVisible");
  Annotation manual("manual", "keep", {{1, 2}, {3, 4}});
  manual.sourceKind = AnnotationSource::Manual;
  manual.fileId = file.metadata.id;
  check(annotationEligibleForTab(file, manual, AnnotationSource::Manual),
        "Manual marks stay eligible while detection results are hidden");
  file.metadata.sampleFormat.selectedChannel = 1;
  check(annotationEligibleForTab(file, algorithm, AnnotationSource::Algorithm),
        "Another channel keeps its formal detection rows");
  check(detectionPrefixRecoveryWouldRepeatFusion(
            "frequency-fusion-comparison-limit-exceeded open=3 sealed=1 comparisons=4") &&
            !detectionPrefixRecoveryWouldRepeatFusion("source changed"),
        "A failed fusion must not be projected again");
  QTemporaryDir dir;
  check(dir.isValid(), "log directory");
  const auto path = RuntimeLog::install(dir.filePath("logs/signal-studio.log"), 480);
  const auto first = RuntimeLog::recentLines(4);
  check(!first.isEmpty() && first.front().contains("path=") && first.front().contains(path),
        "The runtime log starts with its path: " + first.join(" | "));
  RuntimeLog::write("detection", "run=r phase=preflight windows=0/0 batch=0 event=preflight-rejected error=rejected open=0 sealed=0 comparisons=0 action=none");
  const QString filler(400, QLatin1Char('x'));
  for (int i = 0; i < 8; ++i) RuntimeLog::write("detection", filler);
  check(QFileInfo::exists(path + ".1") && QFileInfo::exists(path + ".3") &&
            !QFileInfo::exists(path + ".4"),
        "The log rotates at the threshold and retains three copies");
  const auto rotated = RuntimeLog::recentLines(8);
  check(!rotated.isEmpty() && rotated.front().contains("path="),
        "Each rotated file records its path");
  RuntimeLog::install(path, 8 * 1024 * 1024);
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index)
    threads.emplace_back([&] {
      for (int line = 0; line < 20; ++line) RuntimeLog::write("detection", "thread-line");
    });
  for (auto& thread : threads) thread.join();
  QFile written(path);
  check(written.open(QIODevice::ReadOnly), "reopen log");
  const auto text = QString::fromUtf8(written.readAll());
  int copies = text.count("thread-line");
  for (const auto suffix : {".1", ".2", ".3"}) {
    QFile older(path + suffix);
    if (older.open(QIODevice::ReadOnly))
      copies += QString::fromUtf8(older.readAll()).count("thread-line");
  }
  check(copies == 80, "Concurrent log writes are all retained");
  RuntimeLog::uninstall();
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const std::vector<std::pair<const char*, std::function<void()>>> cases{
      {"A-B-A-zero-ROI-manual-undo", aBAZeroRoiAndManualUndo},
      {"cancel-fail-late-coverage", failedCancelledLateAndIncomplete},
      {"file-and-sample-channel-isolation", filesAndChannels},
      {"real-ssproj-stage-persist-commit-GC-faults", faultBoundaries},
      {"running-save-reopen-recovery", runningSaveAndDurableRecovery},
      {"private-canonical-GC-and-authority-discovery", safePrivateGcAndRetiredDiscovery},
      {"Copy-Reference-Preserve", copyReferencePreserve},
      {"legacy-multi-run-and-ambiguous-timestamps", legacyMultiRunAdjudicationAndAmbiguity},
      {"retired-channel-snapshot-and-GC", retiredChannelSnapshot},
      {"save-GC-real-lock-competition", saveGcLockCompetition},
      {"transaction-metadata-count-segment-budgets", transactionBudgetRejection},
      {"pending-keeps-formal-until-success", pendingKeepsFormalResultUntilSuccess},
      {"unpublished-terminal-stays-indexed", unpublishedTerminalStaysIndexed},
      {"latest-sequence-per-track", latestSequencePerTrackWithoutUi},
      {"unpublished-preview-display-and-log", unpublishedPreviewFormalDisplayAndRuntimeLog}};
  int failed = 0;
  for (const auto& [name, test] : cases) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
  }
  std::cout << cases.size() - failed << '/' << cases.size() << " UX-R5 detection lifecycle cases passed\n";
  return failed ? 1 : 0;
}
