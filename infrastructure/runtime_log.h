#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>

namespace signalstudio {

// Thread-safe rotating log shared by the GUI, detection tasks, and benchmarks.
// The file is the full record. A mirror receives the same formatted lines.
class RuntimeLog {
 public:
  static constexpr qint64 kDefaultRotateBytes = 8 * 1024 * 1024;
  static constexpr int kRetainedRotations = 3;

  // Empty path uses the application local data directory logs/signal-studio.log.
  // The first line of a new file records that absolute path.
  static QString install(const QString& path = {},
                         qint64 rotateBytes = kDefaultRotateBytes);
  static void uninstall();
  static bool installed();
  static QString filePath();
  static void write(const QString& category, const QString& message);
  static void setMirror(QObject* target, std::function<void(const QString& line)> mirror);
  static QStringList recentLines(int maximumLines);

 private:
  RuntimeLog() = default;
};

}  // namespace signalstudio
