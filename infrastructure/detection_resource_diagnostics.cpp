#include "infrastructure/detection_resource_diagnostics.h"
#include <QJsonDocument>
#include <QJsonObject>

namespace signalstudio {
namespace {
QString q(const std::string& value) { return QString::fromStdString(value); }
QString decimal(std::uint64_t value) { return QString::number(static_cast<qulonglong>(value)); }
QString status(ResourceProbeStatus value) {
  switch (value) {
    case ResourceProbeStatus::Probed: return "probed";
    case ResourceProbeStatus::NotRequested: return "not-requested";
    case ResourceProbeStatus::Unavailable: return "unavailable";
  }
  return "unavailable";
}
QJsonObject probe(const ResourceByteProbe& value) {
  return {{"status", status(value.status)}, {"total", decimal(value.totalBytes)},
          {"available", decimal(value.availableBytes)}, {"detail", q(value.detail)}};
}
QJsonObject decision(const ResourceDecision& value) {
  const auto kind = value.kind == ResourceDecisionKind::Accepted ? "Accepted"
      : value.kind == ResourceDecisionKind::Adapted ? "Adapted" : "Rejected";
  return {{"kind", kind}, {"errorCode", q(value.errorCode)}, {"phase", q(value.phase)},
          {"resource", q(value.resource)}, {"unit", resourceDecisionUnit(value)},
          {"requested", decimal(value.requested)}, {"available", decimal(value.available)},
          {"reserve", decimal(value.reserve)}, {"granted", decimal(value.granted)}};
}
}
bool mergeRuntimeResourceDiagnostics(const std::string& performanceJson,
                                     const RuntimeResourceSnapshot& snapshot,
                                     const DiskLedgerEvaluation* ledger,
                                     const ResourceDecision* sampleDecision,
                                     std::string& merged, std::string& error) {
  constexpr std::size_t maximumBytes = 65536;
  const auto reject = [&] { error = "resource.diagnostics-rejected"; return false; };
  if (performanceJson.size() > maximumBytes) return reject();
  QJsonObject profile;
  if (!performanceJson.empty()) {
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(performanceJson), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return reject();
    profile = document.object();
    if (profile.contains("schema") && profile["schema"] != "signal-studio-detection-profile-v1")
      return reject();
  }
  profile["schema"] = "signal-studio-detection-profile-v1";
  QJsonObject resources{{"ram", probe(snapshot.ram)}, {"vram", probe(snapshot.vram)},
      {"volume", QJsonObject{{"status", status(snapshot.volume.status)},
          {"key", q(snapshot.volume.volumeKey)}, {"available", decimal(snapshot.volume.availableBytes)},
          {"total", decimal(snapshot.volume.totalBytes)}, {"detail", q(snapshot.volume.detail)}}},
      {"capturedAtSteadyNs", decimal(snapshot.capturedAtSteadyNs)}};
  if (ledger) {
    resources["diskLedger"] = decision(ledger->decision);
    resources["diskPeakPhase"] = q(ledger->peakPhase);
    resources["diskPeakVolume"] = q(ledger->peakVolumeKey);
    resources["diskPeakBytes"] = decimal(ledger->peakBytes);
  }
  if (sampleDecision) resources["sample"] = decision(*sampleDecision);
  profile["resources"] = resources;
  const auto bytes = QJsonDocument(profile).toJson(QJsonDocument::Compact);
  if (static_cast<std::size_t>(bytes.size()) > maximumBytes) return reject();
  merged = bytes.toStdString();
  error.clear();
  return true;
}
}
