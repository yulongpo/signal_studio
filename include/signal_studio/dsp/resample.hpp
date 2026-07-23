#pragma once

#include "signal_studio/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace signal::dsp {

/// Rational resample ratio L/M (output rate = L/M * input rate). Both must be positive.
struct ResampleRatio final {
  std::uint32_t num{1};
  std::uint32_t den{1};
  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ResampleRatio&, const ResampleRatio&) = default;
};

class IResampler {
 public:
  virtual ~IResampler() = default;
  /// Resample a real-valued block by the ratio. Anti-aliasing is applied for both up- and
  /// down-sampling. Returns the resampled block.
  [[nodiscard]] virtual core::Result<std::vector<double>>
  process(const ResampleRatio& ratio, std::span<const double> input) = 0;
};

/// Polyphase FIR resampler. The anti-alias/anti-image lowpass is designed by the windowed-sinc
/// method at cutoff min(fs_in, fs_out)/2 with gain L, then decomposed into L polyphase branches.
class PolyphaseResampler final : public IResampler {
 public:
  PolyphaseResampler() = default;
  [[nodiscard]] core::Result<std::vector<double>>
  process(const ResampleRatio& ratio, std::span<const double> input) override;
};

}  // namespace signal::dsp
