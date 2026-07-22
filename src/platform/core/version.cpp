#include "signal_studio/core/version.hpp"

#include "signal_studio/core/build_config.hpp"

#include <array>
#include <charconv>

namespace signal::core {

std::string SemanticVersion::to_string() const {
  std::array<char, 24> output{};
  char* cursor = output.data();
  const auto append = [&cursor, &output](std::uint16_t value) {
    const auto result = std::to_chars(cursor, output.data() + output.size(), value);
    cursor = result.ptr;
  };
  append(major);
  *cursor++ = '.';
  append(minor);
  *cursor++ = '.';
  append(patch);
  return {output.data(), cursor};
}

const BuildInfo& build_info() noexcept {
  static constexpr BuildInfo info{
      "Signal Processing Platform",
      {SIGNAL_STUDIO_CONFIG_VERSION_MAJOR, SIGNAL_STUDIO_CONFIG_VERSION_MINOR, SIGNAL_STUDIO_CONFIG_VERSION_PATCH},
      SIGNAL_STUDIO_CONFIG_GIT_REVISION,
      SIGNAL_STUDIO_CONFIG_COMPILER,
      SIGNAL_STUDIO_CONFIG_GENERATOR,
      SIGNAL_STUDIO_CONFIG_BUILD_TYPE,
      SIGNAL_STUDIO_CONFIG_BUILD_TIMESTAMP,
      SIGNAL_STUDIO_CUDA_AVAILABLE != 0,
  };
  return info;
}

}  // namespace signal::core
