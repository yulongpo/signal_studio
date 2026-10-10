#include "infrastructure/format_template_store.h"
#include "infrastructure/sample_format_json.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>
#include <algorithm>

namespace signalstudio {
FormatTemplateStore::FormatTemplateStore(QSettings& settings):settings_(settings) {
    const QStringList names{"通用 CI16","通用 CF32","通用 CI8","通用 CU8","实数 Int16 ADC"};
    for(int i=0;i<5;++i){SampleFormat f;if(i==1){f.componentEncoding=ComponentEncoding::Float32;f.normalizeIntegerAdc=false;}
        if(i==2||i==3){f.componentEncoding=i==2?ComponentEncoding::Int8:ComponentEncoding::UInt8;f.byteOrder=ByteOrder::NotApplicable;}
        if(i==4){f.structure=SampleStructure::Real;f.iqLayout=IQLayout::NotApplicable;}
        templates_.push_back({QString("builtin-%1").arg(i),names[i],f,true});}
    const auto bytes=settings_.value("import/templates-v1").toByteArray();
    if(!bytes.isEmpty()){std::vector<FormatTemplate> user;if(bytes.size()>1024*1024)error_="模板存储超过预算";else if(decode(QJsonDocument::fromJson(bytes).object(),user,error_))templates_.insert(templates_.end(),user.begin(),user.end());}
    recent_=settings_.value("import/recent-templates").toStringList().mid(0,8);
    recent_.removeIf([&](const auto& id){return std::none_of(templates_.begin(),templates_.end(),[&](const auto& t){return t.id==id;});});
}
bool FormatTemplateStore::decode(const QJsonObject& root,std::vector<FormatTemplate>& output,QString& error){
    if(root["format"].toString()!="signal-studio-format-templates"||root["version"].toInteger()!=1||!root["templates"].isArray()){error="不是版本1的原生格式模板；原型JSON不能作为工程/模板直接导入";return false;}
    const auto list=root["templates"].toArray();if(list.size()>128){error="最多128个用户模板";return false;}
    std::vector<FormatTemplate> staged;QStringList ids;
    for(const auto& value:list){const auto object=value.toObject();FormatTemplate t;t.id=object["id"].toString();t.name=object["name"].toString().trimmed();
        if(t.id.isEmpty()||t.id.size()>80||t.id.startsWith("builtin-")||ids.contains(t.id)||t.name.isEmpty()||t.name.size()>40||!object["sampleFormat"].isObject()){error="模板名称/ID/格式无效或重复";return false;}
        try { t.format=decodeSampleFormat(object["sampleFormat"].toObject()); } catch(const std::exception& e){error=QString::fromUtf8(e.what());return false;}
        ids.append(t.id);staged.push_back(std::move(t));}
    output=std::move(staged);error.clear();return true;
}
QJsonObject FormatTemplateStore::encode() const {QJsonArray list;for(const auto& t:templates_)if(!t.builtin)list.append(QJsonObject{{"id",t.id},{"name",t.name},{"sampleFormat",encodeSampleFormat(t.format)}});return {{"format","signal-studio-format-templates"},{"version",1},{"templates",list}};}
void FormatTemplateStore::save(){settings_.setValue("import/templates-v1",QJsonDocument(encode()).toJson(QJsonDocument::Compact));settings_.setValue("import/recent-templates",recent_);settings_.sync();}
bool FormatTemplateStore::add(const QString& name,const SampleFormat& format,QString& error){std::string invalid;if(name.trimmed().isEmpty()||name.trimmed().size()>40||templates_.size()>=133||!validateSampleFormat(format,invalid)){error=invalid.empty()?"模板名称1–40字，最多128个用户模板":QString::fromStdString(invalid);return false;}templates_.push_back({QUuid::createUuid().toString(QUuid::WithoutBraces),name.trimmed(),format,false});save();error.clear();return settings_.status()==QSettings::NoError;}
bool FormatTemplateStore::importFile(const QString& path,QString& error){QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024){error="模板文件不可读或超过1 MiB";return false;}QJsonParseError parse;const auto doc=QJsonDocument::fromJson(file.readAll(),&parse);std::vector<FormatTemplate> staged;if(parse.error!=QJsonParseError::NoError||!decode(doc.object(),staged,error)){if(error.isEmpty())error=parse.errorString();return false;}
    auto merged=templates_;for(auto& t:staged){const auto found=std::find_if(merged.begin(),merged.end(),[&](const auto& old){return old.id==t.id;});if(found==merged.end())merged.push_back(t);else *found=t;}
    if(merged.size()>133){error="用户模板超过128个";return false;}templates_=std::move(merged);save();return settings_.status()==QSettings::NoError;
}
bool FormatTemplateStore::exportFile(const QString& path,QString& error) const {QSaveFile file(path);const auto data=QJsonDocument(encode()).toJson();if(!file.open(QIODevice::WriteOnly)||file.write(data)!=data.size()||!file.commit()){error="模板原子导出失败："+file.errorString();return false;}error.clear();return true;}
void FormatTemplateStore::used(const QString& id){recent_.removeAll(id);recent_.prepend(id);recent_=recent_.mid(0,8);save();}
}
