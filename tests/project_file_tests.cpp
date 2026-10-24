#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "application/session.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/project_file.h"
#include "infrastructure/project_store.h"
#include "infrastructure/raw_sample_reader.h"
#include "infrastructure/source_loader.h"

using namespace signalstudio;

namespace {
// Exception assertions remain active in Release. Each case is isolated and the
// runner continues after a failure, so one broken boundary cannot hide another.
void check(bool condition, const QString& message) {
  if (!condition) throw std::runtime_error(message.toUtf8().toStdString());
}
QString q(const std::string& value) { return QString::fromStdString(value); }
QByteArray read(const QString& path) {
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "Read fixture: " + file.errorString());
  const auto bytes = file.readAll();
  check(file.error() == QFileDevice::NoError, file.errorString());
  return bytes;
}
void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), "Write fixture: " + file.errorString());
  check(file.write(bytes) == bytes.size(), file.errorString());
  check(file.flush(), file.errorString());
}
QString mkdir(const QString& path) {
  check(QDir().mkpath(path), "Cannot create fixture directory: " + path);
  return path;
}
QJsonObject object(const QString& path) {
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(read(path), &error);
  check(error.error == QJsonParseError::NoError && document.isObject(),
        "Fixture JSON is not an object");
  return document.object();
}
void writeObject(const QString& path, const QJsonObject& value) {
  write(path, QJsonDocument(value).toJson(QJsonDocument::Indented));
}
using Tree = QMap<QString, QByteArray>;
Tree tree(const QString& directory) {
  Tree result;
  QDir root(directory);
  QDirIterator iterator(directory, QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const auto path = iterator.next();
    result.insert(root.relativeFilePath(path), read(path));
  }
  return result;
}
void sameExistingBytes(const QString& directory, const Tree& before,
                       const QString& context) {
  for (auto it = before.cbegin(); it != before.cend(); ++it)
    check(QFileInfo::exists(QDir(directory).filePath(it.key())) &&
              read(QDir(directory).filePath(it.key())) == it.value(),
          context + ": modified/deleted existing file " + it.key());
}
// Include session-only resolution fields omitted by the persisted state token.
QByteArray memory(const Project& project) {
  QJsonArray files;
  for (const auto& file : project.files) {
    const auto& m = file.metadata;
    files.append(QJsonObject{{"id", q(m.id)}, {"path", q(m.path)},
        {"signature", q(m.referenceSignature)},
        {"bytes", QString::number(m.referenceBytes)},
        {"status", static_cast<int>(m.availability.status)},
        {"available", QString::number(m.availability.availableSamples)},
        {"generation", QString::number(m.availability.generation)},
        {"error", q(m.availability.error)}});
  }
  return ProjectStore::stateToken(project) + QJsonDocument(QJsonObject{
      {"projectId", q(project.projectId)}, {"assetDirectory", q(project.assetDirectory)},
      {"files", files}}).toJson(QJsonDocument::Compact);
}
void raw(const QString& path, int seed = 0) {
  QByteArray bytes(4096 * 4, '\0');
  for (int index = 0; index < 4096 * 2; ++index)
    qToLittleEndian<qint16>(static_cast<qint16>((index * 73 + seed) % 30000),
                            bytes.data() + index * 2);
  write(path, bytes);
}
Project fixture(const QString& directory, bool real = false) {
  Session session;
  // Detection history always refers to an actual immutable source fixture,
  // including when Session's demo flag excludes external codec references.
  const auto path = QDir(directory).filePath("capture.raw");
  raw(path);
  FileMetadata metadata{"file-fixture", "capture.raw", path.toStdString(),
                        1e6, 100e6, 4096, !real};
  const auto fingerprint = iqSourceFingerprint(path);
  check(!fingerprint.isEmpty(), "RAW fixture has no immutable source fingerprint");
  metadata.availability = {4096, LoadStatus::Ready, 7,
                           fingerprint.toStdString(), {}};
  check(!session.addDemoFile(metadata).empty(), "Session fixture failed");
  const auto mark = session.addMark({{128, 1024}, {99.9e6, 100.1e6}});
  check(!mark.empty() && session.createChannelFromActiveMark(),
        "Session annotation/channel fixture failed");
  check(session.commitAnnotationGeometry(
            mark, {{192, 960}, {99.92e6, 100.08e6}}).ok,
        "Fixture revision must differ from channel creation snapshot");
  auto& project = session.project();
  project.name = "工程文件行为夹具";
  auto& file = project.files.front();
  file.channels.front().page = NarrowbandPage::Eye;
  DetectionRun run;
  run.request.runId = "project-file-fixture-run";
  const auto* profile = findDetectionProfile("pipeline-fixture-v1");
  check(profile != nullptr, "Registered fixture profile missing");
  run.request.profile = *profile;
  run.request.sourceIdentity = annotationSourceIdentity(file.metadata);
  run.request.sourceIdentity.projectGeneration = 19;
  run.request.requestedRange = {{128, 1024}, {99.9e6, 100.1e6}};
  run.request.contextRange = fullRange(file.metadata);
  run.request.rangeOrigin = "selection";
  run.lifecycle = DetectionLifecycle::Running;
  run.eligibleWindows = 1;
  run.completedWindows = 1;
  run.readFrames = 1;
  run.coverage.push_back({DetectionCoverageKind::Completed,
                          run.request.requestedRange, "fixture evidence"});
  QString error;
  check(appendDetectionChunk(directory, run,
      {QJsonObject{{"windowId", "0"}, {"fixture", true}}}, {}, error), error);
  run.lifecycle = DetectionLifecycle::Completed;
  run.computationPhase = "completed";
  check(writeDetectionRun(directory, run, error), error);
  file.detectionRuns.push_back(run);
  project.assetDirectory = directory.toStdString();
  return project;
}
// Compare evidence that must remain immutable even when the live RAW path moves.
void sameEvidence(const Project& expected, const Project& actual) {
  check(expected.files.size() == actual.files.size() &&
            expected.activeFileId == actual.activeFileId &&
            expected.activeChannelId == actual.activeChannelId &&
            expected.narrowbandWorkspaceOpen == actual.narrowbandWorkspaceOpen,
        "Project/file/workspace selection changed");
  for (std::size_t i = 0; i < expected.files.size(); ++i) {
    const auto& a = expected.files[i];
    const auto& b = actual.files[i];
    check(a.metadata.id == b.metadata.id &&
              a.metadata.sampleCount == b.metadata.sampleCount &&
              a.metadata.sampleRateHz == b.metadata.sampleRateHz &&
              a.metadata.centerFrequencyHz == b.metadata.centerFrequencyHz &&
              a.metadata.sampleFormat == b.metadata.sampleFormat && a.view == b.view,
          "File identity, physical interpretation or view changed");
    check(exportAnnotationsJson(a) == exportAnnotationsJson(b),
          "Annotation IDs, revisions, geometry or origin evidence changed");
    check(a.channels.size() == b.channels.size() &&
              a.detectionRuns.size() == b.detectionRuns.size(),
          "Channels or detection history lost");
    for (std::size_t j = 0; j < a.channels.size(); ++j) {
      const auto& x = a.channels[j];
      const auto& y = b.channels[j];
      check(x.id == y.id && x.page == y.page && x.sourceTime == y.sourceTime &&
                x.centerFrequencyHz == y.centerFrequencyHz &&
                x.bandwidthHz == y.bandwidthHz &&
                x.outputSampleRateHz == y.outputSampleRateHz &&
                x.sourceSnapshot && y.sourceSnapshot &&
                encodeAnnotationChannelSnapshot(*x.sourceSnapshot) ==
                    encodeAnnotationChannelSnapshot(*y.sourceSnapshot),
            "Channel ID, parameters, page or immutable snapshot changed");
    }
    for (std::size_t j = 0; j < a.detectionRuns.size(); ++j)
      check(encodeDetectionRun(a.detectionRuns[j]) ==
                encodeDetectionRun(b.detectionRuns[j]),
            "Detection request, source identity, coverage or shard hash changed");
  }
}
void checkDetectionShards(const Project& expected, const Project& actual) {
  for (std::size_t i = 0; i < expected.files.size(); ++i)
    for (std::size_t j = 0; j < expected.files[i].detectionRuns.size(); ++j) {
      QJsonArray a, b;
      QString error;
      check(readDetectionChunks(q(expected.assetDirectory),
                expected.files[i].detectionRuns[j], a, error), error);
      check(readDetectionChunks(q(actual.assetDirectory),
                actual.files[i].detectionRuns[j], b, error), error);
      check(!a.isEmpty() && a == b, "Detection result shard contents changed");
    }
}
void save(const QString& path, Project& project,
          AssetSaveMode mode = AssetSaveMode::Copy) {
  QString error;
  check(ProjectFileCodec::save(path, project, error, mode), "Save: " + error);
}

Project load(const QString& path);
void absoluteOnlySourceReference() {
  QTemporaryDir destination;
  QTemporaryDir source(QDir::current().filePath("ux-r3-source-reference-XXXXXX"));
  check(destination.isValid() && source.isValid(), "Source-reference fixture directories unavailable");
  auto project = fixture(source.path(), true);
  const auto path = destination.filePath("absolute-source.ssproj");
  save(path, project);
  const auto document = object(path);
  const auto reference = document["sourceReferences"].toArray()[0].toObject();
  const auto proposedRelative = QDir(destination.path()).relativeFilePath(q(project.files[0].metadata.path));
  if (QDir::isAbsolutePath(proposedRelative))
    check(reference["relativePath"].toString().isEmpty(),
          "A source on another drive must not masquerade as a relative path");
  auto loaded = load(path);
  sameEvidence(project, loaded);
  check(loaded.files[0].metadata.availability.status != LoadStatus::Failed,
        "A verified absolute source on another drive must remain available");
  // Absolute-only references are explicit and valid even on a single-volume host.
  auto absoluteOnly = document;
  auto refs = absoluteOnly["sourceReferences"].toArray();
  auto updated = refs[0].toObject(); updated["relativePath"] = QString{};
  refs[0] = updated; absoluteOnly["sourceReferences"] = refs;
  writeObject(path, absoluteOnly);
  sameEvidence(project, load(path));
}
Project load(const QString& path) {
  QString error;
  Project project;
  check(ProjectFileCodec::load(path, project, error), "Load: " + error);
  return project;
}
void rejectedLoad(const QString& path, Project& target) {
  const auto before = memory(target);
  QString error;
  check(!ProjectFileCodec::load(path, target, error) && !error.isEmpty(),
        "Malformed/missing input must fail with a diagnostic: " + path);
  check(memory(target) == before, "Failed load replaced/partially changed target");
}

void directCreateAndStableId() {
  QTemporaryDir directory;
  check(directory.isValid(), "Temporary directory unavailable");
  const auto parent = mkdir(directory.filePath("任意目录 与工程名不同"));
  const auto path = QDir(parent).filePath("直接创建.SSPROJ");
  Project empty;
  empty.name = "Name independent of filename";
  save(path, empty);
  check(!QUuid(q(empty.projectId)).isNull(), "New project needs a stable UUID");
  const auto id = empty.projectId;
  const auto envelope = object(path);
  check(envelope["magic"] == "SignalStudioProject" &&
            envelope["schemaVersion"] == 1 && envelope["projectId"] == q(id),
        "Container identity/schema missing");
  check(!QFileInfo::exists(QDir(parent).filePath("project.json")),
        "Direct creation must not require a directory project.json");
  auto reopened = load(path);
  check(reopened.files.empty() && reopened.name == empty.name &&
            reopened.projectId == id, "Empty project failed roundtrip");
  reopened.name = "Renamed without changing identity";
  save(path, reopened);
  check(load(path).projectId == id, "Normal save regenerated projectId");
}
void evidenceRoundtrip() {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto expected = project;
  const auto path = directory.filePath("evidence.ssproj");
  save(path, project);
  const auto actual = load(path);
  sameEvidence(expected, actual);
  checkDetectionShards(expected, actual);
  check(actual.projectId == project.projectId &&
            object(path)["sourceReferences"].toArray().isEmpty(),
        "Demo must not acquire a fabricated external RAW reference");
}
void moveWholeProjectAndAssets() {
  QTemporaryDir directory;
  const auto old = mkdir(directory.filePath("before"));
  auto project = fixture(old, true);
  const auto path = QDir(old).filePath("movable.ssproj");
  save(path, project);
  const auto expected = project;
  const auto moved = directory.filePath("after");
  check(QDir().rename(old, moved), "Cannot move complete fixture bundle");
  const auto actual = load(QDir(moved).filePath("movable.ssproj"));
  sameEvidence(expected, actual);
  check(QFileInfo(q(actual.files[0].metadata.path)).canonicalFilePath() ==
            QFileInfo(QDir(moved).filePath("capture.raw")).canonicalFilePath(),
        "Moved project did not prefer its relative RAW reference");
  check(actual.files[0].metadata.availability.status != LoadStatus::Failed &&
            availableSamples(actual.files[0].metadata) == 4096,
        "Moved valid RAW must remain available");
  check(read(QDir(moved).filePath("capture.raw")).size() == 4096 * 4,
        "Moving project changed RAW length");
}
void saveAsCopy() {
  QTemporaryDir directory;
  const auto original = mkdir(directory.filePath("original"));
  const auto destination = mkdir(directory.filePath("destination"));
  auto project = fixture(original, true);
  const auto sourcePath = QDir(original).filePath("original.ssproj");
  save(sourcePath, project);
  const auto source = project;
  const auto bytes = tree(original);
  project.name = "Independent copy";
  const auto copyPath = QDir(destination).filePath("copy.ssproj");
  save(copyPath, project);
  check(tree(original) == bytes, "Save As Copy modified the original project/assets");
  check(project.projectId == source.projectId &&
            project.assetDirectory != source.assetDirectory,
        "Copy must preserve project identity with an independent asset root");
  const auto copy = load(copyPath);
  sameEvidence(source, copy);
  checkDetectionShards(source, copy);
  check(object(copyPath)["assetMode"] == "copy" &&
            QDir::isRelativePath(object(copyPath)["assetDirectory"].toString()),
        "Copy asset location must move with its container");
  check(QDir().rename(q(source.assetDirectory), q(source.assetDirectory) + "-parked"),
        "Cannot temporarily detach original assets");
  sameEvidence(copy, load(copyPath));
}
void saveAsReference() {
  QTemporaryDir directory;
  const auto original = mkdir(directory.filePath("original"));
  const auto destination = mkdir(directory.filePath("destination"));
  auto project = fixture(original);
  const auto sourcePath = QDir(original).filePath("original.ssproj");
  save(sourcePath, project);
  const auto source = project;
  const auto bytes = tree(original);
  project.name = "Reference with a distinct container state";
  const auto referencePath = QDir(destination).filePath("reference.ssproj");
  save(referencePath, project, AssetSaveMode::Reference);
  const auto reference = load(referencePath);
  sameEvidence(source, reference);
  checkDetectionShards(source, reference);
  check(reference.assetDirectory == source.assetDirectory &&
            reference.projectId == source.projectId &&
            object(referencePath)["assetMode"] == "reference",
        "Reference must point to the selected original asset root");
  check(tree(original) == bytes,
        "Save As Reference modified original bytes (including state-v6.json)");
}
void relocateRealSource() {
  QTemporaryDir directory;
  auto project = fixture(directory.path(), true);
  const auto path = directory.filePath("source.ssproj");
  save(path, project);
  auto& source = project.files[0].metadata;
  const auto before = memory(project);
  const auto wrong = directory.filePath("wrong-same-length.raw");
  raw(wrong, 19);
  QString error;
  check(!ProjectFileCodec::relocate(source, wrong, error) && !error.isEmpty() &&
            memory(project) == before,
        "Different same-length RAW must be refused without rebinding");
  write(wrong, read(wrong).left(4096));
  check(!ProjectFileCodec::relocate(source, wrong, error) && memory(project) == before,
        "Wrong-length RAW must be refused without rebinding");
  const auto moved = directory.filePath("source-moved.raw");
  check(QFile::rename(q(source.path), moved), "Cannot move RAW fixture");
  const auto expected = project;
  check(ProjectFileCodec::relocate(source, moved, error), error);
  sameEvidence(expected, project);
  check(q(source.path) == QFileInfo(moved).absoluteFilePath() &&
            source.referenceSignature == expected.files[0].metadata.referenceSignature,
        "Relocation changed identity instead of resolving the same RAW");
  save(path, project);
  sameEvidence(expected, load(path));
}
void missingSourceRecovery() {
  QTemporaryDir directory;
  auto project = fixture(directory.path(), true);
  const auto path = directory.filePath("missing.ssproj");
  save(path, project);
  const auto sourcePath = q(project.files[0].metadata.path);
  const auto parked = directory.filePath("parked.raw");
  check(QFile::rename(sourcePath, parked), "Cannot hide RAW fixture");
  auto missing = load(path);
  sameEvidence(project, missing);
  check(missing.files[0].metadata.availability.status == LoadStatus::Failed &&
            availableSamples(missing.files[0].metadata) == 0 &&
            !missing.files[0].metadata.availability.error.empty(),
        "Missing RAW must open as recoverable metadata with zero available samples");
  save(path, missing);
  auto recovered = load(path);
  QString error;
  check(ProjectFileCodec::relocate(recovered.files[0].metadata, parked, error), error);
  sameEvidence(project, recovered);
  // A successful identity check is not a fresh disk scan. Reopen the saved
  // resolution to verify recovery without asking this codec to perform DSP.
  save(path, recovered);
  const auto reopened = load(path);
  RawSampleReader reader;
  check(q(reopened.files[0].metadata.path) == QFileInfo(parked).absoluteFilePath() &&
            reader.open(parked, reopened.files[0].metadata.sampleFormat, error) &&
            reader.sampleCount() == reopened.files[0].metadata.sampleCount,
        "Resolved source path/format/count did not survive save/reopen: " + error);
  sameEvidence(project, reopened);
}
void wrongSourceAtStoredPath() {
  QTemporaryDir directory;
  auto project = fixture(directory.path(), true);
  const auto path = directory.filePath("changed-source.ssproj");
  save(path, project);
  raw(q(project.files[0].metadata.path), 31);
  const auto actual = load(path);
  sameEvidence(project, actual);
  check(actual.files[0].metadata.availability.status == LoadStatus::Failed &&
            availableSamples(actual.files[0].metadata) == 0,
        "An existing same-length wrong RAW must not silently receive the annotations");
  const auto bytes = read(path);
  const auto before = memory(project);
  QString error;
  check(!ProjectFileCodec::save(path, project, error) && !error.isEmpty() &&
            memory(project) == before && read(path) == bytes,
        "Changed RAW must not replace an existing saved project");
}
void corruptAsset(const QString& prefix, bool remove) {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto path = directory.filePath("corrupt.ssproj");
  save(path, project);
  QString asset;
  for (const auto& value : object(path)["assets"].toArray()) {
    const auto relative = value.toObject()["path"].toString();
    if (relative.startsWith(prefix)) { asset = relative; break; }
  }
  check(!asset.isEmpty(), "Requested asset kind missing from test fixture");
  const auto assetPath = QDir(q(project.assetDirectory)).filePath(asset);
  if (remove) {
    check(QFile::remove(assetPath), "Cannot hide test asset");
  } else {
    auto bytes = read(assetPath);
    check(!bytes.isEmpty(), "Empty asset fixture");
    bytes[bytes.size() / 2] = char(bytes[bytes.size() / 2] ^ 1);
    write(assetPath, bytes);  // Same length: hash verification is required.
  }
  auto target = project;
  target.name = "Keep this live project";
  rejectedLoad(path, target);
}
void malformedContainer(const std::function<void(QJsonObject&)>& mutate) {
  QTemporaryDir directory;
  auto project = fixture(directory.path(), true);
  const auto path = directory.filePath("malformed.ssproj");
  save(path, project);
  auto document = object(path);
  mutate(document);
  writeObject(path, document);
  auto target = project;
  target.name = "Retain target sentinel";
  rejectedLoad(path, target);
}
void malformedJsonAndMissingAssets() {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto path = directory.filePath("invalid-json.ssproj");
  save(path, project);
  const auto goodBytes = read(path);
  for (const auto& bytes : {QByteArray("{truncated"), QByteArray("[]"), QByteArray()}) {
    write(path, bytes);
    rejectedLoad(path, project);
  }
  write(path, goodBytes);
  check(QDir().rename(q(project.assetDirectory), q(project.assetDirectory) + "-hidden"),
        "Cannot hide asset root");
  rejectedLoad(path, project);
  rejectedLoad(directory.filePath("does-not-exist.ssproj"), project);
}
void legacyV6Conversion() {
  QTemporaryDir directory;
  const auto legacyRoot = mkdir(directory.filePath("legacy"));
  auto legacy = fixture(legacyRoot, true);
  const auto source = QDir(legacyRoot).filePath("project.json");
  QString error;
  check(ProjectStore::save(source, legacy, error), error);
  const auto before = tree(legacyRoot);
  auto target = legacy;
  rejectedLoad(source, target);  // Explicit conversion is the only entry point.
  const auto destination = directory.filePath("converted.ssproj");
  check(ProjectFileCodec::importLegacy(source, destination, target, error), error);
  sameEvidence(legacy, target);
  checkDetectionShards(legacy, target);
  sameEvidence(target, load(destination));
  check(!target.projectId.empty() && tree(legacyRoot) == before,
        "Legacy conversion changed original project/RAW/annotation/detection bytes");
  const auto protectedTarget = memory(target);
  const auto protectedBytes = read(destination);
  check(!ProjectFileCodec::importLegacy(source, destination, target, error) &&
            !error.isEmpty() && memory(target) == protectedTarget &&
            read(destination) == protectedBytes && tree(legacyRoot) == before,
        "Legacy conversion must refuse an existing destination atomically");
}
void cancelledLegacyConversion() {
  QTemporaryDir directory;
  auto project = fixture(directory.path(), true);
  const auto legacy = directory.filePath("project.json");
  QString error;
  check(ProjectStore::save(legacy, project, error), error);
  const auto original = tree(directory.path());
  const auto live = memory(project);
  const auto destination = directory.filePath("cancelled-conversion.ssproj");
  check(!ProjectFileCodec::importLegacy(legacy, destination, project, error, [] { return true; }) &&
            !error.isEmpty() && !QFileInfo::exists(destination) &&
            memory(project) == live && tree(directory.path()) == original,
        "Cancelled legacy conversion must preserve source assets and the live project");
}
void rejectLegacyVersions() {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto path = directory.filePath("project.json");
  QString error;
  check(ProjectStore::save(path, project, error), error);
  const auto valid = object(path);
  for (int version = 1; version <= 5; ++version) {
    auto old = valid;
    old["version"] = version;
    writeObject(path, old);
    const auto before = tree(directory.path());
    const auto targetBefore = memory(project);
    const auto destination = directory.filePath(QString("v%1.ssproj").arg(version));
    check(!ProjectFileCodec::importLegacy(path, destination, project, error) &&
              !error.isEmpty() && !QFileInfo::exists(destination) &&
              memory(project) == targetBefore && tree(directory.path()) == before,
          QString("v%1 must be rejected without changing source/target").arg(version));
  }
}
void corruptLegacyAssets() {
  QTemporaryDir directory;
  const auto legacyRoot = mkdir(directory.filePath("legacy"));
  auto legacy = fixture(legacyRoot);
  const auto path = QDir(legacyRoot).filePath("project.json");
  QString error;
  check(ProjectStore::save(path, legacy, error), error);
  QDirIterator it(QDir(legacyRoot).filePath("detections"), {"*.json"},
                  QDir::Files, QDirIterator::Subdirectories);
  check(it.hasNext(), "Legacy detection shard fixture missing");
  write(it.next(), "broken immutable evidence");
  const auto before = tree(legacyRoot);
  const auto targetBefore = memory(legacy);
  const auto destination = directory.filePath("refuse-corrupt.ssproj");
  check(!ProjectFileCodec::importLegacy(path, destination, legacy, error) &&
            !error.isEmpty() && !QFileInfo::exists(destination) &&
            memory(legacy) == targetBefore && tree(legacyRoot) == before,
        "Corrupt legacy shards must not publish a conversion or replace target");
}
void existingForeignTarget(bool validProject) {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto path = directory.filePath("belongs-to-someone-else.ssproj");
  if (validProject) {
    Project other;
    other.name = "Another project identity";
    save(path, other);
  } else {
    write(path, "unrelated user bytes -- do not overwrite");
  }
  const auto before = tree(directory.path());
  const auto targetBefore = memory(project);
  QString error;
  check(!ProjectFileCodec::save(path, project, error) && !error.isEmpty() &&
            memory(project) == targetBefore,
        "Save must refuse an unrelated existing target without explicit replacement");
  sameExistingBytes(directory.path(), before, "Existing target protection");
}
void invalidSavePreservesTarget() {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto path = directory.filePath("valid.ssproj");
  save(path, project);
  const auto original = project;
  const auto before = read(path);
  project.files[0].metadata.sampleRateHz = 0;
  const auto targetBefore = memory(project);
  QString error;
  check(!ProjectFileCodec::save(path, project, error) && !error.isEmpty() &&
            read(path) == before && memory(project) == targetBefore,
        "Invalid state must not overwrite saved bytes or mutate the caller");
  sameEvidence(original, load(path));
  check(!ProjectFileCodec::save(directory.filePath("legacy.json"), project, error),
        "Native codec must not save over a JSON legacy entry point");
}

// QFile::setPermissions alone does not enforce directory write protection on
// Windows. Deny directory creation/file creation on a fresh, test-owned leaf,
// verify the fixture, and always remove the explicit deny before temp cleanup.
class NoWriteDirectory {
 public:
  explicit NoWriteDirectory(QString path) : path_(std::move(path)) {}
  ~NoWriteDirectory() {
#ifdef Q_OS_WIN
    if (changed_) run({path_, "/remove:d", "*S-1-1-0"});
#else
    if (changed_) QFile::setPermissions(path_, permissions_);
#endif
  }
  void deny() {
#ifdef Q_OS_WIN
    check(run({path_, "/deny", "*S-1-1-0:(OI)(CI)(WD,AD)"}),
          "icacls could not establish no-write fixture");
    changed_ = true;
#else
    permissions_ = QFile::permissions(path_);
    changed_ = true;
    check(QFile::setPermissions(path_, permissions_ &
              ~(QFileDevice::WriteOwner | QFileDevice::WriteUser |
                QFileDevice::WriteGroup | QFileDevice::WriteOther)),
          "Cannot remove directory write permissions");
#endif
    QFile probe(QDir(path_).filePath("must-not-be-created"));
    check(!probe.open(QIODevice::WriteOnly),
          "No-write fixture ineffective; refusing to report this boundary passed");
  }
 private:
#ifdef Q_OS_WIN
  static bool run(const QStringList& arguments) {
    QProcess process;
    process.start("icacls.exe", arguments);
    if (!process.waitForStarted(5000)) return false;
    if (!process.waitForFinished(10000)) { process.kill(); process.waitForFinished(); return false; }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
  }
#else
  QFileDevice::Permissions permissions_;
#endif
  QString path_;
  bool changed_ = false;
};
void noWriteDirectory() {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto denied = mkdir(directory.filePath("denied"));
  NoWriteDirectory permissions(denied);
  permissions.deny();
  const auto targetBefore = memory(project);
  QString error;
  const auto path = QDir(denied).filePath("cannot-save.ssproj");
  check(!ProjectFileCodec::save(path, project, error) && !error.isEmpty() &&
            !QFileInfo::exists(path) && memory(project) == targetBefore,
        "No-write directory must fail with no project/identity replacement");
}
void cancelledLoad(bool immediate) {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto path = directory.filePath("cancel-load.ssproj");
  save(path, project);
  int total = 0;
  QString error;
  Project probe;
  check(ProjectFileCodec::load(path, probe, error, [&] { ++total; return false; }), error);
  check(total > 1, "Load cancellation checkpoints missing");
  auto target = project;
  target.name = "Live target must survive cancellation";
  const auto targetBefore = memory(target);
  const auto before = tree(directory.path());
  int calls = 0;
  const int stop = immediate ? 1 : total;
  check(!ProjectFileCodec::load(path, target, error, [&] { return ++calls >= stop; }) &&
            !error.isEmpty() && calls >= stop && memory(target) == targetBefore &&
            tree(directory.path()) == before,
        "Cancelled load must preserve live target and all original bytes");
  sameEvidence(project, load(path));  // Cancellation scope must not leak.
}
enum class CancelSaveAt { Entry, NearPublication, LastCheckpoint };
void cancelledSave(CancelSaveAt when) {
  QTemporaryDir directory;
  auto project = fixture(directory.path());
  const auto path = directory.filePath("cancel-save.ssproj");
  save(path, project);
  QString error;
  int total = 0;
  if (when != CancelSaveAt::Entry) {
    auto probe = project;
    probe.name = "Checkpoint counting probe";
    // Count an in-place update, with exactly the same copy topology as the
    // cancellation attempt. A Save As would add asset-copy checkpoints.
    check(ProjectFileCodec::save(path, probe, error, AssetSaveMode::Copy,
              [&] { ++total; return false; }), error);
    check(total > 1, "Save cancellation checkpoints missing");
    save(path, project);
  }
  const auto before = tree(directory.path());
  project.name = "Cancelled changed project name";
  const auto targetBefore = memory(project);
  int calls = 0;
  const auto cancelled = [&] {
    ++calls;
    if (when == CancelSaveAt::Entry) return true;
    if (when == CancelSaveAt::LastCheckpoint) return calls >= total;
    return calls >= total - 1;
  };
  check(!ProjectFileCodec::save(path, project, error, AssetSaveMode::Copy, cancelled) &&
            !error.isEmpty() && calls > 0 && memory(project) == targetBefore,
        "Cancelled save must fail without replacing in-memory identity/state");
  sameExistingBytes(directory.path(), before, "Cancelled save");
  check(read(path) == before.value("cancel-save.ssproj"),
        "Cancelled save published new container bytes");
  check(load(path).name != project.name, "Cancelled state became the saved state");
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  const std::vector<std::pair<const char*, std::function<void()>>> cases{
      {"direct-create-stable-id", directCreateAndStableId},
      {"annotation-channel-detection-roundtrip", evidenceRoundtrip},
      {"move-project-assets-and-raw", moveWholeProjectAndAssets},
      {"save-as-copy", saveAsCopy},
      {"save-as-reference-original-bytes", saveAsReference},
      {"raw-relocation-and-same-length-rejection", relocateRealSource},
      {"missing-source-save-and-recovery", missingSourceRecovery},
      {"wrong-raw-at-stored-path", wrongSourceAtStoredPath},
      {"corrupt-annotation-asset", [] { corruptAsset("annotations/", false); }},
      {"missing-annotation-asset", [] { corruptAsset("annotations/", true); }},
      {"corrupt-detection-asset", [] { corruptAsset("detections/", false); }},
      {"missing-detection-asset", [] { corruptAsset("detections/", true); }},
      {"invalid-json-and-missing-assets", malformedJsonAndMissingAssets},
      {"container-magic", [] { malformedContainer([](auto& o) { o["magic"] = "other"; }); }},
      {"container-version", [] { malformedContainer([](auto& o) { o["schemaVersion"] = 2; }); }},
      {"container-project-id", [] { malformedContainer([](auto& o) { o["projectId"] = "not-a-uuid"; }); }},
      {"container-state", [] { malformedContainer([](auto& o) { o["state"] = QJsonArray{}; }); }},
      {"missing-source-reference", [] { malformedContainer([](auto& o) { o["sourceReferences"] = QJsonArray{}; }); }},
      {"duplicate-source-reference", [] { malformedContainer([](auto& o) {
        auto refs = o["sourceReferences"].toArray(); refs.append(refs[0]); o["sourceReferences"] = refs;
      }); }},
      {"reference-physical-mismatch", [] { malformedContainer([](auto& o) {
        auto refs = o["sourceReferences"].toArray(); auto ref = refs[0].toObject();
        ref["sampleRateHz"] = 2e6; refs[0] = ref; o["sourceReferences"] = refs;
      }); }},
      {"manifest-path-traversal", [] { malformedContainer([](auto& o) {
        auto assets = o["assets"].toArray(); auto ref = assets[0].toObject();
        ref["path"] = "annotations/../../outside.json"; assets[0] = ref; o["assets"] = assets;
      }); }},
      {"legacy-v6-explicit-readonly-conversion", legacyV6Conversion},
      {"cancel-legacy-conversion", cancelledLegacyConversion},
      {"reject-legacy-v1-through-v5", rejectLegacyVersions},
      {"legacy-corrupt-shards", corruptLegacyAssets},
      {"protect-existing-nonproject-target", [] { existingForeignTarget(false); }},
      {"protect-existing-other-project", [] { existingForeignTarget(true); }},
      {"invalid-save-preserves-target", invalidSavePreservesTarget},
      {"absolute-only-and-cross-drive-source-reference", absoluteOnlySourceReference},
      {"no-write-directory", noWriteDirectory},
      {"cancel-load-entry", [] { cancelledLoad(true); }},
      {"cancel-load-late", [] { cancelledLoad(false); }},
      {"cancel-save-entry", [] { cancelledSave(CancelSaveAt::Entry); }},
      {"cancel-save-near-publication", [] { cancelledSave(CancelSaveAt::NearPublication); }},
      {"cancel-save-last-checkpoint", [] { cancelledSave(CancelSaveAt::LastCheckpoint); }},
  };
  int failed = 0;
  for (const auto& [name, test] : cases) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << cases.size() - failed << '/' << cases.size()
            << " project file behavior cases passed\n";
  return failed ? 1 : 0;
}
