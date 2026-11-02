#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include "infrastructure/detection_sample_stream.h"
#include "tools/detection_benchmark/quality.h"
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
using namespace signalstudio;
namespace ss=signalstudio::sample_stream;
namespace {
void require(bool okay,const QString& error){if(!okay)throw std::runtime_error(error.toUtf8().toStdString());}
QJsonObject resources() {
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX usage{};usage.cb=sizeof(usage);
  QJsonObject value;
  if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&usage),sizeof(usage)))
    value={{"rssBytes",double(usage.WorkingSetSize)},{"peakRssBytes",double(usage.PeakWorkingSetSize)},
        {"privateBytes",double(usage.PrivateUsage)}};
  FILETIME created{},exited{},kernel{},user{};
  if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) {
    const auto ticks=[](FILETIME t){return (quint64(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    value["cpuMilliseconds"]=double(ticks(kernel)+ticks(user))/10000;
  }
  return value;
#else
  return {{"available",false}};
#endif
}
std::uint64_t mix(std::uint64_t x) {
  x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
  x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);
}
float noise(std::uint64_t x){return float(mix(x)>>40)*(2.f/16777216.f)-1.f;}
QJsonArray segments(const std::vector<Segment>& activity) {
  QJsonArray result;
  for(const auto& s:activity) result.append(QJsonObject{{"startSample",QString::number(s.range.time.begin)},
      {"endSample",QString::number(s.range.time.end)},{"uncertaintySamples",QString::number(s.uncertaintySamples)}});
  return result;
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  QCommandLineParser args;args.addHelpOption();
  args.setApplicationDescription("Bounded CPU Sample benchmark: stateless synthetic IQ, no model or field-RF claim");
  args.addOptions({{"output","New output directory","path"},
      {"seconds","Physical duration","seconds","0.1"},
      {"sample-rate","Actual sample rate, not a reduced-duration proxy","Hz","20000000"},
      {"scenario","continuous/bursts/blank/low-snr/overlap/hopping","name","bursts"},
      {"mode","legacy/stream/auto","name","stream"},
      {"dsp-compute","Explicit CPU reference or required GPU DSP: cpu/gpu","name","cpu"},
      {"block","Output block cap","samples","131072"},
      {"memory-mib","Total helper grant","MiB","96"},
      {"scratch-mib","Independent temporary storage cap","MiB","16384"},
      {"timeout","Cooperative cancellation deadline","seconds","1800"}});
  args.process(app);
  QJsonObject report{{"schema","signal-studio-sample-refinement-benchmark-v1"},{"pass",false},
      {"startedUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"source","deterministic stateless synthetic complex64 IQ callback; no full-source vector or model inference"},
      {"nativeUiVerified",false},{"fieldRfAccuracyVerified",false}};
  QString root;
  bool outputReady=false;
  QElapsedTimer clock;clock.start();
  try {
    require(args.isSet("output"),"--output required");root=QFileInfo(args.value("output")).absoluteFilePath();
    QDir directory(root);
    require(!directory.exists() || directory.entryList(QDir::AllEntries|QDir::NoDotAndDotDot).isEmpty(),"Output must be new/empty");
    require(directory.mkpath("."),"Cannot create output root");
    outputReady=true;
    const double fs=args.value("sample-rate").toDouble(),seconds=args.value("seconds").toDouble();
    require(std::isfinite(fs) && fs>=1 && fs<=1e12 && std::isfinite(seconds) && seconds>0 &&
        static_cast<long double>(seconds)*fs<=static_cast<long double>(std::uint64_t{1}<<34),"Invalid bounded duration/Fs");
    const auto count=static_cast<SampleIndex>(std::llround(seconds*fs));require(count>0,"No physical samples");
    const auto mode=args.value("mode"),scenario=args.value("scenario");
    require(mode=="legacy" || mode=="stream" || mode=="auto","Unknown numerical mode");
    require(QStringList{"continuous","bursts","blank","low-snr","overlap","hopping"}.contains(scenario),"Unknown synthetic scenario");
    const auto timeout=args.value("timeout").toDouble();require(std::isfinite(timeout) && timeout>0,"Invalid deadline");
    ss::Context context;
    context.input.candidateId="synthetic-sample-benchmark";context.input.sampleRateHz=fs;
    context.input.windowCoverage=scenario=="continuous"?.8:.2;
    context.relativeCenterHz=.1*fs;context.bandwidthHz=.24*fs;
    context.input.observation={{0,count},{context.relativeCenterHz-context.bandwidthHz/2,context.relativeCenterHz+context.bandwidthHz/2}};
    context.input.publishBounds={{0,count},{-fs/2,fs/2}};
    context.searchSamples={0,count};context.availableSamples=count;
    context.parameters.timeStrategy=DetectionTimeStrategy::Sample;
    context.options.workingCellBudget=96*1024*1024;
    context.options.cancelled=[&]{return clock.elapsed()>timeout*1000;};
    ss::Config config;
    config.mode=mode=="legacy"?ss::Mode::LegacyExact:mode=="stream"?ss::Mode::StreamingFir:ss::Mode::Auto;
    bool valid=false;
    config.outputBlockSamples=args.value("block").toUInt(&valid);require(valid,"Invalid block size");
    const auto scratchMiB=args.value("scratch-mib").toULongLong(&valid);require(valid && scratchMiB<=32768,"Invalid scratch cap");
    config.scratchBudgetBytes=scratchMiB*1024*1024;
    const auto memoryMiB=args.value("memory-mib").toULongLong(&valid);require(valid && memoryMiB>=16 && memoryMiB<=1024,"Invalid memory grant");
    ss::Runtime runtime;
    std::unique_ptr<gpu_dsp::Session> gpu;
    const auto compute=args.value("dsp-compute");
    require(compute=="cpu" || compute=="gpu","Invalid compute backend");
    if(compute=="gpu") {
      QDir deployed(QCoreApplication::applicationDirPath());
      gpu_dsp::Config gpuConfig;
      gpuConfig.moduleLibrary=deployed.filePath("SignalStudioGpuDspCuda.dll").toStdString();
      gpuConfig.cufftLibrary=deployed.filePath("cufft64_11.dll").toStdString();
      const auto jit=deployed.filePath("nvJitLink_120_0.dll");
      if(QFileInfo::exists(jit)) gpuConfig.nvJitLinkLibrary=jit.toStdString();
      gpuConfig.cancelled=context.options.cancelled;
      gpu_dsp::Capabilities caps;gpu_dsp::Error error;
      gpu=gpu_dsp::Session::create(gpuConfig,caps,error);
      report["gpuSetup"]=QJsonObject{{"device",QString::fromStdString(caps.deviceName)},
          {"ready",bool(gpu)},{"reason",QString::fromStdString(error.stage+": "+error.detail)}};
      require(bool(gpu),"Required GPU DSP unavailable: "+QString::fromStdString(error.detail));
      runtime.gpu=gpu.get();
    }
    report["dspCompute"]=compute;
    runtime.memoryGrantBytes=memoryMiB*1024*1024;
    runtime.resultByteGrant=std::min<std::uint64_t>(runtime.memoryGrantBytes/4,16ULL*1024*1024);
    runtime.readerResidentReserveBytes=0;
    runtime.sourceReadBlockSamples=131072;
    runtime.privateScratchRoot=directory.filePath("sample-scratch");
    std::uint64_t work=0,leased=0,maxLease=0;bool leaseError=false;
    runtime.work.consumedCells=&work;
    runtime.reserveScratch=[&](std::uint64_t bytes,QString& error) {
      if(bytes>config.scratchBudgetBytes || leased>config.scratchBudgetBytes-bytes){error="Benchmark lease cap exceeded";return false;}
      leased+=bytes;maxLease=std::max(maxLease,leased);return true;
    };
    runtime.releaseScratch=[&](std::uint64_t bytes){if(bytes>leased)leaseError=true;else leased-=bytes;};
    runtime.verifySource=[](QString&){return true;}; // immutable pure function below, not a file integrity substitute
    runtime.read=[&](TimeRange range,std::span<ss::Complex> destination,QString& error) {
      if(range.begin>=range.end || range.end>count || range.end-range.begin!=destination.size() || destination.size()>131072) {
        error="Synthetic callback range exceeds frozen support or block cap";return false;
      }
      for(std::size_t n=0;n<destination.size();++n) {
        if(!(n%4096) && context.options.cancelled()){error="Synthetic source cancelled";return false;}
        const auto j=range.begin+n;
        const double fraction=double(j)/count;
        const bool on=scenario=="continuous" ||
            (scenario!="blank" && ((fraction>=.1 && fraction<.4)||(fraction>=.6 && fraction<.9)));
        const double fc=scenario=="hopping" && fraction>=.5 ? .16 : .1;
        const double angle=std::fmod(2*std::numbers::pi*fc*double(j),2*std::numbers::pi);
        const float noiseAmplitude=scenario=="low-snr"?2.17794f:.02f;
        auto value=noiseAmplitude*ss::Complex{noise(j*2),noise(j*2+1)};
        if(scenario=="blank") value={0,0};
        if(on) value+=ss::Complex{float(std::cos(angle)),float(std::sin(angle))};
        if(scenario=="overlap" && fraction>=.3 && fraction<.7) {
          const double other=std::fmod(2*std::numbers::pi*.15*double(j),2*std::numbers::pi);
          value+=.7f*ss::Complex{float(std::cos(other)),float(std::sin(other))};
        }
        destination[n]=value;
      }
      return true;
    };
    report["scenario"]=scenario;report["sampleRateHz"]=fs;report["seconds"]=seconds;
    report["sourceSamples"]=QString::number(count);report["requestedMode"]=mode;
    report["noiseModel"]="SplitMix64 deterministic component uniforms; low-snr expected input ratio approximately -5dB, uncalibrated; blank is exact zero IQ";
    report["memoryGrantBytes"]=QString::number(runtime.memoryGrantBytes);
    report["scratchBudgetBytes"]=QString::number(config.scratchBudgetBytes);
    report["memoryBefore"]=resources();
    ss::Result result;ss::Failure failure;
    const bool okay=ss::run(context,config,runtime,result,failure);
    report["gpuFirBlocks"]=QString::number(result.statistics.gpuFirBlocks);
    if(gpu) {
      const auto& m=gpu->metrics();
      report["gpuMetrics"]=QJsonObject{{"setupMs",m.setupMs},{"fftMs",m.fftMs},
          {"h2dMs",m.h2dMs},{"d2hMs",m.d2hMs},{"kernelsMs",m.kernelsMs},
          {"hostMs",m.totalHostMs},{"fftExecutions",QString::number(m.fftExecutions)},
          {"devicePeakBytes",QString::number(m.devicePeakBytes)},
          {"timingMeaning",QString::fromStdString(m.timingMeaning)}};
    }
    const auto& p=result.statistics.plan;
    report["memoryAfter"]=resources();report["elapsedMs"]=clock.elapsed();
    report["helperReturned"]=okay;report["pass"]=okay && !leaseError && leased==0;
    report["leaseReturned"]=leased==0 && !leaseError;report["maximumLeaseBytes"]=QString::number(maxLease);
    report["sourceWorkCells"]=QString::number(work);report["failureCode"]=int(failure.code);
    report["failureStage"]=failure.stage;report["error"]=failure.detail;report["cleanupFailure"]=failure.cleanupFailure;
    report["route"]=int(result.route);report["publicationEligible"]=result.publicationEligible;
    report["needsMeasuredCoarse"]=result.needsMeasuredCoarse;
    report["status"]=int(result.refinement.status);report["segments"]=segments(result.refinement.activity);
    std::vector<benchmark::QualityBox> truth,predictions;
    const auto addTruth=[&](double first,double after) {
      const TimeRange time{static_cast<SampleIndex>(std::ceil(first*count)),static_cast<SampleIndex>(std::ceil(after*count))};
      if(time.begin<time.end) truth.push_back({time,context.input.observation.frequency});
    };
    if(scenario=="continuous") addTruth(0,1);
    else if(scenario=="overlap") addTruth(.1,.9);
    else if(scenario!="blank"){addTruth(.1,.4);addTruth(.6,.9);}
    for(const auto& segment:result.refinement.activity) predictions.push_back({segment.range.time,segment.range.frequency});
    report["rawProposalQuality"]=benchmark::matchBoxes(predictions,truth,true,fs);
    report["eligibleSampleOnlyQuality"]=benchmark::matchBoxes(result.publicationEligible?predictions:std::vector<benchmark::QualityBox>{},truth,true,fs);
    report["qualityScope"]="Single candidate band activity union, Sample only; no measured Coarse. Ineligible proposals cannot count as published activity; not multi-object model accuracy.";
    const auto& e=result.refinement.evidence;
    QJsonObject numeric{{"sourceOriginSample",QString::number(e.sourceOriginSample)},
        {"sourceSupportEndSample",QString::number(e.sourceSupportEndSample)},
        {"smoothingSamples",QString::number(e.smoothingSamples)},
        {"filterSupportSamples",QString::number(e.filterSupportSamples)}};
    if(e.noise) numeric["noise"]=*e.noise;if(e.burst) numeric["burst"]=*e.burst;
    if(e.threshold) numeric["threshold"]=*e.threshold;
    if(e.comparisonThreshold) numeric["comparisonThreshold"]=*e.comparisonThreshold;
    if(e.onFraction) numeric["onFraction"]=*e.onFraction;
    report["numericalEvidence"]=numeric;
    report["fallbackReason"]=QString::fromStdString(result.refinement.fallbackReason);
    report["energyMethod"]=QString::fromStdString(result.refinement.evidence.energyMethod);
    report["routeReason"]=result.autoReason;report["precisionPolicy"]=result.precisionPolicy;
    report["precisionReason"]=result.precisionReason;report["precisionGuardPassed"]=result.precisionGuardPassed;
    report["nativeEnvelopeEstablished"]=result.nativeEnvelopeEstablished;
    report["plan"]=QJsonObject{{"count",QString::number(p.count)},{"radius",int(p.radius)},{"fftSamples",int(p.fftSamples)},
        {"blockSamples",int(p.outputBlockSamples)},{"estimatedResidentBytes",QString::number(p.estimatedResidentBytes)},
        {"legacyResidentEstimateBytes",QString::number(p.legacyResidentEstimateBytes)},{"legacyEligible",p.legacyEligible},
        {"tailBound",p.tailBound},{"scratchPeakBytes",QString::number(result.statistics.scratchPeakBytes)},
        {"scratchBytesRead",QString::number(result.statistics.scratchBytesRead)},
        {"scratchBytesWritten",QString::number(result.statistics.scratchBytesWritten)},
        {"sourceSamplesRead",QString::number(result.statistics.sourceSamplesRead)}};
    report["boundary"]="Synthetic algorithm/resource evidence only; actual DSP backend recorded. No model, STFT fallback or formal publication; pass is execution/cleanup, not detection accuracy.";
  } catch(const std::exception& exception){report["error"]=QString::fromUtf8(exception.what());report["elapsedMs"]=clock.elapsed();}
  if(outputReady) {
    QSaveFile file(QDir(root).filePath("measurement.json"));
    if(!file.open(QIODevice::WriteOnly)){std::cerr<<"Cannot write measurement\n";return 2;}
    const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);
    if(file.write(bytes)!=bytes.size() || !file.commit()){std::cerr<<"Cannot publish measurement\n";return 2;}
  }
  std::cout<<QJsonDocument(QJsonObject{{"report",QDir(root).filePath("measurement.json")},
      {"pass",report["pass"]},{"elapsedMs",report["elapsedMs"]},{"error",report["error"]}})
      .toJson(QJsonDocument::Compact).constData()<<'\n';
  return report["pass"].toBool()?0:1;
}
