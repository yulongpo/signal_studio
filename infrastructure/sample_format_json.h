#pragma once
#include "domain/sample_format.h"
#include <QJsonObject>
#include <QRegularExpression>
#include <stdexcept>
#include <cmath>
#include <QStringList>

namespace signalstudio {
inline QJsonObject encodeSampleFormat(const SampleFormat& f){
    return {{"structure",f.structure==SampleStructure::Real?"real":"complex"},
        {"encoding",QString::fromStdString(formatId(f).substr(1))},
        {"byteOrder",f.byteOrder==ByteOrder::NotApplicable?"na":f.byteOrder==ByteOrder::Little?"le":"be"},
        {"iqLayout",f.iqLayout==IQLayout::NotApplicable?"na":f.iqLayout==IQLayout::IQInterleaved?"iq":f.iqLayout==IQLayout::QIInterleaved?"qi":"planar"},
        {"channels",static_cast<int>(f.channels)},{"selectedChannel",static_cast<int>(f.selectedChannel)},
        {"channelLayout",f.channelLayout==ChannelLayout::TimeInterleaved?"interleaved":"planar"},
        {"headerBytes",QString::number(f.headerBytes)},{"trailerBytes",QString::number(f.trailerBytes)},
        {"normalizeIntegerAdc",f.normalizeIntegerAdc}};
}
inline SampleFormat decodeSampleFormat(const QJsonObject& o){
    const auto fail=[](){throw std::runtime_error("无效的样本格式 JSON");};
    const auto choice=[&](const char* key,const QStringList& values){const auto v=o[key];if(!v.isString())fail();const int n=values.indexOf(v.toString());if(n<0)fail();return n;};
    const auto integer=[&](const char* key,int maximum){const auto v=o[key];const double x=v.toDouble(-1);if(!v.isDouble()||x<0||x>maximum||std::floor(x)!=x)fail();return static_cast<std::uint32_t>(x);};
    const auto index=[&](const char* key){const auto v=o[key];if(!v.isString()||!QRegularExpression("^(0|[1-9][0-9]{0,19})$").match(v.toString()).hasMatch())fail();bool ok=false;const auto n=v.toString().toULongLong(&ok);if(!ok)fail();return n;};
    SampleFormat f;
    f.structure=static_cast<SampleStructure>(choice("structure",{"complex","real"}));
    f.componentEncoding=static_cast<ComponentEncoding>(choice("encoding",{"I8","U8","I16","I32","F32","F64"}));
    f.byteOrder=static_cast<ByteOrder>(choice("byteOrder",{"na","le","be"}));
    f.iqLayout=static_cast<IQLayout>(choice("iqLayout",{"na","iq","qi","planar"}));
    f.channelLayout=static_cast<ChannelLayout>(choice("channelLayout",{"interleaved","planar"}));
    f.channels=integer("channels",128);f.selectedChannel=integer("selectedChannel",127);
    f.headerBytes=index("headerBytes");f.trailerBytes=index("trailerBytes");
    if(!o["normalizeIntegerAdc"].isBool())fail();f.normalizeIntegerAdc=o["normalizeIntegerAdc"].toBool();
    std::string error;if(!validateSampleFormat(f,error))throw std::runtime_error(error);
    return f;
}
}
