#pragma once

#include <QString>
#include <cstdint>
#include <string>
#include <vector>

#include "domain/project.h"

class QThread;

namespace signalstudio {

// Snapshot for one diagnostic generation. The card copies it on the UI thread;
// buildDetectionDiagnosticReport does not read Session or widgets.
struct DetectionDiagnosticInput {
  bool hasMetadata = false;
  FileMetadata metadata;
  std::uint64_t projectGeneration = 0;
  double sampleRateHz = 0;
  std::uint64_t availableSamples = 0;
  DetectionRequest request;
  QString projectDirectory;
  bool fixturesEnabled = false;
  QString draftIssue;
  QString sourceIssue;
  QString configurationIssue;
  QString rangeIssue;
  QString profileIssue;
  bool hasRun = false;
  bool includeCoverage = false;
  bool hasCoverage = false;
  std::string runId;
  DetectionSourceValidity sourceValidity = DetectionSourceValidity::Current;
  std::string runError;
  std::uint64_t readFrames = 0;
  std::uint64_t refinedCandidates = 0;
  std::uint64_t totalRefinementCandidates = 0;
  double runSampleRateHz = 0;
  std::vector<DetectionCoverage> coverage;
};

struct DetectionDiagnosticReport {
  QString diagnostics;
  QString preflightDetail;
  QString calculationDetails;
  QString physicalTimes;
  QString blockingIssue;
  bool planInvoked = false;
  // True only when a non-null scheduling thread differs from the thread that
  // actually built this report. Direct callers pass a null scheduling thread.
  bool ranOffCallerThread = false;
};

// ASSERT: the source plan is off the scheduling thread only when both pointers
// are set and different. Tests can call this without a DetectionCard.
bool detectionSourcePlanRunsOffCallerThread(const QThread* schedulingThread,
                                            const QThread* workerThread);

// Pure diagnostic text. Calls makeDetectionSourcePlan only for a direct call
// (null scheduling thread) or when detectionSourcePlanRunsOffCallerThread is
// true. DetectionCard::refresh / refreshStatus / refreshTiming do not call it.
DetectionDiagnosticReport buildDetectionDiagnosticReport(
    const DetectionDiagnosticInput& input, const QThread* schedulingThread);

}  // namespace signalstudio
