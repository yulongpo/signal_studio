# 基础骨架验证记录

本页保留 2026-10-08 骨架阶段的历史结果；其中旧尺寸截图与待完成条目不代表当前验收状态。当前 UI、交互及显示器 2 的 4K / 150% 验收以 [本轮记录](ui-parity.md) 为准，后续不再运行旧分辨率组合。

日期：2026-10-08，Asia/Shanghai。验证对象为本仓库新开发的 C++20 / Qt 原生工程；不沿用浏览器原型或旧 Signal Studio 的测试结果。

## 构建和状态验证

本机工具链：Windows x64、Visual Studio 2026、MSVC 19.51.36260.0、CMake 4.3.1、Qt 6.11.1 `msvc2022_64`。Debug 与 Release 均完成配置、编译及自动运行库部署。

| 检查 | Debug | Release | 证据 |
|---|---|---|---|
| `signal_studio_core` | 通过 | 通过 | 范围边界、uint64 精度、文件独立状态、40 条历史、标记/通道删除、严格原生 JSON 往返及非法输入原子性 |
| `signal_studio_ui` | 通过 | 通过 | 原生 Qt 事件、布局、持续创建、取消回滚、边角编辑、文件模式恢复、树 Ctrl/Shift 选择、滚轮期间参数修改等 |
| CTest | 2/2 | 2/2 | [Debug 执行日志](ctest-debug.txt)、[Release 执行日志](ctest-release.txt) |

Qt UI 明细记录为 **22 passed / 0 failed / 0 skipped**，包括初始化、清理和 20 个行为场景：[UI 明细](ui-debug-results.txt)。大样本索引测试覆盖大于 `2^53` 以及 `UINT64_MAX` 附近的视窗；辅助 Y 轴和标记边角测试覆盖波形/PSD、时频/瀑布方向。

UI 回归使用 Qt offscreen 平台，在实际控件中投递 Qt 鼠标、滚轮与键盘事件。此平台报告不支持系统鼠标捕获；因此这些结果验证处理函数和状态转换，不证明 OS 捕获丢失、真实指针递送或实际窗口失焦已全部人工验收。

## GPU、缓存与布局

实际 Windows 平台创建了 QRhi Direct3D 11 设备，识别为硬件设备，驱动名称为 `AMD Radeon(TM) 880M Graphics`，vendor `0x1002` / device `0x150e`。导航、辅助图和主图都成功提交了图形帧。

Release 的 [机器可读 GPU 记录](release-gpu-report.json) 为：

```json
{
  "hardwareRenderer": true,
  "completedFrames": 6,
  "heatmapUploads": 1,
  "uploadsAfterOverlay": 1,
  "powerGenerations": 2,
  "overlayReusesHeatmap": true,
  "mainPlotHeight": 244,
  "pass": true
}
```

两次初始模拟功率生成来自创建和窗口尺寸确定；持续选择提示等覆盖层更新后，功率生成数与热力图上传数不变。帧提交数是该次启动时的观测值，不是帧率或吞吐指标。

Qt 渲染截图已经读回复查，主图、热力图、轴线、标记与参数均可见：

| 逻辑窗口尺寸 | 主图有效高度 | 证据 |
|---|---|---|
| 1366 × 768 | 244 DIP | [截图](screenshots/signal-studio-release-gpu-1366x768.png) |
| 1920 × 1080 | 556 DIP | [截图](screenshots/signal-studio-release-gpu-1920x1080.png) |

本机当前屏幕缩放为约 150%；截图包含实际设备像素，例如 1366 × 768 逻辑窗口对应约 2049 × 1152 的 PNG。此处没有修改系统 DPI，也没有把 PNG 像素高度当作逻辑绘图区高度。

## 独立运行与部署

`scripts/verify-package.ps1` 从各自输出目录启动，将 PATH 限制为 Windows 系统路径，清除 Qt/QML 插件相关环境，并明确使用 Windows 平台：

- Debug：`--smoke-test --require-gpu` 退出码 0。
- Release：`--smoke-test --require-gpu` 退出码 0。
- Release：`--smoke-test --software-renderer` 退出码 0，记录为 QPainter 软件绘制、0 次 GPU 上传。

目录中包含 Qt DLL、Windows 平台插件、`qt.conf` 和本机 VS 14.51.36231 的 10 个 x64 CRT DLL。Debug 额外包含 Windows SDK 10.0.26100.0 的 `ucrtbased.dll`。运行验证没有把 Qt 安装目录或其他项目运行目录加入 PATH。

Qt 部署器提示缺少 Direct3D 12 的 `dxcompiler.dll` / `dxil.dll`；本工程当前采用 Direct3D 11，实际启动与渲染均通过。Vulkan SDK 当前未安装，本阶段没有启用 Vulkan。Qt GuiPrivate 的版本绑定提示已通过严格锁定 Qt 6.11.1 处理，升级仍须重新验证。

## 尚未完成的验收

真实 IQ 读取、FFT/STFT、DDC、后台计算、检测、设备输入、大文件性能、Linux 构建部署、Vulkan、GPU 曲线几何及实时瀑布新增行更新均未实现或验证。覆盖层仍由 CPU QPainter 栅格化，再由 GPU 纹理合成；本记录证明加速后端与缓存生效，不声称达到 60/100 FPS。

完整 A1.4.3 的 59 项用例、全套系统捕获生命周期、分隔条回滚/复位和所有布局/刻度细节仍需按 [验收清单](skeleton-checklist.md) 继续推进。当前完成判断为“可独立构建、运行、保存状态并使用 GPU 显示的基础骨架”。
