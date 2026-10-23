#include "infrastructure/narrowband_analysis/analysis_service.h"
#include <utility>
namespace signalstudio {
NarrowbandAnalysisService::NarrowbandAnalysisService(ChannelSampleCache& cache):cache_(cache),worker_([this]{run();}) {}
NarrowbandAnalysisService::~NarrowbandAnalysisService() {
  {std::lock_guard lock(mutex_);stopping_=true;pending_.reset();if(cancellation_)cancellation_->store(true);}
  changed_.notify_one();if(worker_.joinable())worker_.join();
}
void NarrowbandAnalysisService::submit(FileMetadata source,Channel channel,AnalysisRequest request,NarrowbandAnalysisSettings settings) {
  {std::lock_guard lock(mutex_);if(cancellation_)cancellation_->store(true);completed_.reset();
   pending_=Work{std::move(source),std::move(channel),std::move(request),settings};}
  changed_.notify_one();
}
void NarrowbandAnalysisService::cancel() {
  std::lock_guard lock(mutex_);pending_.reset();completed_.reset();if(cancellation_)cancellation_->store(true);
}
std::shared_ptr<const NarrowbandAnalysisResult> NarrowbandAnalysisService::takeResult() {
  std::lock_guard lock(mutex_);return std::exchange(completed_,{});
}
bool NarrowbandAnalysisService::busy() const {std::lock_guard lock(mutex_);return active_ || pending_.has_value();}
void NarrowbandAnalysisService::run() {
  for(;;) {
    std::unique_lock lock(mutex_);changed_.wait(lock,[this]{return stopping_ || pending_.has_value();});
    if(stopping_)return;
    auto work=std::move(*pending_);pending_.reset();active_=true;
    auto stop=std::make_shared<std::atomic_bool>(false);cancellation_=stop;lock.unlock();
    auto result=std::make_shared<NarrowbandAnalysisResult>();result->identity=work.request.identity;
    try {
      auto source=std::make_shared<AnalysisSourceData>(readAnalysisSource(cache_,work.source,work.channel,work.request,[stop]{return stop->load();}));
      *result=analyzeNarrowband(std::move(source),work.settings,[stop]{return stop->load();});
    } catch(const std::exception& e) {result->status=AnalysisStatus::Failed;result->message=e.what();}
    lock.lock();active_=false;
    if(!stop->load() && !stopping_ && !pending_)completed_=std::move(result);
  }
}
}  // namespace signalstudio
