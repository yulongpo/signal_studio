#include "infrastructure/detection_projection.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_protocol.h"

namespace signalstudio {
namespace {
std::string hash(const std::string& text) {
  return QCryptographicHash::hash(QByteArray::fromStdString(text),
                                  QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}
bool reserveAnnotation(const Annotation& annotation, std::uint64_t& resident,
                       std::uint64_t budget, QString& error) {
  const auto encoded = QJsonDocument(encodeAnnotation(annotation))
                           .toJson(QJsonDocument::Compact);
  const auto bytes = 4096 + 4 * static_cast<std::uint64_t>(encoded.size());
  if (bytes > budget || resident > budget - bytes) {
    error = "Frequency publication exceeds its resident memory budget";
    return false;
  }
  resident += bytes;
  return true;
}
QJsonObject trackEvidence(const FrequencyTrack& track) {
  QJsonArray windows;
  for (const auto& window : track.windows) {
    QJsonArray bands;
    for (const auto& band : window.bands)
      bands.append(QJsonObject{{"lowerHz", band.frequency.lowerHz},
                               {"upperHz", band.frequency.upperHz},
                               {"lowerBin", band.lowerBin},
                               {"upperBin", band.upperBin},
                               {"score", band.score},
                               {"peakIndex", QString::number(band.peakIndex)}});
    windows.append(QJsonObject{
        {"windowId", QString::fromStdString(window.windowId)},
        {"ordinal", QString::number(window.ordinal)},
        {"score", window.score},
        {"nominalStartSample", QString::number(window.nominalSupport.begin)},
        {"nominalEndSample", QString::number(window.nominalSupport.end)},
        {"contextStartSample", QString::number(window.actualSupport.begin)},
        {"contextEndSample", QString::number(window.actualSupport.end)},
        {"bands", bands}});
  }
  return {
      {"version", "frequency-association-unique-window-v1"},
      {"seedId", QString::fromStdString(track.seedId)},
      {"originalLowerHz", track.originalFrequency.lowerHz},
      {"originalUpperHz", track.originalFrequency.upperHz},
      {"fusedLowerHz", track.frequency.lowerHz},
      {"fusedUpperHz", track.frequency.upperHz},
      {"uniqueHits", QString::number(track.hits)},
      {"eligibleWindowDenominator", QString::number(track.windowDenominator)},
      {"windowCoverage", track.windowCoverage},
      {"windows", windows}};
}
}  // namespace

bool DetectionResultProjector::reset(const DetectionRequest& request,
                                     std::uint64_t denominator,
                                     std::uint64_t residentBudget,
                                     QString& error) {
  initialized_ = false;
  windowEvidence_ = {};
  windowEvidenceBytes_ = 0;
  request_ = request;
  denominator_ = denominator;
  residentBudget_ = residentBudget;
  error.clear();
  return true;
}

bool DetectionResultProjector::consume(
    const QJsonArray& results, const QJsonArray& mappings, QString& error,
    const PostprocessCancellation& cancelled) {
  if (results.size() != mappings.size() || results.size() > 4096) {
    error = "Production candidate/mapping count mismatch";
    return false;
  }
  for (qsizetype index = 0; index < results.size(); ++index) {
    const auto result = results[index].toObject();
    const auto mapped = mappings[index].toObject();
    quint64 windowId = 0, begin = 0, end = 0, contextBegin = 0, contextEnd = 0;
    if (result.size() != 2 || !result["candidates"].isArray() ||
        result["windowId"] != mapped["windowId"] ||
        !workerUnsigned(mapped["windowId"], windowId, error) ||
        !workerUnsigned(mapped["startSample"], begin, error) ||
        !workerUnsigned(mapped["endSample"], end, error) ||
        !workerUnsigned(mapped["contextStartSample"], contextBegin, error) ||
        !workerUnsigned(mapped["contextEndSample"], contextEnd, error)) {
      if (error.isEmpty()) error = "Invalid production window identity";
      return false;
    }
    const double first = mapped["frequencyFirstBinHz"].toDouble(NAN);
    const double step = mapped["frequencyStepHz"].toDouble(NAN);
    if (!std::isfinite(first) || !std::isfinite(step) || step <= 0 ||
        first != request_.sourceIdentity.centerFrequencyHz -
                     request_.sourceIdentity.sampleRateHz / 2 ||
        step !=
            request_.sourceIdentity.sampleRateHz / request_.profile.fftSize ||
        begin >= end || contextBegin >= contextEnd ||
        begin < request_.requestedRange.time.begin ||
        end > request_.requestedRange.time.end ||
        contextEnd > request_.sourceIdentity.availableSamples ||
        result["candidates"].toArray().size() > request_.parameters.topK) {
      error = "Production window mapping exceeds its source snapshot";
      return false;
    }
    if (!initialized_) {
      FrequencyTrackerConfig config;
      config.runId = request_.runId;
      config.fileId = request_.sourceIdentity.fileId;
      config.inputBins = request_.profile.fftSize;
      config.peakIndexLimit = detectionModelInputBins(request_) / 4;
      config.firstBinHz = first;
      config.binWidthHz = step;
      config.minHits = request_.parameters.minHits;
      config.maxMiss = request_.parameters.maxMiss;
      config.mergeToleranceHz = request_.parameters.mergeFrequencyHz;
      config.containmentCenterToleranceHz = 2 * step;
      const auto& parameters = request_.parameters;
      const double gap =
          parameters.splitBursts
              ? (parameters.timeStrategy == DetectionTimeStrategy::Coarse
                     ? parameters.coarseSplitGapSeconds
                     : parameters.minOffSeconds)
              : 0;
      config.fragmentGapSamples = static_cast<SampleIndex>(
          std::ceil(gap * request_.sourceIdentity.sampleRateHz));
      config.totalAnalysisWindows = denominator_;
      config.limits.residentByteBudget = residentBudget_ / 2;
      std::string issue;
      if (!tracker_.reset(config, issue)) {
        error = QString::fromStdString(issue);
        return false;
      }
      firstBinHz_ = first;
      binWidthHz_ = step;
      initialized_ = true;
    } else if (first != firstBinHz_ || step != binWidthHz_) {
      error = "Production FFT coordinate grid changed within a run";
      return false;
    }
    WindowObservation observation;
    observation.runId = request_.runId;
    observation.fileId = request_.sourceIdentity.fileId;
    observation.windowId = std::to_string(windowId);
    observation.ordinal = tracker_.processedWindows();
    observation.nominalSupport = {begin, end};
    if (mapped.contains("nominalStartSample") ||
        mapped.contains("nominalEndSample")) {
      quint64 nominalBegin = 0, nominalEnd = 0;
      if (!workerUnsigned(mapped["nominalStartSample"], nominalBegin, error) ||
          !workerUnsigned(mapped["nominalEndSample"], nominalEnd, error) ||
          nominalBegin >= nominalEnd || nominalBegin > begin ||
          nominalEnd < end ||
          nominalEnd > request_.sourceIdentity.availableSamples) {
        error = "Production nominal window context is invalid";
        return false;
      }
      observation.nominalSupport = {nominalBegin, nominalEnd};
    }
    observation.actualSupport = {contextBegin, contextEnd};
    for (const auto value : result["candidates"].toArray()) {
      const auto candidate = value.toObject();
      quint64 peakIndex = 0;
      if (candidate.size() != 4 || !candidate["lowerBin"].isDouble() ||
          !candidate["upperBin"].isDouble() || !candidate["score"].isDouble() ||
          !workerUnsigned(candidate["peakIndex"], peakIndex, error) ||
          peakIndex >= detectionModelInputBins(request_) / 4) {
        error = "Unsupported production candidate fields";
        return false;
      }
      observation.candidates.push_back(
          {candidate["lowerBin"].toDouble(), candidate["upperBin"].toDouble(),
           candidate["score"].toDouble(), static_cast<std::size_t>(peakIndex)});
    }
    std::string issue;
    if (!tracker_.consume(observation, issue, cancelled)) {
      error = QString::fromStdString(issue);
      return false;
    }
    QJsonObject evidence{{"windowId", mapped["windowId"]}};
    for (const auto& key : {"headEvidence", "scnPostprocess"})
      if (mapped.contains(key)) evidence[key] = mapped[key];
    const auto bytes =
        8ULL *
            static_cast<std::uint64_t>(
                QJsonDocument(evidence).toJson(QJsonDocument::Compact).size()) +
        1024;
    if (bytes > residentBudget_ / 2 ||
        windowEvidenceBytes_ > residentBudget_ / 2 - bytes) {
      error = "Window provenance exceeds its resident budget";
      return false;
    }
    windowEvidenceBytes_ += bytes;
    windowEvidence_[QString::number(windowId)] = evidence;
  }
  return true;
}

bool DetectionResultProjector::finish(
    std::vector<FrequencyTrack>& tracks, QString& error,
    const PostprocessCancellation& cancelled) const {
  if (!initialized_) {
    tracks.clear();
    return true;
  }
  std::string issue;
  if (!tracker_.finish(tracks, issue, cancelled)) {
    error = QString::fromStdString(issue);
    return false;
  }
  return true;
}

std::string detectionTrackRecordId(const std::string& seedId) {
  return seedId.size() <= 128 ? seedId : "track:" + hash(seedId);
}

bool projectFrequencyTracks(const DetectionRequest& request,
                            const AnnotationSourceSet& sourceSet,
                            const std::vector<FrequencyTrack>& tracks,
                            std::uint64_t residentBudget,
                            DetectionPublication& target, QString& error) {
  DetectionPublication publication;
  publication.sourceSet = sourceSet;
  std::uint64_t resident = 65536;
  for (const auto& track : tracks) {
    auto frequency = track.frequency;
    frequency.lowerHz =
        std::max(frequency.lowerHz, request.requestedRange.frequency.lowerHz);
    frequency.upperHz =
        std::min(frequency.upperHz, request.requestedRange.frequency.upperHz);
    if (frequency.lowerHz >= frequency.upperHz) continue;
    if (publication.annotations.size() >= annotationSegmentBudget) {
      error = "Frequency publication exceeds object count limit";
      return false;
    }
    Annotation annotation;
    annotation.id = "detection:" + hash(request.runId + ':' + track.seedId);
    annotation.name = request.profile.backend == DetectionBackendKind::Scn
                          ? "SCN 最大谱频率观察"
                          : "FFSCN IQ 频率观察";
    annotation.fileId = request.sourceIdentity.fileId;
    annotation.sourceSetId = sourceSet.id;
    annotation.originRecordId = detectionTrackRecordId(track.seedId);
    annotation.sourceKind = AnnotationSource::Algorithm;
    const TimeRange report{
        std::max(track.nominalSupport.begin, request.requestedRange.time.begin),
        std::min(track.nominalSupport.end, request.requestedRange.time.end)};
    if (report.begin >= report.end) continue;
    annotation.observation = ViewRange{report, frequency};
    annotation.observationTimeQuality = TimeQuality::WindowObservation;
    annotation.revisionId = annotation.id + ":r0";
    annotation.reviewRevisionId = annotation.revisionId;
    annotation.revisions.push_back(
        {annotation.revisionId, {}, "algorithm-origin", "model", {}, {}});
    annotation.reviewHistory.push_back(
        {ReviewStatus::Pending,
         annotation.revisionId,
         "model",
         {},
         "Frequency observation requires activity review"});
    auto origin = std::make_shared<OriginEvidence>();
    origin->formatId = "detection-v1";
    origin->originRef = "track:" + track.seedId;
    origin->runId = request.runId;
    origin->originalObservation =
        ViewRange{track.nominalSupport, track.originalFrequency};
    origin->coordinateInterpretation =
        "Physical RF; nominal/FFT contexts are observations, not transmission "
        "intervals";
    origin->scoreMean = track.scoreMean;
    origin->scoreMax = track.scoreMax;
    origin->windowCount = track.windowDenominator;
    for (const auto& window : track.windows) {
      bool ok = false;
      const auto id = QString::fromStdString(window.windowId).toULongLong(&ok);
      if (!ok) {
        error = "Track contains a nonnumeric source window ID";
        return false;
      }
      origin->windowIds.push_back(id);
    }
    origin->originalFieldsJson = QJsonDocument(trackEvidence(track))
                                     .toJson(QJsonDocument::Compact)
                                     .toStdString();
    origin->refineStatus = RefineStatus::Skipped;
    origin->fallbackReason =
        "Time localization is pending; model predicts frequency within "
        "observation contexts";
    annotation.originEvidence = std::move(origin);
    refreshAnnotationProjection(annotation);
    if (!reserveAnnotation(annotation, resident, residentBudget, error))
      return false;
    publication.annotations.push_back(std::move(annotation));
  }
  target = std::move(publication);
  return true;
}

bool DetectionResultProjector::attachWindowEvidence(
    DetectionPublication& publication, QString& error) const {
  for (auto& annotation : publication.annotations) {
    if (!annotation.originEvidence) continue;
    auto origin = std::make_shared<OriginEvidence>(*annotation.originEvidence);
    auto fields = QJsonDocument::fromJson(
                      QByteArray::fromStdString(origin->originalFieldsJson))
                      .object();
    QJsonArray windows;
    for (const auto id : origin->windowIds) {
      const auto evidence = windowEvidence_[QString::number(id)].toObject();
      if (evidence.isEmpty()) {
        error = "Track lacks durable window provenance";
        return false;
      }
      windows.append(evidence);
      const auto path = evidence["headEvidence"].toObject()["path"].toString();
      if (!path.isEmpty()) origin->evidenceRefs.push_back(path.toStdString());
    }
    fields["windowPostprocess"] = windows;
    origin->originalFieldsJson =
        QJsonDocument(fields).toJson(QJsonDocument::Compact).toStdString();
    annotation.originEvidence = std::move(origin);
  }
  return validateDetectionPublicationBudget(publication, residentBudget_ * 2,
                                            error);
}

bool validateDetectionPublicationBudget(const DetectionPublication& publication,
                                        std::uint64_t residentBudget,
                                        QString& error) {
  std::uint64_t resident = 65536;
  for (const auto& annotation : publication.annotations)
    if (!reserveAnnotation(annotation, resident, residentBudget, error))
      return false;
  return true;
}

QJsonArray encodeDetectionPublication(const DetectionPublication& publication) {
  QJsonArray results;
  for (const auto& annotation : publication.annotations)
    results.append(QJsonObject{
        {"kind", annotation.segments.empty() ? "frequency-annotation-v1"
                                             : "refined-annotation-v1"},
        {"annotation", encodeAnnotation(annotation)}});
  return results;
}

bool decodeDetectionPublication(const QJsonArray& records,
                                const DetectionRequest& request,
                                const AnnotationSourceSet& sourceSet,
                                std::uint64_t residentBudget,
                                DetectionPublication& target, QString& error) {
  DetectionPublication publication;
  publication.sourceSet = sourceSet;
  std::uint64_t resident = 65536;
  FileMetadata metadata;
  std::unordered_set<std::string> ids;
  metadata.id = request.sourceIdentity.fileId;
  metadata.path = request.sourceIdentity.path;
  metadata.sampleFormat = request.sourceIdentity.sampleFormat;
  metadata.sampleRateHz = request.sourceIdentity.sampleRateHz;
  metadata.centerFrequencyHz = request.sourceIdentity.centerFrequencyHz;
  metadata.sampleCount = request.sourceIdentity.physicalSamples;
  for (const auto value : records) {
    const auto record = value.toObject();
    Annotation annotation;
    const bool refined = record["kind"] == "refined-annotation-v1";
    if (record.size() != 2 ||
        (!refined && record["kind"] != "frequency-annotation-v1") ||
        !decodeAnnotation(record["annotation"].toObject(), metadata, annotation,
                          error))
      return false;
    if (annotation.fileId != request.sourceIdentity.fileId ||
        !ids.insert(annotation.id).second ||
        annotation.sourceSetId != sourceSet.id ||
        annotation.sourceKind != AnnotationSource::Algorithm ||
        !annotation.originEvidence || annotation.deleted ||
        annotation.classLabel.has_value() ||
        annotation.reviewStatus != ReviewStatus::Pending ||
        annotation.revisions.size() != 1 ||
        annotation.revisionId != annotation.id + ":r0" ||
        annotation.revisions.front().id != annotation.revisionId ||
        !annotation.revisions.front().parentRevisionId.empty() ||
        annotation.revisions.front().action != "algorithm-origin" ||
        annotation.revisions.front().actor != "model" ||
        annotation.revisions.front().nameBefore ||
        annotation.revisions.front().nameAfter ||
        annotation.revisions.front().noteBefore ||
        annotation.revisions.front().noteAfter ||
        annotation.revisions.front().observationChanged ||
        annotation.revisions.front().observationBefore ||
        annotation.revisions.front().observationAfter ||
        annotation.reviewHistory.size() != 1 ||
        annotation.reviewHistory.front().status != ReviewStatus::Pending ||
        annotation.reviewHistory.front().revisionId != annotation.revisionId ||
        annotation.reviewHistory.front().actor != "model" ||
        annotation.reviewRevisionId != annotation.revisionId ||
        annotation.originEvidence->originalSegments != annotation.segments ||
        annotation.originEvidence->refineMethod == RefineMethod::Manual ||
        annotation.observationFrequencyFrame != FrequencyFrame::RF ||
        annotation.originEvidence->observationFrequencyFrame !=
            FrequencyFrame::RF ||
        annotation.originEvidence->runId != request.runId ||
        (!refined && !annotation.segments.empty()) || !annotation.observation ||
        annotation.observationTimeQuality != TimeQuality::WindowObservation ||
        annotation.observation->time.begin <
            request.requestedRange.time.begin ||
        annotation.observation->time.end > request.requestedRange.time.end ||
        annotation.observation->frequency.lowerHz <
            request.requestedRange.frequency.lowerHz ||
        annotation.observation->frequency.upperHz >
            request.requestedRange.frequency.upperHz) {
      error = "Stored frequency publication differs from its immutable run";
      return false;
    }
    for (const auto& segment : annotation.segments) {
      if (segment.range.time.begin < request.requestedRange.time.begin ||
          segment.range.time.end > request.requestedRange.time.end ||
          segment.range.frequency.lowerHz <
              request.requestedRange.frequency.lowerHz ||
          segment.range.frequency.upperHz >
              request.requestedRange.frequency.upperHz ||
          segment.timeQuality != TimeQuality::AlgorithmEstimated ||
          segment.frequencyQuality == FrequencyQuality::Manual ||
          segment.frequencyFrame != FrequencyFrame::RF || segment.confirmed) {
        error = "Stored activity differs from immutable run bounds or review";
        return false;
      }
    }
    const auto& changes = annotation.revisions.front().segmentChanges;
    if (changes.size() != annotation.segments.size()) {
      error = "Raw algorithm revision differs from original activity";
      return false;
    }
    for (const auto& segment : annotation.segments) {
      const auto change =
          std::find_if(changes.begin(), changes.end(),
                       [&](const auto& item) { return item.id == segment.id; });
      if (change == changes.end() || change->before || !change->after ||
          *change->after != segment) {
        error = "Raw algorithm revision contains modified activity";
        return false;
      }
    }
    if (!reserveAnnotation(annotation, resident, residentBudget, error))
      return false;
    publication.annotations.push_back(std::move(annotation));
  }
  target = std::move(publication);
  return true;
}
}  // namespace signalstudio
