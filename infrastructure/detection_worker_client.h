#pragma once

#include <QProcess>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "domain/detection.h"
#include "infrastructure/detection_protocol.h"

namespace signalstudio {

// Parent steady_clock stamp taken when this process observes the event.
// These values are not the worker process clock.
struct DetectionParentClockStamp {
  std::string name;
  std::uint64_t steadyNanoseconds = 0;
};

// One QProcess per client. Callers construct a new client for each run and
// stop() ends that process. This type does not retain a worker, engine, or
// CUDA context for a later run.
class DetectionWorkerClient {
 public:
  ~DetectionWorkerClient();
  bool start(const QString& executable, const DetectionRequest&, QString&,
             const std::function<bool()>& cancelled,
             std::uint32_t maximumExecutionBatch = 0);
  bool exchange(DetectionWorkerPacket, DetectionWorkerPacket&, QString&,
                const std::function<bool()>& cancelled);
  void stop(bool graceful=false);
  const QJsonObject& evidence() const { return evidence_; }
  QJsonObject diagnostics() const;
  const std::vector<DetectionParentClockStamp>& parentClockStamps() const {
    return parentClockStamps_;
  }

 private:
  bool writePacket(const DetectionWorkerPacket&, QString&);
  bool readPacket(DetectionWorkerPacket&, QString&,
                  const std::function<bool()>& cancelled);
  void sampleResources();
  void stampParentClock(const char* name);
  QProcess process_;
  DetectionPacketDecoder decoder_;
  DetectionRequest request_;
  quint64 sequence_ = 0;
  QJsonObject evidence_;
  QByteArray stderrTail_;
  qint64 processId_=0;
  quint64 bytesRead_=0, bytesWritten_=0;
  bool terminationRequested_=false, gracefulRequested_=false;
  QJsonObject resource_;
  std::vector<DetectionParentClockStamp> parentClockStamps_;
};

}  // namespace signalstudio
