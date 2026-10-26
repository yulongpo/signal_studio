#include "infrastructure/detection_worker_client.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>

#include "infrastructure/detection_store.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace signalstudio {

DetectionWorkerClient::~DetectionWorkerClient() { stop(); }

void DetectionWorkerClient::stop(bool graceful) {
  sampleResources();
  if (process_.state() != QProcess::NotRunning) {
    gracefulRequested_=graceful;
    if(graceful) {
      process_.closeWriteChannel();
      process_.waitForFinished(250);
    }
    if(process_.state()!=QProcess::NotRunning) {
      terminationRequested_=true;
      process_.kill();
      process_.waitForFinished(1000);
    }
  }
  stderrTail_=(stderrTail_+process_.readAllStandardError()).right(8192);
}
void DetectionWorkerClient::sampleResources() {
#ifdef Q_OS_WIN
  if(!processId_) return;
  const auto handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,DWORD(processId_));
  if(!handle) return;
  PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
  if(GetProcessMemoryInfo(handle,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
    resource_={{"available",true},{"peakRssBytes",double(memory.PeakWorkingSetSize)},
        {"rssBytes",double(memory.WorkingSetSize)},{"privateBytes",double(memory.PrivateUsage)}};
  FILETIME created{},exited{},kernel{},user{};
  if(GetProcessTimes(handle,&created,&exited,&kernel,&user)) {
    const auto ticks=[](FILETIME time){return (quint64(time.dwHighDateTime)<<32)|time.dwLowDateTime;};
    resource_["cpuMilliseconds"]=double(ticks(kernel)+ticks(user))/10000;
  }
  CloseHandle(handle);
#endif
}
QJsonObject DetectionWorkerClient::diagnostics() const {
  QJsonObject result{{"processId",double(processId_)},
      {"state",int(process_.state())},{"exitObserved",processId_!=0 && process_.state()==QProcess::NotRunning},
      {"stderrTail",QString::fromUtf8(stderrTail_)},{"bytesRead",QString::number(bytesRead_)},
      {"bytesWritten",QString::number(bytesWritten_)},{"exchanges",QString::number(sequence_)},
      {"terminationRequested",terminationRequested_},{"gracefulRequested",gracefulRequested_},
      {"resource",resource_}};
  if(processId_ && process_.state()==QProcess::NotRunning) {
    result["exitCode"]=process_.exitCode();
    result["exitStatus"]=process_.exitStatus()==QProcess::NormalExit ? "normal" : "crash";
  }
  return result;
}

bool DetectionWorkerClient::writePacket(const DetectionWorkerPacket& packet,
                                        QString& error) {
  const auto bytes = encodeWorkerPacket(packet, error);
  if (bytes.isEmpty()) return false;
  if (process_.write(bytes) != bytes.size()) {
    error = "Worker input pipe rejected the bounded packet";
    return false;
  }
  bytesWritten_+=bytes.size();
  return true;
}

bool DetectionWorkerClient::readPacket(DetectionWorkerPacket& packet,
                                       QString& error,
                                       const std::function<bool()>& cancelled) {
  QElapsedTimer timeout;
  timeout.start();
  while (timeout.elapsed() < 45000) {
    if (cancelled()) {
      error = "Cancelled at worker boundary";
      stop();
      return false;
    }
    std::optional<DetectionWorkerPacket> available;
    if (!decoder_.take(available, error)) return false;
    if (available) {
      sampleResources();
      packet = std::move(*available);
      return true;
    }
    process_.waitForReadyRead(20);
    const auto bytes = process_.readAllStandardOutput();
    bytesRead_+=bytes.size();
    // Diagnostics are separate from the binary wire protocol and bounded.
    stderrTail_=(stderrTail_+process_.readAllStandardError()).right(8192);
    if (!bytes.isEmpty() && !decoder_.append(bytes, error)) return false;
    if (process_.state() == QProcess::NotRunning && bytes.isEmpty()) {
      error = QString("Worker exited before a complete response (code %1, %2): %3")
          .arg(process_.exitCode()).arg(process_.exitStatus()==QProcess::NormalExit ? "normal" : "crash")
          .arg(QString::fromUtf8(stderrTail_));
      return false;
    }
  }
  error = "Worker response timed out";
  stop();
  return false;
}

bool DetectionWorkerClient::start(const QString& executable,
                                  const DetectionRequest& request,
                                  QString& error,
                                  const std::function<bool()>& cancelled,
                                  std::uint32_t maximumExecutionBatch) {
  if (!QFileInfo(executable).isFile()) {
    error = "Detection worker executable is missing";
    return false;
  }
  request_ = request;
  process_.setProgram(executable);
  process_.setArguments(
      {"--maximum-execution-batch",
       QString::number(maximumExecutionBatch ? maximumExecutionBatch
                                             : request.parameters.batchSize)});
  process_.setProcessChannelMode(QProcess::SeparateChannels);
  process_.start();
  if (!process_.waitForStarted(3000)) {
    error = "Detection worker could not start: " + process_.errorString();
    return false;
  }
  processId_=process_.processId();
  DetectionWorkerPacket hello{
      workerEnvelope("hello", "handshake", 0, {{"client", "SignalStudio"}}),
      {}};
  DetectionWorkerPacket ready;
  if (!writePacket(hello, error) || !readPacket(ready, error, cancelled))
    return false;
  if (ready.envelope["type"] != "ready") {
    error = "Worker protocol handshake failed";
    return false;
  }
  DetectionWorkerPacket begin{
      workerEnvelope("begin", "begin", 0, encodeDetectionRequest(request),
                     QString::fromStdString(request.runId),
                     request.sourceIdentity.projectGeneration,
                     QString::fromStdString(request.sourceIdentity.fileId)),
      {}};
  if (!exchange(std::move(begin), ready, error, cancelled)) return false;
  evidence_ = ready.envelope["payload"].toObject();
  return true;
}

bool DetectionWorkerClient::exchange(DetectionWorkerPacket packet,
                                     DetectionWorkerPacket& response,
                                     QString& error,
                                     const std::function<bool()>& cancelled) {
  packet.envelope["sequence"] = QString::number(++sequence_);
  if (!writePacket(packet, error) || !readPacket(response, error, cancelled))
    return false;
  const auto& envelope = response.envelope;
  if (envelope["requestId"] != packet.envelope["requestId"] ||
      envelope["sequence"] != packet.envelope["sequence"] ||
      envelope["runId"] != packet.envelope["runId"] ||
      envelope["fileId"] != packet.envelope["fileId"] ||
      envelope["projectGeneration"] != packet.envelope["projectGeneration"]) {
    error = "Worker response correlation does not match the request";
    return false;
  }
  if (envelope["type"] == "error") {
    error = envelope["payload"].toObject()["message"].toString(
        "Worker rejected the request");
    return false;
  }
  const auto expected = packet.envelope["type"] == "batch"       ? "result"
                        : packet.envelope["type"] == "completed" ? "completed"
                                                                 : "ready";
  if (envelope["type"] != expected) {
    error = "Unexpected worker response stage";
    return false;
  }
  return true;
}

}  // namespace signalstudio
