#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/annotation.h"

namespace signalstudio {

enum class DetectionLifecycle {
  Queued,
  Validating,
  Running,
  Cancelling,
  Completed,
  Cancelled,
  Failed,
  Interrupted
};
enum class DetectionSourceValidity { Current, Missing, Changed, Unsupported };
enum class DetectionTimeStrategy { Sample, Roi, Coarse, None };
enum class DetectionCoverageKind {
  Read,
  Eligible,
  Completed,
  Excluded,
  Unread
};
enum class DetectionBackendKind { TensorRt, Scn, Experimental17, Fixture };

struct DetectionParameters {
  double scoreThreshold = .7;
  double nmsIou = .3;
  std::uint32_t topK = 512;
  std::uint32_t batchSize = 2;
  std::uint32_t minHits = 2;
  std::uint32_t maxMiss = 1;
  double mergeFrequencyHz = 100000;
  double bandwidthMarginDb = 6;
  DetectionTimeStrategy timeStrategy = DetectionTimeStrategy::Sample;
  bool splitBursts = true;
  double sampleEdgeRatio = .6;
  double minOnSeconds = .005;
  double minOffSeconds = .010;
  double smoothSeconds = .001;
  double coarseSplitGapSeconds = .020;
  double coarseMinBurstSeconds = .010;
  bool operator==(const DetectionParameters&) const = default;
};

struct DetectionProfile {
  std::string id;
  std::uint32_t version = 1;
  std::string name;
  std::string status;
  DetectionBackendKind backend = DetectionBackendKind::TensorRt;
  std::uint32_t fftSize = 65536;
  std::uint32_t modelFrames = 10;
  std::uint32_t modelHopFrames = 5;
  double hopSeconds = .001;
  bool maximumPsdOnly = false;
  bool supportsReal = false;
};

struct DetectionRequest {
  std::string runId;
  std::optional<std::string> parentRunId;
  SourceIdentity sourceIdentity;
  DetectionProfile profile;
  DetectionParameters parameters;
  ViewRange requestedRange;
  ViewRange contextRange;
  std::string rangeOrigin;
  std::string sourceAnnotationId;
  std::string sourceRevisionId;
  std::string modelManifestPath;
  std::string modelSha256;
  std::uint64_t workingSetBudgetBytes = 256 * 1024 * 1024;
  std::uint64_t evidenceBudgetBytes = 32 * 1024 * 1024;
};

struct DetectionCoverage {
  DetectionCoverageKind kind = DetectionCoverageKind::Unread;
  ViewRange range;
  std::string reason;
};
struct DetectionChunkReference {
  std::uint64_t sequence = 0;
  std::string relativePath;
  std::string sha256;
  std::uint32_t resultCount = 0;
};
struct DetectionRun {
  DetectionRequest request;
  DetectionLifecycle lifecycle = DetectionLifecycle::Queued;
  DetectionSourceValidity sourceValidity = DetectionSourceValidity::Current;
  std::vector<DetectionCoverage> coverage;
  std::vector<DetectionChunkReference> chunks;
  std::uint64_t readFrames = 0;
  std::uint64_t eligibleWindows = 0;
  std::uint64_t completedWindows = 0;
  std::uint64_t resultCount = 0;
  std::uint64_t startedAtMilliseconds = 0;
  std::uint64_t finishedAtMilliseconds = 0;
  std::string error;
  std::string device;
  std::string runtime;
  std::string manifestRelativePath;
  bool recoverable = false;
};

struct DetectionValidation {
  bool ok = true;
  std::string error;
  explicit operator bool() const { return ok; }
};

const std::vector<DetectionProfile>& detectionProfiles();
const DetectionProfile* findDetectionProfile(const std::string& id);
const char* detectionLifecycleName(DetectionLifecycle);
const char* detectionSourceValidityName(DetectionSourceValidity);
const char* detectionCoverageKindName(DetectionCoverageKind);
const char* detectionTimeStrategyName(DetectionTimeStrategy);
bool detectionIsTerminal(DetectionLifecycle);
bool detectionTransitionAllowed(DetectionLifecycle from, DetectionLifecycle to);
DetectionValidation validateDetectionRequest(const DetectionRequest&);
DetectionValidation validateDetectionRun(const DetectionRun&);

}  // namespace signalstudio
