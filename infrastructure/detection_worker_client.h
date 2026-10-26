#pragma once

#include <QProcess>
#include <functional>

#include "domain/detection.h"
#include "infrastructure/detection_protocol.h"

namespace signalstudio {

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

 private:
  bool writePacket(const DetectionWorkerPacket&, QString&);
  bool readPacket(DetectionWorkerPacket&, QString&,
                  const std::function<bool()>& cancelled);
  void sampleResources();
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
};

}  // namespace signalstudio
