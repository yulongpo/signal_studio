#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <filesystem>
#include <limits>

#include "application/annotation_service.h"
#include "infrastructure/annotation_json.h"
#include "infrastructure/annotation_store.h"
#include "infrastructure/label_import.h"
#include "infrastructure/label_discovery.h"
#include "infrastructure/wav_metadata.h"

using namespace signalstudio;
namespace {
QDir root() {
#ifdef SS_SOURCE_DIR
  return QDir(QString::fromUtf8(SS_SOURCE_DIR));
#else
  return QDir(QDir::currentPath());
#endif
}
QByteArray fixture(const QString& name) {
  QFile f(root().filePath("tests/fixtures/wideband/" + name));
  if (!f.open(QIODevice::ReadOnly)) qFatal("fixture unavailable");
  return f.readAll();
}
void write(const QString& path, const QByteArray& raw) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(raw) != raw.size())
    qFatal("fixture write failed");
}
void sizeFile(const QString& path, quint64 size) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      !file.resize(static_cast<qint64>(size)))
    qFatal("sparse sample fixture failed");
}
FileMetadata target(const QString& path, double fs = 60000000,
                    quint64 count = 600000, double fc = 0) {
  FileMetadata m;
  m.id = "target-file";
  m.path = path.toUtf8().toStdString();
  m.name = QFileInfo(path).fileName().toUtf8().toStdString();
  m.demo = false;
  m.sampleRateHz = fs;
  m.centerFrequencyHz = fc;
  m.sampleCount = count;
  m.effectiveBandwidthHz = fs;
  return m;
}
FileMetadata torchTarget(QTemporaryDir& dir) {
  const auto base = dir.filePath("0000000");
  write(base + ".sigmf-meta", fixture("torchsig-0000000.sigmf-meta"));
  write(base + ".json", fixture("torchsig-0000000.json"));
  sizeFile(base + ".sigmf-data", 3200000);
  auto m = target(base + ".sigmf-data", 20000000, 400000, 2500000000);
  m.sampleFormat.componentEncoding = ComponentEncoding::Float32;
  m.sampleFormat.normalizeIntegerAdc = false;
  return m;
}
QJsonObject readJson(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) qFatal("JSON fixture unavailable");
  return QJsonDocument::fromJson(file.readAll()).object();
}
QString baseOf(const FileMetadata& m) {
  return QString::fromStdString(m.path).chopped(11);
}
LabelFieldMapping sampleMapping() {
  LabelFieldMapping m;
  m.delimiter = LabelDelimiter::Comma;
  m.classColumn = 0;
  m.startColumn = 1;
  m.endColumn = 2;
  m.lowerColumn = 3;
  m.upperColumn = 4;
  m.timeUnit = LabelTimeUnit::Samples;
  m.interval = LabelInterval::InclusiveEnd;
  m.indexOrigin = 1;
  m.frequencyLayout = LabelFrequencyLayout::Edges;
  m.frequencyUnit = LabelFrequencyUnit::Hz;
  m.frequencyReference = LabelFrequencyReference::Baseband;
  return m;
}
FileMetadata matlabPair(QTemporaryDir& dir, const QByteArray& text) {
  QDir(dir.path()).mkpath("data");
  QDir(dir.path()).mkpath("label");
  sizeFile(dir.filePath("data/name.data"), 4000);
  write(dir.filePath("label/name.txt"), text);
  return target(dir.filePath("data/name.data"), 1000, 1000, 10000);
}
FileMetadata wavPair(QTemporaryDir& dir, const QByteArray& text,
                     quint32 fs = 1000, quint32 count = 1000, double fc = 10000) {
  QDir(dir.path()).mkpath("data");
  QDir(dir.path()).mkpath("label");
  // Noncanonical RIFF: JUNK before and after IQ. Discovery/prepare must retain
  // the descriptor's layout, not count header/trailer bytes as raw IQ samples.
  QByteArray header(56, '\0');
  header.replace(0, 4, "RIFF");
  qToLittleEndian<quint32>(56 + count * 4 + 12 - 8, header.data() + 4);
  header.replace(8, 8, "WAVEfmt ");
  qToLittleEndian<quint32>(16, header.data() + 16);
  qToLittleEndian<quint16>(1, header.data() + 20);
  qToLittleEndian<quint16>(2, header.data() + 22);
  qToLittleEndian<quint32>(fs, header.data() + 24);
  qToLittleEndian<quint32>(fs * 4, header.data() + 28);
  qToLittleEndian<quint16>(4, header.data() + 32);
  qToLittleEndian<quint16>(16, header.data() + 34);
  header.replace(36, 4, "JUNK");
  qToLittleEndian<quint32>(4, header.data() + 40);
  header.replace(48, 4, "data");
  qToLittleEndian<quint32>(count * 4, header.data() + 52);
  const auto path = dir.filePath("data/name.wav");
  write(path, header + QByteArray(count * 4, '\0') +
                  QByteArray::fromHex("4a554e4b0400000000000000"));
  write(dir.filePath("label/name.txt"), text);
  WavDescriptor wav;
  QString error;
  if (!readWavDescriptor(path, wav, error)) qFatal("WAV fixture: %s", qPrintable(error));
  auto metadata = target(path, wav.sampleRateHz, wav.sampleCount, fc);
  metadata.sampleFormat = wav.format;
  return metadata;
}
}  // namespace
class LabelImportTests : public QObject {
  Q_OBJECT
 private slots:
  void manualTxtRequiresMappingAndPreviewIsBounded() {
    QTemporaryDir dir;
    const auto metadata = matlabPair(dir, "BPSK,1,10,-100,100\n");
    const auto manual = dir.filePath("arbitrary-manual-label.txt");
    write(manual, "BPSK,1,10,-100,100\n");
    const auto unknown = importLabelSource(manual, metadata);
    QCOMPARE(unknown.state, LabelImportState::NeedsMapping);
    QVERIFY(!unknown.error.isEmpty() && unknown.annotations.empty());
    QString preview, error;
    QVERIFY(previewLabelSource(manual, preview, error));
    QVERIFY(!hasGeneratorLabelHeader(preview));
    const auto mapped = importMappedTextLabels(manual, metadata, sampleMapping());
    QVERIFY2(mapped.ok(), qPrintable(mapped.error));
    QCOMPARE(mapped.annotations.size(), std::size_t{1});
    write(manual, QByteArray(50000, 'a'));
    QVERIFY(previewLabelSource(manual, preview, error, {}, 32));
    QCOMPARE(preview.size(), qsizetype{32});
    LabelImportOptions limited;
    limited.maximumBytes = 4;
    QCOMPARE(importLabelSource(manual, metadata, 0, false, limited).state,
             LabelImportState::LimitExceeded);
    limited = {};
    limited.cancelled = [] { return true; };
    QCOMPARE(importLabelSource(manual, metadata, 0, false, limited).state,
             LabelImportState::Cancelled);
    QVERIFY(!previewLabelSource(dir.filePath("missing.txt"), preview, error));
    QVERIFY(preview.isEmpty() && !error.isEmpty());

    const auto generatorData = dir.filePath("generator.raw");
    sizeFile(generatorData, 2400000);
    const auto generatorMetadata = target(generatorData);
    const auto known = fixture("generator-60m-10ms.txt");
    write(manual, known);
    const auto imported = importLabelSource(manual, generatorMetadata);
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    QCOMPARE(imported.annotations.size(), std::size_t{12});
    const auto bad = QByteArray("1: time_start:0 time_end:0.01 sig_len:invalid\n") +
                     known.mid(known.indexOf('\n') + 1);
    write(manual, bad);
    QVERIFY(previewLabelSource(manual, preview, error));
    QVERIFY(hasGeneratorLabelHeader(preview));
    const auto rejected = importLabelSource(manual, generatorMetadata);
    QCOMPARE(rejected.state, LabelImportState::Error);
    QVERIFY(!rejected.ok() && rejected.annotations.empty());
  }

  void strictMatlabPairAndReverseLookup() {
    QTemporaryDir dir;
    auto metadata = matlabPair(dir, "BPSK,1,10,-100,100\n");
    const auto found = discoverLabelCandidates(metadata);
    QCOMPARE(found.state, LabelDiscoveryState::NeedsMapping);
    QCOMPARE(found.candidates.size(), std::size_t{1});
    QCOMPARE(found.candidates[0].labelPath, dir.filePath("label/name.txt"));
    const auto reverse = discoverDataForLabel(dir.filePath("label/name.txt"));
    QCOMPARE(reverse.state, LabelDiscoveryState::NeedsMapping);
    QCOMPARE(reverse.candidates.size(), std::size_t{1});
    QCOMPARE(reverse.candidates[0].dataPath, dir.filePath("data/name.data"));
    QCOMPARE(reverse.candidates[0].profile, found.candidates[0].profile);
    FileState file;
    file.metadata = metadata;
    const auto blocked = prepareDiscoveredLabels(found.candidates[0], file);
    QCOMPARE(blocked.disposition, LabelImportDisposition::NeedsMapping);
    QVERIFY(file.marks.empty());
    QFile::remove(dir.filePath("label/name.txt"));
    write(dir.filePath("label/other.txt"), "unrelated");
    QCOMPARE(discoverLabelCandidates(metadata).state, LabelDiscoveryState::NotFound);
    const auto stale = prepareDiscoveredLabels(found.candidates[0], file, 0, sampleMapping());
    QCOMPARE(stale.disposition, LabelImportDisposition::Rejected);
  }

  void pairedGeneratorDiscoveryRoundtrip_data() {
    QTest::addColumn<bool>("wav");
    QTest::newRow("wav-riff-layout") << true;
    QTest::newRow("raw-data-retained") << false;
  }

  void pairedGeneratorDiscoveryRoundtrip() {
    QFETCH(bool, wav);
    QTemporaryDir dir;
    const auto text = fixture("generator-60m-10ms.txt");
    FileState file;
    if (wav) {
      file.metadata = wavPair(dir, text, 60000000, 600000, 0);
      QCOMPARE(file.metadata.sampleFormat.headerBytes, quint64{56});
      QCOMPARE(file.metadata.sampleFormat.trailerBytes, quint64{12});
    } else {
      file.metadata = matlabPair(dir, text);
      sizeFile(dir.filePath("data/name.data"), 2400000);
      file.metadata = target(dir.filePath("data/name.data"));
    }
    const auto forward = discoverLabelCandidates(file.metadata);
    const auto reverse = discoverDataForLabel(dir.filePath("label/name.txt"));
    QCOMPARE(forward.state, LabelDiscoveryState::Found);
    QCOMPARE(reverse.state, LabelDiscoveryState::Found);
    QCOMPARE(forward.candidates.size(), std::size_t{1});
    QCOMPARE(reverse.candidates.size(), std::size_t{1});
    const auto& candidate = reverse.candidates[0];
    QCOMPARE(candidate.profile, LabelProfile::GeneratorText);
    QCOMPARE(candidate.profile, forward.candidates[0].profile);
    QCOMPARE(candidate.dataPath, forward.candidates[0].dataPath);
    QCOMPARE(candidate.labelPath, forward.candidates[0].labelPath);
    QCOMPARE(candidate.preview, forward.candidates[0].preview);
    const auto prepared = prepareDiscoveredLabels(candidate, file, 7);
    const auto fromData = prepareDiscoveredLabels(forward.candidates[0], file, 7);
    QCOMPARE(prepared.disposition, LabelImportDisposition::Ready);
    QCOMPARE(fromData.disposition, LabelImportDisposition::Ready);
    QVERIFY(prepared.imported.sourceSet == fromData.imported.sourceSet);
    QVERIFY(prepared.imported.sourceSet.sourceIdentity == annotationSourceIdentity(file.metadata, 7));
    QCOMPARE(prepared.imported.annotations.size(), std::size_t{12});
    std::size_t segments = 0;
    for (const auto& annotation : prepared.imported.annotations) {
      segments += annotation.segments.size();
      QVERIFY(validateAnnotation(annotation, annotationBounds(file.metadata)).ok);
    }
    QCOMPARE(segments, std::size_t{17});
    AnnotationService service;
    QVERIFY(publishDiscoveredLabels(service, file, prepared, 7).ok);
    QCOMPARE(file.marks.size(), std::size_t{12});
    QCOMPARE(prepareDiscoveredLabels(forward.candidates[0], file, 7).disposition,
             LabelImportDisposition::AlreadyImported);
    const auto duplicate = publishDiscoveredLabels(service, file, fromData, 7);
    QVERIFY(duplicate.ok && !duplicate.changed);
    QVERIFY(service.undo(file).ok);
    QVERIFY(file.marks.empty());
  }

  void wavUnknownLabelsRequireMappingAndBoundedPreview() {
    QTemporaryDir dir;
    FileState file;
    file.metadata = wavPair(dir, "BPSK,1,10,-100,100\n");
    const auto label = dir.filePath("label/name.txt");
    const auto forward = discoverLabelCandidates(file.metadata);
    const auto reverse = discoverDataForLabel(label);
    QCOMPARE(forward.state, LabelDiscoveryState::NeedsMapping);
    QCOMPARE(reverse.state, LabelDiscoveryState::NeedsMapping);
    QCOMPARE(forward.candidates.size(), std::size_t{1});
    QCOMPARE(reverse.candidates.size(), std::size_t{1});
    for (const auto& candidate : {forward.candidates[0], reverse.candidates[0]}) {
      QCOMPARE(candidate.profile, LabelProfile::MatlabDataTxt);
      const auto blocked = prepareDiscoveredLabels(candidate, file);
      QCOMPARE(blocked.disposition, LabelImportDisposition::NeedsMapping);
      QVERIFY(blocked.imported.annotations.empty());
      const auto mapped = prepareDiscoveredLabels(candidate, file, 0, sampleMapping());
      QCOMPARE(mapped.disposition, LabelImportDisposition::Ready);
      QCOMPARE(mapped.imported.annotations.size(), std::size_t{1});
      const auto& range = mapped.imported.annotations[0].segments[0].range;
      QVERIFY(range.time == (TimeRange{0, 10}));
      QVERIFY(range.frequency == (FrequencyRange{9900, 10100}));
      QVERIFY(mapped.imported.sourceSet.sourceIdentity.sampleFormat == file.metadata.sampleFormat);
    }
    write(label, QByteArray(64, ' ') + fixture("generator-60m-10ms.txt"));
    LabelDiscoveryOptions options;
    options.maximumPreviewBytes = 32;
    const auto bounded = discoverLabelCandidates(file.metadata, options);
    const auto boundedReverse = discoverDataForLabel(label, options);
    // A whitespace-only preview of a larger source is neither Empty nor a
    // license to read beyond the preview budget to guess its schema.
    QCOMPARE(bounded.state, LabelDiscoveryState::NeedsMapping);
    QCOMPARE(boundedReverse.state, LabelDiscoveryState::NeedsMapping);
    QCOMPARE(bounded.candidates.size(), std::size_t{1});
    QCOMPARE(boundedReverse.candidates.size(), std::size_t{1});
    QCOMPARE(bounded.candidates[0].preview, QString(32, QLatin1Char(' ')));
    QCOMPARE(boundedReverse.candidates[0].preview, bounded.candidates[0].preview);
    for (const auto mode : {0, 1, 2}) {
      options = {};
      if (mode == 0) options.maximumDirectoryEntries = 1;
      if (mode == 1) options.parsing.maximumBytes = 4;
      if (mode == 2) options.parsing.cancelled = [] { return true; };
      const auto expected = mode == 2 ? LabelDiscoveryState::Cancelled : LabelDiscoveryState::LimitExceeded;
      const auto limitedForward = discoverLabelCandidates(file.metadata, options);
      const auto limitedReverse = discoverDataForLabel(label, options);
      QCOMPARE(limitedForward.state, expected);
      QCOMPARE(limitedReverse.state, expected);
      QVERIFY(limitedForward.candidates.empty() && limitedReverse.candidates.empty());
    }
    write(label, " \n\t");
    QCOMPARE(discoverLabelCandidates(file.metadata).state, LabelDiscoveryState::Empty);
    QCOMPARE(discoverDataForLabel(label).state, LabelDiscoveryState::Empty);
  }

  void wavPairCollisionsRequireExplicitSelection() {
    QTemporaryDir dir;
    FileState file;
    file.metadata = wavPair(dir, fixture("generator-60m-10ms.txt"), 60000000, 600000, 0);
    const auto label = dir.filePath("label/name.txt");
    const auto raw = dir.filePath("data/name.data");
    sizeFile(raw, 2400000);
    const auto reverse = discoverDataForLabel(label);
    QCOMPARE(reverse.state, LabelDiscoveryState::Ambiguous);
    QCOMPARE(reverse.candidates.size(), std::size_t{2});
    QStringList paths;
    for (const auto& candidate : reverse.candidates) {
      paths.push_back(candidate.dataPath);
      QCOMPARE(candidate.profile, LabelProfile::GeneratorText);
      FileState selected;
      selected.metadata = candidate.dataPath == raw ? target(raw) : file.metadata;
      QCOMPARE(prepareDiscoveredLabels(candidate, selected).disposition,
               LabelImportDisposition::Ready);
    }
    QVERIFY(paths.contains(raw));
    QVERIFY(paths.contains(dir.filePath("data/name.wav")));
    QVERIFY(file.marks.empty());
    QVERIFY(QFile::remove(raw));
    write(dir.filePath("data/name.txt"), "BPSK,1,10,-100,100\n");
    const auto forward = discoverLabelCandidates(file.metadata);
    QCOMPARE(forward.state, LabelDiscoveryState::Ambiguous);
    QCOMPARE(forward.candidates.size(), std::size_t{2});
    for (const auto& candidate : forward.candidates) {
      const bool known = candidate.labelPath == label;
      QCOMPARE(candidate.profile, known ? LabelProfile::GeneratorText : LabelProfile::MatlabDataTxt);
      QCOMPARE(prepareDiscoveredLabels(candidate, file).disposition,
               known ? LabelImportDisposition::Ready : LabelImportDisposition::NeedsMapping);
    }
    // Neither a similar stem nor an adjacent label replaces the strict pair.
    QVERIFY(QFile::rename(dir.filePath("data/name.wav"), dir.filePath("data/name-other.wav")));
    const auto missing = discoverDataForLabel(label);
    QCOMPARE(missing.state, LabelDiscoveryState::NotFound);
    QVERIFY(missing.candidates.empty());
    QCOMPARE(prepareDiscoveredLabels(reverse.candidates[0], file).disposition,
             LabelImportDisposition::Rejected);
  }

  void wavPairCasingIsStrict_data() {
    QTest::addColumn<QString>("before");
    QTest::addColumn<QString>("after");
    QTest::addColumn<QString>("dataPath");
    QTest::addColumn<QString>("labelPath");
    QTest::newRow("data-directory") << "data" << "Data" << "Data/name.wav" << "label/name.txt";
    QTest::newRow("label-directory") << "label" << "Label" << "data/name.wav" << "Label/name.txt";
    QTest::newRow("wav-suffix") << "data/name.wav" << "data/name.WAV" << "data/name.WAV" << "label/name.txt";
    QTest::newRow("txt-suffix") << "label/name.txt" << "label/name.TXT" << "data/name.wav" << "label/name.TXT";
    QTest::newRow("data-stem") << "data/name.wav" << "data/NAME.wav" << "data/NAME.wav" << "label/name.txt";
    QTest::newRow("label-stem") << "label/name.txt" << "label/NAME.txt" << "data/name.wav" << "label/NAME.txt";
  }

  void wavPairCasingIsStrict() {
    QFETCH(QString, before);
    QFETCH(QString, after);
    QFETCH(QString, dataPath);
    QFETCH(QString, labelPath);
    QTemporaryDir dir;
    auto metadata = wavPair(dir, "BPSK,1,10,-100,100\n");
    QDir base(dir.path());
    QVERIFY(base.rename(before, "temporary"));
    QVERIFY(base.rename("temporary", after));
    metadata.path = dir.filePath(dataPath).toStdString();
    const auto forward = discoverLabelCandidates(metadata);
    const auto reverse = discoverDataForLabel(dir.filePath(labelPath));
    QCOMPARE(forward.state, LabelDiscoveryState::Ambiguous);
    QCOMPARE(reverse.state, LabelDiscoveryState::Ambiguous);
    QVERIFY(forward.candidates.empty() && reverse.candidates.empty());
  }

  void wavPairAliasesAreAmbiguous_data() {
    QTest::addColumn<QString>("relativePath");
    QTest::newRow("wav-file") << "data/name.wav";
    QTest::newRow("txt-file") << "label/name.txt";
    QTest::newRow("data-directory") << "data";
    QTest::newRow("label-directory") << "label";
  }

  void wavPairAliasesAreAmbiguous() {
    QFETCH(QString, relativePath);
    QTemporaryDir dir;
    const auto metadata = wavPair(dir, "BPSK,1,10,-100,100\n");
    const auto alias = dir.filePath(relativePath);
    const auto destination = dir.filePath("relocated");
    const bool directory = QFileInfo(alias).isDir();
    QVERIFY(QDir(dir.path()).rename(relativePath, "relocated"));
    std::error_code error;
    const auto from = std::filesystem::path(destination.toStdWString());
    const auto to = std::filesystem::path(alias.toStdWString());
    if (directory) std::filesystem::create_directory_symlink(from, to, error);
    else std::filesystem::create_symlink(from, to, error);
    if (error) QSKIP("Creating filesystem symlinks is unavailable on this host");
    const auto forward = discoverLabelCandidates(metadata);
    const auto reverse = discoverDataForLabel(dir.filePath("label/name.txt"));
    QCOMPARE(forward.state, LabelDiscoveryState::Ambiguous);
    QCOMPARE(reverse.state, LabelDiscoveryState::Ambiguous);
    QVERIFY(forward.candidates.empty() && reverse.candidates.empty());
  }

  void casingAndMultipleCandidatesAreExplicit() {
    QTemporaryDir dir;
    const auto metadata = matlabPair(dir, "BPSK,1,10,-100,100\n");
    QVERIFY(QFile::rename(dir.filePath("label/name.txt"), dir.filePath("label/temporary.txt")));
    QVERIFY(QFile::rename(dir.filePath("label/temporary.txt"), dir.filePath("label/NAME.txt")));
    QCOMPARE(discoverLabelCandidates(metadata).state, LabelDiscoveryState::Ambiguous);
    QVERIFY(QFile::rename(dir.filePath("label/NAME.txt"), dir.filePath("label/temporary.txt")));
    QVERIFY(QFile::rename(dir.filePath("label/temporary.txt"), dir.filePath("label/name.txt")));
    write(dir.filePath("data/name.txt"), "BPSK,1,10,-100,100\n");
    const auto ambiguous = discoverLabelCandidates(metadata);
    QCOMPARE(ambiguous.state, LabelDiscoveryState::Ambiguous);
    QCOMPARE(ambiguous.candidates.size(), std::size_t{2});
    FileState file;
    file.metadata = metadata;
    const auto selected = prepareDiscoveredLabels(ambiguous.candidates[0], file, 0, sampleMapping());
    QCOMPARE(selected.disposition, LabelImportDisposition::Ready);
  }

  void discoveryAndParserBudgetsAndCancellation() {
    QTemporaryDir dir;
    const auto metadata = matlabPair(dir, "BPSK,1,10,-100,100\nQPSK,11,20,-100,100\n");
    LabelDiscoveryOptions options;
    options.maximumDirectoryEntries = 1;
    QCOMPARE(discoverLabelCandidates(metadata, options).state, LabelDiscoveryState::LimitExceeded);
    options = {};
    options.parsing.maximumBytes = 4;
    QCOMPARE(discoverLabelCandidates(metadata, options).state, LabelDiscoveryState::LimitExceeded);
    options = {};
    options.parsing.cancelled = [] { return true; };
    QCOMPARE(discoverLabelCandidates(metadata, options).state, LabelDiscoveryState::Cancelled);
    const auto cancelled = importMappedTextLabels(dir.filePath("label/name.txt"), metadata,
                                                  sampleMapping(), 0, options.parsing);
    QCOMPARE(cancelled.state, LabelImportState::Cancelled);
    QVERIFY(cancelled.annotations.empty());
    options = {};
    options.parsing.timeoutMs = 1;
    options.parsing.cancelled = [] { QTest::qSleep(20); return false; };
    QCOMPARE(discoverLabelCandidates(metadata, options).state, LabelDiscoveryState::LimitExceeded);
    LabelImportOptions limit;
    limit.maximumRecords = 1;
    const auto rejected = importMappedTextLabels(dir.filePath("label/name.txt"), metadata,
                                                 sampleMapping(), 0, limit);
    QCOMPARE(rejected.state, LabelImportState::LimitExceeded);
    QVERIFY(rejected.annotations.empty());
    limit = {};
    limit.maximumLines = 1;
    QCOMPARE(importMappedTextLabels(dir.filePath("label/name.txt"), metadata,
                                    sampleMapping(), 0, limit).state, LabelImportState::LimitExceeded);
    write(dir.filePath("label/name.txt"), QByteArray("BPSK,1,10,-100,100\n") + QByteArray::fromHex("c3"));
    QVERIFY(!importMappedTextLabels(dir.filePath("label/name.txt"), metadata, sampleMapping()).ok());
  }

  void mappedIndicesUnitsAndSavedMapping() {
    QTemporaryDir dir;
    const auto metadata = matlabPair(dir, "BPSK,1,10,-100,100\n");
    const auto imported = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, sampleMapping());
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    QCOMPARE(imported.sourceSet.formatId, std::string("mapped-txt-v1"));
    const auto& range = imported.annotations[0].segments[0].range;
    QCOMPARE(range.time.begin, quint64{0});
    QCOMPARE(range.time.end, quint64{10});
    QCOMPARE(range.frequency.lowerHz, 9900.0);
    QCOMPARE(range.frequency.upperHz, 10100.0);
    LabelFieldMapping restored;
    QString error;
    QVERIFY(restoreLabelFieldMapping(imported.sourceSet, restored, error));
    QCOMPARE(encodeLabelFieldMapping(restored), encodeLabelFieldMapping(sampleMapping()));
    const auto again = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, restored, 99);
    QCOMPARE(again.sourceSet.id, imported.sourceSet.id);
    auto invalid = encodeLabelFieldMapping(restored);
    invalid["interval"] = 99;
    QVERIFY(!decodeLabelFieldMapping(invalid, restored, error));
    QCOMPARE(encodeLabelFieldMapping(restored), encodeLabelFieldMapping(sampleMapping()));
    auto rf = sampleMapping();
    rf.frequencyReference = LabelFrequencyReference::RF;
    rf.frequencyUnit = LabelFrequencyUnit::KHz;
    rf.frequencyLayout = LabelFrequencyLayout::CenterBandwidth;
    write(dir.filePath("label/name.txt"), "QAM,1,10,10,0.2\n");
    const auto absolute = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, rf);
    QVERIFY2(absolute.ok(), qPrintable(absolute.error));
    QCOMPARE(absolute.annotations[0].segments[0].range.frequency.lowerHz, 9900.0);
  }

  void mappedRoundingFramesAndPhysicalRejection() {
    QTemporaryDir dir;
    const auto metadata = matlabPair(dir, "BPSK,0.0005,0.0015,-100,100\n");
    auto mapping = sampleMapping();
    mapping.timeUnit = LabelTimeUnit::Seconds;
    mapping.indexOrigin = 0;
    mapping.interval = LabelInterval::ExclusiveEnd;
    auto imported = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, mapping);
    QVERIFY(!imported.ok() && imported.annotations.empty());
    mapping.allowTimeRounding = true;
    imported = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, mapping);
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    QCOMPARE(imported.annotations[0].segments[0].range.time.begin, quint64{1});
    QCOMPARE(imported.annotations[0].segments[0].range.time.end, quint64{2});
    QCOMPARE(imported.annotations[0].segments[0].timeQuality, TimeQuality::RoundedLabel);
    mapping = sampleMapping();
    mapping.timeUnit = LabelTimeUnit::Frames;
    mapping.frameHopSamples = 5;
    mapping.frameLengthSamples = 10;
    write(dir.filePath("label/name.txt"), "BPSK,2,3,-100,100\n");
    imported = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, mapping);
    QVERIFY(imported.ok());
    QCOMPARE(imported.annotations[0].segments[0].range.time.begin, quint64{5});
    QCOMPARE(imported.annotations[0].segments[0].range.time.end, quint64{20});
    for (const auto& bad : {"BPSK,0,10,-100,100\n", "BPSK,1,18446744073709551615,-100,100\n",
                            "BPSK,1,10,nan,100\n", "BPSK,1,10,-600,100\n",
                            "BPSK,1,10,-100,100\nQPSK,1,10,-100,99999\n"}) {
      write(dir.filePath("label/name.txt"), bad);
      imported = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, mapping);
      QVERIFY2(!imported.ok() && imported.annotations.empty(), bad);
    }
    mapping = sampleMapping();
    mapping.delimiter = LabelDelimiter::Tab;
    write(dir.filePath("label/name.txt"), "\t1\t10\t-100\t100\n");
    QVERIFY(!importMappedTextLabels(dir.filePath("label/name.txt"), metadata, mapping).ok());
  }

  void unknownTimeObservationAndClassOnlyAreNotRectangles() {
    QTemporaryDir dir;
    const auto metadata = matlabPair(dir, "BPSK,1,10,-100,100\n");
    auto mapping = sampleMapping();
    mapping.timeUnit = LabelTimeUnit::Unknown;
    auto imported = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, mapping);
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    QVERIFY(imported.annotations[0].segments.empty());
    QVERIFY(imported.annotations[0].observation.has_value());
    QCOMPARE(imported.annotations[0].observationTimeQuality, TimeQuality::Unknown);
    QVERIFY(imported.annotations[0].originEvidence->originalObservation.has_value());
    mapping.frequencyLayout = LabelFrequencyLayout::Unknown;
    imported = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, mapping);
    QCOMPARE(imported.state, LabelImportState::Incomplete);
    QCOMPARE(imported.incompleteRecords, std::size_t{1});
    QVERIFY(imported.annotations.empty());
    QVERIFY(!imported.sourceSet.originalText.empty());
    write(dir.filePath("label/name.txt"), "\n \t\n");
    QCOMPARE(discoverLabelCandidates(metadata).state, LabelDiscoveryState::Empty);
    const auto empty = importMappedTextLabels(dir.filePath("label/name.txt"), metadata, sampleMapping());
    QCOMPARE(empty.state, LabelImportState::Empty);
    QVERIFY(empty.ok() && empty.annotations.empty());
  }

  void generatorDiscoveryAndSafePublication() {
    QTemporaryDir dir;
    const auto data = dir.filePath("capture.raw");
    const auto label = dir.filePath("capture.txt");
    sizeFile(data, 2400000);
    write(label, fixture("generator-60m-10ms.txt"));
    FileState file;
    file.metadata = target(data);
    file.metadata.availability.fingerprint = "known-data";
    auto discovered = discoverLabelCandidates(file.metadata);
    QCOMPARE(discovered.state, LabelDiscoveryState::Found);
    QCOMPARE(discovered.candidates[0].profile, LabelProfile::GeneratorText);
    const auto prepared = prepareDiscoveredLabels(discovered.candidates[0], file, 7);
    QCOMPARE(prepared.disposition, LabelImportDisposition::Ready);
    AnnotationService service;
    QVERIFY(!publishDiscoveredLabels(service, file, prepared, 8).ok);
    QVERIFY(file.marks.empty());
    QVERIFY(publishDiscoveredLabels(service, file, prepared, 7).ok);
    QVERIFY(service.createManual(file, "Manual", {{0, 100}, {-100, 100}}).ok);
    const auto count = file.marks.size();
    const auto annotationId = file.marks[0].id;
    QVERIFY(service.rename(file, annotationId, "User revised", "keep").ok);
    const auto again = prepareDiscoveredLabels(discovered.candidates[0], file, 7);
    QCOMPARE(again.disposition, LabelImportDisposition::AlreadyImported);
    QVERIFY(!publishDiscoveredLabels(service, file, again, 7).changed);
    QCOMPARE(file.marks.size(), count);
    QCOMPARE(findAnnotation(file, annotationId)->name, std::string("User revised"));
    write(label, fixture("generator-60m-10ms.txt") + "\n");
    const auto changed = prepareDiscoveredLabels(discovered.candidates[0], file, 7);
    QCOMPARE(changed.disposition, LabelImportDisposition::SourceChanged);
    QVERIFY(!publishDiscoveredLabels(service, file, changed, 7).ok);
    // A prior plan must also notice a new source published in the meantime.
    QCOMPARE(file.marks.size(), count);
    auto growing = file;
    growing.metadata.availability.status = LoadStatus::Partial;
    growing.metadata.availability.availableSamples = 1000;
    QVERIFY(publishDiscoveredLabels(service, growing, prepared, 7).ok);
    auto metadataChanged = file;
    metadataChanged.metadata.availability.fingerprint = "different-data";
    QVERIFY(!publishDiscoveredLabels(service, metadataChanged, prepared, 7).ok);
  }

  void incompleteEvidenceConfirmationCancellationDeduplicationAndUndo() {
    QTemporaryDir dir;
    FileState file;
    file.metadata = matlabPair(dir, "BPSK\nQAM\n");
    const auto candidate = discoverLabelCandidates(file.metadata).candidates[0];
    auto mapping = sampleMapping();
    mapping.timeUnit = LabelTimeUnit::Unknown;
    mapping.frequencyLayout = LabelFrequencyLayout::Unknown;
    const auto prepared = prepareDiscoveredLabels(candidate, file, 4, mapping);
    QCOMPARE(prepared.disposition, LabelImportDisposition::IncompleteEvidence);
    QCOMPARE(prepared.imported.state, LabelImportState::Incomplete);
    QVERIFY(!prepared.imported.error.isEmpty());
    QCOMPARE(prepared.imported.incompleteRecords, std::size_t{2});
    QVERIFY(prepared.imported.annotations.empty());
    QVERIFY(prepared.classCounts.empty());
    QVERIFY(!prepared.timeExtent && !prepared.frequencyExtent);
    // Preview/confirmation cancelled: no publication has happened.
    QVERIFY(file.sourceSets.empty() && file.marks.empty());
    LabelDiscoveryOptions options;
    options.parsing.cancelled = [] { return true; };
    const auto cancelled = prepareDiscoveredLabels(candidate, file, 4, mapping, options);
    AnnotationService service;
    QVERIFY(!publishDiscoveredLabels(service, file, cancelled, 4).ok);
    QVERIFY(file.sourceSets.empty() && file.marks.empty());
    const auto published = publishDiscoveredLabels(service, file, prepared, 4);
    QVERIFY2(published.ok, published.error.c_str());
    QVERIFY(published.changed);
    QCOMPARE(file.sourceSets.size(), std::size_t{1});
    QVERIFY(file.marks.empty());
    QCOMPARE(file.sourceSets.front().originalText, std::string("BPSK\nQAM\n"));
    QVERIFY(!file.sourceSets.front().diagnostics.empty());
    const auto sourceId = file.sourceSets.front().id;
    LabelFieldMapping restored;
    QString error;
    QVERIFY(restoreLabelFieldMapping(file.sourceSets.front(), restored, error));
    QCOMPARE(encodeLabelFieldMapping(restored), encodeLabelFieldMapping(mapping));
    AnnotationSourceSet decoded;
    QVERIFY(decodeSourceSet(encodeSourceSet(file.sourceSets.front()), file.metadata, decoded, error));
    QCOMPARE(decoded.originalText, file.sourceSets.front().originalText);
    QCOMPARE(decoded.coordinateInterpretation, file.sourceSets.front().coordinateInterpretation);
    QJsonObject index;
    const auto projectPath = dir.filePath("evidence-only.ssproj");
    QVERIFY2(writeAnnotationStore(projectPath, file, index, error), qPrintable(error));
    FileState reopened;
    reopened.metadata = file.metadata;
    QVERIFY2(readAnnotationStore(projectPath, index, reopened, error), qPrintable(error));
    QCOMPARE(reopened.sourceSets.size(), std::size_t{1});
    QVERIFY(reopened.marks.empty());
    QCOMPARE(reopened.sourceSets.front().originalText, file.sourceSets.front().originalText);
    QVERIFY(restoreLabelFieldMapping(reopened.sourceSets.front(), restored, error));
    const auto again = prepareDiscoveredLabels(candidate, file, 4, restored);
    QCOMPARE(again.disposition, LabelImportDisposition::AlreadyImported);
    QVERIFY(!publishDiscoveredLabels(service, file, again, 4).changed);
    QCOMPARE(file.sourceSets.size(), std::size_t{1});
    QVERIFY(service.undo(file).ok);
    QVERIFY(file.sourceSets.empty() && file.marks.empty());
    QVERIFY(service.redo(file).ok);
    QCOMPARE(file.sourceSets.front().id, sourceId);
    write(dir.filePath("label/name.txt"), "BPSK\nQAM\nFSK\n");
    const auto changed = prepareDiscoveredLabels(candidate, file, 4, restored);
    QCOMPARE(changed.disposition, LabelImportDisposition::SourceChanged);
    QVERIFY(!publishDiscoveredLabels(service, file, changed, 4).ok);
    QCOMPARE(file.sourceSets.size(), std::size_t{1});
    QVERIFY(file.marks.empty());
  }

  void mappedProfileUsesAnnotationServiceAndProfileChangesRequireReview() {
    QTemporaryDir dir;
    FileState file;
    file.metadata = matlabPair(dir, "BPSK,1,10,-100,100\n");
    const auto candidate = discoverLabelCandidates(file.metadata).candidates[0];
    const auto prepared = prepareDiscoveredLabels(candidate, file, 1, sampleMapping());
    QCOMPARE(prepared.disposition, LabelImportDisposition::Ready);
    AnnotationService service;
    const auto published = publishDiscoveredLabels(service, file, prepared, 1);
    // Integration must register mapped-txt-v1 in domain/annotation.cpp.
    QVERIFY2(published.ok, published.error.c_str());
    QCOMPARE(file.marks.size(), std::size_t{1});
    QVERIFY(validateFileAnnotations(file).ok);
    const auto snapshot = file;
    auto changedMapping = sampleMapping();
    changedMapping.interval = LabelInterval::ExclusiveEnd;
    const auto changed = prepareDiscoveredLabels(candidate, file, 1, changedMapping);
    QCOMPARE(changed.disposition, LabelImportDisposition::SourceChanged);
    QVERIFY(!publishDiscoveredLabels(service, file, changed, 1).ok);
    LabelFieldMapping restored;
    QString error;
    QVERIFY(restoreLabelFieldMapping(file.sourceSets.front(), restored, error));
    const auto reused = reusableLabelFieldMapping(file, candidate.labelPath);
    QVERIFY(reused);
    QCOMPARE(encodeLabelFieldMapping(*reused), encodeLabelFieldMapping(restored));
    QVERIFY(!reusableLabelFieldMapping(file, dir.filePath("moved/" + QFileInfo(candidate.labelPath).fileName())));
    auto wrongBinding = file;
    wrongBinding.metadata.sampleRateHz *= 2;
    QVERIFY(!reusableLabelFieldMapping(wrongBinding, candidate.labelPath));
    auto conflicting = file;
    auto other = conflicting.sourceSets.front();
    other.coordinateInterpretation = QJsonDocument(encodeLabelFieldMapping(changedMapping)).toJson(QJsonDocument::Compact).toStdString();
    conflicting.sourceSets.push_back(other);
    QVERIFY(!reusableLabelFieldMapping(conflicting, candidate.labelPath));
    QCOMPARE(prepareDiscoveredLabels(candidate, file, 2, restored).disposition,
             LabelImportDisposition::AlreadyImported);
    file = snapshot;
    QVERIFY(service.deleteBatch(file, {file.marks[0].id}).ok);
    const auto deleted = prepareDiscoveredLabels(candidate, file, 2, restored);
    QCOMPARE(deleted.disposition, LabelImportDisposition::AlreadyImported);
    QVERIFY(!publishDiscoveredLabels(service, file, deleted, 2).changed);
    QVERIFY(file.marks[0].deleted);
  }

  void sigmfEmptyIsValidAndSupplementalConflictsRemainErrors() {
    QTemporaryDir dir;
    const auto metadata = torchTarget(dir);
    const auto base = baseOf(metadata);
    auto meta = readJson(base + ".sigmf-meta");
    meta["annotations"] = QJsonArray{};
    write(base + ".sigmf-meta", QJsonDocument(meta).toJson());
    auto imported = importTorchSigLabels(base + ".sigmf-meta", metadata);
    QVERIFY(!imported.ok());  // Four supplemental events conflict with zero meta records.
    auto extra = readJson(base + ".json");
    extra["events"] = QJsonArray{};
    extra["n_events"] = 0;
    write(base + ".json", QJsonDocument(extra).toJson());
    imported = importTorchSigLabels(base + ".sigmf-meta", metadata);
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    QCOMPARE(imported.state, LabelImportState::Empty);
    QVERIFY(imported.annotations.empty());
    LabelImportOptions depth;
    depth.maximumJsonDepth = 1;
    QCOMPARE(importTorchSigLabels(base + ".sigmf-meta", metadata, 0, false, depth).state,
             LabelImportState::LimitExceeded);
    FileState file;
    file.metadata = metadata;
    const auto found = discoverLabelCandidates(metadata);
    QCOMPARE(found.state, LabelDiscoveryState::Found);
    const auto prepared = prepareDiscoveredLabels(found.candidates[0], file);
    QCOMPARE(prepared.disposition, LabelImportDisposition::Empty);
  }

  void generatorFixtureAndPhysicalBinding() {
    QTemporaryDir dir;
    const auto path = dir.filePath("generator.txt"),
               data = dir.filePath("samples.raw");
    const auto raw = fixture("generator-60m-10ms.txt");
    write(path, raw);
    sizeFile(data, 2400000);
    const auto m = target(data);
    const auto imported = importGeneratorLabels(path, m);
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    QCOMPARE(imported.annotations.size(), std::size_t{12});
    std::size_t count = 0;
    for (const auto& a : imported.annotations) {
      count += a.segments.size();
      QCOMPARE(a.reviewStatus, ReviewStatus::Pending);
      QCOMPARE(a.sourceKind, AnnotationSource::ImportedLabel);
      QVERIFY(a.originEvidence && !a.originEvidence->scoreMean &&
              !a.originEvidence->scoreMax);
      QVERIFY(validateAnnotation(a, annotationBounds(m)).ok);
    }
    QCOMPARE(count, std::size_t{17});
    const auto& a = imported.annotations.front();
    QCOMPARE(a.segments.size(), std::size_t{2});
    QCOMPARE(a.segments[0].range.time.begin, quint64{180000});
    QCOMPARE(a.segments[0].range.time.end, quint64{300000});
    QCOMPARE(a.segments[1].range.time.begin, quint64{420000});
    QCOMPARE(a.segments[1].range.time.end, quint64{600000});
    QCOMPARE(a.segments[0].uncertaintySamples, quint64{301});
    QCOMPARE(a.segments[0].range.frequency.lowerHz,
             801339.0 - 30000000 - 229890.0 / 2);
    QCOMPARE(imported.sourceSet.originalText, raw.toStdString());
    const auto second = importGeneratorLabels(path, m, 123);
    QCOMPARE(second.sourceSet.id, imported.sourceSet.id);
    QCOMPARE(second.annotations.front().id, a.id);
    auto changed = m;
    changed.centerFrequencyHz = 100000000;
    const auto shifted = importGeneratorLabels(path, changed);
    QVERIFY(shifted.ok());
    QVERIFY(shifted.sourceSet.id != imported.sourceSet.id);
    QCOMPARE(shifted.annotations[0].segments[0].range.frequency.lowerHz,
             a.segments[0].range.frequency.lowerHz + 100000000);
    changed = m;
    changed.sampleCount--;
    QVERIFY(!importGeneratorLabels(path, changed).ok());
    changed = m;
    changed.sampleRateHz /= 2;
    QVERIFY(!importGeneratorLabels(path, changed).ok());
    auto partial = m;
    partial.availability.status = LoadStatus::Partial;
    partial.availability.availableSamples = 200000;
    const auto tail = importGeneratorLabels(path, partial);
    QVERIFY(tail.ok());
    QCOMPARE(tail.annotations[0].segments[1].range.time.end, quint64{600000});
    QVERIFY(std::any_of(
        tail.diagnostics.begin(), tail.diagnostics.end(),
        [](const auto& d) { return d.code == "outside-available-prefix"; }));
  }
  void generatorRoundingAndRejectWholeBatch() {
    QTemporaryDir dir;
    const auto path = dir.filePath("labels.txt"),
               data = dir.filePath("samples.raw");
    sizeFile(data, 1234567 * 4);
    const auto m = target(data, 1234567, 1234567);
    const QByteArray good =
        "1: sig_len:1234567 time_end:1.00000 time_start:0.00000\r\n 1): SNR:2 "
        "CNR:3 SPS:4 FD:5 LEN:1234567 BW:100000 FC:617283 sys:BPSK mod:BPSK "
        "TIME:0.00001,0.00002 0.50000,1.00000 \r\n";
    write(path, good);
    const auto result = importGeneratorLabels(path, m);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.annotations[0].segments[0].range.time.begin, quint64{12});
    QCOMPARE(result.annotations[0].segments[0].range.time.end, quint64{25});
    QCOMPARE(result.annotations[0].segments[0].uncertaintySamples, quint64{7});
    QCOMPARE(result.sourceSet.originalText, good.toStdString());
    const auto fields =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(
                result.annotations[0].originEvidence->originalFieldsJson))
            .object();
    QCOMPARE(fields["textQuantizationSeconds"].toDouble(), 1e-5);
    QCOMPARE(fields["fields"].toObject()["CNR"].toString(), QString("3"));
    QCOMPARE(fields["fields"].toObject()["SNR"].toString(), QString("2"));
    for (const auto& replacement :
         std::vector<std::pair<QByteArray, QByteArray>>{
             {"mod:BPSK", "mod:"},
             {"FC:617283", "FC:9999999"},
             {"BW:100000", "BW:9999999"},
             {"SNR:2", "SNR:nan"},
             {"LEN:1234567", "LEN:1234566"},
             {"TIME:0.00001,0.00002", "TIME:0.00001 0.00002"},
             {"0.50000,1.00000", "0.50000,1.00001"},
             {"SPS:4", "SPS:4 SPS:5"},
             {"sig_len:1234567", "sig_len:18446744073709551616"}}) {
      auto bad = good;
      bad.replace(replacement.first, replacement.second);
      write(path, bad);
      const auto rejected = importGeneratorLabels(path, m);
      QVERIFY2(!rejected.ok(), replacement.first.constData());
      QVERIFY(rejected.annotations.empty());
    }
    write(path, good + good.mid(good.indexOf(" 1):")));
    const auto duplicate = importGeneratorLabels(path, m);
    QVERIFY(!duplicate.ok() && duplicate.annotations.empty());
  }
  void torchSigFixtureAndOrderIndependentMatching() {
    QTemporaryDir dir;
    auto m = torchTarget(dir);
    const auto base = baseOf(m);
    auto result = importTorchSigLabels(base + ".sigmf-meta", m);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.annotations.size(), std::size_t{4});
    const auto& segment = result.annotations[0].segments[0];
    QCOMPARE(segment.range.time.begin, quint64{51107});
    QCOMPARE(segment.range.time.end, quint64{343277});
    QCOMPARE(segment.range.frequency.lowerHz, 2502021605.9327126);
    QCOMPARE(segment.range.frequency.upperHz, 2502282009.1227803);
    QCOMPARE(*result.annotations[0].classLabel, std::string("64qam"));
    QCOMPARE(segment.timeQuality, TimeQuality::SampleLabel);
    QCOMPARE(segment.uncertaintySamples, quint64{0});
    auto json = readJson(base + ".json");
    const auto events = json["events"].toArray();
    QJsonArray reverse;
    for (qsizetype i = events.size(); i > 0; --i) reverse.append(events[i - 1]);
    json["events"] = reverse;
    json["unknown_optional"] = QJsonObject{
        {"nullable", QJsonValue(QJsonValue::Null)}, {"detail", "kept"}};
    write(base + ".json", QJsonDocument(json).toJson());
    result = importTorchSigLabels(base + ".json", m);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.annotations.size(), std::size_t{4});
    const auto raw =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(
                result.annotations[0].originEvidence->originalFieldsJson))
            .object();
    QCOMPARE(raw["rfbenchEvent"].toObject()["event_id"].toInteger(), qint64{0});
    QCOMPARE(raw["rfbenchHeaderReference"].toObject()["sourceHash"].toString(),
             QString::fromStdString(result.sourceSet.supplementalHash));
    QVERIFY(
        QJsonDocument::fromJson(QByteArray::fromStdString(
                                    result.sourceSet.supplementalOriginalText))
            .object()
            .contains("unknown_optional"));
    QCOMPARE(importLabelSource(base + ".sigmf-data", m).annotations.size(),
             std::size_t{4});
    m.availability.status = LoadStatus::Partial;
    m.availability.availableSamples = 100000;
    const auto partial = importTorchSigLabels(base + ".sigmf-meta", m);
    QVERIFY(partial.ok());
    QCOMPARE(partial.annotations[0].segments[0].range.time.end,
             quint64{343277});
    QFile::remove(base + ".json");
    const auto metaOnly = importTorchSigLabels(base + ".sigmf-meta", m);
    QVERIFY(metaOnly.ok());
    QVERIFY(metaOnly.sourceSet.supplementalHash.empty());
  }
  void torchSigConflictsAndUnsupported() {
    QTemporaryDir dir;
    const auto m = torchTarget(dir);
    const auto base = baseOf(m);
    const auto validMeta = fixture("torchsig-0000000.sigmf-meta"),
               validJson = fixture("torchsig-0000000.json");
    const auto rejectMeta = [&](const QJsonObject& meta) {
      write(base + ".sigmf-meta", QJsonDocument(meta).toJson());
      const auto result = importTorchSigLabels(base + ".sigmf-meta", m);
      QVERIFY2(!result.ok(), qPrintable(result.error));
      QVERIFY(result.annotations.empty());
      write(base + ".sigmf-meta", validMeta);
    };
    for (const auto& key : QStringList{"core:label", "core:freq_upper_edge",
                                       "core:sample_count"}) {
      auto meta = QJsonDocument::fromJson(validMeta).object();
      auto records = meta["annotations"].toArray();
      auto item = records[0].toObject();
      item.remove(key);
      records[0] = item;
      meta["annotations"] = records;
      rejectMeta(meta);
    }
    {
      auto meta = QJsonDocument::fromJson(validMeta).object();
      auto records = meta["annotations"].toArray();
      auto item = records[0].toObject();
      item["core:label"] = 42;
      records[0] = item;
      meta["annotations"] = records;
      rejectMeta(meta);
      item["core:label"] = "bpsk";
      item["core:sample_start"] = 1e30;
      records[0] = item;
      meta["annotations"] = records;
      rejectMeta(meta);
    }
    for (const auto& key : QStringList{"core:offset", "core:num_channels",
                                       "core:datatype", "core:dataset"}) {
      auto meta = QJsonDocument::fromJson(validMeta).object();
      auto global = meta["global"].toObject();
      global[key] = key == "core:datatype" ? QJsonValue("ci16_le")
                    : key == "core:dataset"
                        ? QJsonValue("../outside.sigmf-data")
                        : QJsonValue(2);
      meta["global"] = global;
      rejectMeta(meta);
    }
    {
      auto meta = QJsonDocument::fromJson(validMeta).object();
      auto captures = meta["captures"].toArray();
      captures.append(captures[0]);
      meta["captures"] = captures;
      rejectMeta(meta);
    }
    {
      auto json = QJsonDocument::fromJson(validJson).object();
      auto events = json["events"].toArray();
      auto item = events[0].toObject();
      item["freq_upper_hz"] = item["freq_upper_hz"].toDouble() + .001;
      events[0] = item;
      json["events"] = events;
      write(base + ".json", QJsonDocument(json).toJson());
      const auto result = importTorchSigLabels(base + ".sigmf-meta", m);
      QVERIFY(!result.ok() && result.annotations.empty());
      const auto explicitMeta =
          importTorchSigLabels(base + ".sigmf-meta", m, 0, true);
      QVERIFY(explicitMeta.ok());
      QVERIFY(!explicitMeta.sourceSet.supplementalHash.empty());
      write(base + ".json", validJson);
    }
    {
      auto json = QJsonDocument::fromJson(validJson).object();
      auto events = json["events"].toArray();
      events[1] = events[0];
      json["events"] = events;
      write(base + ".json", QJsonDocument(json).toJson());
      QVERIFY(!importTorchSigLabels(base + ".sigmf-meta", m).ok());
      write(base + ".json", validJson);
    }
    sizeFile(QString::fromStdString(m.path), 3199999);
    QVERIFY(!importTorchSigLabels(base + ".sigmf-meta", m).ok());
    QVERIFY(!importLabelSource(base + ".yolo", m).ok());
  }
  void codecFullRoundtripAndExports() {
    QTemporaryDir dir;
    auto m = torchTarget(dir);
    const auto imported = importTorchSigLabels(baseOf(m) + ".sigmf-meta", m);
    QVERIFY(imported.ok());
    auto a = imported.annotations[0];
    const auto before = a.segments[0];
    auto after = before;
    after.range.time.begin++;
    after.timeQuality = TimeQuality::Manual;
    after.frequencyQuality = FrequencyQuality::Manual;
    a.segments[0] = after;
    const auto old = a.revisionId;
    a.revisionId = a.id + ":r1";
    a.reviewRevisionId = a.revisionId;
    AnnotationRevision revision{
        a.revisionId, old, "geometry", "user", "2026-10-13T00:00:00Z", "note"};
    revision.segmentChanges.push_back({before.id, before, after});
    revision.nameBefore = a.name;
    revision.nameAfter = "edited";
    revision.noteBefore = a.note;
    revision.noteAfter = "note";
    a.revisions.push_back(revision);
    a.reviewHistory.push_back({ReviewStatus::Accepted, old, "user",
                               "2026-10-13T00:00:00Z",
                               "accepted old revision"});
    a.name = "edited";
    a.note = "note\nsecond line";
    a.derivedFrom = {"candidate-1"};
    refreshAnnotationProjection(a);
    Annotation decoded;
    QString error;
    const auto encoded = encodeAnnotation(a);
    QVERIFY2(decodeAnnotation(encoded, m, decoded, error), qPrintable(error));
    QCOMPARE(encodeAnnotation(decoded), encoded);
    QCOMPARE(*decoded.originEvidence, *a.originEvidence);
    QVERIFY(decoded.originEvidence->originalSegments[0].range.time.begin !=
            decoded.segments[0].range.time.begin);
    AnnotationSourceSet set;
    QVERIFY2(
        decodeSourceSet(encodeSourceSet(imported.sourceSet), m, set, error),
        qPrintable(error));
    QCOMPARE(set, imported.sourceSet);
    SourceIdentity identity;
    QVERIFY(decodeSourceIdentity(encodeSourceIdentity(set.sourceIdentity),
                                 identity, error));
    QCOMPARE(identity, set.sourceIdentity);
    AnnotationChannelSnapshot snapshot{a.id,
                                       a.segments[0].id,
                                       old,
                                       "original",
                                       before.range,
                                       AnnotationSource::ImportedLabel,
                                       ReviewStatus::Accepted,
                                       set.sourceIdentity},
        snapshotDecoded;
    QVERIFY2(decodeAnnotationChannelSnapshot(
                 encodeAnnotationChannelSnapshot(snapshot), m, snapshotDecoded,
                 error),
             qPrintable(error));
    QCOMPARE(snapshot, snapshotDecoded);
    FileState file;
    file.metadata = m;
    file.sourceSets = {set};
    file.marks = {a};
    const auto json = exportAnnotationsJson(file),
               csv = exportAnnotationsCsv(file);
    QVERIFY(QJsonDocument::fromJson(json).isObject());
    QVERIFY(json.endsWith('\n'));
    QVERIFY(!json.contains('\r'));
    QVERIFY(csv.contains("51108,343277"));
    QVERIFY(csv.endsWith('\n'));
    QCOMPARE(csv.count('\n'), 2);
    auto big = identity;
    big.projectGeneration = std::numeric_limits<quint64>::max();
    big.physicalSamples = (quint64{1} << 53) + 73;
    big.availableSamples = big.physicalSamples;
    big.hashedRange = TimeRange{(quint64{1} << 53) + 1, big.physicalSamples};
    QVERIFY(decodeSourceIdentity(encodeSourceIdentity(big), identity, error));
    QCOMPARE(identity, big);
    auto bad = encoded;
    auto segments = bad["segments"].toArray();
    auto segment = segments[0].toObject();
    auto range = segment["range"].toObject();
    for (const auto value : QJsonArray{51108, "051108", "18446744073709551616",
                                       "51108\n", "51108\r\n"}) {
      range["start"] = value;
      segment["range"] = range;
      segments[0] = segment;
      bad["segments"] = segments;
      QVERIFY(!decodeAnnotation(bad, m, decoded, error));
      QCOMPARE(encodeAnnotation(decoded), encoded);
    }
    bad = encoded;
    bad["reviewRevisionId"] = "missing";
    QVERIFY(!decodeAnnotation(bad, m, decoded, error));
    bad = encoded;
    bad["sourceKind"] = "unknown";
    QVERIFY(!decodeAnnotation(bad, m, decoded, error));
    bad = encoded;
    auto evidence = bad["originEvidence"].toObject();
    evidence["scoreMean"] = .9;
    bad["originEvidence"] = evidence;
    QVERIFY(!decodeAnnotation(bad, m, decoded, error));
    bad = encoded;
    auto revisions = bad["revisions"].toArray();
    auto item = revisions[0].toObject();
    item["parentRevisionId"] = item["id"];
    revisions[0] = item;
    bad["revisions"] = revisions;
    QVERIFY(!decodeAnnotation(bad, m, decoded, error));
  }
  void largeSharedAndRecordEvidenceRemainLossless() {
    QTemporaryDir directory;
    const auto metadata = torchTarget(directory);
    const auto base = baseOf(metadata);
    const auto originalMeta =
        QJsonDocument::fromJson(fixture("torchsig-0000000.sigmf-meta"))
            .object();
    const auto originalSupplemental =
        QJsonDocument::fromJson(fixture("torchsig-0000000.json")).object();
    const QString payload(15 * 1024 * 1024, QLatin1Char('L'));
    for (const auto& kind : QStringList{"global", "sigmfRecord",
                                        "rfbenchHeader", "rfbenchEvent"}) {
      auto primary = originalMeta;
      auto supplemental = originalSupplemental;
      if (kind == "global") {
        auto global = primary["global"].toObject();
        global["unknown_optional"] = payload;
        primary["global"] = global;
      } else if (kind == "sigmfRecord") {
        auto records = primary["annotations"].toArray();
        auto record = records[0].toObject();
        record["unknown_optional"] = payload;
        records[0] = record;
        primary["annotations"] = records;
      } else if (kind == "rfbenchHeader") {
        supplemental["unknown_optional"] = payload;
      } else {
        auto events = supplemental["events"].toArray();
        auto event = events[0].toObject();
        event["unknown_optional"] = payload;
        events[0] = event;
        QJsonArray reverse;
        for (qsizetype i = events.size(); i > 0; --i)
          reverse.append(events[i - 1]);
        supplemental["events"] = reverse;
      }
      const auto primaryBytes = QJsonDocument(primary).toJson();
      const auto supplementalBytes = QJsonDocument(supplemental).toJson();
      write(base + ".sigmf-meta", primaryBytes);
      write(base + ".json", supplementalBytes);
      const auto imported =
          importTorchSigLabels(base + ".sigmf-meta", metadata);
      QVERIFY2(imported.ok(), qPrintable(imported.error));
      QCOMPARE(imported.annotations.size(), std::size_t{4});
      for (const auto& annotation : imported.annotations) {
        QVERIFY(annotation.originEvidence->originalFieldsJson.size() < 4096);
        const auto origin =
            QJsonDocument::fromJson(
                QByteArray::fromStdString(
                    annotation.originEvidence->originalFieldsJson))
                .object();
        QVERIFY(!origin.contains("global") && !origin.contains("capture") &&
                !origin.contains("rfbenchHeader"));
        QCOMPARE(origin["globalReference"].toObject()["sourceHash"].toString(),
                 QString::fromStdString(imported.sourceSet.sourceHash));
      }
      const auto origin =
          QJsonDocument::fromJson(
              QByteArray::fromStdString(
                  imported.annotations[0].originEvidence->originalFieldsJson))
              .object();
      if (kind == "sigmfRecord") {
        const auto reference = origin["sigmfAnnotation"]
                                   .toObject()["snapshotReference"]
                                   .toObject();
        QCOMPARE(reference["jsonPointer"].toString(),
                 QString("/annotations/0"));
        const auto record = primary["annotations"].toArray()[0].toObject();
        QCOMPARE(reference["canonicalObjectSha256"].toString(),
                 QString::fromLatin1(
                     QCryptographicHash::hash(
                         QJsonDocument(record).toJson(QJsonDocument::Compact),
                         QCryptographicHash::Sha256)
                         .toHex()));
      } else if (kind == "rfbenchEvent") {
        const auto reference =
            origin["rfbenchEvent"].toObject()["snapshotReference"].toObject();
        QCOMPARE(reference["jsonPointer"].toString(), QString("/events/3"));
        QCOMPARE(reference["sourceHash"].toString(),
                 QString::fromStdString(imported.sourceSet.supplementalHash));
      }
      FileState file;
      file.metadata = metadata;
      file.sourceSets = {imported.sourceSet};
      file.marks = imported.annotations;
      const auto exported =
          QJsonDocument::fromJson(exportAnnotationsJson(file)).object();
      const auto source = exported["sourceSets"].toArray()[0].toObject();
      QVERIFY(source["originalText"].toString().toUtf8() == primaryBytes);
      QVERIFY(source["supplementalOriginalText"].toString().toUtf8() ==
              supplementalBytes);
      QString error;
      QJsonObject index;
      const auto project = directory.filePath(kind + ".json");
      QVERIFY2(writeAnnotationStore(project, file, index, error),
               qPrintable(error));
      FileState restored;
      restored.metadata = metadata;
      QVERIFY2(readAnnotationStore(project, index, restored, error),
               qPrintable(error));
      QCOMPARE(restored.marks.size(), std::size_t{4});
      QVERIFY(restored.sourceSets[0].originalText ==
              primaryBytes.toStdString());
      QVERIFY(restored.sourceSets[0].supplementalOriginalText ==
              supplementalBytes.toStdString());
    }
  }

  void normalizedBatchBudgetRejectsAtomically() {
    QTemporaryDir directory;
    const auto metadata = torchTarget(directory);
    const auto base = baseOf(metadata);
    auto primary =
        QJsonDocument::fromJson(fixture("torchsig-0000000.sigmf-meta"))
            .object();
    const QJsonObject record{
        {"core:sample_start", 0},
        {"core:sample_count", 1},
        {"core:freq_lower_edge", 2499999900.0},
        {"core:freq_upper_edge", 2500000100.0},
        {"core:label", "bpsk"},
        {"unknown_optional", QString(4096, QLatin1Char('R'))}};
    QJsonArray records;
    for (int i = 0; i < 4000; ++i) records.append(record);
    primary["annotations"] = records;
    const auto raw = QJsonDocument(primary).toJson(QJsonDocument::Compact);
    QVERIFY(raw.size() < 32 * 1024 * 1024);
    write(base + ".sigmf-meta", raw);
    QVERIFY(QFile::remove(base + ".json"));
    // Exercise the normalized byte budget independently of the interactive
    // deadline, including when this large fixture runs in Debug under load.
    LabelImportOptions options;
    options.timeoutMs = 30000;
    const auto rejected =
        importTorchSigLabels(base + ".sigmf-meta", metadata, 0, false, options);
    QVERIFY(!rejected.ok());
    QVERIFY(rejected.annotations.empty());
    QVERIFY2(rejected.error.contains("累计预算"), qUtf8Printable(rejected.error));
  }

  void largeGeneratorRecordUsesSourceLineReference() {
    QTemporaryDir directory;
    const auto data = directory.filePath("source.raw");
    const auto path = directory.filePath("large.txt");
    sizeFile(data, 4000000);
    const auto metadata = target(data, 1000000, 1000000);
    const auto raw =
        QByteArray(
            "1: time_start:0.00000 time_end:1.00000 sig_len:1000000\n"
            "1): mod:BPSK sys:BPSK FC:500000 BW:100000 FD:1 LEN:1000000 SPS:1 "
            "CNR:1 SNR:1 TIME:0.10000,0.20000 OPT:") +
        QByteArray(128 * 1024, 'T') + "\n";
    write(path, raw);
    const auto imported = importGeneratorLabels(path, metadata);
    QVERIFY2(imported.ok(), qPrintable(imported.error));
    const auto origin =
        QJsonDocument::fromJson(
            QByteArray::fromStdString(
                imported.annotations[0].originEvidence->originalFieldsJson))
            .object();
    QVERIFY(origin.contains("recordReference"));
    QCOMPARE(origin["recordReference"].toObject()["textLineNumber"].toString(),
             QString("2"));
    QVERIFY(!origin.contains("rawLine") && !origin.contains("fields"));
    QVERIFY(imported.sourceSet.originalText == raw.toStdString());
  }

  void actualTwoSourceAssets() {
    QFile assets(root().filePath(
        "docs/implementation/wideband-detection/M0/assets.json"));
    QVERIFY(assets.open(QIODevice::ReadOnly));
    const auto all = QJsonDocument::fromJson(assets.readAll()).object();
    const auto pair = all["datasets"]
                          .toObject()["matlabGenerator"]
                          .toObject()["goldenPair"]
                          .toObject();
    const auto wavPath = pair["wav"].toObject()["path"].toString(),
               txtPath = pair["txtLabel"].toObject()["path"].toString();
    if (!QFileInfo::exists(wavPath))
      QSKIP("External generator M0 golden unavailable");
    WavDescriptor wav;
    QString error;
    QVERIFY2(readWavDescriptor(wavPath, wav, error), qPrintable(error));
    auto m = target(wavPath, wav.sampleRateHz, wav.sampleCount, 0);
    m.sampleFormat = wav.format;
    QCOMPARE(m.sampleFormat.headerBytes, wav.dataOffset);
    QCOMPARE(m.sampleFormat.trailerBytes, wav.fileBytes - wav.dataOffset - wav.dataBytes);
    const auto generator = importGeneratorLabels(txtPath, m);
    QVERIFY2(generator.ok(), qPrintable(generator.error));
    QCOMPARE(generator.annotations.size(), std::size_t{10});
    std::size_t segments = 0;
    for (const auto& a : generator.annotations) segments += a.segments.size();
    QCOMPARE(segments, std::size_t{65});
    FileState wavFile;
    wavFile.metadata = m;
    const auto forward = discoverLabelCandidates(m);
    const auto reverse = discoverDataForLabel(txtPath);
    QCOMPARE(forward.state, LabelDiscoveryState::Found);
    QCOMPARE(reverse.state, LabelDiscoveryState::Found);
    QCOMPARE(forward.candidates.size(), std::size_t{1});
    QCOMPARE(reverse.candidates.size(), std::size_t{1});
    for (const auto& candidate : {forward.candidates[0], reverse.candidates[0]}) {
      QCOMPARE(candidate.profile, LabelProfile::GeneratorText);
      QCOMPARE(candidate.dataPath, QFileInfo(wavPath).absoluteFilePath());
      QCOMPARE(candidate.labelPath, QFileInfo(txtPath).absoluteFilePath());
      const auto prepared = prepareDiscoveredLabels(candidate, wavFile);
      QCOMPARE(prepared.disposition, LabelImportDisposition::Ready);
      QVERIFY(prepared.imported.sourceSet == generator.sourceSet);
      QVERIFY(prepared.imported.sourceSet.sourceIdentity == annotationSourceIdentity(m));
      QCOMPARE(prepared.imported.annotations.size(), std::size_t{10});
      std::size_t discoveredSegments = 0;
      for (std::size_t i = 0; i < prepared.imported.annotations.size(); ++i) {
        const auto& annotation = prepared.imported.annotations[i];
        discoveredSegments += annotation.segments.size();
        QCOMPARE(annotation.id, generator.annotations[i].id);
        QVERIFY(annotation.segments == generator.annotations[i].segments);
      }
      QCOMPARE(discoveredSegments, std::size_t{65});
    }
    qInfo() << "Actual generator annotations" << generator.annotations.size()
            << "segments" << segments << "source SHA256"
            << QString::fromStdString(generator.sourceSet.sourceHash);
    const QString data =
        "W:/wbSig53_rfbench20m/train/baseline/0000000.sigmf-data";
    if (!QFileInfo::exists(data))
      QSKIP("External TorchSig M0 golden unavailable");
    m = target(data, 20000000, 400000, 2500000000);
    m.sampleFormat.componentEncoding = ComponentEncoding::Float32;
    m.sampleFormat.normalizeIntegerAdc = false;
    const auto torch = importTorchSigLabels(data, m);
    QVERIFY2(torch.ok(), qPrintable(torch.error));
    QCOMPARE(torch.annotations.size(), std::size_t{4});
    qInfo() << "Actual TorchSig annotations" << torch.annotations.size()
            << "source SHA256"
            << QString::fromStdString(torch.sourceSet.sourceHash);
  }
};
QTEST_GUILESS_MAIN(LabelImportTests)
#include "label_import_tests.moc"
