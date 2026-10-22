#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <chrono>
#include "infrastructure/narrowband_analysis/analysis_source.h"
#include "infrastructure/narrowband_analysis/analysis_service.h"

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
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  try {testContinuousSource();testMappingAndFailureBoundaries();testRawConstellationAndLatestRequest();std::cout<<"PASS continuous source, raw IQ constellation, provenance, prefixes, concurrency and cancellation\n";}
  catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
