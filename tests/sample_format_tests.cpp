#include "tests/fixtures/raw_fixture.h"
#include "infrastructure/raw_sample_reader.h"
#include "infrastructure/int16_iq_file.h"
#include "infrastructure/sample_format_json.h"
#include "infrastructure/project_store.h"
#include "application/session.h"
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
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir dir;check(dir.isValid(),"temporary directory");const auto path=dir.filePath("source.raw");
    int cases=0;
    for(int structure=0;structure<2;++structure)for(int encoding=0;encoding<6;++encoding)
    for(int endian=1;endian<=2;++endian)for(int layout=1;layout<=3;++layout)for(int channelLayout=0;channelLayout<2;++channelLayout){
        SampleFormat f;f.structure=static_cast<SampleStructure>(structure);f.componentEncoding=static_cast<ComponentEncoding>(encoding);
        f.byteOrder=bytesPerComponent(f)==1?ByteOrder::NotApplicable:static_cast<ByteOrder>(endian);
        f.iqLayout=structure?IQLayout::NotApplicable:static_cast<IQLayout>(layout);f.channelLayout=static_cast<ChannelLayout>(channelLayout);
        f.channels=3;f.selectedChannel=2;f.headerBytes=7;f.trailerBytes=5;f.normalizeIntegerAdc=encoding<4;
        check(decodeSampleFormat(encodeSampleFormat(f))==f,"format JSON roundtrip");
        const auto b=fixtures::generate(f);QFile file(path);check(file.open(QIODevice::WriteOnly),"fixture open");check(file.write(b)==b.size(),"fixture write");file.close();
        QString error;RawSampleReader raw;check(raw.open(path,f,error),"reader open matrix");check(raw.sampleCount()==11,"matrix sample count");
        for(int n=0;n<11;++n){std::complex<double> z;check(raw.sampleAt(n,z,&error),"read matrix");auto wanted=fixtures::reference(2,n)/adcScale(f);if(structure)wanted.imag(0);check(std::abs(z-wanted)<1e-12,"matrix values/selected channel");}
        std::vector<std::complex<float>> data;check(raw.read({2,9},data,{},&error),"stream read matrix");
        for(int n=2;n<9;++n){auto wanted=fixtures::reference(2,n)/adcScale(f);if(structure)wanted.imag(0);check(std::abs(std::complex<double>(data[n-2])-wanted)<1e-5,"stream matrix values");}
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
