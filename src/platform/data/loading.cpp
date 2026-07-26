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
  auto loader = std::unique_ptr<IncrementalLoader>{
      new IncrementalLoader{std::move(data_source_version_id), std::move(path), std::move(descriptor), plan}};
  if (plan.requested_range.empty()) {
    loader->publish(true);
  }
  return loader;
}

core::Status IncrementalLoader::process_next() {
  std::lock_guard lock(mutex_);
  if (!publication_allowed_) {
    return data_error(core::ErrorReason::cancelled, "Cancelled import attempts cannot process more data");
  }
  if (error_) {
    return data_error(core::ErrorReason::invalid_argument, "Failed import attempts cannot be restarted");
  }
  if (next_sample_ == plan_.requested_range.end()) {
    return core::Status::success();
  }
  source_state_ = DataSourceState::loading;
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
  if (next_sample_ == plan_.requested_range.end()) {
    publish(true);
  }
  return core::Status::success();
}

core::Status IncrementalLoader::cancel_import() {
  std::lock_guard lock(mutex_);
  if (!publication_allowed_) {
    return core::Status::success();
  }
  if (source_state_ == DataSourceState::complete) {
    return data_error(core::ErrorReason::invalid_argument, "Completed imports cannot be cancelled");
  }
  publication_allowed_ = false;
  if (!error_ && next_sample_ > plan_.requested_range.begin()) {
    publish(false);
  } else {
    source_state_ = DataSourceState::unavailable;
    published_range_.reset();
  }
  return core::Status::success();
}

core::Result<std::unique_ptr<IncrementalLoader>> IncrementalLoader::retry() const {
  std::string version;
  std::filesystem::path path;
  SignalDescriptor descriptor;
  LoadPlan plan;
  {
    std::lock_guard lock(mutex_);
    if (!publication_allowed_ || !error_ || !error_->retryable) {
      return data_error(core::ErrorReason::invalid_argument, "Only retryable failed imports can be retried");
    }
    version = data_source_version_id_;
    path = path_;
    descriptor = descriptor_;
    plan = plan_;
  }
  return create(std::move(version), std::move(path), std::move(descriptor), plan);
}

core::Result<std::unique_ptr<IncrementalLoader>> IncrementalLoader::edit_parameters(std::string data_source_version_id,
                                                                                    std::filesystem::path path,
                                                                                    SignalDescriptor descriptor,
                                                                                    LoadPlan plan) const {
  {
    std::lock_guard lock(mutex_);
    if (!publication_allowed_ || !error_) {
      return data_error(core::ErrorReason::invalid_argument, "Only failed imports can edit recovery parameters");
    }
  }
  return create(std::move(data_source_version_id), std::move(path), std::move(descriptor), plan);
}

core::Result<std::string> IncrementalLoader::view_log() const {
  std::lock_guard lock(mutex_);
  if (!error_ || error_->log_uri.empty()) {
    return data_error(core::ErrorReason::unavailable, "This import attempt has no failure log");
  }
  return error_->log_uri;
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
  source_state_ = DataSourceState::unavailable;
  const auto log_uri = "data-load://" + data_source_version_id_ + "/log";
  error_ = ImportErrorDetail{std::move(error), true, log_uri};
  published_range_.reset();
  real_samples_.clear();
  complex_samples_.clear();
}

LoadSnapshot IncrementalLoader::snapshot() const {
  std::lock_guard lock(mutex_);
  return LoadSnapshot{source_state_,
                      plan_.requested_range,
                      next_sample_,
                      bytes_read_,
                      publication_allowed_ && !error_ && next_sample_ < plan_.requested_range.end(),
                      error_,
                      published_range_};
}

} // namespace signal::data
