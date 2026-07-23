#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace signal::plugin {

/// Capability a plugin declares (e.g. "algorithm.fft", "format.mat").
struct PluginCapability final {
  std::string id;
  std::string detail;
  friend bool operator==(const PluginCapability&, const PluginCapability&) = default;
};

/// Descriptor reported by an algorithm plugin (API-PLG-002).
struct AlgorithmPluginDescriptor final {
  std::string plugin_id;
  std::uint32_t version_major{};
  std::uint32_t version_minor{};
  std::uint32_t version_patch{};
  std::vector<PluginCapability> capabilities;
  friend bool operator==(const AlgorithmPluginDescriptor&, const AlgorithmPluginDescriptor&) = default;
};

/// Request to run an algorithm plugin on a sample block.
struct AlgorithmRequest final {
  std::string operation;             // e.g. "fft", "detect-tokens"
  data::SignalSlice input;
  std::map<std::string, double> parameters;
  friend bool operator==(const AlgorithmRequest&, const AlgorithmRequest&) = default;
};

/// Result of an algorithm run.
struct AlgorithmResult final {
  std::vector<double> scalar_outputs;
  std::vector<data::ComplexSample> complex_outputs;
  std::string notes;
  friend bool operator==(const AlgorithmResult&, const AlgorithmResult&) = default;
};

/// Algorithm plugin interface (API-PLG-002). No UI dependency. Implementations may wrap the C ABI
/// or be linked directly.
class IAlgorithmPlugin {
 public:
  virtual ~IAlgorithmPlugin() = default;
  [[nodiscard]] virtual const AlgorithmPluginDescriptor& descriptor() const noexcept = 0;
  [[nodiscard]] virtual core::Result<AlgorithmResult> run(const AlgorithmRequest& request) = 0;
};

/// A trivial built-in algorithm plugin used as a reference implementation and test fixture. It
/// computes time-domain RMS of a real input for operation "rms".
class RmsAlgorithmPlugin final : public IAlgorithmPlugin {
 public:
  RmsAlgorithmPlugin();
  [[nodiscard]] const AlgorithmPluginDescriptor& descriptor() const noexcept override;
  [[nodiscard]] core::Result<AlgorithmResult> run(const AlgorithmRequest& request) override;

 private:
  AlgorithmPluginDescriptor descriptor_;
};

}  // namespace signal::plugin
