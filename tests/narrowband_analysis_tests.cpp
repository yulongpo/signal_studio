#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <chrono>
#include "infrastructure/narrowband_analysis/analysis_source.h"
#include "infrastructure/narrowband_analysis/analysis_service.h"
#include "infrastructure/narrowband_analysis/pulse_shaping.h"
#include "infrastructure/narrowband_analysis/symbol_rate_estimator.h"

using namespace signalstudio;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
FileMetadata sourceFixture() {
  FileMetadata source;source.id="source";source.name="tone.raw";
  source.path=(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/tone.raw").toStdString();
  source.sampleRateHz=20e6;source.centerFrequencyHz=100e6;source.sampleCount=400000;source.demo=false;
  return source;
}
Channel channelFixture() {
  Channel c;c.id="channel";c.name="tone";c.centerFrequencyHz=102e6;c.bandwidthHz=1e6;
  c.sourceTime={0,400000};c.outputSampleRateHz=2e6;c.processingState=ChannelProcessingState::Ready;return c;
}
AnalysisRequest requestFor(const FileMetadata& source,const Channel& c,TimeRange range) {
  AnalysisRequest request;request.identity={1,1,c.configVersion,c.id,analysisSourceKey(source,c),range};return request;
}
void testContinuousSource() {
  const auto source=sourceFixture();auto channel=channelFixture();ChannelSampleCache cache;
  auto request=requestFor(source,channel,{0,40000});
  const auto full=readAnalysisSource(cache,source,channel,request);
  check(full.status==AnalysisStatus::Ready && full.outputSamples.begin>0 && full.outputSamples.end<40000 &&
        full.samples.size()==full.outputSamples.end-full.outputSamples.begin,"Analysis must exclude exact FIR padding, not only endpoint samples");
  ChannelDspPlan plan;QString error;check(makeChannelDspPlan(source,channel,plan,error),"Continuous fixture DSP plan");
  const auto valid=channelValidOutputSamples(source,channel,plan);
  check(valid==full.outputSamples,"Read interval must match exact valid convolution support");
  request=requestFor(source,channel,{15000,19000});
  const auto cross=readAnalysisSource(cache,source,channel,request);
  ChannelSampleData direct;
  check(processChannelSamples(source,channel,plan,{15000,19000},direct),"Direct cross-block reference");
  check(cross.status==AnalysisStatus::Ready && direct.samples.size()==cross.samples.size(),"Continuous analysis must cross 16384-sample blocks");
  double maximum=0;
  for(std::size_t i=0;i<cross.samples.size();++i)maximum=std::max(maximum,static_cast<double>(std::abs(cross.samples[i]-direct.samples[i])));
  check(maximum<=1e-6,"Cross-block cached complex samples must match direct DSP at 1e-6");
  AnalysisSourceData parallelA,parallelB;
  std::thread a([&]{parallelA=readAnalysisSource(cache,source,channel,request);});
  std::thread b([&]{parallelB=readAnalysisSource(cache,source,channel,request);});
  a.join();b.join();
  check(parallelA.status==AnalysisStatus::Ready && parallelB.samples==parallelA.samples && cache.stats().bytes<=ChannelSampleCache::defaultBudgetBytes,"Concurrent source readers must preserve IQ and LRU accounting");
  channel.outputSampleRateHz=3e6;++channel.configVersion;
  const auto changed=readAnalysisSource(cache,source,channel,request);
  check(changed.status==AnalysisStatus::Cancelled && changed.samples.empty(),"Old Fs/config requests cannot publish new data");
  request=requestFor(source,channel,{15000,19000});
  const auto newRate=readAnalysisSource(cache,source,channel,request);
  check(newRate.status==AnalysisStatus::Ready && newRate.outputRateHz==3000000,"New Fs must reach the real IQ source");
  std::cout<<"source: direct/cache maximum error="<<maximum<<", valid=["<<valid.begin<<','<<valid.end<<")\n";
}
void testMappingAndFailureBoundaries() {
  auto source=sourceFixture();auto c=channelFixture();ChannelSampleCache cache;
  auto request=requestFor(source,c,{0,40000});
  AnalysisSourceData mapping;mapping.sourceOrigin=(SampleIndex{1}<<53)+17;mapping.sourceRateHz=20000000;mapping.outputRateHz=2000000;
  SamplePosition position;
  check(mapAnalysisPosition(mapping,{5,.25},position) && position.integer==(SampleIndex{1}<<53)+69 && position.fraction==.5,
        "Source indices above 2^53 must retain integer and fractional precision");
  mapping.sourceOrigin=std::numeric_limits<SampleIndex>::max()-10;
  check(!mapAnalysisPosition(mapping,{5,.25},position),"Source mapping overflow must be refused");
  check(readAnalysisSource(cache,source,c,request,[]{return true;}).status==AnalysisStatus::Cancelled,"Cancellation before work");
  int calls=0;
  const auto cancelled=readAnalysisSource(cache,source,c,request,[&]{return ++calls>5;});
  check(cancelled.status==AnalysisStatus::Cancelled && cancelled.samples.empty(),"Mid-plan/read cancellation cannot return usable IQ");
  request.sampleBudget=100;
  check(readAnalysisSource(cache,source,c,request).status==AnalysisStatus::Failed,"Oversized analysis must not silently truncate its interval");
  source.availability={210000,LoadStatus::Partial,5,{}, {}};
  request=requestFor(source,c,{0,40000});
  check(readAnalysisSource(cache,source,c,request).status==AnalysisStatus::Failed,"First unread source sample remains outside analysis");
  c.sourceTime.end=210000;++c.configVersion;
  request=requestFor(source,c,{0,21000});
  const auto partial=readAnalysisSource(cache,source,c,request);
  check(partial.status==AnalysisStatus::Ready && partial.sourceSamples.end<=210000,"A valid Partial channel uses only its loaded prefix");
  const auto identity=partial.identity;
  check(analysisIdentityCurrent(identity,source,c,1,1),"Current identity accepted");
  check(!analysisIdentityCurrent(identity,source,c,2,1) && !analysisIdentityCurrent(identity,source,c,1,2),"Project/request generations must gate publication");
  ++source.availability.generation;
  check(!analysisIdentityCurrent(identity,source,c,1,1),"Loading generation changes invalidate results");
  source.availability.status=LoadStatus::Failed;
  request=requestFor(source,c,{0,1000});
  check(readAnalysisSource(cache,source,c,request).samples.empty(),"Missing/failed data never produces fallback IQ");
}
void testRawConstellationAndLatestRequest() {
  const auto source=sourceFixture();const auto channel=channelFixture();ChannelSampleCache cache;
  auto request=requestFor(source,channel,{1000,19000});
  auto samples=std::make_shared<AnalysisSourceData>(readAnalysisSource(cache,source,channel,request));
  NarrowbandAnalysisSettings settings;settings.pointCount=1024;
  const auto raw=analyzeNarrowband(samples,settings);
  check(raw.status==AnalysisStatus::Ready && raw.constellation.size()==1024 && raw.constellationStage==AnalysisStage::RawIq,
        "Raw constellation must not require symbol rate or synchronization");
  for(std::size_t i=0;i<raw.constellation.size();++i) {
    const auto offset=i*(samples->samples.size()-1)/(raw.constellation.size()-1);
    check(raw.constellation[i].value==samples->samples[offset] && raw.constellation[i].output.integer==1000+offset &&
          raw.constellation[i].source.integer==(1000+offset)*10,"Every plotted complex point must retain its exact IQ value and provenance");
  }
  check(raw.axisMinimum<-.25 && raw.axisMaximum>.25 && raw.axisMaximum<.3,"Full-point scaling must reflect actual amplitude, not fixed +/-1");
  auto shifted=std::make_shared<AnalysisSourceData>(*samples);
  for(auto& sample:shifted->samples)sample+=std::complex<float>(2,3);
  settings.removeDc=true;settings.normalizePower=true;
  const auto normalized=analyzeNarrowband(shifted,settings);
  check(std::abs(normalized.removedDc-std::complex<double>(2,3))<.002 && normalized.gain>3.9 && normalized.gain<4.1,
        "Explicit DC/RMS preprocessing must use all continuous samples");
  NarrowbandAnalysisService service(cache);
  service.submit(source,channel,request,{});
  auto latest=request;latest.identity.requestGeneration=9;latest.identity.outputSamples={2000,4000};
  service.submit(source,channel,latest,{});
  std::shared_ptr<const NarrowbandAnalysisResult> result;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(!result && std::chrono::steady_clock::now()<deadline) {result=service.takeResult();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
  check(result && result->status==AnalysisStatus::Ready && result->identity.requestGeneration==9 && result->source->outputSamples==TimeRange{2000,4000},
        "Replaceable background service must only publish the latest request");
  service.submit(source,channel,request,{});service.cancel();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!service.takeResult(),"Cancelled service work must not publish");
}
void testFractionalEyeTruth() {
  const auto source=sourceFixture();const auto channel=channelFixture();ChannelSampleCache cache;
  auto samples=std::make_shared<AnalysisSourceData>(readAnalysisSource(cache,source,channel,requestFor(source,channel,{1000,19000})));
  NarrowbandAnalysisSettings settings;
  check(analyzeNarrowband(samples,settings).eyeStatus==AnalysisStatus::NeedSymbolRate,"Unknown Rs cannot create an eye");
  settings.symbolRateMode=SymbolRateMode::Manual;settings.symbolRateHz=625000; // 3.2 physical samples/UI
  double error=0,referencePower=0;
  for(int periods=1;periods<=4;++periods) {
    settings.eyePeriods=periods;settings.eyeTraces=32<< (periods-1);
    const auto result=analyzeNarrowband(samples,settings);
    check(result.eyeStatus==AnalysisStatus::Ready && result.eyes.size()==static_cast<std::size_t>(settings.eyeTraces) &&
          result.samplesPerSymbol==3.2 && !result.timingLocked,"Manual fractional-SPS eye is a period hypothesis, never a lock");
    for(const auto& trace:result.eyes) {
      check(trace.i.size()==static_cast<std::size_t>(periods*16+1) && trace.outputPositions.size()==trace.i.size(),"Eye UI grid and provenance must agree");
      for(std::size_t i=0;i<trace.i.size();++i) {
        const auto pos=trace.outputPositions[i];const double n=static_cast<double>(pos.integer)+pos.fraction;
        const auto truth=.25*std::exp(std::complex<double>(0,2*3.14159265358979323846*50000*n/2000000));
        const auto measured=std::complex<double>(trace.i[i],trace.q[i]);error+=std::norm(measured-truth);referencePower+=std::norm(truth);
      }
    }
  }
  const double nrmse=std::sqrt(error/referencePower);
  check(nrmse<=1e-3,"Bandlimited real-IQ interpolation/eye must meet fixed independent analytic NRMSE");
  settings.eyeStage=AnalysisStage::MatchedFiltered;
  const auto filtered=analyzeNarrowband(samples,settings);
  check(filtered.eyeStatus==AnalysisStatus::Ready && filtered.filterDiscardedSamples>0 &&
        filtered.eyes[0].outputStart.integer>samples->outputSamples.begin,"Matched eye must retain group-delay compensation and discard real FIR edges");
  settings.symbolRateHz=3000000;
  check(analyzeNarrowband(samples,settings).eyeStatus==AnalysisStatus::NotApplicable,"Impossible Rs cannot produce an eye");
  std::cout<<"eye: fractional SPS=3.2, analytic NRMSE="<<nrmse<<"\n";
  for(const auto* modulation:{"bpsk","qpsk"}) {
    auto recorded=source;
    recorded.path=(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/"+modulation+"-a35-ideal.raw").toStdString();
    recorded.sampleCount=static_cast<SampleIndex>(QFileInfo(QString::fromStdString(recorded.path)).size()/4);
    auto selected=channel;selected.sourceTime.end=recorded.sampleCount;
    auto iq=std::make_shared<AnalysisSourceData>(readAnalysisSource(cache,recorded,selected,
      requestFor(recorded,selected,{0,(recorded.sampleCount+9)/10})));
    settings.symbolRateHz=625000;settings.eyePeriods=2;settings.eyeTraces=64;
    const auto eye=analyzeNarrowband(iq,settings);
    check(eye.status==AnalysisStatus::Ready && eye.eyeStatus==AnalysisStatus::Ready && eye.eyes.size()==64,
          "Generated real-file BPSK/QPSK must produce manual-Rs matched eyes");
    double difference=0;
    for(const auto& trace:eye.eyes)for(std::size_t p=0;p<trace.i.size();++p)difference+=std::abs(trace.i[p]-trace.q[p]);
    check(difference>1,"I and Q must be independently derived components, not one repeated synthetic trace");
  }
}
std::shared_ptr<AnalysisSourceData> generatedSource(ChannelSampleCache& cache,const std::string& name,double outputRate=2e6,double bandwidth=1e6) {
  auto source=sourceFixture();source.path=(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/"+QString::fromStdString(name)+".raw").toStdString();
  source.sampleCount=static_cast<SampleIndex>(QFileInfo(QString::fromStdString(source.path)).size()/4);
  auto c=channelFixture();c.id=name;c.sourceTime.end=source.sampleCount;c.outputSampleRateHz=outputRate;c.bandwidthHz=bandwidth;
  const auto count=(source.sampleCount*static_cast<std::uint64_t>(outputRate)+19999999)/20000000;
  return std::make_shared<AnalysisSourceData>(readAnalysisSource(cache,source,c,requestFor(source,c,{0,count})));
}
void testBlindSymbolRateCandidates() {
  ChannelSampleCache cache;QJsonArray diagnostics;bool passed=true;
  for(const auto* modulation:{"bpsk","qpsk","16qam"})for(const auto* alpha:{"20","35","50"})for(const auto* noise:{"ideal","awgn20"}) {
    const auto name=std::string(modulation)+"-a"+alpha+"-"+noise;
    const auto source=generatedSource(cache,name);
    check(source->status==AnalysisStatus::Ready,"Actual modulation fixture must supply continuous IQ");
    const auto estimate=estimateSymbolRate(source->samples,source->outputRateHz);
    bool found=false;QJsonArray candidates;
    for(std::size_t i=0;i<estimate.candidates.size();++i) {
      const auto& candidate=estimate.candidates[i];
      if(i<3 && std::abs(candidate.rateHz-625000)/625000<=.01)found=true;
      candidates.push_back(QJsonObject{{"rateHz",candidate.rateHz},{"score",candidate.score},{"prominenceDb",candidate.prominenceDb},{"method",QString::fromStdString(candidate.method)}});
    }
    diagnostics.push_back(QJsonObject{{"fixture",QString::fromStdString(name)},{"foundTop3",found},{"reliable",estimate.reliable},{"observedSamples",static_cast<qint64>(estimate.observedSamples)},{"candidates",candidates}});
    std::cout<<"Rs "<<name<<": "<<(found?"PASS":"FAIL")<<" reliable="<<estimate.reliable;
    for(const auto& candidate:estimate.candidates)std::cout<<" ["<<candidate.rateHz<<','<<candidate.score<<','<<candidate.prominenceDb<<"dB]";
    std::cout<<'\n';passed=passed && found;
  }
  const auto report=qEnvironmentVariable("SS_NARROWBAND_REPORT_DIR");
  check(passed,"Blind top-three Rs candidates must meet fixed 1% truth threshold for all 18 positive files");
  QFile rateIndex(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/rate-index.json");check(rateIndex.open(QIODevice::ReadOnly),"Additional independent rates must be generated through scripts");
  for(const auto& item:QJsonDocument::fromJson(rateIndex.readAll()).object()["records"].toArray()) {
    const auto truth=item.toObject();const auto expected=truth["symbolRateHz"].toDouble();
    const auto source=generatedSource(cache,truth["name"].toString().toStdString(),truth["outputRateHz"].toDouble(),truth["channelBandwidthHz"].toDouble());
    check(source->status==AnalysisStatus::Ready,"Variable-symbol-rate real IQ must load");
    const auto estimate=estimateSymbolRate(source->samples,source->outputRateHz);bool found=false;QJsonArray candidates;
    for(std::size_t i=0;i<estimate.candidates.size();++i){const auto& c=estimate.candidates[i];if(i<3 && std::abs(c.rateHz-expected)/expected<=.01)found=true;candidates.append(QJsonObject{{"rateHz",c.rateHz},{"score",c.score}});}
    diagnostics.append(QJsonObject{{"fixture",truth["name"]},{"truthRateHz",expected},{"foundTop3",found},{"reliable",estimate.reliable},{"candidates",candidates}});
    std::cout<<"Variable Rs "<<expected<<": "<<(found?"PASS":"FAIL")<<'\n';
    check(found && estimate.reliable,"Independent 250k/1M Rs must meet the same fixed top-three 1% threshold");
  }
  if(!report.isEmpty()){QDir().mkpath(report);QFile file(report+"/symbol-rate.json");check(file.open(QIODevice::WriteOnly),"Open rate diagnostic report");file.write(QJsonDocument(diagnostics).toJson());}
  for(const double rate:{1.5e6,4e6,8e6}) {
    const auto source=generatedSource(cache,"qpsk-a35-awgn20",rate);
    const auto estimate=estimateSymbolRate(source->samples,source->outputRateHz);bool found=false;
    for(std::size_t i=0;i<std::min<std::size_t>(3,estimate.candidates.size());++i)if(std::abs(estimate.candidates[i].rateHz-625000)/625000<=.01)found=true;
    check(found,"Variable actual Fs must not be replaced by bandwidth or a default Rs");
  }
  {
    auto source=sourceFixture();auto c=channelFixture();c.bandwidthHz=1234567;c.sampling={ChannelSamplingMode::RecommendedFactor,2};
    const auto plan=planChannelOutputRate(source,c);check(plan.valid && plan.adjusted,"Fractional bandwidth fixture must exercise actual rate adjustment");
    const auto iq=generatedSource(cache,"qpsk-a35-awgn20",plan.realizedRateHz,c.bandwidthHz);
    const auto estimate=estimateSymbolRate(iq->samples,iq->outputRateHz);bool found=false;
    for(std::size_t i=0;i<std::min<std::size_t>(3,estimate.candidates.size());++i)if(std::abs(estimate.candidates[i].rateHz-625000)/625000<.01)found=true;
    check(found,"Automatically adjusted actual Fs must feed blind Rs estimation");
  }
  for(const auto* name:{"noise","short-burst","hop","qpsk-low-snr","short-qpsk"}) {
    const auto source=generatedSource(cache,name);
    const auto estimate=estimateSymbolRate(source->samples,source->outputRateHz);
    check(!estimate.reliable,"Noise/short bursts/hops cannot claim a reliable symbol rate");
  }
  for(const auto pair:{std::pair{"fsk",ModulationHypothesis::Fsk},std::pair{"ofdm",ModulationHypothesis::Ofdm},std::pair{"gmsk",ModulationHypothesis::Gmsk}}) {
    const auto source=generatedSource(cache,pair.first);NarrowbandAnalysisSettings settings;settings.hypothesis=pair.second;settings.symbolRateMode=SymbolRateMode::Estimated;
    const auto result=analyzeNarrowband(source,settings);
    check(result.status==AnalysisStatus::Ready && !result.constellation.empty() && result.eyeStatus==AnalysisStatus::NotApplicable && !result.timingLocked && !result.carrierLocked,
          "Unsupported hypotheses retain actual raw IQ without false eye/synchronization claims");
  }
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  try {testContinuousSource();testMappingAndFailureBoundaries();testRawConstellationAndLatestRequest();testFractionalEyeTruth();testBlindSymbolRateCandidates();std::cout<<"PASS continuous source, real constellation/eye, blind rate candidates and provenance\n";}
  catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
