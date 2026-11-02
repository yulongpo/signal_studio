#pragma once

#include <QJsonObject>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "domain/project.h"
#include "infrastructure/raw_sample_reader.h"
#include "infrastructure/detection_profiling.h"
#include "infrastructure/gpu_dsp.h"
#include "infrastructure/mkl_dft.h"

namespace signalstudio {
using DetectionSourceCancel = std::function<bool()>;

struct DetectionSourceConfig {
  // Pure source fixtures remain an explicit CPU reference; production requests
  // enable GPU preference and retain bounded, reported CPU fallback.
  bool preferGpu = false;
  std::uint32_t inputAdaptationVersion = 1;
  std::string profileId = "ffscn-gui-iq-v1";
  std::uint32_t profileVersion = 1;
  std::uint32_t fftSize = 65536, modelFrames = 10, modelHopFrames = 5;
  double hopSeconds = .001;
  std::uint64_t readBlockBytes = 4 * 1024 * 1024;
  std::uint64_t workingSetBudgetBytes = 256 * 1024 * 1024;
  std::uint64_t evidenceBudgetBytes = 32 * 1024 * 1024;
  // Zero together with legacyCombinedRawHeads keeps one window's raw head
  // inside the JSON evidence budget. A new request that does not retain heads
  // sets the flag false so that unsaved .f32 payload is not charged.
  std::uint64_t rawHeadBudgetBytes = 0;
  bool legacyCombinedRawHeads = true;
  std::uint32_t maximumCandidates = 512;
  // Host-resident windows use oneMKL when the runtime is present. GPU upload
  // of those same frames measured slower than the host transform.
  bool useHostMkl = true;
  bool operator==(const DetectionSourceConfig&) const = default;
};

struct DetectionFftSupport {
  TimeRange actualSamples;
  std::uint64_t leadingPadding = 0, trailingPadding = 0;
  bool operator==(const DetectionFftSupport&) const = default;
};

struct DetectionFrameMapping {
  std::uint64_t frameIndex = 0;
  TimeRange slotSamples, actualSamples;
  std::uint64_t validSamples = 0, paddedSamples = 0;
  std::uint64_t powerFloorBins = 0, densityUnderflowGuardBins = 0;
  std::vector<DetectionFftSupport> fftSupports;
  std::string inputHash;
};

struct DetectionSpectrumFrame {
  DetectionFrameMapping mapping;
  double frequencyFirstBinHz = 0, frequencyStepHz = 0;
  std::string units = "dBFS/Hz for normalized ADC; dB(unit^2/Hz) for float";
  std::vector<float> densityDb;
};

struct DetectionSourceExclusion {
  TimeRange samples;
  std::string reason;
  bool operator==(const DetectionSourceExclusion&) const = default;
};

struct DetectionSourcePlan {
  DetectionSourceConfig config;
  SourceIdentity sourceIdentity;
  ViewRange requestedRange;
  TimeRange contextRange;
  std::uint64_t frameOriginSample = 0, hopSamples = 0;
  std::uint64_t realFrameCount = 0, globalWindowCount = 0;
  std::uint32_t transformBins = 0, actualWindowFrames = 0;
  std::uint64_t windowOriginFrame = 0, tailWindowFirstFrame = 0;
  std::uint64_t eligibleWindowCount = 0, firstRegularWindow = 0;
  std::uint64_t eligibleRegularWindows = 0;
  std::uint64_t regularWindowCount = 0;
  std::uint64_t sourceLeadingPadding = 0, sourceTrailingPadding = 0;
  std::uint64_t slotLeadingPadding = 0, slotTrailingPadding = 0;
  std::uint64_t availableTailSamples = 0, estimatedPeakBytes = 0;
  std::uint64_t estimatedWindowEvidenceBytes = 0;
  std::uint64_t modelHeadBytesPerWindow = 0, outputJsonReserveBytes = 65536;
  double frequencyFirstBinHz = 0, frequencyStepHz = 0;
  bool anchoredTailWindow = false, eligibleTailWindow = false;
  bool referenceConfigCompatible = false;
  // Configuration identity alone does not prove complete production L1 parity.
  bool productionProfileCompatible = false;
  std::string gridVersion = "origin0-floor-hop-gui-edge-v1";
  std::string numericalVersion = "cpu-f64-fft-density-log-ddof1-v1";
  std::string numericalPolicy =
      "float32 power floor; float64 density/log and ddof=1 statistics; "
      "finite zero-power guard differs from reference float32 underflow";
  std::string hashMeaning =
      "selected decoded IQ float32 LE and FFT mapping; not a whole-file hash";
  std::vector<DetectionSourceExclusion> exclusions;
  bool operator==(const DetectionSourcePlan&) const = default;
};

struct DetectionModelWindow {
  std::uint64_t globalWindowIndex = 0, firstFrameIndex = 0;
  ViewRange reportRange;
  TimeRange actualContextRange;
  std::vector<DetectionFrameMapping> frames;
  // Row-major [actual frames, analysis bins]; full Fs even for a frequency ROI.
  std::vector<float> densityDb, normalizedTensor;
  double meanDb = 0, sampleStdDb = 0, normalizationStdDb = 0;
  bool epsilonGuardApplied = false;
  bool floorGuardApplied = false;
  bool densityUnderflowGuardApplied = false;
  std::string tensorHash;
};

// Pure integer-grid planning. It does not claim the physical file was opened or
// hashed; open() additionally checks its real sample count and quick identity.
// cancel is polled around validation and inside the evidence walk. A cancelled
// call leaves target unchanged. The default empty callback preserves callers.
bool makeDetectionSourcePlan(const FileMetadata&, ViewRange requested,
                             DetectionSourceConfig, DetectionSourcePlan&,
                             QString& error,
                             std::uint64_t projectGeneration = 0,
                             const DetectionSourceCancel& cancel = {});

// True when plan was built for this source, channel, prefix, requested grid,
// model profile, and config. quickFingerprint still has to be checked by open().
bool detectionPreparedSourcePlanMatches(const FileMetadata& source,
                                        ViewRange requested,
                                        const DetectionSourceConfig& config,
                                        std::uint64_t projectGeneration,
                                        const DetectionSourcePlan& plan);

// Successful plans only. Capacity is fixed. IQ samples and worker state are
// not stored. Identity, prefix, grid, model, and config fields all participate
// in the key; a change misses and plans again.
inline constexpr std::size_t kDetectionSourcePlanCacheCapacity = 8;
struct DetectionSourcePlanCacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t stores = 0;
  std::size_t retained = 0;
  std::size_t capacity = kDetectionSourcePlanCacheCapacity;
};
DetectionSourcePlanCacheStats detectionSourcePlanCacheStatsForTest();
void clearDetectionSourcePlanCacheForTest();
QJsonObject encodeDetectionFrameMapping(const DetectionFrameMapping&);
std::uint32_t detectionExecutionBatchSize(const DetectionSourcePlan&,
                                          std::uint32_t requestedBatch,
                                          std::uint64_t& reservedBytes);

class SpectrumSpool;
struct DetectionFrameCache;

// Full-band source DSP, independent of display-frame reduction and model
// runtime.
class FixedDetectionSource {
 public:
  FixedDetectionSource();
  ~FixedDetectionSource();
  FixedDetectionSource(const FixedDetectionSource&) = delete;
  FixedDetectionSource& operator=(const FixedDetectionSource&) = delete;
  bool withGpu(const DetectionSourceCancel&,
               const std::function<bool(gpu_dsp::Session*)>&);
  // prepared, when non-null and still matching, is copied. File identity,
  // sample count, and the live fingerprint are still checked. A mismatch does
  // not plan a different grid for this call.
  bool open(const FileMetadata&, ViewRange requested, DetectionSourceConfig,
            QString& error, std::uint64_t projectGeneration = 0,
            const DetectionSourceCancel& cancel = {},
            const DetectionSourcePlan* prepared = nullptr);
  const DetectionSourcePlan& plan() const { return plan_; }
  QJsonObject profiling() const {
    auto value = profiling_.snapshot();
    value["gpuDsp"] = gpuProfiling();
    return value;
  }
  QJsonObject gpuProfiling() const;
  bool readFrame(std::uint64_t absoluteFrame, DetectionSpectrumFrame&,
                 QString& error, const DetectionSourceCancel& cancel = {});
  bool readModelWindow(std::uint64_t eligibleWindow, DetectionModelWindow&,
                       QString& error,
                       const DetectionSourceCancel& cancel = {});
  bool streamWindows(const std::function<bool(const DetectionModelWindow&)>&,
                     QString& error, const DetectionSourceCancel& cancel = {});
  // FFSCN frequency analysis keeps its full-frame spool here until close or a
  // failed rebuild. SCN and a zero bandwidth margin do not install one.
  void adoptFrequencySpool(std::unique_ptr<SpectrumSpool> spool);
  void clearFrequencySpool();
  bool frequencySpoolCovers(std::uint64_t firstFrame,
                            std::uint64_t endFrame) const;
  bool replayFrequencyFrame(std::uint64_t absoluteFrame,
                            std::vector<float>& densityDb,
                            TimeRange& slotSamples, bool& ioFailure,
                            QString& error,
                            const DetectionSourceCancel& cancel = {});
  std::uint64_t replayedSpectrumFrames() const {
    return replayedSpectrumFrames_;
  }

 private:
  DetectionFrameMapping frameMapping(std::uint64_t absoluteFrame) const;
  std::uint64_t windowStart(std::uint64_t eligibleWindow) const;
  bool sourceCurrent(QString& error) const;
  bool readGpuFrame(DetectionSpectrumFrame&, QString&,
                    const DetectionSourceCancel&);
  enum class CachedFrameTake { Miss, Hit, Rejected };
  CachedFrameTake takeCachedFrame(std::uint64_t index,
                                  DetectionSpectrumFrame& target,
                                  QString& error,
                                  const DetectionSourceCancel& cancel);
  void rememberFrame(std::uint64_t index, const DetectionSpectrumFrame& frame);
  void clearFrameCache();
  std::uint64_t frameCacheBudget() const;
  DetectionSourcePlan plan_;
  std::unique_ptr<RawSampleReader> reader_;
  QString path_, fingerprint_;
  std::vector<double> hann_;
  double hannEnergy_ = 0;
  DetectionProfiler profiling_;
  DetectionSourceCancel gpuCancel_;
  gpu_dsp::Capabilities gpuCapabilities_;
  std::string gpuFallback_;
  std::uint64_t gpuFrames_ = 0, gpuWindows_ = 0, cpuFrames_ = 0, cpuWindows_ = 0;
  std::uint64_t mklFrames_ = 0;
  std::string stftRoute_ = "unselected";
  std::string stftSuppressedReason_;
  bool modelWindowStftLocked_ = false;
  bool suppressGpuFrame_ = false;
  bool hostMklFrame_ = false;
  void notePlainStftRoute(bool usedGpu);
  MklDft mkl_;
  std::unique_ptr<SpectrumSpool> frequencySpool_;
  std::unique_ptr<DetectionFrameCache> frameCache_;
  std::uint64_t replayedSpectrumFrames_ = 0;
  // Last member: destroyed before the stable cancellation state it references.
  std::unique_ptr<gpu_dsp::Session> gpu_;
};
}  // namespace signalstudio
