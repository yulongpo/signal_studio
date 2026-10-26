#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
#include "tools/detection_benchmark/quality.h"
using namespace signalstudio;
using namespace signalstudio::benchmark;
namespace {
void check(bool okay,const char* error){if(!okay) throw std::runtime_error(error);}
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  try {
    // The first prediction can match either truth; the second can match only
    // the first. A greedy pairing would incorrectly report one missing object.
    const auto assignment=matchBoxes({{{0,100},{0,20}},{{0,100},{1,9}}},
        {{{0,100},{0,10}},{{0,100},{10,20}}},true,1000);
    check(assignment["tp"].toInt()==2 && assignment["f1"].toDouble()==1,"non-greedy matching lost a valid one-to-one assignment");
    const auto duplicate=matchBoxes({{{0,100},{0,10}},{{0,100},{0,10}}},
        {{{0,100},{0,10}}},true,1000);
    check(duplicate["tp"].toInt()==1 && duplicate["fp"].toInt()==1,"duplicate prediction counted twice");
    const auto unrelatedTime=matchBoxes({{{200,300},{0,10}}},{{{0,100},{0,10}}},true,1000);
    check(unrelatedTime["tp"].toInt()==0,"disjoint activity events matched");
    check(matchBoxes({{{200,300},{0,10}}},{{{0,100},{0,10}}},false,1000)["tp"].toInt()==1,
        "frequency-only matching incorrectly required time");
    constexpr SampleIndex origin=(SampleIndex{1}<<54);
    const auto late=matchBoxes({{{origin+1,origin+101},{0,10}}},
        {{{origin,origin+100},{0,10}}},true,1000);
    check(std::abs(late["matchedTimeBoundaryMeanSeconds"].toDouble()-.001)<1e-12,
        "sample coordinate precision was lost before subtraction");
    std::vector<QualityBox> objects,events;
    Annotation annotation;
    annotation.observation=ViewRange{{0,1000},{-20,20}};
    collectQualityBoxes(annotation,{{100,200},{-5,5}},objects,events,true);
    check(objects.size()==1 && events.empty() && objects[0].frequency==FrequencyRange{-5,5},
        "unknown-time observation manufactured an activity event or escaped request bounds");
    objects.clear();events.clear();
    Segment segment;segment.range={{50,150},{-20,0}};annotation.segments.push_back(segment);
    collectQualityBoxes(annotation,{{100,200},{-5,5}},objects,events,true);
    check(events.size()==1 && events[0].time==TimeRange{100,150} &&
        events[0].frequency==FrequencyRange{-5,0},"physical truth clipping is wrong");
    check(!matchBoxes(std::vector<QualityBox>(4097),{},true,1000)["available"].toBool(),
        "metric capacity was silently truncated");
    std::cout<<"PASS benchmark physical one-to-one quality and observation/event separation\n";
    return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
