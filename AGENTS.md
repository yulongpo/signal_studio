# Signal Studio 开发约定

面向用户默认使用简体中文；命令、代码及技术标识使用英文。

## 依据与独立性

- UI 与交互基线为 A1.4.3。冻结副本位于 `docs/prototype/a1.4.3/`，来源路径、文件大小与 SHA-256 记录在该目录的 `source_manifest.json`。
- 采用全新 C++20 / Qt 6.11.1 Widgets 工程；不读取、复制或链接旧 Signal Studio 或 ISA 的业务源码。构建输入必须来自本仓库与明确安装的开发工具。
- 不将外部原型目录作为构建输入；原型 HTML 和演示 JSON 是产品依据及互操作样例。归档 README 中的原型测试结果属于原型，不属于本仓库 Qt 工程的验证。

## 当前阶段

- 建立主窗口、工程树、全局导航、辅助图、主图、右侧参数与底部结果区域；保持 A1.4.3 布局和物理时间/频率坐标语义。
- 建立与 Qt 无关的 domain/application 状态层、Qt JSON 工程持久化层，以及模拟图谱与交互层。工程 JSON 导入须完整校验后替换状态。
- 图谱显示采用 QRhiWidget 的纹理与 QPainter 交互覆盖层，Windows 使用 QRhi Direct3D11。预留 Ubuntu 默认 OpenGL 与后续 Vulkan 后端；同一窗口内使用同一种图形 API。提供明确的 QWidget 软件显示回退。模拟图谱数据生成不等于 GPU DSP 计算。
- 当前 QPainter 内容转为 RGBA 图像后上传 QRhi 纹理，不属于完整 GPU 几何绘制。后续曲线顶点缓冲、瓦片/纹理行增量更新及异步计算边界见 `docs/architecture/rendering.md`。
- 本阶段使用模拟元数据与模拟图谱；真实 IQ 读取、FFT/STFT、DDC、信号检测、设备输入及 DSP 性能验证另行实施。不得把占位或模拟结果描述为真实分析能力。

## 构建与验证

- 公共 Windows CMake presets：`vs2026-qt611-debug`、`vs2026-qt611-release`；跨平台预设 `portable-ninja` 面向后续 Ubuntu 验证。Qt 根路径由 `SS_QT_ROOT` 指定。本机路径仅放在被忽略的 `CMakeUserPresets.json` 中。
- Windows 工具链为 Visual Studio 18 2026、x64、C++20。QRhi 的 GuiPrivate 兼容范围有限，CMake 锁定 Qt 6.11.1 EXACT，并使用同版本 ShaderTools 生成内置 `.qsb`。升级 Qt 必须重新编译并重跑图形验证。
- `scripts/build.ps1` 默认完成 Windows 配置、构建、CTest；每步检查退出码。`SS_BUILD_TESTS=OFF` 可关闭测试。Ninja 预设不包含 Linux 部署实现或已通过的 Linux 验收声明。
- Debug / Release 可执行目录分别为 `out/vs2026-qt611-debug_bin` 与 `out/vs2026-qt611-release_bin`，Windows 构建自动运行 `windeployqt`。
- 测试使用本仓库 `tests/`；不运行外部原型测试来证明 Qt 工程通过。构建、测试、独立启动及截图分别记录真实结果与未验证边界。
- 独立启动验收必须从对应输出目录运行，并确认无需外部源码目录及 Qt 安装目录参与运行。
- `scripts/screenshot.ps1`、`scripts/verify-package.ps1` 和 `scripts/gpu_smoke.ps1` 默认通过 Windows 平台与 `--require-gpu` 验证；成功条件必须包含非 CPU QRhi 驱动及至少一次纹理上传。`-SoftwareRenderer` 明确切换软件回退验证并标明软件结果。
- CTest 的基础 UI 测试使用 offscreen 与 QWidget 回退验证交互，不证明 GPU 加速。GPU smoke 独立执行并记录本机驱动及实际 renderer 证据。

## 变更纪律

搜索优先使用 `rg`；独立查询可并行，共享状态和连续决策由同一 Agent 处理。测试应覆盖行为和数据边界，避免复述实现的低价值测试。除用户明确授权外不提交、不推送。
