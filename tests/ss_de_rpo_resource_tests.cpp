// Runtime resource snapshot and phase-overlap disk ledger.
// Standalone main. Pure budget/probe tests plus Qt JSON diagnostics.
// Does not start a GPU, a worker, or a Qt application.
#include "domain/detection.h"
#include "infrastructure/detection_resource_diagnostics.h"
#include "infrastructure/detection_source.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace signalstudio;

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

bool has(const std::string& text, const char* token) {
  return text.find(token) != std::string::npos;
}

DetectionDiskUsage sampleUsage() {
  DetectionDiskUsage usage;
  usage.durableVolumeKey = "D:\\";
  usage.storeRawHead = true;
  usage.rawHeadBytes = 1000;
  usage.scratchBytes = 5000;
  usage.resultShardBytes = 800;
  usage.largestResultShardBytes = 800;
  usage.publicationShardBytes = 900;
  usage.largestPublicationShardBytes = 900;
  usage.checkpointBytes = 100;
  usage.checkpointExistingBytes = 80;
  usage.projectBytes = 200;
  usage.projectExistingBytes = 150;
  return usage;
}

DiskLedgerEvaluation evalUsage(const DetectionDiskUsage& usage,
                               std::uint64_t available,
                               const std::string& volume = "D:\\",
                               std::uint64_t reserve = 0) {
  std::vector<DiskLedgerLine> lines;
  appendPhaseOverlapDiskLines(usage, lines);
  const DiskVolumeBudget budget{volume, true, available};
  return evaluatePhaseOverlapDiskLedger(lines, std::span<const DiskVolumeBudget>(&budget, 1),
                                        reserve);
}

void testCheckedArithmetic() {
  std::uint64_t out = 7;
  report("arith-mul-ok", checkedMulU64(2044, 196608, out) && out == 401866752ull,
         std::to_string(out));
  out = 7;
  const auto max = std::numeric_limits<std::uint64_t>::max();
  report("arith-mul-overflow", !checkedMulU64(max, 2, out) && out == 0, std::to_string(out));
  out = 7;
  report("arith-add-ok", checkedAddU64(1000, 80, out) && out == 1080, std::to_string(out));
  out = 7;
  report("arith-add-overflow", !checkedAddU64(max, 1, out) && out == 0, std::to_string(out));
}

void testSampleDecisionUnchanged() {
  constexpr auto kSixGiB = 6ull << 30;
  constexpr auto kOneGiB = kSampleContiguousAllocationCapBytes;
  const auto invalid = decideSamplePhaseBudget(0, kOneGiB, kSampleWorkUnitCellCeiling + 1);
  report("sample-zero-auth",
         invalid.kind == ResourceDecisionKind::Rejected &&
             invalid.errorCode == "sample.invalid-context" && invalid.granted == 0 &&
             invalid.resource == "ram" && has(invalid.message, "phase=time-refine") &&
             has(invalid.message, "reject sample.invalid-context"),
         invalid.errorCode);
  const auto accepted =
      decideSamplePhaseBudget(kSixGiB, kOneGiB, kSampleWorkUnitCellCeiling);
  report("sample-6gib-accepted",
         accepted.kind == ResourceDecisionKind::Accepted && accepted.errorCode.empty() &&
             accepted.granted == kOneGiB && accepted.available == kSixGiB &&
             accepted.resource == "ram" &&
             std::string(resourceDecisionUnit(accepted)) == "bytes" &&
             has(accepted.message, "a larger authorization is a ceiling"),
         accepted.message);
  const auto cells = decideSamplePhaseBudget(kSixGiB, 1024, kSampleWorkUnitCellCeiling + 1);
  report("sample-cells",
         cells.kind == ResourceDecisionKind::Rejected &&
             cells.errorCode == "sample.work-unit-ceiling" && cells.resource == "work-unit" &&
             cells.granted == 0 && cells.requested == kSampleWorkUnitCellCeiling + 1 &&
             std::string(resourceDecisionUnit(cells)) == "cells",
         cells.message);
  const auto chunk = decideSamplePhaseBudget(1024, 1025, 10);
  report("sample-chunk",
         chunk.kind == ResourceDecisionKind::Rejected &&
             chunk.errorCode == "sample.chunk-exceeds-grant" && chunk.granted == 0 &&
             chunk.requested == 1025 && chunk.available == 1024,
         chunk.errorCode);
  const auto adapted = decideSamplePhaseBudget(kSixGiB, kOneGiB + 1, 10);
  report("sample-adapted",
         adapted.kind == ResourceDecisionKind::Adapted &&
             adapted.errorCode == "sample.grant-exceeds-contiguous-cap" &&
             adapted.granted == kOneGiB && adapted.available == kSixGiB &&
             has(adapted.message, "planner must chunk"),
         std::to_string(adapted.granted));
}

void testRawHead() {
  RawHeadDiskInput off;
  off.windows = 2044;
  off.bytesPerWindow = 196608;
  const auto omitted = assessRawHeadDisk(off);
  report("raw-off-zero-quota",
         omitted.applies && !omitted.overflow && omitted.requiredBytes == 0 &&
             omitted.decision.kind == ResourceDecisionKind::Accepted &&
             omitted.decision.requested == 0 && omitted.decision.granted == 0 &&
             omitted.decision.errorCode.empty() &&
             has(omitted.decision.message, "not retained") &&
             has(omitted.decision.message, "windows=2044") &&
             has(omitted.decision.message, "perWindow=196608"),
         omitted.decision.message);

  RawHeadDiskInput huge = off;
  huge.windows = std::numeric_limits<std::uint64_t>::max();
  huge.bytesPerWindow = 2;
  const auto hugeOff = assessRawHeadDisk(huge);
  report("raw-off-ignores-overflow",
         hugeOff.applies && !hugeOff.overflow &&
             hugeOff.decision.kind == ResourceDecisionKind::Accepted &&
             hugeOff.decision.granted == 0,
         hugeOff.decision.errorCode);

  RawHeadDiskInput explicitQuota = off;
  explicitQuota.explicitQuotaBytes = 256ull * 1024 * 1024;
  const auto explicitReject = assessRawHeadDisk(explicitQuota);
  report("raw-explicit-256mib",
         explicitReject.applies && explicitReject.requiredBytes == 401866752ull &&
             explicitReject.decision.kind == ResourceDecisionKind::Rejected &&
             explicitReject.decision.errorCode == "resource.explicit-raw-head-quota" &&
             explicitReject.decision.errorCode != "resource.raw-head-shortfall" &&
             explicitReject.decision.requested == 401866752ull &&
             explicitReject.decision.available == 268435456ull &&
             explicitReject.decision.granted == 0,
         explicitReject.decision.message);

  RawHeadDiskInput overCap = off;
  overCap.explicitQuotaBytes = kDetectionExplicitRawHeadMaxBytes + 1;
  const auto cap = assessRawHeadDisk(overCap);
  report("raw-explicit-cap",
         cap.decision.kind == ResourceDecisionKind::Rejected &&
             cap.decision.errorCode == "resource.explicit-raw-head-cap" &&
             cap.decision.available == kDetectionExplicitRawHeadMaxBytes &&
             cap.decision.granted == 0,
         cap.decision.errorCode);

  RawHeadDiskInput retained = off;
  retained.retainRawModelOutput = true;
  retained.volumeKnown = true;
  retained.volumeAvailableBytes = 401866752ull;
  const auto fit = assessRawHeadDisk(retained);
  report("raw-on-exact-volume",
         fit.applies && fit.requiredBytes == 401866752ull &&
             fit.decision.kind == ResourceDecisionKind::Accepted &&
             fit.decision.granted == 401866752ull &&
             fit.decision.requested == 401866752ull &&
             fit.decision.available == 401866752ull && fit.decision.errorCode.empty(),
         fit.decision.message);

  retained.volumeAvailableBytes = 401866751ull;
  const auto shortVolume = assessRawHeadDisk(retained);
  report("raw-on-short-volume",
         shortVolume.decision.kind == ResourceDecisionKind::Rejected &&
             shortVolume.decision.errorCode == "resource.raw-head-shortfall" &&
             shortVolume.decision.requested == 401866752ull &&
             shortVolume.decision.available == 401866751ull &&
             shortVolume.decision.granted == 0 &&
             has(shortVolume.decision.message, "401866752"),
         shortVolume.decision.message);

  retained.volumeKnown = false;
  retained.explicitQuotaBytes = 0;
  const auto unknown = assessRawHeadDisk(retained);
  report("raw-on-unknown-volume",
         unknown.decision.kind == ResourceDecisionKind::Rejected &&
             unknown.decision.errorCode == "resource.raw-head-shortfall" &&
             unknown.decision.available == 0 && unknown.decision.granted == 0 &&
             has(unknown.decision.message, "unavailable"),
         unknown.decision.message);

  retained.volumeKnown = true;
  retained.volumeAvailableBytes = 500000000ull;
  retained.explicitQuotaBytes = 401866751ull;
  const auto shortQuota = assessRawHeadDisk(retained);
  report("raw-on-short-quota-before-volume",
         shortQuota.decision.errorCode == "resource.raw-head-shortfall" &&
             shortQuota.decision.available == 401866751ull &&
             shortQuota.decision.requested == 401866752ull,
         shortQuota.decision.message);

  RawHeadDiskInput overflow = retained;
  overflow.windows = std::numeric_limits<std::uint64_t>::max();
  overflow.bytesPerWindow = 2;
  overflow.explicitQuotaBytes = 0;
  const auto wrapped = assessRawHeadDisk(overflow);
  report("raw-on-overflow",
         wrapped.overflow && wrapped.requiredBytes == 0 &&
             wrapped.decision.kind == ResourceDecisionKind::Rejected &&
             wrapped.decision.errorCode == "resource.arithmetic-overflow" &&
             wrapped.decision.granted == 0 &&
             wrapped.decision.errorCode != "resource.raw-head-shortfall",
         wrapped.decision.message);

  RawHeadDiskInput legacy;
  legacy.legacyCombined = true;
  legacy.retainRawModelOutput = true;
  legacy.windows = 2044;
  legacy.bytesPerWindow = 196608;
  const auto legacyDecision = assessRawHeadDisk(legacy);
  DetectionDiskUsage usage;
  usage.rawHeadBytes = 5;
  report("raw-legacy-unchanged",
         !legacyDecision.applies &&
             legacyDecision.decision.errorCode == "resource.legacy-combined-unchanged" &&
             !applyAcceptedRawHeadToUsage(legacyDecision, usage) && usage.rawHeadBytes == 5,
         legacyDecision.decision.errorCode);

  DetectionDiskUsage charged;
  const bool applied = applyAcceptedRawHeadToUsage(fit, charged);
  report("raw-apply-accepted",
         applied && charged.storeRawHead && charged.rawHeadBytes == 401866752ull,
         std::to_string(charged.rawHeadBytes));
  DetectionDiskUsage notStored;
  notStored.rawHeadBytes = 9;
  const bool omittedApplied = applyAcceptedRawHeadToUsage(omitted, notStored);
  report("raw-apply-omitted",
         omittedApplied && !notStored.storeRawHead && notStored.rawHeadBytes == 0,
         std::to_string(notStored.rawHeadBytes));
}

void testLedgerArithmetic() {
  const auto usage = sampleUsage();
  const auto peak = evalUsage(usage, 6900);
  report("ledger-refine-peak",
         peak.decision.kind == ResourceDecisionKind::Accepted &&
             peak.decision.requested == 6900 && peak.decision.granted == 6900 &&
             peak.decision.available == 6900 && peak.decision.reserve == 0 &&
             peak.peakPhase == "refine" && peak.peakBytes == 6900 &&
             peak.decision.resource == "disk" &&
             std::string(resourceDecisionUnit(peak.decision)) == "bytes" &&
             has(peak.decision.message, "rawHead=1000") &&
             has(peak.decision.message, "scratch=5000") &&
             has(peak.decision.message, "shards=800") &&
             has(peak.decision.message, "checkpoint=100") &&
             has(peak.decision.message, "projectAtomic=0"),
         peak.decision.message);

  const auto tight = evalUsage(usage, 6899);
  report("ledger-one-byte-short",
         tight.decision.kind == ResourceDecisionKind::Rejected &&
             tight.decision.errorCode == "resource.disk-shortfall" &&
             tight.decision.requested == 6900 && tight.decision.granted == 0 &&
             tight.decision.available == 6899 && tight.peakPhase == "refine" &&
             tight.decision.errorCode != "resource.raw-head-shortfall",
         tight.decision.message);

  DetectionDiskUsage projectOnly;
  projectOnly.durableVolumeKey = "D:\\";
  projectOnly.projectBytes = 1000;
  projectOnly.projectExistingBytes = 1000;
  const auto doubled = evalUsage(projectOnly, 1500);
  report("ledger-project-atomic-double",
         doubled.decision.kind == ResourceDecisionKind::Rejected &&
             doubled.decision.errorCode == "resource.disk-shortfall" &&
             doubled.decision.requested == 2000 && doubled.peakPhase == "publishing" &&
             doubled.decision.granted == 0 &&
             has(doubled.decision.message, "projectAtomic=2000"),
         doubled.decision.message);
  const auto projectFits = evalUsage(projectOnly, 2000);
  report("ledger-project-atomic-fits",
         projectFits.decision.kind == ResourceDecisionKind::Accepted &&
             projectFits.decision.granted == 2000 && projectFits.decision.requested == 2000,
         projectFits.decision.message);
  projectOnly.projectExistingBytes = 0;
  const auto firstSave = evalUsage(projectOnly, 1000);
  report("ledger-project-first-save",
         firstSave.decision.kind == ResourceDecisionKind::Accepted &&
             firstSave.decision.requested == 1000 && firstSave.decision.granted == 1000,
         std::to_string(firstSave.decision.requested));
  const auto firstShort = evalUsage(projectOnly, 999);
  report("ledger-project-first-short",
         firstShort.decision.requested == 1000 &&
             firstShort.decision.kind == ResourceDecisionKind::Rejected,
         std::to_string(firstShort.decision.requested));

  DetectionDiskUsage checkpointOnly;
  checkpointOnly.durableVolumeKey = "D:\\";
  checkpointOnly.checkpointBytes = 1000;
  checkpointOnly.checkpointExistingBytes = 1000;
  const auto checkpointPeak = evalUsage(checkpointOnly, 1500);
  report("ledger-checkpoint-atomic-only-while-writing",
         checkpointPeak.decision.kind == ResourceDecisionKind::Rejected &&
             checkpointPeak.peakPhase == "checkpoint" &&
             checkpointPeak.decision.requested == 2000 &&
             has(checkpointPeak.decision.message, "checkpoint=2000") &&
             has(checkpointPeak.decision.message, "projectAtomic=0"),
         checkpointPeak.decision.message);
  const auto checkpointFits = evalUsage(checkpointOnly, 2000);
  report("ledger-checkpoint-fits",
         checkpointFits.decision.kind == ResourceDecisionKind::Accepted &&
             checkpointFits.peakPhase == "checkpoint" &&
             checkpointFits.decision.granted == 2000,
         checkpointFits.decision.message);

  DetectionDiskUsage shard = sampleUsage();
  shard.scratchBytes = 0;
  shard.rawHeadBytes = 0;
  shard.storeRawHead = false;
  shard.largestResultShardBytes = kDetectionShardFileCapBytes + 1;
  shard.resultShardBytes = shard.largestResultShardBytes;
  shard.publicationShardBytes = 0;
  shard.projectBytes = 0;
  shard.checkpointBytes = 0;
  const auto boundary = evalUsage(shard, 1ull << 40);
  report("ledger-shard-boundary",
         boundary.decision.kind == ResourceDecisionKind::Rejected &&
             boundary.decision.errorCode == "resource.shard-boundary" &&
             boundary.decision.resource == "format" &&
             boundary.decision.requested == kDetectionShardFileCapBytes + 1 &&
             boundary.decision.available == kDetectionShardFileCapBytes &&
             boundary.decision.granted == 0 && boundary.peakPhase == "checkpoint" &&
             kDetectionShardFileCapBytes == 8ull * 1024 * 1024,
         boundary.decision.message);

  shard.largestResultShardBytes = kDetectionShardFileCapBytes;
  shard.resultShardBytes = kDetectionShardFileCapBytes;
  const auto exactShard = evalUsage(shard, kDetectionShardFileCapBytes);
  report("ledger-shard-exact-cap",
         exactShard.decision.kind == ResourceDecisionKind::Accepted &&
             exactShard.decision.errorCode != "resource.shard-boundary" &&
             exactShard.decision.granted == kDetectionShardFileCapBytes,
         exactShard.decision.errorCode);

  std::vector<DiskLedgerLine> unsplittable(1);
  unsplittable[0].kind = DiskLedgerKind::Shards;
  unsplittable[0].phase = "publishing";
  unsplittable[0].volumeKey = "D:\\";
  unsplittable[0].payloadBytes = kDetectionShardFileCapBytes + 1;
  const DiskVolumeBudget budget{"D:\\", true, 1ull << 40};
  const auto missingSplit = evaluatePhaseOverlapDiskLedger(
      unsplittable, std::span<const DiskVolumeBudget>(&budget, 1), 0);
  report("ledger-shard-unsplit",
         missingSplit.decision.errorCode == "resource.shard-boundary" &&
             missingSplit.decision.requested == kDetectionShardFileCapBytes + 1 &&
             missingSplit.decision.granted == 0,
         missingSplit.decision.message);

  DetectionDiskUsage heads;
  heads.durableVolumeKey = "D:\\";
  heads.storeRawHead = true;
  heads.rawHeadBytes = 5000;
  const auto headShort = evalUsage(heads, 4000);
  report("ledger-raw-head-itself",
         headShort.decision.kind == ResourceDecisionKind::Rejected &&
             headShort.decision.errorCode == "resource.raw-head-shortfall" &&
             headShort.decision.requested == 5000 && headShort.decision.available == 4000 &&
             headShort.decision.granted == 0 && has(headShort.decision.message, "rawHead=5000"),
         headShort.decision.message);

  DetectionDiskUsage mixed = heads;
  mixed.scratchBytes = 5000;
  const auto scratchBlamed = evalUsage(mixed, 5500);
  report("ledger-scratch-not-blamed-on-raw-head",
         scratchBlamed.decision.errorCode == "resource.disk-shortfall" &&
             scratchBlamed.decision.requested == 10000 &&
             scratchBlamed.peakPhase == "refine" && scratchBlamed.decision.granted == 0 &&
             has(scratchBlamed.decision.message, "scratch=5000") &&
             has(scratchBlamed.decision.message, "rawHead=5000"),
         scratchBlamed.decision.message);

  DetectionDiskUsage off = sampleUsage();
  off.storeRawHead = false;
  off.rawHeadBytes = 401866752ull;
  off.scratchBytes = 0;
  off.resultShardBytes = 0;
  off.publicationShardBytes = 0;
  off.checkpointBytes = 0;
  off.projectBytes = 0;
  std::vector<DiskLedgerLine> offLines;
  appendPhaseOverlapDiskLines(off, offLines);
  const auto omittedHead = evaluatePhaseOverlapDiskLedger(offLines, {}, 0);
  report("ledger-raw-head-off-not-charged",
         offLines.empty() && omittedHead.decision.kind == ResourceDecisionKind::Accepted &&
             omittedHead.decision.requested == 0 && omittedHead.decision.granted == 0 &&
             omittedHead.peakBytes == 0,
         std::to_string(offLines.size()));

  DetectionDiskUsage split = sampleUsage();
  split.scratchVolumeKey = "S:\\";
  std::vector<DiskLedgerLine> splitLines;
  appendPhaseOverlapDiskLines(split, splitLines);
  const DiskVolumeBudget volumes[] = {{"D:\\", true, 100000}, {"S:\\", true, 4000}};
  const auto splitEval = evaluatePhaseOverlapDiskLedger(splitLines, volumes, 0);
  report("ledger-scratch-volume-separate",
         splitEval.decision.kind == ResourceDecisionKind::Rejected &&
             splitEval.decision.errorCode == "resource.disk-shortfall" &&
             splitEval.peakVolumeKey == "S:\\" && splitEval.decision.requested == 5000 &&
             splitEval.peakPhase == "refine" && splitEval.perVolume.size() == 2,
         splitEval.peakVolumeKey + " " + std::to_string(splitEval.decision.requested));

  const auto reserve = evalUsage(sampleUsage(), 100, "D:\\", 101);
  report("ledger-reserve",
         reserve.decision.kind == ResourceDecisionKind::Rejected &&
             reserve.decision.errorCode == "resource.reserve-exceeds-available" &&
             reserve.decision.reserve == 101 && reserve.decision.available == 100 &&
             reserve.decision.granted == 0,
         reserve.decision.message);

  std::vector<DiskLedgerLine> missing(1);
  missing[0].kind = DiskLedgerKind::Scratch;
  missing[0].phase = "refine";
  missing[0].volumeKey = "missing";
  missing[0].payloadBytes = 10;
  const auto unknown = evaluatePhaseOverlapDiskLedger(missing, {}, 0);
  report("ledger-unknown-volume",
         unknown.decision.kind == ResourceDecisionKind::Rejected &&
             unknown.decision.errorCode == "resource.volume-unavailable" &&
             unknown.decision.requested == 10 && unknown.decision.available == 0 &&
             unknown.decision.granted == 0,
         unknown.decision.errorCode);

  const auto max = std::numeric_limits<std::uint64_t>::max();
  std::vector<DiskLedgerLine> overflow(2);
  overflow[0].kind = DiskLedgerKind::Scratch;
  overflow[0].phase = "refine";
  overflow[0].volumeKey = "D:\\";
  overflow[0].payloadBytes = max;
  overflow[1] = overflow[0];
  overflow[1].kind = DiskLedgerKind::Shards;
  overflow[1].payloadBytes = 1;
  overflow[1].largestUnitBytes = 1;
  const DiskVolumeBudget room{"D:\\", true, max};
  const auto sumOverflow = evaluatePhaseOverlapDiskLedger(
      overflow, std::span<const DiskVolumeBudget>(&room, 1), 0);
  report("ledger-sum-overflow",
         sumOverflow.decision.kind == ResourceDecisionKind::Rejected &&
             sumOverflow.decision.errorCode == "resource.arithmetic-overflow" &&
             sumOverflow.decision.granted == 0 && sumOverflow.decision.requested == 0,
         sumOverflow.decision.errorCode);

  std::vector<DiskLedgerLine> atomic(1);
  atomic[0].kind = DiskLedgerKind::ProjectAtomic;
  atomic[0].phase = "publishing";
  atomic[0].volumeKey = "D:\\";
  atomic[0].payloadBytes = max;
  atomic[0].existingBytes = 1;
  atomic[0].atomicReplace = true;
  const auto atomicOverflow = evaluatePhaseOverlapDiskLedger(
      atomic, std::span<const DiskVolumeBudget>(&room, 1), 0);
  report("ledger-atomic-overflow",
         atomicOverflow.decision.errorCode == "resource.arithmetic-overflow" &&
             atomicOverflow.decision.granted == 0,
         atomicOverflow.decision.message);
}

void testSnapshotAndResident() {
  const auto first = captureRuntimeResourceSnapshot(".", nullptr);
  const auto second = captureRuntimeResourceSnapshot(".", nullptr);
  report("snapshot-vram-not-requested",
         first.vram.status == ResourceProbeStatus::NotRequested &&
             first.vram.totalBytes == 0 && first.vram.availableBytes == 0 &&
             first.vram.detail == "not-requested",
         first.vram.detail);
  const bool ramHonest =
      (first.ram.status == ResourceProbeStatus::Probed && first.ram.totalBytes > 0 &&
       first.ram.availableBytes <= first.ram.totalBytes && !first.ram.detail.empty()) ||
      (first.ram.status == ResourceProbeStatus::Unavailable && first.ram.availableBytes == 0 &&
       first.ram.totalBytes == 0 && !first.ram.detail.empty());
  report("snapshot-ram-honest", ramHonest,
         std::to_string(static_cast<int>(first.ram.status)) + " total=" +
             std::to_string(first.ram.totalBytes) +
             " available=" + std::to_string(first.ram.availableBytes));
  const bool volumeHonest =
      (first.volume.status == ResourceProbeStatus::Probed && !first.volume.volumeKey.empty() &&
       first.volume.availableBytes <= first.volume.totalBytes) ||
      (first.volume.status == ResourceProbeStatus::Unavailable &&
       first.volume.availableBytes == 0 && !first.volume.detail.empty());
  report("snapshot-volume-honest", volumeHonest,
         first.volume.volumeKey + " " + first.volume.detail + " available=" +
             std::to_string(first.volume.availableBytes));
  report("snapshot-clock-monotonic",
         first.capturedAtSteadyNs > 0 && second.capturedAtSteadyNs >= first.capturedAtSteadyNs,
         std::to_string(first.capturedAtSteadyNs));

  VramProbeInput injected;
  injected.probed = true;
  injected.totalBytes = 8000;
  injected.availableBytes = 3000;
  injected.deviceIdentity = "injected-gpu-0";
  const auto withVram = captureRuntimeResourceSnapshot(".", &injected);
  report("snapshot-vram-injected",
         withVram.vram.status == ResourceProbeStatus::Probed &&
             withVram.vram.totalBytes == 8000 && withVram.vram.availableBytes == 3000 &&
             withVram.vram.detail == "injected-gpu-0",
         withVram.vram.detail);
  VramProbeInput bad = injected;
  bad.availableBytes = 8001;
  const auto inconsistent = captureRuntimeResourceSnapshot(".", &bad);
  report("snapshot-vram-inconsistent",
         inconsistent.vram.status == ResourceProbeStatus::Unavailable &&
             inconsistent.vram.totalBytes == 0 && inconsistent.vram.availableBytes == 0 &&
             inconsistent.vram.detail == "vram-probe-inconsistent",
         inconsistent.vram.detail);
  VramProbeInput failed;
  const auto noVram = captureRuntimeResourceSnapshot(".", &failed);
  report("snapshot-vram-failed",
         noVram.vram.status == ResourceProbeStatus::Unavailable &&
             noVram.vram.totalBytes == 0 && noVram.vram.availableBytes == 0,
         noVram.vram.detail);

  ResourceByteProbe ram;
  ram.status = ResourceProbeStatus::Probed;
  ram.totalBytes = 1000;
  ram.availableBytes = 400;
  const auto cells = decideResidentAgainstProbe("time-refine", "work-unit", 12, ram, 0);
  report("resident-rejects-cells",
         cells.kind == ResourceDecisionKind::Rejected &&
             cells.errorCode == "resource.invalid-resource" && cells.resource == "invalid" &&
             cells.granted == 0 && cells.requested == 12 &&
             std::string(resourceDecisionUnit(cells)) == "bytes",
         cells.message);
  const auto fit = decideResidentAgainstProbe("inference", "vram", 100, ram, 50);
  report("resident-grants-request",
         fit.kind == ResourceDecisionKind::Accepted && fit.granted == 100 &&
             fit.requested == 100 && fit.available == 400 && fit.reserve == 50 &&
             fit.resource == "vram",
         fit.message);
  const auto shortfall = decideResidentAgainstProbe("inference", "ram", 351, ram, 50);
  report("resident-no-shrink",
         shortfall.kind == ResourceDecisionKind::Rejected &&
             shortfall.errorCode == "resource.resident-shortfall" && shortfall.granted == 0 &&
             shortfall.requested == 351 && shortfall.available == 400,
         shortfall.message);
  ResourceByteProbe unknown;
  unknown.status = ResourceProbeStatus::NotRequested;
  const auto skipped = decideResidentAgainstProbe("inference", "vram", 1, unknown, 0);
  report("resident-unprobed",
         skipped.errorCode == "resource.probe-unavailable" && skipped.available == 0 &&
             skipped.granted == 0 && skipped.requested == 1,
         skipped.errorCode);
}

void testDiagnosticsJson() {
  RuntimeResourceSnapshot snapshot;
  snapshot.ram.status = ResourceProbeStatus::Probed;
  snapshot.ram.totalBytes = 100;
  snapshot.ram.availableBytes = 40;
  snapshot.vram.status = ResourceProbeStatus::NotRequested;
  snapshot.vram.detail = "not-requested";
  snapshot.volume.status = ResourceProbeStatus::Probed;
  snapshot.volume.volumeKey = "D:\\";
  snapshot.volume.availableBytes = 6900;
  snapshot.volume.totalBytes = 9000;
  snapshot.capturedAtSteadyNs = 15;
  const auto ledger = evalUsage(sampleUsage(), 6900);
  const auto sample = decideSamplePhaseBudget(6ull << 30, 32, kSampleWorkUnitCellCeiling + 1);
  std::string merged = "sentinel";
  std::string error;
  const bool emptyOk =
      mergeRuntimeResourceDiagnostics("", snapshot, &ledger, &sample, merged, error);
  report("json-empty",
         emptyOk && error.empty() &&
             has(merged, "\"schema\":\"signal-studio-detection-profile-v1\"") &&
             has(merged, "\"resources\":") && has(merged, "\"total\":\"100\"") &&
             has(merged, "\"available\":\"40\"") && has(merged, "\"unit\":\"bytes\"") &&
             has(merged, "\"unit\":\"cells\"") &&
             has(merged, "\"requested\":\"268435457\"") && has(merged, "\"granted\":\"6900\"") &&
             has(merged, "\"granted\":\"0\"") && has(merged, "D:\\\\") && merged.size() <= 65536,
         error + " " + std::to_string(merged.size()));

  const std::string timeline =
      "{\"schema\":\"signal-studio-detection-profile-v1\",\"timeline\":{\"clock\":"
      "\"steady_clock_ns\",\"marks\":[{\"name\":\"t_accepted\",\"steadyNs\":\"15\"}]},"
      "\"note\":\"brace } inside\"}";
  merged = "sentinel";
  const bool kept = mergeRuntimeResourceDiagnostics(timeline, snapshot, nullptr, nullptr, merged, error);
  report("json-keeps-timeline",
         kept && has(merged, "t_accepted") && has(merged, "steadyNs") &&
             has(merged, "\"15\"") && has(merged, "brace } inside") &&
             has(merged, "\"schema\":\"signal-studio-detection-profile-v1\"") &&
             has(merged, "\"resources\":") && merged.find("\"sample\"") == std::string::npos,
         merged);

  const std::string pretty =
      "{\n  \"schema\" : \"signal-studio-detection-profile-v1\",\n  \"resources\" : "
      "{ \"old\" : true },\n  \"timeline\" : { \"clock\" : \"steady_clock_ns\" }\n}\n";
  merged = "sentinel";
  const bool replaced =
      mergeRuntimeResourceDiagnostics(pretty, snapshot, &ledger, &sample, merged, error);
  report("json-replaces-resources",
         replaced && merged.find("\"old\"") == std::string::npos &&
             has(merged, "steady_clock_ns") && has(merged, "\"resources\":") &&
             has(merged, "\"diskPeakBytes\":\"6900\""),
         merged);

  merged = "sentinel";
  const bool badSchema =
      !mergeRuntimeResourceDiagnostics("{\"schema\":\"other\"}", snapshot, nullptr, nullptr,
                                      merged, error) &&
      merged == "sentinel" && error == "resource.diagnostics-rejected";
  report("json-rejects-schema", badSchema, error + " merged=" + merged);

  std::string huge = "{\"schema\":\"signal-studio-detection-profile-v1\",\"pad\":\"";
  huge.append(65200, 'a');
  huge += "\"}";
  merged = "sentinel";
  const bool tooBig =
      !mergeRuntimeResourceDiagnostics(huge, snapshot, &ledger, &sample, merged, error) &&
      merged == "sentinel";
  report("json-rejects-over-64kib", tooBig, error + " in=" + std::to_string(huge.size()));
}

const std::string kTimeline =
    "{\"schema\":\"signal-studio-detection-profile-v1\",\"timeline\":{\"clock\":"
    "\"steady_clock_ns\",\"marks\":[{\"name\":\"t_accepted\",\"steadyNs\":\"15\"}]}}";
constexpr std::uint64_t kSixGiB = 6ull << 30;
constexpr std::uint64_t kHeadWindows = 2044;
constexpr std::uint64_t kHeadBytes = 196608;
constexpr std::uint64_t kHeadProduct = 401866752ull;

QString number(std::uint64_t value) {
  return QString::number(static_cast<qulonglong>(value));
}
QJsonObject profileOf(const std::string& json) {
  return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
}
QJsonObject resourcesOf(const std::string& json) {
  return profileOf(json).value(QStringLiteral("resources")).toObject();
}
bool timelineKept(const std::string& json) {
  const auto marks = profileOf(json).value(QStringLiteral("timeline")).toObject().value(
      QStringLiteral("marks"));
  const auto mark = marks.toArray().at(0).toObject();
  return profileOf(json).value(QStringLiteral("schema")).toString() ==
             QStringLiteral("signal-studio-detection-profile-v1") &&
         mark.value(QStringLiteral("name")).toString() == QStringLiteral("t_accepted") &&
         mark.value(QStringLiteral("steadyNs")).toString() == QStringLiteral("15");
}
bool deferredWithoutBytes(const QJsonObject& facts, const char* name) {
  const auto fact = facts.value(QString::fromUtf8(name)).toObject();
  return fact.value(QStringLiteral("status")).toString() == QStringLiteral("deferred") &&
         !fact.contains(QStringLiteral("bytes")) &&
         !fact.contains(QStringLiteral("existingBytes"));
}
RuntimeResourceSnapshot injectedSnapshot(std::uint64_t ramAvailable,
                                         std::uint64_t ramTotal,
                                         std::uint64_t volumeAvailable) {
  RuntimeResourceSnapshot snapshot;
  snapshot.ram.status = ResourceProbeStatus::Probed;
  snapshot.ram.totalBytes = ramTotal;
  snapshot.ram.availableBytes = ramAvailable;
  snapshot.ram.detail = "injected";
  snapshot.vram.status = ResourceProbeStatus::NotRequested;
  snapshot.vram.detail = "not-requested";
  snapshot.volume.status = ResourceProbeStatus::Probed;
  snapshot.volume.volumeKey = "D:\\";
  snapshot.volume.availableBytes = volumeAvailable;
  snapshot.volume.totalBytes = std::max(volumeAvailable, std::uint64_t{1});
  snapshot.volume.detail = "injected";
  snapshot.capturedAtSteadyNs = 15;
  return snapshot;
}
DetectionSourcePlan makePlan(std::uint64_t windows, std::uint64_t perWindow,
                             std::uint64_t resident) {
  DetectionSourcePlan plan;
  plan.eligibleWindowCount = windows;
  plan.modelHeadBytesPerWindow = perWindow;
  plan.estimatedPeakBytes = resident;
  return plan;
}
DetectionRequest makeRequest(bool retain, std::uint64_t quota, std::uint64_t grant) {
  DetectionRequest request;
  request.retainRawModelOutput = retain;
  request.rawHeadBudgetBytes = quota;
  request.legacyCombinedEvidence = false;
  request.workingSetBudgetBytes = grant;
  return request;
}

void testProductionHelpers() {
  {
    QTemporaryDir directory;
    report("preflight-temp-dir", directory.isValid(),
           directory.errorString().toStdString());
    auto request = makeRequest(false, 0, kSixGiB);
    const auto scratchBudget = request.sampleScratchBudgetBytes;
    std::string json = kTimeline;
    QString error;
    const bool ok = assessDetectionResourcePreflight(
        directory.path(), makePlan(kHeadWindows, kHeadBytes, 4096), request, json, error);
    const auto profile = profileOf(json);
    const auto resources = profile.value(QStringLiteral("resources")).toObject();
    const auto ram = resources.value(QStringLiteral("ram")).toObject();
    const auto vram = resources.value(QStringLiteral("vram")).toObject();
    const auto volume = resources.value(QStringLiteral("volume")).toObject();
    const auto facts = resources.value(QStringLiteral("diskFacts")).toObject();
    const auto authorization =
        resources.value(QStringLiteral("workingSetAuthorization")).toObject();
    const QDir listing(directory.path());
    report("preflight-real-no-files",
           listing.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(),
           std::to_string(listing.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size()));
    report("preflight-real-no-gpu",
           vram.value(QStringLiteral("status")).toString() ==
                   QStringLiteral("not-requested") &&
               vram.value(QStringLiteral("total")).toString() == QStringLiteral("0") &&
               vram.value(QStringLiteral("available")).toString() == QStringLiteral("0"),
           vram.value(QStringLiteral("detail")).toString().toStdString());
    report("preflight-real-timeline", timelineKept(json) &&
                                          profile.value(QStringLiteral("schema")).toString() ==
                                              QStringLiteral("signal-studio-detection-profile-v1"),
           error.toStdString());
    const auto detail = volume.value(QStringLiteral("detail")).toString();
    report("preflight-real-volume",
           volume.value(QStringLiteral("status")).toString() == QStringLiteral("probed") &&
               !volume.value(QStringLiteral("key")).toString().isEmpty() &&
               (detail.contains(QStringLiteral("GetDiskFreeSpaceExW")) ||
                detail.contains(QStringLiteral("statvfs"))),
           (volume.value(QStringLiteral("key")).toString() + QStringLiteral(" ") + detail)
               .toStdString());
    const bool ramHonest =
        ram.value(QStringLiteral("status")).toString() == QStringLiteral("probed") ||
        ram.value(QStringLiteral("status")).toString() == QStringLiteral("unavailable");
    const bool ceilingKept =
        authorization.value(QStringLiteral("bytes")).toString() == number(kSixGiB) &&
        authorization.contains(QStringLiteral("comparedToAvailableRam")) &&
        authorization.value(QStringLiteral("comparedToAvailableRam")).isBool() &&
        !authorization.value(QStringLiteral("comparedToAvailableRam")).toBool();
    const bool deferredScratch = deferredWithoutBytes(facts, "scratch") &&
                                 deferredWithoutBytes(facts, "publicationShards") &&
                                 deferredWithoutBytes(facts, "checkpoint") &&
                                 deferredWithoutBytes(facts, "projectAtomic");
    if (ok) {
      report("preflight-real-raw-off",
             error.isEmpty() && ceilingKept && deferredScratch &&
                 resources.value(QStringLiteral("diskPeakBytes")).toString() ==
                     QStringLiteral("0") &&
                 resources.value(QStringLiteral("rawHeadBranch")).toString() ==
                     QStringLiteral("off") &&
                 facts.value(QStringLiteral("rawHead")).toObject().value(
                     QStringLiteral("bytes")) == QStringLiteral("0") &&
                 resources.value(QStringLiteral("rawHeadProduct"))
                         .toObject()
                         .value(QStringLiteral("status")) == QStringLiteral("not-taken") &&
                 resources.value(QStringLiteral("sample")).toObject().value(
                     QStringLiteral("kind")) == QStringLiteral("Accepted") &&
                 resources.value(QStringLiteral("sample")).toObject().value(
                     QStringLiteral("granted")) == QStringLiteral("4096") &&
                 resources.value(QStringLiteral("sample")).toObject().value(
                     QStringLiteral("available")) == number(kSixGiB) &&
                 number(scratchBudget) != QStringLiteral("0") &&
                 resources.value(QStringLiteral("diskPeakBytes")).toString() !=
                     number(scratchBudget),
             error.toStdString());
    } else {
      report("preflight-real-raw-off",
             ram.value(QStringLiteral("status")).toString() ==
                     QStringLiteral("unavailable") &&
                 error.contains(QStringLiteral("resource.probe-unavailable")) &&
                 ceilingKept && deferredScratch && timelineKept(json),
             error.toStdString());
    }
    report("preflight-real-ram-status", ramHonest, ram.value(QStringLiteral("status"))
                                                       .toString()
                                                       .toStdString());
  }

  {
    const auto snapshot = injectedSnapshot(1ull << 20, 64ull << 30, 1ull << 30);
    std::string json = kTimeline;
    QString error;
    const bool rejected = !assessDetectionResourcePreflight(
        snapshot, makePlan(1, 1, 2ull << 20), makeRequest(false, 0, kSixGiB), json, error);
    const auto resources = resourcesOf(json);
    const auto resident = resources.value(QStringLiteral("sourceResident")).toObject();
    const auto sample = resources.value(QStringLiteral("sample")).toObject();
    report("preflight-resident-not-grant",
           rejected && timelineKept(json) &&
               error.contains(QStringLiteral("resource.resident-shortfall")) &&
               error.contains(QStringLiteral("ceiling")) &&
               !error.contains(QStringLiteral("grant-exceeds")) &&
               resident.value(QStringLiteral("requested")) == number(2ull << 20) &&
               resident.value(QStringLiteral("available")) == number(1ull << 20) &&
               sample.value(QStringLiteral("kind")) == QStringLiteral("Accepted") &&
               sample.value(QStringLiteral("available")) == number(kSixGiB) &&
               sample.value(QStringLiteral("granted")) == number(2ull << 20) &&
               !resources.value(QStringLiteral("workingSetAuthorization"))
                    .toObject()
                    .value(QStringLiteral("comparedToAvailableRam"))
                    .toBool(),
           error.toStdString());
  }

  {
    constexpr auto kEstimate = 32ull << 20;
    constexpr auto kAvailable = 1536ull << 20;
    const auto snapshot = injectedSnapshot(kAvailable, 2ull << 30, 1ull << 30);
    std::string json = kTimeline;
    QString error;
    const bool ok = assessDetectionResourcePreflight(
        snapshot, makePlan(1, 1, kEstimate), makeRequest(false, 0, kSixGiB), json, error);
    const auto resources = resourcesOf(json);
    const auto sample = resources.value(QStringLiteral("sample")).toObject();
    const auto resident = resources.value(QStringLiteral("sourceResident")).toObject();
    report("preflight-large-grant-fits-actual-ram",
           ok && error.isEmpty() && sample.value(QStringLiteral("kind")) ==
                                       QStringLiteral("Accepted") &&
               sample.value(QStringLiteral("available")) == number(kSixGiB) &&
               sample.value(QStringLiteral("granted")) == number(kEstimate) &&
               resident.value(QStringLiteral("available")) == number(kAvailable) &&
               resident.value(QStringLiteral("granted")) == number(kEstimate) &&
               resources.value(QStringLiteral("sampleCells"))
                       .toObject()
                       .value(QStringLiteral("status")) == QStringLiteral("deferred") &&
               !resources.value(QStringLiteral("sampleCells")).toObject().contains(
                   QStringLiteral("bytes")),
           error.toStdString());
  }

  {
    const auto estimate = kSampleContiguousAllocationCapBytes + 1;
    const auto snapshot = injectedSnapshot(2ull << 30, 2ull << 30, 1ull << 30);
    std::string json = kTimeline;
    QString error;
    const bool ok = assessDetectionResourcePreflight(
        snapshot, makePlan(1, 1, estimate), makeRequest(false, 0, kSixGiB), json, error);
    const auto sample = resourcesOf(json).value(QStringLiteral("sample")).toObject();
    report("preflight-adapted-grant-is-not-failure",
           ok && error.isEmpty() &&
               sample.value(QStringLiteral("kind")) == QStringLiteral("Adapted") &&
               sample.value(QStringLiteral("errorCode")) ==
                   QStringLiteral("sample.grant-exceeds-contiguous-cap") &&
               sample.value(QStringLiteral("granted")) ==
                   number(kSampleContiguousAllocationCapBytes) &&
               sample.value(QStringLiteral("available")) == number(kSixGiB),
           error.toStdString());
  }

  {
    const auto snapshot = injectedSnapshot(1000, 2000, 1ull << 20);
    std::string json = kTimeline;
    QString error;
    const bool rejected = !assessDetectionResourcePreflight(
        snapshot, makePlan(1, 1, 100), makeRequest(false, 0, 50), json, error);
    report("preflight-estimate-exceeds-grant",
           rejected && error.contains(QStringLiteral("sample.chunk-exceeds-grant")) &&
               !error.contains(QStringLiteral("resource.resident-shortfall")) &&
               timelineKept(json),
           error.toStdString());
  }

  {
    const auto snapshot = injectedSnapshot(100, 64ull << 30, 1ull << 30);
    std::string json = kTimeline;
    QString error;
    const bool ok = assessDetectionResourcePreflight(
        snapshot, makePlan(kHeadWindows, kHeadBytes, 0), makeRequest(false, 0, kSixGiB), json,
        error);
    const auto resources = resourcesOf(json);
    const auto resident = resources.value(QStringLiteral("sourceResident")).toObject();
    report("preflight-unknown-resident-stays-deferred",
           ok && error.isEmpty() &&
               resident.value(QStringLiteral("status")) == QStringLiteral("deferred") &&
               !resident.contains(QStringLiteral("granted")) &&
               !resident.contains(QStringLiteral("bytes")) &&
               !resources.contains(QStringLiteral("sample")) && timelineKept(json),
           error.toStdString());
  }

  {
    auto snapshot = injectedSnapshot(1ull << 30, 64ull << 30, kHeadProduct);
    std::string json = kTimeline;
    QString error;
    const bool ok = assessDetectionResourcePreflight(
        snapshot, makePlan(kHeadWindows, kHeadBytes, 4096), makeRequest(true, 0, kSixGiB), json,
        error);
    const auto resources = resourcesOf(json);
    report("preflight-raw-on-exact-volume",
           ok && error.isEmpty() &&
               resources.value(QStringLiteral("volumeEnforced")).toBool() &&
               resources.value(QStringLiteral("diskPeakBytes")) == number(kHeadProduct) &&
               resources.value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("kind")) == QStringLiteral("Accepted") &&
               resources.value(QStringLiteral("rawHeadBranch")) == QStringLiteral("on"),
           error.toStdString());
    snapshot.volume.availableBytes = kHeadProduct - 1;
    json = kTimeline;
    const bool shortVolume = !assessDetectionResourcePreflight(
        snapshot, makePlan(kHeadWindows, kHeadBytes, 4096), makeRequest(true, 0, kSixGiB), json,
        error);
    const auto expectedRemain =
        QStringLiteral(
            "Raw model output retention rejected before inference: %1 windows "
            "x %2 bytes require %3 bytes; %4 bytes remain.")
            .arg(static_cast<qulonglong>(kHeadWindows))
            .arg(static_cast<qulonglong>(kHeadBytes))
            .arg(static_cast<qulonglong>(kHeadProduct))
            .arg(static_cast<qulonglong>(kHeadProduct - 1));
    report("preflight-raw-on-short-volume",
           shortVolume && error == expectedRemain &&
               resourcesOf(json).value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("errorCode")) ==
                   QStringLiteral("resource.raw-head-shortfall") &&
               timelineKept(json),
           error.toStdString());
    snapshot.volume.status = ResourceProbeStatus::Unavailable;
    snapshot.volume.availableBytes = 0;
    snapshot.volume.totalBytes = 0;
    snapshot.volume.detail = "injected-unavailable";
    json = kTimeline;
    const bool unknown = !assessDetectionResourcePreflight(
        snapshot, makePlan(kHeadWindows, kHeadBytes, 4096), makeRequest(true, 0, kSixGiB), json,
        error);
    report("preflight-raw-on-unknown-volume",
           unknown &&
               error ==
                   QStringLiteral(
                       "Raw model output retention rejected before inference: %1 "
                       "windows x %2 bytes require %3 bytes; remaining disk "
                       "bytes are unavailable.")
                       .arg(static_cast<qulonglong>(kHeadWindows))
                       .arg(static_cast<qulonglong>(kHeadBytes))
                       .arg(static_cast<qulonglong>(kHeadProduct)) &&
               resourcesOf(json).value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("errorCode")) ==
                   QStringLiteral("resource.raw-head-shortfall"),
           error.toStdString());
    snapshot = injectedSnapshot(1ull << 30, 64ull << 30, 500000000ull);
    json = kTimeline;
    const bool shortQuota = !assessDetectionResourcePreflight(
        snapshot, makePlan(kHeadWindows, kHeadBytes, 4096),
        makeRequest(true, kHeadProduct - 1, kSixGiB), json, error);
    report("preflight-raw-on-short-quota",
           shortQuota &&
               error.startsWith(QStringLiteral(
                   "Raw model head disk budget rejected before inference:")) &&
               error.contains(QStringLiteral("limit")) &&
               !error.contains(QStringLiteral("bytes remain")) &&
               resourcesOf(json).value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("errorCode")) ==
                   QStringLiteral("resource.raw-head-shortfall"),
           error.toStdString());
  }

  {
    const auto snapshot = injectedSnapshot(1ull << 30, 64ull << 30, 10);
    std::string json = kTimeline;
    QString error;
    constexpr auto kQuota = 256ull * 1024 * 1024;
    const bool rejected = !assessDetectionResourcePreflight(
        snapshot, makePlan(kHeadWindows, kHeadBytes, 4096), makeRequest(false, kQuota, kSixGiB),
        json, error);
    report("preflight-explicit-quota",
           rejected &&
               error ==
                   QStringLiteral(
                       "Raw model head disk budget rejected before inference: "
                       "%1 windows x %2 bytes; limit %3 bytes. Reduce the range, "
                       "increase the frame interval or explicitly provision a "
                       "bounded artifact quota.")
                       .arg(static_cast<qulonglong>(kHeadWindows))
                       .arg(static_cast<qulonglong>(kHeadBytes))
                       .arg(static_cast<qulonglong>(kQuota)) &&
               resourcesOf(json).value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("errorCode")) ==
                   QStringLiteral("resource.explicit-raw-head-quota") &&
               !error.contains(QStringLiteral("shortfall")),
           error.toStdString());
    json = kTimeline;
    const bool overCap = !assessDetectionResourcePreflight(
        snapshot, makePlan(1, 1, 4096),
        makeRequest(false, kDetectionExplicitRawHeadMaxBytes + 1, kSixGiB), json, error);
    const auto product = resourcesOf(json).value(QStringLiteral("rawHeadProduct")).toObject();
    report("preflight-explicit-cap",
           overCap &&
               error.startsWith(QStringLiteral(
                   "Raw model head disk budget rejected before inference:")) &&
               resourcesOf(json).value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("errorCode")) ==
                   QStringLiteral("resource.explicit-raw-head-cap") &&
               product.value(QStringLiteral("status")) == QStringLiteral("over-cap") &&
               !product.contains(QStringLiteral("bytes")),
           error.toStdString());
    json = kTimeline;
    const bool fitsQuota = assessDetectionResourcePreflight(
        snapshot, makePlan(10, 100, 4096), makeRequest(false, 1ull << 20, kSixGiB), json, error);
    const auto resources = resourcesOf(json);
    report("preflight-explicit-quota-defers-volume",
           fitsQuota && error.isEmpty() &&
               !resources.value(QStringLiteral("volumeEnforced")).toBool() &&
               resources.value(QStringLiteral("rawHeadBranch")) ==
                   QStringLiteral("explicit-quota") &&
               resources.value(QStringLiteral("diskFacts"))
                       .toObject()
                       .value(QStringLiteral("rawHead"))
                       .toObject()
                       .value(QStringLiteral("bytes")) == QStringLiteral("1000") &&
               resources.value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("available")) == number(1ull << 20) &&
               resources.value(QStringLiteral("volume")).toObject().value(
                   QStringLiteral("available")) == QStringLiteral("10") &&
               deferredWithoutBytes(resources.value(QStringLiteral("diskFacts")).toObject(),
                                    "scratch"),
           error.toStdString());
    DetectionDiskUsage usage;
    usage.durableVolumeKey = "D:\\";
    usage.storeRawHead = true;
    usage.rawHeadBytes = 1000;
    json = kTimeline;
    const bool stageRejects = !assessDetectionResourceStage(
        "checkpoint", usage, kDiskFactRawHead, snapshot, json, error);
    report("stage-explicit-product-uses-free-bytes",
           stageRejects && error.contains(QStringLiteral("resource.raw-head-shortfall")) &&
               resourcesOf(json).value(QStringLiteral("diskPeakBytes")) ==
                   QStringLiteral("1000") &&
               timelineKept(json),
           error.toStdString());
  }

  {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const auto snapshot = injectedSnapshot(1ull << 30, 64ull << 30, max);
    std::string json = kTimeline;
    QString error;
    const bool overflow = !assessDetectionResourcePreflight(
        snapshot, makePlan(max, 2, 4096), makeRequest(true, 0, kSixGiB), json, error);
    const auto product = resourcesOf(json).value(QStringLiteral("rawHeadProduct")).toObject();
    report("preflight-raw-on-overflow",
           overflow && error.contains(QStringLiteral("overflow the disk quota")) &&
               error.startsWith(QStringLiteral(
                   "Raw model output retention rejected before inference:")) &&
               !error.contains(QStringLiteral("shortfall")) &&
               resourcesOf(json).value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("errorCode")) ==
                   QStringLiteral("resource.arithmetic-overflow") &&
               product.value(QStringLiteral("status")) == QStringLiteral("overflow") &&
               !product.contains(QStringLiteral("bytes")),
           error.toStdString());
    json = kTimeline;
    const bool ignored = assessDetectionResourcePreflight(
        snapshot, makePlan(max, 2, 4096), makeRequest(false, 0, kSixGiB), json, error);
    report("preflight-raw-off-ignores-overflow",
           ignored && error.isEmpty() &&
               resourcesOf(json).value(QStringLiteral("rawHeadProduct")).toObject().value(
                   QStringLiteral("status")) == QStringLiteral("not-taken") &&
               resourcesOf(json).value(QStringLiteral("diskFacts"))
                       .toObject()
                       .value(QStringLiteral("rawHead"))
                       .toObject()
                       .value(QStringLiteral("bytes")) == QStringLiteral("0"),
           error.toStdString());
    json = kTimeline;
    auto legacy = makeRequest(true, 0, kSixGiB);
    legacy.legacyCombinedEvidence = true;
    const bool legacyOk = assessDetectionResourcePreflight(
        snapshot, makePlan(kHeadWindows, kHeadBytes, 4096), legacy, json, error);
    const auto rawHead = resourcesOf(json)
                             .value(QStringLiteral("diskFacts"))
                             .toObject()
                             .value(QStringLiteral("rawHead"))
                             .toObject();
    report("preflight-legacy-unchanged",
           legacyOk && error.isEmpty() &&
               resourcesOf(json).value(QStringLiteral("rawHeadBranch")) ==
                   QStringLiteral("legacy-combined") &&
               rawHead.value(QStringLiteral("status")) == QStringLiteral("legacy-combined") &&
               !rawHead.contains(QStringLiteral("bytes")) &&
               resourcesOf(json).value(QStringLiteral("diskPeakBytes")) == QStringLiteral("0"),
           error.toStdString());
  }

  {
    const auto snapshot = injectedSnapshot(1ull << 30, 64ull << 30, 1980);
    auto usage = sampleUsage();
    const auto withoutScratch = kDiskFactRawHead | kDiskFactResultShards | kDiskFactCheckpoint;
    std::string json = kTimeline;
    QString error;
    const bool accepted = assessDetectionResourceStage("checkpoint", usage, withoutScratch,
                                                       snapshot, json, error);
    const auto acceptedResources = resourcesOf(json);
    const auto acceptedFacts = acceptedResources.value(QStringLiteral("diskFacts")).toObject();
    report("stage-defers-unknown-scratch",
           accepted && error.isEmpty() &&
               acceptedResources.value(QStringLiteral("diskPeakBytes")) ==
                   QStringLiteral("1980") &&
               acceptedResources.value(QStringLiteral("diskPeakPhase")) ==
                   QStringLiteral("checkpoint") &&
               deferredWithoutBytes(acceptedFacts, "scratch") &&
               !acceptedResources.value(QStringLiteral("knownPeakComplete")).toBool() &&
               acceptedResources.value(QStringLiteral("ledgerScope")) ==
                   QStringLiteral("known-only") &&
               timelineKept(json),
           error.toStdString() + " peak=" +
               acceptedResources.value(QStringLiteral("diskPeakBytes")).toString().toStdString());
    json = kTimeline;
    const bool overlapped = !assessDetectionResourceStage(
        "refine", usage, withoutScratch | kDiskFactScratch, snapshot, json, error);
    report("stage-overlap-charges-known-scratch",
           overlapped && error.contains(QStringLiteral("resource.disk-shortfall")) &&
               error.contains(QStringLiteral("phase=refine")) &&
               resourcesOf(json).value(QStringLiteral("diskPeakBytes")) ==
                   QStringLiteral("6900") &&
               resourcesOf(json)
                       .value(QStringLiteral("diskFacts"))
                       .toObject()
                       .value(QStringLiteral("scratch"))
                       .toObject()
                       .value(QStringLiteral("bytes")) == QStringLiteral("5000") &&
               timelineKept(json),
           error.toStdString());
  }

  {
    auto usage = sampleUsage();
    usage.scratchVolumeKey = "S:\\";
    auto durable = injectedSnapshot(1ull << 30, 64ull << 30, 100000);
    auto scratch = injectedSnapshot(1ull << 30, 64ull << 30, 4000);
    scratch.volume.volumeKey = "S:\\";
    const auto facts = kDiskFactRawHead | kDiskFactResultShards | kDiskFactScratch |
                       kDiskFactCheckpoint;
    std::string json = kTimeline;
    QString error;
    const bool separate = !assessDetectionResourceStage(
        "refine", usage, facts, durable, json, error, &scratch);
    report("stage-scratch-volume-separate",
           separate && error.contains(QStringLiteral("resource.disk-shortfall")) &&
               resourcesOf(json).value(QStringLiteral("diskPeakVolume")) ==
                   QStringLiteral("S:\\") &&
               resourcesOf(json).value(QStringLiteral("diskPeakBytes")) ==
                   QStringLiteral("5000") &&
               resourcesOf(json).value(QStringLiteral("scratchVolume")).toObject().value(
                   QStringLiteral("key")) == QStringLiteral("S:\\"),
           error.toStdString());
    json = kTimeline;
    const auto preserved = json;
    const bool missing = !assessDetectionResourceStage("refine", usage, facts, durable, json,
                                                       error);
    report("stage-scratch-volume-unavailable",
           missing && error.contains(QStringLiteral("resource.volume-unavailable")) &&
               timelineKept(json) && json != preserved,
           error.toStdString());
  }

  {
    DetectionDiskUsage usage;
    usage.durableVolumeKey = "D:\\";
    usage.scratchBytes = 99999;
    usage.projectBytes = 100;
    usage.projectExistingBytes = 100;
    const auto snapshot = injectedSnapshot(1ull << 30, 64ull << 30, 250);
    std::string json = kTimeline;
    QString error;
    const bool ok = assessDetectionResourceStage("publishing", usage, kDiskFactProject, snapshot,
                                                 json, error);
    const auto scratch = resourcesOf(json)
                             .value(QStringLiteral("diskFacts"))
                             .toObject()
                             .value(QStringLiteral("scratch"))
                             .toObject();
    report("stage-publishing-scratch-not-live",
           ok && error.isEmpty() &&
               resourcesOf(json).value(QStringLiteral("diskPeakBytes")) ==
                   QStringLiteral("200") &&
               scratch.value(QStringLiteral("status")) == QStringLiteral("not-live") &&
               !scratch.contains(QStringLiteral("bytes")) && timelineKept(json),
           error.toStdString());
  }

  {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    DetectionDiskUsage usage;
    usage.durableVolumeKey = "D:\\";
    usage.checkpointBytes = max;
    usage.checkpointExistingBytes = 1;
    const auto snapshot = injectedSnapshot(1ull << 30, 64ull << 30, max);
    std::string json = kTimeline;
    QString error;
    const bool overflow = !assessDetectionResourceStage("checkpoint", usage, kDiskFactCheckpoint,
                                                        snapshot, json, error);
    report("stage-atomic-overflow",
           overflow && error.contains(QStringLiteral("resource.arithmetic-overflow")) &&
               !error.contains(QStringLiteral("shortfall")) && timelineKept(json),
           error.toStdString());
    usage.checkpointBytes = 0;
    usage.checkpointExistingBytes = 0;
    usage.resultShardBytes = kDetectionShardFileCapBytes + 1;
    usage.largestResultShardBytes = usage.resultShardBytes;
    json = kTimeline;
    const bool boundary = !assessDetectionResourceStage(
        "checkpoint", usage, kDiskFactResultShards, snapshot, json, error);
    report("stage-shard-boundary",
           boundary && error.contains(QStringLiteral("resource.shard-boundary")) &&
               resourcesOf(json).value(QStringLiteral("diskLedger")).toObject().value(
                   QStringLiteral("resource")) == QStringLiteral("format"),
           error.toStdString());
  }

  {
    std::string json = "sentinel";
    QString error;
    const bool invalid = !assessDetectionResourceStage(
        "compile", DetectionDiskUsage{}, 0, injectedSnapshot(1, 1, 1), json, error);
    report("stage-invalid-name", invalid && json == "sentinel" &&
                                     error == QStringLiteral("resource.invalid-stage"),
           error.toStdString());
    std::string huge =
        "{\"schema\":\"signal-studio-detection-profile-v1\",\"pad\":\"";
    huge.append(65200, 'a');
    huge += "\"}";
    const auto preserved = huge;
    const bool tooBig = !assessDetectionResourcePreflight(
        injectedSnapshot(1ull << 30, 64ull << 30, 1ull << 30), makePlan(1, 1, 4096),
        makeRequest(false, 0, kSixGiB), huge, error);
    report("preflight-rejects-over-64kib",
           tooBig && huge == preserved &&
               error == QStringLiteral("resource.diagnostics-rejected"),
           error.toStdString() + " in=" + std::to_string(preserved.size()));
  }
}
}  // namespace

int main() {
  testCheckedArithmetic();
  testSampleDecisionUnchanged();
  testRawHead();
  testLedgerArithmetic();
  testSnapshotAndResident();
  testDiagnosticsJson();
  testProductionHelpers();
  std::cout << (gFailures ? "FAIL" : "PASS") << " ss_de_rpo_resource_tests failures="
            << gFailures << '\n';
  return gFailures == 0 ? 0 : 1;
}
