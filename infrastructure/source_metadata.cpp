#include "infrastructure/source_metadata.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

#include "infrastructure/wav_metadata.h"

namespace signalstudio {
SourceSuggestions readWavMetadata(const QString& path) {
  SourceSuggestions s;
  s.dataPath = QFileInfo(path).absoluteFilePath();
  s.formatLocked = true;
  s.sampleRateLocked = true;
  s.provenance = "WAV RIFF · PCM16 I/Q · Fs/格式/数据边界来自容器";
  s.warnings =
      "WAV 不包含 RF 中心频率；请人工填写并确认 Fc，未知时须明确确认基带参考 0 "
      "Hz。";
  WavDescriptor descriptor;
  if (!readWavDescriptor(path, descriptor, s.error)) return s;
  s.sampleRateHz = descriptor.sampleRateHz;
  s.sampleCount = descriptor.sampleCount;
  s.format = descriptor.format;
  return s;
}
SourceSuggestions suggestSourceMetadata(const QString& path) {
  SourceSuggestions s;
  s.dataPath = QFileInfo(path).absoluteFilePath();
  const auto name = QFileInfo(path).fileName();
  const auto value = [&](const QString& key) -> std::optional<double> {
    const auto m =
        QRegularExpression(
            "(?:^|_)" + key + "([0-9]+(?:\\.[0-9]+)?)([kmg]?(?:sps|hz))",
            QRegularExpression::CaseInsensitiveOption)
            .match(name);
    if (!m.hasMatch()) return {};
    bool ok = false;
    double v = m.captured(1).toDouble(&ok);
    const auto unit = m.captured(2).toLower();
    if (unit.startsWith('k'))
      v *= 1e3;
    else if (unit.startsWith('m'))
      v *= 1e6;
    else if (unit.startsWith('g'))
      v *= 1e9;
    return ok && std::isfinite(v) && v >= 0 ? std::optional<double>{v}
                                            : std::optional<double>{};
  };
  s.sampleRateHz = value("FS");
  s.centerFrequencyHz = value("FC");
  s.bandwidthHz = value("BW");
  const auto m =
      QRegularExpression("(?:^|[_.])(C|R)(I8|U8|I16|I32|F32|F64)(?:[_.]|$)",
                         QRegularExpression::CaseInsensitiveOption)
          .match(name);
  if (m.hasMatch()) {
    SampleFormat f;
    f.structure = m.captured(1).toUpper() == "R" ? SampleStructure::Real
                                                 : SampleStructure::ComplexIQ;
    const QStringList encodings{"I8", "U8", "I16", "I32", "F32", "F64"};
    f.componentEncoding = static_cast<ComponentEncoding>(
        encodings.indexOf(m.captured(2).toUpper()));
    f.iqLayout = f.structure == SampleStructure::Real ? IQLayout::NotApplicable
                                                      : IQLayout::IQInterleaved;
    f.byteOrder = bytesPerComponent(f) == 1 ? ByteOrder::NotApplicable
                                            : ByteOrder::Little;
    f.normalizeIntegerAdc = f.componentEncoding < ComponentEncoding::Float32;
    s.format = f;
  }
  s.provenance = QString("Fs：%1 · Fc：%2 · 格式：%3")
                     .arg(s.sampleRateHz ? "文件名" : "需手动确认",
                          s.centerFrequencyHz ? "文件名" : "需手动确认",
                          s.format ? "文件名建议" : "未识别，需确认");
  if (path.endsWith(".wav", Qt::CaseInsensitive)) {
    auto wav = readWavMetadata(path);
    if (!wav.error.isEmpty()) return wav;
    if (s.sampleRateHz && *s.sampleRateHz != *wav.sampleRateHz)
      wav.conflicts += "Fs 与文件名冲突；采用 WAV 容器且不可覆盖。 ";
    if (s.format) {
      auto named = *s.format;
      named.headerBytes = wav.format->headerBytes;
      named.trailerBytes = wav.format->trailerBytes;
      if (named != *wav.format)
        wav.conflicts += "格式与文件名冲突；采用 WAV PCM16 I/Q 且不可覆盖。 ";
    }
    if (s.centerFrequencyHz)
      wav.conflicts +=
          "文件名中的 Fc 不能证明 WAV 的 RF 中心频率；仍需人工填写。 ";
    wav.bandwidthHz = s.bandwidthHz;
    return wav;
  }
  if (path.endsWith(".sigmf-meta", Qt::CaseInsensitive) ||
      path.endsWith(".sigmf-data", Qt::CaseInsensitive)) {
    const auto meta = readSigmfMetadata(path);
    s.dataPath = meta.dataPath;
    s.error = meta.error;
    s.warnings = meta.warnings;
    if (!meta.error.isEmpty()) {
      s.format.reset();
      s.sampleRateHz.reset();
      s.centerFrequencyHz.reset();
      return s;
    }
    const auto conflict = [&](const QString& key, const auto& before,
                              const auto& after) {
      if (before && after && *before != *after)
        s.conflicts += key + "与文件名冲突；采用SigMF（可手动覆盖）。 ";
    };
    conflict("Fs", s.sampleRateHz, meta.sampleRateHz);
    conflict("Fc", s.centerFrequencyHz, meta.centerFrequencyHz);
    conflict("格式", s.format, meta.format);
    if (meta.sampleRateHz) s.sampleRateHz = meta.sampleRateHz;
    if (meta.centerFrequencyHz) s.centerFrequencyHz = meta.centerFrequencyHz;
    if (meta.format) s.format = meta.format;
    s.provenance = meta.provenance + "优先 · " + s.provenance;
  }
  return s;
}
}  // namespace signalstudio
