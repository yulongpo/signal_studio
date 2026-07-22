#include "signal_studio/workbench/module.hpp"

#include <QtCore/qglobal.h>

#include <array>

namespace signal::workbench {

const core::ModuleDescriptor& module_descriptor() noexcept {
  static_assert(QT_VERSION >= QT_VERSION_CHECK(6, 10, 3),
                "SignalStudio::Workbench requires Qt 6.10.3 or newer");
  static constexpr std::array dependencies{
      core::ModuleId::visualization, core::ModuleId::task_runtime, core::ModuleId::core};
  static constexpr std::array<std::string_view, 3> capabilities{
      "workbench.commands", "workbench.docking-contracts", "workbench.qt-widgets-private"};
  static constexpr core::ModuleDescriptor descriptor{
      core::ModuleId::workbench,
      "SignalStudio::Workbench",
      "signal::workbench",
      {1, 0, 0},
      dependencies,
      capabilities};
  return descriptor;
}

}  // namespace signal::workbench
