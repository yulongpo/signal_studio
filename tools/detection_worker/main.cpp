#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "infrastructure/detection_protocol.h"
#include "infrastructure/detection_store.h"

namespace signalstudio {
namespace {
bool readExact(char* destination, std::size_t count) {
  while (count) {
    const auto read = std::fread(destination, 1, count, stdin);
    if (!read) return false;
    destination += read;
    count -= read;
  }
  return true;
}

bool readPacket(DetectionWorkerPacket& packet, QString& error) {
  QByteArray bytes(4, '\0');
  if (!readExact(bytes.data(), 4)) return false;
  const auto controlSize = qFromLittleEndian<quint32>(bytes.constData());
  if (!controlSize || controlSize > 65536) return false;
  bytes.resize(4 + controlSize);
  if (!readExact(bytes.data() + 4, controlSize)) return false;
  const auto json = QJsonDocument::fromJson(bytes.mid(4));
  if (!json.isObject()) return false;
  const auto binary = json.object()["binary"].toObject();
  const auto size = binary["byteLength"].toDouble(0);
  if (size < 0 || size > 33554432 || size != std::floor(size)) return false;
  const auto previous = bytes.size();
  bytes.resize(previous + static_cast<qsizetype>(size));
  if (!readExact(bytes.data() + previous, static_cast<std::size_t>(size)))
    return false;
  DetectionPacketDecoder decoder;
  std::optional<DetectionWorkerPacket> decoded;
  if (!decoder.append(bytes, error) || !decoder.take(decoded, error) ||
      !decoded)
    return false;
  packet = std::move(*decoded);
  return true;
}

DetectionWorkerPacket response(const DetectionWorkerPacket& request,
                               const QString& type, QJsonObject payload) {
  auto envelope = request.envelope;
  envelope.remove("binary");
  envelope["type"] = type;
  envelope["payload"] = std::move(payload);
  return {envelope, {}};
}

bool fixtureBatch(const DetectionWorkerPacket& packet,
                  const DetectionRequest& request, QJsonObject& payload,
                  QString& error) {
  const auto windows =
      packet.envelope["payload"].toObject()["windows"].toArray();
  const auto shape = packet.envelope["binary"].toObject()["shape"].toArray();
  if (windows.isEmpty() || windows.size() > 2 || shape.size() != 4 ||
      shape[0].toInt() != windows.size() || shape[1] != 1 ||
      shape[2].toInt() != static_cast<int>(request.profile.modelFrames) ||
      shape[3].toInt() != static_cast<int>(request.profile.fftSize)) {
    error = "Fixture tensor shape does not match the frozen request";
    return false;
  }
  std::vector<float> input;
  if (!decodeFloat32Payload(packet, input, error)) return false;
  QJsonArray results;
  const auto bins = request.profile.fftSize;
  const auto frames = request.profile.modelFrames;
  for (qsizetype batch = 0; batch < windows.size(); ++batch) {
    quint64 windowId = 0;
    if (!workerUnsigned(windows[batch].toObject()["windowId"], windowId, error))
      return false;
    double best = 0;
    std::uint32_t bestBin = 0;
    for (std::uint32_t bin = 0; bin < bins; ++bin) {
      double mean = 0;
      for (std::uint32_t frame = 0; frame < frames; ++frame)
        mean += input[static_cast<std::size_t>(batch) * frames * bins +
                      static_cast<std::size_t>(frame) * bins + bin] /
                frames;
      if (mean > best) {
        best = mean;
        bestBin = bin;
      }
    }
    QJsonArray candidates;
    // Deliberately simple synthetic fixture. It is not a model or a measured
    // detector and is never selected by a production profile.
    if (best > 3)
      candidates.append(QJsonObject{
          {"lowerBin", std::max(0.0, static_cast<double>(bestBin) - 3)},
          {"upperBin", std::min(static_cast<double>(bins - 1),
                                static_cast<double>(bestBin) + 3)},
          {"score", .95}});
    results.append(QJsonObject{{"windowId", QString::number(windowId)},
                               {"candidates", candidates}});
  }
  payload = {{"windows", results}, {"backend", "pipeline-fixture-v1"}};
  return true;
}
}  // namespace
}  // namespace signalstudio

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  using namespace signalstudio;
  DetectionRequest request;
  bool begun = false;
  quint64 lastSequence = 0;
  for (;;) {
    DetectionWorkerPacket packet;
    QString error;
    if (!readPacket(packet, error)) return std::feof(stdin) ? 0 : 2;
    const auto type = packet.envelope["type"].toString();
    quint64 sequence = 0;
    if (!workerUnsigned(packet.envelope["sequence"], sequence, error)) return 2;
    QJsonObject payload;
    QString resultType;
    if (begun && (type == "hello" || type == "begin")) {
      resultType = "error";
      error = "Worker cannot replace an active immutable request";
    } else if (type == "hello") {
      resultType = "ready";
      payload = {{"workerVersion", 1}, {"backend", "fixture-only"}};
    } else if (type == "begin") {
      begun = decodeDetectionRequest(packet.envelope["payload"].toObject(),
                                     request, error);
      if (begun &&
          (sequence != 1 ||
           packet.envelope["runId"] != QString::fromStdString(request.runId) ||
           packet.envelope["fileId"] !=
               QString::fromStdString(request.sourceIdentity.fileId) ||
           packet.envelope["projectGeneration"] !=
               QString::number(request.sourceIdentity.projectGeneration))) {
        begun = false;
        error = "Worker begin identity does not match its immutable request";
      }
      lastSequence = sequence;
      if (begun && request.profile.backend != DetectionBackendKind::Fixture) {
        begun = false;
        error = "本节点尚未接入真实模型资产，请完成模型部署预检后使用该配置";
      }
      resultType = begun ? "ready" : "error";
    } else if (begun &&
               (sequence != lastSequence + 1 ||
                packet.envelope["runId"] !=
                    QString::fromStdString(request.runId) ||
                packet.envelope["fileId"] !=
                    QString::fromStdString(request.sourceIdentity.fileId) ||
                packet.envelope["projectGeneration"] !=
                    QString::number(
                        request.sourceIdentity.projectGeneration))) {
      resultType = "error";
      error = "Worker run identity or sequence differs from the begun request";
    } else if (type == "batch" && begun) {
      lastSequence = sequence;
      resultType =
          fixtureBatch(packet, request, payload, error) ? "result" : "error";
    } else if ((type == "completed" || type == "cancel") && begun) {
      lastSequence = sequence;
      resultType = "completed";
      begun = false;
    } else {
      resultType = "error";
      error = "Unexpected worker request stage";
    }
    if (!error.isEmpty()) payload = {{"message", error}};
    const auto encoded =
        encodeWorkerPacket(response(packet, resultType, payload), error);
    if (encoded.isEmpty() ||
        std::fwrite(encoded.constData(), 1, encoded.size(), stdout) !=
            static_cast<std::size_t>(encoded.size()) ||
        std::fflush(stdout))
      return 3;
  }
}
