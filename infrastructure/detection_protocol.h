#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>
#include <vector>

namespace signalstudio {

struct DetectionWorkerPacket {
  QJsonObject envelope;
  QByteArray binary;
};

// Frozen v1 wire format: uint32 LE JSON length, bounded UTF-8 JSON, then
// the exact declared binary payload. No filesystem paths or scripts are
// executed.
bool validateWorkerPacket(const DetectionWorkerPacket&, QString& error);
QByteArray encodeWorkerPacket(const DetectionWorkerPacket&, QString& error);
QJsonObject workerEnvelope(const QString& type, const QString& requestId,
                           quint64 sequence, const QJsonObject& payload,
                           const QString& runId = {}, quint64 generation = 0,
                           const QString& fileId = {});
void attachFloat32Payload(DetectionWorkerPacket&, const std::vector<float>&,
                          const std::vector<int>& shape);
bool decodeFloat32Payload(const DetectionWorkerPacket&, std::vector<float>&,
                          QString& error);
bool workerUnsigned(const QJsonValue&, quint64& target, QString& error);

class DetectionPacketDecoder {
 public:
  bool append(const QByteArray&, QString& error);
  bool take(std::optional<DetectionWorkerPacket>&, QString& error);
  qsizetype bufferedBytes() const { return buffer_.size(); }
  void clear() { buffer_.clear(); }

 private:
  QByteArray buffer_;
};

}  // namespace signalstudio
