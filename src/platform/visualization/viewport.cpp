#include "signal_studio/visualization/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

namespace signal::visualization {

ViewportRange::ViewportRange(double minimum, double maximum)
    : minimum_(std::min(minimum, maximum)), maximum_(std::max(minimum, maximum)), lo_(minimum_), hi_(maximum_) {}

bool ViewportRange::contains(double value) const noexcept {
  return value >= minimum_ && value <= maximum_;
}

core::Status ViewportRange::set_window(double lo, double hi) {
  if (lo > hi) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::visualization, core::ErrorReason::invalid_argument},
                                 "viewport window must not be inverted");
  }
  lo_ = std::clamp(lo, minimum_, maximum_);
  hi_ = std::clamp(hi, minimum_, maximum_);
  if (hi_ < lo_) std::swap(lo_, hi_);
  clamp_window();
  notify();
  return core::Status::success();
}

core::Status ViewportRange::zoom(double center, double factor) {
  if (factor <= 0.0) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::visualization, core::ErrorReason::invalid_argument},
                                 "zoom factor must be positive");
  }
  const double span = hi_ - lo_;
  const double new_span = std::clamp(span * factor, 0.0, maximum_ - minimum_);
  lo_ = std::clamp(center - new_span / 2.0, minimum_, maximum_ - new_span);
  hi_ = lo_ + new_span;
  notify();
  return core::Status::success();
}

core::Status ViewportRange::pan(double fraction_of_span) {
  const double span = hi_ - lo_;
  double delta = span * fraction_of_span;
  lo_ += delta;
  hi_ += delta;
  clamp_window();
  notify();
  return core::Status::success();
}

std::uint64_t ViewportRange::subscribe(Subscriber subscriber) {
  std::uint64_t id = next_id_++;
  subscribers_.emplace_back(id, std::move(subscriber));
  return id;
}

void ViewportRange::unsubscribe(std::uint64_t id) {
  std::erase_if(subscribers_, [id](const auto& entry) { return entry.first == id; });
}

void ViewportRange::clamp_window() {
  if (lo_ < minimum_) {
    hi_ += minimum_ - lo_;
    lo_ = minimum_;
  }
  if (hi_ > maximum_) {
    lo_ -= hi_ - maximum_;
    hi_ = maximum_;
  }
  if (lo_ < minimum_) lo_ = minimum_;
  if (hi_ > maximum_) hi_ = maximum_;
  if (hi_ < lo_) hi_ = lo_;
}

void ViewportRange::notify() {
  for (auto& [id, sub] : subscribers_) {
    if (sub) sub();
  }
}

FrequencyViewport::FrequencyViewport(double max_hz) : range_(0.0, std::max(0.0, max_hz)) {}

core::Status FrequencyViewport::set_frequency_window(double lo_hz, double hi_hz) {
  return range_.set_window(lo_hz, hi_hz);
}

TimeViewport::TimeViewport(LoadedDataRange loaded) : loaded_(loaded) {
  const double dur = loaded_.duration_seconds();
  range_ = ViewportRange(0.0, dur > 0.0 ? dur : 1.0);
}

core::Status TimeViewport::set_loaded_range(LoadedDataRange loaded) {
  loaded_ = loaded;
  const double dur = loaded_.duration_seconds();
  range_ = ViewportRange(0.0, dur > 0.0 ? dur : 1.0);
  return core::Status::success();
}

ViewportController::ViewportController(LoadedDataRange loaded, double max_frequency_hz)
    : time_(loaded), frequency_(max_frequency_hz) {}

std::string_view ViewportController::frequency_unit() const noexcept {
  return frequency_unit_for(frequency_.range().window_hi());
}

std::string_view frequency_unit_for(double hz) noexcept {
  if (hz >= 1.0e9) return "GHz";
  if (hz >= 1.0e6) return "MHz";
  if (hz >= 1.0e3) return "kHz";
  return "Hz";
}

std::string format_frequency(double hz) noexcept {
  const std::string_view unit = frequency_unit_for(hz);
  double scaled = hz;
  if (unit == "GHz") scaled = hz / 1.0e9;
  else if (unit == "MHz") scaled = hz / 1.0e6;
  else if (unit == "kHz") scaled = hz / 1.0e3;
  std::ostringstream ss;
  // Preserve Hz-level precision: 6 significant digits covers kHz..GHz with sub-Hz resolution.
  ss.precision(6);
  ss << scaled << " " << unit;
  return ss.str();
}

}  // namespace signal::visualization
