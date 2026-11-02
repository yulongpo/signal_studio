#include "infrastructure/runtime_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

namespace signalstudio {
namespace {
QMutex& logMutex() {
  static QMutex mutex;
  return mutex;
}
struct LogState {
  bool active = false;
  bool handlerInstalled = false;
  QString path;
  qint64 rotateBytes = RuntimeLog::kDefaultRotateBytes;
  QObject* mirrorTarget = nullptr;
  std::function<void(const QString&)> mirror;
  QtMessageHandler previousHandler = nullptr;
};
LogState& state() {
  static LogState value;
  return value;
}
QString formatLine(const QString& category, const QString& message) {
  return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz") +
         " [" + category + "] " + message;
}
void rotateUnlocked(LogState& log) {
  QFile::remove(log.path + ".3");
  QFile::rename(log.path + ".2", log.path + ".3");
  QFile::rename(log.path + ".1", log.path + ".2");
  QFile::rename(log.path, log.path + ".1");
}
void writeUnlocked(LogState& log, const QString& category, const QString& message,
                   QString* formatted) {
  if (!log.active || log.path.isEmpty()) return;
  const auto line = formatLine(category, message);
  const auto bytes = line.toUtf8();
  const auto incoming = static_cast<qint64>(bytes.size()) + 1;
  if (QFileInfo::exists(log.path) &&
      QFileInfo(log.path).size() + incoming > log.rotateBytes)
    rotateUnlocked(log);
  const bool fresh = !QFileInfo::exists(log.path) || QFileInfo(log.path).size() == 0;
  QFile file(log.path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
  QTextStream stream(&file);
  if (fresh && category != QLatin1String("runtime-log"))
    stream << formatLine("runtime-log", "path=" + log.path) << '\n';
  stream << line << '\n';
  stream.flush();
  if (formatted) *formatted = line;
}
void qtMessageHandler(QtMsgType type, const QMessageLogContext& context,
                      const QString& message) {
  if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
    thread_local bool inside = false;
    if (!inside) {
      inside = true;
      RuntimeLog::write(type == QtWarningMsg ? QStringLiteral("qt-warning")
                                             : QStringLiteral("qt-critical"),
                        message);
      inside = false;
    }
  }
  const auto previous = state().previousHandler;
  if (previous) previous(type, context, message);
}
}  // namespace

QString RuntimeLog::install(const QString& path, qint64 rotateBytes) {
  QString target = path;
  if (target.isEmpty()) {
    const auto root =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    target = QDir(root).filePath(QStringLiteral("logs/signal-studio.log"));
  }
  QDir().mkpath(QFileInfo(target).absolutePath());
  target = QFileInfo(target).absoluteFilePath();
  QString first;
  {
    QMutexLocker lock(&logMutex());
    auto& log = state();
    log.path = target;
    log.rotateBytes = rotateBytes > 0 ? rotateBytes : kDefaultRotateBytes;
    log.active = true;
    if (!log.handlerInstalled) {
      log.previousHandler = qInstallMessageHandler(qtMessageHandler);
      log.handlerInstalled = true;
    }
    const bool fresh = !QFileInfo::exists(log.path) || QFileInfo(log.path).size() == 0;
    if (fresh) writeUnlocked(log, "runtime-log", "path=" + log.path, &first);
  }
  return target;
}
void RuntimeLog::uninstall() {
  QMutexLocker lock(&logMutex());
  state().active = false;
}
bool RuntimeLog::installed() {
  QMutexLocker lock(&logMutex());
  return state().active;
}
QString RuntimeLog::filePath() {
  QMutexLocker lock(&logMutex());
  return state().path;
}
void RuntimeLog::write(const QString& category, const QString& message) {
  QString line;
  QObject* target = nullptr;
  std::function<void(const QString&)> mirror;
  {
    QMutexLocker lock(&logMutex());
    auto& log = state();
    if (!log.active) return;
    writeUnlocked(log, category, message, &line);
    target = log.mirrorTarget;
    mirror = log.mirror;
  }
  if (!target || !mirror || line.isEmpty()) return;
  QMetaObject::invokeMethod(
      target, [mirror, line] { mirror(line); }, Qt::QueuedConnection);
}
void RuntimeLog::setMirror(QObject* target,
                           std::function<void(const QString& line)> mirror) {
  QMutexLocker lock(&logMutex());
  state().mirrorTarget = target;
  state().mirror = std::move(mirror);
}
QStringList RuntimeLog::recentLines(int maximumLines) {
  QString path;
  {
    QMutexLocker lock(&logMutex());
    path = state().path;
  }
  QStringList lines;
  if (path.isEmpty() || maximumLines <= 0) return lines;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return lines;
  constexpr qint64 kTailBytes = 256 * 1024;
  const bool partialTail = file.size() > kTailBytes;
  if (partialTail) file.seek(file.size() - kTailBytes);
  QString text = QString::fromUtf8(file.readAll());
  if (partialTail) {
    const auto split = text.indexOf('\n');
    if (split >= 0) text.remove(0, split + 1);
  }
  lines = text.split('\n', Qt::SkipEmptyParts);
  if (lines.size() > maximumLines)
    lines.erase(lines.begin(), lines.end() - maximumLines);
  return lines;
}
}  // namespace signalstudio
