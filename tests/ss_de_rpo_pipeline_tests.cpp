// SS-DE-RPO continuation: preparation cancellation, source-plan cache, and
// the parent-process execution timeline. Fixture runs do not publish shards.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "application/annotation_service.h"
#include "infrastructure/detection_model_snapshot.h"
#include "infrastructure/detection_source.h"
#include "infrastructure/detection_task.h"
#include "infrastructure/source_loader.h"
#include <set>

using namespace signalstudio;

namespace {
void check(bool condition, const QString& detail) {
  if (!condition) throw std::runtime_error(detail.toStdString());
}

static_assert(std::string_view{kDetectionMarkFinalShardPersisted} ==
              "t_final_shard_persisted");
static_assert(std::string_view{kDetectionMarkFinalShardPersisted} !=
              "t_durable_publication");

struct TimelineMark {
  QString name;
  quint64 steadyNs = 0;
};

void writeIq(const QString& path, int samples) {
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), file.errorString());
  const QByteArray bytes(samples * 4, '\0');
  check(file.write(bytes) == bytes.size(), "Write IQ fixture");
}

FileMetadata planMetadata(const QString& id, const QString& path, int samples,
                          double sampleRateHz) {
  FileMetadata metadata;
  metadata.id = id.toStdString();
  metadata.path = path.toStdString();
  metadata.sampleRateHz = sampleRateHz;
  metadata.centerFrequencyHz = 1e6;
  metadata.sampleCount = static_cast<std::uint64_t>(samples);
  metadata.effectiveBandwidthHz = 0;
  return metadata;
}

ViewRange fullBand(const FileMetadata& metadata) {
  const auto half = metadata.sampleRateHz / 2.0;
  return {{0, metadata.sampleCount},
          {metadata.centerFrequencyHz - half,
           metadata.centerFrequencyHz + half}};
}

DetectionSourceConfig planConfig() {
  DetectionSourceConfig config;
  config.preferGpu = false;
  config.inputAdaptationVersion = 2;
  config.profileId = "pipeline-fixture-v1";
  config.profileVersion = 1;
  config.fftSize = 1024;
  config.modelFrames = 10;
  config.modelHopFrames = 5;
  config.hopSeconds = 0.001;
  config.legacyCombinedRawHeads = false;
  config.maximumCandidates = 512;
  config.useHostMkl = true;
  return config;
}

DetectionRequest requestFor(const FileMetadata& metadata, const std::string& id) {
  DetectionRequest request;
  request.runId = id;
  request.gpuDspPreferred = false;
  request.inputAdaptationVersion = 2;
  request.sourceIdentity = annotationSourceIdentity(metadata, 3);
  const auto path = QString::fromStdString(metadata.path);
  if (QFileInfo::exists(path))
    request.sourceIdentity.quickFingerprint =
        iqSourceFingerprint(path).toStdString();
  request.profile = *findDetectionProfile("pipeline-fixture-v1");
  request.parameters.mergeFrequencyHz =
      metadata.sampleRateHz >= 1e6 ? 100000.0 : metadata.sampleRateHz / 10.0;
  request.requestedRange = fullBand(metadata);
  request.contextRange = request.requestedRange;
  request.contextRange.time.end = availableSamples(metadata);
  request.rangeOrigin = "ss-de-rpo-pipeline";
  return request;
}

DetectionSourcePlan planOnce(const FileMetadata& metadata,
                             const DetectionSourceConfig& config,
                             ViewRange range) {
  DetectionSourcePlan plan;
  QString error;
  check(makeDetectionSourcePlan(metadata, range, config, plan, error, 3),
        error);
  return plan;
}

std::vector<TimelineMark> timelineMarks(const DetectionRun& run) {
  const auto document = QJsonDocument::fromJson(
      QByteArray::fromStdString(run.performanceJson));
  check(document.isObject(), "Performance JSON is not an object");
  const auto root = document.object();
  check(root.value(QStringLiteral("schema")).toString() ==
            QStringLiteral("signal-studio-detection-profile-v1"),
        "Timeline schema changed");
  const auto timeline = root.value(QStringLiteral("timeline")).toObject();
  check(timeline.value(QStringLiteral("clock")).toString() ==
            QStringLiteral("steady_clock_ns"),
        "Timeline clock is not the parent steady clock");
  check(timeline.value(QStringLiteral("runId")).toString().toStdString() ==
            run.request.runId,
        "Timeline run id differs from the run");
  std::vector<TimelineMark> marks;
  std::set<QString> names;
  for (const auto& value : timeline.value(QStringLiteral("marks")).toArray()) {
    const auto object = value.toObject();
    bool ok = false;
    const auto steadyNs =
        object.value(QStringLiteral("steadyNs")).toString().toULongLong(&ok);
    check(ok, "steadyNs is not a decimal string");
    check(names.insert(object.value("name").toString()).second,
          "Timeline contains a duplicated stage name");
    marks.push_back(
        {object.value(QStringLiteral("name")).toString(), steadyNs});
  }
  for (std::size_t i = 1; i < marks.size(); ++i) {
    check(marks[i].steadyNs >= marks[i - 1].steadyNs,
          "Timeline moved backwards at " + marks[i].name);
  }
  return marks;
}

int countMark(const std::vector<TimelineMark>& marks, const QString& name) {
  int count = 0;
  for (const auto& mark : marks)
    if (mark.name == name) ++count;
  return count;
}

void requireSubsequence(const std::vector<TimelineMark>& marks,
                        const std::vector<QString>& names) {
  std::size_t cursor = 0;
  for (const auto& name : names) {
    bool found = false;
    while (cursor < marks.size()) {
      if (marks[cursor].name == name) {
        found = true;
        ++cursor;
        break;
      }
      ++cursor;
    }
    check(found, "Missing timeline mark " + name);
  }
}

void requireAbsent(const std::vector<TimelineMark>& marks,
                   const std::vector<QString>& names) {
  for (const auto& name : names)
    check(countMark(marks, name) == 0, "Unexpected timeline mark " + name);
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
  throw std::runtime_error("Task did not reach a terminal state: " + id);
}

void testSnapshotCancellation() {
  DetectionRequest request;
  request.profile = *findDetectionProfile("pipeline-fixture-v1");
  request.modelManifestPath = "keep-manifest";
  request.modelSha256 = "unchanged";
  QString error = "stale";
  check(!freezeDetectionModelSnapshot(request, error, [] { return true; }),
        "Cancelled fixture snapshot returned success");
  check(error == "Cancelled during model snapshot", error);
  check(request.modelManifestPath == "keep-manifest" &&
            request.modelSha256 == "unchanged",
        "Cancelled snapshot wrote request fields");

  int calls = 0;
  check(freezeDetectionModelSnapshot(
            request, error, [&] { return ++calls > 1; }),
        error);
  check(calls == 1 && error.isEmpty() &&
            request.modelManifestPath == "keep-manifest" &&
            request.modelSha256 == "unchanged",
        "Fixture snapshot did not stop at the first clear checkpoint");

  request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  check(!freezeDetectionModelSnapshot(request, error, [] { return true; }) &&
            error == "Cancelled during model snapshot" &&
            !error.contains(QStringLiteral("模型包未就绪")),
        "Non-fixture cancellation reached package resolution: " + error);
  check(request.modelSha256 == "unchanged",
        "Cancelled production snapshot changed the hash");
}

void testPrepareCancellationAndChannel() {
  QTemporaryDir directory;
  check(directory.isValid(), directory.errorString());
  clearDetectionSourcePlanCacheForTest();
  auto metadata = planMetadata("prepare-source",
                               directory.filePath("missing-prepare.iq"), 200,
                               10000);
  auto request = requestFor(metadata, "prepare-cancel");
  DetectionTaskManager tasks("must-not-start-worker");
  QueuedDetectionRun prepared;
  prepared.run.device = "sentinel";
  int calls = 0;
  QString error;
  const auto before = detectionSourcePlanCacheStatsForTest();
  const bool preparedOk = tasks.prepareQueuedRun(
      directory.path(), metadata, request, prepared, error,
      [&] { return ++calls >= 2; });
  check(!preparedOk && error == "Cancelled during detection preparation" &&
            calls == 2 && prepared.run.device == "sentinel" &&
            !prepared.hasSourcePlan && tasks.snapshots().empty() &&
            !QDir(directory.filePath("detections")).exists(),
        "Preparation cancel planned, queued, or rewrote the output: calls=" +
            QString::number(calls) + " error=" + error);
  check(detectionSourcePlanCacheStatsForTest().misses == before.misses,
        "Cancel before planning counted a cache miss");

  writeIq(directory.filePath("prepare.iq"), 200);
  metadata.path = directory.filePath("prepare.iq").toStdString();
  request = requestFor(metadata, "prepare-after-cancel");
  check(tasks.prepareQueuedRun(directory.path(), metadata, request, prepared,
                               error) &&
            prepared.hasSourcePlan && prepared.sourcePlan.hopSamples == 10 &&
            tasks.snapshots().empty(),
        "A later prepare stayed blocked after cancellation: " + error);

  auto shifted = metadata;
  shifted.sampleFormat.channels = 2;
  shifted.sampleFormat.selectedChannel = 1;
  prepared = {};
  prepared.run.device = "sentinel";
  check(!tasks.prepareQueuedRun(directory.path(), shifted, request, prepared,
                                error) &&
            error ==
                "Source metadata differs from the immutable detection request" &&
            prepared.run.device == "sentinel",
        "Channel identity did not fail closed: " + error);
}

void testSourcePlanCache() {
  clearDetectionSourcePlanCacheForTest();
  const auto metadata = planMetadata("plan-a", "unused-plan.iq", 200, 10000);
  const auto range = fullBand(metadata);
  const auto config = planConfig();
  DetectionSourcePlan untouched;
  untouched.hopSamples = 42;
  untouched.gridVersion = "sentinel";
  int calls = 0;
  QString error;
  check(!makeDetectionSourcePlan(metadata, range, config, untouched, error, 3,
                                 [&] { return ++calls == 4; }) &&
            error == "Cancelled during source planning" && calls == 4 &&
            untouched.hopSamples == 42 && untouched.gridVersion == "sentinel",
        "Fourth planning checkpoint did not stop before the cache: " + error);
  auto stats = detectionSourcePlanCacheStatsForTest();
  check(stats.hits == 0 && stats.misses == 0 && stats.stores == 0 &&
            stats.retained == 0 &&
            stats.capacity == kDetectionSourcePlanCacheCapacity,
        "Cancelled planning mutated the cache");

  const auto first = planOnce(metadata, config, range);
  stats = detectionSourcePlanCacheStatsForTest();
  check(stats.misses == 1 && stats.stores == 1 && stats.hits == 0 &&
            stats.retained == 1,
        "First plan was not a miss plus store");
  const auto second = planOnce(metadata, config, range);
  stats = detectionSourcePlanCacheStatsForTest();
  check(second == first && stats.hits == 1 && stats.misses == 1 &&
            stats.stores == 1,
        "Cached plan was not an equivalent hit");
  check(detectionPreparedSourcePlanMatches(metadata, range, config, 3, first),
        "Fresh plan did not match its own source");

  auto channel = metadata;
  channel.sampleFormat.channels = 2;
  channel.sampleFormat.selectedChannel = 1;
  const auto channelPlan = planOnce(channel, config, fullBand(channel));
  check(!(channelPlan == first) &&
            !detectionPreparedSourcePlanMatches(channel, range, config, 3,
                                                first),
        "Channel change reused the previous plan");

  auto prefix = metadata;
  prefix.availability.status = LoadStatus::Partial;
  prefix.availability.availableSamples = 80;
  const auto prefixPlan = planOnce(prefix, config, range);
  check(!(prefixPlan == first) && prefixPlan.sourceIdentity.availableSamples == 80,
        "Partial prefix reused the full-file plan");

  auto grid = config;
  grid.fftSize = 65536;
  const auto wide = planOnce(metadata, grid, range);
  auto narrow = config;
  narrow.fftSize = 32768;
  const auto narrower = planOnce(metadata, narrow, range);
  check(!(wide == first) && !(narrower == wide) &&
            wide.frequencyStepHz != narrower.frequencyStepHz,
        "FFT grid change reused a plan");

  auto scn = config;
  scn.profileId = "scn-max-psd-v1";
  scn.fftSize = 32768;
  scn.modelFrames = 16;
  const auto scnPlan = planOnce(metadata, scn, range);
  check(!(scnPlan == first) && scnPlan.config.profileId == "scn-max-psd-v1",
        "SCN profile reused the fixture plan");

  auto candidates = config;
  candidates.maximumCandidates = 64;
  const auto fewer = planOnce(metadata, candidates, range);
  check(!(fewer == first) && fewer.config.maximumCandidates == 64,
        "Candidate budget reused a plan");

  auto renamed = metadata;
  renamed.id = "plan-b";
  renamed.path = "other-plan.iq";
  const auto other = planOnce(renamed, config, fullBand(renamed));
  check(!(other == first), "Source id or path reused a plan");

  clearDetectionSourcePlanCacheForTest();
  for (int i = 1; i <= 9; ++i) {
    auto item = metadata;
    item.id = "evict-" + std::to_string(i);
    planOnce(item, config, range);
  }
  stats = detectionSourcePlanCacheStatsForTest();
  check(stats.retained == 8 && stats.capacity == 8 && stats.misses == 9 &&
            stats.stores == 9 && stats.hits == 0,
        "Plan cache did not keep a bounded eight entries");
  auto newest = metadata;
  newest.id = "evict-9";
  planOnce(newest, config, range);
  auto oldest = metadata;
  oldest.id = "evict-1";
  planOnce(oldest, config, range);
  stats = detectionSourcePlanCacheStatsForTest();
  check(stats.hits == 1 && stats.misses == 10 && stats.retained == 8,
        "Eviction did not miss the oldest key and hit the newest");

  const auto warm = detectionSourcePlanCacheStatsForTest();
  DetectionSourcePlan ignored;
  ignored.hopSamples = 7;
  check(!makeDetectionSourcePlan(metadata, range, config, ignored, error, 3,
                                 [] { return true; }) &&
            error == "Cancelled during source planning" &&
            ignored.hopSamples == 7,
        "Warm-cache cancellation consulted the cache: " + error);
  const auto afterCancel = detectionSourcePlanCacheStatsForTest();
  check(afterCancel.hits == warm.hits && afterCancel.misses == warm.misses,
        "Cancellation incremented cache counters");
}

void testPreparedPlanReuseAndFingerprint() {
  QTemporaryDir directory;
  check(directory.isValid(), directory.errorString());
  const auto path = directory.filePath("reuse.iq");
  writeIq(path, 200);
  auto metadata = planMetadata("reuse-source", path, 200, 10000);
  metadata.availability.fingerprint =
      iqSourceFingerprint(path).toStdString();
  const auto range = fullBand(metadata);
  const auto config = planConfig();
  clearDetectionSourcePlanCacheForTest();
  const auto plan = planOnce(metadata, config, range);
  check(detectionSourcePlanCacheStatsForTest().misses == 1,
        "Reuse fixture did not plan once");

  {
    FixedDetectionSource source;
    QString error;
    check(source.open(metadata, range, config, error, 3, {}, &plan), error);
    check(source.plan().hopSamples == plan.hopSamples &&
              source.plan().frequencyStepHz == plan.frequencyStepHz,
          "Opened source did not keep the prepared grid");
  }
  check(detectionSourcePlanCacheStatsForTest().misses == 1 &&
            detectionSourcePlanCacheStatsForTest().hits == 0,
        "Prepared open planned again");

  {
    FixedDetectionSource source;
    QString error;
    check(source.open(metadata, range, config, error, 3), error);
  }
  check(detectionSourcePlanCacheStatsForTest().hits == 1 &&
            detectionSourcePlanCacheStatsForTest().misses == 1,
        "Unprepared open did not hit the cached plan");

  auto tampered = plan;
  tampered.frequencyStepHz += 1.0;
  {
    FixedDetectionSource source;
    QString error;
    check(!source.open(metadata, range, config, error, 3, {}, &tampered) &&
              error.contains("Prepared source plan no longer matches"),
          "Tampered grid was accepted: " + error);
  }
  check(detectionSourcePlanCacheStatsForTest().misses == 1,
        "A mismatched prepared plan was replanned");

  QFile file(path);
  check(file.resize(file.size() + 4), "Resize IQ fixture");
  {
    FixedDetectionSource source;
    QString error;
    check(!source.open(metadata, range, config, error, 3, {}, &plan) &&
              error.contains("检测源与已读前缀的物理文件身份不一致"),
          "Changed file identity passed validation: " + error);
  }
  check(detectionSourcePlanCacheStatsForTest().misses == 1,
        "Fingerprint rejection replanned the source");
}

void testPersistThenQueueRecordsOneManifestMark() {
  QTemporaryDir directory;
  check(directory.isValid(), directory.errorString());
  const auto path = directory.filePath("manifest.iq");
  writeIq(path, 200);
  const auto metadata = planMetadata("manifest-source", path, 200, 10000);
  const auto request = requestFor(metadata, "manifest-once");
  DetectionTaskManager tasks("must-not-start-worker");
  QueuedDetectionRun prepared;
  QString error;
  check(tasks.prepareQueuedRun(directory.path(), metadata, request, prepared,
                               error) &&
            tasks.persistQueuedRun(directory.path(), prepared.run, error) &&
            tasks.queuePreparedRun(directory.path(), metadata,
                                   std::move(prepared), error),
        error);
  const auto runs = tasks.snapshots();
  check(runs.size() == 1, "Persist-then-queue did not retain one run");
  const auto marks = timelineMarks(runs[0]);
  check(countMark(marks, QString::fromLatin1(kDetectionMarkManifestCommitted)) ==
            1,
        "Manifest mark was missing or duplicated");
  requireSubsequence(marks, {QString::fromLatin1(kDetectionMarkModelSnapshotReady),
                             QString::fromLatin1(kDetectionMarkPreflightStart),
                             QString::fromLatin1(kDetectionMarkSourcePlanReady),
                             QString::fromLatin1(kDetectionMarkPreflightEnd),
                             QString::fromLatin1(kDetectionMarkManifestCommitted)});
  requireAbsent(marks, {QStringLiteral("t_terminal"),
                        QStringLiteral("t_durable_publication"),
                        QString::fromLatin1(kDetectionMarkFinalShardPersisted)});
}

qint64 workerProcessId(const DetectionRun& run) {
  const auto document = QJsonDocument::fromJson(
      QByteArray::fromStdString(run.performanceJson));
  return static_cast<qint64>(
      document.object().value(QStringLiteral("worker")).toObject()
          .value(QStringLiteral("processId"))
          .toDouble());
}

DetectionRun runShortFixture(DetectionTaskManager& tasks,
                             const FileMetadata& metadata,
                             const std::string& id, bool noteAccepted) {
  auto request = requestFor(metadata, id);
  request.profile.fftSize = 3001;
  request.profile.modelFrames = std::numeric_limits<std::uint32_t>::max();
  request.profile.modelHopFrames = 7;
  request.profile.hopSeconds = 0.002;
  if (noteAccepted) {
    const auto accepted = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    tasks.noteSteadyMark(id, "t_accepted", accepted);
  }
  QString error;
  check(tasks.enqueue(
            QFileInfo(QString::fromStdString(metadata.path)).absolutePath(),
            metadata, request, error),
        error);
  const auto terminal = waitForRun(tasks, id);
  check(terminal.lifecycle == DetectionLifecycle::Completed,
        "Short fixture failed: " + QString::fromStdString(terminal.error));
  return terminal;
}

void testExecutionTimelineAndIndependentWorkers() {
  QTemporaryDir directory;
  check(directory.isValid(), directory.errorString());
  const auto path = directory.filePath("short.iq");
  writeIq(path, 17);
  auto metadata = planMetadata("timeline-source", path, 17, 1e6);
  DetectionTaskManager tasks(workerPath());
  const auto first = runShortFixture(tasks, metadata, "timeline-a", true);
  const auto second = runShortFixture(tasks, metadata, "timeline-b", false);
  const auto firstMarks = timelineMarks(first);
  const auto secondMarks = timelineMarks(second);
  const std::vector<QString> stages = {
      QStringLiteral("t_accepted"),
      QString::fromLatin1(kDetectionMarkModelSnapshotReady),
      QString::fromLatin1(kDetectionMarkPreflightStart),
      QString::fromLatin1(kDetectionMarkSourcePlanReady),
      QString::fromLatin1(kDetectionMarkPreflightEnd),
      QString::fromLatin1(kDetectionMarkManifestCommitted),
      QString::fromLatin1(kDetectionMarkSourceOpen),
      QStringLiteral("t_worker_spawn"),
      QStringLiteral("t_worker_hello"),
      QStringLiteral("t_first_iq_read"),
      QStringLiteral("t_engine_ready"),
      QString::fromLatin1(kDetectionMarkFirstModelInput),
      QStringLiteral("t_first_infer_start"),
      QStringLiteral("t_first_infer_end"),
      QString::fromLatin1(kDetectionMarkInferenceComplete),
      QString::fromLatin1(kDetectionMarkPipelineTerminal)};
  requireSubsequence(firstMarks, stages);
  requireSubsequence(
      secondMarks, std::vector<QString>(stages.begin() + 1, stages.end()));
  check(firstMarks.back().name == QString::fromLatin1(kDetectionMarkPipelineTerminal) &&
            secondMarks.back().name ==
                QString::fromLatin1(kDetectionMarkPipelineTerminal),
        "Terminal mark is not the last parent mark");
  check(countMark(firstMarks,
                  QString::fromLatin1(kDetectionMarkManifestCommitted)) == 1 &&
            countMark(secondMarks, QString::fromLatin1(
                                       kDetectionMarkManifestCommitted)) == 1,
        "Execution duplicated the manifest mark");
  const std::vector<QString> absent = {
      QStringLiteral("t_durable_publication"),
      QString::fromLatin1(kDetectionMarkFinalShardPersisted),
      QStringLiteral("t_sample_refine_complete"),
      QString::fromLatin1(kDetectionMarkFrequencyComplete),
      QString::fromLatin1(kDetectionMarkFirstCandidateDecoded),
      QString::fromLatin1(kDetectionMarkFirstPreview)};
  requireAbsent(firstMarks, absent);
  requireAbsent(secondMarks, absent);
  const auto firstPid = workerProcessId(first);
  const auto secondPid = workerProcessId(second);
  check(firstPid > 0 && secondPid > 0 && firstPid != secondPid,
        "Sequential runs did not use two independent worker processes");
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    testSnapshotCancellation();
    testPrepareCancellationAndChannel();
    testSourcePlanCache();
    testPreparedPlanReuseAndFingerprint();
    testPersistThenQueueRecordsOneManifestMark();
    testExecutionTimelineAndIndependentWorkers();
    std::cout << "PASS cancellation, source-plan cache, and parent timeline\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
