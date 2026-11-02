#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "application/annotation_service.h"

using namespace signalstudio;

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void checkResult(const AnnotationResult& result, const char* message) {
  if (!result)
    throw std::runtime_error(std::string(message) + ": " + result.error);
}
FileState fileFixture() {
  FileState file;
  file.metadata = {"file-1",    "test.iq", "test.iq", 1'000'000,
                   100'000'000, 10'000,    false};
  file.metadata.effectiveBandwidthHz = 400'000;
  file.view = fullRange(file.metadata);
  return file;
}
ViewRange geometry(SampleIndex start = 100, SampleIndex end = 500) {
  return {{start, end}, {99'900'000, 100'100'000}};
}
AnnotationSourceSet sourceFixture(
    const FileState& file, std::string id,
    AnnotationSource source = AnnotationSource::ImportedLabel) {
  AnnotationSourceSet set;
  set.id = std::move(id);
  set.fileId = file.metadata.id;
  set.sampleChannel = file.metadata.sampleFormat.selectedChannel;
  set.sourceKind = source;
  set.formatId = source == AnnotationSource::Algorithm ? "detection-v1"
                                                       : "generator-txt-v1";
  set.parserId = "fixture-parser";
  set.parserVersion = "1";
  set.profileId = "fixture-profile";
  set.profileVersion = "1";
  set.originalText = "Original source text\n";
  set.sourceHash = "sha256:" + set.id;
  set.coordinateInterpretation = "RF Hz, half-open uint64 samples";
  set.sourceIdentity = annotationSourceIdentity(file.metadata, 7);
  if (source == AnnotationSource::Algorithm) set.runId = "run:" + set.id;
  return set;
}
Annotation annotationFixture(const AnnotationSourceSet& set, std::string id,
                             std::vector<ViewRange> ranges = {geometry()}) {
  Annotation annotation;
  annotation.id = std::move(id);
  annotation.fileId = set.fileId;
  annotation.name = "Original event";
  annotation.sourceSetId = set.id;
  annotation.originRecordId = "record:" + annotation.id;
  annotation.sourceKind = set.sourceKind;
  annotation.classLabel = "64qam";
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    Segment segment;
    segment.id = annotation.id + ":segment:" + std::to_string(i);
    segment.range = ranges[i];
    segment.timeQuality = set.sourceKind == AnnotationSource::Algorithm
                              ? TimeQuality::AlgorithmEstimated
                              : TimeQuality::RoundedLabel;
    segment.frequencyQuality = set.sourceKind == AnnotationSource::Algorithm
                                   ? FrequencyQuality::ModelEstimated
                                   : FrequencyQuality::NominalLabel;
    segment.uncertaintySamples = 6;
    annotation.segments.push_back(std::move(segment));
  }
  auto evidence = std::make_shared<OriginEvidence>();
  evidence->formatId = set.formatId;
  evidence->originRef = "saved/source.txt";
  evidence->originalFieldsJson =
      R"({"TIME":[[0.0001,0.0005]],"unknown":{"retain":true}})";
  evidence->originalSegments = annotation.segments;
  evidence->coordinateInterpretation = set.coordinateInterpretation;
  if (set.sourceKind == AnnotationSource::Algorithm) {
    evidence->runId = set.runId;
    evidence->scoreMean = .8;
    evidence->scoreMax = .95;
    evidence->windowIds = {1, 3};
    evidence->windowCount = 4;
    evidence->refineMethod = RefineMethod::Sample;
    evidence->refineStatus = RefineStatus::Fallback;
    evidence->fallbackReason = "Insufficient energy support";
  }
  annotation.originEvidence = std::move(evidence);
  return annotation;
}
std::string manual(AnnotationService& service, FileState& file) {
  const auto result = service.createManual(file, "Manual event", geometry());
  checkResult(result, "Manual creation failed");
  return result.id;
}

void checkSourceUnchanged(const FileState& file, const Annotation& before) {
  const auto* source = findAnnotation(file, before.id);
  check(source && source->name == before.name && source->range == before.range &&
            source->fileId == before.fileId && source->sourceSetId == before.sourceSetId &&
            source->originRecordId == before.originRecordId &&
            source->sourceKind == before.sourceKind && source->classLabel == before.classLabel &&
            source->note == before.note && source->activeSegmentId == before.activeSegmentId &&
            source->revisionId == before.revisionId && source->segments == before.segments &&
            source->observation == before.observation &&
            source->observationFrequencyFrame == before.observationFrequencyFrame &&
            source->observationTimeQuality == before.observationTimeQuality &&
            source->originEvidence == before.originEvidence &&
            source->conversionSnapshot == before.conversionSnapshot &&
            source->reviewStatus == before.reviewStatus &&
            source->reviewRevisionId == before.reviewRevisionId &&
            source->reviewHistory == before.reviewHistory && source->revisions == before.revisions &&
            source->derivedFrom == before.derivedFrom && source->deleted == before.deleted,
        "Read-only source identity, geometry, quality, or immutable evidence changed");
}

Annotation convertSourceToManual(AnnotationService& service, FileState& file,
                                  const std::string& sourceId) {
  const auto before = *findAnnotation(file, sourceId);
  const auto setBefore = *findSourceSet(file, before.sourceSetId);
  const auto history = service.historySize(file);
  const auto editId = file.revisionEditingAnnotationId;
  const auto editRevision = file.revisionEditingBaseRevisionId;
  const auto segment = before.segments.empty() ? "missing" : before.segments.front().id;
  check(before.sourceKind != AnnotationSource::Manual && !service.canEdit(file, sourceId) &&
            !service.beginRevision(file, sourceId) &&
            !service.rename(file, sourceId, "Forbidden", "Forbidden") &&
            !service.commitGeometry(file, sourceId, segment, geometry()) &&
            !service.commitObservationFrequency(file, sourceId, {99'950'000, 100'050'000}) &&
            !service.addSegment(file, sourceId, geometry()) &&
            !service.deleteSegment(file, sourceId, segment) &&
            !service.splitSegment(file, sourceId, segment, 300) &&
            !service.mergeSegments(file, sourceId, {segment, "other"}) &&
            !service.adoptObservation(file, sourceId) &&
            !service.restoreOrigin(file, sourceId) &&
            !service.confirmSegment(file, sourceId, segment) &&
            !service.reviewBatch(file, {sourceId}, ReviewStatus::Accepted) &&
            !service.deleteBatch(file, {sourceId}),
        "Imported and Algorithm sources must reject direct writes before conversion");
  Channel channel;
  const auto request = annotationChannelRequest(file, sourceId, file.view.time);
  check(!prepareAnnotationChannel(file, request, channel) &&
            !service.bindChannelSource(file, channel, request) &&
            !service.bindChannelSource(file, channel, sourceId, segment) &&
            !channel.sourceSnapshot && channel.annotationId.empty() &&
            service.historySize(file) == history &&
            file.revisionEditingAnnotationId == editId &&
            file.revisionEditingBaseRevisionId == editRevision,
        "Read-only channel/write rejections must leave candidates, drafts, and history unchanged");
  checkSourceUnchanged(file, before);
  std::vector<std::string> created;
  const auto converted = service.convertToManual(file, {sourceId}, {}, created);
  checkResult(converted, "Explicit conversion to Manual failed");
  check(converted.changed && created.size() == 1 && service.historySize(file) == history + 1,
        "Explicit conversion must create one atomic Manual command");
  const auto copy = *findAnnotation(file, created.front());
  check(copy.id != sourceId && copy.sourceSetId != before.sourceSetId &&
            copy.revisionId != before.revisionId && copy.sourceKind == AnnotationSource::Manual &&
            service.canEdit(file, copy.id) && copy.name == before.name &&
            copy.note == before.note && copy.classLabel == before.classLabel &&
            copy.reviewStatus == before.reviewStatus && copy.segments.size() == before.segments.size() &&
            copy.observation == before.observation &&
            copy.observationFrequencyFrame == before.observationFrequencyFrame &&
            copy.observationTimeQuality == before.observationTimeQuality && copy.conversionSnapshot,
        "Conversion must preserve content and quality in an independent editable Manual copy");
  for (std::size_t i = 0; i < copy.segments.size(); ++i) {
    auto expected = before.segments[i];
    check(expected.id != copy.segments[i].id, "Copied segments require independent identities");
    expected.id = copy.segments[i].id;
    check(expected == copy.segments[i], "Conversion changed segment geometry/frame/quality");
  }
  const auto& snapshot = *copy.conversionSnapshot;
  check(snapshot.sourceAnnotationId == before.id && snapshot.sourceRevisionId == before.revisionId &&
            snapshot.sourceKind == before.sourceKind && snapshot.sourceSet == setBefore &&
            snapshot.sourceSegments == before.segments && snapshot.sourceObservation == before.observation &&
            snapshot.sourceOriginEvidence == *before.originEvidence &&
            *findSourceSet(file, setBefore.id) == setBefore,
        "Conversion must retain complete independent source evidence");
  checkSourceUnchanged(file, before);
  return copy;
}

void testLegacyCompatibility() {
  static_assert(std::is_same_v<Mark, Annotation>);
  static_assert(std::is_const_v<
                typename decltype(Annotation{}.originEvidence)::element_type>);
  auto file = fileFixture();
  file.marks.emplace_back("old-mark", "Old manual", geometry());
  Channel channel{"old-channel", "Old channel", "old-mark",
                  100'000'000,   200'000,       {100, 500}};
  file.channels.push_back(channel);
  check(attachLegacyAnnotations(file).ok,
        "Legacy migration must preserve old initializer compatibility");
  check(file.marks[0].id == "old-mark" && file.marks[0].name == "Old manual",
        "Legacy IDs and names changed");
  check(file.marks[0].segments.size() == 1 &&
            file.marks[0].segments[0].range == geometry(),
        "Legacy geometry changed");
  check(file.marks[0].originEvidence->formatId == "legacy-manual-v1",
        "Legacy origin must remain explicit");
  check(annotationConfirmed(file.marks[0]),
        "Legacy manual creation should be confirmed");
  check(file.channels[0].sourceSnapshot &&
            file.channels[0].sourceRelation == ChannelSourceRelation::Current,
        "Legacy channel must bind an immutable source snapshot");
  check(validateFileAnnotations(file).ok, "Migrated legacy file must validate");
  check(attachLegacyAnnotations(file).ok && file.sourceSets.size() == 1,
        "Migration must be idempotent");
  auto real = fileFixture();
  real.metadata.sampleFormat.structure = SampleStructure::Real;
  real.marks.emplace_back("real-mark", "Real manual",
                          ViewRange{{1, 10}, {100, 1000}});
  check(attachLegacyAnnotations(real).ok, "Real ADC legacy migration failed");
  check(real.marks.front().segments.front().frequencyFrame ==
            FrequencyFrame::RealOneSided,
        "Real migration requires one-sided frame");
}

void testSourcesAndImmutableOrigin() {
  auto file = fileFixture();
  AnnotationService service;
  const auto manualId = manual(service, file);
  check(annotationConfirmed(*findAnnotation(file, manualId)),
        "Manual creation must be Accepted");
  auto importedSet = sourceFixture(file, "imported");
  auto imported = annotationFixture(importedSet, "label-1",
                                    {geometry(), geometry(800, 900)});
  const auto raw = imported.originEvidence->originalFieldsJson;
  checkResult(service.publishSourceSet(file, importedSet, {imported}),
              "Import publication failed");
  const auto source = *findAnnotation(file, "label-1");
  const auto sourceOrigin = source.originEvidence;
  auto* label = findAnnotation(file, source.id);
  check(label->reviewStatus == ReviewStatus::Pending &&
            label->segments.size() == 2,
        "Imported burst label must be Pending and multi-segment");
  const auto copy = convertSourceToManual(service, file, source.id);
  label = findAnnotation(file, copy.id);
  const auto origin = label->originEvidence;
  const auto segmentId = label->segments.front().id;
  check(service.canEdit(file, label->id),
        "Explicit Manual copies must support direct editing");
  checkResult(service.beginRevision(file, label->id),
              "Beginning explicit revision failed");
  checkResult(
      service.commitGeometry(file, label->id, segmentId, geometry(101, 500)),
      "Revision geometry failed");
  label = findAnnotation(file, copy.id);
  check(label->sourceKind == AnnotationSource::Manual &&
            label->originEvidence == origin && label->conversionSnapshot == copy.conversionSnapshot,
        "Human revision must preserve Manual identity and both immutable evidence records");
  check(label->conversionSnapshot->sourceOriginEvidence.originalFieldsJson == raw &&
            label->conversionSnapshot->sourceOriginEvidence.originalSegments[0].range == geometry() &&
            sourceOrigin->originalFieldsJson == raw &&
            label->originEvidence->originalSegments[0].range == geometry(),
        "Unknown origin fields and original geometry changed");
  check(label->revisions.size() == 2 &&
            label->revisions.back().parentRevisionId ==
                label->revisions.front().id,
        "Revision parent chain is incomplete");
  checkResult(service.reviewBatch(file, {label->id}, ReviewStatus::Accepted),
              "Acceptance failed");
  const auto reviewCount = label->reviewHistory.size();
  const auto historyCount = service.historySize(file);
  const auto repeated =
      service.reviewBatch(file, {label->id, label->id}, ReviewStatus::Accepted);
  check(repeated.ok && !repeated.changed && file.marks.size() == 3 &&
            label->reviewHistory.size() == reviewCount &&
            service.historySize(file) == historyCount,
        "Repeated acceptance must be idempotent");
  checkResult(
      service.commitGeometry(file, label->id, segmentId, geometry(110, 500)),
      "Accepted geometry revision failed");
  check(label->reviewStatus == ReviewStatus::Pending &&
            label->reviewRevisionId == label->revisionId,
        "Accepted revision must become Pending when geometry changes");
  checkResult(service.restoreOrigin(file, label->id),
              "Restoring original geometry failed");
  check(label->segments.front().range == geometry() &&
            label->sourceKind == AnnotationSource::Manual &&
            label->conversionSnapshot->sourceKind == AnnotationSource::ImportedLabel,
        "Restoring origin must not change source identity");
  checkSourceUnchanged(file, source);
  check(validateFileAnnotations(file).ok,
        "Source invariants failed after revisions");
}

void testUnknownObservation() {
  auto file = fileFixture();
  AnnotationService service;
  auto set = sourceFixture(file, "algorithm", AnnotationSource::Algorithm);
  auto annotation = annotationFixture(set, "unknown", {});
  annotation.observation = geometry();
  annotation.observationTimeQuality = TimeQuality::WindowObservation;
  auto evidence = std::make_shared<OriginEvidence>(*annotation.originEvidence);
  evidence->originalObservation = annotation.observation;
  annotation.originEvidence = evidence;
  checkResult(service.publishSourceSet(file, set, {annotation}),
              "Observation publication failed");
  const auto source = *findAnnotation(file, "unknown");
  const auto* unknown = findAnnotation(file, source.id);
  check(unknown->segments.empty() && unknown->range == ViewRange{},
        "Unknown time must not fabricate an active rectangle");
  const auto reviewCount = unknown->reviewHistory.size();
  const auto historyCount = service.historySize(file);
  check(!service.confirmSegment(file, unknown->id, "missing") &&
            unknown->segments.empty() &&
            unknown->reviewHistory.size() == reviewCount &&
            service.historySize(file) == historyCount,
        "Confirmation must not turn an observation into a located segment");
  const auto copy = convertSourceToManual(service, file, source.id);
  unknown = findAnnotation(file, copy.id);
  checkResult(service.reviewBatch(file, {copy.id}, ReviewStatus::Accepted),
              "Unknown object can be reviewed");
  Channel channel;
  check(!service.bindChannelSource(file, channel, copy.id, "missing"),
        "Unknown time cannot bind a nonexistent located segment");
  checkResult(service.beginRevision(file, copy.id),
              "Observation revision session failed");
  checkResult(service.adoptObservation(file, copy.id),
              "Explicit observation adoption failed");
  unknown = findAnnotation(file, copy.id);
  check(unknown->segments.size() == 1 &&
            unknown->segments[0].timeQuality == TimeQuality::Manual &&
            unknown->reviewStatus == ReviewStatus::Pending,
        "Adopted observation must be manual geometry awaiting confirmation");
  check(unknown->originEvidence->originalSegments.empty() &&
            unknown->originEvidence->originalObservation == geometry(),
        "Manual adoption must retain unknown-time origin");
  checkResult(service.reviewBatch(file, {unknown->id}, ReviewStatus::Accepted),
              "Adopted observation acceptance failed");
  checkResult(service.bindChannelSource(file, channel, unknown->id,
                                        unknown->segments.front().id),
              "Adopted explicit segment should permit channel source");
  auto invalid = *unknown;
  invalid.segments[0].timeQuality = TimeQuality::Unknown;
  check(!validateAnnotation(invalid, annotationBounds(file.metadata)),
        "Unknown-time segment must be rejected");
  checkSourceUnchanged(file, source);
}

void testPendingSingleSegmentConfirmation() {
  auto file = fileFixture();
  AnnotationService service;
  const auto set = sourceFixture(file, "pending-confirmation");
  const auto imported =
      annotationFixture(set, "pending-event", {geometry(), geometry(800, 900)});
  checkResult(service.publishSourceSet(file, set, {imported}),
              "Multi-segment confirmation fixture publication failed");
  const auto copy = convertSourceToManual(service, file, imported.id);
  auto* annotation = findAnnotation(file, copy.id);
  const auto firstId = annotation->segments.front().id;
  const auto secondId = annotation->segments.back().id;
  checkResult(service.select(file, {annotation->id}, annotation->id, secondId),
              "Confirmation fixture selection failed");
  const auto before = *annotation;
  const auto sourceBefore = file.sourceSets.front();
  const auto historyBefore = service.historySize(file);
  const auto result =
      service.confirmSegment(file, annotation->id, firstId, "segment-reviewer");
  checkResult(result, "Pending activity segment confirmation failed");
  auto expectedSegments = before.segments;
  expectedSegments.front().confirmed = true;
  check(result.changed && annotation->segments == expectedSegments &&
            annotation->reviewStatus == ReviewStatus::Pending &&
            !annotationConfirmed(*annotation),
        "Confirming one Pending segment must not accept or confirm siblings");
  check(annotation->id == before.id &&
            annotation->sourceKind == before.sourceKind &&
            annotation->sourceSetId == before.sourceSetId &&
            annotation->originRecordId == before.originRecordId &&
            annotation->fileId == before.fileId &&
            annotation->name == before.name &&
            annotation->classLabel == before.classLabel &&
            annotation->note == before.note &&
            annotation->revisionId == before.revisionId &&
            annotation->reviewRevisionId == before.reviewRevisionId &&
            annotation->activeSegmentId == secondId &&
            file.activeSegmentId == secondId &&
            annotation->revisions == before.revisions &&
            annotation->originEvidence == before.originEvidence &&
            file.sourceSets.front() == sourceBefore,
        "Single-segment confirmation must preserve identity, origin, "
        "geometry revision, selection and source text");
  const auto& review = annotation->reviewHistory.back();
  check(annotation->reviewHistory.size() == before.reviewHistory.size() + 1 &&
            review.status == ReviewStatus::Pending &&
            review.revisionId == before.revisionId &&
            review.actor == "segment-reviewer" && !review.timestamp.empty() &&
            review.note == "Confirmed activity segment: " + firstId &&
            service.historySize(file) == historyBefore + 1,
        "Confirmation must record the actor and exact segment in review "
        "history without a geometry revision");
  check(segmentCanCreateChannel(*annotation, firstId) &&
            segmentCanCreateChannel(*annotation, secondId),
        "Quality and legacy confirmation do not restrict a located channel "
        "source");
  Channel confirmedChannel;
  confirmedChannel.id = "pending-channel";
  checkResult(service.bindChannelSource(file, confirmedChannel, annotation->id,
                                        firstId),
              "Confirmed Pending segment must bind a channel");
  check(
      confirmedChannel.sourceSnapshot->reviewStatus == ReviewStatus::Pending &&
          confirmedChannel.sourceSnapshot->revisionId == before.revisionId &&
          confirmedChannel.sourceSnapshot->range == before.segments[0].range,
      "Pending channel snapshot must retain its unchanged source revision");
  Channel unconfirmedChannel;
  check(service.bindChannelSource(file, unconfirmedChannel, annotation->id,
                                  secondId)
                .ok &&
            unconfirmedChannel.sourceSnapshot &&
            unconfirmedChannel.segmentId == secondId,
        "An unconfirmed sibling can be explicitly chosen without acceptance");
  const auto auditCount = annotation->reviewHistory.size();
  const auto commandCount = service.historySize(file);
  const auto repeated =
      service.confirmSegment(file, annotation->id, firstId, "another-reviewer");
  check(repeated.ok && !repeated.changed &&
            annotation->reviewHistory.size() == auditCount &&
            service.historySize(file) == commandCount,
        "Repeated confirmation must not duplicate commands or audit records");
  checkResult(
      service.reviewBatch(file, {annotation->id}, ReviewStatus::Accepted),
      "Whole-object acceptance after segment confirmation failed");
  check(annotationConfirmed(*annotation) &&
            annotation->segments.front().confirmed &&
            annotation->segments.back().confirmed &&
            segmentCanCreateChannel(*annotation, secondId),
        "Whole-object acceptance must still confirm every activity segment");
  check(validateFileAnnotations(file).ok,
        "Confirmed Pending and Accepted objects must remain valid v5 states");
}

void testConfirmationUndoRedoAndRejection() {
  auto file = fileFixture();
  AnnotationService service;
  const auto set =
      sourceFixture(file, "confirmation-history", AnnotationSource::Algorithm);
  auto event = annotationFixture(set, "confirmation-event",
                                       {geometry(), geometry(800, 900)});
  checkResult(service.publishSourceSet(file, set, {event}),
              "Confirmation history fixture publication failed");
  event = convertSourceToManual(service, file, event.id);
  auto* annotation = findAnnotation(file, event.id);
  const auto segmentId = annotation->segments.front().id;
  const auto revisionId = annotation->revisionId;
  const auto origin = annotation->originEvidence;
  checkResult(service.confirmSegment(file, event.id, segmentId),
              "Confirmation history setup failed");
  Channel channel;
  channel.id = "confirmed-history-channel";
  channel.sourceTime = geometry().time;
  checkResult(service.bindChannelSource(file, channel, event.id, segmentId),
              "Confirmation history channel binding failed");
  file.channels.push_back(channel);
  const auto snapshot = file.channels.front().sourceSnapshot;
  const auto confirmationReviews = annotation->reviewHistory;
  checkResult(service.undo(file), "Segment confirmation undo failed");
  check(!annotation->segments.front().confirmed &&
            annotation->reviewStatus == ReviewStatus::Pending &&
            annotation->revisionId == revisionId &&
            annotation->reviewHistory == confirmationReviews &&
            annotation->originEvidence == origin &&
            file.channels.front().sourceSnapshot == snapshot &&
            file.channels.front().sourceRelation ==
                ChannelSourceRelation::Current,
        "Undo must restore only confirmation state while retaining immutable "
        "review history and channel snapshot");
  checkResult(service.redo(file), "Segment confirmation redo failed");
  check(
      annotation->segments.front().confirmed &&
          !annotation->segments.back().confirmed &&
          annotation->reviewStatus == ReviewStatus::Pending &&
          annotation->reviewHistory == confirmationReviews &&
          file.channels.front().sourceRelation ==
              ChannelSourceRelation::Current,
      "Redo must restore the same single confirmation without duplicate audit");
  checkResult(service.reviewBatch(file, {event.id}, ReviewStatus::Rejected),
              "Confirmed Pending object rejection failed");
  check(
      annotation->reviewStatus == ReviewStatus::Rejected &&
          !annotation->segments.front().confirmed &&
          !annotation->segments.back().confirmed &&
          segmentCanCreateChannel(*annotation, segmentId) &&
          file.channels.front().sourceRelation ==
              ChannelSourceRelation::Current,
      "Rejection must clear confirmation and preserve the referenced channel");
  const auto rejectionReviews = annotation->reviewHistory;
  const auto rejectionHistory = service.historySize(file);
  check(
      !service.confirmSegment(file, event.id, segmentId) &&
          annotation->reviewHistory == rejectionReviews &&
          service.historySize(file) == rejectionHistory,
      "Rejected objects cannot be reconfirmed without changing review status");
  auto invalid = *annotation;
  invalid.segments.front().confirmed = true;
  check(!validateAnnotation(invalid, annotationBounds(file.metadata)),
        "A persisted Rejected confirmation must fail domain validation");
  checkResult(service.undo(file), "Rejection after confirmation undo failed");
  check(annotation->reviewStatus == ReviewStatus::Pending &&
            annotation->segments.front().confirmed &&
            !annotation->segments.back().confirmed &&
            annotation->reviewHistory == rejectionReviews &&
            file.channels.front().sourceRelation ==
                ChannelSourceRelation::Current,
        "Undo rejection must restore the exact Pending confirmation set");
  checkResult(service.redo(file), "Rejection after confirmation redo failed");
  check(
      annotation->reviewStatus == ReviewStatus::Rejected &&
          !annotation->segments.front().confirmed &&
          annotation->reviewHistory == rejectionReviews &&
          file.channels.front().sourceSnapshot == snapshot,
      "Redo rejection must retain append-only review records and DDC snapshot");
  check(validateFileAnnotations(file).ok,
        "Confirmation/rejection undo and redo left an invalid state");
}

void testConfirmationGeometryInvalidation() {
  auto file = fileFixture();
  AnnotationService service;
  const auto set = sourceFixture(file, "confirmed-geometry");
  auto event = annotationFixture(set, "geometry-event",
                                       {geometry(), geometry(800, 900)});
  checkResult(service.publishSourceSet(file, set, {event}),
              "Confirmed geometry fixture publication failed");
  event = convertSourceToManual(service, file, event.id);
  auto* annotation = findAnnotation(file, event.id);
  const auto firstId = annotation->segments.front().id;
  const auto secondId = annotation->segments.back().id;
  checkResult(service.confirmSegment(file, event.id, firstId),
              "First geometry confirmation failed");
  checkResult(service.confirmSegment(file, event.id, secondId),
              "Second geometry confirmation failed");
  checkResult(service.reviewBatch(file, {event.id}, ReviewStatus::Accepted),
              "Accepted multi-segment setup failed");
  const auto unchangedSibling = annotation->segments.front();
  const auto revisionBefore = annotation->revisionId;
  const auto origin = annotation->originEvidence;
  Channel channel;
  channel.id = "geometry-confirmation-channel";
  channel.sourceTime = geometry().time;
  checkResult(service.bindChannelSource(file, channel, event.id, firstId),
              "Confirmed geometry channel binding failed");
  file.channels.push_back(channel);
  const auto snapshot = file.channels.front().sourceSnapshot;
  checkResult(service.beginRevision(file, event.id),
              "Confirmed geometry revision session failed");
  checkResult(
      service.commitGeometry(file, event.id, secondId, geometry(810, 910)),
      "Confirmed geometry update failed");
  const auto revised = annotation->revisionId;
  const auto revisionReviews = annotation->reviewHistory;
  check(revised != revisionBefore &&
            annotation->reviewStatus == ReviewStatus::Pending &&
            annotation->reviewRevisionId == revised &&
            annotation->segments.front() == unchangedSibling &&
            !annotation->segments.back().confirmed &&
            annotation->segments.front().range == geometry() &&
            annotation->originEvidence == origin &&
            file.channels.front().sourceRelation ==
                ChannelSourceRelation::Current,
        "Editing one Accepted segment must preserve the confirmed sibling "
        "byte-for-byte and make only the changed segment Pending");
  checkResult(service.undo(file), "Confirmed geometry revision undo failed");
  check(annotation->revisionId == revisionBefore &&
            annotation->segments.front().confirmed &&
            annotation->segments.back().confirmed &&
            annotation->reviewStatus == ReviewStatus::Accepted &&
            annotation->reviewHistory == revisionReviews &&
            file.channels.front().sourceRelation ==
                ChannelSourceRelation::Current,
        "Geometry undo must restore confirmations bound to the old revision");
  checkResult(service.redo(file), "Confirmed geometry revision redo failed");
  check(annotation->revisionId == revised &&
            annotation->segments.front() == unchangedSibling &&
            !annotation->segments.back().confirmed &&
            annotation->segments.back().range == geometry(810, 910) &&
            annotation->reviewHistory == revisionReviews &&
            file.channels.front().sourceSnapshot == snapshot &&
            file.channels.front().sourceTime == geometry().time,
        "Geometry redo must retain original review/DDC evidence and require "
        "fresh confirmation");
  checkResult(service.confirmSegment(file, event.id, firstId),
              "Revised Pending segment confirmation failed");
  check(annotation->revisionId == revised &&
            annotation->reviewHistory.back().revisionId == revised &&
            segmentCanCreateChannel(*annotation, firstId) &&
            segmentCanCreateChannel(*annotation, secondId) &&
            file.channels.front().sourceRelation ==
                ChannelSourceRelation::Current,
        "New-revision confirmation must not relabel an existing old DDC "
        "snapshot");
  check(validateFileAnnotations(file).ok,
        "Geometry confirmation revisions must remain valid v5 states");
}

void testConfirmationAvailableAndEffectiveBounds() {
  auto file = fileFixture();
  file.metadata.availability.status = LoadStatus::Partial;
  file.metadata.availability.availableSamples = 1000;
  AnnotationService service;
  const auto set = sourceFixture(file, "confirmation-prefix");
  auto event =
      annotationFixture(set, "prefix-event",
                        {geometry(),
                         geometry(800, 1000),
                         geometry(900, 1001),
                         {{100, 600}, {99'600'000, 99'700'000}}});
  checkResult(service.publishSourceSet(file, set, {event}),
              "Confirmation prefix fixture publication failed");
  event = convertSourceToManual(service, file, event.id);
  auto* annotation = findAnnotation(file, event.id);
  const auto origin = annotation->originEvidence;
  const auto atBoundary = annotation->segments[1].id;
  checkResult(service.confirmSegment(file, event.id, atBoundary),
              "Confirmation ending exactly at available boundary failed");
  check(annotation->segments[1].confirmed &&
            !annotation->segments[0].confirmed &&
            !annotation->segments[2].confirmed &&
            !annotation->segments[3].confirmed,
        "Only the explicitly confirmed read-boundary segment may change");
  const auto history = service.historySize(file);
  const auto reviews = annotation->reviewHistory;
  check(!service.confirmSegment(file, event.id, annotation->segments[2].id),
        "Even one sample beyond the available prefix must reject confirmation");
  check(!service.confirmSegment(file, event.id, annotation->segments[3].id),
        "A physical label outside the effective band must not be confirmed");
  check(annotation->segments[2].range == event.segments[2].range &&
            annotation->segments[3].range == event.segments[3].range &&
            annotation->reviewHistory == reviews &&
            service.historySize(file) == history &&
            annotation->originEvidence == origin,
        "Rejected confirmations must preserve physical tails, original "
        "evidence and the complete existing command history");
  file.metadata.availability.availableSamples = 0;
  check(!service.confirmSegment(file, event.id, atBoundary),
        "Previously confirmed geometry cannot bypass a now-unavailable prefix");
  file.metadata.availability.availableSamples = 1000;
  auto* unknown = &annotation->segments[0];
  unknown->timeQuality = TimeQuality::Unknown;
  check(!service.confirmSegment(file, event.id, unknown->id),
        "Unknown-time active segments must not be confirmed");
  unknown->timeQuality = TimeQuality::RoundedLabel;
  unknown->frequencyQuality = FrequencyQuality::Unknown;
  check(!service.confirmSegment(file, event.id, unknown->id),
        "Unknown-frequency active segments must not be confirmed");
  unknown->frequencyQuality = FrequencyQuality::NominalLabel;
  check(validateFileAnnotations(file).ok,
        "Unconfirmed physical tails must remain valid while Pending has one "
        "confirmed available segment");
}

void testConfirmationCurrentSourceAndRevision() {
  auto file = fileFixture();
  file.metadata.availability.fingerprint = "original-fingerprint";
  AnnotationService service;
  const auto set = sourceFixture(file, "confirmation-source");
  auto event = annotationFixture(set, "source-event");
  checkResult(service.publishSourceSet(file, set, {event}),
              "Confirmation source fixture publication failed");
  event = convertSourceToManual(service, file, event.id);
  auto* annotation = findAnnotation(file, event.id);
  const auto segmentId = annotation->segments.front().id;
  const auto reviewCount = annotation->reviewHistory.size();
  const auto commandCount = service.historySize(file);
  const auto metadataBefore = file.metadata;
  const auto revisionId = annotation->revisionId;
  for (const auto status : {LoadStatus::Loading, LoadStatus::Failed}) {
    file.metadata.availability.status = status;
    file.metadata.availability.availableSamples = 1000;
    check(!service.confirmSegment(file, event.id, segmentId),
          "Loading/failed source cannot confirm a segment");
  }
  file.metadata = metadataBefore;
  findSourceSet(file, annotation->sourceSetId)->deleted = true;
  check(!service.confirmSegment(file, event.id, segmentId),
        "Deleted source set cannot confirm a segment");
  findSourceSet(file, annotation->sourceSetId)->deleted = false;
  file.metadata.path = "replacement.iq";
  check(!service.confirmSegment(file, event.id, segmentId),
        "Changed source path cannot confirm an old label");
  file.metadata = metadataBefore;
  file.metadata.availability.fingerprint = "changed-fingerprint";
  check(!service.confirmSegment(file, event.id, segmentId),
        "Changed source fingerprint cannot confirm an old label");
  file.metadata = metadataBefore;
  file.metadata.sampleFormat.componentEncoding = ComponentEncoding::Float32;
  check(!service.confirmSegment(file, event.id, segmentId),
        "Changed sample format cannot confirm an old label");
  file.metadata = metadataBefore;
  file.metadata.sampleFormat.channels = 2;
  file.metadata.sampleFormat.selectedChannel = 1;
  check(!service.confirmSegment(file, event.id, segmentId),
        "Another selected sample channel cannot confirm an old label");
  file.metadata = metadataBefore;
  file.metadata.sampleRateHz *= 2;
  check(!service.confirmSegment(file, event.id, segmentId),
        "Changed source sample rate cannot confirm an old label");
  file.metadata = metadataBefore;
  annotation->reviewRevisionId = "obsolete-review";
  check(!service.confirmSegment(file, event.id, segmentId),
        "Review state must bind to the current geometry revision");
  annotation->reviewRevisionId = revisionId;
  annotation->revisionId = "missing-revision";
  annotation->reviewRevisionId = annotation->revisionId;
  check(!service.confirmSegment(file, event.id, segmentId),
        "Confirmation cannot bind to a nonexistent current revision");
  annotation->segments.front().confirmed = true;
  check(!segmentCanCreateChannel(*annotation, segmentId),
        "Missing current revision must block the domain channel predicate");
  annotation->segments.front().confirmed = false;
  annotation->revisionId = revisionId;
  annotation->reviewRevisionId = revisionId;
  check(
      annotation->reviewHistory.size() == reviewCount &&
          service.historySize(file) == commandCount &&
          !annotation->segments.front().confirmed,
      "Invalid confirmation attempts must not publish review or command state");
  checkResult(service.confirmSegment(file, event.id, segmentId),
              "Restored current source must allow confirmation");
  auto invalid = *annotation;
  invalid.reviewRevisionId = "obsolete-review";
  check(!segmentCanCreateChannel(invalid, segmentId),
        "Confirmed flags cannot bypass a stale review revision");
  invalid = *annotation;
  invalid.reviewStatus = static_cast<ReviewStatus>(99);
  check(!segmentCanCreateChannel(invalid, segmentId) &&
            !validateAnnotation(invalid, annotationBounds(file.metadata)),
        "Unknown review states cannot masquerade as non-Rejected confirmation");
  checkResult(service.deleteBatch(file, {event.id}),
              "Confirmed source deletion failed");
  check(!service.confirmSegment(file, event.id, segmentId) &&
            !segmentCanCreateChannel(*annotation, segmentId),
        "Deleted annotations must not regain confirmation");
  checkResult(service.undo(file), "Confirmed source deletion undo failed");
  check(annotation->reviewStatus == ReviewStatus::Pending &&
            annotation->segments.front().confirmed &&
            annotation->reviewRevisionId == annotation->revisionId &&
            validateFileAnnotations(file).ok,
        "Deletion undo must restore the exact valid Pending confirmation");
}

void testPhysicalAndAvailableBounds() {
  auto file = fileFixture();
  file.metadata.availability.status = LoadStatus::Partial;
  file.metadata.availability.availableSamples = 1000;
  AnnotationService service;
  auto set = sourceFixture(file, "tail");
  auto annotation = annotationFixture(
      set, "tail-label", {{{8000, 9000}, {99'600'000, 99'700'000}}});
  checkResult(service.publishSourceSet(file, set, {annotation}),
              "Physical label tail outside available/effective bounds must be "
              "retained");
  const auto source = *findAnnotation(file, annotation.id);
  annotation = convertSourceToManual(service, file, source.id);
  check(!service.createManual(file, "Unavailable", geometry(1000, 2000)),
        "Manual creation cannot exceed available prefix");
  check(!service.createManual(file, "Outside effective",
                              {{1, 10}, {99'600'000, 99'700'000}}),
        "Manual creation cannot exceed effective band");
  checkResult(service.beginRevision(file, annotation.id),
              "Tail label revision session failed");
  check(!service.commitGeometry(file, annotation.id,
                                annotation.segments.front().id,
                                geometry(8000, 9000)),
        "Manual edit cannot claim unavailable samples");
  checkSourceUnchanged(file, source);
  auto invalidSet = sourceFixture(file, "physical-overflow");
  auto invalid =
      annotationFixture(invalidSet, "overrun", {geometry(9000, 11000)});
  const auto before = file.marks.size();
  check(!service.publishSourceSet(file, invalidSet, {invalid}) &&
            file.marks.size() == before && !findSourceSet(file, invalidSet.id),
        "Invalid import must leave the complete existing state intact");
  auto huge = fileFixture();
  huge.metadata.sampleCount = std::numeric_limits<SampleIndex>::max();
  auto hugeSet = sourceFixture(huge, "uint64");
  const auto start = (SampleIndex{1} << 53) + 123;
  auto exact =
      annotationFixture(hugeSet, "exact", {geometry(start, start + 99)});
  checkResult(service.publishSourceSet(huge, hugeSet, {exact}),
              "uint64 range publication failed");
  check(huge.marks[0].segments[0].range.time.begin == start,
        "Sample indices must remain exact above double integer precision");
  auto overflow = exact;
  overflow.id = "bad";
  overflow.segments[0].range.time = {std::numeric_limits<SampleIndex>::max(),
                                     0};
  check(!validateAnnotation(overflow, annotationBounds(huge.metadata)),
        "Wrapped uint64 range must be rejected");
}

void testSegmentsAndSelection() {
  auto file = fileFixture();
  AnnotationService service;
  const auto id = manual(service, file);
  auto* annotation = findAnnotation(file, id);
  const auto first = annotation->segments.front().id;
  checkResult(service.splitSegment(file, id, first, 300),
              "Segment split failed");
  check(annotation->segments.size() == 2 &&
            annotation->segments[0].range.time == TimeRange{100, 300} &&
            annotation->segments[1].range.time == TimeRange{300, 500},
        "Split must preserve exact union without overlap or gap");
  const auto second = annotation->segments[1].id;
  check(!service.splitSegment(file, id, first, 100),
        "Boundary split must reject empty segments");
  checkResult(service.select(file, {id}, id, second),
              "Segment selection failed");
  check(annotation->range == annotation->segments[1].range &&
            file.activeSegmentId == second,
        "Compatibility projection must follow one selected segment, not a "
        "bounding rectangle");
  checkResult(service.undo(file),
              "Selection should not invalidate geometry undo");
  check(annotation->segments.size() == 1 &&
            annotation->segments[0].range == geometry(),
        "Undo split must restore exact geometry");
  checkResult(service.redo(file), "Redo split failed");
  check(findSegment(*annotation, second), "Redo must reuse stable segment IDs");
  checkResult(service.mergeSegments(file, id, {first, second}),
              "Adjacent same-object merge failed");
  check(annotation->segments.size() == 1 &&
            annotation->segments[0].range == geometry(),
        "Merge changed total physical bounds");
  checkResult(service.addSegment(file, id, geometry(700, 800)),
              "Adding burst segment failed");
  const auto third = annotation->segments.back().id;
  checkResult(service.mergeSegments(file, id, {first, third}),
              "Explicit same-object gap merge failed");
  check(annotation->revisions.back().action ==
                "merge-segments-including-gap-manually" &&
            !annotation->revisions.back().note.empty(),
        "Including a gap must record the explicit manual decision");
  checkResult(service.addSegment(file, id, geometry(1000, 1100)),
              "Adding deletable segment failed");
  const auto last = annotation->segments.back().id;
  checkResult(service.deleteSegment(file, id, last),
              "Deleting a segment failed");
  check(annotation->segments.size() == 1,
        "Delete segment must remain inside the same object");
  check(!service.mergeSegments(file, id, {first, "other-object-segment"}),
        "Merge must reject another object's segment");
}

void testUnifiedSelectionOrderFilterAndHover() {
  auto file = fileFixture();
  AnnotationService service;
  const auto set =
      sourceFixture(file, "selection", AnnotationSource::Algorithm);
  std::vector<Annotation> values;
  std::vector<std::string> order;
  for (int i = 0; i < 268; ++i) {
    const auto id = "selection:" + std::to_string(i);
    values.push_back(annotationFixture(set, id));
    order.push_back(id);
  }
  checkResult(service.publishSourceSet(file, set, std::move(values)),
              "Selection fixture publication failed");
  checkResult(service.chooseSelection(file, order[5],
                                      AnnotationSelectionMode::Replace, order),
              "Plain selection failed");
  const auto anchor = file.selectionAnchorId;
  checkResult(service.chooseSelection(file, order[210],
                                      AnnotationSelectionMode::Range, order),
              "Cross-page range failed");
  check(file.selectedMarkIds.size() == 206 && file.activeMarkId == order[210] &&
            file.selectionAnchorId == anchor,
        "Shift must use the complete initiating order and preserve its anchor");
  checkResult(service.chooseSelection(file, order[5],
                                      AnnotationSelectionMode::Toggle, order),
              "Toggle failed");
  check(file.selectedMarkIds.size() == 205 && file.activeMarkId == order[210] &&
            file.selectionAnchorId == order[5],
        "Removing a noncurrent object must retain the current object");
  auto reversed = order;
  std::reverse(reversed.begin(), reversed.end());
  checkResult(service.chooseSelection(file, order[2],
                                      AnnotationSelectionMode::Range, reversed),
              "Reverse-view range failed");
  check(
      file.selectedMarkIds == std::vector<std::string>(reversed.begin() + 262,
                                                       reversed.begin() + 266),
      "Range selection must follow the initiating view, including direction");
  const auto ids = file.selectedMarkIds;
  const auto active = file.activeMarkId;
  const auto history = service.historySize(file);
  const auto* target = findAnnotation(file, order[100]);
  check(service.hover(file, target->id, target->activeSegmentId),
        "Hover must publish a separate transient identity");
  check(
      file.selectedMarkIds == ids && file.activeMarkId == active &&
          service.historySize(file) == history &&
          file.hoveredSegmentId == target->activeSegmentId,
      "Hover must not alter selection, current segment, geometry, or history");
  checkResult(service.select(file, order, order[267]),
              "All-matching selection failed");
  check(file.selectedMarkIds.size() == 268,
        "Selection must not have a table page or tree preview cap");
  file.annotationFilter.minimumMean = .9;
  service.reconcileSelection(file);
  check(file.selectedMarkIds == order && file.activeMarkId == order[267] &&
            file.hoveredMarkId == target->id,
        "A table filter must not invalidate legal graph/tree selection or overlay hover");
  std::vector<std::string> matching;
  for (const auto& id : order)
    if (annotationEligibleForTab(file, *findAnnotation(file, id), AnnotationSource::Algorithm))
      matching.push_back(id);
  check(matching.empty() && annotationCounts(file).total == 268 &&
            !service.chooseSelection(file, order[100], AnnotationSelectionMode::Replace, matching),
        "The table's filtered ID order must exclude below-threshold rows");
  checkResult(service.select(file, matching), "Applying the table filter intersection failed");
  check(file.selectedMarkIds.empty() && file.activeMarkId.empty() &&
            file.activeSegmentId.empty() && file.selectionAnchorId.empty() &&
            file.hoveredMarkId == target->id &&
            annotationCounts(file).total == 268,
        "The table must intersect its selection explicitly while overlay hover stays independent");
  file.annotationFilter.minimumMean = 0;
  service.reconcileSelection(file);
  check(file.selectedMarkIds.empty(),
        "Restoring a filter must not resurrect hidden selection");
  checkResult(service.select(file, order), "Select before hiding layer failed");
  file.annotationLayers[2] = false;
  service.reconcileSelection(file);
  service.hover(file, target->id, target->activeSegmentId);
  check(file.selectedMarkIds == order && file.activeMarkId == order.back() &&
            file.hoveredMarkId.empty() && annotationCounts(file).matching == order.size() &&
            annotationCounts(file).selected == order.size() &&
            annotationSelectable(file, *target) &&
            annotationEligibleForTab(file, *target, AnnotationSource::Algorithm) &&
            !annotationOverlayVisible(file, *target) && !annotationVisible(file, *target),
        "A hidden overlay must retain table selection and eligibility while preventing graph hover/drawing");
  checkResult(service.chooseSelection(file, order[210], AnnotationSelectionMode::Replace, order),
              "A table row with a hidden overlay must remain selectable");
  check(file.selectedMarkIds == std::vector<std::string>{order[210]},
        "Table selection must not depend on the graph layer switch");
  file.annotationLayers[2] = true;
  checkResult(service.select(file, {order[0]}, order[0]),
              "Select before sample-channel change failed");
  service.hover(file, order[1]);
  file.metadata.sampleFormat.channels = 2;
  file.metadata.sampleFormat.selectedChannel = 1;
  service.reconcileSelection(file);
  check(file.selectedMarkIds.empty() && file.selectionAnchorId.empty() &&
            file.hoveredMarkId.empty() &&
            annotationCounts(file).matching == 0 &&
            annotationCounts(file).total == 0,
        "Selection and matching counts must be confined to one sample channel");
  const auto other = manual(service, file);
  checkResult(service.select(file, {other}, other),
              "New-channel selection failed");
  check(annotationCounts(file).total == 1 && file.activeMarkId == other,
        "The new channel must select only its own objects");
  file.metadata.sampleFormat.selectedChannel = 0;
  service.reconcileSelection(file);
  check(file.selectedMarkIds.empty() && annotationCounts(file).total == 268,
        "Changing sample channel back must not revive a stale selection");
}

void testAtomicBatchAndConcurrentUndo() {
  auto file = fileFixture();
  AnnotationService service;
  const auto id = manual(service, file);
  const auto segmentId = file.marks.front().segments.front().id;
  auto set =
      sourceFixture(file, "algorithm-arrival", AnnotationSource::Algorithm);
  auto algorithm = annotationFixture(set, "algorithm-after-old-command",
                                     {geometry(1000, 2000)});
  checkResult(service.publishSourceSet(file, set, {algorithm}),
              "Concurrent algorithm publication failed");
  checkResult(service.undo(file), "Undo old manual creation failed");
  check(!findAnnotation(file, id) && findAnnotation(file, algorithm.id) &&
            findSourceSet(file, set.id),
        "Old-command undo must preserve algorithm objects and sets appended "
        "later");
  checkResult(service.redo(file), "Old-command redo failed");
  check(file.marks.size() == 2 &&
            findSegment(*findAnnotation(file, id), segmentId),
        "Redo must keep both sources and stable IDs");
  const auto sourceBefore = *findAnnotation(file, algorithm.id);
  const auto copy = convertSourceToManual(service, file, algorithm.id);
  auto* manualObject = findAnnotation(file, id);
  auto* algorithmObject = findAnnotation(file, algorithm.id);
  auto* copiedObject = findAnnotation(file, copy.id);
  const auto oldManual = manualObject->reviewStatus;
  const auto oldAlgorithm = algorithmObject->reviewStatus;
  const auto oldCopy = copiedObject->reviewStatus;
  check(!service.reviewBatch(file, {id, algorithm.id, "missing"},
                             ReviewStatus::Rejected),
        "Missing batch review target must reject");
  check(manualObject->reviewStatus == oldManual &&
            algorithmObject->reviewStatus == oldAlgorithm,
        "Failed batch review must not partially change valid objects");
  const auto historyBefore = service.historySize(file);
  check(!service.reviewBatch(file, {id, algorithm.id}, ReviewStatus::Rejected) &&
            !service.deleteBatch(file, {id, algorithm.id}) &&
            manualObject->reviewStatus == oldManual && !manualObject->deleted &&
            service.historySize(file) == historyBefore,
        "A read-only member must atomically reject otherwise valid Manual review/deletion targets");
  checkSourceUnchanged(file, sourceBefore);
  check(!service.reviewBatch(file, {id, copy.id, "missing"}, ReviewStatus::Rejected) &&
            manualObject->reviewStatus == oldManual && copiedObject->reviewStatus == oldCopy,
        "A missing member after valid Manual targets must reject the entire review batch");
  checkResult(
      service.reviewBatch(file, {id, copy.id}, ReviewStatus::Rejected),
      "Atomic review batch failed");
  check(manualObject->reviewStatus == ReviewStatus::Rejected &&
            copiedObject->reviewStatus == ReviewStatus::Rejected &&
            algorithmObject->reviewStatus == oldAlgorithm,
        "Batch review should change exactly the selected objects");
  checkResult(service.undo(file), "Atomic review undo failed");
  check(manualObject->reviewStatus == oldManual &&
            copiedObject->reviewStatus == oldCopy && algorithmObject->reviewStatus == oldAlgorithm,
        "Undo batch review must restore all selected objects together");
  check(!service.deleteBatch(file, {id, "missing"}) && !manualObject->deleted,
        "Batch delete must prevalidate all targets");
  check(!service.deleteBatch(file, {id, copy.id, "missing"}) &&
            !manualObject->deleted && !copiedObject->deleted,
        "A missing member after valid Manual targets must reject the entire deletion batch");
  checkResult(service.deleteBatch(file, {id, copy.id}),
              "Soft deletion failed");
  check(file.marks.size() == 3 && manualObject->deleted &&
            copiedObject->deleted && !algorithmObject->deleted,
        "Deletion must preserve immutable source evidence");
  checkResult(service.undo(file), "Soft deletion undo failed");
  check(!manualObject->deleted && !copiedObject->deleted && !algorithmObject->deleted,
        "Soft deletion undo failed to restore objects");
  checkSourceUnchanged(file, sourceBefore);
  check(validateFileAnnotations(file).ok,
        "Concurrent history left invalid state");
}

void testChannelSnapshotsAndSourceChanges() {
  auto file = fileFixture();
  AnnotationService service;
  const auto id = manual(service, file);
  const auto segment = findAnnotation(file, id)->segments.front().id;
  Channel channel;
  channel.id = "ddc";
  channel.sourceTime = {100, 500};
  checkResult(service.bindChannelSource(file, channel, id, segment),
              "Channel source binding failed");
  file.channels.push_back(channel);
  const auto snapshot = file.channels.front().sourceSnapshot;
  checkResult(service.commitGeometry(file, id, segment, geometry(150, 550)),
              "Source revision failed");
  check(
      file.channels.front().sourceRelation == ChannelSourceRelation::Changed &&
          file.channels.front().sourceSnapshot == snapshot &&
          snapshot->range == geometry() &&
          file.channels.front().sourceTime == TimeRange{100, 500},
      "Source revision must not recompute channel geometry");
  checkResult(service.undo(file), "Source revision undo failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Current,
        "Undo should restore current relation");
  checkResult(service.reviewBatch(file, {id}, ReviewStatus::Rejected),
              "Rejection failed");
  check(
      file.channels.front().sourceRelation == ChannelSourceRelation::Current &&
          file.channels.size() == 1,
      "Rejected source must retain DDC and its snapshot");
  checkResult(service.undo(file), "Rejection undo failed");
  checkResult(service.deleteBatch(file, {id}),
              "Referenced source soft deletion failed");
  check(file.channels.size() == 1 && file.channels.front().sourceRelation ==
                                         ChannelSourceRelation::Missing,
        "Referenced source deletion must not cascade to DDC");
  checkResult(service.undo(file), "Referenced source restoration failed");
  file.metadata.effectiveBandwidthHz = 500'000;
  refreshChannelSourceRelations(file);
  check(
      file.channels.front().sourceRelation == ChannelSourceRelation::Current &&
          service.canEdit(file, id),
      "Changing the visible analysis band does not change sample identity");
  file.metadata.effectiveBandwidthHz = file.metadata.sampleRateHz;
  file.metadata.sampleFormat.componentEncoding = ComponentEncoding::Float32;
  refreshChannelSourceRelations(file);
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Changed,
        "Changed source format must invalidate source relation");
  file.metadata.sampleFormat.componentEncoding = ComponentEncoding::Int16;
  file.metadata.sampleFormat.channels = 2;
  file.metadata.sampleFormat.selectedChannel = 1;
  refreshChannelSourceRelations(file);
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Changed,
        "Changed selected sample channel must be recognized");
  check(!service.bindChannelSource(file, channel, id, segment),
        "Cannot rebind an annotation from another sample channel");
  check(!service.canEdit(file, id),
        "Geometry edits must not target another sample channel's source");
  const auto newId = manual(service, file);
  const auto* newSet =
      findSourceSet(file, findAnnotation(file, newId)->sourceSetId);
  check(newSet && newSet->sampleChannel == 1,
        "New manual marks must bind the newly selected sample channel");
  check(validateFileAnnotations(file).ok,
        "Historical source sets must survive channel changes");
}

void testSourceIdempotenceAndValidation() {
  auto file = fileFixture();
  AnnotationService service;
  auto set = sourceFixture(file, "source-a");
  auto annotation = annotationFixture(set, "label-a");
  auto mutableAlias =
      std::make_shared<OriginEvidence>(*annotation.originEvidence);
  annotation.originEvidence = mutableAlias;
  checkResult(service.publishSourceSet(file, set, {annotation}),
              "Initial source publication failed");
  const auto publishedRaw = file.marks[0].originEvidence->originalFieldsJson;
  mutableAlias->originalFieldsJson = "Caller mutates its own parse result";
  check(file.marks[0].originEvidence->originalFieldsJson == publishedRaw,
        "Publication must detach evidence from a caller's mutable alias");
  auto duplicateSet = set;
  duplicateSet.id = "different-local-id";
  auto duplicate = annotationFixture(duplicateSet, "different-label-id");
  const auto result = service.publishSourceSet(file, duplicateSet, {duplicate});
  check(result.ok && !result.changed && result.id == set.id &&
            file.marks.size() == 1,
        "Same immutable source imported twice must be idempotent");
  auto conflict = set;
  conflict.sourceHash = "different-content";
  check(!service.publishSourceSet(file, conflict, {annotation}),
        "Same source-set ID with changed content must reject");
  auto changed = sourceFixture(file, "changed-source");
  changed.sourceIdentity.sampleFormat.componentEncoding =
      ComponentEncoding::Float32;
  check(!service.publishSourceSet(
            file, changed, {annotationFixture(changed, "changed-label")}),
        "Publication must validate source identity");
  auto badEvidence =
      std::make_shared<OriginEvidence>(*file.marks[0].originEvidence);
  badEvidence->scoreMean = 1.;
  auto invalid = file.marks[0];
  invalid.originEvidence = badEvidence;
  check(!validateAnnotation(invalid, annotationBounds(file.metadata)),
        "Imported source must not contain fabricated model scores");
  auto algorithmSet =
      sourceFixture(file, "scores", AnnotationSource::Algorithm);
  auto algorithm = annotationFixture(algorithmSet, "bad-scores");
  auto evidence = std::make_shared<OriginEvidence>(*algorithm.originEvidence);
  evidence->scoreMax = .7;
  algorithm.originEvidence = evidence;
  check(!service.publishSourceSet(file, algorithmSet, {algorithm}),
        "scoreMax lower than scoreMean must reject");
  evidence->scoreMax = .9;
  evidence->windowIds = {1, 1};
  check(!service.publishSourceSet(file, algorithmSet, {algorithm}),
        "Duplicate model window IDs must reject");
  evidence->windowIds = {1, 2};
  evidence->windowCount = 1;
  check(!service.publishSourceSet(file, algorithmSet, {algorithm}),
        "Hits cannot exceed coverage denominator");
  auto invalidRevision = file.marks[0];
  invalidRevision.reviewRevisionId = "different-revision";
  check(!validateAnnotation(invalidRevision, annotationBounds(file.metadata)),
        "Review must bind current revision");
  invalidRevision = file.marks[0];
  invalidRevision.revisions[0].parentRevisionId = "future-parent";
  check(!validateAnnotation(invalidRevision, annotationBounds(file.metadata)),
        "Missing revision parent must reject");
  auto baseband = sourceFixture(file, "baseband");
  auto basebandAnnotation = annotationFixture(baseband, "bb");
  basebandAnnotation.segments[0].range.frequency = {-100'000, 100'000};
  basebandAnnotation.segments[0].frequencyFrame = FrequencyFrame::Baseband;
  auto basebandEvidence =
      std::make_shared<OriginEvidence>(*basebandAnnotation.originEvidence);
  basebandEvidence->originalSegments = basebandAnnotation.segments;
  basebandAnnotation.originEvidence = basebandEvidence;
  checkResult(service.publishSourceSet(file, baseband, {basebandAnnotation}),
              "Negative baseband frequencies should be legal");
  check(findAnnotation(file, "bb")->segments[0].range.frequency.lowerHz ==
            -100'000,
        "Validation must not modify stored frequency coordinates");
  check(!service.canUndo(file) && !service.canRedo(file) && service.historySize(file) == 0 &&
            !service.undo(file) && file.marks.size() == 2 &&
            findSourceSet(file, set.id) && findSourceSet(file, baseband.id),
        "Imported source management must not enter Manual undo or remove/resurrect system sources");
}

void testBudgetsAndIndependentHistories() {
  auto file = fileFixture();
  AnnotationService service;
  const auto id = manual(service, file);
  for (int i = 0; i < 55; ++i)
    checkResult(service.rename(file, id, "Name " + std::to_string(i), "note"),
                "History stress revision failed");
  check(service.historySize(file) == annotationCommandBudget,
        "Annotation history must stay at 40 commands");
  for (std::size_t i = 0; i < annotationCommandBudget; ++i)
    checkResult(service.undo(file), "Bounded history undo failed");
  check(!service.canUndo(file) && service.canRedo(file),
        "History boundary is incorrect");
  auto other = fileFixture();
  other.metadata.id = "file-2";
  check(!service.canUndo(other),
        "Annotation histories must be isolated by file");
  const auto otherId = manual(service, other);
  check(service.canUndo(other) && findAnnotation(other, otherId),
        "Second-file history failed");
  service.clear(other.metadata.id);
  check(!service.canUndo(other) && service.canRedo(file),
        "Clearing a file history must not affect another file");
  auto large = fileFixture();
  auto set = sourceFixture(large, "large");
  auto many = annotationFixture(set, "many", {});
  many.segments.reserve(annotationSegmentBudget);
  for (std::size_t i = 0; i < annotationSegmentBudget; ++i) {
    Segment segment;
    segment.id = "s:" + std::to_string(i);
    segment.range = geometry();
    segment.timeQuality = TimeQuality::SampleLabel;
    segment.frequencyQuality = FrequencyQuality::NominalLabel;
    many.segments.push_back(std::move(segment));
  }
  auto origin = std::make_shared<OriginEvidence>(*many.originEvidence);
  origin->originalSegments = many.segments;
  many.originEvidence = origin;
  checkResult(service.publishSourceSet(large, set, {many}),
              "100000 segment pressure fixture must publish");
  check(large.marks[0].segments.size() == annotationSegmentBudget &&
            validateFileAnnotations(large).ok,
        "100000 segment pressure fixture must remain valid");
  check(!service.createManual(large, "Too many", geometry()),
        "Additional segment beyond 100000 must reject");
  check(large.marks.size() == 1,
        "Budget rejection must not partially publish state");
}
void testDirectEditsPreserveOriginAndMetadataQuality() {
  for (const auto source :
       {AnnotationSource::ImportedLabel, AnnotationSource::Manual,
        AnnotationSource::Algorithm}) {
    auto file = fileFixture();
    AnnotationService service;
    std::string id;
    std::optional<Annotation> originalSource;
    if (source == AnnotationSource::Manual) {
      id = manual(service, file);
    } else {
      const auto set = sourceFixture(file, "direct-source", source);
      checkResult(service.publishSourceSet(
                      file, set, {annotationFixture(set, "direct-event")}),
                  "Direct-edit publication failed");
      originalSource = *findAnnotation(file, "direct-event");
      id = convertSourceToManual(service, file, originalSource->id).id;
    }
    auto* annotation = findAnnotation(file, id);
    const auto origin = annotation->originEvidence;
    const auto originId = annotation->originRecordId;
    const auto setId = annotation->sourceSetId;
    const auto segmentId = annotation->segments.front().id;
    for (const auto review : {ReviewStatus::Accepted, ReviewStatus::Rejected}) {
      checkResult(service.reviewBatch(file, {id}, review),
                  "Review setup failed");
      const auto confirmed = annotation->segments.front().confirmed;
      checkResult(
          service.rename(file, id, "Name " + std::to_string(int(review)),
                         "Edited note"),
          "Manual creation and explicit source copies must allow direct metadata edits");
      check(annotation->reviewStatus == review &&
                annotation->segments.front().confirmed == confirmed &&
                annotation->reviewRevisionId == annotation->revisionId,
            "Metadata revisions must preserve quality and confirmation");
    }
    const auto history = service.historySize(file);
    const auto revisionCount = annotation->revisions.size();
    check(
        !service.rename(file, id, annotation->name, annotation->note).changed &&
            !service
                 .commitGeometry(file, id, segmentId,
                                 annotation->segments.front().range)
                 .changed &&
            service.historySize(file) == history &&
            annotation->revisions.size() == revisionCount,
        "Unchanged submissions must not create commands or revisions");
    auto next = annotation->segments.front().range;
    next.frequency.lowerHz += .125;
    const auto originalUpper = next.frequency.upperHz;
    checkResult(service.commitGeometry(file, id, segmentId, next),
                "Direct geometry edit failed");
    next.time.begin += 1;
    checkResult(service.commitGeometry(file, id, segmentId, next),
                "Consecutive direct edit must not require beginRevision");
    check(annotation->reviewStatus == ReviewStatus::Pending &&
              annotation->segments.front().range.frequency.upperHz ==
                  originalUpper &&
              annotation->originEvidence == origin &&
              annotation->originRecordId == originId &&
              annotation->sourceSetId == setId &&
              annotation->sourceKind == AnnotationSource::Manual &&
              file.marks.size() == (originalSource ? 2u : 1u),
          "Direct edits must preserve provenance, precision, and object "
          "identity");
    checkResult(service.undo(file), "Geometry undo failed");
    check(
        annotation->segments.front().range.time.begin == geometry().time.begin,
        "One geometry submission must correspond to one undo");
    check(validateFileAnnotations(file).ok, "Direct edit invariant failed");
    if (originalSource) {
      checkSourceUnchanged(file, *originalSource);
      check(annotation->conversionSnapshot &&
                annotation->conversionSnapshot->sourceKind == source &&
                annotation->conversionSnapshot->sourceOriginEvidence == *originalSource->originEvidence,
            "Repeated Manual edits and undo must preserve the actual Imported/Algorithm provenance");
    }
  }
}

void testUnknownFrequencyAndEditContext() {
  auto file = fileFixture();
  file.metadata.sampleFormat.channels = 2;
  AnnotationService service;
  const auto set =
      sourceFixture(file, "frequency-only", AnnotationSource::Algorithm);
  auto value = annotationFixture(set, "observation-only", {});
  value.observation = geometry();
  value.observationTimeQuality = TimeQuality::WindowObservation;
  auto evidence = std::make_shared<OriginEvidence>(*value.originEvidence);
  evidence->originalObservation = value.observation;
  value.originEvidence = evidence;
  checkResult(service.publishSourceSet(file, set, {value}),
              "Observation setup failed");
  const auto publishedOrigin = findAnnotation(file, value.id)->originEvidence;
  const auto originalEvidence = *publishedOrigin;
  check(originalEvidence == *evidence,
        "Publication must retain the complete fixture evidence contents");
  const auto sourceBefore = *findAnnotation(file, value.id);
  value = convertSourceToManual(service, file, value.id);
  const auto manualOrigin = value.originEvidence;
  checkResult(service.select(file, {value.id}, value.id),
              "Observation selection failed");
  const auto context = annotationEditContext(file, value.id, 23);
  check(annotationEditContextMatches(file, context, 23),
        "Draft context must match");
  check(!annotationEditContextMatches(file, context, 24),
        "A replacement project must invalidate a draft");
  const FrequencyRange next{99'950'000.125, 100'090'000.875};
  checkResult(service.commitObservationFrequency(file, value.id, next),
              "Unknown-time frequency edit failed");
  const auto* annotation = findAnnotation(file, value.id);
  check(annotation->segments.empty() && annotation->range == ViewRange{} &&
            annotation->observation->time == geometry().time &&
            annotation->observation->frequency == next &&
            annotation->observationTimeQuality ==
                TimeQuality::WindowObservation &&
            annotation->originEvidence == manualOrigin &&
            annotation->conversionSnapshot->sourceOriginEvidence == originalEvidence &&
            findAnnotation(file, sourceBefore.id)->originEvidence == publishedOrigin,
        "Frequency editing must retain unknown activity time and origin");
  check(!annotationEditContextMatches(file, context, 23),
        "A newer object revision must invalidate an old draft");
  const auto revision = annotation->revisionId;
  check(!service.commitObservationFrequency(file, value.id, next).changed &&
            annotation->revisionId == revision,
        "Unchanged frequency must not create a revision");
  check(!service.commitObservationFrequency(file, value.id, {100e6, 100e6}),
        "Empty frequency must be rejected atomically");
  check(annotation->observation->frequency == next,
        "Rejected frequency must leave geometry unchanged");
  const auto current = annotationEditContext(file, value.id, 23);
  file.metadata.sampleFormat.selectedChannel = 1;
  check(!annotationEditContextMatches(file, current, 23) &&
            !service.canEdit(file, value.id),
        "Switching sample channels must invalidate editing");
  file.metadata.sampleFormat.selectedChannel = 0;
  file.metadata.centerFrequencyHz += 100;
  check(!service.canEdit(file, value.id),
        "Physical source identity validation remains required");
  checkSourceUnchanged(file, sourceBefore);
}

void testSegmentDeletionSnapshotAndConcurrentUndo() {
  auto file = fileFixture();
  AnnotationService service;
  const auto set =
      sourceFixture(file, "delete-source", AnnotationSource::Algorithm);
  auto value =
      annotationFixture(set, "delete-event", {geometry(), geometry(700, 900)});
  checkResult(service.publishSourceSet(file, set, {value}),
              "Delete fixture failed");
  const auto sourceBefore = *findAnnotation(file, value.id);
  value = convertSourceToManual(service, file, value.id);
  checkResult(service.reviewBatch(file, {value.id}, ReviewStatus::Accepted),
              "Delete confirmation failed");
  auto* annotation = findAnnotation(file, value.id);
  const auto first = annotation->segments.front().id;
  const auto second = annotation->segments.back().id;
  Channel channel;
  channel.id = "retained-channel";
  checkResult(service.bindChannelSource(file, channel, value.id, first),
              "Channel snapshot setup failed");
  file.channels.push_back(channel);
  const auto snapshot = channel.sourceSnapshot;
  checkResult(service.deleteSegment(file, value.id, first),
              "Direct segment deletion failed");
  check(
      file.channels.front().sourceRelation == ChannelSourceRelation::Missing &&
          file.channels.front().sourceSnapshot == snapshot &&
          file.channels.size() == 1,
      "Deleting a bound segment must retain its channel and immutable "
      "snapshot");
  const auto newer =
      sourceFixture(file, "later-worker", AnnotationSource::Algorithm);
  checkResult(service.publishSourceSet(
                  file, newer, {annotationFixture(newer, "later-result")}),
              "Concurrent worker publication failed");
  checkResult(service.undo(file), "Segment deletion undo failed");
  annotation = findAnnotation(file, value.id);
  check(annotation->segments.size() == 2 &&
            findAnnotation(file, "later-result") &&
            file.channels.front().sourceRelation ==
                ChannelSourceRelation::Current,
        "Undo must restore only touched IDs and keep later worker results");
  checkResult(service.redo(file), "Segment deletion redo failed");
  checkResult(service.deleteSegment(file, value.id, second),
              "Last segment deletion failed");
  annotation = findAnnotation(file, value.id);
  check(annotation->deleted && annotation->segments.size() == 1 &&
            file.channels.front().sourceSnapshot == snapshot,
        "Deleting the last segment must soft-delete its object with geometry "
        "retained");
  checkResult(service.undo(file), "Last segment deletion undo failed");
  check(!findAnnotation(file, value.id)->deleted &&
            findAnnotation(file, "later-result"),
        "One undo must restore last-segment object deletion");
  checkSourceUnchanged(file, sourceBefore);
}

void testRestoreOriginPreservesConfirmedSibling() {
  auto file = fileFixture();
  AnnotationService service;
  const auto set =
      sourceFixture(file, "restore-sibling", AnnotationSource::Algorithm);
  auto value = annotationFixture(set, "restore-object",
                                       {geometry(), geometry(700, 900)});
  checkResult(service.publishSourceSet(file, set, {value}),
              "Restore fixture failed");
  const auto sourceBefore = *findAnnotation(file, value.id);
  value = convertSourceToManual(service, file, value.id);
  checkResult(service.reviewBatch(file, {value.id}, ReviewStatus::Accepted),
              "Restore Accepted setup failed");
  auto* annotation = findAnnotation(file, value.id);
  const auto first = annotation->segments.front();
  const auto second = annotation->segments.back();
  const auto origin = annotation->originEvidence;
  checkResult(service.select(file, {value.id}, value.id, second.id),
              "Restore current-segment setup failed");
  checkResult(
      service.commitGeometry(file, value.id, second.id, geometry(710, 910)),
      "Restore second-segment edit failed");
  const auto edited = annotation->segments.back();
  const auto beforeRevision = annotation->revisionId;
  const auto beforeHistory = service.historySize(file);
  checkResult(service.restoreOrigin(file, value.id),
              "Restore original geometry failed");
  check(annotation->segments.front() == first &&
            annotation->segments.back().range == second.range &&
            !annotation->segments.back().confirmed &&
            annotation->activeSegmentId == second.id &&
            annotation->reviewStatus == ReviewStatus::Pending &&
            annotation->originEvidence == origin &&
            service.historySize(file) == beforeHistory + 1,
        "Restoring changed geometry must retain the entire confirmed sibling "
        "and origin");
  checkResult(service.undo(file), "Restore geometry undo failed");
  check(annotation->segments.front() == first &&
            annotation->segments.back() == edited &&
            annotation->revisionId == beforeRevision,
        "One undo must restore exactly the geometry before restoration");
  checkSourceUnchanged(file, sourceBefore);
}

void testLocatedChannelRequestsAndRelations() {
  auto file = fileFixture();
  file.metadata.availability = {
      1000, LoadStatus::Loading, 0, "source-hash", {}};
  AnnotationService service;
  const auto set =
      sourceFixture(file, "channel-requests", AnnotationSource::Algorithm);
  auto event = annotationFixture(set, "request-event",
                                       {geometry(), geometry(800, 1400)});
  checkResult(service.publishSourceSet(file, set, {event}),
              "Channel request fixture failed");
  const auto sourceBefore = *findAnnotation(file, event.id);
  event = convertSourceToManual(service, file, event.id);
  auto* annotation = findAnnotation(file, event.id);
  const auto first = annotation->segments.front().id;
  const auto second = annotation->segments.back().id;
  auto request = annotationChannelRequest(file, event.id, {0, 1000}, 7, second);
  Channel candidate;
  check(request.range.time == TimeRange{800, 1400} &&
            !prepareAnnotationChannel(file, request, candidate) &&
            prepareAnnotationChannel(file, request, candidate).field ==
                "sourceTime",
        "Located defaults must retain the exact segment, including its unread "
        "tail");

  request = annotationChannelRequest(file, event.id, {0, 1000}, 7, first);
  request.range.time = {150, 450};
  request.range.frequency = {99.95e6, 100.05e6};
  const auto before = *annotation;
  const auto history = service.historySize(file);
  checkResult(service.bindChannelSource(file, candidate, request),
              "A Pending segment in the frozen Loading prefix must bind");
  check(candidate.sourceTime == request.range.time &&
            candidate.sourceSnapshot &&
            candidate.sourceSnapshot->range == request.range &&
            candidate.sourceSnapshot->boundRange == geometry() &&
            candidate.sourceSnapshot->bindingKind ==
                ChannelBindingKind::LocatedSegment &&
            annotation->revisionId == before.revisionId &&
            annotation->reviewStatus == before.reviewStatus &&
            annotation->reviewHistory == before.reviewHistory &&
            annotation->originEvidence == before.originEvidence &&
            service.historySize(file) == history,
        "Creation must save crop and complete binding without changing review "
        "or evidence");
  file.channels.push_back(candidate);
  const auto snapshot = file.channels.front().sourceSnapshot;
  for (const auto band :
       {FrequencyRange{99.89e6, 100.05e6}, FrequencyRange{99.95e6, 100.11e6},
        FrequencyRange{100.15e6, 100.20e6}}) {
    auto outside = request;
    outside.range.frequency = band;
    const auto rejected = prepareAnnotationChannel(file, outside, candidate);
    check(!rejected && rejected.field == "frequency" &&
              rejected.error.find("所选活动段") != std::string::npos,
          "A physically valid band outside either binding edge must not create "
          "a LocatedSegment channel");
    auto malformed = file;
    auto altered = std::make_shared<AnnotationChannelSnapshot>(*snapshot);
    altered->range.frequency = band;
    malformed.channels.front().sourceSnapshot = altered;
    check(!validateFileAnnotations(malformed),
          "Canonical snapshot validation must reject the same frequency "
          "escape as channel preparation");
  }
  checkResult(service.rename(file, event.id, "renamed", "new note"),
              "Rename failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Current,
        "Name and note revisions must not stale the channel binding");
  for (const auto quality : {ReviewStatus::Accepted, ReviewStatus::Rejected,
                             ReviewStatus::Pending}) {
    checkResult(service.reviewBatch(file, {event.id}, quality),
                "Quality failed");
    check(
        file.channels.front().sourceRelation == ChannelSourceRelation::Current,
        "Quality changes must not stale a channel");
    Channel other;
    checkResult(service.bindChannelSource(file, other, request),
                "Every valid quality state must permit configured extraction");
    check(annotation->reviewStatus == quality,
          "Channel creation must preserve the selected quality state");
  }
  checkResult(
      service.commitGeometry(file, event.id, second, geometry(810, 990)),
      "Sibling geometry failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Current,
        "An unrelated sibling revision must not stale the bound first segment");
  checkResult(service.commitGeometry(file, event.id, first, geometry(120, 500)),
              "Bound geometry failed");
  check(
      file.channels.front().sourceRelation == ChannelSourceRelation::Changed &&
          file.channels.front().sourceSnapshot == snapshot,
      "Only bound geometry changes must mark the immutable channel Changed");
  const auto stale = prepareAnnotationChannel(file, request, candidate);
  check(!stale && stale.field == "segmentId",
        "A stale request must report its bound-segment field");
  checkResult(service.undo(file), "Bound geometry undo failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Current,
        "Undo must restore the exact bound relation");
  checkResult(service.deleteSegment(file, event.id, first),
              "Bound deletion failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Missing,
        "Deleting the bound segment must retain a Missing channel");
  checkResult(service.undo(file), "Bound deletion undo failed");
  check(
      file.channels.front().sourceRelation == ChannelSourceRelation::Current &&
          file.channels.front().sourceSnapshot == snapshot,
      "Deletion undo must restore Current without replacing the creation "
      "snapshot");
  auto invalid = request;
  invalid.range.time = {100, 1001};
  check(
      prepareAnnotationChannel(file, invalid, candidate).field == "sourceTime",
      "Unread extraction must identify its time field");
  invalid = request;
  invalid.range.frequency = {99'800'000, 100'200'000};
  check(prepareAnnotationChannel(file, invalid, candidate).field == "frequency",
        "Expanding a bound segment must identify its frequency field");
  invalid = request;
  invalid.outputSampleRateHz = 100000;
  check(prepareAnnotationChannel(file, invalid, candidate).field ==
            "outputSampleRateHz",
        "Insufficient output rate must identify its rate field");
  file.metadata.availability.status = LoadStatus::Failed;
  file.metadata.availability.error = "fixture read failure";
  const auto failed = prepareAnnotationChannel(file, request, candidate);
  check(!failed && failed.field == "source" &&
            failed.error.find("fixture read failure") != std::string::npos,
        "A failed source must expose its saved I/O cause");
  checkSourceUnchanged(file, sourceBefore);
}

void testLegacyChannelBindingFrequencyBounds() {
  for (const auto frame : {FrequencyFrame::RF, FrequencyFrame::Baseband}) {
    auto file = fileFixture();
    file.metadata.effectiveBandwidthHz = file.metadata.sampleRateHz;
    AnnotationService service;
    const auto set = sourceFixture(file, "legacy-binding");
    auto event = annotationFixture(set, "legacy-event");
    if (frame == FrequencyFrame::Baseband) {
      event.segments.front().range.frequency = {-100'000, 100'000};
      event.segments.front().frequencyFrame = frame;
      auto evidence = std::make_shared<OriginEvidence>(*event.originEvidence);
      evidence->originalSegments = event.segments;
      event.originEvidence = std::move(evidence);
    }
    checkResult(service.publishSourceSet(file, set, {event}),
                "Legacy binding fixture failed");
    const auto sourceBefore = *findAnnotation(file, event.id);
    event = convertSourceToManual(service, file, event.id);
    const auto* annotation = findAnnotation(file, event.id);
    const auto segmentId = annotation->segments.front().id;
    for (const auto frequency : {FrequencyRange{99.875e6, 100.075e6},
                                 FrequencyRange{99.925e6, 100.125e6},
                                 FrequencyRange{100.125e6, 100.225e6}}) {
      Channel outside;
      outside.sourceTime = {150, 450};
      outside.centerFrequencyHz =
          frequency.lowerHz + (frequency.upperHz - frequency.lowerHz) / 2;
      outside.bandwidthHz = frequency.upperHz - frequency.lowerHz;
      const auto rejected =
          service.bindChannelSource(file, outside, event.id, segmentId);
      check(!rejected && rejected.field == "frequency" &&
                !outside.sourceSnapshot && outside.annotationId.empty(),
            "The legacy overload must reject RF extraction escaping either "
            "edge of an RF or Baseband binding without publishing a snapshot");
    }
    Channel crop;
    crop.id = "legacy-crop";
    crop.sourceTime = {150, 450};
    crop.centerFrequencyHz = 100e6;
    crop.bandwidthHz = 100'000;
    checkResult(service.bindChannelSource(file, crop, event.id, segmentId),
                "An RF crop inside either coordinate frame must bind");
    file.channels.push_back(crop);
    check(crop.sourceSnapshot->range ==
                  ViewRange{{150, 450}, {99.95e6, 100.05e6}} &&
              crop.sourceSnapshot->boundRange == geometry() &&
              crop.sourceRelation == ChannelSourceRelation::Current &&
              validateFileAnnotations(file).ok,
          "An accepted legacy binding must satisfy canonical full-range "
          "validation after physical-frequency conversion");
    checkSourceUnchanged(file, sourceBefore);
  }
}

void testChannelNamesUsePersistentUnicodeLimit() {
  auto file = fileFixture();
  AnnotationService service;
  const auto id = manual(service, file);
  auto request = annotationChannelRequest(file, id, file.view.time);
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
      {std::string(79, 'a') + "\xf0\x9f\x93\xbb", false},
      {"", false},
      {" \t\xe3\x80\x80", false},
      {"\xe4\xb8", false},
      {"\xc0\xaf", false},
      {"\xed\xa0\x80", false},
      {"\xf4\x90\x80\x80", false}};
  for (const auto& [name, accepted] : cases) {
    request.name = name;
    Channel candidate;
    candidate.name = "unchanged";
    const auto prepared = prepareAnnotationChannel(file, request, candidate);
    check(bool(prepared) == accepted &&
              bool(validateChannelName(name)) == accepted &&
              (accepted
                   ? candidate.name == name
                   : prepared.field == "name" && candidate.name == "unchanged"),
          "Creation must use the persisted UTF-16 length limit, reject "
          "malformed UTF-8 and preserve the candidate on name failure");
  }
}

void testUserRangeChannelsPreserveUnknownTime() {
  auto file = fileFixture();
  file.metadata.availability = {1000, LoadStatus::Partial, 0, {}, {}};
  AnnotationService service;
  const auto set =
      sourceFixture(file, "user-range", AnnotationSource::Algorithm);
  auto observation = annotationFixture(set, "unknown-channel", {});
  observation.observation = geometry(700, 900);
  auto evidence = std::make_shared<OriginEvidence>(*observation.originEvidence);
  evidence->originalObservation = observation.observation;
  observation.originEvidence = evidence;
  checkResult(service.publishSourceSet(file, set, {observation}),
              "Unknown-time fixture failed");
  const auto sourceBefore = *findAnnotation(file, observation.id);
  observation = convertSourceToManual(service, file, observation.id);
  auto request = annotationChannelRequest(file, observation.id, {800, 1200}, 7);
  check(request.bindingKind == ChannelBindingKind::UserRange &&
            request.segmentId.empty() &&
            request.range.time == TimeRange{800, 1000},
        "Unknown-time defaults must use visible time intersected with the "
        "readable prefix");
  request = annotationChannelRequest(file, observation.id, {1200, 1800}, 7);
  Channel candidate;
  const auto empty = prepareAnnotationChannel(file, request, candidate);
  check(!empty && empty.field == "sourceTime" &&
            request.range.time == TimeRange{1000, 1000},
        "An empty intersection must remain empty with a specific field reason");
  request.range.time = {
      0, 1000};  // An explicit choice may extend beyond observation support.
  checkResult(
      service.reviewBatch(file, {observation.id}, ReviewStatus::Rejected),
      "Ignored unknown-time setup failed");
  const auto before = *findAnnotation(file, observation.id);
  file.metadata.availability.availableSamples = 1200;
  checkResult(service.bindChannelSource(file, candidate, request),
              "UserRange binding failed");
  check(
      candidate.sourceSnapshot->bindingKind == ChannelBindingKind::UserRange &&
          candidate.sourceSnapshot->boundRange == before.observation &&
          candidate.sourceSnapshot->sourceIdentity.availableSamples == 1200 &&
          candidate.sourceSnapshot->range.time == TimeRange{0, 1000} &&
          findAnnotation(file, observation.id)->segments.empty() &&
          findAnnotation(file, observation.id)->observation ==
              before.observation &&
          findAnnotation(file, observation.id)->reviewStatus ==
              ReviewStatus::Rejected &&
          findAnnotation(file, observation.id)->originEvidence ==
              before.originEvidence,
      "UserRange must capture actual support without inventing algorithm "
      "activity or changing quality");
  file.channels.push_back(candidate);
  checkResult(service.commitObservationFrequency(file, observation.id,
                                                 {99'910'000, 100'090'000}),
              "Observation frequency revision failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Changed,
        "A changed observation band must mark UserRange Changed");
  checkResult(service.undo(file), "Observation frequency undo failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Current,
        "Undo must restore the original UserRange binding");
  checkResult(service.adoptObservation(file, observation.id),
              "Explicit adoption failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Changed,
        "Changing an observation into located activity must stale the "
        "UserRange binding");
  checkResult(service.undo(file), "Adoption undo failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Current,
        "Structure undo must restore Current");
  checkResult(service.deleteBatch(file, {observation.id}),
              "UserRange deletion failed");
  check(file.channels.front().sourceRelation == ChannelSourceRelation::Missing,
        "Deleting an observation must retain its Missing UserRange channel");
  checkResult(service.undo(file), "UserRange deletion undo failed");
  check(validateFileAnnotations(file).ok &&
            file.channels.front().sourceRelation ==
                ChannelSourceRelation::Current,
        "Unknown-time channels and undo must retain valid physical snapshots");
  checkSourceUnchanged(file, sourceBefore);
}

std::string persistedSelection(const FileState& file) {
  std::string text;
  auto put = [&](const std::string& value) {
    text.append(value);
    text.push_back('|');
  };
  for (const auto& id : file.selectedMarkIds) put(id);
  put(file.activeMarkId);
  put(file.activeSegmentId);
  put(file.selectionAnchorId);
  put(file.selectionSampleChannel
          ? std::to_string(*file.selectionSampleChannel)
          : std::string{"-"});
  put(file.display.psdFromSelection ? "1" : "0");
  put(file.revisionEditingAnnotationId);
  put(file.revisionEditingBaseRevisionId);
  for (const auto& mark : file.marks) {
    put(mark.id);
    put(mark.revisionId);
    put(mark.activeSegmentId);
    put(std::to_string(mark.revisions.size()));
    put(std::to_string(mark.reviewHistory.size()));
    put(std::to_string(mark.range.time.begin));
    put(std::to_string(mark.range.time.end));
    put(std::to_string(mark.range.frequency.lowerHz));
    put(std::to_string(mark.range.frequency.upperHz));
    put(mark.deleted ? "1" : "0");
    put(mark.name);
    put(std::to_string(static_cast<int>(mark.sourceKind)));
    for (const auto& revision : mark.revisions) put(revision.action);
  }
  return text;
}

void testSelectionWriteGuard() {
  auto file = fileFixture();
  AnnotationService service;
  const auto idA = manual(service, file);
  const auto idB = manual(service, file);
  auto importedSet = sourceFixture(file, "imported-selection");
  auto imported = annotationFixture(importedSet, "imported-selection-label");
  checkResult(service.publishSourceSet(file, importedSet, {imported}),
              "Imported selection fixture failed");
  const auto importedId = imported.id;
  auto algorithmSet =
      sourceFixture(file, "algorithm-selection", AnnotationSource::Algorithm);
  auto algorithm =
      annotationFixture(algorithmSet, "algorithm-selection-result");
  checkResult(service.publishSourceSet(file, algorithmSet, {algorithm}),
              "Algorithm selection fixture failed");
  const auto algorithmId = algorithm.id;
  auto* markA = findAnnotation(file, idA);
  const auto first = markA->segments.front().id;
  checkResult(service.splitSegment(file, idA, first, 300),
              "Selection guard split failed");
  check(markA->segments.size() == 2, "Selection guard split must add a segment");
  const auto second = markA->segments[1].id;
  checkResult(service.select(file, {idA}, idA, first),
              "Initial segment selection failed");
  file.display.psdFromSelection = true;
  checkResult(service.beginRevision(file, idA), "Revision edit setup failed");
  service.reconcileSelection(file);
  check(file.selectionSampleChannel &&
            *file.selectionSampleChannel ==
                file.metadata.sampleFormat.selectedChannel &&
            file.revisionEditingAnnotationId == idA &&
            file.activeSegmentId == first && file.display.psdFromSelection,
        "Initial reconcile must keep the committed selection context");

  bool reject = true;
  int guardCalls = 0;
  int notes = 0;
  service.setWriteGuard([&](std::string& error) {
    ++guardCalls;
    if (reject) error = "detection commit";
    return !reject;
  });
  service.setWroteNotifier([&] { ++notes; });
  const auto frozen = persistedSelection(file);
  const auto history = service.historySize(file);
  const auto revisionsA = findAnnotation(file, idA)->revisions;
  const std::vector<std::string> order{idA, idB, importedId, algorithmId};
  auto unchanged = [&](const char* message) {
    check(persistedSelection(file) == frozen, message);
    check(service.historySize(file) == history, message);
    check(findAnnotation(file, idA)->revisions == revisionsA, message);
    check(notes == 0, message);
  };
  auto rejectWrite = [&](const auto& operation, const char* message) {
    const auto calls = guardCalls;
    check(operation(), message);
    check(guardCalls == calls + 1, message);
    unchanged(message);
  };
  auto leaveWrite = [&](const auto& operation, const char* message) {
    const auto calls = guardCalls;
    check(operation(), message);
    check(guardCalls == calls, message);
    unchanged(message);
  };

  rejectWrite(
      [&] {
        const auto denied = service.select(file, {idA}, idA, first);
        return !denied.ok && denied.error == "detection commit";
      },
      "Rejecting guard must refuse an unchanged selection before reconcile");
  rejectWrite(
      [&] {
        const auto denied = service.select(file, {idB}, idB);
        return !denied.ok && denied.error == "detection commit" &&
               file.activeMarkId == idA;
      },
      "Rejecting guard must block a selection change");
  rejectWrite(
      [&] {
        const auto denied = service.chooseSelection(
            file, idB, AnnotationSelectionMode::Range, order);
        return !denied.ok && denied.error == "detection commit";
      },
      "Rejecting guard must block range selection");
  rejectWrite(
      [&] {
        const auto denied = service.chooseSelection(
            file, importedId, AnnotationSelectionMode::Toggle, order);
        return !denied.ok && denied.error == "detection commit";
      },
      "Rejecting guard must block toggle selection");
  rejectWrite(
      [&] {
        const auto denied = service.select(file, {});
        return !denied.ok && denied.error == "detection commit" &&
               file.selectedMarkIds.size() == 1 && file.display.psdFromSelection;
      },
      "Rejecting guard must block clearing the selection");
  rejectWrite(
      [&] {
        const auto denied = service.select(file, {idA}, idA, second);
        return !denied.ok && denied.error == "detection commit" &&
               findAnnotation(file, idA)->activeSegmentId == first &&
               findAnnotation(file, idA)->revisions == revisionsA;
      },
      "Rejecting guard must block an active segment switch");
  leaveWrite(
      [&] {
        const auto denied = service.select(file, {idA}, "missing-mark");
        return !denied.ok && denied.error != "detection commit";
      },
      "Invalid selection must fail without the write guard");
  leaveWrite(
      [&] {
        const auto denied = service.chooseSelection(
            file, idB, AnnotationSelectionMode::Replace, {});
        return !denied.ok && denied.error != "detection commit";
      },
      "Selection outside the initiating order must fail without the write guard");
  const auto callsBeforeHover = guardCalls;
  check(service.hover(file, idB), "Hover must update while the write guard rejects");
  check(file.hoveredMarkId == idB && persistedSelection(file) == frozen &&
            guardCalls == callsBeforeHover && notes == 0,
        "Hover must not persist selection or consult the write guard");
  leaveWrite(
      [&] {
        service.reconcileSelection(file);
        return file.hoveredMarkId == idB && file.activeMarkId == idA;
      },
      "Clean reconcile must not consult the write guard");

  file.metadata.sampleFormat.selectedChannel = 1;
  const auto mismatched = persistedSelection(file);
  const auto mismatchCalls = guardCalls;
  const auto hovered = file.hoveredMarkId;
  service.reconcileSelection(file);
  check(persistedSelection(file) == mismatched && file.hoveredMarkId == hovered &&
            guardCalls == mismatchCalls + 1 && notes == 0 &&
            file.selectedMarkIds.size() == 1,
        "Rejecting guard must block channel-mismatch selection cleanup");
  file.metadata.sampleFormat.selectedChannel = 0;
  check(persistedSelection(file) == frozen, "Restoring the sample channel must restore the fingerprint");
  leaveWrite(
      [&] {
        service.reconcileSelection(file);
        return file.activeMarkId == idA && file.hoveredMarkId == idB;
      },
      "Reconcile after restoring the channel must stay a no-op under the guard");

  reject = false;
  auto accepted = [&](const auto& operation, const char* message) {
    const auto noted = notes;
    const AnnotationResult result = operation();
    check(result.ok && result.changed, message);
    check(notes == noted + 1, message);
  };
  accepted([&] { return service.select(file, {idB}, idB); },
           "Released guard must apply an ordinary selection and notify once");
  const auto repeatNotes = notes;
  const auto repeat = service.select(file, {idB}, idB);
  check(repeat.ok && !repeat.changed && notes == repeatNotes,
        "Repeating the current selection must not notify again");
  accepted([&] {
             return service.chooseSelection(file, idA, AnnotationSelectionMode::Range,
                                            order);
           },
           "Released guard must apply range selection and notify once");
  check(file.selectedMarkIds.size() == 2 && file.selectionAnchorId == idB,
        "Range selection must keep the anchor and include the span");
  accepted(
      [&] {
        return service.chooseSelection(file, algorithmId, AnnotationSelectionMode::Toggle,
                                       order);
      },
      "Released guard must toggle an algorithm annotation into the selection");
  check(std::find(file.selectedMarkIds.begin(), file.selectedMarkIds.end(),
                  algorithmId) != file.selectedMarkIds.end() &&
            !service.canEdit(file, algorithmId),
        "A selected algorithm annotation must remain read-only");
  const auto revisionId = findAnnotation(file, idA)->revisionId;
  const auto segmentNotes = notes;
  const auto switched = service.select(file, {idA}, idA, second);
  check(switched.ok && switched.changed && notes == segmentNotes + 1 &&
            findAnnotation(file, idA)->activeSegmentId == second &&
            findAnnotation(file, idA)->revisionId == revisionId &&
            findAnnotation(file, idA)->revisions == revisionsA &&
            file.activeSegmentId == second &&
            findAnnotation(file, idA)->range ==
                findAnnotation(file, idA)->segments[1].range,
        "Segment switch must notify once without a revision");
  file.display.psdFromSelection = true;
  accepted([&] { return service.select(file, {}); },
           "Released guard must clear the selection and notify once");
  check(file.selectedMarkIds.empty() && file.activeMarkId.empty() &&
            !file.display.psdFromSelection,
        "Clearing the selection must drop the PSD selection scope");

  const auto createNotes = notes;
  const auto createHistory = service.historySize(file);
  const auto created = service.createManual(file, "Single notify", geometry(1200, 1400));
  check(created.ok && notes == createNotes + 1 &&
            service.historySize(file) == createHistory + 1 &&
            file.activeMarkId == created.id,
        "Creating a manual annotation must notify once for the command and its selection");
  checkResult(service.undo(file), "Create undo failed");
  check(notes == createNotes + 1,
        "Undo must not emit another project-write notification");
  check(!findAnnotation(file, created.id) && service.canRedo(file) &&
            service.historySize(file) == createHistory + 1,
        "Undo must remove the created annotation and retain its redo command");

  const auto importedRevisions = findAnnotation(file, importedId)->revisions;
  const auto importedHistory = service.historySize(file);
  const auto importedNotes = notes;
  checkResult(service.select(file, {importedId}, importedId),
              "Imported annotation must remain selectable while unlocked");
  check(notes == importedNotes + 1 && !service.canEdit(file, importedId),
        "Selecting an imported annotation must notify once and stay non-editable");
  const auto importedSegment = findAnnotation(file, importedId)->segments.front().id;
  check(!service.commitGeometry(file, importedId, importedSegment, geometry(200, 400)) &&
            !service.rename(file, importedId, "blocked", "blocked") &&
            !service.deleteBatch(file, {importedId}) &&
            service.historySize(file) == importedHistory &&
            findAnnotation(file, importedId)->revisions == importedRevisions &&
            notes == importedNotes + 1,
        "Imported geometry, name, and deletion must stay refused");

  const auto algorithmRevisions = findAnnotation(file, algorithmId)->revisions;
  const auto algorithmHistory = service.historySize(file);
  const auto algorithmNotes = notes;
  checkResult(service.select(file, {algorithmId}, algorithmId),
              "Algorithm annotation must remain selectable while unlocked");
  check(notes == algorithmNotes + 1 && !service.canEdit(file, algorithmId),
        "Selecting an algorithm annotation must notify once and stay non-editable");
  const auto algorithmSegment =
      findAnnotation(file, algorithmId)->segments.front().id;
  check(!service.commitGeometry(file, algorithmId, algorithmSegment,
                                geometry(220, 420)) &&
            !service.rename(file, algorithmId, "blocked", "blocked") &&
            !service.deleteBatch(file, {algorithmId}) &&
            service.historySize(file) == algorithmHistory &&
            findAnnotation(file, algorithmId)->revisions == algorithmRevisions &&
            notes == algorithmNotes + 1,
        "Algorithm geometry, name, and deletion must stay refused");
}
}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests{
      {"legacy compatibility", testLegacyCompatibility},
      {"three sources and immutable origin", testSourcesAndImmutableOrigin},
      {"direct edits preserve origin and metadata quality",
       testDirectEditsPreserveOriginAndMetadataQuality},
      {"unknown frequency and edit context",
       testUnknownFrequencyAndEditContext},
      {"segment deletion snapshot and concurrent undo",
       testSegmentDeletionSnapshotAndConcurrentUndo},
      {"restore origin preserves confirmed sibling",
       testRestoreOriginPreservesConfirmedSibling},
      {"located channel requests and relations",
       testLocatedChannelRequestsAndRelations},
      {"legacy channel binding full frequency bounds",
       testLegacyChannelBindingFrequencyBounds},
      {"channel names use persistent Unicode limit",
       testChannelNamesUsePersistentUnicodeLimit},
      {"UserRange channels preserve unknown time",
       testUserRangeChannelsPreserveUnknownTime},
      {"unknown-time observation", testUnknownObservation},
      {"Pending single-segment confirmation",
       testPendingSingleSegmentConfirmation},
      {"confirmation undo/redo and rejection",
       testConfirmationUndoRedoAndRejection},
      {"confirmation geometry invalidation",
       testConfirmationGeometryInvalidation},
      {"confirmation available and effective bounds",
       testConfirmationAvailableAndEffectiveBounds},
      {"confirmation current source and revision",
       testConfirmationCurrentSourceAndRevision},
      {"physical and available bounds", testPhysicalAndAvailableBounds},
      {"multi-segment editing and selection", testSegmentsAndSelection},
      {"unified selection, order, filter, and hover",
       testUnifiedSelectionOrderFilterAndHover},
      {"atomic batch and concurrent undo", testAtomicBatchAndConcurrentUndo},
      {"channel snapshots and source changes",
       testChannelSnapshotsAndSourceChanges},
      {"source idempotence and validation", testSourceIdempotenceAndValidation},
      {"budgets and independent histories",
       testBudgetsAndIndependentHistories},
      {"selection write guard", testSelectionWriteGuard}};
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
