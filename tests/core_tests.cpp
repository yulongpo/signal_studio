#include "application/session.h"
#include "infrastructure/project_store.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace signalstudio;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(double left, double right, double tolerance = 1e-7) {
    return std::abs(left - right) <= tolerance;
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Could not read test file");
    return file.readAll();
}

void writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "Could not write test file");
    check(file.write(data) == data.size(), "Could not write complete test data");
}

void testClamping() {
    const FileMetadata metadata{"file", "bounds.iq", "", 40e6, 100e6, 10'000, true};
    const auto bounds = fullRange(metadata);
    check(bounds.time == TimeRange{0, 10'000}, "Full time range must use sample indices");
    check(bounds.frequency == FrequencyRange{80e6, 120e6}, "Full frequency range must use absolute Hz");
    auto clamped = clampRange({{9'000, 11'500}, {115e6, 125e6}}, metadata, 2048);
    check(clamped.time == TimeRange{7'500, 10'000}, "Panning at upper time boundary must preserve span");
    check(clamped.frequency == FrequencyRange{110e6, 120e6}, "Panning at frequency boundary must preserve span");
    clamped = clampRange({{5'000, 5'000}, {100e6, 100e6}}, metadata, 2048);
    check(clamped.time.end - clamped.time.begin == 2048, "Minimum time span must equal 2*hop samples");
    check(close(clamped.frequency.upperHz - clamped.frequency.lowerHz, 19'531.25), "Minimum frequency span must equal Fs/FFT");
    clamped = clampRange({{8'000, 2'000}, {105e6, 95e6}}, metadata, 2048);
    check(clamped.time == TimeRange{2'000, 8'000}, "Reversed time must be ordered");
    check(clamped.frequency == FrequencyRange{95e6, 105e6}, "Reversed frequency must be ordered");
    clamped = clampRange({{0, 1}, {0, 1e300}}, metadata, 16384);
    check(clamped.time == bounds.time && clamped.frequency == bounds.frequency,
          "Minimum resolution larger than file must resolve to file bounds");
    clamped = clampRange({{0, 100}, {std::numeric_limits<double>::quiet_NaN(), 100e6}}, metadata, 2048);
    check(clamped.frequency == bounds.frequency, "Nonfinite UI frequency input must recover full band");

    auto huge = metadata;
    huge.sampleCount = std::numeric_limits<SampleIndex>::max();
    clamped = clampRange({{huge.sampleCount - 3, huge.sampleCount}, bounds.frequency}, huge, 2048);
    check(clamped.time == TimeRange{huge.sampleCount - 2048, huge.sampleCount},
          "uint64 upper-bound clamping must not overflow or lose sample precision");
}

void testSessionHistory() {
    Session session;
    check(session.project().files.size() == 2, "Startup must provide two deterministic demo files");
    const auto firstId = session.project().activeFileId;
    const auto secondId = session.project().files[1].metadata.id;
    const auto firstOriginal = session.activeFile()->view;
    auto firstChanged = firstOriginal;
    firstChanged.time.begin += 4000;
    firstChanged.time.end += 4000;
    check(session.setView(firstChanged), "Changed first view must be recorded");
    check(session.canBack() && !session.canForward(), "Recorded view must enable back only");
    check(session.activateFile(secondId) && !session.canBack(), "Files must have independent history");
    const auto secondOriginal = session.activeFile()->view;
    auto secondChanged = secondOriginal;
    secondChanged.frequency.lowerHz += 1000;
    secondChanged.frequency.upperHz -= 1000;
    check(session.setView(secondChanged) && session.canBack(), "Second file must record its own view");
    check(session.back() && session.activeFile()->view == secondOriginal, "Second back must restore second view");
    check(session.activateFile(firstId) && session.activeFile()->view == firstChanged, "First file view must survive switching");
    check(session.back() && session.activeFile()->view == firstOriginal, "First back must restore first view");
    check(session.forward() && session.activeFile()->view == firstChanged, "Forward must restore future view");
    check(session.back(), "Back before history branching must work");
    auto branched = firstOriginal;
    branched.time.end -= 4000;
    check(session.setView(branched) && !session.canForward(), "New navigation must clear forward branch");
    session.replaceProject(session.project());
    check(!session.canBack() && !session.canForward(), "Replacing project must discard all histories");
    for (int index = 0; index < 55; ++index) {
        auto view = session.activeFile()->view;
        view.time.begin += 3000;
        view.time.end += 3000;
        check(session.setView(view), "History test view must change");
    }
    int count = 0;
    while (session.back()) ++count;
    check(count == 40, "Each file history must keep at most 40 entries");
    check(!session.activateFile("missing"), "Unknown file must not activate");
    session.newProject();
    check(session.project().files.empty() && !session.activeFile(), "New project must be truly empty");
    check(!session.canBack() && !session.removeActiveFile(), "Empty project operations must be safe");
    session.addDemoFile();
    check(session.removeActiveFile() && !session.activeFile() && session.project().activeFileId.empty(),
          "Removing last file must leave valid empty project");
}

void testSelectionAndChannels() {
    Session session;
    auto* first = session.activeFile();
    const auto firstFileId = first->metadata.id;
    const auto secondFileId = session.project().files[1].metadata.id;
    const auto firstMarkId = first->marks[0].id;
    const auto secondMarkId = first->marks[1].id;
    const auto originalView = first->view;
    session.selectMarks({firstMarkId, secondMarkId, firstMarkId, "missing"}, secondMarkId);
    check(first->selectedMarkIds.size() == 2 && first->activeMarkId == secondMarkId,
          "Selection must remove duplicate and nonexistent ids and preserve active mark");
    check(first->view == originalView && !session.canBack(), "Selection must not navigate");
    check(session.createChannelFromActiveMark(), "Active mark must create a demo channel");
    const auto channelCenter = first->channels[0].centerFrequencyHz;
    const auto channelBandwidth = first->channels[0].bandwidthHz;
    findMark(*first, secondMarkId)->range.frequency = {105e6, 110e6};
    check(first->channels[0].centerFrequencyHz == channelCenter &&
          first->channels[0].bandwidthHz == channelBandwidth,
          "Channel extraction parameters must remain fixed after mark edits");
    check(session.focusMark(firstMarkId) && session.canBack(), "Focus must navigate with padding and record history");
    check(first->view.time.begin < first->marks[0].range.time.begin &&
          first->view.time.end > first->marks[0].range.time.end,
          "Focused mark must have time padding when file boundaries allow it");
    session.selectMarks({firstMarkId, secondMarkId}, firstMarkId);
    first->display.psdFromSelection = true;
    const auto result = session.deleteSelectedMarks();
    check(result.marks == 2 && result.channels == 1, "Batch delete must report linked mark/channel removals");
    check(first->marks.empty() && first->channels.empty() && first->selectedMarkIds.empty() &&
          first->activeMarkId.empty() && !first->display.psdFromSelection,
          "Delete must clear selection and restore visible-window PSD scope");
    check(session.back() && first->marks.empty(), "View back must not undo business deletion");
    check(session.activateFile(secondFileId) && session.activeFile()->marks.size() == 1,
          "Deleting first-file marks must preserve second-file marks");
    session.selectMarks({});
    check(!session.createChannelFromActiveMark() && !session.activeFile()->display.psdFromSelection,
          "Empty selection must disable mark-based PSD and channel creation");
    session.activateFile(firstFileId);
    const auto tinyId = session.addMark({{1, 2}, {99e6, 99e6 + 1}});
    check(!tinyId.empty() && findMark(*session.activeFile(), tinyId)->range.time == TimeRange{1, 2},
          "Small legacy or imported marks must retain nonzero spans below STFT resolution");
    check(session.addMark({{1, 1}, {99e6, 100e6}}).empty(), "Zero sample span must be rejected");
}

void testSerialization() {
    QTemporaryDir directory;
    check(directory.isValid(), "Temporary directory must be available");
    const auto path = directory.filePath(QStringLiteral("工程.json"));
    const auto comparisonPath = directory.filePath(QStringLiteral("comparison.json"));
    const auto badPath = directory.filePath(QStringLiteral("invalid.json"));
    Session session;
    session.createChannelFromActiveMark();
    auto& first = session.project().files[0];
    first.metadata.sampleCount = std::numeric_limits<SampleIndex>::max();
    first.metadata.path = "D:/演示数据/捕获.iq";
    constexpr SampleIndex largeIndex = (SampleIndex{1} << 53) + 17;
    first.view.time = {largeIndex, largeIndex + 4096};
    first.marks[0].range.time = {largeIndex + 1, largeIndex + 2049};
    session.activateFile(session.project().files[1].metadata.id);
    session.createChannelFromActiveMark();
    QString error;
    check(ProjectStore::save(path, session.project(), error), "Valid native project must save");
    const auto sourceBytes = readFile(path);
    check(sourceBytes.contains("18446744073709551615") && sourceBytes.contains("9007199254741009"),
          "uint64 indices must be decimal strings in JSON");
    check(!sourceBytes.contains("past") && !sourceBytes.contains("future"), "View histories must not serialize");
    Project loaded;
    check(ProjectStore::load(path, loaded, error), "Saved project must load");
    check(loaded.files[0].metadata.sampleCount == std::numeric_limits<SampleIndex>::max() &&
          loaded.files[0].view.time.begin == largeIndex && loaded.files[0].marks[0].range.time.begin == largeIndex + 1,
          "Roundtrip must preserve all uint64 bits beyond 2^53");
    check(loaded.activeFileId == session.project().activeFileId &&
          loaded.files[1].display.mainMode == MainMode::Waterfall &&
          loaded.files[1].display.auxiliaryMode == AuxiliaryMode::Psd &&
          loaded.files[1].display.psdFromSelection && loaded.files[1].channels.size() == 1,
          "Roundtrip must preserve active file and independent display/selection/channel state");
    check(ProjectStore::save(comparisonPath, loaded, error) && readFile(comparisonPath) == sourceBytes,
          "Every serialized field must survive a canonical roundtrip");

    Project edgeProject;
    FileState edgeFile;
    edgeFile.metadata = {"edge-file", "fractional-frequency.iq", "", 19'481'875.246919144,
                         774'187'639.2423745, 10000, true};
    edgeFile.view = fullRange(edgeFile.metadata);
    edgeFile.marks.push_back({"edge-mark", "边界标记", {{0, 10000},
        {edgeFile.view.frequency.lowerHz, 779'965'662.0001838}}});
    edgeFile.activeMarkId = "edge-mark";
    edgeFile.selectedMarkIds = {"edge-mark"};
    edgeProject.activeFileId = "edge-file";
    edgeProject.files.push_back(edgeFile);
    Session edgeSession;
    edgeSession.replaceProject(edgeProject);
    check(edgeSession.createChannelFromActiveMark() &&
          ProjectStore::save(directory.filePath(QStringLiteral("frequency-edge.json")), edgeSession.project(), error),
          "Derived channel edge rounding must not reject a valid file-boundary mark");

    const auto validRoot = QJsonDocument::fromJson(sourceBytes).object();
    auto reject = [&](QJsonObject malformed) {
        writeFile(badPath, QJsonDocument(malformed).toJson());
        check(!ProjectStore::load(badPath, loaded, error) && !error.isEmpty(),
              "Malformed project must fail with an error");
        check(ProjectStore::save(comparisonPath, loaded, error) && readFile(comparisonPath) == sourceBytes,
              "Failed import must preserve the complete previous project");
    };
    auto mutateFirst = [&](const std::function<void(QJsonObject&)>& mutate) {
        auto root = validRoot;
        auto files = root[QStringLiteral("files")].toArray();
        auto file = files[0].toObject();
        mutate(file);
        files[0] = file;
        root[QStringLiteral("files")] = files;
        reject(root);
    };
    auto root = validRoot;
    root[QStringLiteral("schema")] = QStringLiteral("signal-studio-a1.4.3-prototype");
    reject(root);
    root = validRoot;
    root[QStringLiteral("version")] = 2;
    reject(root);
    root = validRoot;
    root[QStringLiteral("activeFileId")] = QStringLiteral("missing");
    reject(root);
    root = validRoot;
    auto duplicateFiles = root[QStringLiteral("files")].toArray();
    duplicateFiles.append(duplicateFiles[0]);
    root[QStringLiteral("files")] = duplicateFiles;
    reject(root);
    mutateFirst([](QJsonObject& file) {
        auto metadata = file[QStringLiteral("metadata")].toObject();
        metadata[QStringLiteral("sampleCount")] = 9007199254741009.0;
        file[QStringLiteral("metadata")] = metadata;
    });
    mutateFirst([](QJsonObject& file) {
        auto metadata = file[QStringLiteral("metadata")].toObject();
        metadata[QStringLiteral("sampleCount")] = QStringLiteral("18446744073709551616");
        file[QStringLiteral("metadata")] = metadata;
    });
    mutateFirst([](QJsonObject& file) {
        auto metadata = file[QStringLiteral("metadata")].toObject();
        metadata[QStringLiteral("sampleRateHz")] = 0;
        file[QStringLiteral("metadata")] = metadata;
    });
    mutateFirst([](QJsonObject& file) {
        auto display = file[QStringLiteral("display")].toObject();
        display[QStringLiteral("mainMode")] = QStringLiteral("unknown");
        file[QStringLiteral("display")] = display;
    });
    mutateFirst([](QJsonObject& file) {
        auto display = file[QStringLiteral("display")].toObject();
        display[QStringLiteral("stftSize")] = 2049;
        file[QStringLiteral("display")] = display;
    });
    mutateFirst([](QJsonObject& file) {
        auto display = file[QStringLiteral("display")].toObject();
        display[QStringLiteral("grid")] = 1;
        file[QStringLiteral("display")] = display;
    });
    mutateFirst([](QJsonObject& file) {
        auto marks = file[QStringLiteral("marks")].toArray();
        marks.append(marks[0]);
        file[QStringLiteral("marks")] = marks;
    });
    mutateFirst([](QJsonObject& file) {
        auto channels = file[QStringLiteral("channels")].toArray();
        auto channel = channels[0].toObject();
        channel[QStringLiteral("sourceMarkId")] = QStringLiteral("missing");
        channels[0] = channel;
        file[QStringLiteral("channels")] = channels;
    });
    mutateFirst([](QJsonObject& file) {
        file[QStringLiteral("selectedMarkIds")] = QJsonArray{QStringLiteral("missing")};
    });
    mutateFirst([](QJsonObject& file) {
        auto view = file[QStringLiteral("view")].toObject();
        view[QStringLiteral("time")] = QJsonObject{{QStringLiteral("begin"), QStringLiteral("2")},
                                                 {QStringLiteral("end"), QStringLiteral("1")}};
        file[QStringLiteral("view")] = view;
    });
    writeFile(badPath, QByteArray("{invalid json"));
    check(!ProjectStore::load(badPath, loaded, error), "Invalid JSON syntax must fail");
    check(ProjectStore::save(comparisonPath, loaded, error) && readFile(comparisonPath) == sourceBytes,
          "Invalid JSON syntax must leave target unchanged");
    auto invalidState = loaded;
    invalidState.files[0].metadata.sampleRateHz = 0;
    check(!ProjectStore::save(path, invalidState, error) && readFile(path) == sourceBytes,
          "Invalid save must leave existing destination unchanged");
    check(!ProjectStore::load(directory.filePath(QStringLiteral("missing.json")), loaded, error),
          "Missing input path must return an error");
    check(!ProjectStore::save(directory.filePath(QStringLiteral("missing/sub/project.json")), loaded, error),
          "Unwritable output path must return an error");
    const Project empty;
    check(ProjectStore::save(path, empty, error) && ProjectStore::load(path, loaded, error) &&
          loaded.files.empty() && loaded.activeFileId.empty(), "Empty native project must roundtrip");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    try {
        testClamping();
        std::cout << "PASS boundary clamping and uint64 precision\n";
        testSessionHistory();
        std::cout << "PASS independent file state and 40-entry view history\n";
        testSelectionAndChannels();
        std::cout << "PASS selection, focus, linked deletion and fixed demo channels\n";
        testSerialization();
        std::cout << "PASS strict atomic native JSON and full-state uint64 roundtrip\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
