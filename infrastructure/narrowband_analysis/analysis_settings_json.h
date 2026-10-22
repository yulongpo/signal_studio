#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <cmath>
#include <numbers>
#include "domain/narrowband_analysis_settings.h"

namespace signalstudio {
inline QJsonObject encodeAnalysisSettings(const NarrowbandAnalysisSettings& s) {
  return {{"pointCount",s.pointCount},{"removeDc",s.removeDc},{"normalizePower",s.normalizePower},
    {"constellationAutoScale",s.constellationAutoScale},{"sampleBudget",static_cast<qint64>(s.sampleBudget)},
    {"symbolRateMode",static_cast<int>(s.symbolRateMode)},{"symbolRateHz",s.symbolRateHz},
    {"hypothesis",static_cast<int>(s.hypothesis)},{"constellationStage",static_cast<int>(s.constellationStage)},
    {"eyeStage",static_cast<int>(s.eyeStage)},{"eyePointsPerUi",s.eyePointsPerUi},{"eyeTimingOffsetUi",s.eyeTimingOffsetUi},
    {"matchedFilter",s.matchedFilter},{"timingRecovery",s.timingRecovery},{"carrierRecovery",s.carrierRecovery},
    {"rolloff",s.rolloff},{"filterSpanSymbols",s.filterSpanSymbols},{"cfoMode",static_cast<int>(s.cfoMode)},
    {"manualCfoHz",s.manualCfoHz},{"manualPhaseRadians",s.manualPhaseRadians},
    {"timingLoopBandwidth",s.timingLoopBandwidth},{"carrierLoopBandwidth",s.carrierLoopBandwidth}};
}
inline bool decodeAnalysisSettings(const QJsonValue& value,NarrowbandAnalysisSettings& output,QString& error) {
  if(!value.isObject()){error="analysis 必须是对象。";return false;}
  const auto object=value.toObject();NarrowbandAnalysisSettings s;
  const auto number=[&](const char* key,double& target,double minimum,double maximum,bool integral=false) {
    if(!object.contains(key))return true;
    const auto item=object.value(key);
    if(!item.isDouble() || !std::isfinite(item.toDouble()) || item.toDouble()<minimum || item.toDouble()>maximum || (integral && std::floor(item.toDouble())!=item.toDouble())) {
      error=QString("analysis.%1 类型或范围无效。").arg(key);return false;
    }
    target=item.toDouble();return true;
  };
  const auto integer=[&](const char* key,int& target,int minimum,int maximum) {
    double raw=target;if(!number(key,raw,minimum,maximum,true))return false;target=static_cast<int>(raw);return true;
  };
  const auto boolean=[&](const char* key,bool& target) {
    if(!object.contains(key))return true;
    if(!object.value(key).isBool()){error=QString("analysis.%1 必须是布尔值。").arg(key);return false;}
    target=object.value(key).toBool();return true;
  };
  int rateMode=static_cast<int>(s.symbolRateMode),hypothesis=static_cast<int>(s.hypothesis),stage=static_cast<int>(s.constellationStage),eye=static_cast<int>(s.eyeStage),cfo=static_cast<int>(s.cfoMode);
  double budget=static_cast<double>(s.sampleBudget);
  if(!integer("pointCount",s.pointCount,256,20000) || !number("sampleBudget",budget,256,8*1024*1024,true) ||
     !boolean("removeDc",s.removeDc) || !boolean("normalizePower",s.normalizePower) || !boolean("constellationAutoScale",s.constellationAutoScale) ||
     !integer("symbolRateMode",rateMode,0,2) || !number("symbolRateHz",s.symbolRateHz,0,4e9) || !integer("hypothesis",hypothesis,0,6) ||
     !integer("constellationStage",stage,0,4) || !integer("eyeStage",eye,0,4) || !integer("eyePointsPerUi",s.eyePointsPerUi,4,16) ||
     !number("eyeTimingOffsetUi",s.eyeTimingOffsetUi,-2,2) || !boolean("matchedFilter",s.matchedFilter) ||
     !boolean("timingRecovery",s.timingRecovery) || !boolean("carrierRecovery",s.carrierRecovery) ||
     !number("rolloff",s.rolloff,.05,1) || !integer("filterSpanSymbols",s.filterSpanSymbols,2,32) ||
     !integer("cfoMode",cfo,0,2) || !number("manualCfoHz",s.manualCfoHz,-2e9,2e9) ||
     !number("manualPhaseRadians",s.manualPhaseRadians,-20*std::numbers::pi,20*std::numbers::pi) ||
     !number("timingLoopBandwidth",s.timingLoopBandwidth,.0001,.1) || !number("carrierLoopBandwidth",s.carrierLoopBandwidth,.0001,.1))return false;
  if((s.eyePointsPerUi!=4 && s.eyePointsPerUi!=8 && s.eyePointsPerUi!=16) || (rateMode==1 && !(s.symbolRateHz>0))) {
    error="analysis：绘图密度必须为 4/8/16，手工 Rs 必须为正。";return false;
  }
  s.sampleBudget=static_cast<std::size_t>(budget);s.symbolRateMode=static_cast<SymbolRateMode>(rateMode);s.hypothesis=static_cast<ModulationHypothesis>(hypothesis);
  s.constellationStage=static_cast<AnalysisStage>(stage);s.eyeStage=static_cast<AnalysisStage>(eye);s.cfoMode=static_cast<CfoMode>(cfo);
  output=s;error.clear();return true;
}
}  // namespace signalstudio
