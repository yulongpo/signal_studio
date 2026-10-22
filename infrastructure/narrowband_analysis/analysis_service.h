#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include "infrastructure/narrowband_analysis/constellation_analysis.h"
namespace signalstudio {
// One bounded worker and one replaceable pending request; no GUI callbacks and
// no references to mutable Session state are retained by the worker.
class NarrowbandAnalysisService {
 public:
  explicit NarrowbandAnalysisService(ChannelSampleCache&);
  ~NarrowbandAnalysisService();
  void submit(FileMetadata,Channel,AnalysisRequest,NarrowbandAnalysisSettings);
  void cancel();
  std::shared_ptr<const NarrowbandAnalysisResult> takeResult();
  bool busy() const;
 private:
  struct Work { FileMetadata source;Channel channel;AnalysisRequest request;NarrowbandAnalysisSettings settings; };
  void run();
  ChannelSampleCache& cache_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Work> pending_;
  std::shared_ptr<std::atomic_bool> cancellation_;
  std::shared_ptr<const NarrowbandAnalysisResult> completed_;
  bool stopping_=false,active_=false;
  std::thread worker_;
};
}  // namespace signalstudio
