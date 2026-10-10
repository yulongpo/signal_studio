#include "infrastructure/source_metadata.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

namespace signalstudio {
SourceSuggestions readSigmfMetadata(const QString& path) {
    SourceSuggestions s; s.dataPath=QFileInfo(path).absoluteFilePath();
    const QFileInfo selected(path); QString metaPath;
    if(path.endsWith(".sigmf-meta",Qt::CaseInsensitive))metaPath=selected.absoluteFilePath();
    else if(path.endsWith(".sigmf-data",Qt::CaseInsensitive))metaPath=selected.absoluteFilePath().chopped(11)+".sigmf-meta";
    else return s;
    QFile file(metaPath);
    const auto fail=[&](const QString& reason){s.error="SigMF："+reason;return s;};
    if(!file.open(QIODevice::ReadOnly))return fail("配对元数据文件不可读取");
    if(file.size()>1024*1024)return fail("元数据超过1 MiB预算");
    QJsonParseError parse;const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
    if(parse.error!=QJsonParseError::NoError||!document.isObject())return fail("JSON无效："+parse.errorString());
    const auto root=document.object();
    if(!root["global"].isObject()||!root["captures"].isArray()||!root["annotations"].isArray())return fail("需要global、captures和annotations");
    const auto global=root["global"].toObject();
    if(!QRegularExpression("^1\\.[0-9]+\\.[0-9]+$").match(global["core:version"].toString()).hasMatch())return fail("版本缺失或非支持的1.x版本");
    QString dataset=QFileInfo(metaPath).fileName().chopped(11)+".sigmf-data";
    if(global.contains("core:dataset")) {
        if(!global["core:dataset"].isString())return fail("dataset必须为同目录文件名");
        dataset=global["core:dataset"].toString();
        if(dataset.isEmpty()||dataset.contains('/')||dataset.contains('\\')||dataset.contains(':')||dataset=="."||dataset=="..")return fail("拒绝跨目录dataset路径");
    }
    const QDir directory=QFileInfo(metaPath).absoluteDir();
    const QFileInfo data(directory.filePath(dataset));s.dataPath=data.absoluteFilePath();
    if(!data.isFile()||data.canonicalPath()!=QFileInfo(metaPath).canonicalPath())return fail("数据不存在或配对指向目录外");
    if(path.endsWith(".sigmf-data",Qt::CaseInsensitive)&&selected.canonicalFilePath()!=data.canonicalFilePath())return fail("所选数据与元数据配对冲突");
    const auto datatype=global["core:datatype"].toString();
    const auto type=QRegularExpression("^([cr])(i8|u8|i16|i32|f32|f64)(?:_(le|be))?$").match(datatype);
    if(!type.hasMatch())return fail("不支持datatype："+datatype+"，不会猜成CI16");
    SampleFormat f;f.structure=type.captured(1)=="r"?SampleStructure::Real:SampleStructure::ComplexIQ;
    const QStringList encodings{"i8","u8","i16","i32","f32","f64"};f.componentEncoding=static_cast<ComponentEncoding>(encodings.indexOf(type.captured(2)));
    if((bytesPerComponent(f)==1&&!type.captured(3).isEmpty())||(bytesPerComponent(f)>1&&type.captured(3).isEmpty()))return fail("datatype字节序不符合规范");
    f.byteOrder=bytesPerComponent(f)==1?ByteOrder::NotApplicable:type.captured(3)=="be"?ByteOrder::Big:ByteOrder::Little;
    f.iqLayout=f.structure==SampleStructure::Real?IQLayout::NotApplicable:IQLayout::IQInterleaved;f.normalizeIntegerAdc=f.componentEncoding<ComponentEncoding::Float32;
    const auto integer=[&](const QJsonValue& v,quint64& result){if(!v.isDouble())return false;const auto n=v.toInteger(-1);if(n<0||v.toDouble()!=static_cast<double>(n))return false;result=static_cast<quint64>(n);return true;};
    quint64 channels=1;if(global.contains("core:num_channels")&&(!integer(global["core:num_channels"],channels)||channels<1||channels>128))return fail("通道数必须在1–128范围");f.channels=static_cast<std::uint32_t>(channels);
    if(global.contains("core:sample_rate")){const auto value=global["core:sample_rate"];const double fs=value.toDouble(-1);if(!value.isDouble()||!std::isfinite(fs)||fs<=0||fs>1e12)return fail("sample_rate无效");s.sampleRateHz=fs;}
    if(global.contains("core:trailing_bytes")&&!integer(global["core:trailing_bytes"],f.trailerBytes))return fail("trailing_bytes无效");
    if(global.contains("core:offset")){quint64 offset=0;if(!integer(global["core:offset"],offset))return fail("offset无效");if(offset)s.warnings+="offset是外部样本编号，不作为磁盘字节偏移；工程使用本文件0起始样本索引。 ";}
    const auto captures=root["captures"].toArray();quint64 previous=0;
    for(qsizetype i=0;i<captures.size();++i){
        if(!captures[i].isObject())return fail("capture必须是对象");const auto capture=captures[i].toObject();quint64 begin=0;
        if(!integer(capture["core:sample_start"],begin)||(i&&begin<=previous)||(!i&&begin!=0))return fail("只支持从0开始、递增capture索引");previous=begin;
        if(capture.contains("core:frequency")){const auto value=capture["core:frequency"];const double fc=value.toDouble(-1);if(!value.isDouble()||!std::isfinite(fc)||fc<0||fc>1e12)return fail("frequency超出本应用非负频率范围");if(s.centerFrequencyHz&&*s.centerFrequencyHz!=fc)return fail("分段变频数据暂不支持统一RF轴");s.centerFrequencyHz=fc;}
        quint64 header=0;if(capture.contains("core:header_bytes")){if(!integer(capture["core:header_bytes"],header)||(i&&header))return fail("不支持段间非样本header");if(!i)f.headerBytes=header;}
        if(capture.contains("core:datetime"))s.warnings+="采集时间："+capture["core:datetime"].toString().left(80)+"。 ";
    }
    for(const auto& extension:global["core:extensions"].toArray()){const auto e=extension.toObject();if(!e["optional"].toBool(false))return fail("不支持必需扩展："+e["name"].toString());s.warnings+="忽略可选扩展："+e["name"].toString().left(80)+"。 ";}
    const QStringList supported{"core:datatype","core:version","core:sample_rate","core:num_channels","core:dataset","core:trailing_bytes","core:offset","core:extensions"};QStringList ignored;
    for(auto it=global.begin();it!=global.end();++it)if(!supported.contains(it.key()))ignored.append(it.key());
    if(!ignored.empty())s.warnings+="未用于解码的字段："+ignored.join(", ")+"。 ";if(!root["annotations"].toArray().isEmpty())s.warnings+="annotations不自动转为工程标记。 ";
    std::string error;if(!validateSampleFormat(f,error))return fail(QString::fromStdString(error));s.format=f;s.provenance="SigMF "+global["core:version"].toString();return s;
}
}
