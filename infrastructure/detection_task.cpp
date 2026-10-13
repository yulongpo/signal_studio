#include "infrastructure/detection_task.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_protocol.h"
#include "infrastructure/detection_source.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/detection_worker_client.h"
#include "infrastructure/source_loader.h"

namespace signalstudio {
namespace {
std::uint64_t nowMilliseconds() {
  return static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
}
std::string digest(const QByteArray& bytes) {
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}
AnnotationSourceSet sourceSetForRequest(const DetectionRequest& request) {
  AnnotationSourceSet set;
  set.id = "run:" + digest(QByteArray::fromStdString(request.runId));
  set.fileId = request.sourceIdentity.fileId;
  set.sampleChannel = request.sourceIdentity.selectedSampleChannel;
  set.sourceKind = AnnotationSource::Algorithm;
  set.formatId = "detection-v1";
  set.parserId = "detection-worker-v1";
  set.parserVersion = "1";
  set.profileId = request.profile.id;
  set.profileVersion = std::to_string(request.profile.version);
  set.runId = request.runId;
  set.sourceIdentity = request.sourceIdentity;
  set.coordinateInterpretation =
      "Full-band fixed grid; report clipped to immutable ROI";
  set.originalText = QJsonDocument(encodeDetectionRequest(request))
                         .toJson(QJsonDocument::Compact)
                         .toStdString();
  set.sourceHash = digest(QByteArray::fromStdString(set.originalText));
  return set;
}
QJsonObject mapping(const DetectionModelWindow& window,
                    const DetectionSourcePlan& plan) {
  QJsonArray frames;
  for (const auto& frame : window.frames) {
    QJsonArray supports;
    for (const auto& support : frame.fftSupports)
      supports.append(QJsonObject{
          {"startSample", QString::number(support.actualSamples.begin)},
          {"endSample", QString::number(support.actualSamples.end)},
          {"leadingPadding", QString::number(support.leadingPadding)},
          {"trailingPadding", QString::number(support.trailingPadding)}});
    frames.append(
        QJsonObject{{"frameIndex", QString::number(frame.frameIndex)},
                    {"startSample", QString::number(frame.actualSamples.begin)},
                    {"endSample", QString::number(frame.actualSamples.end)},
                    {"validSamples", QString::number(frame.validSamples)},
                    {"paddedSamples", QString::number(frame.paddedSamples)},
                    {"powerFloorBins", QString::number(frame.powerFloorBins)},
                    {"densityUnderflowGuardBins",
                     QString::number(frame.densityUnderflowGuardBins)},
                    {"fftSupports", supports},
                    {"inputHash", QString::fromStdString(frame.inputHash)}});
  }
  return {
      {"windowId", QString::number(window.globalWindowIndex)},
      {"firstFrameIndex", QString::number(window.firstFrameIndex)},
      {"startSample", QString::number(window.reportRange.time.begin)},
      {"endSample", QString::number(window.reportRange.time.end)},
      {"contextStartSample", QString::number(window.actualContextRange.begin)},
      {"contextEndSample", QString::number(window.actualContextRange.end)},
      {"tensorHash", QString::fromStdString(window.tensorHash)},
      {"frequencyFirstBinHz", plan.frequencyFirstBinHz},
      {"frequencyStepHz", plan.frequencyStepHz},
      {"normalizationMeanDb", window.meanDb},
      {"normalizationStdDb", window.normalizationStdDb},
      {"epsilonGuardApplied", window.epsilonGuardApplied},
      {"floorGuardApplied", window.floorGuardApplied},
      {"densityUnderflowGuardApplied", window.densityUnderflowGuardApplied},
      {"gridVersion", QString::fromStdString(plan.gridVersion)},
      {"numericalVersion", QString::fromStdString(plan.numericalVersion)},
      {"numericalPolicy", QString::fromStdString(plan.numericalPolicy)},
      {"frames", frames}};
}
void addCoverage(DetectionRun& run, DetectionCoverageKind kind, ViewRange range,
                 std::string reason = {}) {
  if (range.time.begin >= range.time.end) return;
  auto existing = std::find_if(
      run.coverage.begin(), run.coverage.end(), [&](const auto& value) {
        return value.kind == kind && value.reason == reason &&
               value.range.frequency == range.frequency &&
               value.range.time.end >= range.time.begin &&
               value.range.time.begin <= range.time.end;
      });
  if (existing == run.coverage.end())
    run.coverage.push_back({kind, range, std::move(reason)});
  else {
    existing->range.time.begin =
        std::min(existing->range.time.begin, range.time.begin);
    existing->range.time.end =
        std::max(existing->range.time.end, range.time.end);
  }
}
bool appendFixtureAnnotations(
    const DetectionRequest& request, std::uint64_t denominator,
    const std::vector<DetectionModelWindow>& windows, const QJsonArray& results,
    const DetectionSourcePlan& plan, DetectionPublication& publication,
    std::uint64_t& residentBytes, std::uint64_t budgetBytes, QString& error) {
  if (results.size() != static_cast<qsizetype>(windows.size())) {
    error = "Worker result window count does not match the input";
    return false;
  }
  for (qsizetype i = 0; i < results.size(); ++i) {
    const auto result = results[i].toObject();
    const auto& window = windows[static_cast<std::size_t>(i)];
    if (result.size() != 2 ||
        result["windowId"] != QString::number(window.globalWindowIndex) ||
        !result["candidates"].isArray()) {
      error = "Worker result window identity is invalid";
      return false;
    }
    std::size_t candidateIndex = 0;
    for (const auto value : result["candidates"].toArray()) {
      const auto stableCandidateIndex = candidateIndex++;
      const auto candidate = value.toObject();
      if (candidate.size() != 3 || !candidate["lowerBin"].isDouble() ||
          !candidate["upperBin"].isDouble() || !candidate["score"].isDouble()) {
        error = "Fixture candidate has unsupported fields or numeric types";
        return false;
      }
      const double low = candidate["lowerBin"].toDouble(-1);
      const double high = candidate["upperBin"].toDouble(-1);
      const double score = candidate["score"].toDouble(-1);
      if (!std::isfinite(low) || !std::isfinite(high) ||
          !std::isfinite(score) || low < 0 || low >= high ||
          high >= request.profile.fftSize || score < 0 || score > 1) {
        error = "Worker candidate exceeds its tensor or score bounds";
        return false;
      }
      auto observation = window.reportRange;
      observation.frequency = {
          std::max(request.requestedRange.frequency.lowerHz,
                   plan.frequencyFirstBinHz + low * plan.frequencyStepHz),
          std::min(request.requestedRange.frequency.upperHz,
                   plan.frequencyFirstBinHz + high * plan.frequencyStepHz)};
      if (observation.frequency.lowerHz >= observation.frequency.upperHz)
        continue;
      if (publication.annotations.size() >= annotationSegmentBudget) {
        error = "Detection annotations exceed the bounded publication budget";
        return false;
      }
      Annotation annotation;
      const auto stableKey = request.runId + ":w" +
                             std::to_string(window.globalWindowIndex) + ":c" +
                             std::to_string(stableCandidateIndex);
      annotation.id =
          "detection:" + digest(QByteArray::fromStdString(stableKey));
      annotation.fileId = request.sourceIdentity.fileId;
      annotation.sourceSetId = publication.sourceSet.id;
      annotation.originRecordId = annotation.id;
      annotation.name = "系统链路夹具（非模型）";
      annotation.sourceKind = AnnotationSource::Algorithm;
      annotation.observation = observation;
      annotation.observationTimeQuality = TimeQuality::WindowObservation;
      annotation.revisionId = annotation.id + ":r0";
      auto origin = std::make_shared<OriginEvidence>();
      origin->formatId = "detection-v1";
      origin->originRef = "window:" + std::to_string(window.globalWindowIndex);
      origin->originalFieldsJson =
          QJsonDocument(candidate).toJson(QJsonDocument::Compact).toStdString();
      origin->coordinateInterpretation =
          "Full-band FFT bin centers mapped to RF; observation only";
      origin->runId = request.runId;
      origin->originalObservation = observation;
      origin->scoreMean = score;
      origin->scoreMax = score;
      origin->windowIds = {window.globalWindowIndex};
      origin->windowCount = denominator;
      origin->fallbackReason =
          "Pipeline fixture has no validated time localization";
      annotation.originEvidence = std::move(origin);
      const auto encodedBytes =
          static_cast<std::uint64_t>(QJsonDocument(encodeAnnotation(annotation))
                                         .toJson(QJsonDocument::Compact)
                                         .size());
      // Canonical objects, JSON nodes, command patches and temporary copies
      // coexist during publication. This is a conservative budget estimate,
      // separate from the measured RSS acceptance in M5.
      const auto reserve = 4096 + 4 * encodedBytes;
      if (reserve > budgetBytes || residentBytes > budgetBytes - reserve) {
        error = "Accumulated detection publication exceeds working set budget";
        return false;
      }
      residentBytes += reserve;
      publication.annotations.push_back(std::move(annotation));
    }
  }
  return true;
}
}  // namespace

bool recoverDetectionPublication(const QString& directory,
                                 const DetectionRun& run,
                                 DetectionPublication& target, QString& error) {
  if (run.request.profile.backend != DetectionBackendKind::Fixture) {
    error =
        "This backend requires its registered result projection to recover "
        "annotations";
    return false;
  }
  DetectionPublication publication;
  publication.sourceSet = sourceSetForRequest(run.request);
  std::uint64_t shardReserve = 65536, residentBytes = 65536;
  for (const auto& shard : run.chunks) {
    const auto size = QFileInfo(QDir(directory).filePath(
                                    QString::fromStdString(shard.relativePath)))
                          .size();
    if (size > 0)
      shardReserve =
          std::max(shardReserve, 8 * static_cast<std::uint64_t>(size) + 65536);
  }
  if (shardReserve >= run.request.workingSetBudgetBytes) {
    error = "Recovery shard reserve exceeds working set budget";
    return false;
  }
  const auto budgetBytes = run.request.workingSetBudgetBytes - shardReserve;
  const bool visited = visitDetectionChunks(
      directory, run,
      [&](const QJsonObject& chunk, QString& error) {
        const auto mappings = chunk["mappings"].toArray();
        const auto results = chunk["results"].toArray();
        if (mappings.size() != results.size()) {
          error = "Recovered window mapping count mismatch";
          return false;
        }
        for (qsizetype i = 0; i < mappings.size(); ++i) {
          const auto mapped = mappings[i].toObject();
          DetectionModelWindow window;
          quint64 start = 0, end = 0, windowId = 0;
          if (!workerUnsigned(mapped["startSample"], start, error) ||
              !workerUnsigned(mapped["endSample"], end, error) ||
              !workerUnsigned(mapped["windowId"], windowId, error))
            return false;
          window.globalWindowIndex = windowId;
          window.reportRange = {{start, end},
                                run.request.requestedRange.frequency};
          DetectionSourcePlan plan;
          if (!mapped["frequencyFirstBinHz"].isDouble() ||
              !mapped["frequencyStepHz"].isDouble() ||
              !std::isfinite(mapped["frequencyFirstBinHz"].toDouble()) ||
              !std::isfinite(mapped["frequencyStepHz"].toDouble()) ||
              mapped["frequencyStepHz"].toDouble() <= 0) {
            error = "Recovered window has no valid physical FFT grid";
            return false;
          }
          plan.frequencyFirstBinHz = mapped["frequencyFirstBinHz"].toDouble();
          plan.frequencyStepHz = mapped["frequencyStepHz"].toDouble();
          if (!appendFixtureAnnotations(run.request, run.eligibleWindows,
                                        {window}, QJsonArray{results[i]}, plan,
                                        publication, residentBytes, budgetBytes,
                                        error))
            return false;
        }
        return true;
      },
      error);
  if (!visited) return false;
  target = std::move(publication);
  return true;
}

DetectionTaskManager::DetectionTaskManager(QString executable,
                                           PersistRun persistRun)
    : workerExecutable_(std::move(executable)),
      persistRun_(persistRun ? std::move(persistRun) : writeDetectionRun) {}
DetectionTaskManager::~DetectionTaskManager() {
  for (const auto& job : jobs_) job->cancelled = true;
  if (thread_.joinable()) thread_.join();
}
bool DetectionTaskManager::enqueue(const QString& directory,
                                   const FileMetadata& metadata,
                                   DetectionRequest request, QString& error) {
  const auto valid = validateDetectionRequest(request);
  if (!valid || !QDir(directory).exists()) {
    error = valid ? "Create or save a project directory before detection"
                  : QString::fromStdString(valid.error);
    return false;
  }
  const auto current = annotationSourceIdentity(
      metadata, request.sourceIdentity.projectGeneration);
  if (current.fileId != request.sourceIdentity.fileId ||
      current.path != request.sourceIdentity.path ||
      current.sampleFormat != request.sourceIdentity.sampleFormat ||
      current.sampleRateHz != request.sourceIdentity.sampleRateHz ||
      current.centerFrequencyHz != request.sourceIdentity.centerFrequencyHz ||
      current.physicalSamples != request.sourceIdentity.physicalSamples ||
      current.availableSamples != request.sourceIdentity.availableSamples) {
    error = "Source metadata differs from the immutable detection request";
    return false;
  }
  DetectionSourceConfig config;
  config.profileId = request.profile.id;
  config.profileVersion = request.profile.version;
  config.fftSize = request.profile.fftSize;
  config.modelFrames = request.profile.modelFrames;
  config.modelHopFrames = request.profile.modelHopFrames;
  config.hopSeconds = request.profile.hopSeconds;
  config.workingSetBudgetBytes = request.workingSetBudgetBytes;
  DetectionSourcePlan plan;
  if (!makeDetectionSourcePlan(metadata, request.requestedRange, config, plan,
                               error, request.sourceIdentity.projectGeneration))
    return false;
  const std::uint64_t payloadBytes =
      static_cast<std::uint64_t>(request.parameters.batchSize) *
      request.profile.modelFrames * request.profile.fftSize * sizeof(float);
  const auto pipelineReserve = plan.estimatedPeakBytes + 6 * payloadBytes +
                               2 * 33554432ULL + 2 * 65536ULL;
  if (pipelineReserve + 65536 > request.workingSetBudgetBytes) {
    error =
        "Working set budget cannot hold the bounded DSP, batch and worker "
        "transport";
    return false;
  }
  request.contextRange = {
      {std::min(request.requestedRange.time.begin, plan.contextRange.begin),
       std::max(std::min(request.requestedRange.time.end,
                         availableSamples(metadata)),
                plan.contextRange.end)},
      {metadata.centerFrequencyHz - metadata.sampleRateHz / 2,
       metadata.centerFrequencyHz + metadata.sampleRateHz / 2}};
  std::lock_guard lock(mutex_);
  const auto queued = std::count_if(
      jobs_.begin(), jobs_.end(),
      [](const auto& job) { return !detectionIsTerminal(job->run.lifecycle); });
  if (queued >= 32 || jobs_.size() >= 4096 ||
      std::any_of(jobs_.begin(), jobs_.end(), [&](const auto& job) {
        return job->run.request.runId == request.runId;
      })) {
    error = "Task queue is full or the run ID already exists";
    return false;
  }
  auto job = std::make_shared<Job>();
  job->directory = directory;
  job->metadata = metadata;
  job->publicationBudgetBytes = request.workingSetBudgetBytes - pipelineReserve;
  if (request.sourceIdentity.quickFingerprint.empty()) {
    request.sourceIdentity.quickFingerprint =
        iqSourceFingerprint(QString::fromStdString(metadata.path))
            .toStdString();
    job->metadata.availability.fingerprint =
        request.sourceIdentity.quickFingerprint;
  }
  job->run.request = std::move(request);
  job->run.device = "CPU spectrum / isolated worker";
  job->run.runtime = "worker-v1";
  if (!persistRun_(directory, job->run, error)) return false;
  jobs_.push_back(std::move(job));
  return true;
}
bool DetectionTaskManager::cancel(const std::string& runId) {
  std::lock_guard lock(mutex_);
  for (const auto& job : jobs_) {
    if (job->run.request.runId != runId ||
        detectionIsTerminal(job->run.lifecycle) || job->finishing)
      continue;
    job->cancelled = true;
    job->run.lifecycle = job->started ? DetectionLifecycle::Cancelling
                                      : DetectionLifecycle::Cancelled;
    if (!job->started) {
      job->run.finishedAtMilliseconds = nowMilliseconds();
      addCoverage(job->run, DetectionCoverageKind::Unread,
                  job->run.request.requestedRange,
                  "Cancelled before processing");
      job->finished = true;
      QString error;
      job->terminalPersisted = persistRun_(job->directory, job->run, error);
      if (!job->terminalPersisted) {
        job->run.lifecycle = DetectionLifecycle::Failed;
        job->run.error = error.toStdString();
      }
    }
    return true;
  }
  return false;
}
void DetectionTaskManager::cancelGeneration(std::uint64_t generation) {
  const auto runs = snapshots();
  for (const auto& run : runs)
    if (run.request.sourceIdentity.projectGeneration == generation)
      cancel(run.request.runId);
}
void DetectionTaskManager::invalidateSource(const std::string& runId,
                                            DetectionSourceValidity validity) {
  {
    std::lock_guard lock(mutex_);
    for (const auto& job : jobs_)
      if (job->run.request.runId == runId &&
          !detectionIsTerminal(job->run.lifecycle))
        job->run.sourceValidity = validity;
  }
  cancel(runId);
}
void DetectionTaskManager::pump() {
  std::lock_guard lock(mutex_);
  if (active_ && !active_->finished) return;
  if (thread_.joinable()) thread_.join();
  active_.reset();
  for (const auto& job : jobs_) {
    if (job->started || job->run.lifecycle != DetectionLifecycle::Queued)
      continue;
    active_ = job;
    job->started = true;
    thread_ = std::jthread([this, job] { execute(job); });
    break;
  }
}
std::vector<DetectionRun> DetectionTaskManager::snapshots() const {
  std::lock_guard lock(mutex_);
  std::vector<DetectionRun> result;
  for (const auto& job : jobs_) result.push_back(job->run);
  return result;
}
bool DetectionTaskManager::takePublication(const std::string& runId,
                                           DetectionPublication& publication) {
  std::lock_guard lock(mutex_);
  for (const auto& job : jobs_) {
    if (job->run.request.runId != runId || !job->finished ||
        !job->terminalPersisted || job->publicationTaken)
      continue;
    job->publicationTaken = true;
    publication = std::move(job->publication);
    return !publication.sourceSet.id.empty();
  }
  return false;
}
bool DetectionTaskManager::checkpoint(const std::shared_ptr<Job>& job,
                                      QString& error) {
  DetectionRun run;
  {
    std::lock_guard lock(mutex_);
    run = job->run;
  }
  return persistRun_(job->directory, run, error);
}
void DetectionTaskManager::finish(const std::shared_ptr<Job>& job,
                                  DetectionLifecycle lifecycle,
                                  const QString& error) {
  DetectionRun terminal;
  {
    std::lock_guard lock(mutex_);
    // Cancellation is accepted before this commit point. Afterwards callers
    // receive false while the immutable terminal snapshot is being persisted.
    job->finishing = true;
    terminal = job->run;
    terminal.lifecycle =
        job->cancelled ? DetectionLifecycle::Cancelled : lifecycle;
    terminal.error = error.toStdString();
    terminal.finishedAtMilliseconds = nowMilliseconds();
    if (terminal.lifecycle != DetectionLifecycle::Completed) {
      auto unread = terminal.request.requestedRange;
      for (const auto& coverage : terminal.coverage)
        if (coverage.kind == DetectionCoverageKind::Completed)
          unread.time.begin =
              std::max(unread.time.begin, coverage.range.time.end);
      addCoverage(terminal, DetectionCoverageKind::Unread, unread,
                  "No completed model response was persisted for this range");
    }
  }
  for (;;) {
    QString writeError;
    const bool persisted = persistRun_(job->directory, terminal, writeError);
    std::lock_guard lock(mutex_);
    if (!persisted) {
      terminal.lifecycle = DetectionLifecycle::Failed;
      terminal.sourceValidity = job->run.sourceValidity;
      terminal.error += "; " + writeError.toStdString();
    } else if (terminal.sourceValidity != job->run.sourceValidity) {
      // Source changes remain effective during disk IO. Keep the same terminal
      // lifecycle and durably replace its validity before exposing results.
      terminal.sourceValidity = job->run.sourceValidity;
      continue;
    }
    job->run = std::move(terminal);
    job->terminalPersisted = persisted;
    if (!persisted) job->publication = {};
    job->finished = true;
    break;
  }
}
void DetectionTaskManager::execute(const std::shared_ptr<Job>& job) {
  QString error;
  const auto request = job->run.request;
  const auto cancelled = [&] { return job->cancelled.load(); };
  {
    std::lock_guard lock(mutex_);
    job->run.lifecycle = DetectionLifecycle::Validating;
    job->run.startedAtMilliseconds = nowMilliseconds();
  }
  if (!checkpoint(job, error)) {
    finish(job, DetectionLifecycle::Failed, error);
    return;
  }
  FixedDetectionSource source;
  DetectionSourceConfig config;
  config.profileId = request.profile.id;
  config.profileVersion = request.profile.version;
  config.fftSize = request.profile.fftSize;
  config.modelFrames = request.profile.modelFrames;
  config.modelHopFrames = request.profile.modelHopFrames;
  config.hopSeconds = request.profile.hopSeconds;
  config.workingSetBudgetBytes = request.workingSetBudgetBytes;
  if (cancelled() ||
      !source.open(job->metadata, request.requestedRange, config, error,
                   request.sourceIdentity.projectGeneration)) {
    if (!cancelled()) {
      std::lock_guard lock(mutex_);
      job->run.sourceValidity =
          QFileInfo(QString::fromStdString(request.sourceIdentity.path))
                  .exists()
              ? DetectionSourceValidity::Changed
              : DetectionSourceValidity::Missing;
    }
    finish(job, DetectionLifecycle::Failed, error);
    return;
  }
  const auto& plan = source.plan();
  {
    std::lock_guard lock(mutex_);
    job->run.eligibleWindows = plan.eligibleWindowCount;
    for (const auto& exclusion : plan.exclusions)
      addCoverage(job->run, DetectionCoverageKind::Excluded,
                  {exclusion.samples, request.requestedRange.frequency},
                  exclusion.reason);
  }
  if (!plan.eligibleWindowCount) {
    error = "Requested range has no complete real model window";
    finish(job, DetectionLifecycle::Failed, error);
    return;
  }
  DetectionWorkerClient worker;
  if (!worker.start(workerExecutable_, request, error, cancelled)) {
    finish(job, DetectionLifecycle::Failed, error);
    return;
  }
  DetectionPublication publication;
  publication.sourceSet = sourceSetForRequest(request);
  auto& set = publication.sourceSet;
  {
    std::lock_guard lock(mutex_);
    job->run.lifecycle = DetectionLifecycle::Running;
  }
  std::optional<std::uint64_t> readFrameEnd;
  for (std::uint64_t index = 0; index < plan.eligibleWindowCount;) {
    if (cancelled()) {
      finish(job, DetectionLifecycle::Cancelled);
      return;
    }
    std::vector<DetectionModelWindow> windows;
    std::vector<float> tensor;
    QJsonArray mappings;
    for (std::uint32_t batch = 0; batch < request.parameters.batchSize &&
                                  index < plan.eligibleWindowCount;
         ++batch, ++index) {
      DetectionModelWindow window;
      if (!source.readModelWindow(index, window, error, cancelled)) {
        if (!cancelled()) {
          const auto path = QString::fromStdString(request.sourceIdentity.path);
          std::lock_guard lock(mutex_);
          if (!QFileInfo(path).exists())
            job->run.sourceValidity = DetectionSourceValidity::Missing;
          else if (iqSourceFingerprint(path).toStdString() !=
                   request.sourceIdentity.quickFingerprint)
            job->run.sourceValidity = DetectionSourceValidity::Changed;
        }
        finish(job, DetectionLifecycle::Failed, error);
        return;
      }
      mappings.append(mapping(window, plan));
      tensor.insert(tensor.end(), window.normalizedTensor.begin(),
                    window.normalizedTensor.end());
      windows.push_back(std::move(window));
    }
    {
      std::lock_guard lock(mutex_);
      for (const auto& window : windows) {
        const auto end = window.firstFrameIndex + config.modelFrames;
        job->run.readFrames +=
            end - std::max(window.firstFrameIndex,
                           readFrameEnd.value_or(window.firstFrameIndex));
        readFrameEnd = end;
        addCoverage(
            job->run, DetectionCoverageKind::Read,
            {window.actualContextRange, request.contextRange.frequency});
        addCoverage(job->run, DetectionCoverageKind::Eligible,
                    window.reportRange);
      }
    }
    if (!checkpoint(job, error)) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    DetectionWorkerPacket packet{
        workerEnvelope("batch", "batch:" + QString::number(index), index,
                       {{"windows", mappings}},
                       QString::fromStdString(request.runId),
                       request.sourceIdentity.projectGeneration,
                       QString::fromStdString(set.fileId)),
        {}};
    attachFloat32Payload(packet, tensor,
                         {static_cast<int>(windows.size()), 1,
                          static_cast<int>(config.modelFrames),
                          static_cast<int>(config.fftSize)});
    DetectionWorkerPacket response;
    if (!worker.exchange(std::move(packet), response, error, cancelled)) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    const auto sourcePath = QString::fromStdString(request.sourceIdentity.path);
    if (iqSourceFingerprint(sourcePath).toStdString() !=
            request.sourceIdentity.quickFingerprint ||
        !QFileInfo(sourcePath).exists()) {
      {
        std::lock_guard lock(mutex_);
        job->run.sourceValidity = QFileInfo(sourcePath).exists()
                                      ? DetectionSourceValidity::Changed
                                      : DetectionSourceValidity::Missing;
      }
      finish(job, DetectionLifecycle::Failed,
             "Source changed after reading the model window");
      return;
    }
    const auto results =
        response.envelope["payload"].toObject()["windows"].toArray();
    auto nextPublication = publication;
    auto nextPublicationBytes = job->publicationBytes;
    if (!appendFixtureAnnotations(request, plan.eligibleWindowCount, windows,
                                  results, plan, nextPublication,
                                  nextPublicationBytes,
                                  job->publicationBudgetBytes, error)) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    bool exceedsPublication = false;
    {
      std::lock_guard lock(mutex_);
      exceedsPublication =
          job->publication.annotations.size() >
          annotationSegmentBudget - nextPublication.annotations.size();
    }
    if (exceedsPublication) {
      finish(job, DetectionLifecycle::Failed,
             "Detection publication exceeds 100000 objects");
      return;
    }
    DetectionRun next;
    {
      std::lock_guard lock(mutex_);
      next = job->run;
    }
    next.completedWindows += windows.size();
    for (const auto& window : windows) {
      addCoverage(next, DetectionCoverageKind::Completed, window.reportRange);
    }
    if (!appendDetectionChunk(job->directory, next, results, mappings, error)) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    {
      std::lock_guard lock(mutex_);
      if (job->cancelled) next.lifecycle = DetectionLifecycle::Cancelling;
      if (job->run.sourceValidity != DetectionSourceValidity::Current)
        next.sourceValidity = job->run.sourceValidity;
      job->run = std::move(next);
      job->publication.sourceSet = publication.sourceSet;
      job->publicationBytes = nextPublicationBytes;
      job->publication.annotations.insert(
          job->publication.annotations.end(),
          std::make_move_iterator(nextPublication.annotations.begin()),
          std::make_move_iterator(nextPublication.annotations.end()));
    }
  }
  DetectionWorkerPacket end{
      workerEnvelope("completed", "end", 0, {},
                     QString::fromStdString(request.runId),
                     request.sourceIdentity.projectGeneration,
                     QString::fromStdString(set.fileId)),
      {}};
  DetectionWorkerPacket response;
  if (!worker.exchange(std::move(end), response, error, cancelled)) {
    finish(job, DetectionLifecycle::Failed, error);
    return;
  }
  finish(job, DetectionLifecycle::Completed);
}
}  // namespace signalstudio
