#include "signal_studio/plugin_sdk/algorithm_plugin.hpp"

#include <cmath>

namespace signal::plugin {

RmsAlgorithmPlugin::RmsAlgorithmPlugin() {
  descriptor_.plugin_id = "builtin.rms";
  descriptor_.version_major = 1;
  descriptor_.version_minor = 0;
  descriptor_.version_patch = 0;
  descriptor_.capabilities.push_back({"algorithm.statistics", "time-domain RMS"});
}

const AlgorithmPluginDescriptor& RmsAlgorithmPlugin::descriptor() const noexcept {
  return descriptor_;
}

core::Result<AlgorithmResult> RmsAlgorithmPlugin::run(const AlgorithmRequest& request) {
  if (request.operation != "rms") {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::plugin_sdk, core::ErrorReason::invalid_argument},
                                 "unsupported operation: " + request.operation);
  }
  if (request.input.kind() != data::SignalKind::real) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::plugin_sdk, core::ErrorReason::invalid_argument},
                                 "rms operation requires a real-valued input");
  }
  const auto values = request.input.real_values();
  if (values.empty()) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::plugin_sdk, core::ErrorReason::invalid_argument},
                                 "rms operation requires non-empty input");
  }
  // PluginSDK cannot depend on DSP (approved DAG), so compute RMS inline.
  double sq_sum = 0.0;
  for (double v : values) sq_sum += v * v;
  AlgorithmResult result;
  result.scalar_outputs.push_back(std::sqrt(sq_sum / static_cast<double>(values.size())));
  result.notes = "rms";
  return result;
}

}  // namespace signal::plugin
