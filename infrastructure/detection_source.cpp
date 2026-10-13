#include "infrastructure/detection_source.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "application/annotation_service.h"
#include "infrastructure/source_loader.h"
#include "infrastructure/wav_metadata.h"

namespace signalstudio {
namespace {
[[noreturn]] void invalid(const QString& message) {
  throw std::runtime_error(message.toUtf8().toStdString());
}
void require(bool condition, const QString& message) {
  if (!condition) invalid(message);
}
bool cancelled(const DetectionSourceCancel& cancel) {
  return cancel && cancel();
}
TimeRange intersection(TimeRange a, TimeRange b) {
  const auto first = std::max(a.begin, b.begin);
  const auto last = std::min(a.end, b.end);
  return first < last ? TimeRange{first, last} : TimeRange{};
}
std::uint64_t ceilDivide(std::uint64_t value, std::uint64_t divisor) {
  return value / divisor + (value % divisor != 0);
}
void exclude(DetectionSourcePlan& plan, TimeRange range,
             const std::string& reason) {
  if (range.begin < range.end) plan.exclusions.push_back({range, reason});
}
QString canonicalPath(const std::string& path) {
  const QFileInfo file(QString::fromUtf8(path));
  return file.canonicalFilePath().isEmpty() ? file.absoluteFilePath()
                                            : file.canonicalFilePath();
}
void validateConfig(const DetectionSourceConfig& config,
                    const FileMetadata& source) {
  require(!config.profileId.empty() && config.profileId.size() <= 128 &&
              config.profileVersion > 0,
          "检测 profile 身份无效");
  require(config.fftSize >= 32 && config.fftSize <= 262144 &&
              (config.fftSize & (config.fftSize - 1)) == 0,
          "检测 FFT 长度须为 32–262144 的 2 次幂");
  const bool scn = config.profileId == "scn-max-psd-v1";
  require(config.modelFrames == (scn ? 16U : 10U) && config.modelHopFrames == 5,
          "模型上下文须使用已注册的真实时间槽与 5 帧滑窗");
  require(std::isfinite(config.hopSeconds) && config.hopSeconds > 0 &&
              config.hopSeconds <= 1,
          "检测 hop 时间无效");
  if (config.profileId == "ffscn-gui-iq-v1")
    require(config.profileVersion == 1 && config.fftSize == 65536 &&
                config.hopSeconds == .001,
            "ffscn-gui-iq-v1 的 FFT/hop/version 已冻结");
  if (scn)
    require(config.profileVersion == 1 && config.fftSize == 32768 &&
                config.hopSeconds == .001,
            "scn-max-psd-v1 的 FFT/hop/version 已冻结");
  require(config.readBlockBytes >= bytesPerFrame(source.sampleFormat) &&
              config.readBlockBytes <= 32 * 1024 * 1024,
          "检测单次读取预算须包含完整帧且不超过 32 MiB");
  require(config.workingSetBudgetBytes <= 256 * 1024 * 1024,
          "检测 CPU 工作集预算不得超过本期 256 MiB 上限");
}
void validateSource(const FileMetadata& source, ViewRange request,
                    RawSampleReader& reader, QString& error,
                    QString& fingerprint) {
  require(source.sampleFormat.structure == SampleStructure::ComplexIQ,
          "复数 IQ profile 不接受实数 ADC");
  require(!source.id.empty() && source.sampleCount > 0 &&
              std::isfinite(source.sampleRateHz) && source.sampleRateHz > 0 &&
              source.sampleRateHz <= 1e12 &&
              std::isfinite(source.centerFrequencyHz) &&
              source.centerFrequencyHz >= 0 && source.centerFrequencyHz <= 1e12,
          "检测数据源 Fs/Fc/物理长度无效");
  require(source.availability.availableSamples <= source.sampleCount,
          "可用前缀超出物理样本范围");
  const auto path = canonicalPath(source.path);
  fingerprint = iqSourceFingerprint(path);
  if (!source.availability.fingerprint.empty())
    require(fingerprint.toStdString() == source.availability.fingerprint,
            "检测源与已读前缀的物理文件身份不一致");
  if (path.endsWith(".wav", Qt::CaseInsensitive)) {
    WavDescriptor descriptor;
    const bool parsed = readWavDescriptor(path, descriptor, error);
    require(parsed, error);
    require(descriptor.sampleRateHz == source.sampleRateHz &&
                descriptor.format == source.sampleFormat,
            "检测源 WAV 的 Fs 或格式与容器冲突");
  }
  const bool opened = reader.open(path, source.sampleFormat, error);
  require(opened, error);
  require(reader.sampleCount() == source.sampleCount,
          "检测源物理样本数与元数据不一致");
  require(request.time.begin < request.time.end &&
              request.time.end <= source.sampleCount,
          "检测请求时间超出物理文件");
  const auto lower = source.centerFrequencyHz - source.sampleRateHz / 2;
  const auto upper = source.centerFrequencyHz + source.sampleRateHz / 2;
  require(std::isfinite(request.frequency.lowerHz) &&
              std::isfinite(request.frequency.upperHz) &&
              request.frequency.lowerHz < request.frequency.upperHz &&
              request.frequency.lowerHz >= lower &&
              request.frequency.upperHz <= upper,
          "检测频率 ROI 超出物理 Nyquist 频段");
  require(iqSourceFingerprint(path) == fingerprint,
          "检测源在打开过程中发生变化");
}
void validatePlanMetadata(const FileMetadata& source, ViewRange request) {
  std::string formatError;
  const bool validFormat =
      validateSampleFormat(source.sampleFormat, formatError);
  require(validFormat, QString::fromStdString(formatError));
  require(source.sampleFormat.structure == SampleStructure::ComplexIQ &&
              !source.id.empty() && source.sampleCount > 0 &&
              std::isfinite(source.sampleRateHz) && source.sampleRateHz > 0 &&
              source.sampleRateHz <= 1e12 &&
              std::isfinite(source.centerFrequencyHz) &&
              source.centerFrequencyHz >= 0 && source.centerFrequencyHz <= 1e12,
          "检测 source metadata 无效或不属于复数 profile");
  require(source.availability.availableSamples <= source.sampleCount,
          "可用前缀超出物理样本范围");
  const auto fileLimit =
      static_cast<std::uint64_t>(std::numeric_limits<qint64>::max());
  const auto& format = source.sampleFormat;
  require(format.headerBytes <= fileLimit &&
              format.trailerBytes <= fileLimit - format.headerBytes &&
              source.sampleCount <=
                  (fileLimit - format.headerBytes - format.trailerBytes) /
                      bytesPerFrame(format),
          "源样本/格式偏移超出 QFile 有符号字节地址范围");
  require(std::isfinite(source.effectiveBandwidthHz) &&
              source.effectiveBandwidthHz >= 0 &&
              source.effectiveBandwidthHz <= source.sampleRateHz,
          "显示有效带宽无效");
  require(request.time.begin < request.time.end &&
              request.time.end <= source.sampleCount,
          "检测请求时间超出物理文件");
  const double lower = source.centerFrequencyHz - source.sampleRateHz / 2;
  const double upper = source.centerFrequencyHz + source.sampleRateHz / 2;
  require(std::isfinite(request.frequency.lowerHz) &&
              std::isfinite(request.frequency.upperHz) &&
              request.frequency.lowerHz < request.frequency.upperHz &&
              request.frequency.lowerHz >= lower &&
              request.frequency.upperHz <= upper,
          "检测频率 ROI 超出物理 Nyquist 频段");
}
void buildGrid(DetectionSourcePlan& plan) {
  const auto& config = plan.config;
  const auto available = plan.sourceIdentity.availableSamples;
  const double hop =
      std::floor(plan.sourceIdentity.sampleRateHz * config.hopSeconds);
  require(hop >= 1 && hop < 18446744073709551616.0,
          "floor(Fs*hopSeconds) 必须至少为 1 个样本");
  plan.hopSamples = static_cast<std::uint64_t>(hop);
  plan.realFrameCount = available / plan.hopSamples;
  plan.availableTailSamples = available % plan.hopSamples;
  if (plan.hopSamples < config.fftSize) {
    const auto halo = config.fftSize - plan.hopSamples;
    const auto padding =
        halo > plan.availableTailSamples ? halo - plan.availableTailSamples : 0;
    plan.sourceLeadingPadding = padding / 2;
    plan.sourceTrailingPadding = padding - plan.sourceLeadingPadding;
  } else {
    const auto padding =
        (config.fftSize - plan.hopSamples % config.fftSize) % config.fftSize;
    plan.slotLeadingPadding = padding / 2;
    plan.slotTrailingPadding = padding - plan.slotLeadingPadding;
  }
  const auto records = ceilDivide(plan.hopSamples, config.fftSize);
  require(records <= 4096, "单个时间槽的 FFT 记录数量超过 4096 预算");
  const auto chunkSamples = std::min<std::uint64_t>(
      config.fftSize,
      config.readBlockBytes / bytesPerFrame(plan.sourceIdentity.sampleFormat));
  const auto tensorBytes =
      std::uint64_t{config.fftSize} * config.modelFrames * sizeof(float);
  plan.estimatedPeakBytes =
      2 * tensorBytes +
      config.fftSize *
          (sizeof(std::complex<double>) + 2 * sizeof(double) + sizeof(float)) +
      chunkSamples * sizeof(std::complex<float>) +
      chunkSamples * bytesPerFrame(plan.sourceIdentity.sampleFormat) +
      records * config.modelFrames * sizeof(DetectionFftSupport);
  require(plan.estimatedPeakBytes <= config.workingSetBudgetBytes,
          "FFT/模型输入工作集超过配置预算");
  plan.frequencyFirstBinHz = plan.sourceIdentity.centerFrequencyHz -
                             plan.sourceIdentity.sampleRateHz / 2;
  plan.frequencyStepHz = plan.sourceIdentity.sampleRateHz / config.fftSize;
  plan.referenceConfigCompatible =
      config.profileId == "ffscn-gui-iq-v1" && config.profileVersion == 1;
}
void buildWindows(DetectionSourcePlan& plan) {
  const auto length = plan.config.modelFrames;
  const auto step = plan.config.modelHopFrames;
  const auto available = plan.sourceIdentity.availableSamples;
  const auto requested = plan.requestedRange.time;
  exclude(
      plan,
      intersection(requested, {available, plan.sourceIdentity.physicalSamples}),
      "unread-available-prefix");
  if (plan.realFrameCount < length) {
    exclude(plan, intersection(requested, {0, available}),
            "insufficient-real-model-frames");
    return;
  }
  const auto lastStart = plan.realFrameCount - length;
  plan.regularWindowCount = lastStart / step + 1;
  plan.anchoredTailWindow = lastStart % step != 0;
  plan.globalWindowCount = plan.regularWindowCount + plan.anchoredTailWindow;
  const auto gridEnd = plan.realFrameCount * plan.hopSamples;
  exclude(plan, intersection(requested, {gridEnd, available}),
          "incomplete-time-slot-tail");
  const auto eligible = intersection(requested, {0, gridEnd});
  if (eligible.begin == eligible.end) return;
  const auto beginFrame = eligible.begin / plan.hopSamples;
  const auto minimumStart = beginFrame < length ? 0 : beginFrame - length + 1;
  const auto maximumStart = (eligible.end - 1) / plan.hopSamples;
  const auto first = ceilDivide(minimumStart, step);
  const auto last = std::min(lastStart / step, maximumStart / step);
  if (first <= last) {
    plan.firstRegularWindow = first;
    plan.eligibleRegularWindows = last - first + 1;
  }
  plan.eligibleTailWindow = plan.anchoredTailWindow &&
                            lastStart >= minimumStart &&
                            lastStart <= maximumStart;
  plan.eligibleWindowCount =
      plan.eligibleRegularWindows + plan.eligibleTailWindow;
}
bool transform(std::vector<std::complex<double>>& values,
               const DetectionSourceCancel& cancel) {
  const auto count = values.size();
  for (std::size_t i = 1, reverse = 0; i < count; ++i) {
    auto bit = count >> 1;
    while (reverse & bit) {
      reverse ^= bit;
      bit >>= 1;
    }
    reverse ^= bit;
    if (i < reverse) std::swap(values[i], values[reverse]);
  }
  for (std::size_t length = 2; length <= count; length <<= 1) {
    if (cancelled(cancel)) return false;
    const double phase = -2 * std::numbers::pi / static_cast<double>(length);
    const std::complex<double> rotation{std::cos(phase), std::sin(phase)};
    for (std::size_t first = 0; first < count; first += length) {
      std::complex<double> factor{1, 0};
      for (std::size_t offset = 0; offset < length / 2; ++offset) {
        const auto even = values[first + offset];
        const auto odd = factor * values[first + offset + length / 2];
        values[first + offset] = even + odd;
        values[first + offset + length / 2] = even - odd;
        factor *= rotation;
      }
    }
  }
  return true;
}
void hashFloat(QCryptographicHash& hash, float value) {
  char bytes[4];
  qToLittleEndian(std::bit_cast<quint32>(value), bytes);
  hash.addData(QByteArrayView(bytes, 4));
}
void hashSupport(QCryptographicHash& hash, const DetectionFftSupport& support) {
  const auto bytes =
      QJsonDocument(
          QJsonObject{{"begin", QString::number(support.actualSamples.begin)},
                      {"end", QString::number(support.actualSamples.end)},
                      {"left", QString::number(support.leadingPadding)},
                      {"right", QString::number(support.trailingPadding)}})
          .toJson(QJsonDocument::Compact);
  hash.addData(bytes);
}
double inputScale(const SampleFormat& format) {
  if (format.componentEncoding >= ComponentEncoding::Float32 ||
      format.normalizeIntegerAdc)
    return 1;
  auto normalized = format;
  normalized.normalizeIntegerAdc = true;
  return 1 / adcScale(normalized);
}
bool readTransformInput(RawSampleReader& reader,
                        const DetectionFftSupport& support,
                        const DetectionSourcePlan& plan,
                        std::vector<std::complex<double>>& work,
                        QCryptographicHash& hash, QString& error,
                        const DetectionSourceCancel& cancel) {
  std::fill(work.begin(), work.end(), std::complex<double>{});
  hashSupport(hash, support);
  const auto chunkSamples = std::min<std::uint64_t>(
      4194304, plan.config.readBlockBytes /
                   bytesPerFrame(plan.sourceIdentity.sampleFormat));
  const double scale = inputScale(plan.sourceIdentity.sampleFormat);
  for (auto first = support.actualSamples.begin;
       first < support.actualSamples.end;) {
    if (cancelled(cancel)) {
      error = "检测源计算已取消";
      return false;
    }
    const auto end =
        first + std::min(chunkSamples, support.actualSamples.end - first);
    std::vector<std::complex<float>> decoded;
    if (!reader.read({first, end}, decoded, cancel, &error)) {
      if (error.isEmpty()) error = "检测源读取取消或失败";
      return false;
    }
    const auto offset =
        support.leadingPadding + first - support.actualSamples.begin;
    for (std::size_t i = 0; i < decoded.size(); ++i) {
      const float real = static_cast<float>(decoded[i].real() * scale);
      const float imaginary = static_cast<float>(decoded[i].imag() * scale);
      work[static_cast<std::size_t>(offset) + i] = {real, imaginary};
      hashFloat(hash, real);
      hashFloat(hash, imaginary);
    }
    first = end;
  }
  return true;
}
DetectionFrameMapping frameMappingForPlan(const DetectionSourcePlan& plan,
                                          std::uint64_t index) {
  DetectionFrameMapping mapping;
  mapping.frameIndex = index;
  const auto anchor = index * plan.hopSamples;
  mapping.slotSamples = {anchor, anchor + plan.hopSamples};
  const auto addSupport = [&](qint64 virtualBegin, TimeRange bounds) {
    const auto virtualEnd = virtualBegin + plan.config.fftSize;
    const auto first =
        std::max<qint64>(virtualBegin, static_cast<qint64>(bounds.begin));
    const auto last =
        std::min<qint64>(virtualEnd, static_cast<qint64>(bounds.end));
    const DetectionFftSupport support{
        {static_cast<SampleIndex>(first), static_cast<SampleIndex>(last)},
        static_cast<std::uint64_t>(first - virtualBegin),
        static_cast<std::uint64_t>(virtualEnd - last)};
    mapping.fftSupports.push_back(support);
    mapping.validSamples +=
        support.actualSamples.end - support.actualSamples.begin;
    mapping.paddedSamples += support.leadingPadding + support.trailingPadding;
  };
  if (plan.hopSamples < plan.config.fftSize) {
    addSupport(static_cast<qint64>(anchor) -
                   static_cast<qint64>(plan.sourceLeadingPadding),
               {0, plan.sourceIdentity.availableSamples});
  } else {
    const auto records = ceilDivide(plan.hopSamples, plan.config.fftSize);
    for (std::uint64_t record = 0; record < records; ++record)
      addSupport(static_cast<qint64>(anchor + record * plan.config.fftSize) -
                     static_cast<qint64>(plan.slotLeadingPadding),
                 mapping.slotSamples);
  }
  mapping.actualSamples = {mapping.fftSupports.front().actualSamples.begin,
                           mapping.fftSupports.back().actualSamples.end};
  return mapping;
}
std::uint64_t windowStartForPlan(const DetectionSourcePlan& plan,
                                 std::uint64_t eligible) {
  return eligible < plan.eligibleRegularWindows
             ? (plan.firstRegularWindow + eligible) * plan.config.modelHopFrames
             : plan.realFrameCount - plan.config.modelFrames;
}
}  // namespace

bool makeDetectionSourcePlan(const FileMetadata& source, ViewRange requested,
                             DetectionSourceConfig config,
                             DetectionSourcePlan& target, QString& error,
                             std::uint64_t generation) {
  try {
    validatePlanMetadata(source, requested);
    validateConfig(config, source);
    DetectionSourcePlan plan;
    plan.config = std::move(config);
    plan.sourceIdentity = annotationSourceIdentity(source, generation);
    plan.requestedRange = requested;
    buildGrid(plan);
    buildWindows(plan);
    if (plan.eligibleWindowCount) {
      const auto first = windowStartForPlan(plan, 0);
      const auto last = windowStartForPlan(plan, plan.eligibleWindowCount - 1) +
                        plan.config.modelFrames - 1;
      plan.contextRange = {frameMappingForPlan(plan, first).actualSamples.begin,
                           frameMappingForPlan(plan, last).actualSamples.end};
    }
    target = std::move(plan);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = QString::fromUtf8(exception.what());
    if (error.isEmpty()) error = "检测网格规划失败";
    return false;
  }
}

bool FixedDetectionSource::open(const FileMetadata& source, ViewRange requested,
                                DetectionSourceConfig config, QString& error,
                                std::uint64_t generation) {
  reader_.reset();
  plan_ = {};
  try {
    const bool planned = makeDetectionSourcePlan(
        source, requested, std::move(config), plan_, error, generation);
    require(planned, error);
    auto reader = std::make_unique<RawSampleReader>();
    validateSource(source, requested, *reader, error, fingerprint_);
    path_ = canonicalPath(source.path);
    plan_.sourceIdentity.quickFingerprint = fingerprint_.toStdString();
    reader_ = std::move(reader);
    hann_.resize(plan_.config.fftSize);
    hannEnergy_ = 0;
    for (std::size_t i = 0; i < hann_.size(); ++i) {
      hann_[i] = .5 - .5 * std::cos(2 * std::numbers::pi * i / hann_.size());
      hannEnergy_ += hann_[i] * hann_[i];
    }
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    reader_.reset();
    error = QString::fromUtf8(exception.what());
    if (error.isEmpty()) error = "检测源校验失败";
    return false;
  }
}
DetectionFrameMapping FixedDetectionSource::frameMapping(
    std::uint64_t index) const {
  return frameMappingForPlan(plan_, index);
}
std::uint64_t FixedDetectionSource::windowStart(std::uint64_t eligible) const {
  return windowStartForPlan(plan_, eligible);
}
bool FixedDetectionSource::sourceCurrent(QString& error) const {
  if (!reader_ || iqSourceFingerprint(path_) != fingerprint_) {
    error = "检测源未打开或物理文件已变化";
    return false;
  }
  return true;
}
bool FixedDetectionSource::readFrame(std::uint64_t index,
                                     DetectionSpectrumFrame& target,
                                     QString& error,
                                     const DetectionSourceCancel& cancel) {
  if (!sourceCurrent(error)) return false;
  if (index >= plan_.realFrameCount) {
    error = "真实检测帧索引越界";
    return false;
  }
  DetectionSpectrumFrame frame;
  frame.mapping = frameMapping(index);
  frame.frequencyFirstBinHz = plan_.frequencyFirstBinHz;
  frame.frequencyStepHz = plan_.frequencyStepHz;
  frame.units = plan_.sourceIdentity.sampleFormat.componentEncoding <
                        ComponentEncoding::Float32
                    ? "dBFS/Hz"
                    : "dB(unit^2/Hz)";
  std::vector<std::complex<double>> work(plan_.config.fftSize);
  std::vector<double> power(plan_.config.fftSize, 0);
  QCryptographicHash inputHash(QCryptographicHash::Sha256);
  inputHash.addData(QByteArray::fromStdString(
      sampleFormatKey(plan_.sourceIdentity.sampleFormat)));
  for (const auto& support : frame.mapping.fftSupports) {
    if (!readTransformInput(*reader_, support, plan_, work, inputHash, error,
                            cancel))
      return false;
    for (std::size_t i = 0; i < work.size(); ++i) work[i] *= hann_[i];
    if (!transform(work, cancel)) {
      error = "检测 FFT 已取消";
      return false;
    }
    for (std::size_t i = 0; i < work.size(); ++i) {
      const auto bin = (i + work.size() / 2) % work.size();
      power[bin] += std::norm(work[i]);
    }
  }
  frame.densityDb.resize(power.size());
  const double records = frame.mapping.fftSupports.size();
  const double densityScale = plan_.sourceIdentity.sampleRateHz * hannEnergy_;
  for (std::size_t i = 0; i < power.size(); ++i) {
    const float averaged = static_cast<float>(power[i] / records);
    if (!std::isfinite(averaged)) {
      error = "FFT 功率超出 float32 有限范围";
      return false;
    }
    if (averaged < std::numeric_limits<float>::min())
      ++frame.mapping.powerFloorBins;
    const float clampedPower =
        std::max(averaged, std::numeric_limits<float>::min());
    if (clampedPower / static_cast<float>(densityScale) == 0)
      ++frame.mapping.densityUnderflowGuardBins;
    frame.densityDb[i] =
        static_cast<float>(10 * std::log10(clampedPower / densityScale));
  }
  frame.mapping.inputHash = inputHash.result().toHex().toStdString();
  if (!sourceCurrent(error)) return false;
  target = std::move(frame);
  error.clear();
  return true;
}
bool FixedDetectionSource::readModelWindow(
    std::uint64_t eligible, DetectionModelWindow& target, QString& error,
    const DetectionSourceCancel& cancel) {
  if (!sourceCurrent(error)) return false;
  if (eligible >= plan_.eligibleWindowCount) {
    error = "模型窗口无覆盖或上下文不足；不会复制缺失时间帧";
    return false;
  }
  DetectionModelWindow window;
  window.firstFrameIndex = windowStart(eligible);
  window.globalWindowIndex = eligible < plan_.eligibleRegularWindows
                                 ? plan_.firstRegularWindow + eligible
                                 : plan_.regularWindowCount;
  window.reportRange = {
      intersection(plan_.requestedRange.time,
                   {window.firstFrameIndex * plan_.hopSamples,
                    (window.firstFrameIndex + plan_.config.modelFrames) *
                        plan_.hopSamples}),
      plan_.requestedRange.frequency};
  const auto values =
      std::size_t{plan_.config.fftSize} * plan_.config.modelFrames;
  window.densityDb.reserve(values);
  double mean = 0, squareDeviation = 0;
  std::uint64_t count = 0;
  for (std::uint32_t row = 0; row < plan_.config.modelFrames; ++row) {
    DetectionSpectrumFrame frame;
    if (!readFrame(window.firstFrameIndex + row, frame, error, cancel))
      return false;
    for (const float value : frame.densityDb) {
      const double deviation = value - mean;
      mean += deviation / ++count;
      squareDeviation += deviation * (value - mean);
    }
    window.densityDb.insert(window.densityDb.end(), frame.densityDb.begin(),
                            frame.densityDb.end());
    window.densityUnderflowGuardApplied |=
        frame.mapping.densityUnderflowGuardBins != 0;
    window.floorGuardApplied |= frame.mapping.powerFloorBins != 0;
    window.frames.push_back(std::move(frame.mapping));
  }
  window.meanDb = mean;
  window.sampleStdDb = std::sqrt(std::max(0.0, squareDeviation / (count - 1)));
  window.normalizationStdDb =
      std::max(window.sampleStdDb,
               static_cast<double>(std::numeric_limits<float>::epsilon()));
  window.epsilonGuardApplied =
      window.sampleStdDb < std::numeric_limits<float>::epsilon();
  window.normalizedTensor.reserve(values);
  QCryptographicHash tensorHash(QCryptographicHash::Sha256);
  for (const float value : window.densityDb) {
    const float normalized =
        static_cast<float>((value - mean) / window.normalizationStdDb);
    window.normalizedTensor.push_back(normalized);
    hashFloat(tensorHash, normalized);
  }
  window.tensorHash = tensorHash.result().toHex().toStdString();
  window.actualContextRange = {window.frames.front().actualSamples.begin,
                               window.frames.back().actualSamples.end};
  if (cancelled(cancel)) {
    error = "检测模型输入计算已取消";
    return false;
  }
  if (!sourceCurrent(error)) return false;
  target = std::move(window);
  error.clear();
  return true;
}
bool FixedDetectionSource::streamWindows(
    const std::function<bool(const DetectionModelWindow&)>& consumer,
    QString& error, const DetectionSourceCancel& cancel) {
  if (!consumer) {
    error = "检测输入接收方为空";
    return false;
  }
  if (cancelled(cancel)) {
    error = "检测输入传递已取消";
    return false;
  }
  for (std::uint64_t i = 0; i < plan_.eligibleWindowCount; ++i) {
    DetectionModelWindow window;
    if (!readModelWindow(i, window, error, cancel)) return false;
    if (!consumer(window)) {
      error = "检测输入接收方停止";
      return false;
    }
  }
  if (!sourceCurrent(error)) return false;
  error.clear();
  return true;
}
}  // namespace signalstudio
