#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "application/annotation_service.h"
#include "infrastructure/detection_frequency_analysis.h"

using namespace signalstudio;
namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void success(bool condition, const QString& error) {
  if (!condition) throw std::runtime_error(error.toStdString());
}
bool near(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}
FileMetadata writeSource(QTemporaryDir& directory, std::uint64_t count = 130) {
  const auto path = directory.filePath("frequency-evidence.raw");
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), "cannot create source fixture");
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto phase = 2 * std::numbers::pi * i / 16;
    const float amplitude = static_cast<float>(.01 + .003 * (i / 10));
    const std::complex<float> sample{
        amplitude * static_cast<float>(std::cos(phase)),
        amplitude * static_cast<float>(std::sin(phase))};
    char bytes[8];
    qToLittleEndian(std::bit_cast<std::uint32_t>(sample.real()), bytes);
    qToLittleEndian(std::bit_cast<std::uint32_t>(sample.imag()), bytes + 4);
    check(file.write(bytes, 8) == 8, "cannot write source fixture");
  }
  file.close();
  FileMetadata metadata;
  metadata.id = "frequency-file";
  metadata.path = path.toStdString();
  metadata.sampleRateHz = 10000;
  metadata.sampleCount = count;
  metadata.effectiveBandwidthHz = 10000;
  metadata.demo = false;
  metadata.sampleFormat.componentEncoding = ComponentEncoding::Float32;
  metadata.sampleFormat.normalizeIntegerAdc = false;
  return metadata;
}
ViewRange fixtureFullRange(const FileMetadata& metadata) {
  return {{0, metadata.sampleCount}, {-5000, 5000}};
}
DetectionSourceConfig sourceConfig() {
  DetectionSourceConfig config;
  config.profileId = "fixed-grid-fixture-v1";
  config.fftSize = 64;
  return config;
}
DetectionRequest requestFor(
    const FileMetadata& metadata,
    DetectionBackendKind backend = DetectionBackendKind::Fixture) {
  DetectionRequest request;
  request.runId = "frequency-run";
  request.sourceIdentity = annotationSourceIdentity(metadata, 0);
  request.profile.id = "fixed-grid-fixture-v1";
  request.profile.backend = backend;
  request.profile.fftSize = 64;
  request.requestedRange = fixtureFullRange(metadata);
  request.contextRange = request.requestedRange;
  return request;
}
FrequencyTrack trackFor(const DetectionRequest& request, TimeRange support,
                        FrequencyRange frequency = {-1500, 1500},
                        const std::string& id = "frequency-track") {
  FrequencyTrack track;
  track.seedId = id;
  track.runId = request.runId;
  track.fileId = request.sourceIdentity.fileId;
  track.frequency = track.originalFrequency = track.originalFrequencyEnvelope =
      frequency;
  track.nominalSupport = track.actualSupport = support;
  return track;
}
double percentileTen(std::vector<float> values) {
  std::sort(values.begin(), values.end());
  const auto rank = .1 * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(rank));
  const auto upper = static_cast<std::size_t>(std::ceil(rank));
  const double gamma = rank - std::floor(rank);
  const float difference = values[upper] - values[lower];
  const float product =
      difference * static_cast<float>(gamma >= .5 ? 1 - gamma : gamma);
  return gamma >= .5 ? values[upper] - product : values[lower] + product;
}
std::vector<float> spectra(FixedDetectionSource& provider, std::uint64_t first,
                           std::uint64_t end) {
  QString error;
  std::vector<float> values;
  for (auto index = first; index < end; ++index) {
    DetectionSpectrumFrame frame;
    success(provider.readFrame(index, frame, error), error);
    values.insert(values.end(), frame.densityDb.begin(), frame.densityDb.end());
  }
  return values;
}
void guiThresholdAndConservativeWidth() {
  std::vector<double> mean(256, -90);
  mean[90] = -60;
  DetectionFrequencyEvidence evidence;
  QString error;
  success(refineGuiFrequencySpectrum(mean, {40.1, 140}, 0, 1, -90, 6, evidence,
                                     error),
          error);
  check(evidence.originalFirstBin == 41 && evidence.originalLastBin == 140,
        "GUI uses searchsorted centres rather than floor edges");
  check(evidence.shrunk && evidence.frequency == FrequencyRange{84.5, 94.5},
        "single peak retains conservative ten-percent original bandwidth");
  check(evidence.measuredFirstBin == 90 && evidence.measuredLastBin == 90,
        "measured evidence remains separate from conservative final span");
  std::fill(mean.begin(), mean.end(), -90);
  std::fill(mean.begin() + 60, mean.begin() + 110, -60);
  success(refineGuiFrequencySpectrum(mean, {40.1, 140}, 0, 1, -90, 6, evidence,
                                     error),
          error);
  check(!evidence.shrunk &&
            evidence.skippedReason == "measured-width-at-least-half-original",
        "width exactly half cannot shrink");
  check(evidence.frequency == FrequencyRange{40.5, 140.5},
        "unshrunk original uses bin-centre half-bin edges");
  std::fill(mean.begin(), mean.end(), -90);
  mean[90] = -84;
  success(refineGuiFrequencySpectrum(mean, {40.1, 140}, 0, 1, -90, 6, evidence,
                                     error),
          error);
  check(!evidence.shrunk &&
            evidence.skippedReason == "peak-does-not-strictly-exceed-threshold",
        "GUI peak threshold is strictly greater than");
  mean[41] = -60;
  success(refineGuiFrequencySpectrum(mean, {40.1, 140}, 0, 1, -90, 6, evidence,
                                     error),
          error);
  check(evidence.frequency == FrequencyRange{40.5, 50.5},
        "conservative width stays inside mapped original edge");
  success(refineGuiFrequencySpectrum(mean, {40.1, 140}, 0, 1, -90, 0, evidence,
                                     error),
          error);
  check(evidence.frequency == FrequencyRange{40.1, 140} &&
            !evidence.hasNoiseFloor && !evidence.applied &&
            evidence.skippedReason ==
                "nonpositive-margin-disables-gui-refinement",
        "zero margin preserves GUI disabled-refinement semantics");
}
void globalPercentileCountsUniqueFrames() {
  QTemporaryDir directory;
  auto metadata = writeSource(directory);
  FixedDetectionSource provider;
  QString error;
  success(provider.open(metadata, fixtureFullRange(metadata), sourceConfig(),
                        error),
          error);
  check(provider.plan().eligibleWindowCount == 2 &&
            provider.plan().anchoredTailWindow,
        "fixture must exercise overlapping anchored tail window");
  auto request = requestFor(metadata);
  std::vector tracks{trackFor(request, {0, 130})};
  std::vector<DetectionFrequencyEvidence> evidence;
  const auto expectedFloor = percentileTen(spectra(provider, 0, 13));
  auto repeated = spectra(provider, 0, 10);
  const auto tail = spectra(provider, 3, 13);
  repeated.insert(repeated.end(), tail.begin(), tail.end());
  const auto wrongFloor = percentileTen(repeated);
  check(std::abs(expectedFloor - wrongFloor) > .01,
        "fixture must distinguish repeated-window percentile");
  success(applyDetectionFrequencyAnalysis(provider, request, tracks, evidence,
                                          error),
          error);
  check(evidence.size() == 1 && evidence[0].percentileSampleCount == 13 * 64 &&
            evidence[0].globalFrameCount == 13 && evidence[0].frameCount == 13,
        "global floor and track mean use unique real frames");
  check(near(evidence[0].noiseFloorDb, expectedFloor),
        "radix percentile matches sorted linear reference");
  check(evidence[0].firstFrame == 0 && evidence[0].lastFrameExclusive == 13 &&
            evidence[0].meanSpectrumSha256.size() == 64,
        "physical frame references and mean spectrum hash retained");
  check(tracks[0].nominalSupport == TimeRange{0, 130} &&
            tracks[0].actualSupport == TimeRange{0, 130},
        "frequency analysis cannot invent or revise emission timing");
}
void requestedContextAndShortTrackFallback() {
  QTemporaryDir directory;
  auto metadata = writeSource(directory, 400);
  auto request = requestFor(metadata);
  request.requestedRange.time = {150, 180};
  FixedDetectionSource provider;
  QString error;
  success(
      provider.open(metadata, request.requestedRange, sourceConfig(), error),
      error);
  check(provider.plan().firstRegularWindow == 2 &&
            provider.plan().eligibleRegularWindows == 2,
        "fixture covers two context windows starting at frame ten");
  const auto expected = percentileTen(spectra(provider, 10, 25));
  std::vector tracks{trackFor(request, {155, 175})};
  const auto original = tracks[0].frequency;
  std::vector<DetectionFrequencyEvidence> evidence;
  success(applyDetectionFrequencyAnalysis(provider, request, tracks, evidence,
                                          error),
          error);
  check(evidence[0].firstFrame == 16 && evidence[0].lastFrameExclusive == 18 &&
            evidence[0].globalFrameCount == 15,
        "track support follows searchsorted sample grid in eligible context");
  check(near(evidence[0].noiseFloorDb, expected),
        "global floor does not include unrelated file frames");
  check(tracks[0].frequency == original && !evidence[0].applied &&
            evidence[0].skippedReason == "fewer-than-three-real-context-frames",
        "short context remains explicit original-frequency fallback");
}
void scnUsesUniqueMeanNoiseAndMaximumGate() {
  QTemporaryDir directory;
  auto metadata = writeSource(directory);
  auto request = requestFor(metadata, DetectionBackendKind::Scn);
  FixedDetectionSource provider;
  QString error;
  success(provider.open(metadata, fixtureFullRange(metadata), sourceConfig(),
                        error),
          error);
  std::vector tracks{trackFor(request, {0, 130}, {-600, 1250}),
                     trackFor(request, {0, 130}, {-1500, -650}, "occupied")};
  const auto values = spectra(provider, 0, 13);
  std::vector<float> average(64), maximum(64, -1000);
  for (std::size_t bin = 0; bin < 64; ++bin) {
    double sum = 0;
    for (std::size_t frame = 0; frame < 13; ++frame) {
      sum += values[frame * 64 + bin];
      maximum[bin] = std::max(maximum[bin], values[frame * 64 + bin]);
    }
    average[bin] = static_cast<float>(sum / 13);
  }
  std::vector<FrequencyRange> occupancy;
  for (const auto& track : tracks) occupancy.push_back(track.frequency);
  std::vector<ScnFrequencyRefinement> expected;
  std::string domainError;
  check(refineScnFrequencyBands(occupancy, occupancy, average, maximum, -5000,
                                10000.0 / 64, 6, expected, domainError),
        domainError.c_str());
  std::vector<DetectionFrequencyEvidence> evidence;
  success(applyDetectionFrequencyAnalysis(provider, request, tracks, evidence,
                                          error),
          error);
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    check(tracks[i].frequency == expected[i].frequency &&
              evidence[i].noiseSampleCount == expected[i].noiseSampleCount &&
              near(evidence[i].noiseFloorDb, expected[i].noiseFloorDb),
          "SCN source frequency analysis uses all fused occupancy and mean "
          "minimum");
    check(evidence[i].maximumSpectrumSha256.size() == 64 &&
              evidence[i].percentileSampleCount == 0 &&
              evidence[i].frameCount == 13,
          "SCN maximum evidence stays separate from GUI percentile and mean");
  }
}
void failureAndCancellationPublishNothing() {
  QTemporaryDir directory;
  auto metadata = writeSource(directory);
  auto request = requestFor(metadata);
  FixedDetectionSource provider;
  QString error;
  success(provider.open(metadata, fixtureFullRange(metadata), sourceConfig(),
                        error),
          error);
  std::vector tracks{trackFor(request, {0, 130})};
  const auto original = tracks[0].frequency;
  DetectionFrequencyEvidence sentinel;
  sentinel.seedId = "unchanged";
  std::vector evidence{sentinel};
  int checks = 0;
  check(!applyDetectionFrequencyAnalysis(provider, request, tracks, evidence,
                                         error, [&] { return ++checks > 30; }),
        "mid-DSP cancellation must fail");
  check(error.contains("取消") || error.contains("cancelled"),
        "cancellation explains the failed measurement");
  check(evidence == std::vector{sentinel} && tracks[0].frequency == original,
        "cancelled measurement cannot publish partial frequency results");
  request.evidenceBudgetBytes = 1;
  check(!applyDetectionFrequencyAnalysis(provider, request, tracks, evidence,
                                         error),
        "insufficient evidence budget must fail before work");
  check(error == "frequency-analysis-resident-budget-exceeded" &&
            evidence == std::vector{sentinel},
        "budget failure leaves caller evidence untouched");
  request.evidenceBudgetBytes = 32 * 1024 * 1024;
  tracks[0].runId = "foreign-run";
  check(!applyDetectionFrequencyAnalysis(provider, request, tracks, evidence,
                                         error),
        "cross-run tracks cannot borrow measurement context");
  check(error == "frequency-analysis-invalid-track-scope-or-support",
        "cross-run scope failure is explicit");
  std::vector<double> bad(64, -90);
  bad[32] = std::numeric_limits<double>::quiet_NaN();
  auto pureOutput = sentinel;
  check(!refineGuiFrequencySpectrum(bad, {0, 40}, 0, 1, -90, 6, pureOutput,
                                    error),
        "nonfinite measurement must fail");
  check(pureOutput == sentinel, "pure failure is atomic too");
}
void excludedShortInputHasNoFrequencyResults() {
  QTemporaryDir directory;
  auto metadata = writeSource(directory, 90);
  auto request = requestFor(metadata);
  FixedDetectionSource provider;
  QString error;
  success(provider.open(metadata, fixtureFullRange(metadata), sourceConfig(),
                        error),
          error);
  check(provider.plan().eligibleWindowCount == 0,
        "short fixture must lack complete real model context");
  std::vector<FrequencyTrack> tracks;
  std::vector evidence{DetectionFrequencyEvidence{}};
  success(applyDetectionFrequencyAnalysis(provider, request, tracks, evidence,
                                          error),
          error);
  check(evidence.empty() && tracks.empty(),
        "excluded short input completes without invented frequency evidence");
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    guiThresholdAndConservativeWidth();
    globalPercentileCountsUniqueFrames();
    requestedContextAndShortTrackFallback();
    scnUsesUniqueMeanNoiseAndMaximumGate();
    failureAndCancellationPublishNothing();
    excludedShortInputHasNoFrequencyResults();
    std::cout << "frequency analysis checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
