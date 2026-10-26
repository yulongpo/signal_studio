#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

#include "infrastructure/detection_source.h"
#include "infrastructure/source_loader.h"
#include "infrastructure/wav_metadata.h"

using namespace signalstudio;
namespace {
QDir sourceRoot() {
#ifdef SS_SOURCE_DIR
  return QDir(QString::fromUtf8(SS_SOURCE_DIR));
#else
  return QDir(QDir::currentPath());
#endif
}
FileMetadata source(
    QTemporaryDir& directory, double fs, std::uint64_t count,
    const std::function<std::complex<float>(std::uint64_t)>& generate,
    const QString& name = "source.raw") {
  const auto path = directory.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) qFatal("source fixture unavailable");
  QByteArray bytes;
  bytes.reserve(static_cast<qsizetype>(count * 8));
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto sample = generate(i);
    char words[8];
    qToLittleEndian(std::bit_cast<quint32>(sample.real()), words);
    qToLittleEndian(std::bit_cast<quint32>(sample.imag()), words + 4);
    bytes.append(words, 8);
  }
  if (file.write(bytes) != bytes.size()) qFatal("source fixture write failed");
  FileMetadata metadata;
  metadata.id = "source-file";
  metadata.path = path.toUtf8().toStdString();
  metadata.sampleRateHz = fs;
  metadata.sampleCount = count;
  metadata.effectiveBandwidthHz = fs;
  metadata.demo = false;
  metadata.sampleFormat.componentEncoding = ComponentEncoding::Float32;
  metadata.sampleFormat.normalizeIntegerAdc = false;
  return metadata;
}
DetectionSourceConfig fixtureConfig(std::uint32_t fftSize = 64) {
  DetectionSourceConfig config;
  config.profileId = "fixed-grid-fixture-v1";
  config.fftSize = fftSize;
  return config;
}
ViewRange physicalRequest(const FileMetadata& metadata) {
  return {{0, metadata.sampleCount},
          {metadata.centerFrequencyHz - metadata.sampleRateHz / 2,
           metadata.centerFrequencyHz + metadata.sampleRateHz / 2}};
}
bool excluded(const DetectionSourcePlan& plan, const std::string& reason) {
  return std::any_of(plan.exclusions.begin(), plan.exclusions.end(),
                     [&](const auto& item) { return item.reason == reason; });
}
double integratedPower(const DetectionSpectrumFrame& frame) {
  double sum = 0;
  for (const auto value : frame.densityDb) sum += std::pow(10.0, value / 10);
  return sum * frame.frequencyStepHz;
}
}  // namespace

class DetectionSourceTests : public QObject {
  Q_OBJECT
 private slots:
  void optionalGpuGolden_data() {
    QTest::addColumn<int>("bins"); QTest::addColumn<bool>("zero");
    QTest::newRow("production-65536") << 65536 << false;
    QTest::newRow("irregular-3001") << 3001 << false;
    QTest::newRow("zero-floor-4096") << 4096 << true;
  }
  void optionalGpuGolden() {
    QFETCH(int,bins); QFETCH(bool,zero);
    QTemporaryDir directory;
    auto metadata=source(directory,20000000,400000,[zero](std::uint64_t i) {
      if(zero) return std::complex<float>{};
      auto bits=i+0x9e3779b97f4a7c15ULL;
      bits=(bits^(bits>>30))*0xbf58476d1ce4e5b9ULL;
      bits=(bits^(bits>>27))*0x94d049bb133111ebULL;
      bits^=bits>>31;
      const double phase=2*std::numbers::pi*.137*double(i);
      return std::complex<float>{float(.6*std::cos(phase)+double(bits&65535)/65535-.5),
          float(.6*std::sin(phase)+double((bits>>16)&65535)/65535-.5)};
    });
    auto config=fixtureConfig(static_cast<std::uint32_t>(bins));
    config.inputAdaptationVersion=2;config.hopSeconds=.00025;
    config.useHostMkl=false;
    FixedDetectionSource cpu,host;QString error;
    QVERIFY2(cpu.open(metadata,physicalRequest(metadata),config,error),qPrintable(error));
    config.useHostMkl=true;
    const auto mkl = sourceRoot().filePath(
        "out/toolchains/fft-libs/conda/Library/bin/mkl_rt.3.dll");
    if (qEnvironmentVariableIsEmpty("SS_MKL_DLL") && QFileInfo::exists(mkl))
      qputenv("SS_MKL_DLL", QFile::encodeName(mkl));
    QVERIFY2(host.open(metadata,physicalRequest(metadata),config,error),qPrintable(error));
    double dbError=0,tensorError=0;
    for(const auto index:{std::uint64_t{0},cpu.plan().eligibleWindowCount-1}) {
      DetectionModelWindow expected,actual;
      QVERIFY2(cpu.readModelWindow(index,expected,error),qPrintable(error));
      QVERIFY2(host.readModelWindow(index,actual,error),qPrintable(error));
      if (host.gpuProfiling()["mklFrames"].toInteger() == 0) {
        const auto reason=QJsonDocument(host.gpuProfiling()).toJson(QJsonDocument::Compact);
        if(qEnvironmentVariableIntValue("SS_REQUIRE_MKL")) QFAIL(reason.constData());
        QSKIP(reason.constData());
      }
      QCOMPARE(actual.densityDb.size(),expected.densityDb.size());
      QCOMPARE(actual.frames.size(),expected.frames.size());
      QCOMPARE(actual.epsilonGuardApplied,expected.epsilonGuardApplied);
      for(std::size_t row=0;row<actual.frames.size();++row) {
        QCOMPARE(actual.frames[row].inputHash,expected.frames[row].inputHash);
        QCOMPARE(actual.frames[row].fftSupports,expected.frames[row].fftSupports);
        QCOMPARE(actual.frames[row].powerFloorBins,expected.frames[row].powerFloorBins);
        QCOMPARE(actual.frames[row].densityUnderflowGuardBins,expected.frames[row].densityUnderflowGuardBins);
      }
      for(std::size_t i=0;i<actual.densityDb.size();++i) {
        dbError=std::max(dbError,std::abs(double(actual.densityDb[i])-expected.densityDb[i]));
        tensorError=std::max(tensorError,std::abs(double(actual.normalizedTensor[i])-expected.normalizedTensor[i]));
      }
    }
    qInfo() << "oneMKL versus handwritten CPU reference" << bins << dbError << tensorError
            << QJsonDocument(host.gpuProfiling()).toJson(QJsonDocument::Compact);
    QVERIFY(dbError<3.8e-5); QVERIFY(tensorError<5e-6);
    const auto profile=host.gpuProfiling();
    QVERIFY(profile["mklFrames"].toInteger()>0);
    QCOMPARE(profile["gpuFrames"].toInteger(),qint64{0});
  }
  void hostMklMissingStaysHandwritten() {
    qunsetenv("SS_MKL_DLL");
    QTemporaryDir directory;
    auto metadata=source(directory,20000000,80000,[](std::uint64_t i) {
      const double phase=2*std::numbers::pi*.01*double(i);
      return std::complex<float>{float(std::cos(phase)),float(std::sin(phase))};
    });
    auto config=fixtureConfig(4096);
    config.inputAdaptationVersion=2;config.hopSeconds=.00025;
    config.useHostMkl=true;config.preferGpu=true;
    FixedDetectionSource provider;QString error;
    QVERIFY2(provider.open(metadata,physicalRequest(metadata),config,error),qPrintable(error));
    DetectionModelWindow window;
    QVERIFY2(provider.readModelWindow(0,window,error),qPrintable(error));
    const auto profile=provider.gpuProfiling();
    QCOMPARE(profile["mklFrames"].toInteger(),qint64{0});
    QCOMPARE(profile["gpuFrames"].toInteger(),qint64{0});
    QVERIFY(profile["cpuFrames"].toInteger()>0);
    QVERIFY(!window.densityDb.empty());
  }
  void adaptiveShortRoiTailAndIrregularGrid() {
    QTemporaryDir directory;
    auto metadata = source(directory, 100000, 17, [](std::uint64_t) {
      return std::complex<float>{1, 0};
    });
    auto config = fixtureConfig(3001);
    config.inputAdaptationVersion = 2;
    FixedDetectionSource provider;
    QString error;
    auto roi = physicalRequest(metadata);
    roi.time = {16, 17};
    roi.frequency = {-1000, 1000};
    QVERIFY2(provider.open(metadata, roi, config, error), qPrintable(error));
    QCOMPARE(provider.plan().transformBins, 4096U);
    QCOMPARE(provider.plan().actualWindowFrames, 1U);
    QCOMPARE(provider.plan().eligibleWindowCount, quint64{1});
    DetectionModelWindow window;
    QVERIFY2(provider.readModelWindow(0, window, error), qPrintable(error));
    QCOMPARE(window.reportRange, roi);
    QCOMPARE(window.frames.size(), std::size_t{1});
    QCOMPARE(window.densityDb.size(), std::size_t{3001});
    QCOMPARE(window.frames[0].validSamples, quint64{17});
    QCOMPARE(window.frames[0].paddedSamples, quint64{4079});
    QVERIFY(std::all_of(window.densityDb.begin(), window.densityDb.end(),
                        [](float value) { return std::isfinite(value); }));
    auto scn = config;
    scn.profileId = "scn-max-psd-v1";
    DetectionSourcePlan budgetPlan;
    QVERIFY2(makeDetectionSourcePlan(metadata, roi, scn, budgetPlan, error),
             qPrintable(error));
    QCOMPARE(budgetPlan.modelHeadBytesPerWindow, quint64{98304});
    scn.evidenceBudgetBytes = 5000;
    QVERIFY(!makeDetectionSourcePlan(metadata, roi, scn, budgetPlan, error));
    QVERIFY(error.contains("模型原始输出"));
    metadata = source(
        directory, 100000, 2500,
        [](std::uint64_t) { return std::complex<float>{1, 0}; },
        "partial-adapted.raw");
    metadata.availability.status = LoadStatus::Partial;
    metadata.availability.availableSamples = 1234;
    QVERIFY2(provider.open(metadata, physicalRequest(metadata), config, error),
             qPrintable(error));
    QCOMPARE(provider.plan().realFrameCount, quint64{13});
    QVERIFY(excluded(provider.plan(), "unread-available-prefix"));
    QVERIFY(!excluded(provider.plan(), "incomplete-time-slot-tail"));
    QVERIFY(provider.readModelWindow(provider.plan().eligibleWindowCount - 1,
                                     window, error));
    QCOMPARE(window.reportRange.time.end, SampleIndex{1234});
    QCOMPARE(window.frames.back().slotSamples, (TimeRange{1200, 1234}));
    for (const auto& frame : window.frames)
      QVERIFY(frame.actualSamples.end <= 1234);
    QCOMPARE(provider.plan().frequencyStepHz, 100000.0 / 3001);
    metadata.sampleRateHz = 10000000;
    metadata.sampleCount = 10000000;
    metadata.effectiveBandwidthHz = metadata.sampleRateHz;
    metadata.availability.status = LoadStatus::Ready;
    metadata.availability.availableSamples = metadata.sampleCount;
    config.fftSize = 32;
    config.modelFrames = 1;
    config.hopSeconds = 1;
    DetectionSourcePlan oversized;
    QVERIFY(!makeDetectionSourcePlan(metadata, physicalRequest(metadata),
                                     config, oversized, error));
    QVERIFY(error.contains("8MiB"));
  }

  void frozenProfileDeclarationMatchesProviderConfig() {
    QFile file(sourceRoot().filePath(
        "contracts/wideband-detection/profiles/ffscn-gui-iq-v1.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto profile = QJsonDocument::fromJson(file.readAll()).object();
    const auto input = profile["input"].toObject();
    const DetectionSourceConfig config;
    QCOMPARE(profile["profileId"].toString(),
             QString::fromStdString(config.profileId));
    QCOMPARE(profile["profileVersion"].toInt(), int(config.profileVersion));
    QCOMPARE(input["nfft"].toInt(), int(config.fftSize));
    QCOMPARE(input["hopSeconds"].toDouble(), config.hopSeconds);
    QCOMPARE(input["modelFrames"].toInt(), int(config.modelFrames));
    QCOMPARE(input["modelHopFrames"].toInt(), int(config.modelHopFrames));
    QCOMPARE(input["frameOriginSample"].toString(), QString("0"));
    QCOMPARE(input["window"].toString(), QString("periodic-hann"));
    QCOMPARE(input["hopRounding"].toString(),
             QString("floor(sourceSampleRateHz*hopSeconds)"));
    QCOMPARE(input["powerAveraging"].toString(),
             QString("mean-of-individual-fft-powers"));
    QCOMPARE(input["densityScaling"].toString(),
             QString("power/(analysisSampleRateHz*sum(window^2))"));
    QCOMPARE(input["tailWindow"].toString(),
             QString("anchor-last-complete-window-once"));
  }

  void periodicHannDensityAndPowerAveraging() {
    QTemporaryDir directory;
    auto metadata = source(directory, 10000, 130, [](std::uint64_t) {
      return std::complex<float>{1, 0};
    });
    FixedDetectionSource provider;
    QString error;
    QVERIFY2(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                           error),
             qPrintable(error));
    const auto& plan = provider.plan();
    QCOMPARE(plan.hopSamples, quint64{10});
    QCOMPARE(plan.realFrameCount, quint64{13});
    QCOMPARE(plan.sourceLeadingPadding, quint64{27});
    QCOMPARE(plan.sourceTrailingPadding, quint64{27});
    QCOMPARE(plan.globalWindowCount, quint64{2});
    QVERIFY(plan.anchoredTailWindow);
    DetectionSpectrumFrame frame;
    QVERIFY(provider.readFrame(6, frame, error));
    QCOMPARE(frame.mapping.actualSamples, (TimeRange{33, 97}));
    QCOMPARE(frame.mapping.validSamples, quint64{64});
    QCOMPARE(frame.mapping.paddedSamples, quint64{0});
    const auto dc = 10 * std::log10(2.0 * 64 / (3 * metadata.sampleRateHz));
    QVERIFY(std::abs(frame.densityDb[32] - dc) < 1e-5);
    QVERIFY(std::abs(frame.densityDb[31] - (dc - 10 * std::log10(4.0))) < 1e-5);
    QVERIFY(std::abs(integratedPower(frame) - 1) < 1e-6);
    QVERIFY(provider.readFrame(0, frame, error));
    QCOMPARE(frame.mapping.fftSupports[0],
             (DetectionFftSupport{{0, 37}, 27, 0}));
    QVERIFY(provider.readFrame(12, frame, error));
    QCOMPARE(frame.mapping.fftSupports[0],
             (DetectionFftSupport{{93, 130}, 0, 27}));
    const auto two = source(
        directory, 128000, 1280,
        [](std::uint64_t i) {
          return std::complex<float>{(i / 64) % 2 ? -1.f : 1.f, 0};
        },
        "two-fft.raw");
    QVERIFY(provider.open(two, physicalRequest(two), fixtureConfig(), error));
    QVERIFY(provider.readFrame(0, frame, error));
    QCOMPARE(frame.mapping.fftSupports.size(), std::size_t{2});
    // Opposite FFT coefficients cancel; individual FFT powers must average
    // to 1.
    QVERIFY(std::abs(integratedPower(frame) - 1) < 1e-6);
    QVERIFY(std::abs(frame.densityDb[32] -
                     10 * std::log10(128.0 / (3 * 128000))) < 1e-5);
  }

  void remainderAndSlotPaddingMatchFrozenRules() {
    QTemporaryDir directory;
    auto metadata = source(directory, 10000, 139, [](std::uint64_t i) {
      return std::complex<float>{static_cast<float>(i % 7), 0};
    });
    FixedDetectionSource provider;
    QString error;
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    QCOMPARE(provider.plan().sourceLeadingPadding, quint64{22});
    QCOMPARE(provider.plan().sourceTrailingPadding, quint64{23});
    DetectionSpectrumFrame frame;
    QVERIFY(provider.readFrame(12, frame, error));
    QCOMPARE(frame.mapping.fftSupports[0],
             (DetectionFftSupport{{98, 139}, 0, 23}));
    metadata = source(
        directory, 50000, 520,
        [](std::uint64_t) { return std::complex<float>{1, 0}; },
        "halo-tail.raw");
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    QCOMPARE(provider.plan().sourceLeadingPadding, quint64{0});
    QCOMPARE(provider.plan().sourceTrailingPadding, quint64{0});
    QVERIFY(provider.readFrame(9, frame, error));
    QCOMPARE(frame.mapping.actualSamples, (TimeRange{450, 514}));
    QCOMPARE(frame.mapping.paddedSamples, quint64{0});
    QVERIFY(excluded(provider.plan(), "incomplete-time-slot-tail"));
    metadata = source(
        directory, 160000, 1600,
        [](std::uint64_t) { return std::complex<float>{1, 0}; },
        "slot-padding.raw");
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    QCOMPARE(provider.plan().slotLeadingPadding, quint64{16});
    QCOMPARE(provider.plan().slotTrailingPadding, quint64{16});
    QVERIFY(provider.readFrame(0, frame, error));
    QCOMPARE(frame.mapping.fftSupports.size(), std::size_t{3});
    QCOMPARE(frame.mapping.fftSupports[0],
             (DetectionFftSupport{{0, 48}, 16, 0}));
    QCOMPARE(frame.mapping.fftSupports[1],
             (DetectionFftSupport{{48, 112}, 0, 0}));
    QCOMPARE(frame.mapping.fftSupports[2],
             (DetectionFftSupport{{112, 160}, 0, 16}));
    QCOMPARE(frame.mapping.validSamples, quint64{160});
    QCOMPARE(frame.mapping.paddedSamples, quint64{32});
  }

  void blockSizeRoiAndAbsoluteGridInvariant() {
    QTemporaryDir directory;
    auto metadata = source(directory, 64000, 1041, [](std::uint64_t i) {
      const auto phase = 2 * std::numbers::pi * (i % 64) / 64;
      const float amplitude = .1f + static_cast<float>(i / 64) / 100;
      return std::complex<float>{
          amplitude * static_cast<float>(std::cos(phase)),
          amplitude * static_cast<float>(std::sin(phase))};
    });
    metadata.effectiveBandwidthHz =
        12000;  // Display bandwidth must not crop the model FFT.
    auto small = fixtureConfig();
    small.readBlockBytes = 152;
    FixedDetectionSource a, b;
    QString error;
    QVERIFY(a.open(metadata, physicalRequest(metadata), small, error));
    QVERIFY(
        b.open(metadata, physicalRequest(metadata), fixtureConfig(), error));
    QCOMPARE(a.plan().eligibleWindowCount, quint64{3});
    std::vector<quint64> starts;
    QVERIFY(a.streamWindows(
        [&](const auto& window) {
          starts.push_back(window.firstFrameIndex);
          return true;
        },
        error));
    QCOMPARE(starts, (std::vector<quint64>{0, 5, 6}));
    DetectionModelWindow first, second;
    for (quint64 i = 0; i < 3; ++i) {
      QVERIFY(a.readModelWindow(i, first, error));
      QVERIFY(b.readModelWindow(i, second, error));
      QCOMPARE(first.densityDb, second.densityDb);
      QCOMPARE(first.normalizedTensor, second.normalizedTensor);
      QCOMPARE(first.tensorHash, second.tensorHash);
      QCOMPARE(first.frames.size(), std::size_t{10});
      for (std::size_t row = 0; row < first.frames.size(); ++row) {
        QCOMPARE(first.frames[row].frameIndex, first.firstFrameIndex + row);
        QCOMPARE(first.frames[row].inputHash, second.frames[row].inputHash);
      }
    }
    const ViewRange roi{{700, 800}, {-5000, 1000}};
    QVERIFY(b.open(metadata, roi, fixtureConfig(), error));
    QCOMPARE(b.plan().frameOriginSample, quint64{0});
    QCOMPARE(b.plan().firstRegularWindow, quint64{1});
    QCOMPARE(b.plan().eligibleWindowCount, quint64{2});
    QCOMPARE(b.plan().contextRange, (TimeRange{320, 1024}));
    QCOMPARE(b.plan().frequencyFirstBinHz, -32000.0);
    QCOMPARE(b.plan().frequencyStepHz, 1000.0);
    QVERIFY(a.readModelWindow(1, first, error));
    QVERIFY(b.readModelWindow(0, second, error));
    QCOMPARE(first.normalizedTensor, second.normalizedTensor);
    QCOMPARE(second.reportRange, roi);
    QCOMPARE(second.globalWindowIndex, quint64{1});
  }

  void shortPartialFloorAndZeroGuard() {
    QTemporaryDir directory;
    auto metadata = source(directory, 64000, 9 * 64,
                           [](std::uint64_t) { return std::complex<float>{}; });
    FixedDetectionSource provider;
    QString error;
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    QCOMPARE(provider.plan().realFrameCount, quint64{9});
    QCOMPARE(provider.plan().eligibleWindowCount, quint64{0});
    QVERIFY(excluded(provider.plan(), "insufficient-real-model-frames"));
    DetectionModelWindow window;
    QVERIFY(!provider.readModelWindow(0, window, error));
    metadata = source(
        directory, 64000, 20 * 64,
        [](std::uint64_t) { return std::complex<float>{}; }, "partial.raw");
    metadata.availability.status = LoadStatus::Partial;
    metadata.availability.availableSamples = 12 * 64 + 5;
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    QCOMPARE(provider.plan().realFrameCount, quint64{12});
    QCOMPARE(provider.plan().availableTailSamples, quint64{5});
    QCOMPARE(provider.plan().eligibleWindowCount, quint64{2});
    QVERIFY(excluded(provider.plan(), "unread-available-prefix"));
    QVERIFY(excluded(provider.plan(), "incomplete-time-slot-tail"));
    QVERIFY(provider.readModelWindow(1, window, error));
    QCOMPARE(window.firstFrameIndex, quint64{2});
    QVERIFY(window.epsilonGuardApplied);
    QVERIFY(window.floorGuardApplied);
    // At this smaller Fs/N the reference float32 density is subnormal, not
    // zero.
    QVERIFY(!window.densityUnderflowGuardApplied);
    QCOMPARE(window.sampleStdDb, 0.0);
    QVERIFY(std::all_of(window.normalizedTensor.begin(),
                        window.normalizedTensor.end(),
                        [](float value) { return value == 0; }));
    for (const auto& frame : window.frames)
      QVERIFY(frame.actualSamples.end <=
              metadata.availability.availableSamples);
    metadata = source(
        directory, 12345, 144,
        [](std::uint64_t) { return std::complex<float>{1, 0}; },
        "fractional-hop.raw");
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    QCOMPARE(provider.plan().hopSamples, quint64{12});
    DetectionSpectrumFrame frame;
    QVERIFY(provider.readFrame(11, frame, error));
    QCOMPARE(frame.mapping.slotSamples, (TimeRange{132, 144}));
  }

  void invalidBudgetCancellationAndSourceChanges() {
    QTemporaryDir directory;
    auto metadata = source(directory, 64000, 640, [](std::uint64_t) {
      return std::complex<float>{1, 0};
    });
    FixedDetectionSource provider;
    QString error;
    auto config = fixtureConfig();
    config.workingSetBudgetBytes = 1;
    QVERIFY(!provider.open(metadata, physicalRequest(metadata), config, error));
    config = fixtureConfig();
    config.fftSize = 60;
    QVERIFY(!provider.open(metadata, physicalRequest(metadata), config, error));
    config = fixtureConfig();
    config.readBlockBytes = 32 * 1024 * 1024 + 1;
    QVERIFY(!provider.open(metadata, physicalRequest(metadata), config, error));
    config = fixtureConfig();
    config.profileId = "ffscn-gui-iq-v1";
    QVERIFY(!provider.open(metadata, physicalRequest(metadata), config, error));
    auto wrongCount = metadata;
    wrongCount.sampleCount++;
    QVERIFY(!provider.open(wrongCount, physicalRequest(metadata),
                           fixtureConfig(), error));
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    DetectionModelWindow result;
    result.tensorHash = "unchanged-on-failure";
    int calls = 0;
    QVERIFY(!provider.readModelWindow(0, result, error,
                                      [&] { return ++calls >= 3; }));
    QVERIFY(error.contains("取消"));
    QCOMPARE(result.tensorHash, std::string("unchanged-on-failure"));
    DetectionSpectrumFrame frame;
    QVERIFY(!provider.readFrame(10, frame, error));
    QFile file(QString::fromUtf8(metadata.path));
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(1),
                             QFileDevice::FileModificationTime));
    file.close();
    QVERIFY(!provider.readFrame(0, frame, error));
    QVERIFY(error.contains("变化"));
    metadata.availability.fingerprint = "old-loaded-prefix";
    QVERIFY(!provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                           error));
    QVERIFY(error.contains("身份"));
  }

  void numericalGuardsAndStreamingFailuresAreIndependent() {
    QTemporaryDir directory;
    auto metadata = source(directory, 64000, 640, [](std::uint64_t index) {
      return std::complex<float>{index % 64 == 32 ? 1.f : 0.f, 0};
    });
    FixedDetectionSource provider;
    QString error;
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    DetectionModelWindow window;
    QVERIFY(provider.readModelWindow(0, window, error));
    QVERIFY(window.epsilonGuardApplied);
    QVERIFY(!window.floorGuardApplied);
    QVERIFY(!window.densityUnderflowGuardApplied);
    QCOMPARE(window.sampleStdDb, 0.0);
    QVERIFY(!provider.streamWindows({}, error));
    QVERIFY(error.contains("为空"));
    QVERIFY(!provider.streamWindows([](const auto&) { return false; }, error));
    QVERIFY(error.contains("停止"));
    const auto shortSource = source(
        directory, 64000, 576,
        [](std::uint64_t) { return std::complex<float>{}; },
        "short-stream.raw");
    QVERIFY(provider.open(shortSource, physicalRequest(shortSource),
                          fixtureConfig(), error));
    bool received = false;
    QVERIFY(!provider.streamWindows(
        [&](const auto&) {
          received = true;
          return true;
        },
        error, [] { return true; }));
    QVERIFY(!received);
    QVERIFY(error.contains("取消"));
  }

  void frozen20mZeroInputHasExplicitFiniteNumericalGuard() {
    QTemporaryDir directory;
    const auto metadata =
        source(directory, 20000000, 200000,
               [](std::uint64_t) { return std::complex<float>{}; });
    FixedDetectionSource provider;
    QString error;
    QVERIFY(provider.open(metadata, physicalRequest(metadata),
                          DetectionSourceConfig{}, error));
    QCOMPARE(provider.plan().numericalVersion,
             std::string("cpu-f64-fft-density-log-ddof1-v1"));
    DetectionModelWindow window;
    QVERIFY(provider.readModelWindow(0, window, error));
    QVERIFY(window.floorGuardApplied);
    QVERIFY(window.densityUnderflowGuardApplied);
    QVERIFY(window.epsilonGuardApplied);
    QCOMPARE(window.sampleStdDb, 0.0);
    const float referenceDensity =
        std::numeric_limits<float>::min() / float(20000000.0 * 24576);
    QCOMPARE(referenceDensity, 0.f);
    QVERIFY(std::isinf(std::log10(referenceDensity)));
    const double finiteFloorDb =
        10 *
        std::log10(std::numeric_limits<float>::min() / (20000000.0 * 24576));
    QVERIFY(std::abs(window.meanDb - finiteFloorDb) < 1e-4);
    for (const auto& frame : window.frames) {
      QCOMPARE(frame.powerFloorBins, quint64{65536});
      QCOMPARE(frame.densityUnderflowGuardBins, quint64{65536});
    }
    QVERIFY(std::all_of(window.normalizedTensor.begin(),
                        window.normalizedTensor.end(),
                        [](float value) { return value == 0; }));
    qInfo() << "20M zero IQ: reference float32 density underflows; finite"
            << provider.plan().numericalVersion.c_str() << "floor dB"
            << window.meanDb << "power floor and std epsilon are distinct";
  }

  void wavContainerOverridesAndRawReadBudgetAreRejected() {
    QTemporaryDir directory;
    const auto path = directory.filePath("container.wav");
    QByteArray body("WAVEfmt ", 8);
    const auto append16 = [&](quint16 value) {
      char encoded[2];
      qToLittleEndian(value, encoded);
      body.append(encoded, 2);
    };
    const auto append32 = [&](quint32 value) {
      char encoded[4];
      qToLittleEndian(value, encoded);
      body.append(encoded, 4);
    };
    append32(16);
    append16(1);
    append16(2);
    append32(64000);
    append32(256000);
    append16(4);
    append16(16);
    body.append("data", 4);
    append32(640 * 4);
    for (int i = 0; i < 640; ++i) {
      append16(16384);
      append16(0);
    }
    body.append("JUNK", 4);
    append32(4);
    body.append("tail", 4);
    QByteArray bytes("RIFF", 4);
    char encodedSize[4];
    qToLittleEndian(static_cast<quint32>(body.size()), encodedSize);
    bytes.append(encodedSize, 4);
    bytes.append(body);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    WavDescriptor descriptor;
    QString error;
    QVERIFY(readWavDescriptor(path, descriptor, error));
    FileMetadata metadata;
    metadata.id = "wav-container-source";
    metadata.path = path.toUtf8().toStdString();
    metadata.sampleRateHz = descriptor.sampleRateHz;
    metadata.sampleCount = descriptor.sampleCount;
    metadata.sampleFormat = descriptor.format;
    FixedDetectionSource provider;
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    auto forged = metadata;
    forged.sampleFormat.headerBytes += 4;
    forged.sampleFormat.trailerBytes -= 4;
    QVERIFY(!provider.open(forged, physicalRequest(forged), fixtureConfig(),
                           error));
    QVERIFY(error.contains("容器"));
    forged = metadata;
    forged.sampleRateHz /= 2;
    QVERIFY(!provider.open(forged, physicalRequest(forged), fixtureConfig(),
                           error));
    QVERIFY(error.contains("容器"));
    metadata.sampleRateHz = 262144000;
    metadata.sampleCount = 2621440;
    metadata.sampleFormat = {};
    metadata.sampleFormat.componentEncoding = ComponentEncoding::Float64;
    metadata.sampleFormat.normalizeIntegerAdc = false;
    metadata.sampleFormat.channels = 128;
    auto config = fixtureConfig(262144);
    config.readBlockBytes = 32 * 1024 * 1024;
    DetectionSourcePlan plan;
    QVERIFY(makeDetectionSourcePlan(metadata, physicalRequest(metadata), config,
                                    plan, error));
    QVERIFY(plan.estimatedPeakBytes > 32 * 1024 * 1024);
    config.workingSetBudgetBytes = 40 * 1024 * 1024;
    QVERIFY(!makeDetectionSourcePlan(metadata, physicalRequest(metadata),
                                     config, plan, error));
    QVERIFY(error.contains("工作集"));
  }

  void selectedIntegerChannelAndInvalidIqUseActualReader() {
    QTemporaryDir directory;
    const auto path = directory.filePath("two-channel-i16.raw");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray bytes;
    for (int i = 0; i < 640; ++i) {
      char frame[8];
      qToLittleEndian<quint16>(16384, frame);
      qToLittleEndian<quint16>(0, frame + 2);
      qToLittleEndian<quint16>(8192, frame + 4);
      qToLittleEndian<quint16>(0, frame + 6);
      bytes.append(frame, 8);
    }
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    FileMetadata metadata;
    metadata.id = "two-channel-source";
    metadata.path = path.toUtf8().toStdString();
    metadata.sampleRateHz = 64000;
    metadata.centerFrequencyHz = 2500000000;
    metadata.sampleCount = 640;
    metadata.sampleFormat.channels = 2;
    FixedDetectionSource provider;
    QString error;
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    DetectionSpectrumFrame first, second;
    QVERIFY(provider.readFrame(0, first, error));
    QCOMPARE(first.units, std::string("dBFS/Hz"));
    QVERIFY(std::abs(integratedPower(first) - .25) < 1e-6);
    metadata.sampleFormat.normalizeIntegerAdc = false;
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error));
    QVERIFY(provider.readFrame(0, second, error));
    QCOMPARE(first.densityDb, second.densityDb);
    QVERIFY(first.mapping.inputHash != second.mapping.inputHash);
    metadata.sampleFormat.selectedChannel = 1;
    QVERIFY(provider.open(metadata, physicalRequest(metadata), fixtureConfig(),
                          error, 11));
    QCOMPARE(provider.plan().sourceIdentity.selectedSampleChannel, quint32{1});
    QCOMPARE(provider.plan().sourceIdentity.projectGeneration, quint64{11});
    QVERIFY(provider.readFrame(0, second, error));
    QVERIFY(std::abs(integratedPower(second) - .0625) < 1e-6);
    QCOMPARE(second.frequencyFirstBinHz, 2500000000.0 - 32000);
    const auto invalidSource = source(
        directory, 64000, 640,
        [](std::uint64_t) {
          return std::complex<float>{std::numeric_limits<float>::quiet_NaN(),
                                     0};
        },
        "invalid-iq.raw");
    QVERIFY(provider.open(invalidSource, physicalRequest(invalidSource),
                          fixtureConfig(), error));
    second.units = "unchanged-invalid-output";
    QVERIFY(!provider.readFrame(0, second, error));
    QVERIFY(error.contains("非有限"));
    QCOMPARE(second.units, std::string("unchanged-invalid-output"));
  }

  void exactPlanningBeyondDoubleIntegerRange() {
    FileMetadata metadata;
    metadata.id = "integer-boundary-source";
    metadata.sampleRateHz = 64000;
    metadata.sampleCount = (quint64{1} << 53) + 1024;
    metadata.effectiveBandwidthHz = 12000;
    const auto first = (quint64{1} << 53) + 512;
    const ViewRange requested{{first, first + 64}, {-1000, 1000}};
    DetectionSourcePlan plan;
    QString error;
    const auto generation = std::numeric_limits<quint64>::max();
    QVERIFY2(makeDetectionSourcePlan(metadata, requested, fixtureConfig(), plan,
                                     error, generation),
             qPrintable(error));
    QCOMPARE(plan.sourceIdentity.projectGeneration, generation);
    QCOMPARE(plan.sourceIdentity.physicalSamples, metadata.sampleCount);
    QCOMPARE(plan.realFrameCount, metadata.sampleCount / 64);
    QCOMPARE(plan.requestedRange.time.begin, first);
    QCOMPARE(plan.requestedRange.time.end, first + 64);
    QVERIFY(plan.contextRange.begin > (quint64{1} << 53));
    QVERIFY(plan.contextRange.begin <= first);
    QVERIFY(plan.contextRange.end >= requested.time.end);
    QCOMPARE(plan.contextRange.begin % 64, quint64{0});
    QVERIFY(plan.eligibleWindowCount > 0 && plan.eligibleWindowCount <= 4);
    QVERIFY(plan.estimatedPeakBytes < 256 * 1024 * 1024);
    auto overflow = metadata;
    overflow.sampleCount = std::numeric_limits<quint64>::max();
    QVERIFY(!makeDetectionSourcePlan(overflow, requested, fixtureConfig(), plan,
                                     error));
  }

  void actual20mTorchCpuGolden() {
    QFile audit(sourceRoot().filePath(
        "docs/implementation/wideband-detection/M0/assets.json"));
    QVERIFY(audit.open(QIODevice::ReadOnly));
    const auto identity = QJsonDocument::fromJson(audit.readAll())
                              .object()["datasets"]
                              .toObject()["matlabGenerator"]
                              .toObject()["goldenPair"]
                              .toObject()["wav"]
                              .toObject();
    const auto path = identity["path"].toString();
    if (!QFileInfo::exists(path))
      QSKIP("M0 external 20M generator golden unavailable");
    QFile oracleFile(sourceRoot().filePath(
        "tests/fixtures/wideband/detection-source-20m-golden.json"));
    QVERIFY(oracleFile.open(QIODevice::ReadOnly));
    const auto oracle = QJsonDocument::fromJson(oracleFile.readAll()).object();
    QCOMPARE(oracle["wavSha256"], identity["sha256"]);
    QFile input(path);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QVERIFY(hash.addData(&input));
    QCOMPARE(QString::fromLatin1(hash.result().toHex()),
             oracle["wavSha256"].toString());
    WavDescriptor descriptor;
    QString error;
    QVERIFY(readWavDescriptor(path, descriptor, error));
    FileMetadata metadata;
    metadata.id = "actual-20m-golden";
    metadata.path = path.toUtf8().toStdString();
    metadata.sampleFormat = descriptor.format;
    metadata.sampleRateHz = descriptor.sampleRateHz;
    metadata.sampleCount = descriptor.sampleCount;
    metadata.effectiveBandwidthHz = 16000000;
    FixedDetectionSource provider;
    QVERIFY2(provider.open(metadata, physicalRequest(metadata),
                           DetectionSourceConfig{}, error),
             qPrintable(error));
    QVERIFY(provider.plan().referenceConfigCompatible);
    QVERIFY(!provider.plan().productionProfileCompatible);
    QCOMPARE(provider.plan().realFrameCount, quint64{800});
    QCOMPARE(provider.plan().eligibleWindowCount, quint64{159});
    QCOMPARE(provider.plan().sourceLeadingPadding, quint64{22768});
    QCOMPARE(provider.plan().sourceTrailingPadding, quint64{22768});
    QCOMPARE(provider.plan().frequencyStepHz, 20000000.0 / 65536);
    const auto bins = oracle["sampledBins"].toArray();
    double maxDbError = 0, maxTensorError = 0;
    for (const auto value : oracle["frames"].toArray()) {
      const auto expected = value.toObject();
      DetectionSpectrumFrame frame;
      QVERIFY2(
          provider.readFrame(expected["frameIndex"].toString().toULongLong(),
                             frame, error),
          qPrintable(error));
      QCOMPARE(frame.densityDb.size(), std::size_t{65536});
      QCOMPARE(frame.mapping.powerFloorBins, quint64{0});
      QCOMPARE(frame.mapping.densityUnderflowGuardBins, quint64{0});
      const auto powers = expected["sampledDensityDb"].toArray();
      for (qsizetype i = 0; i < bins.size(); ++i)
        maxDbError = std::max(
            maxDbError,
            std::abs(
                frame.densityDb[static_cast<std::size_t>(bins[i].toInt())] -
                powers[i].toDouble()));
    }
    for (const auto value : oracle["windows"].toArray()) {
      const auto expected = value.toObject();
      DetectionModelWindow window;
      const auto first = expected["firstFrameIndex"].toString().toULongLong();
      QVERIFY2(provider.readModelWindow(first / 5, window, error),
               qPrintable(error));
      QCOMPARE(window.firstFrameIndex, first);
      QCOMPARE(window.frames.size(), std::size_t{10});
      QCOMPARE(window.normalizedTensor.size(), std::size_t{655360});
      QVERIFY(!window.epsilonGuardApplied && !window.floorGuardApplied &&
              !window.densityUnderflowGuardApplied);
      QVERIFY(std::abs(window.meanDb - expected["meanDb"].toDouble()) < .005);
      QVERIFY(std::abs(window.sampleStdDb -
                       expected["sampleStdDb"].toDouble()) < .005);
      for (const auto item : expected["sampledTensor"].toArray()) {
        const auto sample = item.toArray();
        const auto index = static_cast<std::size_t>(sample[0].toInt() * 65536 +
                                                    sample[1].toInt());
        maxTensorError = std::max(
            maxTensorError,
            std::abs(window.normalizedTensor[index] - sample[2].toDouble()));
      }
    }
    qInfo().noquote() << "20M real WAV / independent Torch CPU oracle: sampled "
                         "PSD max error dB"
                      << maxDbError << "normalized tensor max error"
                      << maxTensorError;
    QVERIFY(maxDbError < oracle["psdToleranceDb"].toDouble());
    QVERIFY(maxTensorError < oracle["normalizationTolerance"].toDouble());
  }
};
QTEST_GUILESS_MAIN(DetectionSourceTests)
#include "detection_source_tests.moc"
