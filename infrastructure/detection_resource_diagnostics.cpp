#include "infrastructure/detection_resource_diagnostics.h"

#include "infrastructure/detection_source.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace signalstudio {
namespace {
constexpr std::size_t kMaximumProfileBytes = 65536;

QString q(const std::string& value) { return QString::fromStdString(value); }
QString decimal(std::uint64_t value) {
  return QString::number(static_cast<qulonglong>(value));
}
QString statusName(ResourceProbeStatus value) {
  switch (value) {
    case ResourceProbeStatus::Probed:
      return QStringLiteral("probed");
    case ResourceProbeStatus::NotRequested:
      return QStringLiteral("not-requested");
    case ResourceProbeStatus::Unavailable:
      return QStringLiteral("unavailable");
  }
  return QStringLiteral("unavailable");
}
QJsonObject probeJson(const ResourceByteProbe& value) {
  return {{QStringLiteral("status"), statusName(value.status)},
          {QStringLiteral("total"), decimal(value.totalBytes)},
          {QStringLiteral("available"), decimal(value.availableBytes)},
          {QStringLiteral("detail"), q(value.detail)}};
}
QJsonObject volumeJson(const VolumeResourceProbe& value) {
  return {{QStringLiteral("status"), statusName(value.status)},
          {QStringLiteral("key"), q(value.volumeKey)},
          {QStringLiteral("available"), decimal(value.availableBytes)},
          {QStringLiteral("total"), decimal(value.totalBytes)},
          {QStringLiteral("detail"), q(value.detail)}};
}
QJsonObject decisionJson(const ResourceDecision& value) {
  const auto kind = value.kind == ResourceDecisionKind::Accepted ? QStringLiteral("Accepted")
                    : value.kind == ResourceDecisionKind::Adapted ? QStringLiteral("Adapted")
                                                                  : QStringLiteral("Rejected");
  return {{QStringLiteral("kind"), kind},
          {QStringLiteral("errorCode"), q(value.errorCode)},
          {QStringLiteral("phase"), q(value.phase)},
          {QStringLiteral("resource"), q(value.resource)},
          {QStringLiteral("unit"), QString::fromUtf8(resourceDecisionUnit(value))},
          {QStringLiteral("requested"), decimal(value.requested)},
          {QStringLiteral("available"), decimal(value.available)},
          {QStringLiteral("reserve"), decimal(value.reserve)},
          {QStringLiteral("granted"), decimal(value.granted)}};
}
QJsonObject deferredFact(const char* detail) {
  return {{QStringLiteral("status"), QStringLiteral("deferred")},
          {QStringLiteral("detail"), QString::fromUtf8(detail)}};
}
QJsonObject knownFact(std::uint64_t bytes, const char* detail) {
  return {{QStringLiteral("status"), QStringLiteral("known")},
          {QStringLiteral("bytes"), decimal(bytes)},
          {QStringLiteral("detail"), QString::fromUtf8(detail)}};
}
bool publishProfile(const std::string& performanceJson,
                    const RuntimeResourceSnapshot& snapshot,
                    const DiskLedgerEvaluation* ledger,
                    const ResourceDecision* sampleDecision,
                    const QJsonObject& extras, std::string& merged,
                    QString& error) {
  std::string documentText;
  std::string mergeError;
  if (!mergeRuntimeResourceDiagnostics(performanceJson, snapshot, ledger,
                                       sampleDecision, documentText, mergeError)) {
    error = q(mergeError);
    return false;
  }
  QJsonParseError parse;
  const auto parsed = QJsonDocument::fromJson(QByteArray::fromStdString(documentText), &parse);
  if (parse.error != QJsonParseError::NoError || !parsed.isObject()) {
    error = QStringLiteral("resource.diagnostics-rejected");
    return false;
  }
  auto profile = parsed.object();
  auto resources = profile.value(QStringLiteral("resources")).toObject();
  for (auto it = extras.constBegin(); it != extras.constEnd(); ++it)
    resources.insert(it.key(), it.value());
  profile.insert(QStringLiteral("resources"), resources);
  const auto bytes = QJsonDocument(profile).toJson(QJsonDocument::Compact);
  if (static_cast<std::size_t>(bytes.size()) > kMaximumProfileBytes) {
    error = QStringLiteral("resource.diagnostics-rejected");
    return false;
  }
  merged = bytes.toStdString();
  error.clear();
  return true;
}
QString windowsText(std::uint64_t windows, std::uint64_t perWindow) {
  return QStringLiteral("%1 windows x %2 bytes")
      .arg(static_cast<qulonglong>(windows))
      .arg(static_cast<qulonglong>(perWindow));
}
QString rawHeadError(const DetectionSourcePlan& plan, const DetectionRequest& request,
                     const RawHeadDiskAssessment& assessment,
                     const VolumeResourceProbe& volume) {
  if (request.legacyCombinedEvidence ||
      assessment.decision.kind != ResourceDecisionKind::Rejected)
    return {};
  const auto windows = plan.eligibleWindowCount;
  const auto perWindow = plan.modelHeadBytesPerWindow;
  const auto& code = assessment.decision.errorCode;
  if (code == "resource.arithmetic-overflow") {
    if (request.retainRawModelOutput) {
      return QStringLiteral(
                 "Raw model output retention rejected before inference: %1 "
                 "overflow the disk quota.")
          .arg(windowsText(windows, perWindow));
    }
    return QStringLiteral(
               "Raw model head disk budget rejected before inference: %1 "
               "overflow the disk quota.")
        .arg(windowsText(windows, perWindow));
  }
  if (code == "resource.explicit-raw-head-cap") {
    return QStringLiteral(
               "Raw model head disk budget rejected before inference: explicit "
               "quota %1 bytes exceeds the 2 GiB cap.")
        .arg(static_cast<qulonglong>(request.rawHeadBudgetBytes));
  }
  if (code == "resource.explicit-raw-head-quota" ||
      (code == "resource.raw-head-shortfall" && request.rawHeadBudgetBytes != 0 &&
       assessment.requiredBytes > request.rawHeadBudgetBytes)) {
    return QStringLiteral(
               "Raw model head disk budget rejected before inference: %1; "
               "limit %2 bytes. Reduce the range, increase the frame "
               "interval or explicitly provision a bounded artifact quota.")
        .arg(windowsText(windows, perWindow))
        .arg(static_cast<qulonglong>(request.rawHeadBudgetBytes));
  }
  if (code == "resource.raw-head-shortfall" &&
      volume.status != ResourceProbeStatus::Probed) {
    return QStringLiteral(
               "Raw model output retention rejected before inference: %1 "
               "require %2 bytes; remaining disk bytes are unavailable.")
        .arg(windowsText(windows, perWindow))
        .arg(static_cast<qulonglong>(assessment.requiredBytes));
  }
  if (code == "resource.raw-head-shortfall") {
    return QStringLiteral(
               "Raw model output retention rejected before inference: %1 "
               "require %2 bytes; %3 bytes remain.")
        .arg(windowsText(windows, perWindow))
        .arg(static_cast<qulonglong>(assessment.requiredBytes))
        .arg(static_cast<qulonglong>(volume.availableBytes));
  }
  return q(code + ": " + assessment.decision.message);
}
QString residentError(const ResourceDecision& decision) {
  return QStringLiteral(
             "%1: estimated source resident %2 bytes; available %3 bytes. "
             "The working-set authorization is a ceiling.")
      .arg(q(decision.errorCode))
      .arg(static_cast<qulonglong>(decision.requested))
      .arg(static_cast<qulonglong>(decision.available));
}
const char* rawHeadBranch(const DetectionRequest& request) {
  if (request.legacyCombinedEvidence) return "legacy-combined";
  if (request.retainRawModelOutput) return "on";
  if (request.rawHeadBudgetBytes > 0) return "explicit-quota";
  return "off";
}
bool knownStage(const char* stage) {
  return stage != nullptr &&
         (std::strcmp(stage, "checkpoint") == 0 || std::strcmp(stage, "refine") == 0 ||
          std::strcmp(stage, "publishing") == 0);
}
DetectionDiskUsage visibleUsage(const DetectionDiskUsage& knownUsage,
                                std::uint32_t knownFacts) {
  DetectionDiskUsage visible = knownUsage;
  if ((knownFacts & kDiskFactRawHead) == 0 || !visible.storeRawHead) {
    visible.storeRawHead = false;
    visible.rawHeadBytes = 0;
  }
  if ((knownFacts & kDiskFactResultShards) == 0) {
    visible.resultShardBytes = 0;
    visible.largestResultShardBytes = 0;
  }
  if ((knownFacts & kDiskFactPublicationShards) == 0) {
    visible.publicationShardBytes = 0;
    visible.largestPublicationShardBytes = 0;
  }
  if ((knownFacts & kDiskFactScratch) == 0) visible.scratchBytes = 0;
  if ((knownFacts & kDiskFactCheckpoint) == 0) {
    visible.checkpointBytes = 0;
    visible.checkpointExistingBytes = 0;
  }
  if ((knownFacts & kDiskFactProject) == 0) {
    visible.projectBytes = 0;
    visible.projectExistingBytes = 0;
  }
  return visible;
}
QJsonObject diskFactsForStage(const char* stage, const DetectionDiskUsage& usage,
                              std::uint32_t knownFacts, bool* peakComplete) {
  const bool publishing = std::strcmp(stage, "publishing") == 0;
  bool deferred = false;
  QJsonObject facts;
  if ((knownFacts & kDiskFactRawHead) != 0) {
    facts.insert(QStringLiteral("rawHead"),
                 usage.storeRawHead ? knownFact(usage.rawHeadBytes, "known raw-head bytes")
                                    : knownFact(0, "not-retained"));
  } else {
    deferred = true;
    facts.insert(QStringLiteral("rawHead"),
                 deferredFact("raw-head bytes are not known yet"));
  }
  if ((knownFacts & kDiskFactResultShards) != 0) {
    auto fact = knownFact(usage.resultShardBytes, "known result shard bytes");
    fact.insert(QStringLiteral("largestUnitBytes"), decimal(usage.largestResultShardBytes));
    facts.insert(QStringLiteral("resultShards"), fact);
  } else {
    deferred = true;
    facts.insert(QStringLiteral("resultShards"),
                 deferredFact("result shard bytes are not known yet"));
  }
  if ((knownFacts & kDiskFactPublicationShards) != 0) {
    auto fact = knownFact(usage.publicationShardBytes, "known publication shard bytes");
    fact.insert(QStringLiteral("largestUnitBytes"),
                decimal(usage.largestPublicationShardBytes));
    facts.insert(QStringLiteral("publicationShards"), fact);
  } else {
    deferred = true;
    facts.insert(QStringLiteral("publicationShards"),
                 deferredFact("publication shard bytes are not known yet"));
  }
  if ((knownFacts & kDiskFactScratch) != 0) {
    facts.insert(QStringLiteral("scratch"),
                 knownFact(usage.scratchBytes, "known scratch bytes"));
  } else if (publishing) {
    facts.insert(QStringLiteral("scratch"),
                 QJsonObject{{QStringLiteral("status"), QStringLiteral("not-live")},
                             {QStringLiteral("detail"),
                              QStringLiteral("scratch is reclaimed before publishing "
                                             "and is outside this peak")}});
  } else {
    deferred = true;
    facts.insert(QStringLiteral("scratch"),
                 deferredFact("scratch bytes are not knowable until the refine "
                              "spill size is computed"));
  }
  if ((knownFacts & kDiskFactCheckpoint) != 0) {
    auto fact = knownFact(usage.checkpointBytes, "known checkpoint payload");
    fact.insert(QStringLiteral("existingBytes"), decimal(usage.checkpointExistingBytes));
    facts.insert(QStringLiteral("checkpoint"), fact);
  } else {
    deferred = true;
    facts.insert(QStringLiteral("checkpoint"),
                 deferredFact("checkpoint payload and the existing file size are not known yet"));
  }
  if ((knownFacts & kDiskFactProject) != 0) {
    auto fact = knownFact(usage.projectBytes, "known project payload");
    fact.insert(QStringLiteral("existingBytes"), decimal(usage.projectExistingBytes));
    facts.insert(QStringLiteral("projectAtomic"), fact);
  } else {
    deferred = true;
    facts.insert(QStringLiteral("projectAtomic"),
                 deferredFact("project payload and the existing file size are not known yet"));
  }
  if (peakComplete != nullptr) *peakComplete = !deferred;
  return facts;
}
}  // namespace

bool mergeRuntimeResourceDiagnostics(const std::string& performanceJson,
                                     const RuntimeResourceSnapshot& snapshot,
                                     const DiskLedgerEvaluation* ledger,
                                     const ResourceDecision* sampleDecision,
                                     std::string& merged, std::string& error) {
  const auto reject = [&] {
    error = "resource.diagnostics-rejected";
    return false;
  };
  if (performanceJson.size() > kMaximumProfileBytes) return reject();
  QJsonObject profile;
  if (!performanceJson.empty()) {
    QJsonParseError parse;
    const auto document =
        QJsonDocument::fromJson(QByteArray::fromStdString(performanceJson), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return reject();
    profile = document.object();
    if (profile.contains(QStringLiteral("schema")) &&
        profile.value(QStringLiteral("schema")) !=
            QStringLiteral("signal-studio-detection-profile-v1"))
      return reject();
  }
  profile.insert(QStringLiteral("schema"),
                 QStringLiteral("signal-studio-detection-profile-v1"));
  QJsonObject resources{{QStringLiteral("ram"), probeJson(snapshot.ram)},
                        {QStringLiteral("vram"), probeJson(snapshot.vram)},
                        {QStringLiteral("volume"), volumeJson(snapshot.volume)},
                        {QStringLiteral("capturedAtSteadyNs"),
                         decimal(snapshot.capturedAtSteadyNs)}};
  if (ledger) {
    resources.insert(QStringLiteral("diskLedger"), decisionJson(ledger->decision));
    resources.insert(QStringLiteral("diskPeakPhase"), q(ledger->peakPhase));
    resources.insert(QStringLiteral("diskPeakVolume"), q(ledger->peakVolumeKey));
    resources.insert(QStringLiteral("diskPeakBytes"), decimal(ledger->peakBytes));
  }
  if (sampleDecision)
    resources.insert(QStringLiteral("sample"), decisionJson(*sampleDecision));
  profile.insert(QStringLiteral("resources"), resources);
  const auto bytes = QJsonDocument(profile).toJson(QJsonDocument::Compact);
  if (static_cast<std::size_t>(bytes.size()) > kMaximumProfileBytes) return reject();
  merged = bytes.toStdString();
  error.clear();
  return true;
}

bool assessDetectionResourcePreflight(const QString& assetDirectory,
                                      const DetectionSourcePlan& plan,
                                      const DetectionRequest& request,
                                      std::string& performanceJson,
                                      QString& error) {
  const auto snapshot =
      captureRuntimeResourceSnapshot(assetDirectory.toStdString(), nullptr);
  return assessDetectionResourcePreflight(snapshot, plan, request, performanceJson, error);
}

bool assessDetectionResourcePreflight(const RuntimeResourceSnapshot& snapshot,
                                      const DetectionSourcePlan& plan,
                                      const DetectionRequest& request,
                                      std::string& performanceJson,
                                      QString& error) {
  RawHeadDiskInput input;
  input.legacyCombined = request.legacyCombinedEvidence;
  input.retainRawModelOutput = request.retainRawModelOutput;
  input.windows = plan.eligibleWindowCount;
  input.bytesPerWindow = plan.modelHeadBytesPerWindow;
  input.explicitQuotaBytes = request.rawHeadBudgetBytes;
  input.volumeKnown = snapshot.volume.status == ResourceProbeStatus::Probed;
  input.volumeAvailableBytes = input.volumeKnown ? snapshot.volume.availableBytes : 0;
  const auto assessment = assessRawHeadDisk(input);
  const bool estimateKnown = plan.estimatedPeakBytes > 0;
  ResourceDecision sample;
  ResourceDecision resident;
  const ResourceDecision* samplePointer = nullptr;
  if (estimateKnown) {
    // Cell count is not known here. The sample ceiling function still requires
    // the argument; source preflight records sampleCells as deferred.
    sample = decideSamplePhaseBudget(request.workingSetBudgetBytes,
                                     plan.estimatedPeakBytes, 0);
    resident = decideResidentAgainstProbe("source", "ram", plan.estimatedPeakBytes,
                                          snapshot.ram, 0);
    samplePointer = &sample;
  }

  DiskLedgerEvaluation recorded;
  const bool chargeRetainedVolume =
      request.retainRawModelOutput && !request.legacyCombinedEvidence &&
      assessment.applies && assessment.decision.kind == ResourceDecisionKind::Accepted &&
      assessment.decision.granted > 0;
  if (chargeRetainedVolume) {
    DetectionDiskUsage usage;
    usage.durableVolumeKey = snapshot.volume.volumeKey;
    if (applyAcceptedRawHeadToUsage(assessment, usage)) {
      std::vector<DiskLedgerLine> lines;
      appendPhaseOverlapDiskLines(usage, lines);
      const auto budget = diskBudgetFromVolumeProbe(snapshot.volume);
      recorded = evaluatePhaseOverlapDiskLedger(
          lines, std::span<const DiskVolumeBudget>(&budget, 1), 0);
    } else {
      recorded.decision = assessment.decision;
      recorded.peakPhase = "preflight";
      recorded.peakVolumeKey = snapshot.volume.volumeKey;
      recorded.peakBytes = assessment.requiredBytes;
    }
  } else if (request.legacyCombinedEvidence) {
    recorded = evaluatePhaseOverlapDiskLedger({}, {}, 0);
    recorded.peakPhase = "preflight";
    recorded.peakVolumeKey = snapshot.volume.volumeKey;
  } else {
    recorded.decision = assessment.decision;
    recorded.peakPhase = "preflight";
    recorded.peakVolumeKey = snapshot.volume.volumeKey;
    recorded.peakBytes =
        assessment.overflow ? 0 : assessment.requiredBytes;
  }

  QJsonObject facts;
  QJsonObject product;
  if (request.legacyCombinedEvidence) {
    facts.insert(QStringLiteral("rawHead"),
                 QJsonObject{{QStringLiteral("status"), QStringLiteral("legacy-combined")},
                             {QStringLiteral("detail"),
                              QStringLiteral("legacy combined evidence stays on "
                                             "applyDetectionEvidencePreflight")}});
    product.insert(QStringLiteral("status"), QStringLiteral("legacy-combined"));
  } else if (!request.retainRawModelOutput && request.rawHeadBudgetBytes == 0) {
    facts.insert(QStringLiteral("rawHead"), knownFact(0, "not-retained"));
    product.insert(QStringLiteral("status"), QStringLiteral("not-taken"));
    product.insert(QStringLiteral("detail"),
                   QStringLiteral("retain off with no explicit quota does not "
                                  "multiply windows by per-window bytes"));
  } else if (assessment.overflow ||
             assessment.decision.errorCode == "resource.explicit-raw-head-cap") {
    const auto overflow = assessment.overflow;
    facts.insert(QStringLiteral("rawHead"),
                 QJsonObject{{QStringLiteral("status"),
                              overflow ? QStringLiteral("overflow")
                                       : QStringLiteral("over-cap")},
                             {QStringLiteral("detail"),
                              overflow ? QStringLiteral("windows x per-window bytes overflow uint64")
                                       : QStringLiteral("explicit quota exceeds 2 GiB "
                                                        "before a product is taken")}});
    product.insert(QStringLiteral("status"),
                   overflow ? QStringLiteral("overflow") : QStringLiteral("over-cap"));
  } else if (assessment.decision.kind == ResourceDecisionKind::Accepted &&
             assessment.decision.granted == 0) {
    facts.insert(QStringLiteral("rawHead"), knownFact(0, "no raw-head bytes to store"));
    product.insert(QStringLiteral("status"), QStringLiteral("known"));
    product.insert(QStringLiteral("bytes"), decimal(0));
  } else {
    facts.insert(QStringLiteral("rawHead"),
                 knownFact(assessment.requiredBytes, "checked raw-head product"));
    product.insert(QStringLiteral("status"), QStringLiteral("known"));
    product.insert(QStringLiteral("bytes"), decimal(assessment.requiredBytes));
  }
  facts.insert(QStringLiteral("resultShards"),
               deferredFact("result shard bytes are not knowable at preflight"));
  facts.insert(QStringLiteral("publicationShards"),
               deferredFact("refined publication bytes are not knowable at preflight"));
  facts.insert(QStringLiteral("scratch"),
               deferredFact("scratch bytes are not knowable until the refine spill size is computed"));
  facts.insert(QStringLiteral("checkpoint"),
               deferredFact("checkpoint payload and the existing file size are not knowable at preflight"));
  facts.insert(QStringLiteral("projectAtomic"),
               deferredFact("project payload and the existing file size are not knowable at preflight"));

  QJsonObject sourceResident;
  if (estimateKnown) {
    sourceResident = decisionJson(resident);
    sourceResident.insert(QStringLiteral("status"), QStringLiteral("known"));
  } else {
    sourceResident = deferredFact(
        "plan.estimatedPeakBytes is 0; no source resident byte count was invented");
  }
  QJsonObject extras{
      {QStringLiteral("sourceResident"), sourceResident},
      {QStringLiteral("sampleCells"),
       deferredFact("source preflight does not invent a sample cell count")},
      {QStringLiteral("workingSetAuthorization"),
       QJsonObject{{QStringLiteral("bytes"), decimal(request.workingSetBudgetBytes)},
                   {QStringLiteral("role"), QStringLiteral("ceiling")},
                   {QStringLiteral("comparedToAvailableRam"), false}}},
      {QStringLiteral("rawHeadBranch"), QString::fromUtf8(rawHeadBranch(request))},
      {QStringLiteral("rawHeadProduct"), product},
      {QStringLiteral("diskFacts"), facts},
      {QStringLiteral("volumeEnforced"),
       request.retainRawModelOutput && !request.legacyCombinedEvidence},
      {QStringLiteral("knownPeakComplete"), false},
      {QStringLiteral("ledgerScope"), QStringLiteral("known-only")}};
  if (!publishProfile(performanceJson, snapshot, &recorded, samplePointer, extras,
                      performanceJson, error))
    return false;

  if (const auto raw = rawHeadError(plan, request, assessment, snapshot.volume);
      !raw.isEmpty()) {
    error = raw;
    return false;
  }
  if (estimateKnown && sample.kind == ResourceDecisionKind::Rejected) {
    error = QStringLiteral("%1: %2").arg(q(sample.errorCode), q(sample.message));
    return false;
  }
  if (estimateKnown && resident.kind == ResourceDecisionKind::Rejected) {
    error = residentError(resident);
    return false;
  }
  if (chargeRetainedVolume && recorded.decision.kind == ResourceDecisionKind::Rejected) {
    error = q(recorded.decision.errorCode + ": " + recorded.decision.message);
    return false;
  }
  error.clear();
  return true;
}

bool assessDetectionResourceStage(const char* stage, const DetectionDiskUsage& knownUsage,
                                  std::uint32_t knownFacts,
                                  const RuntimeResourceSnapshot& snapshot,
                                  std::string& performanceJson, QString& error,
                                  const RuntimeResourceSnapshot* scratchSnapshot,
                                  std::uint64_t reserveBytes) {
  if (!knownStage(stage)) {
    error = QStringLiteral("resource.invalid-stage");
    return false;
  }
  auto visible = visibleUsage(knownUsage, knownFacts);
  if (visible.durableVolumeKey.empty())
    visible.durableVolumeKey = snapshot.volume.volumeKey;
  if ((knownFacts & kDiskFactScratch) != 0 && visible.scratchVolumeKey.empty())
    visible.scratchVolumeKey = visible.durableVolumeKey;

  std::vector<DiskLedgerLine> lines;
  appendPhaseOverlapDiskLines(visible, lines);
  std::vector<DiskVolumeBudget> budgets;
  budgets.push_back(diskBudgetFromVolumeProbe(snapshot.volume));
  const bool separateScratch =
      (knownFacts & kDiskFactScratch) != 0 && visible.scratchBytes > 0 &&
      !visible.scratchVolumeKey.empty() &&
      visible.scratchVolumeKey != visible.durableVolumeKey;
  if (separateScratch && scratchSnapshot != nullptr &&
      scratchSnapshot->volume.volumeKey == visible.scratchVolumeKey) {
    budgets.push_back(diskBudgetFromVolumeProbe(scratchSnapshot->volume));
  }
  const auto evaluation = evaluatePhaseOverlapDiskLedger(lines, budgets, reserveBytes);
  bool peakComplete = false;
  QJsonObject extras{
      {QStringLiteral("stage"), QString::fromUtf8(stage)},
      {QStringLiteral("diskFacts"),
       diskFactsForStage(stage, knownUsage, knownFacts, &peakComplete)},
      {QStringLiteral("knownPeakComplete"), peakComplete},
      {QStringLiteral("volumeEnforced"), true},
      {QStringLiteral("ledgerScope"), QStringLiteral("known-only")}};
  if (separateScratch && scratchSnapshot != nullptr)
    extras.insert(QStringLiteral("scratchVolume"), volumeJson(scratchSnapshot->volume));
  if (!publishProfile(performanceJson, snapshot, &evaluation, nullptr, extras,
                      performanceJson, error))
    return false;
  if (evaluation.decision.kind == ResourceDecisionKind::Rejected) {
    error = q(evaluation.decision.errorCode + ": " + evaluation.decision.message);
    return false;
  }
  error.clear();
  return true;
}
}  // namespace signalstudio
