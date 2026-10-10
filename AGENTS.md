# Signal Studio 开发约定

面向用户默认使用简体中文；命令、代码及技术标识使用英文。

## 依据与独立性

- UI 与交互基线为 A1.4.3。冻结副本位于 `docs/prototype/a1.4.3/`，来源路径、文件大小与 SHA-256 记录在该目录的 `source_manifest.json`。
- 采用全新 C++20 / Qt 6.11.1 Widgets 工程；不复制或链接旧 Signal Studio 或 ISA 的业务源码。用户指定 `ISA_算法中心重构开发` 后，允许只读参考 `D:/project/SCN_based_PSD_Detection` 的图谱抽取、预览及缓存策略，相关实现仍在本仓库独立编写。构建输入必须来自本仓库与明确安装的开发工具。
- 不将外部原型目录作为构建输入；原型 HTML 和演示 JSON 是产品依据及互操作样例。归档 README 中的原型测试结果属于原型，不属于本仓库 Qt 工程的验证。

## 当前阶段

- 完整对齐 A1.4.3 主窗口、工程树、全局导航、辅助图、主图、右侧参数与底部结果区域及交互；保持物理时间/频率坐标语义。适配性验收仅使用第二块连接显示器的 4K 物理尺寸 3840×2160、150% DPI（2560×1440 逻辑工作区），不继续进行 1366×768 等尺寸/DPI 验证。本机第二块连接屏为 Redmi 27 NU，Windows 原生设备路径为 DISPLAY6；不能把设备路径后缀误当当前连接序号。原生验收必须使用该屏全屏窗口、实际屏幕 DPI，报告包含连接序号、友好名称、原生设备路径与尺寸；禁止以主屏超大窗口截图替代。
- 建立与 Qt 无关的 domain/application 状态层、Qt JSON 工程持久化层，以及模拟和真实 IQ 图谱交互。工程 JSON 导入须完整校验后替换状态。
- 图谱显示采用 QRhiWidget 的纹理与 QPainter 交互覆盖层，Windows 使用 QRhi Direct3D11。预留 Ubuntu 默认 OpenGL 与后续 Vulkan 后端；同一窗口内使用同一种图形 API。提供明确的 QWidget 软件显示回退。模拟图谱数据生成不等于 GPU DSP 计算。
- 图谱数据使用 QRhi 顶点缓冲及标量热图纹理/LUT 绘制；坐标、文字、网格和游标使用 QPainter 覆盖层。不得把仅上传 CPU 整图描述为 GPU 数据绘制。实现与资源更新边界见 `docs/architecture/rendering.md` 和 `docs/architecture/linked-spectral-analysis.md`。
- 应用启动时显示工程工作流入口；新建工程必须创建独立目录并立即写入 `project.json`，可从目录打开工程，同时兼容导入旧版单 JSON。文件菜单提供打开演示工程，三份原型演示文件也可由显式 `--demo-data` 或测试夹具加载。工程就绪后添加/打开信号，再进入宽带图谱操作。交替小端 int16 IQ 支持 I、Q、IQ RMS 与峰值包络波形切换、可设置的中心有效带宽、5–16 阶 STFT、PSD/STFT 与图谱抽取；项目 JSON 保存文件级有效带宽、视图、标记和工程结构，右侧栏显示/辅助图设置由 QSettings 全局持久化并跨文件共用（有效带宽仍按文件保存）。QSettings 也保存窗口/侧栏/分组布局及最近工程。窄带工作区已有真实 CPU DDC/FIR/重采样、波形、PSD/STFT，以及明确标示的合成星座、眼图、识别和解调示例。宽窄带 N 均表示可见频段分析点数；缩小频段增加实际帧时长，不足时补零并显示有效观测与补零量；补零不提高真实分辨率。PSD/STFT 参数和缓存独立，窄带默认全通道 Welch 平均谱，短 STFT 至少 10 帧。原生工程版本 3 保存独立参数、物理长度和可用前缀，继续导入版本 1/2。实际 IQ 顺序扫描 4 MiB 块并生成不超过 2048 点的峰值导航包络；停止后只分析已读前缀。游标按文件/通道隔离、驻留帧 PSD 直接复用实际 STFT 帧。信号检测、设备输入、真实同步/模型/解调、Ubuntu 运行及持续采集吞吐仍未实现。不得把占位或模拟结果描述为真实分析能力。

## 构建与验证

- 公共 Windows CMake presets：`vs2026-qt611-debug`、`vs2026-qt611-release`；跨平台预设 `portable-ninja` 面向后续 Ubuntu 验证。Qt 根路径由 `SS_QT_ROOT` 指定。本机路径仅放在被忽略的 `CMakeUserPresets.json` 中。
- Windows 工具链为 Visual Studio 18 2026、x64、C++20。QRhi 的 GuiPrivate 兼容范围有限，CMake 锁定 Qt 6.11.1 EXACT，并使用同版本 ShaderTools 生成内置 `.qsb`。升级 Qt 必须重新编译并重跑图形验证。
- `scripts/build.ps1` 默认完成 Windows 配置、构建、CTest；每步检查退出码。`SS_BUILD_TESTS=OFF` 可关闭测试。Ninja 预设不包含 Linux 部署实现或已通过的 Linux 验收声明。
- Debug / Release 可执行目录分别为 `out/vs2026-qt611-debug_bin` 与 `out/vs2026-qt611-release_bin`，Windows 构建自动运行 `windeployqt`。
- Windows 部署从生成器实际选择的 Visual Studio 安装中探测最新完整 x64 CRT 并逐个复制 DLL；Debug 额外从 Windows SDK 复制 `ucrtbased.dll`。缺少完整运行库时部署失败，不用本机预装 VC 运行库代替输出目录的交付内容。
- 测试使用本仓库 `tests/`；不运行外部原型测试来证明 Qt 工程通过。构建、测试、独立启动及截图分别记录真实结果与未验证边界。
- 独立启动验收必须从对应输出目录运行，并确认无需外部源码目录及 Qt 安装目录参与运行。
- `scripts/screenshot.ps1`、`scripts/verify-package.ps1` 和 `scripts/gpu_smoke.ps1` 默认通过 Windows 平台与 `--require-gpu` 验证；成功条件必须包含非 CPU QRhi 驱动及至少一次纹理上传。`-SoftwareRenderer` 明确切换软件回退验证并标明软件结果。
- CTest 的基础 UI 测试使用 offscreen 与 QWidget 回退验证交互，不证明 GPU 加速。GPU smoke 独立执行并记录本机驱动及实际 renderer 证据。

## 变更纪律

搜索优先使用 `rg`；独立查询可并行，共享状态和连续决策由同一 Agent 处理。测试应覆盖行为和数据边界，避免复述实现的低价值测试。除用户明确授权外不提交、不推送。
