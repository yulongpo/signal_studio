#include "infrastructure/detection_protocol.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace signalstudio {
namespace {
constexpr qsizetype kControlBudget = 65536;
constexpr qsizetype kBinaryBudget = 33554432;
QString digest(const QByteArray& data) {
  return QString::fromLatin1(
      QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
bool fail(QString& error, const QString& reason) {
  error = reason;
  return false;
}
bool onlyKeys(const QJsonObject& object, const QSet<QString>& allowed) {
  for (auto it = object.begin(); it != object.end(); ++it)
    if (!allowed.contains(it.key())) return false;
  return true;
}
bool validId(const QJsonValue& value) {
  return value.isString() && !value.toString().isEmpty() &&
         value.toString().size() <= 128;
}
bool finitePayload(const QJsonValue& value, int depth = 0,
                   const QString& key = {}) {
  if (depth > 32 || value.isUndefined()) return false;
  if (value.isNull())
    return key == "parentRunId" || key == "hashedRange" ||
           key == "contextRange";
  if (value.isDouble()) return std::isfinite(value.toDouble());
  if (value.isObject()) {
    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
      if (!finitePayload(it.value(), depth + 1, it.key())) return false;
  } else if (value.isArray()) {
    for (const auto item : value.toArray())
      if (!finitePayload(item, depth + 1)) return false;
  }
  return true;
}
bool binarySize(const QJsonObject& descriptor, qsizetype& bytes,
                QString& error) {
  if (!onlyKeys(descriptor,
                {"byteLength", "dtype", "byteOrder", "shape", "sha256"}) ||
      !descriptor["byteLength"].isDouble() ||
      descriptor["byteOrder"] != "little" || !descriptor["shape"].isArray() ||
      !QRegularExpression("\\A[0-9a-f]{64}\\z")
           .match(descriptor["sha256"].toString())
           .hasMatch())
    return fail(error, "Invalid worker binary descriptor");
  const auto length = descriptor["byteLength"].toDouble(-1);
  if (!std::isfinite(length) || length < 0 || length > kBinaryBudget ||
      std::floor(length) != length)
    return fail(error, "Worker binary length exceeds 32 MiB");
  const auto type = descriptor["dtype"].toString();
  quint64 expected = type == "float32"     ? 4
                     : type == "complex64" ? 8
                     : type == "uint8"     ? 1
                                           : 0;
  if (!expected) return fail(error, "Unsupported worker binary dtype");
  const auto shape = descriptor["shape"].toArray();
  if (shape.isEmpty() || shape.size() > 5)
    return fail(error, "Worker binary shape rank is invalid");
  for (const auto& value : shape) {
    const auto dimension = value.toDouble(-1);
    if (!value.isDouble() || dimension < 1 || dimension > 4194304 ||
        std::floor(dimension) != dimension ||
        expected > static_cast<quint64>(kBinaryBudget) /
                       static_cast<quint64>(dimension))
      return fail(error, "Worker binary shape exceeds byte budget");
    expected *= static_cast<quint64>(dimension);
  }
  if (expected != static_cast<quint64>(length))
    return fail(error, "Worker shape/dtype does not match byte length");
  bytes = static_cast<qsizetype>(length);
  return true;
}
bool validateEnvelope(const QJsonObject& envelope, QString& error) {
  if (!onlyKeys(envelope,
                {"protocolVersion", "type", "requestId", "sequence", "payload",
                 "runId", "projectGeneration", "fileId", "binary"}) ||
      envelope["protocolVersion"] != 1 || !envelope["payload"].isObject() ||
      !validId(envelope["requestId"]) || !finitePayload(envelope["payload"]))
    return fail(error, "Worker protocol version/envelope is invalid");
  static const QSet<QString> types{"hello",  "ready",  "begin",     "batch",
                                   "result", "cancel", "completed", "error"};
  const auto type = envelope["type"].toString();
  if (!types.contains(type)) return fail(error, "Unknown worker control type");
  quint64 sequence = 0;
  if (!workerUnsigned(envelope["sequence"], sequence, error)) return false;
  const bool correlated = type == "begin" || type == "batch" ||
                          type == "result" || type == "cancel" ||
                          type == "completed";
  if ((correlated || envelope.contains("runId")) && !validId(envelope["runId"]))
    return fail(error, "Worker run correlation is missing");
  if ((correlated || envelope.contains("fileId")) &&
      !validId(envelope["fileId"]))
    return fail(error, "Worker file correlation is missing");
  if (correlated || envelope.contains("projectGeneration")) {
    quint64 generation = 0;
    if (!workerUnsigned(envelope["projectGeneration"], generation, error))
      return false;
  }
  return true;
}
}  // namespace

bool workerUnsigned(const QJsonValue& value, quint64& target, QString& error) {
  if (!value.isString() || !QRegularExpression("\\A(?:0|[1-9][0-9]{0,19})\\z")
                                .match(value.toString())
                                .hasMatch())
    return fail(error,
                "Worker indices require canonical uint64 decimal strings");
  bool ok = false;
  const auto result = value.toString().toULongLong(&ok);
  if (!ok) return fail(error, "Worker uint64 index overflow");
  target = result;
  return true;
}
bool validateWorkerPacket(const DetectionWorkerPacket& packet, QString& error) {
  error.clear();
  if (!validateEnvelope(packet.envelope, error)) return false;
  if (!packet.envelope.contains("binary"))
    return packet.binary.isEmpty() ||
           fail(error, "Undeclared worker binary data");
  if (!packet.envelope["binary"].isObject())
    return fail(error, "Invalid binary descriptor");
  qsizetype bytes = 0;
  const auto descriptor = packet.envelope["binary"].toObject();
  if (!binarySize(descriptor, bytes, error)) return false;
  if (packet.binary.size() != bytes ||
      digest(packet.binary) != descriptor["sha256"].toString())
    return fail(error, "Worker binary length/hash mismatch");
  return true;
}
QByteArray encodeWorkerPacket(const DetectionWorkerPacket& packet,
                              QString& error) {
  if (!validateWorkerPacket(packet, error)) return {};
  const auto json =
      QJsonDocument(packet.envelope).toJson(QJsonDocument::Compact);
  if (json.isEmpty() || json.size() > kControlBudget) {
    fail(error, "Worker control message exceeds 64 KiB");
    return {};
  }
  QByteArray output(4, '\0');
  qToLittleEndian<quint32>(static_cast<quint32>(json.size()), output.data());
  output.append(json);
  output.append(packet.binary);
  return output;
}
QJsonObject workerEnvelope(const QString& type, const QString& requestId,
                           quint64 sequence, const QJsonObject& payload,
                           const QString& runId, quint64 generation,
                           const QString& fileId) {
  QJsonObject envelope{{"protocolVersion", 1},
                       {"type", type},
                       {"requestId", requestId},
                       {"sequence", QString::number(sequence)},
                       {"payload", payload}};
  if (!runId.isEmpty()) {
    envelope["runId"] = runId;
    envelope["projectGeneration"] = QString::number(generation);
    envelope["fileId"] = fileId;
  }
  return envelope;
}
void attachFloat32Payload(DetectionWorkerPacket& packet,
                          const std::vector<float>& values,
                          const std::vector<int>& shape) {
  packet.binary.resize(static_cast<qsizetype>(values.size() * sizeof(float)));
  for (std::size_t i = 0; i < values.size(); ++i)
    qToLittleEndian<quint32>(std::bit_cast<quint32>(values[i]),
                             packet.binary.data() + i * sizeof(float));
  QJsonArray dimensions;
  for (const auto dimension : shape) dimensions.append(dimension);
  packet.envelope["binary"] =
      QJsonObject{{"byteLength", static_cast<int>(packet.binary.size())},
                  {"dtype", "float32"},
                  {"byteOrder", "little"},
                  {"shape", dimensions},
                  {"sha256", digest(packet.binary)}};
}
bool decodeFloat32Payload(const DetectionWorkerPacket& packet,
                          std::vector<float>& values, QString& error) {
  if (!validateWorkerPacket(packet, error) ||
      packet.envelope["binary"].toObject()["dtype"] != "float32")
    return fail(error,
                error.isEmpty() ? "Expected float32 worker payload" : error);
  std::vector<float> candidate(static_cast<std::size_t>(packet.binary.size()) /
                               4);
  for (std::size_t i = 0; i < candidate.size(); ++i) {
    candidate[i] = std::bit_cast<float>(
        qFromLittleEndian<quint32>(packet.binary.constData() + i * 4));
    if (!std::isfinite(candidate[i]))
      return fail(error, "Nonfinite worker float32 value");
  }
  values = std::move(candidate);
  return true;
}
bool DetectionPacketDecoder::append(const QByteArray& bytes, QString& error) {
  if (buffer_.size() + bytes.size() > kControlBudget + kBinaryBudget + 4)
    return fail(error, "Worker receive buffer exceeds bounded packet budget");
  buffer_.append(bytes);
  return true;
}
bool DetectionPacketDecoder::take(std::optional<DetectionWorkerPacket>& packet,
                                  QString& error) {
  packet.reset();
  if (buffer_.size() < 4) return true;
  const auto jsonBytes = qFromLittleEndian<quint32>(buffer_.constData());
  if (!jsonBytes || jsonBytes > kControlBudget)
    return fail(error, "Invalid worker control length");
  if (buffer_.size() < 4 + static_cast<qsizetype>(jsonBytes)) return true;
  QJsonParseError parseError;
  const auto document =
      QJsonDocument::fromJson(buffer_.mid(4, jsonBytes), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return fail(error, "Malformed worker control JSON");
  const auto envelope = document.object();
  if (!validateEnvelope(envelope, error)) return false;
  qsizetype binaryBytes = 0;
  if (envelope.contains("binary") &&
      (!envelope["binary"].isObject() ||
       !binarySize(envelope["binary"].toObject(), binaryBytes, error)))
    return fail(error, error.isEmpty() ? "Invalid binary descriptor" : error);
  const auto packetBytes = 4 + static_cast<qsizetype>(jsonBytes) + binaryBytes;
  if (buffer_.size() < packetBytes) return true;
  DetectionWorkerPacket candidate{envelope,
                                  buffer_.mid(4 + jsonBytes, binaryBytes)};
  if (!validateWorkerPacket(candidate, error)) return false;
  buffer_.remove(0, packetBytes);
  packet = std::move(candidate);
  return true;
}
}  // namespace signalstudio
