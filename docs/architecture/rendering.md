# 图谱渲染后端

本阶段选择 Qt Widgets + QRhiWidget，将后端与图谱交互、业务坐标及工程状态分开。该选择按 Windows 与 Ubuntu 的后续交付目标确定。用户给出的[“介绍 Direct2D”选型会话](https://chatgpt.com/share/6ac7b6db-aa24-83e8-9334-526fa8dc83da)是决策背景；本轮已确认采用 QRhi。

## 平台与兼容范围

Windows 使用 QRhi Direct3D11；Ubuntu 预留 Qt 默认 OpenGL 后端，Vulkan 可在后续环境验证后选择。同一顶层窗口的图谱保持同一种 API，避免窗口合成冲突。Qt 官方说明了这些默认后端及单窗口约束，见 [QRhiWidget](https://doc.qt.io/qt-6.11/qrhiwidget.html)。

QRhiWidget 属于公开 Widgets API，但其使用的 QRhi/QShader 等 Gui 类没有通常的源码与二进制兼容保证，需要 `Qt6::GuiPrivate`。本仓库以 `find_package(Qt6 6.11.1 EXACT ...)` 锁定开发、ShaderTools 与运行库版本；升级 Qt 后须整体重新编译并验证各后端。依据见 [QRhiWidget 的兼容说明](https://doc.qt.io/qt-6.11/qrhiwidget.html)。

Windows 的 VS 2026 presets 与输出部署用于本机验证。`portable-ninja` 提供无 VS 路径的配置入口，以 `SS_QT_ROOT` 找到平台对应的 Qt 6.11.1 安装；Ubuntu 构建、插件部署、驱动兼容与 GPU 验收尚未在本阶段完成。

```bash
export SS_QT_ROOT=/path/to/Qt/6.11.1/gcc_64
cmake --preset portable-ninja
cmake --build --preset portable-ninja
ctest --preset portable-ninja
```

Ninja 构建目录为 `out/portable-ninja`，Debug 可执行目录为 `out/portable-ninja-debug_bin`。需要 CMake 4.2+、Ninja、C++20 编译器及同版本 Qt 私有开发头文件和 ShaderTools；Linux 依赖部署后续单独实现。

## 当前绘制路径

图谱保留物理时间/频率业务坐标。显示层生成模拟图谱及覆盖层，QPainter 先将当前内容绘制到 RGBA 图像，再通过 QRhi 纹理上传和采样管线显示。文字、网格、标记边界与曲线目前仍含 CPU 栅格化工作；GPU 验收证明 QRhi 驱动、纹理上传与合成生效，不证明整条绘图流水线已转换为 GPU 几何计算。

顶点与片元着色器位于 `ui/charts/shaders/`，分别由应用与 UI 测试的独立 `qt_add_shaders` 资源目标生成并内置为 `:/signalstudio/shaders/texture.vert.qsb` 和 `texture.frag.qsb`。同版本 Qt 的默认 qsb 配置包含 SPIR-V、GLSL、HLSL 5.0 和 MSL。当前不启用 `PRECOMPILE`，无需为普通 shader 构建额外要求 FXC 位于 PATH；后续可明确启用原生预编译。相关命令说明见 [qt_add_shaders](https://doc.qt.io/qt-6/qt-add-shaders.html)，本机生成规则以 Qt 6.11.1 的 `Qt6ShaderToolsMacros.cmake` 为准。

## 下一阶段边界

曲线可以转为顶点缓冲与独立 QRhi 管线，使时间范围变化主要更新坐标变换，数据变化才上传必要的曲线片段。时频/瀑布可以使用分级瓦片、可见区域纹理与环形历史，新帧只更新新增行；缩放期间提供对应分辨率的纹理，避免反复上传整幅高分辨率图像。

IQ 读取、FFT/STFT、DDC 和后续检测在计算层实施，由工作线程提供不可变显示结果。GUI/RHI 线程只接收结果、处理资源更新与提交绘制，不在交互事件中执行整段 DSP。请求携带文件和视图代次，迟到结果不得替换更新后的显示；输入、计算队列与图形资源各自保留明确容量和释放生命周期。

## 验收证据

基础 CTest UI 用例在 offscreen 软件回退模式下验证交互，不作为 GPU 成功证据。Windows 的 `scripts/gpu_smoke.ps1`、`verify-package.ps1` 与 `screenshot.ps1` 默认传入 `--require-gpu`：实际 QRhi 驱动必须不是 CPU，且纹理上传计数大于零。`-SoftwareRenderer` 专门验证 QWidget 回退并明确记录软件结果。GPU 运行记录应包含 Qt 版本、实际 backend、驱动/设备名称、上传计数、退出码与截图；新后端或 Qt 升级后重新收集。
