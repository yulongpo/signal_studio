#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/project_store.h"

using namespace signalstudio;

namespace {
void check(bool condition, const QString& message) {
  if (!condition) throw std::runtime_error(message.toStdString());
}
QByteArray read(const QString& path) {
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), file.errorString());
  return file.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), file.errorString());
  check(file.write(bytes) == bytes.size(), file.errorString());
}
Project projectFixture() {
  Project project;
  FileState file;
  file.metadata = {"file-1", "capture.iq", "capture.iq", 1e6,
                   100e6,    10'000,       true};
  file.view = fullRange(file.metadata);
  project.files.push_back(std::move(file));
  project.activeFileId = "file-1";
  return project;
}
ViewRange geometry(SampleIndex begin = 100, SampleIndex end = 500) {
  return {{begin, end}, {99.9e6, 100.1e6}};
}
void testRevisionSnapshotsAndSoftDelete() {
  QTemporaryDir directory;
  const auto path = directory.filePath("project.json");
  auto project = projectFixture();
  auto& file = project.files.front();
  AnnotationService service;
  const auto created = service.createManual(file, "carrier", geometry());
  check(created.ok, QString::fromStdString(created.error));
  auto* annotation = findAnnotation(file, created.id);
  const auto originalRevision = annotation->revisionId;
  Channel channel;
  channel.id = "channel-1";
  channel.name = "channel";
  channel.sourceMarkId = annotation->id;
  channel.centerFrequencyHz = 100e6;
  channel.bandwidthHz = 200e3;
  channel.sourceTime = annotation->range.time;
  channel.visibleSourceTime = channel.sourceTime;
  channel.visibleBasebandFrequency = {-2e6, 2e6};
  check(service
            .bindChannelSource(file, channel, annotation->id,
                               annotation->activeSegmentId)
            .ok,
        "Bind confirmed segment snapshot");
  file.channels.push_back(channel);
  check(service
            .commitGeometry(file, annotation->id, annotation->activeSegmentId,
                            geometry(150, 450))
            .ok,
        "Geometry must create a new revision");
  check(file.channels[0].sourceSnapshot->revisionId == originalRevision &&
            file.channels[0].sourceSnapshot->range == geometry(),
        "Later editing must preserve channel snapshot");
  check(service.deleteBatch(file, {created.id}).ok,
        "Soft deletion must be a command");
  QString error;
  check(ProjectStore::save(path, project, error), error);
  Project loaded;
  check(ProjectStore::load(path, loaded, error), error);
  const auto& reopened = loaded.files.front();
  check(reopened.marks.front().deleted && reopened.channels.size() == 1 &&
            reopened.channels.front().sourceSnapshot->range == geometry() &&
            reopened.channels.front().sourceRelation ==
                ChannelSourceRelation::Missing,
        "Deleted annotation and retained snapshot must survive reopening");
  check(exportAnnotationsJson(reopened) == exportAnnotationsJson(file),
        "Full annotation evidence and deltas must roundtrip");
  check(service.undo(file).ok && !file.marks.front().deleted,
        "Undo deletion must restore object without replacing channel");
}
void testLegacyVersions() {
  QTemporaryDir directory;
  const auto path = directory.filePath("project.json");
  auto project = projectFixture();
  auto& file = project.files.front();
  file.marks.emplace_back("old-id", "legacy", geometry());
  file.selectedMarkIds = {"old-id"};
  file.activeMarkId = "old-id";
  QString error;
  check(ProjectStore::save(path, project, error), error);
  const auto root = QJsonDocument::fromJson(read(path)).object();
  for (int version = 1; version <= 4; ++version) {
    auto legacy = root;
    legacy["version"] = version;
    auto files = legacy["files"].toArray();
    auto old = files[0].toObject();
    old.remove("annotationStore");
    old.remove("annotationLayers");
    old.remove("activeSegmentId");
    old["marks"] = QJsonArray{QJsonObject{
        {"id", "old-id"},
        {"name", "legacy"},
        {"range",
         QJsonObject{{"time", QJsonObject{{"begin", "100"}, {"end", "500"}}},
                     {"frequency", QJsonObject{{"lowerHz", 99.9e6},
                                               {"upperHz", 100.1e6}}}}}}};
    files[0] = old;
    legacy["files"] = files;
    write(path, QJsonDocument(legacy).toJson());
    Project loaded;
    check(ProjectStore::load(path, loaded, error), error);
    check(loaded.files[0].marks[0].id == "old-id" &&
              loaded.files[0].marks[0].segments[0].range == geometry() &&
              loaded.files[0].activeMarkId == "old-id" &&
              loaded.files[0].sourceSets.size() == 1,
          "Legacy IDs, geometry and selection must migrate");
    check(ProjectStore::save(path, loaded, error), error);
    check(QJsonDocument::fromJson(read(path)).object()["version"] == 5,
          "Migration must write v5");
  }
}
void testShardIntegrityAndAtomicLoad() {
  QTemporaryDir directory;
  const auto path = directory.filePath("project.json");
  auto project = projectFixture();
  auto& file = project.files.front();
  for (int i = 0; i < 4200; ++i)
    file.marks.emplace_back("mark-" + std::to_string(i), "carrier", geometry());
  QString error;
  check(ProjectStore::save(path, project, error), error);
  const auto bytes = read(path);
  auto root = QJsonDocument::fromJson(bytes).object();
  auto files = root["files"].toArray();
  auto entry = files[0].toObject();
  auto index = entry["annotationStore"].toObject();
  auto shards = index["shards"].toArray();
  check(shards.size() >= 2 && !entry.contains("marks"),
        "v5 must shard canonical annotations without a second range store");
  Project loaded;
  check(ProjectStore::load(path, loaded, error), error);
  check(loaded.files.front().marks.size() == 4200,
        "All sharded objects must survive");
  const auto before = exportAnnotationsJson(loaded.files.front());
  {
    auto malformed = root;
    auto malformedFiles = malformed["files"].toArray();
    auto malformedFile = malformedFiles[0].toObject();
    malformedFile["activeSegmentId"] = "missing-segment";
    malformedFiles[0] = malformedFile;
    malformed["files"] = malformedFiles;
    write(path, QJsonDocument(malformed).toJson());
    check(!ProjectStore::load(path, loaded, error) &&
              error.contains("活动段") &&
              exportAnnotationsJson(loaded.files.front()) == before,
          "Invalid active-segment reference must reject atomically");
  }

  auto badRef = shards[0].toObject();
  badRef["path"] = "../escape.json";
  shards[0] = badRef;
  index["shards"] = shards;
  entry["annotationStore"] = index;
  files[0] = entry;
  root["files"] = files;
  write(path, QJsonDocument(root).toJson());
  check(!ProjectStore::load(path, loaded, error) && !error.isEmpty() &&
            exportAnnotationsJson(loaded.files.front()) == before,
        "Path traversal must reject atomically with a diagnostic");
  write(path, bytes);
  const auto first = QJsonDocument::fromJson(bytes)
                         .object()["files"]
                         .toArray()[0]
                         .toObject()["annotationStore"]
                         .toObject()["shards"]
                         .toArray()[0]
                         .toObject();
  const auto shardPath = directory.filePath(first["path"].toString());
  auto damaged = read(shardPath);
  damaged[damaged.size() / 2] ^= 1;
  write(shardPath, damaged);
  check(!ProjectStore::load(path, loaded, error) && error.contains("hash") &&
            exportAnnotationsJson(loaded.files.front()) == before,
        "Corrupt content must reject without replacing target");
  check(!ProjectStore::save(path, project, error) && read(path) == bytes,
        "Corrupt existing shard must not be silently overwritten");
}
void testUint64AndPartial() {
  QTemporaryDir directory;
  auto project = projectFixture();
  auto& file = project.files.front();
  constexpr SampleIndex index = (SampleIndex{1} << 53) + 17;
  file.metadata.sampleCount = std::numeric_limits<SampleIndex>::max();
  file.metadata.availability = {
      index + 1000, LoadStatus::Partial, 0, "hash", {}};
  file.view.time = {index, index + 512};
  // An imported/legacy label in the unread physical tail is preserved.
  file.marks.emplace_back("tail", "tail", geometry(index + 2000, index + 3000));
  AnnotationService service;
  const auto created =
      service.createManual(file, "precise", geometry(index, index + 500));
  check(created.ok, QString::fromStdString(created.error));
  const auto path = directory.filePath("project.json");
  QString error;
  check(ProjectStore::save(path, project, error), error);
  Project loaded;
  check(ProjectStore::load(path, loaded, error), error);
  check(loaded.files[0].marks[0].segments[0].range.time.begin == index + 2000 &&
            loaded.files[0].marks[1].segments[0].range.time.begin == index &&
            availableSamples(loaded.files[0].metadata) == index + 1000,
        "Exact uint64 and unread tail labels must persist independently");
  check(!service
             .commitGeometry(loaded.files[0], created.id,
                             loaded.files[0].marks[1].activeSegmentId,
                             geometry(index, index + 2000))
             .ok,
        "Manual editing cannot extend into unread prefix");
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    testRevisionSnapshotsAndSoftDelete();
    testLegacyVersions();
    testShardIntegrityAndAtomicLoad();
    testUint64AndPartial();
    std::cout << "PASS v5 revision/snapshot, v1-v4 migration, shard atomicity, "
                 "uint64/Partial\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
