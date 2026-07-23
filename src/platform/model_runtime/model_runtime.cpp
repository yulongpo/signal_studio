#include "signal_studio/model_runtime/model_runtime.hpp"

#include <algorithm>
#include <utility>

namespace signal::model {

namespace {
core::Status model_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::model_runtime, reason}, std::move(message));
}

/// Null inference backend: used when ONNX Runtime is not installed. It honestly reports
/// unavailable and rejects every run() instead of fabricating model outputs.
class NullInferenceSession final : public IInferenceSession {
public:
  core::Result<InferenceResult> run(const InferenceRequest& /*request*/) override {
    return model_failure(core::ErrorReason::unavailable,
                         "ONNX Runtime is not installed in this environment; model inference is unavailable");
  }
  bool available() const noexcept override {
    return false;
  }
  std::string runtime_name() const noexcept override {
    return "null";
  }
};
} // namespace

core::Status ModelRegistry::install(ModelInfo info) {
  if (info.model_id.empty()) {
    return model_failure(core::ErrorReason::invalid_argument, "model id must be non-empty");
  }
  if (resolve(info.model_id).has_value()) {
    return model_failure(core::ErrorReason::invalid_argument, "model already installed: " + info.model_id);
  }
  models_.push_back(std::move(info));
  return core::Status::success();
}

std::optional<ModelInfo> ModelRegistry::resolve(const std::string& model_id) const {
  auto it = std::find_if(models_.begin(), models_.end(), [&](const ModelInfo& m) { return m.model_id == model_id; });
  if (it == models_.end())
    return std::nullopt;
  return *it;
}

std::vector<ModelInfo> ModelRegistry::list() const {
  return models_;
}

std::unique_ptr<IInferenceSession> make_inference_session() {
  // ONNX Runtime (BL1.0 default) is not installed. Return the honest null backend; callers must
  // check available() before relying on inference, and never fabricate results.
  return std::make_unique<NullInferenceSession>();
}

} // namespace signal::model
