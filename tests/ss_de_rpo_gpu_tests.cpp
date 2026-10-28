// Pure GPU-account helpers. No CUDA, TensorRT, or live forward.
// --live / --adapted remain on detection_model_backend_tests and are not run
// here. Injected allocation failures are labelled injected.
#include "infrastructure/detection_model_backend.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

using namespace signalstudio;

namespace {
int gFailures = 0;

void expect(bool condition, const std::string& name,
            const std::string& detail = {}) {
  if (condition) {
    std::cout << name << " PASS";
    if (!detail.empty()) std::cout << ' ' << detail;
    std::cout << '\n';
    return;
  }
  ++gFailures;
  std::cout << name << " FAIL " << detail << '\n';
}

std::string textOf(const QString& value) { return value.toStdString(); }

DetectionProfileExtent sampleProfile() {
  DetectionProfileExtent profile;
  profile.known = true;
  profile.profileIndex = 1;
  const std::int64_t minimum[4] = {1, 1, 1, 2048};
  const std::int64_t optimum[4] = {2, 1, 10, 65536};
  const std::int64_t maximum[4] = {2, 1, 64, 65536};
  for (int index = 0; index < 4; ++index) {
    profile.minimum[index] = minimum[index];
    profile.optimum[index] = optimum[index];
    profile.maximum[index] = maximum[index];
  }
  return profile;
}

bool sameDims(const QJsonArray& array, const std::int64_t expected[4]) {
  if (array.size() != 4) return false;
  for (int index = 0; index < 4; ++index)
    if (array.at(index).toVariant().toLongLong() != expected[index])
      return false;
  return true;
}

DetectionDevicePlan planWith(
    std::uint32_t requested, std::uint32_t workerLimit,
    std::uint32_t profileMaximum, std::uint64_t bytesPerUnit,
    std::uint64_t freeBytes, std::uint64_t totalBytes,
    std::uint64_t reserveBytes, int cudaIndex,
    const std::function<bool(std::uint32_t, std::uint64_t)>& reject = {},
    const std::function<bool()>& cancelled = {}) {
  return planDetectionDeviceExecution(
      requested, workerLimit, profileMaximum, bytesPerUnit, freeBytes,
      totalBytes, reserveBytes, cudaIndex, sampleProfile(), reject, cancelled);
}
}  // namespace

int main() {
  DetectionModelResourceLimits limits;
  expect(limits.maximumExecutionBatch == 2, "gpu-helper-default-batch",
         "maximumExecutionBatch=" +
             std::to_string(limits.maximumExecutionBatch));
  DetectionModelRuntimeHooks hooks;
  expect(!hooks.failAfterQueuedOutput && !hooks.failDeviceAllocation &&
             !hooks.cancelled,
         "gpu-helper-production-hooks-empty",
         "production hooks must stay empty unless a test sets them");

  std::int64_t copied[4] = {};
  const std::int64_t source[4] = {1, 1, 10, 2048};
  expect(copyDetectionProfileDims(4, source, copied) && copied[0] == 1 &&
             copied[3] == 2048,
         "gpu-helper-profile-dims-copy");
  expect(!copyDetectionProfileDims(3, source, copied) &&
             !copyDetectionProfileDims(4, nullptr, copied) &&
             !copyDetectionProfileDims(4, source, nullptr),
         "gpu-helper-profile-dims-reject-rank");

  expect(nextDetectionWorkerLimitAfterResourceFailure(4, true) == 2 &&
             nextDetectionWorkerLimitAfterResourceFailure(2, true) == 1 &&
             nextDetectionWorkerLimitAfterResourceFailure(1, true) == 0 &&
             nextDetectionWorkerLimitAfterResourceFailure(2, false) == 0,
         "gpu-helper-downshift-step");
  expect(detectionReportedLogicalBatch(true, 2, 1) == 2 &&
             detectionReportedLogicalBatch(false, 2, 1) == 1,
         "gpu-helper-scn-logical-stays-enqueue",
         "SCN physical batch stays 1 while logical batch stays the enqueue count");
  expect(selectDetectionExecutionBatch(2, 2, 1, 100, 100000, 0) == 1,
         "gpu-helper-scn-profile-physical-one");

  const auto capped = planWith(16, limits.maximumExecutionBatch, 16, 10,
                               1'000'000'000ULL, 2'000'000'000ULL, 7, 2);
  expect(capped.ok && capped.account.physicalBatch == 2 &&
             capped.account.logicalBatch == 2 &&
             capped.account.requestedBatch == 16 &&
             capped.account.cudaDeviceIndex == 2,
         "gpu-helper-worker-ceiling-stays-2",
         textOf(capped.error));

  const auto unknown = planWith(2, 2, 16, 10, 0, 999, 7, 0);
  expect(unknown.ok && unknown.account.physicalBatch == 2 &&
             unknown.account.freeDeviceBytes == 0 &&
             unknown.account.totalDeviceBytes == 999 &&
             unknown.account.reservedDeviceBytes == 7 &&
             unknown.account.neededDeviceBytes == 27 &&
             !unknown.account.injected,
         "gpu-helper-unknown-vram-records-bytes",
         "free 0 is unknown and is still recorded");

  QJsonObject evidence;
  writeDetectionDeviceEvidence(evidence, unknown.account, unknown.profile);
  const auto minimum = evidence.value(QStringLiteral("profileMin")).toArray();
  const auto optimum = evidence.value(QStringLiteral("profileOpt")).toArray();
  const auto maximum = evidence.value(QStringLiteral("profileMax")).toArray();
  expect(evidence.value(QStringLiteral("cudaDeviceIndex")).toInt() == 0 &&
             evidence.value(QStringLiteral("deviceIndex")).toInt() == 0 &&
             evidence.value(QStringLiteral("optimizationProfile")).toInt() ==
                 1 &&
             evidence.value(QStringLiteral("requestedBatch")).toInt() == 2 &&
             evidence.value(QStringLiteral("requestedMaximumBatch")).toInt() ==
                 2 &&
             evidence.value(QStringLiteral("logicalBatch")).toInt() == 2 &&
             evidence.value(QStringLiteral("logicalMaximumBatch")).toInt() ==
                 2 &&
             evidence.value(QStringLiteral("physicalBatch")).toInt() == 2 &&
             evidence.value(QStringLiteral("freeDeviceBytes")).toString() ==
                 QStringLiteral("0") &&
             evidence.value(QStringLiteral("totalDeviceBytes")).toString() ==
                 QStringLiteral("999") &&
             evidence.value(QStringLiteral("reservedDeviceBytes")).toString() ==
                 QStringLiteral("7") &&
             evidence.value(QStringLiteral("neededDeviceBytes")).toString() ==
                 QStringLiteral("27") &&
             sameDims(minimum, unknown.profile.minimum) &&
             sameDims(optimum, unknown.profile.optimum) &&
             sameDims(maximum, unknown.profile.maximum) &&
             !evidence.contains(QStringLiteral("allocationFailureKind")),
         "gpu-helper-evidence-fields",
         QJsonDocument(evidence).toJson(QJsonDocument::Compact).toStdString());

  const auto vram = planWith(2, 2, 16, 5000, 6000, 8000, 0, 1);
  expect(vram.ok && vram.account.physicalBatch == 1 &&
             vram.account.requestedBatch == 2 && !vram.account.injected &&
             vram.attemptedBatches.size() == 1 &&
             vram.attemptedBatches[0] == 1,
         "gpu-helper-vram-selects-batch1", textOf(vram.error));

  const auto unfit = planWith(2, 2, 16, 5000, 4000, 8000, 0, 1);
  expect(!unfit.ok && unfit.account.physicalBatch == 0 &&
             !unfit.account.injected &&
             unfit.error.contains(QStringLiteral("needed 5000 bytes")) &&
             unfit.error.contains(QStringLiteral("free 4000 bytes")) &&
             unfit.error.contains(QStringLiteral("no window is skipped")) &&
             !unfit.error.contains(QStringLiteral("Injected")) &&
             !unfit.error.contains(QStringLiteral("cudaErrorMemoryAllocation")),
         "gpu-helper-batch1-unfit", textOf(unfit.error));

  const auto downshift = planWith(
      2, 2, 16, 100, 100000, 200000, 50, 3,
      [](std::uint32_t batch, std::uint64_t) { return batch == 2; });
  QJsonObject downshiftEvidence;
  writeDetectionDeviceEvidence(downshiftEvidence, downshift.account,
                               downshift.profile);
  expect(downshift.ok && downshift.account.physicalBatch == 1 &&
             downshift.account.logicalBatch == 1 &&
             downshift.account.requestedBatch == 2 &&
             downshift.account.neededDeviceBytes == 150 &&
             downshift.account.injected && downshift.attemptedBatches.size() == 2 &&
             downshift.attemptedBatches[0] == 2 &&
             downshift.attemptedBatches[1] == 1 &&
             detectionAdaptationWasInjected(downshift.adaptationReason) &&
             downshift.adaptationReason.contains(
                 QStringLiteral("same windows")) &&
             !downshift.adaptationReason.contains(
                 QStringLiteral("cudaErrorMemoryAllocation")) &&
             downshiftEvidence.value(QStringLiteral("allocationFailureKind"))
                     .toString() == QStringLiteral("injected") &&
             downshiftEvidence.value(QStringLiteral("cudaDeviceIndex")).toInt() ==
                 3 &&
             downshiftEvidence.value(QStringLiteral("physicalBatch")).toInt() ==
                 1,
         "gpu-helper-injected-downshift",
         textOf(downshift.adaptationReason));

  const auto rejected = planWith(
      2, 2, 16, 100, 100000, 200000, 50, 3,
      [](std::uint32_t, std::uint64_t) { return true; });
  expect(!rejected.ok && rejected.account.physicalBatch == 0 &&
             rejected.account.logicalBatch == 0 && rejected.account.injected &&
             rejected.attemptedBatches.size() == 2 &&
             rejected.attemptedBatches[0] == 2 &&
             rejected.attemptedBatches[1] == 1 &&
             rejected.error.contains(
                 QStringLiteral("Injected device allocation failure")) &&
             rejected.error.contains(QStringLiteral("needed 150 bytes")) &&
             rejected.error.contains(QStringLiteral("free 100000 bytes")) &&
             rejected.error.contains(
                 QStringLiteral("Retrying legal batch 1 for the same windows")) &&
             rejected.error.contains(QStringLiteral("no window is skipped")) &&
             !rejected.error.contains(
                 QStringLiteral("cudaErrorMemoryAllocation")),
         "gpu-helper-injected-batch1-reject", textOf(rejected.error));

  const auto cancelled = planWith(2, 2, 16, 100, 100000, 200000, 50, 0, {},
                                  [] { return true; });
  expect(!cancelled.ok && cancelled.account.physicalBatch == 0 &&
             cancelled.attemptedBatches.empty() && !cancelled.account.injected &&
             cancelled.error == detectionExecutionCancelledMessage(),
         "gpu-helper-cancel-before-allocation", textOf(cancelled.error));

  int cancelChecks = 0;
  const auto cancelLater = planWith(
      2, 2, 16, 100, 100000, 200000, 50, 0,
      [](std::uint32_t batch, std::uint64_t) { return batch == 2; },
      [&] { return ++cancelChecks > 1; });
  expect(!cancelLater.ok && cancelLater.account.physicalBatch == 0 &&
             !cancelLater.account.injected &&
             cancelLater.attemptedBatches.size() == 1 &&
             cancelLater.attemptedBatches[0] == 2 &&
             cancelLater.error == detectionExecutionCancelledMessage(),
         "gpu-helper-cancel-does-not-accept-smaller-batch",
         textOf(cancelLater.error));

  const auto illegal = planWith(3, 2, 16, 10, 1000, 2000, 0, 0);
  expect(!illegal.ok &&
             illegal.error.contains(QStringLiteral("1, 2, 4, 8 or 16")),
         "gpu-helper-illegal-tier", textOf(illegal.error));

  const auto kMax = std::numeric_limits<std::uint64_t>::max();
  const auto overflow = planWith(2, 2, 16, kMax, kMax, kMax, 1, 0);
  expect(!overflow.ok && !overflow.account.injected &&
             overflow.error.contains(QStringLiteral("overflow")) &&
             overflow.error.contains(QStringLiteral("no window is skipped")),
         "gpu-helper-needed-overflow", textOf(overflow.error));

  if (gFailures != 0) {
    std::cerr << gFailures << " gpu helper checks failed\n";
    return 1;
  }
  std::cout << "ss_de_rpo_gpu_tests passed\n";
  return 0;
}
