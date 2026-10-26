#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <QTextStream>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include "infrastructure/detection_model_snapshot.h"
#include "infrastructure/detection_refined_publication.h"
#include "infrastructure/detection_store.h"
#include "infrastructure/detection_task.h"

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

using namespace signalstudio;
namespace {
void require(bool okay, const QString& error) {
  if (!okay) throw std::runtime_error(error.toUtf8().toStdString());
}
void save(const QString& path, const QJsonObject& object) {
  QSaveFile file(path);
  require(file.open(QIODevice::WriteOnly), file.errorString());
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
  require(file.write(bytes) == bytes.size() && file.commit(), file.errorString());
}
QJsonObject memory() {
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX usage{};
  usage.cb = sizeof(usage);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&usage),sizeof(usage)))
    return {{"rssBytes",double(usage.WorkingSetSize)},
        {"peakRssBytes",double(usage.PeakWorkingSetSize)},
        {"privateBytes",double(usage.PrivateUsage)}};
#endif
  return {{"available",false}};
}
}

int main(int argc, char** argv) {
  QCoreApplication app(argc,argv);
  QCommandLineParser args;
  args.setApplicationDescription("Isolated real detection pipeline benchmark; never edits the source project");
  args.addHelpOption();
  args.addOptions({{{"r","request"},"Existing run manifest or encoded request","json"},
      {{"o","output"},"New/empty output directory","directory"},
      {"strategy","Override sample/roi/coarse/none","name"},
      {"batch","Override requested batch","count"},
      {"fft","Override analysis FFT size","bins"},
      {"hop","Override frame interval in seconds","seconds"},
      {"limit-seconds","Restrict the requested range duration","seconds"},
      {"raw-head-budget-mib","Explicit independent raw-head disk quota; zero means legacy combined","MiB"},
      {"cancel-windows","Cancel after at least this many durable windows","count"},
      {"timeout","Cancel at this wall-clock deadline","seconds","900"}});
  args.process(app);
  QJsonObject report{{"schema","signal-studio-detection-benchmark-v1"},
      {"pass",false},{"nativeUiVerified",false},
      {"scope","production headless pipeline; publication timing is not UI paint latency"},
      {"binary",QCoreApplication::applicationFilePath()},
      {"startedUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
  QString root;
  QElapsedTimer timer;
  timer.start();
  try {
    require(args.isSet("request") && args.isSet("output"),"--request and --output are required");
    root = QFileInfo(args.value("output")).absoluteFilePath();
    QDir directory(root);
    require(!directory.exists() || directory.entryList(QDir::AllEntries|QDir::NoDotAndDotDot).isEmpty(),
        "Output must be new or empty; evidence cannot be overwritten");
    require(directory.mkpath("."),"Cannot create benchmark output");
    QFile input(args.value("request"));
    require(input.open(QIODevice::ReadOnly) && input.size() <= 8*1024*1024,"Cannot read bounded request JSON");
    QJsonParseError parse;
    auto object = QJsonDocument::fromJson(input.readAll(),&parse).object();
    require(parse.error == QJsonParseError::NoError,"Invalid request JSON");
    if (object.contains("run")) object = object["run"].toObject();
    if (object.contains("request")) object = object["request"].toObject();
    DetectionRequest request;
    QString error;
    require(decodeDetectionRequest(object,request,error),error);
    report["originalRequest"] = object;
    request.runId = "ux-r6-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    request.parentRunId.reset();
    if (args.isSet("strategy")) {
      const auto strategy = args.value("strategy");
      require(strategy=="sample" || strategy=="roi" || strategy=="coarse" || strategy=="none","Unknown strategy");
      request.parameters.timeStrategy = strategy=="sample" ? DetectionTimeStrategy::Sample
          : strategy=="roi" ? DetectionTimeStrategy::Roi
          : strategy=="coarse" ? DetectionTimeStrategy::Coarse : DetectionTimeStrategy::None;
    }
    if (args.isSet("batch")) request.parameters.batchSize = args.value("batch").toUInt();
    if (args.isSet("fft")) request.profile.fftSize = args.value("fft").toUInt();
    if (args.isSet("hop")) request.profile.hopSeconds = args.value("hop").toDouble();
    if (args.isSet("raw-head-budget-mib")) {
      bool valid=false;
      const auto mib=args.value("raw-head-budget-mib").toULongLong(&valid);
      require(valid && mib<=2048,"Raw-head quota must be 0..2048 MiB");
      request.rawHeadBudgetBytes=mib*1024*1024;
    }
    if (args.isSet("limit-seconds")) {
      const double seconds = args.value("limit-seconds").toDouble();
      require(std::isfinite(seconds) && seconds > 0,"Invalid range duration");
      const auto count = static_cast<SampleIndex>(std::min<long double>(
          request.requestedRange.time.end-request.requestedRange.time.begin,
          seconds*request.sourceIdentity.sampleRateHz));
      require(count > 0,"Duration contains no samples");
      request.requestedRange.time.end = request.requestedRange.time.begin+count;
    }
    const auto& id = request.sourceIdentity;
    FileMetadata metadata;
    metadata.id=id.fileId; metadata.path=id.path;
    metadata.name=QFileInfo(QString::fromStdString(id.path)).fileName().toStdString();
    metadata.sampleRateHz=id.sampleRateHz; metadata.centerFrequencyHz=id.centerFrequencyHz;
    metadata.sampleFormat=id.sampleFormat; metadata.sampleCount=id.physicalSamples; metadata.demo=false;
    metadata.effectiveBandwidthHz=id.effectiveFrequency.upperHz-id.effectiveFrequency.lowerHz;
    metadata.availability.availableSamples=id.availableSamples;
    metadata.availability.status=id.availableSamples==id.physicalSamples ? LoadStatus::Ready : LoadStatus::Partial;
    metadata.availability.fingerprint=id.quickFingerprint;
    require(freezeDetectionModelSnapshot(request,error),error);
    report["request"] = encodeDetectionRequest(request);
    report["memoryBefore"] = memory();
    report["processId"] = double(QCoreApplication::applicationPid());
    save(directory.filePath("measurement.json"),report);
    QFile trace(directory.filePath("progress.jsonl"));
    require(trace.open(QIODevice::WriteOnly|QIODevice::NewOnly),trace.errorString());
    DetectionTaskManager manager(QCoreApplication::applicationDirPath()+
#ifdef Q_OS_WIN
        "/SignalStudioDetectionWorker.exe"
#else
        "/SignalStudioDetectionWorker"
#endif
    );
    require(manager.enqueue(root,metadata,request,error),error);
    const auto deadline = args.value("timeout").toDouble()*1000;
    require(std::isfinite(deadline) && deadline > 0,"Invalid timeout");
    const auto cancelAt = args.value("cancel-windows").toULongLong();
    bool cancellationSent=false, timedOut=false;
    DetectionRun terminal;
    QString lastState;
    while (true) {
      manager.pump();
      const auto runs=manager.snapshots();
      require(!runs.empty(),"Enqueued run disappeared");
      const auto& run=runs.front();
      const auto state=QString::fromStdString(run.computationPhase)+":"+
          QString::number(run.completedWindows)+":"+QString::number(run.refinedCandidates);
      if(state!=lastState) {
        auto record=QJsonObject{{"elapsedMs",double(timer.elapsed())},
            {"phase",QString::fromStdString(run.computationPhase)},
            {"completedWindows",QString::number(run.completedWindows)},
            {"eligibleWindows",QString::number(run.eligibleWindows)},
            {"refined",QString::number(run.refinedCandidates)}, {"memory",memory()}};
        trace.write(QJsonDocument(record).toJson(QJsonDocument::Compact)+"\n");
        trace.flush();
        lastState=state;
      }
      if(detectionIsTerminal(run.lifecycle)) {terminal=run;break;}
      if(!cancellationSent && ((cancelAt && run.completedWindows>=cancelAt) || timer.elapsed()>deadline)) {
        timedOut=timer.elapsed()>deadline;
        cancellationSent=manager.cancel(request.runId);
      }
      QCoreApplication::processEvents();
      QThread::msleep(10);
    }
    const auto publicationMs=timer.elapsed();
    DetectionPublication publication;
    const bool available=manager.takePublication(request.runId,publication);
    report["run"]=encodeDetectionRun(terminal);
    report["publicationAvailable"]=available;
    report["annotationCount"]=double(publication.annotations.size());
    std::size_t segments=0;
    for(const auto& annotation:publication.annotations) segments+=annotation.segments.size();
    report["segmentCount"]=double(segments);
    report["elapsedMs"]=double(publicationMs);
    report["memoryAfter"]=memory();
    report["timedOut"]=timedOut;
    report["cancellationSent"]=cancellationSent;
    report["windowsPerSecond"]=1000.0*terminal.completedWindows/std::max<qint64>(1,publicationMs);
    if(available) save(directory.filePath("publication.json"),{{"records",encodeDetectionPublication(publication)}});
    quint64 stored=0;
    QDirIterator files(root,QDir::Files,QDirIterator::Subdirectories);
    while(files.hasNext()) {files.next();stored+=files.fileInfo().size();}
    report["storedBytesBeforeFinalReport"]=QString::number(stored);
    report["pass"]=terminal.lifecycle==DetectionLifecycle::Completed && available && !timedOut;
    save(directory.filePath("measurement.json"),report);
    QTextStream(stdout)<<QJsonDocument(report).toJson(QJsonDocument::Compact)<<Qt::endl;
    return report["pass"].toBool() ? 0 : 1;
  } catch(const std::exception& error) {
    report["error"]=QString::fromUtf8(error.what());report["elapsedMs"]=double(timer.elapsed());
    if(!root.isEmpty()) save(QDir(root).filePath("measurement.json"),report);
    std::cerr<<error.what()<<'\n';return 1;
  }
}
