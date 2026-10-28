#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "domain/detection.h"

namespace signalstudio {

// Each power value represents one half-open source-sample cell. The final cell
// is clipped to supportEndSample. Values are band-limited linear power, not dB.
struct DetectionEnergySeries {
  SampleIndex originSample = 0;
  SampleIndex stepSamples = 1;
  SampleIndex supportEndSample = 0;
  SampleIndex filterSupportSamples = 0;
  std::vector<float> power;
  std::string energyMethod;
};

// Row-major time x frequency. Time cells are physical source slots, distinct
// from the longer FFT supports. Frequency edges use the file's RF coordinate.
struct DetectionSpectrumRoi {
  std::vector<TimeRange> timeSlots;
  std::vector<double> frequencyEdgesHz;
  std::vector<float> powerDb;
  SampleIndex fftSupportUncertaintySamples = 0;
};

struct DetectionRefinementInput {
  std::string candidateId;
  ViewRange observation;
  ViewRange publishBounds;
  double sampleRateHz = 0;
  // Unique hit windows / the frozen total analysis-window count. A track at
  // least 70% covered uses continuous-carrier boundary refinement.
  double windowCoverage = 0;
  std::vector<Segment> fallbackActivity;
  std::optional<DetectionEnergySeries> energy;
  std::optional<DetectionSpectrumRoi> spectrum;
};

struct DetectionRefinementOptions {
  double sampleNoiseQuantile = .005;
  double sampleBurstQuantile = .5;
  double sampleMinimumBurstNoiseRatio = 1.3;
  double sampleMinimumOnFraction = .03;
  double sampleMinimumRunSeconds = .0005;
  std::size_t roiFrequencyRows = 512;
  std::size_t roiTimeColumns = 64;
  std::size_t workingCellBudget = 4 * 1024 * 1024;
  // Caller-owned result grant; the historical default preserves the domain
  // run limit. Reject before growing activity output, never truncate it.
  std::size_t maximumActivitySegments = annotationSegmentBudget;
  // Checked between bounded stages and every 4096 sample cells/coarse frames.
  std::function<bool()> cancelled;
};

struct DetectionRefinementEvidence {
  // Float32 power/smoothed storage and NumPy 2.4 scalar percentile
  // interpolation, with a float64 rolling sum instead of torch float32
  // cumulative subtraction. Boundary parity must be measured on the actual
  // source.
  std::string numericalVersion = "signalstudio-refinement-m0p-parameters-v2";
  std::string energyMethod;
  std::optional<double> noise;
  std::optional<double> burst;
  std::optional<double> threshold;
  std::optional<double> comparisonThreshold;
  std::optional<double> onFraction;
  // Gates compare mean positive residuals after subtracting the ROI dB mean.
  // These are neither calibrated dB power nor the sample energy threshold.
  std::optional<double> roiPositiveMean;
  std::optional<double> roiOffFraction;
  std::optional<double> roiTimeThreshold;
  std::optional<double> roiFrequencyThreshold;
  SampleIndex sourceOriginSample = 0;
  SampleIndex sourceStepSamples = 0;
  SampleIndex sourceSupportEndSample = 0;
  SampleIndex smoothingSamples = 0;
  SampleIndex filterSupportSamples = 0;
  SampleIndex fftSupportUncertaintySamples = 0;
  SampleIndex quantizationUncertaintySamples = 0;
  SampleIndex minOnSamples = 0;
  SampleIndex minOffSamples = 0;
  bool fineParametersApplied = false;
  bool durationParametersApplied = false;
  std::size_t roiSmoothingCells = 0;
  bool coarseParametersApplied = false;
  bool splittingApplied = false;
  bool continuousTrack = false;
  double effectiveEdgeRatio = 0;
  double effectiveMinOnSeconds = 0;
  double effectiveMinOffSeconds = 0;
  double effectiveSmoothSeconds = 0;
  double effectiveCoarseMinBurstSeconds = 0;
  double effectiveCoarseSplitGapSeconds = 0;
};

// ResourceLimit may drop one measurement and keep a labelled fallback.
// WorkingCellLimit, HardFailure and Cancelled must not publish activity.
enum class DetectionFailureCode : int {
  None = 0,
  ResourceLimit = 1,
  HardFailure = 2,
  Cancelled = 3,
  WorkingCellLimit = 4,
};

struct DetectionRefinementResult {
  bool ok = true;
  std::string error;
  DetectionFailureCode failureCode = DetectionFailureCode::None;
  RefineMethod method = RefineMethod::None;
  RefineStatus status = RefineStatus::Skipped;
  ViewRange observation;
  std::vector<Segment> activity;
  std::string fallbackReason;
  DetectionRefinementEvidence evidence;
};

DetectionRefinementResult refineDetection(
    const DetectionRefinementInput& input,
    const DetectionParameters& parameters,
    const DetectionRefinementOptions& options = {});

// Fine branches disable Coarse controls. Their measured fallback uses fixed
// Coarse defaults while preserving the active split setting and stored values.
DetectionParameters coarseDetectionFallbackParameters(
    const DetectionParameters& parameters);

struct DetectionWindowScore {
  std::uint64_t windowId = 0;
  double score = 0;
};
struct DetectionWindowEvidence {
  bool ok = true;
  std::string error;
  std::vector<std::uint64_t> windowIds;
  std::uint64_t totalAnalysisWindows = 0;
  std::optional<double> scoreMean;
  std::optional<double> scoreMax;
  double coverage = 0;
};

// Duplicate detections in one window contribute only the maximum score once.
// The denominator is the caller's frozen total analysis-window count. Coverage
// describes window evidence, never signal duty cycle or task progress.
DetectionWindowEvidence summarizeDetectionWindows(
    const std::vector<DetectionWindowScore>& scores,
    std::uint64_t totalAnalysisWindows);

struct DetectionEvaluationEvent {
  std::string id;
  FrequencyRange frequency;
  std::optional<TimeRange> activity;
};
enum class DetectionEvaluationDimension { TimeFrequency, Frequency, Time };
enum class DetectionEvaluationFileState { Completed, Failed, NotProcessed };

struct DetectionEvaluationMatch {
  std::size_t predictionIndex = 0;
  std::size_t truthIndex = 0;
  double iou = 0;
  double timeIou = 0;
  double frequencyIou = 0;
  SampleIndex startAbsoluteErrorSamples = 0;
  SampleIndex endAbsoluteErrorSamples = 0;
  double startAbsoluteErrorSeconds = 0;
  double endAbsoluteErrorSeconds = 0;
  double centerAbsoluteErrorHz = 0;
  double bandwidthAbsoluteErrorHz = 0;
};

struct DetectionEvaluationMetric {
  double threshold = 0;
  std::uint64_t truePositive = 0;
  std::uint64_t falsePositive = 0;
  std::uint64_t falseNegative = 0;
  double precision = 0;
  double recall = 0;
  double f1 = 0;
  std::vector<DetectionEvaluationMatch> matches;
  std::optional<double> startErrorMedianSeconds;
  std::optional<double> startErrorP95Seconds;
  std::optional<double> endErrorMedianSeconds;
  std::optional<double> endErrorP95Seconds;
  std::optional<double> centerErrorMeanHz;
  std::optional<double> bandwidthErrorMeanHz;
};

struct DetectionEvaluationResult {
  bool ok = true;
  std::string error;
  std::string evaluatorVersion = "signalstudio-threshold-cardinality-iou-v1";
  std::uint64_t predictionCount = 0;
  std::uint64_t truthCount = 0;
  std::uint64_t unknownTimePredictions = 0;
  double unknownTimeFraction = 0;
  std::uint64_t splitTruthCount = 0;
  std::uint64_t mergedPredictionCount = 0;
  std::vector<DetectionEvaluationMetric> metrics;
};

// Unknown-time predictions remain in the denominator and have zero TF/time
// IoU. Truth must have time for TF/time evaluation; invalid truth is an error.
double detectionEventIoU(const DetectionEvaluationEvent& prediction,
                         const DetectionEvaluationEvent& truth,
                         DetectionEvaluationDimension dimension);
DetectionEvaluationResult evaluateDetections(
    const std::vector<DetectionEvaluationEvent>& predictions,
    const std::vector<DetectionEvaluationEvent>& truth, double sampleRateHz,
    const std::vector<double>& thresholds = {.3, .5, .7},
    DetectionEvaluationDimension dimension =
        DetectionEvaluationDimension::TimeFrequency,
    std::size_t matrixCellBudget = 4 * 1024 * 1024);

struct DetectionEvaluationFile {
  std::string id;
  DetectionEvaluationFileState state = DetectionEvaluationFileState::Completed;
  std::string failureReason;
  double sampleRateHz = 0;
  std::vector<DetectionEvaluationEvent> predictions;
  std::vector<DetectionEvaluationEvent> truth;
};
struct DetectionEvaluationFileResult {
  std::string id;
  DetectionEvaluationFileState state;
  std::string failureReason;
  DetectionEvaluationResult result;
};
struct DetectionDatasetEvaluation {
  bool ok = true;
  std::string error;
  std::vector<DetectionEvaluationFileResult> files;
  DetectionEvaluationResult overall;
};

// Failed/unprocessed files retain all truth and zero predictions. Matches are
// computed within each file at each threshold, then micro-aggregated.
DetectionDatasetEvaluation evaluateDetectionDataset(
    const std::vector<DetectionEvaluationFile>& files,
    const std::vector<double>& thresholds = {.3, .5, .7},
    DetectionEvaluationDimension dimension =
        DetectionEvaluationDimension::TimeFrequency,
    std::size_t matrixCellBudget = 4 * 1024 * 1024);

}  // namespace signalstudio
