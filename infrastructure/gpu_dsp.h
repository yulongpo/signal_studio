#pragma once
// Optional CUDA DSP. No CUDA SDK types, static CUDA linkage or process-global
// GPU cache. A failed operation leaves caller output spans unchanged.
#include <cstdint>
namespace signalstudio::gpu_dsp {
enum class ErrorCode { None, Unavailable, Unsupported, InvalidInput, ResourceLimit,
                       Cancelled, WrongThread, Driver, Fft, Numerical };
struct Limits {
  std::uint64_t deviceBytes = 256ULL * 1024 * 1024;
  std::uint64_t pinnedBytes = 64ULL * 1024 * 1024;
  std::uint64_t hostBytes = 64ULL * 1024 * 1024;
  std::uint32_t plans = 4, maximumTransform = 1U << 22, maximumBatch = 64;
  std::uint64_t maximumElements = 8ULL * 1024 * 1024;
};
struct FirFilter {
  std::uint64_t owner = 0, generation = 0;
  std::uint32_t fftSamples = 0, radius = 0;
};
struct StftSummary { std::uint64_t powerFloorBins = 0, densityUnderflowGuardBins = 0; };
struct RadixPrefix { std::uint32_t prefix = 0, significantBytes = 0; };
}
#ifndef SS_GPU_DSP_MODULE_ABI_ONLY
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace signalstudio::gpu_dsp {
inline constexpr const char* kFirMethod = "cuda-cufft-periodic-fir-f32-v1";
inline constexpr const char* kStftMethod = "cuda-cufft-stft-f64-ordered-power-v1";
inline constexpr const char* kNormalizeMethod = "cuda-cub-f64-two-pass-ddof1-v1";
using Cancel = std::function<bool()>;
struct Error { ErrorCode code = ErrorCode::None; std::string stage, detail; };
struct Config {
  int deviceOrdinal = 0;
  // Explicit resolved paths or application-owned deployed paths. Empty cuFFT
  // path means dependency unavailable; never search arbitrary current-dir DLLs.
  std::string moduleLibrary, cufftLibrary, nvJitLinkLibrary;
  Limits limits;
  Cancel cancelled;
};
struct Capabilities {
  bool driverLoaded = false, cufftLoaded = false, sessionReady = false;
  int driverVersion = 0, cufftVersion = 0, deviceOrdinal = -1;
  int computeMajor = 0, computeMinor = 0;
  std::string deviceName, driverLibrary, cufftLibrary, moduleLibrary, fallbackReason;
  std::uint64_t deviceTotalBytes = 0, deviceFreeBytesAtSetup = 0;
};
struct Metrics {
  double setupMs = 0, hostAllocationMs = 0, planMs = 0;
  double h2dMs = 0, fftMs = 0, kernelsMs = 0, d2hMs = 0, totalHostMs = 0;
  std::uint64_t h2dBytes = 0, d2hBytes = 0, fftExecutions = 0, kernelLaunches = 0;
  std::uint64_t deviceBytes = 0, devicePeakBytes = 0, pinnedBytes = 0;
  std::uint64_t workspaceBytes = 0, hostStagingBytes = 0;
  std::uint32_t planCount = 0;
  std::uint64_t libraryAlgorithmCalls = 0, sdkTemporaryBytes = 0;
  std::string timingMeaning = "CUDA events may include first-use JIT/enqueue gaps; kernelLaunches counts pointwise launches only; CUB internals opaque; libraryAlgorithmCalls counts CUB calls; not UI latency";
};
struct PeriodicFirInput {
  // Unmixed, decoded complex<float>, already period-wrapped by the caller.
  // raw.size() == count + 2*radius, zero-padded by GPU to filter.fftSamples.
  // localStart describes raw[0] relative to the periodic context, NOT file time.
  std::span<const std::complex<float>> raw;
  std::int64_t localStart = 0;
  std::uint64_t contextN = 0;
  double sampleRateHz = 0, relativeCenterHz = 0;
  std::uint32_t count = 0;
};
struct StftInput {
  // Packed [records,K] decoded complex<float>, including explicit caller padding.
  // Hann is periodic, supplied as float64 to preserve the CPU Golden coefficients.
  std::span<const std::complex<float>> records;
  std::span<const double> hann;
  std::uint32_t transformBins = 0, recordCount = 0, outputBins = 0;
  double sampleRateHz = 0, hannEnergy = 0;
  // Empty means coefficient 1 for every record. Weights are accumulated in
  // record order; recordDivisor is explicit, normally recordCount.
  std::span<const double> recordWeights;
  double recordDivisor = 0;
};
struct NormalizationSummary {
  double mean = 0, sampleStd = 0, normalizationStd = 0;
  bool epsilonGuardApplied = false;
  std::string method;
};

class Session final {
 public:
  // Creates/retains the selected device's primary context, owns one nonblocking
  // stream, restores the caller context after EVERY call, and releases via RAII.
  // Must be created, used and destroyed on the SAME thread. Optional failure is
  // returned here; callers keep their existing CPU/LegacyExact fallback policy.
  static std::unique_ptr<Session> create(const Config&, Capabilities&, Error&);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;
  const Capabilities& capabilities() const;
  const Metrics& metrics() const; // cumulative counters/times, owner-thread only

  // Exactly one prepared FIR is retained. New preparation invalidates its old
  // token. Coefficients are REAL [K], taps [0,2*radius], zero outside taps.
  bool prepareFir(std::span<const float> paddedCoefficients, std::uint32_t radius,
                  FirFilter&, Error&);
  // Frequency mix reproduces periodic mixedIQ indexing and float phase rounding;
  // C2C FFT -> coefficient multiply -> inverse /K -> norm. Only count energies
  // return unless a same-sized filteredIQ Golden tap is explicitly supplied.
  bool periodicFirBlock(const FirFilter&, const PeriodicFirInput&,
      std::span<float> energy, Error&,
      std::span<std::complex<float>> filteredIqTap = {});
  // Z2Z, ordered record sum, fftshift, area-overlap resampling, float power floor,
  // float64 density/log -> float32. Summary/output publish atomically on success.
  bool stft(const StftInput&, std::span<float> densityDb, StftSummary&, Error&);
  // CUB FP64 two-pass reduction. CPU source Golden remains ordered Welford.
  // No handwritten GPU reduction and no tolerance is relaxed.
  bool normalize(std::span<const float>, std::span<float> normalized,
                 NormalizationSummary&, Error&);
  // IEEE ordered-float keys, exact uint64 counts. Up to four prefix lanes;
  // prefix matches the most significant 0..3 bytes, counts the next byte.
  // Finite inputs only. Output layout [prefixes.size(),256].
  bool radixHistogram(std::span<const float>, std::span<const RadixPrefix>,
                      std::span<std::uint64_t> counts, Error&);
  // Ascending float order statistics. ranks are into the sorted series.
  // Output values match the host radix conversion. Failure leaves output unchanged.
  bool selectOrderStatistics(std::span<const float> values, std::span<const std::uint64_t> ranks,
                             std::span<float> orderStatistics, Error&);
  // Drains in-flight work before staging/device resources can be released.
  // Cancellation is cooperative between bounded launches; no fake preemption.
  bool drain(Error&);
 private:
  Session();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace signalstudio::gpu_dsp

#endif // SS_GPU_DSP_MODULE_ABI_ONLY

// Small, versioned module ABI: POD only, no STL ownership crosses DLL boundaries.
namespace signalstudio::gpu_dsp::module_abi {
inline constexpr std::uint32_t version = 4;
struct Failure { int code = 0; char stage[96]{}; char detail[2048]{}; };
struct Options {
  Limits limits;
  int device = 0;
  int (*cancel)(void*) = nullptr;
  void* cancelState = nullptr;
};
struct Info { int driver=0,cufft=0,major=0,minor=0; std::uint64_t total=0,free=0; char name[256]{}; };
struct Telemetry {
  double setup=0,hostAlloc=0,plan=0,h2d=0,fft=0,kernels=0,d2h=0,host=0;
  std::uint64_t inBytes=0,outBytes=0,execs=0,launches=0,device=0,peak=0,pinned=0,workspace=0,hostStaging=0;
  std::uint32_t plans=0;
  std::uint64_t sdkCalls=0,sdkTemporaryBytes=0;
};
struct Stft {
  const void* records=nullptr; const double* hann=nullptr; const double* weights=nullptr;
  std::uint32_t k=0,batch=0,bins=0;
  double fs=0,windowEnergy=0,divisor=0;
};
struct Api {
  std::uint32_t abiVersion=version, bytes=sizeof(Api);
  void* (*create)(const Options*,Info*,Failure*) = nullptr;
  void (*destroy)(void*) = nullptr;
  int (*prepare)(void*,const float*,std::uint32_t,std::uint32_t,FirFilter*,Failure*) = nullptr;
  int (*fir)(void*,const FirFilter*,const void*,std::uint32_t,std::int64_t,std::uint64_t,double,double,
             std::uint32_t,float*,void*,Failure*) = nullptr;
  int (*stft)(void*,const Stft*,float*,StftSummary*,Failure*) = nullptr;
  int (*normalize)(void*,const float*,std::uint32_t,float*,double*,Failure*) = nullptr;
  int (*histogram)(void*,const float*,std::uint32_t,const RadixPrefix*,std::uint32_t,std::uint64_t*,Failure*) = nullptr;
  int (*select)(void*,const float*,std::uint32_t,const std::uint64_t*,std::uint32_t,float*,Failure*) = nullptr;
  int (*drain)(void*,Failure*) = nullptr;
  void (*telemetry)(void*,Telemetry*) = nullptr;
};
using GetApi = const Api* (*)(std::uint32_t);
}
