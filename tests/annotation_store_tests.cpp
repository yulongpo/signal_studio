#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "application/annotation_service.h"
#include "application/session.h"
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
void testLocatedFrequencyBoundsAndAtomicLoad() {
  QTemporaryDir directory;
  const auto path = directory.filePath("project.json");
  auto project = projectFixture();
  auto& file = project.files.front();
  AnnotationService service;
  const auto created = service.createManual(file, "located", geometry());
  check(created.ok, "Located source fixture failed");
  auto request = annotationChannelRequest(file, created.id, file.view.time);
  request.range = {{150, 450}, {99.95e6, 100.05e6}};
  Channel channel;
  channel.id = "located-channel";
  check(service.bindChannelSource(file, channel, request).ok,
        "A time and frequency crop must bind");
  channel.visibleSourceTime = channel.sourceTime;
  channel.visibleBasebandFrequency = {-channel.outputSampleRateHz / 2,
                                      channel.outputSampleRateHz / 2};
  file.channels.push_back(channel);
  QString error;
  check(ProjectStore::save(path, project, error), error);
  const auto validBytes = read(path);
  Project loaded;
  check(ProjectStore::load(path, loaded, error), error);
  const auto preserved = loaded.files.front().channels.front().sourceSnapshot;
  for (const auto band :
       {FrequencyRange{99.89e6, 100.05e6}, FrequencyRange{99.95e6, 100.11e6},
        FrequencyRange{100.15e6, 100.20e6}}) {
    auto root = QJsonDocument::fromJson(validBytes).object();
    auto files = root["files"].toArray();
    auto entry = files[0].toObject();
    auto channels = entry["channels"].toArray();
    auto malformed = channels[0].toObject();
    auto binding = malformed["annotationSource"].toObject();
    auto range = binding["range"].toObject();
    range["lowerHz"] = band.lowerHz;
    range["upperHz"] = band.upperHz;
    binding["range"] = range;
    malformed["annotationSource"] = binding;
    channels[0] = malformed;
    entry["channels"] = channels;
    files[0] = entry;
    root["files"] = files;
    write(path, QJsonDocument(root).toJson());
    check(!ProjectStore::load(path, loaded, error) &&
              error.contains("超出创建时绑定段") &&
              loaded.files.front().channels.front().sourceSnapshot == preserved,
          "An expanded or shifted v6 snapshot must reject atomically while "
          "retaining the previously opened valid crop");
  }
}

void testUserRangeRoundtripAndMalformedBinding() {
  QTemporaryDir directory;
  const auto path = directory.filePath("project.json");
  auto project = projectFixture();
  auto& file = project.files.front();
  file.metadata.availability = {1000, LoadStatus::Partial, 0, {}, {}};
  AnnotationService service;
  AnnotationSourceSet set;
  set.id = "unknown-set";
  set.fileId = file.metadata.id;
  set.sourceKind = AnnotationSource::Algorithm;
  set.formatId = "detection-v1";
  set.sourceIdentity = annotationSourceIdentity(file.metadata);
  Annotation annotation;
  annotation.id = "unknown";
  annotation.fileId = file.metadata.id;
  annotation.sourceSetId = set.id;
  annotation.originRecordId = "record";
  annotation.name = "frequency observation";
  annotation.sourceKind = set.sourceKind;
  annotation.observation = geometry(700, 900);
  auto evidence = std::make_shared<OriginEvidence>();
  evidence->formatId = set.formatId;
  evidence->originalObservation = annotation.observation;
  annotation.originEvidence = evidence;
  const auto published = service.publishSourceSet(file, set, {annotation});
  check(published.ok,
        "UserRange fixture failed: " + QString::fromStdString(published.error));
  check(service.reviewBatch(file, {annotation.id}, ReviewStatus::Rejected).ok,
        "UserRange ignored-quality fixture failed");
  auto request = annotationChannelRequest(file, annotation.id, {100, 600});
  Channel channel;
  channel.id = "user-range-channel";
  check(service.bindChannelSource(file, channel, request).ok,
        "UserRange channel failed");
  channel.visibleSourceTime = channel.sourceTime;
  channel.visibleBasebandFrequency = {-channel.outputSampleRateHz / 2,
                                      channel.outputSampleRateHz / 2};
  file.channels.push_back(channel);
  QString error;
  check(ProjectStore::save(path, project, error), error);
  Project loaded;
  check(ProjectStore::load(path, loaded, error), error);
  const auto& reopened = loaded.files.front();
  check(
      reopened.marks.front().segments.empty() &&
          reopened.marks.front().observation == annotation.observation &&
          reopened.marks.front().reviewStatus == ReviewStatus::Rejected &&
          *reopened.channels.front().sourceSnapshot ==
              *channel.sourceSnapshot &&
          reopened.channels.front().sourceRelation ==
              ChannelSourceRelation::Current,
      "v6 must retain unknown activity, quality and exact UserRange snapshots");
  auto root = QJsonDocument::fromJson(read(path)).object();
  auto files = root["files"].toArray();
  auto entry = files[0].toObject();
  auto channels = entry["channels"].toArray();
  auto malformed = channels[0].toObject();
  auto binding = malformed["annotationSource"].toObject();
  binding["segmentId"] = "invented-activity";
  malformed["annotationSource"] = binding;
  channels[0] = malformed;
  entry["channels"] = channels;
  files[0] = entry;
  root["files"] = files;
  write(path, QJsonDocument(root).toJson());
  check(!ProjectStore::load(path, loaded, error) && !error.isEmpty() &&
            *loaded.files.front().channels.front().sourceSnapshot ==
                *channel.sourceSnapshot,
        "A fabricated UserRange segment must fail without replacing the open "
        "project");
}

void testChannelNameCreationEditingAndPersistence() {
  QTemporaryDir directory;
  const auto path = directory.filePath("project.json");
  Session session;
  check(!session.addDemoFile("names.iq", 1e6, 100e6, .01).empty(),
        "Name fixture file failed");
  const auto annotationId = session.addMark(geometry());
  auto* file = session.activeFile();
  auto request = annotationChannelRequest(*file, annotationId, file->view.time);
  request.name = "initial";
  check(session.createAnnotationChannel(request).ok,
        "Initial named channel fixture failed");
  QString error;
  check(ProjectStore::save(path, session.project(), error), error);
  auto savedBytes = read(path);
  std::string chinese, supplementary;
  for (int index = 0; index < 80; ++index) chinese += "通";
  for (int index = 0; index < 40; ++index) supplementary += "\xf0\x9f\x93\xbb";
  const std::vector<std::pair<std::string, bool>> cases{
      {std::string(80, 'a'), true},
      {std::string(81, 'a'), false},
      {chinese, true},
      {chinese + "通", false},
      {supplementary, true},
      {supplementary + "a", false},
      {" \t\xe3\x80\x80", false},
      {"\xe4\xb8", false},
      {"\xed\xa0\x80", false}};
  for (const auto& [name, accepted] : cases) {
    const auto count = file->channels.size();
    request.name = name;
    const auto created = session.createAnnotationChannel(request);
    check(created.ok == accepted &&
              (accepted
                   ? file->channels.size() == count + 1
                   : created.field == "name" && file->channels.size() == count),
          "Creation must accept 80 UTF-16 units and reject 81 without adding "
          "a channel");
    const auto before = file->channels.front();
    auto edited = before;
    edited.name = name;
    check(session.updateChannel(edited.id, edited) == accepted,
          "Editing must enforce the same Unicode name contract as creation");
    check(file->channels.front().sourceSnapshot == before.sourceSnapshot &&
              file->channels.front().name == (accepted ? name : before.name) &&
              file->channels.front().configVersion ==
                  before.configVersion + (accepted ? 1 : 0),
          "Name validation must preserve immutable evidence and leave "
          "rejected edits unchanged");
    if (accepted) {
      check(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))
                    .size() == static_cast<qsizetype>(channelNameUtf16Limit),
            "Boundary fixtures must use exactly 80 real QString units");
      check(ProjectStore::save(path, session.project(), error), error);
      savedBytes = read(path);
      Project loaded;
      check(ProjectStore::load(path, loaded, error), error);
      check(loaded.files.front().channels.front().name == name &&
                loaded.files.front().channels.back().name == name,
            "Every accepted creation and edit must reopen with exact UTF-8 "
            "bytes, including 40 supplementary characters");
    } else {
      auto invalid = session.project();
      invalid.files.front().channels.front().name = name;
      check(!ProjectStore::save(path, invalid, error) && !error.isEmpty() &&
                read(path) == savedBytes,
            "Invalid Unicode or overlong names must reject before lossy "
            "conversion and preserve the saved project bytes");
      const auto qtName =
          QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
      if (qtName.toUtf8().toStdString() == name) {
        auto root = QJsonDocument::fromJson(savedBytes).object();
        auto files = root["files"].toArray();
        auto entry = files[0].toObject();
        auto channels = entry["channels"].toArray();
        auto channel = channels[0].toObject();
        channel["name"] = qtName;
        channels[0] = channel;
        entry["channels"] = channels;
        files[0] = entry;
        root["files"] = files;
        write(path, QJsonDocument(root).toJson());
        Project target = session.project();
        check(!ProjectStore::load(path, target, error) && !error.isEmpty() &&
                  target.files.front().channels.front().name == before.name,
              "The JSON reader must reject the same overlong or blank Unicode "
              "names without replacing the open project");
        write(path, savedBytes);
      }
    }
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
        "v6 must shard canonical annotations without a second range store");
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
    testLocatedFrequencyBoundsAndAtomicLoad();
    testUserRangeRoundtripAndMalformedBinding();
    testChannelNameCreationEditingAndPersistence();
    testShardIntegrityAndAtomicLoad();
    testUint64AndPartial();
    std::cout
        << "PASS v6 bindings, failure atomicity, "
           "shard integrity, uint64/Partial\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
