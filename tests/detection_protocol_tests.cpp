#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "infrastructure/detection_protocol.h"

using namespace signalstudio;

namespace {
void check(bool condition, const QString& message) {
  if (!condition) throw std::runtime_error(message.toUtf8().toStdString());
}
QString hash(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
DetectionWorkerPacket fixture() {
  DetectionWorkerPacket packet;
  packet.envelope =
      workerEnvelope("batch", "request-1", 7, {}, "run-1", 42, "file-1");
  attachFloat32Payload(packet, {1.25f, -2.5f, 3.0f, 0.0f}, {1, 1, 2, 2});
  return packet;
}
QByteArray rawPacket(const QByteArray& json) {
  QByteArray result(4, '\0');
  qToLittleEndian<quint32>(static_cast<quint32>(json.size()), result.data());
  result.append(json);
  return result;
}
void fragmentedPacketsAndHash() {
  const auto packet = fixture();
  QString error;
  const auto wire = encodeWorkerPacket(packet, error);
  check(!wire.isEmpty(), error);
  DetectionPacketDecoder decoder;
  std::optional<DetectionWorkerPacket> decoded;
  for (qsizetype i = 0; i < wire.size(); ++i) {
    check(decoder.append(wire.mid(i, 1), error) && decoder.take(decoded, error),
          error);
    check(decoded.has_value() == (i == wire.size() - 1),
          "A fragmented packet is available only after its exact last binary "
          "byte");
  }
  check(decoded->envelope == packet.envelope &&
            decoded->binary == packet.binary && decoder.bufferedBytes() == 0,
        "Framing must preserve both the correlated control and exact binary "
        "bytes");
  std::vector<float> values;
  check(decodeFloat32Payload(*decoded, values, error) &&
            values == std::vector<float>({1.25f, -2.5f, 3.0f, 0.0f}),
        error);
  check(decoder.append(wire + wire, error), error);
  check(decoder.take(decoded, error) && decoded &&
            decoder.take(decoded, error) && decoded &&
            decoder.take(decoded, error) && !decoded,
        "Coalesced packets must remain separate");
  auto corrupt = wire;
  corrupt[corrupt.size() - 1] = static_cast<char>(corrupt.back() ^ 1);
  check(decoder.append(corrupt, error) && !decoder.take(decoded, error) &&
            !decoded,
        "Corrupt binary hash must prevent packet publication");
  decoder.clear();
  check(decoder.bufferedBytes() == 0,
        "Decoder clearing must discard damaged bytes");
}
void invalidEnvelopeAndUnsignedIndices() {
  QString error;
  quint64 number = 17;
  check(workerUnsigned(QString::number(std::numeric_limits<quint64>::max()),
                       number, error) &&
            number == std::numeric_limits<quint64>::max(),
        error);
  for (const auto& invalid :
       {QString("18446744073709551616"), QString("0\n"), QString("01"),
        QString("-1"), QString("+1"), QString("1.5"), QString("1 ")}) {
    number = 17;
    check(!workerUnsigned(invalid, number, error) && number == 17,
          "Invalid uint64 spelling/overflow must preserve the output index");
  }
  auto packet = fixture();
  for (const auto& key :
       {"runId", "projectGeneration", "fileId", "requestId", "sequence"}) {
    auto missing = packet;
    missing.envelope.remove(key);
    check(encodeWorkerPacket(missing, error).isEmpty(),
          "Correlated packets require every identity field");
  }
  packet.envelope["protocolVersion"] = 2;
  check(encodeWorkerPacket(packet, error).isEmpty(),
        "Unsupported protocol versions must fail");
  packet = fixture();
  packet.envelope["execute"] = "arbitrary-script";
  check(encodeWorkerPacket(packet, error).isEmpty(),
        "Unknown executable fields are not permitted");
  packet = fixture();
  auto descriptor = packet.envelope["binary"].toObject();
  descriptor["sha256"] = QString(64, 'a') + '\n';
  packet.envelope["binary"] = descriptor;
  check(encodeWorkerPacket(packet, error).isEmpty(),
        "Hashes require exactly 64 lowercase hex characters");
  packet = fixture();
  descriptor = packet.envelope["binary"].toObject();
  descriptor["shape"] = QJsonArray{4, 4};
  packet.envelope["binary"] = descriptor;
  check(encodeWorkerPacket(packet, error).isEmpty(),
        "Binary size and shape cannot disagree");
}
void controlAndBinaryBudgets() {
  QString error;
  DetectionWorkerPacket control;
  control.envelope = workerEnvelope("hello", "r", 0, {{"text", ""}});
  const auto overhead =
      QJsonDocument(control.envelope).toJson(QJsonDocument::Compact).size();
  control.envelope["payload"] = QJsonObject{
      {"text", QString(static_cast<qsizetype>(65536) - overhead, 'x')}};
  auto wire = encodeWorkerPacket(control, error);
  check(wire.size() == 65536 + 4, "The exact 64 KiB control boundary is valid");
  auto payload = control.envelope["payload"].toObject();
  payload["text"] = payload["text"].toString() + 'x';
  control.envelope["payload"] = payload;
  check(encodeWorkerPacket(control, error).isEmpty(),
        "64 KiB plus one control byte must fail");
  DetectionPacketDecoder decoder;
  QByteArray length(4, '\0');
  qToLittleEndian<quint32>(65537, length.data());
  std::optional<DetectionWorkerPacket> output;
  check(decoder.append(length, error) && !decoder.take(output, error),
        "Invalid lengths are rejected before allocating the declared control "
        "payload");
  decoder.clear();
  DetectionWorkerPacket binary;
  binary.envelope = workerEnvelope("batch", "r", 0, {}, "run", 1, "file");
  binary.binary = QByteArray(33554432, '\0');
  binary.envelope["binary"] = QJsonObject{{"byteLength", 33554432},
                                          {"dtype", "float32"},
                                          {"byteOrder", "little"},
                                          {"shape", QJsonArray{2, 4194304}},
                                          {"sha256", hash(binary.binary)}};
  check(validateWorkerPacket(binary, error), error);
  auto descriptor = binary.envelope["binary"].toObject();
  descriptor["byteLength"] = 33554436;
  binary.envelope["binary"] = descriptor;
  check(encodeWorkerPacket(binary, error).isEmpty(),
        "Declared binary payloads cannot exceed 32 MiB");
  check(!decoder.append(QByteArray(33554432 + 65536 + 5, '\0'), error),
        "Receive buffering must remain bounded even before a packet is parsed");
}
void nonfiniteAndMalformedJson() {
  QString error;
  auto packet = fixture();
  attachFloat32Payload(packet, {std::numeric_limits<float>::quiet_NaN()}, {1});
  std::vector<float> sentinel{17};
  check(!decodeFloat32Payload(packet, sentinel, error) &&
            sentinel == std::vector<float>{17},
        "A finite SHA-256 descriptor does not make nonfinite tensor values "
        "valid");
  packet = fixture();
  attachFloat32Payload(packet, {std::numeric_limits<float>::infinity()}, {1});
  check(!decodeFloat32Payload(packet, sentinel, error),
        "Infinity cannot be model input");
  packet = fixture();
  packet.envelope["payload"] = QJsonObject{
      {"nested",
       QJsonObject{{"value", std::numeric_limits<double>::infinity()}}}};
  check(encodeWorkerPacket(packet, error).isEmpty(),
        "Nonfinite nested control values must fail before JSON serialization");
  DetectionPacketDecoder decoder;
  std::optional<DetectionWorkerPacket> decoded;
  const auto invalidJson = QByteArray(
      "{\"protocolVersion\":1,\"type\":\"hello\",\"requestId\":\"r\","
      "\"sequence\":\"0\",\"payload\":{\"nested\":{\"value\":1e999}}}");
  check(decoder.append(rawPacket(invalidJson), error) &&
            !decoder.take(decoded, error),
        "JSON float overflow must not turn into usable control state");
  decoder.clear();
  check(decoder.append(rawPacket("[1,2,3]"), error) &&
            !decoder.take(decoded, error),
        "A JSON array is not a control envelope");
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    fragmentedPacketsAndHash();
    invalidEnvelopeAndUnsignedIndices();
    controlAndBinaryBudgets();
    nonfiniteAndMalformedJson();
    std::cout << "Detection protocol boundary tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
