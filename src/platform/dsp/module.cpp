#include "signal_studio/dsp/module.hpp"

#include <array>

namespace signal::dsp {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static constexpr std::array dependencies{
      core::ModuleId::data, core::ModuleId::compute, core::ModuleId::core};
  static constexpr std::array<std::string_view, 3> capabilities{"dsp.fft", "dsp.psd", "dsp.stft"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::dsp, "SignalStudio::DSP", "signal::dsp", {1, 0, 0}, dependencies, capabilities};
  return descriptor;
}

}  // namespace signal::dsp
