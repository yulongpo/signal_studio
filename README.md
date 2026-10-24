# Signal Studio

独立开发的 C++20 / Qt 6.11.1 Widgets 离线 IQ 分析应用。窄带 v2 在 `rebuild/a143-native` 基线上由 `feature/narrowband-sampling-constellation-eye` 开发；见[窄带使用说明](docs/implementation/narrowband-constellation-eye-v2/USER_GUIDE.md)及[验收记录](docs/implementation/narrowband-constellation-eye-v2/VALIDATION.md)。主工作区依据 [A1.4.3](docs/prototype/a1.4.3/BASELINE.md)，宽带检测及三来源标记依据已获批的 [M0-P](docs/prototype/wideband-detection-r1/index.html)。旧 Signal Studio、ISA 和训练仓库不参与应用的源码、构建或运行；经授权只读参考的算法能力、显示策略及模型资产另有记录。

当前已完成宽带离线检测，以及获批 U0 方案的标记选择、直接编辑、软删除、通道和持久化改造；U4 的 Debug/Release 完整回归、原生流程、独立部署和规格/质量复审均已通过。当前交付入口为 [U4 交付说明](docs/implementation/annotation-interactions/U4/DELIVERY.md)和[手动验证](docs/implementation/annotation-interactions/U4/MANUAL_VALIDATION.md)，节点提交和复审记录见[标记交互状态](docs/implementation/annotation-interactions/EXECUTION_STATUS.md)。宽带模型部署及历史验证保留在 [R6 交付说明](docs/implementation/wideband-detection/R6/DELIVERY.md)。大量数据效果、完整 L3/L4 与真实 RF 精度仍待手动验证。

本次窄带 v2 的交付入口为[交付说明](docs/implementation/narrowband-constellation-eye-v2/DELIVERY.md)：默认目标 2B、实际采样率规划、真实两图和单载波恢复。两配置35个必要验证项及指定实屏/独立部署已闭环；完整回归中的失败与补测分别记录，提交前执行独立子代理终审。

2026-10-23 反馈修复扩展 M-PSK/M-QAM/M-APSK（含 32QAM、交叉 QAM 和可编辑 APSK 环），明确“同步恢复”入口与阶段回退，修复 Windows 原生样式下上调按钮覆盖，并统一星座/眼图的标题设置条与“更多设置”。范围、条件和当次定向证据见[反馈修复记录](docs/implementation/narrowband-constellation-eye-v2/feedback-2026-10-23/FIX_REPORT.md)，提交前复审与分支顺序见[同步说明](docs/implementation/narrowband-constellation-eye-v2/feedback-2026-10-23/SYNC.md)；原 W8 归档保持原结论，不将后续定向回归混作全量。

## 工程、图谱与标记

启动后新建或打开工程，再添加信号文件。新建工程在任意现有目录直接创建 `.ssproj` 文件，伴随资产默认保存到 `.ssproj.assets/<projectId>/`；演示工程通过文件菜单或 `--demo-data` 显式打开。原生格式 `signal-studio-native-project` 当前为 **v6**，仅接受当前版本，不再导入或迁移 v1–v5；样本索引使用 uint64、JSON 十进制字符串，频率使用 Hz。`.ssproj` 为 magic/schemaVersion=1、稳定 projectId 的 JSON 容器，内嵌 v6 状态和校验资产清单；旧 v6 JSON 从“导入旧工程 v6”显式只读转换。工程通过 `QSaveFile` 原子保存。工程保存文件配置、独立 PSD/STFT 参数、可用前缀、统一标记、来源修订和通道快照，不复制原始样本；重开仍需原始文件与工程附属记录。布局、全局显示偏好和最近工程另由 QSettings 保存。

RAW 导入可配置实数/复数、int8/uint8/int16/int32/float32/float64、字节序、IQ/QI/平面布局、多通道及头尾字节；导入预检确认后只读访问。支持 SigMF 元数据与配套数据，以及本任务生成器的 IQ WAV。文件级采样率、中心频率、有效带宽和物理长度参与坐标换算。顺序加载以 4 MiB 块扫描生成不超过 2048 点的导航包络；停止后只分析可用前缀，不把未读尾部当作噪声。

宽带与窄带具有导航、波形/PSD、时频/瀑布和物理轴交互。显示分析点数表示当前可见频带的分析点数；频带变窄时所需观测增长，观测不足则说明补零量，补零不提高真实分辨率。PSD/STFT 参数与缓存独立；PSD 支持周期图、Bartlett、Welch、DPSS 多窗、Burg 及平均/最大/最小统计。显示抽帧、缩放与已提交检测任务相互独立。

信号导入后有界自动发现可信标签；`data/name.data` 严格配对 `label/name.txt`，未知列式 TXT 必须显式映射索引、单位和频率解释。无标签可手工标记。原始样本标签、手动标记、算法检测结果共用记录、图层、选择、修订与质量判断。原始标签仅适配本任务生成器的 TXT，以及指定 TorchSig 数据的 SigMF annotations；保留原文、来源 hash、量化、原始与当前几何。图、表、左树共享所选对象和当前活动段；全选覆盖当前筛选的全部页，左树按需加载。单击选择不缩放，定位才调整视区。

三来源均可直接编辑：表格双击或 F2、完整范围表单及图形控制点都在同一对象上自动生成修订，原始证据不变。多 TIME 段仍属于同一对象，范围编辑只作用于当前段；时间未定位对象只修改频率。删除先预览对象、段及关联通道数量，再执行可撤销的软删除；一次撤销保留后来发布的后台结果。

质量“待判断 / 已采纳 / 已忽略”为可选判断，不限制编辑或创建通道。统一通道入口一次明确选择一个来源：`LocatedSegment` 绑定当前段并允许裁剪，`UserRange` 保存人工提取范围，不伪造算法活动段。无有效默认时段仍打开配置并给出字段原因，扩大到可用前缀须显式操作。有效配置执行真实 CPU DDC/FIR/有理重采样，只读取可用前缀。通道保留创建快照；名称、备注、质量及无关兄弟段变化不影响来源关系，绑定几何或物理来源变化显示 Changed，删除显示 Missing，撤销恢复后可回到 Current；已有通道不自动重算。

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

构建脚本依次配置、编译、CTest；公共 presets 为 `vs2026-qt611-debug/release`。可执行目录为 `out/vs2026-qt611-debug_bin/` 和 `out/vs2026-qt611-release_bin/`。Windows 自动部署 Qt、插件和完整 CRT；**模型、TensorRT/CUDA DLL 及许可证须独立部署**。完成构建后从对应输出目录启动 `SignalStudio.exe`（可用 `--project <file.ssproj>` 打开工程），独立运行身份与结果见 U4 交付记录。

运行时不需要 Python、训练仓库、Qt 安装路径或 TensorRT SDK 路径，但需要原始样本、工程附属文件、匹配的模型包和 NVIDIA 驱动。当前冻结模型包验证环境为 RTX 5060 Laptop / SM 12.0 / TensorRT 10.11；不承诺其它 GPU 可直接复用序列化引擎，缺少兼容设备或模型时有明确不可用原因。显示软件回退不提供 CPU 模型推理。模型与 CUDA/TensorRT 大资产在被忽略的 `out/`，**Git clone 不包含它们**；准确部署顺序、SHA 和重建入口见 [模型交付](docs/implementation/wideband-detection/R6/MODEL_DEPLOYMENT.md)。

Python 工具优先使用 `D:/profiles/anaconda/python.exe`。节点中间文件写到 `out/tasks/wideband-detection/<node>/{logs,diagnostics,assets,local-builds,deferred}/`，可复用环境写到 `out/toolchains/`；不在 `out/` 根目录堆放任务产物。见 [目录约定](docs/architecture/output-layout.md)。

## 验证与剩余限制

U4 本轮 Debug/Release 均完成 **23/23 CTest、108/108 Qt UI，0 失败、0 跳过**。Debug CTest 合计 518.20 秒、UI 370.81 秒；Release 合计 126.36 秒、UI 69.06 秒，原 UI 480 秒保护不变。Debug 重新构建后 3 个 UI 程序身份变化，已完整重跑。正式模型原生 Debug **43/43**、Release **46/46**；Release 标记 U1/U2/U3 **8/8、10/10、12/12**，覆盖实际 IQ CPU DDC、来源快照、v6 重开及旧版备份。两配置宽/窄带 GPU/软件共 8 种模式的包验证和 smoke 各自全部通过；缺模型和受控无兼容推理设备两情景仍可使用标签、手动标记与历史。详见 [U4 验证](docs/implementation/annotation-interactions/U4/VALIDATION.md)和[原生验收](docs/implementation/annotation-interactions/U4/NATIVE_ACCEPTANCE.md)。

本次窄带 v2 指定 Redmi 27 NU，要求物理 3840×2160、150% DPI、2560×1440 逻辑全屏。验收核对当次 Windows 活动设备友好名、原生设备路径、首选物理模式、连接序号及 Qt DPI；历史 U27U2D / DISPLAY2 和 Redmi / DISPLAY6 不代替当次探针。虚拟 4K 帧缓冲的渲染证据单独标注，不算指定实屏通过。

代表性样本是本机实际读取的 TorchSig 合成基准 SigMF 文件：400000 个 CF32 样本、20 MS/s、20 ms、Fc 2.5 GHz。四条原始标签来自其 meta；标记交互场景中的手动和算法对象为明确的 UI 夹具。正式模型场景另执行真实 FFSCN Sample/ROI 前向，AMD Radeon 880M 的 QRhi D3D11 显示与 NVIDIA RTX 5060 的 TensorRT 推理分开记录。本轮未新增 SCN 前向效果验证；实际 IQ/DDC、UI 夹具或少量模型前向都不表示 RF 实采精度通过。

当前正式模型启动反馈 Debug **566.36 ms**，未达 200 ms 建议目标；Release **134.95 ms**。Release 最终真实任务启动/取消可见反馈约 **113.05/157.97 ms**，取消完成 **179.27 ms**，已发布前缀精确保留。十万合成活动段选择/筛选 P95 约 **84.22/70.47 ms**，两者各 20 次；选择含图表帧，筛选不含图表帧。Debug 未执行最终取消/压力，不宣称全部配置性能通过；准确字段及范围见 [U4 交付说明](docs/implementation/annotation-interactions/U4/DELIVERY.md)。

Windows 显示为 QRhi D3D11：曲线顶点和标量功率纹理/LUT 由 GPU 绘制，坐标与交互覆盖层由 QPainter 合成。GPU 显示、软件显示、CPU DSP、TensorRT 模型分别记录；offscreen UI 测试与模拟图谱不证明模型效果。继续使用既有脚本，输出显式放入新的节点目录；`AnnotationInteractions` 复用 U1–U3 场景，没有 U4 模式：

```powershell
$manualRoot = [IO.Path]::GetFullPath('out/tasks/wideband-detection/U4/diagnostics/manual-v1')
if (Test-Path -LiteralPath $manualRoot) { throw 'Use a new manual validation directory' }
./scripts/verify-package.ps1 -Configuration Release -ReportPath "$manualRoot/package-gpu.json"
./scripts/verify-package.ps1 -Configuration Release -SoftwareRenderer -ReportPath "$manualRoot/package-software.json"
./scripts/gpu_smoke.ps1 -Configuration Release -OutputDirectory "$manualRoot/gpu"
./scripts/screenshot.ps1 -Configuration Release -AnnotationInteractions U3 -OutputDirectory "$manualRoot/native-u3"
```

大量 G/T 配对矩阵和真实 RF 效果由用户交付后手动执行；[模型效果验证入口](docs/implementation/wideband-detection/R6/MANUAL_VALIDATION.md)提供新身份、新目录和结果解释。标记、通道及部署复核使用 [U4 手动验证](docs/implementation/annotation-interactions/U4/MANUAL_VALIDATION.md)。少量代表性功能/流程成功不替代 L3/L4，不覆盖所有统计谱的精度、跨硬件吞吐或受控冷/热启动基准。

窄带 v2 已接入默认目标 2B 与倍率/手工 Fs 规划、真实连续 IQ 星座和眼图，以及 BPSK/QPSK/16QAM 假设下的 Rs 候选、CFO、RRC、定时与相位恢复；细节与未通过项以本次验收记录为准。窄带模型识别与解调仍是明确标示的合成演示。设备输入、持续采集、实际调制分类、位流和 Ubuntu 运行尚未完成；`portable-ninja` 与 OpenGL/Vulkan 接口保留后续跨平台路径。

## 代码边界

| 目录 | 职责 |
|---|---|
| `domain/`、`application/` | 物理坐标、来源/修订、检测契约、跟踪、会话与命令 |
| `infrastructure/` | 样本读取、PSD/STFT、CPU DDC、JSON/分片持久化、worker 通信与模型后端 |
| `app/`、`ui/` | 工程工作流、参数提交、记录/证据、图谱交互与 QRhi 渲染 |
| `tools/detection_worker/` | 隔离运行的原生 TensorRT worker |
| `tools/detection/`、`scripts/` | 模型打包、独立参考/评价、手动套件、构建/部署/截图 |
| `tests/`、`docs/` | 行为/边界回归、冻结原型、架构与逐节点实际证据 |

代码遵循现有命名与 Google 格式。[显示架构](docs/architecture/rendering.md)、[谱参数与前缀加载](docs/architecture/spectral-settings-and-loading.md)、[窄带通道](docs/architecture/narrowband.md)说明当前边界；较早 [UI 基线验收](docs/acceptance/ui-parity.md)保留阶段事实。检测状态见 R1–R6，标记交互和 v6 通道状态见 U1–U4。

本轮 UX-R3 的设计、格式、验证与交付索引见 [UX-R3 计划](docs/implementation/ux-r3/PLAN.md)。窄带新增独立星座图与眼图导航页，共享真实分析状态；识别/解调示例仍与真实模型能力分开。验证结果以本轮 VALIDATION.md 为准。
