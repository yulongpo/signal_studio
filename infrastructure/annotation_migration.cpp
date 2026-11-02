#include "infrastructure/annotation_migration.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"

namespace signalstudio {
namespace {
bool revised(const Annotation& a) {
  if (a.sourceKind == AnnotationSource::Manual) return false;
  if (std::any_of(a.revisions.begin(), a.revisions.end(), [](const auto& r) {
        return r.action == "ux-r5-freeze-original";
      })) return false;
  return std::any_of(a.revisions.begin(), a.revisions.end(), [](const auto& r) {
           return r.actor != "source" && r.actor != "model" && !r.actor.empty() &&
                  r.actor != "native-parser" && r.action != "import" &&
                  r.action != "source-publication" && r.action != "algorithm-origin";
         }) ||
         std::any_of(a.reviewHistory.begin(), a.reviewHistory.end(), [](const auto& r) {
           return r.actor != "source" && r.actor != "model" && r.actor != "native-parser" && !r.actor.empty();
         });
}
}
bool migrateRevisedSourceAnnotations(Project& project, QString& error) {
  error.clear();
  auto candidate = project;
  AnnotationService service;
  const auto stamp = std::to_string(QDateTime::currentMSecsSinceEpoch());
  for (auto& file : candidate.files) {
    const auto activeChannel = file.metadata.sampleFormat.selectedChannel;
    std::vector<std::string> changedIds;
    for (const auto& a : file.marks) if (revised(a)) changedIds.push_back(a.id);
    for (const auto& id : changedIds) {
      const auto original = *findAnnotation(file, id);
      const auto* set = findSourceSet(file, original.sourceSetId);
      if (!set || !original.originEvidence || original.deleted || set->deleted) {
        error = "旧非人工修订缺少有效来源，需人工确认；原工程未改写：" + QString::fromStdString(id);
        return false;
      }
      file.metadata.sampleFormat.selectedChannel = set->sampleChannel;
      const auto originalSourceSet = *set;
      // Historical deleted annotations require an explicit recovery decision,
      // rather than silently reviving source records.
      std::vector<std::string> created;
      const auto converted = service.convertToManual(file, {id}, {}, created);
      if (!converted || created.empty()) {
        error = "旧来源人工修订无法安全迁移：" + QString::fromStdString(id + " " + converted.error);
        return false;
      }
      auto* copy = findAnnotation(file, created.front());
      auto baseline = std::make_shared<OriginEvidence>(*copy->originEvidence);
      baseline->originalFieldsJson = QJsonDocument(QJsonObject{
          {"migration", "UX-R5 historical non-Manual user revision"},
          {"legacyEditedAnnotation", encodeAnnotation(original)},
          {"legacySourceSet", encodeSourceSet(originalSourceSet)}}).toJson(QJsonDocument::Compact).toStdString();
      copy->originEvidence = std::move(baseline);
      auto* source = findAnnotation(file, id);
      source->segments = original.originEvidence->originalSegments;
      source->observation = original.originEvidence->originalObservation;
      source->observationFrequencyFrame = original.originEvidence->observationFrequencyFrame;
      if (!source->segments.empty()) source->activeSegmentId = source->segments.front().id;
      else source->activeSegmentId.clear();
      for (const auto& r : original.revisions) {
        if (r.nameBefore) { source->name = *r.nameBefore; break; }
      }
      for (const auto& r : original.revisions) {
        if (r.noteBefore) { source->note = *r.noteBefore; break; }
      }
      const auto parent = source->revisionId;
      auto revision = id + ":r5-freeze";
      while (std::any_of(source->revisions.begin(), source->revisions.end(),
                         [&](const auto& r) { return r.id == revision; })) revision += "x";
      if (revision.size() > 128) { error = "旧来源修订身份过长，需人工确认"; return false; }
      source->revisionId = revision;
      source->reviewRevisionId = revision;
      source->reviewStatus = ReviewStatus::Pending;
      source->revisions.push_back({revision, parent, "ux-r5-freeze-original", "system-migration", stamp,
                                   "User-edited state preserved in Manual " + created.front()});
      source->reviewHistory.push_back({ReviewStatus::Pending, revision, "system-migration", stamp,
                                      "Original geometry restored; edits retained as independent Manual"});
      refreshAnnotationProjection(*source);
    }
    file.metadata.sampleFormat.selectedChannel = activeChannel;
    refreshChannelSourceRelations(file);
    if (const auto checked = validateFileAnnotations(file); !checked) {
      error = QString::fromStdString(checked.error);
      return false;
    }
  }
  project = std::move(candidate);
  return true;
}
}
