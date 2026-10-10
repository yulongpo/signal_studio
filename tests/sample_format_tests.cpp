#include "tests/fixtures/raw_fixture.h"
#include "infrastructure/raw_sample_reader.h"
#include "infrastructure/int16_iq_file.h"
#include "infrastructure/sample_format_json.h"
#include "infrastructure/project_store.h"
#include "application/session.h"
#include "infrastructure/channel_processor.h"
#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <iostream>
#include <thread>
#include <chrono>
#include <limits>
#include <numbers>

using namespace signalstudio;
void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
void extendedNumericalChecks(const QString& directory) {
    QString error;
    {FileMetadata embedded;embedded.path=":/signalstudio/demo/narrowband_demo.iq";embedded.sampleFormat.iqLayout=IQLayout::QIInterleaved;Int16IqFile reader;
        check(!reader.open(embedded,error)&&error.contains("固定"),"embedded legacy resource cannot silently ignore format override");}
    const auto path=QDir(directory).filePath("extended.raw");
    ChannelSampleData reference;
    ChannelSampleCache cache;
    int cases=0;
    for(int encoding=0;encoding<6;++encoding)for(int endian=1;endian<=2;++endian)
    for(int layout=1;layout<=3;++layout)for(int channelsLayout=0;channelsLayout<2;++channelsLayout){
        SampleFormat f;f.componentEncoding=static_cast<ComponentEncoding>(encoding);
        f.byteOrder=bytesPerComponent(f)==1?ByteOrder::NotApplicable:static_cast<ByteOrder>(endian);
        f.iqLayout=static_cast<IQLayout>(layout);f.channels=3;f.selectedChannel=2;
        f.channelLayout=static_cast<ChannelLayout>(channelsLayout);f.headerBytes=7;f.trailerBytes=5;f.normalizeIntegerAdc=false;
        {QFile file(path);check(file.open(QIODevice::WriteOnly),"DDC fixture open");const auto bytes=fixtures::generate(f,64);check(file.write(bytes)==bytes.size(),"DDC fixture write");}
        FileMetadata source;check(describeRawFile(path,f,1e6,0,0,source,error),"DDC format descriptor");source.id="format-source";
        Channel channel;channel.id="format-channel";channel.sourceTime={0,64};channel.centerFrequencyHz=0;channel.bandwidthHz=100000;channel.outputSampleRateHz=500000;
        ChannelDspPlan plan;check(makeChannelDspPlan(source,channel,plan,error),"DDC format plan");
        ChannelSampleData data;check(processChannelSamples(source,channel,plan,{0,32},data),"DDC actual format samples");
        if(!cases)reference=data;
        check(data.samples.size()==reference.samples.size()&&data.waveformScale==1,"DDC count and amplitude unit");
        for(std::size_t n=0;n<data.samples.size();++n)check(std::abs(data.samples[n]-reference.samples[n])<1e-4,"DDC all format numeric equivalence");
        ChannelSampleData cached;check(cache.process(source,channel,plan,{0,32},cached),"DDC format cache");
        for(std::size_t n=0;n<data.samples.size();++n)check(std::abs(cached.samples[n]-data.samples[n])<1e-4,"DDC cache cannot reuse another format");
        source.availability.status=LoadStatus::Partial;source.availability.availableSamples=32;channel.sourceTime.end=32;
        check(makeChannelDspPlan(source,channel,plan,error)&&processChannelSamples(source,channel,plan,{0,16},data),"DDC actual available prefix");
        ++cases;
    }
    for(int encoding=0;encoding<6;++encoding)for(bool normalize:{false,true}){
        SampleFormat f;f.componentEncoding=static_cast<ComponentEncoding>(encoding);f.structure=SampleStructure::Real;f.iqLayout=IQLayout::NotApplicable;
        f.byteOrder=bytesPerComponent(f)==1?ByteOrder::NotApplicable:ByteOrder::Big;f.normalizeIntegerAdc=encoding<4&&normalize;
        const double minimum=encoding<2?-128:encoding==2?-32768:encoding==3?-2147483648.0:-.75;
        const double maximum=encoding<2?127:encoding==2?32767:encoding==3?2147483647.0:.625;
        QByteArray bytes;fixtures::appendComponent(bytes,f,minimum);fixtures::appendComponent(bytes,f,maximum);
        {QFile file(path);check(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"extrema fixture write");}
        RawSampleReader reader;check(reader.open(path,f,error),"extrema open");std::complex<double> z;
        check(reader.sampleAt(0,z,&error)&&z.real()==minimum/adcScale(f),"negative/uint8 midpoint extremum");
        check(reader.sampleAt(1,z,&error)&&z.real()==maximum/adcScale(f),"maximum extremum");
    }
    for(auto encoding:{ComponentEncoding::Float32,ComponentEncoding::Float64})for(double value:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}){
        SampleFormat f;f.componentEncoding=encoding;f.normalizeIntegerAdc=false;
        QByteArray bytes;fixtures::appendComponent(bytes,f,value);fixtures::appendComponent(bytes,f,0);
        {QFile file(path);check(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"invalid float fixture");}
        RawSampleReader reader;std::complex<double> z;check(reader.open(path,f,error)&&!reader.sampleAt(0,z,&error),"nonfinite all floating types reject");
    }
    for(int encoding=0;encoding<6;++encoding)for(bool real:{false,true}){
        SampleFormat f;f.componentEncoding=static_cast<ComponentEncoding>(encoding);f.byteOrder=bytesPerComponent(f)==1?ByteOrder::NotApplicable:ByteOrder::Little;
        f.structure=real?SampleStructure::Real:SampleStructure::ComplexIQ;f.iqLayout=real?IQLayout::NotApplicable:IQLayout::IQInterleaved;f.normalizeIntegerAdc=encoding<4;
        QByteArray bytes;const double scale=adcScale(f);
        for(int n=0;n<1024;++n){const double phase=2*std::numbers::pi*64*n/1024;
            const auto value=[&](double v){return encoding<4?std::round(v*scale):v;};
            fixtures::appendComponent(bytes,f,value(.5*std::cos(phase)));if(!real)fixtures::appendComponent(bytes,f,value(.5*std::sin(phase)));}
        {QFile file(path);check(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"tone fixture write");}
        FileMetadata source;check(describeRawFile(path,f,1024,1000000,0,source,error),"tone descriptor");
        Int16IqFile reader;check(reader.open(source,error),"tone adapter");auto spectral=reader.spectralSource();spectral.sampleRateHz=1024;
        PsdSettings settings;settings.parameters.window=SpectralWindow::Rectangular;settings.parameters.method=SpectralMethod::Periodogram;
        std::string message;auto frame=analyzeSpectrum(spectral,{0,1024},real?FrequencyRange{0,512}:FrequencyRange{-512,512},real?512:1024,settings,message);
        check(bool(frame),"tone PSD every encoding");double power=0;for(auto v:frame->linearPower)power+=v*frame->binHz;
        check(std::abs(power-(real?.125:.25))<.004,"known tone PSD power normalization");
        check(frame->binAt(64)==static_cast<std::size_t>(std::distance(frame->linearPower.begin(),std::max_element(frame->linearPower.begin(),frame->linearPower.end()))),"known tone peak");
        SpectrogramSettings stft;stft.parameters=settings.parameters;const auto frames=analyzeSpectrogram(spectral,{0,1024},real?FrequencyRange{0,512}:FrequencyRange{-512,512},real?512:1024,2,stft,1);
        check(frames&&!frames->frames.empty(),"known tone actual STFT every encoding");
        for(const auto& actual:frames->frames)check(actual->binAt(64)==static_cast<std::size_t>(std::distance(actual->linearPower.begin(),std::max_element(actual->linearPower.begin(),actual->linearPower.end()))),"known tone STFT peak");
        if(!real){
            Channel channel;channel.id="layout-switch";channel.sourceTime={0,1024};channel.centerFrequencyHz=source.centerFrequencyHz;channel.bandwidthHz=400;channel.outputSampleRateHz=1024;
            ChannelDspPlan plan;check(makeChannelDspPlan(source,channel,plan,error),"tone cache plan");ChannelSampleCache cache;
            ChannelSampleData first,second;check(cache.process(source,channel,plan,{0,1024},first),"tone IQ cache");
            source.sampleFormat.iqLayout=IQLayout::QIInterleaved;Int16IqFile swapped;check(swapped.open(source,error),"QI reinterpret same physical data");
            check(cache.process(source,channel,plan,{0,1024},second),"tone QI cache");double difference=0;for(std::size_t n=0;n<first.samples.size();++n)difference+=std::abs(first.samples[n]-second.samples[n]);check(difference>10&&cache.stats().misses==2,"same physical file format change cannot reuse cached IQ");
            auto opposite=swapped.spectralSource();opposite.sampleRateHz=1024;auto changed=analyzeSpectrum(opposite,{0,1024},{-512,512},1024,settings,message);
            check(changed&&changed->binAt(-64)==static_cast<std::size_t>(std::distance(changed->linearPower.begin(),std::max_element(changed->linearPower.begin(),changed->linearPower.end()))),"QI layout changes real PSD peak sign");
        }
        std::vector<float> waveform;check(reader.waveform({0,1024},1024,WaveformMode::I,waveform),"tone waveform");
        check(std::abs(waveform.front()-.5*scale)<1.0,"tone waveform preserves raw amplitude");
        if(real){Channel channel;ChannelDspPlan plan;check(!makeChannelDspPlan(source,channel,plan,error),"real ADC DDC explicitly rejected");}
    }
    std::cout<<"PASS "<<cases<<" DDC format/layout/channel cases, actual prefix and cache equivalence; all encoding extrema and known tones\n";
}
void verifyScriptFixtures(const QString& directory) {
    QFile manifest(QDir(directory).filePath("manifest.json"));check(manifest.open(QIODevice::ReadOnly),"script manifest");
    const auto fixtures=QJsonDocument::fromJson(manifest.readAll()).object()["fixtures"].toArray();check(fixtures.size()==6,"six independent script fixtures");
    SampleFormat last;
    for(const auto& value:fixtures){const auto item=value.toObject();const auto path=QDir(directory).filePath(item["file"].toString());last=decodeSampleFormat(item["sampleFormat"].toObject());
        QFile file(path);check(file.open(QIODevice::ReadOnly),"script fixture read");check(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex()==item["sha256"].toString().toLatin1(),"script fixture hash");file.close();
        RawSampleReader reader;QString error;check(reader.open(path,last,error),"script RAW decoder");const auto expected=item["expected"].toArray();check(reader.sampleCount()==static_cast<quint64>(expected.size()),"script sample count");
        for(int n=0;n<expected.size();++n){std::complex<double> z;check(reader.sampleAt(n,z,&error),"script sample value");const auto pair=expected[n].toArray();check(z==std::complex<double>(pair[0].toDouble(),pair[1].toDouble()),"independent Python reference exact equality");}}
    RawSampleReader reader;QString error;check(!reader.open(QDir(directory).filePath("truncated.raw"),last,error),"script truncated file rejects");
    std::cout<<"PASS independent script fixtures, SHA-256 and 384 reference samples\n";
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--benchmark"){
        QTemporaryDir temporary;const auto filePath=temporary.filePath("benchmark-ci16.raw");
        {QFile file(filePath);check(file.open(QIODevice::WriteOnly)&&file.resize(64*1024*1024),"benchmark actual fixture");}
        QElapsedTimer elapsed;elapsed.start();SourceLoadSnapshot result;
        {SourceLoader loader(filePath,16*1024*1024,{});do{result=loader.snapshot();if(elapsed.elapsed()>180000)throw std::runtime_error("benchmark timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(!result.finished);}
        check(result.error.isEmpty()&&result.loaded==16*1024*1024&&result.envelope.size()<=2048,"benchmark complete real scan");
        const auto milliseconds=elapsed.elapsed();const QJsonObject report{{"pass",true},{"format","CI16"},{"physicalBytes",64*1024*1024},{"decodedSamples",QString::number(result.loaded)},{"elapsedMilliseconds",milliseconds},{"envelopePoints",static_cast<int>(result.envelope.size())},{"fixture","Temporary 64 MiB zero-code RAW file; real QFile scan and decoding; deleted after run"}};
        QFile output(QString::fromLocal8Bit(argv[2]));check(output.open(QIODevice::WriteOnly)&&output.write(QJsonDocument(report).toJson())>0,"benchmark report");
        std::cout<<"PASS actual 64 MiB scan, "<<milliseconds<<" ms\n";return 0;
    }
    QTemporaryDir dir;check(dir.isValid(),"temporary directory");const auto path=dir.filePath("source.raw");
    extendedNumericalChecks(dir.path());
    if(argc>1)verifyScriptFixtures(QString::fromLocal8Bit(argv[1]));
    int cases=0;
    for(int structure=0;structure<2;++structure)for(int encoding=0;encoding<6;++encoding)
    for(int endian=1;endian<=2;++endian)for(int layout=1;layout<=3;++layout)for(int channelLayout=0;channelLayout<2;++channelLayout)for(int selected=0;selected<3;++selected){
        SampleFormat f;f.structure=static_cast<SampleStructure>(structure);f.componentEncoding=static_cast<ComponentEncoding>(encoding);
        f.byteOrder=bytesPerComponent(f)==1?ByteOrder::NotApplicable:static_cast<ByteOrder>(endian);
        f.iqLayout=structure?IQLayout::NotApplicable:static_cast<IQLayout>(layout);f.channelLayout=static_cast<ChannelLayout>(channelLayout);
        f.channels=3;f.selectedChannel=selected;f.headerBytes=7;f.trailerBytes=5;f.normalizeIntegerAdc=encoding<4;
        check(decodeSampleFormat(encodeSampleFormat(f))==f,"format JSON roundtrip");
        const auto b=fixtures::generate(f);QFile file(path);check(file.open(QIODevice::WriteOnly),"fixture open");check(file.write(b)==b.size(),"fixture write");file.close();
        QString error;RawSampleReader raw;check(raw.open(path,f,error),"reader open matrix");check(raw.sampleCount()==11,"matrix sample count");
        for(int n=0;n<11;++n){std::complex<double> z;check(raw.sampleAt(n,z,&error),"read matrix");auto wanted=fixtures::reference(selected,n)/adcScale(f);if(structure)wanted.imag(0);check(std::abs(z-wanted)<1e-12,"matrix values/selected channel");}
        std::vector<std::complex<float>> data;check(raw.read({2,9},data,{},&error),"stream read matrix");
        for(int n=2;n<9;++n){auto wanted=fixtures::reference(selected,n)/adcScale(f);if(structure)wanted.imag(0);check(std::abs(std::complex<double>(data[n-2])-wanted)<1e-5,"stream matrix values");}
        check(!raw.read({0,11},data,[]{return true;}),"cancel read");
        FileMetadata m;m.sampleFormat=f;m.path=path.toStdString();m.name="matrix";m.sampleRateHz=1e6;m.centerFrequencyHz=0;m.sampleCount=11;m.demo=false;
        Session session;check(!session.addDemoFile(m).empty(),"format session");check(ProjectStore::save(dir.filePath("project.json"),session.project(),error),"format project save");
        Project restored;check(ProjectStore::load(dir.filePath("project.json"),restored,error),"format project load");check(restored.files.front().metadata.sampleFormat==f,"project v4 format values");++cases;
    }
    std::string message;std::uint64_t count;SampleFormat f;
    check(sampleCountForBytes(f,65536,count,message)&&count==16384,"CI16 count");f.structure=SampleStructure::Real;f.iqLayout=IQLayout::NotApplicable;
    check(sampleCountForBytes(f,65536,count,message)&&count==32768,"RI16 count");
    check(!sampleCountForBytes(f,65537,count,message),"tail must reject");f.headerBytes=UINT64_MAX;f.trailerBytes=UINT64_MAX;
    check(!sampleCountForBytes(f,UINT64_MAX,count,message),"offset overflow");
    f={};auto json=encodeSampleFormat(f);json["channels"]=129;bool rejected=false;try{decodeSampleFormat(json);}catch(...){rejected=true;}check(rejected,"invalid channels reject");
    f.componentEncoding=ComponentEncoding::Float64;f.normalizeIntegerAdc=false;QByteArray b;fixtures::appendComponent(b,f,std::numeric_limits<double>::infinity());fixtures::appendComponent(b,f,0);
    {QFile file(path);check(file.open(QIODevice::WriteOnly),"nonfinite fixture");file.write(b);}RawSampleReader raw;QString error;check(raw.open(path,f,error),"nonfinite open");std::complex<double> z;check(!raw.sampleAt(0,z,&error),"nonfinite rejected");
    // Exact old CI16 normalized read and waveform ADC equivalence.
    f={};{QFile file(path);file.open(QIODevice::WriteOnly);file.write(fixtures::generate(f));}
    Int16IqFile old;check(old.open(path,error),"old reader");FileMetadata m;m.path=path.toStdString();m.sampleCount=11;m.sampleFormat=f;Int16IqFile adapter;check(adapter.open(m,error),"format adapter");
    for(int n=0;n<11;++n)check(old.sampleAt(n)==adapter.sampleAt(n),"CI16 old numeric equality");
    // Real single-side density: rectangular coherent cosine integrates to A^2/2.
    SpectralSource source;source.real=true;source.sampleRateHz=1024;
    source.read=[](TimeRange r,std::vector<std::complex<float>>& out,const SpectralCancel&){out.resize(r.end-r.begin);for(std::size_t n=0;n<out.size();++n)out[n]={float(.5*std::cos(2*std::numbers::pi*64*(r.begin+n)/1024)),0};return true;};
    PsdSettings settings;settings.parameters.window=SpectralWindow::Rectangular;settings.parameters.method=SpectralMethod::Periodogram;
    std::string err;auto frame=analyzeSpectrum(source,{0,1024},{0,512},512,settings,err);check(bool(frame),"real PSD");
    double power=0;for(float p:frame->linearPower)power+=p*frame->binHz;check(std::abs(power-.125)<1e-5,"real one-side power");check(frame->binAt(64)==std::distance(frame->linearPower.begin(),std::max_element(frame->linearPower.begin(),frame->linearPower.end())),"real peak");
    // Legacy v1/v2/v3 ignore absent format fields and keep CI16 semantics.
    Session legacy;legacy.addDemoFile("legacy",1e6,100e6,1);
    const auto projectPath=dir.filePath("legacy.json");check(ProjectStore::save(projectPath,legacy.project(),error),"legacy seed");QJsonObject root;
    {QFile file(projectPath);file.open(QIODevice::ReadOnly);root=QJsonDocument::fromJson(file.readAll()).object();}
    for(int version:{1,2,3}){auto document=root;document["version"]=version;auto files=document["files"].toArray();auto entry=files[0].toObject();auto metadata=entry["metadata"].toObject();metadata.remove("sampleFormat");entry["metadata"]=metadata;files[0]=entry;document["files"]=files;
        {QFile file(projectPath);file.open(QIODevice::WriteOnly);file.write(QJsonDocument(document).toJson());}Project restored;check(ProjectStore::load(projectPath,restored,error),"legacy v1/v2/v3 load");check(restored.files[0].metadata.sampleFormat==SampleFormat{},"legacy CI16 fallback");}
    // Non-four-byte frame and stop/restart boundary with meaningful real reads.
    f={};f.componentEncoding=ComponentEncoding::Int8;f.byteOrder=ByteOrder::NotApplicable;f.channels=3;
    {QFile file(path);file.open(QIODevice::WriteOnly);check(file.resize(6*8*1024*1024),"bounded sparse fixture");}
    SourceLoadSnapshot snapshot;
    {SourceLoader loader(path,8*1024*1024,f);for(int wait=0;wait<1000;++wait){snapshot=loader.snapshot();if(snapshot.loaded>0||snapshot.finished)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}loader.stop();for(int wait=0;wait<1000;++wait){snapshot=loader.snapshot();if(snapshot.finished)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
    check(snapshot.finished&&snapshot.error.isEmpty()&&snapshot.loaded>0&&snapshot.loaded<=snapshot.target&&snapshot.envelope.size()<=2048,"non4 frame stopped complete prefix");
    {SourceLoader loader(path,snapshot.loaded,f);for(int wait=0;wait<1000;++wait){auto s=loader.snapshot();if(s.finished){check(s.loaded==snapshot.loaded&&s.error.isEmpty(),"restart saved prefix");break;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
    std::cout<<"PASS "<<cases<<" format/reader/project matrix cases; counts, overflow, nonfinite, cancellation, CI16 equality, real PSD, v1/v2/v3 migration, non4 frame prefix\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
