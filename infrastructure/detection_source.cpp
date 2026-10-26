#include "infrastructure/detection_source.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "application/annotation_service.h"
#include "domain/detection.h"
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
  require(
      config.inputAdaptationVersion == 1 || config.inputAdaptationVersion == 2,
      "未知的检测输入适配版本");
  require(config.fftSize >= 32 && config.fftSize <= 262144 &&
              (config.inputAdaptationVersion == 2 ||
               (config.fftSize & (config.fftSize - 1)) == 0),
          "检测分析长度须为32–262144；不规则长度由适配层重算FFT");
  const bool scn = config.profileId == "scn-max-psd-v1";
  require(config.modelFrames > 0 && config.modelHopFrames > 0,
          "检测窗口与滑动帧数须为正整数");
  require(std::isfinite(config.hopSeconds) && config.hopSeconds > 0 &&
              config.hopSeconds <= 100,
          "检测 hop 时间无效");
  if (config.inputAdaptationVersion == 1) {
    require(
        config.modelFrames == (scn ? 16U : 10U) && config.modelHopFrames == 5,
        "历史v1模型上下文和滑窗身份发生变化");
    if (config.profileId == "ffscn-gui-iq-v1")
      require(config.profileVersion == 1 && config.fftSize == 65536 &&
                  config.hopSeconds == .001,
              "ffscn-gui-iq-v1 的 FFT/hop/version 已冻结");
    if (scn)
      require(config.profileVersion == 1 && config.fftSize == 32768 &&
                  config.hopSeconds == .001,
              "scn-max-psd-v1 的 FFT/hop/version 已冻结");
  }
  require(config.readBlockBytes >= bytesPerFrame(source.sampleFormat) &&
              config.readBlockBytes <= 32 * 1024 * 1024,
          "检测单次读取预算须包含完整帧且不超过 32 MiB");
  require(config.workingSetBudgetBytes > 0 &&
              config.workingSetBudgetBytes <= 1024ULL * 1024 * 1024,
          "检测CPU工作集预算无效");
  require(config.maximumCandidates > 0 && config.maximumCandidates <= 512,
          "检测候选数预算须为1–512");
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
  const bool opened = reader.open(path, source.sampleFormat, error,RawSampleReader::Access::Sequential);
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
  const bool adapted = config.inputAdaptationVersion == 2;
  plan.transformBins = adapted ? std::bit_ceil(config.fftSize) : config.fftSize;
  plan.realFrameCount = adapted ? ceilDivide(available, plan.hopSamples)
                                : available / plan.hopSamples;
  plan.actualWindowFrames = static_cast<std::uint32_t>(
      adapted ? std::min<std::uint64_t>(config.modelFrames, plan.realFrameCount)
              : config.modelFrames);
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
  const auto records = ceilDivide(
      adapted ? std::min(plan.hopSamples, available) : plan.hopSamples,
      plan.transformBins);
  const auto windowFrames = std::max(1U, plan.actualWindowFrames);
  require(records <= config.workingSetBudgetBytes /
                         (sizeof(DetectionFftSupport) * windowFrames),
          "真实FFT支持记录超过CPU工作集预算");
  const auto chunkSamples = std::min<std::uint64_t>(
      plan.transformBins,
      config.readBlockBytes / bytesPerFrame(plan.sourceIdentity.sampleFormat));
  const auto tensorBytes =
      std::uint64_t{config.fftSize} * windowFrames * sizeof(float);
  plan.estimatedPeakBytes =
      2 * tensorBytes +
      plan.transformBins *
          (sizeof(std::complex<double>) + 2 * sizeof(double) + sizeof(float)) +
      (plan.transformBins == config.fftSize ? 0
                                            : config.fftSize * sizeof(double)) +
      chunkSamples * sizeof(std::complex<float>) +
      chunkSamples * bytesPerFrame(plan.sourceIdentity.sampleFormat) +
      records * windowFrames * sizeof(DetectionFftSupport);
  require(plan.estimatedPeakBytes <= config.workingSetBudgetBytes,
          "FFT/模型输入工作集超过配置预算");
  plan.frequencyFirstBinHz = plan.sourceIdentity.centerFrequencyHz -
                             plan.sourceIdentity.sampleRateHz / 2;
  plan.frequencyStepHz = plan.sourceIdentity.sampleRateHz / config.fftSize;
  plan.referenceConfigCompatible =
      config.profileId == "ffscn-gui-iq-v1" && config.profileVersion == 1 &&
      config.fftSize == 65536 && config.modelFrames == 10 &&
      config.modelHopFrames == 5 && config.hopSeconds == .001;
  if (adapted) {
    plan.gridVersion = "origin0-ceil-hop-real-prefix-v2";
    plan.numericalVersion = "cpu-f64-radix2-power-regrid-ddof1-v2";
  }
}
void buildWindows(DetectionSourcePlan& plan) {
  const auto length = plan.actualWindowFrames;
  const auto step = plan.config.modelHopFrames;
  const auto available = plan.sourceIdentity.availableSamples;
  const auto requested = plan.requestedRange.time;
  exclude(
      plan,
      intersection(requested, {available, plan.sourceIdentity.physicalSamples}),
      "unread-available-prefix");
  if (plan.config.inputAdaptationVersion == 2) {
    const auto eligible = intersection(requested, {0, available});
    if (!length || eligible.begin == eligible.end) return;
    const auto lastPossible = plan.realFrameCount - length;
    plan.windowOriginFrame =
        std::min(eligible.begin / plan.hopSamples, lastPossible);
    const auto endFrame = ceilDivide(eligible.end, plan.hopSamples);
    const auto lastStart = std::max(
        plan.windowOriginFrame,
        std::min(lastPossible, endFrame > length ? endFrame - length : 0));
    plan.tailWindowFirstFrame = lastStart;
    plan.regularWindowCount = (lastStart - plan.windowOriginFrame) / step + 1;
    plan.anchoredTailWindow = (lastStart - plan.windowOriginFrame) % step != 0;
    plan.eligibleRegularWindows = plan.regularWindowCount;
    plan.eligibleTailWindow = plan.anchoredTailWindow;
    plan.globalWindowCount = plan.regularWindowCount + plan.anchoredTailWindow;
    plan.eligibleWindowCount = plan.globalWindowCount;
    return;
  }
  if (plan.realFrameCount < length) {
    exclude(plan, intersection(requested, {0, available}),
            "insufficient-real-model-frames");
    return;
  }
  const auto lastStart = plan.realFrameCount - length;
  plan.tailWindowFirstFrame = lastStart;
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
void hashFloatBlock(QCryptographicHash& hash,std::span<const float> values) {
  static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
  hash.addData(QByteArrayView(reinterpret_cast<const char*>(values.data()),qsizetype(values.size_bytes())));
#else
  std::array<char,16384> bytes{};
  for(std::size_t first=0;first<values.size();) {
    const auto count=std::min<std::size_t>(bytes.size()/4,values.size()-first);
    for(std::size_t i=0;i<count;++i) qToLittleEndian(std::bit_cast<quint32>(values[first+i]),bytes.data()+i*4);
    hash.addData(QByteArrayView(bytes.data(),qsizetype(count*4)));first+=count;
  }
#endif
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
template<class Scalar>
bool readTransformInput(RawSampleReader& reader,
                        const DetectionFftSupport& support,
                        const DetectionSourcePlan& plan,
                        std::vector<std::complex<Scalar>>& work,
                        QCryptographicHash& hash, QString& error,
                        const DetectionSourceCancel& cancel) {
  std::fill(work.begin(), work.end(), std::complex<Scalar>{});
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
      decoded[i]={real,imaginary};
    }
    // std::complex<float> guarantees interleaved array-oriented scalar access.
    hashFloatBlock(hash,{reinterpret_cast<const float*>(decoded.data()),decoded.size()*2});
    first = end;
  }
  return true;
}
DetectionFrameMapping frameMappingForPlan(const DetectionSourcePlan& plan,
                                          std::uint64_t index) {
  DetectionFrameMapping mapping;
  mapping.frameIndex = index;
  const auto anchor = index * plan.hopSamples;
  const bool adapted = plan.config.inputAdaptationVersion == 2;
  mapping.slotSamples = {
      anchor,
      adapted ? anchor + std::min(plan.hopSamples,
                                  plan.sourceIdentity.availableSamples - anchor)
              : anchor + plan.hopSamples};
  const auto addSupport = [&](qint64 virtualBegin, TimeRange bounds) {
    const auto virtualEnd = virtualBegin + plan.transformBins;
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
  if (adapted) {
    const auto slotLength = mapping.slotSamples.end - mapping.slotSamples.begin;
    const auto records = ceilDivide(slotLength, plan.transformBins);
    const auto padding =
        (plan.transformBins - slotLength % plan.transformBins) %
        plan.transformBins;
    if (slotLength < plan.transformBins) {
      addSupport(static_cast<qint64>(anchor) - static_cast<qint64>(padding / 2),
                 {0, plan.sourceIdentity.availableSamples});
    } else {
      for (std::uint64_t record = 0; record < records; ++record)
        addSupport(static_cast<qint64>(anchor + record * plan.transformBins) -
                       static_cast<qint64>(padding / 2),
                   mapping.slotSamples);
    }
  } else if (plan.hopSamples < plan.config.fftSize) {
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
             ? plan.windowOriginFrame + (plan.firstRegularWindow + eligible) *
                                            plan.config.modelHopFrames
             : plan.tailWindowFirstFrame;
}
}  // namespace

QJsonObject encodeDetectionFrameMapping(const DetectionFrameMapping& frame) {
  QJsonArray supports;
  for (const auto& support : frame.fftSupports)
    supports.append(QJsonObject{
        {"startSample", QString::number(support.actualSamples.begin)},
        {"endSample", QString::number(support.actualSamples.end)},
        {"leadingPadding", QString::number(support.leadingPadding)},
        {"trailingPadding", QString::number(support.trailingPadding)}});
  return {{"frameIndex", QString::number(frame.frameIndex)},
          {"startSample", QString::number(frame.actualSamples.begin)},
          {"endSample", QString::number(frame.actualSamples.end)},
          {"validSamples", QString::number(frame.validSamples)},
          {"paddedSamples", QString::number(frame.paddedSamples)},
          {"powerFloorBins", QString::number(frame.powerFloorBins)},
          {"densityUnderflowGuardBins",
           QString::number(frame.densityUnderflowGuardBins)},
          {"fftSupports", supports},
          {"inputHash", QString::fromStdString(frame.inputHash)}};
}

std::uint32_t detectionExecutionBatchSize(const DetectionSourcePlan& plan,
                                          std::uint32_t requestedBatch,
                                          std::uint64_t& reservedBytes) {
  for (auto batch = static_cast<std::uint32_t>(
           std::min<std::uint64_t>(requestedBatch, plan.eligibleWindowCount));
       batch > 0; --batch) {
    const auto payload = std::uint64_t{batch} * plan.actualWindowFrames *
                         plan.config.fftSize * sizeof(float);
    const auto reserve =
        plan.estimatedPeakBytes + 6 * payload + 2 * 33554432ULL + 2 * 65536ULL;
    if (payload <= 32 * 1024 * 1024 &&
        batch * (plan.estimatedWindowEvidenceBytes +
                 plan.outputJsonReserveBytes) <
            8 * 1024 * 1024 &&
        reserve + 65536 <= plan.config.workingSetBudgetBytes) {
      reservedBytes = reserve;
      return batch;
    }
  }
  reservedBytes = 0;
  return 0;
}

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
    if (plan.config.inputAdaptationVersion == 2 && plan.realFrameCount) {
      const auto firstLength =
          std::min(plan.hopSamples, plan.sourceIdentity.availableSamples);
      const auto lastLength = plan.sourceIdentity.availableSamples -
                              (plan.realFrameCount - 1) * plan.hopSamples;
      const auto firstPad =
          (plan.transformBins - firstLength % plan.transformBins) %
          plan.transformBins;
      const auto lastPad =
          (plan.transformBins - lastLength % plan.transformBins) %
          plan.transformBins;
      plan.sourceLeadingPadding = firstPad / 2;
      plan.sourceTrailingPadding = lastPad - lastPad / 2;
    }
    buildWindows(plan);
    if (const auto* profile = findDetectionProfile(plan.config.profileId)) {
      if (profile->backend != DetectionBackendKind::Fixture) {
        DetectionRequest request;
        request.inputAdaptationVersion = plan.config.inputAdaptationVersion;
        request.profile = *profile;
        request.profile.fftSize = plan.config.fftSize;
        plan.modelHeadBytesPerWindow =
            3ULL * (detectionModelInputBins(request) / 4) * sizeof(float);
      }
      // The SCN DTO has at most six objects per raw candidate, including
      // measurements, two fusion sets, refinement and the final candidate.
      // Finite JSON doubles need at most 32 bytes; 1536 bytes covers all keys,
      // values and bounded skip reasons. This is a storage bound, not a model
      // input-shape restriction. Other backends retain one candidate object.
      plan.outputJsonReserveBytes +=
          1024 +
          std::uint64_t{plan.config.maximumCandidates} *
              (profile->backend == DetectionBackendKind::Scn ? 1536 : 192);
    }
    if (plan.eligibleWindowCount) {
      const auto first = windowStartForPlan(plan, 0);
      const auto last = windowStartForPlan(plan, plan.eligibleWindowCount - 1) +
                        plan.actualWindowFrames - 1;
      for (const auto window :
           {std::uint64_t{0}, plan.eligibleWindowCount - 1}) {
        std::uint64_t bytes = 4096;
        const auto start = windowStartForPlan(plan, window);
        for (std::uint32_t row = 0; row < plan.actualWindowFrames; ++row) {
          const auto anchor = (start + row) * plan.hopSamples;
          const auto slotLength =
              plan.config.inputAdaptationVersion == 2
                  ? std::min(plan.hopSamples,
                             plan.sourceIdentity.availableSamples - anchor)
                  : plan.hopSamples;
          const auto supportCount = ceilDivide(slotLength, plan.transformBins);
          require(
              supportCount <
                  (8 * 1024 * 1024 - plan.outputJsonReserveBytes - bytes) / 96,
              "单帧FFT观测证据超过8MiB分片，无法按当前证据格式保存");
          auto frame = frameMappingForPlan(plan, start + row);
          frame.inputHash.assign(64, '0');
          frame.powerFloorBins = frame.densityUnderflowGuardBins =
              plan.config.fftSize;
          bytes += QJsonDocument(encodeDetectionFrameMapping(frame))
                       .toJson(QJsonDocument::Compact)
                       .size() +
                   1;
          require(
              bytes + plan.outputJsonReserveBytes < 8 * 1024 * 1024,
              "单窗口真实观测证据超过当前8MiB分片或证据预算，无法保存该窗口");
        }
        plan.estimatedWindowEvidenceBytes =
            std::max(plan.estimatedWindowEvidenceBytes, bytes);
      }
      require(plan.estimatedWindowEvidenceBytes + plan.modelHeadBytesPerWindow +
                      plan.outputJsonReserveBytes <=
                  plan.config.evidenceBudgetBytes,
              "单窗口观测、模型原始输出与候选证据超过当前保存预算");
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
  gpu_.reset();
  gpuCancel_ = {};
  gpuCapabilities_ = {};
  gpuFallback_.clear();
  gpuFrames_ = gpuWindows_ = cpuFrames_ = cpuWindows_ = 0;
  plan_ = {};
  profiling_ = DetectionProfiler{};
  try {
    const bool planned = makeDetectionSourcePlan(
        source, requested, std::move(config), plan_, error, generation);
    require(planned, error);
    auto reader = std::make_unique<RawSampleReader>();
    validateSource(source, requested, *reader, error, fingerprint_);
    path_ = canonicalPath(source.path);
    plan_.sourceIdentity.quickFingerprint = fingerprint_.toStdString();
    reader_ = std::move(reader);
    hann_.resize(plan_.transformBins);
    hannEnergy_ = 0;
    for (std::size_t i = 0; i < hann_.size(); ++i) {
      hann_[i] = .5 - .5 * std::cos(2 * std::numbers::pi * i / hann_.size());
      hannEnergy_ += hann_[i] * hann_[i];
    }
    if (plan_.config.preferGpu) {
      const QDir deployed(QCoreApplication::applicationDirPath());
      gpu_dsp::Config gpuConfig;
      gpuConfig.moduleLibrary = deployed.filePath("SignalStudioGpuDspCuda.dll").toStdString();
      gpuConfig.cufftLibrary = deployed.filePath("cufft64_11.dll").toStdString();
      const auto jit = deployed.filePath("nvJitLink_120_0.dll");
      if (QFileInfo::exists(jit)) gpuConfig.nvJitLinkLibrary = jit.toStdString();
      gpuConfig.limits.hostBytes = std::min<std::uint64_t>(64ULL<<20,plan_.config.workingSetBudgetBytes/4);
      gpuConfig.cancelled = [this] { return gpuCancel_ && gpuCancel_(); };
      gpu_dsp::Error gpuError;
      gpu_ = gpu_dsp::Session::create(gpuConfig,gpuCapabilities_,gpuError);
      if (!gpu_) gpuFallback_ = gpuError.stage + ": " + gpuError.detail;
    } else gpuFallback_ = "CPU reference explicitly requested";
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
QJsonObject FixedDetectionSource::gpuProfiling() const {
  QJsonObject result{{"preferred",plan_.config.preferGpu},
      {"ready",bool(gpu_)},{"device",QString::fromStdString(gpuCapabilities_.deviceName)},
      {"fallbackReason",QString::fromStdString(gpuFallback_).left(1024)},
      {"gpuFrames",qint64(gpuFrames_)},{"cpuFrames",qint64(cpuFrames_)},
      {"gpuWindows",qint64(gpuWindows_)},{"cpuWindows",qint64(cpuWindows_)},
      {"mklFrames",qint64(mklFrames_)},
      {"mklDetail",QString::fromStdString(mkl_.detail()).left(1024)}};
  if (gpu_) {
    const auto& m = gpu_->metrics();
    result["metrics"] = QJsonObject{{"setupMs",m.setupMs},{"planMs",m.planMs},
        {"h2dMs",m.h2dMs},{"fftMs",m.fftMs},{"kernelsMs",m.kernelsMs},
        {"d2hMs",m.d2hMs},{"hostMs",m.totalHostMs},
        {"h2dBytes",qint64(m.h2dBytes)},{"d2hBytes",qint64(m.d2hBytes)},
        {"fftExecutions",qint64(m.fftExecutions)},
        {"pointwiseKernelLaunches",qint64(m.kernelLaunches)},
        {"sdkAlgorithmCalls",qint64(m.libraryAlgorithmCalls)},
        {"devicePeakBytes",qint64(m.devicePeakBytes)},
        {"workspaceBytes",qint64(m.workspaceBytes)},
        {"pinnedBytes",qint64(m.pinnedBytes)},
        {"timingMeaning",QString::fromStdString(m.timingMeaning)}};
  }
  return result;
}
bool FixedDetectionSource::withGpu(const DetectionSourceCancel& cancel,
    const std::function<bool(gpu_dsp::Session*)>& operation) {
  auto previous = std::move(gpuCancel_);
  gpuCancel_ = cancel;
  const auto restore = qScopeGuard([this,&previous] { gpuCancel_ = std::move(previous); });
  return operation(gpu_.get());
}
bool FixedDetectionSource::readGpuFrame(DetectionSpectrumFrame& frame,
    QString& error,const DetectionSourceCancel& cancel) {
  const auto records = frame.mapping.fftSupports.size();
  const auto elements = std::uint64_t(plan_.transformBins)*records;
  // Tiny transforms cannot amortize upload/launch overhead. Preserve the exact
  // existing CPU path and explicitly record why it was selected.
  if (plan_.transformBins < 2048 || !records || elements > 8ULL*1024*1024 ||
      plan_.estimatedPeakBytes > plan_.config.workingSetBudgetBytes ||
      (elements+plan_.transformBins)*sizeof(std::complex<float>) >
          plan_.config.workingSetBudgetBytes-plan_.estimatedPeakBytes) {
    gpuFallback_ = "STFT size below GPU threshold or packed input exceeds working grant";
    return false;
  }
  gpuCancel_ = cancel;
  const auto clearCancel = qScopeGuard([this] { gpuCancel_ = {}; });
  std::vector<std::complex<float>> packed;
  packed.reserve(static_cast<std::size_t>(elements));
  std::vector<std::complex<float>> work(plan_.transformBins);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArray::fromStdString(sampleFormatKey(plan_.sourceIdentity.sampleFormat)));
  for (const auto& support : frame.mapping.fftSupports) {
    DetectionProfileScope reading(profiling_,"iq-read-and-input-hash");
    if (!readTransformInput(*reader_,support,plan_,work,hash,error,cancel)) {
      gpuFallback_ = "GPU input read failed: " + error.toStdString();
      return false;
    }
    packed.insert(packed.end(),work.begin(),work.end());
  }
  gpu_dsp::StftInput input;
  input.records = packed; input.hann = hann_;
  input.transformBins = plan_.transformBins;
  input.recordCount = static_cast<std::uint32_t>(records);
  input.outputBins = plan_.config.fftSize;
  input.sampleRateHz = plan_.sourceIdentity.sampleRateHz;
  input.hannEnergy = hannEnergy_; input.recordDivisor = double(records);
  std::vector<float> density(plan_.config.fftSize);
  gpu_dsp::StftSummary summary;
  gpu_dsp::Error failure;
  DetectionProfileScope gpuProfile(profiling_,"gpu-stft");
  if (!gpu_->stft(input,density,summary,failure)) {
    gpuFallback_ = failure.stage + ": " + failure.detail;
    return false;
  }
  frame.densityDb = std::move(density);
  frame.mapping.powerFloorBins = summary.powerFloorBins;
  frame.mapping.densityUnderflowGuardBins = summary.densityUnderflowGuardBins;
  frame.mapping.inputHash = hash.result().toHex().toStdString();
  error.clear(); return true;
}
bool FixedDetectionSource::readFrame(std::uint64_t index,
                                     DetectionSpectrumFrame& target,
                                     QString& error,
                                     const DetectionSourceCancel& cancel) {
  DetectionProfileScope frameProfile(profiling_,"frame-total");
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
  if (!suppressGpuFrame_ && gpu_ && readGpuFrame(frame,error,cancel)) {
    if (!sourceCurrent(error)) return false;
    ++gpuFrames_;
    target = std::move(frame);
    return true;
  }
  if (cancelled(cancel)) { error = "检测源计算已取消"; return false; }
  // GPU output is unpublished on failure. Re-read the same physical support
  // through the existing CPU reference, preserving hash and mapping semantics.
  frame.mapping = frameMapping(index);
  ++cpuFrames_;
  std::vector<std::complex<double>> work(plan_.transformBins);
  std::vector<double> power(plan_.transformBins, 0);
  QCryptographicHash inputHash(QCryptographicHash::Sha256);
  inputHash.addData(QByteArray::fromStdString(
      sampleFormatKey(plan_.sourceIdentity.sampleFormat)));
  for (const auto& support : frame.mapping.fftSupports) {
    {
      DetectionProfileScope readProfile(profiling_,"iq-read-and-input-hash");
      if (!readTransformInput(*reader_, support, plan_, work, inputHash, error,cancel))
        return false;
    }
    DetectionProfileScope fftProfile(profiling_,"window-fft-power");
    for (std::size_t i = 0; i < work.size(); ++i) work[i] *= hann_[i];
    if (hostMklFrame_) {
      std::string why;
      if (!mkl_.forward(work.data(), static_cast<std::uint32_t>(work.size()), why)) {
        error = QString::fromStdString(why);
        return false;
      }
      ++mklFrames_;
    } else if (!transform(work, cancel)) {
      error = "检测 FFT 已取消";
      return false;
    }
    for (std::size_t i = 0; i < work.size(); ++i) {
      const auto bin = (i + work.size() / 2) % work.size();
      power[bin] += std::norm(work[i]);
    }
  }
  if (power.size() != plan_.config.fftSize) {
    std::vector<double> resampled(plan_.config.fftSize, 0);
    const double scale = double(power.size()) / resampled.size();
    for (std::size_t bin = 0; bin < resampled.size(); ++bin) {
      const double left = bin * scale, right = (bin + 1) * scale;
      for (auto source = static_cast<std::size_t>(left);
           source <
           std::min(power.size(), static_cast<std::size_t>(std::ceil(right)));
           ++source)
        resampled[bin] += power[source] *
                          std::max(0.0, std::min(right, double(source + 1)) -
                                            std::max(left, double(source))) /
                          scale;
    }
    power = std::move(resampled);
  }
  frame.densityDb.resize(power.size());
  QElapsedTimer densityTimer;densityTimer.start();
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
  profiling_.record("density-log",densityTimer.nsecsElapsed());
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
      intersection(
          plan_.requestedRange.time,
          {window.firstFrameIndex * plan_.hopSamples,
           std::min(plan_.sourceIdentity.availableSamples,
                    (window.firstFrameIndex + plan_.actualWindowFrames) *
                        plan_.hopSamples)}),
      plan_.requestedRange.frequency};
  const auto values =
      std::size_t{plan_.config.fftSize} * plan_.actualWindowFrames;
  window.densityDb.reserve(values);
  std::string mklError;
  const bool hostCandidate = plan_.config.useHostMkl && plan_.transformBins >= 2048 &&
                             std::has_single_bit(plan_.transformBins);
  const bool hostMkl = hostCandidate && mkl_.prepare(plan_.transformBins, mklError);
  if (hostCandidate && !hostMkl) gpuFallback_ = mklError;
  const bool previousSuppress = suppressGpuFrame_;
  const bool previousHost = hostMklFrame_;
  // A missing oneMKL DLL stays on the handwritten transform. It does not
  // upload the window frame by frame to recover a GPU STFT.
  suppressGpuFrame_ = hostCandidate;
  hostMklFrame_ = hostMkl;
  const auto restoreHost = qScopeGuard([&] {
    suppressGpuFrame_ = previousSuppress;
    hostMklFrame_ = previousHost;
  });
  double mean = 0, squareDeviation = 0;
  std::uint64_t count = 0;
  for (std::uint32_t row = 0; row < plan_.actualWindowFrames; ++row) {
    DetectionSpectrumFrame frame;
    if (!readFrame(window.firstFrameIndex + row, frame, error, cancel))
      return false;
    DetectionProfileScope normalizeProfile(profiling_,"window-statistics");
    window.densityDb.insert(window.densityDb.end(), frame.densityDb.begin(),
                            frame.densityDb.end());
    window.densityUnderflowGuardApplied |=
        frame.mapping.densityUnderflowGuardBins != 0;
    window.floorGuardApplied |= frame.mapping.powerFloorBins != 0;
    window.frames.push_back(std::move(frame.mapping));
  }
  bool gpuNormalized = false;
  if (gpu_ && values >= 65536) {
    gpuCancel_ = cancel;
    const auto clearCancel = qScopeGuard([this] { gpuCancel_ = {}; });
    gpu_dsp::NormalizationSummary summary;
    gpu_dsp::Error failure;
    std::vector<float> normalized(values);
    DetectionProfileScope gpuProfile(profiling_,"gpu-normalize");
    if (gpu_->normalize(window.densityDb,normalized,summary,failure)) {
      window.normalizedTensor = std::move(normalized);
      window.meanDb = summary.mean;
      window.sampleStdDb = summary.sampleStd;
      window.normalizationStdDb = summary.normalizationStd;
      window.epsilonGuardApplied = summary.epsilonGuardApplied;
      gpuNormalized = true;
      ++gpuWindows_;
    } else gpuFallback_ = failure.stage + ": " + failure.detail;
  }
  if (!gpuNormalized) {
    if (cancelled(cancel)) { error = "检测模型输入计算已取消"; return false; }
    ++cpuWindows_;
    for (const float value : window.densityDb) {
      const double deviation = value - mean;
      mean += deviation / ++count;
      squareDeviation += deviation * (value - mean);
    }
    window.meanDb = mean;
    window.sampleStdDb = std::sqrt(std::max(0.0, squareDeviation / (count - 1)));
    window.normalizationStdDb =
        std::max(window.sampleStdDb,
                 static_cast<double>(std::numeric_limits<float>::epsilon()));
    window.epsilonGuardApplied =
        window.sampleStdDb < std::numeric_limits<float>::epsilon();
    window.normalizedTensor.reserve(values);
    DetectionProfileScope normalizeProfile(profiling_,"normalize-and-tensor-hash");
    for (const float value : window.densityDb) {
      const float normalized =
          static_cast<float>((value - mean) / window.normalizationStdDb);
      window.normalizedTensor.push_back(normalized);
    }
  }
  QCryptographicHash tensorHash(QCryptographicHash::Sha256);
  hashFloatBlock(tensorHash,window.normalizedTensor);
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
