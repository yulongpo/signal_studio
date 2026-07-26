#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"

#include <cstdint>

namespace signal::data::detail {

struct WavHeaderFields final {
  std::uint32_t riff_size{};
  std::uint16_t audio_format{};
  std::uint16_t channels{};
  std::uint32_t sample_rate{};
  std::uint32_t byte_rate{};
  std::uint16_t block_align{};
  std::uint16_t bits_per_sample{};
  std::uint32_t data_size{};
};

[[nodiscard]] core::Result<WavHeaderFields> calculate_wav_header_fields(const SignalDescriptor& descriptor,
                                                                        std::uint64_t sample_bytes);

} // namespace signal::data::detail
