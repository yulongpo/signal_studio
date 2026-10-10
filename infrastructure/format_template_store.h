#pragma once
#include "domain/sample_format.h"
#include <QJsonObject>
#include <QStringList>
#include <vector>
class QSettings;
namespace signalstudio {
struct FormatTemplate { QString id,name;SampleFormat format;bool builtin=false; };
class FormatTemplateStore {
public:
    explicit FormatTemplateStore(QSettings& settings);
    const std::vector<FormatTemplate>& templates() const{return templates_;}
    const QStringList& recent() const{return recent_;}
    QString error() const{return error_;}
    bool add(const QString& name,const SampleFormat& format,QString& error);
    bool importFile(const QString& path,QString& error);
    bool exportFile(const QString& path,QString& error) const;
    void used(const QString& id);
    static bool decode(const QJsonObject&,std::vector<FormatTemplate>&,QString&);
    QJsonObject encode() const;
private:
    void save();
    QSettings& settings_;std::vector<FormatTemplate> templates_;QStringList recent_;QString error_;
};
}
