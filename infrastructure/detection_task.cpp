#include "infrastructure/detection_task.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "application/annotation_service.h"
#include "domain/scn_detection_postprocess.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_frequency_analysis.h"
#include "infrastructure/detection_projection.h"
#include "infrastructure/detection_protocol.h"
#include "infrastructure/detection_refined_publication.h"
#include "infrastructure/detection_source.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/detection_time_analysis.h"
#include "infrastructure/detection_worker_client.h"
#include "infrastructure/source_loader.h"

namespace signalstudio {
namespace {
std::string boundedErrorSummary(const QString& error) {
  constexpr qsizetype kMaximumBytes = 4096;
  constexpr qsizetype kLeadingBytes = 3072;
  const QByteArray separator = "\n...\n";
  const auto bytes = error.toUtf8();
  if (bytes.size() <= kMaximumBytes) return bytes.toStdString();
  const auto continuation = [&](qsizetype index) {
    return (static_cast<unsigned char>(bytes[index]) & 0xc0) == 0x80;
  };
  auto leadingEnd = kLeadingBytes;
  while (continuation(leadingEnd)) --leadingEnd;
  auto trailingBegin =
      bytes.size() - (kMaximumBytes - kLeadingBytes - separator.size());
  while (continuation(trailingBegin)) ++trailingBegin;
  // Keep the original cause and the last failure, without cutting a UTF-8
  // code point or exceeding the persistent run's error budget.
  return (bytes.left(leadingEnd) + separator + bytes.mid(trailingBegin))
      .toStdString();
}

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
  for (const auto& frame : window.frames)
    frames.append(encodeDetectionFrameMapping(frame));
  return {
      {"windowId", QString::number(window.globalWindowIndex)},
      {"firstFrameIndex", QString::number(window.firstFrameIndex)},
      {"nominalStartSample",
       QString::number(window.firstFrameIndex * plan.hopSamples)},
      {"nominalEndSample",
       QString::number(std::min(
           plan.sourceIdentity.availableSamples,
           (window.firstFrameIndex + window.frames.size()) * plan.hopSamples))},
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
      {"analysisBins", static_cast<int>(plan.config.fftSize)},
      {"transformBins", static_cast<int>(plan.transformBins)},
      {"requestedFrames", static_cast<double>(plan.config.modelFrames)},
      {"actualFrames", static_cast<int>(window.frames.size())},
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

void preserveRuntimeEvidence(AnnotationSourceSet& set,
                             const QJsonObject& evidence) {
  if (evidence.isEmpty()) return;
  set.supplementalOriginalText =
      QJsonDocument(evidence).toJson(QJsonDocument::Compact).toStdString();
  set.supplementalHash =
      digest(QByteArray::fromStdString(set.supplementalOriginalText));
}

bool decodeProductionResults(const DetectionRequest& request,
                             const DetectionWorkerPacket& response,
                             QJsonArray& target, QString& error) {
  const auto payload = response.envelope["payload"].toObject();
  const auto windows = payload["windows"].toArray();
  const auto width = detectionModelInputBins(request) / 4;
  const auto shape = response.envelope["binary"].toObject()["shape"].toArray();
  if (payload["candidateEncoding"] != "raw-heads-v1" || windows.isEmpty() ||
      windows.size() > 2 ||
      shape != QJsonArray{static_cast<int>(windows.size()), 3, 1,
                          static_cast<int>(width)}) {
    error = "Production candidate encoding or head shape is invalid";
    return false;
  }
  std::vector<float> values;
  if (!decodeFloat32Payload(response, values, error)) return false;
  DetectionDecoderConfig config;
  config.kind = request.profile.backend == DetectionBackendKind::Scn
                    ? DetectionDecoderKind::ScnThreeBin
                    : DetectionDecoderKind::FfscnSevenBin;
  config.inputBins = detectionModelInputBins(request);
  config.scoreThreshold = request.parameters.scoreThreshold;
  config.nmsIou = request.parameters.nmsIou;
  config.topK = request.parameters.topK;
  QJsonArray results;
  for (qsizetype member = 0; member < windows.size(); ++member) {
    const auto window = windows[member].toObject();
    if (window.size() != 2 || !window["candidates"].isArray() ||
        !window["candidates"].toArray().isEmpty()) {
      error = "Production window metadata contains unexpected candidates";
      return false;
    }
    const auto begin =
        values.data() + static_cast<std::size_t>(member) * 3 * width;
    HeadView heads{{begin, width},
                   {begin + width, width},
                   {begin + 2 * width, width},
                   request.profile.backend == DetectionBackendKind::Scn
                       ? std::array<std::size_t, 3>{width, 1, 1}
                       : std::array<std::size_t, 3>{1, 1, width}};
    std::vector<DecodedCandidate> candidates;
    DetectionDecodeSummary summary;
    std::string issue;
    if (!decodeDetectionHeads(heads, config, candidates, summary, issue)) {
      error = QString::fromStdString(issue);
      return false;
    }
    QJsonArray encoded;
    for (const auto& candidate : candidates) {
      const auto scale = detectionModelBinScale(request);
      const auto lower = std::clamp(candidate.lowerBin * scale, 0.0,
                                    double(request.profile.fftSize - 1));
      const auto upper = std::clamp(candidate.upperBin * scale, 0.0,
                                    double(request.profile.fftSize - 1));
      if (upper <= lower ||
          candidate.peakIndex * scale >= request.profile.fftSize / 4.0)
        continue;
      encoded.append(
          QJsonObject{{"lowerBin", lower},
                      {"upperBin", upper},
                      {"score", candidate.score},
                      {"peakIndex", QString::number(candidate.peakIndex)}});
    }
    results.append(
        QJsonObject{{"windowId", window["windowId"]}, {"candidates", encoded}});
  }
  target = std::move(results);
  return true;
}

QJsonObject encodeCandidate(const DecodedCandidate& candidate) {
  return {{"lowerBin", candidate.lowerBin},
          {"upperBin", candidate.upperBin},
          {"score", candidate.score},
          {"peakIndex", QString::number(candidate.peakIndex)}};
}
QJsonArray encodeCandidates(const std::vector<DecodedCandidate>& values) {
  QJsonArray result;
  for (const auto& candidate : values)
    result.append(encodeCandidate(candidate));
  return result;
}
bool postprocessScnResults(const std::vector<DetectionModelWindow>& windows,
                           const DetectionSourcePlan& plan,
                           const DetectionRequest& request, QJsonArray& results,
                           QJsonArray& mappings, QString& error,
                           const PostprocessCancellation& cancelled) {
  if (results.size() != static_cast<qsizetype>(windows.size())) {
    error = "SCN decoded window count differs from its physical spectra";
    return false;
  }
  for (qsizetype index = 0; index < results.size(); ++index) {
    auto result = results[index].toObject();
    std::vector<DecodedCandidate> raw;
    for (const auto value : result["candidates"].toArray()) {
      const auto candidate = value.toObject();
      quint64 peak = 0;
      if (!workerUnsigned(candidate["peakIndex"], peak, error)) return false;
      raw.push_back(
          {candidate["lowerBin"].toDouble(), candidate["upperBin"].toDouble(),
           candidate["score"].toDouble(), static_cast<std::size_t>(peak)});
    }
    ScnWindowPostprocess processed;
    std::string issue;
    if (!postprocessScnWindow(
            windows[static_cast<std::size_t>(index)].densityDb,
            windows[static_cast<std::size_t>(index)].frames.size(),
            plan.config.fftSize, raw, plan.frequencyFirstBinHz,
            plan.frequencyStepHz, processed, issue, cancelled,
            detectionModelInputBins(request) / 4, request.spectrumMode)) {
      error = QString::fromStdString(issue);
      return false;
    }
    QJsonArray measurements, refinements;
    for (const auto& measurement : processed.measurements) {
      auto encoded = encodeCandidate(measurement.candidate);
      encoded["signalLevelDb"] = measurement.signalLevelDb;
      encoded["neighborLevelDb"] = measurement.neighborLevelDb;
      encoded["cnrDb"] = measurement.cnrDb;
      encoded["accepted"] = measurement.accepted;
      measurements.append(encoded);
    }
    for (const auto& refinement : processed.refinements) {
      QJsonObject encoded{
          {"originalLowerHz", refinement.originalFrequency.lowerHz},
          {"originalUpperHz", refinement.originalFrequency.upperHz},
          {"lowerHz", refinement.frequency.lowerHz},
          {"upperHz", refinement.frequency.upperHz},
          {"applied", refinement.applied},
          {"noiseSampleCount", QString::number(refinement.noiseSampleCount)},
          {"skippedReason", QString::fromStdString(refinement.skippedReason)}};
      if (refinement.noiseSampleCount) {
        encoded["noiseFloorDb"] = refinement.noiseFloorDb;
        encoded["thresholdDb"] = refinement.thresholdDb;
      }
      refinements.append(encoded);
    }
    auto mapped = mappings[index].toObject();
    mapped["scnPostprocess"] = QJsonObject{
        {"version", "scn-max-cnr-fusion-min-bandwidth-before-tracking-v1"},
        {"cnrThresholdDb", 3.0},
        {"bandwidthMarginDb", 3.0},
        {"fusionIou", .1},
        {"fusionOverlapRatio", .5},
        {"fusionGapHz", 0},
        {"candidateCap", 150},
        {"meanSpectrumInferenceCalls",
         request.spectrumMode == DetectionSpectrumMode::Mean ? 1 : 0},
        {"inputSpectrumMode", static_cast<int>(request.spectrumMode)},
        {"rawCandidates", encodeCandidates(raw)},
        {"measurements", measurements},
        {"unfilteredFused", encodeCandidates(processed.unfilteredFused)},
        {"filteredFused", encodeCandidates(processed.filteredFused)},
        {"refinements", refinements}};
    mappings[index] = mapped;
    result["candidates"] = encodeCandidates(processed.candidates);
    results[index] = result;
  }
  return true;
}

void preserveFrequencyEvidence(
    DetectionPublication& publication,
    const std::vector<DetectionFrequencyEvidence>& values) {
  for (auto& annotation : publication.annotations) {
    const auto found =
        std::find_if(values.begin(), values.end(), [&](const auto& value) {
          return detectionTrackRecordId(value.seedId) ==
                 annotation.originRecordId;
        });
    if (found == values.end() || !annotation.originEvidence) continue;
    auto origin = std::make_shared<OriginEvidence>(*annotation.originEvidence);
    auto fields = QJsonDocument::fromJson(
                      QByteArray::fromStdString(origin->originalFieldsJson))
                      .object();
    QJsonObject frequency{
        {"method", QString::fromStdString(found->method)},
        {"applied", found->applied},
        {"shrunk", found->shrunk},
        {"skippedReason", QString::fromStdString(found->skippedReason)},
        {"originalLowerHz", found->originalFrequency.lowerHz},
        {"originalUpperHz", found->originalFrequency.upperHz},
        {"correctedLowerHz", found->frequency.lowerHz},
        {"correctedUpperHz", found->frequency.upperHz},
        {"firstFrame", QString::number(found->firstFrame)},
        {"lastFrameExclusive", QString::number(found->lastFrameExclusive)},
        {"uniqueFrameCount", QString::number(found->frameCount)},
        {"globalFrameCount", QString::number(found->globalFrameCount)},
        {"percentileSampleCount",
         QString::number(found->percentileSampleCount)},
        {"meanSpectrumSha256",
         QString::fromStdString(found->meanSpectrumSha256)},
        {"maximumSpectrumSha256",
         QString::fromStdString(found->maximumSpectrumSha256)},
        {"numericalPolicy", QString::fromStdString(found->numericalPolicy)}};
    if (found->hasNoiseFloor) {
      frequency["noiseFloorDb"] = found->noiseFloorDb;
      frequency["thresholdDb"] = found->thresholdDb;
      if (found->hasPeak)
        origin->estimatedCnrDb = found->peakDb - found->noiseFloorDb;
    }
    frequency["hasNoiseFloor"] = found->hasNoiseFloor;
    frequency["hasPeak"] = found->hasPeak;
    frequency["hasMeasuredBins"] = found->hasMeasuredBins;
    frequency["noiseSampleCount"] = QString::number(found->noiseSampleCount);
    frequency["globalFirstFrame"] = QString::number(found->globalFirstFrame);
    frequency["globalLastFrameExclusive"] =
        QString::number(found->globalLastFrameExclusive);
    frequency["originalFirstBin"] = QString::number(found->originalFirstBin);
    frequency["originalLastBin"] = QString::number(found->originalLastBin);
    if (found->hasPeak) frequency["peakDb"] = found->peakDb;
    if (found->hasMeasuredBins) {
      frequency["measuredFirstBin"] = QString::number(found->measuredFirstBin);
      frequency["measuredLastBin"] = QString::number(found->measuredLastBin);
    }
    fields["frequencyAnalysis"] = frequency;
    origin->originalFieldsJson =
        QJsonDocument(fields).toJson(QJsonDocument::Compact).toStdString();
    annotation.originEvidence = std::move(origin);
  }
}
}  // namespace

bool recoverDetectionPublication(const QString& directory,
                                 const DetectionRun& run,
                                 DetectionPublication& target, QString& error) {
  if (run.request.profile.backend != DetectionBackendKind::Fixture) {
    DetectionPublication publication;
    publication.sourceSet = sourceSetForRequest(run.request);
    const auto publicationBudget = run.request.workingSetBudgetBytes / 2;
    std::uint64_t shardReserve = 65536;
    for (const auto& shard : run.chunks) {
      const auto bytes =
          QFileInfo(QDir(directory).filePath(
                        QString::fromStdString(shard.relativePath)))
              .size();
      if (bytes > 0)
        shardReserve = std::max(shardReserve,
                                8 * static_cast<std::uint64_t>(bytes) + 65536);
    }
    if (shardReserve + publicationBudget + 65536 >=
        run.request.workingSetBudgetBytes) {
      error =
          "Recovery shard/publication reserve exceeds the working set budget";
      return false;
    }
    const auto budget = run.request.workingSetBudgetBytes - shardReserve -
                        publicationBudget - 65536;
    DetectionResultProjector projector;
    if (!projector.reset(run.request, run.eligibleWindows, budget, error))
      return false;
    std::uint64_t publicationBytes = 65536;
    std::unordered_set<std::string> savedIds;
    bool finalized = false, hasSavedRecords = false;
    const bool visited = visitDetectionChunks(
        directory, run,
        [&](const QJsonObject& chunk, QString& issue) {
          const auto results = chunk["results"].toArray();
          const auto mappings = chunk["mappings"].toArray();
          if (finalized) {
            issue = "Result records follow a completed publication";
            return false;
          }
          if (!mappings.isEmpty()) {
            if (hasSavedRecords) {
              issue = "Window records follow publication records";
              return false;
            }
            preserveRuntimeEvidence(
                publication.sourceSet,
                mappings[0].toObject()["modelEvidence"].toObject());
            return projector.consume(results, mappings, issue);
          }
          hasSavedRecords = true;
          for (const auto value : results) {
            const auto record = value.toObject();
            if (record["kind"] == "frequency-publication-end-v1") {
              quint64 count = 0;
              if (finalized || record.size() != 2 ||
                  !workerUnsigned(record["count"], count, issue) ||
                  count !=
                      static_cast<quint64>(publication.annotations.size())) {
                issue =
                    "Durable publication end marker count or fields are "
                    "invalid";
                return false;
              }
              finalized = true;
              continue;
            }
            if (finalized || (record["kind"] != "frequency-annotation-v1" &&
                              record["kind"] != "refined-annotation-v1")) {
              issue = "Unknown durable production result projection";
              return false;
            }
            const auto recordBytes =
                4096 + 4 * static_cast<std::uint64_t>(
                               QJsonDocument(record)
                                   .toJson(QJsonDocument::Compact)
                                   .size());
            if (recordBytes > publicationBudget ||
                publicationBytes > publicationBudget - recordBytes ||
                publication.annotations.size() >= annotationSegmentBudget) {
              issue = "Recovered publication exceeds its resident budget";
              return false;
            }
            DetectionPublication decoded;
            if (!decodeDetectionPublication(
                    QJsonArray{record}, run.request, publication.sourceSet,
                    publicationBudget, decoded, issue) ||
                decoded.annotations.size() != 1)
              return false;
            auto annotation = std::move(decoded.annotations.front());
            if (!savedIds.insert(annotation.id).second) {
              issue = "Duplicate durable annotation identity";
              return false;
            }
            publicationBytes += recordBytes;
            publication.annotations.push_back(std::move(annotation));
          }
          return true;
        },
        error);
    if (!visited) return false;
    if (!finalized) {
      publication.annotations.clear();
      std::vector<FrequencyTrack> tracks;
      if (!projector.finish(tracks, error) ||
          !projectFrequencyTracks(run.request, publication.sourceSet, tracks,
                                  budget, publication, error) ||
          !projector.attachWindowEvidence(publication, error))
        return false;
    }
    target = std::move(publication);
    return true;
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
  config.inputAdaptationVersion = request.inputAdaptationVersion;
  config.profileId = request.profile.id;
  config.profileVersion = request.profile.version;
  config.fftSize = request.profile.fftSize;
  config.modelFrames = request.profile.modelFrames;
  config.modelHopFrames = request.profile.modelHopFrames;
  config.hopSeconds = request.profile.hopSeconds;
  config.workingSetBudgetBytes = request.workingSetBudgetBytes;
  config.evidenceBudgetBytes = request.evidenceBudgetBytes;
  config.maximumCandidates = request.parameters.topK;
  DetectionSourcePlan plan;
  if (!makeDetectionSourcePlan(metadata, request.requestedRange, config, plan,
                               error, request.sourceIdentity.projectGeneration))
    return false;
  std::uint64_t pipelineReserve = 0;
  const auto executionBatch = detectionExecutionBatchSize(
      plan, request.parameters.batchSize, pipelineReserve);
  if (!executionBatch) {
    error =
        "A single window exceeds the DSP, binary transport or evidence shard "
        "budget after reducing the execution batch";
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
  job->executionBatchSize = executionBatch;
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
    terminal.error = boundedErrorSummary(error);
    terminal.finishedAtMilliseconds = nowMilliseconds();
    if (terminal.lifecycle == DetectionLifecycle::Completed)
      terminal.computationPhase = "completed";
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
  if (terminal.request.profile.backend != DetectionBackendKind::Fixture &&
      terminal.completedWindows > 0) {
    bool needsPrefix = false;
    {
      std::lock_guard lock(mutex_);
      needsPrefix = job->publication.annotations.empty();
    }
    if (needsPrefix) {
      DetectionPublication prefix;
      QString recoveryError;
      if (recoverDetectionPublication(job->directory, terminal, prefix,
                                      recoveryError)) {
        std::lock_guard lock(mutex_);
        job->publication = std::move(prefix);
      } else {
        terminal.error = boundedErrorSummary(
            QString::fromStdString(terminal.error) +
            "; Durable prefix projection failed: " + recoveryError);
        std::lock_guard lock(mutex_);
        job->publication = {};
      }
    }
  }
  // Refinement may outlive the model loop. Its final source identity
  // participates in the same terminal persistence boundary as a change during
  // inference.
  const auto sourcePath =
      QString::fromStdString(terminal.request.sourceIdentity.path);
  auto observedValidity = DetectionSourceValidity::Current;
  if (!QFileInfo(sourcePath).exists())
    observedValidity = DetectionSourceValidity::Missing;
  else if (iqSourceFingerprint(sourcePath).toStdString() !=
           terminal.request.sourceIdentity.quickFingerprint)
    observedValidity = DetectionSourceValidity::Changed;
  if (observedValidity != DetectionSourceValidity::Current) {
    std::lock_guard lock(mutex_);
    if (job->run.sourceValidity == DetectionSourceValidity::Current)
      job->run.sourceValidity = observedValidity;
    terminal.sourceValidity = job->run.sourceValidity;
  }
  for (;;) {
    QString writeError;
    const bool persisted = persistRun_(job->directory, terminal, writeError);
    std::lock_guard lock(mutex_);
    if (!persisted) {
      terminal.lifecycle = DetectionLifecycle::Failed;
      if (terminal.computationPhase == "completed")
        terminal.computationPhase = "publishing";
      terminal.sourceValidity = job->run.sourceValidity;
      terminal.error = boundedErrorSummary(
          QString::fromStdString(terminal.error) + "; " + writeError);
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
  config.inputAdaptationVersion = request.inputAdaptationVersion;
  config.profileId = request.profile.id;
  config.profileVersion = request.profile.version;
  config.fftSize = request.profile.fftSize;
  config.modelFrames = request.profile.modelFrames;
  config.modelHopFrames = request.profile.modelHopFrames;
  config.hopSeconds = request.profile.hopSeconds;
  config.workingSetBudgetBytes = request.workingSetBudgetBytes;
  config.evidenceBudgetBytes = request.evidenceBudgetBytes;
  config.maximumCandidates = request.parameters.topK;
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
  if (!worker.start(workerExecutable_, request, error, cancelled,
                    job->executionBatchSize)) {
    finish(job, DetectionLifecycle::Failed, error);
    return;
  }
  const auto runtimeBatch = worker.evidence()["logicalMaximumBatch"].toInt();
  if (runtimeBatch > 0)
    job->executionBatchSize = std::min(
        job->executionBatchSize, static_cast<std::uint32_t>(runtimeBatch));
  DetectionPublication publication;
  publication.sourceSet = sourceSetForRequest(request);
  auto& set = publication.sourceSet;
  const bool fixture = request.profile.backend == DetectionBackendKind::Fixture;
  DetectionResultProjector projector;
  if (!fixture && !projector.reset(request, plan.eligibleWindowCount,
                                   job->publicationBudgetBytes / 4, error)) {
    finish(job, DetectionLifecycle::Failed, error);
    return;
  }
  if (!fixture) preserveRuntimeEvidence(set, worker.evidence());
  {
    std::lock_guard lock(mutex_);
    job->run.lifecycle = DetectionLifecycle::Running;
    if (!fixture) {
      job->run.device = worker.evidence()["device"].toString().toStdString();
      job->run.runtime = worker.evidence()["tensorRt"].toString().toStdString();
    }
  }
  std::optional<std::uint64_t> readFrameEnd;
  for (std::uint64_t index = 0; index < plan.eligibleWindowCount;) {
    if (cancelled()) {
      finish(job, DetectionLifecycle::Cancelled);
      return;
    }
    std::vector<DetectionModelWindow> windows;
    {
      std::lock_guard lock(mutex_);
      job->run.computationPhase = "reading";
    }
    std::vector<float> tensor;
    QJsonArray mappings;
    for (std::uint32_t batch = 0;
         batch < job->executionBatchSize && index < plan.eligibleWindowCount;
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
      const auto& input =
          request.profile.maximumPsdOnly ||
                  request.spectrumMode != DetectionSpectrumMode::ModelDefault
              ? window.densityDb
              : window.normalizedTensor;
      tensor.insert(tensor.end(), input.begin(), input.end());
      windows.push_back(std::move(window));
    }
    {
      std::lock_guard lock(mutex_);
      for (const auto& window : windows) {
        const auto end = window.firstFrameIndex + window.frames.size();
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
    QJsonArray windowIds;
    for (const auto& mapped : mappings)
      windowIds.append(
          QJsonObject{{"windowId", mapped.toObject()["windowId"]}});
    DetectionWorkerPacket packet{
        workerEnvelope("batch", "batch:" + QString::number(index), index,
                       {{"windows", windowIds}},
                       QString::fromStdString(request.runId),
                       request.sourceIdentity.projectGeneration,
                       QString::fromStdString(set.fileId)),
        {}};
    attachFloat32Payload(packet, tensor,
                         {static_cast<int>(windows.size()), 1,
                          static_cast<int>(plan.actualWindowFrames),
                          static_cast<int>(config.fftSize)});
    DetectionWorkerPacket response;
    {
      std::lock_guard lock(mutex_);
      job->run.computationPhase = "inference";
    }
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
    auto results = response.envelope["payload"].toObject()["windows"].toArray();
    if (!fixture)
      preserveRuntimeEvidence(
          set,
          response.envelope["payload"].toObject()["modelEvidence"].toObject());
    auto nextPublication = publication;
    auto nextPublicationBytes = job->publicationBytes;
    if (!fixture &&
        (!decodeProductionResults(request, response, results, error) ||
         (request.profile.backend == DetectionBackendKind::Scn &&
          !postprocessScnResults(windows, plan, request, results, mappings,
                                 error, cancelled)) ||
         !storeDetectionHeadEvidence(job->directory, request, response,
                                     mappings, error) ||
         !projector.consume(results, mappings, error, cancelled))) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    if (fixture && !appendFixtureAnnotations(
                       request, plan.eligibleWindowCount, windows, results,
                       plan, nextPublication, nextPublicationBytes,
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
  if (!fixture) {
    std::vector<FrequencyTrack> tracks;
    std::vector<DetectionFrequencyEvidence> frequencyEvidence;
    auto frequencyRequest = request;
    if (frequencyRequest.parameters.timeStrategy ==
        DetectionTimeStrategy::Coarse)
      frequencyRequest.parameters.bandwidthMarginDb =
          DetectionParameters{}.bandwidthMarginDb;
    {
      std::lock_guard lock(mutex_);
      job->run.computationPhase = "frequency";
    }
    if (!projector.finish(tracks, error, cancelled) ||
        (request.profile.backend != DetectionBackendKind::Scn &&
         !applyDetectionFrequencyAnalysis(source, frequencyRequest, tracks,
                                          frequencyEvidence, error,
                                          cancelled)) ||
        !projectFrequencyTracks(request, set, tracks,
                                job->publicationBudgetBytes / 2, publication,
                                error)) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    preserveFrequencyEvidence(publication, frequencyEvidence);
    {
      std::lock_guard lock(mutex_);
      job->run.computationPhase = "refine";
      job->run.totalRefinementCandidates = publication.annotations.size();
    }
    if (!checkpoint(job, error)) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    const auto refinedProgress = [&](std::size_t done, std::size_t total) {
      std::lock_guard lock(mutex_);
      job->run.refinedCandidates = done;
      job->run.totalRefinementCandidates = total;
    };
    if (!applyDetectionTimeAnalysis(source, job->metadata, request, tracks,
                                    publication, error, cancelled,
                                    refinedProgress) ||
        !projector.attachWindowEvidence(publication, error) ||
        !fuseDetectionPublication(request, publication, error, cancelled) ||
        !validateDetectionPublicationBudget(
            publication, job->publicationBudgetBytes / 2, error)) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    {
      std::lock_guard lock(mutex_);
      job->run.computationPhase = "publishing";
    }
    // Final projections are durable before they are visible. Interrupted final
    // shards without the end marker recover the raw frequency observations.
    const auto shardBudget = std::min<std::uint64_t>(
        8 * 1024 * 1024 - 8192, job->publicationBudgetBytes / 32);
    QJsonArray shard;
    std::uint64_t shardBytes = 0;
    const auto flush = [&]() {
      if (shard.isEmpty()) return true;
      if (cancelled()) {
        error = "Cancelled before durable projection shard";
        return false;
      }
      DetectionRun next;
      {
        std::lock_guard lock(mutex_);
        next = job->run;
      }
      if (!appendDetectionChunk(job->directory, next, shard, {}, error)) {
        return false;
      }
      {
        std::lock_guard lock(mutex_);
        if (job->cancelled) next.lifecycle = DetectionLifecycle::Cancelling;
        next.sourceValidity = job->run.sourceValidity;
        job->run = std::move(next);
      }
      shard = {};
      shardBytes = 0;
      return true;
    };
    const auto append = [&](const QJsonObject& record) {
      const auto bytes =
          static_cast<std::uint64_t>(
              QJsonDocument(record).toJson(QJsonDocument::Compact).size()) +
          1;
      if (bytes > shardBudget) {
        error = "One frequency projection exceeds the bounded shard budget";
        return false;
      }
      if ((shard.size() >= 4096 || shardBytes > shardBudget - bytes) &&
          !flush())
        return false;
      shard.append(record);
      shardBytes += bytes;
      return true;
    };
    for (const auto& annotation : publication.annotations)
      if (!append(QJsonObject{{"kind", "refined-annotation-v1"},
                              {"annotation", encodeAnnotation(annotation)}})) {
        finish(job, DetectionLifecycle::Failed, error);
        return;
      }
    if (!append(QJsonObject{
            {"kind", "frequency-publication-end-v1"},
            {"count", QString::number(publication.annotations.size())}}) ||
        !flush()) {
      finish(job, DetectionLifecycle::Failed, error);
      return;
    }
    {
      std::lock_guard lock(mutex_);
      job->publication = std::move(publication);
    }
  }
  finish(job, DetectionLifecycle::Completed);
}
}  // namespace signalstudio
