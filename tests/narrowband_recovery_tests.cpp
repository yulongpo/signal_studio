#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include "infrastructure/narrowband_analysis/constellation_analysis.h"
#include "infrastructure/narrowband_analysis/eye_analysis.h"

using namespace signalstudio;
namespace {
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
std::shared_ptr<AnalysisSourceData> load(ChannelSampleCache& cache,const QString& name,double fs=2e6) {
  FileMetadata source;source.id=name.toStdString();source.name=(name+".raw").toStdString();
  source.path=(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/"+name+".raw").toStdString();source.demo=false;
  source.sampleRateHz=20e6;source.centerFrequencyHz=100e6;source.sampleCount=static_cast<SampleIndex>(QFileInfo(QString::fromStdString(source.path)).size()/4);
  Channel c;c.id=name.toStdString();c.name="recovery";c.centerFrequencyHz=102e6;c.bandwidthHz=1e6;c.outputSampleRateHz=fs;c.sourceTime={0,source.sampleCount};
  AnalysisRequest request;request.identity={1,1,1,c.id,analysisSourceKey(source,c),{0,(source.sampleCount*static_cast<SampleIndex>(fs)+19999999)/20000000}};
  return std::make_shared<AnalysisSourceData>(readAnalysisSource(cache,source,c,request));
}
std::vector<std::complex<double>> symbols(const QString& name) {
  QFile file(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/"+name+".symbols");check(file.open(QIODevice::ReadOnly),"Read independent TX symbols");
  const auto bytes=file.readAll();std::vector<std::complex<double>> output;
  for(qsizetype i=0;i+8<=bytes.size();i+=8) {
    const auto re=std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData()+i));
    const auto im=std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData()+i+4));output.emplace_back(re,im);
  }
  return output;
}
QJsonObject validate(const QString& name,ModulationHypothesis hypothesis,double fs=2e6,bool automaticRate=false) {
  ChannelSampleCache cache;const auto source=load(cache,name,fs);check(source->status==AnalysisStatus::Ready,"Actual recovery IQ source: "+source->message);
  NarrowbandAnalysisSettings settings;settings.hypothesis=hypothesis;settings.symbolRateMode=automaticRate?SymbolRateMode::Estimated:SymbolRateMode::Manual;
  QFile truthFile(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/"+name+".truth.json");check(truthFile.open(QIODevice::ReadOnly),"Read transmit truth");
  const auto parameters=QJsonDocument::fromJson(truthFile.readAll()).object();settings.rolloff=parameters["alpha"].toDouble(.35);
  settings.symbolRateHz=automaticRate?0:625000;settings.cfoMode=CfoMode::Automatic;settings.matchedFilter=true;settings.timingRecovery=true;settings.carrierRecovery=true;
  settings.constellationStage=AnalysisStage::CarrierRecovered;
  settings.eyeStage=AnalysisStage::CarrierRecovered;
  const auto result=analyzeNarrowband(source,settings);
  const auto truth=symbols(name);const double origin=parameters["symbolOriginSourceSample"].toDouble(),period=parameters["sourceSamplesPerSymbol"].toDouble(32);
  const bool noisy=name.contains("awgn20");const double expectedCfo=parameters["cfoHz"].toDouble();
  double timingError=0,referencePower=0;std::vector<std::complex<double>> observed,reference;
  for(const auto& symbol:result.synchronizedSymbols) {
    const double sourcePosition=static_cast<double>(symbol.source.integer)+symbol.source.fraction;
    const auto index=static_cast<std::int64_t>(std::llround((sourcePosition-origin)/period));
    if(index<0 || index>=static_cast<std::int64_t>(truth.size()))continue;
    const double error=(sourcePosition-origin-index*period)/period;timingError+=error*error;
    observed.emplace_back(symbol.value);reference.push_back(truth[static_cast<std::size_t>(index)]);referencePower+=std::norm(reference.back());
  }
  double evm=1e6;
  const int ambiguities=hypothesis==ModulationHypothesis::Bpsk?2:4;
  for(int rotation=0;rotation<ambiguities;++rotation) {
    const auto phase=std::polar(1.0,2*std::numbers::pi*rotation/ambiguities);double numerator=0,denominator=0;
    for(std::size_t i=0;i<observed.size();++i){numerator+=std::real(reference[i]*std::conj(observed[i]*phase));denominator+=std::norm(observed[i]);}
    const double gain=numerator/std::max(1e-30,denominator);if(gain<=0)continue;
    double error=0;for(std::size_t i=0;i<observed.size();++i)error+=std::norm(observed[i]*phase*gain-reference[i]);
    evm=std::min(evm,std::sqrt(error/std::max(1e-30,referencePower)));
  }
  const double timing=std::sqrt(timingError/std::max<std::size_t>(1,observed.size()));
  const double cfoError=std::abs(result.estimatedCfoHz-expectedCfo)/625000;
  const bool eye=result.eyeStatus==AnalysisStatus::Ready && !result.eyes.empty() &&
      std::abs(std::complex<float>(result.eyes[0].i[0],result.eyes[0].q[0])-result.synchronizedSymbols[0].value)<=1e-3;
  const bool passed=result.timingLocked && result.carrierLocked && eye && observed.size()>4096 && cfoError<=.001 && timing<=(noisy?.10:.05) && evm<=(noisy?.18:.05);
  std::cout<<name.toStdString()<<" Fs="<<fs<<" timingLock="<<result.timingLocked<<" carrierLock="<<result.carrierLocked<<" N="<<observed.size()
    <<" CFO="<<result.estimatedCfoHz<<" CFO/Rs error="<<cfoError<<" timing/UI="<<timing<<" EVM="<<evm<<" TED jitter="<<result.timingErrorUi
    <<" conditional="<<result.conditionalEvm<<" : "<<result.syncMessage<<'\n';
  return {{"fixture",name},{"sourceSampleSnrDb",parameters["snrDb"]},{"outputRateHz",fs},{"automaticRate",automaticRate},{"alignedEye",eye},
    {"timingLocked",result.timingLocked},{"carrierLocked",result.carrierLocked},{"symbols",static_cast<qint64>(observed.size())},
    {"estimatedCfoHz",result.estimatedCfoHz},{"cfoErrorOverRs",cfoError},{"timingRmsUi",timing},{"truthEvm",evm},
    {"conditionalEvm",result.conditionalEvm},{"message",QString::fromStdString(result.syncMessage)},{"pass",passed}};
}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  try {
    QJsonArray diagnostics;bool passed=true;
    for(const auto entry:{std::pair{"bpsk",ModulationHypothesis::Bpsk},std::pair{"qpsk",ModulationHypothesis::Qpsk},std::pair{"16qam",ModulationHypothesis::Qam16}})
      for(const auto* condition:{"ideal","awgn20"}) {const auto report=validate(QString(entry.first)+"-a35-"+condition,entry.second);passed=passed && report["pass"].toBool();diagnostics.push_back(report);}
    for(const auto fs:{1.5e6,4e6}) {const auto report=validate("qpsk-a35-awgn20",ModulationHypothesis::Qpsk,fs,true);passed=passed && report["pass"].toBool();diagnostics.push_back(report);}
    for(const auto* alpha:{"20","50"})for(const auto entry:{std::pair{"qpsk",ModulationHypothesis::Qpsk},std::pair{"16qam",ModulationHypothesis::Qam16}}) {
      const auto report=validate(QString(entry.first)+"-a"+alpha+"-ideal",entry.second);passed=passed && report["pass"].toBool();diagnostics.push_back(report);
    }
    {const auto report=validate("qpsk-clock100ppm",ModulationHypothesis::Qpsk);passed=passed && report["pass"].toBool();diagnostics.push_back(report);}
    const auto root=qEnvironmentVariable("SS_NARROWBAND_REPORT_DIR");if(!root.isEmpty()){QDir().mkpath(root);QFile report(root+"/recovery.json");check(report.open(QIODevice::WriteOnly),"Recovery diagnostic report");report.write(QJsonDocument(diagnostics).toJson());}
    check(passed,"Recovery must meet unchanged CFO, timing and true-symbol EVM thresholds");
    ChannelSampleCache cache;
    {
      const auto source=load(cache,"qpsk-a35-awgn20");NarrowbandAnalysisSettings s;
      s.hypothesis=ModulationHypothesis::Qpsk;s.symbolRateMode=SymbolRateMode::Manual;s.symbolRateHz=625000;
      s.cfoMode=CfoMode::Automatic;s.matchedFilter=true;s.timingRecovery=true;s.carrierRecovery=true;
      s.constellationStage=AnalysisStage::CarrierRecovered;s.eyeStage=AnalysisStage::CarrierRecovered;
      const auto locked=analyzeNarrowband(source,s);check(locked.carrierLocked && !locked.eyes.empty(),"Transition regression must begin from actual lock");
      s.symbolRateMode=SymbolRateMode::Unknown;
      const auto unknown=analyzeNarrowband(source,s);
      check(!unknown.timingLocked && !unknown.carrierLocked && unknown.clockSymbols.empty() && unknown.synchronizedSymbols.empty() &&
            unknown.constellationStage==AnalysisStage::RawIq && !unknown.constellation.empty() && unknown.eyeStatus==AnalysisStatus::NeedSymbolRate,
            "Manual to Unknown must not use the retained positive Rs or report stale lock");
      s.symbolRateMode=SymbolRateMode::Estimated;
      auto unavailable=s;unavailable.cfoMode=CfoMode::Off;
      const auto unreliable=analyzeNarrowband(load(cache,"noise"),unavailable);
      check(!unreliable.timingLocked && !unreliable.carrierLocked && unreliable.synchronizedSymbols.empty() &&
            unreliable.stages.empty() && unreliable.filterDiscardedSamples==0 && unreliable.clockSymbols.empty(),
            "Unreliable automatic Rs must not reuse a retained manual number even with CFO bypassed");
      auto manualCfo=s;manualCfo.symbolRateMode=SymbolRateMode::Unknown;manualCfo.cfoMode=CfoMode::Manual;manualCfo.manualCfoHz=15625;
      manualCfo.matchedFilter=false;manualCfo.timingRecovery=false;manualCfo.carrierRecovery=false;
      manualCfo.constellationStage=AnalysisStage::CfoCorrected;manualCfo.eyeStage=AnalysisStage::RawIq;
      const auto cfoOnly=analyzeNarrowband(source,manualCfo);
      const auto expectedFirst=std::complex<double>(source->samples.front())*std::polar(1.,-2*std::numbers::pi*15625*static_cast<double>(source->outputSamples.begin)/source->outputRateHz);
      check(cfoOnly.constellationStage==AnalysisStage::CfoCorrected && cfoOnly.stages.size()==1 && cfoOnly.filterDiscardedSamples==0 &&
            !cfoOnly.timingLocked && !cfoOnly.carrierLocked && cfoOnly.eyeStatus==AnalysisStatus::NeedSymbolRate &&
            std::abs(std::complex<double>(cfoOnly.constellation.front().value)-expectedFirst)<1e-6,
            "Unknown Rs must still allow independent manual CFO with exact NCO output");
      s.symbolRateMode=SymbolRateMode::Manual;
      const auto position=[](const SamplePosition& p){return static_cast<double>(p.integer)+p.fraction;};
      const double period=position(locked.clockSymbols[1].output)-position(locked.clockSymbols[0].output);
      for(const auto stage:{AnalysisStage::TimingAligned,AnalysisStage::CarrierRecovered})for(const double offset:{-2.,-.25,.25,2.}) {
        auto shifted=locked;s.eyeStage=stage;s.eyeTimingOffsetUi=offset;buildEye(shifted,s);
        check(shifted.eyeStatus==AnalysisStatus::Ready && !shifted.eyes.empty(),"Recovered eye offsets must keep interpolation within real support");
        const double firstUi=std::ceil(std::max(0.,-offset))+offset;
        const auto index=static_cast<std::size_t>(std::floor(firstUi));const double phase=firstUi-index;
        const double expected=position(locked.clockSymbols[index].output)+(phase>0?phase*(position(locked.clockSymbols[index+1].output)-position(locked.clockSymbols[index].output)):0);
        check(std::abs(position(shifted.eyes[0].outputPositions[0])-expected)<1e-7,"Signed eye offset must affect recovered fractional clock positions");
        if(offset==.25)check(std::abs(position(shifted.eyes[0].outputStart)-position(locked.eyes[0].outputStart)-period*.25)<1e-7 &&
          std::abs(shifted.eyes[0].i[0]-locked.eyes[0].i[0])>1e-5,"Quarter-UI offset must change both source position and real interpolated data");
      }
      std::cout<<"PASS stale-Rs rejection, independent manual CFO and signed recovered-eye offsets\n";
    }
    for(const auto* name:{"noise","short-burst","short-qpsk","qpsk-low-snr","ofdm","fsk","gmsk","hop"}) {
      const auto source=load(cache,name);NarrowbandAnalysisSettings settings;
      settings.hypothesis=ModulationHypothesis::Qpsk;settings.symbolRateMode=SymbolRateMode::Manual;settings.symbolRateHz=625000;
      settings.cfoMode=CfoMode::Automatic;settings.matchedFilter=true;settings.timingRecovery=true;settings.carrierRecovery=true;settings.constellationStage=AnalysisStage::CarrierRecovered;
      const auto result=analyzeNarrowband(source,settings);
      check(!result.carrierLocked && !result.constellation.empty(),std::string("Negative case cannot claim carrier lock or discard true raw: ")+name);
    }
    std::cout<<"PASS single-carrier recovery against independent source symbols, CFO and timing truth\n";
  }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
