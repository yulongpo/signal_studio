#include "application/detection_result_service.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <algorithm>
#include <limits>
#include <optional>
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
// Drops only this channel's previous formal algorithm result. Other indexed
// tasks, files, IQ channels, manual marks, and imported labels stay.
void discardCommittedFormalResult(FileState& file, CurrentDetectionSlot& slot) {
  const auto previous = slot.committedRunId;
  slot.committedRunId.clear();
  slot.suppressFormalResults = true;
  slot.unpublishedPreview = false;
  slot.stale = true;
  if (previous.empty()) return;
  std::unordered_set<std::string> sets, annotations;
  for (const auto& set : file.sourceSets)
    if (set.sourceKind == AnnotationSource::Algorithm &&
        set.sampleChannel == slot.sampleChannel && set.runId == previous)
      sets.insert(set.id);
  for (const auto& annotation : file.marks)
    if (annotation.sourceKind == AnnotationSource::Algorithm &&
        sets.contains(annotation.sourceSetId))
      annotations.insert(annotation.id);
  for (auto& channel : file.channels)
    if (channel.sourceSnapshot &&
        channel.sourceSnapshot->sourceKind == AnnotationSource::Algorithm &&
        annotations.contains(channel.sourceSnapshot->annotationId)) {
      addId(slot.retiredAnnotationIds, channel.sourceSnapshot->annotationId);
      channel.sourceRelation = ChannelSourceRelation::Retired;
    }
  addId(slot.retiredRunIds, previous);
  std::erase_if(file.marks, [&](const auto& annotation) {
    return annotations.contains(annotation.id);
  });
  std::erase_if(file.sourceSets, [&](const auto& set) { return sets.contains(set.id); });
  std::erase_if(file.detectionRuns, [&](const auto& run) {
    return run.request.runId == previous;
  });
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
  if (file.annotationFilter.runId == previous) file.annotationFilter.runId.clear();
}
QJsonArray algorithmMarksFor(const FileState& file, std::optional<std::uint32_t> channel) {
  QJsonArray data;
  for (const auto& mark : file.marks) {
    if (mark.sourceKind != AnnotationSource::Algorithm) continue;
    const auto* set = findSourceSet(file, mark.sourceSetId);
    const auto markChannel = set ? std::optional<std::uint32_t>{set->sampleChannel}
                                 : std::nullopt;
    if (channel) {
      if (markChannel && *markChannel == *channel) data.push_back(encodeAnnotation(mark));
    } else if (!markChannel) {
      data.push_back(encodeAnnotation(mark));
    }
  }
  return data;
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
QJsonArray marksOf(const FileState& file, bool algorithm) {
  QJsonArray data;
  for (const auto& mark : file.marks)
    if ((mark.sourceKind == AnnotationSource::Algorithm) == algorithm)
      data.push_back(encodeAnnotation(mark));
  return data;
}
bool sameFormalResult(const Project& live, const Project& candidate, QString& error) {
  if (live.files.size() != candidate.files.size()) {
    error = "检测启动改变了工程文件集合，上次正式结果仍在";
    return false;
  }
  for (const auto& before : live.files) {
    const FileState* after = nullptr;
    for (const auto& file : candidate.files)
      if (file.metadata.id == before.metadata.id) after = &file;
    if (!after) {
      error = "检测启动改变了文件身份，上次正式结果仍在";
      return false;
    }
    if (marksOf(before, false) != marksOf(*after, false)) {
      error = "检测启动改变了人工标记或原始标签，上次正式结果仍在";
      return false;
    }
    if (!before.currentDetectionSlots) continue;
    if (algorithmMarksFor(before, std::nullopt) != algorithmMarksFor(*after, std::nullopt)) {
      error = "检测启动改变了已有正式结果，上次正式结果仍在";
      return false;
    }
    std::unordered_set<std::uint32_t> channels;
    for (const auto& slot : *before.currentDetectionSlots)
      channels.insert(slot.sampleChannel);
    if (after->currentDetectionSlots)
      for (const auto& slot : *after->currentDetectionSlots)
        channels.insert(slot.sampleChannel);
    for (const auto channel : channels) {
      const auto* oldSlot = currentDetectionSlot(before, channel);
      const auto* newSlot = currentDetectionSlot(*after, channel);
      const auto oldId = oldSlot ? oldSlot->committedRunId : std::string{};
      const auto newId = newSlot ? newSlot->committedRunId : std::string{};
      const auto oldMarks = algorithmMarksFor(before, channel);
      const auto newMarks = algorithmMarksFor(*after, channel);
      if (oldId == newId && oldMarks == newMarks) continue;
      // The only legal formal change at start is discarding this channel.
      const bool acceptedDiscard =
          newSlot && !newSlot->pendingRunId.empty() && newSlot->committedRunId.empty() &&
          newSlot->suppressFormalResults && newMarks.isEmpty();
      if (!acceptedDiscard) {
        error = "检测启动改变了已有正式结果，上次正式结果仍在";
        return false;
      }
    }
  }
  error.clear();
  return true;
}
bool restoreProjectBytes(const QString& path, const QByteArray& bytes, QString& error) {
  QSaveFile output(path);
  if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() ||
      !output.commit()) {
    error = output.errorString().isEmpty() ? QString("工程文件回滚失败")
                                            : output.errorString();
    return false;
  }
  error.clear();
  return true;
}
QString logToken(const std::string& value) {
  auto text = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
  for (int index = 0; index < text.size(); ++index)
    if (text.at(index).isSpace()) text[index] = QLatin1Char('_');
  return text;
}
QString tokenFragment(const QString& text) {
  QString out;
  out.reserve(text.size());
  for (const auto ch : text) {
    if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('[') ||
        ch == QLatin1Char(']'))
      out += ch;
    else
      out += QLatin1Char('_');
  }
  return out;
}
std::optional<QString> shortJsonScalar(const QJsonValue& value) {
  if (value.isObject() || value.isArray()) return std::nullopt;
  QString text;
  if (value.isNull())
    text = QStringLiteral("null");
  else if (value.isBool())
    text = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  else if (value.isDouble())
    text = QString::number(value.toDouble(), 'g', 12);
  else if (value.isString())
    text = value.toString();
  else
    return std::nullopt;
  if (text.size() > 64) return std::nullopt;
  for (const auto ch : text)
    if (ch.isSpace() || ch == QLatin1Char('=') || ch == QLatin1Char('"') ||
        ch == QLatin1Char('%'))
      return std::nullopt;
  return text;
}
QString jsonLeaf(const QString& path, const QJsonValue& recovered,
                 const QJsonValue& publication) {
  const auto shown = path.isEmpty() ? QStringLiteral("<root>") : path;
  const auto recoveredScalar = shortJsonScalar(recovered);
  const auto publicationScalar = shortJsonScalar(publication);
  if (recoveredScalar && publicationScalar)
    return shown + QStringLiteral(" recoveredValue=") + *recoveredScalar +
           QStringLiteral(" publicationValue=") + *publicationScalar;
  if (recovered.isArray() || publication.isArray())
    return shown + QStringLiteral(" recoveredArrayCount=") +
           QString::number(recovered.isArray() ? recovered.toArray().size() : 0) +
           QStringLiteral(" publicationArrayCount=") +
           QString::number(publication.isArray() ? publication.toArray().size() : 0) +
           QStringLiteral(" contentDiffers=1");
  if (recovered.isObject() || publication.isObject())
    return shown + QStringLiteral(" recoveredObjectKeys=") +
           QString::number(recovered.isObject() ? recovered.toObject().size() : 0) +
           QStringLiteral(" publicationObjectKeys=") +
           QString::number(publication.isObject() ? publication.toObject().size() : 0) +
           QStringLiteral(" contentDiffers=1");
  const auto bytes = [](const QJsonValue& value) {
    if (!value.isString()) return QStringLiteral("0");
    const auto text = value.toString();
    if (text.size() > 1000000)
      return QString::number(text.size()) + QStringLiteral("utf16");
    return QString::number(
        static_cast<qlonglong>(text.toUtf8().size()));
  };
  return shown + QStringLiteral(" recoveredFieldBytes=") + bytes(recovered) +
         QStringLiteral(" publicationFieldBytes=") + bytes(publication) +
         QStringLiteral(" contentDiffers=1");
}
QString jsonDifference(const QJsonValue& recovered, const QJsonValue& publication,
                       const QString& path, int depth) {
  if (recovered == publication) return {};
  if (path.size() > 180 || depth >= 12)
    return (path.isEmpty() ? QStringLiteral("<root>") : path) +
           QStringLiteral(" depthLimit=1");
  const bool sameKind = recovered.isObject() == publication.isObject() &&
                        recovered.isArray() == publication.isArray() &&
                        recovered.isString() == publication.isString() &&
                        recovered.isDouble() == publication.isDouble() &&
                        recovered.isBool() == publication.isBool() &&
                        recovered.isNull() == publication.isNull();
  if (!sameKind || (!recovered.isObject() && !recovered.isArray()))
    return jsonLeaf(path, recovered, publication);
  if (recovered.isArray()) {
    const auto left = recovered.toArray();
    const auto right = publication.toArray();
    const auto count = std::min(left.size(), right.size());
    for (qsizetype index = 0; index < count; ++index) {
      if (left.at(index) == right.at(index)) continue;
      const auto child = path + QLatin1Char('[') + QString::number(index) + QLatin1Char(']');
      return jsonDifference(left.at(index), right.at(index), child, depth + 1);
    }
    return jsonLeaf(path, recovered, publication);
  }
  const auto left = recovered.toObject();
  const auto right = publication.toObject();
  std::vector<QString> keys;
  keys.reserve(static_cast<std::size_t>(left.size() + right.size()));
  for (auto it = left.constBegin(); it != left.constEnd(); ++it) keys.push_back(it.key());
  for (auto it = right.constBegin(); it != right.constEnd(); ++it)
    if (!left.contains(it.key())) keys.push_back(it.key());
  std::sort(keys.begin(), keys.end());
  for (const auto& key : keys) {
    const bool inLeft = left.contains(key);
    const bool inRight = right.contains(key);
    if (inLeft && inRight && left.value(key) == right.value(key)) continue;
    const auto child = path.isEmpty() ? tokenFragment(key)
                                      : path + QLatin1Char('.') + tokenFragment(key);
    if (!inLeft || !inRight)
      return child + QStringLiteral(" recoveredPresent=") + (inLeft ? QStringLiteral("1") : QStringLiteral("0")) +
             QStringLiteral(" publicationPresent=") + (inRight ? QStringLiteral("1") : QStringLiteral("0"));
    return jsonDifference(left.value(key), right.value(key), child, depth + 1);
  }
  return jsonLeaf(path.isEmpty() ? QStringLiteral("<root>") : path, recovered, publication);
}
QString firstJsonDifference(const QJsonObject& recovered, const QJsonObject& publication) {
  if (recovered == publication) return {};
  const auto found = jsonDifference(QJsonValue(recovered), QJsonValue(publication), {}, 0);
  return found.isEmpty() ? QStringLiteral("<unlocated>") : found;
}
struct PublicationAnnotationMismatch {
  std::size_t index = 0;
  std::string publicationId;
  std::string recoveredId;
  QString field;
};
QString publicationMismatchSuffix(const DetectionPublication& recovered,
                                  const DetectionPublication& publication,
                                  bool sourceEncodingDiffers, const QString& sourceField,
                                  const PublicationAnnotationMismatch* annotation) {
  // recovered* is recoverDetectionPublication. publication* is the in-memory
  // publication passed to replaceCurrentDetection. Long text stays out of the
  // log; the field path is the locator.
  QString suffix = QStringLiteral(" recoveredAnnotations=") +
                   QString::number(recovered.annotations.size()) +
                   QStringLiteral(" publicationAnnotations=") +
                   QString::number(publication.annotations.size()) +
                   QStringLiteral(" recoveredSourceSetId=") +
                   logToken(recovered.sourceSet.id) +
                   QStringLiteral(" publicationSourceSetId=") +
                   logToken(publication.sourceSet.id);
  if (sourceEncodingDiffers) {
    suffix += QStringLiteral(" sourceSetEncodingDiffers=1");
    if (!sourceField.isEmpty())
      suffix += QStringLiteral(" firstDifferingSourceSetField=") + sourceField;
  }
  if (!annotation) return suffix;
  suffix += QStringLiteral(" firstDifferingAnnotationIndex=") +
            QString::number(annotation->index) + QStringLiteral(" annotationId=") +
            logToken(annotation->publicationId);
  if (annotation->recoveredId != annotation->publicationId)
    suffix += QStringLiteral(" recoveredAnnotationId=") + logToken(annotation->recoveredId);
  if (!annotation->field.isEmpty())
    suffix += QStringLiteral(" firstDifferingField=") + annotation->field;
  return suffix;
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
  discardCommittedFormalResult(*file, *slot);
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
bool DetectionResultService::publishPendingStart(
    Project& live, const Project& candidate,
    const std::function<bool()>& stillCurrent,
    const std::function<bool(QString&)>& commitQueued, const QString& path,
    QString& error) {
  if (!stillCurrent || !commitQueued || !stillCurrent()) {
    error = "检测事务已丢弃，提交期间工程已变化。上次正式结果仍在";
    return false;
  }
  if (!sameFormalResult(live, candidate, error)) return false;
  QFile priorFile(path);
  if (!priorFile.open(QIODevice::ReadOnly)) {
    error = "检测事务保存失败，上次正式结果仍在：" + priorFile.errorString();
    return false;
  }
  const auto prior = priorFile.readAll();
  if (priorFile.error() != QFileDevice::NoError) {
    error = "检测事务保存失败，上次正式结果仍在：" + priorFile.errorString();
    return false;
  }
  priorFile.close();
  Project staged = candidate;
  QString saveError;
  if (!ProjectFileCodec::save(path, staged, saveError, AssetSaveMode::Preserve, {},
                              stillCurrent)) {
    error = saveError.contains("atomic commit")
                ? QString("检测事务已丢弃，提交期间工程已变化。上次正式结果仍在")
                : "检测事务保存失败，上次正式结果仍在：" + saveError;
    return false;
  }
  QString queueError;
  if (!commitQueued(queueError)) {
    QString restoreError;
    if (!restoreProjectBytes(path, prior, restoreError)) {
      error = "检测任务未能启动：" + queueError + "。回滚工程文件失败：" + restoreError;
      return false;
    }
    error = "检测任务未能启动：" + queueError + "。上次正式结果仍在";
    return false;
  }
  live = std::move(staged);
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
void DetectionResultService::retainUnpublishedDetection(
    Project& project, const DetectionResultTicket& ticket, const DetectionRun& run) {
  auto* file = fileFor(project, ticket.fileId);
  auto* slot = file ? slotFor(*file, ticket.sampleChannel) : nullptr;
  // A non-completed record may stay indexed beside a later pending run.
  // Retiring it as well would make the next project save reject the index.
  if (!file || !slot || slot->generation != ticket.slotGeneration ||
      slot->pendingRunId != ticket.runId || run.request.runId != ticket.runId ||
      !detectionIsTerminal(run.lifecycle) ||
      run.lifecycle == DetectionLifecycle::Completed)
    return;
  auto current = std::find_if(
      file->detectionRuns.begin(), file->detectionRuns.end(),
      [&](const auto& old) { return old.request.runId == ticket.runId; });
  if (current == file->detectionRuns.end()) file->detectionRuns.push_back(run);
  else *current = run;
  slot->pendingRunId.clear();
  slot->stale = true;
  slot->suppressFormalResults = true;
  slot->unpublishedPreview = true;
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
    slot->suppressFormalResults = false;
    slot->unpublishedPreview = false;
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
    // Keep these exact prefixes. The suffix names recovered/publication
    // annotation counts, both source-set ids, and, when the counts match, the
    // first encodeAnnotation mismatch (index, publication annotation id, JSON field).
    const auto recoveredSource = encodeSourceSet(recovered.sourceSet);
    const auto publicationSource = encodeSourceSet(publication.sourceSet);
    const bool sourceDiffers = recoveredSource != publicationSource;
    const bool countDiffers = recovered.annotations.size() != publication.annotations.size();
    std::optional<PublicationAnnotationMismatch> annotationDiff;
    if (!countDiffers) {
      for (std::size_t i = 0; i < recovered.annotations.size(); ++i) {
        const auto recoveredObject = encodeAnnotation(recovered.annotations[i]);
        const auto publicationObject = encodeAnnotation(publication.annotations[i]);
        if (recoveredObject == publicationObject) continue;
        annotationDiff = PublicationAnnotationMismatch{
            i, publication.annotations[i].id, recovered.annotations[i].id,
            firstJsonDifference(recoveredObject, publicationObject)};
        break;
      }
    }
    if (sourceDiffers || countDiffers || annotationDiff) {
      const auto suffix = publicationMismatchSuffix(
          recovered, publication, sourceDiffers,
          sourceDiffers ? firstJsonDifference(recoveredSource, publicationSource) : QString{},
          annotationDiff ? &annotationDiff.value() : nullptr);
      if (sourceDiffers || countDiffers)
        return reject(QStringLiteral("持久检测来源/对象数与 publication 不同") + suffix);
      return reject(QStringLiteral("持久检测对象与 publication 不同") + suffix);
    }
    if (inject(options, DetectionTransactionPoint::Persist)) return reject("Injected detection persist failure");
    if (options.stillCurrent && !options.stillCurrent())
      return reject("检测事务已丢弃，提交期间工程已变化。已丢弃的正式结果不会恢复");
    bool generationStale = false;
    if (!ProjectFileCodec::save(path, candidate, error, AssetSaveMode::Preserve, {},
         [&] {
           if (inject(options, DetectionTransactionPoint::Commit)) return false;
           if (options.stillCurrent && !options.stillCurrent()) {
             generationStale = true;
             return false;
           }
           return true;
         }))
      return reject(generationStale
                        ? QString("检测事务已丢弃，提交期间工程已变化。已丢弃的正式结果不会恢复")
                        : error);
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
