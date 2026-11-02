#pragma once

#include <QJsonObject>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <vector>

#include "domain/detection_postprocess.h"
#include "infrastructure/label_import.h"
#include "infrastructure/detection_task.h"

namespace signalstudio::benchmark {
struct QualityBox {TimeRange time;FrequencyRange frequency;};
inline void collectQualityBoxes(const Annotation& annotation,const ViewRange& requested,
    std::vector<QualityBox>& objects,std::vector<QualityBox>& events,bool acceptObservation) {
  bool hasTime=false;
  FrequencyRange frequency{};
  for(const auto& segment:annotation.segments) {
    const auto begin=std::max(segment.range.time.begin,requested.time.begin);
    const auto end=std::min(segment.range.time.end,requested.time.end);
    const FrequencyRange band{std::max(segment.range.frequency.lowerHz,requested.frequency.lowerHz),
        std::min(segment.range.frequency.upperHz,requested.frequency.upperHz)};
    if(begin>=end || band.lowerHz>=band.upperHz) continue;
    events.push_back({{begin,end},band});
    if(!hasTime) frequency=band;
    else {frequency.lowerHz=std::min(frequency.lowerHz,band.lowerHz);frequency.upperHz=std::max(frequency.upperHz,band.upperHz);}
    hasTime=true;
  }
  if(!hasTime && acceptObservation && annotation.observation) {
    frequency={std::max(annotation.observation->frequency.lowerHz,requested.frequency.lowerHz),
        std::min(annotation.observation->frequency.upperHz,requested.frequency.upperHz)};
    hasTime=frequency.lowerHz<frequency.upperHz;
  }
  if(hasTime) objects.push_back({requested.time,frequency});
}
inline QJsonObject matchBoxes(std::vector<QualityBox> predicted,std::vector<QualityBox> truth,
                              bool timeRequired,double sampleRate) {
  if(predicted.size()>4096 || truth.size()>4096 ||
      (!predicted.empty() && truth.size()>1000000/predicted.size()))
    return {{"available",false},{"error","Exact benchmark matching exceeds bounded pair capacity; no truncation"}};
  const auto less=[](const auto& a,const auto& b) {
    if(a.frequency.lowerHz!=b.frequency.lowerHz) return a.frequency.lowerHz<b.frequency.lowerHz;
    if(a.time.begin!=b.time.begin) return a.time.begin<b.time.begin;
    return a.time.end<b.time.end;
  };
  std::sort(predicted.begin(),predicted.end(),less);std::sort(truth.begin(),truth.end(),less);
  std::vector<std::vector<int>> edges(predicted.size());
  for(std::size_t p=0;p<predicted.size();++p)
    for(std::size_t t=0;t<truth.size();++t) {
      const auto frequency=frequencyIntersectionOverUnion(predicted[p].frequency,truth[t].frequency);
      if(frequency<.3) continue;
      const auto begin=std::max(predicted[p].time.begin,truth[t].time.begin);
      const auto end=std::min(predicted[p].time.end,truth[t].time.end);
      const auto unionBegin=std::min(predicted[p].time.begin,truth[t].time.begin);
      const auto unionEnd=std::max(predicted[p].time.end,truth[t].time.end);
      if(timeRequired && (end<=begin || double(end-begin)/double(unionEnd-unionBegin)<.5)) continue;
      edges[p].push_back(int(t));
    }
  std::vector<int> owner(truth.size(),-1);
  const auto augment=[&](auto&& self,int p,std::vector<bool>& seen)->bool {
    for(auto t:edges[p]) {
      if(seen[t]) continue;seen[t]=true;
      if(owner[t]<0 || self(self,owner[t],seen)) {owner[t]=p;return true;}
    }
    return false;
  };
  std::size_t tp=0;
  for(std::size_t p=0;p<predicted.size();++p) {std::vector<bool> seen(truth.size());tp+=augment(augment,int(p),seen);}
  const double precision=predicted.empty() ? (truth.empty()?1:0) : double(tp)/predicted.size();
  const double recall=truth.empty() ? (predicted.empty()?1:0) : double(tp)/truth.size();
  double boundarySum=0,boundaryMax=0,frequencySum=0,frequencyMax=0;
  for(std::size_t t=0;t<owner.size();++t) if(owner[t]>=0) {
    const auto& p=predicted[owner[t]];const auto& actual=truth[t];
    const auto difference=[](SampleIndex a,SampleIndex b){return double(a>b?a-b:b-a);};
    for(const auto delta:{difference(p.time.begin,actual.time.begin)/sampleRate,
        difference(p.time.end,actual.time.end)/sampleRate}) {
      boundarySum+=delta;boundaryMax=std::max(boundaryMax,delta);
    }
    for(const auto delta:{std::abs(p.frequency.lowerHz-actual.frequency.lowerHz),
        std::abs(p.frequency.upperHz-actual.frequency.upperHz)}) {
      frequencySum+=delta;frequencyMax=std::max(frequencyMax,delta);
    }
  }
  return {{"available",true},{"predicted",double(predicted.size())},{"truth",double(truth.size())},
      {"tp",double(tp)},{"fp",double(predicted.size()-tp)},{"fn",double(truth.size()-tp)},
      {"precision",precision},{"recall",recall},{"f1",precision+recall==0 ? 0 : 2*precision*recall/(precision+recall)},
      {"frequencyIouThreshold",.3},{"timeIouThreshold",timeRequired?.5:0},
      {"matchedTimeBoundaryMeanSeconds",timeRequired && tp ? boundarySum/(2*tp):0},
      {"matchedTimeBoundaryMaximumSeconds",timeRequired?boundaryMax:0},
      {"matchedFrequencyBoundaryMeanHz",tp?frequencySum/(2*tp):0},
      {"matchedFrequencyBoundaryMaximumHz",frequencyMax},
      {"matching","maximum-cardinality bipartite; geometry order; no class prediction assumed"}};
}
inline QJsonObject measureQuality(const QString& labels,const FileMetadata& metadata,
                                  const DetectionRequest& request,const DetectionPublication& publication) {
  const auto imported=importLabelSource(labels,metadata,request.sourceIdentity.projectGeneration);
  if(!imported) return {{"available",false},{"error",imported.error}};
  std::vector<QualityBox> trueObjects,trueEvents,predObjects,predEvents;
  for(const auto& annotation:imported.annotations)
    collectQualityBoxes(annotation,request.requestedRange,trueObjects,trueEvents,false);
  for(const auto& annotation:publication.annotations)
    collectQualityBoxes(annotation,request.requestedRange,predObjects,predEvents,true);
  const auto frequency=matchBoxes(predObjects,trueObjects,false,metadata.sampleRateHz);
  const auto events=matchBoxes(predEvents,trueEvents,true,metadata.sampleRateHz);
  return {{"available",frequency["available"].toBool() && events["available"].toBool()},
      {"labelPath",QFileInfo(labels).absoluteFilePath()},
      {"parser",QString::fromStdString(imported.sourceSet.formatId)},
      {"frequencyOnly",frequency},{"activityEvents",events},
      {"boundary","physical bounds clipped to request; frequency-only observations are never activity events; not modulation or field-RF accuracy"}};
}
}
