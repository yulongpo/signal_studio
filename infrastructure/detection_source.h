#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

#include "domain/project.h"
#include "infrastructure/raw_sample_reader.h"
#include "infrastructure/detection_profiling.h"

namespace signalstudio {
using DetectionSourceCancel = std::function<bool()>;

struct DetectionSourceConfig {
  std::uint32_t inputAdaptationVersion = 1;
  std::string profileId = "ffscn-gui-iq-v1";
  std::uint32_t profileVersion = 1;
  std::uint32_t fftSize = 65536, modelFrames = 10, modelHopFrames = 5;
  double hopSeconds = .001;
  std::uint64_t readBlockBytes = 4 * 1024 * 1024;
  std::uint64_t workingSetBudgetBytes = 256 * 1024 * 1024;
  std::uint64_t evidenceBudgetBytes = 32 * 1024 * 1024;
  std::uint32_t maximumCandidates = 512;
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
bool makeDetectionSourcePlan(const FileMetadata&, ViewRange requested,
                             DetectionSourceConfig, DetectionSourcePlan&,
                             QString& error,
                             std::uint64_t projectGeneration = 0);
QJsonObject encodeDetectionFrameMapping(const DetectionFrameMapping&);
std::uint32_t detectionExecutionBatchSize(const DetectionSourcePlan&,
                                          std::uint32_t requestedBatch,
                                          std::uint64_t& reservedBytes);

// Full-band source DSP, independent of display-frame reduction and model
// runtime.
class FixedDetectionSource {
 public:
  bool open(const FileMetadata&, ViewRange requested, DetectionSourceConfig,
            QString& error, std::uint64_t projectGeneration = 0);
  const DetectionSourcePlan& plan() const { return plan_; }
  QJsonObject profiling() const { return profiling_.snapshot(); }
  bool readFrame(std::uint64_t absoluteFrame, DetectionSpectrumFrame&,
                 QString& error, const DetectionSourceCancel& cancel = {});
  bool readModelWindow(std::uint64_t eligibleWindow, DetectionModelWindow&,
                       QString& error,
                       const DetectionSourceCancel& cancel = {});
  bool streamWindows(const std::function<bool(const DetectionModelWindow&)>&,
                     QString& error, const DetectionSourceCancel& cancel = {});

 private:
  DetectionFrameMapping frameMapping(std::uint64_t absoluteFrame) const;
  std::uint64_t windowStart(std::uint64_t eligibleWindow) const;
  bool sourceCurrent(QString& error) const;
  DetectionSourcePlan plan_;
  std::unique_ptr<RawSampleReader> reader_;
  QString path_, fingerprint_;
  std::vector<double> hann_;
  double hannEnergy_ = 0;
  DetectionProfiler profiling_;
};
}  // namespace signalstudio
