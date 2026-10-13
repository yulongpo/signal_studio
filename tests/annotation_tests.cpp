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
  auto* label = findAnnotation(file, "label-1");
  const auto origin = label->originEvidence;
  check(label->reviewStatus == ReviewStatus::Pending &&
            label->segments.size() == 2,
        "Imported burst label must be Pending and multi-segment");
  const auto segmentId = label->segments.front().id;
  check(!service.commitGeometry(file, label->id, segmentId, geometry(101, 500)),
        "Imported source must be read-only before revision session");
  checkResult(service.beginRevision(file, label->id),
              "Beginning explicit revision failed");
  checkResult(
      service.commitGeometry(file, label->id, segmentId, geometry(101, 500)),
      "Revision geometry failed");
  label = findAnnotation(file, "label-1");
  check(label->sourceKind == AnnotationSource::ImportedLabel &&
            label->originEvidence == origin,
        "Human revision must preserve source and immutable origin ownership");
  check(label->originEvidence->originalFieldsJson == raw &&
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
  check(repeated.ok && !repeated.changed && file.marks.size() == 2 &&
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
            label->sourceKind == AnnotationSource::ImportedLabel,
        "Restoring origin must not change source identity");
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
  const auto* unknown = findAnnotation(file, "unknown");
  check(unknown->segments.empty() && unknown->range == ViewRange{},
        "Unknown time must not fabricate an active rectangle");
  checkResult(service.reviewBatch(file, {unknown->id}, ReviewStatus::Accepted),
              "Unknown object can be reviewed");
  Channel channel;
  check(!service.bindChannelSource(file, channel, "unknown", "missing"),
        "Unknown time must not permit DDC creation");
  checkResult(service.beginRevision(file, "unknown"),
              "Observation revision session failed");
  checkResult(service.adoptObservation(file, "unknown"),
              "Explicit observation adoption failed");
  unknown = findAnnotation(file, "unknown");
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
  check(!service.createManual(file, "Unavailable", geometry(1000, 2000)),
        "Manual creation cannot exceed available prefix");
  check(!service.createManual(file, "Outside effective",
                              {{1, 10}, {99'600'000, 99'700'000}}),
        "Manual creation cannot exceed effective band");
  checkResult(service.beginRevision(file, "tail-label"),
              "Tail label revision session failed");
  check(!service.commitGeometry(file, "tail-label",
                                annotation.segments.front().id,
                                geometry(8000, 9000)),
        "Manual edit cannot claim unavailable samples");
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
  auto* manualObject = findAnnotation(file, id);
  auto* algorithmObject = findAnnotation(file, algorithm.id);
  const auto oldManual = manualObject->reviewStatus;
  const auto oldAlgorithm = algorithmObject->reviewStatus;
  check(!service.reviewBatch(file, {id, algorithm.id, "missing"},
                             ReviewStatus::Rejected),
        "Missing batch review target must reject");
  check(manualObject->reviewStatus == oldManual &&
            algorithmObject->reviewStatus == oldAlgorithm,
        "Failed batch review must not partially change valid objects");
  checkResult(
      service.reviewBatch(file, {id, algorithm.id}, ReviewStatus::Rejected),
      "Atomic review batch failed");
  check(manualObject->reviewStatus == ReviewStatus::Rejected &&
            algorithmObject->reviewStatus == ReviewStatus::Rejected,
        "Batch review should change exactly the selected objects");
  checkResult(service.undo(file), "Atomic review undo failed");
  check(manualObject->reviewStatus == oldManual &&
            algorithmObject->reviewStatus == oldAlgorithm,
        "Undo batch review must restore all selected objects together");
  check(!service.deleteBatch(file, {id, "missing"}) && !manualObject->deleted,
        "Batch delete must prevalidate all targets");
  checkResult(service.deleteBatch(file, {id, algorithm.id}),
              "Soft deletion failed");
  check(file.marks.size() == 2 && manualObject->deleted &&
            algorithmObject->deleted,
        "Deletion must preserve immutable source evidence");
  checkResult(service.undo(file), "Soft deletion undo failed");
  check(!manualObject->deleted && !algorithmObject->deleted,
        "Soft deletion undo failed to restore objects");
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
      file.channels.front().sourceRelation == ChannelSourceRelation::Rejected &&
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
}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests{
      {"legacy compatibility", testLegacyCompatibility},
      {"three sources and immutable origin", testSourcesAndImmutableOrigin},
      {"unknown-time observation", testUnknownObservation},
      {"physical and available bounds", testPhysicalAndAvailableBounds},
      {"multi-segment editing and selection", testSegmentsAndSelection},
      {"atomic batch and concurrent undo", testAtomicBatchAndConcurrentUndo},
      {"channel snapshots and source changes",
       testChannelSnapshotsAndSourceChanges},
      {"source idempotence and validation", testSourceIdempotenceAndValidation},
      {"budgets and independent histories",
       testBudgetsAndIndependentHistories}};
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
