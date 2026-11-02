#include "domain/annotation.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {
AnnotationValidation invalid(std::string error) {
  return {false, std::move(error)};
}
bool validId(const std::string& value) {
  return !value.empty() && value.size() <= 128;
}
bool knownTime(TimeQuality quality) {
  return quality == TimeQuality::SampleLabel ||
         quality == TimeQuality::RoundedLabel ||
         quality == TimeQuality::AlgorithmEstimated ||
         quality == TimeQuality::Manual;
}
bool knownFrequency(FrequencyQuality quality) {
  return quality == FrequencyQuality::NominalLabel ||
         quality == FrequencyQuality::ModelEstimated ||
         quality == FrequencyQuality::Refined ||
         quality == FrequencyQuality::Manual;
}
bool finiteRange(FrequencyRange range) {
  return std::isfinite(range.lowerHz) && std::isfinite(range.upperHz) &&
         range.lowerHz >= -1e12 && range.upperHz <= 1e12 &&
         range.lowerHz < range.upperHz;
}
AnnotationValidation validateRange(ViewRange range, FrequencyFrame frame,
                                   const AnnotationBounds& bounds,
                                   bool available) {
  if (!std::isfinite(bounds.sampleRateHz) || bounds.sampleRateHz <= 0 ||
      !std::isfinite(bounds.centerFrequencyHz) ||
      bounds.availableSamples > bounds.physicalSamples ||
      !finiteRange(bounds.physicalFrequency) ||
      !finiteRange(bounds.effectiveFrequency))
    return invalid("Invalid physical source bounds");
  if (range.time.begin >= range.time.end ||
      range.time.end > bounds.physicalSamples)
    return invalid("Sample range exceeds physical source or is empty");
  if (available && range.time.end > bounds.availableSamples)
    return invalid("Sample range exceeds the available source prefix");
  if (!finiteRange(range.frequency))
    return invalid("Frequency bounds must be finite and ordered");
  auto frequency = range.frequency;
  if (bounds.structure == SampleStructure::Real) {
    if (frame != FrequencyFrame::RealOneSided)
      return invalid(
          "Real ADC annotations require RealOneSided frequency coordinates");
  } else {
    if (frame == FrequencyFrame::RealOneSided)
      return invalid(
          "Complex IQ annotations cannot use RealOneSided coordinates");
    if (frame == FrequencyFrame::Baseband) {
      frequency.lowerHz += bounds.centerFrequencyHz;
      frequency.upperHz += bounds.centerFrequencyHz;
    }
  }
  const auto band =
      available ? bounds.effectiveFrequency : bounds.physicalFrequency;
  if (!std::isfinite(frequency.lowerHz) || !std::isfinite(frequency.upperHz) ||
      frequency.lowerHz < band.lowerHz || frequency.upperHz > band.upperHz)
    return invalid(available
                       ? "Frequency range exceeds the effective analysis band"
                       : "Frequency range exceeds physical Nyquist bounds");
  return {};
}
}  // namespace

AnnotationValidation validateChannelName(std::string_view name) {
  std::size_t units = 0;
  bool hasText = false;
  for (std::size_t offset = 0; offset < name.size();) {
    const auto lead = static_cast<unsigned char>(name[offset++]);
    std::uint32_t codepoint = lead;
    std::size_t trailing = 0;
    if (lead >= 0xc2 && lead <= 0xdf) {
      codepoint &= 0x1f;
      trailing = 1;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      codepoint &= 0x0f;
      trailing = 2;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      codepoint &= 0x07;
      trailing = 3;
    } else if (lead > 0x7f) {
      return invalid("通道名称不是有效的 UTF-8 文本。");
    }
    if (trailing > name.size() - offset)
      return invalid("通道名称不是有效的 UTF-8 文本。");
    for (std::size_t index = 0; index < trailing; ++index) {
      const auto next = static_cast<unsigned char>(name[offset++]);
      if ((next & 0xc0) != 0x80)
        return invalid("通道名称不是有效的 UTF-8 文本。");
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((trailing == 2 && codepoint < 0x800) ||
        (trailing == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
      return invalid("通道名称不是有效的 UTF-8 文本。");
    units += codepoint >= 0x10000 ? 2 : 1;
    if (units > channelNameUtf16Limit)
      return invalid("通道名称最多 80 个字符（补充平面字符计为 2 个）。");
    // Unicode White_Space, matching QString::trimmed() for valid scalar text.
    const bool whitespace =
        (codepoint >= 0x09 && codepoint <= 0x0d) || codepoint == 0x20 ||
        codepoint == 0x85 || codepoint == 0xa0 || codepoint == 0x1680 ||
        (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 ||
        codepoint == 0x2029 || codepoint == 0x202f || codepoint == 0x205f ||
        codepoint == 0x3000;
    hasText = hasText || !whitespace;
  }
  return hasText ? AnnotationValidation{}
                 : invalid("通道名称必须包含有效文本。");
}

Annotation::Annotation(std::string identifier, std::string displayName,
                       ViewRange initialRange)
    : id(std::move(identifier)),
      name(std::move(displayName)),
      range(initialRange) {
  originRecordId = id;
  revisionId = id + ":r0";
  activeSegmentId = id + ":s0";
  segments.push_back({activeSegmentId, initialRange, TimeQuality::Manual,
                      FrequencyQuality::Manual, true});
  reviewStatus = ReviewStatus::Accepted;
  reviewRevisionId = revisionId;
  auto evidence = std::make_shared<OriginEvidence>();
  evidence->formatId = "legacy-manual-v1";
  evidence->originRef = id;
  evidence->originalSegments = segments;
  evidence->refineMethod = RefineMethod::Manual;
  evidence->refineStatus = RefineStatus::Applied;
  originEvidence = std::move(evidence);
  revisions.push_back({revisionId, {}, "legacy-migration", "legacy", {}, {}});
  reviewHistory.push_back(
      {reviewStatus, revisionId, "legacy", {}, "Legacy manual mark"});
}

const Segment* findSegment(const Annotation& annotation,
                           const std::string& id) {
  const auto found =
      std::find_if(annotation.segments.begin(), annotation.segments.end(),
                   [&](const Segment& segment) { return segment.id == id; });
  return found == annotation.segments.end() ? nullptr : &*found;
}
Segment* findSegment(Annotation& annotation, const std::string& id) {
  return const_cast<Segment*>(findSegment(std::as_const(annotation), id));
}
const Segment* activeSegment(const Annotation& annotation) {
  if (const auto* selected =
          findSegment(annotation, annotation.activeSegmentId))
    return selected;
  return annotation.segments.empty() ? nullptr : &annotation.segments.front();
}
void refreshAnnotationProjection(Annotation& annotation) {
  if (const auto* selected = activeSegment(annotation)) {
    annotation.activeSegmentId = selected->id;
    annotation.range = selected->range;
  } else {
    annotation.activeSegmentId.clear();
    annotation.range = {};  // Observation is not an active emission rectangle.
  }
}

AnnotationValidation validateSegment(const Segment& segment,
                                     const AnnotationBounds& bounds,
                                     bool requireAvailable) {
  if (!validId(segment.id)) return invalid("Invalid segment ID");
  if (!knownTime(segment.timeQuality) ||
      !knownFrequency(segment.frequencyQuality))
    return invalid(
        "Unknown or window-only time/frequency belongs in observation, not "
        "active segments");
  return validateRange(segment.range, segment.frequencyFrame, bounds,
                       requireAvailable);
}

AnnotationValidation validateAnnotation(const Annotation& annotation,
                                        const AnnotationBounds& bounds) {
  if (!validId(annotation.id) || !validId(annotation.fileId) ||
      !validId(annotation.sourceSetId) || !validId(annotation.originRecordId) ||
      !validId(annotation.revisionId))
    return invalid(
        "Annotation identity fields must be nonempty and at most 128 bytes");
  if (annotation.name.size() > 256 || annotation.note.size() > 4096 ||
      (annotation.classLabel && annotation.classLabel->size() > 256) ||
      annotation.derivedFrom.size() > 128)
    return invalid("Annotation text or derivation count exceeds its budget");
  if (annotation.segments.size() > annotationSegmentBudget)
    return invalid("Annotation segment budget exceeded");
  if (annotation.segments.empty() && !annotation.observation &&
      !annotation.deleted)
    return invalid(
        "Annotation must have active segments or an explicit observation");
  std::unordered_set<std::string> ids;
  for (const auto& segment : annotation.segments) {
    if (!ids.insert(segment.id).second) return invalid("Duplicate segment ID");
    if (const auto result = validateSegment(segment, bounds, false); !result)
      return result;
  }
  if (!annotation.activeSegmentId.empty() &&
      !ids.contains(annotation.activeSegmentId))
    return invalid("Active segment does not belong to the annotation");
  if (annotation.observation) {
    if (const auto result =
            validateRange(*annotation.observation,
                          annotation.observationFrequencyFrame, bounds, false);
        !result)
      return result;
    if (annotation.observationTimeQuality != TimeQuality::Unknown &&
        annotation.observationTimeQuality != TimeQuality::WindowObservation)
      return invalid(
          "Observation time must remain Unknown or WindowObservation");
  }
  if (!annotation.originEvidence)
    return invalid("Missing immutable origin evidence");
  const auto& origin = *annotation.originEvidence;
  const bool manualFormat =
      origin.formatId == "manual-v1" || origin.formatId == "legacy-manual-v1";
  const bool importedFormat = origin.formatId == "generator-txt-v1" ||
                              origin.formatId == "torchsig-rfbench-sigmf-v1" ||
                              origin.formatId == "mapped-txt-v1";
  if ((annotation.sourceKind == AnnotationSource::Manual && !manualFormat) ||
      (annotation.sourceKind == AnnotationSource::ImportedLabel &&
       !importedFormat) ||
      (annotation.sourceKind == AnnotationSource::Algorithm &&
       origin.formatId != "detection-v1"))
    return invalid("Source kind and immutable origin format disagree");
  if (origin.originRef.size() > 1024 || origin.fallbackReason.size() > 1024 ||
      origin.runId.size() > 128 ||
      origin.windowIds.size() > annotationSegmentBudget ||
      origin.originalSegments.size() > annotationSegmentBudget)
    return invalid("Origin evidence exceeds its field budget");
  const auto scoreValid = [](const std::optional<double>& value) {
    return !value || (std::isfinite(*value) && *value >= 0 && *value <= 1);
  };
  if (!scoreValid(origin.scoreMean) || !scoreValid(origin.scoreMax) ||
      (origin.scoreMean && origin.scoreMax &&
       *origin.scoreMax < *origin.scoreMean) ||
      (origin.estimatedCnrDb && !std::isfinite(*origin.estimatedCnrDb)))
    return invalid("Model scores or estimated energy are invalid");
  if (annotation.sourceKind != AnnotationSource::Algorithm &&
      (origin.scoreMean || origin.scoreMax || origin.estimatedCnrDb ||
       !origin.windowIds.empty() || origin.windowCount != 0 ||
       !origin.runId.empty()))
    return invalid(
        "Manual and imported sources cannot fabricate algorithm evidence");
  std::unordered_set<std::uint64_t> windows;
  for (const auto id : origin.windowIds)
    if (!windows.insert(id).second)
      return invalid("Model window IDs must be unique");
  if (origin.windowIds.size() > origin.windowCount)
    return invalid("Window hits exceed the coverage denominator");
  for (const auto& segment : origin.originalSegments)
    if (const auto result = validateSegment(segment, bounds, false); !result)
      return result;
  if (origin.originalObservation)
    if (const auto result =
            validateRange(*origin.originalObservation,
                          origin.observationFrequencyFrame, bounds, false);
        !result)
      return result;
  std::unordered_set<std::string> revisions;
  for (const auto& revision : annotation.revisions) {
    if (!validId(revision.id) || !revisions.insert(revision.id).second)
      return invalid("Invalid or duplicate revision ID");
    if (!revision.parentRevisionId.empty() &&
        !revisions.contains(revision.parentRevisionId))
      return invalid(
          "Revision parent must refer to an earlier immutable revision");
  }
  if (!revisions.contains(annotation.revisionId))
    return invalid("Current revision does not exist");
  if (annotation.reviewRevisionId != annotation.revisionId)
    return invalid("Review status must bind to the current revision");
  if (annotation.reviewStatus != ReviewStatus::Pending &&
      annotation.reviewStatus != ReviewStatus::Accepted &&
      annotation.reviewStatus != ReviewStatus::Rejected)
    return invalid("Invalid review status");
  for (const auto& review : annotation.reviewHistory)
    if (!revisions.contains(review.revisionId))
      return invalid("Review references a missing revision");
  if (annotation.reviewStatus == ReviewStatus::Accepted) {
    for (const auto& segment : annotation.segments)
      if (!segment.confirmed)
        return invalid(
            "Accepted active segments require explicit confirmation");
  } else if (annotation.reviewStatus == ReviewStatus::Rejected) {
    for (const auto& segment : annotation.segments)
      if (segment.confirmed)
        return invalid("Rejected segments cannot remain confirmed");
  }
  return {};
}

bool annotationConfirmed(const Annotation& annotation) {
  return !annotation.deleted &&
         annotation.reviewStatus == ReviewStatus::Accepted &&
         annotation.reviewRevisionId == annotation.revisionId;
}
bool segmentCanCreateChannel(const Annotation& annotation,
                             const std::string& segmentId) {
  const auto* segment = findSegment(annotation, segmentId);
  const bool currentRevision =
      !annotation.revisionId.empty() &&
      annotation.reviewRevisionId == annotation.revisionId &&
      std::any_of(annotation.revisions.begin(), annotation.revisions.end(),
                  [&](const auto& revision) {
                    return revision.id == annotation.revisionId;
                  });
  return !annotation.deleted &&
         (annotation.reviewStatus == ReviewStatus::Pending ||
          annotation.reviewStatus == ReviewStatus::Accepted ||
          annotation.reviewStatus == ReviewStatus::Rejected) &&
         currentRevision && segment && knownTime(segment->timeQuality) &&
         knownFrequency(segment->frequencyQuality);
}
}  // namespace signalstudio
