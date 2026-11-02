#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QtEndian>
#include <bit>
#include <iostream>
#include <numbers>
#include "domain/constellation.h"
#include "domain/constellation_observation.h"
#include "infrastructure/narrowband_analysis/constellation_analysis.h"
#include "infrastructure/narrowband_analysis/analysis_settings_json.h"
using namespace signalstudio;
namespace {
void check(bool ok,const std::string& reason){if(!ok)throw std::runtime_error(reason);}
QByteArray read(const QString& path){QFile f(path);check(f.open(QIODevice::ReadOnly),"Cannot read "+path.toStdString());return f.readAll();}
void checkDecisionCells(const ConstellationDefinition& definition) {
    check(constellationDecisionCells(definition,-3,3,[]{return true;}).empty(),"Reference geometry honors cancellation without publishing partial cells");
    const auto cells=constellationDecisionCells(definition,-3,3);check(cells.size()==definition.points.size(),"All in-view template cells must exist");
    double area=0;
    for(const auto& cell:cells) {
        const auto& polygon=cell.polygon;check(polygon.size()>=3,"Nonempty convex reference cell");
        double twice=0;
        for(std::size_t i=0;i<polygon.size();++i) {
            const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];twice+=std::imag(std::conj(a)*b);
            const auto nearest=nearestConstellationPoint(a,definition);
            check(std::norm(a-definition.points[cell.templateIndex])<=std::norm(a-definition.points[nearest])+1e-9,"Every cell vertex must satisfy actual nearest-neighbor inequalities, including cross-QAM missing corners");
        }
        area+=std::abs(twice)/2;
    }
    check(std::abs(area-36)<1e-7,"Reference cells cover the viewport with exact total area");
    for(int probe=0;probe<127;++probe) {
        const std::complex<double> z{2.9*std::sin(probe*1.337+.017),2.9*std::cos(probe*.773+.041)};
        double best=1e30;std::size_t nearest=0;
        for(std::size_t i=0;i<definition.points.size();++i)if(const auto d=std::norm(z-definition.points[i]);d<best){best=d;nearest=i;}
        const auto it=std::find_if(cells.begin(),cells.end(),[&](const auto& cell){return cell.templateIndex==nearest;});
        check(it!=cells.end(),"Brute-force nearest cell exists");
        for(std::size_t i=0;i<it->polygon.size();++i) {
            const auto a=it->polygon[i],b=it->polygon[(i+1)%it->polygon.size()];
            check(std::imag(std::conj(b-a)*(z-a))>=-1e-9,"Independent exhaustive nearest point lies in the displayed decision polygon");
        }
    }
}
QJsonObject run(const QJsonObject& record) {
    const auto root=qEnvironmentVariable("SS_MODULATION_FIXTURES"),name=record["name"].toString();
    FileMetadata source;source.id="modulation";source.demo=false;source.path=(root+'/'+name+".raw").toStdString();source.sampleRateHz=20e6;source.centerFrequencyHz=100e6;
    source.sampleCount=QFileInfo(QString::fromStdString(source.path)).size()/4;
    Channel channel;channel.id="modulation";channel.centerFrequencyHz=102e6;channel.bandwidthHz=1e6;channel.outputSampleRateHz=2e6;channel.sourceTime={0,source.sampleCount};
    AnalysisRequest request;request.identity={1,1,1,channel.id,analysisSourceKey(source,channel),{0,(source.sampleCount+9)/10}};
    ChannelSampleCache cache;auto data=std::make_shared<AnalysisSourceData>(readAnalysisSource(cache,source,channel,request));
    check(data->status==AnalysisStatus::Ready,data->message);
    NarrowbandAnalysisSettings settings;const auto family=record["family"].toString();
    settings.hypothesis=family=="psk"?ModulationHypothesis::Psk:family=="qam"?ModulationHypothesis::Qam:ModulationHypothesis::Apsk;
    settings.modulationOrder=record["order"].toInt();settings.symbolRateMode=SymbolRateMode::Manual;settings.symbolRateHz=record["rs"].toDouble();
    // High-order PSK requires a sufficiently close coarse-frequency hypothesis;
    // its Mth-power frequency ambiguity shrinks as M grows. Record this input.
    settings.cfoMode=family=="psk" && settings.modulationOrder>=32?CfoMode::Manual:CfoMode::Automatic;
    settings.manualCfoHz=record["cfo"].toDouble();settings.matchedFilter=settings.timingRecovery=settings.carrierRecovery=true;
    if(family=="qam" && settings.modulationOrder>=512){settings.filterSpanSymbols=20;settings.timingLoopBandwidth=.0005;}
    settings.constellationStage=settings.eyeStage=AnalysisStage::CarrierRecovered;
    QString error;NarrowbandAnalysisSettings decoded;check(decodeAnalysisSettings(encodeAnalysisSettings(settings),decoded,error) && decoded==settings,"v6 new family round trip");
    ConstellationDefinition geometry;std::string why;check(makeConstellation(settings,geometry,why),why);check(geometry.points.size()==static_cast<std::size_t>(settings.modulationOrder),"M point geometry");
    for(int probe=0;probe<257;++probe) {
        const std::complex<double> z{2.5*std::sin(probe*1.337),2.5*std::cos(probe*.773)};double brute=1e30;
        for(const auto point:geometry.points)brute=std::min(brute,std::norm(z-point));
        check(std::abs(std::norm(z-geometry.points[nearestConstellationPoint(z,geometry)])-brute)<1e-10,"Fast decision equals independent exhaustive distance");
    }
    const auto result=analyzeNarrowband(data,settings);
    checkDecisionCells(geometry);
    const auto observation=observeConstellation(result,settings);
    check(observation.decisionAvailable==result.carrierLocked,"Conditional decisions require actual carrier/timing success");
    if(observation.decisionAvailable) {
        check(observation.samples.size()==observation.decisions.size() && observation.samples.size()<=20000,"Bounded traceable decision projection");
        for(std::size_t i=0;i<observation.decisions.size();++i) {
            const auto& sample=observation.samples[i];const auto& decision=observation.decisions[i];
            check(sample.output==decision.output && sample.source==decision.source,"Decision retains exact source/output positions");
            const auto index=nearestConstellationPoint(std::complex<double>(sample.value)/observation.referenceScale,geometry);
            check(index==observation.templateIndices[i] && std::abs(std::complex<double>(decision.value)-geometry.points[index]*observation.referenceScale)<1e-6*std::max(1.0,observation.referenceScale),"Actual samples project through the shared decision function");
        }
    }
    const auto bytes=read(root+'/'+name+".symbols");std::vector<std::complex<double>> truth;
    for(qsizetype i=0;i+8<=bytes.size();i+=8)truth.emplace_back(std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData()+i)),std::bit_cast<float>(qFromLittleEndian<quint32>(bytes.constData()+i+4)));
    std::vector<std::complex<double>> observed,reference;double timing=0;
    for(const auto& symbol:result.synchronizedSymbols) {
        const auto t=(static_cast<double>(symbol.source.integer)+symbol.source.fraction-record["symbolOrigin"].toDouble())/32;
        const auto i=static_cast<std::int64_t>(std::llround(t));if(i<0 || i>=static_cast<std::int64_t>(truth.size()))continue;
        observed.emplace_back(symbol.value);reference.push_back(truth[static_cast<std::size_t>(i)]);timing+=(t-i)*(t-i);
    }
    double evm=1e6;
    for(int rotation=0;rotation<geometry.symmetry;++rotation) {
        const auto phase=std::polar(1.0,2*std::numbers::pi*rotation/geometry.symmetry);double dot=0,power=0,refPower=0;
        for(std::size_t i=0;i<observed.size();++i){dot+=std::real(reference[i]*std::conj(observed[i]*phase));power+=std::norm(observed[i]);refPower+=std::norm(reference[i]);}
        const double gain=dot/std::max(1e-30,power);if(gain<=0)continue;
        double cost=0;for(std::size_t i=0;i<observed.size();++i)cost+=std::norm(observed[i]*phase*gain-reference[i]);
        evm=std::min(evm,std::sqrt(cost/std::max(1e-30,refPower)));
    }
    timing=std::sqrt(timing/std::max<std::size_t>(1,observed.size()));
    const double cfo=std::abs(result.estimatedCfoHz-record["cfo"].toDouble())/settings.symbolRateHz;
    const double evmLimit=std::min(.18,geometry.minimumDistance*.45);
    const bool passed=result.carrierLocked && result.timingLocked && observed.size()>4096 && timing<.1 && evm<evmLimit && cfo<.001;
    std::cout<<name.toStdString()<<" pass="<<passed<<" carrier="<<result.carrierLocked<<" CFO/Rs="<<cfo<<" EVM="<<evm<<" timing="<<timing<<" conditional="<<result.conditionalEvm<<" "<<result.syncMessage<<std::endl;
    return {{"name",name},{"manualCfo",settings.cfoMode==CfoMode::Manual},{"sourceSnrDb",record["snrDb"]},{"rrcSpan",settings.filterSpanSymbols},{"timingBandwidth",settings.timingLoopBandwidth},{"truthEvmLimit",evmLimit},{"pass",passed},{"truthEvm",evm},{"timingUi",timing},{"cfoOverRs",cfo},{"carrierLocked",result.carrierLocked},{"message",QString::fromStdString(result.syncMessage)}};
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        {
            NarrowbandAnalysisSettings s;s.hypothesis=ModulationHypothesis::Apsk;s.modulationOrder=8;
            ConstellationDefinition reference;std::string why;check(makeConstellation(s,reference,why),why);
            for(const auto* rings:{"2:1e-200:0,6:2e-200:0","2:1e-300:0,6:2e-300:0"}) {
                s.apskRings=rings;ConstellationDefinition scaled;
                check(makeConstellation(s,scaled,why),"Small APSK relative radii remain valid");double power=0;
                check(scaled.points.size()==reference.points.size(),"Scale invariant APSK order");
                for(std::size_t i=0;i<scaled.points.size();++i) {
                    const auto point=scaled.points[i];check(std::isfinite(point.real()) && std::isfinite(point.imag()),"Small APSK radii must normalize to finite points");
                    check(std::abs(point-reference.points[i])<1e-12,"Relative radius scale must not change APSK geometry");power+=std::norm(point);
                }
                check(std::abs(power/scaled.points.size()-1)<1e-12 && scaled.minimumDistance>0,"Small APSK normalization has unit power and distinct points");
                QString error;NarrowbandAnalysisSettings decoded;check(decodeAnalysisSettings(encodeAnalysisSettings(s),decoded,error) && decoded==s,"Small APSK relative radii v6 round trip");
            }
            std::cout<<"PASS tiny APSK relative radii normalize finitely and preserve geometry/v6"<<std::endl;
        }
        const auto index=QJsonDocument::fromJson(read(qEnvironmentVariable("SS_MODULATION_FIXTURES")+"/modulation-index.json")).object();
        check(index["records"].toArray().size()==50 && index["schemaVersion"].toInt()==2,"Full 50-case family/order matrix required");
        QJsonArray reports;bool pass=true;
        for(const auto record:index["records"].toArray()){const auto r=run(record.toObject());pass=pass && r["pass"].toBool();reports.append(r);}
        {
            NarrowbandAnalysisSettings s;s.hypothesis=ModulationHypothesis::Apsk;s.modulationOrder=16;s.apskRings="4:1:45,12:3.15:15";
            ConstellationDefinition custom;std::string customError;check(makeConstellation(s,custom,customError),customError);checkDecisionCells(custom);
            QString error;NarrowbandAnalysisSettings decoded;check(decodeAnalysisSettings(encodeAnalysisSettings(s),decoded,error) && decoded==s,"Custom APSK ring v6 round trip");
            auto invalid=encodeAnalysisSettings(s);invalid["modulationOrder"]=3;check(!decodeAnalysisSettings(invalid,decoded,error),"Reject nonbinary order");
            invalid=encodeAnalysisSettings(s);invalid["apskRings"]="4:2:45,12:1:15";check(!decodeAnalysisSettings(invalid,decoded,error),"Reject non-increasing radii");
            invalid["apskRings"]="4:1:45,11:2:15";check(!decodeAnalysisSettings(invalid,decoded,error),"Reject ring count mismatch");
            invalid["apskRings"]=false;check(!decodeAnalysisSettings(invalid,decoded,error),"Reject non-text rings");
            s.apskRings=std::string(1025,' ')+"4:1:45,12:3.15:15";ConstellationDefinition oversized;std::string why;
            check(!makeConstellation(s,oversized,why),"GUI/DSP and v6 reject the same oversized ring configuration");
            QJsonObject legacy{{"hypothesis",3}};check(decodeAnalysisSettings(legacy,decoded,error) && decoded.hypothesis==ModulationHypothesis::Qam16,"Old v6 enum values retain meaning");
        }
        {
            FileMetadata metadata;metadata.id="noise";metadata.demo=false;metadata.path=(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/noise.raw").toStdString();
            metadata.sampleRateHz=20e6;metadata.centerFrequencyHz=100e6;metadata.sampleCount=QFileInfo(QString::fromStdString(metadata.path)).size()/4;
            Channel channel;channel.id="negative";channel.centerFrequencyHz=102e6;channel.bandwidthHz=1e6;channel.outputSampleRateHz=2e6;channel.sourceTime={0,metadata.sampleCount};
            AnalysisRequest request;request.identity={1,1,1,channel.id,analysisSourceKey(metadata,channel),{0,metadata.sampleCount/10}};
            ChannelSampleCache cache;auto data=std::make_shared<AnalysisSourceData>(readAnalysisSource(cache,metadata,channel,request));check(data->status==AnalysisStatus::Ready,"Negative real noise IQ");
            for(const auto& entry:{std::pair{ModulationHypothesis::Psk,8},std::pair{ModulationHypothesis::Qam,32},std::pair{ModulationHypothesis::Qam,256},std::pair{ModulationHypothesis::Qam,4096},std::pair{ModulationHypothesis::Apsk,64},std::pair{ModulationHypothesis::Apsk,256}}) {
                NarrowbandAnalysisSettings s;s.hypothesis=entry.first;s.modulationOrder=entry.second;s.symbolRateMode=SymbolRateMode::Manual;s.symbolRateHz=625000;
                s.cfoMode=CfoMode::Manual;s.matchedFilter=s.timingRecovery=s.carrierRecovery=true;s.constellationStage=AnalysisStage::CarrierRecovered;
                const auto result=analyzeNarrowband(data,s);check(!result.carrierLocked && !result.constellation.empty(),"Noise must retain IQ without dense-template false lock");
                const auto observation=observeConstellation(result,s);check(!observation.decisionAvailable && observation.decisions.empty() && !observation.samples.empty(),"Noise/failure must not publish conditional decided symbols");
            }
            std::cout<<"PASS six noise hypotheses and strict family/order/ring persistence"<<std::endl;
        }
        const auto output=qEnvironmentVariable("SS_NARROWBAND_REPORT_DIR");QDir().mkpath(output);QFile file(output+"/modulation.json");check(file.open(QIODevice::WriteOnly),"Report write");file.write(QJsonDocument(reports).toJson());
        check(pass,"Family/order recovery cases failed");return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
