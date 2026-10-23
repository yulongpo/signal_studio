#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <iostream>
#include "infrastructure/narrowband_analysis/constellation_analysis.h"
using namespace signalstudio;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();if(args.size()!=3)return 2;
    FileMetadata source;source.id="user-feedback";source.demo=false;source.path=args[1].toStdString();
    source.sampleRateHz=60e6;source.centerFrequencyHz=30e6;source.sampleFormat.headerBytes=44;
    source.sampleCount=(QFileInfo(args[1]).size()-44)/4;
    Channel channel;channel.id="user-feedback";channel.centerFrequencyHz=9962746.14;channel.bandwidthHz=6769558.28;channel.outputSampleRateHz=13540000;
    channel.sourceTime={823,597805};AnalysisRequest request;
    request.identity={1,1,1,channel.id,analysisSourceKey(source,channel),{0,((channel.sourceTime.end-channel.sourceTime.begin)*13540000+59999999)/60000000}};
    ChannelSampleCache cache;auto data=std::make_shared<AnalysisSourceData>(readAnalysisSource(cache,source,channel,request));
    if(data->status!=AnalysisStatus::Ready){std::cerr<<data->message;return 1;}
    NarrowbandAnalysisSettings settings;settings.symbolRateMode=SymbolRateMode::Estimated;settings.cfoMode=CfoMode::Automatic;
    settings.matchedFilter=settings.timingRecovery=settings.carrierRecovery=true;settings.normalizePower=settings.removeDc=true;
    settings.constellationStage=settings.eyeStage=AnalysisStage::CarrierRecovered;
    QJsonArray reports;bool pass=true;
    for(const auto h:{ModulationHypothesis::Qpsk,ModulationHypothesis::Qam}) {
        settings.hypothesis=h;settings.modulationOrder=32;const auto result=analyzeNarrowband(data,settings);
        const bool expected=h==ModulationHypothesis::Qam;
        const bool ok=result.carrierLocked==expected && !result.constellation.empty() &&
          (!expected || (std::abs(result.symbolRateHz-4891160)/4891160<.01 && std::abs(result.estimatedCfoHz+167185.14)<4891.16 && result.eyeStatus==AnalysisStatus::Ready));
        pass=pass && ok;
        reports.append(QJsonObject{{"assumption",expected?"32QAM":"QPSK"},{"pass",ok},{"symbolRateHz",result.symbolRateHz},{"estimatedCfoHz",result.estimatedCfoHz},
            {"carrierLocked",result.carrierLocked},{"timingLocked",result.timingLocked},{"actualStage",static_cast<int>(result.constellationStage)},
            {"conditionalEvm",result.conditionalEvm},{"eyeTraces",static_cast<qint64>(result.eyes.size())},{"message",QString::fromStdString(result.syncMessage)}});
        std::cout<<(expected?"32QAM":"QPSK")<<" pass="<<ok<<" rate="<<result.symbolRateHz<<" CFO="<<result.estimatedCfoHz<<" locked="<<result.carrierLocked<<" conditionalEVM="<<result.conditionalEvm<<" "<<result.syncMessage<<std::endl;
    }
    QFile file(args[2]);if(!file.open(QIODevice::WriteOnly))return 2;
    file.write(QJsonDocument(QJsonObject{{"source",args[1]},{"inputKind","user-provided generated wideband IQ; truth labels used only for validation"},{"pass",pass},{"runs",reports}}).toJson());
    return pass?0:1;
}
