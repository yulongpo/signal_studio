#include "application/annotation_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {
AnnotationResult fail(std::string error) {
  return {false, false, {}, std::move(error)};
}
AnnotationResult success(std::string id = {}, bool changed = false) {
  return {true, changed, std::move(id), {}};
}
std::string now() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}
std::size_t segmentCount(const FileState& file) {
  std::size_t count = 0;
  for (const auto& annotation : file.marks) count += annotation.segments.size();
  return count;
}
bool sameSource(const AnnotationSourceSet& a, const AnnotationSourceSet& b) {
  const auto& x = a.sourceIdentity;
  const auto& y = b.sourceIdentity;
  return !a.sourceHash.empty() && a.sourceHash == b.sourceHash &&
         a.supplementalHash == b.supplementalHash && a.fileId == b.fileId &&
         a.sampleChannel == b.sampleChannel && a.sourceKind == b.sourceKind &&
         a.formatId == b.formatId && a.parserId == b.parserId &&
         a.parserVersion == b.parserVersion && a.profileId == b.profileId &&
         a.profileVersion == b.profileVersion &&
         a.coordinateInterpretation == b.coordinateInterpretation &&
         a.runId == b.runId && x.sampleFormat == y.sampleFormat &&
         x.sampleRateHz == y.sampleRateHz &&
         x.centerFrequencyHz == y.centerFrequencyHz &&
         x.physicalSamples == y.physicalSamples &&
         x.formatRevision == y.formatRevision;
}
bool sourceChanged(const SourceIdentity& snapshot,
                   const FileMetadata& metadata) {
  const auto current = annotationSourceIdentity(metadata);
  return snapshot.fileId != current.fileId ||
         snapshot.sampleFormat != current.sampleFormat ||
         snapshot.selectedSampleChannel != current.selectedSampleChannel ||
         snapshot.sampleRateHz != current.sampleRateHz ||
         snapshot.centerFrequencyHz != current.centerFrequencyHz ||
         snapshot.physicalSamples != current.physicalSamples ||
         (!snapshot.path.empty() && snapshot.path != current.path) ||
         (!snapshot.quickFingerprint.empty() &&
          !current.quickFingerprint.empty() &&
          snapshot.quickFingerprint != current.quickFingerprint);
}
std::string defaultManualSourceId(const FileState& file) {
  return "manual:" + file.metadata.id + ":ch" +
         std::to_string(file.metadata.sampleFormat.selectedChannel);
}
bool validId(const std::string& id) { return !id.empty() && id.size() <= 128; }
auto annotationIndex(FileState& file) {
  std::unordered_map<std::string, Annotation*> index;
  index.reserve(file.marks.size());
  for (auto& annotation : file.marks) index.emplace(annotation.id, &annotation);
  return index;
}
bool sameCommandState(const auto& a, const auto& b) {
  return a.name == b.name && a.note == b.note && a.revisionId == b.revisionId &&
         a.reviewRevisionId == b.reviewRevisionId &&
         a.reviewStatus == b.reviewStatus && a.deleted == b.deleted &&
         a.observation == b.observation;
}
}  // namespace

AnnotationBounds annotationBounds(const FileMetadata& metadata) {
  const auto effective = fullRange(metadata).frequency;
  const bool real = metadata.sampleFormat.structure == SampleStructure::Real;
  const FrequencyRange physical =
      real ? FrequencyRange{0, metadata.sampleRateHz / 2}
           : FrequencyRange{
                 metadata.centerFrequencyHz - metadata.sampleRateHz / 2,
                 metadata.centerFrequencyHz + metadata.sampleRateHz / 2};
  return {metadata.sampleCount,
          availableSamples(metadata),
          metadata.sampleRateHz,
          metadata.centerFrequencyHz,
          physical,
          effective,
          metadata.sampleFormat.structure};
}
SourceIdentity annotationSourceIdentity(const FileMetadata& metadata,
                                        std::uint64_t generation) {
  SourceIdentity identity;
  identity.projectGeneration = generation;
  identity.fileId = metadata.id;
  identity.path = metadata.path;
  identity.quickFingerprint = metadata.availability.fingerprint;
  identity.selectedSampleChannel = metadata.sampleFormat.selectedChannel;
  identity.sampleFormat = metadata.sampleFormat;
  identity.sampleRateHz = metadata.sampleRateHz;
  identity.centerFrequencyHz = metadata.centerFrequencyHz;
  identity.physicalSamples = metadata.sampleCount;
  identity.availableSamples = availableSamples(metadata);
  identity.effectiveFrequency = fullRange(metadata).frequency;
  return identity;
}
Annotation* findAnnotation(FileState& file, const std::string& id) {
  return findMark(file, id);
}
const Annotation* findAnnotation(const FileState& file, const std::string& id) {
  return findMark(file, id);
}
AnnotationSourceSet* findSourceSet(FileState& file, const std::string& id) {
  return const_cast<AnnotationSourceSet*>(
      findSourceSet(std::as_const(file), id));
}
const AnnotationSourceSet* findSourceSet(const FileState& file,
                                         const std::string& id) {
  const auto found =
      std::find_if(file.sourceSets.begin(), file.sourceSets.end(),
                   [&](const auto& set) { return set.id == id; });
  return found == file.sourceSets.end() ? nullptr : &*found;
}
ChannelSourceRelation channelSourceRelation(const FileState& file,
                                            const Channel& channel) {
  if (!channel.sourceSnapshot || channel.annotationId.empty() ||
      channel.segmentId.empty() || channel.revisionId.empty())
    return ChannelSourceRelation::LegacyNeedsReview;
  const auto* annotation = findAnnotation(file, channel.annotationId);
  if (!annotation || annotation->deleted) return ChannelSourceRelation::Missing;
  const auto* sourceSet = findSourceSet(file, annotation->sourceSetId);
  if (!sourceSet || sourceSet->deleted) return ChannelSourceRelation::Missing;
  if (annotation->reviewStatus == ReviewStatus::Rejected)
    return ChannelSourceRelation::Rejected;
  const auto* segment = findSegment(*annotation, channel.segmentId);
  if (!segment || annotation->revisionId != channel.revisionId ||
      segment->range != channel.sourceSnapshot->range ||
      sourceChanged(channel.sourceSnapshot->sourceIdentity, file.metadata))
    return ChannelSourceRelation::Changed;
  if (!segmentCanCreateChannel(*annotation, channel.segmentId))
    return ChannelSourceRelation::Unconfirmed;
  return ChannelSourceRelation::Current;
}
bool annotationSourceCurrent(const FileState& file,
                             const Annotation& annotation) {
  const auto* set = findSourceSet(file, annotation.sourceSetId);
  return set && !set->deleted && !annotation.deleted &&
         set->fileId == file.metadata.id &&
         annotation.fileId == file.metadata.id &&
         set->sourceKind == annotation.sourceKind &&
         set->sampleChannel == file.metadata.sampleFormat.selectedChannel &&
         !sourceChanged(set->sourceIdentity, file.metadata);
}
void refreshChannelSourceRelations(FileState& file) {
  for (auto& channel : file.channels)
    channel.sourceRelation = channelSourceRelation(file, channel);
}

AnnotationValidation validateFileAnnotations(const FileState& file) {
  if (segmentCount(file) > annotationSegmentBudget)
    return {false, "File activity segment budget exceeded (100000)"};
  std::unordered_set<std::string> sets, annotations;
  for (const auto& set : file.sourceSets) {
    if (!validId(set.id) || set.fileId != file.metadata.id ||
        !sets.insert(set.id).second ||
        set.sampleChannel >= set.sourceIdentity.sampleFormat.channels ||
        set.sourceIdentity.fileId != file.metadata.id ||
        set.sourceIdentity.selectedSampleChannel != set.sampleChannel)
      return {false,
              "Invalid source-set identity, duplicate ID, or sample channel"};
  }
  const auto bounds = annotationBounds(file.metadata);
  for (const auto& annotation : file.marks) {
    if (annotation.fileId != file.metadata.id ||
        !annotations.insert(annotation.id).second)
      return {false, "Invalid annotation file identity or duplicate ID"};
    const auto* set = findSourceSet(file, annotation.sourceSetId);
    if (!set || set->sourceKind != annotation.sourceKind)
      return {false,
              "Annotation source-set reference or source kind is invalid"};
    if (const auto result = validateAnnotation(annotation, bounds); !result)
      return result;
  }
  for (const auto& channel : file.channels) {
    if (!channel.sourceSnapshot)
      continue;  // Explicit legacy migration is handled by the caller.
    const auto& snapshot = *channel.sourceSnapshot;
    if (channel.annotationId != snapshot.annotationId ||
        channel.segmentId != snapshot.segmentId ||
        channel.revisionId != snapshot.revisionId ||
        snapshot.sourceIdentity.fileId != file.metadata.id)
      return {false, "Channel source snapshot IDs disagree"};
    const auto* annotation = findAnnotation(file, channel.annotationId);
    if (!annotation) return {false, "Channel references a missing annotation"};
    if (std::none_of(annotation->revisions.begin(), annotation->revisions.end(),
                     [&](const auto& revision) {
                       return revision.id == channel.revisionId;
                     }))
      return {false, "Channel references a missing immutable revision"};
  }
  return {};
}

AnnotationValidation attachLegacyAnnotations(FileState& file) {
  // Migration is atomic, and limited to unbound legacy-manual compatibility
  // objects; it never rewrites imported or algorithm evidence.
  auto migrated = file;
  const auto setId = "legacy:" + file.metadata.id;
  bool used = false;
  for (auto& annotation : migrated.marks) {
    if (!annotation.fileId.empty() || !annotation.sourceSetId.empty()) continue;
    if (!annotation.originEvidence ||
        annotation.sourceKind != AnnotationSource::Manual ||
        annotation.originEvidence->formatId != "legacy-manual-v1")
      return {false, "An unbound non-legacy annotation cannot be migrated"};
    used = true;
    annotation.fileId = file.metadata.id;
    annotation.sourceSetId = setId;
    if (file.metadata.sampleFormat.structure == SampleStructure::Real) {
      for (auto& segment : annotation.segments)
        segment.frequencyFrame = FrequencyFrame::RealOneSided;
      auto evidence =
          std::make_shared<OriginEvidence>(*annotation.originEvidence);
      evidence->originalSegments = annotation.segments;
      evidence->observationFrequencyFrame = FrequencyFrame::RealOneSided;
      annotation.originEvidence = std::move(evidence);
    }
    refreshAnnotationProjection(annotation);
  }
  if (used && !findSourceSet(migrated, setId)) {
    AnnotationSourceSet set;
    set.id = setId;
    set.fileId = file.metadata.id;
    set.sourceKind = AnnotationSource::Manual;
    set.sampleChannel = file.metadata.sampleFormat.selectedChannel;
    set.formatId = "legacy-manual-v1";
    set.parserId = "legacy-project-migration";
    set.parserVersion = "1";
    set.sourceIdentity = annotationSourceIdentity(file.metadata);
    set.coordinateInterpretation =
        file.metadata.sampleFormat.structure == SampleStructure::Real
            ? "RealOneSided Hz"
            : "RF Hz";
    migrated.sourceSets.push_back(std::move(set));
  }
  if (const auto result = validateFileAnnotations(migrated); !result)
    return result;
  // Existing DDC parameters define the legacy source snapshot, not any later
  // edits to the corresponding mark.
  for (auto& channel : migrated.channels) {
    if (channel.sourceSnapshot || channel.sourceMarkId.empty()) continue;
    const auto* annotation = findAnnotation(migrated, channel.sourceMarkId);
    if (!annotation) return {false, "Legacy channel references a missing mark"};
    const auto* segment = activeSegment(*annotation);
    if (!segment)
      return {false, "Legacy channel references an observation-only object"};
    auto snapshot = std::make_shared<AnnotationChannelSnapshot>();
    snapshot->annotationId = annotation->id;
    snapshot->segmentId = segment->id;
    snapshot->revisionId = annotation->revisionId;
    snapshot->name = annotation->name;
    snapshot->range = {channel.sourceTime,
                       {channel.centerFrequencyHz - channel.bandwidthHz / 2,
                        channel.centerFrequencyHz + channel.bandwidthHz / 2}};
    snapshot->sourceKind = annotation->sourceKind;
    snapshot->reviewStatus = annotation->reviewStatus;
    snapshot->sourceIdentity = annotationSourceIdentity(file.metadata);
    channel.annotationId = snapshot->annotationId;
    channel.segmentId = snapshot->segmentId;
    channel.revisionId = snapshot->revisionId;
    channel.sourceIdentity = snapshot->sourceIdentity;
    channel.sourceSnapshot = std::move(snapshot);
  }
  refreshChannelSourceRelations(migrated);
  if (const auto* active = findAnnotation(migrated, migrated.activeMarkId))
    migrated.activeSegmentId = active->activeSegmentId;
  file = std::move(migrated);
  return {};
}

std::string AnnotationService::nextId(const FileState& file,
                                      const std::string& prefix) {
  for (;;) {
    auto id = prefix + std::to_string(nextIdentifier_++);
    bool exists = findAnnotation(file, id) || findSourceSet(file, id);
    for (const auto& annotation : file.marks) {
      if (exists) break;
      exists =
          findSegment(annotation, id) != nullptr ||
          std::any_of(annotation.revisions.begin(), annotation.revisions.end(),
                      [&](const auto& revision) { return revision.id == id; });
    }
    if (!exists) return id;
  }
}
AnnotationService::State AnnotationService::state(const Annotation& a) {
  return {a.name,
          a.note,
          a.activeSegmentId,
          a.revisionId,
          a.reviewRevisionId,
          a.reviewStatus,
          a.deleted,
          a.observation};
}
void AnnotationService::installState(Annotation& a, const State& s) {
  a.name = s.name;
  a.note = s.note;
  a.activeSegmentId = s.activeSegmentId;
  a.revisionId = s.revisionId;
  a.reviewRevisionId = s.reviewRevisionId;
  a.reviewStatus = s.reviewStatus;
  a.deleted = s.deleted;
  a.observation = s.observation;
  refreshAnnotationProjection(a);
}
AnnotationService::Patch AnnotationService::difference(
    const Annotation& before, const Annotation& after) {
  Patch patch;
  patch.annotationId = before.id;
  patch.before = state(before);
  patch.after = state(after);
  std::unordered_map<std::string, const Segment*> following;
  for (const auto& segment : after.segments)
    following.emplace(segment.id, &segment);
  for (const auto& segment : before.segments) {
    const auto found = following.find(segment.id);
    if (found == following.end())
      patch.segments.push_back({segment.id, segment, {}});
    else {
      if (segment != *found->second)
        patch.segments.push_back({segment.id, segment, *found->second});
      following.erase(found);
    }
  }
  for (const auto& segment : after.segments)
    if (following.contains(segment.id))
      patch.segments.push_back({segment.id, {}, segment});
  return patch;
}

std::size_t AnnotationService::commandBytes(const Command& command) {
  std::size_t bytes = sizeof(Command);
  for (const auto& patch : command.patches) {
    bytes += sizeof(Patch) + patch.annotationId.size() +
             patch.before.name.size() + patch.after.name.size() +
             patch.before.note.size() + patch.after.note.size();
    for (const auto& change : patch.segments)
      bytes += sizeof(SegmentChange) + change.id.size() +
               (change.before ? change.before->id.size() : 0) +
               (change.after ? change.after->id.size() : 0);
    if (patch.createdAnnotation)
      bytes += sizeof(Annotation) +
               patch.createdAnnotation->segments.size() * sizeof(Segment) +
               patch.createdAnnotation->name.size() +
               patch.createdAnnotation->note.size();
  }
  if (command.createdSourceSet)
    bytes += sizeof(AnnotationSourceSet) +
             command.createdSourceSet->originalText.size() +
             command.createdSourceSet->supplementalOriginalText.size();
  return bytes;
}
void AnnotationService::record(FileState& file, Command command) {
  auto& history = histories_[file.metadata.id];
  for (const auto& discarded : history.future) history.bytes -= discarded.bytes;
  history.future.clear();
  command.bytes = commandBytes(command);
  history.bytes += command.bytes;
  history.past.push_back(std::move(command));
  while (history.past.size() > annotationCommandBudget ||
         history.bytes > annotationCommandByteBudget) {
    history.bytes -= history.past.front().bytes;
    history.past.erase(history.past.begin());
  }
}

AnnotationResult AnnotationService::createManual(FileState& file,
                                                 std::string name,
                                                 ViewRange range,
                                                 std::string actor) {
  Segment segment;
  segment.id = nextId(file, "segment-");
  segment.range = range;
  segment.confirmed = true;
  segment.frequencyFrame =
      file.metadata.sampleFormat.structure == SampleStructure::Real
          ? FrequencyFrame::RealOneSided
          : FrequencyFrame::RF;
  if (const auto result =
          validateSegment(segment, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  if (segmentCount(file) >= annotationSegmentBudget)
    return fail("File activity segment budget exceeded (100000)");
  Annotation annotation;
  annotation.id = nextId(file, "annotation-");
  annotation.name = std::move(name);
  annotation.fileId = file.metadata.id;
  annotation.sourceSetId = defaultManualSourceId(file);
  if (const auto* existing = findSourceSet(file, annotation.sourceSetId);
      existing && (existing->deleted ||
                   sourceChanged(existing->sourceIdentity, file.metadata)))
    annotation.sourceSetId = nextId(file, "manual-source-");
  annotation.originRecordId = annotation.id;
  annotation.sourceKind = AnnotationSource::Manual;
  annotation.revisionId = nextId(file, "revision-");
  annotation.reviewRevisionId = annotation.revisionId;
  annotation.reviewStatus = ReviewStatus::Accepted;
  annotation.segments.push_back(segment);
  annotation.activeSegmentId = segment.id;
  auto evidence = std::make_shared<OriginEvidence>();
  evidence->formatId = "manual-v1";
  evidence->originRef = annotation.id;
  evidence->originalSegments = annotation.segments;
  evidence->refineMethod = RefineMethod::Manual;
  evidence->refineStatus = RefineStatus::Applied;
  annotation.originEvidence = std::move(evidence);
  annotation.revisions.push_back(
      {annotation.revisionId, {}, "create-manual", actor, now(), {}});
  annotation.reviewHistory.push_back({ReviewStatus::Accepted,
                                      annotation.revisionId, actor, now(),
                                      "Manual creation"});
  refreshAnnotationProjection(annotation);
  if (const auto result =
          validateAnnotation(annotation, annotationBounds(file.metadata));
      !result)
    return fail(result.error);
  Command command;
  if (!findSourceSet(file, annotation.sourceSetId)) {
    AnnotationSourceSet set;
    set.id = annotation.sourceSetId;
    set.fileId = file.metadata.id;
    set.sampleChannel = file.metadata.sampleFormat.selectedChannel;
    set.formatId = "manual-v1";
    set.parserId = "signal-studio-manual";
    set.parserVersion = "1";
    set.sourceIdentity = annotationSourceIdentity(file.metadata);
    command.createdSourceSet = set;
    file.sourceSets.push_back(std::move(set));
  } else if (findSourceSet(file, annotation.sourceSetId)->sourceKind !=
             AnnotationSource::Manual)
    return fail("Manual source-set ID is occupied by another source kind");
  Patch patch;
  patch.annotationId = annotation.id;
  patch.created = true;
  patch.createdAnnotation = annotation;
  patch.after = state(annotation);
  command.patches.push_back(std::move(patch));
  file.marks.push_back(annotation);
  record(file, std::move(command));
  select(file, {annotation.id}, annotation.id, segment.id);
  return success(annotation.id, true);
}

AnnotationResult AnnotationService::publishSourceSet(
    FileState& file, AnnotationSourceSet set,
    std::vector<Annotation> annotations) {
  if (!validId(set.id) || set.fileId != file.metadata.id ||
      set.sourceIdentity.fileId != file.metadata.id ||
      set.sampleChannel >= file.metadata.sampleFormat.channels ||
      set.sampleChannel != set.sourceIdentity.selectedSampleChannel)
    return fail(
        "Source-set identity does not match the bound file and channel");
  for (const auto& existing : file.sourceSets) {
    if (!existing.deleted && sameSource(existing, set))
      return success(existing.id);
    if (existing.id == set.id)
      return fail(
          "Source-set ID already exists with conflicting immutable content");
  }
  if (sourceChanged(set.sourceIdentity, file.metadata))
    return fail("Source identity changed before annotation publication");
  const auto existingAnnotations = annotationIndex(file);
  std::unordered_set<std::string> ids;
  std::size_t count = segmentCount(file);
  for (auto& annotation : annotations) {
    if (annotation.fileId != file.metadata.id ||
        annotation.sourceSetId != set.id ||
        annotation.sourceKind != set.sourceKind ||
        !ids.insert(annotation.id).second ||
        existingAnnotations.contains(annotation.id))
      return fail(
          "Annotation batch has conflicting identities or duplicate IDs");
    if (annotation.revisions.empty()) {
      if (annotation.revisionId.empty())
        annotation.revisionId = annotation.id + ":r0";
      annotation.revisions.push_back({annotation.revisionId,
                                      {},
                                      "source-publication",
                                      "source",
                                      now(),
                                      {}});
      annotation.reviewStatus =
          annotation.sourceKind == AnnotationSource::Manual
              ? ReviewStatus::Accepted
              : ReviewStatus::Pending;
      annotation.reviewRevisionId = annotation.revisionId;
      for (auto& segment : annotation.segments)
        segment.confirmed = annotation.reviewStatus == ReviewStatus::Accepted;
      annotation.reviewHistory.push_back({annotation.reviewStatus,
                                          annotation.revisionId,
                                          "source",
                                          now(),
                                          {}});
    }
    refreshAnnotationProjection(annotation);
    if (const auto result =
            validateAnnotation(annotation, annotationBounds(file.metadata));
        !result)
      return fail(result.error);
    annotation.originEvidence =
        std::make_shared<const OriginEvidence>(*annotation.originEvidence);
    if (count > annotationSegmentBudget - annotation.segments.size())
      return fail("File activity segment budget exceeded (100000)");
    count += annotation.segments.size();
  }
  Command command;
  command.createdSourceSet = set;
  for (const auto& annotation : annotations) {
    Patch patch;
    patch.annotationId = annotation.id;
    patch.created = true;
    patch.createdAnnotation = annotation;
    patch.after = state(annotation);
    command.patches.push_back(std::move(patch));
  }
  const auto id = set.id;
  const auto source = set.sourceKind;
  if (source != AnnotationSource::Algorithm &&
      commandBytes(command) > annotationCommandByteBudget)
    return fail(
        "Atomic import exceeds the annotation command memory budget (128 MiB)");
  file.sourceSets.push_back(std::move(set));
  file.marks.insert(file.marks.end(),
                    std::make_move_iterator(annotations.begin()),
                    std::make_move_iterator(annotations.end()));
  // Worker publication is independent of user commands, preserving old
  // undo/redo.
  if (source != AnnotationSource::Algorithm) record(file, std::move(command));
  refreshChannelSourceRelations(file);
  return success(id, true);
}

AnnotationResult AnnotationService::beginRevision(FileState& file,
                                                  const std::string& id) {
  const auto* annotation = findAnnotation(file, id);
  if (!annotation || annotation->deleted)
    return fail("Annotation is unavailable");
  file.revisionEditingAnnotationId = id;
  file.revisionEditingBaseRevisionId = annotation->revisionId;
  return success(id);
}
void AnnotationService::endRevision(FileState& file) {
  file.revisionEditingAnnotationId.clear();
  file.revisionEditingBaseRevisionId.clear();
}
bool AnnotationService::canEdit(const FileState& file,
                                const std::string& id) const {
  const auto* annotation = findAnnotation(file, id);
  const auto* set =
      annotation ? findSourceSet(file, annotation->sourceSetId) : nullptr;
  if (!set || set->deleted ||
      set->sampleChannel != file.metadata.sampleFormat.selectedChannel ||
      sourceChanged(set->sourceIdentity, file.metadata))
    return false;
  return annotation && !annotation->deleted &&
         (annotation->sourceKind == AnnotationSource::Manual ||
          (file.revisionEditingAnnotationId == id &&
           file.revisionEditingBaseRevisionId == annotation->revisionId));
}

AnnotationResult AnnotationService::commit(FileState& file,
                                           const Annotation& before,
                                           Annotation after, std::string action,
                                           std::string actor) {
  auto initial = difference(before, after);
  if (initial.before == initial.after && initial.segments.empty())
    return success(before.id);
  if (segmentCount(file) - before.segments.size() >
      annotationSegmentBudget - after.segments.size())
    return fail("File activity segment budget exceeded (100000)");
  after.revisionId = nextId(file, "revision-");
  after.reviewRevisionId = after.revisionId;
  after.reviewStatus = ReviewStatus::Pending;
  for (auto& segment : after.segments) segment.confirmed = false;
  refreshAnnotationProjection(after);
  auto patch = difference(before, after);
  AnnotationRevision revision;
  revision.id = after.revisionId;
  revision.parentRevisionId = before.revisionId;
  revision.action = std::move(action);
  revision.actor = actor;
  revision.timestamp = now();
  revision.segmentChanges = patch.segments;
  if (before.name != after.name) {
    revision.nameBefore = before.name;
    revision.nameAfter = after.name;
  }
  if (before.note != after.note) {
    revision.noteBefore = before.note;
    revision.noteAfter = after.note;
  }
  revision.observationChanged = before.observation != after.observation;
  if (revision.observationChanged) {
    revision.observationBefore = before.observation;
    revision.observationAfter = after.observation;
  }
  after.revisions.push_back(std::move(revision));
  after.reviewHistory.push_back({ReviewStatus::Pending, after.revisionId,
                                 std::move(actor), now(),
                                 "Revision requires review"});
  if (const auto result =
          validateAnnotation(after, annotationBounds(file.metadata));
      !result)
    return fail(result.error);
  auto* target = findAnnotation(file, before.id);
  if (!target || target->revisionId != before.revisionId)
    return fail("Annotation changed before commit");
  Command command;
  command.patches.push_back(std::move(patch));
  if (commandBytes(command) > annotationCommandByteBudget)
    return fail(
        "Atomic revision exceeds the annotation command memory budget (128 "
        "MiB)");
  *target = std::move(after);
  record(file, std::move(command));
  if (file.revisionEditingAnnotationId == target->id)
    file.revisionEditingBaseRevisionId = target->revisionId;
  if (file.activeMarkId == target->id)
    file.activeSegmentId = target->activeSegmentId;
  refreshChannelSourceRelations(file);
  return success(target->id, true);
}

AnnotationResult AnnotationService::commitGeometry(FileState& file,
                                                   const std::string& id,
                                                   const std::string& segmentId,
                                                   ViewRange range,
                                                   std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Read-only source requires an explicit current revision session");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  auto* segment = findSegment(after, segmentId);
  if (!segment)
    return fail("Selected segment does not belong to this annotation");
  if (segment->range == range) return success(id);
  segment->range = range;
  segment->timeQuality = TimeQuality::Manual;
  segment->frequencyQuality = FrequencyQuality::Manual;
  segment->uncertaintySamples = 0;
  segment->clippedAtRoi = false;
  if (const auto result =
          validateSegment(*segment, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  return commit(file, before, std::move(after), "geometry", std::move(actor));
}
AnnotationResult AnnotationService::addSegment(FileState& file,
                                               const std::string& id,
                                               ViewRange range,
                                               std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Read-only source requires an explicit current revision session");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  Segment segment;
  segment.id = nextId(file, "segment-");
  segment.range = range;
  segment.frequencyFrame =
      file.metadata.sampleFormat.structure == SampleStructure::Real
          ? FrequencyFrame::RealOneSided
          : FrequencyFrame::RF;
  if (const auto result =
          validateSegment(segment, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  after.segments.push_back(segment);
  after.activeSegmentId = segment.id;
  return commit(file, before, std::move(after), "add-segment",
                std::move(actor));
}
AnnotationResult AnnotationService::deleteSegment(FileState& file,
                                                  const std::string& id,
                                                  const std::string& segmentId,
                                                  std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Read-only source requires an explicit current revision session");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  if (!findSegment(after, segmentId))
    return fail("Selected segment does not belong to this annotation");
  if (after.segments.size() == 1 && !after.observation)
    return fail(
        "Delete the object explicitly instead of erasing its last geometry");
  std::erase_if(after.segments,
                [&](const auto& segment) { return segment.id == segmentId; });
  if (after.activeSegmentId == segmentId) after.activeSegmentId.clear();
  return commit(file, before, std::move(after), "delete-segment",
                std::move(actor));
}
AnnotationResult AnnotationService::splitSegment(FileState& file,
                                                 const std::string& id,
                                                 const std::string& segmentId,
                                                 SampleIndex split,
                                                 std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Read-only source requires an explicit current revision session");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  auto* segment = findSegment(after, segmentId);
  if (!segment || split <= segment->range.time.begin ||
      split >= segment->range.time.end)
    return fail("Split sample must lie strictly inside the selected segment");
  auto second = *segment;
  second.id = nextId(file, "segment-");
  second.range.time.begin = split;
  segment->range.time.end = split;
  segment->timeQuality = TimeQuality::Manual;
  segment->uncertaintySamples = 0;
  second.timeQuality = TimeQuality::Manual;
  second.uncertaintySamples = 0;
  if (const auto result =
          validateSegment(*segment, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  if (const auto result =
          validateSegment(second, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  after.segments.push_back(std::move(second));
  return commit(file, before, std::move(after), "split-segment",
                std::move(actor));
}
AnnotationResult AnnotationService::mergeSegments(
    FileState& file, const std::string& id,
    const std::vector<std::string>& segmentIds, std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Read-only source requires an explicit current revision session");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  std::unordered_set<std::string> selected(segmentIds.begin(),
                                           segmentIds.end());
  if (selected.size() < 2 || selected.size() != segmentIds.size())
    return fail("Merge requires distinct segments from one annotation");
  std::vector<Segment> parts;
  for (const auto& selectedId : segmentIds) {
    const auto* segment = findSegment(before, selectedId);
    if (!segment) return fail("Merge cannot cross annotation boundaries");
    parts.push_back(*segment);
  }
  std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) {
    return a.range.time.begin < b.range.time.begin;
  });
  auto merged = parts.front();
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].frequencyFrame != merged.frequencyFrame ||
        parts[i].range.frequency != merged.range.frequency)
      return fail(
          "Merge requires identical frequency bounds and frequency coordinate "
          "frame");
    merged.range.time.end =
        std::max(merged.range.time.end, parts[i].range.time.end);
  }
  merged.timeQuality = TimeQuality::Manual;
  merged.uncertaintySamples = 0;
  if (const auto result =
          validateSegment(merged, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  std::erase_if(after.segments, [&](const auto& segment) {
    return selected.contains(segment.id);
  });
  after.segments.push_back(merged);
  after.activeSegmentId = merged.id;
  const bool includesGap = [&] {
    auto end = parts.front().range.time.end;
    for (std::size_t i = 1; i < parts.size(); ++i) {
      if (parts[i].range.time.begin > end) return true;
      end = std::max(end, parts[i].range.time.end);
    }
    return false;
  }();
  auto result = commit(
      file, before, std::move(after),
      includesGap ? "merge-segments-including-gap-manually" : "merge-segments",
      std::move(actor));
  if (result.changed && includesGap)
    findAnnotation(file, id)->revisions.back().note =
        "Explicit manual merge includes formerly inactive time gaps";
  return result;
}
AnnotationResult AnnotationService::adoptObservation(FileState& file,
                                                     const std::string& id,
                                                     std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Read-only source requires an explicit current revision session");
  const auto* annotation = findAnnotation(file, id);
  if (!annotation->observation || !annotation->segments.empty())
    return fail("Only an observation-only object can adopt its observation");
  const auto before = *annotation;
  auto after = before;
  Segment segment;
  segment.id = nextId(file, "segment-");
  segment.range = *after.observation;
  segment.frequencyFrame = after.observationFrequencyFrame;
  if (const auto result =
          validateSegment(segment, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  after.segments.push_back(segment);
  after.activeSegmentId = segment.id;
  return commit(file, before, std::move(after), "adopt-observation-manually",
                std::move(actor));
}
AnnotationResult AnnotationService::rename(FileState& file,
                                           const std::string& id,
                                           std::string name, std::string note,
                                           std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Read-only source requires an explicit current revision session");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  after.name = std::move(name);
  after.note = std::move(note);
  return commit(file, before, std::move(after), "rename-note",
                std::move(actor));
}
AnnotationResult AnnotationService::restoreOrigin(FileState& file,
                                                  const std::string& id,
                                                  std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Read-only source requires an explicit current revision session");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  if (!before.originEvidence) return fail("Original evidence is unavailable");
  after.segments = before.originEvidence->originalSegments;
  after.observation = before.originEvidence->originalObservation;
  after.activeSegmentId.clear();
  refreshAnnotationProjection(after);
  // Restoration can retain physical imported tails; it never claims those
  // samples have become available for analysis.
  return commit(file, before, std::move(after), "restore-origin",
                std::move(actor));
}

AnnotationResult AnnotationService::confirmSegment(FileState& file,
                                                   const std::string& id,
                                                   const std::string& segmentId,
                                                   std::string actor) {
  auto* annotation = findAnnotation(file, id);
  if (!annotation || annotation->deleted)
    return fail("Annotation is unavailable");
  if (annotation->reviewStatus == ReviewStatus::Rejected)
    return fail("Rejected annotations cannot confirm activity segments");
  const auto* set = findSourceSet(file, annotation->sourceSetId);
  if (!set || set->deleted || set->fileId != file.metadata.id ||
      annotation->fileId != file.metadata.id ||
      set->sourceKind != annotation->sourceKind ||
      set->sampleChannel != file.metadata.sampleFormat.selectedChannel ||
      sourceChanged(set->sourceIdentity, file.metadata))
    return fail(
        "Annotation source is unavailable, changed, or belongs to another "
        "sample channel");
  if (file.metadata.availability.status == LoadStatus::Loading ||
      file.metadata.availability.status == LoadStatus::Failed)
    return fail(
        "Activity confirmation requires a ready source or partial "
        "available prefix");
  const auto bounds = annotationBounds(file.metadata);
  if (const auto result = validateAnnotation(*annotation, bounds); !result)
    return fail(result.error);
  const auto* segment = findSegment(*annotation, segmentId);
  if (!segment)
    return fail("Activity confirmation requires an explicit located segment");
  if (const auto result = validateSegment(*segment, bounds, true); !result)
    return fail(result.error);
  if (segment->confirmed) return success(id);

  auto after = *annotation;
  findSegment(after, segmentId)->confirmed = true;
  after.reviewHistory.push_back({after.reviewStatus, after.revisionId,
                                 std::move(actor), now(),
                                 "Confirmed activity segment: " + segmentId});
  if (const auto result = validateAnnotation(after, bounds); !result)
    return fail(result.error);
  Command command;
  command.patches.push_back(difference(*annotation, after));
  if (commandBytes(command) > annotationCommandByteBudget)
    return fail("Segment confirmation exceeds the annotation command budget");
  *annotation = std::move(after);
  record(file, std::move(command));
  refreshChannelSourceRelations(file);
  return success(id, true);
}

AnnotationResult AnnotationService::reviewBatch(
    FileState& file, const std::vector<std::string>& ids, ReviewStatus status,
    std::string actor, std::string note) {
  if (note.size() > 4096) return fail("Review note exceeds its budget");
  std::unordered_set<std::string> selected;
  std::vector<Annotation> afters;
  Command command;
  const auto index = annotationIndex(file);
  for (const auto& id : ids) {
    if (!selected.insert(id).second) continue;
    const auto found = index.find(id);
    const auto* before = found == index.end() ? nullptr : found->second;
    if (!before || before->deleted)
      return fail("Batch review contains an unavailable annotation");
    if (const auto result =
            validateAnnotation(*before, annotationBounds(file.metadata));
        !result)
      return fail(result.error);
    if (before->reviewStatus == status &&
        before->reviewRevisionId == before->revisionId)
      continue;
    auto after = *before;
    after.reviewStatus = status;
    after.reviewRevisionId = after.revisionId;
    for (auto& segment : after.segments)
      segment.confirmed = status == ReviewStatus::Accepted;
    after.reviewHistory.push_back(
        {status, after.revisionId, actor, now(), note});
    command.patches.push_back(difference(*before, after));
    afters.push_back(std::move(after));
  }
  if (command.patches.empty()) return success();
  if (commandBytes(command) > annotationCommandByteBudget)
    return fail(
        "Atomic review exceeds the annotation command memory budget (128 MiB)");
  for (auto& after : afters) *index.at(after.id) = std::move(after);
  record(file, std::move(command));
  refreshChannelSourceRelations(file);
  return success({}, true);
}
AnnotationResult AnnotationService::deleteBatch(
    FileState& file, const std::vector<std::string>& ids, std::string actor) {
  std::unordered_set<std::string> selected;
  std::vector<Annotation> after;
  Command command;
  const auto index = annotationIndex(file);
  for (const auto& id : ids) {
    if (!selected.insert(id).second) continue;
    const auto found = index.find(id);
    const auto* annotation = found == index.end() ? nullptr : found->second;
    if (!annotation)
      return fail("Batch delete contains an unavailable annotation");
    if (annotation->deleted) continue;
    auto changed = *annotation;
    changed.deleted = true;
    changed.revisionId = nextId(file, "revision-");
    changed.reviewRevisionId = changed.revisionId;
    changed.reviewStatus = ReviewStatus::Pending;
    for (auto& segment : changed.segments) segment.confirmed = false;
    changed.revisions.push_back({changed.revisionId,
                                 annotation->revisionId,
                                 "delete",
                                 actor,
                                 now(),
                                 {}});
    changed.reviewHistory.push_back({ReviewStatus::Pending, changed.revisionId,
                                     actor, now(), "Soft deletion"});
    if (const auto result =
            validateAnnotation(changed, annotationBounds(file.metadata));
        !result)
      return fail(result.error);
    command.patches.push_back(difference(*annotation, changed));
    after.push_back(std::move(changed));
  }
  if (after.empty()) return success();
  if (commandBytes(command) > annotationCommandByteBudget)
    return fail(
        "Atomic deletion exceeds the annotation command memory budget (128 "
        "MiB)");
  for (auto& changed : after) *index.at(changed.id) = std::move(changed);
  record(file, std::move(command));
  std::erase_if(file.selectedMarkIds,
                [&](const auto& id) { return selected.contains(id); });
  if (selected.contains(file.activeMarkId)) {
    file.activeMarkId.clear();
    file.activeSegmentId.clear();
  }
  if (selected.contains(file.revisionEditingAnnotationId)) endRevision(file);
  reconcileSelection(file);
  refreshChannelSourceRelations(file);
  return success({}, true);
}

AnnotationResult AnnotationService::select(FileState& file,
                                           std::vector<std::string> ids,
                                           std::string activeId,
                                           std::string segmentId) {
  reconcileSelection(file);
  auto index = annotationIndex(file);
  std::unordered_set<std::string> selected;
  std::vector<std::string> legal;
  for (auto& id : ids) {
    const auto found = index.find(id);
    if (found != index.end() && annotationVisible(file, *found->second) &&
        selected.insert(id).second)
      legal.push_back(std::move(id));
  }
  if (!activeId.empty() && !selected.contains(activeId))
    return fail("Active annotation is not in the selected set");
  if (activeId.empty() && !legal.empty()) activeId = legal.back();
  auto* annotation = findAnnotation(file, activeId);
  if (annotation && !segmentId.empty() && !findSegment(*annotation, segmentId))
    return fail("Active segment does not belong to the annotation");
  const auto old = file.activeMarkId;
  const bool changed =
      file.selectedMarkIds != legal || old != activeId ||
      (!segmentId.empty() && file.activeSegmentId != segmentId);
  const bool changeAnchor = legal.empty() || file.selectedMarkIds != legal ||
                            old != activeId || file.selectionAnchorId.empty();
  file.selectedMarkIds = std::move(legal);
  file.activeMarkId = std::move(activeId);
  if (changeAnchor) file.selectionAnchorId = file.activeMarkId;
  if (annotation) {
    if (!segmentId.empty()) annotation->activeSegmentId = segmentId;
    refreshAnnotationProjection(*annotation);
    file.activeSegmentId = annotation->activeSegmentId;
  } else
    file.activeSegmentId.clear();
  if (old != file.activeMarkId) endRevision(file);
  return success(file.activeMarkId, changed);
}

void AnnotationService::reconcileSelection(FileState& file) {
  const auto channel = file.metadata.sampleFormat.selectedChannel;
  if (file.selectionSampleChannel && *file.selectionSampleChannel != channel) {
    file.selectedMarkIds.clear();
    file.activeMarkId.clear();
    file.activeSegmentId.clear();
    file.selectionAnchorId.clear();
    file.hoveredMarkId.clear();
    file.hoveredSegmentId.clear();
    endRevision(file);
  }
  file.selectionSampleChannel = channel;
  const auto index = annotationIndex(file);
  const auto visible = [&](const std::string& id) {
    const auto found = index.find(id);
    return found != index.end() && annotationVisible(file, *found->second);
  };
  std::unordered_set<std::string> selected;
  std::erase_if(file.selectedMarkIds, [&](const auto& id) {
    return !visible(id) || !selected.insert(id).second;
  });
  if (!selected.contains(file.activeMarkId)) {
    file.activeMarkId = file.selectedMarkIds.empty()
                            ? std::string{}
                            : file.selectedMarkIds.back();
    endRevision(file);
  }
  if (!visible(file.selectionAnchorId))
    file.selectionAnchorId = file.activeMarkId;
  if (const auto found = index.find(file.activeMarkId); found != index.end()) {
    auto& active = *found->second;
    if (!findSegment(active, active.activeSegmentId))
      refreshAnnotationProjection(active);
    file.activeSegmentId = active.activeSegmentId;
  } else {
    file.activeSegmentId.clear();
  }
  if (!visible(file.hoveredMarkId)) {
    file.hoveredMarkId.clear();
    file.hoveredSegmentId.clear();
  } else if (!findSegment(*index.at(file.hoveredMarkId),
                          file.hoveredSegmentId)) {
    file.hoveredSegmentId.clear();
  }
}

AnnotationResult AnnotationService::chooseSelection(
    FileState& file, const std::string& id, AnnotationSelectionMode mode,
    const std::vector<std::string>& orderedIds, const std::string& segmentId) {
  reconcileSelection(file);
  const auto* target = findAnnotation(file, id);
  if (!target || !annotationVisible(file, *target))
    return fail("Annotation is not in the current matching results");
  if (!segmentId.empty() && !findSegment(*target, segmentId))
    return fail("Active segment does not belong to the annotation");
  std::unordered_set<std::string> visible;
  for (const auto& annotation : file.marks)
    if (annotationVisible(file, annotation)) visible.insert(annotation.id);
  std::vector<std::string> order;
  std::unordered_set<std::string> ordered;
  for (const auto& value : orderedIds)
    if (visible.contains(value) && ordered.insert(value).second)
      order.push_back(value);
  const auto last = std::find(order.begin(), order.end(), id);
  if (last == order.end())
    return fail("Annotation is missing from the initiating view order");
  auto ids = file.selectedMarkIds;
  auto active = id;
  auto anchor = file.selectionAnchorId;
  auto first = std::find(order.begin(), order.end(), anchor);
  if ((mode == AnnotationSelectionMode::Range ||
       mode == AnnotationSelectionMode::ExtendRange) &&
      first != order.end()) {
    if (mode == AnnotationSelectionMode::Range) ids.clear();
    const auto begin = std::min(first, last), end = std::max(first, last);
    ids.insert(ids.end(), begin, end + 1);
  } else if (mode == AnnotationSelectionMode::Toggle) {
    const auto found = std::find(ids.begin(), ids.end(), id);
    if (found == ids.end()) {
      ids.push_back(id);
    } else {
      ids.erase(found);
      active = file.activeMarkId == id
                   ? (ids.empty() ? std::string{} : ids.back())
                   : file.activeMarkId;
    }
    anchor = id;
  } else {
    ids = {id};
    anchor = id;
  }
  const auto result = select(file, std::move(ids), active,
                             active == id ? segmentId : std::string{});
  if (result.ok) file.selectionAnchorId = std::move(anchor);
  return result;
}

bool AnnotationService::hover(FileState& file, std::string id,
                              std::string segmentId) {
  const auto* annotation = findAnnotation(file, id);
  if (!annotation || !annotationVisible(file, *annotation)) {
    id.clear();
    segmentId.clear();
  } else if (!findSegment(*annotation, segmentId)) {
    segmentId.clear();
  }
  if (file.hoveredMarkId == id && file.hoveredSegmentId == segmentId)
    return false;
  file.hoveredMarkId = std::move(id);
  file.hoveredSegmentId = std::move(segmentId);
  return true;
}
AnnotationResult AnnotationService::bindChannelSource(
    FileState& file, Channel& channel, const std::string& id,
    const std::string& segmentId) const {
  const auto* annotation = findAnnotation(file, id);
  if (!annotation || !segmentCanCreateChannel(*annotation, segmentId))
    return fail("Channel creation requires a confirmed explicit segment");
  const auto* set = findSourceSet(file, annotation->sourceSetId);
  if (!set || set->deleted ||
      set->sampleChannel != file.metadata.sampleFormat.selectedChannel ||
      sourceChanged(set->sourceIdentity, file.metadata))
    return fail(
        "Annotation source is unavailable, changed, or belongs to another "
        "sample channel");
  if (file.metadata.sampleFormat.structure == SampleStructure::Real)
    return fail("Real ADC sources do not support complex DDC channels");
  const auto* segment = findSegment(*annotation, segmentId);
  if (const auto result =
          validateSegment(*segment, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  auto snapshot = std::make_shared<AnnotationChannelSnapshot>();
  snapshot->annotationId = id;
  snapshot->segmentId = segmentId;
  snapshot->revisionId = annotation->revisionId;
  snapshot->name = annotation->name;
  snapshot->range = segment->range;
  snapshot->sourceKind = annotation->sourceKind;
  snapshot->reviewStatus = annotation->reviewStatus;
  snapshot->sourceIdentity = annotationSourceIdentity(file.metadata);
  channel.sourceMarkId = id;
  channel.annotationId = id;
  channel.segmentId = segmentId;
  channel.revisionId = annotation->revisionId;
  channel.sourceIdentity = snapshot->sourceIdentity;
  channel.sourceSnapshot = std::move(snapshot);
  channel.sourceRelation = ChannelSourceRelation::Current;
  return success(id, true);
}

AnnotationResult AnnotationService::applyCommand(FileState& file,
                                                 const Command& command,
                                                 bool forward) {
  // Validate every touched value first. Unrelated worker additions stay intact.
  const auto index = annotationIndex(file);
  for (const auto& patch : command.patches) {
    const auto found = index.find(patch.annotationId);
    const auto* annotation = found == index.end() ? nullptr : found->second;
    if (patch.created) {
      if (forward && annotation)
        return fail("Redo identity conflicts with an existing annotation");
      if (!forward &&
          (!annotation || !sameCommandState(state(*annotation), patch.after)))
        return fail("Undo target changed after its creation");
      if (!forward)
        for (const auto& channel : file.channels)
          if (channel.annotationId == patch.annotationId ||
              channel.sourceMarkId == patch.annotationId)
            return fail("Created annotation is now referenced by a channel");
      continue;
    }
    if (!annotation || !sameCommandState(state(*annotation),
                                         forward ? patch.before : patch.after))
      return fail("Command target changed since this revision");
    for (const auto& change : patch.segments) {
      const auto* segment = findSegment(*annotation, change.id);
      const auto& expected = forward ? change.before : change.after;
      if ((expected && (!segment || *segment != *expected)) ||
          (!expected && segment))
        return fail("Command segment changed since this revision");
    }
  }
  if (command.createdSourceSet) {
    const auto* source = findSourceSet(file, command.createdSourceSet->id);
    if (forward && source) return fail("Redo source-set identity conflicts");
    if (!forward) {
      if (!source || *source != *command.createdSourceSet)
        return fail("Undo source-set changed after publication");
      std::unordered_set<std::string> created;
      for (const auto& patch : command.patches)
        if (patch.created) created.insert(patch.annotationId);
      for (const auto& annotation : file.marks)
        if (annotation.sourceSetId == source->id &&
            !created.contains(annotation.id))
          return fail("Source set is now referenced by another annotation");
      for (const auto& channel : file.channels)
        if (created.contains(channel.annotationId) ||
            created.contains(channel.sourceMarkId))
          return fail("Created annotation is now referenced by a channel");
    }
  }
  for (const auto& patch : command.patches) {
    if (patch.created) {
      if (forward)
        file.marks.push_back(*patch.createdAnnotation);
      else
        std::erase_if(file.marks, [&](const auto& a) {
          return a.id == patch.annotationId;
        });
    } else {
      auto* annotation = index.at(patch.annotationId);
      for (const auto& change : patch.segments) {
        const auto& replacement = forward ? change.after : change.before;
        auto* segment = findSegment(*annotation, change.id);
        if (replacement) {
          if (segment)
            *segment = *replacement;
          else
            annotation->segments.push_back(*replacement);
        } else
          std::erase_if(annotation->segments,
                        [&](const auto& s) { return s.id == change.id; });
      }
      installState(*annotation, forward ? patch.after : patch.before);
    }
  }
  if (command.createdSourceSet) {
    if (forward)
      file.sourceSets.push_back(*command.createdSourceSet);
    else
      std::erase_if(file.sourceSets, [&](const auto& s) {
        return s.id == command.createdSourceSet->id;
      });
  }
  std::erase_if(file.selectedMarkIds, [&](const auto& id) {
    const auto* a = findAnnotation(file, id);
    return !a || a->deleted;
  });
  if (const auto* active = findAnnotation(file, file.activeMarkId);
      active && !active->deleted)
    file.activeSegmentId = active->activeSegmentId;
  else {
    file.activeMarkId.clear();
    file.activeSegmentId.clear();
  }
  endRevision(file);
  reconcileSelection(file);
  refreshChannelSourceRelations(file);
  return success({}, true);
}
AnnotationResult AnnotationService::undo(FileState& file) {
  auto found = histories_.find(file.metadata.id);
  if (found == histories_.end() || found->second.past.empty())
    return fail("No annotation command to undo");
  auto& history = found->second;
  const auto result = applyCommand(file, history.past.back(), false);
  if (!result) return result;
  history.future.push_back(std::move(history.past.back()));
  history.past.pop_back();
  return result;
}
AnnotationResult AnnotationService::redo(FileState& file) {
  auto found = histories_.find(file.metadata.id);
  if (found == histories_.end() || found->second.future.empty())
    return fail("No annotation command to redo");
  auto& history = found->second;
  const auto result = applyCommand(file, history.future.back(), true);
  if (!result) return result;
  history.past.push_back(std::move(history.future.back()));
  history.future.pop_back();
  return result;
}
bool AnnotationService::canUndo(const FileState& file) const {
  const auto found = histories_.find(file.metadata.id);
  return found != histories_.end() && !found->second.past.empty();
}
bool AnnotationService::canRedo(const FileState& file) const {
  const auto found = histories_.find(file.metadata.id);
  return found != histories_.end() && !found->second.future.empty();
}
std::size_t AnnotationService::historySize(const FileState& file) const {
  const auto found = histories_.find(file.metadata.id);
  return found == histories_.end()
             ? 0
             : found->second.past.size() + found->second.future.size();
}
void AnnotationService::clear() { histories_.clear(); }
void AnnotationService::clear(const std::string& fileId) {
  histories_.erase(fileId);
}
}  // namespace signalstudio
