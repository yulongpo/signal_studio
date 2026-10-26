// Standalone, Qt-free service tests. Registration for the integrating agent:
// add_executable(SignalStudioUxR5AnnotationTests tests/ux_r5_annotation_tests.cpp)
// target_link_libraries(SignalStudioUxR5AnnotationTests PRIVATE SignalStudioCore)
// set_target_properties(SignalStudioUxR5AnnotationTests PROPERTIES AUTOMOC OFF)
// ss_target_defaults(SignalStudioUxR5AnnotationTests)
// add_test(NAME signal_studio_ux_r5_annotations COMMAND SignalStudioUxR5AnnotationTests)
// set_tests_properties(signal_studio_ux_r5_annotations PROPERTIES TIMEOUT 60)

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "application/annotation_service.h"

using namespace signalstudio;

namespace {
void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
void ok(const AnnotationResult& result, const std::string& message) {
  check(result.ok, message + ": " + result.error);
}
ViewRange geometry(SampleIndex begin = 101, SampleIndex end = 501) {
  return {{begin, end}, {99'950'000, 100'050'000}};
}
FileState fixture() {
  FileState file;
  file.metadata = {"ux-r5-file", "test.iq", "test.iq", 1'000'000,
                   100'000'000, 10'000, false};
  file.metadata.effectiveBandwidthHz = 400'000;
  file.view = fullRange(file.metadata);
  return file;
}
AnnotationSourceSet sourceSet(const FileState& file, std::string id,
                              AnnotationSource kind) {
  AnnotationSourceSet set;
  set.id = std::move(id);
  set.fileId = file.metadata.id;
  set.sampleChannel = file.metadata.sampleFormat.selectedChannel;
  set.sourceKind = kind;
  set.formatId = kind == AnnotationSource::Algorithm ? "detection-v1"
                                                    : "generator-txt-v1";
  set.parserId = "synthetic-unit-fixture";
  set.parserVersion = "1";
  set.profileId = "test-profile";
  set.profileVersion = "2";
  set.originalPath = "external/source.txt";
  set.originalText = "Original source\r\nretained\n";
  set.supplementalOriginalText = "Supplementary evidence\n";
  set.sourceHash = "fixture-identity:" + set.id;
  set.supplementalHash = "fixture-supplement";
  set.coordinateInterpretation = "RF/baseband Hz; half-open samples";
  set.sourceIdentity = annotationSourceIdentity(file.metadata, 7);
  set.diagnostics.push_back({DiagnosticSeverity::Warning, "fixture", "Keep me", "record", 3});
  if (kind == AnnotationSource::Algorithm) set.runId = "run:" + set.id;
  return set;
}
Annotation annotation(const AnnotationSourceSet& set, std::string id,
                      std::vector<ViewRange> ranges = {geometry()}) {
  Annotation value;
  value.id = std::move(id);
  value.name = "Original name";
  value.note = "Original note\r\nwith newlines";
  value.classLabel = "64-QAM";
  value.fileId = set.fileId;
  value.sourceKind = set.sourceKind;
  value.sourceSetId = set.id;
  value.originRecordId = "record:" + value.id;
  value.revisionId = value.id + ":r0";
  value.reviewRevisionId = value.revisionId;
  value.revisions.push_back({value.revisionId, {}, "publication", "fixture", "1", {}});
  value.reviewHistory.push_back({ReviewStatus::Pending, value.revisionId, "fixture", "1", {}});
  for (const auto& range : ranges) {
    Segment segment;
    segment.id = value.id + ":s" + std::to_string(value.segments.size());
    segment.range = range;
    segment.timeQuality = set.sourceKind == AnnotationSource::Algorithm
                              ? TimeQuality::AlgorithmEstimated : TimeQuality::RoundedLabel;
    segment.frequencyQuality = set.sourceKind == AnnotationSource::Algorithm
                                   ? FrequencyQuality::Refined : FrequencyQuality::NominalLabel;
    segment.uncertaintySamples = 7;
    segment.clippedAtRoi = true;
    value.segments.push_back(std::move(segment));
  }
  auto origin = std::make_shared<OriginEvidence>();
  origin->formatId = set.formatId;
  origin->originRef = "external/evidence.txt";
  origin->originalFieldsJson = R"({"unknown":[1,"keep"],"note":"raw"})";
  origin->coordinateInterpretation = set.coordinateInterpretation;
  origin->originalSegments = value.segments;
  origin->evidenceRefs = {"evidence-only-not-a-runtime-dependency"};
  if (set.sourceKind == AnnotationSource::Algorithm) {
    origin->runId = set.runId;
    origin->parentCandidateId = "parent";
    origin->scoreMean = .8;
    origin->scoreMax = .9;
    origin->estimatedCnrDb = 12.5;
    origin->windowIds = {1, 3};
    origin->windowCount = 5;
    origin->refineMethod = RefineMethod::Sample;
    origin->refineStatus = RefineStatus::Fallback;
    origin->fallbackReason = "Synthetic fallback evidence";
  }
  value.originEvidence = std::move(origin);
  refreshAnnotationProjection(value);
  return value;
}
std::vector<std::string> publish(AnnotationService& service, FileState& file,
                                  AnnotationSource kind, int count = 1,
                                  std::string prefix = "source") {
  auto set = sourceSet(file, prefix + "-set", kind);
  std::vector<Annotation> values;
  std::vector<std::string> ids;
  for (int i = 0; i < count; ++i) {
    ids.push_back(prefix + "-" + std::to_string(i));
    values.push_back(annotation(set, ids.back()));
  }
  ok(service.publishSourceSet(file, std::move(set), std::move(values)), "publish fixture");
  return ids;
}
bool sameSegments(const std::vector<Segment>& a, const std::vector<Segment>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto left = a[i], right = b[i];
    // Invalid-input tests also assert bit-independent semantic preservation of
    // NaN. Ordinary floating-point equality cannot compare an unchanged NaN.
    if (std::isnan(left.range.frequency.lowerHz) && std::isnan(right.range.frequency.lowerHz))
      left.range.frequency.lowerHz = right.range.frequency.lowerHz = 0;
    if (std::isnan(left.range.frequency.upperHz) && std::isnan(right.range.frequency.upperHz))
      left.range.frequency.upperHz = right.range.frequency.upperHz = 0;
    if (left != right) return false;
  }
  return true;
}
bool sameAnnotation(const Annotation& a, const Annotation& b) {
  return a.id == b.id && a.name == b.name && a.range == b.range &&
         a.fileId == b.fileId && a.sourceSetId == b.sourceSetId &&
         a.originRecordId == b.originRecordId && a.sourceKind == b.sourceKind &&
         a.classLabel == b.classLabel && a.note == b.note &&
         a.activeSegmentId == b.activeSegmentId && a.revisionId == b.revisionId &&
         sameSegments(a.segments, b.segments) && a.observation == b.observation &&
         a.observationFrequencyFrame == b.observationFrequencyFrame &&
         a.observationTimeQuality == b.observationTimeQuality &&
         a.originEvidence == b.originEvidence &&
         a.conversionSnapshot == b.conversionSnapshot &&
         a.reviewStatus == b.reviewStatus && a.reviewRevisionId == b.reviewRevisionId &&
         a.reviewHistory == b.reviewHistory && a.revisions == b.revisions &&
         a.derivedFrom == b.derivedFrom && a.deleted == b.deleted;
}
void unchanged(const FileState& before, const FileState& after) {
  check(before.sourceSets == after.sourceSets && before.marks.size() == after.marks.size(),
        "failed operation published an object or source set");
  for (std::size_t i = 0; i < before.marks.size(); ++i)
    check(sameAnnotation(before.marks[i], after.marks[i]), "failed operation changed an annotation");
  check(before.view == after.view && before.selectedMarkIds == after.selectedMarkIds &&
            before.activeMarkId == after.activeMarkId &&
            before.activeSegmentId == after.activeSegmentId &&
            before.revisionEditingAnnotationId == after.revisionEditingAnnotationId &&
            before.revisionEditingBaseRevisionId == after.revisionEditingBaseRevisionId,
        "failed operation changed view, selection, or edit context");
}
void sameGeometry(const Segment& original, const Segment& copy) {
  auto expected = original;
  expected.id = copy.id;
  check(original.id != copy.id && expected == copy, "conversion changed half-open geometry/frame/quality");
}
void snapshotMatches(const Annotation& source, const AnnotationSourceSet& set,
                     const Annotation& copy) {
  check(copy.conversionSnapshot != nullptr, "missing conversion snapshot");
  const auto& snapshot = *copy.conversionSnapshot;
  check(snapshot.sourceAnnotationId == source.id &&
            snapshot.sourceRevisionId == source.revisionId &&
            snapshot.sourceOriginRecordId == source.originRecordId &&
            snapshot.sourceKind == source.sourceKind && snapshot.sourceSet == set &&
            snapshot.sourceName == source.name && snapshot.sourceNote == source.note &&
            snapshot.sourceClassLabel == source.classLabel &&
            snapshot.sourceSegments == source.segments &&
            snapshot.sourceObservation == source.observation &&
            snapshot.sourceObservationFrequencyFrame == source.observationFrequencyFrame &&
            snapshot.sourceObservationTimeQuality == source.observationTimeQuality &&
            snapshot.sourceActiveSegmentId == source.activeSegmentId &&
            snapshot.sourceReviewStatus == source.reviewStatus &&
            snapshot.sourceOriginEvidence == *source.originEvidence,
        "conversion snapshot omitted or changed source evidence");
}
void testCapabilities() {
  static_assert(std::is_const_v<typename decltype(Annotation{}.conversionSnapshot)::element_type>);
  auto file = fixture();
  AnnotationService service;
  const auto imported = publish(service, file, AnnotationSource::ImportedLabel, 1, "label")[0];
  const auto algorithm = publish(service, file, AnnotationSource::Algorithm, 1, "algorithm")[0];
  const auto manual = service.createManual(file, "Manual", geometry());
  ok(manual, "create manual");
  for (const auto& id : {imported, algorithm, manual.id}) {
    const auto c = service.capabilities(file, id);
    const bool writable = id == manual.id;
    check(c.view && c.select && c.locate && c.exportData, "valid source became unreadable");
    check(c.editGeometry == writable && c.editMetadata == writable &&
              c.manageSegments == writable && c.review == writable &&
              c.deleteAnnotation == writable && c.createChannelDirectly == writable &&
              c.convertToManual != writable && service.canEdit(file, id) == writable,
          "source capability matrix is inconsistent");
  }
  check(!service.capabilities(file, "missing").view, "missing object has capabilities");
  const auto saved = file;
  for (int defect = 0; defect < 5; ++defect) {
    file = saved;
    auto* value = findAnnotation(file, manual.id);
    if (defect == 0) value->fileId = "wrong-file";
    if (defect == 1) value->revisionId = "missing-revision";
    if (defect == 2) findSourceSet(file, value->sourceSetId)->deleted = true;
    if (defect == 3) findSourceSet(file, value->sourceSetId)->sourceIdentity.sampleRateHz *= 2;
    if (defect == 4) value->deleted = true;
    check(!service.canEdit(file, manual.id), "invalid Manual identity is writable");
    check(!service.rename(file, manual.id, "Blocked", {}), "invalid Manual identity was edited");
  }
}
void testReadOnlyWriteApis() {
  for (const auto kind : {AnnotationSource::ImportedLabel, AnnotationSource::Algorithm}) {
    auto file = fixture();
    AnnotationService service;
    auto set = sourceSet(file, "read-only", kind);
    auto located = annotation(set, "located", {geometry(), geometry(601, 801)});
    located.segments[0].range.time.begin = 111;
    auto observation = annotation(set, "observation", {});
    observation.observation = geometry(100, 800);
    ok(service.publishSourceSet(file, set, {located, observation}), "publish read-only API fixtures");
    const auto id = located.id;
    const auto observationId = observation.id;
    const auto segment = findAnnotation(file, id)->activeSegmentId;
    const auto otherSegment = findAnnotation(file, id)->segments[1].id;
    const std::vector<std::pair<std::string, std::function<AnnotationResult()>>> writes = {
        {"beginRevision", [&] { return service.beginRevision(file, id); }},
        {"commitGeometry", [&] { return service.commitGeometry(file, id, segment, geometry(102, 502)); }},
        {"commitObservationFrequency", [&] { return service.commitObservationFrequency(file, observationId, {99'960'000, 100'040'000}); }},
        {"addSegment", [&] { return service.addSegment(file, id, geometry(600, 700)); }},
        {"deleteSegment", [&] { return service.deleteSegment(file, id, segment); }},
        {"splitSegment", [&] { return service.splitSegment(file, id, segment, 300); }},
        {"mergeSegments", [&] { return service.mergeSegments(file, id, {segment, otherSegment}); }},
        {"adoptObservation", [&] { return service.adoptObservation(file, observationId); }},
        {"rename", [&] { return service.rename(file, id, "Forbidden", "Forbidden note"); }},
        {"restoreOrigin", [&] { return service.restoreOrigin(file, id); }},
        {"confirmSegment", [&] { return service.confirmSegment(file, id, segment); }},
        {"reviewBatch", [&] { return service.reviewBatch(file, {id}, ReviewStatus::Accepted); }},
        {"deleteBatch", [&] { return service.deleteBatch(file, {id}); }}};
    const auto before = file;
    for (const auto& [name, write] : writes) {
      check(!write(), name + " accepted a read-only source");
      unchanged(before, file);
      check(service.historySize(file) == 0, name + " recorded a read-only command");
    }
    Channel channel;
    channel.name = "Unchanged candidate";
    channel.outputSampleRateHz = 12345;
    const auto request = annotationChannelRequest(file, id, {100, 500});
    check(!prepareAnnotationChannel(file, request, channel), "prepare accepted a read-only source");
    check(!service.bindChannelSource(file, channel, request), "request bind accepted a read-only source");
    check(!service.bindChannelSource(file, channel, id, segment), "direct bind accepted a read-only source");
    check(channel.name == "Unchanged candidate" && channel.outputSampleRateHz == 12345 &&
              channel.annotationId.empty() && !channel.sourceSnapshot,
          "rejected channel creation modified its candidate");
    check(!service.undo(file) && !service.redo(file), "source publication entered Manual undo history");
    unchanged(before, file);
  }
}
void testMixedBatchesAreAtomic() {
  auto file = fixture();
  AnnotationService service;
  const auto imported = publish(service, file, AnnotationSource::ImportedLabel)[0];
  const auto algorithm = publish(service, file, AnnotationSource::Algorithm, 1, "algorithm")[0];
  const auto manual = service.createManual(file, "Manual", geometry());
  ok(manual, "create manual");
  for (const auto& invalid : {imported, algorithm, std::string{"missing"}})
    for (const bool reverse : {false, true}) {
      std::vector<std::string> ids{manual.id, invalid};
      if (reverse) std::reverse(ids.begin(), ids.end());
      const auto before = file;
      const auto history = service.historySize(file);
      check(!service.reviewBatch(file, ids, ReviewStatus::Rejected), "mixed batch review succeeded");
      unchanged(before, file);
      check(!service.deleteBatch(file, ids), "mixed batch deletion succeeded");
      unchanged(before, file);
      check(service.historySize(file) == history, "failed batch changed history");
    }
}
void testManualCommandsRemainUsable() {
  auto file = fixture();
  AnnotationService service;
  const auto created = service.createManual(file, "Manual", geometry());
  ok(created, "create manual");
  const auto id = created.id;
  const auto segment = findAnnotation(file, id)->activeSegmentId;
  ok(service.beginRevision(file, id), "begin Manual revision");
  ok(service.rename(file, id, "Renamed", "Note"), "rename Manual");
  ok(service.commitGeometry(file, id, segment, geometry(103, 503)), "edit Manual geometry");
  ok(service.confirmSegment(file, id, segment), "confirm Manual segment");
  ok(service.addSegment(file, id, geometry(600, 800)), "add Manual segment");
  const auto other = findAnnotation(file, id)->activeSegmentId;
  ok(service.splitSegment(file, id, other, 700), "split Manual segment");
  const auto third = findAnnotation(file, id)->segments.back().id;
  ok(service.mergeSegments(file, id, {other, third}), "merge Manual segments");
  ok(service.deleteSegment(file, id, other), "delete Manual segment");
  ok(service.restoreOrigin(file, id), "restore Manual baseline");
  check(findAnnotation(file, id)->segments[0].range == geometry(), "restore lost Manual baseline");
  ok(service.reviewBatch(file, {id}, ReviewStatus::Accepted), "review Manual");
  ok(service.deleteBatch(file, {id}), "delete Manual");
  check(findAnnotation(file, id)->deleted, "Manual was not deleted");
  ok(service.undo(file), "undo Manual delete");
  check(!findAnnotation(file, id)->deleted, "undo did not restore Manual");
  ok(service.redo(file), "redo Manual delete");
  check(findAnnotation(file, id)->deleted, "redo did not delete Manual");
  ok(service.undo(file), "restore Manual for channel");
  const auto request = annotationChannelRequest(file, id, {100, 500});
  Channel channel;
  ok(prepareAnnotationChannel(file, request, channel), "prepare Manual channel");
  ok(service.bindChannelSource(file, channel, request), "bind Manual request");
  Channel direct;
  ok(service.bindChannelSource(file, direct, id, segment), "bind Manual segment directly");
}
void testSingleConversions() {
  for (const auto kind : {AnnotationSource::ImportedLabel, AnnotationSource::Algorithm}) {
    auto file = fixture();
    AnnotationService service;
    const auto id = publish(service, file, kind)[0];
    const auto source = *findAnnotation(file, id);
    const auto set = *findSourceSet(file, source.sourceSetId);
    std::vector<std::string> ids;
    const auto result = service.convertToManual(file, {id}, {}, ids);
    ok(result, "single conversion");
    check(result.changed && ids.size() == 1 && result.id == ids.front(), "conversion did not return its new ID");
    const auto copy = *findAnnotation(file, ids[0]);
    check(copy.id != source.id && copy.sourceSetId != source.sourceSetId &&
              copy.revisionId != source.revisionId && copy.sourceKind == AnnotationSource::Manual &&
              copy.name == source.name && copy.note == source.note && copy.classLabel == source.classLabel &&
              copy.derivedFrom == std::vector<std::string>{id} &&
              copy.reviewStatus == source.reviewStatus && copy.revisions.size() == 1,
          "conversion identity, text or quality is wrong");
    sameGeometry(source.segments[0], copy.segments[0]);
    snapshotMatches(source, set, copy);
    check(copy.originEvidence->formatId == "manual-v1" && copy.originEvidence->runId.empty() &&
              !copy.originEvidence->scoreMean && copy.originEvidence->originalSegments == copy.segments,
          "Manual baseline fabricated Algorithm evidence or lost conversion geometry");
    check(sameAnnotation(source, *findAnnotation(file, id)) && set == *findSourceSet(file, set.id),
          "conversion modified the source");
    ok(service.rename(file, copy.id, "New manual name", "New manual note"), "edit converted copy");
    check(sameAnnotation(source, *findAnnotation(file, id)), "editing Manual modified source");
    check(validateFileAnnotations(file).ok, "converted file is invalid");
  }
}
void testBatchConversion(int count) {
  auto file = fixture();
  AnnotationService service;
  const auto sources = publish(service, file, AnnotationSource::ImportedLabel, count);
  const auto before = file;
  std::vector<std::string> ids;
  ok(service.convertToManual(file, sources, {}, ids), "batch conversion");
  check(ids.size() == static_cast<std::size_t>(count) && file.sourceSets.size() == 2 &&
            service.historySize(file) == 1, "batch did not publish one source set/command");
  const auto sourceSetId = findAnnotation(file, ids[0])->sourceSetId;
  const auto committed = file;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto* copy = findAnnotation(file, ids[i]);
    check(copy && copy->sourceSetId == sourceSetId && copy->derivedFrom[0] == sources[i],
          "batch source/ID association is wrong");
    check(sameAnnotation(before.marks[i], *findAnnotation(file, sources[i])), "batch modified source");
  }
  ok(service.undo(file), "undo entire batch");
  unchanged(before, file);
  check(!service.canUndo(file) && service.canRedo(file), "batch needed more than one undo");
  ok(service.redo(file), "redo entire batch");
  unchanged(committed, file);
  check(service.historySize(file) == 1 && validateFileAnnotations(file).ok,
        "redo changed batch history or validity");
}
void testSnapshotSurvivesRetirement() {
  auto file = fixture();
  AnnotationService service;
  const auto sourceId = publish(service, file, AnnotationSource::Algorithm)[0];
  const auto source = *findAnnotation(file, sourceId);
  const auto set = *findSourceSet(file, source.sourceSetId);
  std::vector<std::string> ids;
  ok(service.convertToManual(file, {sourceId}, {}, ids), "convert before retirement");
  const auto copyId = ids[0];
  std::erase_if(file.marks, [&](const auto& value) { return value.id == sourceId; });
  std::erase_if(file.sourceSets, [&](const auto& value) { return value.id == set.id; });
  snapshotMatches(source, set, *findAnnotation(file, copyId));
  check(validateFileAnnotations(file).ok && service.canEdit(file, copyId),
        "Manual copy depends on retired objects");
  ok(service.undo(file), "undo after source retirement");
  check(file.marks.empty() && file.sourceSets.empty(), "undo resurrected old Algorithm data");
  ok(service.redo(file), "redo without source objects/assets");
  snapshotMatches(source, set, *findAnnotation(file, copyId));
  check(!findAnnotation(file, sourceId) && validateFileAnnotations(file).ok,
        "redo resurrected Algorithm source or broke Manual copy");
}
void testSegmentsAndConversionBaseline() {
  auto file = fixture();
  AnnotationService service;
  auto set = sourceSet(file, "segments", AnnotationSource::ImportedLabel);
  auto source = annotation(set, "multi", {geometry(), geometry(701, 901)});
  source.segments[1].frequencyFrame = FrequencyFrame::Baseband;
  source.segments[1].range.frequency = {-40'000, 40'000};
  source.segments[1].timeQuality = TimeQuality::SampleLabel;
  source.segments[1].frequencyQuality = FrequencyQuality::ModelEstimated;
  source.segments[1].confirmed = true;
  source.activeSegmentId = source.segments[1].id;
  // Current geometry differs from the original immutable source baseline.
  source.segments[0].range.time.begin = 111;
  source.observation = geometry(10, 1000);
  source.observationTimeQuality = TimeQuality::WindowObservation;
  refreshAnnotationProjection(source);
  ok(service.publishSourceSet(file, set, {source}), "publish multi-segment source");
  source = *findAnnotation(file, source.id);
  std::vector<std::string> all, current;
  ok(service.convertToManual(file, {source.id}, {}, all), "convert all segments");
  ManualConversionOptions options;
  options.currentSegmentOnly = true;
  ok(service.convertToManual(file, {source.id}, options, current), "convert current segment");
  const auto allCopy = *findAnnotation(file, all[0]);
  const auto currentCopy = *findAnnotation(file, current[0]);
  check(allCopy.segments.size() == 2 && currentCopy.segments.size() == 1 &&
            allCopy.observation == source.observation && currentCopy.observation == source.observation,
        "all/current conversion changed observation or segment count");
  sameGeometry(source.segments[0], allCopy.segments[0]);
  sameGeometry(source.segments[1], allCopy.segments[1]);
  sameGeometry(source.segments[1], currentCopy.segments[0]);
  snapshotMatches(source, set, currentCopy);
  check(currentCopy.conversionSnapshot->currentSegmentOnly &&
            currentCopy.conversionSnapshot->convertedSegmentId == source.activeSegmentId &&
            !allCopy.conversionSnapshot->currentSegmentOnly &&
            allCopy.conversionSnapshot->convertedSegmentId.empty(),
        "segment conversion mode was not captured");
  const auto segmentId = allCopy.segments[0].id;
  ok(service.commitGeometry(file, allCopy.id, segmentId, geometry(201, 601)), "edit converted segment");
  ok(service.restoreOrigin(file, allCopy.id), "restore conversion-time baseline");
  check(findAnnotation(file, allCopy.id)->segments[0].range.time.begin == 111 &&
            source.originEvidence->originalSegments[0].range.time.begin == 101,
        "Manual restore used old source origin instead of conversion geometry");
  check(sameAnnotation(source, *findAnnotation(file, source.id)), "segment operations changed source");
}
void testUnknownObservationAndExplicitTime() {
  auto file = fixture();
  AnnotationService service;
  auto set = sourceSet(file, "unknown", AnnotationSource::Algorithm);
  auto source = annotation(set, "frequency-only", {});
  source.observation = ViewRange{{41, 901}, {-20'000, 20'000}};
  source.observationFrequencyFrame = FrequencyFrame::Baseband;
  source.observationTimeQuality = TimeQuality::Unknown;
  auto evidence = std::make_shared<OriginEvidence>(*source.originEvidence);
  evidence->originalObservation = source.observation;
  evidence->observationFrequencyFrame = source.observationFrequencyFrame;
  source.originEvidence = std::move(evidence);
  ok(service.publishSourceSet(file, set, {source}), "publish unknown-time observation");
  source = *findAnnotation(file, source.id);
  std::vector<std::string> ids;
  ok(service.convertToManual(file, {source.id}, {}, ids), "convert unknown-time observation");
  const auto copyId = ids[0];
  const auto copy = *findAnnotation(file, copyId);
  check(copy.segments.empty() && copy.activeSegmentId.empty() && copy.range == ViewRange{} &&
            copy.observation == source.observation &&
            copy.observationTimeQuality == TimeQuality::Unknown &&
            copy.observationFrequencyFrame == FrequencyFrame::Baseband,
        "conversion fabricated a located time segment");
  snapshotMatches(source, set, copy);
  ok(service.commitObservationFrequency(file, copyId, {-15'000, 15'000}), "edit Manual observation frequency");
  check(findAnnotation(file, copyId)->segments.empty(), "frequency edit fabricated activity time");
  ok(service.addSegment(file, copyId, geometry(321, 451)), "provide explicit Manual time");
  check(findAnnotation(file, copyId)->segments[0].range.time == TimeRange{321, 451},
        "explicit time was replaced with observation support");
  ok(service.restoreOrigin(file, copyId), "restore observation baseline");
  check(findAnnotation(file, copyId)->segments.empty() &&
            findAnnotation(file, copyId)->observation == source.observation,
        "restoration fabricated observation time");
  ManualConversionOptions options;
  options.currentSegmentOnly = true;
  const auto before = file;
  check(!service.convertToManual(file, {source.id}, options, ids) && ids.empty(),
        "current-segment conversion accepted an observation with no active segment");
  unchanged(before, file);
}
void testDuplicateModesAndRevisions() {
  auto file = fixture();
  AnnotationService service;
  auto set = sourceSet(file, "duplicates", AnnotationSource::ImportedLabel);
  auto source = annotation(set, "multi", {geometry(), geometry(701, 901)});
  ok(service.publishSourceSet(file, set, {source}), "publish duplicate fixture");
  std::vector<std::string> ids;
  ok(service.convertToManual(file, {source.id, source.id}, {}, ids), "deduplicate input selection");
  check(ids.size() == 1, "repeated input ID produced multiple copies");
  const auto firstCopyId = ids[0];
  auto before = file;
  const auto history = service.historySize(file);
  const auto repeated = service.convertToManual(file, {source.id}, {}, ids);
  check(repeated.ok && !repeated.changed && ids.empty() && service.historySize(file) == history,
        "default repeated conversion created another copy/command");
  unchanged(before, file);
  ManualConversionOptions options;
  options.currentSegmentOnly = true;
  ok(service.convertToManual(file, {source.id}, options, ids), "convert first active segment");
  check(ids.size() == 1, "all/current mode was incorrectly deduplicated");
  findAnnotation(file, source.id)->activeSegmentId = source.segments[1].id;
  refreshAnnotationProjection(*findAnnotation(file, source.id));
  ok(service.convertToManual(file, {source.id}, options, ids), "convert second active segment");
  check(ids.size() == 1, "distinct active segments were incorrectly deduplicated");
  options = {};
  options.allowExplicitDuplicate = true;
  ok(service.convertToManual(file, {source.id, source.id}, options, ids), "explicit duplicate");
  check(ids.size() == 1 && ids[0] != firstCopyId, "explicit duplicate did not create one independent copy");
  auto* revised = findAnnotation(file, source.id);
  const auto parent = revised->revisionId;
  revised->revisionId = "multi:r1";
  revised->reviewRevisionId = revised->revisionId;
  revised->revisions.push_back({revised->revisionId, parent, "fixture-new-source-revision", "source", "2", {}});
  ok(service.convertToManual(file, {source.id}, {}, ids), "convert different source revision");
  check(ids.size() == 1 && findAnnotation(file, ids[0])->conversionSnapshot->sourceRevisionId == "multi:r1",
        "different source revisions were incorrectly deduplicated");
  before = file;
  check(!service.convertToManual(file, {source.id, "missing"}, {}, ids) && ids.empty(),
        "existing duplicate suppressed invalid batch input");
  unchanged(before, file);
}
void testInvalidConversionIsAtomic() {
  for (int defect = 0; defect < 18; ++defect) {
    auto file = fixture();
    AnnotationService service;
    const auto ids = publish(service, file, AnnotationSource::ImportedLabel, 2);
    auto* bad = findAnnotation(file, ids[1]);
    auto requested = ids;
    if (defect == 0) requested[1] = "missing";
    if (defect == 1) bad->deleted = true;
    if (defect == 2) bad->fileId = "wrong-file";
    if (defect == 3) bad->segments[0].range.time.end = file.metadata.sampleCount + 1;
    if (defect == 4) bad->segments[0].range.frequency.upperHz = 200'000'000;
    if (defect == 5) bad->segments[0].range.frequency.lowerHz = std::numeric_limits<double>::quiet_NaN();
    if (defect == 6) bad->segments[0].timeQuality = TimeQuality::Unknown;
    if (defect == 7) bad->revisionId = "missing-revision";
    if (defect == 8) bad->sourceKind = AnnotationSource::Algorithm;
    if (defect == 9) {
      auto set = sourceSet(file, "wrong-channel", AnnotationSource::ImportedLabel);
      file.metadata.sampleFormat.channels = 2;
      set.sourceIdentity = annotationSourceIdentity(file.metadata);
      set.sampleChannel = 1;
      set.sourceIdentity.selectedSampleChannel = 1;
      bad->sourceSetId = set.id;
      file.sourceSets.push_back(std::move(set));
    }
    if (defect == 10) findSourceSet(file, bad->sourceSetId)->sourceIdentity.centerFrequencyHz += 1;
    if (defect == 11) {
      const auto manual = service.createManual(file, "Manual", geometry());
      ok(manual, "create invalid conversion Manual input");
      requested[1] = manual.id;
    }
    if (defect == 12) findSourceSet(file, bad->sourceSetId)->deleted = true;
    if (defect == 13) file.marks.push_back(*bad);
    if (defect == 14) file.sourceSets.push_back(file.sourceSets.front());
    if (defect == 15) bad->activeSegmentId = "foreign-segment";
    if (defect == 16) bad->originEvidence.reset();
    if (defect == 17) file.metadata.sampleFormat.channels = 0;
    const auto before = file;
    const auto history = service.historySize(file);
    std::vector<std::string> created{"stale-output"};
    check(!service.convertToManual(file, requested, {}, created) && created.empty(),
          "invalid conversion batch succeeded, defect " + std::to_string(defect));
    unchanged(before, file);
    check(service.historySize(file) == history, "invalid conversion changed command history");
  }
}
void testBudgetsAndRedoPreflight() {
  auto file = fixture();
  AnnotationService service;
  const auto id = publish(service, file, AnnotationSource::Algorithm)[0];
  const auto filler = service.createManual(file, "Filler", geometry());
  ok(filler, "create budget filler");
  auto* value = findAnnotation(file, filler.id);
  value->segments.reserve(annotationSegmentBudget - 1);
  while (value->segments.size() < annotationSegmentBudget - 1) {
    auto part = value->segments.front();
    part.id = "filler:" + std::to_string(value->segments.size());
    value->segments.push_back(std::move(part));
  }
  const auto marks = file.marks.size();
  const auto sets = file.sourceSets.size();
  const auto history = service.historySize(file);
  std::vector<std::string> ids;
  const auto failed = service.convertToManual(file, {id}, {}, ids);
  check(!failed && failed.error.find("segment budget") != std::string::npos && ids.empty() &&
            file.marks.size() == marks && file.sourceSets.size() == sets &&
            service.historySize(file) == history, "segment budget was not enforced atomically");

  // Control characters expand in JSON. Count their encoded cost before
  // allocating a snapshot, rather than admitting an unsaveable conversion.
  file = fixture();
  service.clear();
  const auto textSource = publish(service, file, AnnotationSource::ImportedLabel)[0];
  auto* textSet = findSourceSet(file, findAnnotation(file, textSource)->sourceSetId);
  textSet->originalText.assign(3 * 1024 * 1024, '\1');
  const auto textLength = textSet->originalText.size();
  const auto textFailed = service.convertToManual(file, {textSource}, {}, ids);
  check(!textFailed && textFailed.error.find("metadata budget") != std::string::npos &&
            file.marks.size() == 1 && file.sourceSets.size() == 1 && ids.empty() &&
            file.sourceSets[0].originalText.size() == textLength && service.historySize(file) == 0,
        "escaped evidence metadata budget was not enforced before publication");

  file = fixture();
  service.clear();
  const auto sources = publish(service, file, AnnotationSource::Algorithm, 2);
  ok(service.convertToManual(file, sources, {}, ids), "convert redo fixture");
  const auto copiedIds = ids;
  ok(service.undo(file), "undo redo fixture");
  // A collision on the last item must not redo the first item or source set.
  auto collision = file.marks.back();
  collision.id = copiedIds.back();
  file.marks.push_back(std::move(collision));
  const auto before = file;
  check(!service.redo(file), "redo ignored an ID conflict");
  unchanged(before, file);
  check(service.canRedo(file), "failed redo consumed the command");
  file.marks.pop_back();
  file.metadata.sampleRateHz *= 2;
  const auto changedSource = file;
  check(!service.redo(file), "redo ignored physical identity change");
  unchanged(changedSource, file);
}
void testSelectionIgnoresOverlayAndTableFilter() {
  auto file = fixture();
  AnnotationService service;
  const auto labels = publish(service, file, AnnotationSource::ImportedLabel, 2);
  const auto algorithm = publish(service, file, AnnotationSource::Algorithm, 1, "algorithm")[0];
  file.annotationLayers = {false, false, false};
  file.annotationFilter.source = static_cast<int>(AnnotationSource::Manual);
  file.annotationFilter.minimumMean = 1;
  const auto view = file.view;
  ok(service.select(file, {labels[0], algorithm}, algorithm), "cross-tab tree selection");
  check(file.selectedMarkIds.size() == 2 && file.activeMarkId == algorithm && file.view == view,
        "selection used overlay/tab filtering or changed the view");
  service.reconcileSelection(file);
  check(file.selectedMarkIds.size() == 2, "reconciliation dropped a hidden cross-tab source");
  ok(service.chooseSelection(file, labels[0], AnnotationSelectionMode::Replace, labels),
     "choose hidden label");
  ok(service.chooseSelection(file, labels[1], AnnotationSelectionMode::Range, labels),
     "range-select using table ID order");
  check(file.selectedMarkIds == labels && file.view == view, "range selection ignored initiating ID order");
  check(!service.hover(file, labels[0], findAnnotation(file, labels[0])->activeSegmentId) &&
            file.hoveredMarkId.empty(), "hover ignored hidden overlay");
}
void testLegacyChannelSnapshotIsReadOnly() {
  auto file = fixture();
  AnnotationService service;
  const auto id = publish(service, file, AnnotationSource::Algorithm)[0];
  const auto& value = *findAnnotation(file, id);
  auto snapshot = std::make_shared<AnnotationChannelSnapshot>();
  snapshot->annotationId = id;
  snapshot->segmentId = value.activeSegmentId;
  snapshot->revisionId = value.revisionId;
  snapshot->name = "Legacy algorithm channel";
  snapshot->range = geometry();
  snapshot->boundRange = snapshot->range;
  snapshot->sourceKind = value.sourceKind;
  snapshot->reviewStatus = value.reviewStatus;
  snapshot->sourceIdentity = annotationSourceIdentity(file.metadata);
  Channel channel;
  channel.id = "legacy-channel";
  channel.name = snapshot->name;
  channel.annotationId = channel.sourceMarkId = id;
  channel.segmentId = snapshot->segmentId;
  channel.revisionId = snapshot->revisionId;
  channel.sourceSnapshot = snapshot;
  channel.sourceIdentity = snapshot->sourceIdentity;
  channel.sourceTime = snapshot->range.time;
  channel.centerFrequencyHz = 100'000'000;
  channel.bandwidthHz = 100'000;
  channel.outputSampleRateHz = 200'000;
  file.channels.push_back(channel);
  check(validateFileAnnotations(file).ok, "legacy read-only channel no longer loads");
  refreshChannelSourceRelations(file);
  const auto& loaded = file.channels[0];
  check(loaded.sourceSnapshot == snapshot && *loaded.sourceSnapshot == *snapshot &&
            loaded.sourceTime == channel.sourceTime &&
            loaded.outputSampleRateHz == channel.outputSampleRateHz &&
            loaded.centerFrequencyHz == channel.centerFrequencyHz &&
            loaded.bandwidthHz == channel.bandwidthHz &&
            loaded.sourceRelation == ChannelSourceRelation::Current,
        "read-only legacy load changed the creation snapshot or DSP parameters");
}
void testQualityAndRealCoordinates() {
  for (const auto review : {ReviewStatus::Pending, ReviewStatus::Accepted, ReviewStatus::Rejected}) {
    auto file = fixture();
    file.metadata.sampleFormat.structure = SampleStructure::Real;
    file.metadata.sampleFormat.iqLayout = IQLayout::NotApplicable;
    file.metadata.centerFrequencyHz = 0;
    file.view = fullRange(file.metadata);
    AnnotationService service;
    auto set = sourceSet(file, "real", AnnotationSource::ImportedLabel);
    auto source = annotation(set, "real-label", {{{3, 17}, {25'000, 75'000}}});
    source.segments[0].frequencyFrame = FrequencyFrame::RealOneSided;
    source.segments[0].confirmed = review == ReviewStatus::Accepted;
    source.reviewStatus = review;
    auto origin = std::make_shared<OriginEvidence>(*source.originEvidence);
    origin->originalSegments = source.segments;
    source.originEvidence = std::move(origin);
    refreshAnnotationProjection(source);
    ok(service.publishSourceSet(file, set, {source}), "publish real source");
    std::vector<std::string> ids;
    ok(service.convertToManual(file, {source.id}, {}, ids), "convert real source");
    const auto& copy = *findAnnotation(file, ids[0]);
    check(copy.reviewStatus == review && copy.reviewRevisionId == copy.revisionId &&
              copy.segments[0].confirmed == source.segments[0].confirmed,
          "conversion changed review quality");
    sameGeometry(source.segments[0], copy.segments[0]);
    check(validateFileAnnotations(file).ok, "real conversion is invalid");
  }
}
void testManualHistoryBudget() {
  auto file = fixture();
  AnnotationService service;
  const auto result = service.createManual(file, "Manual", geometry());
  ok(result, "create history fixture");
  for (std::size_t i = 0; i < annotationCommandBudget + 5; ++i)
    ok(service.rename(file, result.id, "Name " + std::to_string(i), {}), "fill Manual history");
  check(service.historySize(file) == annotationCommandBudget, "Manual history count budget was exceeded");
  ok(service.undo(file), "undo before invalid conversion");
  const auto before = file;
  std::vector<std::string> ids;
  check(!service.convertToManual(file, {result.id}, {}, ids), "Manual-to-Manual conversion accepted");
  check(service.canRedo(file) && service.historySize(file) == annotationCommandBudget,
        "failed conversion discarded a redo command");
  unchanged(before, file);
}
void testSourceManagementPreservesManualRedo() {
  auto file = fixture();
  AnnotationService service;
  const auto manual = service.createManual(file, "Manual", geometry());
  ok(manual, "create Manual redo fixture");
  ok(service.undo(file), "undo Manual before source management");
  publish(service, file, AnnotationSource::ImportedLabel, 1, "import");
  publish(service, file, AnnotationSource::Algorithm, 1, "detect");
  check(service.canRedo(file) && service.historySize(file) == 1,
        "source management overwrote the Manual redo history");
  const auto source0 = file.marks[0], source1 = file.marks[1];
  ok(service.redo(file), "redo Manual after source management");
  check(service.canEdit(file, manual.id) &&
            sameAnnotation(source0, file.marks[0]) && sameAnnotation(source1, file.marks[1]),
        "redo modified read-only source objects");
}
void testPhysicalTailIsPreserved() {
  auto file = fixture();
  file.metadata.availability.status = LoadStatus::Partial;
  file.metadata.availability.availableSamples = 300;
  AnnotationService service;
  const auto id = publish(service, file, AnnotationSource::ImportedLabel)[0];
  std::vector<std::string> ids;
  ok(service.convertToManual(file, {id}, {}, ids), "convert valid physical tail");
  const auto& copy = *findAnnotation(file, ids[0]);
  check(copy.segments[0].range.time == TimeRange{101, 501} &&
            copy.conversionSnapshot->sourceSet.sourceIdentity.availableSamples == 300,
        "conversion truncated physical label geometry to the loaded prefix");
  Channel channel;
  const auto request = annotationChannelRequest(file, copy.id, {100, 500});
  check(!prepareAnnotationChannel(file, request, channel),
        "conversion pretended unread physical samples were available for DDC");
}
}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> cases = {
      {"capability_matrix_and_invalid_identity", testCapabilities},
      {"read_only_service_write_apis_and_channels", testReadOnlyWriteApis},
      {"mixed_review_delete_batch_atomicity", testMixedBatchesAreAtomic},
      {"manual_edit_review_channel_undo_redo", testManualCommandsRemainUsable},
      {"imported_algorithm_single_conversion", testSingleConversions},
      {"ten_item_atomic_conversion_undo_redo", [] { testBatchConversion(10); }},
      {"hundred_item_atomic_conversion_undo_redo", [] { testBatchConversion(100); }},
      {"snapshot_survives_source_retirement", testSnapshotSurvivesRetirement},
      {"all_current_segments_frames_and_restore_baseline", testSegmentsAndConversionBaseline},
      {"unknown_observation_and_explicit_manual_time", testUnknownObservationAndExplicitTime},
      {"duplicate_modes_segments_and_revisions", testDuplicateModesAndRevisions},
      {"invalid_conversion_batch_atomicity", testInvalidConversionIsAtomic},
      {"segment_metadata_budgets_and_redo_preflight", testBudgetsAndRedoPreflight},
      {"selection_context_independent_of_overlay_and_tab", testSelectionIgnoresOverlayAndTableFilter},
      {"legacy_channel_snapshot_read_only_load", testLegacyChannelSnapshotIsReadOnly},
      {"review_quality_and_real_one_sided_coordinates", testQualityAndRealCoordinates},
      {"manual_command_history_budget", testManualHistoryBudget},
      {"source_management_preserves_manual_redo", testSourceManagementPreservesManualRedo},
      {"physical_tail_geometry_and_available_prefix", testPhysicalTailIsPreserved}};
  int failures = 0;
  for (const auto& [name, test] : cases) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size() << " passed\n";
  return failures ? 1 : 0;
}
