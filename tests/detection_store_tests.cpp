#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "infrastructure/detection_store.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace signalstudio;

namespace {
void check(bool condition, const QString& message) {
  if (!condition) throw std::runtime_error(message.toUtf8().toStdString());
}
QByteArray readFile(const QString& path) {
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), file.errorString());
  return file.readAll();
}
void writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), file.errorString());
  check(file.write(bytes) == bytes.size(), file.errorString());
}
QString hash(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
DetectionRun fixture(const QString& directory,
                     const std::string& id = "run-1") {
  DetectionRun run;
  run.request.runId = id;
  run.request.profile = *findDetectionProfile("pipeline-fixture-v1");
  auto& source = run.request.sourceIdentity;
  source.projectGeneration = std::numeric_limits<std::uint64_t>::max();
  source.fileId = "file-1";
  source.path = QDir(directory).filePath("capture.iq").toStdString();
  source.quickFingerprint = "16000000:2026-10-13T04:00:00Z";
  source.sampleRateHz = 20e6;
  source.centerFrequencyHz = 2.5e9;
  source.effectiveFrequency = {2.49e9, 2.51e9};
  source.physicalSamples = 400000;
  source.availableSamples = 400000;
  run.request.requestedRange = {{100, 300000}, {2.495e9, 2.505e9}};
  run.request.contextRange = {{0, 400000}, source.effectiveFrequency};
  run.request.rangeOrigin = "selection";
  run.lifecycle = DetectionLifecycle::Running;
  run.eligibleWindows = 3;
  run.completedWindows = 1;
  run.readFrames = 10;
  run.coverage.push_back({DetectionCoverageKind::Completed,
                          {{100, 100000}, {2.495e9, 2.505e9}},
                          ""});
  return run;
}
QJsonArray results() {
  return {QJsonObject{{"windowId", "0"},
                      {"candidates", QJsonArray{QJsonObject{{"lowerBin", 100},
                                                            {"upperBin", 200},
                                                            {"score", .8}}}}}};
}
QJsonArray mappings() {
  return {QJsonObject{
      {"windowId", "0"},
      {"firstFrameIndex", "0"},
      {"startSample", "100"},
      {"endSample", "100000"},
      {"contextStartSample", "0"},
      {"contextEndSample", "160000"},
      {"frequencyFirstBinHz", 2.49e9},
      {"frequencyStepHz", 20e6 / 65536},
      {"tensorHash", QString(64, 'a')},
      {"normalizationMeanDb", -90},
      {"normalizationStdDb", 2},
      {"epsilonGuardApplied", false},
      {"frames", QJsonArray{QJsonObject{{"frameIndex", "0"},
                                        {"startSample", "0"},
                                        {"endSample", "65536"},
                                        {"validSamples", "65536"},
                                        {"paddedSamples", "0"},
                                        {"inputHash", QString(64, 'b')}}}}}};
}
void roundtripAndRecovery() {
  QTemporaryDir directory;
  auto run = fixture(directory.path());
  QString error;
  DetectionRequest request;
  check(decodeDetectionRequest(encodeDetectionRequest(run.request), request,
                               error),
        error);
  check(
      request.sourceIdentity == run.request.sourceIdentity &&
          request.parameters == run.request.parameters &&
          request.contextRange == run.request.contextRange,
      "Source identity, uint64 generation, parameters and ROI must roundtrip");
  auto oldManifest = encodeDetectionRun(run);
  oldManifest["version"] = 1;
  oldManifest.remove("computationPhase");
  oldManifest.remove("refinedCandidates");
  oldManifest.remove("totalRefinementCandidates");
  DetectionRun oldRun;
  check(decodeDetectionRun(oldManifest, oldRun, error) &&
            oldRun.totalRefinementCandidates == 0 &&
            oldRun.computationPhase == "none",
        "Version 1 run history must remain readable without invented progress");
  auto progress = encodeDetectionRun(run);
  progress["computationPhase"] = "refine";
  progress["totalRefinementCandidates"] = "3";
  progress["refinedCandidates"] = "2";
  check(decodeDetectionRun(progress, oldRun, error) &&
            oldRun.refinedCandidates == 2,
        "Version 2 must preserve actual refinement progress");
  progress["refinedCandidates"] = "4";
  check(!decodeDetectionRun(progress, oldRun, error),
        "Refinement progress cannot exceed its frozen candidate total");
  writeFile(directory.filePath("project.json"), "untouched project index");
  check(writeDetectionRun(directory.path(), run, error), error);
  check(
      appendDetectionChunk(directory.path(), run, results(), mappings(), error),
      error);
  check(run.chunks.size() == 1 && run.resultCount == 1 &&
            QString::fromStdString(run.manifestRelativePath)
                .startsWith("detections/"),
        "Append must publish a content-addressed reference and count window "
        "DTOs");
  const auto chunkPath =
      directory.filePath(QString::fromStdString(run.chunks[0].relativePath));
  const auto originalChunk = readFile(chunkPath);
  check(hash(originalChunk).toStdString() == run.chunks[0].sha256,
        "Immutable chunk identity must match its actual bytes");
  DetectionRun recovered;
  check(loadDetectionRun(directory.path(), "run-1", recovered, error), error);
  check(recovered.lifecycle == DetectionLifecycle::Interrupted &&
            recovered.recoverable && recovered.resultCount == 1 &&
            recovered.coverage.size() == 1,
        "An active disk snapshot must recover as Interrupted with retained "
        "coverage");
  check(
      readFile(directory.filePath("project.json")) == "untouched project index",
      "Run publication and recovery must never save the project index");
  QJsonArray chunks;
  check(readDetectionChunks(directory.path(), recovered, chunks, error), error);
  check(
      chunks.size() == 1 &&
          chunks[0].toObject()["results"].toArray() == results() &&
          chunks[0].toObject()["mappings"].toArray() == mappings(),
      "Explicit recovery must return validated results and physical mappings");
  const auto discovered = discoverDetectionRuns(directory.path(), error);
  check(error.isEmpty() && discovered.size() == 1 && discovered[0].recoverable,
        "Discovery must expose only published run manifests");
  run.lifecycle = DetectionLifecycle::Cancelled;
  run.recoverable = true;
  check(writeDetectionRun(directory.path(), run, error), error);
  check(loadDetectionRun(directory.path(), "run-1", recovered, error) &&
            recovered.lifecycle == DetectionLifecycle::Cancelled,
        error);
  check(!appendDetectionChunk(directory.path(), run, results(), {}, error),
        "Terminal snapshots cannot be appended as if execution had resumed");
  check(readFile(chunkPath) == originalChunk,
        "Terminal updates must not overwrite immutable chunks");
}
void failedReadsAndAtomicPublication() {
  QTemporaryDir directory;
  auto run = fixture(directory.path());
  QString error;
  check(
      appendDetectionChunk(directory.path(), run, results(), mappings(), error),
      error);
  const auto manifestPath =
      directory.filePath(QString::fromStdString(run.manifestRelativePath));
  const auto manifest = readFile(manifestPath);
  const auto path =
      directory.filePath(QString::fromStdString(run.chunks[0].relativePath));
  const auto bytes = readFile(path);
  writeFile(path, bytes + " ");
  DetectionRun sentinel = fixture(directory.path(), "sentinel");
  check(
      !loadDetectionRun(directory.path(), "run-1", sentinel, error) &&
          sentinel.request.runId == "sentinel",
      "A corrupt hash must reject the whole read without mutating its target");
  QJsonArray chunkSentinel{QJsonObject{{"sentinel", true}}};
  check(!readDetectionChunks(directory.path(), run, chunkSentinel, error) &&
            chunkSentinel.size() == 1 &&
            chunkSentinel[0].toObject()["sentinel"].toBool(),
        "A failed explicit read must preserve the caller's prior result array");
  check(!appendDetectionChunk(directory.path(), run, results(), {}, error) &&
            run.chunks.size() == 1 && readFile(manifestPath) == manifest,
        "A damaged old shard must prevent manifest replacement");
  writeFile(path, bytes);
  auto wrongSource = run;
  wrongSource.request.sourceIdentity.quickFingerprint = "source-changed";
  check(!writeDetectionRun(directory.path(), wrongSource, error) &&
            readFile(manifestPath) == manifest,
        "A shard cannot be republished under a changed source identity");
  auto dropped = run;
  dropped.chunks.clear();
  dropped.resultCount = 0;
  check(!writeDetectionRun(directory.path(), dropped, error) &&
            readFile(manifestPath) == manifest,
        "A stale snapshot cannot discard already published result chunks");
  auto object = encodeDetectionRun(run);
  auto refs = object["chunks"].toArray();
  auto ref = refs[0].toObject();
  for (const auto& invalid :
       {QString("../outside.json"), QString("/outside.json"),
        QString("\\outside.json"), QString("C:/outside.json"),
        QString("detections/x/chunk.json:stream")}) {
    ref["relativePath"] = invalid;
    refs[0] = ref;
    object["chunks"] = refs;
    check(!decodeDetectionRun(object, sentinel, error),
          "Chunk path escape must be rejected");
  }
  writeFile(manifestPath, manifest.left(manifest.size() / 2));
  check(!loadDetectionRun(directory.path(), "run-1", sentinel, error),
        "A truncated crash manifest must not yield partially recovered state");
}

void failedManifestReplacementPreservesDataAndCause() {
#ifdef Q_OS_WIN
  QTemporaryDir directory;
  auto run = fixture(directory.path(), "locked-manifest");
  QString error;
  check(writeDetectionRun(directory.path(), run, error), error);
  const auto path = directory.filePath(
      encodeDetectionRun(run)["manifestRelativePath"].toString());
  const auto original = readFile(path);
  // Permit validation reads, but deny replacement of this test-owned file.
  const auto handle = CreateFileW(
      reinterpret_cast<const wchar_t*>(path.utf16()), GENERIC_READ,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(handle != INVALID_HANDLE_VALUE,
        "Cannot establish the test-owned manifest replacement lock");
  const bool published = writeDetectionRun(directory.path(), run, error);
  CloseHandle(handle);
  check(!published && readFile(path) == original,
        "A denied atomic replacement must preserve the previous manifest");
  check(error.contains("commit") && error.contains("manifest.json") &&
            !error.contains("Unknown error"),
        "Failed replacement must retain its operation, path and Qt cause: " +
            error);
  check(writeDetectionRun(directory.path(), run, error),
        "The unchanged snapshot must publish after releasing the test lock");
#endif
}
void budgetsAndMalformedRequests() {
  QTemporaryDir directory;
  auto run = fixture(directory.path());
  QString error;
  QJsonArray atLimit;
  for (int i = 0; i < 4096; ++i)
    atLimit.append(QJsonObject{{"windowId", QString::number(i)},
                               {"candidates", QJsonArray{}}});
  check(appendDetectionChunk(directory.path(), run, atLimit, {}, error), error);
  auto overLimit = atLimit;
  overLimit.append(
      QJsonObject{{"windowId", "4096"}, {"candidates", QJsonArray{}}});
  check(!appendDetectionChunk(directory.path(), run, overLimit, {}, error) &&
            run.resultCount == 4096,
        "4097 records must fail without changing the run");
  check(!appendDetectionChunk(
            directory.path(), run,
            {QJsonObject{{"description", QString(8 * 1024 * 1024, 'x')}}}, {},
            error),
        "A chunk including its envelope cannot exceed 8 MiB");
  auto object = encodeDetectionRequest(run.request);
  DetectionRequest sentinel;
  sentinel.runId = "sentinel";
  object["workingSetBudgetBytes"] = "18446744073709551616";
  check(!decodeDetectionRequest(object, sentinel, error) &&
            sentinel.runId == "sentinel",
        "Budget overflow must not lose uint64 precision or mutate the target");
  object = encodeDetectionRequest(run.request);
  object["evidenceBudgetBytes"] = "268435457";
  check(!decodeDetectionRequest(object, sentinel, error),
        "Evidence budget exceeds the configured limit");
  object = encodeDetectionRequest(run.request);
  auto source = object["sourceIdentity"].toObject();
  source["availableSamples"] = "400001";
  object["sourceIdentity"] = source;
  check(!decodeDetectionRequest(object, sentinel, error),
        "Available prefix must be physically possible");
  object = encodeDetectionRequest(run.request);
  auto context = object["contextRange"].toObject();
  context["frequencyLowerHz"] = 2.495e9;
  context["frequencyUpperHz"] = 2.505e9;
  object["contextRange"] = context;
  check(
      !decodeDetectionRequest(object, sentinel, error),
      "A full-band tensor context cannot claim only the report frequency ROI");
  object = encodeDetectionRequest(run.request);
  auto profile = object["profile"].toObject();
  profile["hopSeconds"] = .002;
  object["profile"] = profile;
  check(!decodeDetectionRequest(object, sentinel, error),
        "Changing preprocessing requires a new profile identity");
  auto mapping = mappings()[0].toObject();
  mapping["endSample"] = "400001";
  check(
      !appendDetectionChunk(directory.path(), run, results(), {mapping}, error),
      "Mappings cannot claim support beyond the read prefix");
  mapping = mappings()[0].toObject();
  mapping["windowId"] = "0\n";
  check(
      !appendDetectionChunk(directory.path(), run, results(), {mapping}, error),
      "Mapping window indices are canonical uint64 strings");
  run.request.evidenceBudgetBytes = 1;
  check(!appendDetectionChunk(directory.path(), run, results(), mappings(),
                              error),
        "Evidence limits apply to stored mappings");
  auto narrowEffective = fixture(directory.path(), "narrow-effective");
  narrowEffective.request.sourceIdentity.effectiveFrequency = {2.495e9,
                                                               2.505e9};
  check(writeDetectionRun(directory.path(), narrowEffective, error),
        "Full physical context remains valid when the display effective "
        "bandwidth is narrower: " +
            error);
  auto partial = fixture(directory.path(), "partial-source");
  partial.request.sourceIdentity.physicalSamples = 500000;
  partial.request.requestedRange.time.end = 450000;
  partial.coverage.push_back(
      {DetectionCoverageKind::Unread,
       {{400000, 450000}, partial.request.requestedRange.frequency},
       "Source prefix has not been read"});
  check(writeDetectionRun(directory.path(), partial, error),
        "A physical ROI must retain unread coverage beyond the available "
        "prefix: " +
            error);
  auto falselyCompleted = partial;
  falselyCompleted.coverage.back().kind = DetectionCoverageKind::Completed;
  check(!writeDetectionRun(directory.path(), falselyCompleted, error),
        "Unread physical samples cannot be labelled completed analysis");
}
void unpublishedChunksAndLargeIndices() {
  QTemporaryDir directory;
  auto run = fixture(directory.path(), "run-large");
  constexpr std::uint64_t start = (std::uint64_t{1} << 53) + 3;
  run.request.sourceIdentity.physicalSamples = start + 1000000;
  run.request.sourceIdentity.availableSamples = start + 1000000;
  run.request.requestedRange.time = {start, start + 100000};
  run.request.contextRange.time = {start, start + 200000};
  run.coverage.clear();
  QString error;
  check(writeDetectionRun(directory.path(), run, error), error);
  const auto relative =
      encodeDetectionRun(run)["manifestRelativePath"].toString();
  writeFile(directory.filePath(QFileInfo(relative).path() + "/chunk-" +
                               QString(64, 'c') + ".json"),
            "unreferenced crash leftover");
  DetectionRun loaded;
  check(loadDetectionRun(directory.path(), "run-large", loaded, error), error);
  check(loaded.request.requestedRange.time.begin == start &&
            loaded.chunks.empty(),
        "Large sample offsets must remain exact; orphan shards must not be "
        "adopted");
  const auto orphanDir = directory.filePath("detections/" + QString(64, 'd'));
  check(QDir().mkpath(orphanDir), "Create unpublished run directory");
  writeFile(orphanDir + "/chunk-" + QString(64, 'e') + ".json", "orphan");
  const auto discovered = discoverDetectionRuns(directory.path(), error);
  check(error.isEmpty() && discovered.size() == 1,
        "Orphan-only directories are not recoverable runs");
}
void accumulatedReadsAndStreamingBudget() {
  QTemporaryDir directory;
  auto run = fixture(directory.path(), "streaming-budget");
  run.request.workingSetBudgetBytes = 16 * 1024 * 1024;
  QString error;
  for (int index = 0; index < 3; ++index) {
    check(appendDetectionChunk(
              directory.path(), run,
              {QJsonObject{{"windowId", QString::number(index)},
                           {"description", QString(1024 * 1024, 'x')}}},
              {}, error),
          error);
  }
  QJsonArray target{QJsonObject{{"sentinel", true}}};
  check(!readDetectionChunks(directory.path(), run, target, error) &&
            error.contains("budget") && target.size() == 1 &&
            target[0].toObject()["sentinel"].toBool(),
        "Accumulated JSON must fail its budget before replacing prior state");
  int visited = 0;
  check(visitDetectionChunks(
            directory.path(), run,
            [&](const QJsonObject& shard, QString&) {
              ++visited;
              return shard["results"].toArray().size() == 1;
            },
            error) &&
            visited == 3,
        error);
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    roundtripAndRecovery();
    failedReadsAndAtomicPublication();
    failedManifestReplacementPreservesDataAndCause();
    budgetsAndMalformedRequests();
    unpublishedChunksAndLargeIndices();
    accumulatedReadsAndStreamingBudget();
    std::cout << "Detection store boundary tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
