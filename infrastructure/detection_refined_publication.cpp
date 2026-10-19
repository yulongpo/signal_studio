#include "infrastructure/detection_refined_publication.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

#include "domain/detection_refined_fusion.h"
#include "infrastructure/annotation_json.h"

namespace signalstudio {
namespace {
std::string digest(const QJsonArray& ids) {
  return QCryptographicHash::hash(
             QJsonDocument(ids).toJson(QJsonDocument::Compact),
             QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}
ViewRange united(ViewRange a, ViewRange b) {
  return {
      {std::min(a.time.begin, b.time.begin), std::max(a.time.end, b.time.end)},
      {std::min(a.frequency.lowerHz, b.frequency.lowerHz),
       std::max(a.frequency.upperHz, b.frequency.upperHz)}};
}
bool mergeOrigins(const DetectionRequest& request,
                  const std::vector<Annotation>& parents, Annotation& result,
                  QString& error) {
  if (parents.empty() || !parents.front().originEvidence ||
      !parents.front().originEvidence->originalObservation) {
    error = "Refined fusion lacks immutable parent observation";
    return false;
  }
  auto origin =
      std::make_shared<OriginEvidence>(*parents.front().originEvidence);
  QJsonArray snapshots, parentIds;
  std::map<std::uint64_t, QJsonObject> windows;
  std::set<std::string> references;
  for (const auto& parent : parents) {
    if (parent.sourceSetId != result.sourceSetId ||
        parent.fileId != result.fileId || !parent.originEvidence ||
        !parent.originEvidence->originalObservation ||
        parent.originEvidence->runId != request.runId ||
        parent.originEvidence->windowCount != origin->windowCount) {
      error = "Refined fusion cannot mix source, file or run identity";
      return false;
    }
    snapshots.append(encodeAnnotation(parent));
    parentIds.append(QString::fromStdString(parent.id));
    origin->originalObservation =
        united(*origin->originalObservation,
               *parent.originEvidence->originalObservation);
    if (parent.originEvidence->refineStatus == RefineStatus::Fallback)
      origin->refineStatus = RefineStatus::Fallback;
    for (const auto& ref : parent.originEvidence->evidenceRefs)
      references.insert(ref);
    const auto fields =
        QJsonDocument::fromJson(QByteArray::fromStdString(
                                    parent.originEvidence->originalFieldsJson))
            .object();
    for (const auto item : fields["windows"].toArray()) {
      const auto window = item.toObject();
      bool ok = false;
      const auto id = window["windowId"].toString().toULongLong(&ok);
      const auto score = window["score"].toDouble(-1);
      if (!ok || !std::isfinite(score) || score < 0 || score > 1) {
        error = "Refined fusion lacks a valid unique-window contribution";
        return false;
      }
      if (!windows.contains(id) || windows[id]["score"].toDouble() < score)
        windows[id] = window;
    }
  }
  origin->windowIds.clear();
  origin->scoreMean.reset();
  origin->scoreMax.reset();
  QJsonArray unionWindows;
  double sum = 0, maximum = 0;
  for (const auto& [id, window] : windows) {
    origin->windowIds.push_back(id);
    unionWindows.append(window);
    sum += window["score"].toDouble();
    maximum = std::max(maximum, window["score"].toDouble());
  }
  if (!windows.empty()) {
    origin->scoreMean = sum / windows.size();
    origin->scoreMax = maximum;
  }
  origin->windowCount = parents.front().originEvidence->windowCount;
  if (origin->refineStatus == RefineStatus::Fallback)
    origin->fallbackReason =
        "Fused activity includes fallback; see immutable parent evidence";
  origin->evidenceRefs.assign(references.begin(), references.end());
  origin->estimatedCnrDb.reset();
  origin->originRef = "refined-fusion:" + digest(parentIds);
  origin->parentCandidateId = parents.front().originRecordId;
  // Individual thresholds and frequency estimates only describe their parent.
  // Keep those measurements in immutable snapshots, not in the union summary.
  QJsonObject fields{
      {"version", "signalstudio-refined-publication-v1"},
      {"uniqueHits", QString::number(windows.size())},
      {"eligibleWindowDenominator", QString::number(origin->windowCount)},
      {"windowCoverage",
       origin->windowCount
           ? static_cast<double>(windows.size()) / origin->windowCount
           : 0},
      {"windows", unionWindows}};
  fields["refinedFusion"] =
      QJsonObject{{"version", "signalstudio-refined-publication-v1"},
                  {"maximumGapSamples", "0"},
                  {"parentIds", parentIds},
                  {"parents", snapshots},
                  {"windowCoverage", origin->windowCount
                                         ? static_cast<double>(windows.size()) /
                                               origin->windowCount
                                         : 0}};
  origin->originalFieldsJson =
      QJsonDocument(fields).toJson(QJsonDocument::Compact).toStdString();
  result.originEvidence = std::move(origin);
  return true;
}
}  // namespace

bool fuseDetectionPublication(const DetectionRequest& request,
                              DetectionPublication& publication, QString& error,
                              const DetectionSourceCancel& cancel) {
  std::vector<DetectionRefinedEvent> events;
  std::uint64_t bytes = 0;
  for (std::size_t parent = 0; parent < publication.annotations.size();
       ++parent)
    for (const auto& segment : publication.annotations[parent].segments) {
      bytes += sizeof(DetectionRefinedEvent) + segment.id.size() * 2;
      if (events.size() >= annotationSegmentBudget ||
          bytes > 16 * 1024 * 1024) {
        error = "Refined input events exceed the bounded fusion budget";
        return false;
      }
      events.push_back({segment, parent, segment.id});
    }
  if (events.empty()) return true;
  DetectionRefinedFusionOptions options;
  options.binWidthHz =
      request.sourceIdentity.sampleRateHz / request.profile.fftSize;
  options.cancelled = cancel;
  std::vector<DetectionRefinedFusion> fused;
  DetectionRefinedFusionSummary summary;
  std::string issue;
  if (!fuseRefinedDetectionEvents(events, options, fused, summary, issue)) {
    error = QString::fromStdString(issue);
    return false;
  }
  if (!summary.mergedPairs) return true;
  std::vector<std::size_t> roots(publication.annotations.size());
  std::iota(roots.begin(), roots.end(), 0);
  const auto root = [&](std::size_t parent) {
    while (roots[parent] != parent) parent = roots[parent];
    return parent;
  };
  for (const auto& event : fused) {
    const auto first =
        root(events[event.memberEventIndices.front()].parentIndex);
    for (const auto member : event.memberEventIndices)
      roots[root(events[member].parentIndex)] = first;
  }
  std::map<std::size_t, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < roots.size(); ++i) groups[root(i)].push_back(i);
  DetectionPublication output;
  output.sourceSet = publication.sourceSet;
  for (const auto& [parentRoot, indices] : groups) {
    if (cancel && cancel()) {
      error = "Cancelled during refined publication fusion";
      return false;
    }
    std::vector<Annotation> parents;
    for (const auto index : indices)
      parents.push_back(publication.annotations[index]);
    std::sort(parents.begin(), parents.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    auto annotation = parents.front();
    if (annotation.segments.empty()) {
      output.annotations.push_back(std::move(annotation));
      continue;
    }
    if (parents.size() > 128) {
      error = "Refined fusion parent lineage exceeds its bounded schema";
      return false;
    }
    QJsonArray ids;
    for (const auto& parent : parents)
      ids.append(QString::fromStdString(parent.id));
    if (parents.size() > 1) {
      annotation.id = "detection:" + digest(ids);
      annotation.originRecordId = "fusion:" + digest(ids);
      annotation.derivedFrom.clear();
      for (const auto& parent : parents)
        annotation.derivedFrom.push_back(parent.id);
      for (const auto& parent : parents) {
        if (!annotation.observation || !parent.observation) {
          error = "Refined fusion lacks parent observation";
          return false;
        }
        annotation.observation =
            united(*annotation.observation, *parent.observation);
      }
      if (!mergeOrigins(request, parents, annotation, error)) return false;
    }
    annotation.segments.clear();
    for (const auto& event : fused)
      if (root(events[event.memberEventIndices.front()].parentIndex) ==
          parentRoot)
        annotation.segments.push_back(event.segment);
    std::sort(
        annotation.segments.begin(), annotation.segments.end(),
        [](const auto& a, const auto& b) {
          return std::tie(a.range.time.begin, a.range.frequency.lowerHz, a.id) <
                 std::tie(b.range.time.begin, b.range.frequency.lowerHz, b.id);
        });
    for (std::size_t i = 0; i < annotation.segments.size(); ++i)
      annotation.segments[i].id =
          annotation.id + "/activity/" + std::to_string(i);
    annotation.activeSegmentId = annotation.segments.front().id;
    if (!annotation.originEvidence) {
      error = "Refined fusion lacks immutable origin evidence";
      return false;
    }
    auto origin = std::make_shared<OriginEvidence>(*annotation.originEvidence);
    origin->originalSegments = annotation.segments;
    annotation.originEvidence = std::move(origin);
    annotation.revisionId = annotation.id + ":r0";
    annotation.reviewRevisionId = annotation.revisionId;
    annotation.revisions = {
        {annotation.revisionId, {}, "algorithm-origin", "model", {}, {}}};
    annotation.reviewHistory = {{ReviewStatus::Pending,
                                 annotation.revisionId,
                                 "model",
                                 {},
                                 "Algorithm activity requires review"}};
    for (const auto& segment : annotation.segments)
      annotation.revisions.front().segmentChanges.push_back(
          {segment.id, std::nullopt, segment});
    refreshAnnotationProjection(annotation);
    output.annotations.push_back(std::move(annotation));
  }
  if (!validateDetectionPublicationBudget(
          output, request.workingSetBudgetBytes / 2, error))
    return false;
  publication = std::move(output);
  return true;
}
}  // namespace signalstudio
