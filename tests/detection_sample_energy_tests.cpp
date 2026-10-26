#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "domain/detection_refinement.h"
#include "infrastructure/detection_sample_energy.h"
#include "infrastructure/raw_sample_reader.h"
#include "infrastructure/wav_metadata.h"

using namespace signalstudio;
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void check(bool value, const QString& message) {
  if (!value) throw std::runtime_error(message.toStdString());
}
constexpr std::uint64_t kParityBudget = 256 * 1024 * 1024;
std::uint64_t exactSample(const QJsonObject& config, const char* key) {
  const auto value = config.value(key);
  check(value.isString(), QString(key) + " must be a uint64 decimal string");
  const auto text = value.toString();
  check(!text.isEmpty() &&
            std::all_of(text.begin(), text.end(),
                        [](QChar c) { return c >= '0' && c <= '9'; }),
        QString(key) + " must contain only decimal digits");
  bool ok = false;
  const auto result = text.toULongLong(&ok);
  check(ok, QString(key) + " overflows uint64");
  return result;
}
double number(const QJsonObject& config, const char* key) {
  const auto value = config.value(key);
  check(value.isDouble() && std::isfinite(value.toDouble()),
        QString(key) + " must be a finite number");
  return value.toDouble();
}
double optionalNumber(const QJsonObject& config, const char* key,
                      double fallback) {
  return config.contains(key) ? number(config, key) : fallback;
}
QString hashFile(const QString& path) {
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "Cannot hash " + path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  check(hash.addData(&file), "Cannot finish hashing " + path);
  return QString::fromLatin1(hash.result().toHex());
}
QJsonObject artifact(const QString& path, const char* dtype,
                     std::uint64_t count) {
  return {{"path", QFileInfo(path).absoluteFilePath()},
          {"dtype", dtype},
          {"samples", QString::number(count)},
          {"bytes", QString::number(QFileInfo(path).size())},
          {"sha256", hashFile(path)}};
}
void writeFloats(const QString& path, std::span<const float> values) {
  QSaveFile file(path);
  check(file.open(QIODevice::WriteOnly), "Cannot write " + path);
  QByteArray block;
  constexpr std::size_t kBlock = 131072;
  block.resize(kBlock * sizeof(float));
  for (std::size_t first = 0; first < values.size(); first += kBlock) {
    const auto count = std::min(kBlock, values.size() - first);
    for (std::size_t i = 0; i < count; ++i)
      qToLittleEndian(std::bit_cast<quint32>(values[first + i]),
                      block.data() + i * sizeof(float));
    const auto bytes = static_cast<qint64>(count * sizeof(float));
    check(file.write(block.constData(), bytes) == bytes,
          "Short write to " + path);
  }
  check(file.commit(), "Cannot commit " + path);
}
void writeJson(const QString& path, const QJsonObject& value) {
  QSaveFile file(path);
  check(file.open(QIODevice::WriteOnly), "Cannot write " + path);
  const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
  check(file.write(bytes) == bytes.size() && file.commit(),
        "Cannot finish writing " + path);
}
int fileParity(const QStringList& args) {
  if (args.size() == 2 && args[1] == "--help") {
    std::cout << "--file <PCM16-IQ.wav> --config <json> --output <directory>\n";
    return 0;
  }
  check(args.size() == 7, "Require --file, --config and --output");
  QJsonObject paths;
  for (int i = 1; i < args.size(); i += 2) {
    check(args[i] == "--file" || args[i] == "--config" || args[i] == "--output",
          "Unknown CLI option " + args[i]);
    check(!paths.contains(args[i]), "Duplicate CLI option " + args[i]);
    paths[args[i]] = QFileInfo(args[i + 1]).absoluteFilePath();
  }
  check(paths.size() == 3, "Require all three distinct CLI options");
  const auto source = paths["--file"].toString();
  const auto configPath = paths["--config"].toString();
  const auto output = paths["--output"].toString();
  QFile configFile(configPath);
  check(configFile.open(QIODevice::ReadOnly) && configFile.size() <= 65536,
        "Config must be a readable JSON file at most 64 KiB");
  const auto configBytes = configFile.readAll();
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(configBytes, &parse);
  check(parse.error == QJsonParseError::NoError && document.isObject(),
        "Invalid parity config JSON");
  const auto config = document.object();
  check(config["schema"] == "signalstudio-sample-energy-parity-config-v1",
        "Unknown parity config schema");
  const auto begin = exactSample(config, "startSample");
  const auto end = exactSample(config, "endSample");
  const auto rate = number(config, "sampleRateHz");
  const auto center = number(config, "centerFrequencyHz");
  check(config["bandRFHz"].isObject(), "bandRFHz must be an RF band object");
  const auto band = config["bandRFHz"].toObject();
  const FrequencyRange frequency{number(band, "lowerHz"),
                                 number(band, "upperHz")};
  DetectionParameters parameters;
  parameters.smoothSeconds = number(config, "smoothSeconds");
  parameters.sampleEdgeRatio = optionalNumber(config, "sampleEdgeRatio", .6);
  parameters.minOnSeconds = optionalNumber(config, "minOnSeconds", .005);
  parameters.minOffSeconds = optionalNumber(config, "minOffSeconds", .010);
  if (config.contains("splitBursts")) {
    check(config["splitBursts"].isBool(), "splitBursts must be boolean");
    parameters.splitBursts = config["splitBursts"].toBool();
  }
  const auto coverage = optionalNumber(config, "windowCoverage", 0);
  check(begin < end && rate > 0 && frequency.upperHz > frequency.lowerHz &&
            frequency.upperHz - frequency.lowerHz <= rate &&
            frequency.lowerHz >= center - rate / 2 &&
            frequency.upperHz <= center + rate / 2 &&
            parameters.smoothSeconds >= 0 && parameters.smoothSeconds <= 60 &&
            parameters.minOnSeconds >= 0 && parameters.minOnSeconds <= 60 &&
            parameters.minOffSeconds >= 0 && parameters.minOffSeconds <= 60 &&
            parameters.sampleEdgeRatio >= 0 &&
            parameters.sampleEdgeRatio <= 1 && coverage >= 0 && coverage <= 1,
        "Invalid source interval, RF band or refinement parameters");
  const auto count = end - begin;
  check(static_cast<long double>(count) * 12 + 2097152 <= kParityBudget &&
            count <= std::numeric_limits<std::size_t>::max(),
        "Exact source interval exceeds the fixed 256 MiB working budget");
  QString error;
  WavDescriptor wav;
  check(readWavDescriptor(source, wav, error), error);
  check(rate == wav.sampleRateHz && end <= wav.sampleCount,
        "Config rate or source interval differs from PCM16-IQ WAV");
  RawSampleReader reader;
  check(reader.open(source, wav.format, error), error);
  check(reader.sampleCount() == wav.sampleCount, "PCM16-IQ count mismatch");
  std::vector<std::complex<float>> samples(static_cast<std::size_t>(count));
  for (auto first = begin; first < end;) {
    const auto after = first + std::min<std::uint64_t>(131072, end - first);
    std::vector<std::complex<float>> block;
    check(reader.read({first, after}, block, {}, &error), error);
    std::copy(block.begin(), block.end(), samples.begin() + (first - begin));
    first = after;
  }
  std::vector<float> power;
  check(detectionBandEnergy(
            std::move(samples), rate,
            (frequency.lowerHz + frequency.upperHz) / 2 - center,
            frequency.upperHz - frequency.lowerHz, power, kParityBudget, error),
        error);
  check(QDir().mkpath(output), "Cannot create output directory");
  const auto powerPath = QDir(output).filePath("native-power.f32");
  const auto smoothedPath = QDir(output).filePath("native-smoothed.f32");
  writeFloats(powerPath, power);
  const auto smooth = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(parameters.smoothSeconds * rate));
  {
    std::vector<float> smoothed(power.size());
    double running = 0;
    for (std::size_t i = 0; i < power.size(); ++i) {
      running += power[i];
      if (i >= smooth) running -= power[i - smooth];
      smoothed[i] = static_cast<float>(running / smooth);
    }
    writeFloats(smoothedPath, smoothed);
  }
  DetectionRefinementInput input;
  input.candidateId = "same-source-energy-parity";
  input.observation = {{begin, end}, frequency};
  input.publishBounds = input.observation;
  input.sampleRateHz = rate;
  input.windowCoverage = coverage;
  input.energy.emplace();
  input.energy->originSample = begin;
  input.energy->supportEndSample = end;
  input.energy->filterSupportSamples = count;
  input.energy->energyMethod =
      "cpu-exact-length-complex64-raised-cosine-fft-v1";
  input.energy->power = std::move(power);
  DetectionRefinementOptions options;
  options.workingCellBudget = static_cast<std::size_t>(count);
  const auto refined = refineDetection(input, parameters, options);
  check(refined.ok, QString::fromStdString(refined.error));
  QJsonArray segments;
  for (const auto& segment : refined.activity)
    segments.append(
        QJsonObject{{"startSample", QString::number(segment.range.time.begin)},
                    {"endSample", QString::number(segment.range.time.end)}});
  const auto& evidence = refined.evidence;
  QJsonObject native{
      {"status",
       refined.status == RefineStatus::Applied ? "applied" : "fallback"},
      {"fallbackReason", QString::fromStdString(refined.fallbackReason)},
      {"numericalVersion", QString::fromStdString(evidence.numericalVersion)},
      {"energyMethod", QString::fromStdString(evidence.energyMethod)},
      {"smoothingNumericalVersion", "native-f64-causal-rolling-f32-storage-v1"},
      {"smoothedExport", "same arithmetic loop as domain refinement"},
      {"smoothingSamples", QString::number(evidence.smoothingSamples)},
      {"minOnSamples", QString::number(evidence.minOnSamples)},
      {"minOffSamples", QString::number(evidence.minOffSamples)},
      {"continuousTrack", evidence.continuousTrack},
      {"splittingApplied", evidence.splittingApplied},
      {"effectiveEdgeRatio", evidence.effectiveEdgeRatio},
      {"durationParametersApplied", evidence.durationParametersApplied},
      {"segments", segments}};
  const auto add = [&](const char* key, std::optional<double> value) {
    if (value) native[key] = *value;
  };
  add("noise", evidence.noise);
  add("burst", evidence.burst);
  add("threshold", evidence.threshold);
  add("comparisonThreshold", evidence.comparisonThreshold);
  add("onFraction", evidence.onFraction);
  const QJsonObject report{
      {"schema", "signalstudio-native-sample-energy-parity-v1"},
      {"config", config},
      {"configPath", configPath},
      {"configSha256", hashFile(configPath)},
      {"sourcePath", source},
      {"sourceSha256", hashFile(source)},
      {"sourcePhysicalSamples", QString::number(wav.sampleCount)},
      {"sourceStartSample", QString::number(begin)},
      {"sourceEndSample", QString::number(end)},
      {"fftSamples", QString::number(count)},
      {"workingByteBudget", QString::number(kParityBudget)},
      {"executablePath", QCoreApplication::applicationFilePath()},
      {"executableSha256", hashFile(QCoreApplication::applicationFilePath())},
      {"power", artifact(powerPath, "float32-little-endian", count)},
      {"smoothed", artifact(smoothedPath, "float32-little-endian", count)},
      {"nativeRefinement", native}};
  writeJson(QDir(output).filePath("native-energy.json"), report);
  std::cout << "Native exact source energy exported: " << count << " samples\n";
  return 0;
}
void arbitraryTransform() {
  QString error;
  for (const auto size : {1U, 2U, 15U, 17U, 30U, 77U, 103U, 320U}) {
    std::vector<std::complex<float>> values(size);
    for (std::size_t i = 0; i < size; ++i)
      values[i] = {static_cast<float>(std::sin(.23 * i)),
                   static_cast<float>(std::cos(.17 * i))};
    const auto input = values;
    require(detectionComplexTransform(values, false, 8 * 1024 * 1024, error),
            "Forward FFT failed");
    for (std::size_t k = 0; k < size; ++k) {
      std::complex<double> expected{};
      for (std::size_t i = 0; i < size; ++i)
        expected += std::complex<double>(input[i]) *
                    std::polar(1.0, -2 * std::numbers::pi * i * k / size);
      require(
          std::abs(std::complex<double>(values[k]) - expected) < .00002 * size,
          "Arbitrary-length FFT differs from direct DFT");
    }
    require(detectionComplexTransform(values, true, 8 * 1024 * 1024, error),
            "Inverse FFT failed");
    for (std::size_t i = 0; i < size; ++i)
      require(std::abs(values[i] - input[i]) < .00002, "FFT roundtrip differs");
  }
}
void physicalEnergy() {
  constexpr std::size_t size = 1000;
  std::vector<std::complex<float>> input(size);
  for (std::size_t i = 0; i < size; ++i) {
    const auto tone = [&](double bin) {
      const double angle = 2 * std::numbers::pi * i * bin / size;
      return std::complex<float>(std::cos(angle), std::sin(angle));
    };
    input[i] = tone(100) + .7f * tone(300);
  }
  QString error;
  std::vector<float> output;
  require(
      detectionBandEnergy(input, 1000, 100, 80, output, 8 * 1024 * 1024, error),
      "Physical energy failed");
  require(output.size() == size, "Sample filtering changed exact length");
  for (const auto value : output)
    require(std::abs(value - 1) < .0001,
            "Band energy retained an out-of-band tone");
  output = {23};
  require(!detectionBandEnergy(input, 1000, 100, 80, output, 1, error),
          "Memory budget must reject");
  require(output == std::vector<float>{23}, "Rejected energy changed output");
  require(!detectionBandEnergy(input, 1000, 100, 80, output, 8 * 1024 * 1024,
                               error, [] { return true; }),
          "Cancellation must reject");
  require(output == std::vector<float>{23}, "Cancelled energy changed output");
}
}  // namespace
void boundedPowerOfTwoBackend() {
  for(const std::size_t size:{1U,2U,8U,64U,1024U,8192U,65536U}) {
    std::vector<std::complex<float>> source(size);
    for(std::size_t i=0;i<size;++i) source[i]={float(std::cos(2*std::numbers::pi*7*i/size)),
        float(.3*std::sin(2*std::numbers::pi*13*i/size))};
    auto exact=source,fast=source;QString error;
    require(detectionComplexTransform(exact,false,8*1024*1024,error),"Legacy power-of-two reference failed");
    require(detectionBoundedRadixTwoTransform(fast,false,8*1024*1024,error),"Bounded radix-two forward failed");
    for(std::size_t i=0;i<size;++i)
      require(std::abs(fast[i]-exact[i])/size<=2e-6,"New FIR FFT differs from native reference beyond normalized tolerance");
    require(detectionBoundedRadixTwoTransform(fast,true,8*1024*1024,error),"Bounded radix-two inverse failed");
    for(std::size_t i=0;i<size;++i)
      require(std::abs(fast[i]-source[i])<=3e-6,"Bounded radix-two roundtrip failed");
  }
  std::vector<std::complex<float>> values(3,{1,2});const auto original=values;QString error;
  require(!detectionBoundedRadixTwoTransform(values,false,8*1024*1024,error) && values==original,
      "Non-power-of-two FIR plan must reject without changing input");
  values.resize(2);const auto before=values;
  require(!detectionBoundedRadixTwoTransform(values,false,1,error) && values==before,"FFT budget rejection changed input");
  require(!detectionBoundedRadixTwoTransform(values,false,8*1024*1024,error,[]{return true;}) && values==before,
      "Pre-cancelled FFT changed input");
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    if (argc > 1) return fileParity(app.arguments());
    arbitraryTransform();
    physicalEnergy();
    boundedPowerOfTwoBackend();
    std::cout << "Exact-length sample FFT/energy checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
