#include "infrastructure/project_store.h"

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

ViewRange decodeRange(const QJsonValue& value, const FileMetadata& metadata) {
    const auto range = object(value, QStringLiteral("range"));
    const auto time = object(range.value(QStringLiteral("time")), QStringLiteral("time"));
    const auto frequency = object(range.value(QStringLiteral("frequency")), QStringLiteral("frequency"));
    const ViewRange result{{sampleIndex(time, "begin"), sampleIndex(time, "end")},
                          {number(frequency, "lowerHz"), number(frequency, "upperHz")}};
    const auto bounds = fullRange(metadata);
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
    QString palette;
    switch (display.palette) {
    case Palette::Turbo: palette = QStringLiteral("turbo"); break;
    case Palette::Viridis: palette = QStringLiteral("viridis"); break;
    case Palette::Gray: palette = QStringLiteral("gray"); break;
    }
    return {{QStringLiteral("mainMode"), mainMode},
            {QStringLiteral("auxiliaryMode"), auxiliaryMode},
            {QStringLiteral("palette"), palette},
            {QStringLiteral("stftSize"), display.stftSize},
            {QStringLiteral("psdSize"), display.psdSize},
            {QStringLiteral("dynamicRangeDb"), display.dynamicRangeDb},
            {QStringLiteral("referenceLevelDb"), display.referenceLevelDb},
            {QStringLiteral("absoluteFrequency"), display.absoluteFrequency},
            {QStringLiteral("grid"), display.grid},
            {QStringLiteral("colorScale"), display.colorScale},
            {QStringLiteral("psdFromSelection"), display.psdFromSelection},
            {QStringLiteral("auxiliaryMin"), display.auxiliaryMin},
            {QStringLiteral("auxiliaryMax"), display.auxiliaryMax}};
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
    const auto palette = string(data, "palette", 32);
    require(palette == QStringLiteral("turbo") || palette == QStringLiteral("viridis") || palette == QStringLiteral("gray"),
            QStringLiteral("未知配色"));
    display.palette = palette == QStringLiteral("gray") ? Palette::Gray :
                      palette == QStringLiteral("viridis") ? Palette::Viridis : Palette::Turbo;
    display.stftSize = fftSize(data, "stftSize");
    display.psdSize = fftSize(data, "psdSize");
    display.dynamicRangeDb = number(data, "dynamicRangeDb");
    require(display.dynamicRangeDb > 0 && display.dynamicRangeDb <= 10000, QStringLiteral("动态范围无效"));
    display.referenceLevelDb = number(data, "referenceLevelDb");
    display.absoluteFrequency = boolean(data, "absoluteFrequency");
    display.grid = boolean(data, "grid");
    display.colorScale = boolean(data, "colorScale");
    display.psdFromSelection = boolean(data, "psdFromSelection");
    display.auxiliaryMin = number(data, "auxiliaryMin");
    display.auxiliaryMax = number(data, "auxiliaryMax");
    require(display.auxiliaryMin < display.auxiliaryMax &&
            std::isfinite(display.auxiliaryMax - display.auxiliaryMin), QStringLiteral("辅助 Y 轴范围无效"));
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
        for (const auto& channel : file.channels)
            channels.append(QJsonObject{{QStringLiteral("id"), text(channel.id)},
                                        {QStringLiteral("name"), text(channel.name)},
                                        {QStringLiteral("sourceMarkId"), text(channel.sourceMarkId)},
                                        {QStringLiteral("centerFrequencyHz"), channel.centerFrequencyHz},
                                        {QStringLiteral("bandwidthHz"), channel.bandwidthHz}});
        QJsonArray selected;
        for (const auto& id : file.selectedMarkIds) selected.append(text(id));
        files.append(QJsonObject{
            {QStringLiteral("metadata"), QJsonObject{
                {QStringLiteral("id"), text(metadata.id)}, {QStringLiteral("name"), text(metadata.name)},
                {QStringLiteral("path"), text(metadata.path)},
                {QStringLiteral("sampleRateHz"), metadata.sampleRateHz},
                {QStringLiteral("centerFrequencyHz"), metadata.centerFrequencyHz},
                {QStringLiteral("sampleCount"), QString::number(metadata.sampleCount)},
                {QStringLiteral("demo"), metadata.demo}}},
            {QStringLiteral("view"), encodeRange(file.view)},
            {QStringLiteral("display"), encodeDisplay(file.display)},
            {QStringLiteral("marks"), marks}, {QStringLiteral("channels"), channels},
            {QStringLiteral("selectedMarkIds"), selected},
            {QStringLiteral("activeMarkId"), text(file.activeMarkId)}});
    }
    return {{QStringLiteral("schema"), QStringLiteral("signal-studio-native-project")},
            {QStringLiteral("version"), 1}, {QStringLiteral("name"), text(project.name)},
            {QStringLiteral("activeFileId"), text(project.activeFileId)},
            {QStringLiteral("files"), files}};
}

Project decodeProject(const QJsonObject& root) {
    require(string(root, "schema", 100) == QStringLiteral("signal-studio-native-project"),
            QStringLiteral("不支持的工程格式；需要 Signal Studio 原生工程"));
    require(number(root, "version") == 1, QStringLiteral("不支持的原生工程版本"));
    Project project;
    project.name = utf8(string(root, "name", 80));
    project.activeFileId = utf8(string(root, "activeFileId", 100, true));
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
        file.metadata.demo = boolean(metadata, "demo");
        const auto bounds = fullRange(file.metadata);
        require(file.metadata.sampleRateHz > 0 && file.metadata.sampleCount > 0 &&
                std::isfinite(bounds.frequency.lowerHz) && std::isfinite(bounds.frequency.upperHz) &&
                bounds.frequency.lowerHz < bounds.frequency.upperHz &&
                std::isfinite(bounds.frequency.upperHz - bounds.frequency.lowerHz), QStringLiteral("IQ 文件元数据无效"));
        file.display = decodeDisplay(data.value(QStringLiteral("display")));
        file.view = decodeRange(data.value(QStringLiteral("view")), file.metadata);
        QSet<QString> markIds;
        for (const auto& markValue : array(data, "marks", 2000)) {
            const auto markData = object(markValue, QStringLiteral("mark"));
            const auto id = string(markData, "id", 100);
            require(!markIds.contains(id), QStringLiteral("标记 id 重复"));
            markIds.insert(id);
            file.marks.push_back({utf8(id), utf8(string(markData, "name", 80)),
                                  decodeRange(markData.value(QStringLiteral("range")), file.metadata)});
        }
        QSet<QString> channelIds;
        for (const auto& channelValue : array(data, "channels", 1000)) {
            const auto channelData = object(channelValue, QStringLiteral("channel"));
            const auto id = string(channelData, "id", 100);
            const auto sourceId = string(channelData, "sourceMarkId", 100);
            require(!channelIds.contains(id) && markIds.contains(sourceId), QStringLiteral("窄带通道 id 或源标记引用无效"));
            channelIds.insert(id);
            Channel channel{utf8(id), utf8(string(channelData, "name", 80)), utf8(sourceId),
                            number(channelData, "centerFrequencyHz"), number(channelData, "bandwidthHz")};
            // Center +/- bandwidth/2 can differ from the original edges by an ULP.
            const auto roundingTolerance = std::max({1.0, std::abs(bounds.frequency.lowerHz),
                std::abs(bounds.frequency.upperHz)}) * std::numeric_limits<double>::epsilon() * 8;
            require(channel.bandwidthHz > 0 && channel.bandwidthHz <= file.metadata.sampleRateHz + roundingTolerance &&
                    channel.centerFrequencyHz >= bounds.frequency.lowerHz &&
                    channel.centerFrequencyHz <= bounds.frequency.upperHz &&
                    channel.centerFrequencyHz - channel.bandwidthHz / 2 >= bounds.frequency.lowerHz - roundingTolerance &&
                    channel.centerFrequencyHz + channel.bandwidthHz / 2 <= bounds.frequency.upperHz + roundingTolerance,
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
        project.files.push_back(std::move(file));
    }
    require(project.files.empty() ? project.activeFileId.empty() : fileIds.contains(text(project.activeFileId)),
            QStringLiteral("活动文件引用无效"));
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
    }
    return true;
}

} // namespace signalstudio
