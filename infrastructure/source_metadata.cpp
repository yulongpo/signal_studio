#include "infrastructure/source_metadata.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

namespace signalstudio {
SourceSuggestions suggestSourceMetadata(const QString& path){
    SourceSuggestions s;s.dataPath=QFileInfo(path).absoluteFilePath();const auto name=QFileInfo(path).fileName();
    const auto value=[&](const QString& key)->std::optional<double>{const auto m=QRegularExpression("(?:^|_)"+key+"([0-9]+(?:\\.[0-9]+)?)([kmg]?(?:sps|hz))",QRegularExpression::CaseInsensitiveOption).match(name);if(!m.hasMatch())return {};bool ok=false;double v=m.captured(1).toDouble(&ok);const auto unit=m.captured(2).toLower();if(unit.startsWith('k'))v*=1e3;else if(unit.startsWith('m'))v*=1e6;else if(unit.startsWith('g'))v*=1e9;return ok&&std::isfinite(v)&&v>=0?std::optional<double>{v}:std::optional<double>{};};
    s.sampleRateHz=value("FS");s.centerFrequencyHz=value("FC");s.bandwidthHz=value("BW");
    const auto m=QRegularExpression("(?:^|[_.])(C|R)(I8|U8|I16|I32|F32|F64)(?:[_.]|$)",QRegularExpression::CaseInsensitiveOption).match(name);
    if(m.hasMatch()){SampleFormat f;f.structure=m.captured(1).toUpper()=="R"?SampleStructure::Real:SampleStructure::ComplexIQ;
        const QStringList encodings{"I8","U8","I16","I32","F32","F64"};f.componentEncoding=static_cast<ComponentEncoding>(encodings.indexOf(m.captured(2).toUpper()));
        f.iqLayout=f.structure==SampleStructure::Real?IQLayout::NotApplicable:IQLayout::IQInterleaved;f.byteOrder=bytesPerComponent(f)==1?ByteOrder::NotApplicable:ByteOrder::Little;
        f.normalizeIntegerAdc=f.componentEncoding<ComponentEncoding::Float32;s.format=f;
    }
    s.provenance=QString("Fs：%1 · Fc：%2 · 格式：%3").arg(s.sampleRateHz?"文件名":"需手动确认",s.centerFrequencyHz?"文件名":"需手动确认",s.format?"文件名建议":"未识别，需确认");return s;
}
}
