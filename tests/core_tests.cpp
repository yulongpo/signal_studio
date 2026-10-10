#include "application/session.h"
#include "infrastructure/int16_iq_file.h"
#include "infrastructure/project_store.h"
#include "ui/charts/chart_interaction.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace signalstudio;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(double left, double right, double tolerance = 1e-7) {
    return std::abs(left - right) <= tolerance;
}

void testSharedChartInteractionMath() {
    using namespace chart_interaction;
    const auto zoomed = zoomAround({-200.0, 200.0}, .5, .25);
    check(close(zoomed.first, -150.0) && close(zoomed.last, 50.0),
          "Shared axis zoom must preserve the pointer anchor");
    const auto shifted = panByFraction({10.0, 30.0}, -.25);
    check(close(shifted.first, 5.0) && close(shifted.last, 25.0),
          "Shared axis pan must use the visible span");
    const auto selected = selectFractions({0.0, 100.0}, .8, .2);
    check(close(selected.first, 20.0) && close(selected.last, 80.0),
          "Shared box selection must normalize reverse drag direction");
    check(close(fractionAt(75.0, 50.0, 100.0), .25),
          "Shared coordinate transform must map pixels to clamped axis fractions");
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

void seedPrototypeFiles(Session& session) {
    const auto first = session.addDemoFile();
    session.addDemoFile();
    session.addDemoFile();
    session.activateFile(first);
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
    check(clamped.time.end - clamped.time.begin == 1, "Waveform view must zoom to one sample independently of FFT");
    check(clamped.frequency.upperHz > clamped.frequency.lowerHz, "Frequency range must stay positive without old Fs/FFT clipping");
    clamped = clampRange({{5'000, 5'001}, {100e6, 100e6}}, metadata, 2048, 8192);
    check(clamped.time.end - clamped.time.begin == 1,
          "Insufficient FFT windows must not silently extend the waveform view");
    clamped = clampRange({{8'000, 2'000}, {105e6, 95e6}}, metadata, 2048);
    check(clamped.time == TimeRange{2'000, 8'000}, "Reversed time must be ordered");
    check(clamped.frequency == FrequencyRange{95e6, 105e6}, "Reversed frequency must be ordered");
    clamped = clampRange({{0, 1}, {0, 1e300}}, metadata, 16384);
    check(clamped.time == TimeRange{0, 1} && clamped.frequency == bounds.frequency,
          "FFT settings must not force the user time view to full file");
    clamped = clampRange({{0, 100}, {std::numeric_limits<double>::quiet_NaN(), 100e6}}, metadata, 2048);
    check(clamped.frequency == bounds.frequency, "Nonfinite UI frequency input must recover full band");

    auto effective = metadata;
    effective.effectiveBandwidthHz = 8e6;
    const auto effectiveBounds = fullRange(effective);
    check(effectiveBounds.frequency == FrequencyRange{96e6, 104e6},
          "Effective bandwidth must center the analysis range on the tuned frequency");
    clamped = clampRange({{0, 10'000}, {80e6, 120e6}}, effective, 2048);
    check(clamped.frequency == effectiveBounds.frequency,
          "Frequency panning and fit must remain inside the configured effective bandwidth");

    auto huge = metadata;
    huge.sampleCount = std::numeric_limits<SampleIndex>::max();
    clamped = clampRange({{huge.sampleCount - 3, huge.sampleCount}, bounds.frequency}, huge, 2048);
    check(clamped.time == TimeRange{huge.sampleCount - 3, huge.sampleCount},
          "uint64 upper-bound clamping must not overflow or lose sample precision");
}

void testSessionHistory() {
    Session session;
    check(session.project().files.empty() && !session.activeFile(),
          "A new session must start in an empty project state");
    seedPrototypeFiles(session);
    check(session.project().files.size() == 3, "Startup must match the three-file prototype baseline");
    check(session.project().files[0].metadata.demoSeed==1&&session.project().files[1].metadata.demoSeed==3&&
          session.project().files[2].metadata.demoSeed==7,"Startup demo seeds must match prototype 1/3/7");
    for(const auto& file:session.project().files)
        check(file.marks.empty()&&file.channels.empty()&&file.display.mainMode==MainMode::TimeFrequency&&
              file.display.auxiliaryMode==AuxiliaryMode::Waveform&&file.display.dynamicRangeDb==80&&
              file.display.referenceLevelDb==0&&file.display.auxiliaryMin==-32768&&file.display.auxiliaryMax==32768,
              "Prototype startup must have empty marks/channels and common display defaults");
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
    session.activateFile(firstId);
    check(session.setEffectiveBandwidthHz(10e6) &&
          session.activeFile()->metadata.effectiveBandwidthHz == 10e6 &&
          session.activeFile()->view.frequency == FrequencyRange{95e6, 105e6},
          "Changing the effective bandwidth must clamp the current view around the tuned center");
    session.newProject();
    check(session.project().files.empty() && !session.activeFile(), "New project must be truly empty");
    check(!session.canBack() && !session.removeActiveFile(), "Empty project operations must be safe");
    session.addDemoFile();
    check(session.removeActiveFile() && !session.activeFile() && session.project().activeFileId.empty(),
          "Removing last file must leave valid empty project");
    const auto customId=session.addDemoFile("custom_capture.iq",40e6,125e6,180,"D:/模拟输入/custom_capture.iq");
    check(!customId.empty()&&session.activeFile()->metadata.sampleCount==7'200'000'000&&
          session.activeFile()->metadata.path=="D:/模拟输入/custom_capture.iq"&&session.activeFile()->marks.empty(),
          "Add dialog metadata must produce a demo file without reading IQ or creating marks");
    const auto fileCount=session.project().files.size();
    check(session.addDemoFile("invalid.iq",0,100e6,180).empty()&&session.project().files.size()==fileCount,
          "Invalid add dialog metadata must not mutate project");
}

void testSelectionAndChannels() {
    Session session;
    seedPrototypeFiles(session);
    session.addMark({{7'600'000'000,8'400'000'000},{96e6,110e6}});
    session.addMark({{8'000'000'000,8'880'000'000},{100e6,114e6}});
    auto* first = session.activeFile();
    const auto firstFileId = first->metadata.id;
    const auto secondFileId = session.project().files[1].metadata.id;
    session.activateFile(secondFileId);
    session.addMark({{2'000'000'000,2'320'000'000},{2447e6,2453e6}});
    session.activateFile(firstFileId);
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

void testAuxiliarySnapshotsAndPsdScope() {
    Session session;
    seedPrototypeFiles(session);
    const auto firstId=session.project().activeFileId;
    const auto secondId=session.project().files[1].metadata.id;
    const auto original=session.snapshot();
    check(!session.setPsdFromSelection(true),"Selection PSD scope must require an active mark");
    check(session.setAuxiliaryRange(-40,40)&&session.canBack(),"Auxiliary Y changes must enter view history");
    check(session.setAuxiliaryMode(AuxiliaryMode::Psd)&&session.activeFile()->display.auxiliaryMin==-100&&
          session.activeFile()->display.auxiliaryMax==0,"PSD switch must restore its independent Y range");
    check(session.setAuxiliaryRange(-120,-20),"PSD Y must be editable independently");
    const auto changed=session.snapshot();
    check(session.back()&&session.activeFile()->display.auxiliaryMin==-100&&
          session.activeFile()->display.waveformMin==-40,"Back must restore both auxiliary ranges without changing mode");
    check(session.back()&&session.snapshot()==original,"Second back must restore original combined snapshot");
    check(session.forward()&&session.forward()&&session.snapshot()==changed,"Forward must restore both Y ranges");
    check(session.setAuxiliaryMode(AuxiliaryMode::Waveform)&&session.activeFile()->display.auxiliaryMin==-40&&
          session.activeFile()->display.auxiliaryMax==40,"Waveform switch must preserve its earlier zoom");
    const auto beforeReset=session.snapshot();
    session.resetView();
    check(session.snapshot()==original&&session.back()&&session.snapshot()==beforeReset,
          "Reset must restore both default Y ranges in one undoable view step");
    const auto transientBase=session.snapshot();
    check(session.setAuxiliaryRange(-20,20,false),"Uncommitted auxiliary gesture must update live state");
    session.activateFile(secondId);
    const auto secondBase=session.snapshot();
    check(session.restoreSnapshot(transientBase)&&session.snapshot()==secondBase,
          "Late gesture rollback must restore its original file without switching active file");
    session.activateFile(firstId);
    check(session.snapshot()==transientBase,"Rollback must restore the original file's complete Y state");
    session.setAuxiliaryRange(-30,30,false);
    session.activateFile(secondId);
    check(session.commitViewChange(transientBase)&&session.snapshot()==secondBase&&!session.canBack(),
          "Late wheel commit must record its source file without affecting current file history");
    session.activateFile(firstId);
    check(session.back()&&session.snapshot()==transientBase,"Source-file back must undo late wheel commit");
    const auto markId=session.addMark({{100,200},{99e6,101e6}});
    check(!markId.empty()&&session.setPsdFromSelection(true),"Active mark must enable selection PSD");
    session.deleteSelectedMarks();
    check(!session.activeFile()->display.psdFromSelection,"Deleting active PSD source must return to visible scope");
    check(session.setAuxiliaryRange(-100000,100000)&&session.activeFile()->display.auxiliaryMin==-65536&&
          session.activeFile()->display.auxiliaryMax==65536,"ADC-count waveform Y must clamp to full supported display bounds");
}

void testPowerDisplayRange() {
    SpectrogramData heatmap;
    auto frame = std::make_shared<SpectralFrame>();
    frame->linearPower = {1e-8f, 1e-4f, 0, -1, std::numeric_limits<float>::quiet_NaN()};
    heatmap.frames.push_back(frame);
    SpectralFrame psd; psd.linearPower = {1e-9f, 1e-3f};
    const auto fit = fitPowerDisplayRange(heatmap, &psd);
    check(fit.valid && !fit.limited && close(fit.range.referenceLevelDb,-27,1e-5) && close(fit.range.lowerDb(),-93,1e-5),
        "Power fit must include full heatmap and current PSD with 3 dB margins");
    frame->linearPower = {1e-6f,1e-6f}; heatmap.frames = {frame};
    const auto constant = fitPowerDisplayRange(heatmap,nullptr);
    check(constant.valid && close(constant.range.dynamicRangeDb,6), "Constant power must retain both margins");
    frame->linearPower = {0,-1,std::numeric_limits<float>::infinity()};
    check(!fitPowerDisplayRange(heatmap,nullptr).valid, "No positive finite power must disable fit");
    frame->linearPower = {1e-30f,1e30f};
    const auto limited = fitPowerDisplayRange(heatmap,nullptr);
    check(limited.valid && limited.limited && limited.range.referenceLevelDb==100 && limited.range.lowerDb()==-203,
        "Power fit must use the analysis floor and report legal reference limits");
    check(!PowerDisplayRange{0,0}.valid() && !PowerDisplayRange{101,80}.valid() &&
        validPsdRange(-10200,-200) && !validPsdRange(-10400,0), "Power and PSD limits must be consistent");

    Session session; seedPrototypeFiles(session);
    session.addMark({{100,10000},{99e6,101e6}}); session.createChannelFromActiveMark();
    session.setAuxiliaryMode(AuxiliaryMode::Psd); session.pinCursor(session.activeFile()->metadata.id,250,100e6,false);
    const auto pin = session.linkedCursor(session.activeFile()->metadata.id);
    const auto canBack = session.canBack();
    check(session.setPowerDisplayRange({-200,10000}), "Maximum legal display range must apply");
    for (const auto& file : session.project().files) {
        check(file.display.psdMin==-10200 && file.display.psdMax==-200 && file.display.referenceLevelDb==-200,
            "Power parameters must update all file PSD axes");
        for (const auto& channel : file.channels) check(channel.psdAxisMinimum==-10200 && channel.psdAxisMaximum==-200,
            "Power parameters must update all channel PSD axes");
    }
    check(session.canBack()==canBack && session.linkedCursor(session.activeFile()->metadata.id).sourceSample==pin.sourceSample,
        "Power edits must preserve history and linked pin");
    check(session.setAuxiliaryRange(-300,-220,false) && session.activeFile()->display.psdMax==-220,
        "Manual PSD range must be independent without legacy fixed bounds");
    check(session.setPowerDisplayRange({-10,60}) && session.activeFile()->display.psdMin==-70,
        "Next display parameter change must realign manual PSD range");
    check(!session.setPowerDisplayRange({0,10001}) && session.activeFile()->display.psdMin==-70,
        "Invalid parameter command must not mutate state");
    session.setPowerDisplayRange({-200,10000});
    QTemporaryDir directory; QString error; Project loaded;
    const auto path=directory.filePath("power.json");
    check(ProjectStore::save(path,session.project(),error) && ProjectStore::load(path,loaded,error),
        "Extreme legal power ranges must roundtrip without changing project format");
    check(loaded.files.front().display.psdMin==-10200 && loaded.files.front().channels.front().psdAxisMinimum==-10200,
        "File and channel PSD ranges must survive project load");
}

void testSerialization() {
    QTemporaryDir directory;
    check(directory.isValid(), "Temporary directory must be available");
    const auto path = directory.filePath(QStringLiteral("工程.json"));
    const auto comparisonPath = directory.filePath(QStringLiteral("comparison.json"));
    const auto badPath = directory.filePath(QStringLiteral("invalid.json"));
    Session session;
    seedPrototypeFiles(session);
    session.addMark({{7'600'000'000,8'400'000'000},{96e6,110e6}});
    session.addMark({{8'000'000'000,8'880'000'000},{100e6,114e6}});
    session.createChannelFromActiveMark();
    auto& first = session.project().files[0];
    first.metadata.sampleCount = std::numeric_limits<SampleIndex>::max();
    first.metadata.path = "D:/演示数据/捕获.iq";
    first.metadata.demo = false;
    first.metadata.declaredBandwidthHz = 80e6;
    constexpr SampleIndex largeIndex = (SampleIndex{1} << 53) + 17;
    first.view.time = {largeIndex, largeIndex + 4096};
    first.marks[0].range.time = {largeIndex + 1, largeIndex + 2049};
    auto& largeChannel = first.channels.front();
    largeChannel.sourceMarkId = first.marks[0].id;
    largeChannel.sourceTime = first.marks[0].range.time;
    largeChannel.visibleSourceTime = {largeIndex + 20, largeIndex + 1024};
    largeChannel.visibleBasebandFrequency = {-2e6, 2e6};
    largeChannel.absoluteFrequencyLabels = true;
    largeChannel.processingState = ChannelProcessingState::Ready;
    session.activateFile(session.project().files[1].metadata.id);
    session.addMark({{2'000'000'000,2'320'000'000},{2447e6,2453e6}});
    auto* second=session.activeFile();
    second->display.mainMode=MainMode::Waterfall;second->display.palette=Palette::CoolEditClassic;
    second->display.waveformMode=WaveformMode::Q;
    second->display.psdSize=8192;second->display.stftSize=4096;
    second->display.psd.parameters.window=SpectralWindow::Kaiser;second->display.psd.parameters.kaiserBeta=9.1;
    second->display.psd.statistic=SpectrumStatistic::Maximum;second->display.spectrogram.parameters.overlap=.75;
    first.metadata.availability={largeIndex+10000,LoadStatus::Partial,0,"test-source-fingerprint",{}};
    second->display.dynamicRangeDb=60;second->display.referenceLevelDb=-20;
    second->metadata.effectiveBandwidthHz=4e6;
    second->view=clampRange(second->view,second->metadata,second->display.stftSize);
    second->display.absoluteFrequency=false;second->display.grid=false;second->display.colorScale=true;
    session.setAuxiliaryMode(AuxiliaryMode::Psd);
    session.setAuxiliaryRange(-110,-10,false);
    session.setPsdFromSelection(true);
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
          loaded.files[0].view.time.begin == largeIndex && loaded.files[0].marks[0].range.time.begin == largeIndex + 1 &&
          !loaded.files[0].metadata.demo && loaded.files[0].metadata.declaredBandwidthHz == 80e6 &&
          loaded.files[0].metadata.path == "D:/演示数据/捕获.iq",
          "Roundtrip must preserve all uint64 bits beyond 2^53");
    check(loaded.files[0].channels.front().sourceTime == TimeRange{largeIndex + 1, largeIndex + 2049} &&
          loaded.files[0].channels.front().visibleSourceTime == TimeRange{largeIndex + 20, largeIndex + 1024} &&
          loaded.files[0].channels.front().processingState == ChannelProcessingState::Ready &&
          loaded.files[0].channels.front().absoluteFrequencyLabels,
          "Channel snapshots must retain exact uint64 source and visible ranges");
    check(loaded.activeFileId == session.project().activeFileId &&
          loaded.files[1].display.mainMode == MainMode::Waterfall &&
          loaded.files[1].display.auxiliaryMode == AuxiliaryMode::Psd &&
          loaded.files[1].display.waveformMode == WaveformMode::Q &&
          loaded.files[1].display.palette == Palette::CoolEditClassic &&
          loaded.files[1].metadata.effectiveBandwidthHz == 4e6 &&
          loaded.files[1].display.psdFromSelection && loaded.files[1].channels.size() == 1,
          "Roundtrip must preserve active file and independent display/selection/channel state");
    check(loaded.files[0].metadata.availability.status==LoadStatus::Partial&&availableSamples(loaded.files[0].metadata)==largeIndex+10000&&loaded.files[0].metadata.availability.fingerprint=="test-source-fingerprint","Version 3 must preserve the physical length and bounded prefix separately");
    check(loaded.files[1].display.psd.parameters==second->display.psd.parameters&&loaded.files[1].display.psd.statistic==SpectrumStatistic::Maximum&&loaded.files[1].display.spectrogram.parameters.overlap==.75,"Independent PSD/STFT parameters must survive roundtrip");
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
    auto legacyRoot = validRoot;
    legacyRoot[QStringLiteral("version")] = 1;
    legacyRoot.remove(QStringLiteral("activeChannelId"));
    legacyRoot.remove(QStringLiteral("narrowbandWorkspaceOpen"));
    auto legacyFiles = legacyRoot[QStringLiteral("files")].toArray();
    auto legacyFirst = legacyFiles[0].toObject();
    auto legacyChannels = legacyFirst[QStringLiteral("channels")].toArray();
    auto legacyChannel = legacyChannels[0].toObject();
    for (const auto* key : {"outputSampleRateHz", "filter", "processingState", "configVersion", "sourceTime",
                            "wholeSource", "preserveSourceTime", "page", "visibleSourceTime",
                            "visibleBasebandFrequency", "psdFftSize", "stftFftSize", "waveform",
                            "symbolRate", "eyePeriods", "eyeTraces", "eyeComponent", "selectedBit",
                            "constellationMinimum", "constellationMaximum", "relativeTime", "absoluteFrequencyLabels"})
        legacyChannel.remove(QString::fromLatin1(key));
    legacyChannels[0] = legacyChannel;
    legacyFirst[QStringLiteral("channels")] = legacyChannels;
    legacyFiles[0] = legacyFirst;
    legacyRoot[QStringLiteral("files")] = legacyFiles;
    writeFile(badPath, QJsonDocument(legacyRoot).toJson());
    Project migrated;
    const bool migrationSucceeded = ProjectStore::load(badPath, migrated, error);
    check(migrationSucceeded && !migrated.files.empty() && !migrated.files[0].channels.empty() &&
          migrated.files[0].channels[0].processingState == ChannelProcessingState::LegacyNeedsReview &&
          migrated.files[0].channels[0].sourceTime == migrated.files[0].marks[0].range.time,
          "Version 1 channels must migrate from their source marks as pending-review snapshots");
    auto reject = [&](QJsonObject malformed) {
        writeFile(badPath, QJsonDocument(malformed).toJson());
        check(!ProjectStore::load(badPath, loaded, error) && !error.isEmpty(),
              "Malformed project must fail with an error");
        check(loaded.files[0].metadata.availability.status==LoadStatus::Partial&&availableSamples(loaded.files[0].metadata)==largeIndex+10000&&loaded.files[0].metadata.availability.fingerprint=="test-source-fingerprint","Version 3 must preserve the physical length and bounded prefix separately");
    check(loaded.files[1].display.psd.parameters==second->display.psd.parameters&&loaded.files[1].display.psd.statistic==SpectrumStatistic::Maximum&&loaded.files[1].display.spectrogram.parameters.overlap==.75,"Independent PSD/STFT parameters must survive roundtrip");
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
    root[QStringLiteral("version")] = 4;
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
    mutateFirst([](QJsonObject& file) {auto d=file["display"].toObject();auto p=d["psdSettings"].toObject();auto params=p["parameters"].toObject();params["overlap"]=1.0;p["parameters"]=params;d["psdSettings"]=p;file["display"]=d;});
    mutateFirst([](QJsonObject& file) {auto d=file["display"].toObject();d.remove("spectrogramSettings");file["display"]=d;});
    mutateFirst([](QJsonObject& file) {auto m=file["metadata"].toObject();m["availableSamples"]="18446744073709551616";file["metadata"]=m;});
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
        auto display=file[QStringLiteral("display")].toObject();
        display[QStringLiteral("psdMin")]=-20000;
        file[QStringLiteral("display")]=display;
    });
    mutateFirst([](QJsonObject& file) {
        auto metadata=file[QStringLiteral("metadata")].toObject();
        metadata[QStringLiteral("demoSeed")]=-1;
        file[QStringLiteral("metadata")]=metadata;
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
    check(loaded.files[0].metadata.availability.status==LoadStatus::Partial&&availableSamples(loaded.files[0].metadata)==largeIndex+10000&&loaded.files[0].metadata.availability.fingerprint=="test-source-fingerprint","Version 3 must preserve the physical length and bounded prefix separately");
    check(loaded.files[1].display.psd.parameters==second->display.psd.parameters&&loaded.files[1].display.psd.statistic==SpectrumStatistic::Maximum&&loaded.files[1].display.spectrogram.parameters.overlap==.75,"Independent PSD/STFT parameters must survive roundtrip");
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

void testInt16IqFilePipeline() {
    QTemporaryDir directory;
    check(directory.isValid(), "IQ fixture directory must be available");
    const auto path = directory.filePath(QStringLiteral("IQ0_FS1Msps_BW800kHz_FC10MHz.dat"));
    QByteArray bytes;
    bytes.resize(4096 * 4);
    for (int n = 0; n < 4096; ++n) {
        qToLittleEndian<qint16>(16384, reinterpret_cast<uchar*>(bytes.data()) + n * 4);
        qToLittleEndian<qint16>(0, reinterpret_cast<uchar*>(bytes.data()) + n * 4 + 2);
    }
    writeFile(path, bytes);

    QString error;
    const auto descriptor = describeInt16IqFile(path, error);
    check(descriptor.has_value() && error.isEmpty(), "IQ filename metadata must parse");
    check(descriptor->metadata.sampleRateHz == 1e6 && descriptor->metadata.centerFrequencyHz == 10e6 &&
          descriptor->declaredBandwidthHz == 800e3 && descriptor->metadata.sampleCount == 4096 &&
          !descriptor->metadata.demo, "IQ filename fields and interleaved-int16 sample count must be retained");

    Int16IqFile iq;
    check(iq.open(path, error) && iq.sampleCount() == 4096, "Raw IQ file must map without decoding the full file");
    std::vector<float> waveform;
    check(iq.waveform({0, 4096}, 16, WaveformMode::IqRms, waveform) && waveform.size() == 16,
          "Waveform extractor must return the requested IQ RMS envelope");
    check(std::all_of(waveform.begin(), waveform.end(), [](float value) { return std::abs(value - 16384.0f) < .02f; }),
          "IQ RMS waveform must be expressed in ADC-count-equivalent amplitude");
    check(iq.waveform({0, 4096}, 16, WaveformMode::I, waveform) &&
          std::all_of(waveform.begin(), waveform.end(), [](float value) { return std::abs(value - 16384.0f) < 1.0f; }),
          "I mode must expose ADC-count-equivalent signed in-phase samples");
    check(iq.waveform({0, 4096}, 16, WaveformMode::Q, waveform) &&
          std::all_of(waveform.begin(), waveform.end(), [](float value) { return std::abs(value) < .001f; }),
          "Q mode must expose ADC-count-equivalent signed quadrature samples");

    std::vector<float> psd;
    check(iq.psd({0, 4096}, {9.5e6, 10.5e6}, 1e6, 10e6, 256, 64, psd) && psd.size() == 64,
          "Welch PSD must produce the requested frequency trace");
    check(std::distance(psd.begin(), std::max_element(psd.begin(), psd.end())) >= 30 &&
          std::distance(psd.begin(), std::max_element(psd.begin(), psd.end())) <= 33,
          "Complex DC tone must map to the center-frequency PSD bins");

    std::vector<float> heatmap;
    const FileMetadata metadata = descriptor->metadata;
    check(iq.spectrogram(metadata, {{0, 4096}, {9.5e6, 10.5e6}}, MainMode::TimeFrequency,
                         QSize(32, 16), 256, heatmap) && heatmap.size() == 512,
          "STFT extractor must return a bounded raster for the requested view");
    check(*std::max_element(heatmap.begin(), heatmap.end()) > -80,
          "STFT raster must include the real DC tone instead of synthetic cells");

    QByteArray cleanSamples(4096 * 4, '\0');
    QByteArray outsideSignal = cleanSamples;
    for (int n = 0; n < 1024; ++n)
        qToLittleEndian<qint16>(20000, reinterpret_cast<uchar*>(outsideSignal.data()) + n * 4);
    const auto cleanPath = directory.filePath(QStringLiteral("clean_FS1Msps_BW800kHz_FC10MHz.dat"));
    const auto outsidePath = directory.filePath(QStringLiteral("outside_FS1Msps_BW800kHz_FC10MHz.dat"));
    writeFile(cleanPath, cleanSamples);
    writeFile(outsidePath, outsideSignal);
    Int16IqFile cleanIq, outsideIq;
    check(cleanIq.open(cleanPath, error) && outsideIq.open(outsidePath, error),
          "Visible-window FFT fixtures must open");
    std::vector<float> cleanPsd, outsidePsd;
    check(cleanIq.psd({1024, 2048}, {9.5e6, 10.5e6}, 1e6, 10e6, 256, 64, cleanPsd) &&
          outsideIq.psd({1024, 2048}, {9.5e6, 10.5e6}, 1e6, 10e6, 256, 64, outsidePsd),
          "PSD must calculate from the requested visible time range");
    check(cleanPsd == outsidePsd,
          "PSD must not read samples before or after the visible time range");
    std::vector<float> cleanHeatmap, outsideHeatmap;
    const ViewRange visibleView{{1024, 2048}, {9.5e6, 10.5e6}};
    check(cleanIq.spectrogram(metadata, visibleView, MainMode::TimeFrequency, QSize(32, 16), 256, cleanHeatmap) &&
          outsideIq.spectrogram(metadata, visibleView, MainMode::TimeFrequency, QSize(32, 16), 256, outsideHeatmap),
          "STFT must calculate from the requested visible time range");
    check(cleanHeatmap == outsideHeatmap,
          "STFT must not read samples before or after the visible time range");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    try {
        testClamping();
        std::cout << "PASS boundary clamping and uint64 precision\n";
        testSharedChartInteractionMath();
        std::cout << "PASS shared wideband and narrowband chart interaction math\n";
        testSessionHistory();
        std::cout << "PASS independent file state and 40-entry view history\n";
        testSelectionAndChannels();
        std::cout << "PASS selection, focus, linked deletion and fixed demo channels\n";
        testAuxiliarySnapshotsAndPsdScope();
        std::cout << "PASS independent auxiliary modes, combined snapshot history and PSD scope\n";
        testSerialization();
        std::cout << "PASS strict atomic native JSON and full-state uint64 roundtrip\n";
        testPowerDisplayRange();
        std::cout << "PASS full numerical power fit, global PSD coupling and extreme-range roundtrip\n";
        testInt16IqFilePipeline();
        std::cout << "PASS int16 IQ metadata, mapped reads, waveform, Welch PSD and STFT\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
