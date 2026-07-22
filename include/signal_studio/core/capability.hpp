#pragma once

#include "signal_studio/core/error.hpp"

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace signal::core {

enum class CapabilityAvailability : std::uint8_t {
  unavailable = 0,
  available = 1,
  degraded = 2,
};

[[nodiscard]] bool is_known_capability_availability(CapabilityAvailability availability) noexcept;

struct Capability final {
  std::string id;
  CapabilityAvailability availability{CapabilityAvailability::unavailable};
  std::string provider;
  std::string detail;
};

class CapabilityRegistry final {
 public:
  [[nodiscard]] Status register_capability(Capability capability);
  [[nodiscard]] std::optional<Capability> find(std::string_view id) const;
  [[nodiscard]] bool is_available(std::string_view id) const;
  [[nodiscard]] std::vector<Capability> snapshot() const;

 private:
  mutable std::shared_mutex mutex_;
  std::vector<Capability> capabilities_;
};

}  // namespace signal::core
