#include "infrastructure/label_discovery.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <algorithm>
#include <stdexcept>

namespace signalstudio {
namespace {
QString q(const std::string& text) { return QString::fromUtf8(text.data(), text.size()); }
QString canonical(const QString& path) {
  const QFileInfo info(path);
  return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
}
bool samePath(const QString& a, const QString& b) {
  return canonical(a) == canonical(b);
}
bool pathAlias(const QString& path) {
  QFileInfo info(path);
  for (;;) {
    if (info.isSymLink() || info.isJunction()) return true;
    const auto parent = info.dir().absolutePath();
    if (parent == info.absoluteFilePath()) return false;
    info.setFile(parent);
  }
}
bool sameBinding(SourceIdentity a, SourceIdentity b) {
  a.projectGeneration = b.projectGeneration = 0;
  a.availableSamples = b.availableSamples = 0;
  return a == b;
}
struct DiscoveryAbort : std::runtime_error {
  LabelDiscoveryState state;
  DiscoveryAbort(LabelDiscoveryState s, const QString& text)
      : std::runtime_error(text.toUtf8().toStdString()), state(s) {}
};
class Inspector {
 public:
  explicit Inspector(const LabelDiscoveryOptions& options) : options_(options) {
    timer_.start();
    if (options.maximumDirectoryEntries == 0 || options.maximumDirectoryEntries > 100000 ||
        options.maximumPreviewBytes <= 0 || options.maximumPreviewBytes > 65536 ||
        options.parsing.maximumBytes <= 0 || options.parsing.maximumBytes > 32 * 1024 * 1024 ||
        options.parsing.timeoutMs <= 0)
      fail(LabelDiscoveryState::Error, "标签发现限额无效");
    check();
  }
  [[noreturn]] static void fail(LabelDiscoveryState state, const QString& message) {
    throw DiscoveryAbort(state, message);
  }
  void check() const {
    if (options_.parsing.cancelled && options_.parsing.cancelled())
      fail(LabelDiscoveryState::Cancelled, "标签发现已取消");
    if (timer_.elapsed() > options_.parsing.timeoutMs)
      fail(LabelDiscoveryState::LimitExceeded, "标签发现超时");
  }
  // Enumerate all entries, including casing variants and aliases. entryList
  // would allocate an unbounded list before it could enforce the entry budget.
  QString exact(const QString& directory, const QString& expected) {
    check();
    const auto key = QDir(directory).absolutePath();
    if (!cache_.contains(key)) {
      const QFileInfo dir(key);
      if (!dir.exists()) return {};
      if (!dir.isDir() || !dir.isReadable()) fail(LabelDiscoveryState::Error, "标签目录不可读取：" + key);
      if (pathAlias(key)) fail(LabelDiscoveryState::Ambiguous, "目录为路径别名，请显式选择：" + key);
      QStringList entries;
      QDirIterator it(key, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
      while (it.hasNext()) {
        check();
        if (++entriesSeen_ > options_.maximumDirectoryEntries)
          fail(LabelDiscoveryState::LimitExceeded, "标签发现目录项超过预算，未选择任何候选");
        it.next();
        entries.push_back(it.fileName());
      }
      cache_.emplace(key, std::move(entries));
      inspected.push_back(key);
    }
    QStringList matches;
    for (const auto& name : cache_.at(key))
      if (name.compare(expected, Qt::CaseInsensitive) == 0) matches.push_back(name);
    if (matches.isEmpty()) return {};
    if (matches.size() != 1 || matches.front() != expected)
      fail(LabelDiscoveryState::Ambiguous, "同名大小写冲突或主名不严格一致：" + expected);
    const auto found = QDir(key).filePath(matches.front());
    if (pathAlias(found))
      fail(LabelDiscoveryState::Ambiguous, "候选为路径别名，请显式选择：" + found);
    return found;
  }
  QString preview(const QString& path) {
    check();
    QString text, error;
    LabelImportState state;
    if (!previewLabelSource(path, text, error, options_.parsing, options_.maximumPreviewBytes, &state))
      fail(state == LabelImportState::Cancelled ? LabelDiscoveryState::Cancelled :
           state == LabelImportState::LimitExceeded ? LabelDiscoveryState::LimitExceeded :
           LabelDiscoveryState::Error, error);
    check();
    return text;
  }
  QStringList inspected;
 private:
  const LabelDiscoveryOptions& options_;
  QElapsedTimer timer_;
  std::size_t entriesSeen_ = 0;
  std::map<QString, QStringList> cache_;
};
void assess(LabelImportPlan& plan, const FileState& file) {
  const auto& current = plan.imported.sourceSet;
  for (const auto& previous : file.sourceSets) {
    if (previous.fileId != current.fileId || previous.sampleChannel != current.sampleChannel ||
        previous.sourceKind != AnnotationSource::ImportedLabel) continue;
    // Deleted sets remain a deliberate user decision; automatic rechecks
    // must not resurrect their objects or erase their revised geometry.
    if (previous.id == current.id && previous.sourceHash == current.sourceHash &&
        previous.supplementalHash == current.supplementalHash &&
        previous.formatId == current.formatId && previous.profileId == current.profileId &&
        previous.parserId == current.parserId && previous.parserVersion == current.parserVersion &&
        previous.profileVersion == current.profileVersion &&
        previous.coordinateInterpretation == current.coordinateInterpretation &&
        sameBinding(previous.sourceIdentity, current.sourceIdentity)) {
      plan.disposition = LabelImportDisposition::AlreadyImported;
      plan.message = "同一来源及转换已导入，保留当前修订（含软删除）";
      return;
    }
    const bool sameLocation = samePath(q(previous.originalPath), q(current.originalPath));
    const bool sameText = previous.sourceHash == current.sourceHash &&
                          previous.formatId == current.formatId;
    if (previous.id == current.id || sameLocation || sameText) {
      plan.disposition = LabelImportDisposition::SourceChanged;
      plan.message = "标签内容、补充文件、物理绑定或字段解释已变化；请保留/比对/显式重载";
      return;
    }
  }
  plan.disposition = plan.imported.state == LabelImportState::Empty
                         ? LabelImportDisposition::Empty :
                     plan.imported.state == LabelImportState::Incomplete
                         ? LabelImportDisposition::IncompleteEvidence : LabelImportDisposition::Ready;
}
}  // namespace

LabelDiscoveryResult discoverLabelCandidates(const FileMetadata& target,
                                             const LabelDiscoveryOptions& options) {
  LabelDiscoveryResult result;
  try {
    Inspector inspector(options);
    const QFileInfo data(q(target.path));
    if (!data.isFile() || !data.isReadable())
      Inspector::fail(LabelDiscoveryState::Error, "数据源不可读取");
    if (pathAlias(data.absoluteFilePath())) Inspector::fail(LabelDiscoveryState::Ambiguous, "数据源为路径别名，请显式确认");
    const auto directory = data.absolutePath();
    // Also detect source casing collisions on case-sensitive filesystems.
    inspector.exact(directory, data.fileName());
    if (data.fileName().endsWith(".sigmf-data", Qt::CaseSensitive)) {
      const auto stem = data.fileName().chopped(11);
      const auto meta = inspector.exact(directory, stem + ".sigmf-meta");
      const auto extra = inspector.exact(directory, stem + ".json");
      if (!meta.isEmpty()) result.candidates.push_back(
          {LabelProfile::TorchSig, data.absoluteFilePath(), meta, extra, inspector.preview(meta)});
      else if (!extra.isEmpty()) Inspector::fail(LabelDiscoveryState::Error, "存在 rfbench JSON，但缺少配对 SigMF meta");
    } else if (data.suffix().compare("sigmf-data", Qt::CaseInsensitive) == 0) {
      Inspector::fail(LabelDiscoveryState::Ambiguous, "SigMF 扩展名大小写与确定性 profile 不一致");
    } else {
      const auto stem = data.completeBaseName();
      const QDir sourceDir(directory);
      const bool matlab = sourceDir.dirName() == "data" && data.suffix() == "data";
      if (!matlab && sourceDir.dirName().compare("data", Qt::CaseInsensitive) == 0 &&
          data.suffix().compare("data", Qt::CaseInsensitive) == 0)
        Inspector::fail(LabelDiscoveryState::Ambiguous, "data 目录或 .data 扩展名大小写不严格一致");
      QString sibling;
      if (matlab) {
        auto parent = sourceDir;
        parent.cdUp();
        inspector.exact(parent.absolutePath(), "data");
        const auto labelDir = inspector.exact(parent.absolutePath(), "label");
        if (!labelDir.isEmpty()) sibling = inspector.exact(labelDir, stem + ".txt");
      }
      const auto adjacent = inspector.exact(directory, stem + ".txt");
      for (const auto& label : {sibling, adjacent}) {
        if (label.isEmpty()) continue;
        const auto text = inspector.preview(label);
        const bool generator = hasGeneratorLabelHeader(text);
        result.candidates.push_back({generator ? LabelProfile::GeneratorText : LabelProfile::MatlabDataTxt,
                                     data.absoluteFilePath(), label, {}, text});
      }
    }
    result.inspectedDirectories = inspector.inspected;
    inspector.check();
    if (result.candidates.size() > 1) {
      result.state = LabelDiscoveryState::Ambiguous;
      result.message = "发现多个同名标签来源，请选择后预览；未自动导入";
    } else if (result.candidates.empty()) {
      result.message = "未发现标签，可手工创建或选择标签文件";
    } else {
      const auto& candidate = result.candidates.front();
      if (candidate.profile == LabelProfile::MatlabDataTxt &&
          QFileInfo(candidate.labelPath).size() <= options.maximumPreviewBytes &&
          candidate.preview.trimmed().isEmpty()) {
        result.state = LabelDiscoveryState::Empty;
        result.message = "找到空 TXT，未生成标记；可手工创建或选择其它标签文件";
        return result;
      }
      result.state = candidate.profile == LabelProfile::MatlabDataTxt
                         ? LabelDiscoveryState::NeedsMapping : LabelDiscoveryState::Found;
      result.message = result.state == LabelDiscoveryState::NeedsMapping
                           ? "同名配对已确认；TXT 字段含义未知，请配置字段映射"
                           : "发现唯一可信 profile，仍需整批格式与物理校验";
    }
  } catch (const DiscoveryAbort& e) {
    result.state = e.state;
    result.message = QString::fromUtf8(e.what());
    result.candidates.clear();
  } catch (const std::exception& e) {
    result.state = LabelDiscoveryState::Error;
    result.message = QString::fromUtf8(e.what());
    result.candidates.clear();
  }
  return result;
}

LabelDiscoveryResult discoverDataForLabel(const QString& path,
                                          const LabelDiscoveryOptions& options) {
  LabelDiscoveryResult result;
  try {
    Inspector inspector(options);
    const QFileInfo label(path);
    auto directory = label.dir();
    if (directory.dirName() != "label" || label.suffix() != "txt" || !label.isFile())
      Inspector::fail(LabelDiscoveryState::Error, "反查只接受 label/<name>.txt");
    inspector.exact(label.absolutePath(), label.fileName());
    directory.cdUp();
    const auto dataDir = inspector.exact(directory.absolutePath(), "data");
    const auto data = dataDir.isEmpty() ? QString() : inspector.exact(dataDir, label.completeBaseName() + ".data");
    result.inspectedDirectories = inspector.inspected;
    if (data.isEmpty()) result.message = "未发现严格同名 data/<name>.data；不匹配其它数据";
    else if (!QFileInfo(data).isFile()) Inspector::fail(LabelDiscoveryState::Error, "配对数据不是文件");
    else {
      result.state = LabelDiscoveryState::Found;
      result.candidates.push_back({LabelProfile::MatlabDataTxt, data, label.absoluteFilePath(), {}, inspector.preview(path)});
      result.message = "已找到同名原始数据；请通过源导入预检确认编码及 Fs/Fc";
    }
    inspector.check();
  } catch (const DiscoveryAbort& e) {
    result.state = e.state;
    result.message = QString::fromUtf8(e.what());
    result.candidates.clear();
  } catch (const std::exception& e) {
    result.state = LabelDiscoveryState::Error;
    result.message = QString::fromUtf8(e.what());
    result.candidates.clear();
  }
  return result;
}

LabelImportPlan prepareDiscoveredLabels(const LabelCandidate& chosen, const FileState& file,
                                        std::uint64_t generation,
                                        const std::optional<LabelFieldMapping>& mapping,
                                        const LabelDiscoveryOptions& options) {
  LabelImportPlan plan;
  const auto discovered = discoverLabelCandidates(file.metadata, options);
  // An explicitly selected candidate may resolve multiple real candidates,
  // but casing/alias failures have no selectable candidate and stay blocked.
  const auto found = std::find_if(discovered.candidates.begin(), discovered.candidates.end(),
      [&](const auto& c) { return c.labelPath == chosen.labelPath && c.dataPath == chosen.dataPath &&
                                 c.profile == chosen.profile && c.supplementalPath == chosen.supplementalPath; });
  if (found == discovered.candidates.end()) {
    plan.message = discovered.message.isEmpty() ? "候选或数据绑定已变化，请重新发现" : discovered.message;
    plan.imported.error = plan.message;
    plan.imported.state = discovered.state == LabelDiscoveryState::Cancelled ? LabelImportState::Cancelled :
                          discovered.state == LabelDiscoveryState::LimitExceeded ? LabelImportState::LimitExceeded : LabelImportState::Error;
    return plan;
  }
  if (found->profile == LabelProfile::MatlabDataTxt && !mapping) {
    plan.disposition = LabelImportDisposition::NeedsMapping;
    plan.imported.state = LabelImportState::NeedsMapping;
    plan.message = plan.imported.error = "未知 TXT 必须先确认字段映射，未导入任何标记";
    return plan;
  }
  if (found->profile == LabelProfile::TorchSig && mapping) {
    plan.message = plan.imported.error = "SigMF 不能使用 TXT 字段映射";
    plan.imported.state = LabelImportState::Error;
    return plan;
  }
  if (mapping) plan.imported = importMappedTextLabels(found->labelPath, file.metadata, *mapping, generation, options.parsing);
  else if (found->profile == LabelProfile::TorchSig)
    plan.imported = importTorchSigLabels(found->labelPath, file.metadata, generation, false, options.parsing);
  else plan.imported = importGeneratorLabels(found->labelPath, file.metadata, generation, options.parsing);
  if (!plan.imported.ok()) {
    plan.message = plan.imported.error;
    if (plan.imported.state == LabelImportState::Incomplete &&
        plan.imported.incompleteRecords > 0 && plan.imported.annotations.empty() &&
        !plan.imported.sourceSet.id.empty()) assess(plan, file);
    return plan;
  }
  for (const auto& a : plan.imported.annotations) {
    ++plan.classCounts[a.classLabel.value_or("")];
    if (a.observation) {
      const auto f = a.observation->frequency;
      if (!plan.frequencyExtent) plan.frequencyExtent = f;
      else { plan.frequencyExtent->lowerHz = std::min(plan.frequencyExtent->lowerHz, f.lowerHz);
             plan.frequencyExtent->upperHz = std::max(plan.frequencyExtent->upperHz, f.upperHz); }
    }
    for (const auto& s : a.segments) {
      if (!plan.timeExtent) plan.timeExtent = s.range.time;
      else { plan.timeExtent->begin = std::min(plan.timeExtent->begin, s.range.time.begin);
             plan.timeExtent->end = std::max(plan.timeExtent->end, s.range.time.end); }
      if (!plan.frequencyExtent) plan.frequencyExtent = s.range.frequency;
      else { plan.frequencyExtent->lowerHz = std::min(plan.frequencyExtent->lowerHz, s.range.frequency.lowerHz);
             plan.frequencyExtent->upperHz = std::max(plan.frequencyExtent->upperHz, s.range.frequency.upperHz); }
    }
  }
  assess(plan, file);
  return plan;
}

AnnotationResult publishDiscoveredLabels(AnnotationService& service, FileState& file,
                                         const LabelImportPlan& prepared,
                                         std::uint64_t generation) {
  if (prepared.disposition != LabelImportDisposition::Ready &&
      prepared.disposition != LabelImportDisposition::Empty &&
      prepared.disposition != LabelImportDisposition::IncompleteEvidence &&
      prepared.disposition != LabelImportDisposition::AlreadyImported)
    return {false, false, {}, "Label import is blocked pending mapping/conflict resolution"};
  const auto& identity = prepared.imported.sourceSet.sourceIdentity;
  const bool sourceOnly = prepared.imported.state == LabelImportState::Incomplete &&
                          prepared.imported.incompleteRecords > 0 &&
                          prepared.imported.annotations.empty() &&
                          prepared.imported.sourceSet.formatId == "mapped-txt-v1";
  if ((!prepared.imported.ok() && !sourceOnly) || identity.projectGeneration != generation ||
      !sameBinding(identity, annotationSourceIdentity(file.metadata, generation)))
    return {false, false, {}, "Project/file identity changed before label publication"};
  auto rechecked = prepared;
  assess(rechecked, file);
  if (rechecked.disposition == LabelImportDisposition::SourceChanged)
    return {false, false, {}, rechecked.message.toUtf8().toStdString()};
  if (rechecked.disposition == LabelImportDisposition::AlreadyImported)
    return {true, false, prepared.imported.sourceSet.id, {}};
  return service.publishSourceSet(file, prepared.imported.sourceSet, prepared.imported.annotations);
}
}  // namespace signalstudio
