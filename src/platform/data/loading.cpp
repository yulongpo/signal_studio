#include "signal_studio/data/loading.hpp"
#include "signal_studio/data/io.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace signal::data {
namespace {

core::Status data_error(core::ErrorReason reason, std::string message, std::string detail = {}) {
  return core::Status::failure({core::ErrorDomain::data, reason}, std::move(message), std::move(detail));
}

} // namespace

core::Result<LoadPlan> make_initial_load_plan(const std::filesystem::path& path, const SignalDescriptor& descriptor,
                                              std::uint64_t start_sample, std::uint64_t configured_initial_bytes,
                                              std::uint64_t configured_chunk_bytes) {
  if (configured_chunk_bytes == 0U) {
    return data_error(core::ErrorReason::invalid_argument, "Load chunk size must be positive");
  }
  BoundedFileReader reader{path, 1U};
  const auto file_size = reader.size();
  if (!file_size)
    return file_size.error();
  const auto facts = calculate_data_facts(file_size.value(), descriptor, configured_initial_bytes);
  if (!facts)
    return facts.error();
  if (start_sample < descriptor.requested_sample_range.begin() ||
      start_sample > descriptor.requested_sample_range.end() || start_sample > facts.value().available_frames) {
    return data_error(core::ErrorReason::invalid_argument, "Initial load start is outside confirmed frames");
  }
  const auto confirmed_end = std::min(facts.value().available_frames, descriptor.requested_sample_range.end());
  const auto remaining_frames = confirmed_end - start_sample;
  if (remaining_frames > std::numeric_limits<std::uint64_t>::max() / facts.value().frame_bytes) {
    return data_error(core::ErrorReason::invalid_argument, "Remaining load byte count overflows uint64");
  }
  const auto remaining_bytes = remaining_frames * facts.value().frame_bytes;
  const auto desired_bytes = std::min(configured_initial_bytes, remaining_bytes);
  const auto target_bytes = desired_bytes - desired_bytes % facts.value().frame_bytes;
  const auto target_frames = target_bytes / facts.value().frame_bytes;
  const auto range = SampleRange::from_count(start_sample, target_frames);
  if (!range)
    return range.error();
  auto chunk_bytes = std::min(configured_chunk_bytes, target_bytes == 0U ? facts.value().frame_bytes : target_bytes);
  chunk_bytes -= chunk_bytes % facts.value().frame_bytes;
  if (chunk_bytes == 0U)
    chunk_bytes = facts.value().frame_bytes;
  return LoadPlan{range.value(), target_bytes, facts.value().frame_bytes, chunk_bytes};
}

LoadedDataRange::LoadedDataRange(SampleRange range, SignalBuffer samples, std::string data_source_version_id)
    : range_(range), samples_(std::move(samples)), data_source_version_id_(std::move(data_source_version_id)) {
  if (range_.size() != samples_.size() || data_source_version_id_.empty()) {
    throw std::invalid_argument("LoadedDataRange invariants are invalid");
  }
}

IncrementalLoader::IncrementalLoader(std::string data_source_version_id, std::filesystem::path path,
                                     SignalDescriptor descriptor, LoadPlan plan)
    : data_source_version_id_(std::move(data_source_version_id)), path_(std::move(path)),
      descriptor_(std::move(descriptor)), plan_(plan), next_sample_(plan.requested_range.begin()) {}

core::Result<std::unique_ptr<IncrementalLoader>> IncrementalLoader::create(std::string data_source_version_id,
                                                                           std::filesystem::path path,
                                                                           SignalDescriptor descriptor, LoadPlan plan) {
  const auto validation = descriptor.validate();
  if (!validation)
    return validation;
  if (data_source_version_id.empty() || plan.frame_bytes == 0U || plan.chunk_bytes == 0U ||
      plan.chunk_bytes % plan.frame_bytes != 0U || !descriptor.requested_sample_range.contains(plan.requested_range)) {
    return data_error(core::ErrorReason::invalid_argument, "Incremental load configuration is invalid");
  }
  if (plan.requested_range.size() > std::numeric_limits<std::uint64_t>::max() / plan.frame_bytes ||
      plan.target_bytes != plan.requested_range.size() * plan.frame_bytes) {
    return data_error(core::ErrorReason::invalid_argument, "Incremental load size overflows uint64");
  }
  BoundedFileReader reader{path, 1U};
  const auto size = reader.size();
  if (!size)
    return size.error();
  return std::unique_ptr<IncrementalLoader>{
      new IncrementalLoader{std::move(data_source_version_id), std::move(path), std::move(descriptor), plan}};
}

core::Status IncrementalLoader::start() {
  std::lock_guard lock(mutex_);
  if (progress_state_ != LoadProgressState::not_started) {
    return data_error(core::ErrorReason::invalid_argument, "Only an unstarted load can begin reading");
  }
  progress_state_ = LoadProgressState::reading;
  source_state_ = DataSourceState::loading;
  logs_.push_back({1U, "Bounded source loading started"});
  if (plan_.requested_range.empty()) {
    publish(true);
    progress_state_ = LoadProgressState::complete;
    logs_.push_back({2U, "Bounded source loading completed"});
  }
  return core::Status::success();
}

core::Status IncrementalLoader::process_next() {
  std::lock_guard lock(mutex_);
  if (progress_state_ != LoadProgressState::reading) {
    return data_error(core::ErrorReason::invalid_argument, "Load processing requires active reading");
  }
  if (next_sample_ == plan_.requested_range.end()) {
    publish(true);
    progress_state_ = LoadProgressState::complete;
    return core::Status::success();
  }
  const auto chunk_frames = std::min(plan_.chunk_bytes / plan_.frame_bytes, plan_.requested_range.end() - next_sample_);
  const auto range = SampleRange::from_count(next_sample_, chunk_frames);
  if (!range) {
    fail(range.error());
    return range.error();
  }
  auto read = read_raw_samples(path_, descriptor_, range.value(), chunk_frames * plan_.frame_bytes);
  if (!read) {
    fail(read.error());
    return read.error();
  }
  const auto view = read.value().samples.view();
  if (descriptor_.signal_kind == SignalKind::real) {
    real_samples_.insert(real_samples_.end(), view.real_values().begin(), view.real_values().end());
  } else {
    complex_samples_.insert(complex_samples_.end(), view.complex_values().begin(), view.complex_values().end());
  }
  next_sample_ = range.value().end();
  bytes_read_ += read.value().bytes_read;
  logs_.push_back({static_cast<std::uint64_t>(logs_.size() + 1U), "A complete frame-aligned chunk was read"});
  if (next_sample_ == plan_.requested_range.end()) {
    publish(true);
    progress_state_ = LoadProgressState::complete;
    logs_.push_back({static_cast<std::uint64_t>(logs_.size() + 1U), "Bounded source loading completed"});
  }
  return core::Status::success();
}

core::Status IncrementalLoader::cancel() {
  std::lock_guard lock(mutex_);
  if (progress_state_ == LoadProgressState::cancelled)
    return core::Status::success();
  if (progress_state_ != LoadProgressState::not_started && progress_state_ != LoadProgressState::reading) {
    return data_error(core::ErrorReason::invalid_argument, "This loading progress cannot be cancelled");
  }
  progress_state_ = LoadProgressState::cancellation_requested;
  publish(false);
  progress_state_ = LoadProgressState::cancelled;
  logs_.push_back(
      {static_cast<std::uint64_t>(logs_.size() + 1U), "Loading cancelled at the last complete sample frame"});
  return core::Status::success();
}

void IncrementalLoader::publish(bool complete) {
  const auto loaded = SampleRange::make(plan_.requested_range.begin(), next_sample_);
  if (!loaded) {
    fail(loaded.error());
    return;
  }
  auto samples = descriptor_.signal_kind == SignalKind::real ? SignalBuffer::from_real(real_samples_)
                                                             : SignalBuffer::from_complex(complex_samples_);
  published_range_ =
      std::make_shared<const LoadedDataRange>(loaded.value(), std::move(samples), data_source_version_id_);
  source_state_ =
      complete ? DataSourceState::complete
               : (loaded.value().empty() ? DataSourceState::unavailable : DataSourceState::partial_read_available);
}

void IncrementalLoader::fail(core::Status error) {
  progress_state_ = LoadProgressState::failed;
  source_state_ = DataSourceState::unavailable;
  const auto log_uri = "data-load://" + data_source_version_id_ + "/log";
  failure_ = ImportFailure{std::move(error),
                           {ImportRecoveryAction::retry, ImportRecoveryAction::edit_parameters,
                            ImportRecoveryAction::cancel, ImportRecoveryAction::view_log},
                           log_uri};
  logs_.push_back(
      {static_cast<std::uint64_t>(logs_.size() + 1U), "Loading failed; no unvalidated data source was published"});
  published_range_.reset();
  real_samples_.clear();
  complex_samples_.clear();
}

LoadSnapshot IncrementalLoader::snapshot() const {
  std::lock_guard lock(mutex_);
  const double progress =
      plan_.target_bytes == 0U ? 1.0 : static_cast<double>(bytes_read_) / static_cast<double>(plan_.target_bytes);
  return LoadSnapshot{progress_state_, source_state_, plan_.requested_range, next_sample_, bytes_read_, progress,
                      failure_,        logs_,         published_range_};
}

} // namespace signal::data
