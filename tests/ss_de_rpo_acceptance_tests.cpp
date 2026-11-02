// SS-DE-RPO acceptance plans. Real IQ metadata comes from UX-R6 manifests.
// Window counts are whatever makeDetectionSourcePlan returns. This file does
// not synthesize IQ, repeat frames, or start a Worker.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "domain/detection.h"
#include "domain/sample_format.h"
#include "infrastructure/detection_source.h"
#include "infrastructure/detection_store.h"

using namespace signalstudio;

namespace {
constexpr std::uint64_t kTargets[] = {20, 159, 363, 2044};
constexpr char kCatalogRel[][160] = {
    "docs/implementation/ux-r6/evidence/baseline/user-failure-run-records.json",
    "docs/implementation/ux-r6/evidence/baseline/"
    "run-0c82d126-38e7-4b14-8778-491fc5c670f3-original-manifest.json",
    "docs/implementation/ux-r6/evidence/baseline/"
    "run-fcce913a-39cc-4108-96cc-a21224078378-original-manifest.json",
};

struct CatalogRequest {
  QString label;
  QString catalogFile;
  DetectionRequest request;
  std::optional<std::uint64_t> recordedEligible;
};

struct PlanHit {
  bool ok = false;
  std::uint64_t windows = 0;
  QString error;
};

int gFailures = 0;
QJsonArray gRows;

void addRow(const QString& id, const QString& status, const QString& detail) {
  const bool pass = status == QLatin1String("PASS");
  if (!pass) ++gFailures;
  std::cout << id.toStdString() << ' ' << status.toStdString() << ' '
            << detail.toStdString() << '\n';
  gRows.append(QJsonObject{{"id", id},
                           {"status", status},
                           {"detail", detail},
                           {"gpu", false},
                           {"syntheticRepeatedFrames", false}});
}

QStringList sourceRoots() {
  QStringList roots;
  if (const auto env = qEnvironmentVariable("SS_DE_RPO_SOURCE"); !env.isEmpty())
    roots.append(env);
  roots.append(QDir::currentPath());
  QDir cursor(QCoreApplication::applicationDirPath());
  for (int depth = 0; depth < 6; ++depth) {
    roots.append(cursor.absolutePath());
    if (!cursor.cdUp()) break;
  }
  roots.append(QStringLiteral("D:/project/ss-de-rpo-wt/continue-g"));
  roots.append(QStringLiteral("D:/project/signal_studio_rebuild"));
  roots.removeDuplicates();
  return roots;
}

QString findCatalog(const QString& relative) {
  for (const auto& root : sourceRoots()) {
    const auto path = QDir(root).filePath(relative);
    if (QFileInfo::exists(path)) return QFileInfo(path).absoluteFilePath();
  }
  return {};
}

std::optional<std::uint64_t> jsonCount(const QJsonValue& value) {
  if (value.isString()) {
    bool ok = false;
    const auto text = value.toString();
    const auto count = text.toULongLong(&ok);
    if (!ok || QString::number(count) != text) return std::nullopt;
    return count;
  }
  if (value.isDouble()) {
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0 || std::floor(number) != number)
      return std::nullopt;
    return static_cast<std::uint64_t>(number);
  }
  return std::nullopt;
}

// A missing count is not a comparison. A present but unparsable count is a failure.
std::optional<std::uint64_t> recordedWindows(const QJsonObject& object, const QString& label) {
  if (!object.contains(QStringLiteral("eligibleWindows")) ||
      object.value(QStringLiteral("eligibleWindows")).isNull())
    return std::nullopt;
  const auto parsed = jsonCount(object.value(QStringLiteral("eligibleWindows")));
  if (!parsed) {
    addRow(QStringLiteral("R-06-replay"), QStringLiteral("FAIL"),
           label + QStringLiteral(": eligibleWindows is present but not a uint64"));
  }
  return parsed;
}

DetectionSourceConfig sourceConfigFor(const DetectionRequest& request) {
  DetectionSourceConfig config;
  config.preferGpu = request.gpuDspPreferred;
  config.inputAdaptationVersion = request.inputAdaptationVersion;
  config.profileId = request.profile.id;
  config.profileVersion = request.profile.version;
  config.fftSize = request.profile.fftSize;
  config.modelFrames = request.profile.modelFrames;
  config.modelHopFrames = request.profile.modelHopFrames;
  config.hopSeconds = request.profile.hopSeconds;
  config.workingSetBudgetBytes = request.workingSetBudgetBytes;
  config.evidenceBudgetBytes = request.evidenceBudgetBytes;
  config.rawHeadBudgetBytes = request.rawHeadBudgetBytes;
  config.legacyCombinedRawHeads = request.legacyCombinedEvidence;
  config.maximumCandidates = request.parameters.topK;
  return config;
}

FileMetadata metadataFrom(const DetectionRequest& request) {
  const auto& identity = request.sourceIdentity;
  FileMetadata metadata;
  metadata.id = identity.fileId;
  metadata.path = identity.path;
  metadata.name = QFileInfo(QString::fromStdString(identity.path)).fileName().toStdString();
  metadata.sampleRateHz = identity.sampleRateHz;
  metadata.centerFrequencyHz = identity.centerFrequencyHz;
  metadata.sampleFormat = identity.sampleFormat;
  metadata.sampleCount = identity.physicalSamples;
  metadata.demo = false;
  metadata.effectiveBandwidthHz =
      identity.effectiveFrequency.upperHz - identity.effectiveFrequency.lowerHz;
  metadata.availability.availableSamples = identity.availableSamples;
  metadata.availability.status = identity.availableSamples == identity.physicalSamples
                                     ? LoadStatus::Ready
                                     : LoadStatus::Partial;
  metadata.availability.fingerprint = identity.quickFingerprint;
  return metadata;
}

QString identityError(const DetectionRequest& request) {
  const auto& identity = request.sourceIdentity;
  const auto path = QString::fromStdString(identity.path);
  QFileInfo info(path);
  if (!info.exists() || !info.isFile())
    return QStringLiteral("real IQ is missing: %1").arg(path);
  std::uint64_t payload = 0;
  if (!checkedMulU64(identity.physicalSamples, bytesPerFrame(identity.sampleFormat), payload))
    return QStringLiteral("physical sample byte count overflows");
  std::uint64_t expected = 0;
  if (!checkedMulU64(1, payload, expected)) return QStringLiteral("size arithmetic failed");
  if (identity.sampleFormat.headerBytes > ~payload ||
      identity.sampleFormat.trailerBytes > ~payload - identity.sampleFormat.headerBytes)
    return QStringLiteral("header and trailer overflow the file size");
  expected = payload + identity.sampleFormat.headerBytes + identity.sampleFormat.trailerBytes;
  if (static_cast<std::uint64_t>(info.size()) != expected)
    return QStringLiteral("IQ size %1 != header+trailer+physicalSamples*frame %2; refusing to invent samples")
        .arg(info.size())
        .arg(expected);
  if (identity.availableSamples > identity.physicalSamples)
    return QStringLiteral("available prefix exceeds the physical file");
  return {};
}

PlanHit planAt(const DetectionRequest& base, std::uint64_t endSample) {
  PlanHit hit;
  if (endSample <= base.requestedRange.time.begin ||
      endSample > base.sourceIdentity.physicalSamples) {
    hit.error = QStringLiteral("requested end leaves the real physical file");
    return hit;
  }
  auto request = base;
  request.requestedRange.time.end = endSample;
  if (request.sourceIdentity.physicalSamples != base.sourceIdentity.physicalSamples ||
      request.sourceIdentity.availableSamples != base.sourceIdentity.availableSamples) {
    hit.error = QStringLiteral("planner input changed the physical sample count");
    return hit;
  }
  const auto metadata = metadataFrom(request);
  if (metadata.sampleCount != base.sourceIdentity.physicalSamples || metadata.demo) {
    hit.error = QStringLiteral("metadata is not the manifest's physical file");
    return hit;
  }
  DetectionSourcePlan plan;
  QString error;
  hit.ok = makeDetectionSourcePlan(metadata, request.requestedRange, sourceConfigFor(request),
                                   plan, error, request.sourceIdentity.projectGeneration);
  hit.windows = plan.eligibleWindowCount;
  hit.error = error;
  return hit;
}

QString profileText(const DetectionRequest& request) {
  return QStringLiteral("%1 hop=%2 fft=%3 frames=%4 hopFrames=%5 schema=%6")
      .arg(QString::fromStdString(request.profile.id))
      .arg(request.profile.hopSeconds, 0, 'g', 12)
      .arg(request.profile.fftSize)
      .arg(request.profile.modelFrames)
      .arg(request.profile.modelHopFrames)
      .arg(request.inputAdaptationVersion);
}

void takeRequest(const QJsonObject& requestObject, const QString& label,
                 const QString& catalogFile, std::optional<std::uint64_t> recorded,
                 std::vector<CatalogRequest>& out) {
  DetectionRequest request;
  QString error;
  if (!decodeDetectionRequest(requestObject, request, error)) {
    addRow(QStringLiteral("R-06-decode"), QStringLiteral("FAIL"),
           label + ": " + error);
    return;
  }
  out.push_back(CatalogRequest{label, catalogFile, std::move(request), recorded});
}

void collect(const QJsonValue& value, const QString& label, const QString& catalogFile,
             std::vector<CatalogRequest>& out) {
  if (value.isArray()) {
    int index = 0;
    for (const auto& item : value.toArray())
      collect(item, label + QStringLiteral("#") + QString::number(index++), catalogFile, out);
    return;
  }
  if (!value.isObject()) return;
  const auto object = value.toObject();
  if (object.contains(QStringLiteral("run")) && object.value(QStringLiteral("run")).isObject()) {
    const auto run = object.value(QStringLiteral("run")).toObject();
    const auto recorded = recordedWindows(run, label);
    if (run.contains(QStringLiteral("request"))) {
      takeRequest(run.value(QStringLiteral("request")).toObject(), label, catalogFile, recorded,
                  out);
      return;
    }
  }
  if (object.contains(QStringLiteral("request")) &&
      object.value(QStringLiteral("request")).isObject()) {
    takeRequest(object.value(QStringLiteral("request")).toObject(), label, catalogFile,
                recordedWindows(object, label), out);
    return;
  }
  if (object.contains(QStringLiteral("schemaVersion")) &&
      object.contains(QStringLiteral("sourceIdentity")))
    takeRequest(object, label, catalogFile, std::nullopt, out);
}

bool loadCatalogFile(const QString& path, std::vector<CatalogRequest>& out) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    addRow(QStringLiteral("R-06-catalog"), QStringLiteral("FAIL"),
           QStringLiteral("cannot open %1").arg(path));
    return false;
  }
  if (file.size() > 8 * 1024 * 1024) {
    addRow(QStringLiteral("R-06-catalog"), QStringLiteral("FAIL"),
           QStringLiteral("%1 exceeds 8 MiB; refusing a bulk read").arg(path));
    return false;
  }
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
  if (parse.error != QJsonParseError::NoError) {
    addRow(QStringLiteral("R-06-catalog"), QStringLiteral("FAIL"),
           path + ": " + parse.errorString());
    return false;
  }
  collect(document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object()),
          QFileInfo(path).fileName(), path, out);
  return true;
}

bool originalHopProfile(const DetectionRequest& request) {
  return std::abs(request.profile.hopSeconds - 0.00025) <= 1e-12 &&
         request.profile.modelFrames == 10 && request.profile.modelHopFrames == 5;
}

// Main confirmed this prefix. The count is still taken from SourcePlan.
std::optional<std::uint64_t> originalPrefixEnd(const DetectionRequest& request, QString& error) {
  const double rate = request.sourceIdentity.sampleRateHz;
  if (!std::isfinite(rate) || rate <= 0.0) {
    error = QStringLiteral("sample rate is not a positive finite value");
    return std::nullopt;
  }
  const long double samples = 0.02625L * static_cast<long double>(rate);
  const auto rounded = std::llround(samples);
  if (rounded <= 0 || std::fabsl(samples - static_cast<long double>(rounded)) > 1e-6L) {
    error = QStringLiteral("0.02625 s is not an integer sample count at this rate");
    return std::nullopt;
  }
  const auto count = static_cast<std::uint64_t>(rounded);
  if (count > std::numeric_limits<std::uint64_t>::max() - request.requestedRange.time.begin) {
    error = QStringLiteral("0.02625 s prefix overflows the requested begin");
    return std::nullopt;
  }
  return request.requestedRange.time.begin + count;
}

std::optional<std::uint64_t> findEndForWindows(const DetectionRequest& request,
                                              std::uint64_t target, QString& error) {
  const auto full = planAt(request, request.sourceIdentity.physicalSamples);
  if (!full.ok) {
    error = full.error;
    return std::nullopt;
  }
  if (full.windows < target) {
    error = QStringLiteral("full physical file plans %1 windows").arg(full.windows);
    return std::nullopt;
  }
  if (full.windows == target) return request.sourceIdentity.physicalSamples;
  std::uint64_t lower = request.requestedRange.time.begin + 1;
  std::uint64_t upper = request.sourceIdentity.physicalSamples;
  std::optional<std::uint64_t> found;
  while (lower <= upper) {
    const auto middle = lower + (upper - lower) / 2;
    const auto hit = planAt(request, middle);
    if (!hit.ok) {
      if (middle == upper) break;
      lower = middle + 1;
      error = hit.error;
      continue;
    }
    if (hit.windows < target) lower = middle + 1;
    else {
      if (hit.windows == target) found = middle;
      if (middle == 0) break;
      upper = middle - 1;
    }
  }
  return found;
}

QString signature(const DetectionRequest& request) {
  const auto& identity = request.sourceIdentity;
  return QString::fromStdString(identity.path) + "|" + QString::number(identity.physicalSamples) +
         "|" + QString::number(identity.availableSamples) + "|" + profileText(request) + "|" +
         QString::number(request.workingSetBudgetBytes) + "|" +
         QString::number(request.evidenceBudgetBytes) + "|" +
         QString::number(request.legacyCombinedEvidence);
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  QCommandLineParser options;
  options.addOption({QStringLiteral("matrix-out"),
                     QStringLiteral("Write the acceptance matrix JSON here"),
                     QStringLiteral("path")});
  options.addOption({QStringLiteral("resident-redetect"),
                     QStringLiteral("Reserved warm entry. Does not launch a Worker.")});
  options.process(application);
  if (options.isSet(QStringLiteral("resident-redetect"))) {
    std::cout
        << "R-19-warm BLOCKED warm means one resident application redetects with an "
           "independent Worker per run. This executable does not start a Worker and "
           "will not relabel a new process as warm.\n";
    return 2;
  }

  std::vector<CatalogRequest> catalog;
  for (const auto& relative : kCatalogRel) {
    const auto path = findCatalog(QString::fromLatin1(relative));
    if (path.isEmpty()) {
      addRow(QStringLiteral("R-06-catalog"), QStringLiteral("FAIL"),
             QStringLiteral("missing UX-R6 manifest %1").arg(QString::fromLatin1(relative)));
      continue;
    }
    loadCatalogFile(path, catalog);
  }
  const auto n0Request =
      QStringLiteral("D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/N0/request-raw-off-v2.json");
  if (QFileInfo::exists(n0Request)) loadCatalogFile(n0Request, catalog);
  const auto extra = qEnvironmentVariable("SS_DE_RPO_REQUEST_JSON");
  if (!extra.isEmpty()) {
    for (const auto& piece : extra.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
      const auto path = QFileInfo(piece).absoluteFilePath();
      if (!QFileInfo::exists(path)) {
        addRow(QStringLiteral("R-06-catalog"), QStringLiteral("FAIL"),
               QStringLiteral("SS_DE_RPO_REQUEST_JSON missing %1").arg(path));
        continue;
      }
      loadCatalogFile(path, catalog);
    }
  }
  if (catalog.empty()) {
    addRow(QStringLiteral("R-06"), QStringLiteral("FAIL"),
           QStringLiteral("no real IQ request was decoded"));
  }

  std::map<QString, const CatalogRequest*> unique;
  std::uint64_t maxWindows = 0;
  for (const auto& item : catalog) {
    const auto identity = identityError(item.request);
    if (!identity.isEmpty()) {
      addRow(QStringLiteral("R-06-identity"), QStringLiteral("FAIL"),
             item.label + ": " + identity);
      continue;
    }
    const auto recordedEnd = std::min(item.request.requestedRange.time.end,
                                      item.request.sourceIdentity.physicalSamples);
    const auto planned = planAt(item.request, recordedEnd);
    if (!planned.ok) {
      addRow(QStringLiteral("R-06-replay"), QStringLiteral("FAIL"),
             item.label + " " + profileText(item.request) + " error=" + planned.error);
      continue;
    }
    if (item.recordedEligible && *item.recordedEligible != planned.windows) {
      addRow(QStringLiteral("R-06-replay"), QStringLiteral("FAIL"),
             QStringLiteral("%1 recorded %2 production SourcePlan %3")
                 .arg(item.label)
                 .arg(*item.recordedEligible)
                 .arg(planned.windows));
    } else if (item.recordedEligible) {
      addRow(QStringLiteral("R-06-replay"), QStringLiteral("PASS"),
             QStringLiteral("%1 windows=%2 %3")
                 .arg(item.label)
                 .arg(planned.windows)
                 .arg(profileText(item.request)));
    }
    const auto full = planAt(item.request, item.request.sourceIdentity.physicalSamples);
    if (full.ok) maxWindows = std::max(maxWindows, full.windows);
    unique.emplace(signature(item.request), &item);
  }

  for (const auto target : kTargets) {
    if (target == 20) {
      const CatalogRequest* chosen = nullptr;
      std::uint64_t chosenEnd = 0;
      QString mismatch;
      for (const auto& [key, item] : unique) {
        (void)key;
        if (!originalHopProfile(item->request)) continue;
        if (const auto identity = identityError(item->request); !identity.isEmpty()) continue;
        QString error;
        const auto end = originalPrefixEnd(item->request, error);
        if (!end) {
          mismatch = item->label + QStringLiteral(": ") + error;
          continue;
        }
        const auto check = planAt(item->request, *end);
        const auto observed = check.ok ? check.windows : 0;
        if (!check.ok || observed != 20) {
          mismatch = QStringLiteral("%1 prefixEnd=%2 plans %3 %4")
                         .arg(item->label)
                         .arg(*end)
                         .arg(check.ok ? QString::number(observed) : QStringLiteral("error"))
                         .arg(check.ok ? profileText(item->request) : check.error);
          continue;
        }
        chosen = item;
        chosenEnd = *end;
        break;
      }
      if (!chosen) {
        addRow(QStringLiteral("R-06-20"), QStringLiteral("FAIL"),
               QStringLiteral("original IQ prefix 0.02625s with hop 0.00025, 10 frames and "
                              "5 hop frames did not plan 20 windows on the real file. "
                              "Refusing the hop 0.0001 first-map hit and synthetic frames. %1")
                   .arg(mismatch));
      } else {
        addRow(QStringLiteral("R-06-20"), QStringLiteral("PASS"),
               QStringLiteral("windows=20 prefixSeconds=0.02625 endSample=%1 physicalSamples=%2 %3 "
                              "source=%4 catalog=%5")
                   .arg(chosenEnd)
                   .arg(chosen->request.sourceIdentity.physicalSamples)
                   .arg(profileText(chosen->request))
                   .arg(QString::fromStdString(chosen->request.sourceIdentity.path))
                   .arg(chosen->label));
      }
      continue;
    }
    const CatalogRequest* chosen = nullptr;
    std::uint64_t chosenEnd = 0;
    QString searchError;
    for (const auto& [key, item] : unique) {
      (void)key;
      if (const auto identity = identityError(item->request); !identity.isEmpty()) continue;
      QString error;
      const auto end = findEndForWindows(item->request, target, error);
      if (!end) {
        searchError = error;
        continue;
      }
      const auto check = planAt(item->request, *end);
      if (!check.ok || check.windows != target) {
        searchError = check.ok ? QStringLiteral("planner did not repeat the searched count")
                               : check.error;
        continue;
      }
      if (item->request.sourceIdentity.physicalSamples <
          item->request.sourceIdentity.availableSamples) {
        searchError = QStringLiteral("available samples exceed the physical file");
        continue;
      }
      chosen = item;
      chosenEnd = *end;
      break;
    }
    const auto id = QStringLiteral("R-06-%1").arg(target);
    if (!chosen) {
      addRow(id, QStringLiteral("FAIL"),
             QStringLiteral("no UX-R6 manifest profile on the real IQ plans %1 windows "
                            "(max full-file plan %2). Refusing to repeat synthetic frames "
                            "or extend physicalSamples. Last profile search: %3")
                 .arg(target)
                 .arg(maxWindows)
                 .arg(searchError));
      continue;
    }
    addRow(id, QStringLiteral("PASS"),
           QStringLiteral("windows=%1 endSample=%2 physicalSamples=%3 %4 source=%5 catalog=%6")
               .arg(target)
               .arg(chosenEnd)
               .arg(chosen->request.sourceIdentity.physicalSamples)
               .arg(profileText(chosen->request))
               .arg(QString::fromStdString(chosen->request.sourceIdentity.path))
               .arg(chosen->label));
  }

  QJsonObject matrix{{"schema", "signal-studio-ss-de-rpo-acceptance-v1"},
                     {"syntheticRepeatedFrames", false},
                     {"workerLaunched", false},
                     {"gpuExecuted", false},
                     {"maxFullFileWindows", QString::number(maxWindows)},
                     {"rows", gRows}};
  const auto encoded = QJsonDocument(matrix).toJson(QJsonDocument::Indented);
  if (options.isSet(QStringLiteral("matrix-out"))) {
    QFile out(options.value(QStringLiteral("matrix-out")));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        out.write(encoded) != encoded.size()) {
      std::cout << "R-06-matrix FAIL cannot write matrix output\n";
      return 1;
    }
  }
  std::cout << (gFailures ? "FAIL" : "PASS") << " ss_de_rpo_acceptance_tests failures="
            << gFailures << '\n';
  return gFailures == 0 ? 0 : 1;
}
