#pragma once

#include "signal_studio/core/result.hpp"
#include "signal_studio/visualization/data_series.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace signal::visualization {

enum class ChartViewKind : std::uint8_t {
  time_waveform = 0,
  spectrum = 1,
  spectrogram = 2,
  constellation = 3,
  eye_diagram = 4,
};

/// Abstract chart view (API-VIS-001). Public API is Qt-free; the underlying QWidget is exposed
/// via native_widget() for the host application to embed in its own Qt layout.
class IChartView {
public:
  using NeedsDataCallback = std::function<bool()>;

  virtual ~IChartView() = default;
  [[nodiscard]] virtual ChartViewKind kind() const noexcept = 0;
  /// Bind a data series. Replaces any previously bound series.
  virtual void bind(std::shared_ptr<const IDataSeries> series) = 0;
  [[nodiscard]] virtual std::shared_ptr<const IDataSeries> bound_series() const noexcept = 0;
  /// Visibility toggles rendering AND data demand: a hidden view reports no data need so its
  /// dedicated computation can stop (approved hide-and-stop invariant).
  virtual void set_visible(bool visible) = 0;
  [[nodiscard]] virtual bool visible() const noexcept = 0;
  /// Returns the QWidget* as void* so the host (which links Qt itself) can embed it.
  [[nodiscard]] virtual void* native_widget() noexcept = 0;
  /// Callback the host polls to decide whether to keep producing data for this view.
  virtual void set_needs_data_callback(NeedsDataCallback callback) = 0;
  [[nodiscard]] virtual bool needs_data() const noexcept = 0;
};

[[nodiscard]] std::string_view to_string(ChartViewKind kind) noexcept;

} // namespace signal::visualization
