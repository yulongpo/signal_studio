#pragma once

#include <QDialog>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include "infrastructure/channel_processor.h"

namespace signalstudio {
// The GUI timer owns the job. Closing the dialog requests cancellation before
// destroying it; the worker never accesses Session, widgets or Qt event queues.
inline void validateChannelDspAsync(QDialog* dialog, FileMetadata source, Channel channel,
                                   std::function<void(bool, QString, QString)> completed) {
  struct State {
    std::atomic_bool cancelled{false}, done{false};
    bool ok = false;
    QString error, field;
  };
  struct Job {
    std::shared_ptr<State> state=std::make_shared<State>();
    std::thread thread;
    ~Job() { state->cancelled=true; if(thread.joinable()) thread.join(); }
  };
  auto job=std::make_shared<Job>();
  auto state=job->state;
  job->thread=std::thread([state, source=std::move(source), channel=std::move(channel)] {
    try {
      ChannelDspPlan plan;
      state->ok=makeChannelDspPlan(source,channel,plan,state->error,&state->field,[state]{return state->cancelled.load();});
    } catch (const std::exception& e) { state->error=QString::fromUtf8(e.what()); }
    state->done.store(true,std::memory_order_release);
  });
  auto* timer=new QTimer(dialog);
  QObject::connect(dialog,&QDialog::finished,timer,[state]{state->cancelled=true;});
  QObject::connect(timer,&QTimer::timeout,timer,[job,timer,completed=std::move(completed)] {
    if(!job->state->done.load(std::memory_order_acquire)) return;
    timer->stop();
    if(job->thread.joinable())job->thread.join();
    if(!job->state->cancelled)completed(job->state->ok,job->state->error,job->state->field);
    timer->deleteLater();
  });
  timer->start(15);
}
}  // namespace signalstudio
