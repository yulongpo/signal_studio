#include "domain/detection_refinement.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_set>
#include <utility>

namespace signalstudio {
namespace {

struct IndexRun {
  std::size_t begin = 0;
  std::size_t end = 0;  // Half-open.
};

bool cancelled(const DetectionRefinementOptions& options) {
  return options.cancelled && options.cancelled();
}

bool finiteIn(double value, double lower, double upper) {
  return std::isfinite(value) && value >= lower && value <= upper;
}

bool validFrequency(FrequencyRange frequency) {
  return std::isfinite(frequency.lowerHz) && std::isfinite(frequency.upperHz) &&
         frequency.lowerHz < frequency.upperHz &&
         std::isfinite(frequency.upperHz - frequency.lowerHz);
}

bool validView(ViewRange view) {
  return view.time.begin < view.time.end && validFrequency(view.frequency);
}

ViewRange intersect(ViewRange a, ViewRange b) {
  return {
      {std::max(a.time.begin, b.time.begin), std::min(a.time.end, b.time.end)},
      {std::max(a.frequency.lowerHz, b.frequency.lowerHz),
       std::min(a.frequency.upperHz, b.frequency.upperHz)}};
}

SampleIndex saturatedAdd(SampleIndex a, SampleIndex b) {
  return a > std::numeric_limits<SampleIndex>::max() - b
             ? std::numeric_limits<SampleIndex>::max()
             : a + b;
}

SampleIndex saturatedCeilingSamples(double value) {
  if (!std::isfinite(value) ||
      value >= static_cast<double>(std::numeric_limits<SampleIndex>::max()))
    return std::numeric_limits<SampleIndex>::max();
  return static_cast<SampleIndex>(std::ceil(value));
}

bool cellsForSeconds(double seconds, double sampleRateHz, double step,
                     std::size_t minimum, bool roundUp, std::size_t& cells) {
  const double count = seconds * sampleRateHz / step;
  if (!std::isfinite(count) || count < 0 ||
      count >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
    return false;
  cells = std::max(
      minimum,
      static_cast<std::size_t>(roundUp ? std::ceil(count) : std::floor(count)));
  return true;
}

double quantile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  const double position = fraction * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  const double weight = position - static_cast<double>(lower);
  return (1 - weight) * values[lower] + weight * values[upper];
}

double sampleQuantile(std::vector<float> values, double fraction) {
  const double position = fraction * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  std::nth_element(values.begin(), values.begin() + lower, values.end());
  const float a = values[lower];
  if (lower == upper) return a;
  const float b = *std::min_element(values.begin() + upper, values.end());
  const float weight = static_cast<float>(position - lower);
  const float difference = b - a;
  return weight < .5f ? a + difference * weight
                      : b - difference * (1.f - weight);
}

std::vector<IndexRun> maskRuns(const std::vector<bool>& mask, std::size_t minOn,
                               bool& exceededBudget) {
  std::vector<IndexRun> runs;
  for (std::size_t i = 0; i < mask.size();) {
    if (!mask[i]) {
      ++i;
      continue;
    }
    const auto begin = i;
    while (i < mask.size() && mask[i]) ++i;
    if (i - begin < minOn) continue;
    if (runs.size() == annotationSegmentBudget) {
      exceededBudget = true;
      return {};
    }
    runs.push_back({begin, i});
  }
  return runs;
}

void removeShortRuns(std::vector<IndexRun>& runs, std::size_t minimum) {
  std::erase_if(
      runs, [minimum](IndexRun run) { return run.end - run.begin < minimum; });
}

void mergeSmallGaps(std::vector<IndexRun>& runs, std::size_t minimumGap) {
  std::size_t output = 0;
  for (const auto run : runs) {
    if (output && run.begin - runs[output - 1].end < minimumGap)
      runs[output - 1].end = run.end;
    else
      runs[output++] = run;
  }
  runs.resize(output);
}

std::vector<IndexRun> fineRuns(const std::vector<bool>& mask, std::size_t minOn,
                               std::size_t minOff, bool split,
                               bool& exceededBudget) {
  auto runs = maskRuns(mask, minOn, exceededBudget);
  mergeSmallGaps(runs, minOff);
  if (!split && !runs.empty()) runs = {{runs.front().begin, runs.back().end}};
  return runs;
}

bool publishSegment(DetectionRefinementResult& result,
                    const DetectionRefinementInput& input, ViewRange range,
                    TimeQuality timeQuality, FrequencyQuality frequencyQuality,
                    SampleIndex uncertainty,std::size_t maximum) {
  const auto clipped = intersect(range, input.publishBounds);
  if (!validView(clipped)) return true;
  if(result.activity.size()>=maximum) return false;
  Segment segment;
  segment.id =
      input.candidateId + "/activity/" + std::to_string(result.activity.size());
  segment.range = clipped;
  segment.timeQuality = timeQuality;
  segment.frequencyQuality = frequencyQuality;
  segment.uncertaintySamples = uncertainty;
  segment.clippedAtRoi = clipped != range;
  result.activity.push_back(std::move(segment));
  return true;
}

DetectionRefinementResult invalidRefinement(DetectionRefinementResult result,
                                            std::string error) {
  result.ok = false;
  result.error = std::move(error);
  result.activity.clear();
  result.status = RefineStatus::Skipped;
  return result;
}

DetectionRefinementResult fallback(DetectionRefinementResult result,
                                   const DetectionRefinementInput& input,
                                   std::string reason) {
  result.activity.clear();
  result.status = RefineStatus::Fallback;
  result.fallbackReason = std::move(reason);
  for (const auto& original : input.fallbackActivity) {
    auto segment = original;
    segment.range = intersect(original.range, input.publishBounds);
    if (!validView(segment.range)) continue;
    segment.clippedAtRoi |= segment.range != original.range;
    result.activity.push_back(std::move(segment));
  }
  return result;
}

bool validateSpectrum(const DetectionSpectrumRoi& spectrum, std::size_t budget,
                      std::string& error) {
  const auto times = spectrum.timeSlots.size();
  const auto edges = spectrum.frequencyEdgesHz.size();
  if (!times || edges < 2 || times > budget || edges - 1 > budget ||
      times > budget / (edges - 1) ||
      spectrum.powerDb.size() != times * (edges - 1)) {
    error = "Spectrum ROI shape exceeds the working-cell budget";
    return false;
  }
  const auto step =
      spectrum.timeSlots.front().end - spectrum.timeSlots.front().begin;
  for (std::size_t i = 0; i < times; ++i) {
    const auto slot = spectrum.timeSlots[i];
    if (slot.begin >= slot.end ||
        (i && slot.begin != spectrum.timeSlots[i - 1].end) ||
        (i + 1 < times && slot.end - slot.begin != step) ||
        slot.end - slot.begin > step) {
      error = "Spectrum time slots must be contiguous physical source cells";
      return false;
    }
  }
  for (std::size_t i = 0; i < edges; ++i)
    if (!std::isfinite(spectrum.frequencyEdgesHz[i]) ||
        (i &&
         !(spectrum.frequencyEdgesHz[i - 1] < spectrum.frequencyEdgesHz[i])) ||
        (i && !std::isfinite(spectrum.frequencyEdgesHz[i] -
                             spectrum.frequencyEdgesHz[i - 1]))) {
      error = "Spectrum frequency edges must be finite and increasing";
      return false;
    }
  for (const auto value : spectrum.powerDb)
    if (!std::isfinite(value)) {
      error = "Spectrum power contains a nonfinite value";
      return false;
    }
  return true;
}

struct SpectrumCrop {
  std::size_t firstTime = 0, timeCount = 0;
  std::size_t firstFrequency = 0, frequencyCount = 0;
};

SpectrumCrop cropSpectrum(const DetectionSpectrumRoi& spectrum,
                          ViewRange observation) {
  const auto& slots = spectrum.timeSlots;
  const auto& edges = spectrum.frequencyEdgesHz;
  const auto first = std::lower_bound(
      slots.begin(), slots.end(), observation.time.begin,
      [](TimeRange slot, SampleIndex sample) { return slot.begin < sample; });
  const auto after = std::lower_bound(
      slots.begin(), slots.end(), observation.time.end,
      [](TimeRange slot, SampleIndex sample) { return slot.begin < sample; });
  if (first == slots.end() || after <= first) return {};
  std::vector<double> centers(edges.size() - 1);
  for (std::size_t i = 0; i < centers.size(); ++i)
    centers[i] = edges[i] * .5 + edges[i + 1] * .5;
  const auto indexForHz = [&](double hz) {
    return std::min(centers.size() - 1,
                    static_cast<std::size_t>(
                        std::lower_bound(centers.begin(), centers.end(), hz) -
                        centers.begin()));
  };
  const auto f0 = indexForHz(observation.frequency.lowerHz);
  const auto f1 = indexForHz(observation.frequency.upperHz);
  if (observation.frequency.upperHz <= edges.front() ||
      observation.frequency.lowerHz >= edges.back())
    return {};
  return {static_cast<std::size_t>(first - slots.begin()),
          static_cast<std::size_t>(after - first), f0, f1 - f0 + 1};
}

struct InterpolationCoordinate {
  std::size_t lower = 0, upper = 0;
  double upperWeight = 0;
};

InterpolationCoordinate interpolateCoordinate(std::size_t destination,
                                              std::size_t inputSize,
                                              std::size_t outputSize) {
  const double source = std::clamp(
      (static_cast<double>(destination) + .5) * inputSize / outputSize - .5, 0.,
      static_cast<double>(inputSize - 1));
  const auto lower = static_cast<std::size_t>(std::floor(source));
  return {lower, std::min(lower + 1, inputSize - 1), source - lower};
}

std::vector<double> resizeRoi(const DetectionSpectrumRoi& spectrum,
                              SpectrumCrop crop, std::size_t rows,
                              std::size_t columns) {
  const auto inputColumns = spectrum.frequencyEdgesHz.size() - 1;
  const auto sample = [&](std::size_t time, std::size_t frequency) {
    return static_cast<double>(
        spectrum.powerDb[(crop.firstTime + time) * inputColumns +
                         crop.firstFrequency + frequency]);
  };
  std::vector<double> resized(rows * columns);
  for (std::size_t f = 0; f < rows; ++f) {
    const auto y = interpolateCoordinate(f, crop.frequencyCount, rows);
    for (std::size_t t = 0; t < columns; ++t) {
      const auto x = interpolateCoordinate(t, crop.timeCount, columns);
      const double lower = (1 - x.upperWeight) * sample(x.lower, y.lower) +
                           x.upperWeight * sample(x.upper, y.lower);
      const double upper = (1 - x.upperWeight) * sample(x.lower, y.upper) +
                           x.upperWeight * sample(x.upper, y.upper);
      resized[f * columns + t] =
          (1 - y.upperWeight) * lower + y.upperWeight * upper;
    }
  }
  return resized;
}

std::vector<bool> centeredMask(const std::vector<bool>& mask,
                               std::size_t width) {
  std::vector<bool> result(mask.size());
  for (std::size_t i = 0; i < mask.size(); ++i) {
    unsigned on = 0;
    const auto half = width / 2;
    const auto first = i >= half ? i - half : 0;
    const auto end = std::min(mask.size(), i + half + 1);
    for (auto j = first; j < end; ++j) on += mask[j] ? 1 : 0;
    result[i] = on > half;  // Zero padding is included in the divisor.
  }
  return result;
}

void roiProfiles(std::vector<double>& roi, std::size_t rows,
                 std::size_t columns, std::vector<bool>& timeOn,
                 std::vector<bool>& frequencyOn, double thresholdScale,
                 std::size_t smoothingCells,
                 DetectionRefinementEvidence& evidence) {
  for (unsigned round = 0; round < 3; ++round) {
    const double mean =
        std::accumulate(roi.begin(), roi.end(), 0.) / roi.size();
    for (auto& value : roi) value -= mean;
  }
  double positiveSum = 0;
  std::size_t positiveCount = 0;
  for (auto& value : roi) {
    value = std::max(0., value);
    if (value > 0) {
      positiveSum += value;
      ++positiveCount;
    }
  }
  const double positiveMean = positiveCount ? positiveSum / positiveCount : 0;
  const double fractionOff =
      1. - static_cast<double>(positiveCount) / roi.size();
  evidence.roiPositiveMean = positiveMean;
  evidence.roiOffFraction = fractionOff;
  evidence.roiTimeThreshold = fractionOff * .5 * positiveMean * thresholdScale;
  evidence.roiFrequencyThreshold =
      fractionOff * .25 * positiveMean * thresholdScale;
  timeOn.assign(columns, false);
  frequencyOn.assign(rows, false);
  for (std::size_t t = 0; t < columns; ++t) {
    double sum = 0;
    for (std::size_t f = 0; f < rows; ++f) sum += roi[f * columns + t];
    timeOn[t] = sum / rows > fractionOff * .5 * positiveMean * thresholdScale;
  }
  for (std::size_t f = 0; f < rows; ++f) {
    double sum = 0;
    for (std::size_t t = 0; t < columns; ++t) sum += roi[f * columns + t];
    frequencyOn[f] =
        sum / columns > fractionOff * .25 * positiveMean * thresholdScale;
  }
  timeOn = centeredMask(timeOn, smoothingCells);
  frequencyOn = centeredMask(frequencyOn, smoothingCells);
}

bool validEvaluationEvent(const DetectionEvaluationEvent& event,
                          bool requireTime) {
  return validFrequency(event.frequency) &&
         (!requireTime || event.activity.has_value()) &&
         (!event.activity || event.activity->begin < event.activity->end);
}

double intervalIoU(double intersection, double a, double b) {
  const double scale = std::max(a, b);
  const double overlap = intersection / scale;
  return overlap / (a / scale + b / scale - overlap);
}

// The rectangular Hungarian algorithm works on the smaller side as rows. A
// cardinality bonus exceeding all possible summed IoU differences makes valid
// match count the first objective independently at every threshold.
std::vector<std::pair<std::size_t, std::size_t>> assignAtThreshold(
    const std::vector<double>& matrix, std::size_t predictions,
    std::size_t truth, double threshold) {
  if (!predictions || !truth) return {};
  const bool transpose = predictions > truth;
  const auto rows = std::min(predictions, truth);
  const auto columns = std::max(predictions, truth);
  const double bonus = static_cast<double>(rows) + 1;
  const auto iouAt = [&](std::size_t row, std::size_t column) {
    return transpose ? matrix[column * truth + row]
                     : matrix[row * truth + column];
  };
  std::vector<double> rowPotential(rows + 1), columnPotential(columns + 1);
  std::vector<std::size_t> rowAtColumn(columns + 1), previous(columns + 1);
  for (std::size_t row = 1; row <= rows; ++row) {
    rowAtColumn[0] = row;
    std::size_t column = 0;
    std::vector<double> distance(columns + 1,
                                 std::numeric_limits<double>::infinity());
    std::vector<bool> visited(columns + 1);
    do {
      visited[column] = true;
      const auto currentRow = rowAtColumn[column];
      double delta = std::numeric_limits<double>::infinity();
      std::size_t nextColumn = 0;
      for (std::size_t candidate = 1; candidate <= columns; ++candidate) {
        if (visited[candidate]) continue;
        const double iou = iouAt(currentRow - 1, candidate - 1);
        const double cost = iou >= threshold ? -(bonus + iou) : 0;
        const double reduced =
            cost - rowPotential[currentRow] - columnPotential[candidate];
        if (reduced < distance[candidate]) {
          distance[candidate] = reduced;
          previous[candidate] = column;
        }
        if (distance[candidate] < delta) {
          delta = distance[candidate];
          nextColumn = candidate;
        }
      }
      for (std::size_t candidate = 0; candidate <= columns; ++candidate) {
        if (visited[candidate]) {
          rowPotential[rowAtColumn[candidate]] += delta;
          columnPotential[candidate] -= delta;
        } else {
          distance[candidate] -= delta;
        }
      }
      column = nextColumn;
    } while (rowAtColumn[column]);
    do {
      const auto preceding = previous[column];
      rowAtColumn[column] = rowAtColumn[preceding];
      column = preceding;
    } while (column);
  }
  std::vector<std::pair<std::size_t, std::size_t>> matches;
  for (std::size_t column = 1; column <= columns; ++column) {
    if (!rowAtColumn[column]) continue;
    const auto row = rowAtColumn[column] - 1;
    if (iouAt(row, column - 1) < threshold) continue;
    matches.emplace_back(transpose ? column - 1 : row,
                         transpose ? row : column - 1);
  }
  std::sort(matches.begin(), matches.end());
  return matches;
}

SampleIndex absoluteDifference(SampleIndex a, SampleIndex b) {
  return a > b ? a - b : b - a;
}

DetectionEvaluationMatch makeMatch(std::size_t predictionIndex,
                                   std::size_t truthIndex,
                                   const DetectionEvaluationEvent& prediction,
                                   const DetectionEvaluationEvent& truth,
                                   double iou, double sampleRateHz) {
  DetectionEvaluationMatch match;
  match.predictionIndex = predictionIndex;
  match.truthIndex = truthIndex;
  match.iou = iou;
  match.frequencyIou = detectionEventIoU(
      prediction, truth, DetectionEvaluationDimension::Frequency);
  match.timeIou =
      detectionEventIoU(prediction, truth, DetectionEvaluationDimension::Time);
  if (prediction.activity && truth.activity) {
    match.startAbsoluteErrorSamples =
        absoluteDifference(prediction.activity->begin, truth.activity->begin);
    match.endAbsoluteErrorSamples =
        absoluteDifference(prediction.activity->end, truth.activity->end);
    match.startAbsoluteErrorSeconds =
        match.startAbsoluteErrorSamples / sampleRateHz;
    match.endAbsoluteErrorSeconds =
        match.endAbsoluteErrorSamples / sampleRateHz;
  }
  const auto pf = prediction.frequency;
  const auto tf = truth.frequency;
  match.centerAbsoluteErrorHz = std::abs((pf.lowerHz * .5 + pf.upperHz * .5) -
                                         (tf.lowerHz * .5 + tf.upperHz * .5));
  match.bandwidthAbsoluteErrorHz =
      std::abs((pf.upperHz - pf.lowerHz) - (tf.upperHz - tf.lowerHz));
  return match;
}

void finishMetric(DetectionEvaluationMetric& metric, std::uint64_t predictions,
                  std::uint64_t truth, const std::vector<double>& startErrors,
                  const std::vector<double>& endErrors) {
  metric.truePositive = metric.matches.size();
  metric.falsePositive = predictions - metric.truePositive;
  metric.falseNegative = truth - metric.truePositive;
  metric.precision =
      predictions ? static_cast<double>(metric.truePositive) / predictions : 0;
  metric.recall = truth ? static_cast<double>(metric.truePositive) / truth : 0;
  metric.f1 = metric.precision + metric.recall > 0
                  ? 2 * metric.precision * metric.recall /
                        (metric.precision + metric.recall)
                  : 0;
  if (!startErrors.empty()) {
    metric.startErrorMedianSeconds = quantile(startErrors, .5);
    metric.startErrorP95Seconds = quantile(startErrors, .95);
    metric.endErrorMedianSeconds = quantile(endErrors, .5);
    metric.endErrorP95Seconds = quantile(endErrors, .95);
  }
  if (!metric.matches.empty()) {
    double centerMean = 0, bandwidthMean = 0;
    for (const auto& match : metric.matches) {
      centerMean += match.centerAbsoluteErrorHz / metric.matches.size();
      bandwidthMean += match.bandwidthAbsoluteErrorHz / metric.matches.size();
    }
    metric.centerErrorMeanHz = centerMean;
    metric.bandwidthErrorMeanHz = bandwidthMean;
  }
}

bool validThresholds(const std::vector<double>& thresholds) {
  if (thresholds.empty() || thresholds.size() > 64) return false;
  for (const auto threshold : thresholds)
    if (!std::isfinite(threshold) || threshold <= 0 || threshold > 1)
      return false;
  return true;
}

}  // namespace

DetectionParameters coarseDetectionFallbackParameters(
    const DetectionParameters& parameters) {
  const DetectionParameters defaults;
  auto effective = parameters;
  effective.timeStrategy = DetectionTimeStrategy::Coarse;
  effective.bandwidthMarginDb = defaults.bandwidthMarginDb;
  effective.coarseMinBurstSeconds = defaults.coarseMinBurstSeconds;
  effective.coarseSplitGapSeconds = defaults.coarseSplitGapSeconds;
  return effective;
}

DetectionRefinementResult refineDetection(
    const DetectionRefinementInput& input,
    const DetectionParameters& parameters,
    const DetectionRefinementOptions& options) {
  DetectionRefinementResult result;
  result.observation = intersect(input.observation, input.publishBounds);
  result.evidence.continuousTrack = input.windowCoverage >= .7;
  if (input.candidateId.empty() || !validView(input.observation) ||
      !validView(input.publishBounds) || !validView(result.observation) ||
      !std::isfinite(input.sampleRateHz) || input.sampleRateHz <= 0 ||
      !finiteIn(input.windowCoverage, 0, 1) || !options.workingCellBudget ||
      options.workingCellBudget > 256 * 1024 * 1024 ||
      options.maximumActivitySegments > annotationSegmentBudget ||
      input.fallbackActivity.size() > options.maximumActivitySegments)
    return invalidRefinement(std::move(result),
                             "Invalid refinement input or bounds");
  for (const auto& segment : input.fallbackActivity)
    if (!validView(segment.range))
      return invalidRefinement(std::move(result),
                               "Invalid fallback activity segment");

  const bool split =
      parameters.splitBursts &&
      (parameters.timeStrategy == DetectionTimeStrategy::Coarse ||
       !result.evidence.continuousTrack);
  if (parameters.timeStrategy == DetectionTimeStrategy::None) return result;
  if (cancelled(options))
    return invalidRefinement(std::move(result), "Refinement cancelled");
  if (parameters.timeStrategy != DetectionTimeStrategy::Sample &&
      parameters.timeStrategy != DetectionTimeStrategy::Roi &&
      parameters.timeStrategy != DetectionTimeStrategy::Coarse)
    return invalidRefinement(std::move(result), "Unknown refinement strategy");

  if (parameters.timeStrategy == DetectionTimeStrategy::Sample) {
    result.method = RefineMethod::Sample;
    if (!input.energy)
      return fallback(std::move(result), input, "sample-energy-unavailable");
    const auto& energy = *input.energy;
    if (!finiteIn(parameters.sampleEdgeRatio, 0, 1) ||
        !finiteIn(parameters.smoothSeconds, 0, 60) ||
        !finiteIn(parameters.minOnSeconds, 0, 60) ||
        !finiteIn(parameters.minOffSeconds, 0, 60) ||
        !finiteIn(options.sampleNoiseQuantile, 0, 1) ||
        !finiteIn(options.sampleBurstQuantile, 0, 1) ||
        !finiteIn(options.sampleMinimumOnFraction, 0, 1) ||
        !std::isfinite(options.sampleMinimumBurstNoiseRatio) ||
        options.sampleMinimumBurstNoiseRatio < 1 ||
        !finiteIn(options.sampleMinimumRunSeconds, 0, 60) ||
        !energy.stepSamples || energy.supportEndSample <= energy.originSample ||
        energy.power.empty() || energy.power.size() > options.workingCellBudget)
      return invalidRefinement(
          std::move(result),
          "Invalid sample refinement parameters or energy shape");
    const auto span = energy.supportEndSample - energy.originSample;
    const auto requiredCells =
        span / energy.stepSamples + (span % energy.stepSamples ? 1 : 0);
    if (energy.power.size() != requiredCells)
      return invalidRefinement(
          std::move(result),
          "Energy cells do not describe the exact physical support");
    for (std::size_t i = 0; i < energy.power.size(); ++i) {
      if (!(i % 4096) && cancelled(options))
        return invalidRefinement(std::move(result), "Refinement cancelled");
      const auto power = energy.power[i];
      if (!std::isfinite(power) || power < 0)
        return invalidRefinement(
            std::move(result),
            "Sample energy must be finite nonnegative linear power");
    }

    std::size_t smooth = 0, minOn = 0, minOff = 0;
    const double effectiveMinOn =
        parameters.splitBursts
            ? std::max(options.sampleMinimumRunSeconds, parameters.minOnSeconds)
            : options.sampleMinimumRunSeconds;
    const double effectiveMinOff =
        parameters.splitBursts ? parameters.minOffSeconds : 0;
    if (!cellsForSeconds(parameters.smoothSeconds, input.sampleRateHz,
                         static_cast<double>(energy.stepSamples), 1, false,
                         smooth) ||
        !cellsForSeconds(effectiveMinOn, input.sampleRateHz,
                         static_cast<double>(energy.stepSamples), 2, false,
                         minOn) ||
        !cellsForSeconds(effectiveMinOff, input.sampleRateHz,
                         static_cast<double>(energy.stepSamples), 2, false,
                         minOff) ||
        smooth > std::numeric_limits<SampleIndex>::max() / energy.stepSamples ||
        minOn > std::numeric_limits<SampleIndex>::max() / energy.stepSamples ||
        minOff > std::numeric_limits<SampleIndex>::max() / energy.stepSamples)
      return invalidRefinement(std::move(result),
                               "Sample duration conversion overflow");
    if (!parameters.splitBursts) minOff = 0;
    auto& evidence = result.evidence;
    evidence.energyMethod = energy.energyMethod;
    evidence.sourceOriginSample = energy.originSample;
    evidence.sourceStepSamples = energy.stepSamples;
    evidence.sourceSupportEndSample = energy.supportEndSample;
    evidence.filterSupportSamples = energy.filterSupportSamples;
    evidence.quantizationUncertaintySamples = energy.stepSamples;
    evidence.smoothingSamples = smooth * energy.stepSamples;
    evidence.minOnSamples = minOn * energy.stepSamples;
    evidence.minOffSamples = minOff * energy.stepSamples;
    evidence.fineParametersApplied = true;
    evidence.durationParametersApplied = parameters.splitBursts;
    evidence.splittingApplied = split;
    evidence.effectiveEdgeRatio =
        parameters.sampleEdgeRatio * (evidence.continuousTrack ? .5 / .6 : 1);
    evidence.effectiveMinOnSeconds = effectiveMinOn;
    evidence.effectiveMinOffSeconds = effectiveMinOff;
    evidence.effectiveSmoothSeconds = parameters.smoothSeconds;
    std::vector<float> smoothed(energy.power.size());
    double running = 0;
    for (std::size_t i = 0; i < energy.power.size(); ++i) {
      if (!(i % 4096) && cancelled(options))
        return invalidRefinement(std::move(result), "Refinement cancelled");
      running += energy.power[i];
      if (i >= smooth) running -= energy.power[i - smooth];
      smoothed[i] = static_cast<float>(running / smooth);
      if (!std::isfinite(smoothed[i]))
        return invalidRefinement(std::move(result),
                                 "Sample smoothing overflow");
    }
    if (cancelled(options))
      return invalidRefinement(std::move(result), "Refinement cancelled");
    evidence.noise = sampleQuantile(smoothed, options.sampleNoiseQuantile);
    evidence.burst = sampleQuantile(smoothed, options.sampleBurstQuantile);
    if (cancelled(options))
      return invalidRefinement(std::move(result), "Refinement cancelled");
    if (*evidence.burst <=
        *evidence.noise * options.sampleMinimumBurstNoiseRatio)
      return fallback(std::move(result), input,
                      "sample-burst-noise-ratio-too-low");
    evidence.threshold =
        *evidence.noise +
        evidence.effectiveEdgeRatio * (*evidence.burst - *evidence.noise);
    evidence.comparisonThreshold = static_cast<float>(*evidence.threshold);
    std::vector<bool> on(smoothed.size());
    std::size_t onCount = 0;
    for (std::size_t i = 0; i < smoothed.size(); ++i) {
      on[i] = smoothed[i] > *evidence.comparisonThreshold;
      if (on[i]) ++onCount;
    }
    evidence.onFraction = static_cast<double>(onCount) / on.size();
    if (*evidence.onFraction < options.sampleMinimumOnFraction)
      return fallback(std::move(result), input, "sample-on-fraction-too-low");
    bool exceededBudget = false;
    const auto runs = fineRuns(on, minOn, minOff, split, exceededBudget);
    if (exceededBudget)
      return invalidRefinement(std::move(result),
                               "Activity-run budget exceeded");
    const auto uncertainty = saturatedAdd(
        energy.stepSamples,
        saturatedAdd(energy.filterSupportSamples,
                     evidence.smoothingSamples - energy.stepSamples));
    for (const auto run : runs) {
      const auto begin = energy.originSample + run.begin * energy.stepSamples;
      const auto end = run.end == energy.power.size()
                           ? energy.supportEndSample
                           : energy.originSample + run.end * energy.stepSamples;
      if(!publishSegment(result, input, {{begin, end}, input.observation.frequency},
                     TimeQuality::AlgorithmEstimated,
                     FrequencyQuality::ModelEstimated, uncertainty,options.maximumActivitySegments))
        return invalidRefinement(std::move(result),"Activity output exceeds working budget");
    }
    if (result.activity.empty())
      return fallback(std::move(result), input,
                      runs.empty() ? "sample-no-sustained-on-run"
                                   : "sample-runs-outside-publish-bounds");
    result.status = RefineStatus::Applied;
    return result;
  }

  result.method = parameters.timeStrategy == DetectionTimeStrategy::Roi
                      ? RefineMethod::Roi
                      : RefineMethod::Coarse;
  if (!input.spectrum)
    return fallback(std::move(result), input, "spectrum-unavailable");
  std::string spectrumError;
  if (!validateSpectrum(*input.spectrum, options.workingCellBudget,
                        spectrumError))
    return invalidRefinement(std::move(result), std::move(spectrumError));
  const auto& spectrum = *input.spectrum;
  if (cancelled(options))
    return invalidRefinement(std::move(result), "Refinement cancelled");
  const auto crop = cropSpectrum(spectrum, input.observation);
  if (!crop.timeCount)
    return fallback(std::move(result), input,
                    "no-real-spectrum-cells-in-observation");
  auto& evidence = result.evidence;
  evidence.sourceOriginSample = spectrum.timeSlots[crop.firstTime].begin;
  evidence.sourceStepSamples =
      spectrum.timeSlots.front().end - spectrum.timeSlots.front().begin;
  evidence.sourceSupportEndSample =
      spectrum.timeSlots[crop.firstTime + crop.timeCount - 1].end;
  evidence.fftSupportUncertaintySamples = spectrum.fftSupportUncertaintySamples;
  evidence.quantizationUncertaintySamples = evidence.sourceStepSamples;
  evidence.energyMethod = "physical-source-STFT-cells";
  if (crop.timeCount < 3)
    return fallback(std::move(result), input,
                    "fewer-than-three-real-spectrum-frames");

  std::vector<IndexRun> runs;
  FrequencyRange outputFrequency = input.observation.frequency;
  if (parameters.timeStrategy == DetectionTimeStrategy::Roi) {
    if (!finiteIn(parameters.sampleEdgeRatio, 0, 1) ||
        !finiteIn(parameters.smoothSeconds, 0, 60) ||
        !finiteIn(parameters.minOnSeconds, 0, 60) ||
        !finiteIn(parameters.minOffSeconds, 0, 60) ||
        !options.roiFrequencyRows || !options.roiTimeColumns ||
        options.roiFrequencyRows >
            options.workingCellBudget / options.roiTimeColumns)
      return invalidRefinement(
          std::move(result),
          "Invalid ROI refinement parameters or resized shape");
    const auto rows = options.roiFrequencyRows,
               columns = options.roiTimeColumns;
    auto roi = resizeRoi(spectrum, crop, rows, columns);
    if (cancelled(options))
      return invalidRefinement(std::move(result), "Refinement cancelled");
    std::vector<bool> timeOn, frequencyOn;
    // User-confirmed M0-P parameter extension. Default .6 / 1 ms preserves
    // the reference adaptive threshold and five-cell centered mask.
    const auto smoothingCells = static_cast<std::size_t>(std::clamp(
        2 * std::floor(5 * parameters.smoothSeconds / .001 / 2) + 1, 1., 63.));
    roiProfiles(roi, rows, columns, timeOn, frequencyOn,
                parameters.sampleEdgeRatio / .6, smoothingCells, evidence);
    if (cancelled(options))
      return invalidRefinement(std::move(result), "Refinement cancelled");
    const double cellSamples =
        static_cast<double>(evidence.sourceSupportEndSample -
                            evidence.sourceOriginSample) /
        columns;
    std::size_t minOn = 0, minOff = 0;
    if (!cellsForSeconds(parameters.splitBursts ? parameters.minOnSeconds : 0,
                         input.sampleRateHz, cellSamples, 1, true, minOn) ||
        !cellsForSeconds(parameters.splitBursts ? parameters.minOffSeconds : 0,
                         input.sampleRateHz, cellSamples, 1, true, minOff))
      return invalidRefinement(std::move(result),
                               "ROI duration conversion overflow");
    if (!parameters.splitBursts) minOff = 0;
    evidence.fineParametersApplied = true;
    evidence.durationParametersApplied = parameters.splitBursts;
    evidence.effectiveEdgeRatio = parameters.sampleEdgeRatio;
    evidence.effectiveSmoothSeconds = parameters.smoothSeconds;
    evidence.roiSmoothingCells = smoothingCells;
    evidence.splittingApplied = split;
    evidence.effectiveMinOnSeconds =
        parameters.splitBursts ? parameters.minOnSeconds : 0;
    evidence.effectiveMinOffSeconds =
        parameters.splitBursts ? parameters.minOffSeconds : 0;
    evidence.smoothingSamples =
        saturatedCeilingSamples(smoothingCells * cellSamples);
    evidence.minOnSamples = saturatedCeilingSamples(minOn * cellSamples);
    evidence.minOffSamples = saturatedCeilingSamples(minOff * cellSamples);
    bool exceededBudget = false;
    const auto pooledRuns =
        fineRuns(timeOn, minOn, minOff, split, exceededBudget);
    if (exceededBudget)
      return invalidRefinement(std::move(result),
                               "Activity-run budget exceeded");
    for (const auto run : pooledRuns) {
      const auto first = run.begin * crop.timeCount / columns;
      const auto after = (run.end * crop.timeCount + columns - 1) / columns;
      if (after - first >= 3) runs.push_back({first, after});
    }
    const auto firstOn =
        std::find(frequencyOn.begin(), frequencyOn.end(), true);
    if (firstOn != frequencyOn.end()) {
      const auto lastOn =
          std::find(frequencyOn.rbegin(), frequencyOn.rend(), true);
      const auto first =
          static_cast<std::size_t>(firstOn - frequencyOn.begin()) *
          crop.frequencyCount / rows;
      const auto last = frequencyOn.size() -
                        static_cast<std::size_t>(lastOn - frequencyOn.rbegin());
      const auto after = (last * crop.frequencyCount + rows - 1) / rows;
      outputFrequency = {
          spectrum.frequencyEdgesHz[crop.firstFrequency + first],
          spectrum.frequencyEdgesHz[crop.firstFrequency + after]};
    }
    if (runs.empty())
      return fallback(std::move(result), input,
                      pooledRuns.empty()
                          ? "roi-no-sustained-on-mask"
                          : "roi-runs-map-to-fewer-than-three-real-frames");
  } else {
    if (!finiteIn(parameters.bandwidthMarginDb, 0, 100) ||
        !finiteIn(parameters.coarseMinBurstSeconds, 0, 60) ||
        !finiteIn(parameters.coarseSplitGapSeconds, 0, 60))
      return invalidRefinement(std::move(result),
                               "Invalid coarse refinement parameters");
    if (parameters.bandwidthMarginDb <= 0)
      return fallback(std::move(result), input,
                      "nonpositive-margin-disables-coarse-refinement");
    std::vector<double> profile(crop.timeCount);
    const auto stride = spectrum.frequencyEdgesHz.size() - 1;
    const auto strongestCount =
        std::max<std::size_t>(1, (crop.frequencyCount + 19) / 20);
    std::vector<double> band(crop.frequencyCount);
    for (std::size_t t = 0; t < crop.timeCount; ++t) {
      if (!(t % 4096) && cancelled(options))
        return invalidRefinement(std::move(result), "Refinement cancelled");
      for (std::size_t f = 0; f < crop.frequencyCount; ++f)
        band[f] = spectrum.powerDb[(crop.firstTime + t) * stride +
                                   crop.firstFrequency + f];
      std::nth_element(band.begin(), band.end() - strongestCount, band.end());
      profile[t] =
          std::accumulate(band.end() - strongestCount, band.end(), 0.) /
          strongestCount;
    }
    evidence.noise = quantile(profile, .2);
    evidence.threshold = *evidence.noise + parameters.bandwidthMarginDb;
    std::vector<bool> on(profile.size());
    std::size_t onCount = 0;
    for (std::size_t i = 0; i < profile.size(); ++i) {
      on[i] = profile[i] > *evidence.threshold;
      if (on[i]) ++onCount;
    }
    evidence.onFraction = static_cast<double>(onCount) / on.size();
    evidence.coarseParametersApplied = true;
    const bool coarseSplit = split && parameters.coarseSplitGapSeconds > 0;
    evidence.splittingApplied = coarseSplit;
    bool exceededBudget = false;
    runs = maskRuns(on, 1, exceededBudget);
    if (exceededBudget)
      return invalidRefinement(std::move(result),
                               "Activity-run budget exceeded");
    if (coarseSplit) {
      std::size_t gap = 0, minimum = 0;
      if (!cellsForSeconds(parameters.coarseSplitGapSeconds, input.sampleRateHz,
                           static_cast<double>(evidence.sourceStepSamples), 1,
                           true, gap) ||
          !cellsForSeconds(parameters.coarseMinBurstSeconds, input.sampleRateHz,
                           static_cast<double>(evidence.sourceStepSamples), 1,
                           true, minimum))
        return invalidRefinement(std::move(result),
                                 "Coarse duration conversion overflow");
      mergeSmallGaps(runs, gap);
      removeShortRuns(runs, minimum);
      evidence.effectiveCoarseMinBurstSeconds =
          parameters.coarseMinBurstSeconds;
      evidence.effectiveCoarseSplitGapSeconds =
          parameters.coarseSplitGapSeconds;
    } else if (!runs.empty()) {
      runs = {{runs.front().begin, runs.back().end}};
    }
    std::erase_if(runs, [](IndexRun run) { return run.end - run.begin < 3; });
    if (runs.empty())
      return fallback(std::move(result), input, "coarse-no-supported-on-run");
  }
  const auto uncertainty = saturatedAdd(evidence.quantizationUncertaintySamples,
                                        evidence.fftSupportUncertaintySamples);
  for (const auto run : runs) {
    const auto first = crop.firstTime + run.begin;
    const auto last = crop.firstTime + run.end - 1;
    if(!publishSegment(
        result, input,
        {{spectrum.timeSlots[first].begin, spectrum.timeSlots[last].end},
         outputFrequency},
        TimeQuality::AlgorithmEstimated,
        result.method == RefineMethod::Roi ? FrequencyQuality::Refined
                                           : FrequencyQuality::ModelEstimated,
        uncertainty,options.maximumActivitySegments))
      return invalidRefinement(std::move(result),"Activity output exceeds working budget");
  }
  if (result.activity.empty())
    return fallback(std::move(result), input,
                    "spectrum-runs-outside-publish-bounds");
  result.status = RefineStatus::Applied;
  return result;
}

DetectionWindowEvidence summarizeDetectionWindows(
    const std::vector<DetectionWindowScore>& scores,
    std::uint64_t totalAnalysisWindows) {
  DetectionWindowEvidence result;
  result.totalAnalysisWindows = totalAnalysisWindows;
  std::map<std::uint64_t, double> unique;
  for (const auto& item : scores) {
    if (!finiteIn(item.score, 0, 1) || item.windowId >= totalAnalysisWindows) {
      result.ok = false;
      result.error =
          "Window score or ordinal exceeds the frozen analysis-window count";
      return result;
    }
    const auto [entry, inserted] = unique.emplace(item.windowId, item.score);
    if (!inserted) entry->second = std::max(entry->second, item.score);
  }
  if (unique.empty()) return result;
  double mean = 0, maximum = 0;
  for (const auto [id, score] : unique) {
    result.windowIds.push_back(id);
    mean += score / unique.size();
    maximum = std::max(maximum, score);
  }
  result.scoreMean = mean;
  result.scoreMax = maximum;
  result.coverage = static_cast<double>(unique.size()) / totalAnalysisWindows;
  return result;
}

double detectionEventIoU(const DetectionEvaluationEvent& prediction,
                         const DetectionEvaluationEvent& truth,
                         DetectionEvaluationDimension dimension) {
  if (!validEvaluationEvent(prediction, false) ||
      !validEvaluationEvent(truth, false))
    return 0;
  const auto pf = prediction.frequency, tf = truth.frequency;
  const double pw = pf.upperHz - pf.lowerHz, tw = tf.upperHz - tf.lowerHz;
  const double fi = std::max(
      0., std::min(pf.upperHz, tf.upperHz) - std::max(pf.lowerHz, tf.lowerHz));
  if (dimension == DetectionEvaluationDimension::Frequency)
    return intervalIoU(fi, pw, tw);
  if (!prediction.activity || !truth.activity) return 0;
  const auto pt = *prediction.activity, tt = *truth.activity;
  const double pd = static_cast<double>(pt.end - pt.begin),
               td = static_cast<double>(tt.end - tt.begin);
  const auto begin = std::max(pt.begin, tt.begin),
             end = std::min(pt.end, tt.end);
  const double ti = end > begin ? static_cast<double>(end - begin) : 0;
  if (dimension == DetectionEvaluationDimension::Time)
    return intervalIoU(ti, pd, td);
  if (dimension != DetectionEvaluationDimension::TimeFrequency) return 0;
  const double fs = std::max(pw, tw), ts = std::max(pd, td);
  const double intersection = (fi / fs) * (ti / ts);
  const double unionArea =
      (pw / fs) * (pd / ts) + (tw / fs) * (td / ts) - intersection;
  return unionArea > 0 ? intersection / unionArea : 0;
}

DetectionEvaluationResult evaluateDetections(
    const std::vector<DetectionEvaluationEvent>& predictions,
    const std::vector<DetectionEvaluationEvent>& truth, double sampleRateHz,
    const std::vector<double>& thresholds,
    DetectionEvaluationDimension dimension, std::size_t matrixCellBudget) {
  DetectionEvaluationResult result;
  const bool requireTime = dimension != DetectionEvaluationDimension::Frequency;
  if (!std::isfinite(sampleRateHz) || sampleRateHz <= 0 ||
      !validThresholds(thresholds) || !matrixCellBudget ||
      predictions.size() > matrixCellBudget ||
      truth.size() > matrixCellBudget ||
      (!truth.empty() &&
       predictions.size() > matrixCellBudget / truth.size()) ||
      (dimension != DetectionEvaluationDimension::TimeFrequency &&
       dimension != DetectionEvaluationDimension::Frequency &&
       dimension != DetectionEvaluationDimension::Time)) {
    result.ok = false;
    result.error =
        "Invalid evaluation configuration or matrix-cell budget exceeded";
    return result;
  }
  for (const auto& event : predictions)
    if (!validEvaluationEvent(event, false)) {
      result.ok = false;
      result.error = "Invalid prediction geometry";
      return result;
    }
  for (const auto& event : truth)
    if (!validEvaluationEvent(event, requireTime)) {
      result.ok = false;
      result.error = "Invalid truth geometry or missing truth time";
      return result;
    }
  result.predictionCount = predictions.size();
  result.truthCount = truth.size();
  for (const auto& event : predictions)
    if (!event.activity) ++result.unknownTimePredictions;
  result.unknownTimeFraction =
      predictions.empty() ? 0
                          : static_cast<double>(result.unknownTimePredictions) /
                                predictions.size();
  std::vector<double> matrix(predictions.size() * truth.size());
  std::vector<std::size_t> predictionOverlaps(predictions.size()),
      truthOverlaps(truth.size());
  for (std::size_t p = 0; p < predictions.size(); ++p)
    for (std::size_t t = 0; t < truth.size(); ++t) {
      matrix[p * truth.size() + t] =
          detectionEventIoU(predictions[p], truth[t], dimension);
      if (detectionEventIoU(predictions[p], truth[t],
                            DetectionEvaluationDimension::TimeFrequency) > 0) {
        ++predictionOverlaps[p];
        ++truthOverlaps[t];
      }
    }
  result.splitTruthCount =
      std::count_if(truthOverlaps.begin(), truthOverlaps.end(),
                    [](std::size_t count) { return count > 1; });
  result.mergedPredictionCount =
      std::count_if(predictionOverlaps.begin(), predictionOverlaps.end(),
                    [](std::size_t count) { return count > 1; });
  for (const auto threshold : thresholds) {
    DetectionEvaluationMetric metric;
    metric.threshold = threshold;
    std::vector<double> startErrors, endErrors;
    for (const auto [p, t] : assignAtThreshold(matrix, predictions.size(),
                                               truth.size(), threshold)) {
      auto match = makeMatch(p, t, predictions[p], truth[t],
                             matrix[p * truth.size() + t], sampleRateHz);
      if (!std::isfinite(match.centerAbsoluteErrorHz) ||
          !std::isfinite(match.bandwidthAbsoluteErrorHz) ||
          !std::isfinite(match.startAbsoluteErrorSeconds) ||
          !std::isfinite(match.endAbsoluteErrorSeconds)) {
        result.ok = false;
        result.error = "Matched-event error conversion overflow";
        result.metrics.clear();
        return result;
      }
      if (predictions[p].activity && truth[t].activity) {
        startErrors.push_back(match.startAbsoluteErrorSeconds);
        endErrors.push_back(match.endAbsoluteErrorSeconds);
      }
      metric.matches.push_back(std::move(match));
    }
    finishMetric(metric, result.predictionCount, result.truthCount, startErrors,
                 endErrors);
    result.metrics.push_back(std::move(metric));
  }
  return result;
}

DetectionDatasetEvaluation evaluateDetectionDataset(
    const std::vector<DetectionEvaluationFile>& files,
    const std::vector<double>& thresholds,
    DetectionEvaluationDimension dimension, std::size_t matrixCellBudget) {
  DetectionDatasetEvaluation dataset;
  if (!validThresholds(thresholds) || !matrixCellBudget ||
      files.size() > matrixCellBudget ||
      (dimension != DetectionEvaluationDimension::TimeFrequency &&
       dimension != DetectionEvaluationDimension::Frequency &&
       dimension != DetectionEvaluationDimension::Time)) {
    dataset.ok = false;
    dataset.error = "Invalid dataset evaluation configuration";
    return dataset;
  }
  for (const auto threshold : thresholds) {
    DetectionEvaluationMetric metric;
    metric.threshold = threshold;
    dataset.overall.metrics.push_back(std::move(metric));
  }
  std::unordered_set<std::string> identities;
  std::vector<std::vector<double>> startErrors(thresholds.size()),
      endErrors(thresholds.size());
  for (const auto& file : files) {
    if (file.id.empty() || !identities.insert(file.id).second ||
        (file.state != DetectionEvaluationFileState::Completed &&
         file.state != DetectionEvaluationFileState::Failed &&
         file.state != DetectionEvaluationFileState::NotProcessed)) {
      dataset.ok = false;
      dataset.error = "Invalid or duplicate evaluation file identity/state";
      dataset.overall.ok = false;
      return dataset;
    }
    const std::vector<DetectionEvaluationEvent> empty;
    const auto& predictions =
        file.state == DetectionEvaluationFileState::Completed ? file.predictions
                                                              : empty;
    auto result = evaluateDetections(predictions, file.truth, file.sampleRateHz,
                                     thresholds, dimension, matrixCellBudget);
    if (!result.ok) {
      dataset.ok = false;
      dataset.error = file.id + ": " + result.error;
      dataset.overall.ok = false;
      dataset.overall.error = dataset.error;
      return dataset;
    }
    const auto predictionOffset = dataset.overall.predictionCount;
    const auto truthOffset = dataset.overall.truthCount;
    for (std::size_t i = 0; i < result.metrics.size(); ++i)
      for (const auto& match : result.metrics[i].matches) {
        auto global = match;
        global.predictionIndex += predictionOffset;
        global.truthIndex += truthOffset;
        dataset.overall.metrics[i].matches.push_back(global);
        if (predictions[match.predictionIndex].activity &&
            file.truth[match.truthIndex].activity) {
          startErrors[i].push_back(match.startAbsoluteErrorSeconds);
          endErrors[i].push_back(match.endAbsoluteErrorSeconds);
        }
      }
    dataset.overall.predictionCount += result.predictionCount;
    dataset.overall.truthCount += result.truthCount;
    dataset.overall.unknownTimePredictions += result.unknownTimePredictions;
    dataset.overall.splitTruthCount += result.splitTruthCount;
    dataset.overall.mergedPredictionCount += result.mergedPredictionCount;
    dataset.files.push_back(
        {file.id, file.state, file.failureReason, std::move(result)});
  }
  auto& overall = dataset.overall;
  overall.unknownTimeFraction =
      overall.predictionCount
          ? static_cast<double>(overall.unknownTimePredictions) /
                overall.predictionCount
          : 0;
  for (std::size_t i = 0; i < overall.metrics.size(); ++i)
    finishMetric(overall.metrics[i], overall.predictionCount,
                 overall.truthCount, startErrors[i], endErrors[i]);
  return dataset;
}

}  // namespace signalstudio
