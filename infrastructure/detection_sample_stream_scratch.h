#pragma once
// Private implementation detail. No persistent authority, no mmap/hash.
#include "infrastructure/detection_sample_stream.h"
#include "infrastructure/detection_sample_stream_scratch_recovery.h"
#include <QFile>
#include <QTemporaryDir>
#include <memory>

namespace signalstudio::sample_stream::detail {
bool fail(Failure&, ErrorCode, const char* stage, QString detail);
bool cancelled(const Context&, Failure&, const char* stage);
class Scratch {
 public:
  Scratch(const Runtime&, Statistics&);
  ~Scratch();
  bool open(const Context&, const Config&, const Plan&, Failure&);
  bool append(QFile&, std::span<const float>, Failure&);
  bool seal(QFile&, Failure&);
  bool read(QFile&, SampleIndex first, std::span<float>, Failure&);
  bool removeEnergy(Failure&);
  bool close(Failure&); // explicit before any successful return or Legacy retry
  QFile energy, smooth;
 private:
  const Runtime& runtime_;
  Statistics& statistics_;
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<scratch_recovery::OwnerGuard> owner_;
  std::uint64_t reservation_ = 0, liveBytes_ = 0;
  SampleIndex count_ = 0;
  bool leased_ = false;
  QString ownedPath_;
};
bool exactQuantiles(Scratch&, const Context&, const Plan&,
                    double& noise, double& burst, Failure&,
                    gpu_dsp::Session* gpu = nullptr);
} // namespace signalstudio::sample_stream::detail
