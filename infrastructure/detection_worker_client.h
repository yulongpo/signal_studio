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
  void stop();
  const QJsonObject& evidence() const { return evidence_; }

 private:
  bool writePacket(const DetectionWorkerPacket&, QString&);
  bool readPacket(DetectionWorkerPacket&, QString&,
                  const std::function<bool()>& cancelled);
  QProcess process_;
  DetectionPacketDecoder decoder_;
  DetectionRequest request_;
  quint64 sequence_ = 0;
  QJsonObject evidence_;
};

}  // namespace signalstudio
