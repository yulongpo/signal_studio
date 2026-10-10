#pragma once
#include "domain/project.h"
#include <QString>
#include <atomic>
#include <mutex>
#include <thread>

namespace signalstudio {
struct SourceLoadSnapshot {
    SampleIndex loaded = 0, target = 0;
    bool finished = false;
    QString error, fingerprint;
    std::vector<EnvelopePoint> envelope;
};
QString iqSourceFingerprint(const QString& path);
class SourceLoader {
public:
    SourceLoader(QString path, SampleIndex target);
    ~SourceLoader();
    void stop() { stop_ = true; }
    SourceLoadSnapshot snapshot() const;
private:
    void run(QString path, SampleIndex target);
    std::atomic_bool stop_ = false;
    mutable std::mutex mutex_;
    SourceLoadSnapshot snapshot_;
    std::thread worker_;
};
}
