#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

#include "domain/project.h"
#include "infrastructure/detection_protocol.h"

namespace signalstudio {

QJsonObject encodeDetectionRequest(const DetectionRequest& request);
bool decodeDetectionRequest(const QJsonObject& object,
                            DetectionRequest& request, QString& error);
QJsonObject encodeDetectionRun(const DetectionRun& run);
bool decodeDetectionRun(const QJsonObject& object, DetectionRun& run,
                        QString& error);

// Only run manifests are published here. The caller owns project.json and
// explicitly decides whether recovered results belong in the project index.
bool writeDetectionRun(const QString& projectDirectory, const DetectionRun& run,
                       QString& error);
bool appendDetectionChunk(const QString& projectDirectory, DetectionRun& run,
                          const QJsonArray& results, const QJsonArray& mappings,
                          QString& error);
bool storeDetectionHeadEvidence(const QString& projectDirectory,
                                const DetectionRequest&,
                                const DetectionWorkerPacket&,
                                QJsonArray& mappings, QString& error);
bool loadDetectionRun(const QString& projectDirectory, const QString& runId,
                      DetectionRun& run, QString& error,
                      bool verifyChunks = true);
bool readDetectionChunks(const QString& projectDirectory,
                         const DetectionRun& run, QJsonArray& chunks,
                         QString& error);
// Validates and visits one bounded shard at a time. References are valid only
// during the callback; returning false leaves the caller's state unchanged.
bool visitDetectionChunks(
    const QString& projectDirectory, const DetectionRun& run,
    const std::function<bool(const QJsonObject&, QString&)>& visitor,
    QString& error);
std::vector<DetectionRun> discoverDetectionRuns(const QString& projectDirectory,
                                                QString& error);
// Production recovery only follows committed/pending IDs, never old directory
// entries. In particular, a retired/corrupt orphan cannot poison discovery.
std::vector<DetectionRun> discoverDetectionRuns(const QString& projectDirectory,
                                               const Project&, QString& error);
bool detectionRunAssetPaths(const QString& projectDirectory, const DetectionRun&,
                            QStringList& paths, QString& error);

// Called by ProjectFileCodec before exposing an external Reference. A root is
// private only when first created at its owning .ssproj's canonical location;
// once referenced/shared it can never be reclaimed as private automatically.
bool recordDetectionAssetOwnership(const QString& ssprojPath,
                                   const QString& projectId,
                                   const QString& assetRoot,
                                   bool newlyCreated, bool reference,
                                   QString& error,
                                   bool rootLockHeld = false);
struct DetectionGcReport {
  std::uint32_t removedFiles = 0, removedRuns = 0, deferredRuns = 0;
  bool privateRoot = false;
};
// Intended for a background worker. Reads the committed .ssproj, locks the
// private root, and visits only explicit retired IDs. No directory-tree scan.
// Oversized runs stay deferred; no partial budget truncation deletes a run.
bool collectRetiredDetectionAssets(const QString& ssprojPath,
                                   DetectionGcReport&, QString& error,
                                   std::uint32_t maximumRuns = 4,
                                   std::uint32_t maximumFiles = 256);

}  // namespace signalstudio
