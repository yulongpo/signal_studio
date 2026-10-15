# Signal Studio

独立开发的 C++20 / Qt 6.11.1 Widgets 离线 IQ 分析应用，当前分支为 `rebuild/a143-native`。主工作区依据 [A1.4.3](docs/prototype/a1.4.3/BASELINE.md)，宽带检测及三来源标记依据已获批的 [M0-P](docs/prototype/wideband-detection-r1/index.html)。旧 Signal Studio、ISA 和训练仓库不参与应用的源码、构建或运行；经授权只读参考的算法能力、显示策略及模型资产另有记录。

截至 2026-10-15，宽带检测适配、标记/复核和原生功能流程已实现并验证。实际环境为 Windows x64 / Visual Studio 18 2026 / Qt 6.11.1；大量数据效果、完整 L3/L4 与真实 RF 精度仍待手动验证。完整交付状态见 [R6 交付说明](docs/implementation/wideband-detection/R6/DELIVERY.md)，逐节点历史见 [执行状态](docs/implementation/wideband-detection/EXECUTION_STATUS.md)。

## 工程、图谱与标记

启动后新建或打开工程，再添加信号文件。新建工程立即创建独立目录和 `project.json`；演示工程通过文件菜单或 `--demo-data` 显式打开。原生格式 `signal-studio-native-project` 当前为 **v5**，兼容 v1–v4；样本索引使用 uint64、JSON 十进制字符串，频率使用 Hz。工程保存文件配置、独立 PSD/STFT 参数、可用前缀、统一标记、来源修订和通道快照，不复制原始样本；重开仍需原始文件与工程附属检测记录。布局、全局显示偏好和最近工程另由 QSettings 保存。

RAW 导入可配置实数/复数、int8/uint8/int16/int32/float32/float64、字节序、IQ/QI/平面布局、多通道及头尾字节；导入预检确认后只读访问。支持 SigMF 元数据与配套数据，以及本任务生成器的 IQ WAV。文件级采样率、中心频率、有效带宽和物理长度参与坐标换算。顺序加载以 4 MiB 块扫描生成不超过 2048 点的导航包络；停止后只分析可用前缀，不把未读尾部当作噪声。

宽带与窄带具有导航、波形/PSD、时频/瀑布和物理轴交互。显示分析点数表示当前可见频带的分析点数；频带变窄时所需观测增长，观测不足则说明补零量，补零不提高真实分辨率。PSD/STFT 参数与缓存独立；PSD 支持周期图、Bartlett、Welch、DPSS 多窗、Burg 及平均/最大/最小统计。显示抽帧、缩放与已提交检测任务相互独立。

原始样本标签、手动标记、算法检测结果共用记录、图层、选择、修订与复核。原始标签仅适配本任务生成器的 TXT，以及指定 TorchSig 数据的 SigMF annotations；保留原文、来源 hash、量化、原始与当前几何。多 TIME 段仍属于同一对象；图层开关/筛选不改记录，接受/拒绝不复制对象。确认同一对象的活动段后，可创建真实 CPU DDC/FIR/有理重采样通道；已有通道保留来源快照，后续修订显示 Changed，不静默重算。

参数只在 Enter、Tab/Shift+Tab 或应用内切换焦点后提交；输入中、后台刷新及窗口失焦不应用草稿。输入框、下拉框和弹出列表禁止滚轮改值，图谱缩放和侧栏滚动保留。原型对照及获准修订见 [R4 原型闭环](docs/implementation/wideband-detection/R4/PROTOTYPE_CLOSURE.md)。

## 宽带离线检测

FFSCN GUI IQ 与 SCN 通过独立 `SignalStudioDetectionWorker.exe` 执行真实 TensorRT 推理。完整文件、可见范围、ROI 和活动段按提交时的物理范围生成不可变任务；取消保留已发布前缀，失败与正常零结果分开，任务、候选、来源及证据可保存重开。Sample、ROI、Coarse 时间处理保留实际观测支持、未知区间及精修回退，不把频率观察直接宣称为精确活动段。

| 参数 | 实际作用 |
|---|---|
| 检测 FFT / N | 分析频率网格；不规则长度先匹配计算 FFT，再按物理频率适配模型网格 |
| 谱步长 / hop | 真实谱帧的起始间隔，独立于 FFT 帧观测长度 |
| 窗口帧数 / T | 请求积累的真实谱帧数；短文件、ROI、尾部使用实际可得帧，不复制帧补足默认值 |
| 滑动帧数 | 相邻检测窗口在谱帧序列上的起始间隔 |
| 谱类型 | 当前谱、线性功率平均谱、最大谱、最小谱均可检测；默认 FFSCN 使用逐帧输入，默认 SCN 最大保持 |
| 模型规格 | 张量布局、结构对齐、head 步幅和优化 profile 由后端/模型包管理；不是用户恢复参考参数的门槛 |

主 FFSCN 与 SCN 均采用 **`down_nums=11`，模型频率长度为 2 的整数次幂且至少 `2048`**。FFSCN 取不小于分析长度和 2048 的最小 2 次幂；显式统计谱合成为一行。SCN 当前可用 engine 固定输入 32768 点，内部按所选谱进行线性功率重分箱并将预测映射回原始 Hz；32768 不是算法的结构下限。平均谱在线性功率域积累后转回 dB，同样可用于检测和辅助分析。

当前动态 FFSCN 包的两档资源范围是 `T=1–64,F=2048–65536` 和 `T=1–16,F=2048–262144`，批量最多 2；这是实际部署 profile 的覆盖范围，不是网络必须使用 10 帧的结论。后端记录所选 profile、实际帧数、模型网格、适配和预算；资源不足优先减小执行批量，确实不能执行时给出具体原因。没有训练数据效果依据支持所有参数组合的精度保证。详见 [输入适配](docs/implementation/wideband-detection/R2/INPUT_ADAPTATION.md)、[SCN 资产边界](docs/implementation/wideband-detection/R2/SCN_CAPABILITY.md) 和 [短数据流程](docs/implementation/wideband-detection/R3/FLOW_ADAPTATION.md)。

## 构建、模型部署与运行

Windows 要求 VS 2026 x64、CMake、Qt **6.11.1 EXACT**（含 ShaderTools/GuiPrivate）。其它机器设置 `SS_QT_ROOT`；本机路径仅保存在忽略的 `CMakeUserPresets.json`。正式推理构建还须在配置前设置 `SS_DETECTION_TRT_ROOT` 为完整的 TensorRT **10.11.0.33 / CUDA 12.9** SDK 根目录。未配置 SDK 的构建可用图谱和标记，但没有正式模型推理能力。

```powershell
$env:SS_QT_ROOT = 'D:/softwares/Qt/6.11.1/msvc2022_64'
$env:SS_DETECTION_TRT_ROOT = 'D:/project/signal_studio_rebuild/out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11'
./scripts/build.ps1 -Configuration Debug
./scripts/build.ps1 -Configuration Release
./scripts/run.ps1 -Configuration Release
```

构建脚本依次配置、编译、CTest；公共 presets 为 `vs2026-qt611-debug/release`。可执行目录为 `out/vs2026-qt611-debug_bin/` 和 `out/vs2026-qt611-release_bin/`。Windows 自动部署 Qt、插件和完整 CRT；**模型、TensorRT/CUDA DLL 及许可证须独立部署**。本机两套输出已部署并验证，可直接从对应目录启动 `SignalStudio.exe`。

运行时不需要 Python、训练仓库、Qt 安装路径或 TensorRT SDK 路径，但需要原始样本、工程附属文件、匹配的模型包和 NVIDIA 驱动。当前冻结模型包验证环境为 RTX 5060 Laptop / SM 12.0 / TensorRT 10.11；不承诺其它 GPU 可直接复用序列化引擎，缺少兼容设备或模型时有明确不可用原因。显示软件回退不提供 CPU 模型推理。模型与 CUDA/TensorRT 大资产在被忽略的 `out/`，**Git clone 不包含它们**；准确部署顺序、SHA 和重建入口见 [模型交付](docs/implementation/wideband-detection/R6/MODEL_DEPLOYMENT.md)。

Python 工具优先使用 `D:/profiles/anaconda/python.exe`。节点中间文件写到 `out/tasks/wideband-detection/<node>/{logs,diagnostics,assets,local-builds,deferred}/`，可复用环境写到 `out/toolchains/`；不在 `out/` 根目录堆放任务产物。见 [目录约定](docs/architecture/output-layout.md)。

## 验证与剩余限制

本次 R5 Debug/Release 各 **23/23 CTest、94/94 Qt UI**；原生 Debug **43/43**、Release **46/46**，包含真实模型→统一标记→复核→CPU DDC→保存重开。规定屏幕为连接屏 2 Redmi 27 NU / DISPLAY6，3840×2160 物理、150% DPI、全屏。原始报告、截图、身份和边界见 [R5 自检](docs/implementation/wideband-detection/R5/SELF_REVIEW.md)。

Release 可见启动/取消反馈约 104–141/165ms，十万合成活动段选择/筛选 P95 约 69/61ms。Debug 一次启动反馈 **921.536ms，未达 200ms 建议目标**，Debug 未执行最终取消/压力；不宣称全部配置性能通过。历史原子写入故障的底层原因仍不可追溯，新实现通过受控短时/永久冲突和一个原失败文件的完整流程复验；该文件本次活动段为 0，不能当作精度证据。

Windows 显示为 QRhi D3D11：曲线顶点和标量功率纹理/LUT 由 GPU 绘制，坐标与交互覆盖层由 QPainter 合成。GPU 显示、软件显示、CPU DSP、TensorRT 模型分别记录；offscreen UI 测试与模拟图谱不证明模型效果。继续使用既有脚本，输出显式放入节点目录：

```powershell
./scripts/verify-package.ps1 -Configuration Release -ReportPath out/tasks/wideband-detection/R6/diagnostics/package-gpu.json
./scripts/verify-package.ps1 -Configuration Release -SoftwareRenderer -ReportPath out/tasks/wideband-detection/R6/diagnostics/package-software.json
./scripts/gpu_smoke.ps1 -Configuration Release -OutputDirectory out/tasks/wideband-detection/R6/diagnostics/gpu
./scripts/screenshot.ps1 -Configuration Release -WidebandDetection -ProductionModel -FinalAcceptance -OutputDirectory out/tasks/wideband-detection/R6/diagnostics/native
```

大量 G/T 配对矩阵和真实 RF 效果由用户交付后手动执行；[手动验证入口](docs/implementation/wideband-detection/R6/MANUAL_VALIDATION.md) 提供新身份、新目录和结果解释。本次代表性功能/流程成功不替代 L3/L4，不覆盖所有统计谱的精度、跨硬件吞吐或受控冷/热启动基准。

设备输入、持续采集、真实同步、Ubuntu 运行尚未完成；窄带识别/解调、星座和眼图仍为明确标示的合成演示。当前真实窄带能力为 CPU DDC/FIR/重采样、波形和谱分析。`portable-ninja` 与 OpenGL/Vulkan 接口保留后续跨平台路径，没有已通过的 Ubuntu 部署声明。

## 代码边界

| 目录 | 职责 |
|---|---|
| `domain/`、`application/` | 物理坐标、来源/修订、检测契约、跟踪、会话与命令 |
| `infrastructure/` | 样本读取、PSD/STFT、CPU DDC、JSON/分片持久化、worker 通信与模型后端 |
| `app/`、`ui/` | 工程工作流、参数提交、记录/证据、图谱交互与 QRhi 渲染 |
| `tools/detection_worker/` | 隔离运行的原生 TensorRT worker |
| `tools/detection/`、`scripts/` | 模型打包、独立参考/评价、手动套件、构建/部署/截图 |
| `tests/`、`docs/` | 行为/边界回归、冻结原型、架构与逐节点实际证据 |

代码遵循现有命名与 Google 格式。较早的 [显示架构](docs/architecture/rendering.md)、[谱参数与前缀加载](docs/architecture/spectral-settings-and-loading.md)、[UI 基线验收](docs/acceptance/ui-parity.md) 保留阶段事实；当前检测状态以 R1–R6 记录为准。
