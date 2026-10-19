#include "infrastructure/spectral_analysis.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace signalstudio {
bool validSpectralParameters(const SpectralParameters& p) {
    return static_cast<int>(p.method) >= 0 && static_cast<int>(p.method) <= 4 &&
        static_cast<int>(p.window) >= 0 && static_cast<int>(p.window) <= 6 &&
        std::isfinite(p.overlap) && p.overlap >= 0 && p.overlap < 1 &&
        std::isfinite(p.segmentMilliseconds) && p.segmentMilliseconds >= 0 && p.segmentMilliseconds <= 1e9 &&
        std::isfinite(p.kaiserBeta) && p.kaiserBeta >= 0 && p.kaiserBeta <= 30 &&
        std::isfinite(p.timeBandwidth) && p.timeBandwidth >= .5 && p.timeBandwidth <= 32 &&
        p.tapers >= 1 && p.tapers <= 32 && p.burgOrder >= 1 && p.burgOrder <= 256;
}
std::string spectralParameterKey(const SpectralParameters& p) {
    std::ostringstream s; s << std::setprecision(17) << static_cast<int>(p.method) << '/' << static_cast<int>(p.window)
        << '/' << p.overlap << '/' << p.segmentMilliseconds << '/' << p.kaiserBeta << '/' << p.timeBandwidth
        << '/' << p.tapers << '/' << p.burgOrder << '/' << p.removeMean;
    return s.str();
}
std::vector<double> spectralWindow(SpectralWindow type, int length, double beta) {
    if (length <= 0) return {};
    std::vector<double> w(length, 1);
    if (length == 1) return w;
    const double divisor = std::cyl_bessel_i(0.0, beta);
    for (int n = 0; n < length; ++n) {
        const double a = 2 * std::numbers::pi * n / (length - 1), x = 2.0 * n / (length - 1) - 1;
        switch (type) {
        case SpectralWindow::Rectangular: break;
        case SpectralWindow::Hann: w[n] = .5 - .5 * std::cos(a); break;
        case SpectralWindow::Hamming: w[n] = .54 - .46 * std::cos(a); break;
        case SpectralWindow::Blackman: w[n] = .42 - .5 * std::cos(a) + .08 * std::cos(2*a); break;
        case SpectralWindow::BlackmanHarris: w[n] = .35875 - .48829 * std::cos(a) + .14128 * std::cos(2*a) - .01168 * std::cos(3*a); break;
        case SpectralWindow::FlatTop: w[n] = .21557895 - .41663158 * std::cos(a) + .277263158 * std::cos(2*a) - .083578947 * std::cos(3*a) + .006947368 * std::cos(4*a); break;
        case SpectralWindow::Kaiser: w[n] = std::cyl_bessel_i(0.0, beta * std::sqrt(std::max(0.0, 1-x*x))) / divisor; break;
        }
    }
    return w;
}

// Symmetric DPSS, independently computed from the Slepian tridiagonal operator.
// Sturm bisection plus inverse iteration avoids dense N*N storage.
std::vector<std::vector<double>> dpssWindows(int n, double nw, int count, const SpectralCancel& cancel) {
    if (n < 2 || nw <= 0 || nw >= n*.5 || count < 1 || count > n) return {};
    std::vector<double> d(n), e(n-1);
    const double c = std::cos(2*std::numbers::pi*nw/n);
    double lower = 1e300, upper = -1e300;
    for (int i=0;i<n;++i) {
        const double x=(n-1-2.0*i)*.5; d[i]=x*x*c;
        if (i<n-1) e[i]=(i+1.0)*(n-i-1.0)*.5;
    }
    for (int i=0;i<n;++i) {
        const double radius=(i?e[i-1]:0)+(i+1<n?e[i]:0);
        lower=std::min(lower,d[i]-radius); upper=std::max(upper,d[i]+radius);
    }
    const double scale=std::max(1.0, std::max(std::abs(lower),std::abs(upper)));
    for(auto& x:d) x/=scale; for(auto& x:e) x/=scale; lower/=scale; upper/=scale;
    const auto below=[&](double x) {
        int total=0; double q=d[0]-x; if(q<0)++total;
        for(int i=1;i<n;++i) { if(std::abs(q)<1e-18) q=q<0?-1e-18:1e-18; q=d[i]-x-e[i-1]*e[i-1]/q; if(q<0)++total; }
        return total;
    };
    std::vector<std::vector<double>> result;
    for(int k=0;k<count;++k) {
        double lo=lower-1e-12,hi=upper+1e-12;
        const int target=n-1-k;
        for(int step=0;step<64;++step) {if(cancel&&cancel())return {};const double mid=(lo+hi)*.5; if(below(mid)<=target)lo=mid;else hi=mid;}
        const double eigen=(lo+hi)*.5 + 2e-15;
        std::vector<double> v(n), diagonal(n), right(n);
        for(int i=0;i<n;++i) v[i]=std::sin((i+.5)*(k+1)*std::numbers::pi/n);
        for(int iteration=0;iteration<12;++iteration) {
            if(cancel&&cancel())return {};
            diagonal[0]=d[0]-eigen; right[0]=v[0];
            for(int i=1;i<n;++i) {
                if(std::abs(diagonal[i-1])<1e-18)diagonal[i-1]=std::copysign(1e-18,diagonal[i-1]);
                const double ratio=e[i-1]/diagonal[i-1]; diagonal[i]=d[i]-eigen-ratio*e[i-1]; right[i]=v[i]-ratio*right[i-1];
            }
            if(std::abs(diagonal.back())<1e-18)diagonal.back()=1e-18;
            v.back()=right.back()/diagonal.back();
            for(int i=n-2;i>=0;--i)v[i]=(right[i]-e[i]*v[i+1])/diagonal[i];
            for(const auto& previous:result) {double dot=std::inner_product(v.begin(),v.end(),previous.begin(),0.0);for(int i=0;i<n;++i)v[i]-=dot*previous[i];}
            const double norm=std::sqrt(std::inner_product(v.begin(),v.end(),v.begin(),0.0));
            if(!(norm>0)||!std::isfinite(norm))return {};
            for(auto& x:v)x/=norm;
        }
        // Deterministic sign is useful for reference comparisons (PSD ignores it).
        double sign=k%2 ? v[n/4] : std::accumulate(v.begin(),v.end(),0.0);
        if(sign<0)for(auto& x:v)x=-x;
        result.push_back(std::move(v));
    }
    return result;
}
} // namespace signalstudio
