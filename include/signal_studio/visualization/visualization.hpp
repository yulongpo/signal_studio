#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/task_runtime/task_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace signal::visualization {

enum class ChartKind : std::uint8_t {
  time_waveform,
  spectrum,
  psd,
  waterfall,
  spectrogram,
  constellation,
  eye_diagram,
};

enum class TimeDisplayMode : std::uint8_t { real, in_phase_quadrature, magnitude, phase };
enum class SpectrumLayout : std::uint8_t { one_sided, mirrored_two_sided, shifted_two_sided };
enum class ViewQuality : std::uint8_t { placeholder, preview, refined };
enum class AmplitudeScale : std::uint8_t { linear, logarithmic };
enum class RangeMode : std::uint8_t { automatic, manual };
enum class SelectionKind : std::uint8_t { time, frequency, time_frequency };
enum class InteractionMode : std::uint8_t { selection, zoom, pan, cursor };

struct FrequencyRange final {
  std::int64_t begin_hz{};
  std::int64_t end_hz{};

  [[nodiscard]] std::int64_t bandwidth_hz() const noexcept;
  [[nodiscard]] bool contains(std::int64_t frequency_hz) const noexcept;
  [[nodiscard]] bool contains(const FrequencyRange& other) const noexcept;
  friend bool operator==(const FrequencyRange&, const FrequencyRange&) = default;
};

[[nodiscard]] core::Result<FrequencyRange> make_frequency_range(std::int64_t begin_hz, std::int64_t end_hz);
[[nodiscard]] core::Result<std::int64_t> parse_frequency_hz(std::string_view text);
[[nodiscard]] std::string format_frequency_hz(std::int64_t frequency_hz);

struct PsdMetadata final {
  std::uint64_t effective_samples{};
  std::uint64_t fft_frames{};
  std::uint32_t fft_size{};
  std::string window;
  std::string averaging;
  double rbw_hz{};
  std::string unit{"dB/Hz"};
  friend bool operator==(const PsdMetadata&, const PsdMetadata&) = default;
};

struct StftMetadata final {
  std::uint32_t window_size{};
  std::uint32_t hop_size{};
  std::uint32_t fft_size{};
  double overlap_ratio{};
  std::string color_map{"Industrial"};
  std::string interpolation{"nearest"};
  friend bool operator==(const StftMetadata&, const StftMetadata&) = default;
};

struct VisualizationFrame final {
  task::ViewRequestId request_id;
  data::SampleRange time_range;
  FrequencyRange frequency_range;
  ViewQuality quality{ViewQuality::placeholder};
  TimeDisplayMode time_mode{TimeDisplayMode::in_phase_quadrature};
  SpectrumLayout spectrum_layout{SpectrumLayout::shifted_two_sided};
  std::vector<double> time_primary;
  std::vector<double> time_secondary;
  std::vector<double> psd_db_hz;
  std::vector<float> stft_db;
  std::uint32_t stft_rows{};
  std::uint32_t stft_columns{};
  std::vector<double> constellation_i;
  std::vector<double> constellation_q;
  std::vector<double> eye_trace;
  PsdMetadata psd_metadata;
  StftMetadata stft_metadata;
  bool absolute_frequency{};
  std::int64_t center_frequency_hz{};
  std::string data_source_version_id;
};

struct ViewportSnapshot final {
  data::SampleRange loaded_range;
  data::SampleRange time_viewport;
  FrequencyRange effective_frequency_range;
  FrequencyRange frequency_viewport;
  task::ViewRequestId request_id;
  std::string data_source_version_id;
  bool partial_read{};
  std::uint64_t loaded_bytes{};
  std::uint64_t physical_bytes{};
  friend bool operator==(const ViewportSnapshot&, const ViewportSnapshot&) = default;
};

class ViewportController final {
public:
  explicit ViewportController(std::string scope = "visualization");

  [[nodiscard]] core::Result<task::ViewRequestId> bind_source(std::string data_source_version_id,
                                                              data::SampleRange loaded_range,
                                                              FrequencyRange effective_frequency_range,
                                                              bool partial_read = false, std::uint64_t loaded_bytes = 0,
                                                              std::uint64_t physical_bytes = 0);
  [[nodiscard]] core::Result<task::ViewRequestId> set_time(data::SampleRange viewport);
  [[nodiscard]] core::Result<task::ViewRequestId> pan_time(std::int64_t samples);
  [[nodiscard]] core::Result<task::ViewRequestId> resize_time(std::uint64_t begin, std::uint64_t span);
  [[nodiscard]] core::Result<task::ViewRequestId> set_frequency(FrequencyRange viewport);
  [[nodiscard]] core::Result<task::ViewRequestId> reset_frequency();
  [[nodiscard]] core::Result<task::ViewRequestId> restore_recent(std::string_view data_source_version_id);
  void save_recent();

  [[nodiscard]] const ViewportSnapshot& snapshot() const noexcept;
  [[nodiscard]] std::string partial_read_summary() const;

private:
  [[nodiscard]] task::ViewRequestId issue_request();

  std::string scope_;
  std::uint64_t generation_{};
  ViewportSnapshot snapshot_;
  std::map<std::string, data::SampleRange, std::less<>> recent_;
};

class AtomicFrameCoordinator final {
public:
  [[nodiscard]] core::Status begin(ViewportSnapshot expected);
  [[nodiscard]] core::Status commit(VisualizationFrame frame);
  [[nodiscard]] std::optional<VisualizationFrame> frame() const;
  [[nodiscard]] std::optional<ViewportSnapshot> expected() const;

private:
  std::optional<ViewportSnapshot> expected_;
  std::optional<VisualizationFrame> frame_;
};

struct Layer final {
  std::string id;
  std::string label;
  std::string source;
  bool visible{true};
  double opacity{1.0};
  std::int32_t order{};
  friend bool operator==(const Layer&, const Layer&) = default;
};

class LayerModel final {
public:
  [[nodiscard]] core::Status upsert(Layer layer);
  [[nodiscard]] core::Status remove(std::string_view id);
  [[nodiscard]] std::vector<Layer> ordered() const;
  [[nodiscard]] core::Result<std::string> serialize() const;
  [[nodiscard]] core::Status restore(std::string_view serialized);

private:
  std::map<std::string, Layer, std::less<>> layers_;
};

struct DisplayMapping final {
  AmplitudeScale amplitude_scale{AmplitudeScale::logarithmic};
  RangeMode range_mode{RangeMode::automatic};
  double minimum{-90.0};
  double maximum{-20.0};
  std::string color_map{"Industrial"};
  double reference_level{-20.0};
  double dynamic_range{70.0};
  friend bool operator==(const DisplayMapping&, const DisplayMapping&) = default;
};

[[nodiscard]] core::Status validate_display_mapping(const DisplayMapping& mapping);

struct Selection final {
  std::string id;
  std::string name;
  SelectionKind kind{SelectionKind::time};
  data::SampleRange time_range;
  std::optional<FrequencyRange> frequency_range;
  bool visible{true};
  bool locked{};
  bool stale{};
  std::uint32_t dependent_results{};
  friend bool operator==(const Selection&, const Selection&) = default;
};

struct Measurement final {
  std::string id;
  std::string selection_id;
  std::string data_source_version_id;
  data::SampleRange time_range;
  std::optional<FrequencyRange> frequency_range;
  task::ViewRequestId view_request;
  std::string algorithm;
  std::string unit;
  double value{};
  std::int64_t frequency_hz{};
  std::uint64_t sample_index{};
  std::string timestamp_utc;
  bool stale{};
  friend bool operator==(const Measurement&, const Measurement&) = default;
};

struct ChannelEstimate final {
  std::int64_t frequency_shift_hz{};
  std::uint64_t bandwidth_hz{};
  std::uint64_t output_sample_rate_hz{};
  bool complex_output{true};
  std::uint64_t estimated_bytes{};
  friend bool operator==(const ChannelEstimate&, const ChannelEstimate&) = default;
};

class OverlayModel final {
public:
  OverlayModel(data::SampleRange loaded_range, FrequencyRange effective_frequency_range);

  [[nodiscard]] core::Result<Selection> create(Selection selection);
  [[nodiscard]] core::Result<Selection> copy(std::string_view id);
  [[nodiscard]] core::Status update(Selection selection);
  [[nodiscard]] core::Status remove(std::string_view id);
  [[nodiscard]] std::optional<Selection> find(std::string_view id) const;
  [[nodiscard]] std::vector<Selection> selections() const;
  [[nodiscard]] core::Result<Measurement> add_measurement(Measurement measurement);
  [[nodiscard]] std::vector<Measurement> measurements() const;
  [[nodiscard]] core::Result<ChannelEstimate>
  estimate_channel(std::string_view selection_id, std::uint64_t output_sample_rate_hz, bool complex_output) const;
  void mark_dependencies_stale(std::string_view data_source_version_id);

private:
  [[nodiscard]] core::Status validate_selection(const Selection& selection) const;

  data::SampleRange loaded_range_;
  FrequencyRange effective_frequency_range_;
  std::uint64_t next_selection_id_{1};
  std::uint64_t next_measurement_id_{1};
  std::map<std::string, Selection, std::less<>> selections_;
  std::vector<Measurement> measurements_;
};

struct BatchItemResult final {
  std::string selection_id;
  bool succeeded{};
  std::string message;
  friend bool operator==(const BatchItemResult&, const BatchItemResult&) = default;
};

[[nodiscard]] std::vector<BatchItemResult> run_batch(const std::vector<Selection>& selections,
                                                     const std::function<core::Status(const Selection&)>& operation);

class PrefetchController final {
public:
  [[nodiscard]] std::uint64_t begin(std::uint64_t previous_samples, std::uint64_t following_samples);
  void cancel_for_interaction() noexcept;
  void cancel_for_resource_pressure() noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;

private:
  std::uint64_t generation_{};
  bool active_{};
};

struct ChartActivity final {
  bool visible{true};
  bool observer_connected{true};
  std::uint32_t pending_preparations{};
  std::uint64_t paint_epoch{};
  friend bool operator==(const ChartActivity&, const ChartActivity&) = default;
};

class VisibilityController final {
public:
  VisibilityController();
  [[nodiscard]] core::Status set_visible(ChartKind chart, bool visible);
  [[nodiscard]] core::Status note_preparation(ChartKind chart);
  [[nodiscard]] core::Status note_paint(ChartKind chart);
  [[nodiscard]] ChartActivity state(ChartKind chart) const;

private:
  std::map<ChartKind, ChartActivity> states_;
};

struct ChartLayoutItem final {
  ChartKind chart{ChartKind::time_waveform};
  std::uint32_t logical_height{180};
  friend bool operator==(const ChartLayoutItem&, const ChartLayoutItem&) = default;
};

class ChartLayoutModel final {
public:
  [[nodiscard]] core::Status set(std::vector<ChartLayoutItem> items);
  [[nodiscard]] const std::vector<ChartLayoutItem>& items() const noexcept;
  [[nodiscard]] core::Result<std::string> serialize() const;
  [[nodiscard]] core::Status restore(std::string_view serialized);

private:
  std::vector<ChartLayoutItem> items_{
      {ChartKind::time_waveform, 150}, {ChartKind::psd, 170}, {ChartKind::spectrogram, 260}};
};

struct ScreenshotOptions final {
  bool axes{true};
  bool legend{true};
  bool color_scale{true};
  bool cursors{true};
  bool selections{true};
  bool parameter_summary{true};
  friend bool operator==(const ScreenshotOptions&, const ScreenshotOptions&) = default;
};

struct ComponentDescriptor final {
  std::string id;
  std::string purpose;
  bool interactive{};
  friend bool operator==(const ComponentDescriptor&, const ComponentDescriptor&) = default;
};

[[nodiscard]] std::vector<ComponentDescriptor> component_catalog();

struct AnalysisWorkspaceConfiguration final {
  std::string title{"宽带视窗"};
  std::string source_label{"未绑定数据源"};
  std::uint32_t minimum_chart_height{96};
  bool show_waveform{true};
  bool show_psd{true};
  bool show_spectrogram{true};
  std::vector<ChartKind> extra_charts;
};

class IAnalysisWorkspace {
public:
  virtual ~IAnalysisWorkspace() noexcept = default;
  [[nodiscard]] virtual void* native_handle() noexcept = 0;
  [[nodiscard]] virtual core::Status bind_frame(VisualizationFrame frame) = 0;
  [[nodiscard]] virtual core::Status apply_viewport(const ViewportSnapshot& viewport) = 0;
  [[nodiscard]] virtual core::Status set_interaction_mode(InteractionMode mode) = 0;
  [[nodiscard]] virtual core::Status fit_frequency_to_data() = 0;
  [[nodiscard]] virtual core::Result<Selection> create_selection(Selection selection) = 0;
  [[nodiscard]] virtual std::vector<Selection> selections() const = 0;
  [[nodiscard]] virtual core::Status locate_selection(std::string_view id) = 0;
  [[nodiscard]] virtual core::Status save_screenshot(const std::filesystem::path& path, ScreenshotOptions options) = 0;
  [[nodiscard]] virtual ViewportSnapshot viewport() const = 0;
  [[nodiscard]] virtual std::string accessibility_summary() const = 0;
  virtual void set_status(std::string status) = 0;
};

/// 返回以 Qt Widgets 实现、但不在公共签名中暴露 Qt 类型的可视化工作区。
[[nodiscard]] std::unique_ptr<IAnalysisWorkspace> make_analysis_workspace(AnalysisWorkspaceConfiguration configuration);

} // namespace signal::visualization
