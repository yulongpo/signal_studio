#include "signal_studio/visualization/overlay.hpp"

#include <algorithm>

namespace signal::visualization {

void OverlayModel::add_cursor(Cursor cursor) {
  cursors_.push_back(std::move(cursor));
}

void OverlayModel::clear_cursors() noexcept {
  cursors_.clear();
}

core::Status OverlayModel::set_frequency_selection(double lo_hz, double hi_hz) {
  if (lo_hz > hi_hz) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::visualization, core::ErrorReason::invalid_argument},
                                 "frequency selection must not be inverted");
  }
  Selection sel;
  sel.x_lo = lo_hz;
  sel.x_hi = hi_hz;
  selection_ = sel;
  return core::Status::success();
}

void OverlayModel::clear_selection() noexcept {
  selection_.reset();
}

void OverlayModel::set_measurement(Measurement measurement) {
  measurement_ = std::move(measurement);
}

void OverlayModel::clear_measurement() noexcept {
  measurement_.reset();
}

} // namespace signal::visualization
