#include "infrastructure/spectral_settings_json.h"
#include "infrastructure/project_store.h"
#include "domain/power_display.h"
#include "infrastructure/sample_format_json.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace signalstudio {
namespace {

constexpr qint64 maximumDocumentBytes = 32 * 1024 * 1024;

struct InvalidProject { QString message; };

void require(bool condition, const QString& message) {
    if (!condition) throw InvalidProject{message};
}

QString text(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }

QJsonObject object(const QJsonValue& value, const QString& label) {
    require(value.isObject(), label + QStringLiteral(" 必须是对象"));
    return value.toObject();
}

QJsonArray array(const QJsonObject& object, const char* key, int maximum) {
    const auto value = object.value(QLatin1String(key));
    require(value.isArray(), QString::fromLatin1(key) + QStringLiteral(" 必须是数组"));
    const auto result = value.toArray();
    require(result.size() <= maximum, QString::fromLatin1(key) + QStringLiteral(" 超过数量上限"));
    return result;
}

QString string(const QJsonObject& object, const char* key, int maximum, bool allowEmpty = false) {
    const auto value = object.value(QLatin1String(key));
    require(value.isString(), QString::fromLatin1(key) + QStringLiteral(" 必须是字符串"));
    const auto result = value.toString();
    require(result.size() <= maximum && (allowEmpty || !result.trimmed().isEmpty()),
            QString::fromLatin1(key) + QStringLiteral(" 长度或内容无效"));
    return result;
}

double number(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    require(value.isDouble() && std::isfinite(value.toDouble()),
            QString::fromLatin1(key) + QStringLiteral(" 必须是有限数值"));
    return value.toDouble();
}

bool boolean(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    require(value.isBool(), QString::fromLatin1(key) + QStringLiteral(" 必须是布尔值"));
    return value.toBool();
}

SampleIndex sampleIndex(const QJsonObject& object, const char* key) {
    const auto value = string(object, key, 20);
    require(value == QStringLiteral("0") || value.front() != QLatin1Char('0'),
            QString::fromLatin1(key) + QStringLiteral(" 必须是规范十进制样本索引字符串"));
    for (const auto character : value)
        require(character >= QLatin1Char('0') && character <= QLatin1Char('9'),
                QString::fromLatin1(key) + QStringLiteral(" 必须是十进制样本索引字符串"));
    bool ok = false;
    const auto result = value.toULongLong(&ok);
    require(ok, QString::fromLatin1(key) + QStringLiteral(" 超出 uint64 范围"));
    return static_cast<SampleIndex>(result);
}

int fftSize(const QJsonObject& object, const char* key) {
    const auto value = number(object, key);
    require(value >= 16 && value <= 262144 && std::floor(value) == value,
            QString::fromLatin1(key) + QStringLiteral(" 必须为 16–262144 的整数"));
    const auto result = static_cast<int>(value);
    require((result & (result - 1)) == 0,
            QString::fromLatin1(key) + QStringLiteral(" 必须为 2 的幂"));
    return result;
}

QJsonObject encodeRange(const ViewRange& range) {
    return {{QStringLiteral("time"), QJsonObject{{QStringLiteral("begin"), QString::number(range.time.begin)},
                                               {QStringLiteral("end"), QString::number(range.time.end)}}},
            {QStringLiteral("frequency"), QJsonObject{{QStringLiteral("lowerHz"), range.frequency.lowerHz},
                                                    {QStringLiteral("upperHz"), range.frequency.upperHz}}}};
}

QJsonObject encodeTimeRange(const TimeRange& range) {
    return {{QStringLiteral("begin"), QString::number(range.begin)},
            {QStringLiteral("end"), QString::number(range.end)}};
}

TimeRange decodeTimeRange(const QJsonValue& value, SampleIndex maximum, const QString& label) {
    const auto range = object(value, label);
    const TimeRange result{sampleIndex(range, "begin"), sampleIndex(range, "end")};
    require(result.begin < result.end && result.end <= maximum,
            label + QStringLiteral(" 必须为有效的源样本半开区间"));
    return result;
}

QString filterName(ChannelFilter value) {
    switch (value) {
    case ChannelFilter::FastPreview: return QStringLiteral("fastPreview");
    case ChannelFilter::Standard: return QStringLiteral("standard");
    case ChannelFilter::HighRejection: return QStringLiteral("highRejection");
    }
    return QStringLiteral("standard");
}

QString processingStateName(ChannelProcessingState value) {
    switch (value) {
    case ChannelProcessingState::Ready: return QStringLiteral("ready");
    case ChannelProcessingState::LegacyNeedsReview: return QStringLiteral("legacyNeedsReview");
    case ChannelProcessingState::SourceMissing: return QStringLiteral("sourceMissing");
    case ChannelProcessingState::Invalid: return QStringLiteral("invalid");
    }
    return QStringLiteral("invalid");
}

QString pageName(NarrowbandPage value) {
    switch (value) {
    case NarrowbandPage::Observe: return QStringLiteral("observe");
    case NarrowbandPage::Modulation: return QStringLiteral("modulation");
    case NarrowbandPage::DeepLearning: return QStringLiteral("deepLearning");
    case NarrowbandPage::Demodulation: return QStringLiteral("demodulation");
    }
    return QStringLiteral("observe");
}

QString waveformName(NarrowbandWaveform value) {
    switch (value) {
    case NarrowbandWaveform::IQ: return QStringLiteral("iq");
    case NarrowbandWaveform::Magnitude: return QStringLiteral("magnitude");
    case NarrowbandWaveform::Phase: return QStringLiteral("phase");
    case NarrowbandWaveform::Envelope: return QStringLiteral("envelope");
    }
    return QStringLiteral("iq");
}

ViewRange decodeRange(const QJsonValue& value, const FileMetadata& metadata, bool constrainToEffectiveBand = true) {
    const auto range = object(value, QStringLiteral("range"));
    const auto time = object(range.value(QStringLiteral("time")), QStringLiteral("time"));
    const auto frequency = object(range.value(QStringLiteral("frequency")), QStringLiteral("frequency"));
    const ViewRange result{{sampleIndex(time, "begin"), sampleIndex(time, "end")},
                          {number(frequency, "lowerHz"), number(frequency, "upperHz")}};
    auto bounds = fullRange(metadata);
    if (!constrainToEffectiveBand && metadata.sampleFormat.structure!=SampleStructure::Real) {
        bounds.frequency = {metadata.centerFrequencyHz - metadata.sampleRateHz / 2,
                            metadata.centerFrequencyHz + metadata.sampleRateHz / 2};
    }
    require(result.time.begin < result.time.end && result.time.end <= metadata.sampleCount,
            QStringLiteral("时间范围必须有序且位于文件样本范围内"));
    require(result.frequency.lowerHz < result.frequency.upperHz &&
            result.frequency.lowerHz >= bounds.frequency.lowerHz &&
            result.frequency.upperHz <= bounds.frequency.upperHz,
            QStringLiteral("频率范围必须有序且位于文件频带内"));
    return result;
}

QJsonObject encodeDisplay(const DisplaySettings& display) {
    QString mainMode;
    switch (display.mainMode) {
    case MainMode::TimeFrequency: mainMode = QStringLiteral("timeFrequency"); break;
    case MainMode::Waterfall: mainMode = QStringLiteral("waterfall"); break;
    }
    QString auxiliaryMode;
    switch (display.auxiliaryMode) {
    case AuxiliaryMode::Waveform: auxiliaryMode = QStringLiteral("waveform"); break;
    case AuxiliaryMode::Psd: auxiliaryMode = QStringLiteral("psd"); break;
    }
    QString waveformMode;
    switch (display.waveformMode) {
    case WaveformMode::I: waveformMode = QStringLiteral("i"); break;
    case WaveformMode::Q: waveformMode = QStringLiteral("q"); break;
    case WaveformMode::IqRms: waveformMode = QStringLiteral("iqRms"); break;
    case WaveformMode::Envelope: waveformMode = QStringLiteral("envelope"); break;
    }
    QString palette;
    switch (display.palette) {
    case Palette::Turbo: palette = QStringLiteral("turbo"); break;
    case Palette::Viridis: palette = QStringLiteral("viridis"); break;
    case Palette::Gray: palette = QStringLiteral("gray"); break;
    case Palette::Plasma: palette = QStringLiteral("plasma"); break;
    case Palette::Inferno: palette = QStringLiteral("inferno"); break;
    case Palette::Magma: palette = QStringLiteral("magma"); break;
    case Palette::Cividis: palette = QStringLiteral("cividis"); break;
    case Palette::CoolEditClassic: palette = QStringLiteral("coolEditClassic"); break;
    }
    return {{QStringLiteral("mainMode"), mainMode},
            {QStringLiteral("auxiliaryMode"), auxiliaryMode},
            {QStringLiteral("waveformMode"), waveformMode},
            {QStringLiteral("palette"), palette},
            {QStringLiteral("stftSize"), display.stftSize},
            {QStringLiteral("psdSize"), display.psdSize},
            {QStringLiteral("psdSettings"), encodePsdSettings(display.psd)},
            {QStringLiteral("spectrogramSettings"), encodeSpectralParameters(display.spectrogram.parameters)},
            {QStringLiteral("dynamicRangeDb"), display.dynamicRangeDb},
            {QStringLiteral("referenceLevelDb"), display.referenceLevelDb},
            {QStringLiteral("absoluteFrequency"), display.absoluteFrequency},
            {QStringLiteral("grid"), display.grid},
            {QStringLiteral("colorScale"), display.colorScale},
            {QStringLiteral("psdFromSelection"), display.psdFromSelection},
            {QStringLiteral("auxiliaryMin"), display.auxiliaryMin},
            {QStringLiteral("auxiliaryMax"), display.auxiliaryMax},
            {QStringLiteral("waveformMin"), display.auxiliaryMode==AuxiliaryMode::Waveform?display.auxiliaryMin:display.waveformMin},
            {QStringLiteral("waveformMax"), display.auxiliaryMode==AuxiliaryMode::Waveform?display.auxiliaryMax:display.waveformMax},
            {QStringLiteral("waveformAutoFit"), display.waveformAutoFit},
            {QStringLiteral("psdMin"), display.auxiliaryMode==AuxiliaryMode::Psd?display.auxiliaryMin:display.psdMin},
            {QStringLiteral("psdMax"), display.auxiliaryMode==AuxiliaryMode::Psd?display.auxiliaryMax:display.psdMax}};
}

DisplaySettings decodeDisplay(const QJsonValue& value) {
    const auto data = object(value, QStringLiteral("display"));
    DisplaySettings display;
    const auto main = string(data, "mainMode", 32);
    require(main == QStringLiteral("timeFrequency") || main == QStringLiteral("waterfall"), QStringLiteral("未知主图模式"));
    display.mainMode = main == QStringLiteral("waterfall") ? MainMode::Waterfall : MainMode::TimeFrequency;
    const auto auxiliary = string(data, "auxiliaryMode", 32);
    require(auxiliary == QStringLiteral("waveform") || auxiliary == QStringLiteral("psd"), QStringLiteral("未知辅助图模式"));
    display.auxiliaryMode = auxiliary == QStringLiteral("psd") ? AuxiliaryMode::Psd : AuxiliaryMode::Waveform;
    if (data.contains(QStringLiteral("waveformMode"))) {
        const auto waveform = string(data, "waveformMode", 32);
        require(waveform == QStringLiteral("i") || waveform == QStringLiteral("q") ||
                waveform == QStringLiteral("iqRms") || waveform == QStringLiteral("envelope"), QStringLiteral("未知时域波形模式"));
        display.waveformMode = waveform == QStringLiteral("i") ? WaveformMode::I :
            waveform == QStringLiteral("q") ? WaveformMode::Q : waveform == QStringLiteral("envelope") ? WaveformMode::Envelope : WaveformMode::IqRms;
    }
    if (data.contains(QStringLiteral("waveformAutoFit"))) display.waveformAutoFit = boolean(data, "waveformAutoFit");
    const auto palette = string(data, "palette", 32);
    require(palette == QStringLiteral("turbo") || palette == QStringLiteral("viridis") || palette == QStringLiteral("gray") ||
            palette == QStringLiteral("plasma") || palette == QStringLiteral("inferno") ||
            palette == QStringLiteral("magma") || palette == QStringLiteral("cividis") ||
            palette == QStringLiteral("coolEditClassic"),
            QStringLiteral("未知配色"));
    if (palette == QStringLiteral("gray")) display.palette = Palette::Gray;
    else if (palette == QStringLiteral("viridis")) display.palette = Palette::Viridis;
    else if (palette == QStringLiteral("plasma")) display.palette = Palette::Plasma;
    else if (palette == QStringLiteral("inferno")) display.palette = Palette::Inferno;
    else if (palette == QStringLiteral("magma")) display.palette = Palette::Magma;
    else if (palette == QStringLiteral("cividis")) display.palette = Palette::Cividis;
    else if (palette == QStringLiteral("coolEditClassic")) display.palette = Palette::CoolEditClassic;
    else display.palette = Palette::Turbo;
    display.stftSize = fftSize(data, "stftSize");
    display.psdSize = fftSize(data, "psdSize");
    if(data.contains("psdSettings"))display.psd=decodePsdSettings(data["psdSettings"]);
    if(data.contains("spectrogramSettings"))display.spectrogram.parameters=decodeSpectralParameters(data["spectrogramSettings"]);
    display.dynamicRangeDb = number(data, "dynamicRangeDb");
    require(display.dynamicRangeDb > 0 && display.dynamicRangeDb <= 10000, QStringLiteral("动态范围无效"));
    display.referenceLevelDb = number(data, "referenceLevelDb");
    require(display.referenceLevelDb >= -200 && display.referenceLevelDb <= 100,
            QStringLiteral("参考电平必须位于 [-200,100] dBFS"));
    display.absoluteFrequency = boolean(data, "absoluteFrequency");
    display.grid = boolean(data, "grid");
    display.colorScale = boolean(data, "colorScale");
    display.psdFromSelection = boolean(data, "psdFromSelection");
    if(!data.contains("psdSettings"))display.psd.scope=display.psdFromSelection?PsdScope::SourceMark:PsdScope::Visible;
    display.auxiliaryMin = number(data, "auxiliaryMin");
    display.auxiliaryMax = number(data, "auxiliaryMax");
    require(display.auxiliaryMin < display.auxiliaryMax &&
            std::isfinite(display.auxiliaryMax - display.auxiliaryMin), QStringLiteral("辅助 Y 轴范围无效"));
    display.waveformMin=number(data,"waveformMin");display.waveformMax=number(data,"waveformMax");
    display.psdMin=number(data,"psdMin");display.psdMax=number(data,"psdMax");
    require(display.waveformMin>=-65536&&display.waveformMax<=65536&&
            display.waveformMax-display.waveformMin>=2,QStringLiteral("波形 Y 轴范围必须位于 [-65536,65536] 且跨度至少 2"));
    require(validPsdRange(display.psdMin, display.psdMax), QStringLiteral("PSD Y 轴范围必须有限、有序且跨度不超过 10300 dB"));
    const auto activeMin=display.auxiliaryMode==AuxiliaryMode::Waveform?display.waveformMin:display.psdMin;
    const auto activeMax=display.auxiliaryMode==AuxiliaryMode::Waveform?display.waveformMax:display.psdMax;
    require(display.auxiliaryMin==activeMin&&display.auxiliaryMax==activeMax,
            QStringLiteral("辅助图当前 Y 范围必须与活动模式对应"));
    return display;
}

QJsonObject encodeProject(const Project& project) {
    QJsonArray files;
    for (const auto& file : project.files) {
        const auto& metadata = file.metadata;
        QJsonArray marks;
        for (const auto& mark : file.marks)
            marks.append(QJsonObject{{QStringLiteral("id"), text(mark.id)},
                                     {QStringLiteral("name"), text(mark.name)},
                                     {QStringLiteral("range"), encodeRange(mark.range)}});
        QJsonArray channels;
        for (const auto& channel : file.channels) {
            const auto* sourceMark = findMark(file, channel.sourceMarkId);
            const auto sourceTime = channel.sourceTime.begin < channel.sourceTime.end ? channel.sourceTime :
                (sourceMark ? sourceMark->range.time : TimeRange{});
            const auto visibleTime = channel.visibleSourceTime.begin < channel.visibleSourceTime.end ?
                channel.visibleSourceTime : sourceTime;
            const auto visibleFrequency = channel.visibleBasebandFrequency.lowerHz <
                channel.visibleBasebandFrequency.upperHz ? channel.visibleBasebandFrequency :
                FrequencyRange{-channel.outputSampleRateHz / 2, channel.outputSampleRateHz / 2};
            QJsonObject encoded{{QStringLiteral("id"), text(channel.id)},
                                        {QStringLiteral("name"), text(channel.name)},
                                        {QStringLiteral("sourceMarkId"), text(channel.sourceMarkId)},
                                        {QStringLiteral("centerFrequencyHz"), channel.centerFrequencyHz},
                                        {QStringLiteral("bandwidthHz"), channel.bandwidthHz}};
            encoded.insert(QStringLiteral("outputSampleRateHz"), channel.outputSampleRateHz);
            encoded.insert(QStringLiteral("filter"), filterName(channel.filter));
            encoded.insert(QStringLiteral("processingState"), processingStateName(channel.processingState));
            encoded.insert(QStringLiteral("configVersion"), QString::number(channel.configVersion));
            encoded.insert(QStringLiteral("sourceTime"), encodeTimeRange(sourceTime));
            encoded.insert(QStringLiteral("wholeSource"), channel.wholeSource);
            encoded.insert(QStringLiteral("preserveSourceTime"), channel.preserveSourceTime);
            encoded.insert(QStringLiteral("page"), pageName(channel.page));
            encoded.insert(QStringLiteral("visibleSourceTime"), encodeTimeRange(visibleTime));
            encoded.insert(QStringLiteral("visibleBasebandFrequency"), QJsonObject{
                {QStringLiteral("lowerHz"), visibleFrequency.lowerHz},
                {QStringLiteral("upperHz"), visibleFrequency.upperHz}});
            encoded.insert(QStringLiteral("psdFftSize"), channel.psdFftSize);
            encoded.insert("psdSettings",encodePsdSettings(channel.psd));encoded.insert("spectrogramSettings",encodeSpectralParameters(channel.spectrogram.parameters));
            encoded.insert(QStringLiteral("stftFftSize"), channel.stftFftSize);
            encoded.insert(QStringLiteral("waveform"), waveformName(channel.waveform));
            encoded.insert(QStringLiteral("waveformAxisMinimum"), channel.waveformAxisMinimum);
            encoded.insert(QStringLiteral("waveformAxisMaximum"), channel.waveformAxisMaximum);
            encoded.insert(QStringLiteral("waveformAutoScale"), channel.waveformAutoScale);
            encoded.insert(QStringLiteral("psdAxisMinimum"), channel.psdAxisMinimum);
            encoded.insert(QStringLiteral("psdAxisMaximum"), channel.psdAxisMaximum);
            encoded.insert(QStringLiteral("symbolRate"), channel.symbolRate);
            encoded.insert(QStringLiteral("eyePeriods"), channel.eyePeriods);
            encoded.insert(QStringLiteral("eyeTraces"), channel.eyeTraces);
            encoded.insert(QStringLiteral("eyeComponent"), channel.eyeComponent);
            encoded.insert(QStringLiteral("selectedBit"), channel.selectedBit);
            encoded.insert(QStringLiteral("constellationMinimum"), channel.constellationMinimum);
            encoded.insert(QStringLiteral("constellationMaximum"), channel.constellationMaximum);
            encoded.insert(QStringLiteral("relativeTime"), channel.relativeTime);
            encoded.insert(QStringLiteral("absoluteFrequencyLabels"), channel.absoluteFrequencyLabels);
            channels.append(encoded);
        }
        QJsonArray selected;
        for (const auto& id : file.selectedMarkIds) selected.append(text(id));
        files.append(QJsonObject{
            {QStringLiteral("metadata"), QJsonObject{
                {QStringLiteral("id"), text(metadata.id)}, {QStringLiteral("name"), text(metadata.name)},
                {QStringLiteral("path"), text(metadata.path)},
                {QStringLiteral("sampleRateHz"), metadata.sampleRateHz},
                {QStringLiteral("centerFrequencyHz"), metadata.centerFrequencyHz},
                {QStringLiteral("sampleCount"), QString::number(metadata.sampleCount)},
                {QStringLiteral("availableSamples"), QString::number(availableSamples(metadata))},
                {QStringLiteral("sourceFingerprint"), text(metadata.availability.fingerprint)},
                {QStringLiteral("sampleFormat"),encodeSampleFormat(metadata.sampleFormat)},
            {QStringLiteral("declaredBandwidthHz"), metadata.declaredBandwidthHz},
                {QStringLiteral("effectiveBandwidthHz"), metadata.effectiveBandwidthHz > 0 ?
                    metadata.effectiveBandwidthHz : metadata.sampleRateHz},
                {QStringLiteral("demo"), metadata.demo},
                {QStringLiteral("demoSeed"), metadata.demoSeed}}},
            {QStringLiteral("view"), encodeRange(file.view)},
            {QStringLiteral("display"), encodeDisplay(file.display)},
            {QStringLiteral("marks"), marks}, {QStringLiteral("channels"), channels},
            {QStringLiteral("selectedMarkIds"), selected},
            {QStringLiteral("activeMarkId"), text(file.activeMarkId)}});
    }
    return {{QStringLiteral("schema"), QStringLiteral("signal-studio-native-project")},
            {QStringLiteral("version"), 4}, {QStringLiteral("name"), text(project.name)},
            {QStringLiteral("activeFileId"), text(project.activeFileId)},
            {QStringLiteral("activeChannelId"), text(project.activeChannelId)},
            {QStringLiteral("narrowbandWorkspaceOpen"), project.narrowbandWorkspaceOpen},
            {QStringLiteral("files"), files}};
}

Project decodeProject(const QJsonObject& root) {
    require(string(root, "schema", 100) == QStringLiteral("signal-studio-native-project"),
            QStringLiteral("不支持的工程格式；需要 Signal Studio 原生工程"));
    const auto versionValue = number(root, "version");
    require(versionValue == 1 || versionValue == 2 || versionValue == 3 || versionValue == 4, QStringLiteral("不支持的原生工程版本"));
    const auto version = static_cast<int>(versionValue);
    Project project;
    project.name = utf8(string(root, "name", 80));
    project.activeFileId = utf8(string(root, "activeFileId", 100, true));
    if (version >= 2) {
        project.activeChannelId = utf8(string(root, "activeChannelId", 100, true));
        project.narrowbandWorkspaceOpen = boolean(root, "narrowbandWorkspaceOpen");
    }
    QSet<QString> fileIds;
    for (const auto& value : array(root, "files", 150)) {
        const auto data = object(value, QStringLiteral("file"));
        const auto metadata = object(data.value(QStringLiteral("metadata")), QStringLiteral("metadata"));
        FileState file;
        const auto fileId = string(metadata, "id", 100);
        require(!fileIds.contains(fileId), QStringLiteral("文件 id 重复"));
        fileIds.insert(fileId);
        file.metadata.id = utf8(fileId);
        file.metadata.name = utf8(string(metadata, "name", 160));
        file.metadata.path = utf8(string(metadata, "path", 32768, true));
        file.metadata.sampleRateHz = number(metadata, "sampleRateHz");
        file.metadata.centerFrequencyHz = number(metadata, "centerFrequencyHz");
        file.metadata.sampleCount = sampleIndex(metadata, "sampleCount");
        if(version>=4){
            require(metadata["sampleFormat"].isObject(),"版本 4 缺少样本格式");
            try {file.metadata.sampleFormat=decodeSampleFormat(metadata["sampleFormat"].toObject());}
            catch(const std::exception& e){require(false,QString::fromUtf8(e.what()));}
        }
        file.metadata.declaredBandwidthHz = metadata.contains(QStringLiteral("declaredBandwidthHz")) ?
            number(metadata, "declaredBandwidthHz") : 0.0;
        require(file.metadata.declaredBandwidthHz >= 0, QStringLiteral("声明带宽不能为负数"));
        // Old native projects had no effective-band field and displayed the complete sample rate.
        file.metadata.effectiveBandwidthHz = metadata.contains(QStringLiteral("effectiveBandwidthHz")) ?
            number(metadata, "effectiveBandwidthHz") : file.metadata.sampleRateHz;
        if (file.metadata.effectiveBandwidthHz == 0) file.metadata.effectiveBandwidthHz = file.metadata.sampleRateHz;
        file.metadata.demo = boolean(metadata, "demo");
        const auto seed=number(metadata,"demoSeed");
        require(seed>=1&&seed<=std::numeric_limits<int>::max()&&std::floor(seed)==seed,QStringLiteral("演示 seed 必须为正整数"));
        file.metadata.demoSeed=static_cast<int>(seed);
        const auto bounds = fullRange(file.metadata);
        require(file.metadata.sampleRateHz > 0 && file.metadata.sampleCount > 0 &&
                file.metadata.centerFrequencyHz>=0 && file.metadata.effectiveBandwidthHz > 0 && file.metadata.effectiveBandwidthHz <= file.metadata.sampleRateHz/(file.metadata.sampleFormat.structure==SampleStructure::Real?2:1) &&
                std::isfinite(bounds.frequency.lowerHz) && std::isfinite(bounds.frequency.upperHz) &&
                bounds.frequency.lowerHz < bounds.frequency.upperHz &&
                std::isfinite(bounds.frequency.upperHz - bounds.frequency.lowerHz), QStringLiteral("IQ 文件元数据无效"));
        SampleIndex prefix=file.metadata.sampleCount;
        if(version>=3){prefix=sampleIndex(metadata,"availableSamples");require(prefix<=file.metadata.sampleCount,QStringLiteral("可用前缀超出物理文件"));
            file.metadata.availability.fingerprint=utf8(string(metadata,"sourceFingerprint",200,true));}
        file.display = decodeDisplay(data.value(QStringLiteral("display")));
        if(version>=3){const auto d=data["display"].toObject();require(d.contains("psdSettings")&&d.contains("spectrogramSettings"),QStringLiteral("版本 3/4 缺少独立谱参数"));}
        require(file.display.spectrogram.parameters.method==SpectralMethod::Welch||file.display.spectrogram.parameters.method==SpectralMethod::Multitaper,QStringLiteral("不支持的时频图分析方法"));
        file.view = decodeRange(data.value(QStringLiteral("view")), file.metadata);
        QSet<QString> markIds;
        for (const auto& markValue : array(data, "marks", 2000)) {
            const auto markData = object(markValue, QStringLiteral("mark"));
            const auto id = string(markData, "id", 100);
            require(!markIds.contains(id), QStringLiteral("标记 id 重复"));
            markIds.insert(id);
            file.marks.push_back({utf8(id), utf8(string(markData, "name", 80)),
                                  decodeRange(markData.value(QStringLiteral("range")), file.metadata, false)});
        }
        QSet<QString> channelIds;
        const FrequencyRange sampledBand{file.metadata.centerFrequencyHz - file.metadata.sampleRateHz / 2,
                                         file.metadata.centerFrequencyHz + file.metadata.sampleRateHz / 2};
        for (const auto& channelValue : array(data, "channels", 1000)) {
            const auto channelData = object(channelValue, QStringLiteral("channel"));
            const auto id = string(channelData, "id", 100);
            const auto sourceId = string(channelData, "sourceMarkId", 100);
            require(!channelIds.contains(id) && markIds.contains(sourceId), QStringLiteral("窄带通道 id 或源标记引用无效"));
            channelIds.insert(id);
            Channel channel;
            channel.id = utf8(id);
            channel.name = utf8(string(channelData, "name", 80));
            channel.sourceMarkId = utf8(sourceId);
            channel.centerFrequencyHz = number(channelData, "centerFrequencyHz");
            channel.bandwidthHz = number(channelData, "bandwidthHz");
            channel.processingState = ChannelProcessingState::LegacyNeedsReview;
            if (version >= 2) {
                channel.outputSampleRateHz = number(channelData, "outputSampleRateHz");
                require(channel.outputSampleRateHz > 0, QStringLiteral("窄带输出采样率无效"));
                const auto filter = string(channelData, "filter", 32);
                require(filter == QStringLiteral("fastPreview") || filter == QStringLiteral("standard") ||
                        filter == QStringLiteral("highRejection"), QStringLiteral("未知窄带滤波器"));
                channel.filter = filter == QStringLiteral("fastPreview") ? ChannelFilter::FastPreview :
                    filter == QStringLiteral("highRejection") ? ChannelFilter::HighRejection : ChannelFilter::Standard;
                const auto state = string(channelData, "processingState", 32);
                require(state == QStringLiteral("ready") || state == QStringLiteral("legacyNeedsReview") ||
                        state == QStringLiteral("sourceMissing") || state == QStringLiteral("invalid"),
                        QStringLiteral("未知窄带通道状态"));
                channel.processingState = state == QStringLiteral("ready") ? ChannelProcessingState::Ready :
                    state == QStringLiteral("sourceMissing") ? ChannelProcessingState::SourceMissing :
                    state == QStringLiteral("invalid") ? ChannelProcessingState::Invalid : ChannelProcessingState::LegacyNeedsReview;
                channel.configVersion = sampleIndex(channelData, "configVersion");
                require(channel.configVersion > 0, QStringLiteral("通道配置版本无效"));
                channel.sourceTime = decodeTimeRange(channelData.value(QStringLiteral("sourceTime")), file.metadata.sampleCount,
                                                     QStringLiteral("sourceTime"));
                channel.wholeSource = boolean(channelData, "wholeSource");
                channel.preserveSourceTime = boolean(channelData, "preserveSourceTime");
                const auto page = string(channelData, "page", 32);
                require(page == QStringLiteral("observe") || page == QStringLiteral("modulation") ||
                        page == QStringLiteral("deepLearning") || page == QStringLiteral("demodulation"),
                        QStringLiteral("未知窄带工作页"));
                channel.page = page == QStringLiteral("modulation") ? NarrowbandPage::Modulation :
                    page == QStringLiteral("deepLearning") ? NarrowbandPage::DeepLearning :
                    page == QStringLiteral("demodulation") ? NarrowbandPage::Demodulation : NarrowbandPage::Observe;
                channel.visibleSourceTime = decodeTimeRange(channelData.value(QStringLiteral("visibleSourceTime")),
                    file.metadata.sampleCount, QStringLiteral("visibleSourceTime"));
                require(channel.visibleSourceTime.begin >= channel.sourceTime.begin &&
                        channel.visibleSourceTime.end <= channel.sourceTime.end,
                        QStringLiteral("窄带可见时间必须位于通道提取时段内"));
                const auto visibleFrequency = object(channelData.value(QStringLiteral("visibleBasebandFrequency")),
                                                      QStringLiteral("visibleBasebandFrequency"));
                channel.visibleBasebandFrequency = {number(visibleFrequency, "lowerHz"),
                                                    number(visibleFrequency, "upperHz")};
                require(channel.visibleBasebandFrequency.lowerHz < channel.visibleBasebandFrequency.upperHz &&
                        channel.visibleBasebandFrequency.lowerHz >= -channel.outputSampleRateHz / 2 &&
                        channel.visibleBasebandFrequency.upperHz <= channel.outputSampleRateHz / 2,
                        QStringLiteral("窄带可见基带频率范围无效"));
                channel.psdFftSize = fftSize(channelData, "psdFftSize");
                channel.stftFftSize = fftSize(channelData, "stftFftSize");
                if(version>=3){channel.psd=decodePsdSettings(channelData["psdSettings"]);channel.spectrogram.parameters=decodeSpectralParameters(channelData["spectrogramSettings"]);require(channel.spectrogram.parameters.method==SpectralMethod::Welch||channel.spectrogram.parameters.method==SpectralMethod::Multitaper,QStringLiteral("不支持的窄带时频图方法"));}
                else channel.psd.scope=PsdScope::Visible;
                const auto waveform = string(channelData, "waveform", 32);
                require(waveform == QStringLiteral("iq") || waveform == QStringLiteral("magnitude") ||
                        waveform == QStringLiteral("phase") || waveform == QStringLiteral("envelope"),
                        QStringLiteral("未知窄带波形模式"));
                channel.waveform = waveform == QStringLiteral("magnitude") ? NarrowbandWaveform::Magnitude :
                    waveform == QStringLiteral("phase") ? NarrowbandWaveform::Phase :
                    waveform == QStringLiteral("envelope") ? NarrowbandWaveform::Envelope : NarrowbandWaveform::IQ;
                channel.waveformAxisMinimum = channelData.contains(QStringLiteral("waveformAxisMinimum")) ?
                    number(channelData, "waveformAxisMinimum") : -32768.0;
                channel.waveformAxisMaximum = channelData.contains(QStringLiteral("waveformAxisMaximum")) ?
                    number(channelData, "waveformAxisMaximum") : 32768.0;
                channel.waveformAutoScale = channelData.contains(QStringLiteral("waveformAutoScale")) ?
                    boolean(channelData, "waveformAutoScale") : true;
                channel.psdAxisMinimum = channelData.contains(QStringLiteral("psdAxisMinimum")) ?
                    number(channelData, "psdAxisMinimum") : -120.0;
                channel.psdAxisMaximum = channelData.contains(QStringLiteral("psdAxisMaximum")) ?
                    number(channelData, "psdAxisMaximum") : 0.0;
                require(channel.waveformAxisMinimum < channel.waveformAxisMaximum &&
                        channel.waveformAxisMaximum - channel.waveformAxisMinimum <= 2.0e12 &&
                        validPsdRange(channel.psdAxisMinimum, channel.psdAxisMaximum),
                        QStringLiteral("窄带波形或 PSD 纵轴范围无效"));
                channel.symbolRate = number(channelData, "symbolRate");
                require(channel.symbolRate > 0, QStringLiteral("符号率必须大于 0"));
                const auto eyePeriods = number(channelData, "eyePeriods");
                const auto eyeTraces = number(channelData, "eyeTraces");
                const auto eyeComponent = number(channelData, "eyeComponent");
                const auto selectedBit = number(channelData, "selectedBit");
                require(eyePeriods >= 1 && eyePeriods <= 8 && std::floor(eyePeriods) == eyePeriods &&
                        eyeTraces >= 8 && eyeTraces <= 256 && std::floor(eyeTraces) == eyeTraces &&
                        eyeComponent >= 0 && eyeComponent <= 2 && std::floor(eyeComponent) == eyeComponent &&
                        selectedBit >= -1 && selectedBit <= 4095 && std::floor(selectedBit) == selectedBit,
                        QStringLiteral("星座、眼图或位流视图参数超出范围"));
                channel.eyePeriods = static_cast<int>(eyePeriods);
                channel.eyeTraces = static_cast<int>(eyeTraces);
                channel.eyeComponent = static_cast<int>(eyeComponent);
                channel.selectedBit = static_cast<int>(selectedBit);
                channel.constellationMinimum = number(channelData, "constellationMinimum");
                channel.constellationMaximum = number(channelData, "constellationMaximum");
                require(channel.constellationMinimum < channel.constellationMaximum,
                        QStringLiteral("星座图范围无效"));
                channel.relativeTime = boolean(channelData, "relativeTime");
                channel.absoluteFrequencyLabels = channelData.contains(QStringLiteral("absoluteFrequencyLabels")) ?
                    boolean(channelData, "absoluteFrequencyLabels") : false;
            } else {
                const auto* sourceMark = findMark(file, channel.sourceMarkId);
                if (sourceMark) {
                    channel.sourceTime = sourceMark->range.time;
                    channel.visibleSourceTime = channel.sourceTime;
                    channel.outputSampleRateHz = std::max(4e6, channel.bandwidthHz * 1.3);
                    channel.visibleBasebandFrequency = {-channel.outputSampleRateHz / 2,
                                                         channel.outputSampleRateHz / 2};
                }
            }
            // Center +/- bandwidth/2 can differ from the original edges by an ULP.
            const auto roundingTolerance = std::max({1.0, std::abs(sampledBand.lowerHz),
                std::abs(sampledBand.upperHz)}) * std::numeric_limits<double>::epsilon() * 8;
            require(channel.bandwidthHz > 0 && channel.bandwidthHz <= file.metadata.sampleRateHz + roundingTolerance &&
                    channel.centerFrequencyHz >= sampledBand.lowerHz &&
                    channel.centerFrequencyHz <= sampledBand.upperHz &&
                    channel.centerFrequencyHz - channel.bandwidthHz / 2 >= sampledBand.lowerHz - roundingTolerance &&
                    channel.centerFrequencyHz + channel.bandwidthHz / 2 <= sampledBand.upperHz + roundingTolerance,
                    QStringLiteral("窄带通道频率或带宽无效"));
            file.channels.push_back(std::move(channel));
        }
        QSet<QString> selectedIds;
        for (const auto& idValue : array(data, "selectedMarkIds", 2000)) {
            require(idValue.isString(), QStringLiteral("所选标记 id 必须是字符串"));
            const auto id = idValue.toString();
            require(markIds.contains(id) && !selectedIds.contains(id), QStringLiteral("所选标记引用无效或重复"));
            selectedIds.insert(id);
            file.selectedMarkIds.push_back(utf8(id));
        }
        const auto active = string(data, "activeMarkId", 100, true);
        require(active.isEmpty() ? selectedIds.isEmpty() : selectedIds.contains(active), QStringLiteral("活动标记必须属于选择集合"));
        require(!file.display.psdFromSelection || !active.isEmpty(), QStringLiteral("标记 PSD 统计需要活动标记"));
        file.activeMarkId = utf8(active);
        file.metadata.availability.availableSamples=prefix;file.metadata.availability.status=prefix==file.metadata.sampleCount?LoadStatus::Ready:LoadStatus::Partial;
        if(prefix)file.view=clampRange(file.view,file.metadata,file.display.stftSize,file.display.psdSize);
        project.files.push_back(std::move(file));
    }
    require(project.files.empty() ? project.activeFileId.empty() : fileIds.contains(text(project.activeFileId)),
            QStringLiteral("活动文件引用无效"));
    if (!project.activeChannelId.empty()) {
        require(fileIds.contains(text(project.activeFileId)), QStringLiteral("活动通道没有活动源文件"));
        const auto activeFile = std::find_if(project.files.begin(), project.files.end(), [&](const FileState& file) {
            return file.metadata.id == project.activeFileId;
        });
        require(activeFile != project.files.end() && std::any_of(activeFile->channels.begin(), activeFile->channels.end(),
            [&](const Channel& channel) { return channel.id == project.activeChannelId; }),
            QStringLiteral("活动通道不属于活动源文件"));
    }
    return project;
}

} // namespace

bool ProjectStore::save(const QString& path, const Project& project, QString& error) {
    error.clear();
    QJsonObject data;
    try {
        data = encodeProject(project);
        // Validate before opening the destination. Invalid state cannot replace a valid file.
        decodeProject(data);
    } catch (const InvalidProject& invalid) {
        error = invalid.message;
        return false;
    } catch(const std::exception& invalid) {
        error=QString::fromUtf8(invalid.what());return false;
    }
    const auto document = QJsonDocument(data).toJson(QJsonDocument::Indented);
    if (document.size() > maximumDocumentBytes) {
        error = QStringLiteral("工程文件超过 32 MiB 上限");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.write(document) != document.size()) {
        error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

bool ProjectStore::load(const QString& path, Project& target, QString& error) {
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.size() > maximumDocumentBytes) {
        error = QStringLiteral("工程文件超过 32 MiB 上限");
        return false;
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("工程 JSON 无效：") + parseError.errorString();
        return false;
    }
    try {
        auto parsed = decodeProject(document.object());
        target = std::move(parsed);
    } catch (const InvalidProject& invalid) {
        error = invalid.message;
        return false;
    } catch(const std::exception& invalid) {
        error=QString::fromUtf8(invalid.what());return false;
    }
    return true;
}

} // namespace signalstudio
