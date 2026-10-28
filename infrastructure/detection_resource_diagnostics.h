#pragma once
#include "domain/detection.h"

namespace signalstudio {
// Optional, bounded profile diagnostics. JSON stays in the Qt infrastructure
// layer; domain resource arithmetic and probes have no Qt dependency.
bool mergeRuntimeResourceDiagnostics(const std::string& performanceJson,
                                     const RuntimeResourceSnapshot& snapshot,
                                     const DiskLedgerEvaluation* ledger,
                                     const ResourceDecision* sampleDecision,
                                     std::string& merged, std::string& error);
}
