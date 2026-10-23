#include "domain/constellation.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <sstream>
namespace signalstudio {
bool isSingleCarrierHypothesis(ModulationHypothesis h) {
    return h==ModulationHypothesis::Bpsk || h==ModulationHypothesis::Qpsk || h==ModulationHypothesis::Qam16 ||
        h==ModulationHypothesis::Psk || h==ModulationHypothesis::Qam || h==ModulationHypothesis::Apsk;
}
std::string defaultApskRings(int m) {
    if(m==16)return "4:1:45,12:2.85:15";
    if(m==32)return "4:1:45,12:2.84:15,16:5.27:0";
    if(m==64)return "4:1:45,12:2.4:15,20:3.7:9,28:5.2:6.428571428571429";
    // Generic concentric analysis assumptions, not a DVB MODCOD declaration.
    if(m==8)return "2:1:0,6:2:0";
    if(m==128)return "8:1:0,16:2:0,24:3:0,32:4:0,48:5:0";
    if(m==256)return "4:1:0,12:2:0,20:3:0,28:4:0,36:5:0,44:6:0,52:7:0,60:8:0";
    return {};
}
bool makeConstellation(const NarrowbandAnalysisSettings& s,ConstellationDefinition& out,std::string& error) {
    out={};error.clear();int m=s.modulationOrder;
    if(s.hypothesis==ModulationHypothesis::Bpsk)m=2;
    if(s.hypothesis==ModulationHypothesis::Qpsk)m=4;
    if(s.hypothesis==ModulationHypothesis::Qam16)m=16;
    const auto fail=[&](const char* reason){out={};error=reason;return false;};
    if(s.apskRings.size()>1024)return fail("APSK 环配置不能超过 1024 字符。");
    if(!isSingleCarrierHypothesis(s.hypothesis))return fail("请选择 PSK、QAM 或 APSK 分析假设。");
    if(m<2 || m>4096 || (m&(m-1)))return fail("调制阶数 M 必须是 2–4096 范围内的 2 次幂。");
    if(s.hypothesis!=ModulationHypothesis::Qam && m>256)return fail("当前 PSK/APSK 支持至 256 阶；QAM 支持至 4096 阶。");
    const double pi=std::numbers::pi;
    if(s.hypothesis==ModulationHypothesis::Bpsk || s.hypothesis==ModulationHypothesis::Qpsk || s.hypothesis==ModulationHypothesis::Psk) {
        out.symmetry=out.momentOrder=m;
        const double phase=m==4?pi/4:0;
        out.pskOrder=m;out.pskPhase=phase;
        for(int i=0;i<m;++i)out.points.push_back(std::polar(1.0,phase+2*pi*i/m));
    } else if(s.hypothesis==ModulationHypothesis::Qam16 || s.hypothesis==ModulationHypothesis::Qam) {
        if(m<4)return fail("QAM 阶数至少为 4。");
        int bits=0;for(int n=m;n>1;n/=2)++bits;
        if(bits%2==0 || bits==3) {
            const int nx=1<<((bits+1)/2),ny=1<<(bits/2);
            out.gridX=nx;out.gridY=ny;
            out.symmetry=nx==ny?4:2;out.momentOrder=nx==ny?4:2;
            for(int x=0;x<nx;++x)for(int y=0;y<ny;++y)out.points.emplace_back(2*x-nx+1,2*y-ny+1);
        } else {
            // Cross-QAM: a 6k square with four k-by-k corners removed.
            const int k=1<<((bits-5)/2),side=6*k;out.symmetry=out.momentOrder=4;
            out.gridX=out.gridY=side;out.gridCorner=k;
            for(int x=0;x<side;++x)for(int y=0;y<side;++y)
                if(!((x<k || x>=side-k) && (y<k || y>=side-k)))out.points.emplace_back(2*x-side+1,2*y-side+1);
        }
    } else {
        const auto text=s.apskRings.empty()?defaultApskRings(m):s.apskRings;
        std::istringstream rings(text);std::string ring;double previous=0;int total=0,symmetry=0,largest=0;
        while(std::getline(rings,ring,',')) {
            std::replace(ring.begin(),ring.end(),':',' ');std::istringstream values(ring);
            int count=0;double radius=0,angle=0;std::string extra;
            if(!(values>>count>>radius>>angle) || (values>>extra) || count<2 || count>256 ||
                !std::isfinite(radius) || radius<=previous || radius>100 || !std::isfinite(angle) || std::abs(angle)>360 || total+count>m)
                return fail("APSK 环需为 点数:半径:角度，各半径递增，点数总和等于 M。");
            for(int i=0;i<count;++i)out.points.push_back(std::polar(radius,angle*pi/180+2*pi*i/count));
            total+=count;previous=radius;symmetry=std::gcd(symmetry,count);largest=count;
        }
        if(total!=m)return fail("APSK 环点数总和必须等于 M。");
        out.symmetry=symmetry;out.momentOrder=largest;
    }
    double power=0;for(const auto p:out.points)power+=std::norm(p);power/=out.points.size();
    if(power<std::numeric_limits<double>::min()) {
        // APSK radii are relative: rescale before squaring when all radii
        // are tiny, so a valid ring ratio cannot underflow to zero power.
        double scale=0;for(const auto p:out.points)scale=std::max({scale,std::abs(p.real()),std::abs(p.imag())});
        if(!(scale>0) || !std::isfinite(scale))return fail("星座半径无法有效归一化。");
        power=0;for(auto& p:out.points){p/=scale;power+=std::norm(p);}power/=out.points.size();
    }
    if(!(power>0) || !std::isfinite(power))return fail("星座平均功率必须为有限正数。");
    out.gridScale=std::sqrt(power);
    if(out.gridX) {
        out.gridIndices.assign(static_cast<std::size_t>(out.gridX*out.gridY),-1);
        for(std::size_t i=0;i<out.points.size();++i) {
            const int x=static_cast<int>(std::llround((out.points[i].real()+out.gridX-1)/2));
            const int y=static_cast<int>(std::llround((out.points[i].imag()+out.gridY-1)/2));
            out.gridIndices[static_cast<std::size_t>(x*out.gridY+y)]=static_cast<int>(i);
        }
    }
    for(auto& p:out.points) {
        p/=out.gridScale;
        if(!std::isfinite(p.real()) || !std::isfinite(p.imag()))return fail("星座归一化必须产生有限点。");
    }
    if(out.pskOrder){out.minimumDistance=2*std::sin(pi/out.pskOrder);return true;}
    if(out.gridX){out.minimumDistance=2/out.gridScale;return true;}
    out.minimumDistance=std::numeric_limits<double>::infinity();
    for(std::size_t i=0;i<out.points.size();++i)for(std::size_t j=0;j<i;++j)out.minimumDistance=std::min(out.minimumDistance,std::abs(out.points[i]-out.points[j]));
    return true;
}
std::size_t nearestConstellationPoint(std::complex<double> z,const ConstellationDefinition& c) {
    if(c.pskOrder) {
        auto i=static_cast<int>(std::llround((std::arg(z)-c.pskPhase)*c.pskOrder/(2*std::numbers::pi)))%c.pskOrder;
        if(i<0)i+=c.pskOrder;return static_cast<std::size_t>(i);
    }
    if(c.gridX) {
        const int x=static_cast<int>(std::llround(std::clamp((z.real()*c.gridScale+c.gridX-1)/2,0.0,static_cast<double>(c.gridX-1))));
        const int y=static_cast<int>(std::llround(std::clamp((z.imag()*c.gridScale+c.gridY-1)/2,0.0,static_cast<double>(c.gridY-1))));
        const auto index=[&](int a,int b){return c.gridIndices[static_cast<std::size_t>(a*c.gridY+b)];};
        if(const auto cell=index(x,y);cell>=0)return static_cast<std::size_t>(cell);
        const int edgeX=std::clamp(x,c.gridCorner,c.gridX-c.gridCorner-1),edgeY=std::clamp(y,c.gridCorner,c.gridY-c.gridCorner-1);
        const auto a=static_cast<std::size_t>(index(edgeX,y)),b=static_cast<std::size_t>(index(x,edgeY));
        return std::norm(z-c.points[a])<=std::norm(z-c.points[b])?a:b;
    }
    std::size_t best=0;double cost=std::numeric_limits<double>::infinity();
    for(std::size_t i=0;i<c.points.size();++i){const double d=std::norm(z-c.points[i]);if(d<cost){cost=d;best=i;}}
    return best;
}
}
