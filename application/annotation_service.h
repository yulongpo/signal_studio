#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/project.h"

namespace signalstudio {

struct AnnotationResult {
  bool ok = false, changed = false;
  std::string id, error;
  std::string field;
  explicit operator bool() const { return ok; }
};

struct ManualConversionOptions {
  bool currentSegmentOnly = false;
  bool allowExplicitDuplicate = false;
};

struct AnnotationChannelRequest {
  std::string annotationId, segmentId, name;
  ChannelBindingKind bindingKind = ChannelBindingKind::LocatedSegment;
  SourceIdentity sourceIdentity;
  ViewRange boundRange, range;
  double outputSampleRateHz = 0;
  ChannelSamplingPolicy sampling{ChannelSamplingMode::RecommendedFactor, 2.0};
  ChannelFilter filter = ChannelFilter::Standard;
  bool preserveSourceTime = true;
};

// An observation supplies frequency only. Its extraction time comes from the
// caller's visible range intersected with the available prefix; an empty
// intersection remains empty until the user chooses another range.
AnnotationChannelRequest annotationChannelRequest(
    const FileState&, const std::string& annotationId, TimeRange visibleTime,
    std::uint64_t projectGeneration = 0, const std::string& segmentId = {});
AnnotationResult prepareAnnotationChannel(const FileState&,
                                          const AnnotationChannelRequest&,
                                          Channel&);
enum class AnnotationSelectionMode { Replace, Toggle, Range, ExtendRange };

// A UI draft is tied to the exact object and segment it started from. Worker
// publications may append other objects without invalidating this identity.
struct AnnotationEditContext {
  std::uint64_t projectGeneration = 0;
  std::uint32_t sampleChannel = 0;
  std::string fileId, annotationId, baseRevisionId, segmentId;
  bool operator==(const AnnotationEditContext&) const = default;
};
AnnotationEditContext annotationEditContext(const FileState&,
                                            const std::string& annotationId,
                                            std::uint64_t projectGeneration);
bool annotationEditContextMatches(const FileState&,
                                  const AnnotationEditContext&,
                                  std::uint64_t projectGeneration);

AnnotationBounds annotationBounds(const FileMetadata&);
SourceIdentity annotationSourceIdentity(const FileMetadata&,
                                        std::uint64_t projectGeneration = 0);
Annotation* findAnnotation(FileState&, const std::string& id);
const Annotation* findAnnotation(const FileState&, const std::string& id);
AnnotationSourceSet* findSourceSet(FileState&, const std::string& id);
const AnnotationSourceSet* findSourceSet(const FileState&,
                                         const std::string& id);
bool annotationSourceCurrent(const FileState&, const Annotation&);
AnnotationCapabilities annotationCapabilities(const FileState&,
                                               const Annotation&);
ChannelSourceRelation channelSourceRelation(const FileState&, const Channel&);
void refreshChannelSourceRelations(FileState&);
AnnotationValidation validateFileAnnotations(const FileState&);
AnnotationValidation attachLegacyAnnotations(FileState&);

// Per-file bounded command histories, separate from view navigation. The
// service owns no FileState pointer; switching/replacing a project must call
// clear().
class AnnotationService {
 public:
  AnnotationResult createManual(FileState&, std::string name, ViewRange,
                                std::string actor = "user");
  // Validates the entire batch before publishing. Same immutable source
  // identity is idempotent; conflicting IDs never overwrite old objects.
  AnnotationResult publishSourceSet(FileState&, AnnotationSourceSet,
                                    std::vector<Annotation>);
  // All inputs are validated before publication. Existing live copies of the
  // same source/revision/segment mode are skipped unless explicitly duplicated.
  // createdIds contains only new IDs, and is empty on failure or a no-op.
  AnnotationResult convertToManual(FileState&,
                                    const std::vector<std::string>& sourceIds,
                                    const ManualConversionOptions&,
                                    std::vector<std::string>& createdIds);
  AnnotationCapabilities capabilities(const FileState&,
                                        const std::string& annotationId) const;
  AnnotationResult beginRevision(FileState&, const std::string& annotationId);
  void endRevision(FileState&);
  bool canEdit(const FileState&, const std::string& annotationId) const;
  AnnotationResult commitGeometry(FileState&, const std::string& annotationId,
                                  const std::string& segmentId, ViewRange,
                                  std::string actor = "user");
  // Observation time remains unknown. Editing frequency never creates a
  // located activity segment or changes its saved observation support.
  AnnotationResult commitObservationFrequency(FileState&,
                                              const std::string& annotationId,
                                              FrequencyRange,
                                              std::string actor = "user");
  AnnotationResult addSegment(FileState&, const std::string& annotationId,
                              ViewRange, std::string actor = "user");
  AnnotationResult deleteSegment(FileState&, const std::string& annotationId,
                                 const std::string& segmentId,
                                 std::string actor = "user");
  AnnotationResult splitSegment(FileState&, const std::string& annotationId,
                                const std::string& segmentId,
                                SampleIndex splitSample,
                                std::string actor = "user");
  // Explicit same-object merge, with identical frequency bounds/frame. A
  // merged time gap is recorded as a manual choice in the immutable revision.
  AnnotationResult mergeSegments(FileState&, const std::string& annotationId,
                                 const std::vector<std::string>& segmentIds,
                                 std::string actor = "user");
  AnnotationResult adoptObservation(FileState&, const std::string& annotationId,
                                    std::string actor = "user");
  AnnotationResult rename(FileState&, const std::string& annotationId,
                          std::string name, std::string note,
                          std::string actor = "user");
  AnnotationResult restoreOrigin(FileState&, const std::string& annotationId,
                                 std::string actor = "user");
  // Confirms only one known, available segment for its current revision. This
  // does not accept the whole object or create a geometry revision; its review
  // record remains append-only under undo/redo, like the other review actions.
  AnnotationResult confirmSegment(FileState&, const std::string& annotationId,
                                  const std::string& segmentId,
                                  std::string actor = "user");
  AnnotationResult reviewBatch(FileState&,
                               const std::vector<std::string>& annotationIds,
                               ReviewStatus, std::string actor = "user",
                               std::string note = {});
  AnnotationResult deleteBatch(FileState&,
                               const std::vector<std::string>& annotationIds,
                               std::string actor = "user");
  // One undoable soft delete of formal algorithm results. Does not create
  // algorithm marks, and does not accept imported labels or manual marks.
  AnnotationResult deleteAlgorithmBatch(
      FileState&, const std::vector<std::string>& annotationIds,
      std::string actor = "user");
  // plotHidden defaults to false. Hidden marks stay in the detection table.
  AnnotationResult setAlgorithmPlotHidden(
      FileState&, const std::vector<std::string>& annotationIds, bool hidden,
      std::string actor = "user");
  AnnotationResult select(FileState&, std::vector<std::string> ids,
                          std::string activeId = {},
                          std::string segmentId = {});
  // The complete order of the initiating view includes unloaded/page-external
  // objects. Selection is intersected with the legal file/channel context;
  // table filtering/tab intersections belong to the initiating UI's ID order.
  AnnotationResult chooseSelection(FileState&, const std::string& id,
                                   AnnotationSelectionMode,
                                   const std::vector<std::string>& orderedIds,
                                   const std::string& segmentId = {});
  void reconcileSelection(FileState&);
  bool hover(FileState&, std::string id = {}, std::string segmentId = {});
  AnnotationResult bindChannelSource(FileState&, Channel&,
                                     const std::string& annotationId,
                                     const std::string& segmentId) const;
  AnnotationResult bindChannelSource(FileState&, Channel&,
                                     const AnnotationChannelRequest&) const;
  AnnotationResult undo(FileState&);
  AnnotationResult redo(FileState&);
  bool canUndo(const FileState&) const;
  bool canRedo(const FileState&) const;
  std::size_t historySize(const FileState&) const;
  void clear();
  void clear(const std::string& fileId);
  // Reject mark edits while a detection commit still owns the project file.
  void setWriteGuard(std::function<bool(std::string&)> guard) {
    writeGuard_ = std::move(guard);
  }
  void setWroteNotifier(std::function<void()> notifier) {
    wrote_ = std::move(notifier);
  }

 private:
  struct State {
    std::string name, note, activeSegmentId, revisionId, reviewRevisionId;
    ReviewStatus reviewStatus = ReviewStatus::Pending;
    bool deleted = false;
    bool plotHidden = false;
    std::optional<ViewRange> observation;
    bool operator==(const State&) const = default;
  };
  struct Patch {
    std::string annotationId;
    bool created = false;
    std::optional<Annotation> createdAnnotation;
    State before, after;
    std::vector<SegmentChange> segments;
  };
  struct Command {
    std::vector<Patch> patches;
    std::optional<AnnotationSourceSet> createdSourceSet;
    std::size_t bytes = 0;
  };
  struct History {
    std::vector<Command> past, future;
    std::size_t bytes = 0;
  };
  std::unordered_map<std::string, History> histories_;
  std::uint64_t nextIdentifier_ = 1;
  std::function<bool(std::string&)> writeGuard_;
  std::function<void()> wrote_;
  std::optional<AnnotationResult> blockedWrite() const;
  void notifyWrote();

  std::string nextId(const FileState&, const std::string& prefix);
  static State state(const Annotation&);
  static void installState(Annotation&, const State&);
  static Patch difference(const Annotation&, const Annotation&);
  static std::size_t commandBytes(const Command&);
  AnnotationResult commit(FileState&, const Annotation& before,
                          Annotation after, std::string action,
                          std::string actor);
  AnnotationResult applyCommand(FileState&, const Command&, bool forward);
  void record(FileState&, Command);
};

}  // namespace signalstudio
