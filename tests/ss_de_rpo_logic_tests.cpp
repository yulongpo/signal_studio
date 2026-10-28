// SS-DE-RPO logic regressions. Standalone main, same check style as
// tests/detection_task_tests.cpp. R-01 is expected RED on baseline
// 82935ab34ec6126707da152bf46d1f352458ab0c: contextValid rejects
// memoryGrantBytes > 1 GiB with "Invalid sample context/configuration/budgets".
// Do not weaken that assertion to match the bug.
#include <QCoreApplication>
#include <QJsonObject>
#include <QScopeGuard>
#include <QString>

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "infrastructure/detection_sample_stream.h"
#include "infrastructure/detection_source.h"
#include "infrastructure/detection_task.h"

using namespace signalstudio;
namespace ss = signalstudio::sample_stream;

namespace {
constexpr char kBaseline[] = "82935ab34ec6126707da152bf46d1f352458ab0c";
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

void testR01() {
  ss::Plan plan;
  ss::Failure failure;
  const bool accepted = callPlan(kSampleWorkUnitCellCeiling, kSixGiB, plan, failure);
  const auto described = planFailure(accepted, failure);
  const bool invalidContext =
      !accepted && failure.detail.contains("Invalid sample context/configuration/budgets");
  // Desired: a 6 GiB grant at the cell ceiling is a legal plan. Baseline
  // rejects it only because memoryGrantBytes > 1 GiB. That RED is intentional.
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
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  std::cout << "ss_de_rpo_logic_tests baseline=" << kBaseline << '\n';
  try {
    testR01();
    testRawHeadPreflight();
    testR12();
  } catch (const std::exception& error) {
    std::cout << "FAIL uncaught " << error.what() << '\n';
    return 1;
  }
  std::cout << (gFailures ? "FAIL" : "PASS") << " ss_de_rpo_logic_tests failures="
            << gFailures << '\n';
  return gFailures == 0 ? 0 : 1;
}
