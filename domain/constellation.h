#pragma once
#include "domain/narrowband_analysis_settings.h"
#include <complex>
#include <vector>
namespace signalstudio {
struct ConstellationDefinition {
    std::vector<std::complex<double>> points;
    int symmetry=4, momentOrder=4;
    double minimumDistance=0;
    int pskOrder=0,gridX=0,gridY=0,gridCorner=0;
    double pskPhase=0,gridScale=1;
    std::vector<int> gridIndices;
};
bool makeConstellation(const NarrowbandAnalysisSettings&,ConstellationDefinition&,std::string& error);
std::size_t nearestConstellationPoint(std::complex<double>,const ConstellationDefinition&);
bool isSingleCarrierHypothesis(ModulationHypothesis);
std::string defaultApskRings(int order);
}
