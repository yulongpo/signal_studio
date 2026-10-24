#pragma once
#include "domain/constellation.h"
#include "domain/narrowband_analysis.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <utility>
#include <array>

namespace signalstudio {
// Display projection only: no synchronization, phase search or bit mapping.
struct ConstellationObservation {
    AnalysisStage sourceStage=AnalysisStage::RawIq;
    std::vector<TaggedSymbol> samples, decisions;
    std::vector<std::size_t> templateIndices;
    ConstellationDefinition definition;
    double referenceScale=0;
    std::string message;
    bool decisionAvailable=false;
};
inline ConstellationObservation observeConstellation(const NarrowbandAnalysisResult& result,
    const NarrowbandAnalysisSettings& settings) {
    ConstellationObservation out;
    if(result.status!=AnalysisStatus::Ready || !result.source || result.source->samples.empty()) {
        out.message="没有有效的真实 IQ 快照。";return out;
    }
    const std::vector<TaggedSymbol>* symbols=nullptr;
    const ContinuousAnalysisStage* continuous=nullptr;
    auto selected=settings.constellationStage;
    for(;;) {
        if(selected==AnalysisStage::CarrierRecovered && result.carrierLocked && !result.synchronizedSymbols.empty())symbols=&result.synchronizedSymbols;
        if(selected==AnalysisStage::TimingAligned && result.timingLocked && !result.clockSymbols.empty())symbols=&result.clockSymbols;
        if(selected<AnalysisStage::TimingAligned)for(const auto& stage:result.stages)if(stage.stage==selected)continuous=&stage;
        if(symbols || continuous || selected==AnalysisStage::RawIq)break;
        selected=static_cast<AnalysisStage>(static_cast<int>(selected)-1);
    }
    out.sourceStage=selected;
    const auto size=symbols?symbols->size():continuous?continuous->samples.size():result.source->samples.size();
    const auto count=std::min<std::size_t>(20000,size);
    double power=0;
    // RMS uses the entire selected real stage, independent of display thinning.
    for(std::size_t i=0;i<size;++i) {
        const auto value=symbols?std::complex<double>((*symbols)[i].value):continuous?std::complex<double>(continuous->samples[i]):
            (std::complex<double>(result.source->samples[i])-result.removedDc)*result.gain;
        power+=std::norm(value);
    }
    out.referenceScale=std::sqrt(power/std::max<std::size_t>(1,size));
    for(std::size_t i=0;i<count;++i) {
        const auto n=count>1?i*(size-1)/(count-1):0;
        TaggedSymbol tagged;
        if(symbols)tagged=(*symbols)[n];
        else {
            tagged.value=continuous?continuous->samples[n]:static_cast<std::complex<float>>((std::complex<double>(result.source->samples[n])-result.removedDc)*result.gain);
            tagged.output={continuous?continuous->outputBegin+n:result.source->outputSamples.begin+n,0};
            const auto& source=*result.source;
            if(!source.outputRateHz || !source.sourceRateHz) {out.samples.clear();out.message="实际采样率无效。";return out;}
            const auto quotient=tagged.output.integer/source.outputRateHz,remainder=tagged.output.integer%source.outputRateHz;
            const auto limit=std::numeric_limits<SampleIndex>::max();
            if(quotient>limit/source.sourceRateHz || remainder>limit/source.sourceRateHz) {out.samples.clear();out.message="来源位置溢出。";return out;}
            const auto whole=quotient*source.sourceRateHz,product=remainder*source.sourceRateHz;
            if(product/source.outputRateHz>limit-whole || source.sourceOrigin>limit-whole-product/source.outputRateHz) {out.samples.clear();out.message="来源位置溢出。";return out;}
            tagged.source={source.sourceOrigin+whole+product/source.outputRateHz,static_cast<double>(product%source.outputRateHz)/source.outputRateHz};
        }
        out.samples.push_back(tagged);
    }
    if(!makeConstellation(settings,out.definition,out.message))return out;
    if(!(out.referenceScale>1e-12) || !std::isfinite(out.referenceScale)) {out.message="实际阶段 RMS 无效，参考区域与判决不可用。";out.definition={};return out;}
    if(selected!=AnalysisStage::CarrierRecovered || !result.carrierLocked || !result.timingLocked) {
        out.message="判决不可用：需要实际定时与载波锁定；参考模板仅为用户假设。";return out;
    }
    for(const auto& sample:out.samples) {
        if(!std::isfinite(sample.value.real()) || !std::isfinite(sample.value.imag())) {
            out.decisions.clear();out.templateIndices.clear();out.message="判决不可用：真实样本含非有限值。";return out;
        }
        const auto index=nearestConstellationPoint(std::complex<double>(sample.value)/out.referenceScale,out.definition);
        auto decision=sample;decision.value=static_cast<std::complex<float>>(out.definition.points[index]*out.referenceScale);
        out.decisions.push_back(decision);out.templateIndices.push_back(index);
    }
    out.decisionAvailable=true;
    out.message="条件最近邻判决 · 来源：实际载波恢复符号 · 整段 RMS 对齐模板 · 绝对相位/比特极性未知 · 非识别、非位流";
    return out;
}

struct ConstellationCell {std::size_t templateIndex=0;std::vector<std::complex<double>> polygon;};
// Exact Euclidean Voronoi cells, clipped in template coordinates. GUI callers
// cache by template and view bounds. At M=4096, the square grid uses <=4M
// halfplanes for interior sites and <=4*64*M for boundary sites (<1.07M).
// Cross QAM <=2048 has side<=48. PSK/APSK <=256 use <=65536 halfplanes.
inline std::vector<ConstellationCell> constellationDecisionCells(const ConstellationDefinition& c,double low,double high,
    const std::function<bool()>& cancelled={}) {
    std::vector<ConstellationCell> cells;
    if(!std::isfinite(low) || !std::isfinite(high) || low>=high || c.points.empty())return cells;
    for(std::size_t i=0;i<c.points.size();++i) {
        if(cancelled && cancelled())return {};
        std::vector<std::complex<double>> polygon{{low,low},{high,low},{high,high},{low,high}};
        const auto clip=[&](std::size_t j) {
            if(i==j || polygon.empty())return;
            const auto normal=c.points[j]-c.points[i];
            const double offset=(std::norm(c.points[j])-std::norm(c.points[i]))/2;
            const auto distance=[&](auto p){return std::real(p*std::conj(normal))-offset;};
            std::vector<std::complex<double>> next;next.reserve(polygon.size()+1);
            auto a=polygon.back();double da=distance(a);
            for(const auto b:polygon) {
                const double db=distance(b);
                if((da<=0)!=(db<=0))next.push_back(a+(b-a)*(da/(da-db)));
                if(db<=0)next.push_back(b);
                a=b;da=db;
            }
            polygon=std::move(next);
        };
        bool interior=false;
        if(c.gridX) {
            const int x=static_cast<int>(std::llround((c.points[i].real()*c.gridScale+c.gridX-1)/2));
            const int y=static_cast<int>(std::llround((c.points[i].imag()*c.gridScale+c.gridY-1)/2));
            int neighbors=0;
            for(const auto [dx,dy]:std::array<std::pair<int,int>,4>{{{-1,0},{1,0},{0,-1},{0,1}}}) {
                const int a=x+dx,b=y+dy;
                if(a>=0 && a<c.gridX && b>=0 && b<c.gridY) {
                    const auto j=c.gridIndices[static_cast<std::size_t>(a*c.gridY+b)];
                    if(j>=0){clip(static_cast<std::size_t>(j));++neighbors;}
                }
            }
            // All four immediate lattice neighbors bound a full grid cell;
            // no other lattice site can cut it. Missing-corner and outer sites
            // use every competitor, preserving cross-QAM corner diagonals.
            interior=neighbors==4;
        }
        if(!interior)for(std::size_t j=0;j<c.points.size() && !polygon.empty();++j) {
            if((j&63U)==0 && cancelled && cancelled())return {};
            clip(j);
        }
        if(polygon.size()>=3)cells.push_back({i,std::move(polygon)});
    }
    return cells;
}
} // namespace signalstudio
