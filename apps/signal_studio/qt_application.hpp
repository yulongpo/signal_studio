#pragma once

#include "application.hpp"

#include <filesystem>
#include <memory>
#include <string>

class QApplication;

namespace signal::studio {

class QtApplication final {
public:
  QtApplication(QApplication& application, std::filesystem::path state_directory);
  ~QtApplication();
  QtApplication(const QtApplication&) = delete;
  QtApplication& operator=(const QtApplication&) = delete;

  [[nodiscard]] core::Status initialize();
  [[nodiscard]] core::Status prepare_automation_input(const std::filesystem::path& source);
  [[nodiscard]] core::Status show_page(std::string_view page_id);
  /// 自动化验证 Designer 参数面板、控件到类型化模型的绑定以及纯显示更新不提交 DSP 任务。
  [[nodiscard]] core::Status validate_analysis_settings_panel();
  /// 使用真实输入验证参数切换异步提交、取消、最新结果仲裁和隐藏图表计算门禁。
  [[nodiscard]] core::Status validate_analysis_runtime();
  /// 自动化截图可显式展示高级参数区域。
  void set_analysis_settings_advanced(bool advanced);
  void show();
  [[nodiscard]] void* native_handle() noexcept;
  /// 自动化截图时返回当前可见的模态/非模态工作流窗口，否则返回主窗口。
  [[nodiscard]] void* capture_handle() noexcept;
  /// 保存主窗口及当前工作流窗口的合成截图，保持与批准原型相同的页面上下文。
  [[nodiscard]] core::Status save_screenshot(const std::filesystem::path& path);
  void show_import_wizard();
  void show_progress_preview(const std::filesystem::path& source);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace signal::studio
