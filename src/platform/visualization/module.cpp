#include "signal_studio/visualization/module.hpp"

#include <QtCore/qglobal.h>

#include <array>

namespace signal::visualization {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static_assert(QT_VERSION >= QT_VERSION_CHECK(6, 10, 3),
                "SignalStudio::Visualization requires Qt 6.10.3 or newer");
  static constexpr std::array dependencies{
      core::ModuleId::data, core::ModuleId::task_runtime, core::ModuleId::core};
  static constexpr std::array<std::string_view, 3> capabilities{
      "visualization.view-contracts", "visualization.qt-widgets-private", "visualization.export-contracts"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::visualization,
      "SignalStudio::Visualization",
      "signal::visualization",
      {1, 0, 0},
      dependencies,
      capabilities};
  return descriptor;
}

}  // namespace signal::visualization
