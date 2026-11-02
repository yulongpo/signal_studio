#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <stdexcept>
#include <vector>
#include "infrastructure/detection_sample_energy.h"
#include "infrastructure/detection_sample_stream.h"

using namespace signalstudio;
namespace ss=signalstudio::sample_stream;
namespace gd=signalstudio::gpu_dsp;
namespace {
QString scratchParent;
QJsonArray goldenRows;
gd::Session* requiredGpuSession=nullptr; // borrowed, main-thread suite owner below
const gd::Cancel* currentGpuCancellation=nullptr;
QJsonObject gpuMetrics(const gd::Metrics& m) {
  return {{"setupMs",m.setupMs},{"hostAllocationMs",m.hostAllocationMs},{"planMs",m.planMs},
      {"h2dMs",m.h2dMs},{"fftMs",m.fftMs},{"kernelsMs",m.kernelsMs},{"d2hMs",m.d2hMs},{"totalHostMs",m.totalHostMs},
      {"h2dBytes",QString::number(m.h2dBytes)},{"d2hBytes",QString::number(m.d2hBytes)},
      {"fftExecutions",QString::number(m.fftExecutions)},{"kernelLaunches",QString::number(m.kernelLaunches)},
      {"deviceBytes",QString::number(m.deviceBytes)},{"devicePeakBytes",QString::number(m.devicePeakBytes)},
      {"pinnedBytes",QString::number(m.pinnedBytes)},{"workspaceBytes",QString::number(m.workspaceBytes)},
      {"hostStagingBytes",QString::number(m.hostStagingBytes)},{"planCount",static_cast<int>(m.planCount)},
      {"libraryAlgorithmCalls",QString::number(m.libraryAlgorithmCalls)},
      {"sdkTemporaryBytes",QString::number(m.sdkTemporaryBytes)},{"timingMeaning",QString::fromStdString(m.timingMeaning)},
      {"cubCallsObserved",m.libraryAlgorithmCalls>0}};
}
QJsonObject gpuCapabilities(const gd::Capabilities& c) {
  return {{"driverLoaded",c.driverLoaded},{"cufftLoaded",c.cufftLoaded},{"sessionReady",c.sessionReady},
      {"driverVersion",c.driverVersion},{"cufftVersion",c.cufftVersion},{"deviceOrdinal",c.deviceOrdinal},
      {"computeMajor",c.computeMajor},{"computeMinor",c.computeMinor},{"deviceName",QString::fromStdString(c.deviceName)},
      {"driverLibrary",QString::fromStdString(c.driverLibrary)},{"moduleLibrary",QString::fromStdString(c.moduleLibrary)},
      {"cufftLibrary",QString::fromStdString(c.cufftLibrary)},{"deviceTotalBytes",QString::number(c.deviceTotalBytes)},
      {"deviceFreeBytesAtSetup",QString::number(c.deviceFreeBytesAtSetup)},
      {"fallbackReason",QString::fromStdString(c.fallbackReason)}};
}
void check(bool condition,const char* text) {if(!condition)throw std::runtime_error(text);}
void checked(bool condition,const ss::Failure& f) {
  if(!condition)throw std::runtime_error((f.stage+": "+f.detail+"; cleanup="+f.cleanupFailure).toStdString());
}
ss::Context context(SampleIndex n,double fs=1000,SampleIndex origin=0) {
  ss::Context c;
  c.input.candidateId="sample-stream-native-test";c.input.sampleRateHz=fs;
  c.input.observation={{origin,origin+n},{-fs*.12,fs*.12}};
  c.input.publishBounds=c.input.observation;c.searchSamples={origin,origin+n};c.availableSamples=origin+n;
  c.bandwidthHz=fs*.24;c.options.workingCellBudget=2*1024*1024;
  c.parameters.smoothSeconds=.001;c.parameters.minOnSeconds=.003;c.parameters.minOffSeconds=.005;
  return c;
}
struct Fixture {
  QTemporaryDir directory;
  std::uint64_t consumed=0,leased=0,peak=0;
  ss::Runtime runtime;
  gd::Cancel gpuCancellation;
  Fixture():directory(scratchParent.isEmpty()?QDir::tempPath()+"/ss-sample-XXXXXX":scratchParent+"/ss-sample-XXXXXX") {
    check(directory.isValid(),"Cannot create private test scratch");
    runtime.privateScratchRoot=directory.path();runtime.readerResidentReserveBytes=0;
    runtime.memoryGrantBytes=96ULL<<20;runtime.resultByteGrant=16ULL<<20;
    runtime.sourceReadBlockSamples=257;runtime.work.consumedCells=&consumed;
    runtime.verifySource=[](QString&){return true;};
    runtime.reserveScratch=[this](std::uint64_t bytes,QString&){if(leased)return false;leased=bytes;peak=std::max(peak,leased);return true;};
    runtime.releaseScratch=[this](std::uint64_t bytes){check(leased==bytes,"Scratch lease release mismatch");leased=0;};
  }
  ~Fixture(){if(currentGpuCancellation==&gpuCancellation)currentGpuCancellation=nullptr;}
  void clean() {
    check(leased==0,"Scratch lease not released");
    check(QDir(directory.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot).isEmpty(),"Owned scratch not cleaned");
    if(currentGpuCancellation==&gpuCancellation)currentGpuCancellation=nullptr;
  }
  void iq(const ss::Context& c,const std::vector<std::complex<float>>& source) {
    runtime.gpu=requiredGpuSession;
    gpuCancellation=c.options.cancelled;
    if(runtime.gpu)currentGpuCancellation=&gpuCancellation;
    runtime.read=[this,&c,&source](TimeRange range,std::span<ss::Complex> destination,QString& error) {
      if(range.begin<c.searchSamples.begin || range.end>c.searchSamples.end || range.end-range.begin!=destination.size() ||
         destination.size()>runtime.sourceReadBlockSamples) {error="Source callback range/count violation";return false;}
      std::copy_n(source.begin()+static_cast<std::ptrdiff_t>(range.begin-c.searchSamples.begin),destination.size(),destination.begin());return true;
    };
  }
};
ss::Config config(ss::Mode mode=ss::Mode::StreamingFir,std::uint32_t block=257) {
  ss::Config x;x.mode=mode;x.outputBlockSamples=block;x.scratchBudgetBytes=16ULL<<20;x.scratchFreeFloorBytes=0;return x;
}
std::vector<float> smoothed(const std::vector<float>& power,SampleIndex width) {
  std::vector<float> out(power.size());double running=0;
  for(std::size_t i=0;i<power.size();++i){running+=power[i];if(i>=width)running-=power[i-width];out[i]=static_cast<float>(running/width);}
  return out;
}
void sameEnergy(const std::vector<float>& power,double smoothSeconds,std::uint32_t block,
                SampleIndex origin=0,bool split=true,double coverage=0,bool clip=false) {
  auto c=context(power.size(),1000,origin);c.parameters.smoothSeconds=smoothSeconds;
  c.parameters.splitBursts=split;c.input.windowCoverage=coverage;
  if(clip)c.input.publishBounds={{origin+2,origin+power.size()-1},{-100,100}};
  auto input=c.input;input.energy=DetectionEnergySeries{origin,1,origin+power.size(),7,power,"same-float-energy-v1"};
  const auto reference=refineDetection(input,c.parameters,c.options);
  check(reference.ok,"Same-E Golden invalid");
  Fixture fixture;ss::EnergySource energy{power.size(),7,"same-float-energy-v1",{}};
  energy.read=[&](SampleIndex first,std::span<float> dst,QString&){std::copy_n(power.begin()+static_cast<std::ptrdiff_t>(first-origin),dst.size(),dst.begin());return true;};
  std::vector<float> actualSmooth;
  fixture.runtime.smoothedTap=[&](SampleIndex first,std::span<const float> values,QString&){
    check(first==origin+actualSmooth.size() && values.size()<=block,"Same-E bounded tap order");
    actualSmooth.insert(actualSmooth.end(),values.begin(),values.end());return true;
  };
  ss::Result actual;ss::Failure failure;
  checked(ss::refineEnergyBlocks(c,config(ss::Mode::StreamingFir,block),fixture.runtime,energy,actual,failure),failure);
  check(actual.refinement.status==reference.status && actual.refinement.activity==reference.activity &&
        actual.refinement.fallbackReason==reference.fallbackReason,"Same-E status/segments/ROI mismatch");
  const auto& a=actual.refinement.evidence;const auto& b=reference.evidence;
  check(a.noise==b.noise && a.burst==b.burst && a.threshold==b.threshold && a.comparisonThreshold==b.comparisonThreshold &&
        a.onFraction==b.onFraction && a.minOnSamples==b.minOnSamples && a.minOffSamples==b.minOffSamples &&
        a.smoothingSamples==b.smoothingSamples && a.splittingApplied==b.splittingApplied,"Same-E exact quantile/gate evidence mismatch");
  check(actualSmooth==smoothed(power,std::max<SampleIndex>(1,static_cast<SampleIndex>(smoothSeconds*1000))),"Same-E rolling arithmetic differs");
  check(!actual.publicationEligible,"External EnergySource cannot authorize publication");fixture.clean();
}
void exactPostprocessGroups() {
  for(const auto n:{1U,2U,17U,70U,4099U}) {
    std::vector<float> p(n);
    for(std::size_t j=0;j<p.size();++j)p[j]=(j%23>=3 && j%23<19)?10.f:0.f;
    for(const auto block:{1U,7U,257U}) {
      sameEnergy(p,0,block);sameEnergy(p,.004,block);
      sameEnergy(p,(n+31)/1000.,block); // W>N, fixed divisor
    }
    sameEnergy(p,.001,7,0,false);sameEnergy(p,.001,7,0,true,.7);
  }
  std::vector<float> p(60);std::fill(p.begin()+5,p.begin()+25,10);std::fill(p.begin()+30,p.begin()+55,10);
  sameEnergy(p,0,7,(SampleIndex{1}<<53)+9,true,0,true);
  std::vector<float> dynamic{1e15f,1.f,1.f,1.f};dynamic.resize(73,0);
  sameEnergy(dynamic,.004,3); // same rolling cancellation; do not clamp S
  std::vector<float> subnormal(61,std::numeric_limits<float>::denorm_min());
  sameEnergy(subnormal,.007,5);
}
void failureAndQuotaGroups() {
  auto c=context(1000);std::vector<float> power(1000,1);
  ss::EnergySource source{1000,7,"failure-power",{}};
  source.read=[&](SampleIndex first,std::span<float> dst,QString&){std::copy_n(power.begin()+first,dst.size(),dst.begin());return true;};
  {
    Fixture fixture;auto cfg=config();cfg.scratchBudgetBytes=100;
    ss::Result output;output.autoReason="sentinel";ss::Failure f;
    check(!ss::refineEnergyBlocks(c,cfg,fixture.runtime,source,output,f) && f.code==ss::ErrorCode::ResourceLimit,
          "Scratch quota must reject before writing");check(output.autoReason=="sentinel" && !fixture.peak,"Quota rejection mutated output/leased scratch");fixture.clean();
  }
  {
    Fixture fixture;bool cancel=false;c.options.cancelled=[&]{return cancel;};
    fixture.runtime.energyTap=[&](SampleIndex,std::span<const float>,QString&){cancel=true;return true;};
    ss::Result output;output.autoReason="sentinel";ss::Failure f;
    check(!ss::refineEnergyBlocks(c,config(),fixture.runtime,source,output,f) && f.code==ss::ErrorCode::Cancelled,
          "Mid-stream cancellation must be distinguished");check(output.autoReason=="sentinel","Cancelled output changed");fixture.clean();c.options.cancelled={};
  }
  {
    Fixture fixture;fixture.runtime.energyTap=[](SampleIndex,std::span<const float>,QString& why){why="injected tap I/O failure";return false;};
    ss::Result output;output.autoReason="sentinel";ss::Failure f;
    check(!ss::refineEnergyBlocks(c,config(),fixture.runtime,source,output,f),"Injected observer failure swallowed");
    check(output.autoReason=="sentinel","Failed output changed");fixture.clean();
  }
  {
    Fixture fixture;fixture.runtime.work.limit=1;
    ss::Result output;ss::Failure f;
    check(!ss::refineEnergyBlocks(c,config(),fixture.runtime,source,output,f) && f.code==ss::ErrorCode::WorkLimit,
          "Cumulative work guard relaxed");fixture.clean();
  }
  {
    Fixture fixture;power[3]=std::numeric_limits<float>::quiet_NaN();
    ss::Result output;ss::Failure f;
    check(!ss::refineEnergyBlocks(c,config(),fixture.runtime,source,output,f),"Nonfinite E accepted");fixture.clean();power[3]=1;
  }
  {
    Fixture fixture;int checks=0;fixture.runtime.verifySource=[&](QString& why){why="injected identity change";return ++checks==1;};
    ss::Result output;output.autoReason="sentinel";ss::Failure f;
    check(!ss::refineEnergyBlocks(c,config(),fixture.runtime,source,output,f) && f.code==ss::ErrorCode::SourceChanged,
          "Identity change not rejected atomically");check(output.autoReason=="sentinel","Identity failure output changed");fixture.clean();
  }
  {
    Fixture fixture;auto large=context(1200000000,20000000);ss::Plan plan;ss::Failure f;
    checked(ss::makePlan(large,config(ss::Mode::LegacyExact),fixture.runtime,plan,f),f);
    check(!plan.legacyEligible,"60s/20MSps Legacy unexpectedly fits finite grant");
    auto cfg=config();cfg.scratchBudgetBytes=16ULL<<30;
    checked(ss::makePlan(large,cfg,fixture.runtime,plan,f),f);
    check(plan.scratchReservationBytes==9600008192ULL && plan.estimatedResidentBytes<=fixture.runtime.memoryGrantBytes,
          "Long Sample plan is not bounded independently of N");fixture.clean();
  }
}
bool segmentParity(const DetectionRefinementResult& a,const DetectionRefinementResult& b,SampleIndex tolerance) {
  if(a.status!=b.status || a.activity.size()!=b.activity.size())return false;
  const auto diff=[](SampleIndex x,SampleIndex y){return x>y?x-y:y-x;};
  for(std::size_t i=0;i<a.activity.size();++i)
    if(diff(a.activity[i].range.time.begin,b.activity[i].range.time.begin)>tolerance ||
       diff(a.activity[i].range.time.end,b.activity[i].range.time.end)>tolerance)return false;
  return true;
}
void nativeGolden(std::string name,std::vector<ss::Complex> source,ss::Context c,bool requireUsableFir=false) {
  const auto rowIndex=goldenRows.size();
  goldenRows.append(QJsonObject{{"case",QString::fromStdString(name)},{"measurementComplete",false}});
  constexpr std::uint64_t budget=96ULL<<20;
  QString error;std::vector<float> power;std::vector<ss::Complex> filtered;
  const auto observe=[&](std::uint64_t first,std::span<const ss::Complex> values,QString&){
    check(first==filtered.size(),"Legacy observer order");filtered.insert(filtered.end(),values.begin(),values.end());return true;};
  check(detectionBandEnergy(source,c.input.sampleRateHz,c.relativeCenterHz,c.bandwidthHz,power,budget,error,{},observe),
        qPrintable(error));
  const auto width=std::max<SampleIndex>(1,static_cast<SampleIndex>(c.parameters.smoothSeconds*c.input.sampleRateHz));
  const auto referenceSmooth=smoothed(power,width);
  auto input=c.input;input.energy=DetectionEnergySeries{c.searchSamples.begin,1,c.searchSamples.end,source.size(),power,
                                                     "cpu-exact-length-complex64-raised-cosine-fft-v1"};
  const auto golden=refineDetection(input,c.parameters,c.options);check(golden.ok,"Native Golden invalid");
  const auto gpuBefore=requiredGpuSession?gpuMetrics(requiredGpuSession->metrics()):QJsonObject{};
  Fixture fixture;fixture.iq(c,source);std::vector<float> energy,smooth;std::vector<ss::Complex> iq;
  const auto cfg=config(ss::Mode::StreamingFir,4096);
  fixture.runtime.energyTap=[&](SampleIndex,std::span<const float> v,QString&){check(v.size()<=cfg.outputBlockSamples,"Unbounded E tap");energy.insert(energy.end(),v.begin(),v.end());return true;};
  fixture.runtime.smoothedTap=[&](SampleIndex,std::span<const float> v,QString&){smooth.insert(smooth.end(),v.begin(),v.end());return true;};
  fixture.runtime.filteredIqTap=[&](SampleIndex,std::span<const ss::Complex> v,QString&){iq.insert(iq.end(),v.begin(),v.end());return true;};
  ss::Result stream;ss::Failure failure;checked(ss::run(c,cfg,fixture.runtime,stream,failure),failure);fixture.clean();
  check(iq.size()==source.size() && energy.size()==power.size() && smooth.size()==power.size(),"Native stream length differs");
  double amplitude=0,maxComplex=0,maxPower=0,maxSmooth=0,err2=0,golden2=0;
  for(std::size_t i=0;i<source.size();++i) {
    amplitude=std::max(amplitude,static_cast<double>(std::abs(source[i])));
    const double delta=std::abs(iq[i]-filtered[i]);maxComplex=std::max(maxComplex,delta);err2+=delta*delta;golden2+=std::norm(filtered[i]);
    maxPower=std::max(maxPower,std::abs(static_cast<double>(energy[i])-power[i]));
    maxSmooth=std::max(maxSmooth,std::abs(static_cast<double>(smooth[i])-referenceSmooth[i]));
  }
  amplitude=std::max(amplitude,1e-30);const double scale=amplitude*amplitude;
  const double rms=std::sqrt(err2/std::max(golden2,1e-60));
  const SampleIndex tol=std::max<SampleIndex>(2,static_cast<SampleIndex>(std::floor(c.input.sampleRateHz*std::min(.00005,.05*c.parameters.smoothSeconds))));
  bool thresholdParity=golden.evidence.threshold.has_value()==stream.refinement.evidence.threshold.has_value();
  if(golden.evidence.threshold && stream.refinement.evidence.threshold)
    thresholdParity=std::abs(*golden.evidence.threshold-*stream.refinement.evidence.threshold)<=5e-4*scale;
  const bool parity=segmentParity(stream.refinement,golden,tol);
  const bool rawPass=maxComplex/amplitude<=2e-4 && rms<=1e-3 && maxPower/scale<=5e-4 && maxSmooth/scale<=5e-4 && thresholdParity && parity;
  QJsonObject row{{"case",QString::fromStdString(name)},{"samples",QString::number(source.size())},
      {"measurementComplete",true},{"legacySegments",static_cast<int>(golden.activity.size())},
      {"firSegments",static_cast<int>(stream.refinement.activity.size())},{"rawGoldenPass",rawPass},
      {"rawSegmentParity",parity},{"complexMaxNormalized",maxComplex/amplitude},{"complexRmsRelative",rms},
      {"powerMaxNormalized",maxPower/scale},{"smoothMaxNormalized",maxSmooth/scale},
      {"publicationEligible",stream.publicationEligible},{"nativeEnvelopeEstablished",stream.nativeEnvelopeEstablished},
      {"precisionPolicy",stream.precisionPolicy},{"precisionReason",stream.precisionReason},
      {"scratchPeakBytes",QString::number(stream.statistics.scratchPeakBytes)},
      {"sourceSamplesRead",QString::number(stream.statistics.sourceSamplesRead)}};
  goldenRows[rowIndex]=row;
  if(requiredGpuSession) {
    row["gpuFirBlocks"]=QString::number(stream.statistics.gpuFirBlocks);
    row["gpuFirMethod"]=QString::fromStdString(stream.refinement.evidence.energyMethod);
    row["gpuMetricsBefore"]=gpuBefore;row["gpuMetricsAfterFir"]=gpuMetrics(requiredGpuSession->metrics());
    goldenRows[rowIndex]=row;
    check(stream.statistics.gpuFirBlocks>0,"Explicit GPU Golden did not execute GPU FIR blocks");
  }
  if(stream.publicationEligible)check(rawPass,"Empirically eligible FIR failed UNCHANGED native Golden tolerance");
  if(requireUsableFir)check(stream.publicationEligible,"Normal strong native fixture has no usable bounded FIR path");
  // Always preserve actual raw Golden failures in the report; do not turn them
  // into raw PASS merely because Auto recovered by executing Legacy.
  Fixture autoFixture;autoFixture.iq(c,source);ss::Result automatic;
  auto autoConfig=cfg;autoConfig.mode=ss::Mode::Auto;
  checked(ss::run(c,autoConfig,autoFixture.runtime,automatic,failure),failure);autoFixture.clean();
  const bool autoParity=segmentParity(automatic.refinement,golden,tol);
  row["autoGoldenParity"]=autoParity;row["autoUsedLegacy"]=automatic.route==ss::Route::LegacyExact;goldenRows[rowIndex]=row;
  if(requiredGpuSession) {
    row["gpuAutoFirBlocks"]=QString::number(automatic.statistics.gpuFirBlocks);
    row["gpuAutoPrecisionFallback"]=automatic.route==ss::Route::LegacyExact&&automatic.statistics.diagnosticFirPlan.has_value();
    row["gpuAutoAdoptedMethod"]=QString::fromStdString(automatic.refinement.evidence.energyMethod);
    goldenRows[rowIndex]=row;
  }
  check(autoParity,"Auto result failed native Golden parity within eligible Legacy budget");
  if(!stream.precisionGuardPassed) {
    check(automatic.route==ss::Route::LegacyExact,"Fragile fixture did not execute complete Legacy");
    check(automatic.refinement.activity==golden.activity && automatic.refinement.evidence.noise==golden.evidence.noise &&
          automatic.refinement.evidence.burst==golden.evidence.burst && automatic.refinement.evidence.threshold==golden.evidence.threshold,
          "Precision fallback mixed FIR evidence into Legacy");
    Fixture pressure;pressure.iq(c,source);
    // Exact complete-vector cell guard deliberately disallows this Legacy
    // context; bounded block processing remains valid with the SAME guard.
    auto blocked=c;blocked.options.workingCellBudget=1024;ss::Result unknown;
    checked(ss::run(blocked,autoConfig,pressure.runtime,unknown,failure),failure);pressure.clean();
    if(requiredGpuSession) {
      row["gpuPressureFirBlocks"]=QString::number(unknown.statistics.gpuFirBlocks);
      row["gpuPressureOutcome"]="UnknownNeedsMeasuredCoarse";goldenRows[rowIndex]=row;
    }
    check(unknown.route==ss::Route::UnknownNeedsMeasuredCoarse && unknown.needsMeasuredCoarse &&
          unknown.refinement.status==RefineStatus::Fallback && unknown.refinement.activity.empty(),
          "Ineligible complete Legacy must request measured Coarse/Unknown, never fake Applied");
  }
  // Different block length must preserve the original numerical tolerance.
  Fixture other;other.iq(c,source);std::vector<ss::Complex> otherIq;
  other.runtime.filteredIqTap=[&](SampleIndex,std::span<const ss::Complex> v,QString&){otherIq.insert(otherIq.end(),v.begin(),v.end());return true;};
  ss::Result otherResult;auto otherConfig=cfg;otherConfig.outputBlockSamples=7001;
  checked(ss::run(c,otherConfig,other.runtime,otherResult,failure),failure);other.clean();
  double blockError=0;for(std::size_t i=0;i<iq.size();++i)blockError=std::max(blockError,static_cast<double>(std::abs(iq[i]-otherIq[i]))/amplitude);
  const bool blockPass=blockError<=2e-6 && segmentParity(otherResult.refinement,stream.refinement,0);
  row["blockGoldenPass"]=blockPass;row["blockComplexMaxNormalized"]=blockError;goldenRows[rowIndex]=row;
  if(requiredGpuSession) {
    row["gpuOtherBlockFirBlocks"]=QString::number(otherResult.statistics.gpuFirBlocks);
    row["gpuCumulativeMetricsAfterCase"]=gpuMetrics(requiredGpuSession->metrics());goldenRows[rowIndex]=row;
    check(otherResult.statistics.gpuFirBlocks>0,"GPU block-invariance case silently used CPU FIR");
  }
  if(stream.publicationEligible && otherResult.publicationEligible)check(blockPass,"Eligible FIR block-length invariance failed");
}
void goldenGroups() {
  std::mt19937 rng(260326);std::normal_distribution<float> normal;
  const auto signal=[&](std::size_t n,float noise) {
    std::vector<ss::Complex> x(n);for(std::size_t i=0;i<n;++i){
      const bool on=i>=n/10 && i<n*9/10;
      const double angle=2*std::numbers::pi*.03*i;
      x[i]=static_cast<float>(on)*ss::Complex(std::cos(angle),std::sin(angle))+noise*ss::Complex(normal(rng),normal(rng));}
    return x;
  };
  nativeGolden("normal-strong",signal(4099,.02f),context(4099,65536),true);
  nativeGolden("low-snr-input-minus5dB",signal(16384,1.2574f),context(16384,65536));
  auto narrow=context(4099,65536);narrow.bandwidthHz=65536*.02;narrow.relativeCenterHz=65536*.03;
  narrow.input.observation.frequency={65536*.02,65536*.04};narrow.input.publishBounds=narrow.input.observation;
  nativeGolden("narrow-offset",signal(4099,.02f),narrow);
  auto veryNarrow=narrow;veryNarrow.bandwidthHz=65536*.002;
  veryNarrow.input.observation.frequency={65536*.029,65536*.031};veryNarrow.input.publishBounds=veryNarrow.input.observation;
  nativeGolden("very-narrow-offset",signal(4099,.02f),veryNarrow);
  auto nyquist=context(4099,65536);nyquist.bandwidthHz=65536*.9;
  nyquist.input.observation.frequency={-65536*.45,65536*.45};nyquist.input.publishBounds=nyquist.input.observation;
  nativeGolden("nyquist-clipped-raised-cosine",signal(4099,.02f),nyquist);
  auto full=context(4099,65536,(SampleIndex{1}<<53)+9);full.bandwidthHz=65536;
  full.input.observation.frequency={-32768,32768};full.input.publishBounds=full.input.observation;
  nativeGolden("fullband-large-absolute-origin",signal(4099,.02f),full);
  std::vector<ss::Complex> impulse(65537);impulse[0]=1;impulse[4095]={0,1};impulse[4096]=-1;impulse.back()=2;
  auto edge=context(impulse.size(),65536);edge.relativeCenterHz=.1*65536;
  nativeGolden("pure-impulse-Golden-must-remain",impulse,edge);
  std::vector<ss::Complex> sparse(131071);
  for(std::size_t j=0;j<sparse.size();++j)if((j>=4090&&j<4160)||(j>=111000&&j<111070))
    sparse[j]=std::polar(1.f,static_cast<float>(2*std::numbers::pi*.03*j));
  auto quiet=context(sparse.size(),65536);quiet.parameters.minOnSeconds=.0005;
  nativeGolden("sparse-zero-noise-Golden-must-remain",std::move(sparse),quiet);
  nativeGolden("zero",std::vector<ss::Complex>(4099),context(4099,65536));
}
void longGroups() {
  for(const double seconds:{.1,1.,10.,60.}) {
    const auto count=static_cast<std::size_t>(std::llround(seconds*8192));
    std::vector<ss::Complex> x(count);
    for(std::size_t j=0;j<count;++j)x[j]=std::polar(1.f,static_cast<float>(2*std::numbers::pi*.03*j));
    auto c=context(count,8192);c.input.windowCoverage=.8;
    nativeGolden("continuous-reduced-Fs-"+std::to_string(seconds),std::move(x),c);
  }
}
void runBudgetGroup() {
  std::vector<float> p(300003);for(std::size_t j=0;j<p.size();++j)p[j]=j%3<2?10.f:0.f;
  auto c=context(p.size());c.parameters.smoothSeconds=0;c.parameters.minOnSeconds=.002;c.parameters.minOffSeconds=0;
  ss::EnergySource energy{p.size(),7,"pre-merge-budget",{}};
  energy.read=[&](SampleIndex first,std::span<float> dst,QString&){std::copy_n(p.begin()+first,dst.size(),dst.begin());return true;};
  Fixture fixture;ss::Result output;ss::Failure f;
  check(!ss::refineEnergyBlocks(c,config(),fixture.runtime,energy,output,f) && f.detail=="Activity-run budget exceeded",
        "100001 raw runs must fail before being merged to one");fixture.clean();
  c.options.sampleMinimumOnFraction=.9;
  checked(ss::refineEnergyBlocks(c,config(),fixture.runtime,energy,output,f),f);fixture.clean();
  check(output.refinement.fallbackReason=="sample-on-fraction-too-low","ON-fraction gate must precede raw-run budget");
}
} // namespace
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);const auto args=app.arguments();QString report;
  const bool gpuRequired=args.contains("--gpu");
  gd::Capabilities capabilities;
  std::unique_ptr<gd::Session> gpu;
  const auto clearBorrow=qScopeGuard([]{requiredGpuSession=nullptr;currentGpuCancellation=nullptr;});
  QJsonObject requestedGpuPaths;
  const auto save=[&](bool success,const QString& error) {
    if(report.isEmpty())return true;
    QFile file(report);if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;
    QJsonObject result{{"schema","sample-stream-native-golden-v1"},{"cases",goldenRows},
        {"directedTestSuccess",success},{"error",error},{"nativeEnvelopeEstablished",false},
        {"scope","Synthetic native CPU Golden; raw failures retained; no RF/GPU/performance claim"}};
    if(gpuRequired) {
      result["gpuRequired"]=true;
      result["gpu"]=QJsonObject{{"requestedPaths",requestedGpuPaths},{"capabilities",gpuCapabilities(capabilities)},
          {"cumulativeMetrics",gpu?gpuMetrics(gpu->metrics()):QJsonObject{}},
          {"cancellationScope","Fixture-owned callback borrowed only during synchronous run; same main thread"},
          {"cubScope","CUB counters are observational; zero calls do not establish CUB validation"}};
      result["scope"]="Synthetic CPU LegacyExact Golden versus explicit CUDA FIR; original tolerances/raw failures preserved; precision Legacy fallback is recorded, not GPU parity by itself";
    }
    const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Indented);return file.write(bytes)==bytes.size();
  };
  try {
    const auto at=args.indexOf("--scratch-root");if(at>=0){check(at+1<args.size(),"Missing scratch root");scratchParent=args[at+1];check(QDir(scratchParent).exists(),"Scratch parent must exist");}
    const auto out=args.indexOf("--golden-report");if(out>=0){check(out+1<args.size(),"Missing report path");report=args[out+1];}
    if(gpuRequired) {
      const QDir deployed(QCoreApplication::applicationDirPath());
      const auto module=deployed.absoluteFilePath("SignalStudioGpuDspCuda.dll");
      const auto cufft=deployed.absoluteFilePath("cufft64_11.dll");
      const auto jit=deployed.absoluteFilePath("nvJitLink_120_0.dll");
      requestedGpuPaths={{"moduleLibrary",module},{"cufftLibrary",cufft},
          {"nvJitLinkLibrary",QFileInfo(jit).isFile()?jit:QString{}}};
      check(QFileInfo(module).isAbsolute()&&QFileInfo(module).isFile(),"Required deployed SignalStudioGpuDspCuda.dll missing");
      check(QFileInfo(cufft).isAbsolute()&&QFileInfo(cufft).isFile(),"Required deployed cufft64_11.dll missing");
      gd::Config cfg;cfg.moduleLibrary=module.toStdString();cfg.cufftLibrary=cufft.toStdString();
      if(QFileInfo(jit).isFile())cfg.nvJitLinkLibrary=jit.toStdString();
      cfg.cancelled=[] {return currentGpuCancellation&&*currentGpuCancellation&&(*currentGpuCancellation)();};
      gd::Error error;gpu=gd::Session::create(cfg,capabilities,error);
      if(!gpu)throw std::runtime_error("Required GPU DSP initialization failed: "+error.stage+": "+error.detail);
      check(capabilities.sessionReady&&capabilities.cufftLoaded&&capabilities.driverLoaded&&
          capabilities.deviceOrdinal>=0&&!capabilities.deviceName.empty(),"GPU DSP did not expose a real ready device/cuFFT session");
      requiredGpuSession=gpu.get();
    }
    exactPostprocessGroups();failureAndQuotaGroups();
    if(args.contains("--golden")||!report.isEmpty()||gpuRequired)goldenGroups();
    if(args.contains("--long")||gpuRequired)longGroups();
    if(args.contains("--run-budget"))runBudgetGroup();
    if(gpuRequired) {
      gd::Error error;
      if(!gpu->drain(error))throw std::runtime_error("Required GPU drain failed: "+error.stage+": "+error.detail);
      const auto& measured=gpu->metrics();
      check(goldenRows.size()==13,"Explicit GPU validation must retain all thirteen original NativeGolden cases");
      check(measured.fftExecutions>0&&measured.kernelLaunches>0&&measured.h2dBytes>0&&measured.d2hBytes>0,
          "Explicit GPU validation produced no actual cuFFT/kernel/transfer evidence");
    }
    check(save(true,{}),"Cannot write native Golden report");
    std::cout<<"Sample stream directed checks passed; native Golden cases="<<goldenRows.size()<<" (inspect rawGoldenPass; Auto recovery is separate)\n";
    return 0;
  } catch(const std::exception& e) {save(false,QString::fromUtf8(e.what()));std::cerr<<e.what()<<'\n';return 1;}
}
