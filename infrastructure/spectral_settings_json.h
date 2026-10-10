#pragma once
#include "infrastructure/spectral_analysis.h"
#include <QJsonObject>
#include <stdexcept>
namespace signalstudio {
inline QJsonObject encodeSpectralParameters(const SpectralParameters& p) {
    return {{"method",static_cast<int>(p.method)},{"window",static_cast<int>(p.window)},{"overlap",p.overlap},
        {"segmentMilliseconds",p.segmentMilliseconds},{"kaiserBeta",p.kaiserBeta},{"timeBandwidth",p.timeBandwidth},
        {"tapers",p.tapers},{"burgOrder",p.burgOrder},{"removeMean",p.removeMean}};
}
inline SpectralParameters decodeSpectralParameters(const QJsonValue& value) {
    if(!value.isObject())throw std::runtime_error("谱参数必须是对象");const auto o=value.toObject();
    const auto real=[&](const char* key){if(!o[key].isDouble()||!std::isfinite(o[key].toDouble()))throw std::runtime_error("谱参数缺失或数值无效");return o[key].toDouble();};
    const auto integer=[&](const char* key,int min,int max){const double v=real(key);if(v<min||v>max||std::floor(v)!=v)throw std::runtime_error("谱参数枚举或整数无效");return static_cast<int>(v);};
    SpectralParameters p;p.method=static_cast<SpectralMethod>(integer("method",0,4));p.window=static_cast<SpectralWindow>(integer("window",0,6));
    p.overlap=real("overlap");p.segmentMilliseconds=real("segmentMilliseconds");p.kaiserBeta=real("kaiserBeta");p.timeBandwidth=real("timeBandwidth");
    p.tapers=integer("tapers",1,32);p.burgOrder=integer("burgOrder",1,256);if(!o["removeMean"].isBool())throw std::runtime_error("去均值参数无效");p.removeMean=o["removeMean"].toBool();
    if(!validSpectralParameters(p))throw std::runtime_error("谱参数超出有效范围");return p;
}
inline QJsonObject encodePsdSettings(const PsdSettings& s) {return {{"parameters",encodeSpectralParameters(s.parameters)},{"statistic",static_cast<int>(s.statistic)},{"scope",static_cast<int>(s.scope)}};}
inline PsdSettings decodePsdSettings(const QJsonValue& v) {
    if(!v.isObject())throw std::runtime_error("PSD 参数必须是对象");const auto o=v.toObject();PsdSettings s;s.parameters=decodeSpectralParameters(o["parameters"]);
    const auto enumeration=[&](const char* key){const auto v=o[key];if(!v.isDouble()||v.toDouble()!=v.toInt()||v.toInt()<0||v.toInt()>2)throw std::runtime_error("PSD 统计类型或范围无效");return v.toInt();};
    s.statistic=static_cast<SpectrumStatistic>(enumeration("statistic"));s.scope=static_cast<PsdScope>(enumeration("scope"));return s;
}
}
