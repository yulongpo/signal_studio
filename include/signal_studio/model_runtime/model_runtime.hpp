#pragma once

#include "signal_studio/core/result.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace signal::model {

enum class InferenceDevice : std::uint8_t { cpu = 0, cuda = 1 };

/// Registered model package metadata (API-MODEL-001).
struct ModelInfo final {
  std::string model_id;
  std::uint32_t version_major{};
  std::uint32_t version_minor{};
  std::uint32_t version_patch{};
  std::string runtime; // "onnxruntime" when available
  std::string sha256_digest;
  std::vector<std::string> input_names;
  std::vector<std::string> output_names;
  friend bool operator==(const ModelInfo&, const ModelInfo&) = default;
};

/// Inference request (API-MODEL-002). Tensors are row-major float32.
struct InferenceRequest final {
  std::string model_id;
  InferenceDevice device{InferenceDevice::cpu};
  std::vector<std::string> input_names;
  std::vector<std::vector<std::uint64_t>> input_shapes;
  std::vector<float> input_data; // concatenated, row-major
  friend bool operator==(const InferenceRequest&, const InferenceRequest&) = default;
};

struct InferenceResult final {
  std::vector<std::string> output_names;
  std::vector<std::vector<std::uint64_t>> output_shapes;
  std::vector<float> output_data;
  InferenceDevice device{InferenceDevice::cpu};
  std::string runtime;
  friend bool operator==(const InferenceResult&, const InferenceResult&) = default;
};

class IInferenceSession {
public:
  virtual ~IInferenceSession() = default;
  [[nodiscard]] virtual core::Result<InferenceResult> run(const InferenceRequest& request) = 0;
  [[nodiscard]] virtual bool available() const noexcept = 0;
  [[nodiscard]] virtual std::string runtime_name() const noexcept = 0;
};

/// Registry of installed models (API-MODEL-001).
class IModelRegistry {
public:
  virtual ~IModelRegistry() = default;
  [[nodiscard]] virtual core::Status install(ModelInfo info) = 0;
  [[nodiscard]] virtual std::optional<ModelInfo> resolve(const std::string& model_id) const = 0;
  [[nodiscard]] virtual std::vector<ModelInfo> list() const = 0;
};

class ModelRegistry final : public IModelRegistry {
public:
  core::Status install(ModelInfo info) override;
  std::optional<ModelInfo> resolve(const std::string& model_id) const override;
  std::vector<ModelInfo> list() const override;

private:
  std::vector<ModelInfo> models_;
};

/// Construct the best available inference session. ONNX Runtime is the BL1.0 default backend but
/// is not installed in this environment; the returned session reports unavailable()=true and run()
/// returns an honest error rather than fabricating outputs. A deterministic null backend is used
/// for interface testing when ONNX is absent.
[[nodiscard]] std::unique_ptr<IInferenceSession> make_inference_session();

} // namespace signal::model
