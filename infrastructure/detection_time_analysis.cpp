#include "infrastructure/detection_time_analysis.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include "domain/detection_refinement.h"
#include "infrastructure/detection_sample_energy.h"
#include "infrastructure/detection_sample_stream.h"
#include "infrastructure/int16_iq_file.h"
#include "infrastructure/source_metadata.h"

namespace signalstudio {
namespace {
constexpr std::uint64_t kReadSamples = 131072;
constexpr std::size_t kTracePoints = 512;
// Controlled 20 MS/s .1s measurements favor FIR above this short-context range;
// uncertainty still retries complete Legacy within its original working grant.
constexpr SampleIndex kAutoExactSamples=1U<<20;
// Independent process-wide disk leases. The producer's candidate-local quota
// and the actual filesystem free-space checks remain additional constraints.
std::atomic<std::uint64_t> scratchLeasedBytes{0};
constexpr std::uint64_t kProcessScratchCap=32ULL*1024*1024*1024;
class ScratchGrant {
 public:
  ~ScratchGrant(){if(held_) scratchLeasedBytes.fetch_sub(held_);}
  bool reserve(std::uint64_t bytes,QString& error) {
    auto used=scratchLeasedBytes.load();
    do {
      if(bytes>kProcessScratchCap || used>kProcessScratchCap-bytes) {
        error="Process-wide Sample temporary storage lease is exhausted";return false;
      }
    } while(!scratchLeasedBytes.compare_exchange_weak(used,used+bytes));
    held_+=bytes;return true;
  }
  void release(std::uint64_t bytes) {
    if(bytes>held_){invalid_=true;return;}
    held_-=bytes;scratchLeasedBytes.fetch_sub(bytes);
  }
  bool returned() const {return !held_ && !invalid_;}
 private:
  std::uint64_t held_=0;bool invalid_=false;
};
bool resourceFallback(DetectionFailureCode code) {
  return code == DetectionFailureCode::ResourceLimit;
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

bool readSpectrumBody(FixedDetectionSource& source, const FrequencyTrack& track,
                  DetectionSpectrumRoi& spectrum, std::uint64_t budget,
                  std::uint64_t& consumedCells, std::size_t& cellBudget,QString& error,
                  const DetectionSourceCancel& cancel,
                  DetectionFailureCode& code) {
  code = DetectionFailureCode::HardFailure;
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
  // Spectrum consumers retain the float32 matrix, bounded ROI resize or
  // Coarse row/profile work, and source FFT scratch. Sample's three full
  // vectors are not live here and must not define this matrix's cell limit.
  const long double auxiliary=plan.estimatedPeakBytes+static_cast<long double>(width+1)*16+
      static_cast<long double>(end-first)*52+4*1048576;
  cellBudget=auxiliary>=budget ? 0 : static_cast<std::size_t>(std::min<long double>(
      256ULL*1024*1024,(budget-auxiliary)/sizeof(float)));
  const auto times = end > first ? end - first : 0;
  const auto edgeCount = width + 1;
  const double firstHz = plan.frequencyFirstBinHz +
                         (static_cast<double>(firstBin) - .5) * plan.frequencyStepHz;
  const double lastHz = plan.frequencyFirstBinHz +
                        (static_cast<double>(lastBin + 1) - .5) * plan.frequencyStepHz;
  const auto requiredCells = static_cast<long double>(times) * width;
  if (first >= end) {
    error = "Physical STFT ROI has no real frames";
    return false;
  }
  if (requiredCells > cellBudget) {
    code = DetectionFailureCode::WorkingCellLimit;
    error = QString("Spectrum capacity rejected: times=%1 frequencyEdges=%2 requiredCells=%3 allowedCells=%4 frequencyEdgeHz=%5..%6")
                .arg(qulonglong(times))
                .arg(qulonglong(edgeCount))
                .arg(QString::number(requiredCells, 'f', 0))
                .arg(qulonglong(cellBudget))
                .arg(firstHz, 0, 'g', 12)
                .arg(lastHz, 0, 'g', 12);
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
  // One densityDb record per real frame. Do not treat a model-window tensor
  // as a single-frame spectrum. A short file read falls back to readFrame.
  bool useSpool = source.frequencySpoolCovers(first, end);
  if (useSpool) {
    for (auto frameIndex = first; frameIndex < end; ++frameIndex) {
      std::vector<float> density;
      TimeRange slot;
      bool ioFailure = false;
      const bool read = source.replayFrequencyFrame(
          frameIndex, density, slot, ioFailure, error, cancel);
      const bool usable =
          read && density.size() == plan.config.fftSize &&
          std::all_of(density.begin(), density.end(),
                      [](float value) { return std::isfinite(value); });
      if (!usable) {
        if (!read && !ioFailure) return false;
        spectrum.timeSlots.clear();
        spectrum.powerDb.clear();
        error.clear();
        code = DetectionFailureCode::HardFailure;
        useSpool = false;
        break;
      }
      spectrum.timeSlots.push_back(slot);
      spectrum.powerDb.insert(spectrum.powerDb.end(),
                              density.begin() + firstBin,
                              density.begin() + lastBin + 1);
    }
    if (useSpool) return true;
  }
  for (auto frameIndex = first; frameIndex < end; ++frameIndex) {
    DetectionSpectrumFrame frame;
    if (!source.readFrame(frameIndex, frame, error, cancel)) return false;
    spectrum.timeSlots.push_back(frame.mapping.slotSamples);
    spectrum.powerDb.insert(spectrum.powerDb.end(),
                            frame.densityDb.begin() + firstBin,
                            frame.densityDb.begin() + lastBin + 1);
  }
  code = DetectionFailureCode::None;
  return true;
}

bool readEnergy(const FileMetadata& metadata, const FrequencyTrack& track,
                const DetectionRequest& request, DetectionEnergySeries& series,
                std::uint64_t budget, std::uint64_t& consumedCells,
                QString& error, const DetectionSourceCancel& cancel,
                DetectionFailureCode& code) {
  code = DetectionFailureCode::HardFailure;
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
  const auto requiredBytes = static_cast<long double>(count) * 12 + 2097152;
  if (begin >= end) {
    error = "Exact sample search context is empty";
    return false;
  }
  if (requiredBytes > budget) {
    code = DetectionFailureCode::ResourceLimit;
    error = QString("Energy working-set exceeded: requiredBytes=%1 allowedBytes=%2")
                .arg(QString::number(requiredBytes, 'f', 0))
                .arg(qulonglong(budget));
    return false;
  }
  if (count > (std::uint64_t{1} << 34) - consumedCells) {
    error = "Time-refinement cumulative cell work limit exceeded";
    return false;
  }
  consumedCells += count;
  RawSampleReader reader;
  if (!reader.open(QString::fromStdString(metadata.path), metadata.sampleFormat,
                   error,RawSampleReader::Access::Sequential))
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
  DetectionFailureCode bandCode = DetectionFailureCode::HardFailure;
  const auto measured = detectionBandEnergy(
      std::move(samples), metadata.sampleRateHz,
      (track.frequency.lowerHz + track.frequency.upperHz) / 2 -
          metadata.centerFrequencyHz,
      track.frequency.upperHz - track.frequency.lowerHz, series.power, budget,
      error, cancel, {}, &bandCode);
  code = measured ? DetectionFailureCode::None
                  : bandCode == DetectionFailureCode::None
                        ? DetectionFailureCode::HardFailure
                        : bandCode;
  return measured;
}

bool boundedSample(FixedDetectionSource& source,const FileMetadata& metadata,const FrequencyTrack& track,
    const DetectionRequest& request,const DetectionRefinementInput& input,
    const DetectionRefinementOptions& options,std::uint64_t working,std::uint64_t& cells,
    const QString& scratchRoot,sample_stream::Result& output,QJsonObject& report,
    QString& error) {
  namespace stream=sample_stream;
  RawSampleReader reader;
  if(!reader.open(QString::fromStdString(metadata.path),metadata.sampleFormat,error,
      RawSampleReader::Access::Sequential)) return false;
  if(reader.sampleCount()!=request.sourceIdentity.physicalSamples) {
    error="Physical source length changed before Sample stream";return false;
  }
  const auto margin=static_cast<SampleIndex>(std::floor(.008*metadata.sampleRateHz));
  const auto available=request.sourceIdentity.availableSamples;
  stream::Context context;
  context.input=input;context.parameters=request.parameters;context.options=options;
  context.availableSamples=available;
  context.searchSamples={track.nominalSupport.begin>margin?track.nominalSupport.begin-margin:0,
      track.nominalSupport.end>available-std::min(available,margin)?available:track.nominalSupport.end+margin};
  context.relativeCenterHz=(track.frequency.lowerHz+track.frequency.upperHz)/2-metadata.centerFrequencyHz;
  context.bandwidthHz=track.frequency.upperHz-track.frequency.lowerHz;
  stream::Config config;
  // Both application modes retain complete precision fallback. StreamingFir
  // requests a FIR attempt first; it never authorizes publishing an uncertain
  // diagnostic instead of an eligible LegacyExact result.
  config.mode=stream::Mode::Auto;
  config.scratchBudgetBytes=request.sampleScratchBudgetBytes;
  config.scratchFreeFloorBytes=0;
  stream::Runtime runtime;
  // FIR K/C/radius remain fixed and bounded. Precision fallback may use the
  // SAME full working grant previously available to LegacyExact, after FIR
  // workspace and scratch have been released, to avoid unnecessary degradation.
  runtime.memoryGrantBytes=working;
  runtime.resultByteGrant=std::min<std::uint64_t>(runtime.memoryGrantBytes/4,16ULL*1024*1024);
  const auto encodedReadBudget=std::min<std::uint64_t>(16ULL*1024*1024,runtime.memoryGrantBytes/8);
  runtime.readerResidentReserveBytes=encodedReadBudget+kReadSamples*sizeof(std::complex<float>)+65536;
  const auto stride=metadata.sampleFormat.channelLayout==ChannelLayout::TimeInterleaved
      ? metadata.sampleFormat.channels:1;
  const auto encodedPerSample=2ULL*bytesPerComponent(metadata.sampleFormat)*stride;
  runtime.sourceReadBlockSamples=static_cast<std::uint32_t>(std::min<std::uint64_t>(
      kReadSamples,encodedReadBudget/std::max<std::uint64_t>(1,encodedPerSample)));
  runtime.privateScratchRoot=scratchRoot.isEmpty()
      ? QDir::temp().filePath("SignalStudio/detection-sample"):scratchRoot;
  runtime.work.consumedCells=&cells;
  runtime.verifySource=[&](QString& why) {
    if(iqSourceFingerprint(QString::fromStdString(metadata.path)).toStdString()!=request.sourceIdentity.quickFingerprint) {
      why="Frozen source identity changed during bounded Sample refinement";return false;
    }
    return true;
  };
  runtime.read=[&](TimeRange range,std::span<std::complex<float>> destination,QString& why) {
    if(range.end-range.begin!=destination.size() || destination.size()>runtime.sourceReadBlockSamples) {
      why="Bounded Sample reader request exceeds its format-aware block plan";return false;
    }
    std::vector<std::complex<float>> decoded;
    if(!reader.read(range,decoded,options.cancelled,&why)) return false;
    if(decoded.size()!=destination.size()){why="Short Sample IQ block";return false;}
    std::copy(decoded.begin(),decoded.end(),destination.begin());return true;
  };
  ScratchGrant grant;
  runtime.reserveScratch=[&](std::uint64_t bytes,QString& why){return grant.reserve(bytes,why);};
  runtime.releaseScratch=[&](std::uint64_t bytes){grant.release(bytes);};
  stream::Failure failure;
  stream::Plan legacyPlan;
  auto exactConfig=config;exactConfig.mode=stream::Mode::LegacyExact;
  // Small contexts retain the existing exact arithmetic without first paying
  // for a diagnostic FIR pass. Long contexts use the bounded numerical route.
  if(request.sampleEnergyMode==DetectionSampleEnergyMode::Auto &&
      !source.gpuProfiling()["ready"].toBool() &&
      context.searchSamples.end-context.searchSamples.begin<=kAutoExactSamples &&
      stream::makePlan(context,exactConfig,runtime,legacyPlan,failure) && legacyPlan.legacyEligible)
    config.mode=stream::Mode::LegacyExact;
  QString gpuFallback;
  const auto okay=source.withGpu(options.cancelled,[&](gpu_dsp::Session* gpu) {
    runtime.gpu=gpu;
    if(stream::run(context,config,runtime,output,failure)) return true;
    if(!gpu || failure.code!=stream::ErrorCode::Integration || !failure.stage.startsWith("gpu-") ||
        !failure.cleanupFailure.isEmpty() || (options.cancelled && options.cancelled())) return false;
    // The partial spool is closed before retry. Charge both attempts to the
    // existing work ledger, and never concatenate CPU/GPU candidate energies.
    gpuFallback=failure.stage+": "+failure.detail;
    runtime.gpu=nullptr;
    return stream::run(context,config,runtime,output,failure);
  });
  report={{"requestedMode",static_cast<int>(request.sampleEnergyMode)},
      {"gpuFirBlocks",QString::number(output.statistics.gpuFirBlocks)},
      {"gpuFallback",gpuFallback.left(1024)},
      {"autoExactSampleCutoff",QString::number(kAutoExactSamples)},
      {"selectedRoute",static_cast<int>(output.route)},
      {"publicationEligible",output.publicationEligible},
      {"nativeEnvelopeEstablished",output.nativeEnvelopeEstablished},
      {"precisionPolicy",output.precisionPolicy},
      {"precisionGuardEvaluated",output.precisionGuardEvaluated},
      {"precisionGuardPassed",output.precisionGuardPassed},
      {"precisionReason",output.precisionReason},
      {"uncertainComparisonCells",QString::number(output.statistics.uncertainComparisonCells)},
      {"empiricalSmoothAllowance",output.statistics.empiricalSmoothAllowance},
      {"routeReason",output.autoReason},{"contextSamples",QString::number(context.searchSamples.end-context.searchSamples.begin)},
      {"methodVersion",QString::fromStdString(output.refinement.evidence.energyMethod)},
      {"firRadius",int(output.statistics.plan.radius)},{"fftSamples",int(output.statistics.plan.fftSamples)},
      {"blockSamples",int(output.statistics.plan.outputBlockSamples)},
      {"firTailBound",output.statistics.plan.tailBound},
      {"estimatedResidentBytes",QString::number(output.statistics.plan.estimatedResidentBytes)},
      {"legacyResidentEstimateBytes",QString::number(output.statistics.plan.legacyResidentEstimateBytes)},
      {"sourceSamplesRead",QString::number(output.statistics.sourceSamplesRead)},
      {"scratchBytesRead",QString::number(output.statistics.scratchBytesRead)},
      {"scratchBytesWritten",QString::number(output.statistics.scratchBytesWritten)},
      {"scratchPeakBytes",QString::number(output.statistics.scratchPeakBytes)},
      {"scope","versioned FIR is approximate; native numerical comparisons and sensitivity guards required; no universal equivalence theorem"}};
  if(!okay) {
    error=failure.stage+": "+failure.detail;
    report["failureCode"]=static_cast<int>(failure.code);
    report["cleanupFailure"]=failure.cleanupFailure;
    report["resourceFallback"]=failure.code==stream::ErrorCode::ResourceLimit && failure.cleanupFailure.isEmpty();
    return false;
  }
  if(!grant.returned()){error="Sample temporary storage lease was not returned correctly";return false;}
  return true;
}
}  // namespace

bool readSpectrum(FixedDetectionSource& source, const FrequencyTrack& track,
                  DetectionSpectrumRoi& spectrum, std::uint64_t budget,
                  std::uint64_t& consumedCells, std::size_t& cellBudget,
                  QString& error, const DetectionSourceCancel& cancel,
                  DetectionFailureCode* failure) {
  DetectionFailureCode code = DetectionFailureCode::HardFailure;
  const auto ok = readSpectrumBody(source, track, spectrum, budget, consumedCells,
                                   cellBudget, error, cancel, code);
  if (failure) *failure = ok ? DetectionFailureCode::None : code;
  return ok;
}

bool applyDetectionTimeAnalysis(
    FixedDetectionSource& source, const FileMetadata& metadata,
    const DetectionRequest& request, const std::vector<FrequencyTrack>& tracks,
    DetectionPublication& publication, QString& error,
    const DetectionSourceCancel& cancel,
    const std::function<void(std::size_t, std::size_t)>& progress,
    const QString& privateScratchRoot) {
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
    QJsonObject streamEvidence;
    std::optional<DetectionRefinementResult> sampleResult;
    std::size_t spectrumCellBudget=options.workingCellBudget;
    QString fallbackError;
    if (request.parameters.timeStrategy==DetectionTimeStrategy::Sample &&
        request.sampleEnergyMode!=DetectionSampleEnergyMode::LegacyExact) {
      sample_stream::Result streamed;
      if(!boundedSample(source,metadata,*track,request,input,options,working,totalCells,privateScratchRoot,
          streamed,streamEvidence,fallbackError)) {
        if(stopped(cancel,error)) return false;
        if(!streamEvidence["resourceFallback"].toBool()){error=fallbackError;return false;}
      } else {
        sampleResult=std::move(streamed.refinement);
        if(!streamed.publicationEligible && sampleResult->status==RefineStatus::Applied) {
          sampleResult->activity.clear();sampleResult->status=RefineStatus::Fallback;
          sampleResult->fallbackReason="Sample FIR precision not eligible for publication; "+streamed.autoReason.toStdString();
        }
        QJsonArray points;
        for(const auto& point:streamed.statistics.trace)
          points.append(QJsonArray{QString::number(point.sample),point.mean});
        trace={{"kind","measured-causal-mean-power-v1"},{"points",points},
            {"sourceOriginSample",QString::number(sampleResult->evidence.sourceOriginSample)},
            {"sourceSupportEndSample",QString::number(sampleResult->evidence.sourceSupportEndSample)},
            {"aggregation","consecutive-cell mean; at most 512 plotted points"}};
      }
    } else if (request.parameters.timeStrategy == DetectionTimeStrategy::Sample) {
      input.energy.emplace();
      DetectionFailureCode energyCode = DetectionFailureCode::HardFailure;
      if (!readEnergy(metadata, *track, request, *input.energy, working,
                      totalCells, fallbackError, cancel, energyCode)) {
        if (stopped(cancel, error)) return false;
        if (!resourceFallback(energyCode)) {
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
      DetectionFailureCode spectrumCode = DetectionFailureCode::HardFailure;
      if (!readSpectrum(source, *track, *input.spectrum, working, totalCells,spectrumCellBudget,
                        fallbackError, cancel, &spectrumCode)) {
        if (stopped(cancel, error)) return false;
        if (!resourceFallback(spectrumCode)) {
          error = fallbackError;
          return false;
        }
        input.spectrum.reset();
      } else options.workingCellBudget=spectrumCellBudget;
    }
    if (totalCells > (std::uint64_t{1} << 34)) {
      error = "Time-refinement cumulative cell work limit exceeded";
      return false;
    }
    auto effectiveParameters = request.parameters;
    if (effectiveParameters.timeStrategy == DetectionTimeStrategy::Coarse)
      effectiveParameters.bandwidthMarginDb =
          DetectionParameters{}.bandwidthMarginDb;
    auto result = sampleResult ? std::move(*sampleResult) : refineDetection(input, effectiveParameters, options);
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
      input.energy.reset(); // fine result/trace is frozen; Coarse does not use E
      if (!input.spectrum) {
        input.spectrum.emplace();
        QString spectrumError;
        DetectionFailureCode spectrumCode = DetectionFailureCode::HardFailure;
        if (!readSpectrum(source, *track, *input.spectrum, working, totalCells,spectrumCellBudget,
                          spectrumError, cancel, &spectrumCode)) {
          if (stopped(cancel, error)) return false;
          if (!resourceFallback(spectrumCode)) {
            error = spectrumError;
            return false;
          }
          input.spectrum.reset();
          result.fallbackReason += "; " + spectrumError.toStdString();
        } else options.workingCellBudget=spectrumCellBudget;
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
    if(input.spectrum) {
      time["stftWorkingCellBudget"]=QString::number(spectrumCellBudget);
      time["stftInputCells"]=QString::number(input.spectrum->powerDb.size());
    }
    if(!streamEvidence.isEmpty()) time["boundedSample"]=streamEvidence;
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
