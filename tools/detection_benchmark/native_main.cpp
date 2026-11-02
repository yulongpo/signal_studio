#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTest>
#include <QThread>
#include <chrono>
#include <set>
#include <stdexcept>
#include "app/main_window.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/project_file.h"
#include "ui/charts/plot_widget.h"
#include "ui/detection/detection_card.h"
#include "ui/detection/detection_panel.h"
#include "ui/display_target.h"

using namespace signalstudio;
namespace {
std::uint64_t steadyNow() {
  return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
void require(bool ok, const QString& why) {
  if (!ok) throw std::runtime_error(why.toStdString());
}
void save(const QString& path, const QJsonObject& object) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), file.errorString());
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
  require(file.write(bytes) == bytes.size(), file.errorString());
}
bool waitFor(const std::function<bool()>& ready, int milliseconds) {
  return QTest::qWaitFor(ready, milliseconds);
}
}
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addOptions({{"request","Public real IQ request","json"},
      {"output","New empty output directory","path"},
      {"resident-repeat","One excluded warmup plus N measured rechecks","N","0"}});
  parser.process(app);
  QString output;
  try {
    require(parser.isSet("request") && parser.isSet("output"), "request/output are required");
    output = QFileInfo(parser.value("output")).absoluteFilePath();
    QDir root(output);
    require(!root.exists() || root.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(), "Output must be new/empty");
    require(root.mkpath("."), "Cannot create output");
    require(QGuiApplication::platformName() == "windows", "Native Windows is required");
    auto* screen = nativeAcceptanceScreen();
    require(screen != nullptr, "Need one actual 3840x2160 / 150% screen");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.filePath("settings"));
    QCoreApplication::setOrganizationName("SignalStudioRpoBenchmark");
    QCoreApplication::setApplicationName("SignalStudioDetectionNativeBenchmark");
    QFile input(parser.value("request"));
    require(input.open(QIODevice::ReadOnly) && input.size() <= 8 * 1024 * 1024, "Cannot read request");
    auto encoded = QJsonDocument::fromJson(input.readAll()).object();
    if (encoded.contains("run")) encoded = encoded["run"].toObject();
    if (encoded.contains("request")) encoded = encoded["request"].toObject();
    DetectionRun seed;
    QString error;
    require(decodeDetectionRequest(encoded, seed.request, error), error);
    const auto& identity = seed.request.sourceIdentity;
    FileState source;
    source.metadata.id = identity.fileId;
    source.metadata.path = identity.path;
    source.metadata.name = QFileInfo(QString::fromStdString(identity.path)).fileName().toStdString();
    source.metadata.demo = false;
    source.metadata.sampleRateHz = identity.sampleRateHz;
    source.metadata.centerFrequencyHz = identity.centerFrequencyHz;
    source.metadata.sampleFormat = identity.sampleFormat;
    source.metadata.sampleCount = identity.physicalSamples;
    source.metadata.effectiveBandwidthHz = identity.effectiveFrequency.upperHz - identity.effectiveFrequency.lowerHz;
    source.metadata.availability.availableSamples = identity.availableSamples;
    source.metadata.availability.status = identity.availableSamples == identity.physicalSamples ? LoadStatus::Ready : LoadStatus::Partial;
    source.metadata.availability.fingerprint = identity.quickFingerprint;
    MainWindow window;
    require(window.createProject(output, "native-benchmark", &error), error);
    require(window.addImportedSource(std::move(source), error), error);
    require(window.saveProject(root.filePath("native-benchmark.ssproj")), "Cannot save isolated native project");
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
    auto* run = window.findChild<QPushButton*>("runWidebandDetection");
    require(card && panel && run, "Native detection controls missing");
    showFullScreenOnScreen(window, screen);
    require(waitFor([&] { return window.isFullScreen() && window.size() == QSize(2560,1440); }, 10000), "Native fullscreen not established");
    if (auto* rail = window.findChild<QPushButton*>("propRailOpen"); rail && rail->isVisible()) rail->click();
    auto* tab = window.findChild<QPushButton*>("contextDetection");
    require(tab != nullptr, "Detection page missing");
    tab->click();
    seed.request.sourceIdentity = annotationSourceIdentity(window.session().activeFile()->metadata, window.session().projectGeneration());
    card->configure(&seed);
    QCoreApplication::processEvents();
    require(run->isVisible() && run->isEnabled(), "Native run button is not available: " + card->validationError());
    if (auto* plot = window.findChild<PlotWidget*>("mainPlot"))
      require(waitFor([&] { return plot->isDisplaySettled(); }, 30000), "Initial native chart did not settle");
    bool countOk = false;
    const int repeats = parser.value("resident-repeat").toInt(&countOk);
    require(countOk && repeats >= 0 && repeats <= 30, "Invalid resident repeat count");
    QJsonArray rows;
    std::set<qint64> workers;
    const int total = repeats > 0 ? repeats + 1 : 1;
    for (int iteration = 0; iteration != total; ++iteration) {
      require(!panel->projectCommitInProgress() && run->isEnabled(), "Previous run has not settled");
      const auto* oldSlot = currentDetectionSlot(*window.session().activeFile(), identity.selectedSampleChannel);
      const auto previous = oldSlot ? oldSlot->committedRunId : std::string{};
      const auto click = steadyNow();
      QTest::mouseClick(run, Qt::LeftButton);
      require(waitFor([&] {
        const auto* file = window.session().activeFile();
        const auto* slot = currentDetectionSlot(*file, identity.selectedSampleChannel);
        return slot && !slot->committedRunId.empty() && slot->committedRunId != previous && !panel->projectCommitInProgress();
      }, 900000), "Native run did not formally publish; inspect application diagnostics");
      const auto finished = steadyNow();
      const auto* slot = currentDetectionSlot(*window.session().activeFile(), identity.selectedSampleChannel);
      DetectionRun completed;
      bool found = false;
      for (const auto& snapshot : panel->tasks().snapshots()) if (snapshot.request.runId == slot->committedRunId) { completed = snapshot; found = true; }
      require(found && completed.lifecycle == DetectionLifecycle::Completed && completed.completedWindows == completed.eligibleWindows, "Native formal run is incomplete");
      Project reopened;
      require(ProjectFileCodec::load(root.filePath("native-benchmark.ssproj"), reopened, error), error);
      const auto* durable = currentDetectionSlot(reopened.files.front(), identity.selectedSampleChannel);
      require(durable && durable->committedRunId == completed.request.runId, "Native committed slot did not reopen");
      QJsonObject row{{"pass",true},{"formalCommitPerformed",true},{"iteration",iteration},{"excludedWarmup",repeats > 0 && iteration == 0},
          {"clickSteadyNs",QString::number(click)},{"finishedSteadyNs",QString::number(finished)},
          {"clickToFormalMs",double(finished-click)/1e6},{"elapsedMs",double(finished-click)/1e6},{"run",encodeDetectionRun(completed)},
          {"formalProjectReopened",true}};
      const auto profile = QJsonDocument::fromJson(QByteArray::fromStdString(completed.performanceJson)).object();
      const auto workerPid = qint64(profile["worker"].toObject()["processId"].toDouble());
      require(workerPid > 0 && workers.insert(workerPid).second, "Each native run must have an observed independent Worker");
      for (const auto& value : profile["timeline"].toObject()["marks"].toArray()) {
        const auto mark = value.toObject();
        if (mark["name"] == "t_first_infer_start") {
          const auto infer = mark["steadyNs"].toString().toULongLong();
          require(infer >= click, "Inference clock precedes native click");
          row["clickToFirstInferenceMs"] = double(infer-click)/1e6;
        }
      }
      require(row.contains("clickToFirstInferenceMs"), "Native first-inference timestamp was not observed");
      rows.append(row);
      save(root.filePath(QString("run-%1.json").arg(iteration)), row);
      if (repeats == 0) save(root.filePath("measurement.json"), row);
    }
    QJsonArray measurements;
    for (int i = repeats > 0 ? 1 : 0; i < rows.size(); ++i)
      measurements.append(QJsonObject{{"pass",true},{"measurement",root.filePath(QString("run-%1.json").arg(i))}});
    const QJsonObject summary{{"pass",true},{"processId",double(QCoreApplication::applicationPid())},
        {"applicationResident",repeats > 0},{"independentWorkerPerRun",true},{"rows",rows},
        {"runs",measurements},
        {"screen",QJsonObject{{"connectedIndex",QGuiApplication::screens().indexOf(screen)+1},
            {"friendlyName",screen->name()},{"nativeDevicePath",displayDeviceName(screen)},
            {"physicalWidth",displayPixelSize(screen).width()},{"physicalHeight",displayPixelSize(screen).height()},
            {"dpr",screen->devicePixelRatio()},{"logicalWidth",window.width()},{"logicalHeight",window.height()}}},
        {"boundary","Actual native QWidget mouse click through formal commit/reopen; rendering and inference are distinct. OS cache not flushed; each Worker initializes its model/context."}};
    save(root.filePath("native-summary.json"), summary);
    if (repeats > 0) save(root.filePath("resident-summary.json"), summary);
    return 0;
  } catch (const std::exception& exception) {
    if (!output.isEmpty() && !QFileInfo::exists(QDir(output).filePath("native-failure.json")))
      save(QDir(output).filePath("native-failure.json"), {{"pass",false},{"error",QString::fromUtf8(exception.what())}});
    return 1;
  }
}
