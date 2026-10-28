// SS-DE-RPO role B. Planning only: makePlan must not read IQ or allocate the
// sample buffer. Grants above 1 GiB stay legal. The contiguous cap and the
// 256 Mi cell ceiling stay separate. A block shrinks only when the preferred
// block misses the grant; the minimum block that still misses fails closed.
#include <QCoreApplication>
#include <QString>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

#include "infrastructure/detection_sample_stream.h"

using namespace signalstudio;
namespace ss = signalstudio::sample_stream;

namespace {
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

std::string described(bool ok, const ss::Failure& failure) {
  return std::string("ok=") + (ok ? "true" : "false") +
         " code=" + std::to_string(static_cast<int>(failure.code)) +
         " stage=" + failure.stage.toStdString() +
         " detail=" + failure.detail.toStdString();
}

std::uint64_t workingSetBytes() {
  PROCESS_MEMORY_COUNTERS_EX usage{};
  usage.cb = sizeof(usage);
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&usage),
                            sizeof(usage)))
    return 0;
  return static_cast<std::uint64_t>(usage.WorkingSetSize);
}

bool powerOfTwo(std::uint32_t value) {
  return value >= 2 && (value & (value - 1)) == 0;
}

ss::Context makeContext(SampleIndex count, double fs, std::size_t cells) {
  ss::Context context;
  context.input.candidateId = "ss-de-rpo-b";
  context.input.sampleRateHz = fs;
  context.input.observation = {{0, count}, {-fs * 0.12, fs * 0.12}};
  context.input.publishBounds = context.input.observation;
  context.searchSamples = {0, count};
  context.availableSamples = count;
  context.bandwidthHz = fs * 0.24;
  context.relativeCenterHz = 0;
  context.parameters.timeStrategy = DetectionTimeStrategy::Sample;
  context.parameters.smoothSeconds = 0.001;
  context.parameters.minOnSeconds = 0.005;
  context.parameters.minOffSeconds = 0.010;
  context.options.workingCellBudget = cells;
  return context;
}

ss::Config makeConfig(ss::Mode mode, std::uint32_t block, std::uint64_t scratch) {
  ss::Config config;
  config.mode = mode;
  config.outputBlockSamples = block;
  config.scratchBudgetBytes = scratch;
  config.scratchFreeFloorBytes = 0;
  config.firTailAmplitudeBudget = 8e-5;
  return config;
}

ss::Runtime makeRuntime(std::uint64_t grant, std::uint64_t result, std::uint64_t reader,
                        std::uint32_t sourceBlock) {
  ss::Runtime runtime;
  runtime.memoryGrantBytes = grant;
  runtime.resultByteGrant = result;
  runtime.readerResidentReserveBytes = reader;
  runtime.sourceReadBlockSamples = sourceBlock;
  return runtime;
}

bool callPlan(SampleIndex count, double fs, std::size_t cells, ss::Mode mode,
              std::uint32_t block, std::uint64_t scratch, std::uint64_t grant,
              std::uint64_t result, std::uint64_t reader, ss::Plan& plan, ss::Failure& failure,
              int* polls = nullptr) {
  auto context = makeContext(count, fs, cells);
  if (polls) {
    context.options.cancelled = [polls] {
      ++*polls;
      return false;
    };
  }
  const auto config = makeConfig(mode, block, scratch);
  const auto runtime = makeRuntime(grant, result, reader, 257);
  return ss::makePlan(context, config, runtime, plan, failure);
}

void testLargeGrants() {
  constexpr std::uint64_t kResult = 16ull << 20;
  const auto started = std::chrono::steady_clock::now();
  for (int shift = 0; shift <= 4; ++shift) {
    const auto grant = 1ull << (30 + shift);
    const auto label = std::to_string(1 << shift) + "GiB";
    for (const auto mode : {ss::Mode::LegacyExact, ss::Mode::StreamingFir}) {
      ss::Plan plan;
      ss::Failure failure;
      const bool ok = callPlan(1000, 1000, 2 * 1024 * 1024, mode, 257, 16ull << 20, grant,
                               kResult, 0, plan, failure);
      const bool streaming = mode == ss::Mode::StreamingFir;
      const bool shaped = ok && failure.code == ss::ErrorCode::None &&
                          !failure.detail.contains("Invalid sample context/configuration/budgets") &&
                          plan.legacyEligible && plan.estimatedResidentBytes > 0 &&
                          plan.estimatedResidentBytes <= grant &&
                          plan.estimatedResidentBytes <= kSampleContiguousAllocationCapBytes &&
                          plan.outputBlockSamples > 0 && plan.outputBlockSamples <= 257 &&
                          (!streaming || powerOfTwo(plan.fftSamples));
      report(streaming ? "B-grant-stream" : "B-grant-legacy", shaped,
             label + " " + described(ok, failure) +
                 " resident=" + std::to_string(plan.estimatedResidentBytes) +
                 " block=" + std::to_string(plan.outputBlockSamples));
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  report("B-grant-time", elapsed.count() < 2000,
         "elapsedMs=" + std::to_string(elapsed.count()));
}

void testContiguousCapIsNotTheGrant() {
  constexpr SampleIndex kCount = 100000000;
  constexpr std::uint64_t kGrant = 16ull << 30;
  constexpr std::uint64_t kResult = 16ull << 20;
  constexpr std::uint64_t kScratch = 8ull * kCount + 8192;
  ss::Plan legacy;
  ss::Failure legacyFailure;
  const bool legacyOk =
      callPlan(kCount, 1000, kSampleWorkUnitCellCeiling, ss::Mode::LegacyExact, 131072, kScratch,
               kGrant, kResult, 0, legacy, legacyFailure);
  report("B-cap-legacy",
         legacyOk && !legacy.legacyEligible && legacy.count == kCount &&
             legacy.count <= kSampleWorkUnitCellCeiling &&
             legacy.legacyResidentEstimateBytes > kSampleContiguousAllocationCapBytes &&
             legacy.legacyResidentEstimateBytes < kGrant &&
             !legacyFailure.detail.contains("Invalid sample context/configuration/budgets"),
         described(legacyOk, legacyFailure) +
             " exact=" + std::to_string(legacy.legacyResidentEstimateBytes));

  ss::Plan streamed;
  ss::Failure streamFailure;
  const bool streamOk =
      callPlan(kCount, 1000, kSampleWorkUnitCellCeiling, ss::Mode::StreamingFir, 131072, kScratch,
               kGrant, kResult, 0, streamed, streamFailure);
  report("B-cap-stream",
         streamOk && streamed.scratchReservationBytes == kScratch &&
             streamed.estimatedResidentBytes > 0 &&
             streamed.estimatedResidentBytes <= kGrant && streamed.outputBlockSamples > 0 &&
             streamed.outputBlockSamples <= 131072,
         described(streamOk, streamFailure) +
             " resident=" + std::to_string(streamed.estimatedResidentBytes) +
             " scratch=" + std::to_string(streamed.scratchReservationBytes) +
             " block=" + std::to_string(streamed.outputBlockSamples));
}

void testAdaptedChunkStaysStreaming() {
  constexpr std::uint64_t kGrant = 4ull << 30;
  constexpr std::uint64_t kResult = 2ull << 30;
  ss::Plan streamed;
  ss::Failure failure;
  const bool ok = callPlan(1000, 1000, 2 * 1024 * 1024, ss::Mode::StreamingFir, 257, 16ull << 20,
                            kGrant, kResult, 0, streamed, failure);
  report("B-adapted-stream",
         ok && streamed.estimatedResidentBytes > kSampleContiguousAllocationCapBytes &&
             streamed.estimatedResidentBytes <= kGrant && streamed.outputBlockSamples > 0 &&
             streamed.outputBlockSamples <= 257,
         described(ok, failure) +
             " resident=" + std::to_string(streamed.estimatedResidentBytes));

  ss::Plan legacy;
  ss::Failure legacyFailure;
  const bool legacyOk =
      callPlan(1000, 1000, 2 * 1024 * 1024, ss::Mode::LegacyExact, 257, 16ull << 20, kGrant,
               kResult, 0, legacy, legacyFailure);
  report("B-adapted-legacy-plan",
         legacyOk && !legacy.legacyEligible &&
             !legacyFailure.detail.contains("Invalid sample context/configuration/budgets"),
         described(legacyOk, legacyFailure));
}

void testCellCeiling() {
  ss::Plan plan;
  ss::Failure allowed;
  const bool ok = callPlan(1000, 1000, kSampleWorkUnitCellCeiling, ss::Mode::StreamingFir, 257,
                            16ull << 20, 96ull << 20, 16ull << 20, 0, plan, allowed);
  report("B-ceiling-equal", ok && allowed.code == ss::ErrorCode::None,
         described(ok, allowed));

  ss::Failure rejected;
  const bool overflowOk =
      callPlan(1000, 1000, kSampleWorkUnitCellCeiling + 1, ss::Mode::StreamingFir, 257,
               16ull << 20, 96ull << 20, 16ull << 20, 0, plan, rejected);
  report("B-ceiling-plus-one",
         !overflowOk && rejected.code == ss::ErrorCode::InvalidInput &&
             rejected.detail.contains("sample.work-unit-ceiling") &&
             rejected.detail.contains(
                 QString("requested=%1").arg(qulonglong(kSampleWorkUnitCellCeiling + 1))) &&
             rejected.detail.contains(
                 QString("ceiling=%1").arg(qulonglong(kSampleWorkUnitCellCeiling))),
         described(overflowOk, rejected));
}

void testZeroGrant() {
  ss::Plan plan;
  ss::Failure failure;
  const bool ok = callPlan(1000, 1000, 2 * 1024 * 1024, ss::Mode::StreamingFir, 257, 16ull << 20,
                            0, 0, 0, plan, failure);
  report("B-zero-grant",
         !ok && failure.code == ss::ErrorCode::InvalidInput &&
             failure.stage == QStringLiteral("plan") &&
             failure.detail.contains("sample.invalid-context") &&
             failure.detail.contains("phase=time-refine") &&
             failure.detail.contains("Invalid sample context/configuration/budgets") &&
             !failure.detail.contains("sample.work-unit-ceiling"),
         described(ok, failure));

  ss::Failure both;
  const bool bothOk =
      callPlan(1000, 1000, kSampleWorkUnitCellCeiling + 1, ss::Mode::LegacyExact, 257,
               16ull << 20, 0, 0, 0, plan, both);
  report("B-zero-and-ceiling",
         !bothOk && both.code == ss::ErrorCode::InvalidInput &&
             both.detail.contains("sample.work-unit-ceiling") &&
             !both.detail.contains("sample.invalid-context"),
         described(bothOk, both));
}

void testOverflow() {
  const auto before = workingSetBytes();
  for (const auto mode : {ss::Mode::LegacyExact, ss::Mode::StreamingFir}) {
    ss::Plan plan;
    ss::Failure failure;
    const bool ok =
        callPlan(1000, 1000, 2 * 1024 * 1024, mode, 257, 16ull << 20,
                 std::numeric_limits<std::uint64_t>::max(),
                 std::numeric_limits<std::uint64_t>::max(), 0, plan, failure);
    const auto overflowText = QStringLiteral(
        "resource.arithmetic-overflow phase=time-refine requested=0 available=0");
    report(mode == ss::Mode::StreamingFir ? "B-overflow-stream" : "B-overflow-legacy",
           !ok && failure.code == ss::ErrorCode::InvalidInput &&
               failure.stage == QStringLiteral("plan") && failure.detail == overflowText,
           described(ok, failure));
  }
  const auto after = workingSetBytes();
  const auto delta = after > before ? after - before : 0;
  report("B-overflow-memory", before != 0 && after != 0 && delta < (256ull << 20),
         "before=" + std::to_string(before) + " after=" + std::to_string(after));
}

void testLowMemory() {
  constexpr SampleIndex kCount = 20000;
  constexpr double kFs = 65536;
  constexpr std::uint64_t kLarge = 16ull << 30;
  constexpr std::uint64_t kResult = 4096;
  constexpr std::uint32_t kPreferredBlock = 20000;
  constexpr std::uint32_t kPreferredFft = 32768;
  constexpr std::uint32_t kRadius = 607;
  constexpr std::uint32_t kTaps = 1215;
  constexpr std::uint64_t kPreferredResident = 5906400;
  constexpr std::uint64_t kMinimumResident = 4283392;
  ss::Plan preferred;
  ss::Failure preferredFailure;
  const bool preferredOk =
      callPlan(kCount, kFs, 2 * 1024 * 1024, ss::Mode::StreamingFir, 131072, 16ull << 20, kLarge,
               kResult, 0, preferred, preferredFailure);
  report("B-low-preferred",
         preferredOk && preferred.outputBlockSamples == kPreferredBlock &&
             preferred.fftSamples == kPreferredFft && preferred.radius == kRadius &&
             preferred.taps == kTaps && preferred.estimatedResidentBytes == kPreferredResident,
         described(preferredOk, preferredFailure) +
             " block=" + std::to_string(preferred.outputBlockSamples) +
             " fft=" + std::to_string(preferred.fftSamples) +
             " radius=" + std::to_string(preferred.radius) +
             " taps=" + std::to_string(preferred.taps) +
             " resident=" + std::to_string(preferred.estimatedResidentBytes) +
             " smooth=" + std::to_string(preferred.smoothingSamples) +
             " tail=" + std::to_string(preferred.tailBound));

  ss::Plan adapted;
  ss::Failure adaptedFailure;
  const auto adaptGrant = kPreferredResident - 1;
  const bool adaptedOk =
      callPlan(kCount, kFs, 2 * 1024 * 1024, ss::Mode::StreamingFir, 131072, 16ull << 20,
               adaptGrant, kResult, 0, adapted, adaptedFailure);
  report("B-low-adapt",
         adaptedOk && adapted.outputBlockSamples >= 1 &&
             adapted.outputBlockSamples < preferred.outputBlockSamples &&
             adapted.estimatedResidentBytes <= adaptGrant && powerOfTwo(adapted.fftSamples) &&
             adapted.radius == preferred.radius && adapted.taps == preferred.taps &&
             adapted.tailBound == preferred.tailBound &&
             adapted.smoothingSamples == preferred.smoothingSamples,
         described(adaptedOk, adaptedFailure) +
             " block=" + std::to_string(adapted.outputBlockSamples) +
             " resident=" + std::to_string(adapted.estimatedResidentBytes) +
             " radius=" + std::to_string(adapted.radius));

  ss::Plan minimum;
  ss::Failure minimumFailure;
  const bool minimumOk =
      callPlan(kCount, kFs, 2 * 1024 * 1024, ss::Mode::StreamingFir, 1, 16ull << 20, kLarge,
               kResult, 0, minimum, minimumFailure);
  report("B-low-minimum",
         minimumOk && minimum.outputBlockSamples == 1 &&
             minimum.estimatedResidentBytes == kMinimumResident,
         described(minimumOk, minimumFailure) +
             " resident=" + std::to_string(minimum.estimatedResidentBytes));

  ss::Plan equal;
  ss::Failure equalFailure;
  const bool equalOk =
      callPlan(kCount, kFs, 2 * 1024 * 1024, ss::Mode::StreamingFir, 1, 16ull << 20,
               kMinimumResident, kResult, 0, equal, equalFailure);
  report("B-low-equal",
         equalOk && equal.outputBlockSamples == 1 &&
             equal.estimatedResidentBytes <= kMinimumResident,
         described(equalOk, equalFailure) +
             " resident=" + std::to_string(equal.estimatedResidentBytes));

  ss::Plan rejected;
  ss::Failure rejectedFailure;
  const auto failGrant = kMinimumResident - 1;
  const bool rejectedOk =
      callPlan(kCount, kFs, 2 * 1024 * 1024, ss::Mode::StreamingFir, 131072, 16ull << 20, failGrant,
               kResult, 0, rejected, rejectedFailure);
  report("B-low-minimum-fail",
         !rejectedOk && rejectedFailure.code == ss::ErrorCode::ResourceLimit &&
             rejectedFailure.stage == QStringLiteral("fir-plan") &&
             rejectedFailure.detail.contains("phase=time-refine") &&
             rejectedFailure.detail.contains("sample.chunk-exceeds-grant") &&
             rejectedFailure.detail.contains(
                 QString("requested=%1").arg(qulonglong(kMinimumResident))) &&
             rejectedFailure.detail.contains(QString("available=%1").arg(qulonglong(failGrant))),
         described(rejectedOk, rejectedFailure));
}

void testLongPlan() {
  constexpr SampleIndex kCount = std::uint64_t{1} << 34;
  constexpr std::uint64_t kScratch = 8ull * kCount + 8192;
  constexpr std::uint64_t kGrant = 8ull << 30;
  const auto before = workingSetBytes();
  const auto started = std::chrono::steady_clock::now();
  ss::Plan plan;
  ss::Failure failure;
  const bool ok = callPlan(kCount, 1000, kSampleWorkUnitCellCeiling, ss::Mode::StreamingFir, 4096,
                            kScratch, kGrant, 16ull << 20, 0, plan, failure);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  const auto after = workingSetBytes();
  const auto delta = after > before ? after - before : 0;
  report("B-long-plan",
         ok && !plan.legacyEligible && plan.outputBlockSamples == 4096 &&
             plan.estimatedResidentBytes > 0 && plan.estimatedResidentBytes <= kGrant &&
             plan.scratchReservationBytes == kScratch && elapsed.count() < 2000 && before != 0 &&
             after != 0 && delta < (256ull << 20),
         described(ok, failure) + " elapsedMs=" + std::to_string(elapsed.count()) +
             " resident=" + std::to_string(plan.estimatedResidentBytes) +
             " block=" + std::to_string(plan.outputBlockSamples) +
             " before=" + std::to_string(before) + " after=" + std::to_string(after));
}

void testCancellation() {
  constexpr SampleIndex kCount = std::uint64_t{1} << 34;
  constexpr std::uint64_t kScratch = 8ull * kCount + 8192;
  auto context = makeContext(kCount, 1000, kSampleWorkUnitCellCeiling);
  context.options.cancelled = [] { return true; };
  const auto config = makeConfig(ss::Mode::StreamingFir, 4096, kScratch);
  const auto runtime = makeRuntime(8ull << 30, 16ull << 20, 0, 257);
  ss::Plan plan;
  ss::Failure failure;
  const auto started = std::chrono::steady_clock::now();
  const bool ok = ss::makePlan(context, config, runtime, plan, failure);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  report("B-cancel-immediate",
         !ok && failure.code == ss::ErrorCode::Cancelled &&
             failure.stage == QStringLiteral("plan") &&
             elapsed.count() < 1000 &&
             !failure.detail.contains("Invalid sample context/configuration/budgets"),
         described(ok, failure) + " elapsedMs=" + std::to_string(elapsed.count()));

  int polls = 0;
  auto second = makeContext(1000, 1000, 2 * 1024 * 1024);
  second.options.cancelled = [&] { return ++polls >= 2; };
  ss::Plan secondPlan;
  ss::Failure secondFailure;
  const bool secondOk = ss::makePlan(second, makeConfig(ss::Mode::StreamingFir, 257, 16ull << 20),
                                      makeRuntime(96ull << 20, 16ull << 20, 0, 257), secondPlan,
                                      secondFailure);
  report("B-cancel-second-poll",
         !secondOk && secondFailure.code == ss::ErrorCode::Cancelled &&
             secondFailure.stage == QStringLiteral("plan") && polls >= 2 &&
             !secondFailure.detail.contains("Invalid sample context/configuration/budgets"),
         described(secondOk, secondFailure) + " polls=" + std::to_string(polls));

  int kept = 0;
  ss::Plan keptPlan;
  ss::Failure keptFailure;
  const bool keptOk =
      callPlan(1000, 1000, 2 * 1024 * 1024, ss::Mode::StreamingFir, 257, 16ull << 20, 96ull << 20,
               16ull << 20, 0, keptPlan, keptFailure, &kept);
  report("B-plan-polls", keptOk && kept >= 4 && keptFailure.code == ss::ErrorCode::None,
         described(keptOk, keptFailure) + " polls=" + std::to_string(kept));
}

void testProbe96() {
  constexpr SampleIndex kCount = 1200000000;
  constexpr double kFs = 20000000;
  constexpr std::uint64_t kGrant = 96ull << 20;
  constexpr std::uint64_t kResult = 16ull << 20;
  constexpr std::uint64_t kScratchBudget = 16ull << 30;
  constexpr std::uint64_t kScratch = 9600008192ull;
  ss::Plan legacy;
  ss::Failure legacyFailure;
  const bool legacyOk =
      callPlan(kCount, kFs, 2 * 1024 * 1024, ss::Mode::LegacyExact, 257, kScratchBudget, kGrant,
               kResult, 0, legacy, legacyFailure);
  report("B-96mib-legacy", legacyOk && !legacy.legacyEligible,
         described(legacyOk, legacyFailure));

  ss::Plan streamed;
  ss::Failure streamFailure;
  const bool streamOk =
      callPlan(kCount, kFs, 2 * 1024 * 1024, ss::Mode::StreamingFir, 257, kScratchBudget, kGrant,
               kResult, 0, streamed, streamFailure);
  report("B-96mib-stream",
         streamOk && streamed.scratchReservationBytes == kScratch &&
             streamed.estimatedResidentBytes <= kGrant && streamed.outputBlockSamples == 257,
         described(streamOk, streamFailure) +
             " resident=" + std::to_string(streamed.estimatedResidentBytes) +
             " scratch=" + std::to_string(streamed.scratchReservationBytes) +
             " block=" + std::to_string(streamed.outputBlockSamples));
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  std::cout << "ss_de_rpo_sample_tests\n";
  try {
    testLargeGrants();
    testContiguousCapIsNotTheGrant();
    testAdaptedChunkStaysStreaming();
    testCellCeiling();
    testZeroGrant();
    testOverflow();
    testLowMemory();
    testLongPlan();
    testCancellation();
    testProbe96();
  } catch (const std::exception& error) {
    std::cout << "FAIL uncaught " << error.what() << '\n';
    return 1;
  }
  std::cout << (gFailures ? "FAIL" : "PASS") << " ss_de_rpo_sample_tests failures=" << gFailures
            << '\n';
  return gFailures == 0 ? 0 : 1;
}
