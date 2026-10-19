#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "infrastructure/detection_model_backend.h"

using namespace signalstudio;

namespace {
void check(bool condition, const QString& message) {
  if (!condition) throw std::runtime_error(message.toStdString());
}
DetectionRequest request(const std::string& profile) {
  DetectionRequest result;
  result.profile = *findDetectionProfile(profile);
  result.parameters.batchSize = 2;
  if (profile == "ffscn-gui-iq-v1")
    result.modelSha256 =
        "d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81";
  else if (profile == "scn-max-psd-v1")
    result.modelSha256 =
        "ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627";
  return result;
}

void maximumOnlyInput() {
  auto snapshot = request("scn-max-psd-v1");
  const std::size_t bins = snapshot.profile.fftSize;
  const std::size_t frames = snapshot.profile.modelFrames;
  std::vector<float> spectra(2 * frames * bins, -100);
  spectra[3] = -96;
  spectra[bins + 3] = -98;
  spectra[bins + 4] = -92;
  const auto second = frames * bins;
  spectra[second + 8] = -90;
  spectra[second + bins + 9] = -95;
  std::vector<float> actual{42};
  QString error;
  check(prepareDetectionModelInput(snapshot, spectra, 2, actual, error), error);
  check(
      actual.size() == 2 * bins && actual[3] == .5f && actual[4] == 1 &&
          actual[bins + 8] == 1 && actual[bins + 9] == .5f,
      "SCN must normalize maxima independently per batch, never mean spectra");
  check(actual[0] == 0 && actual[bins] == 0,
        "SCN maximum-spectrum minimum is exactly zero");
  spectra.assign(frames * bins, -100);
  check(prepareDetectionModelInput(snapshot, spectra, 1, actual, error) &&
            std::all_of(actual.begin(), actual.end(),
                        [](float value) { return value == 0; }),
        "Constant SCN maxima must produce finite zero minmax input");
}

void shapeFiniteAndBudget() {
  auto snapshot = request("ffscn-gui-iq-v1");
  std::vector<float> input(
      snapshot.profile.fftSize * snapshot.profile.modelFrames, .125f);
  std::vector<float> output{42};
  QString error;
  check(prepareDetectionModelInput(snapshot, input, 1, output, error) &&
            output == input,
        "FFSCN normalized frames must pass through without another transform");
  output = {42};
  check(!prepareDetectionModelInput(snapshot, input, 0, output, error) &&
            output == std::vector<float>{42},
        "Zero batch must fail atomically");
  snapshot.workingSetBudgetBytes = 1;
  check(!prepareDetectionModelInput(snapshot, input, 1, output, error) &&
            output == std::vector<float>{42},
        "Input copies must respect the requested working-set budget");
  snapshot.workingSetBudgetBytes = 256 * 1024 * 1024;
  input[1] = std::numeric_limits<float>::quiet_NaN();
  check(!prepareDetectionModelInput(snapshot, input, 1, output, error),
        "NaN may not enter model inference");
  input[1] = std::numeric_limits<float>::infinity();
  check(!prepareDetectionModelInput(snapshot, input, 1, output, error),
        "Infinity may not enter model inference");
  input[1] = 0;
  input.pop_back();
  check(!prepareDetectionModelInput(snapshot, input, 1, output, error),
        "A shortened tensor must fail before allocation/forward");
  snapshot.profile.modelFrames = 9;
  check(!prepareDetectionModelInput(snapshot, input, 1, output, error),
        "Actual input bytes must match the changed analysis shape");
  input.assign(9 * snapshot.profile.fftSize, .125f);
  check(prepareDetectionModelInput(snapshot, input, 1, output, error),
        "Nine real frames are valid independently of reference defaults");
}

void adaptableInputGrids() {
  auto snapshot = request("ffscn-gui-iq-v1");
  snapshot.profile.fftSize = 32;
  snapshot.profile.modelFrames = 10;
  std::vector<float> input(3 * 32, .25f), output;
  QString error;
  check(prepareDetectionModelInput(snapshot, input, 1, output, error, 3),
        error);
  check(output.size() == 3 * 2048 &&
            std::all_of(output.begin(), output.end(),
                        [](float value) { return value == .25f; }) &&
            detectionModelBinScale(snapshot) == 1.0 / 64,
        "FFSCN interpolates the full physical band without fabricating rows");
  snapshot.profile.fftSize = 3001;
  input.assign(3 * 3001, .25f);
  check(prepareDetectionModelInput(snapshot, input, 1, output, error, 3) &&
            output.size() == 3 * 4096 &&
            detectionModelInputBins(snapshot) == 4096 &&
            detectionModelBinScale(snapshot) == 3001.0 / 4096,
        "An irregular FFSCN grid must match a power of two above 2^down_nums");
  snapshot = request("scn-max-psd-v1");
  snapshot.profile.fftSize = 65536;
  snapshot.profile.modelFrames = 3;
  input.assign(3 * 65536, -100);
  input[0] = 0;
  input[2] = input[3] = 0;
  check(prepareDetectionModelInput(snapshot, input, 1, output, error), error);
  check(output.size() == 32768 && std::abs(output[0] - .969897f) < 1e-5 &&
            output[1] == 1 && output[2] == 0 &&
            detectionModelBinScale(snapshot) == 2,
        "SCN preserves integrated linear power and maps model bins to source "
        "bins");
  snapshot.profile.fftSize = 64;
  input.assign(3 * 64, -100);
  input[2 * 64 + 7] = -90;
  check(prepareDetectionModelInput(snapshot, input, 1, output, error), error);
  check(output[7 * 512] == 1 && output[8 * 512 - 1] == 1 &&
            output[8 * 512] == 0 &&
            detectionModelBinScale(snapshot) == 1.0 / 512,
        "SCN expands the frequency grid while keeping actual-frame maximum "
        "semantics");
}

void selectedSpectrumInputs() {
  QString error;
  for (const auto* profile : {"ffscn-gui-iq-v1", "scn-max-psd-v1"}) {
    auto snapshot = request(profile);
    snapshot.profile.fftSize = 32;
    snapshot.profile.modelFrames = 2;
    std::vector<float> input(64, -100), output;
    input[1] = input[33] = -40;
    input[2] = 0;
    const auto modelBins = detectionModelInputBins(snapshot);
    const auto first = modelBins / 32;
    const auto second = 2 * first;
    for (auto mode :
         {DetectionSpectrumMode::Mean, DetectionSpectrumMode::Maximum,
          DetectionSpectrumMode::Minimum, DetectionSpectrumMode::Current}) {
      snapshot.spectrumMode = mode;
      check(prepareDetectionModelInput(snapshot, input, 1, output, error),
            error);
      check(output.size() == modelBins &&
                std::all_of(output.begin(), output.end(),
                            [](float value) { return std::isfinite(value); }),
            "Every selected spectrum is one real finite model input");
      if (mode == DetectionSpectrumMode::Mean ||
          mode == DetectionSpectrumMode::Maximum)
        check(output[second] > output[first],
              "Mean and maximum must include the transient in the first row");
      else
        check(output[first] > output[second],
              "Minimum and current must use their selected observations");
      if (snapshot.profile.backend == DetectionBackendKind::Scn &&
          mode == DetectionSpectrumMode::Mean)
        check(std::abs(output[first] - 60.0 / (100 - 10 * std::log10(2.0))) <
                  1e-6,
              "Average detection spectrum must average linear power, not dB");
    }
  }
}

void separateMemoryBudgets() {
  constexpr std::uint64_t mib = 1024 * 1024;
  constexpr std::uint64_t gib = 1024 * mib;
  const DetectionModelResourceLimits limits;
  DetectionModelMemoryUsage usage{12 * mib, 21 * mib, 2 * gib,
                                  gib,      4 * gib,  8 * gib};
  QString error;
  check(validateDetectionModelMemory(usage, 256 * mib, limits, error),
        "A valid GPU working set must not consume the independent CPU budget");
  usage.hostWorkingSetBytes = 257 * mib;
  check(!validateDetectionModelMemory(usage, 256 * mib, limits, error) &&
            error.contains("CPU"),
        "GPU capacity may not hide an exceeded CPU intermediate budget");
  usage.hostWorkingSetBytes = 12 * mib;
  usage.engineBlobBytes = 129 * mib;
  check(!validateDetectionModelMemory(usage, 256 * mib, limits, error) &&
            error.contains("asset budget"),
        "Model blob is bounded separately from CPU cache and GPU memory");
  usage.engineBlobBytes = 21 * mib;
  auto small = limits;
  small.maximumDeviceBytes = 2 * gib;
  check(!validateDetectionModelMemory(usage, 256 * mib, small, error) &&
            error.contains("device budget"),
        "An explicit small device budget must reject runtime plus allocations");
  usage.freeDeviceBytes = gib - 1;
  check(!validateDetectionModelMemory(usage, 256 * mib, limits, error) &&
            error.contains("available VRAM"),
        "Device budget never relaxes the actual free-VRAM guard");
  usage.freeDeviceBytes = gib;
  check(validateDetectionModelMemory(usage, 256 * mib, limits, error),
        "The exact available-allocation boundary is accepted");
  usage.freeDeviceBytes = 9 * gib;
  check(!validateDetectionModelMemory(usage, 256 * mib, limits, error),
        "CUDA free memory cannot exceed total physical memory");
  usage.freeDeviceBytes = 4 * gib;
  usage.runtimeDeviceBytes = std::numeric_limits<std::uint64_t>::max();
  check(!validateDetectionModelMemory(usage, 256 * mib, limits, error),
        "Runtime plus allocation overflow cannot evade the device limit");
}

QJsonObject fakeManifest() {
  return {{"schema", "signalstudio-detection-model-package-v1"},
          {"profileId", "ffscn-gui-iq-v1"},
          {"profileVersion", 1},
          {"precision", "FP32"},
          {"engine", QJsonObject{{"path", "model.engine"},
                                 {"bytes", 4},
                                 {"sha256",
                                  "d2de250b11d6bc4a3ccec6d95a232b03ba011e085698"
                                  "d525c3454387094bfe81"}}},
          {"runtime", QJsonObject{{"tensorRt", "10.11.0.33"},
                                  {"cudaRuntime", 12090},
                                  {"gpu", "NVIDIA GeForce RTX 5060 Laptop GPU"},
                                  {"computeCapability", QJsonArray{12, 0}},
                                  {"hardwareCompatibility", "none"},
                                  {"versionCompatible", false}}},
          {"input", QJsonObject{{"name", "spectrum"},
                                {"dtype", "float32"},
                                {"layout", "contiguous-NCHW"},
                                {"minShape", QJsonArray{1, 1, 10, 65536}},
                                {"maxShape", QJsonArray{2, 1, 10, 65536}}}},
          {"heads",
           QJsonArray{QJsonObject{{"name", "hm"},
                                  {"semantic", "hm"},
                                  {"dtype", "float32"},
                                  {"shape", QJsonArray{0, 1, 1, 16384}}},
                      QJsonObject{{"name", "bw"},
                                  {"semantic", "bw"},
                                  {"dtype", "float32"},
                                  {"shape", QJsonArray{0, 1, 1, 16384}}},
                      QJsonObject{{"name", "off"},
                                  {"semantic", "off"},
                                  {"dtype", "float32"},
                                  {"shape", QJsonArray{0, 1, 1, 16384}}}}}};
}

void packageRejections() {
  QTemporaryDir temporary;
  check(temporary.isValid(), "Cannot create package rejection fixture");
  auto snapshot = request("ffscn-gui-iq-v1");
  QJsonObject checked{{"preserved", true}};
  QString error;
  check(!validateDetectionModelPackage(snapshot, temporary.path(), checked,
                                       error) &&
            checked == QJsonObject{{"preserved", true}},
        "Missing manifest must leave the caller's snapshot unchanged");
  const auto directory = QDir(temporary.path()).filePath("ffscn-gui-iq-v1");
  check(QDir().mkpath(directory), "Cannot create fixture profile directory");
  const auto write = [&](const QJsonObject& value) {
    QFile file(QDir(directory).filePath("manifest.json"));
    check(file.open(QIODevice::WriteOnly), "Cannot write fixture manifest");
    check(file.write(QJsonDocument(value).toJson()) > 0,
          "Manifest write failed");
  };
  auto original = fakeManifest();
  for (const auto& field : {"profileId", "profileVersion", "precision",
                            "runtime", "input", "heads"}) {
    auto mutated = original;
    mutated[field] = "tampered";
    write(mutated);
    check(!validateDetectionModelPackage(snapshot, temporary.path(), checked,
                                         error),
          "Manifest identity/type/shape tampering must be rejected");
  }
  auto traversal = original;
  auto engine = traversal["engine"].toObject();
  engine["path"] = "../model.engine";
  traversal["engine"] = engine;
  write(traversal);
  check(!validateDetectionModelPackage(snapshot, temporary.path(), checked,
                                       error),
        "Engine paths must be basenames within their profile package");
  write(original);
  snapshot.modelSha256.clear();
  check(!validateDetectionModelPackage(snapshot, temporary.path(), checked,
                                       error) &&
            error.contains("Immutable request model SHA"),
        "Production inference requires an explicit frozen model identity");
  snapshot = request("ffscn-gui-iq-v1");
  QFile file(QDir(directory).filePath("model.engine"));
  check(file.open(QIODevice::WriteOnly) && file.write("fake") == 4,
        "Cannot write modified engine fixture");
  file.close();
  check(!validateDetectionModelPackage(snapshot, temporary.path(), checked,
                                       error) &&
            error.contains("SHA-256"),
        "An engine must match its frozen SHA before deserialization");
  DetectionModelBackend backend(temporary.path());
  check(!backend.load(snapshot, error) && backend.evidence().isEmpty(),
        "Failed production preflight cannot expose a ready backend");
  DetectionModelOutput output;
  output.hm = {42};
  check(!backend.infer({}, 1, output, error) &&
            output.hm == std::vector<float>{42},
        "No backend may infer before successful preflight");
  snapshot = request("ffscn-17-experimental-v1");
  check(
      !backend.load(snapshot, error) && error.contains("unvalidated"),
      "Experimental 17-order profile is rejected rather than resized/fallback");
}

std::vector<float> readFloats(const QString& path) {
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "Cannot open golden tensor: " + path);
  const auto bytes = file.readAll();
  check(!bytes.isEmpty() && bytes.size() % sizeof(float) == 0,
        "Golden tensor byte count is invalid");
  std::vector<float> values(bytes.size() / sizeof(float));
  std::memcpy(values.data(), bytes.constData(), bytes.size());
  return values;
}

void adaptedLiveParity(const QString& modelRoot, const QString& goldenRoot) {
  QFile manifestFile(QDir(modelRoot).filePath("ffscn-gui-iq-v1/manifest.json"));
  check(manifestFile.open(QIODevice::ReadOnly),
        "Dynamic model package is absent");
  const auto manifest =
      QJsonDocument::fromJson(manifestFile.readAll()).object();
  for (const auto spec : {std::array{1, 1, 2048}, std::array{1, 3, 8192},
                          std::array{2, 10, 65536}, std::array{1, 32, 2048},
                          std::array{1, 1, 131072}}) {
    const auto [batch, frames, bins] = spec;
    auto snapshot = request("ffscn-gui-iq-v1");
    snapshot.modelSha256 =
        manifest["engine"].toObject()["sha256"].toString().toStdString();
    snapshot.profile.fftSize = bins;
    snapshot.profile.modelFrames = frames;
    const auto prefix =
        QString("shape-b%1-t%2-f%3").arg(batch).arg(frames).arg(bins);
    DetectionModelBackend backend(modelRoot);
    QString error;
    check(backend.load(snapshot, error), error);
    const auto input =
        readFloats(QDir(goldenRoot).filePath(prefix + ".input.f32"));
    DetectionModelOutput output;
    check(backend.infer(input, batch, output, error), error);
    check(output.actualFrames == frames && output.inputBins == bins &&
              output.width == bins / 4,
          "Actual dynamic output layout was not preserved");
    const std::array<const std::vector<float>*, 3> actual{
        &output.hm, &output.bw, &output.off};
    double maximum = 0;
    for (int head = 0; head < 3; ++head) {
      const auto expected = readFloats(
          QDir(goldenRoot)
              .filePath(prefix + "." + QStringList{"hm", "bw", "off"}[head] +
                        ".reference.f32"));
      check(expected.size() == actual[head]->size(),
            "Dynamic head length mismatch");
      for (std::size_t index = 0; index < expected.size(); ++index) {
        const double delta =
            std::abs(double((*actual[head])[index]) - expected[index]);
        maximum = std::max(maximum, delta);
        check(std::isfinite((*actual[head])[index]) &&
                  delta <= 1e-4 + 1e-3 * std::abs(expected[index]),
              "Dynamic native head differs from PyTorch: " + prefix);
      }
    }
    std::cout << prefix.toStdString() << " PASS maxAbsolute=" << maximum << ' '
              << QJsonDocument(backend.evidence())
                     .toJson(QJsonDocument::Compact)
                     .toStdString()
              << '\n';
  }
  auto varied = request("scn-max-psd-v1");
  varied.profile.fftSize = 65536;
  varied.profile.modelFrames = 3;
  std::vector<float> spectra(3 * 65536, -100), prepared;
  spectra[2 * 65536 + 19000] = -80;
  spectra[11000] = -85;
  QString error;
  check(prepareDetectionModelInput(varied, spectra, 1, prepared, error), error);
  DetectionModelBackend adapted(modelRoot);
  check(adapted.load(varied, error), error);
  DetectionModelOutput observed;
  check(adapted.infer(spectra, 1, observed, error), error);
  auto identity = request("scn-max-psd-v1");
  identity.profile.modelFrames = 1;
  DetectionModelBackend control(modelRoot);
  check(control.load(identity, error), error);
  DetectionModelOutput expected;
  check(control.infer(prepared, 1, expected, error), error);
  check(observed.hm == expected.hm && observed.bw == expected.bw &&
            observed.off == expected.off && observed.inputBins == 32768 &&
            observed.actualFrames == 3 &&
            adapted.evidence()["meanSpectrumInferenceCalls"].toInt() == 0,
        "SCN adapted real-frame maxima must execute exactly the mapped model "
        "input");
  std::cout << "SCN N65536/T3 versus mapped N32768/T1 PASS\n";
  for (const auto* profile : {"ffscn-gui-iq-v1", "scn-max-psd-v1"}) {
    for (const auto mode :
         {DetectionSpectrumMode::Current, DetectionSpectrumMode::Mean,
          DetectionSpectrumMode::Maximum, DetectionSpectrumMode::Minimum}) {
      auto selected = request(profile);
      if (selected.profile.backend != DetectionBackendKind::Scn)
        selected.modelSha256 =
            manifest["engine"].toObject()["sha256"].toString().toStdString();
      selected.profile.fftSize = 3001;
      selected.profile.modelFrames = 3;
      selected.spectrumMode = mode;
      std::vector<float> input(3 * 3001, -100);
      input[100] = -20;
      input[3001 + 101] = -40;
      input[6002 + 102] = -60;
      DetectionModelBackend backend(modelRoot);
      check(backend.load(selected, error), error);
      DetectionModelOutput output;
      check(backend.infer(input, 1, output, error), error);
      check(output.actualFrames == 3 &&
                output.inputBins == detectionModelInputBins(selected) &&
                backend.evidence()["lastTensorFrames"].toInt() == 1 &&
                backend.evidence()["meanSpectrumInferenceCalls"].toInt() ==
                    (mode == DetectionSpectrumMode::Mean ? 1 : 0),
            "Selected spectrum must reach the real model with actual support");
      for (const auto* head : {&output.hm, &output.bw, &output.off})
        check(head->size() == output.width &&
                  std::all_of(head->begin(), head->end(),
                              [](float value) { return std::isfinite(value); }),
              "Selected-spectrum real forward must return finite raw heads");
      std::cout << profile << " irregular-N3001 mode=" << static_cast<int>(mode)
                << " PASS "
                << QJsonDocument(backend.evidence())
                       .toJson(QJsonDocument::Compact)
                       .toStdString()
                << '\n';
    }
  }
  auto resourceRequest = request("ffscn-gui-iq-v1");
  resourceRequest.modelSha256 =
      manifest["engine"].toObject()["sha256"].toString().toStdString();
  const auto source =
      readFloats(QDir(goldenRoot).filePath("shape-b2-t10-f65536.input.f32"));
  std::vector<float> single(source.begin(), source.begin() + 10 * 65536);
  std::array<std::uint64_t, 2> required{};
  for (std::uint32_t batch = 1; batch <= 2; ++batch) {
    DetectionModelResourceLimits limits;
    limits.maximumExecutionBatch = batch;
    DetectionModelBackend backend(modelRoot, limits);
    check(backend.load(resourceRequest, error), error);
    const auto evidence = backend.evidence();
    required[batch - 1] =
        evidence["runtimeDeviceBytes"].toString().toULongLong() +
        evidence["boundedDeviceBytes"].toString().toULongLong();
    if (batch == 1) {
      DetectionModelOutput output;
      check(backend.infer(single, 1, output, error) && output.batch == 1,
            "Reduced execution batch must still execute the real model");
      output.hm = {42};
      check(!backend.infer(source, 2, output, error) &&
                output.hm == std::vector<float>{42} &&
                evidence["requestedMaximumBatch"] == 2 &&
                evidence["logicalMaximumBatch"] == 1,
            "Execution allocation cannot alter the request or accept B2 into "
            "B1 memory");
    }
  }
  check(required[1] > required[0], "B2 must reserve more memory than B1");
  DetectionModelResourceLimits constrained;
  constrained.maximumDeviceBytes = (required[0] + required[1]) / 2;
  DetectionModelBackend reduced(modelRoot, constrained);
  check(reduced.load(resourceRequest, error), error);
  DetectionModelOutput output;
  check(reduced.evidence()["logicalMaximumBatch"] == 1 &&
            !reduced.evidence()["batchAdaptationReason"].toString().isEmpty() &&
            reduced.infer(single, 1, output, error),
        "A real GPU budget between B1 and B2 must reduce batch before failing");
  std::cout << "Resource batch reduction PASS "
            << QJsonDocument(reduced.evidence())
                   .toJson(QJsonDocument::Compact)
                   .toStdString()
            << '\n';
}

void liveParity(const QString& modelRoot, const QString& referenceRoot) {
  auto snapshot = request("ffscn-gui-iq-v1");
  DetectionModelBackend backend(modelRoot);
  QString error;
  check(backend.load(snapshot, error), error);
  std::cout << "FFSCN resource evidence "
            << QJsonDocument(backend.evidence())
                   .toJson(QJsonDocument::Compact)
                   .toStdString()
            << '\n';
  DetectionModelResourceLimits tiny;
  tiny.maximumDeviceBytes = 1;
  DetectionModelBackend constrained(modelRoot, tiny);
  check(
      !constrained.load(snapshot, error) && error.contains("device budget") &&
          constrained.evidence().isEmpty(),
      "A constrained GPU backend must fail before inference without replacing "
      "the independently ready backend");
  for (const auto& kind : {"synthetic", "real-wav", "constant"}) {
    for (std::uint32_t batch : {1, 2}) {
      const auto prefix =
          QString::fromLatin1(kind) + "-b" + QString::number(batch);
      const auto path = [&](const QString& suffix) {
        return QDir(referenceRoot).filePath(prefix + suffix);
      };
      const auto input = readFloats(path(".input.f32"));
      DetectionModelOutput output;
      check(backend.infer(input, batch, output, error), error);
      double maximum = 0;
      const std::array<const std::vector<float>*, 3> actual{
          &output.hm, &output.bw, &output.off};
      for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto expected = readFloats(path(
            "." + QStringList{"hm", "bw", "off"}[index] + ".reference.f32"));
        check(expected.size() == actual[index]->size(),
              "Head golden shape differs");
        for (std::size_t item = 0; item < expected.size(); ++item) {
          const double difference = std::abs(
              static_cast<double>((*actual[index])[item]) - expected[item]);
          maximum = std::max(maximum, difference);
          check(difference <= 1e-4 + 1e-3 * std::abs(expected[item]),
                "Production head differs from independent PyTorch golden: " +
                    prefix);
        }
      }
      std::cout << prefix.toStdString() << " maximum absolute difference "
                << maximum << '\n';
    }
  }
  check(backend.evidence()["device"].toString() ==
            "NVIDIA GeForce RTX 5060 Laptop GPU",
        "Production evidence must identify the actual device");
  {
    int failHead = -1;
    DetectionModelRuntimeHooks hooks;
    hooks.failAfterQueuedOutput = [&](std::uint32_t index) {
      return static_cast<int>(index) == failHead;
    };
    DetectionModelBackend interrupted(modelRoot, {}, hooks);
    check(interrupted.load(snapshot, error), error);
    const auto input =
        readFloats(QDir(referenceRoot).filePath("constant-b1.input.f32"));
    for (failHead = 0; failHead < 3; ++failHead) {
      DetectionModelOutput preserved;
      preserved.hm = {42};
      preserved.bw = {43};
      preserved.off = {44};
      const auto before =
          interrupted.evidence()["streamDrainCount"].toString().toULongLong();
      check(!interrupted.infer(input, 1, preserved, error) &&
                error == "Injected model failure after queued output copy" &&
                preserved.hm == std::vector<float>{42} &&
                preserved.bw == std::vector<float>{43} &&
                preserved.off == std::vector<float>{44},
            "Each queued-output failure preserves the original error and "
            "caller output");
      check(
          interrupted.evidence()["streamDrainCount"].toString().toULongLong() ==
              before + 1,
          "Every asynchronous early return must drain before host buffers "
          "expire");
    }
    failHead = -1;
    DetectionModelOutput resumed;
    check(interrupted.infer(input, 1, resumed, error) && resumed.batch == 1 &&
              resumed.hm.size() == 16384,
          "The same backend may safely infer after drained injected failures");
    std::cout
        << "Queued-output failures drained before releasing host buffers\n";
  }
  snapshot = request("scn-max-psd-v1");
  check(backend.load(snapshot, error), error);
  std::cout << "SCN resource evidence "
            << QJsonDocument(backend.evidence())
                   .toJson(QJsonDocument::Compact)
                   .toStdString()
            << '\n';
  const auto elements = static_cast<std::size_t>(snapshot.profile.fftSize) *
                        snapshot.profile.modelFrames;
  std::vector<float> spectra(elements, -100);
  for (std::size_t frame = 0; frame < snapshot.profile.modelFrames; ++frame)
    for (std::size_t bin = 8000; bin < 9000; ++bin)
      spectra[frame * snapshot.profile.fftSize + bin] =
          -50 + static_cast<float>(frame);
  DetectionModelOutput one;
  check(backend.infer(spectra, 1, one, error), error);
  const auto duplicate = spectra;
  spectra.insert(spectra.end(), duplicate.begin(), duplicate.end());
  DetectionModelOutput two;
  check(backend.infer(spectra, 2, two, error), error);
  const std::array<const std::vector<float>*, 3> first{&one.hm, &one.bw,
                                                       &one.off};
  const std::array<const std::vector<float>*, 3> doubled{&two.hm, &two.bw,
                                                         &two.off};
  for (std::size_t index = 0; index < first.size(); ++index) {
    check(doubled[index]->size() == 2 * first[index]->size() &&
              std::equal(first[index]->begin(), first[index]->end(),
                         doubled[index]->begin()) &&
              std::equal(first[index]->begin(), first[index]->end(),
                         doubled[index]->begin() + first[index]->size()),
          "SCN logical B2 must execute the unchanged fixed-B1 maximum-spectrum "
          "engine twice");
  }
  check(backend.evidence()["engineMaximumBatch"].toInt() == 1 &&
            backend.evidence()["meanSpectrumInferenceCalls"].toInt() == 0,
        "SCN runtime must report fixed-B1 maximum-only execution");
  std::cout << "SCN maximum-only fixed-B1/logical-B2 check passed\n";
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    maximumOnlyInput();
    shapeFiniteAndBudget();
    adaptableInputGrids();
    selectedSpectrumInputs();
    separateMemoryBudgets();
    packageRejections();
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--live")
      liveParity(QString::fromLocal8Bit(argv[2]),
                 QString::fromLocal8Bit(argv[3]));
    else if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--adapted")
      adaptedLiveParity(QString::fromLocal8Bit(argv[2]),
                        QString::fromLocal8Bit(argv[3]));
    else
      check(argc == 1,
            "Usage: detection-model-backend-tests [--live|--adapted model-root "
            "golden-root]");
    std::cout << "Detection model backend checks passed\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
