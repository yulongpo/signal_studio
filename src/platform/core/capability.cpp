#include "signal_studio/core/capability.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace signal::core {

Status CapabilityRegistry::register_capability(Capability capability) {
  if (capability.id.empty() || capability.provider.empty()) {
    return Status::failure(
        {ErrorDomain::core, ErrorReason::invalid_argument},
        "Capability id and provider are required");
  }
  std::unique_lock lock{mutex_};
  const auto duplicate = std::ranges::find(capabilities_, capability.id, &Capability::id);
  if (duplicate != capabilities_.end()) {
    return Status::failure(
        {ErrorDomain::core, ErrorReason::invalid_argument},
        "Capability id is already registered",
        capability.id);
  }
  capabilities_.push_back(std::move(capability));
  std::ranges::sort(capabilities_, {}, &Capability::id);
  return Status::success();
}

std::optional<Capability> CapabilityRegistry::find(std::string_view id) const {
  std::shared_lock lock{mutex_};
  const auto found = std::ranges::find(capabilities_, id, &Capability::id);
  if (found == capabilities_.end()) {
    return std::nullopt;
  }
  return *found;
}

bool CapabilityRegistry::is_available(std::string_view id) const {
  const auto capability = find(id);
  return capability && capability->availability == CapabilityAvailability::available;
}

std::vector<Capability> CapabilityRegistry::snapshot() const {
  std::shared_lock lock{mutex_};
  return capabilities_;
}

}  // namespace signal::core
