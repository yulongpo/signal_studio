#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "application/annotation_service.h"
#include "infrastructure/channel_processor.h"
#include "infrastructure/project_store.h"

using namespace signalstudio;
namespace {
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
FileMetadata metadata() {
  FileMetadata f;f.id="source";f.name="source.iq";f.path="source.iq";f.sampleRateHz=20e6;f.centerFrequencyHz=100e6;
  f.sampleCount=400000;f.demo=true;return f;
}
Channel channel(double bandwidth=1e6,double factor=2) {
  Channel c;c.id="test-channel";c.name="rate";c.centerFrequencyHz=100e6;c.bandwidthHz=bandwidth;
  c.sourceTime={100,399000};c.visibleSourceTime=c.sourceTime;
  c.sampling={ChannelSamplingMode::RecommendedFactor,factor};return c;
}
void rates() {
  const auto source=metadata();
  for(const double b:{200e3,1e6,5e6,10e6}) {
    auto c=channel(b);const auto p=planChannelOutputRate(source,c);
    check(p.valid && p.realizedRateHz==2*b && p.realizedFactor==2,"Default must realize exact 2B for supported geometry");
    c.outputSampleRateHz=p.realizedRateHz;
    ChannelDspPlan actual;QString error;
    check(makeChannelDspPlan(source,c,actual,error),"Arithmetic recommendation must construct actual FIR coefficients");
    check(actual.outputRateHz==p.dsp.outputRateHz && actual.interpolation==p.dsp.interpolation &&
          actual.stageCoefficients.size()==p.dsp.stages.size(),"Preview and actual processor must use one specification");
  }
  for(const double factor:{1.5,2.,4.,8.}) {
    const auto p=planChannelOutputRate(source,channel(1e6,factor));
    check(p.valid && p.realizedRateHz==factor*1e6,"All bandwidth presets must affect actual output Fs");
  }
  const auto limited=planChannelOutputRate(source,channel(5e6,8));
  check(limited.valid && limited.adjusted && limited.realizedRateHz<=source.sampleRateHz && !limited.reason.empty(),"Automatic 8x must explain source limitation");
  const auto fractional=planChannelOutputRate(source,channel(1234567));
  check(fractional.valid && fractional.adjusted && std::abs(fractional.realizedRateHz-fractional.targetRateHz)/fractional.targetRateHz<.01,"Fractional bandwidth must select nearby executable rational Fs");
  auto edge=channel(10e6);edge.centerFrequencyHz+=5e6;
  check(!planChannelOutputRate(source,edge).valid,"Source transition-band overflow must fail");
  auto manual=channel();manual.sampling.mode=ChannelSamplingMode::Manual;manual.outputSampleRateHz=1500000;
  check(planChannelOutputRate(source,manual).valid,"Complex IQ below 2B can be legal");
  manual.outputSampleRateHz=1000000;
  check(!planChannelOutputRate(source,manual).valid && manual.outputSampleRateHz==1000000,"Invalid manual Fs must not be rewritten");
  manual.outputSampleRateHz=2469134;
  check(!planChannelOutputRate(source,manual).valid && manual.outputSampleRateHz==2469134,"Unsupported manual rational ratio must be rejected unchanged");
  manual.outputSampleRateHz=2000000.001;
  check(!planChannelOutputRate(source,manual).valid && manual.outputSampleRateHz==2000000.001,"Manual sub-Hz values must not silently become integer DSP rates");
  auto odd=source;odd.sampleRateHz=20000001;
  auto oddChannel=channel();oddChannel.outputSampleRateHz=odd.sampleRateHz;oddChannel.sampling.mode=ChannelSamplingMode::Manual;
  const auto oddPlan=planChannelOutputRate(odd,oddChannel);
  check(oddPlan.valid && oddPlan.dsp.sourceDecimationStages==0 && oddPlan.dsp.interpolation==1 && oddPlan.dsp.decimation==1,"Odd source Fs must not be truncated by dyadic decimation");
}
void realOutput() {
  const auto fixtures=qEnvironmentVariable("SS_NARROWBAND_FIXTURES");
  check(!fixtures.isEmpty(),"Real-IQ fixtures must be generated through the script");
  auto source=metadata();source.demo=false;source.path=(fixtures+"/tone.raw").toStdString();
  auto c=channel();c.centerFrequencyHz=102e6;c.sourceTime={0,400000};
  const auto rate=planChannelOutputRate(source,c);check(rate.valid,"Actual source rate planning");
  c.outputSampleRateHz=rate.realizedRateHz;
  ChannelDspPlan plan;QString error;check(makeChannelDspPlan(source,c,plan,error),"Actual CI16 source FIR construction");
  ChannelSampleData data;
  check(processChannelSamples(source,c,plan,{1024,19000},data),"Actual planned 2MS/s DDC must produce complex IQ");
  std::complex<double> step{};double power=0;
  for(std::size_t i=0;i<data.samples.size();++i){power+=std::norm(data.samples[i]);if(i)step+=std::conj(std::complex<double>(data.samples[i-1]))*std::complex<double>(data.samples[i]);}
  const double frequency=std::arg(step)*plan.outputRateHz/(2*3.14159265358979323846);
  check(plan.outputRateHz==2000000 && std::abs(frequency-50000)<2 && std::abs(std::sqrt(power/data.samples.size())-.25)<.002,
        "Actual output Fs, tone frequency and gain must agree with independent source truth");
}
void persistence() {
  QTemporaryDir dir;const auto path=dir.filePath("project.json");
  Project project;FileState file;file.metadata=metadata();file.view=fullRange(file.metadata);
  AnnotationService service;
  const auto mark=service.createManual(file,"source",{{100,399000},{99.5e6,100.5e6}});
  check(mark.ok,"Manual source fixture must exist");
  auto request=annotationChannelRequest(file,mark.id,file.view.time);
  check(request.outputSampleRateHz==2e6 && request.sampling.mode==ChannelSamplingMode::RecommendedFactor,"New requests must use planned 2x default");
  Channel c;c.id="channel";
  check(service.bindChannelSource(file,c,request).ok,"Rate request must bind source snapshot");
  c.visibleSourceTime=c.sourceTime;c.visibleBasebandFrequency={-1e6,1e6};
  file.channels.push_back(c);project.files.push_back(file);project.activeFileId=file.metadata.id;
  QString error;if(!ProjectStore::save(path,project,error))throw std::runtime_error("New v6 save: "+error.toStdString());
  Project loaded;check(ProjectStore::load(path,loaded,error),"New optional sampling policy must load");
  check(loaded.files[0].channels[0].sampling==c.sampling && loaded.files[0].channels[0].outputSampleRateHz==2e6,"New policy and actual Fs must roundtrip");
  QFile input(path);check(input.open(QIODevice::ReadOnly),"Read project fixture");
  auto root=QJsonDocument::fromJson(input.readAll()).object();input.close();
  auto files=root["files"].toArray();auto f=files[0].toObject();auto channels=f["channels"].toArray();auto old=channels[0].toObject();
  old.remove("samplingPolicy");old["outputSampleRateHz"]=4e6;
  channels[0]=old;f["channels"]=channels;files[0]=f;root["files"]=files;
  check(input.open(QIODevice::WriteOnly|QIODevice::Truncate),"Write old v6 fixture");input.write(QJsonDocument(root).toJson());input.close();
  check(ProjectStore::load(path,loaded,error),"Legacy v6 without optional policy must load");
  const auto& historical=loaded.files[0].channels[0];
  check(historical.outputSampleRateHz==4e6 && historical.sampling.mode==ChannelSamplingMode::Manual &&
        *historical.sourceSnapshot==*c.sourceSnapshot,"Legacy actual Fs and immutable source snapshot must survive");
  check(ProjectStore::save(path,loaded,error) && ProjectStore::load(path,loaded,error) && loaded.files[0].channels[0].outputSampleRateHz==4e6,"Legacy Fs must remain unchanged after save/reopen");
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  try { rates();realOutput();persistence();std::cout<<"PASS actual bandwidth-rate planning, CI16 numerical output, manual refusal, odd source rate and v6 compatibility\n"; }
  catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
