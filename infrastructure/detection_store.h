#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <vector>

#include "domain/detection.h"

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

}  // namespace signalstudio
