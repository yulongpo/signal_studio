#include "application/annotation_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {
AnnotationResult fail(std::string error, std::string field = {}) {
  return {false, false, {}, std::move(error), std::move(field)};
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
bool currentIdentity(const FileState& file, const Annotation& annotation) {
  const auto* set = findSourceSet(file, annotation.sourceSetId);
  return validId(annotation.id) && validId(annotation.fileId) &&
         validId(annotation.sourceSetId) && validId(annotation.originRecordId) &&
         validId(annotation.revisionId) && set && validId(set->id) &&
         !set->deleted && set->fileId == file.metadata.id &&
         annotation.fileId == file.metadata.id &&
         set->sourceKind == annotation.sourceKind &&
         set->sampleChannel < file.metadata.sampleFormat.channels &&
         set->sampleChannel == file.metadata.sampleFormat.selectedChannel &&
         set->sourceIdentity.selectedSampleChannel == set->sampleChannel &&
         !sourceChanged(set->sourceIdentity, file.metadata) &&
         std::any_of(annotation.revisions.begin(), annotation.revisions.end(),
                     [&](const auto& revision) {
                       return revision.id == annotation.revisionId;
                     });
}

// Bound retained memory and JSON metadata before copying potentially large
// evidence. Fixed JSON costs conservatively include field names, numeric text,
// optional ranges and punctuation; strings count their actual escaping cost.
// The codec remains authoritative for the complete project's 32 MiB limit.
struct StorageCost {
  std::size_t memory = 0, metadata = 0;
  StorageCost& operator+=(StorageCost other) {
    const auto add = [](std::size_t a, std::size_t b) {
      constexpr auto cap = annotationCommandByteBudget + 1;
      return a >= cap || b >= cap - a ? cap : a + b;
    };
    memory = add(memory, other.memory);
    metadata = add(metadata, other.metadata);
    return *this;
  }
};
StorageCost textCost(std::string_view text) {
  StorageCost cost{text.size(), 2};
  for (const unsigned char c : text)
    cost += {0, c < 0x20 ? 6u : (c == '"' || c == '\\' ? 2u : 1u)};
  return cost;
}
StorageCost identityCost(const SourceIdentity& identity) {
  StorageCost cost{sizeof(SourceIdentity), 2048};
  for (const auto* text : {&identity.fileId, &identity.path,
                            &identity.quickFingerprint, &identity.contentHash})
    cost += textCost(*text);
  return cost;
}
StorageCost sourceSetCost(const AnnotationSourceSet& set) {
  StorageCost cost{sizeof(AnnotationSourceSet), 1024};
  for (const auto* text :
       {&set.id, &set.fileId, &set.formatId, &set.parserId, &set.parserVersion,
        &set.profileId, &set.profileVersion, &set.originalPath,
        &set.originalText, &set.sourceHash, &set.supplementalOriginalText,
        &set.supplementalHash, &set.coordinateInterpretation, &set.runId})
    cost += textCost(*text);
  cost += identityCost(set.sourceIdentity);
  for (const auto& diagnostic : set.diagnostics) {
    cost += {sizeof(AnnotationDiagnostic), 256};
    cost += textCost(diagnostic.code);
    cost += textCost(diagnostic.message);
    cost += textCost(diagnostic.originRecordId);
  }
  return cost;
}
StorageCost segmentCost(const Segment& segment) {
  StorageCost cost{sizeof(Segment), 512};
  cost += textCost(segment.id);
  return cost;
}
StorageCost originCost(const OriginEvidence& origin) {
  StorageCost cost{sizeof(OriginEvidence), 1024};
  for (const auto* text : {&origin.formatId, &origin.originRef,
                            &origin.originalFieldsJson,
                            &origin.coordinateInterpretation, &origin.runId,
                            &origin.parentCandidateId, &origin.fallbackReason})
    cost += textCost(*text);
  for (const auto& segment : origin.originalSegments) cost += segmentCost(segment);
  for ([[maybe_unused]] const auto window : origin.windowIds)
    cost += {sizeof(std::uint64_t), 32};
  for (const auto& ref : origin.evidenceRefs) {
    cost += {sizeof(std::string), 0};
    cost += textCost(ref);
  }
  return cost;
}
StorageCost conversionSnapshotCost(const Annotation& source,
                                    const AnnotationSourceSet& set) {
  StorageCost cost{sizeof(ManualConversionSnapshot), 1024};
  cost += sourceSetCost(set);
  for (const auto* text : {&source.id, &source.revisionId, &source.originRecordId,
                            &source.name, &source.note, &source.activeSegmentId})
    cost += textCost(*text);
  if (source.classLabel) cost += textCost(*source.classLabel);
  for (const auto& segment : source.segments) cost += segmentCost(segment);
  cost += originCost(*source.originEvidence);
  return cost;
}
StorageCost annotationCost(const Annotation& annotation) {
  StorageCost cost{sizeof(Annotation), 1024};
  for (const auto* text :
       {&annotation.id, &annotation.name, &annotation.fileId,
        &annotation.sourceSetId, &annotation.originRecordId, &annotation.note,
        &annotation.activeSegmentId, &annotation.revisionId,
        &annotation.reviewRevisionId})
    cost += textCost(*text);
  if (annotation.classLabel) cost += textCost(*annotation.classLabel);
  for (const auto& id : annotation.derivedFrom) {
    cost += {sizeof(std::string), 0};
    cost += textCost(id);
  }
  for (const auto& segment : annotation.segments) cost += segmentCost(segment);
  if (annotation.originEvidence) cost += originCost(*annotation.originEvidence);
  if (annotation.conversionSnapshot) {
    const auto& snapshot = *annotation.conversionSnapshot;
    cost += {sizeof(ManualConversionSnapshot), 1024};
    cost += sourceSetCost(snapshot.sourceSet);
    for (const auto* text :
         {&snapshot.sourceAnnotationId, &snapshot.sourceRevisionId,
          &snapshot.sourceOriginRecordId, &snapshot.sourceName,
          &snapshot.sourceNote, &snapshot.sourceActiveSegmentId,
          &snapshot.convertedSegmentId})
      cost += textCost(*text);
    if (snapshot.sourceClassLabel) cost += textCost(*snapshot.sourceClassLabel);
    for (const auto& segment : snapshot.sourceSegments) cost += segmentCost(segment);
    cost += originCost(snapshot.sourceOriginEvidence);
  }
  for (const auto& revision : annotation.revisions) {
    cost += {sizeof(AnnotationRevision), 1024};
    for (const auto* text : {&revision.id, &revision.parentRevisionId,
                              &revision.action, &revision.actor,
                              &revision.timestamp, &revision.note})
      cost += textCost(*text);
    for (const auto* text : {&revision.nameBefore, &revision.nameAfter,
                              &revision.noteBefore, &revision.noteAfter})
      if (*text) cost += textCost(**text);
    for (const auto& change : revision.segmentChanges) {
      cost += {sizeof(SegmentChange), 256};
      cost += textCost(change.id);
      if (change.before) cost += segmentCost(*change.before);
      if (change.after) cost += segmentCost(*change.after);
    }
  }
  for (const auto& review : annotation.reviewHistory) {
    cost += {sizeof(ReviewDecision), 256};
    for (const auto* text : {&review.revisionId, &review.actor,
                              &review.timestamp, &review.note})
      cost += textCost(*text);
  }
  return cost;
}
StorageCost fileAnnotationCost(const FileState& file) {
  StorageCost cost;
  for (const auto& set : file.sourceSets) cost += sourceSetCost(set);
  for (const auto& annotation : file.marks) cost += annotationCost(annotation);
  return cost;
}
ViewRange physicalRange(ViewRange range, FrequencyFrame frame, double center) {
  if (frame == FrequencyFrame::Baseband) {
    range.frequency.lowerHz += center;
    range.frequency.upperHz += center;
  }
  return range;
}
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
AnnotationEditContext annotationEditContext(const FileState& file,
                                            const std::string& id,
                                            std::uint64_t generation) {
  const auto* annotation = findAnnotation(file, id);
  return {generation,
          file.metadata.sampleFormat.selectedChannel,
          file.metadata.id,
          id,
          annotation ? annotation->revisionId : std::string{},
          annotation ? annotation->activeSegmentId : std::string{}};
}
bool annotationEditContextMatches(const FileState& file,
                                  const AnnotationEditContext& context,
                                  std::uint64_t generation) {
  const auto* annotation = findAnnotation(file, context.annotationId);
  return annotation && !annotation->deleted &&
         context.projectGeneration == generation &&
         context.fileId == file.metadata.id &&
         context.sampleChannel == file.metadata.sampleFormat.selectedChannel &&
         file.activeMarkId == context.annotationId &&
         annotation->revisionId == context.baseRevisionId &&
         annotation->activeSegmentId == context.segmentId;
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
      channel.revisionId.empty())
    return ChannelSourceRelation::LegacyNeedsReview;
  const auto& snapshot = *channel.sourceSnapshot;
  auto channelMetadata = file.metadata;
  channelMetadata.sampleFormat.selectedChannel = snapshot.sourceIdentity.selectedSampleChannel;
  const auto* annotation = findAnnotation(file, channel.annotationId);
  if (!annotation || annotation->deleted) {
    if (snapshot.sourceKind == AnnotationSource::Algorithm && file.currentDetectionSlots)
      for (const auto& slot : *file.currentDetectionSlots)
        if (slot.sampleChannel == snapshot.sourceIdentity.selectedSampleChannel &&
            std::find(slot.retiredAnnotationIds.begin(), slot.retiredAnnotationIds.end(),
                      snapshot.annotationId) != slot.retiredAnnotationIds.end())
          return sourceChanged(snapshot.sourceIdentity, channelMetadata)
                     ? ChannelSourceRelation::Changed : ChannelSourceRelation::Retired;
    return ChannelSourceRelation::Missing;
  }
  const auto* sourceSet = findSourceSet(file, annotation->sourceSetId);
  if (!sourceSet || sourceSet->deleted) return ChannelSourceRelation::Missing;
  const auto binding = snapshot.boundRange.value_or(snapshot.range);
  if (snapshot.bindingKind == ChannelBindingKind::LocatedSegment) {
    const auto* segment = findSegment(*annotation, channel.segmentId);
    if (!segment) return ChannelSourceRelation::Missing;
    if (physicalRange(segment->range, segment->frequencyFrame,
                      file.metadata.centerFrequencyHz) != binding)
      return ChannelSourceRelation::Changed;
  } else if (snapshot.bindingKind == ChannelBindingKind::UserRange) {
    if (!annotation->segments.empty() || !annotation->observation)
      return ChannelSourceRelation::Changed;
    if (physicalRange(*annotation->observation,
                      annotation->observationFrequencyFrame,
                      file.metadata.centerFrequencyHz)
            .frequency != binding.frequency)
      return ChannelSourceRelation::Changed;
  } else {
    return ChannelSourceRelation::Changed;
  }
  if (annotation->sourceKind != snapshot.sourceKind ||
      sourceSet->fileId != file.metadata.id || annotation->fileId != file.metadata.id ||
      sourceSet->sourceKind != annotation->sourceKind ||
      sourceChanged(sourceSet->sourceIdentity, channelMetadata) ||
      sourceChanged(snapshot.sourceIdentity, channelMetadata))
    return ChannelSourceRelation::Changed;
  return ChannelSourceRelation::Current;
}
bool annotationSourceCurrent(const FileState& file,
                             const Annotation& annotation) {
  return !annotation.deleted && currentIdentity(file, annotation);
}
AnnotationCapabilities annotationCapabilities(const FileState& file,
                                               const Annotation& annotation) {
  AnnotationCapabilities result;
  if (annotation.deleted || !validId(annotation.id) ||
      annotation.fileId != file.metadata.id || !annotationInContext(file, annotation))
    return result;
  result.view = result.select = result.locate = result.exportData = true;
  if (!annotationSourceCurrent(file, annotation)) return result;
  switch (annotation.sourceKind) {
    case AnnotationSource::Manual:
      result.editGeometry = result.editMetadata = result.manageSegments = true;
      result.review = result.deleteAnnotation = result.createChannelDirectly = true;
      break;
    case AnnotationSource::ImportedLabel:
    case AnnotationSource::Algorithm:
      result.convertToManual = true;
      break;
  }
  return result;
}

AnnotationChannelRequest annotationChannelRequest(
    const FileState& file, const std::string& id, TimeRange visibleTime,
    std::uint64_t generation, const std::string& segmentId) {
  AnnotationChannelRequest request;
  request.annotationId = id;
  request.sourceIdentity = annotationSourceIdentity(file.metadata, generation);
  const auto* annotation = findAnnotation(file, id);
  if (!annotation || annotation->deleted) return request;
  request.name = annotation->name;
  const auto* segment = segmentId.empty() ? activeSegment(*annotation)
                                          : findSegment(*annotation, segmentId);
  if (segment) {
    request.segmentId = segment->id;
    request.boundRange = physicalRange(segment->range, segment->frequencyFrame,
                                       file.metadata.centerFrequencyHz);
    request.range = request.boundRange;
  } else if (annotation->segments.empty() && annotation->observation) {
    request.bindingKind = ChannelBindingKind::UserRange;
    request.boundRange = physicalRange(*annotation->observation,
                                       annotation->observationFrequencyFrame,
                                       file.metadata.centerFrequencyHz);
    request.range = {visibleTime, request.boundRange.frequency};
  }
  if (request.bindingKind == ChannelBindingKind::UserRange) {
    const auto prefix = availableSamples(file.metadata);
    request.range.time.begin = std::min(request.range.time.begin, prefix);
    request.range.time.end = std::min(request.range.time.end, prefix);
  }
  const double bandwidth =
      request.range.frequency.upperHz - request.range.frequency.lowerHz;
  Channel proposed;
  proposed.bandwidthHz = bandwidth;
  proposed.centerFrequencyHz = (request.range.frequency.lowerHz + request.range.frequency.upperHz)/2;
  proposed.sampling = request.sampling;
  const auto rate = planChannelOutputRate(file.metadata, proposed);
  request.outputSampleRateHz = rate.valid ? rate.realizedRateHz : 0;
  return request;
}

AnnotationResult prepareAnnotationChannel(
    const FileState& file, const AnnotationChannelRequest& request,
    Channel& candidate) {
  const auto* annotation = findAnnotation(file, request.annotationId);
  if (!annotation || annotation->deleted)
    return fail("来源标记已删除或不存在，请选择一个来源对象。", "annotationId");
  if (!annotationCapabilities(file, *annotation).createChannelDirectly)
    return fail("只有身份有效的人工标记可以直接创建通道，请先转换为人工标记。",
                "annotationId");
  if (!annotationSourceCurrent(file, *annotation) ||
      request.sourceIdentity.fileId.empty() ||
      sourceChanged(request.sourceIdentity, file.metadata))
    return fail("来源文件、样本通道或物理解释已变化，请重新选择来源。",
                "source");
  if (file.metadata.sampleFormat.structure != SampleStructure::ComplexIQ)
    return fail("实数 ADC 尚未实现解析信号转换，不能创建复数 DDC 通道。",
                "source");
  if (file.metadata.availability.status == LoadStatus::Failed)
    return fail(file.metadata.availability.error.empty()
                    ? "来源读取失败，请重新读入或核对文件。"
                    : "来源读取失败：" + file.metadata.availability.error,
                "source");
  std::string formatError;
  if (!validateSampleFormat(file.metadata.sampleFormat, formatError))
    return fail("来源样本格式无效：" + formatError, "source");
  const auto bounds = annotationBounds(file.metadata);
  if (!std::isfinite(file.metadata.sampleRateHz) ||
      file.metadata.sampleRateHz <= 0 ||
      !std::isfinite(file.metadata.centerFrequencyHz) ||
      file.metadata.sampleCount == 0 ||
      !std::isfinite(bounds.effectiveFrequency.lowerHz) ||
      !std::isfinite(bounds.effectiveFrequency.upperHz) ||
      bounds.effectiveFrequency.lowerHz >= bounds.effectiveFrequency.upperHz)
    return fail("来源采样率、中心频率、物理样本数或有效频带无效。", "source");
  if (const auto checked = validateChannelName(request.name); !checked)
    return fail(checked.error, "name");

  if (request.bindingKind == ChannelBindingKind::LocatedSegment) {
    const auto* segment = findSegment(*annotation, request.segmentId);
    if (!segment || !segmentCanCreateChannel(*annotation, request.segmentId))
      return fail("请选择来源对象中的一个明确活动段。", "segmentId");
    if (physicalRange(segment->range, segment->frequencyFrame,
                      file.metadata.centerFrequencyHz) != request.boundRange)
      return fail("所选活动段几何已变化，请重新选择当前段。", "segmentId");
    if (const auto checked = validateSegment(*segment, bounds, false); !checked)
      return fail("来源活动段坐标无效：" + checked.error, "segmentId");
    if (request.range.time.begin < request.boundRange.time.begin ||
        request.range.time.end > request.boundRange.time.end)
      return fail("提取时段必须位于所选活动段内；可裁剪，不能自动扩大。",
                  "sourceTime");
    if (!rangeInside(request.range, request.boundRange))
      return fail("提取频带必须位于所选活动段内；可裁剪，不能扩大或移出。",
                  "frequency");
  } else if (request.bindingKind == ChannelBindingKind::UserRange) {
    if (!request.segmentId.empty() || !annotation->segments.empty() ||
        !annotation->observation)
      return fail("人工提取范围只能绑定时间未定位的频率观察对象。",
                  "segmentId");
    if (physicalRange(*annotation->observation,
                      annotation->observationFrequencyFrame,
                      file.metadata.centerFrequencyHz)
            .frequency != request.boundRange.frequency)
      return fail("来源观察频带已变化，请重新选择来源。", "frequency");
    if (const auto checked = validateAnnotation(*annotation, bounds); !checked)
      return fail("来源观察坐标无效：" + checked.error, "source");
  } else {
    return fail("不支持的通道绑定类型。", "bindingKind");
  }

  const auto& range = request.range;
  if (range.time.begin >= range.time.end)
    return fail("提取时段没有可读样本；请修改时段或显式使用可用前缀。",
                "sourceTime");
  if (range.time.end > availableSamples(file.metadata) ||
      range.time.end > file.metadata.sampleCount)
    return fail("提取结束时间超出已读可用前缀。", "sourceTime");
  const auto sourceBand = fullRange(file.metadata).frequency;
  const auto bandwidth = range.frequency.upperHz - range.frequency.lowerHz;
  if (!std::isfinite(range.frequency.lowerHz) ||
      !std::isfinite(range.frequency.upperHz) || !(bandwidth > 0) ||
      range.frequency.lowerHz < sourceBand.lowerHz ||
      range.frequency.upperHz > sourceBand.upperHz)
    return fail("提取频率必须为有效物理频率，且位于来源有效频带内。",
                "frequency");
  const auto transition = bandwidth * .15;
  if (range.frequency.lowerHz - transition < sourceBand.lowerHz ||
      range.frequency.upperHz + transition > sourceBand.upperHz)
    return fail("提取频带的 FIR 保护带超出来源有效频带。", "frequency");
  if (!std::isfinite(request.outputSampleRateHz) ||
      request.outputSampleRateHz < bandwidth + 2 * transition ||
      request.outputSampleRateHz > file.metadata.sampleRateHz * 4096)
    return fail("输出采样率必须覆盖通带和抗混叠过渡带。", "outputSampleRateHz");
  if (request.filter != ChannelFilter::FastPreview &&
      request.filter != ChannelFilter::Standard &&
      request.filter != ChannelFilter::HighRejection)
    return fail("FIR 滤波配置无效。", "filter");

  candidate.name = request.name;
  candidate.sourceMarkId = request.annotationId;
  candidate.centerFrequencyHz = range.frequency.lowerHz + bandwidth / 2;
  candidate.bandwidthHz = bandwidth;
  candidate.sourceTime = range.time;
  candidate.outputSampleRateHz = request.outputSampleRateHz;
  candidate.sampling = request.sampling;
  candidate.filter = request.filter;
  candidate.wholeSource = false;
  candidate.preserveSourceTime = request.preserveSourceTime;
  return success(request.annotationId);
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
    const bool retired = snapshot.sourceKind == AnnotationSource::Algorithm &&
        file.currentDetectionSlots && std::any_of(file.currentDetectionSlots->begin(), file.currentDetectionSlots->end(),
          [&](const auto& slot) { return slot.sampleChannel == snapshot.sourceIdentity.selectedSampleChannel &&
            std::find(slot.retiredAnnotationIds.begin(), slot.retiredAnnotationIds.end(), snapshot.annotationId) != slot.retiredAnnotationIds.end(); });
    if (!annotation && !retired) return {false, "Channel references a missing annotation"};
    if ((annotation && snapshot.sourceKind != annotation->sourceKind) ||
        (snapshot.bindingKind == ChannelBindingKind::LocatedSegment
             ? snapshot.segmentId.empty()
             : snapshot.bindingKind != ChannelBindingKind::UserRange ||
                   !snapshot.segmentId.empty()))
      return {false, "Channel binding identity or kind is invalid"};
    auto original = file.metadata;
    original.sampleFormat = snapshot.sourceIdentity.sampleFormat;
    original.sampleRateHz = snapshot.sourceIdentity.sampleRateHz;
    original.centerFrequencyHz = snapshot.sourceIdentity.centerFrequencyHz;
    original.sampleCount = snapshot.sourceIdentity.physicalSamples;
    original.effectiveBandwidthHz =
        snapshot.sourceIdentity.effectiveFrequency.upperHz -
        snapshot.sourceIdentity.effectiveFrequency.lowerHz;
    original.availability.status = LoadStatus::Partial;
    original.availability.availableSamples =
        snapshot.sourceIdentity.availableSamples;
    const auto originalBounds = annotationBounds(original);
    Segment captured{"snapshot",
                     snapshot.range,
                     TimeQuality::Manual,
                     FrequencyQuality::Manual,
                     false,
                     0,
                     original.sampleFormat.structure == SampleStructure::Real
                         ? FrequencyFrame::RealOneSided
                         : FrequencyFrame::RF};
    if (const auto checked = validateSegment(captured, originalBounds, false);
        !checked)
      return checked;
    captured.range = snapshot.boundRange.value_or(snapshot.range);
    if (const auto checked = validateSegment(captured, originalBounds, false);
        !checked)
      return checked;
    if (snapshot.bindingKind == ChannelBindingKind::LocatedSegment &&
        !rangeInside(snapshot.range, captured.range))
      return {false, "Channel extraction is outside its captured binding"};
    if (annotation && std::none_of(annotation->revisions.begin(), annotation->revisions.end(),
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
    snapshot->boundRange = snapshot->range;
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
  StorageCost cost{sizeof(Command), 0};
  for (const auto& patch : command.patches) {
    cost += {sizeof(Patch), 0};
    cost += textCost(patch.annotationId);
    for (const auto* value : {&patch.before, &patch.after})
      for (const auto* text : {&value->name, &value->note,
                                &value->activeSegmentId, &value->revisionId,
                                &value->reviewRevisionId})
        cost += textCost(*text);
    for (const auto& change : patch.segments) {
      cost += {sizeof(SegmentChange), 0};
      cost += textCost(change.id);
      if (change.before) cost += segmentCost(*change.before);
      if (change.after) cost += segmentCost(*change.after);
    }
    if (patch.createdAnnotation) cost += annotationCost(*patch.createdAnnotation);
  }
  if (command.createdSourceSet) cost += sourceSetCost(*command.createdSourceSet);
  return cost.memory;
}
std::optional<AnnotationResult> AnnotationService::blockedWrite() const {
  if (!writeGuard_) return std::nullopt;
  std::string error;
  if (writeGuard_(error)) return std::nullopt;
  if (error.empty()) error = "检测结果正在提交，请稍后再试";
  return fail(std::move(error));
}
void AnnotationService::notifyWrote() {
  if (wrote_) wrote_();
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
  notifyWrote();
}

AnnotationResult AnnotationService::createManual(FileState& file,
                                                 std::string name,
                                                 ViewRange range,
                                                 std::string actor) {
  if (const auto blocked = blockedWrite()) return *blocked;
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
  if (const auto blocked = blockedWrite()) return *blocked;
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
  const auto id = set.id;
  const auto source = set.sourceKind;
  Command command;
  // Algorithm publication is not undoable and skips the command budget, so it
  // does not copy each annotation into command.patches. Manual undo is unchanged.
  if (source != AnnotationSource::Algorithm) {
    command.createdSourceSet = set;
    for (const auto& annotation : annotations) {
      Patch patch;
      patch.annotationId = annotation.id;
      patch.created = true;
      patch.createdAnnotation = annotation;
      patch.after = state(annotation);
      command.patches.push_back(std::move(patch));
    }
    if (commandBytes(command) > annotationCommandByteBudget)
      return fail(
          "Atomic import exceeds the annotation command memory budget (128 MiB)");
  }
  // Allocate publication/history storage before making any object visible.
  file.sourceSets.reserve(file.sourceSets.size() + 1);
  file.marks.reserve(file.marks.size() + annotations.size());
  if (source == AnnotationSource::Manual) {
    auto& history = histories_[file.metadata.id];
    history.past.reserve(history.past.size() + 1);
  }
  file.sourceSets.push_back(std::move(set));
  file.marks.insert(file.marks.end(),
                    std::make_move_iterator(annotations.begin()),
                    std::make_move_iterator(annotations.end()));
  // Source imports and worker publications are source-management operations,
  // outside the manual undo stack. Conversion publishes a new Manual set.
  if (source == AnnotationSource::Manual) record(file, std::move(command));
  else notifyWrote();
  refreshChannelSourceRelations(file);
  return success(id, true);
}

AnnotationResult AnnotationService::convertToManual(
    FileState& file, const std::vector<std::string>& sourceIds,
    const ManualConversionOptions& options,
    std::vector<std::string>& createdIds) {
  createdIds.clear();
  if (sourceIds.empty()) return success();
  if (const auto blocked = blockedWrite()) return *blocked;
  std::string formatError;
  if (!validateSampleFormat(file.metadata.sampleFormat, formatError))
    return fail("Conversion source format is invalid: " + formatError, "source");
  const auto bounds = annotationBounds(file.metadata);
  const auto index = annotationIndex(file);
  if (index.size() != file.marks.size())
    return fail("Conversion source IDs are ambiguous", "annotationId");
  std::unordered_set<std::string> sourceSetIds;
  for (const auto& set : file.sourceSets)
    if (!sourceSetIds.insert(set.id).second)
      return fail("Conversion source-set IDs are ambiguous", "source");
  std::unordered_set<std::string> selected;
  struct Input {
    const Annotation* annotation;
    const AnnotationSourceSet* sourceSet;
    const Segment* segment;
  };
  std::vector<Input> inputs;
  auto cost = fileAnnotationCost(file);
  StorageCost commandCost{sizeof(Command) + sizeof(AnnotationSourceSet), 4096};
  std::size_t count = segmentCount(file);
  if (count > annotationSegmentBudget ||
      file.marks.size() > annotationSegmentBudget ||
      file.sourceSets.size() >= annotationSegmentBudget)
    return fail("Conversion exceeds the file object/segment budget (100000)");

  // Index existing conversion keys once; a 100-item selection must not rescan
  // a large result collection once per selected ID.
  const auto key = [](const std::string& id, const std::string& revision,
                      const std::string& setId, AnnotationSource kind,
                      bool currentOnly, const std::string& segmentId) {
    std::string value;
    for (const auto* part : {&id, &revision, &setId, &segmentId})
      value += std::to_string(part->size()) + ":" + *part;
    return value + ":" + std::to_string(static_cast<int>(kind)) +
           (currentOnly ? ":current" : ":all");
  };
  std::unordered_set<std::string> converted;
  if (!options.allowExplicitDuplicate)
    for (const auto& existing : file.marks) {
      if (existing.deleted || existing.sourceKind != AnnotationSource::Manual ||
          !existing.conversionSnapshot)
        continue;
      const auto& snapshot = *existing.conversionSnapshot;
      converted.insert(key(snapshot.sourceAnnotationId, snapshot.sourceRevisionId,
                            snapshot.sourceSet.id, snapshot.sourceKind,
                            snapshot.currentSegmentOnly,
                            snapshot.convertedSegmentId));
    }
  for (const auto& id : sourceIds) {
    if (!selected.insert(id).second) continue;
    const auto found = index.find(id);
    const auto* source = found == index.end() ? nullptr : found->second;
    if (!source || !annotationCapabilities(file, *source).convertToManual)
      return fail("Conversion requires a current, valid imported/algorithm source: " +
                      id,
                  "annotationId");
    if (const auto checked = validateAnnotation(*source, bounds); !checked)
      return fail("Conversion source " + id + ": " + checked.error, "source");
    const auto* set = findSourceSet(file, source->sourceSetId);
    const auto* segment = options.currentSegmentOnly ? activeSegment(*source) : nullptr;
    if (options.currentSegmentOnly && !segment)
      return fail("Current-segment conversion requires a located segment: " + id,
                  "segmentId");
    const std::string segmentId = segment ? segment->id : std::string{};
    if (!options.allowExplicitDuplicate &&
        converted.contains(key(source->id, source->revisionId, set->id,
                                source->sourceKind, options.currentSegmentOnly,
                                segmentId)))
      continue;
    const auto added = segment ? 1 : source->segments.size();
    if (added > annotationSegmentBudget - count ||
        inputs.size() >= annotationSegmentBudget - file.marks.size())
      return fail("Conversion exceeds the file object/segment budget (100000); "
                  "select a smaller batch");
    count += added;
    // This estimate includes the complete source snapshot and two copies of
    // the selected geometry (live geometry and the Manual restore baseline).
    auto addedCost = conversionSnapshotCost(*source, *set);
    addedCost += {sizeof(Annotation) + sizeof(OriginEvidence) +
                      sizeof(AnnotationRevision) + sizeof(ReviewDecision) +
                      sizeof(Patch) + 4096,
                  8192};
    addedCost += textCost(source->name);
    addedCost += textCost(source->note);
    if (source->classLabel) addedCost += textCost(*source->classLabel);
    for (const auto& part : source->segments) {
      if (segment && part.id != segment->id) continue;
      addedCost += segmentCost(part);
      addedCost += segmentCost(part);
    }
    commandCost += addedCost;
    cost += addedCost;
    if (commandCost.memory > annotationCommandByteBudget)
      return fail("Atomic conversion exceeds the command memory budget (128 MiB); "
                  "select a smaller batch");
    if (cost.metadata > annotationMetadataByteBudget)
      return fail("Conversion exceeds the annotation metadata budget (32 MiB); "
                  "select a smaller batch");
    inputs.push_back({source, set, segment});
  }
  if (inputs.empty()) return success();

  // Build the occupied identity set once for the whole transaction. Calling
  // nextId for each copied segment would rescan all existing segments for
  // every new ID and turn a multi-segment batch into quadratic work.
  std::unordered_set<std::string> identities;
  for (const auto& set : file.sourceSets) identities.insert(set.id);
  for (const auto& value : file.marks) {
    identities.insert(value.id);
    for (const auto& segment : value.segments) identities.insert(segment.id);
    for (const auto& revision : value.revisions) identities.insert(revision.id);
  }
  const auto freshId = [&](const std::string& prefix) {
    for (;;) {
      auto id = prefix + std::to_string(nextIdentifier_++);
      if (identities.insert(id).second) return id;
    }
  };
  AnnotationSourceSet manualSet;
  manualSet.id = freshId("manual-conversion-");
  manualSet.fileId = file.metadata.id;
  manualSet.sampleChannel = file.metadata.sampleFormat.selectedChannel;
  manualSet.sourceKind = AnnotationSource::Manual;
  manualSet.formatId = "manual-v1";
  manualSet.parserId = "signal-studio-manual-conversion";
  manualSet.parserVersion = "1";
  manualSet.coordinateInterpretation = "Per-segment original frequency frame; half-open samples";
  manualSet.sourceIdentity = annotationSourceIdentity(file.metadata);
  std::vector<Annotation> copies;
  std::vector<std::string> ids;
  copies.reserve(inputs.size());
  ids.reserve(inputs.size());
  const auto timestamp = now();
  auto finalCost = fileAnnotationCost(file);
  finalCost += sourceSetCost(manualSet);
  for (const auto& input : inputs) {
    const auto& source = *input.annotation;
    auto snapshot = std::make_shared<ManualConversionSnapshot>();
    snapshot->sourceAnnotationId = source.id;
    snapshot->sourceRevisionId = source.revisionId;
    snapshot->sourceOriginRecordId = source.originRecordId;
    snapshot->sourceKind = source.sourceKind;
    snapshot->sourceSet = *input.sourceSet;
    snapshot->sourceName = source.name;
    snapshot->sourceNote = source.note;
    snapshot->sourceClassLabel = source.classLabel;
    snapshot->sourceSegments = source.segments;
    snapshot->sourceObservation = source.observation;
    snapshot->sourceObservationFrequencyFrame = source.observationFrequencyFrame;
    snapshot->sourceObservationTimeQuality = source.observationTimeQuality;
    snapshot->sourceActiveSegmentId = source.activeSegmentId;
    snapshot->sourceReviewStatus = source.reviewStatus;
    snapshot->sourceOriginEvidence = *source.originEvidence;
    snapshot->currentSegmentOnly = options.currentSegmentOnly;
    if (input.segment) snapshot->convertedSegmentId = input.segment->id;

    Annotation copy;
    copy.id = freshId("annotation-");
    copy.fileId = file.metadata.id;
    copy.sourceSetId = manualSet.id;
    copy.sourceKind = AnnotationSource::Manual;
    copy.originRecordId = source.originRecordId;
    copy.revisionId = freshId("revision-");
    copy.name = source.name;
    copy.note = source.note;
    copy.classLabel = source.classLabel;
    copy.observation = source.observation;
    copy.observationFrequencyFrame = source.observationFrequencyFrame;
    copy.observationTimeQuality = source.observationTimeQuality;
    copy.reviewStatus = source.reviewStatus;
    copy.reviewRevisionId = copy.revisionId;
    copy.derivedFrom = {source.id};
    copy.conversionSnapshot = std::move(snapshot);
    for (const auto& original : source.segments) {
      if (input.segment && original.id != input.segment->id) continue;
      auto part = original;
      part.id = freshId("segment-");
      if (original.id == source.activeSegmentId || input.segment)
        copy.activeSegmentId = part.id;
      copy.segments.push_back(std::move(part));
    }
    auto baseline = std::make_shared<OriginEvidence>();
    baseline->formatId = "manual-v1";
    baseline->originRef = copy.id;
    baseline->coordinateInterpretation = input.sourceSet->coordinateInterpretation;
    baseline->originalSegments = copy.segments;
    baseline->originalObservation = copy.observation;
    baseline->observationFrequencyFrame = copy.observationFrequencyFrame;
    copy.originEvidence = std::move(baseline);
    copy.revisions.push_back(
        {copy.revisionId, {}, "convert-to-manual", "user", timestamp, {}});
    copy.reviewHistory.push_back({copy.reviewStatus, copy.revisionId, "user",
                                  timestamp, "Copied source quality at conversion"});
    refreshAnnotationProjection(copy);
    if (const auto checked = validateAnnotation(copy, bounds); !checked)
      return fail(checked.error);
    finalCost += annotationCost(copy);
    if (finalCost.metadata > annotationMetadataByteBudget)
      return fail("Conversion exceeds the annotation metadata budget (32 MiB); "
                  "select a smaller batch");
    ids.push_back(copy.id);
    copies.push_back(std::move(copy));
  }
  // publishSourceSet performs the final batch validation and records exactly
  // one Manual command with this batch's new source set. Selection stays with
  // the caller, which can switch its source tab before selecting createdIds.
  auto result = publishSourceSet(file, std::move(manualSet), std::move(copies));
  if (!result) return result;
  createdIds = std::move(ids);
  result.id = createdIds.front();
  return result;
}

AnnotationResult AnnotationService::beginRevision(FileState& file,
                                                  const std::string& id) {
  if (const auto blocked = blockedWrite()) return *blocked;
  if (!canEdit(file, id))
    return fail("Only a Manual annotation with a current identity can be edited");
  const auto* annotation = findAnnotation(file, id);
  if (!annotation || annotation->deleted)
    return fail("Annotation is unavailable");
  file.revisionEditingAnnotationId = id;
  file.revisionEditingBaseRevisionId = annotation->revisionId;
  return success(id);
}
void AnnotationService::endRevision(FileState& file) {
  if (blockedWrite()) return;
  file.revisionEditingAnnotationId.clear();
  file.revisionEditingBaseRevisionId.clear();
}
bool AnnotationService::canEdit(const FileState& file,
                                const std::string& id) const {
  return capabilities(file, id).editGeometry;
}
AnnotationCapabilities AnnotationService::capabilities(
    const FileState& file, const std::string& id) const {
  const auto* annotation = findAnnotation(file, id);
  return annotation ? annotationCapabilities(file, *annotation)
                    : AnnotationCapabilities{};
}

AnnotationResult AnnotationService::commit(FileState& file,
                                           const Annotation& before,
                                           Annotation after, std::string action,
                                           std::string actor) {
  if (const auto blocked = blockedWrite()) return *blocked;
  if (!canEdit(file, before.id) || after.sourceKind != AnnotationSource::Manual)
    return fail("Only a Manual annotation with a current identity can be edited");
  auto initial = difference(before, after);
  if (initial.before == initial.after && initial.segments.empty())
    return success(before.id);
  if (segmentCount(file) - before.segments.size() >
      annotationSegmentBudget - after.segments.size())
    return fail("File activity segment budget exceeded (100000)");
  after.revisionId = nextId(file, "revision-");
  after.reviewRevisionId = after.revisionId;
  const bool geometryChanged = before.segments != after.segments ||
                               before.observation != after.observation;
  if (geometryChanged) {
    after.reviewStatus = ReviewStatus::Pending;
    std::unordered_set<std::string> changedGeometry;
    for (const auto& change : initial.segments)
      if (change.after &&
          (!change.before || change.before->range != change.after->range ||
           change.before->frequencyFrame != change.after->frequencyFrame))
        changedGeometry.insert(change.id);
    for (auto& segment : after.segments)
      if (changedGeometry.contains(segment.id)) segment.confirmed = false;
  }
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
  after.reviewHistory.push_back(
      {after.reviewStatus, after.revisionId, std::move(actor), now(),
       geometryChanged ? "Geometry revision requires review"
                       : "Metadata revision preserves review"});
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
        "Annotation source is unavailable or its physical identity changed");
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
AnnotationResult AnnotationService::commitObservationFrequency(
    FileState& file, const std::string& id, FrequencyRange frequency,
    std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Annotation source is unavailable or its physical identity changed");
  const auto before = *findAnnotation(file, id);
  if (!before.segments.empty() || !before.observation)
    return fail("Frequency-only editing requires an unknown-time observation");
  if (before.observation->frequency == frequency) return success(id);
  auto physicalFrequency = frequency;
  if (before.observationFrequencyFrame == FrequencyFrame::Baseband) {
    physicalFrequency.lowerHz += file.metadata.centerFrequencyHz;
    physicalFrequency.upperHz += file.metadata.centerFrequencyHz;
  }
  const auto band = annotationBounds(file.metadata).effectiveFrequency;
  if (!std::isfinite(physicalFrequency.lowerHz) ||
      !std::isfinite(physicalFrequency.upperHz) ||
      physicalFrequency.lowerHz >= physicalFrequency.upperHz ||
      physicalFrequency.lowerHz < band.lowerHz ||
      physicalFrequency.upperHz > band.upperHz)
    return fail(
        "Frequency range exceeds the effective analysis band or is empty");
  auto after = before;
  after.observation->frequency = frequency;
  return commit(file, before, std::move(after), "observation-frequency",
                std::move(actor));
}
AnnotationResult AnnotationService::addSegment(FileState& file,
                                               const std::string& id,
                                               ViewRange range,
                                               std::string actor) {
  if (!canEdit(file, id))
    return fail(
        "Annotation source is unavailable or its physical identity changed");
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
        "Annotation source is unavailable or its physical identity changed");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  if (!findSegment(after, segmentId))
    return fail("Selected segment does not belong to this annotation");
  if (after.segments.size() == 1)
    return deleteBatch(file, {id}, std::move(actor));
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
        "Annotation source is unavailable or its physical identity changed");
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
        "Annotation source is unavailable or its physical identity changed");
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
        "Annotation source is unavailable or its physical identity changed");
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
        "Annotation source is unavailable or its physical identity changed");
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
        "Annotation source is unavailable or its physical identity changed");
  const auto before = *findAnnotation(file, id);
  auto after = before;
  if (!before.originEvidence) return fail("Original evidence is unavailable");
  const auto& original = before.originEvidence->originalSegments;
  if (before.observation == before.originEvidence->originalObservation &&
      before.segments.size() == original.size() &&
      std::equal(
          before.segments.begin(), before.segments.end(), original.begin(),
          [](const Segment& current, const Segment& source) {
            return current.id == source.id && current.range == source.range &&
                   current.frequencyFrame == source.frequencyFrame;
          }))
    return success(id);
  after.segments = before.originEvidence->originalSegments;
  std::unordered_map<std::string, const Segment*> currentSegments;
  for (const auto& segment : before.segments)
    currentSegments.emplace(segment.id, &segment);
  for (auto& segment : after.segments) {
    const auto current = currentSegments.find(segment.id);
    if (current != currentSegments.end() &&
        current->second->range == segment.range &&
        current->second->frequencyFrame == segment.frequencyFrame)
      segment = *current->second;
  }
  after.observation = before.originEvidence->originalObservation;
  if (!findSegment(after, before.activeSegmentId))
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
  if (const auto blocked = blockedWrite()) return *blocked;
  if (!capabilities(file, id).review)
    return fail("Only a Manual annotation with a current identity can be reviewed");
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
  if (const auto blocked = blockedWrite()) return *blocked;
  if (note.size() > 4096) return fail("Review note exceeds its budget");
  if (status != ReviewStatus::Pending && status != ReviewStatus::Accepted &&
      status != ReviewStatus::Rejected)
    return fail("Invalid review status");
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
    if (!annotationCapabilities(file, *before).review)
      return fail("Batch review requires only Manual annotations with current identities");
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
  if (const auto blocked = blockedWrite()) return *blocked;
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
    if (annotation->sourceKind != AnnotationSource::Manual ||
        !currentIdentity(file, *annotation))
      return fail("Batch delete requires only Manual annotations with current identities");
    if (annotation->deleted) continue;
    if (!annotationCapabilities(file, *annotation).deleteAnnotation)
      return fail("This annotation cannot be deleted");
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
    if (found != index.end() && annotationSelectable(file, *found->second) &&
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
  if (file.activeSegmentId.empty()) file.display.psdFromSelection = false;
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
    return found != index.end() && annotationSelectable(file, *found->second);
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
  if (file.activeSegmentId.empty()) file.display.psdFromSelection = false;
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
  if (!target || !annotationSelectable(file, *target))
    return fail("Annotation is not in the current matching results");
  if (!segmentId.empty() && !findSegment(*target, segmentId))
    return fail("Active segment does not belong to the annotation");
  std::unordered_set<std::string> visible;
  for (const auto& annotation : file.marks)
    if (annotationSelectable(file, annotation)) visible.insert(annotation.id);
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
  if (!capabilities(file, id).createChannelDirectly)
    return fail("Only a Manual annotation with a current identity can create a channel",
                "annotationId");
  if (!annotation || !segmentCanCreateChannel(*annotation, segmentId))
    return fail("Channel creation requires an explicit located segment");
  if (!annotationSourceCurrent(file, *annotation))
    return fail(
        "Annotation source is unavailable, changed, or belongs to another "
        "sample channel");
  if (file.metadata.sampleFormat.structure == SampleStructure::Real)
    return fail("Real ADC sources do not support complex DDC channels");
  if (file.metadata.availability.status == LoadStatus::Failed)
    return fail("Channel source failed to load");
  const auto* segment = findSegment(*annotation, segmentId);
  auto extraction = *segment;
  if (channel.sourceTime.begin < channel.sourceTime.end)
    extraction.range.time = channel.sourceTime;
  if (channel.bandwidthHz > 0 && std::isfinite(channel.centerFrequencyHz) &&
      std::isfinite(channel.bandwidthHz)) {
    extraction.range.frequency = {
        channel.centerFrequencyHz - channel.bandwidthHz / 2,
        channel.centerFrequencyHz + channel.bandwidthHz / 2};
    extraction.frequencyFrame = FrequencyFrame::RF;
  }
  const auto extractionRange =
      physicalRange(extraction.range, extraction.frequencyFrame,
                    file.metadata.centerFrequencyHz);
  const auto bindingRange = physicalRange(
      segment->range, segment->frequencyFrame, file.metadata.centerFrequencyHz);
  if (!rangeInside(extractionRange, bindingRange))
    return fail("Channel extraction must remain inside the bound segment",
                extractionRange.time.begin < bindingRange.time.begin ||
                        extractionRange.time.end > bindingRange.time.end
                    ? "sourceTime"
                    : "frequency");
  if (const auto result =
          validateSegment(extraction, annotationBounds(file.metadata), true);
      !result)
    return fail(result.error);
  auto snapshot = std::make_shared<AnnotationChannelSnapshot>();
  snapshot->annotationId = id;
  snapshot->segmentId = segmentId;
  snapshot->revisionId = annotation->revisionId;
  snapshot->name = annotation->name;
  snapshot->range = extractionRange;
  snapshot->boundRange = bindingRange;
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

AnnotationResult AnnotationService::bindChannelSource(
    FileState& file, Channel& channel,
    const AnnotationChannelRequest& request) const {
  Channel prepared = channel;
  if (const auto checked = prepareAnnotationChannel(file, request, prepared);
      !checked)
    return checked;
  const auto* annotation = findAnnotation(file, request.annotationId);
  auto snapshot = std::make_shared<AnnotationChannelSnapshot>();
  snapshot->annotationId = request.annotationId;
  snapshot->segmentId = request.segmentId;
  snapshot->revisionId = annotation->revisionId;
  snapshot->name = annotation->name;
  snapshot->range = request.range;
  snapshot->bindingKind = request.bindingKind;
  snapshot->boundRange = request.boundRange;
  snapshot->sourceKind = annotation->sourceKind;
  snapshot->reviewStatus = annotation->reviewStatus;
  snapshot->sourceIdentity = annotationSourceIdentity(
      file.metadata, request.sourceIdentity.projectGeneration);
  prepared.annotationId = snapshot->annotationId;
  prepared.segmentId = snapshot->segmentId;
  prepared.revisionId = snapshot->revisionId;
  prepared.sourceIdentity = snapshot->sourceIdentity;
  prepared.sourceSnapshot = std::move(snapshot);
  prepared.sourceRelation = ChannelSourceRelation::Current;
  channel = std::move(prepared);
  return success(request.annotationId, true);
}

AnnotationResult AnnotationService::applyCommand(FileState& file,
                                                 const Command& command,
                                                 bool forward) {
  // Validate every touched value first. Unrelated worker additions stay intact.
  const auto index = annotationIndex(file);
  auto count = segmentCount(file);
  auto cost = forward ? fileAnnotationCost(file) : StorageCost{};
  for (const auto& patch : command.patches) {
    const auto found = index.find(patch.annotationId);
    const auto* annotation = found == index.end() ? nullptr : found->second;
    if (patch.created) {
      if (!patch.createdAnnotation ||
          patch.createdAnnotation->sourceKind != AnnotationSource::Manual ||
          (annotation && (annotation->sourceKind != AnnotationSource::Manual ||
                          !currentIdentity(file, *annotation))))
        return fail("Only Manual commands can be undone or redone");
      if (forward && annotation)
        return fail("Redo identity conflicts with an existing annotation");
      if (forward) {
        const auto& created = *patch.createdAnnotation;
        const auto* set = command.createdSourceSet
                              ? &*command.createdSourceSet
                              : findSourceSet(file, created.sourceSetId);
        if (!set || set->deleted || set->sourceKind != AnnotationSource::Manual ||
            set->id != created.sourceSetId || set->fileId != file.metadata.id ||
            set->sampleChannel != file.metadata.sampleFormat.selectedChannel ||
            sourceChanged(set->sourceIdentity, file.metadata))
          return fail("Redo source identity changed");
        if (const auto checked =
                validateAnnotation(created, annotationBounds(file.metadata));
            !checked)
          return fail(checked.error);
        if (created.segments.size() > annotationSegmentBudget ||
            count > annotationSegmentBudget - created.segments.size())
          return fail("Redo exceeds the file segment budget (100000)");
        count += created.segments.size();
        cost += annotationCost(created);
      }
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
    if (!annotation || annotation->sourceKind != AnnotationSource::Manual ||
        !currentIdentity(file, *annotation))
      return fail("Only Manual commands with current identities can be undone or redone");
    if (!annotation || !sameCommandState(state(*annotation),
                                         forward ? patch.before : patch.after))
      return fail("Command target changed since this revision");
    for (const auto& change : patch.segments) {
      const auto* segment = findSegment(*annotation, change.id);
      const auto& expected = forward ? change.before : change.after;
      if ((expected && (!segment || *segment != *expected)) ||
          (!expected && segment))
        return fail("Command segment changed since this revision");
      const auto& replacement = forward ? change.after : change.before;
      if (expected) --count;
      if (replacement) {
        if (count >= annotationSegmentBudget)
          return fail("Command exceeds the file segment budget (100000)");
        ++count;
      }
    }
  }
  if (command.createdSourceSet) {
    if (command.createdSourceSet->sourceKind != AnnotationSource::Manual)
      return fail("Source management is outside the Manual command history");
    const auto* source = findSourceSet(file, command.createdSourceSet->id);
    if (forward && source) return fail("Redo source-set identity conflicts");
    if (forward) cost += sourceSetCost(*command.createdSourceSet);
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
  if (forward && command.createdSourceSet &&
      cost.metadata > annotationMetadataByteBudget)
    return fail("Redo exceeds the annotation metadata budget (32 MiB)");
  const auto createdCount = static_cast<std::size_t>(std::count_if(
      command.patches.begin(), command.patches.end(),
      [](const auto& patch) { return patch.created; }));
  if (forward &&
      (file.marks.size() > annotationSegmentBudget ||
       createdCount > annotationSegmentBudget - file.marks.size()))
    return fail("Redo exceeds the annotation object budget (100000)");
  if (forward) {
    file.marks.reserve(file.marks.size() + createdCount);
    if (command.createdSourceSet)
      file.sourceSets.reserve(file.sourceSets.size() + 1);
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
  if (const auto blocked = blockedWrite()) return *blocked;
  auto found = histories_.find(file.metadata.id);
  if (found == histories_.end() || found->second.past.empty())
    return fail("No annotation command to undo");
  auto& history = found->second;
  history.future.reserve(history.future.size() + 1);
  const auto result = applyCommand(file, history.past.back(), false);
  if (!result) return result;
  history.future.push_back(std::move(history.past.back()));
  history.past.pop_back();
  return result;
}
AnnotationResult AnnotationService::redo(FileState& file) {
  if (const auto blocked = blockedWrite()) return *blocked;
  auto found = histories_.find(file.metadata.id);
  if (found == histories_.end() || found->second.future.empty())
    return fail("No annotation command to redo");
  auto& history = found->second;
  history.past.reserve(history.past.size() + 1);
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
