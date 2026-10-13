#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "domain/detection_postprocess.h"
#include "infrastructure/detection_model_backend.h"
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

bool modelBatch(const DetectionWorkerPacket& packet,
                const DetectionRequest& request, DetectionModelBackend& backend,
                QJsonObject& payload, std::vector<float>& rawHeads,
                std::vector<int>& rawShape, QString& error) {
  const auto windows =
      packet.envelope["payload"].toObject()["windows"].toArray();
  const auto shape = packet.envelope["binary"].toObject()["shape"].toArray();
  if (windows.isEmpty() || windows.size() > request.parameters.batchSize ||
      shape.size() != 4 || shape[0].toInt() != windows.size() ||
      shape[1] != 1 ||
      shape[2].toInt() != static_cast<int>(request.profile.modelFrames) ||
      shape[3].toInt() != static_cast<int>(request.profile.fftSize)) {
    error = "Production tensor shape does not match the immutable request";
    return false;
  }
  std::vector<quint64> windowIds;
  for (const auto& value : windows) {
    quint64 id = 0;
    if (!workerUnsigned(value.toObject()["windowId"], id, error)) return false;
    if (std::find(windowIds.begin(), windowIds.end(), id) != windowIds.end()) {
      error = "Production batch repeats a window identity";
      return false;
    }
    windowIds.push_back(id);
  }
  std::vector<float> input;
  DetectionModelOutput output;
  if (!decodeFloat32Payload(packet, input, error) ||
      !backend.infer(input, static_cast<std::uint32_t>(windows.size()), output,
                     error))
    return false;
  const bool scn = request.profile.backend == DetectionBackendKind::Scn;
  DetectionDecoderConfig config;
  config.kind = scn ? DetectionDecoderKind::ScnThreeBin
                    : DetectionDecoderKind::FfscnSevenBin;
  config.inputBins = request.profile.fftSize;
  config.scoreThreshold = request.parameters.scoreThreshold;
  config.nmsIou = request.parameters.nmsIou;
  config.topK = request.parameters.topK;
  QJsonArray results;
  QJsonArray summaries;
  std::vector<float> packed;
  packed.reserve(static_cast<std::size_t>(output.batch) * output.width * 3);
  for (std::size_t batch = 0; batch < output.batch; ++batch) {
    const auto start = batch * output.width;
    const HeadView heads{
        std::span<const float>(output.hm).subspan(start, output.width),
        std::span<const float>(output.bw).subspan(start, output.width),
        std::span<const float>(output.off).subspan(start, output.width),
        scn ? std::array<std::size_t, 3>{output.width, 1, 1}
            : std::array<std::size_t, 3>{1, 1, output.width}};
    std::vector<DecodedCandidate> decoded;
    DetectionDecodeSummary summary;
    std::string decodeError;
    if (!decodeDetectionHeads(heads, config, decoded, summary, decodeError)) {
      error = QString::fromStdString(decodeError);
      return false;
    }
    const auto windowId = QString::number(windowIds[batch]);
    // The full candidate projection is reconstructed from the verified raw
    // heads by the client. This keeps B2/TopK512 inside the frozen 64-KiB JSON
    // budget without dropping candidates or changing inference parameters.
    results.append(
        QJsonObject{{"windowId", windowId}, {"candidates", QJsonArray{}}});
    summaries.append(QJsonObject{
        {"windowId", windowId},
        {"qualifyingPeaks", static_cast<int>(summary.qualifyingPeaks)},
        {"rankedPeaks", static_cast<int>(summary.rankedPeaks)},
        {"invalidWidthCount", static_cast<int>(summary.invalidWidthCount)},
        {"suppressedCount", static_cast<int>(summary.suppressedCount)},
        {"candidateCount", static_cast<int>(decoded.size())},
        {"topKTruncated", summary.topKTruncated}});
    for (const auto head : {heads.heatmap, heads.bandwidth, heads.offset})
      packed.insert(packed.end(), head.begin(), head.end());
  }
  payload = {{"windows", results},
             {"candidateEncoding", "raw-heads-v1"},
             {"modelEvidence", backend.evidence()},
             {"decodeSummary", summaries},
             {"headOrder", QJsonArray{"hm", "bw", "off"}},
             {"backend", scn ? "scn-maximum-tensorrt" : "ffscn-tensorrt"}};
  rawShape = {static_cast<int>(output.batch), 3, 1,
              static_cast<int>(output.width)};
  rawHeads = std::move(packed);
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
  DetectionModelBackend modelBackend;
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
    std::vector<float> rawHeads;
    std::vector<int> rawShape;
    QString resultType;
    if (begun && (type == "hello" || type == "begin")) {
      resultType = "error";
      error = "Worker cannot replace an active immutable request";
    } else if (type == "hello") {
      resultType = "ready";
      payload = {{"workerVersion", 1}, {"backend", "isolated-native-worker"}};
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
        begun = modelBackend.load(request, error);
        if (begun) payload = modelBackend.evidence();
      } else if (begun) {
        payload = {{"backend", "pipeline-fixture-v1"}};
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
      const bool valid =
          request.profile.backend == DetectionBackendKind::Fixture
              ? fixtureBatch(packet, request, payload, error)
              : modelBatch(packet, request, modelBackend, payload, rawHeads,
                           rawShape, error);
      resultType = valid ? "result" : "error";
    } else if ((type == "completed" || type == "cancel") && begun) {
      lastSequence = sequence;
      resultType = "completed";
      begun = false;
      modelBackend.reset();
    } else {
      resultType = "error";
      error = "Unexpected worker request stage";
    }
    if (!error.isEmpty()) payload = {{"message", error}};
    auto reply = response(packet, resultType, payload);
    if (resultType == "result" && !rawHeads.empty())
      attachFloat32Payload(reply, rawHeads, rawShape);
    auto encoded = encodeWorkerPacket(reply, error);
    if (encoded.isEmpty() && resultType == "result") {
      const auto resultError = error;
      reply = response(packet, "error",
                       {{"message",
                         "Model result exceeds the frozen protocol "
                         "budget: " +
                             resultError}});
      encoded = encodeWorkerPacket(reply, error);
    }
    if (encoded.isEmpty() ||
        std::fwrite(encoded.constData(), 1, encoded.size(), stdout) !=
            static_cast<std::size_t>(encoded.size()) ||
        std::fflush(stdout))
      return 3;
  }
}
