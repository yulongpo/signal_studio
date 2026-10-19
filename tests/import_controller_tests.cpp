#include "infrastructure/import_controller.h"
#include "infrastructure/source_metadata.h"
#include "infrastructure/format_template_store.h"
#include "infrastructure/sample_format_json.h"
#include "tests/fixtures/raw_fixture.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QThread>
#include <QtTest>
using namespace signalstudio;
namespace {
void write(const QString& path,const QByteArray& bytes){QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())qFatal("Fixture write failed");}
QJsonObject metadata(const QString& type="ci16_le",double fs=1e6,double fc=0){return {{"global",QJsonObject{{"core:datatype",type},{"core:version","1.2.6"},{"core:sample_rate",fs}}},{"captures",QJsonArray{QJsonObject{{"core:sample_start",0},{"core:frequency",fc}}}},{"annotations",QJsonArray{}}};}
bool settle(ImportController& controller,int timeout=15000){QElapsedTimer elapsed;elapsed.start();while(controller.running()&&elapsed.elapsed()<timeout){controller.poll();QThread::msleep(1);}return !controller.running();}
ImportRow row(const QString& path,const SampleFormat& format={}){ImportRow result;result.metadata.path=path.toStdString();result.metadata.name=QFileInfo(path).fileName().toStdString();result.metadata.sampleRateHz=1e6;result.metadata.sampleFormat=format;result.confirmed=true;result.acquisitionKnown=true;return result;}
}
class ImportTests:public QObject{
    Q_OBJECT
private slots:
void sigmfPriorityAndValidation(){
    QTemporaryDir temp;const auto data=temp.filePath("CI16_FS2Msps_FC5MHz.sigmf-data");const auto meta=temp.filePath("CI16_FS2Msps_FC5MHz.sigmf-meta");write(data,fixtures::generate({}));
    auto root=metadata();write(meta,QJsonDocument(root).toJson());const auto suggestion=suggestSourceMetadata(meta);QVERIFY(suggestion.error.isEmpty());QCOMPARE(*suggestion.sampleRateHz,1e6);QCOMPARE(*suggestion.centerFrequencyHz,0.0);QVERIFY(suggestion.conflicts.contains("Fs"));QVERIFY(suggestion.conflicts.contains("Fc"));QCOMPARE(suggestion.dataPath,data);
    const auto paired=suggestSourceMetadata(data);QCOMPARE(*paired.format,*suggestion.format);
    for(const auto& datatype:QStringList{"ci8","cu8","ri16_be","ci32_be","rf32_le","cf64_be"}){root=metadata(datatype);write(meta,QJsonDocument(root).toJson());auto value=readSigmfMetadata(meta);QVERIFY2(value.error.isEmpty(),qPrintable(value.error));QVERIFY(value.format.has_value());}
    for(const auto& datatype:QStringList{"ci16","ci8_le","cu16_le","cf128_le","wat"}){write(meta,QJsonDocument(metadata(datatype)).toJson());QVERIFY(!readSigmfMetadata(meta).error.isEmpty());}
    root=metadata();auto global=root["global"].toObject();global["core:dataset"]="../outside.raw";root["global"]=global;write(meta,QJsonDocument(root).toJson());QVERIFY(readSigmfMetadata(meta).error.contains("路径"));
    root=metadata();root["captures"]=QJsonArray{QJsonObject{{"core:sample_start",0},{"core:frequency",0}},QJsonObject{{"core:sample_start",10},{"core:frequency",42}}};write(meta,QJsonDocument(root).toJson());QVERIFY(readSigmfMetadata(meta).error.contains("变频"));
    root=metadata();root["captures"]=QJsonArray{QJsonObject{{"core:sample_start",0},{"core:frequency",0}},QJsonObject{{"core:sample_start",10}}};write(meta,QJsonDocument(root).toJson());QVERIFY(readSigmfMetadata(meta).error.contains("不继承"));
    root=metadata();global=root["global"].toObject();global["core:sample_rate"]=0;root["global"]=global;write(meta,QJsonDocument(root).toJson());QVERIFY(!readSigmfMetadata(meta).error.isEmpty());
    root=metadata();global=root["global"].toObject();global["core:extensions"]=QJsonArray{QJsonObject{{"name","unknown"},{"optional",false}}};root["global"]=global;write(meta,QJsonDocument(root).toJson());QVERIFY(readSigmfMetadata(meta).error.contains("必需扩展"));
    write(meta,"{broken");QVERIFY(!readSigmfMetadata(meta).error.isEmpty());write(meta,QByteArray(1024*1024+1,' '));QVERIFY(readSigmfMetadata(meta).error.contains("预算"));
    const auto manual=suggestSourceMetadata(temp.filePath("unidentified.raw"));QVERIFY(!manual.format);QVERIFY(!manual.sampleRateHz);QVERIFY(!manual.centerFrequencyHz);
    const auto zero=suggestSourceMetadata(temp.filePath("RI16_FS1Msps_FC0Hz.raw"));QCOMPARE(*zero.centerFrequencyHz,0.0);
}
void templatesAtomicAndRoundtrip(){
    QTemporaryDir temp;QSettings settings(temp.filePath("settings.ini"),QSettings::IniFormat);FormatTemplateStore store(settings);QString error;SampleFormat f;f.iqLayout=IQLayout::PlanarIQ;f.channels=3;f.selectedChannel=2;f.headerBytes=(quint64{1}<<53)+7;
    QVERIFY(store.add("三通道格式",f,error));const auto id=store.templates().back().id;store.used(id);FormatTemplateStore reopened(settings);QCOMPARE(reopened.templates().back().format,f);QCOMPARE(reopened.recent().front(),id);
    const auto path=temp.filePath("templates.json");QVERIFY(store.exportFile(path,error));QSettings other(temp.filePath("other.ini"),QSettings::IniFormat);FormatTemplateStore imported(other);QVERIFY(imported.importFile(path,error));QCOMPARE(imported.templates().back().format,f);
    auto bad=store.encode();auto array=bad["templates"].toArray();auto item=array[0].toObject();auto format=item["sampleFormat"].toObject();format["channels"]=0;item["sampleFormat"]=format;array[0]=item;bad["templates"]=array;write(path,QJsonDocument(bad).toJson());QVERIFY(!imported.importFile(path,error));QCOMPARE(imported.templates().back().format,f);
    f.structure=SampleStructure::Real;QVERIFY(!store.add("非法实数IQ布局",f,error));QVERIFY(!store.add(QString(41,'x'),{},error));
}
void batchIsolationRetryDedupe(){
    QTemporaryDir temp;const auto complex=temp.filePath("ci16.raw"),real=temp.filePath("ri16.raw"),floating=temp.filePath("cf32.raw"),bad=temp.filePath("bad.raw");SampleFormat rf;rf.structure=SampleStructure::Real;rf.iqLayout=IQLayout::NotApplicable;SampleFormat cf;cf.componentEncoding=ComponentEncoding::Float32;cf.normalizeIntegerAdc=false;
    write(complex,fixtures::generate({}));write(real,fixtures::generate(rf));write(floating,fixtures::generate(cf));write(bad,"odd");ImportController controller;QString error;
    QVERIFY(controller.add(row(complex),error));QVERIFY(!controller.add(row(complex),error));auto different=row(complex);different.metadata.sampleFormat.iqLayout=IQLayout::QIInterleaved;QVERIFY(controller.add(different,error));
    QVERIFY(controller.add(row(real,rf),error));QVERIFY(controller.add(row(floating,cf),error));QVERIFY(controller.add(row(bad),error));QVERIFY(controller.start({0,1,2,3,4}));QVERIFY(settle(controller));QCOMPARE(controller.sources().size(),std::size_t{4});QCOMPARE(controller.rows()[4].status,ImportStatus::Failed);
    controller.markCommitted(controller.rows()[0].metadata);QCOMPARE(controller.sources().size(),std::size_t{3});const auto completed=controller.rows()[2].load.loaded;
    QVERIFY(controller.importLog().join('\n').contains("校验失败"));QVERIFY(controller.importLog().join('\n').contains("实际读取完成"));QVERIFY(controller.importLog().join('\n').contains("已提交工程"));
    write(bad,fixtures::generate({}));QVERIFY(controller.start({4}));QVERIFY(settle(controller));QCOMPARE(controller.rows()[4].status,ImportStatus::Ready);QCOMPARE(controller.rows()[2].load.loaded,completed);
    controller.rows()[4].metadata=controller.rows()[2].metadata;controller.rows()[4].status=ImportStatus::Pending;QVERIFY(!controller.start({4}));QCOMPARE(controller.rows()[4].status,ImportStatus::Failed);QVERIFY(controller.rows()[4].error.contains("重复"));
}
void cancellationAndReopen(){
    QTemporaryDir temp;const auto large=temp.filePath("large.raw");QFile file(large);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(128*1024*1024));file.close();SampleFormat f;f.componentEncoding=ComponentEncoding::Int8;f.byteOrder=ByteOrder::NotApplicable;
    ImportController controller;QString error;QVERIFY(controller.add(row(large,f),error));QVERIFY(controller.add(row(temp.filePath("missing.raw")),error));QVERIFY(controller.start({0,1}));
    QElapsedTimer clock;clock.start();while(controller.rows()[0].load.loaded==0&&clock.elapsed()<15000){controller.poll();QThread::msleep(1);}QVERIFY(controller.rows()[0].load.loaded>0);controller.stopAll();QVERIFY(settle(controller));QCOMPARE(controller.rows()[0].status,ImportStatus::Partial);QVERIFY(controller.canCommit());QVERIFY(controller.rows()[0].load.envelope.size()<=2048);QVERIFY(controller.rows()[0].load.loaded<controller.rows()[0].metadata.sampleCount);
    // Restart scans the full source again, rather than pretending to append a prefix.
    controller.restart();controller.stopRow(0);QVERIFY(settle(controller));
    for(int i=0;i<8;++i){ImportController closing;QVERIFY(closing.add(row(large,f),error));QVERIFY(closing.start({0}));closing.stopAll();}
}
};
QTEST_GUILESS_MAIN(ImportTests)
#include "import_controller_tests.moc"
