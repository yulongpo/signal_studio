// SS-DE-RPO logic regressions. Standalone main, same check style as
// tests/detection_task_tests.cpp. R-01 stays a positive requirement: a 6 GiB
// grant at the cell ceiling must be accepted. Do not invert it back into a
// rejection, and do not treat a byte cap as a cell cap.
#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "application/detection_result_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/detection_model_backend.h"
#include "infrastructure/detection_projection.h"
#include "infrastructure/detection_sample_stream.h"
#include "infrastructure/detection_source.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/detection_task.h"
#include "infrastructure/project_file.h"
#include "infrastructure/source_loader.h"

using namespace signalstudio;
namespace ss = signalstudio::sample_stream;

namespace {
constexpr char kBaseline[] = "7220b2e295963288cb5d11e76946bb77646be0b0";
// Contract kSampleWorkUnitCellCeiling. Equality is legal; one past it is not.
constexpr std::size_t kSampleWorkUnitCellCeiling = std::size_t{256} * 1024 * 1024;
constexpr std::uint64_t kSixGiB = 6ull << 30;
constexpr std::uint64_t kWindows = 2044;
constexpr std::uint64_t kHeadBytes = 196608;
constexpr std::uint64_t kRequiredRawBytes = kWindows * kHeadBytes;
constexpr std::uint64_t kExplicitRawLimit = 256ull * 1024 * 1024;
static_assert(kSampleWorkUnitCellCeiling == 256u * 1024u * 1024u);
static_assert(kRequiredRawBytes == 401866752ull);
static_assert(kExplicitRawLimit == 268435456ull);
static_assert(kSixGiB == 6442450944ull);

int gFailures = 0;

void report(const char* id, bool pass, const std::string& detail) {
  if (pass) {
    std::cout << id << " PASS";
    if (!detail.empty()) std::cout << ' ' << detail;
    std::cout << '\n';
    return;
  }
  ++gFailures;
  std::cout << id << " FAIL " << detail << '\n';
}

std::string planFailure(bool ok, const ss::Failure& failure) {
  return std::string("ok=") + (ok ? "true" : "false") +
         " code=" + std::to_string(static_cast<int>(failure.code)) +
         " stage=" + failure.stage.toStdString() +
         " detail=" + failure.detail.toStdString();
}

ss::Context minimalContext(std::size_t cells) {
  constexpr double fs = 1000;
  constexpr SampleIndex n = 1000;
  ss::Context context;
  context.input.candidateId = "ss-de-rpo-r01";
  context.input.sampleRateHz = fs;
  context.input.observation = {{0, n}, {-fs * 0.12, fs * 0.12}};
  context.input.publishBounds = context.input.observation;
  context.searchSamples = {0, n};
  context.availableSamples = n;
  context.bandwidthHz = fs * 0.24;
  context.relativeCenterHz = 0;
  context.parameters.timeStrategy = DetectionTimeStrategy::Sample;
  context.parameters.smoothSeconds = 0.001;
  context.parameters.minOnSeconds = 0.003;
  context.parameters.minOffSeconds = 0.005;
  context.options.workingCellBudget = cells;
  return context;
}

ss::Config minimalConfig() {
  ss::Config config;
  config.mode = ss::Mode::StreamingFir;
  config.outputBlockSamples = 257;
  config.scratchBudgetBytes = 16ull << 20;
  config.scratchFreeFloorBytes = 0;
  return config;
}

ss::Runtime minimalRuntime(std::uint64_t grant, std::uint64_t* consumed) {
  ss::Runtime runtime;
  runtime.readerResidentReserveBytes = 0;
  runtime.memoryGrantBytes = grant;
  runtime.resultByteGrant = 16ull << 20;
  runtime.sourceReadBlockSamples = 257;
  runtime.work.consumedCells = consumed;
  runtime.work.limit = std::uint64_t{1} << 34;
  runtime.verifySource = [](QString&) { return true; };
  return runtime;
}

bool callPlan(std::size_t cells, std::uint64_t grant, ss::Plan& plan, ss::Failure& failure) {
  std::uint64_t consumed = 0;
  const auto context = minimalContext(cells);
  const auto config = minimalConfig();
  const auto runtime = minimalRuntime(grant, &consumed);
  return ss::makePlan(context, config, runtime, plan, failure);
}

void testResourceDecisions() {
  const std::uint64_t chunk = 32ull << 20;
  const std::array<std::uint64_t, 5> grants{1ull << 30, 2ull << 30, 4ull << 30,
                                            8ull << 30, 16ull << 30};
  for (const auto grant : grants) {
    const auto decision = decideSamplePhaseBudget(grant, chunk, 1024);
    const auto id = "R-02-" + std::to_string(grant >> 30) + "GiB";
    report(id.c_str(),
           decision.kind == ResourceDecisionKind::Accepted &&
               decision.errorCode.empty() && decision.phase == "time-refine" &&
               decision.resource == "ram" && decision.requested == chunk &&
               decision.available == grant && decision.granted == chunk &&
               decision.reserve == 0,
           decision.message);
  }

  const auto adapted =
      decideSamplePhaseBudget(8ull << 30, kSampleContiguousAllocationCapBytes + 1, 1024);
  report("R-02-adapted-above-contiguous",
         adapted.kind == ResourceDecisionKind::Adapted &&
             adapted.errorCode == "sample.grant-exceeds-contiguous-cap" &&
             adapted.resource == "ram" && adapted.phase == "time-refine" &&
             adapted.requested == kSampleContiguousAllocationCapBytes + 1 &&
             adapted.available == (8ull << 30) &&
             adapted.granted == kSampleContiguousAllocationCapBytes &&
             adapted.reserve == 0,
         adapted.message +
             " A grant above 1 GiB must chunk, not be rejected for its size.");

  const auto invalid = decideSamplePhaseBudget(0, 100, 10);
  report("R-03-zero-authorization",
         invalid.kind == ResourceDecisionKind::Rejected &&
             invalid.errorCode == "sample.invalid-context" &&
             invalid.phase == "time-refine" && invalid.resource == "ram" &&
             invalid.available == 0 && invalid.granted == 0 &&
             invalid.requested == 100,
         invalid.message);

  const auto chunkOver = decideSamplePhaseBudget(64ull << 20, (64ull << 20) + 1, 100);
  report("R-03-chunk-exceeds-grant",
         chunkOver.kind == ResourceDecisionKind::Rejected &&
             chunkOver.errorCode == "sample.chunk-exceeds-grant" &&
             chunkOver.resource == "ram" && chunkOver.phase == "time-refine" &&
             chunkOver.requested == (64ull << 20) + 1 &&
             chunkOver.available == (64ull << 20) && chunkOver.granted == 0,
         chunkOver.message);

  const auto cells = decideSamplePhaseBudget(
      8ull << 30, 1024, kSampleWorkUnitCellCeiling + 1);
  report("R-03-cells-not-bytes",
         cells.kind == ResourceDecisionKind::Rejected &&
             cells.errorCode == "sample.work-unit-ceiling" &&
             cells.resource == "work-unit" && cells.phase == "time-refine" &&
             cells.requested ==
                 static_cast<std::uint64_t>(kSampleWorkUnitCellCeiling) + 1 &&
             cells.available ==
                 static_cast<std::uint64_t>(kSampleWorkUnitCellCeiling) &&
             cells.granted == 0,
         cells.message + " Cell ceiling must not be reported as a RAM byte grant.");

  std::uint64_t product = 7;
  report("R-03-checked-mul-overflow",
         !checkedMulU64(~std::uint64_t{0}, 2, product) && product == 0,
         "checkedMulU64 overflow must write 0 and return false");
  report("R-03-checked-mul-raw-head",
         checkedMulU64(kWindows, kHeadBytes, product) && product == kRequiredRawBytes,
         "2044 * 196608 must remain 401866752 without wrap");
}

void testR01() {
  ss::Plan plan;
  ss::Failure failure;
  const bool accepted = callPlan(kSampleWorkUnitCellCeiling, kSixGiB, plan, failure);
  const auto described = planFailure(accepted, failure);
  const bool invalidContext =
      !accepted && failure.detail.contains("Invalid sample context/configuration/budgets");
  // A 6 GiB grant at the cell ceiling is a legal plan. Size alone must not reject it.
  report("R-01", accepted && !invalidContext,
         std::string("6 GiB grant with workingCellBudget at the cell ceiling must be "
                     "accepted and must not fail with Invalid sample "
                     "context/configuration/budgets solely because the grant exceeds "
                     "1 GiB. ") +
             described);

  ss::Failure ceilingFailure;
  const bool ceilingOk =
      callPlan(kSampleWorkUnitCellCeiling, 96ull << 20, plan, ceilingFailure);
  report("R-01-cell-ceiling-allowed", ceilingOk,
         std::string("workingCellBudget equal to 256*1024*1024 must be allowed with a "
                     "96 MiB grant. ") +
             planFailure(ceilingOk, ceilingFailure));

  ss::Failure overflowFailure;
  const bool overflowOk =
      callPlan(kSampleWorkUnitCellCeiling + 1, 96ull << 20, plan, overflowFailure);
  report("R-01-cell-ceiling-plus-one", !overflowOk,
         std::string("workingCellBudget 256*1024*1024+1 must be rejected. ") +
             planFailure(overflowOk, overflowFailure));
}

DetectionSourcePlan widebandPlan() {
  DetectionSourcePlan plan;
  plan.eligibleWindowCount = kWindows;
  plan.modelHeadBytesPerWindow = kHeadBytes;
  plan.estimatedWindowEvidenceBytes = 64;
  return plan;
}

DetectionRequest widebandRequest() {
  DetectionRequest request;
  request.retainRawModelOutput = false;
  request.rawHeadBudgetBytes = 0;
  request.legacyCombinedEvidence = false;
  request.evidenceBudgetBytes = 64ull << 20;
  return request;
}

void testRawHeadPreflight() {
  const auto plan = widebandPlan();
  const QString directory = QStringLiteral("ss-de-rpo-preflight");

  auto omitted = widebandRequest();
  const auto omittedError = applyDetectionEvidencePreflight(directory, plan, omitted);
  report("R-04",
         omittedError.isEmpty() && omitted.rawHeadBudgetBytes == 0 &&
             !omitted.retainRawModelOutput && !omitted.legacyCombinedEvidence,
         std::string("retain false + rawHeadBudgetBytes 0 + 2044 windows must not fail "
                     "raw-head preflight. error=") +
             omittedError.toStdString());

  auto explicitQuota = widebandRequest();
  explicitQuota.rawHeadBudgetBytes = kExplicitRawLimit;
  const auto explicitError =
      applyDetectionEvidencePreflight(directory, plan, explicitQuota);
  const QString limitPhrase =
      QStringLiteral("limit %1 bytes").arg(explicitQuota.rawHeadBudgetBytes);
  report("R-04-explicit-256MiB",
         !explicitError.isEmpty() && explicitError.contains(QStringLiteral("268435456")) &&
             explicitError.contains(limitPhrase) &&
             explicitError.contains(QStringLiteral("before inference")) &&
             explicitError.contains(QStringLiteral("2044 windows x 196608 bytes")),
         std::string("retain false + rawHeadBudgetBytes 256 MiB must still fail "
                     "mentioning limit 268435456. error=") +
             explicitError.toStdString());

  setDetectionVolumeFreeBytesProbeForTest(
      [](const QString&) -> std::optional<std::uint64_t> {
        return std::uint64_t{1024} * 1024;
      });
  const QScopeGuard clearProbe(
      [] { setDetectionVolumeFreeBytesProbeForTest({}); });
  auto retain = widebandRequest();
  retain.retainRawModelOutput = true;
  retain.rawHeadBudgetBytes = 0;
  const auto retainError = applyDetectionEvidencePreflight(directory, plan, retain);
  const bool mentionsRequired = retainError.contains(QStringLiteral("401866752"));
  const bool mentionsProduct = retainError.contains(QStringLiteral("2044")) &&
                               retainError.contains(QStringLiteral("196608"));
  report("R-05",
         !retainError.isEmpty() && retain.rawHeadBudgetBytes == 0 &&
             (mentionsRequired || mentionsProduct) &&
             retainError.contains(QStringLiteral("bytes remain")),
         std::string("retain true + short volume probe must fail mentioning required "
                     "401866752 or the product 2044*196608. error=") +
             retainError.toStdString());
}

void testBatchSelector() {
  const auto unknown = selectDetectionExecutionBatch(16, 2, 16, 1024, 0, 0);
  report("R-15-unknown-vram", unknown == 2,
         "unknown VRAM keeps the worker ceiling 2 and does not jump to 16");
  const auto profile = selectDetectionExecutionBatch(16, 16, 4, 1024, 0, 0);
  report("R-15-profile-cap", profile == 4,
         "profile maximum 4 clamps a request of 16 when VRAM is unknown");
  const auto fits = selectDetectionExecutionBatch(16, 16, 16, 1000, 8000, 0);
  report("R-15-vram-fit", fits == 8,
         "8000 bytes free with 1000 bytes per unit selects batch 8, not 16");
  const auto tight = selectDetectionExecutionBatch(16, 16, 16, 5000, 4000, 0);
  report("R-15-batch1-rejected", tight == 0,
         "batch 1 that does not fit known VRAM returns 0");
  const auto noProfile = selectDetectionExecutionBatch(2, 2, 0, 0, 0, 0);
  report("R-15-no-profile", noProfile == 0, "profile maximum 0 returns 0");
}

void testR12() {
  const QJsonObject evidence{{"lastInference", QJsonObject{{"h2dMs", 1.5}}}};

  DetectionRequest plain;
  plain.retainRawModelOutput = false;
  plain.rawHeadBudgetBytes = 0;
  plain.legacyCombinedEvidence = false;
  AnnotationSourceSet hidden;
  hidden.id = "source";
  preserveDurableModelEvidence(hidden, plain, evidence);
  report("R-12-nondurable",
         !detectionModelEvidenceIsDurable(plain) && hidden.id == "source" &&
             hidden.supplementalOriginalText.empty() && hidden.supplementalHash.empty(),
         "retainRawModelOutput false, rawHeadBudgetBytes 0, legacyCombinedEvidence "
         "false must not copy a fake lastInference object into the source set");

  DetectionRequest retained = plain;
  retained.rawHeadBudgetBytes = 4096;
  AnnotationSourceSet kept;
  kept.id = "source";
  preserveDurableModelEvidence(kept, retained, evidence);
  report("R-12-raw-head-budget",
         detectionModelEvidenceIsDurable(retained) &&
             !kept.supplementalOriginalText.empty() &&
             kept.supplementalOriginalText.find("lastInference") != std::string::npos &&
             kept.supplementalHash.size() == 64,
         "rawHeadBudgetBytes > 0 keeps the fake lastInference evidence");

  DetectionRequest legacy = plain;
  legacy.legacyCombinedEvidence = true;
  AnnotationSourceSet legacySet;
  legacySet.id = "source";
  preserveDurableModelEvidence(legacySet, legacy, evidence);
  report("R-12-legacy-combined",
         detectionModelEvidenceIsDurable(legacy) &&
             legacySet.supplementalOriginalText == kept.supplementalOriginalText &&
             legacySet.supplementalHash == kept.supplementalHash &&
             legacySet.supplementalHash.size() == 64,
         "legacyCombinedEvidence keeps the same supplemental evidence as a raw-head budget");
}

void requireFixture(bool ok, const QString& why) {
  if (!ok) throw std::runtime_error(why.toStdString());
}

// Drives replaceCurrentDetection. The two Chinese prefixes and the
// encodeSourceSet / encodeAnnotation comparison stay in production code.
void testPublicationRejectPrefixes() {
  constexpr std::uint64_t generation = 7;
  QTemporaryDir temp;
  requireFixture(temp.isValid(), "Temporary directory unavailable");
  const auto source = temp.filePath("file-a.iq");
  QFile iq(source);
  requireFixture(iq.open(QIODevice::WriteOnly), "IQ fixture open failed");
  QByteArray bytes(32768 * 4, '\0');
  for (qsizetype i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<char>((i * 13 + 7) & 0xff);
  requireFixture(iq.write(bytes) == bytes.size() && iq.flush(), "IQ fixture write failed");
  iq.close();

  Project project;
  FileState file;
  file.metadata = {"file-a", "file-a.iq", source.toStdString(), 1e6, 100e6, 32768, false};
  file.metadata.availability = {32768, LoadStatus::Ready, 1,
                                iqSourceFingerprint(source).toStdString(), {}};
  file.view = fullRange(file.metadata);
  file.currentDetectionSlots.emplace();
  project.files.push_back(std::move(file));
  project.activeFileId = "file-a";
  const auto path = temp.filePath("publication-prefix.ssproj");
  QString error;
  requireFixture(ProjectFileCodec::save(path, project, error), "Save: " + error);

  DetectionRun run;
  auto metadata = project.files[0].metadata;
  metadata.sampleFormat.selectedChannel = 0;
  run.request.runId = "ss-de-rpo-publication-prefix";
  run.request.profile = *findDetectionProfile("scn-max-psd-v1");
  run.request.profile.fftSize = 2048;
  run.request.profile.modelFrames = run.request.profile.modelHopFrames = 1;
  run.request.parameters.minHits = 1;
  run.request.parameters.mergeFrequencyHz = 0;
  run.request.sourceIdentity = annotationSourceIdentity(metadata, generation);
  const TimeRange time{0, 32768};
  run.request.requestedRange = {time, {99.6e6, 100.4e6}};
  run.request.contextRange = fullRange(metadata);
  run.request.rangeOrigin = "whole-file";
  run.lifecycle = DetectionLifecycle::Running;
  run.startedAtMilliseconds = 100;
  run.readFrames = run.eligibleWindows = run.completedWindows = 1;
  run.coverage = {{DetectionCoverageKind::Read, run.request.contextRange, "controlled fixture"},
                  {DetectionCoverageKind::Eligible, run.request.requestedRange, ""},
                  {DetectionCoverageKind::Completed, run.request.requestedRange, ""}};
  const auto root = QString::fromStdString(project.assetDirectory);
  DetectionPublication empty;
  requireFixture(recoverDetectionPublication(root, run, empty, error),
                 "Empty projection: " + error);
  FrequencyTrack track;
  track.seedId = "controlled-track";
  track.runId = run.request.runId;
  track.fileId = "file-a";
  track.frequency = track.originalFrequency = track.originalFrequencyEnvelope = {99.95e6, 100.05e6};
  track.nominalSupport = track.actualSupport = time;
  track.scoreMean = track.scoreMax = .9;
  track.hits = track.windowDenominator = 1;
  track.windowCoverage = 1;
  track.windows.push_back({"0", 0, .9, time, time, {{track.frequency, 900, 1100, .9, 250}}});
  DetectionPublication publication;
  requireFixture(projectFrequencyTracks(run.request, empty.sourceSet, {track},
                                        16 * 1024 * 1024, publication, error),
                 "Projection: " + error);
  auto records = encodeDetectionPublication(publication);
  records.append(QJsonObject{{"kind", "frequency-publication-end-v1"},
                             {"count", QString::number(publication.annotations.size())}});
  requireFixture(appendDetectionChunk(root, run, records, {}, error), "Chunk: " + error);
  run.lifecycle = DetectionLifecycle::Completed;
  run.computationPhase = "completed";
  run.finishedAtMilliseconds = 101;
  requireFixture(writeDetectionRun(root, run, error), "Complete manifest: " + error);
  requireFixture(recoverDetectionPublication(root, run, publication, error),
                 "Replay: " + error);
  requireFixture(publication.annotations.size() == 1 && !publication.sourceSet.id.empty(),
                 "Fixture publication must contain one annotation and a source set id");

  DetectionResultTicket ticket;
  requireFixture(DetectionResultService::beginDetection(project, run.request, generation,
                                                        ticket, error),
                 "Begin: " + error);
  project.files[0].detectionRuns.push_back(run);
  requireFixture(ProjectFileCodec::save(path, project, error), "Save pending: " + error);
  const auto sourceId = QString::fromStdString(publication.sourceSet.id);
  const auto annotationId = QString::fromStdString(publication.annotations.front().id);
  requireFixture(!sourceId.isEmpty() && !annotationId.isEmpty() &&
                     !sourceId.contains(QLatin1Char(' ')) &&
                     !annotationId.contains(QLatin1Char(' ')),
                 "Fixture ids must be non-empty and free of spaces so the log token matches");

  const QString countPrefix =
      QStringLiteral("持久检测来源/对象数与 publication 不同");
  const QString objectPrefix = QStringLiteral("持久检测对象与 publication 不同");
  AnnotationService annotations;

  auto counted = publication;
  auto extra = counted.annotations.front();
  extra.id += "-x";
  requireFixture(extra.id.size() <= 128 && extra.id != counted.annotations.front().id,
                 "Extra annotation id is not a valid distinct id");
  counted.annotations.push_back(std::move(extra));
  const auto countResult = DetectionResultService::replaceCurrentDetection(
      project, annotations, ticket, generation, run, counted, path, {});
  const auto* slotAfterCount = currentDetectionSlot(project.files[0], 0);
  const bool countPrefixOk = !countResult.committed && countResult.error.startsWith(countPrefix);
  const bool countSuffixOk =
      countResult.error.contains(QStringLiteral(" recoveredAnnotations=1")) &&
      countResult.error.contains(QStringLiteral(" publicationAnnotations=2")) &&
      countResult.error.contains(QStringLiteral(" recoveredSourceSetId=") + sourceId) &&
      countResult.error.contains(QStringLiteral(" publicationSourceSetId=") + sourceId);
  report("publication-count-prefix",
         countPrefixOk && countSuffixOk && slotAfterCount &&
             slotAfterCount->pendingRunId == run.request.runId &&
             slotAfterCount->committedRunId.empty() &&
             !countResult.error.contains(QStringLiteral("firstDifferingAnnotationIndex=")),
         std::string("count mismatch must start with the source/count prefix and name "
                     "both counts and both source set ids. error=") +
             countResult.error.toStdString());

  auto changed = publication;
  changed.annotations.front().note = "ss-de-rpo-object-mismatch";
  requireFixture(encodeAnnotation(changed.annotations.front()) !=
                     encodeAnnotation(publication.annotations.front()),
                 "Note change did not change encodeAnnotation");
  const auto objectResult = DetectionResultService::replaceCurrentDetection(
      project, annotations, ticket, generation, run, changed, path, {});
  const auto* slotAfterObject = currentDetectionSlot(project.files[0], 0);
  const bool objectPrefixOk =
      !objectResult.committed && objectResult.error.startsWith(objectPrefix);
  const bool objectSuffixOk =
      objectResult.error.contains(QStringLiteral(" recoveredAnnotations=1")) &&
      objectResult.error.contains(QStringLiteral(" publicationAnnotations=1")) &&
      objectResult.error.contains(QStringLiteral(" recoveredSourceSetId=") + sourceId) &&
      objectResult.error.contains(QStringLiteral(" publicationSourceSetId=") + sourceId) &&
      objectResult.error.contains(QStringLiteral(" firstDifferingAnnotationIndex=0")) &&
      objectResult.error.contains(QStringLiteral(" annotationId=") + annotationId);
  report("publication-object-prefix",
         objectPrefixOk && objectSuffixOk && slotAfterObject &&
             slotAfterObject->pendingRunId == run.request.runId &&
             slotAfterObject->committedRunId.empty(),
         std::string("same-count annotation mismatch must start with the object prefix "
                     "and name counts, source set ids, index, and annotation id. error=") +
             objectResult.error.toStdString());
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  std::cout << "ss_de_rpo_logic_tests baseline=" << kBaseline << '\n';
  try {
    testResourceDecisions();
    testR01();
    testRawHeadPreflight();
    testBatchSelector();
    testR12();
    testPublicationRejectPrefixes();
  } catch (const std::exception& error) {
    std::cout << "FAIL uncaught " << error.what() << '\n';
    return 1;
  }
  std::cout << (gFailures ? "FAIL" : "PASS") << " ss_de_rpo_logic_tests failures="
            << gFailures << '\n';
  return gFailures == 0 ? 0 : 1;
}
