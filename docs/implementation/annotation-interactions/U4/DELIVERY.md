# U4 标记交互与窄带通道交付说明

2026-10-19，分支 `rebuild/a143-native`，U4 实现基线为 U3 `c182b3e`。
本轮依据用户确认的 [U0–U4 计划](../PLAN.md)，汇总选择、直接编辑、
删除、可选质量、通道及工程 v6；不改变宽带模型、输入适配、检测参数
或 worker 协议。节点状态由[执行状态](../EXECUTION_STATUS.md)记录。

本轮运行验证全部完成：Debug/Release 完整回归、指定外接屏原生流程、
真实 FFSCN/CPU DDC、GPU/软件显示、独立部署和不可用情景均有归档。
总索引见[验证记录](VALIDATION.md)、[原生验收](NATIVE_ACCEPTANCE.md)、
[机器汇总](validation-summary.json)及[产物身份](artifact-manifest.json)。
最终[规格/质量复审](REVIEW.md)已分别得到 SPEC_PASS 和 QUALITY_PASS。
U4 的文档与证据纳入本节点提交；U0–U4 依序提交完成后，统一推送
`origin/rebuild/a143-native`，远端提交身份以 Git 推送及随后的核对为准。

## 交付行为

| 范围 | 当前行为 |
|---|---|
| 选择与展示 | 图、表、左树共享所选 ID、当前对象/段、锚点及悬停；选择限定当前文件/样本通道和筛选，跨页保留；全选覆盖全部匹配，左树按需加载；单击不缩放，定位才改变视区 |
| 直接编辑 | 原始标签、手动及算法对象同等编辑；表格双击/F2、完整范围表单和图形草稿均生成同 ID 的自动修订；原始来源和模型证据不变；多段只改当前段，未知时间只改频率 |
| 草稿与参数 | 输入中和后台刷新不提交；Enter、Tab/Shift+Tab 或应用内切换焦点提交；窗口失焦不提交，切文件/通道/工程取消草稿；滚轮不改控件值；输入框 Delete 只编辑文字 |
| 删除与撤销 | 预览对象、活动段及关联通道数量；筛选内一次批量软删除，可一次撤销；最后一段按对象删除；撤销不移除后续后台发布结果；通道及创建快照保留 |
| 可选质量 | 显示“待判断 / 已采纳 / 已忽略”，供筛选与复查；不限制直接编辑或创建通道；几何变化恢复待判断，名称和备注编辑保留质量 |
| 统一通道入口 | 图、表、左树共用配置；多选须明确选择一个来源；待判断、已忽略、多段当前段和未知时间对象均可进入配置，执行仍校验物理来源、可用范围和 DSP 计划 |
| `LocatedSegment` | 保存完整绑定段和实际提取范围，提取可裁剪时间/频率且必须在绑定范围内；裁剪本身不使来源关系失效 |
| `UserRange` | 无定位段时，默认可见时段与可用前缀交集；人工范围独立保存，不伪造 `segmentId` 或算法活动段；空交集仍显示表单和字段原因，扩大范围须显式使用前缀 |
| 来源关系 | 名称、备注、质量及无关兄弟段变化不使通道失效；绑定几何或物理来源变化为 Changed，绑定对象/段缺失为 Missing，撤销恢复相同绑定可回到 Current；不自动重算 |
| CPU DSP | 实际复数 IQ → CPU DDC → FIR → 有理重采样；Loading/Partial 非空已读前缀可执行；计划、处理器及缓存均限制在可用前缀，空前缀、Failed、越界或来源冲突给出具体原因 |
| 工程保存 | 原生 v6 保存独立参数、物理长度、可用前缀、原始证据、修订及两类通道快照；继续加载 v1–v5；首次覆盖旧版工程保留原始字节备份，再原子发布；旧通道不按新标记重算 |

源码依据为 `application/annotation_service.*`、`application/session.*`、
`app/main_window.cpp`、`ui/annotations/annotation_panel.cpp`、
`ui/parameter_input.h`、`infrastructure/channel_processor.*`、
`infrastructure/annotation_json.cpp` 和 `infrastructure/project_store.cpp`。
当前架构边界见[窄带通道](../../../architecture/narrowband.md)。

## 节点与验证结果

| 节点 | 已归档结果 |
|---|---|
| U0 `6615b33` | 用户已确认局部交互原型；61/61 状态行为、59/59 浏览器；仅合成演示，见[验证记录](../U0/VALIDATION.md) |
| U1 `3384e17` | Debug CTest 23/23、Qt UI 104/104、原生 8/8；SPEC_PASS / QUALITY_PASS，见[验证记录](../U1/VALIDATION.md) |
| U2 `f2ec417` | 最终完整 Qt UI 108/108、原生 10/10；22 项非 UI 程序以身份未变保留原通过结果；SPEC_PASS / QUALITY_PASS，见[验证记录](../U2/VALIDATION.md) |
| U3 `c182b3e` | Debug CTest 23/23、Qt UI 108/108、原生 12/12、3 次实际 IQ CPU DDC；SPEC_PASS / QUALITY_PASS，见[验证记录](../U3/VALIDATION.md) |

U4 使用现有 `build.ps1`、CTest runner、`screenshot.ps1`、
`verify-package.ps1` 和 `gpu_smoke.ps1`。本轮完成 Debug 正式模型
43 场、Release 最终正式模型 46 场，另在 Release 复跑 U1、U2、U3 的
8/10/12 个标记交互场景。Debug 没有复跑本轮 U1–U3。
脚本没有 `-AnnotationInteractions U4`。Qt UI 的 480 秒终止保护不变。

| U4 检查 | 当前结果 | 最终证据位置 |
|---|---|---|
| Debug 配置、构建与完整 CTest | **23/23**，CTest 合计 **518.20 秒**；本轮完整重跑 | [Debug CTest](logs/debug-test-r1.txt) |
| Debug 完整 Qt UI | **108/108**，0 失败、0 跳过；Qt **366.142 秒**、CTest UI **370.81 秒**，低于 480 秒保护 | [Debug Qt 日志](logs/ui-debug.txt) |
| Release 配置、构建与完整 CTest | **23/23**，CTest 合计 **126.36 秒** | [Release 全流程](logs/release-all-r1.txt) |
| Release 完整 Qt UI | **108/108**，0 失败、0 跳过；Qt **68.338 秒**、CTest UI **69.06 秒** | [Release Qt 日志](logs/ui-release.txt) |
| Debug 正式模型原生流程 | **43/43**，实际 FFSCN、CPU DDC、来源快照及保存重开 | [Debug 报告](native/native-debug-r1/wideband-detection-report.json) |
| Release 正式模型最终流程 | **46/46**，另含真实任务取消及合成十万段压力 | [Release 报告](native/native-release-r1/wideband-detection-report.json) |
| Release 标记原生 U1 / U2 / U3 | **8/8、10/10、12/12**，均在当前指定外接屏；U3 有 3 次实际 CPU DDC | [U1](native/native-release-u1-r1/annotation-interactions-report.json)、[U2](native/native-release-u2-r1/annotation-interactions-report.json)、[U3](native/native-release-u3-r1/annotation-interactions-report.json) |
| 代表性 IQ / CPU DDC / v6 重开 | 两配置正式流程各 16384 个实际 DDC 样本；Release U3 另验 8000/8192/8192、v6 重开、v5 原字节备份、未知时间及快照 | [原生验收](NATIVE_ACCEPTANCE.md)与各报告内数量、能量、SHA-256 和工程分片 |
| 真实模型前向 | 每配置的 FFSCN Sample 与 ROI 均各处理 3 个真实窗口；本节点未新增 SCN 前向 | 两份正式模型原生报告；不使用交互夹具替代实际前向 |
| GPU 显示、软件回退与独立部署 | Debug/Release × 宽/窄带 × GPU/软件共 **8 种模式**，包验证 **8/8**、smoke **8/8**；均从 `_bin`、system-only PATH 启动 | [全部显示证据](renderer/)、[宽带 GPU 包](renderer/package-release-wideband-gpu.json)、[窄带软件包](renderer/package-release-narrowband-software.json) |
| Release 可用性 | **2/2**：缺模型、以 `CUDA_VISIBLE_DEVICES=-1` 受控隐藏 CUDA 设备；标签、手动标记和历史仍可用，模型未执行 | [缺模型](availability/missing-models/wideband-availability-report.json)、[无兼容设备](availability/no-compatible-gpu/wideband-availability-report.json) |
| 输入和部署身份 | 384 项身份在全部运行后保持不变：237 项构建输入、两配置共 52 个程序、95 项 runtime/model 资产 | [冻结身份](diagnostics/input-freeze.json)、[最终复核](diagnostics/input-freeze-verified.json)、[产物清单](artifact-manifest.json) |
| 原型及检测契约 | M0-P 22 项、获批 U0、检测实现及 worker 协议不变 | [冻结契约](diagnostics/frozen-contracts.json) |
| 独立规格与质量复审 | **SPEC_PASS / QUALITY_PASS**，无必须修复项；覆盖本轮完整实现和最终证据 | [复审记录](REVIEW.md)、[执行状态](../EXECUTION_STATUS.md) |

U4 开始时核对的 45 项 U3 构建输入与 26 个程序身份一致；随后 Debug
配置/构建重新生成 3 个 UI 程序，因此重新运行完整 Debug CTest，
没有以 U3 结果代替本轮验证。最终冻结的 384 项在全部回归、原生和
部署运行结束后逐项保持不变。518.20 秒是完整 Debug CTest 合计，
不是单个 UI 测试耗时；UI 的 CTest 计时为 370.81 秒，原 480 秒保护不变。

原始中间产物写入 `out/tasks/wideband-detection/U4/`，最终必要日志、
JSON、截图及引用分片归档本目录。历史 U1–U3 和 R1–R6 不重写，
本轮失败、超时或中断单独保留，不计入最终通过。

## 样本、显示器与证据边界

代表性文件位于用户本机 `resources/sigmf_data_demo/0000000.sigmf-data`
及同名 `.sigmf-meta`。该位置不是 Git clone 附带的数据承诺，用户原始
文件不自动加入本轮提交。meta 明确描述为 **TorchSig synthetic enhanced
detection benchmark**；本轮“实际 IQ”表示读取该文件执行分析及 DSP，
不表示现场 RF 实采。

| 样本字段 | 已核对值 |
|---|---|
| 格式与长度 | `cf32_le`，3200000 字节，400000 个复数样本 |
| 采样率 / 时长 / 中心频率 | 20 MS/s / 20 ms / 2.5 GHz |
| 原始标签 | meta 中 4 条 annotations |
| IQ SHA-256 | `3a47e55b5f02851d702d5c5782998bf3e374f67cde592c35689acf9ec5399254` |
| meta SHA-256 | `5efd03dae0f53002c8400ddc2ba65e367d7dde315b232a24cd77a2b1bd982a29` |

Release U1–U3 交互场景的手动及算法标记是明确的 UI 夹具，模型推理
标记为 false；四条原始标签读自该文件的实际 meta。当前 Release U3
重新得到 8000、8192、8192 个有限且能量非零的 DDC 样本，输出哈希
与 [U3 基线](../U3/native-report.json)一致；v6 重开、v5 原字节备份和
未知时间保留均通过，见[本轮报告](native/native-release-u3-r1/annotation-interactions-report.json)。

正式 FFSCN 模型流程单独执行，两配置都完成 Sample/ROI 前向并从
实际 IQ 得到 16384 个有限 DDC 样本，能量为 29129.78350248933，
输出 SHA-256 均为
`2f5c8ce025ce4a7e7040d23e6ab93ccf8255e5ce334eeef7d18611d97d54bece`。
保存重开及绑定几何变化后保留创建快照通过。以上证明本样本的功能
流程、有限输出和持久化一致性，不证明算法定位精度。

用户已经更换外接屏，当前实际探针为 **U27U2D、连接序号 1、
DISPLAY2、3840×2160 物理、150% DPI、2560×1440 逻辑全屏**。
验收选择唯一实际 4K/150% 连接屏，报告记录友好名称、设备路径、
连接序号、原生尺寸及窗口所在屏；对话框按该屏真实 DPI 裁剪。
不以其它屏幕上的超大窗口代替。旧 U1/U2 和 R5 的 Redmi 27 NU /
DISPLAY6 身份保持历史原值，不用作新屏运行证据。

| 证据类别 | 可以支持的结论 | 不能据此扩展的结论 |
|---|---|---|
| offscreen Qt UI | 状态、交互和数据边界回归；使用 QWidget 回退 | GPU 绘图、真实显示器布局或模型效果 |
| 原生标记场景 | 实际 Qt 窗口、当前屏幕/DPI、选择/编辑/通道旅程 | UI 夹具是模型输出或 OS 物理输入设备测试 |
| QRhi GPU 报告 | 实际非 CPU 驱动、曲线顶点、标量纹理/LUT 绘制及上传 | GPU DSP 或 TensorRT 推理已经执行 |
| 软件回退报告 | QWidget/QPainter 显示及独立启动 | CPU 模型推理回退 |
| 实际 IQ CPU DDC | 文件样本、范围/前缀、滤波/重采样及保存重开一致性 | RF 实采精度或全部 DDC 数值误差认证 |
| 正式 worker 报告 | 指定模型、设备、形状和参数的实际前向/映射 | 完整 L3/L4、所有谱型精度或跨 GPU 可复用性 |

本轮 Release U3 空视区/Partial 场景在完整物理文件上控制可用前缀；真实
SourceLoader 停止、Loading 非空前缀及首个未读样本的拒绝由处理器和
Qt 回归单独覆盖。不能把受控前缀截图写成真实停止载入证据。
菜单采集向可见控件投递公开 Qt 鼠标上下文事件，再点击真实菜单动作，
没有用业务槽替代；该机制不等于 OS 物理鼠标输入验收。

冻结 v5 迁移夹具保留原 W: 来源，仅验证 ProjectStore、原始字节备份及
旧通道快照；不改来源身份，不宣称已对旧路径通道执行 DDC。实际
CPU DDC 用本机同哈希样本另建 v6 工程，两个证据链分别保存。

## 本轮性能测量

| 测量 | 实际结果与范围 |
|---|---|
| Debug 正式模型启动可见反馈 | **566.3617 ms**，高于 200 ms 建议目标；本轮 Debug 未执行最终取消和压力 |
| Release 正式模型启动可见反馈 | **134.9452 ms**，达到该建议目标 |
| Release 最终真实任务 | 启动可见反馈 **113.0498 ms**、取消可见反馈 **157.9721 ms**、取消完成 **179.2669 ms**；`durablePrefixExact=true`，已发布前缀精确保留 |
| Release 合成 UI 压力 | 1000 个对象 × 每对象 100 段，共十万活动段；选择 P95 **84.2224 ms**，含首帧与图表帧；筛选 P95 **70.4745 ms**，含首帧但不含图表帧；各测 20 次 |

字段和原始样本序列分别见 [Debug 报告](native/native-debug-r1/wideband-detection-report.json)
及 [Release 报告](native/native-release-r1/wideband-detection-report.json)。
选择与筛选包含的绘图范围不同，不能直接比较成计算快慢；十万段为
合成 UI 压力，不是大量 IQ、真实 RF 或模型准确率证据。Debug 一项
超出目标且未覆盖最终取消/压力，因此不宣称所有配置性能通过。

## 运行与仍待验证范围

交付输出为 `out/vs2026-qt611-debug_bin/` 和
`out/vs2026-qt611-release_bin/`。Qt 及完整 CRT 由构建部署；原始样本、
工程附属记录、TensorRT/CUDA DLL 和模型资产各自保留。源码 clone
不含 `out/` 的模型和 DLL。模型的部署、SHA 与重新导出边界见
[R6 模型部署](../../wideband-detection/R6/MODEL_DEPLOYMENT.md)。

当前模型验证包为 TensorRT 10.11 / RTX 5060 Laptop / SM 12.0，
不承诺其它 GPU 可直接复用 engine。显示 GPU、CPU DDC 和推理 GPU
分别记录：本轮显示为 AMD Radeon 880M 的 QRhi D3D11，模型推理为
NVIDIA RTX 5060 的 TensorRT；软件显示不提供 CPU 推理。当前/平均/最大/最小谱均可检测，
默认 FFSCN 逐帧、SCN 最大保持；两者 `down_nums=11`、模型频率长度
为至少 2048 的 2 次幂。SCN 固定 32768 为部署规格，本轮不修改算法契约，
未执行新的 SCN 前向；其资产身份不变不能作为新增效果通过的证据。

少量代表样本仅验证功能流程。完整 L3/L4、各统计谱/SCN 效果、真实
RF 真值集、长文件和跨设备吞吐、受控冷/热启动仍未完成；旧 M4
失败、超时和中断保持原记录。本轮当前性能边界见上表，不以代表性
流程成功替代全部配置或真实 RF 的性能与效果验收。

设备输入、持续采集、真实同步和 Ubuntu 运行尚未完成。窄带当前真实
能力为 CPU DDC/FIR/重采样、波形、PSD/STFT；星座、眼图、识别和
解调页面仍明确标为合成演示。操作复核及复验入口见
[手动验证](MANUAL_VALIDATION.md)，批量模型效果仍使用
[R6 手动入口](../../wideband-detection/R6/MANUAL_VALIDATION.md)的新身份和新目录。
