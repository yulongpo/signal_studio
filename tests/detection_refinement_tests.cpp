#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "domain/detection_refinement.h"

using namespace signalstudio;

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance = 1e-6) {
  return std::abs(a - b) <= tolerance;
}

DetectionParameters fineParameters() {
  DetectionParameters parameters;
  parameters.smoothSeconds = 0;
  parameters.minOnSeconds = .003;
  parameters.minOffSeconds = .005;
  return parameters;
}

DetectionRefinementInput energyInput(std::size_t count = 70,
                                     SampleIndex origin = 100,
                                     SampleIndex step = 1) {
  DetectionRefinementInput input;
  input.candidateId = "candidate";
  input.sampleRateHz = 1000 * static_cast<double>(step);
  input.observation = {{origin, origin + count * step}, {100, 116}};
  input.publishBounds = input.observation;
  input.energy = DetectionEnergySeries{origin,
                                       step,
                                       origin + count * step,
                                       7,
                                       std::vector<float>(count),
                                       "independent-test-power"};
  return input;
}

void turnOn(DetectionRefinementInput& input, std::size_t begin, std::size_t end,
            float power = 10) {
  std::fill(input.energy->power.begin() + begin,
            input.energy->power.begin() + end, power);
}

DetectionRefinementInput spectrumInput(std::size_t times = 20,
                                       std::size_t frequencies = 16) {
  DetectionRefinementInput input;
  input.candidateId = "spectral-candidate";
  input.sampleRateHz = 1000;
  input.observation = {{100, 100 + times}, {100, 100. + frequencies}};
  input.publishBounds = input.observation;
  DetectionSpectrumRoi spectrum;
  for (std::size_t i = 0; i < times; ++i)
    spectrum.timeSlots.push_back({100 + i, 101 + i});
  for (std::size_t i = 0; i <= frequencies; ++i)
    spectrum.frequencyEdgesHz.push_back(100. + i);
  spectrum.powerDb.assign(times * frequencies, -100);
  spectrum.fftSupportUncertaintySamples = 65;
  input.spectrum = std::move(spectrum);
  return input;
}

void spectralOn(DetectionRefinementInput& input, std::size_t begin,
                std::size_t end, std::size_t f0 = 4, std::size_t f1 = 12) {
  const auto frequencies = input.spectrum->frequencyEdgesHz.size() - 1;
  for (std::size_t t = begin; t < end; ++t)
    for (auto f = f0; f < f1; ++f)
      input.spectrum->powerDb[t * frequencies + f] = 0;
}

void fineFilteringPrecedesGapMerging() {
  auto input = energyInput(40);
  turnOn(input, 4, 6);
  turnOn(input, 7, 30);
  const auto result = refineDetection(input, fineParameters());
  check(result.ok && result.status == RefineStatus::Applied &&
            result.activity.size() == 1 &&
            result.activity[0].range.time == TimeRange{107, 130},
        "Fine minOn must remove a short onset before merging a small gap");
  check(result.evidence.fineParametersApplied &&
            !result.evidence.coarseParametersApplied &&
            result.evidence.minOnSamples == 3 &&
            result.evidence.minOffSamples == 5,
        "Only the fine duration parameters may be marked effective");
}

void fineGapsSplittingAndShortBursts() {
  auto input = energyInput(60);
  turnOn(input, 5, 25);
  turnOn(input, 30, 55);
  auto parameters = fineParameters();
  parameters.coarseMinBurstSeconds = .060;
  parameters.coarseSplitGapSeconds = .060;
  auto result = refineDetection(input, parameters);
  check(result.ok && result.activity.size() == 2 &&
            result.activity[0].range.time == TimeRange{105, 125} &&
            result.activity[1].range.time == TimeRange{130, 155},
        "A gap exactly minOff must remain split and coarse parameters ignored");
  parameters.minOffSeconds = .006;
  result = refineDetection(input, parameters);
  check(result.activity.size() == 1 &&
            result.activity[0].range.time == TimeRange{105, 155},
        "A gap strictly shorter than minOff must merge");
  parameters = fineParameters();
  parameters.splitBursts = false;
  result = refineDetection(input, parameters);
  check(result.activity.size() == 1 &&
            result.activity[0].range.time == TimeRange{105, 155} &&
            !result.evidence.splittingApplied,
        "Disabling split retains first-to-last boundary shrinkage");
  input = energyInput();
  turnOn(input, 5, 25);
  turnOn(input, 30, 34);
  turnOn(input, 40, 60);
  parameters = fineParameters();
  parameters.minOffSeconds = .003;
  parameters.coarseMinBurstSeconds = .060;
  result = refineDetection(input, parameters);
  check(result.activity.size() == 3 &&
            result.activity[1].range.time == TimeRange{130, 134},
        "Fine refinement must preserve a 4 ms burst below coarse minBurst");
}

void causalSmoothingAndContinuousTracks() {
  auto input = energyInput(10);
  turnOn(input, 0, 10);
  auto parameters = fineParameters();
  parameters.smoothSeconds = .004;
  const auto result = refineDetection(input, parameters);
  check(result.ok && result.activity.size() == 1 &&
            result.activity[0].range.time == TimeRange{102, 110} &&
            near(*result.evidence.noise, 2.6125) &&
            *result.evidence.burst == 10 &&
            result.evidence.smoothingSamples == 4,
        "Causal mean must include left zero padding and all four samples");
  input = energyInput(60);
  turnOn(input, 5, 25);
  turnOn(input, 30, 55);
  input.windowCoverage = .7;
  parameters = fineParameters();
  parameters.sampleEdgeRatio = .9;
  const auto continuous = refineDetection(input, parameters);
  check(continuous.activity.size() == 1 &&
            continuous.activity[0].range.time == TimeRange{105, 155} &&
            near(continuous.evidence.effectiveEdgeRatio, .75) &&
            continuous.evidence.continuousTrack &&
            !continuous.evidence.splittingApplied,
        "Continuous edge scaling preserves a working edge-ratio parameter");
}

void physicalCellMappingAndClipping() {
  constexpr SampleIndex origin = (SampleIndex{1} << 53) + 9;
  auto input = energyInput(10, origin, 10);
  input.energy->supportEndSample -= 5;
  input.observation.time.end -= 5;
  input.publishBounds = {{origin + 25, origin + 93}, {101, 115}};
  turnOn(input, 2, 10);
  const auto result = refineDetection(input, fineParameters());
  check(result.ok && result.activity.size() == 1 &&
            result.activity[0].range == input.publishBounds &&
            result.activity[0].clippedAtRoi &&
            result.evidence.sourceOriginSample == origin &&
            result.evidence.sourceStepSamples == 10 &&
            result.evidence.sourceSupportEndSample == origin + 95 &&
            result.evidence.filterSupportSamples == 7,
        "Half-open source cells must retain uint64 precision and clipped final "
        "support");
}

void fallbackAndNoneNeverInventActivity() {
  auto input = energyInput();
  std::fill(input.energy->power.begin(), input.energy->power.end(), 1.f);
  Segment coarse;
  coarse.id = "original-coarse";
  coarse.range = {{110, 160}, {102, 114}};
  coarse.timeQuality = TimeQuality::WindowObservation;
  input.fallbackActivity = {coarse};
  auto parameters = fineParameters();
  auto result = refineDetection(input, parameters);
  check(
      result.ok && result.status == RefineStatus::Fallback &&
          result.fallbackReason == "sample-burst-noise-ratio-too-low" &&
          result.activity == input.fallbackActivity,
      "Unreliable sample evidence must preserve the supplied coarse fallback");
  input.fallbackActivity.clear();
  result = refineDetection(input, parameters);
  check(result.activity.empty() && result.status == RefineStatus::Fallback,
        "An observation with no fallback activity must stay unknown-time");
  input.fallbackActivity = {coarse};
  parameters.timeStrategy = DetectionTimeStrategy::None;
  result = refineDetection(input, parameters);
  check(result.ok && result.method == RefineMethod::None &&
            result.status == RefineStatus::Skipped && result.activity.empty() &&
            !result.evidence.fineParametersApplied &&
            !result.evidence.coarseParametersApplied,
        "None must never turn a window observation into an invented segment");
}

void roiBilinearPoolingAndSourceMapping() {
  auto input = spectrumInput();
  spectralOn(input, 2, 9);
  spectralOn(input, 12, 18);
  auto parameters = fineParameters();
  parameters.timeStrategy = DetectionTimeStrategy::Roi;
  parameters.smoothSeconds = .001;
  parameters.minOnSeconds = .002;
  parameters.minOffSeconds = .002;
  parameters.coarseMinBurstSeconds = .060;
  const auto result = refineDetection(input, parameters);
  check(result.ok && result.status == RefineStatus::Applied &&
            result.activity.size() == 2 &&
            result.activity[0].range.time == TimeRange{102, 109} &&
            result.activity[1].range.time == TimeRange{112, 118} &&
            result.activity[0].range.frequency == FrequencyRange{104, 112},
        "512x64 align_corners=false bilinear pooling must map half-open edges "
        "to physical cells");
  check(result.evidence.fftSupportUncertaintySamples == 65 &&
            result.activity[0].uncertaintySamples == 66 &&
            result.evidence.fineParametersApplied &&
            !result.evidence.coarseParametersApplied,
        "ROI FFT support and source-cell uncertainty must remain explicit");
  input.windowCoverage = .8;
  const auto continuous = refineDetection(input, parameters);
  check(continuous.activity.size() == 1 &&
            continuous.activity.front().range.time == TimeRange{102, 118},
        "Continuous ROI tracks must span first-to-last mask run");
}

void roiGatesRecordTheMeasuredResidualDomain() {
  auto input = spectrumInput(64, 512);
  spectralOn(input, 16, 48, 128, 384);
  auto parameters = fineParameters();
  parameters.timeStrategy = DetectionTimeStrategy::Roi;
  parameters.smoothSeconds = .001;
  const auto result = refineDetection(input, parameters);
  // One quarter of the exact 512x64 grid is 0 dB, the rest -100 dB.
  // Mean subtraction leaves +75/-25; only the +75 quarter remains positive.
  check(result.ok && result.status == RefineStatus::Applied &&
            result.evidence.roiPositiveMean == std::optional<double>{75} &&
            result.evidence.roiOffFraction == std::optional<double>{.75} &&
            result.evidence.roiTimeThreshold == std::optional<double>{28.125} &&
            result.evidence.roiFrequencyThreshold ==
                std::optional<double>{14.0625} &&
            result.activity.front().range == ViewRange{{116, 148}, {228, 484}},
        "ROI gates must preserve independently known residual measurements "
        "and the same physical activity");
  check(!result.evidence.noise && !result.evidence.comparisonThreshold,
        "ROI residual gates cannot masquerade as sample power measurements");
}

void approvedPrototypeParametersApply() {
  auto input = energyInput(60);
  turnOn(input, 5, 25);
  turnOn(input, 30, 55);
  auto parameters = fineParameters();
  parameters.splitBursts = false;
  parameters.minOnSeconds = .060;
  parameters.minOffSeconds = .060;
  const auto single = refineDetection(input, parameters);
  check(single.ok && single.activity.size() == 1 &&
            single.activity.front().range.time == TimeRange{105, 155} &&
            !single.evidence.durationParametersApplied &&
            single.evidence.effectiveMinOffSeconds == 0,
        "Disabled min on/off must not remove or shift the no-split result");
  input = spectrumInput();
  spectralOn(input, 2, 9);
  spectralOn(input, 12, 18);
  parameters = fineParameters();
  parameters.timeStrategy = DetectionTimeStrategy::Roi;
  parameters.minOnSeconds = .002;
  parameters.minOffSeconds = .002;
  parameters.smoothSeconds = .001;
  const auto baseline = refineDetection(input, parameters);
  parameters.smoothSeconds = .013;
  const auto smoothed = refineDetection(input, parameters);
  check(baseline.ok && smoothed.ok &&
            baseline.evidence.roiSmoothingCells == 5 &&
            smoothed.evidence.roiSmoothingCells == 63 &&
            baseline.activity != smoothed.activity,
        "ROI smoothing must alter its measured mask and report clamped cells");
  parameters.smoothSeconds = .001;
  parameters.sampleEdgeRatio = 0;
  const auto lowThreshold = refineDetection(input, parameters);
  check(lowThreshold.ok && lowThreshold.evidence.effectiveEdgeRatio == 0 &&
            baseline.activity != lowThreshold.activity,
        "ROI edge ratio must alter its adaptive threshold and physical edges");
}

void roiSparseRealFramesFallback() {
  auto input = spectrumInput(2);
  spectralOn(input, 0, 2);
  auto parameters = fineParameters();
  parameters.timeStrategy = DetectionTimeStrategy::Roi;
  auto result = refineDetection(input, parameters);
  check(result.ok && result.status == RefineStatus::Fallback &&
            result.activity.empty() &&
            result.fallbackReason == "fewer-than-three-real-spectrum-frames",
        "Interpolated ROI columns must not masquerade as three real frames");
  input = spectrumInput(20);
  spectralOn(input, 0, 1);
  parameters.minOnSeconds = 0;
  parameters.minOffSeconds = 0;
  result = refineDetection(input, parameters);
  check(
      result.ok && result.status == RefineStatus::Fallback &&
          result.activity.empty() &&
          result.fallbackReason ==
              "roi-runs-map-to-fewer-than-three-real-frames",
      "Every run mapping to fewer than three real frames must retain fallback");
}

void coarseOwnsItsDurationParameters() {
  auto input = spectrumInput(30, 40);
  spectralOn(input, 5, 8, 4, 6);
  spectralOn(input, 9, 13, 4, 6);
  spectralOn(input, 20, 26, 4, 6);
  auto parameters = fineParameters();
  parameters.timeStrategy = DetectionTimeStrategy::Coarse;
  parameters.coarseSplitGapSeconds = .002;
  parameters.coarseMinBurstSeconds = .006;
  parameters.minOnSeconds = .060;
  parameters.minOffSeconds = .060;
  auto result = refineDetection(input, parameters);
  check(result.ok && result.activity.size() == 2 &&
            result.activity[0].range.time == TimeRange{105, 113} &&
            result.activity[1].range.time == TimeRange{120, 126} &&
            result.evidence.coarseParametersApplied &&
            !result.evidence.fineParametersApplied,
        "Coarse must merge short gaps before minimum-burst filtering using its "
        "own parameters");
  input.windowCoverage = .8;
  result = refineDetection(input, parameters);
  check(result.activity.size() == 2 && result.evidence.splittingApplied,
        "The continuous-carrier override applies only to sample and ROI");
  parameters.splitBursts = false;
  parameters.coarseMinBurstSeconds = .060;
  result = refineDetection(input, parameters);
  check(result.activity.size() == 1 &&
            result.activity[0].range.time == TimeRange{105, 126} &&
            result.evidence.effectiveCoarseMinBurstSeconds == 0,
        "Disabling coarse split must keep boundary shrinkage and disable burst "
        "rejection");
}

void fineFallbackIgnoresInactiveCoarseHistory() {
  auto input = spectrumInput(200, 8);
  input.energy = DetectionEnergySeries{100,
                                       1,
                                       300,
                                       7,
                                       std::vector<float>(200, 1),
                                       "constant-unreliable-test-power"};
  spectralOn(input, 20, 35, 3, 4);
  spectralOn(input, 47, 62, 3, 4);
  const auto defaults = refineDetection(
      input, coarseDetectionFallbackParameters(DetectionParameters{}));
  check(defaults.ok && defaults.activity.size() == 1 &&
            defaults.activity.front().range.time == TimeRange{120, 162},
        "Measured default fallback must merge the 12 ms gap");
  for (const auto strategy :
       {DetectionTimeStrategy::Sample, DetectionTimeStrategy::Roi}) {
    auto stored = DetectionParameters{};
    stored.timeStrategy = strategy;
    stored.bandwidthMarginDb = 12;
    stored.minOnSeconds = .050;
    stored.coarseSplitGapSeconds = .010;
    stored.coarseMinBurstSeconds = .050;
    const auto before = stored;
    check(refineDetection(input, stored).status == RefineStatus::Fallback,
          "The Fine fixture must actually require measured fallback");
    const auto fallback =
        refineDetection(input, coarseDetectionFallbackParameters(stored));
    check(fallback.ok && fallback.activity == defaults.activity &&
              stored == before,
          "Fine fallback must ignore inactive history and preserve storage");
    stored.timeStrategy = DetectionTimeStrategy::Coarse;
    const auto direct = refineDetection(input, stored);
    check(direct.ok && direct.activity.empty(),
          "Direct Coarse must retain its active minimum-burst rejection");
    stored.splitBursts = false;
    const auto single =
        refineDetection(input, coarseDetectionFallbackParameters(stored));
    check(single.ok && single.activity == defaults.activity &&
              !single.evidence.splittingApplied &&
              single.evidence.effectiveCoarseMinBurstSeconds == 0 &&
              single.evidence.effectiveCoarseSplitGapSeconds == 0,
          "Fallback must preserve noSplit and ignore duration filtering");
  }
}

void invalidInputsBudgetsAndCancellation() {
  auto input = energyInput();
  turnOn(input, 5, 60);
  auto options = DetectionRefinementOptions{};
  options.workingCellBudget = 69;
  check(!refineDetection(input, fineParameters(), options).ok,
        "Sample working-cell budget must be enforced before allocation");
  options = {};
  options.cancelled = [] { return true; };
  const auto cancelled = refineDetection(input, fineParameters(), options);
  check(!cancelled.ok && cancelled.activity.empty() &&
            cancelled.error == "Refinement cancelled",
        "Cancelled refinement must not publish partial activity");
  input.energy->power[0] = std::numeric_limits<float>::quiet_NaN();
  check(!refineDetection(input, fineParameters()).ok,
        "Nonfinite sample power must be rejected strictly");
  input = spectrumInput();
  input.spectrum->powerDb[0] = std::numeric_limits<float>::infinity();
  auto parameters = fineParameters();
  parameters.timeStrategy = DetectionTimeStrategy::Roi;
  check(!refineDetection(input, parameters).ok,
        "Nonfinite ROI power must be rejected strictly");
  input = spectrumInput();
  input.spectrum->timeSlots[3].begin += 1;
  check(!refineDetection(input, parameters).ok,
        "Spectrum cells may not overlap, gap, or have invented source slots");
  input = spectrumInput();
  options = {};
  options.roiFrequencyRows = std::numeric_limits<std::size_t>::max();
  check(!refineDetection(input, parameters, options).ok,
        "Resized ROI shape overflow must fail before allocation");
}

DetectionEvaluationEvent event(std::string id, SampleIndex begin,
                               SampleIndex end, double lower = 0,
                               double upper = 10) {
  return {std::move(id), {lower, upper}, TimeRange{begin, end}};
}

void thresholdCardinalityBeforeIoU() {
  const std::vector<DetectionEvaluationEvent> predictions{event("P1", 0, 10),
                                                          event("P2", 0, 4)};
  const std::vector<DetectionEvaluationEvent> truth{event("T1", 0, 10),
                                                    event("T2", 6, 10)};
  const auto result = evaluateDetections(predictions, truth, 1000, {.3, .5});
  check(result.ok && result.metrics[0].truePositive == 2 &&
            result.metrics[0].matches[0].truthIndex == 1 &&
            result.metrics[0].matches[1].truthIndex == 0 &&
            result.metrics[1].truePositive == 1,
        "Per-threshold cardinality must beat the higher total-IoU single valid "
        "pair");
  const auto tie =
      evaluateDetections({event("P1", 0, 10), event("P2", 0, 8)},
                         {event("T1", 0, 10), event("T2", 0, 7)}, 1000, {.3});
  check(tie.ok && tie.metrics[0].matches[0].truthIndex == 0 &&
            tie.metrics[0].matches[1].truthIndex == 1 &&
            near(tie.metrics[0].matches[1].iou, .875),
        "Among equal cardinalities, one-to-one assignment must maximize summed "
        "IoU");
}

void unknownTimeAndPreciseErrorMetrics() {
  constexpr SampleIndex origin = (SampleIndex{1} << 53) + 1;
  auto known = event("known", origin + 1, origin + 10, 2, 12);
  DetectionEvaluationEvent unknown{"unknown", {0, 10}, std::nullopt};
  const auto target = event("truth", origin, origin + 10);
  const auto result =
      evaluateDetections({known, unknown}, {target}, 1000, {.5});
  check(result.ok && result.predictionCount == 2 &&
            result.unknownTimePredictions == 1 &&
            result.unknownTimeFraction == .5 &&
            result.metrics[0].truePositive == 1 &&
            result.metrics[0].falsePositive == 1 &&
            result.metrics[0].precision == .5 &&
            result.metrics[0].recall == 1 &&
            result.metrics[0].matches[0].startAbsoluteErrorSamples == 1 &&
            near(*result.metrics[0].startErrorP95Seconds, .001),
        "Unknown-time predictions stay in denominators and sample error uses "
        "integer subtraction");
  check(detectionEventIoU(unknown, target,
                          DetectionEvaluationDimension::TimeFrequency) == 0 &&
            detectionEventIoU(unknown, target,
                              DetectionEvaluationDimension::Time) == 0 &&
            detectionEventIoU(unknown, target,
                              DetectionEvaluationDimension::Frequency) == 1,
        "Unknown-time geometry must have zero TF/time IoU while retaining "
        "frequency evidence");
}

void filesNeverCrossMatchAndFailuresRemainDenominator() {
  const auto identical = event("event", 0, 10);
  std::vector<DetectionEvaluationFile> files{
      {"A", DetectionEvaluationFileState::Completed, "", 1000, {identical}, {}},
      {"B", DetectionEvaluationFileState::Completed, "", 1000, {}, {identical}},
      {"C",
       DetectionEvaluationFileState::Failed,
       "read-failed",
       1000,
       {identical},
       {identical}},
      {"D",
       DetectionEvaluationFileState::NotProcessed,
       "cancelled",
       1000,
       {identical},
       {identical}}};
  auto result = evaluateDetectionDataset(files, {.5});
  check(result.ok && result.files.size() == 4 &&
            result.overall.predictionCount == 1 &&
            result.overall.truthCount == 3 &&
            result.overall.metrics[0].truePositive == 0 &&
            result.overall.metrics[0].falsePositive == 1 &&
            result.overall.metrics[0].falseNegative == 3 &&
            result.files[2].failureReason == "read-failed",
        "Across-file matches are forbidden and failed/unprocessed files "
        "contribute all truth and zero predictions");
  files[1].predictions = {identical};
  result = evaluateDetectionDataset(files, {.5});
  check(result.overall.metrics[0].truePositive == 1 &&
            near(result.overall.metrics[0].precision, .5) &&
            near(result.overall.metrics[0].recall, 1. / 3),
        "Dataset metrics must micro-aggregate per-file assignments");
}

void evaluatorValidationAndSplitMergeTopology() {
  const auto result =
      evaluateDetections({event("merged", 0, 20), event("split", 0, 5)},
                         {event("T1", 0, 10), event("T2", 10, 20)}, 1000, {.3});
  check(result.ok && result.splitTruthCount == 1 &&
            result.mergedPredictionCount == 1,
        "Split/merge counts must describe positive-overlap event topology");
  const auto e = event("event", 0, 10);
  check(!evaluateDetections({e, e}, {e, e}, 1000, {.5},
                            DetectionEvaluationDimension::TimeFrequency, 3)
             .ok,
        "Evaluation matrix-cell budget must reject an oversized product");
  check(!evaluateDetections({e}, {e}, 1000, {0}).ok &&
            !evaluateDetections({e}, {e}, 1000,
                                {std::numeric_limits<double>::quiet_NaN()})
                 .ok,
        "Invalid operating thresholds must fail before assignment");
  auto malformed = e;
  malformed.frequency.upperHz = std::numeric_limits<double>::infinity();
  check(!evaluateDetections({malformed}, {e}, 1000).ok,
        "Nonfinite evaluation geometry must fail strictly");
  malformed = e;
  malformed.activity.reset();
  check(!evaluateDetections({e}, {malformed}, 1000).ok,
        "TF truth requires an actual time interval");
  const auto empty = evaluateDetections({}, {}, 1000);
  check(empty.ok && empty.metrics.size() == 3 && empty.metrics[0].f1 == 0,
        "Zero-prediction and zero-truth files must still produce metrics");
  check(
      !evaluateDetectionDataset(
           {{"same", DetectionEvaluationFileState::Completed, "", 1000, {}, {}},
            {"same",
             DetectionEvaluationFileState::Completed,
             "",
             1000,
             {},
             {}}})
           .ok,
      "Duplicate file identity must not silently double-count a dataset");
}

void windowScoresUseUniqueMaximumPerWindow() {
  const auto result = summarizeDetectionWindows({{2, .7}, {0, .8}, {2, .9}}, 5);
  check(result.ok && result.windowIds == std::vector<std::uint64_t>{0, 2} &&
            near(*result.scoreMean, .85) && near(*result.scoreMax, .9) &&
            result.coverage == .4 && result.totalAnalysisWindows == 5,
        "Window hits/mean/maximum must use unique windows and their maximum "
        "score once");
  check(!summarizeDetectionWindows({{5, .9}}, 5).ok &&
            !summarizeDetectionWindows(
                 {{0, std::numeric_limits<double>::infinity()}}, 5)
                 .ok,
        "Out-of-snapshot window ids and nonfinite scores must fail");
  check(summarizeDetectionWindows({}, 0).ok &&
            !summarizeDetectionWindows({{0, .9}}, 0).ok,
        "An empty analysis-window snapshot must never invent a denominator");
}

}  // namespace

void activityGrantRejectsAtomically() {
  auto input=energyInput();turnOn(input,5,30);turnOn(input,40,65);
  const auto parameters=fineParameters();
  const auto original=refineDetection(input,parameters);
  check(original.ok && original.activity.size()==2,"Output quota fixture must have two sustained activities");
  DetectionRefinementOptions options;options.maximumActivitySegments=1;
  const auto bounded=refineDetection(input,parameters,options);
  check(!bounded.ok && bounded.activity.empty() && bounded.error=="Activity output exceeds working budget",
      "Result grant must reject rather than allocate/truncate a partially visible activity list");
}
int main() {
  try {
    fineFilteringPrecedesGapMerging();
    fineGapsSplittingAndShortBursts();
    causalSmoothingAndContinuousTracks();
    physicalCellMappingAndClipping();
    fallbackAndNoneNeverInventActivity();
    fineFallbackIgnoresInactiveCoarseHistory();
    roiBilinearPoolingAndSourceMapping();
    roiGatesRecordTheMeasuredResidualDomain();
    approvedPrototypeParametersApply();
    roiSparseRealFramesFallback();
    coarseOwnsItsDurationParameters();
    invalidInputsBudgetsAndCancellation();
    thresholdCardinalityBeforeIoU();
    unknownTimeAndPreciseErrorMetrics();
    filesNeverCrossMatchAndFailuresRemainDenominator();
    evaluatorValidationAndSplitMergeTopology();
    windowScoresUseUniqueMaximumPerWindow();
    activityGrantRejectsAtomically();
    std::cout << "Detection refinement tests: 18 groups passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
