#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace signal::core {

struct SemanticVersion final {
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint16_t patch{};

  [[nodiscard]] std::string to_string() const;
  friend constexpr bool operator==(const SemanticVersion&, const SemanticVersion&) = default;
  friend constexpr auto operator<=>(const SemanticVersion&, const SemanticVersion&) = default;
};

struct BuildInfo final {
  std::string_view product;
  SemanticVersion version;
  std::string_view source_revision;
  std::string_view compiler;
  std::string_view generator;
  std::string_view build_type;
  std::string_view build_timestamp_utc;
  bool cuda_toolkit_available{};
};

[[nodiscard]] const BuildInfo& build_info() noexcept;

} // namespace signal::core
