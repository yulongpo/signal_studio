#include "signal_studio/data/preview.hpp"
#include "signal_studio/data/io.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

namespace signal::data {
namespace {

core::Status data_error(core::ErrorReason reason, std::string message, std::string detail = {}) {
  return core::Status::failure({core::ErrorDomain::data, reason}, std::move(message), std::move(detail));
}

void add_warning(std::vector<QualityWarning>& warnings, QualityWarning warning) {
  if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end())
    warnings.push_back(warning);
}

std::complex<double> sample_at(const SignalSlice& samples, std::size_t index) {
  if (samples.kind() == SignalKind::real)
    return {samples.real_values()[index], 0.0};
  const auto value = samples.complex_values()[index];
  return {value.real, value.imag};
}

} // namespace

CancellationToken::CancellationToken() : state_(std::make_shared<std::atomic_bool>(false)) {}
void CancellationToken::cancel() noexcept {
  state_->store(true, std::memory_order_release);
}
bool CancellationToken::cancelled() const noexcept {
  return state_->load(std::memory_order_acquire);
}

core::Result<PreviewResult> create_bounded_preview(const std::filesystem::path& path,
                                                   const SignalDescriptor& descriptor, const PreviewOptions& options,
                                                   const CancellationToken& cancellation) {
  if (options.maximum_samples == 0U || options.maximum_read_bytes == 0U || options.spectrum_bins == 0U ||
      !std::isfinite(options.clipping_level) || options.clipping_level <= 0.0 ||
      !std::isfinite(options.dc_warning_ratio) || options.dc_warning_ratio < 0.0 ||
      !std::isfinite(options.unusual_amplitude_level) || options.unusual_amplitude_level <= 0.0) {
    return data_error(core::ErrorReason::invalid_argument, "Preview bounds and quality thresholds are invalid");
  }
  if (cancellation.cancelled())
    return data_error(core::ErrorReason::cancelled, "Preview was cancelled");
  BoundedFileReader reader{path, options.maximum_read_bytes};
  const auto file_size = reader.size();
  if (!file_size)
    return file_size.error();
  const auto facts = calculate_data_facts(file_size.value(), descriptor, options.maximum_read_bytes);
  if (!facts)
    return facts.error();
  const auto by_bytes = options.maximum_read_bytes / facts.value().frame_bytes;
  const auto count = std::min({options.maximum_samples, descriptor.requested_sample_range.size(), by_bytes});
  const auto range = SampleRange::from_count(descriptor.requested_sample_range.begin(), count);
  if (!range)
    return range.error();
  auto raw = read_raw_samples(path, descriptor, range.value(), options.maximum_read_bytes);
  if (!raw)
    return raw.error();
  if (cancellation.cancelled())
    return data_error(core::ErrorReason::cancelled, "Preview was cancelled");

  PreviewResult result;
  result.analyzed_range = range.value();
  result.whole_file = range.value().begin() == 0U && range.value().end() == facts.value().available_frames;
  result.spectrum.limited = !result.whole_file;
  result.spectrum.analyzed_samples = count;
  result.statistics.sample_count = count;
  result.statistics.minimum = std::numeric_limits<double>::infinity();
  result.statistics.maximum = -std::numeric_limits<double>::infinity();
  long double sum{};
  long double square_sum{};
  bool all_zero = true;
  bool clipped{};
  bool unusual{};

  auto observe = [&](double value) {
    if (std::isnan(value)) {
      ++result.statistics.nan_component_count;
      return;
    }
    if (std::isinf(value)) {
      ++result.statistics.infinity_component_count;
      return;
    }
    ++result.statistics.finite_component_count;
    result.statistics.minimum = std::min(result.statistics.minimum, value);
    result.statistics.maximum = std::max(result.statistics.maximum, value);
    sum += value;
    square_sum += static_cast<long double>(value) * value;
    all_zero = all_zero && value == 0.0;
    clipped = clipped || std::abs(value) >= options.clipping_level;
    unusual = unusual || std::abs(value) > options.unusual_amplitude_level;
  };
  if (raw.value().samples.kind() == SignalKind::real) {
    for (const double value : raw.value().samples.view().real_values())
      observe(value);
  } else {
    for (const auto value : raw.value().samples.view().complex_values()) {
      observe(value.real);
      observe(value.imag);
    }
  }
  if (result.statistics.finite_component_count == 0U) {
    result.statistics.minimum = std::numeric_limits<double>::quiet_NaN();
    result.statistics.maximum = std::numeric_limits<double>::quiet_NaN();
    result.statistics.mean = std::numeric_limits<double>::quiet_NaN();
    result.statistics.rms = std::numeric_limits<double>::quiet_NaN();
  } else {
    result.statistics.mean = static_cast<double>(sum / result.statistics.finite_component_count);
    result.statistics.rms = std::sqrt(static_cast<double>(square_sum / result.statistics.finite_component_count));
  }
  if (result.statistics.nan_component_count != 0U)
    add_warning(result.warnings, QualityWarning::nan_present);
  if (result.statistics.infinity_component_count != 0U)
    add_warning(result.warnings, QualityWarning::infinity_present);
  if (result.statistics.finite_component_count != 0U && all_zero)
    add_warning(result.warnings, QualityWarning::all_zero);
  if (clipped)
    add_warning(result.warnings, QualityWarning::clipping);
  if (unusual)
    add_warning(result.warnings, QualityWarning::unusual_amplitude);
  if (result.statistics.finite_component_count != 0U && result.statistics.rms > 0.0 &&
      std::abs(result.statistics.mean) > options.dc_warning_ratio * result.statistics.rms) {
    add_warning(result.warnings, QualityWarning::dc_offset);
  }

  const auto samples = raw.value().samples.view();
  const auto bins = static_cast<std::size_t>(std::min<std::uint64_t>(options.spectrum_bins, count));
  result.spectrum.magnitude.resize(bins);
  for (std::size_t bin = 0; bin < bins; ++bin) {
    if (cancellation.cancelled())
      return data_error(core::ErrorReason::cancelled, "Preview was cancelled");
    std::complex<long double> accumulator{};
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
      const auto value = sample_at(samples, index);
      const long double phase = -2.0L * std::numbers::pi_v<long double> * static_cast<long double>(bin) *
                                static_cast<long double>(index) / static_cast<long double>(count == 0U ? 1U : count);
      accumulator += std::complex<long double>{value.real(), value.imag()} *
                     std::complex<long double>{std::cos(phase), std::sin(phase)};
    }
    result.spectrum.magnitude[bin] =
        count == 0U ? 0.0 : static_cast<double>(std::abs(accumulator) / static_cast<long double>(count));
  }
  return result;
}

CancellationToken PreviewCoordinator::begin_request() {
  current_.cancel();
  current_ = CancellationToken{};
  return current_;
}

void PreviewCoordinator::cancel_current() noexcept {
  current_.cancel();
}

} // namespace signal::data
