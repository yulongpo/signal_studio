#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <array>
#include <mutex>

namespace signalstudio {
// Fixed key/count bound; exact totals/maxima, with no per-window history or
// unbounded event queue. End-to-end percentiles are measured by repeated runs.
class DetectionProfiler {
 public:
  void record(const QString& name, qint64 nanos) {
    auto end = entries_.begin()+count_;
    auto it = std::find_if(entries_.begin(),end,[&](const auto& e){return e.name==name;});
    if(it==end) {
      if(count_==entries_.size()) return;
      it=entries_.begin()+count_++;it->name=name.left(64);
    }
    ++it->calls;it->total+=std::max<qint64>(0,nanos);
    it->maximum=std::max(it->maximum,nanos);
  }
  QJsonObject snapshot() const {
    QJsonObject object;
    for(std::size_t i=0;i<count_;++i) {
      const auto& e=entries_[i];
      object[e.name]=QJsonObject{{"calls",QString::number(e.calls)},
          {"totalMs",e.total/1e6},{"maximumMs",e.maximum/1e6},
          {"meanMs",e.total/1e6/std::max<quint64>(1,e.calls)}};
    }
    return object;
  }
 private:
  struct Entry {QString name;quint64 calls=0;qint64 total=0,maximum=0;};
  std::array<Entry,32> entries_{};
  std::size_t count_=0;
};
class DetectionProfileScope {
 public:
  DetectionProfileScope(DetectionProfiler& target,QString name,std::mutex* mutex=nullptr)
      : target_(target),name_(std::move(name)),mutex_(mutex) {timer_.start();}
  ~DetectionProfileScope() {
    const auto nanos=timer_.nsecsElapsed();
    if(mutex_) {const std::lock_guard lock(*mutex_);target_.record(name_,nanos);}
    else target_.record(name_,nanos);
  }
 private:
  DetectionProfiler& target_;QString name_;std::mutex* mutex_;QElapsedTimer timer_;
};
}
