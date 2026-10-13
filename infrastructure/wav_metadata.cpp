#include "infrastructure/wav_metadata.h"

#include <QFile>
#include <QtEndian>
#include <limits>

namespace signalstudio {
bool readWavDescriptor(const QString& path, WavDescriptor& target,
                       QString& error) {
  target = {};
  QFile file(path);
  const auto fail = [&](const QString& reason) {
    error = "WAV：" + reason;
    return false;
  };
  if (!file.open(QIODevice::ReadOnly))
    return fail("文件不可读取：" + file.errorString());
  if (file.size() < 12) return fail("RIFF 文件头截断");
  WavDescriptor result;
  result.fileBytes = static_cast<std::uint64_t>(file.size());
  // A hostile file with millions of empty chunks must not monopolize import.
  constexpr std::uint64_t headerReadBudget = 1024 * 1024;
  constexpr std::uint64_t chunkBudget = 65536;
  const auto readHeader = [&](qint64 bytes) {
    if (result.metadataBytesRead + static_cast<std::uint64_t>(bytes) >
        headerReadBudget)
      return QByteArray{};
    const auto value = file.read(bytes);
    result.metadataBytesRead += static_cast<std::uint64_t>(value.size());
    return value;
  };
  const auto riff = readHeader(12);
  if (riff.size() != 12) return fail("RIFF 文件头截断");
  if (riff.first(4) == "RF64") return fail("暂不支持 RF64");
  if (riff.first(4) == "RIFX") return fail("仅支持小端 RIFF WAVE");
  if (riff.first(4) != "RIFF" || riff.mid(8, 4) != "WAVE")
    return fail("需要 RIFF WAVE 容器");
  const auto riffSize = qFromLittleEndian<quint32>(riff.constData() + 4);
  const std::uint64_t riffEnd = std::uint64_t{riffSize} + 8;
  if (riffSize < 4) return fail("RIFF 长度无效");
  if (riffEnd > result.fileBytes)
    return fail("RIFF 声明长度超出物理文件，文件截断");
  if (riffEnd != result.fileBytes) return fail("RIFF 声明长度与物理文件不一致");

  bool foundFormat = false, foundData = false;
  std::uint64_t position = 12, chunks = 0;
  while (position < riffEnd) {
    if (++chunks > chunkBudget) return fail("RIFF chunk 数量超过解析预算");
    if (riffEnd - position < 8) return fail("chunk 文件头截断");
    if (!file.seek(static_cast<qint64>(position)))
      return fail("无法定位 chunk 文件头");
    const auto header = readHeader(8);
    if (header.size() != 8) return fail("chunk 文件头截断或超过解析预算");
    const auto id = header.first(4);
    const std::uint64_t size =
        qFromLittleEndian<quint32>(header.constData() + 4);
    const std::uint64_t begin = position + 8;
    if (size > riffEnd - begin)
      return fail("chunk 声明长度超出 RIFF，文件截断");
    const std::uint64_t end = begin + size;
    if ((size & 1) && end == riffEnd)
      return fail("奇数长度 chunk 缺少补齐字节");
    const std::uint64_t next = end + (size & 1);
    if (id == "fmt ") {
      if (foundFormat) return fail("重复 fmt chunk");
      foundFormat = true;
      if (size < 16 || size == 17) return fail("fmt chunk 截断");
      const auto fmt = readHeader(16);
      if (fmt.size() != 16) return fail("fmt 内容截断");
      const auto encoding = qFromLittleEndian<quint16>(fmt.constData());
      const auto channels = qFromLittleEndian<quint16>(fmt.constData() + 2);
      const auto fs = qFromLittleEndian<quint32>(fmt.constData() + 4);
      const auto byteRate = qFromLittleEndian<quint32>(fmt.constData() + 8);
      const auto align = qFromLittleEndian<quint16>(fmt.constData() + 12);
      const auto bits = qFromLittleEndian<quint16>(fmt.constData() + 14);
      if (encoding != 1)
        return fail(
            "仅支持 format=1 的 PCM，不支持浮点、压缩或 extensible 编码");
      if (channels != 2 || bits != 16)
        return fail("仅支持双声道 PCM16（I 在前、Q 在后）");
      if (!fs) return fail("采样率必须大于 0");
      const std::uint64_t expectedByteRate = std::uint64_t{fs} * 4;
      if (expectedByteRate > std::numeric_limits<quint32>::max())
        return fail("采样率对应的 byteRate 溢出");
      if (align != 4 || byteRate != expectedByteRate)
        return fail("blockAlign 或 byteRate 与 PCM16 I/Q 帧不一致");
      if (size >= 18) {
        const auto extra = readHeader(2);
        if (extra.size() != 2) return fail("fmt 扩展长度截断");
        if (qFromLittleEndian<quint16>(extra.constData()) != size - 18)
          return fail("fmt 扩展长度与 chunk 不一致");
      }
      result.sampleRateHz = fs;
    } else if (id == "data") {
      if (foundData) return fail("重复 data chunk");
      foundData = true;
      if (!size || size % 4)
        return fail("data 必须包含完整的非空 PCM16 I/Q 帧");
      result.dataOffset = begin;
      result.dataBytes = size;
      result.sampleCount = size / 4;
    }
    // JUNK, LIST and other chunks are deliberately not loaded into memory.
    position = next;
  }
  if (!foundFormat || !foundData) return fail("缺少 fmt 或 data chunk");
  if (static_cast<std::uint64_t>(file.size()) != result.fileBytes)
    return fail("解析期间物理文件长度变化");
  result.format.headerBytes = result.dataOffset;
  result.format.trailerBytes =
      result.fileBytes - result.dataOffset - result.dataBytes;
  target = result;
  error.clear();
  return true;
}
}  // namespace signalstudio
