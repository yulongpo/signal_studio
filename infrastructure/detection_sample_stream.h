#pragma once

// Bounded CPU Sample refinement; FIR diagnostics have no native equivalence claim.
#include <QString>
#include <complex>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "domain/detection_refinement.h"
#include "infrastructure/gpu_dsp.h"

namespace signalstudio::sample_stream {
using Complex = std::complex<float>;

enum class Mode { LegacyExact, StreamingFir, Auto,
                  StreamingDiagnostic = StreamingFir, AutoConservative = Auto };
struct Config {
  std::uint32_t version = 1;
  Mode mode = Mode::LegacyExact; // Old/missing request extensions retain Legacy.
  std::uint32_t outputBlockSamples = 131072;
  std::uint32_t maximumFftSamples = 1U << 20;
  std::uint32_t maximumFirRadius = 262143;
  double firTailAmplitudeBudget = 8e-5;
  std::uint64_t scratchBudgetBytes = 16ULL << 30;
  std::uint64_t scratchFreeFloorBytes = 2ULL << 30;
};
struct Context {
  DetectionRefinementInput input; // no energy, spectrum or fallbackActivity
  DetectionParameters parameters;
  DetectionRefinementOptions options;
  TimeRange searchSamples; // already expanded by 8ms, clipped to available prefix
  SampleIndex availableSamples = 0;
  double relativeCenterHz = 0;
  double bandwidthHz = 0;
};
struct WorkLedger {
  // Bind the task's EXISTING totalCells here; never reset between candidates,
  // FIR/Legacy retries or measured-Coarse work. Charge source reads incl. halo.
  std::uint64_t* consumedCells = nullptr;
  std::uint64_t limit = std::uint64_t{1} << 34;
};
struct Runtime {
  // Borrowed only for this synchronous run; owned by the producer's source.
  gpu_dsp::Session* gpu = nullptr;
  // Exclusive grants after source/publication/other live allocations are reserved.
  std::uint64_t memoryGrantBytes = 96ULL << 20;
  std::uint64_t resultByteGrant = 16ULL << 20;
  std::uint64_t readerResidentReserveBytes = 32ULL << 20;
  // Decoder/format-aware cap, including planar+channel encoded-byte restrictions.
  std::uint32_t sourceReadBlockSamples = 131072;
  QString privateScratchRoot; // trusted task-owned path; never source/reference
  WorkLedger work;
  // Reader MUST fill exactly destination.size() finite IQ cells, without resizing.
  std::function<bool(TimeRange, std::span<Complex>, QString&)> read;
  std::function<bool(QString&)> verifySource; // frozen identity, mandatory
  // Required for scratch; global shared lease, separate from local hard quota.
  std::function<bool(std::uint64_t, QString&)> reserveScratch;
  std::function<void(std::uint64_t)> releaseScratch;
  // Test/Golden taps only. Every span <= outputBlockSamples; production leaves empty.
  std::function<bool(SampleIndex, std::span<const float>, QString&)> energyTap;
  std::function<bool(SampleIndex, std::span<const float>, QString&)> smoothedTap;
  std::function<bool(SampleIndex, std::span<const Complex>, QString&)> filteredIqTap;
};
enum class ErrorCode { None, InvalidInput, ResourceLimit, WorkLimit, Cancelled, SourceChanged,
                       SourceRead, ScratchIo, Numerical, Integration, Cleanup };
struct Failure {
  ErrorCode code = ErrorCode::None;
  QString stage;
  QString detail;
  QString cleanupFailure;
};
struct Plan {
  SampleIndex count = 0;
  std::uint32_t radius = 0, taps = 0, fftSamples = 0, outputBlockSamples = 0;
  SampleIndex smoothingSamples = 0, minOnSamples = 0, minOffSamples = 0;
  bool continuous = false, split = false;
  double tailBound = 0;
  std::uint64_t estimatedResidentBytes = 0, scratchReservationBytes = 0;
  std::uint64_t legacyResidentEstimateBytes = 0;
  bool legacyEligible = false;
};
struct TracePoint { SampleIndex sample = 0; double mean = 0; };
struct Statistics {
  std::uint64_t gpuFirBlocks = 0;
  Plan plan;
  std::uint64_t sourceSamplesRead = 0, scratchBytesRead = 0;
  std::uint64_t scratchBytesWritten = 0, scratchPeakBytes = 0;
  std::uint64_t rawQualifiedRuns = 0, onCount = 0;
  bool rawRunCountMeasured = false, onCountMeasured = false;
  std::uint64_t externalEnergyCellsRead = 0;
  double maximumInputAmplitude = 0, maximumEnergy = 0, maximumSmoothedMagnitude = 0;
  double empiricalSmoothAllowance = 0;
  std::uint64_t uncertainComparisonCells = 0;
  std::optional<Plan> diagnosticFirPlan;
  std::vector<TracePoint> diagnosticFirTrace;
  std::uint64_t diagnosticFirSegments = 0;
  std::vector<TracePoint> trace;
};
enum class Route { LegacyExact, UnverifiedFirDiagnostic, UnknownNeedsMeasuredCoarse, EmpiricalFir };
struct Result {
  DetectionRefinementResult refinement;
  Statistics statistics;
  Route route = Route::UnknownNeedsMeasuredCoarse;
  QString autoReason;
  bool nativeEnvelopeEstablished = false; // no config/request can set this true
  bool precisionGuardEvaluated = false, precisionGuardPassed = false;
  QString precisionPolicy = "empirical-sample-fir-sensitivity-v1";
  QString precisionReason;
  bool publicationEligible = false;
  bool needsMeasuredCoarse = false;
};

// Planning only: no IQ, FFT, scratch creation or source verification.
bool makePlan(const Context&, const Config&, const Runtime&, Plan&, Failure&);
// Resident IQ/E/S/quantile vectors exist ONLY inside the eligible Legacy branch.
// Streaming uses fixed K/C buffers + quota-owned E/S files + <=512 trace points.
// Atomic caller result: failure/cancel leaves output unchanged.
// FIR eligibility is scoped/empirical, never a universal native error theorem.
// Auto uses eligible FIR, otherwise complete Legacy if its full budget fits;
// unavailable precision fallback returns UNKNOWN requiring measured Coarse.
bool run(const Context&, const Config&, const Runtime&, Result&, Failure&);

struct EnergySource {
  SampleIndex count = 0, filterSupportSamples = 0;
  std::string method;
  std::function<bool(SampleIndex, std::span<float>, QString&)> read;
};
// Same-E native Golden seam. No resampling or coarsening; step is exactly 1.
// Atomic diagnostic output, never an authorization to publish external evidence.
bool refineEnergyBlocks(const Context&, const Config&, const Runtime&,
                        const EnergySource&, Result&, Failure&);
} // namespace signalstudio::sample_stream
