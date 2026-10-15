#include "infrastructure/detection_worker_client.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>

#include "infrastructure/detection_store.h"

namespace signalstudio {

DetectionWorkerClient::~DetectionWorkerClient() { stop(); }

void DetectionWorkerClient::stop() {
  if (process_.state() != QProcess::NotRunning) {
    process_.kill();
    process_.waitForFinished(1000);
  }
}

bool DetectionWorkerClient::writePacket(const DetectionWorkerPacket& packet,
                                        QString& error) {
  const auto bytes = encodeWorkerPacket(packet, error);
  if (bytes.isEmpty()) return false;
  if (process_.write(bytes) != bytes.size()) {
    error = "Worker input pipe rejected the bounded packet";
    return false;
  }
  return true;
}

bool DetectionWorkerClient::readPacket(DetectionWorkerPacket& packet,
                                       QString& error,
                                       const std::function<bool()>& cancelled) {
  QElapsedTimer timeout;
  timeout.start();
  while (timeout.elapsed() < 45000) {
    if (cancelled()) {
      error = "Cancelled at worker boundary";
      stop();
      return false;
    }
    std::optional<DetectionWorkerPacket> available;
    if (!decoder_.take(available, error)) return false;
    if (available) {
      packet = std::move(*available);
      return true;
    }
    process_.waitForReadyRead(20);
    const auto bytes = process_.readAllStandardOutput();
    // Diagnostics are separate from the binary wire protocol and bounded.
    const auto diagnostic = process_.readAllStandardError().left(4096);
    if (!bytes.isEmpty() && !decoder_.append(bytes, error)) return false;
    if (process_.state() == QProcess::NotRunning && bytes.isEmpty()) {
      error = "Worker exited before a complete response: " +
              QString::fromUtf8(diagnostic);
      return false;
    }
  }
  error = "Worker response timed out";
  stop();
  return false;
}

bool DetectionWorkerClient::start(const QString& executable,
                                  const DetectionRequest& request,
                                  QString& error,
                                  const std::function<bool()>& cancelled,
                                  std::uint32_t maximumExecutionBatch) {
  if (!QFileInfo(executable).isFile()) {
    error = "Detection worker executable is missing";
    return false;
  }
  request_ = request;
  process_.setProgram(executable);
  process_.setArguments(
      {"--maximum-execution-batch",
       QString::number(maximumExecutionBatch ? maximumExecutionBatch
                                             : request.parameters.batchSize)});
  process_.setProcessChannelMode(QProcess::SeparateChannels);
  process_.start();
  if (!process_.waitForStarted(3000)) {
    error = "Detection worker could not start: " + process_.errorString();
    return false;
  }
  DetectionWorkerPacket hello{
      workerEnvelope("hello", "handshake", 0, {{"client", "SignalStudio"}}),
      {}};
  DetectionWorkerPacket ready;
  if (!writePacket(hello, error) || !readPacket(ready, error, cancelled))
    return false;
  if (ready.envelope["type"] != "ready") {
    error = "Worker protocol handshake failed";
    return false;
  }
  DetectionWorkerPacket begin{
      workerEnvelope("begin", "begin", 0, encodeDetectionRequest(request),
                     QString::fromStdString(request.runId),
                     request.sourceIdentity.projectGeneration,
                     QString::fromStdString(request.sourceIdentity.fileId)),
      {}};
  if (!exchange(std::move(begin), ready, error, cancelled)) return false;
  evidence_ = ready.envelope["payload"].toObject();
  return true;
}

bool DetectionWorkerClient::exchange(DetectionWorkerPacket packet,
                                     DetectionWorkerPacket& response,
                                     QString& error,
                                     const std::function<bool()>& cancelled) {
  packet.envelope["sequence"] = QString::number(++sequence_);
  if (!writePacket(packet, error) || !readPacket(response, error, cancelled))
    return false;
  const auto& envelope = response.envelope;
  if (envelope["requestId"] != packet.envelope["requestId"] ||
      envelope["sequence"] != packet.envelope["sequence"] ||
      envelope["runId"] != packet.envelope["runId"] ||
      envelope["fileId"] != packet.envelope["fileId"] ||
      envelope["projectGeneration"] != packet.envelope["projectGeneration"]) {
    error = "Worker response correlation does not match the request";
    return false;
  }
  if (envelope["type"] == "error") {
    error = envelope["payload"].toObject()["message"].toString(
        "Worker rejected the request");
    return false;
  }
  const auto expected = packet.envelope["type"] == "batch"       ? "result"
                        : packet.envelope["type"] == "completed" ? "completed"
                                                                 : "ready";
  if (envelope["type"] != expected) {
    error = "Unexpected worker response stage";
    return false;
  }
  return true;
}

}  // namespace signalstudio
