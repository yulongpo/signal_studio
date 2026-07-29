#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::workbench {

enum class InspectorViewKind : std::uint8_t {
  time_domain,
  spectrum,
  stft,
  constellation,
  eye_diagram,
  amplitude_histogram,
  phase_histogram,
  instantaneous_frequency,
};

struct ViewApplicability final {
  InspectorViewKind kind{InspectorViewKind::time_domain};
  bool applicable{};
  std::string reason;
  std::string range;
  std::string unit;
  std::string preprocessing;
  friend bool operator==(const ViewApplicability&, const ViewApplicability&) = default;
};

struct ConstellationOptions final {
  std::uint64_t maximum_points{100'000U};
  bool density_mode{};
  bool reference_grid{true};
  bool compare_before_after{};
  friend bool operator==(const ConstellationOptions&, const ConstellationOptions&) = default;
};

struct EyeDiagramOptions final {
  std::optional<double> symbol_rate_baud;
  std::string synchronization_source;
  friend bool operator==(const EyeDiagramOptions&, const EyeDiagramOptions&) = default;
};

struct InspectorResultBinding final {
  std::string result_id;
  std::string channel_version;
  std::string parameter_version;
  friend bool operator==(const InspectorResultBinding&, const InspectorResultBinding&) = default;
};

struct InspectorChannelState final {
  std::string channel_id;
  std::string channel_version;
  std::string data_source_version_id;
  data::SignalKind signal_kind{data::SignalKind::real};
  std::vector<ViewApplicability> views;
  ConstellationOptions constellation;
  EyeDiagramOptions eye_diagram;
  std::vector<InspectorResultBinding> results;
  std::map<std::string, std::string, std::less<>> parameters;
  friend bool operator==(const InspectorChannelState&, const InspectorChannelState&) = default;
};

struct InspectorLayoutTemplate final {
  std::string id;
  std::vector<InspectorViewKind> views;
  std::vector<std::string> required_plugins;
  friend bool operator==(const InspectorLayoutTemplate&, const InspectorLayoutTemplate&) = default;
};

struct RestoredInspectorLayout final {
  InspectorLayoutTemplate layout;
  bool degraded{};
  std::vector<std::string> missing_plugins;
};

[[nodiscard]] core::Result<InspectorChannelState>
make_inspector_channel_state(std::string channel_id, std::string channel_version, std::string data_source_version_id,
                             const data::SignalDescriptor& descriptor,
                             std::optional<double> symbol_rate_baud = std::nullopt,
                             std::string synchronization_source = {});
[[nodiscard]] core::Status validate_inspector_channel(const InspectorChannelState& state);
[[nodiscard]] bool inspector_result_is_current(const InspectorResultBinding& result,
                                               std::string_view current_channel_version,
                                               std::string_view current_parameter_version) noexcept;
[[nodiscard]] core::Result<RestoredInspectorLayout>
restore_inspector_layout(const InspectorLayoutTemplate& layout, std::span<const std::string> available_plugins);

class InspectorStateStore final {
public:
  [[nodiscard]] core::Status upsert(InspectorChannelState state);
  [[nodiscard]] std::optional<InspectorChannelState> find(std::string_view channel_id) const;
  [[nodiscard]] std::vector<InspectorChannelState> channels() const;
  [[nodiscard]] core::Result<std::string> serialize() const;
  [[nodiscard]] static core::Result<InspectorStateStore> parse(std::string_view serialized);

private:
  std::map<std::string, InspectorChannelState, std::less<>> channels_;
};

[[nodiscard]] std::string_view inspector_view_name(InspectorViewKind kind) noexcept;

} // namespace signal::workbench
