#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain/sample_format.h"

namespace signalstudio {

using SampleIndex = std::uint64_t;
struct TimeRange {
  SampleIndex begin = 0, end = 0;
  bool operator==(const TimeRange&) const = default;
};
struct FrequencyRange {
  double lowerHz = 0, upperHz = 0;
  bool operator==(const FrequencyRange&) const = default;
};
struct ViewRange {
  TimeRange time;
  FrequencyRange frequency;
  bool operator==(const ViewRange&) const = default;
};
inline bool rangeInside(const ViewRange& inner, const ViewRange& outer) {
  return inner.time.begin >= outer.time.begin &&
         inner.time.end <= outer.time.end &&
         inner.frequency.lowerHz >= outer.frequency.lowerHz &&
         inner.frequency.upperHz <= outer.frequency.upperHz;
}

inline constexpr std::size_t annotationSegmentBudget = 100'000;
inline constexpr std::size_t annotationCommandBudget = 40;
inline constexpr std::size_t annotationCommandByteBudget = 128 * 1024 * 1024;
inline constexpr std::size_t annotationMetadataByteBudget = 32 * 1024 * 1024;

enum class AnnotationSource { ImportedLabel, Manual, Algorithm };
struct AnnotationCapabilities {
  bool view = false, select = false, locate = false, exportData = false;
  bool convertToManual = false, editGeometry = false, editMetadata = false;
  bool manageSegments = false, review = false, deleteAnnotation = false;
  bool createChannelDirectly = false;
};
enum class ReviewStatus { Pending, Accepted, Rejected };
enum class FrequencyFrame { RF, Baseband, RealOneSided };
enum class TimeQuality {
  SampleLabel,
  RoundedLabel,
  AlgorithmEstimated,
  Manual,
  WindowObservation,
  Unknown
};
enum class FrequencyQuality {
  NominalLabel,
  ModelEstimated,
  Refined,
  Manual,
  Unknown
};
enum class RefineMethod { Coarse, Sample, Roi, None, Manual };
enum class RefineStatus { Applied, Fallback, Skipped, Unknown };
enum class DiagnosticSeverity { Info, Warning, Error };
enum class ChannelBindingKind { LocatedSegment, UserRange };
enum class ChannelSourceRelation {
  Current,
  Changed,
  Missing,
  Rejected,
  Unconfirmed,
  LegacyNeedsReview,
  Retired
};

struct Segment {
  std::string id;
  ViewRange range;
  TimeQuality timeQuality = TimeQuality::Manual;
  FrequencyQuality frequencyQuality = FrequencyQuality::Manual;
  bool confirmed = false;
  SampleIndex uncertaintySamples = 0;
  FrequencyFrame frequencyFrame = FrequencyFrame::RF;
  bool clippedAtRoi = false;
  bool operator==(const Segment&) const = default;
};

struct SourceIdentity {
  std::uint64_t projectGeneration = 0, formatRevision = 1;
  std::string fileId, path, quickFingerprint, contentHash;
  std::uint32_t selectedSampleChannel = 0;
  SampleFormat sampleFormat;
  double sampleRateHz = 0, centerFrequencyHz = 0;
  FrequencyRange effectiveFrequency;
  SampleIndex physicalSamples = 0, availableSamples = 0;
  std::optional<TimeRange> hashedRange;
  bool operator==(const SourceIdentity&) const = default;
};

struct AnnotationDiagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Info;
  std::string code, message, originRecordId;
  std::uint64_t line = 0;
  bool operator==(const AnnotationDiagnostic&) const = default;
};

struct AnnotationSourceSet {
  std::string id, fileId;
  std::uint32_t sampleChannel = 0;
  AnnotationSource sourceKind = AnnotationSource::Manual;
  std::string formatId, parserId, parserVersion, profileId, profileVersion;
  std::string originalPath, originalText, sourceHash;
  std::string supplementalOriginalText, supplementalHash;
  std::string coordinateInterpretation, runId;
  SourceIdentity sourceIdentity;
  std::vector<AnnotationDiagnostic> diagnostics;
  bool deleted = false;
  bool operator==(const AnnotationSourceSet&) const = default;
};
using SourceSet = AnnotationSourceSet;

// Construct once at import/publication, then share as const. Revisions never
// overwrite this evidence, even if the external source file disappears.
struct OriginEvidence {
  std::string formatId, originRef, originalFieldsJson = "{}";
  std::string coordinateInterpretation, runId, parentCandidateId;
  std::vector<Segment> originalSegments;
  std::optional<ViewRange> originalObservation;
  FrequencyFrame observationFrequencyFrame = FrequencyFrame::RF;
  RefineMethod refineMethod = RefineMethod::None;
  RefineStatus refineStatus = RefineStatus::Unknown;
  std::string fallbackReason;
  std::optional<double> scoreMean, scoreMax, estimatedCnrDb;
  std::vector<std::uint64_t> windowIds;
  std::uint64_t windowCount = 0;
  std::vector<std::string> evidenceRefs;
  bool operator==(const OriginEvidence&) const = default;
};

struct SegmentChange {
  std::string id;
  std::optional<Segment> before, after;
  bool operator==(const SegmentChange&) const = default;
};
struct AnnotationRevision {
  std::string id, parentRevisionId, action, actor, timestamp, note;
  std::vector<SegmentChange> segmentChanges;
  std::optional<std::string> nameBefore, nameAfter, noteBefore, noteAfter;
  bool observationChanged = false;
  std::optional<ViewRange> observationBefore, observationAfter;
  bool operator==(const AnnotationRevision&) const = default;
};
struct ReviewDecision {
  ReviewStatus status = ReviewStatus::Pending;
  std::string revisionId, actor, timestamp, note;
  bool operator==(const ReviewDecision&) const = default;
};

// A value-only, self-contained record of the source at conversion time. Freeze
// it behind shared_ptr<const ...>; neither source retirement nor manual edits
// change it. Source IDs are evidence, not references required for loading.
struct ManualConversionSnapshot {
  std::string sourceAnnotationId, sourceRevisionId, sourceOriginRecordId;
  AnnotationSource sourceKind = AnnotationSource::ImportedLabel;
  AnnotationSourceSet sourceSet;
  std::string sourceName, sourceNote;
  std::optional<std::string> sourceClassLabel;
  std::vector<Segment> sourceSegments;
  std::optional<ViewRange> sourceObservation;
  FrequencyFrame sourceObservationFrequencyFrame = FrequencyFrame::RF;
  TimeQuality sourceObservationTimeQuality = TimeQuality::Unknown;
  std::string sourceActiveSegmentId;
  ReviewStatus sourceReviewStatus = ReviewStatus::Pending;
  OriginEvidence sourceOriginEvidence;
  // Deduplication distinguishes all segments from a specific selected segment.
  bool currentSegmentOnly = false;
  std::string convertedSegmentId;
  bool operator==(const ManualConversionSnapshot&) const = default;
};

struct Annotation {
  std::string id, name;
  // Compatibility projection only. v5 persistence uses segments/observation.
  ViewRange range;
  std::string fileId, sourceSetId, originRecordId;
  AnnotationSource sourceKind = AnnotationSource::Manual;
  std::optional<std::string> classLabel;
  std::string note, activeSegmentId, revisionId;
  std::vector<Segment> segments;
  std::optional<ViewRange> observation;
  FrequencyFrame observationFrequencyFrame = FrequencyFrame::RF;
  TimeQuality observationTimeQuality = TimeQuality::Unknown;
  std::shared_ptr<const OriginEvidence> originEvidence;
  std::shared_ptr<const ManualConversionSnapshot> conversionSnapshot;
  ReviewStatus reviewStatus = ReviewStatus::Pending;
  std::string reviewRevisionId;
  std::vector<ReviewDecision> reviewHistory;
  std::vector<AnnotationRevision> revisions;
  std::vector<std::string> derivedFrom;
  bool deleted = false;

  Annotation() = default;
  Annotation(std::string id, std::string name, ViewRange range);
};
using Mark = Annotation;

struct AnnotationChannelSnapshot {
  std::string annotationId, segmentId, revisionId, name;
  ViewRange range;
  AnnotationSource sourceKind = AnnotationSource::Manual;
  ReviewStatus reviewStatus = ReviewStatus::Accepted;
  SourceIdentity sourceIdentity;
  ChannelBindingKind bindingKind = ChannelBindingKind::LocatedSegment;
  // The complete binding geometry at creation. New snapshots store the actual
  // time/frequency crop in `range`; pre-v6 snapshots retain their original
  // range unchanged, with the actual extraction kept in the channel's DSP
  // parameters.
  std::optional<ViewRange> boundRange;
  bool operator==(const AnnotationChannelSnapshot& other) const {
    return annotationId == other.annotationId && segmentId == other.segmentId &&
           revisionId == other.revisionId && name == other.name &&
           range == other.range && sourceKind == other.sourceKind &&
           reviewStatus == other.reviewStatus &&
           sourceIdentity == other.sourceIdentity &&
           bindingKind == other.bindingKind &&
           boundRange.value_or(range) == other.boundRange.value_or(other.range);
  }
};

struct AnnotationBounds {
  SampleIndex physicalSamples = 0, availableSamples = 0;
  double sampleRateHz = 0, centerFrequencyHz = 0;
  FrequencyRange physicalFrequency, effectiveFrequency;
  SampleStructure structure = SampleStructure::ComplexIQ;
};

struct AnnotationValidation {
  bool ok = true;
  std::string error;
  explicit operator bool() const { return ok; }
};

inline constexpr std::size_t channelNameUtf16Limit = 80;
// Channel names persist as Unicode text with a QString-compatible UTF-16 unit
// limit. Reject malformed UTF-8 before any replacement-character conversion.
AnnotationValidation validateChannelName(std::string_view);

const Segment* findSegment(const Annotation&, const std::string& id);
Segment* findSegment(Annotation&, const std::string& id);
const Segment* activeSegment(const Annotation&);
void refreshAnnotationProjection(Annotation&);
AnnotationValidation validateSegment(const Segment&, const AnnotationBounds&,
                                     bool requireAvailable);
AnnotationValidation validateAnnotation(const Annotation&,
                                        const AnnotationBounds&);
bool annotationConfirmed(const Annotation&);
bool segmentCanCreateChannel(const Annotation&, const std::string& segmentId);

}  // namespace signalstudio
