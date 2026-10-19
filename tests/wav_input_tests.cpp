#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <bit>

#include "infrastructure/raw_sample_reader.h"
#include "infrastructure/source_metadata.h"
#include "infrastructure/wav_metadata.h"

using namespace signalstudio;
namespace {
void put16(QByteArray& bytes, quint16 value) {
  char raw[2];
  qToLittleEndian(value, raw);
  bytes.append(raw, 2);
}
void put32(QByteArray& bytes, quint32 value) {
  char raw[4];
  qToLittleEndian(value, raw);
  bytes.append(raw, 4);
}
QByteArray format(quint16 encoding = 1, quint16 channels = 2,
                  quint32 fs = 20000000, quint32 rate = 80000000,
                  quint16 align = 4, quint16 bits = 16) {
  QByteArray b;
  put16(b, encoding);
  put16(b, channels);
  put32(b, fs);
  put32(b, rate);
  put16(b, align);
  put16(b, bits);
  return b;
}
QByteArray chunk(const QByteArray& id, const QByteArray& payload) {
  QByteArray b = id;
  put32(b, static_cast<quint32>(payload.size()));
  b += payload;
  if (payload.size() & 1) b += '\0';
  return b;
}
QByteArray riff(const QByteArray& chunks) {
  QByteArray b = "RIFF";
  put32(b, static_cast<quint32>(chunks.size() + 4));
  return b + "WAVE" + chunks;
}
QByteArray samples() {
  QByteArray b;
  for (const auto pair : {std::pair<qint16, qint16>{-32768, 32767},
                          {123, -456},
                          {0, -1},
                          {30000, -20000}}) {
    put16(b, std::bit_cast<quint16>(pair.first));
    put16(b, std::bit_cast<quint16>(pair.second));
  }
  return b;
}
bool write(const QString& path, const QByteArray& bytes) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
std::complex<double> decodePair(const char* raw) {
  return {std::bit_cast<qint16>(qFromLittleEndian<quint16>(raw)) / 32768.0,
          std::bit_cast<qint16>(qFromLittleEndian<quint16>(raw + 2)) / 32768.0};
}
}  // namespace
class WavInputTests : public QObject {
  Q_OBJECT
 private slots:
  void noncanonicalLayoutAndReader() {
    QTemporaryDir dir;
    const auto path = dir.filePath("generator.wav");
    const auto data = samples();
    auto fmt = format();
    put16(fmt, 0);  // PCM WAVEFORMATEX with no extension.
    const auto body = chunk("JUNK", "abc") + chunk("fmt ", fmt) +
                      chunk("LIST", "metadata") + chunk("data", data) +
                      chunk("JUNK", "tail!");
    QVERIFY(write(path, riff(body)));
    WavDescriptor d;
    QString error;
    QVERIFY2(readWavDescriptor(path, d, error), qPrintable(error));
    QVERIFY(d.dataOffset != 44);
    QVERIFY(d.format.trailerBytes > 0);
    QCOMPARE(d.sampleRateHz, quint32{20000000});
    QCOMPARE(d.sampleCount, quint64{4});
    QCOMPARE(d.dataBytes, quint64{16});
    QCOMPARE(d.format.channels, quint32{1});
    QCOMPARE(d.format.structure, SampleStructure::ComplexIQ);
    QCOMPARE(d.format.iqLayout, IQLayout::IQInterleaved);
    QCOMPARE(d.format.componentEncoding, ComponentEncoding::Int16);
    QCOMPARE(d.format.byteOrder, ByteOrder::Little);
    QCOMPARE(d.format.headerBytes, d.dataOffset);
    QCOMPARE(d.format.trailerBytes, d.fileBytes - d.dataOffset - d.dataBytes);
    QCOMPARE(d.metadataBytesRead, quint64{12 + 5 * 8 + 18});
    {
      RawSampleReader reader;
      QVERIFY2(reader.open(path, d.format, error), qPrintable(error));
      QCOMPARE(reader.sampleCount(), d.sampleCount);
      for (quint64 n = 0; n < d.sampleCount; ++n) {
        std::complex<double> value;
        QVERIFY(reader.sampleAt(n, value, &error));
        const auto expected = decodePair(data.constData() + n * 4);
        QCOMPARE(value.real(), expected.real());
        QCOMPARE(value.imag(), expected.imag());
      }
      std::complex<double> extra;
      QVERIFY(!reader.sampleAt(d.sampleCount, extra));
      std::vector<std::complex<float>> decoded;
      QVERIFY(reader.read({0, d.sampleCount}, decoded, {}, &error));
      QCOMPARE(decoded.size(), std::size_t{4});
      QVERIFY(!reader.read({0, d.sampleCount + 1}, decoded, {}, &error));
    }
    const auto source = suggestSourceMetadata(path);
    QVERIFY(source.error.isEmpty());
    QVERIFY(source.formatLocked && source.sampleRateLocked);
    QCOMPARE(*source.sampleCount, d.sampleCount);
    QVERIFY(!source.centerFrequencyHz);
    QVERIFY(source.warnings.contains("中心频率"));
    // fmt and data are located by IDs, including data before fmt.
    QVERIFY(write(path, riff(chunk("data", data) + chunk("fmt ", format()))));
    QVERIFY(readWavDescriptor(path, d, error));
    QCOMPARE(d.dataOffset, quint64{20});
    QCOMPARE(d.sampleCount, quint64{4});
  }
  void metadataAuthorityAndRawUnchanged() {
    QTemporaryDir dir;
    const auto path = dir.filePath("RF32_FS1Msps_FC99MHz.wav");
    QVERIFY(
        write(path, riff(chunk("fmt ", format()) + chunk("data", samples()))));
    const auto wav = suggestSourceMetadata(path);
    QCOMPARE(*wav.sampleRateHz, 20000000.0);
    QVERIFY(!wav.centerFrequencyHz);
    QCOMPARE(wav.format->componentEncoding, ComponentEncoding::Int16);
    QVERIFY(wav.conflicts.contains("Fs"));
    QVERIFY(wav.conflicts.contains("格式"));
    QVERIFY(wav.conflicts.contains("Fc"));
    const auto raw =
        suggestSourceMetadata(dir.filePath("CI16_FS1Msps_FC0Hz.raw"));
    QCOMPARE(*raw.sampleRateHz, 1000000.0);
    QCOMPARE(*raw.centerFrequencyHz, 0.0);
    QCOMPARE(raw.format->headerBytes, quint64{0});
    QVERIFY(!raw.formatLocked && !raw.sampleRateLocked);
    QVERIFY(write(path, "broken"));
    const auto broken = suggestSourceMetadata(path);
    QVERIFY(!broken.error.isEmpty());
    QVERIFY(!broken.format && !broken.sampleRateHz &&
            !broken.centerFrequencyHz);
  }
  void malformedAndUnsupported() {
    QTemporaryDir dir;
    const auto path = dir.filePath("bad.wav");
    const auto goodChunks = chunk("fmt ", format()) + chunk("data", samples());
    std::vector<std::pair<QString, QByteArray>> invalid;
    const auto add = [&](const QString& name, const QByteArray& body) {
      invalid.emplace_back(name, riff(body));
    };
    add("duplicate fmt", goodChunks + chunk("fmt ", format()));
    add("duplicate data", goodChunks + chunk("data", samples()));
    add("missing fmt", chunk("data", samples()));
    add("missing data", chunk("fmt ", format()));
    add("empty data", chunk("fmt ", format()) + chunk("data", {}));
    add("partial frame", chunk("fmt ", format()) + chunk("data", "xyz"));
    add("float", chunk("fmt ", format(3)) + chunk("data", samples()));
    add("extensible", chunk("fmt ", format(0xfffe)) + chunk("data", samples()));
    add("mono", chunk("fmt ", format(1, 1)) + chunk("data", samples()));
    add("24bit", chunk("fmt ", format(1, 2, 20000000, 80000000, 4, 24)) +
                     chunk("data", samples()));
    add("align", chunk("fmt ", format(1, 2, 20000000, 80000000, 8)) +
                     chunk("data", samples()));
    add("byteRate", chunk("fmt ", format(1, 2, 20000000, 80000001)) +
                        chunk("data", samples()));
    add("zero Fs",
        chunk("fmt ", format(1, 2, 0, 0)) + chunk("data", samples()));
    add("rate overflow", chunk("fmt ", format(1, 2, 0xffffffff, 0xfffffffc)) +
                             chunk("data", samples()));
    add("fmt short",
        chunk("fmt ", format().first(15)) + chunk("data", samples()));
    add("fmt 17", chunk("fmt ", format() + QByteArray(1, '\0')) +
                      chunk("data", samples()));
    auto badExtra = format();
    put16(badExtra, 4);
    add("fmt extension", chunk("fmt ", badExtra) + chunk("data", samples()));
    add("chunk header short", goodChunks + "ABC");
    QByteArray huge = "JUNK";
    put32(huge, 0xffffffff);
    add("chunk size overflow", goodChunks + huge);
    add("missing odd pad", goodChunks + QByteArray("JUNK\1\0\0\0x", 9));
    auto truncated = riff(goodChunks);
    truncated.chop(1);
    invalid.emplace_back("physical truncation", truncated);
    invalid.emplace_back("bytes beyond RIFF", riff(goodChunks) + "x");
    invalid.emplace_back("short RIFF", "RIFF");
    auto badRiff = riff(goodChunks);
    qToLittleEndian(quint32{0xffffffff}, badRiff.data() + 4);
    invalid.emplace_back("RIFF oversize", badRiff);
    badRiff = riff(goodChunks);
    badRiff.replace(0, 4, "RF64");
    invalid.emplace_back("RF64", badRiff);
    badRiff.replace(0, 4, "RIFX");
    invalid.emplace_back("RIFX", badRiff);
    badRiff = riff(goodChunks);
    badRiff.replace(8, 4, "NONE");
    invalid.emplace_back("not WAVE", badRiff);
    for (const auto& [name, bytes] : invalid) {
      QVERIFY(write(path, bytes));
      WavDescriptor d;
      d.sampleCount = 999;
      QString error;
      QVERIFY2(!readWavDescriptor(path, d, error), qPrintable(name));
      QVERIFY2(!error.isEmpty(), qPrintable(name));
      QCOMPARE(d.sampleCount, quint64{0});
    }
  }
  void boundedHeaderRead() {
    QTemporaryDir dir;
    const auto path = dir.filePath("sparse.wav");
    constexpr quint32 junkBytes = 128 * 1024 * 1024;
    const auto after = chunk("fmt ", format()) + chunk("data", samples());
    QByteArray before = "RIFF";
    put32(before, 4 + 8 + junkBytes + static_cast<quint32>(after.size()));
    before += "WAVEJUNK";
    put32(before, junkBytes);
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(before), qint64{20});
    QVERIFY(f.seek(20 + junkBytes));
    QCOMPARE(f.write(after), qint64{after.size()});
    f.close();
    WavDescriptor d;
    QString error;
    QVERIFY2(readWavDescriptor(path, d, error), qPrintable(error));
    QCOMPARE(d.dataOffset, quint64{20 + junkBytes + 32});
    QCOMPARE(d.metadataBytesRead, quint64{12 + 3 * 8 + 16});
    QCOMPARE(d.sampleCount, quint64{4});
    QByteArray tooMany = chunk("fmt ", format()) + chunk("data", samples());
    for (int i = 0; i < 65536; ++i) tooMany += chunk("JUNK", {});
    QVERIFY(write(path, riff(tooMany)));
    QVERIFY(!readWavDescriptor(path, d, error));
    QVERIFY(error.contains("预算"));
  }
  void actualGeneratorGolden() {
#ifdef SS_SOURCE_DIR
    const QDir root(QString::fromUtf8(SS_SOURCE_DIR));
#else
    const QDir root(QDir::currentPath());
#endif
    QFile assets(
        root.filePath("docs/implementation/wideband-detection/M0/assets.json"));
    QVERIFY(assets.open(QIODevice::ReadOnly));
    const auto pair = QJsonDocument::fromJson(assets.readAll())
                          .object()["datasets"]
                          .toObject()["matlabGenerator"]
                          .toObject()["goldenPair"]
                          .toObject();
    const auto identity = pair["wav"].toObject();
    const auto path = identity["path"].toString();
    if (!QFileInfo::exists(path))
      QSKIP("M0 external generator golden is unavailable on this machine");
    WavDescriptor d;
    QString error;
    QVERIFY2(readWavDescriptor(path, d, error), qPrintable(error));
    QCOMPARE(d.fileBytes, static_cast<quint64>(identity["bytes"].toInteger()));
    QCOMPARE(d.sampleRateHz, quint32{20000000});
    QCOMPARE(d.sampleCount, quint64{16000000});
    QCOMPARE(d.metadataBytesRead, quint64{44});
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QVERIFY(hash.addData(&file));
    QCOMPARE(QString::fromLatin1(hash.result().toHex()),
             identity["sha256"].toString());
    RawSampleReader reader;
    QVERIFY2(reader.open(path, d.format, error), qPrintable(error));
    QCOMPARE(reader.sampleCount(), d.sampleCount);
    quint64 comparisons = 0;
    for (const auto start :
         {quint64{0}, d.sampleCount / 2, d.sampleCount - 1024}) {
      QVERIFY(file.seek(static_cast<qint64>(d.dataOffset + start * 4)));
      const auto raw = file.read(4096);
      QCOMPARE(raw.size(), qsizetype{4096});
      std::vector<std::complex<float>> values;
      QVERIFY2(reader.read({start, start + 1024}, values, {}, &error),
               qPrintable(error));
      for (quint64 n = 0; n < 1024; ++n) {
        const auto expected = decodePair(raw.constData() + n * 4);
        std::complex<double> actual;
        QVERIFY(reader.sampleAt(start + n, actual, &error));
        QCOMPARE(actual.real(), expected.real());
        QCOMPARE(actual.imag(), expected.imag());
        QCOMPARE(values[n].real(), static_cast<float>(expected.real()));
        QCOMPARE(values[n].imag(), static_cast<float>(expected.imag()));
        ++comparisons;
      }
    }
    qInfo().noquote() << "Generator WAV golden:" << path << "SHA256"
                      << identity["sha256"].toString()
                      << "complex frames byte-compared" << comparisons
                      << "metadata bytes read" << d.metadataBytesRead;
  }
};
QTEST_GUILESS_MAIN(WavInputTests)
#include "wav_input_tests.moc"
