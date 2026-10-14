#include "infrastructure/detection_time_analysis.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>

#include "domain/detection_refinement.h"
#include "infrastructure/detection_sample_energy.h"
#include "infrastructure/int16_iq_file.h"
#include "infrastructure/source_metadata.h"

namespace signalstudio {
namespace {
constexpr std::uint64_t kReadSamples = 131072;
constexpr std::size_t kTracePoints = 512;
bool resourceFallback(const QString& error) {
  return error.contains("working budget") ||
         error.contains("working memory budget");
}

bool stopped(const DetectionSourceCancel& cancel, QString& error) {
  if (!cancel || !cancel()) return false;
  error = "Cancelled during physical time refinement";
  return true;
}

QJsonObject evidenceJson(const DetectionRefinementEvidence& evidence) {
  QJsonObject result{
      {"numericalVersion", QString::fromStdString(evidence.numericalVersion)},
      {"energyMethod", QString::fromStdString(evidence.energyMethod)},
      {"continuousTrack", evidence.continuousTrack},
      {"effectiveEdgeRatio", evidence.effectiveEdgeRatio},
      {"effectiveMinOnSeconds", evidence.effectiveMinOnSeconds},
      {"effectiveMinOffSeconds", evidence.effectiveMinOffSeconds},
      {"effectiveSmoothSeconds", evidence.effectiveSmoothSeconds},
      {"effectiveCoarseMinBurstSeconds",
       evidence.effectiveCoarseMinBurstSeconds},
      {"effectiveCoarseSplitGapSeconds",
       evidence.effectiveCoarseSplitGapSeconds},
      {"sourceOriginSample", QString::number(evidence.sourceOriginSample)},
      {"sourceStepSamples", QString::number(evidence.sourceStepSamples)},
      {"sourceSupportEndSample",
       QString::number(evidence.sourceSupportEndSample)},
      {"smoothingSamples", QString::number(evidence.smoothingSamples)},
      {"filterSupportSamples", QString::number(evidence.filterSupportSamples)},
      {"fftSupportUncertaintySamples",
       QString::number(evidence.fftSupportUncertaintySamples)},
      {"quantizationUncertaintySamples",
       QString::number(evidence.quantizationUncertaintySamples)},
      {"minOnSamples", QString::number(evidence.minOnSamples)},
      {"minOffSamples", QString::number(evidence.minOffSamples)},
      {"fineParametersApplied", evidence.fineParametersApplied},
      {"durationParametersApplied", evidence.durationParametersApplied},
      {"roiSmoothingCells", static_cast<int>(evidence.roiSmoothingCells)},
      {"coarseParametersApplied", evidence.coarseParametersApplied},
      {"splittingApplied", evidence.splittingApplied}};
  if (evidence.noise) result["noise"] = *evidence.noise;
  if (evidence.burst) result["burst"] = *evidence.burst;
  if (evidence.threshold) result["threshold"] = *evidence.threshold;
  if (evidence.comparisonThreshold)
    result["comparisonThreshold"] = *evidence.comparisonThreshold;
  if (evidence.onFraction) result["onFraction"] = *evidence.onFraction;
  if (evidence.roiTimeThreshold && evidence.roiFrequencyThreshold) {
    result["roiThresholdDomain"] = "positive-db-residual-mean-v1";
    result["roiTimeThreshold"] = *evidence.roiTimeThreshold;
    result["roiFrequencyThreshold"] = *evidence.roiFrequencyThreshold;
    result["roiPositiveMean"] = *evidence.roiPositiveMean;
    result["roiOffFraction"] = *evidence.roiOffFraction;
  }
  return result;
}

QJsonObject energyTrace(const DetectionEnergySeries& series,
                        double smoothCells) {
  QJsonArray points;
  const auto smooth =
      std::max<std::size_t>(1, static_cast<std::size_t>(smoothCells));
  double sum = 0, bucketSum = 0;
  std::size_t bucketCount = 0;
  const auto bucket = std::max<std::size_t>(
      1, (series.power.size() + kTracePoints - 1) / kTracePoints);
  for (std::size_t i = 0; i < series.power.size(); ++i) {
    sum += series.power[i];
    if (i >= smooth) sum -= series.power[i - smooth];
    bucketSum += sum / smooth;
    ++bucketCount;
    if (bucketCount == bucket || i + 1 == series.power.size()) {
      points.append(QJsonArray{
          QString::number(series.originSample +
                          (i + 1 - bucketCount) * series.stepSamples),
          bucketSum / bucketCount});
      bucketCount = 0;
      bucketSum = 0;
    }
  }
  return {{"kind", "measured-causal-mean-power-v1"},
          {"sourceOriginSample", QString::number(series.originSample)},
          {"sourceSupportEndSample", QString::number(series.supportEndSample)},
          {"aggregation", "consecutive-cell mean; at most 512 plotted points"},
          {"points", points}};
}

bool readSpectrum(FixedDetectionSource& source, const FrequencyTrack& track,
                  DetectionSpectrumRoi& spectrum, std::uint64_t budget,
                  std::uint64_t& consumedCells, QString& error,
                  const DetectionSourceCancel& cancel) {
  const auto& plan = source.plan();
  const auto index = [&](double hz) {
    return static_cast<std::size_t>(std::clamp(
        std::ceil((hz - plan.frequencyFirstBinHz) / plan.frequencyStepHz), 0.,
        static_cast<double>(plan.config.fftSize - 1)));
  };
  const auto firstBin = index(track.frequency.lowerHz);
  const auto lastBin = index(track.frequency.upperHz);
  const auto width = lastBin - firstBin + 1;
  const auto first = std::min(plan.realFrameCount,
                              track.nominalSupport.begin / plan.hopSamples);
  const auto end =
      std::min(plan.realFrameCount,
               track.nominalSupport.end / plan.hopSamples +
                   (track.nominalSupport.end % plan.hopSamples != 0));
  if (first >= end || static_cast<long double>(end - first) * width * 4 +
                              width * sizeof(double) + 1048576 >
                          budget) {
    error = "Physical STFT ROI exceeds the time-refinement working budget";
    return false;
  }
  const auto cells = (end - first) * width;
  if (cells > (std::uint64_t{1} << 34) - consumedCells) {
    error = "Time-refinement cumulative cell work limit exceeded";
    return false;
  }
  consumedCells += cells;
  spectrum.fftSupportUncertaintySamples = plan.config.fftSize;
  spectrum.powerDb.reserve(static_cast<std::size_t>(end - first) * width);
  for (auto bin = firstBin; bin <= lastBin + 1; ++bin)
    spectrum.frequencyEdgesHz.push_back(plan.frequencyFirstBinHz +
                                        (static_cast<double>(bin) - .5) *
                                            plan.frequencyStepHz);
  for (auto frameIndex = first; frameIndex < end; ++frameIndex) {
    DetectionSpectrumFrame frame;
    if (!source.readFrame(frameIndex, frame, error, cancel)) return false;
    spectrum.timeSlots.push_back(frame.mapping.slotSamples);
    spectrum.powerDb.insert(spectrum.powerDb.end(),
                            frame.densityDb.begin() + firstBin,
                            frame.densityDb.begin() + lastBin + 1);
  }
  return true;
}

bool readEnergy(const FileMetadata& metadata, const FrequencyTrack& track,
                const DetectionRequest& request, DetectionEnergySeries& series,
                std::uint64_t budget, std::uint64_t& consumedCells,
                QString& error, const DetectionSourceCancel& cancel) {
  const auto margin =
      static_cast<SampleIndex>(std::floor(.008 * metadata.sampleRateHz));
  const auto begin = track.nominalSupport.begin > margin
                         ? track.nominalSupport.begin - margin
                         : 0;
  const auto available = request.sourceIdentity.availableSamples;
  const auto end =
      track.nominalSupport.end > available - std::min(available, margin)
          ? available
          : track.nominalSupport.end + margin;
  const auto count = end - begin;
  // During filtering IQ and float32 power coexist; during domain refinement
  // energy, smoothed energy and a quantile copy coexist. Reserve the larger.
  if (begin >= end || static_cast<long double>(count) * 12 + 2097152 > budget) {
    error = "Exact sample search context exceeds the working budget";
    return false;
  }
  if (count > (std::uint64_t{1} << 34) - consumedCells) {
    error = "Time-refinement cumulative cell work limit exceeded";
    return false;
  }
  consumedCells += count;
  RawSampleReader reader;
  if (!reader.open(QString::fromStdString(metadata.path), metadata.sampleFormat,
                   error))
    return false;
  std::vector<std::complex<float>> samples(static_cast<std::size_t>(count));
  for (auto sample = begin; sample < end;) {
    std::vector<std::complex<float>> block;
    const auto after = sample + std::min(kReadSamples, end - sample);
    if (!reader.read({sample, after}, block, cancel, &error)) return false;
    std::copy(block.begin(), block.end(), samples.begin() + (sample - begin));
    sample = after;
  }
  series.originSample = begin;
  series.supportEndSample = end;
  series.stepSamples = 1;
  series.filterSupportSamples = count;
  series.energyMethod = "cpu-exact-length-complex64-raised-cosine-fft-v1";
  return detectionBandEnergy(
      std::move(samples), metadata.sampleRateHz,
      (track.frequency.lowerHz + track.frequency.upperHz) / 2 -
          metadata.centerFrequencyHz,
      track.frequency.upperHz - track.frequency.lowerHz, series.power, budget,
      error, cancel);
}
}  // namespace

bool applyDetectionTimeAnalysis(
    FixedDetectionSource& source, const FileMetadata& metadata,
    const DetectionRequest& request, const std::vector<FrequencyTrack>& tracks,
    DetectionPublication& publication, QString& error,
    const DetectionSourceCancel& cancel,
    const std::function<void(std::size_t, std::size_t)>& progress) {
  auto refined = publication;
  const auto working = request.workingSetBudgetBytes * 3 / 4;
  std::uint64_t totalCells = 0;
  std::size_t completed = 0;
  if (progress) progress(0, refined.annotations.size());
  for (auto& annotation : refined.annotations) {
    if (stopped(cancel, error)) return false;
    const auto track =
        std::find_if(tracks.begin(), tracks.end(), [&](const auto& item) {
          return detectionTrackRecordId(item.seedId) ==
                 annotation.originRecordId;
        });
    if (track == tracks.end() || !annotation.originEvidence) {
      error = "Time refinement lacks its immutable frequency track";
      return false;
    }
    DetectionRefinementInput input;
    input.candidateId = annotation.id;
    input.sampleRateHz = metadata.sampleRateHz;
    input.windowCoverage = track->windowCoverage;
    input.observation = {track->nominalSupport, track->frequency};
    input.publishBounds = request.requestedRange;
    DetectionRefinementOptions options;
    options.workingCellBudget = static_cast<std::size_t>(working / 12);
    options.cancelled = cancel;
    QJsonObject trace;
    QString fallbackError;
    if (request.parameters.timeStrategy == DetectionTimeStrategy::Sample) {
      input.energy.emplace();
      if (!readEnergy(metadata, *track, request, *input.energy, working,
                      totalCells, fallbackError, cancel)) {
        if (stopped(cancel, error)) return false;
        if (!resourceFallback(fallbackError)) {
          error = fallbackError;
          return false;
        }
        input.energy.reset();
      } else {
        trace = energyTrace(*input.energy, request.parameters.smoothSeconds *
                                               metadata.sampleRateHz);
      }
    } else if (request.parameters.timeStrategy == DetectionTimeStrategy::Roi ||
               request.parameters.timeStrategy ==
                   DetectionTimeStrategy::Coarse) {
      input.spectrum.emplace();
      if (!readSpectrum(source, *track, *input.spectrum, working, totalCells,
                        fallbackError, cancel)) {
        if (stopped(cancel, error)) return false;
        if (!resourceFallback(fallbackError)) {
          error = fallbackError;
          return false;
        }
        input.spectrum.reset();
      }
    }
    if (totalCells > (std::uint64_t{1} << 34)) {
      error = "Time-refinement cumulative cell work limit exceeded";
      return false;
    }
    auto effectiveParameters = request.parameters;
    if (effectiveParameters.timeStrategy == DetectionTimeStrategy::Coarse)
      effectiveParameters.bandwidthMarginDb =
          DetectionParameters{}.bandwidthMarginDb;
    auto result = refineDetection(input, effectiveParameters, options);
    QJsonObject fallbackCoarseEvidence;
    if (!result.ok) {
      error = QString::fromStdString(result.error);
      return false;
    }
    const auto strategy = request.parameters.timeStrategy;
    if (result.status == RefineStatus::Fallback &&
        (strategy == DetectionTimeStrategy::Sample ||
         strategy == DetectionTimeStrategy::Roi)) {
      // Only a separately measured coarse result can supply fallback activity.
      // Nominal model support alone remains an unknown-time observation.
      if (!input.spectrum) {
        input.spectrum.emplace();
        QString spectrumError;
        if (!readSpectrum(source, *track, *input.spectrum, working, totalCells,
                          spectrumError, cancel)) {
          if (stopped(cancel, error)) return false;
          if (!resourceFallback(spectrumError)) {
            error = spectrumError;
            return false;
          }
          input.spectrum.reset();
          result.fallbackReason += "; " + spectrumError.toStdString();
        }
      }
      if (input.spectrum) {
        const auto coarseParameters =
            coarseDetectionFallbackParameters(request.parameters);
        const auto coarse = refineDetection(input, coarseParameters, options);
        if (!coarse.ok) {
          error = QString::fromStdString(coarse.error);
          return false;
        }
        fallbackCoarseEvidence = evidenceJson(coarse.evidence);
        fallbackCoarseEvidence["parameterPolicy"] =
            "fixed-default-fallback; inactive-coarse-user-values-ignored";
        fallbackCoarseEvidence["status"] =
            coarse.status == RefineStatus::Applied ? "applied" : "fallback";
        fallbackCoarseEvidence["fallbackReason"] =
            QString::fromStdString(coarse.fallbackReason);
        fallbackCoarseEvidence["thresholdDomain"] = "spectrum-density-db-v1";
        if (coarse.status == RefineStatus::Applied) {
          result.activity = coarse.activity;
          result.fallbackReason += "; measured-coarse-fallback";
        }
      }
    }
    if (!fallbackError.isEmpty())
      result.fallbackReason += "; " + fallbackError.toStdString();
    auto origin = std::make_shared<OriginEvidence>(*annotation.originEvidence);
    origin->refineMethod = result.method;
    origin->refineStatus = result.status;
    origin->fallbackReason = result.fallbackReason;
    origin->originalSegments = result.activity;
    auto fields = QJsonDocument::fromJson(
                      QByteArray::fromStdString(origin->originalFieldsJson))
                      .object();
    auto time = evidenceJson(result.evidence);
    time["coarseDefaultNoiseMarginDb"] =
        DetectionParameters{}.bandwidthMarginDb;
    if (!fallbackCoarseEvidence.isEmpty())
      time["fallbackCoarseEvidence"] = fallbackCoarseEvidence;
    time["windowCoverageIsDutyCycle"] = false;
    time["sourceFingerprint"] =
        QString::fromStdString(request.sourceIdentity.quickFingerprint);
    if (!trace.isEmpty()) time["trace"] = trace;
    fields["timeAnalysis"] = time;
    origin->originalFieldsJson =
        QJsonDocument(fields).toJson(QJsonDocument::Compact).toStdString();
    annotation.originEvidence = std::move(origin);
    annotation.segments = std::move(result.activity);
    annotation.activeSegmentId =
        annotation.segments.empty() ? "" : annotation.segments.front().id;
    annotation.name = request.profile.backend == DetectionBackendKind::Scn
                          ? "SCN 最大谱检测"
                          : "FFSCN IQ 检测";
    for (const auto& segment : annotation.segments)
      annotation.revisions.front().segmentChanges.push_back(
          {segment.id, std::nullopt, segment});
    refreshAnnotationProjection(annotation);
    if (progress) progress(++completed, refined.annotations.size());
  }
  if (stopped(cancel, error)) return false;
  if (iqSourceFingerprint(QString::fromStdString(metadata.path))
          .toStdString() != request.sourceIdentity.quickFingerprint) {
    error = "Source identity changed during time refinement";
    return false;
  }
  if (!validateDetectionPublicationBudget(
          refined, request.workingSetBudgetBytes / 2, error))
    return false;
  publication = std::move(refined);
  return true;
}
}  // namespace signalstudio
