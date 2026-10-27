#include "application/detection_result_service.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <algorithm>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/detection_projection.h"
#include "infrastructure/project_file.h"
#include "infrastructure/source_loader.h"

namespace signalstudio {
namespace {
FileState* fileFor(Project& project, const std::string& id) {
  for (auto& file : project.files)
    if (file.metadata.id == id) return &file;
  return nullptr;
}
CurrentDetectionSlot* slotFor(FileState& file, std::uint32_t channel) {
  if (file.currentDetectionSlots)
    for (auto& slot : *file.currentDetectionSlots)
      if (slot.sampleChannel == channel) return &slot;
  return nullptr;
}
bool validId(const std::string& id) {
  return !id.empty() && id.size() <= 128 && id.find('\0') == std::string::npos;
}
void addId(std::vector<std::string>& ids, const std::string& id) {
  if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end())
    ids.push_back(id);
}
bool sourceMatches(const FileState& file, const SourceIdentity& source,
                   bool verifyDisk) {
  auto format = file.metadata.sampleFormat;
  format.selectedChannel = source.selectedSampleChannel;
  const auto bounds = annotationBounds(file.metadata);
  if (file.metadata.id != source.fileId || file.metadata.path != source.path ||
      source.selectedSampleChannel >= format.channels ||
      source.sampleFormat != format ||
      source.sampleFormat.selectedChannel != source.selectedSampleChannel ||
      file.metadata.sampleRateHz != source.sampleRateHz ||
      file.metadata.centerFrequencyHz != source.centerFrequencyHz ||
      file.metadata.sampleCount != source.physicalSamples ||
      availableSamples(file.metadata) < source.availableSamples ||
      bounds.effectiveFrequency != source.effectiveFrequency)
    return false;
  return !verifyDisk ||
         (!source.quickFingerprint.empty() &&
          QFileInfo::exists(QString::fromStdString(source.path)) &&
          iqSourceFingerprint(QString::fromStdString(source.path)).toStdString() ==
              source.quickFingerprint);
}
bool completeCoverage(const DetectionRun& run, QString& error) {
  DetectionRun checked;
  if (!decodeDetectionRun(encodeDetectionRun(run), checked, error)) return false;
  if (run.lifecycle != DetectionLifecycle::Completed ||
      run.sourceValidity != DetectionSourceValidity::Current ||
      !run.error.empty() || !run.finishedAtMilliseconds ||
      run.finishedAtMilliseconds < run.startedAtMilliseconds ||
      !run.readFrames || !run.eligibleWindows ||
      run.completedWindows != run.eligibleWindows) {
    error = "正式检测需要 Completed、有效来源和全部窗口完成";
    return false;
  }
  bool read = false, eligible = false, completed = false;
  for (const auto& part : run.coverage) {
    read |= part.kind == DetectionCoverageKind::Read;
    completed |= part.kind == DetectionCoverageKind::Completed;
    if (part.kind != DetectionCoverageKind::Eligible) continue;
    eligible = true;
    std::vector<TimeRange> ranges;
    for (const auto& done : run.coverage)
      if (done.kind == DetectionCoverageKind::Completed &&
          done.range.frequency.lowerHz <= part.range.frequency.lowerHz &&
          done.range.frequency.upperHz >= part.range.frequency.upperHz)
        ranges.push_back(done.range.time);
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) {
      return a.begin < b.begin;
    });
    auto end = part.range.time.begin;
    for (const auto& range : ranges) {
      if (range.begin > end) break;
      end = std::max(end, range.end);
    }
    if (end < part.range.time.end) {
      error = "检测 coverage 不完整：存在尚未完成的 eligible 范围";
      return false;
    }
  }
  if (!read || !eligible || !completed) {
    error = "检测缺少 read / eligible / completed coverage";
    return false;
  }
  return true;
}
bool revisedSource(const Annotation& a) {
  if (!a.originEvidence) return true;
  if (std::any_of(a.revisions.begin(), a.revisions.end(), [](const auto& r) {
        return r.action == "ux-r5-freeze-original";
      })) return false;
  return std::any_of(a.revisions.begin(), a.revisions.end(), [](const auto& r) {
    return r.actor != "source" && r.actor != "model" && !r.actor.empty() &&
           r.action != "source-publication" && r.action != "algorithm-origin";
  });
}
void retireScope(FileState& file, CurrentDetectionSlot& slot,
                 const std::string& keepRun,
                 const std::string& pendingRun = {}) {
  std::unordered_set<std::string> sets, annotations;
  for (const auto& set : file.sourceSets)
    if (set.sourceKind == AnnotationSource::Algorithm &&
        set.sampleChannel == slot.sampleChannel && set.runId != keepRun) {
      sets.insert(set.id);
      if (set.runId != pendingRun) addId(slot.retiredRunIds, set.runId);
    }
  for (const auto& a : file.marks)
    if (a.sourceKind == AnnotationSource::Algorithm && sets.contains(a.sourceSetId))
      annotations.insert(a.id);
  for (auto& channel : file.channels)
    if (channel.sourceSnapshot &&
        channel.sourceSnapshot->sourceKind == AnnotationSource::Algorithm &&
        annotations.contains(channel.sourceSnapshot->annotationId)) {
      addId(slot.retiredAnnotationIds, channel.sourceSnapshot->annotationId);
      channel.sourceRelation = ChannelSourceRelation::Retired;
    }
  for (const auto& run : file.detectionRuns)
    if (run.request.sourceIdentity.selectedSampleChannel == slot.sampleChannel &&
        run.request.runId != keepRun && run.request.runId != pendingRun)
      addId(slot.retiredRunIds, run.request.runId);
  std::erase_if(file.marks, [&](const auto& a) { return annotations.contains(a.id); });
  std::erase_if(file.sourceSets, [&](const auto& s) { return sets.contains(s.id); });
  std::erase_if(file.detectionRuns, [&](const auto& run) {
    return run.request.sourceIdentity.selectedSampleChannel == slot.sampleChannel &&
           run.request.runId != keepRun && run.request.runId != pendingRun;
  });
  // Selection/draft IDs are session-only projections; retained Manual IDs stay.
  std::erase_if(file.selectedMarkIds, [&](const auto& id) {
    return annotations.contains(id);
  });
  if (annotations.contains(file.activeMarkId)) {
    file.activeMarkId = file.selectedMarkIds.empty() ? std::string{}
                                                    : file.selectedMarkIds.front();
    const auto* active = findAnnotation(file, file.activeMarkId);
    file.activeSegmentId = active ? active->activeSegmentId : std::string{};
    if (!active) file.display.psdFromSelection = false;
  }
  if (annotations.contains(file.selectionAnchorId)) file.selectionAnchorId.clear();
  if (annotations.contains(file.hoveredMarkId)) {
    file.hoveredMarkId.clear();
    file.hoveredSegmentId.clear();
  }
  if (annotations.contains(file.revisionEditingAnnotationId)) {
    file.revisionEditingAnnotationId.clear();
    file.revisionEditingBaseRevisionId.clear();
  }
  if (std::find(slot.retiredRunIds.begin(), slot.retiredRunIds.end(),
                file.annotationFilter.runId) != slot.retiredRunIds.end())
    file.annotationFilter.runId.clear();
}
bool ticketMatches(const Project& project, const FileState& file,
                   const DetectionResultTicket& ticket,
                   std::uint64_t generation, const DetectionRun& run) {
  const auto* slot = currentDetectionSlot(file, ticket.sampleChannel);
  return !project.projectId.empty() && ticket.projectGeneration == generation &&
         slot && slot->generation == ticket.slotGeneration &&
         slot->pendingRunId == ticket.runId &&
         ticket.fileId == file.metadata.id && ticket.runId == run.request.runId &&
         ticket.sourceIdentity == run.request.sourceIdentity &&
         ticket.sampleChannel == run.request.sourceIdentity.selectedSampleChannel &&
         sourceMatches(file, run.request.sourceIdentity, true);
}
bool inject(const DetectionTransactionOptions& options,
            DetectionTransactionPoint point) {
  return options.failAt && options.failAt(point);
}
}  // namespace

bool detectionTicketMatches(const Project& project, const FileState& file,
                            const DetectionResultTicket& ticket,
                            std::uint64_t generation, const DetectionRun& run) {
  return ticketMatches(project, file, ticket, generation, run);
}

const CurrentDetectionSlot* currentDetectionSlot(const FileState& file,
                                                 std::uint32_t channel) {
  if (file.currentDetectionSlots)
    for (const auto& slot : *file.currentDetectionSlots)
      if (slot.sampleChannel == channel) return &slot;
  return nullptr;
}

AnnotationValidation validateCurrentDetectionSlots(const FileState& file) {
  if (!file.currentDetectionSlots) return {};
  std::unordered_set<std::uint32_t> channels;
  std::unordered_set<std::string> retired, committed, pending, retiredAnnotations;
  for (const auto& slot : *file.currentDetectionSlots) {
    if (slot.sampleChannel >= file.metadata.sampleFormat.channels ||
        !channels.insert(slot.sampleChannel).second ||
        (!slot.committedRunId.empty() && !validId(slot.committedRunId)) ||
        (!slot.pendingRunId.empty() && !validId(slot.pendingRunId)) ||
        (!slot.pendingRunId.empty() && !slot.stale) ||
        ((!slot.pendingRunId.empty() || !slot.committedRunId.empty()) &&
         !slot.generation))
      return {false, "Invalid current detection slot identity/generation"};
    if (!slot.committedRunId.empty() && !committed.insert(slot.committedRunId).second)
      return {false, "A detection run is committed to multiple slots"};
    if (!slot.pendingRunId.empty() && !pending.insert(slot.pendingRunId).second)
      return {false, "A detection run is pending in multiple slots"};
    for (const auto& id : slot.retiredRunIds)
      if (!validId(id) || !retired.insert(id).second || retired.size() > 100000)
        return {false, "Invalid or duplicate retired detection run ID"};
    for (const auto& id : slot.retiredAnnotationIds)
      if (!validId(id) || !retiredAnnotations.insert(id).second ||
          retiredAnnotations.size() > annotationSegmentBudget)
        return {false, "Invalid or duplicate retired channel annotation ID"};
  }
  for (const auto& id : committed)
    if (retired.contains(id) || pending.contains(id))
      return {false, "Current detection run is also retired/pending"};
  for (const auto& id : pending)
    if (retired.contains(id)) return {false, "Pending detection run is retired"};
  std::unordered_map<std::string, const DetectionRun*> runs;
  for (const auto& run : file.detectionRuns) {
    const auto& id = run.request.runId;
    if (!runs.emplace(id, &run).second || retired.contains(id))
      return {false, "Duplicate or retired detection run remains indexed"};
    if (run.request.sourceIdentity.fileId != file.metadata.id)
      return {false, "Detection run belongs to a different file"};
    const auto* slot = currentDetectionSlot(
        file, run.request.sourceIdentity.selectedSampleChannel);
    if (!slot || (run.lifecycle == DetectionLifecycle::Completed &&
                  slot->committedRunId != id && slot->pendingRunId != id))
      return {false, "Completed detection run is outside the authority slot"};
  }
  std::unordered_map<std::string, std::size_t> sets;
  for (const auto& set : file.sourceSets) {
    if (set.sourceKind != AnnotationSource::Algorithm) continue;
    const auto* slot = currentDetectionSlot(file, set.sampleChannel);
    if (!slot || set.deleted || slot->committedRunId.empty() ||
        set.runId != slot->committedRunId || !runs.contains(set.runId) ||
        ++sets[set.runId] != 1 ||
        set.sourceIdentity != runs.at(set.runId)->request.sourceIdentity)
      return {false, "Algorithm source set is not the unique committed result"};
  }
  for (const auto& slot : *file.currentDetectionSlots) {
    if (!slot.pendingRunId.empty()) {
      const auto found = runs.find(slot.pendingRunId);
      if (found == runs.end() || found->second->request.sourceIdentity.selectedSampleChannel !=
                                     slot.sampleChannel)
        return {false, "Pending detection slot has no matching durable run"};
    }
    if (slot.committedRunId.empty()) continue;
    const auto found = runs.find(slot.committedRunId);
    if (found == runs.end() || sets[slot.committedRunId] != 1 ||
        found->second->lifecycle != DetectionLifecycle::Completed ||
        found->second->request.sourceIdentity.selectedSampleChannel != slot.sampleChannel)
      return {false, "Committed detection slot has no complete run/source set"};
  }
  for (const auto& a : file.marks) {
    if (retiredAnnotations.contains(a.id))
      return {false, "Retired channel annotation remains in formal results"};
    if (a.sourceKind != AnnotationSource::Algorithm) continue;
    const auto* set = findSourceSet(file, a.sourceSetId);
    if (!set || !sets.contains(set->runId) || !a.originEvidence ||
        a.originEvidence->runId != set->runId)
      return {false, "Algorithm annotation has no committed immutable source"};
  }
  return {};
}

AnnotationValidation migrateLegacyDetectionSlots(
    FileState& file, const std::vector<std::string>& liveRunIds) {
  if (file.currentDetectionSlots) return validateCurrentDetectionSlots(file);
  FileState candidate = file;
  // The caller must preserve historical user edits before retiring any source.
  for (const auto& a : candidate.marks)
    if (a.sourceKind == AnnotationSource::Algorithm && revisedSource(a) &&
        std::none_of(candidate.marks.begin(), candidate.marks.end(), [&](const auto& copy) {
          return copy.sourceKind == AnnotationSource::Manual && copy.conversionSnapshot &&
              copy.conversionSnapshot->sourceAnnotationId == a.id;
        }))
      return {false, "Legacy Algorithm user revisions require Manual preservation before slot migration"};
  std::unordered_map<std::string, const DetectionRun*> runs;
  for (const auto& run : candidate.detectionRuns)
    if (!runs.emplace(run.request.runId, &run).second)
      return {false, "Legacy detection run identity is ambiguous"};
  std::unordered_set<std::uint32_t> scopes;
  for (const auto& run : candidate.detectionRuns)
    scopes.insert(run.request.sourceIdentity.selectedSampleChannel);
  for (const auto& set : candidate.sourceSets)
    if (set.sourceKind == AnnotationSource::Algorithm) {
      if (set.runId.empty() || !runs.contains(set.runId) ||
          runs.at(set.runId)->request.sourceIdentity.selectedSampleChannel != set.sampleChannel)
        return {false, "Legacy Algorithm source has no unambiguous durable run"};
      scopes.insert(set.sampleChannel);
    }
  // Nothing to migrate: keep v6 documents without detection data canonical.
  // Starting a new detection explicitly creates its first authoritative slot.
  if (scopes.empty()) return {};
  candidate.currentDetectionSlots.emplace();
  for (const auto scope : scopes) {
    const DetectionRun* winner = nullptr;
    bool tied = false, hasAlgorithm = false;
    for (const auto& set : candidate.sourceSets)
      hasAlgorithm |= set.sourceKind == AnnotationSource::Algorithm && set.sampleChannel == scope;
    for (const auto& run : candidate.detectionRuns) {
      if (run.request.sourceIdentity.selectedSampleChannel != scope ||
          run.lifecycle != DetectionLifecycle::Completed ||
          run.sourceValidity != DetectionSourceValidity::Current ||
          !sourceMatches(candidate, run.request.sourceIdentity, false)) continue;
      const auto count = std::count_if(candidate.sourceSets.begin(), candidate.sourceSets.end(),
          [&](const auto& set) { return !set.deleted && set.sourceKind == AnnotationSource::Algorithm &&
              set.sampleChannel == scope && set.runId == run.request.runId &&
              set.sourceIdentity == run.request.sourceIdentity; });
      // A run still being saved/recovered has no formal source yet.
      if (!count) continue;
      if (count != 1) return {false, "Legacy completed run has ambiguous source sets"};
      QString why;
      if (!completeCoverage(run, why))
        return {false, "Legacy completed run cannot be safely adjudicated: " + why.toStdString()};
      if (!winner || run.finishedAtMilliseconds > winner->finishedAtMilliseconds) {
        winner = &run;
        tied = false;
      } else if (run.finishedAtMilliseconds == winner->finishedAtMilliseconds) tied = true;
    }
    if (tied)
      return {false, "Legacy detection results are ambiguous; original data was not modified"};
    CurrentDetectionSlot slot;
    slot.sampleChannel = scope;
    slot.generation = 1;
    slot.committedRunId = winner ? winner->request.runId : std::string{};
    // Container live anchors are validated before this call. They remain
    // authoritative if the worker has since completed/cancelled. For a bare
    // v6 file, only an actually interrupted/live manifest supplies that hint.
    for (const auto& run : candidate.detectionRuns) {
      if (run.request.sourceIdentity.selectedSampleChannel != scope ||
          run.request.runId == slot.committedRunId) continue;
      const bool live = std::find(liveRunIds.begin(), liveRunIds.end(),
                                  run.request.runId) != liveRunIds.end() ||
                        run.recoverable || !detectionIsTerminal(run.lifecycle);
      if (!live) continue;
      if (!slot.pendingRunId.empty())
        return {false, "Multiple anchored/live legacy tasks need an explicit recovery decision"};
      slot.pendingRunId = run.request.runId;
    }
    if (!winner && !hasAlgorithm && slot.pendingRunId.empty()) {
      // Preserve a single unpublished terminal task as evidence, not a formal
      // result. Multiple terminal histories still need explicit adjudication.
      for (const auto& run : candidate.detectionRuns)
        if (run.request.sourceIdentity.selectedSampleChannel == scope) {
          if (!slot.pendingRunId.empty())
            return {false, "Multiple unpublished legacy tasks need an explicit recovery decision"};
          slot.pendingRunId = run.request.runId;
        }
    }
    if (!winner && hasAlgorithm && slot.pendingRunId.empty())
      return {false, "Legacy detection results are ambiguous; original data was not modified"};
    slot.stale = !winner || !slot.pendingRunId.empty();
    retireScope(candidate, slot, slot.committedRunId, slot.pendingRunId);
    candidate.currentDetectionSlots->push_back(std::move(slot));
  }
  std::sort(candidate.currentDetectionSlots->begin(), candidate.currentDetectionSlots->end(),
            [](const auto& a, const auto& b) { return a.sampleChannel < b.sampleChannel; });
  if (const auto valid = validateCurrentDetectionSlots(candidate); !valid) return valid;
  file = std::move(candidate);
  return {};
}

bool DetectionResultService::beginDetection(Project& project,
    const DetectionRequest& request, std::uint64_t generation,
    DetectionResultTicket& ticket, QString& error) {
  auto* file = fileFor(project, request.sourceIdentity.fileId);
  if (!file || request.sourceIdentity.projectGeneration != generation ||
      !sourceMatches(*file, request.sourceIdentity, true)) {
    error = "检测来源或工程 generation 已改变";
    return false;
  }
  const auto validRequest = validateDetectionRequest(request);
  if (!validRequest) { error = QString::fromStdString(validRequest.error); return false; }
  if (const auto valid = migrateLegacyDetectionSlots(*file); !valid) {
    error = QString::fromStdString(valid.error); return false;
  }
  if (!file->currentDetectionSlots) file->currentDetectionSlots.emplace();
  const auto channel = request.sourceIdentity.selectedSampleChannel;
  auto* slot = slotFor(*file, channel);
  if (!slot) {
    file->currentDetectionSlots->push_back({});
    slot = &file->currentDetectionSlots->back();
    slot->sampleChannel = channel;
  }
  for (const auto& existing : file->detectionRuns)
    if (existing.request.runId == request.runId) {
      error = "检测 run ID 已使用"; return false;
    }
  for (const auto& s : *file->currentDetectionSlots)
    if (std::find(s.retiredRunIds.begin(), s.retiredRunIds.end(), request.runId) != s.retiredRunIds.end()) {
      error = "退役检测 run ID 不可复用"; return false;
    }
  if (slot->generation == std::numeric_limits<std::uint64_t>::max()) {
    error = "检测 slot generation 已耗尽"; return false;
  }
  if (!slot->pendingRunId.empty()) {
    const auto superseded = slot->pendingRunId;
    addId(slot->retiredRunIds, superseded);
    std::erase_if(file->detectionRuns, [&](const auto& old) { return old.request.runId == superseded; });
  }
  slot->stale = true;
  slot->pendingRunId = request.runId;
  ++slot->generation;
  ticket = {file->metadata.id, request.runId, channel, generation,
            slot->generation, request.sourceIdentity};
  error.clear();
  return true;
}

bool DetectionResultService::prepareRecovery(Project& project, const DetectionRun& run,
    std::uint64_t generation, DetectionResultTicket& ticket, QString& error) {
  const auto* file = fileFor(project, run.request.sourceIdentity.fileId);
  const auto* slot = file ? currentDetectionSlot(*file, run.request.sourceIdentity.selectedSampleChannel) : nullptr;
  if (!file || !slot || slot->pendingRunId != run.request.runId ||
      !sourceMatches(*file, run.request.sourceIdentity, true) || !completeCoverage(run, error)) {
    if (error.isEmpty()) error = "恢复运行不属于权威 pending slot，或来源已改变";
    return false;
  }
  ticket = {file->metadata.id, run.request.runId, slot->sampleChannel, generation,
            slot->generation, run.request.sourceIdentity};
  error.clear();
  return true;
}
bool DetectionResultService::clearCommittedDetection(
    Project& project, const std::string& fileId, std::uint32_t channel,
    bool& clearedPrevious, QString& error) {
  clearedPrevious = false;
  auto* file = fileFor(project, fileId);
  if (!file) {
    error = "检测文件不存在";
    return false;
  }
  auto* slot = slotFor(*file, channel);
  if (!slot || slot->committedRunId.empty()) {
    error.clear();
    return true;
  }
  retireScope(*file, *slot, {}, slot->pendingRunId);
  slot->committedRunId.clear();
  slot->stale = true;
  if (const auto valid = validateCurrentDetectionSlots(*file); !valid) {
    error = QString::fromStdString(valid.error);
    return false;
  }
  clearedPrevious = true;
  error.clear();
  return true;
}
void DetectionResultService::abandonDetection(Project& project, const DetectionResultTicket& ticket) {
  auto* file = fileFor(project, ticket.fileId);
  auto* slot = file ? slotFor(*file, ticket.sampleChannel) : nullptr;
  if (slot && slot->generation == ticket.slotGeneration && slot->pendingRunId == ticket.runId) {
    addId(slot->retiredRunIds, ticket.runId);
    std::erase_if(file->detectionRuns, [&](const auto& old) { return old.request.runId == ticket.runId; });
    slot->pendingRunId.clear();
    slot->stale = true;
  }
}
bool DetectionResultService::markStale(Project& project, const std::string& fileId,
                                      std::uint32_t channel) {
  auto* file = fileFor(project, fileId);
  auto* slot = file ? slotFor(*file, channel) : nullptr;
  if (!slot) return false;
  const bool changed = !slot->stale || !slot->pendingRunId.empty();
  if (!slot->pendingRunId.empty()) {
    const auto pending = slot->pendingRunId;
    addId(slot->retiredRunIds, pending);
    std::erase_if(file->detectionRuns, [&](const auto& old) { return old.request.runId == pending; });
    slot->pendingRunId.clear();
    if (slot->generation != std::numeric_limits<std::uint64_t>::max()) ++slot->generation;
  }
  slot->stale = true;
  return changed;
}

DetectionCommitResult DetectionResultService::replaceCurrentDetection(
    Project& project, AnnotationService& annotations, const DetectionResultTicket& ticket,
    std::uint64_t generation, const DetectionRun& run, DetectionPublication publication,
    const QString& path, const DetectionTransactionOptions& options) {
  try {
    const auto reject = [](const QString& why) { return DetectionCommitResult{false, false, why}; };
    const auto* file = fileFor(project, ticket.fileId);
    QString error;
    if (path.isEmpty() || !file || !ticketMatches(project, *file, ticket, generation, run))
      return reject("拒绝迟到、已取消或不同来源/slot generation 的检测发布");
    if (!completeCoverage(run, error)) return reject(error);
    if (publication.sourceSet.sourceKind != AnnotationSource::Algorithm ||
        publication.sourceSet.runId != run.request.runId ||
        publication.sourceSet.sourceIdentity != run.request.sourceIdentity)
      return reject("检测 publication 与不可变请求不一致");
    std::size_t publicationSegments = 0;
    if (publication.annotations.size() > annotationSegmentBudget)
      return reject("Detection publication object budget exceeded");
    for (const auto& annotation : publication.annotations) {
      if (annotation.segments.size() > annotationSegmentBudget - publicationSegments)
        return reject("Detection publication segment budget exceeded (100000)");
      publicationSegments += annotation.segments.size();
    }
    if (!validateDetectionPublicationBudget(publication,
            run.request.workingSetBudgetBytes / 2, error))
      return reject(error);
    Project candidate = project;
    auto* next = fileFor(candidate, ticket.fileId);
    auto* slot = slotFor(*next, ticket.sampleChannel);
    retireScope(*next, *slot, run.request.runId);
    std::erase_if(next->detectionRuns, [&](const auto& old) { return old.request.runId == run.request.runId; });
    next->detectionRuns.push_back(run);
    // All source construction stays behind the existing batch validator.
    AnnotationService staging;
    const auto selectedChannel = next->metadata.sampleFormat.selectedChannel;
    next->metadata.sampleFormat.selectedChannel = ticket.sampleChannel;
    const auto published = staging.publishSourceSet(*next, publication.sourceSet, publication.annotations);
    next->metadata.sampleFormat.selectedChannel = selectedChannel;
    refreshChannelSourceRelations(*next);
    if (!published) return reject(QString::fromStdString(published.error));
    slot = slotFor(*next, ticket.sampleChannel);
    slot->committedRunId = run.request.runId;
    slot->pendingRunId.clear();
    slot->stale = false;
    if (const auto valid = validateCurrentDetectionSlots(*next); !valid)
      return reject(QString::fromStdString(valid.error));
    if (inject(options, DetectionTransactionPoint::Stage)) return reject("Injected detection stage failure");
    const auto directory = ProjectFileCodec::assetDirectory(path, candidate);
    DetectionRun durable;
    if (!loadDetectionRun(directory, QString::fromStdString(run.request.runId), durable, error) ||
        encodeDetectionRun(durable) != encodeDetectionRun(run))
      return reject(error.isEmpty() ? "持久检测 manifest 与完成快照不同" : error);
    DetectionPublication recovered;
    if (!recoverDetectionPublication(directory, durable, recovered, error)) return reject(error);
    if (encodeSourceSet(recovered.sourceSet) != encodeSourceSet(publication.sourceSet) ||
        recovered.annotations.size() != publication.annotations.size())
      return reject("持久检测来源/对象数与 publication 不同");
    for (std::size_t i = 0; i < recovered.annotations.size(); ++i)
      if (encodeAnnotation(recovered.annotations[i]) != encodeAnnotation(publication.annotations[i]))
        return reject("持久检测对象与 publication 不同");
    if (inject(options, DetectionTransactionPoint::Persist)) return reject("Injected detection persist failure");
    if (!ProjectFileCodec::save(path, candidate, error, AssetSaveMode::Preserve, {},
         [&] { return !inject(options, DetectionTransactionPoint::Commit); }))
      return reject(error);
    // No allocation/IO can fail between the durable commit and publication.
    static_assert(std::is_nothrow_move_assignable_v<Project>);
    project = std::move(candidate);
    // The system operation did not create an undo command. Existing Manual
    // conversion commands hold independent snapshots and remain usable.
    (void)annotations;
    // GC is explicitly deferred and never changes the transaction outcome.
    try {
      const bool interruptedGc = inject(options, DetectionTransactionPoint::Gc);
      return {true, true, interruptedGc ? "Injected GC interruption; committed result is durable" : QString{}};
    } catch (...) {
      return {true, true, "GC hook failed after commit; committed result is durable"};
    }
  } catch (const std::exception& e) {
    return {false, false, QString::fromUtf8(e.what())};
  }
}
}  // namespace signalstudio
