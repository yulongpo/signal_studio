#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

#include "infrastructure/detection_sample_energy.h"
#include "infrastructure/detection_source.h"

using namespace signalstudio;

namespace {
double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}
FileMetadata makeSource(QTemporaryDir& directory, double fs, std::uint64_t count) {
  QFile file(directory.filePath("source.raw"));
  if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("fixture");
  QByteArray bytes;
  bytes.reserve(static_cast<qsizetype>(count * 8));
  for (std::uint64_t i = 0; i < count; ++i) {
    const double phase = 2 * 3.14159265358979323846 * 0.137 * static_cast<double>(i);
    const auto sample = std::complex<float>(static_cast<float>(std::cos(phase)),
                                             static_cast<float>(std::sin(phase)));
    char words[8];
    qToLittleEndian(std::bit_cast<quint32>(sample.real()), words);
    qToLittleEndian(std::bit_cast<quint32>(sample.imag()), words + 4);
    bytes.append(words, 8);
  }
  if (file.write(bytes) != bytes.size()) throw std::runtime_error("write");
  FileMetadata metadata;
  metadata.id = "speed-source";
  metadata.path = file.fileName().toUtf8().toStdString();
  metadata.sampleRateHz = fs;
  metadata.sampleCount = count;
  metadata.effectiveBandwidthHz = fs;
  metadata.sampleFormat.componentEncoding = ComponentEncoding::Float32;
  metadata.sampleFormat.normalizeIntegerAdc = false;
  return metadata;
}
double timeWindows(bool mkl, const FileMetadata& metadata, int repeats, std::uint64_t& mklFrames) {
  DetectionSourceConfig config;
  config.profileId = "fixed-grid-fixture-v1";
  config.fftSize = 65536;
  config.inputAdaptationVersion = 2;
  config.hopSeconds = 0.00025;
  config.useHostMkl = mkl;
  config.preferGpu = false;
  FixedDetectionSource source;
  QString error;
  const ViewRange request{{0, metadata.sampleCount},
                          {-metadata.sampleRateHz / 2, metadata.sampleRateHz / 2}};
  if (!source.open(metadata, request, config, error)) throw std::runtime_error(error.toStdString());
  DetectionModelWindow warmup;
  if (!source.readModelWindow(0, warmup, error)) throw std::runtime_error(error.toStdString());
  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i < repeats; ++i) {
    DetectionModelWindow window;
    if (!source.readModelWindow(0, window, error)) throw std::runtime_error(error.toStdString());
  }
  mklFrames = source.gpuProfiling()["mklFrames"].toInteger();
  return static_cast<double>(timer.nsecsElapsed()) / 1e6 / repeats;
}
double timeEnergy(std::uint32_t length, int repeats) {
  std::vector<std::complex<float>> samples(length);
  for (std::uint32_t i = 0; i < length; ++i)
    samples[i] = {std::sin(0.017f * i), std::cos(0.013f * i)};
  QString error;
  std::vector<float> output;
  if (!detectionBandEnergy(samples, length, length / 8.0, length / 16.0, output,
                           512ULL * 1024 * 1024, error))
    throw std::runtime_error(error.toStdString());
  std::vector<double> samplesMs;
  for (int i = 0; i < repeats; ++i) {
    auto copy = samples;
    QElapsedTimer timer;
    timer.start();
    if (!detectionBandEnergy(std::move(copy), length, length / 8.0, length / 16.0, output,
                             512ULL * 1024 * 1024, error))
      throw std::runtime_error(error.toStdString());
    samplesMs.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
  }
  return median(samplesMs);
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    const auto metadata = makeSource(directory, 20'000'000, 400'000);
    const auto mkl = QStringLiteral(
        "D:/project/signal_studio_rebuild/out/toolchains/fft-libs/conda/Library/bin/mkl_rt.3.dll");
    std::cout << "path\tlength\tlibrary\tms\n";
    qputenv("SS_MKL_DLL", "off");
    std::uint64_t handwrittenMklFrames = 0, mklFrames = 0;
    const auto handwrittenWindow = timeWindows(false, metadata, 20, handwrittenMklFrames);
    const auto handwrittenEnergy64k = timeEnergy(65536, 5);
    const auto handwrittenEnergy256k = timeEnergy(262144, 3);
    if (QFileInfo::exists(mkl)) qputenv("SS_MKL_DLL", QFile::encodeName(mkl));
    const auto mklWindow = timeWindows(true, metadata, 20, mklFrames);
    const auto mklEnergy64k = timeEnergy(65536, 5);
    const auto mklEnergy256k = timeEnergy(262144, 3);
    std::cout << "model-window STFT\t65536 x 10 frames\thandwritten\t" << handwrittenWindow
              << "\tmklFrames\t" << handwrittenMklFrames << "\n";
    std::cout << "model-window STFT\t65536 x 10 frames\toneMKL\t" << mklWindow
              << "\tmklFrames\t" << mklFrames << "\n";
    std::cout << "band energy\t65536\thandwritten\t" << handwrittenEnergy64k << "\n";
    std::cout << "band energy\t65536\toneMKL\t" << mklEnergy64k << "\n";
    std::cout << "band energy\t262144\thandwritten\t" << handwrittenEnergy256k << "\n";
    std::cout << "band energy\t262144\toneMKL\t" << mklEnergy256k << "\n";
    std::cout << "window speedup\t\t\t" << (handwrittenWindow / mklWindow) << "\n";
    std::cout << "energy65536 speedup\t\t\t" << (handwrittenEnergy64k / mklEnergy64k) << "\n";
    std::cout << "energy262144 speedup\t\t\t" << (handwrittenEnergy256k / mklEnergy256k) << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
