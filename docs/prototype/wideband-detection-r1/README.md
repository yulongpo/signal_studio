# 宽带检测与统一标记原型 R1

独立编写的离线 HTML / CSS / JavaScript 交互原型，设计尺寸为 **2560 × 1440 逻辑工作区**。在 Edge / Chrome 中直接打开 [index.html](index.html)，无需服务器、构建步骤、第三方依赖、CDN 或联网字体。品牌 SVG 只引用本仓库 `resources/branding/`；保留 A1.4.3 工程树、三图、参数栏和底部记录的工作区结构。

**浏览器交互原型 · 合成演示 · 不执行真实模型。** 导出的状态含 `demoOnly: true`。导航、波形、PSD、时频/瀑布纹理、模型分数、噪底轨迹、任务计数和算法框均为合成演示，不是 Qt、GPU DSP、真实 IQ 或模型效果证据。窄带通道仅保存状态，不执行 DDC / FIR / 重采样。

4 条 ImportedLabel 几何于 2026-10-13 从 `W:/wbSig53_rfbench20m/train/baseline/0000000.sigmf-meta` 读取，并核对同源 `0000000.json`；内置数据不依赖 W 盘可用。Fs=20 MS/s，Fc=2.5 GHz，400000 samples / 20 ms。SigMF 频率是绝对 RF；rfbench JSON 边界是基带，校验时只加一次 Fc。绘图中的能量纹理仍是合成数据，没有读取 `.sigmf-data`。

## 操作入口

顶部“可重复评审场景”可复位 S0–S9、源缺失、损坏和保存错误。默认 S1 包含四条原始标签、一条双突发手动标记和五条**合成**算法对象。算法数量不表示指定样本的检测结果。S8 预选 sample 结果并展示证据；S9 带一个来源 revision 快照通道。第二文件是绑定冻结生成器夹具用的 60 MS/s / 10 ms / 600000 样本元信息演示；不读取 WAV。

分析菜单、“检测当前区域…”和主图右键均只打开右侧同一检测卡，不自动运行。选择范围/方案后点击“运行检测演示”；“下一演示阶段”按排队、谱帧、模拟推理、模拟精修、完成推进。计数为模拟值。同工程一次只演示一个待完成任务；重复启动不新增副本。后台任务页列出本工程任务及各自源文件，切文件浏览时仍可通过绑定 runId 的按钮推进/取消原文件任务，输出只发布到其源文件。取消任务进入取消中，再推进一次完成安全取消。失败、部分覆盖、零结果与缺模型场景明确分开。

默认主图背景拖框缩放。拖时间轴或频率轴分别平移该轴，滚轮按所在区缩放；图内同时缩放两轴。时频图为横时间/纵频率，瀑布图为横频率/纵时间；内部物理坐标和 selection ID 不变。视图始终受可用前缀/有效频带约束。

“持续标记”每次拖动创建一个 Manual 对象，并保持工具。“检测 ROI”临时进入独立工具，拖完显示待确认浮层，Enter/确认才保存一个 ROI；Esc 取消并恢复之前的工具。ROI 不创建标记，不启动任务。运行请求与必要上下文分别冻结为 `range` 和 `contextRange`；短 ROI 可以从可用前缀读取必要上下文，整体可用文件不足仍拒绝。输出不越过请求范围。

图谱/表格/树使用同一 annotationId。Ctrl 切换选择，Shift 连选；重叠命中弹出来源/段列表。选中手动段后可拖框体及八手柄；原始标签和算法默认只读，点击“创建修订”才可编辑。数值编辑 Enter/Tab/应用内点击提交；图上拖动松开只产生一次命令。Esc 回滚草稿。标记 Ctrl+Z / Ctrl+Y 与视图后退/前进分别保存历史。命令只回滚本次触及的 Annotation/Channel ID 差异，保留之后任务发布的其它算法对象及引用。

接受/拒绝只改变同一对象的 Review；重复接受不增加 Review 记录或标记副本。合法手动创建初始为 Accepted，人工复核记录绑定其初始 revisionId；原始标签与算法默认 Pending。几何修订保存独立 revision，并统一回到 Pending，必须重新确认当前 revision。每条 Review 记录绑定 revisionId / reviewedRevisionId；重复接受同一 revision 不增加记录。sourceKind、annotationId 与 origin 不变。多段对象分别绘图，不把突发间隔画实。详情支持添加/删除段、拆分当前段及显式合并同对象相邻段；合并包含间隔是人工修订，不是自动融合。

参数草稿在 Enter、Tab/Shift+Tab、应用内其它位置点击后提交；输入中刷新或应用失焦不应用、不覆盖草稿。切文件/模式/工程撤销草稿。输入框、下拉框与其选择过程不通过滚轮改值；鼠标位于参数控件时侧栏仍滚动。主图/侧栏滚动保留。

FFSCN GUI IQ 主方案为 10 × 65536；FFT 窗 3.2768 ms、谱步长 1 ms、10 帧标称 10 ms / 观测支持 12.2768 ms、滑动 5 帧 / 5 ms。hop 不作为独立时间分辨率。SCN 对照为 16 帧最大累积谱，网络单行 32768。**FFSCN-17 的 17 是频率阶数，时间上下文仍为 10 真实帧**，实验 N 支持 8192–131072 的 2 次幂，默认 131072。sample / ROI / coarse 互斥，各分支有效参数在控件禁用及文字中明确；不拆分时分段参数禁用。每种方案保存自身已提交参数；切方案恢复对应配置，不把其他方案的分数阈值或草稿带入。未访问方案使用明确的演示默认值。这些是配置/交互演示，未验证模型效果。

标签预览只有两类：**broadband_signal_generation/write_test_record.m 的真实冒号带键 TXT**和 TorchSig SigMF / 同源 rfbench JSON，不支持任意 TXT 或新造 key=value 格式。内置生成器文本逐字冻结自本仓库 `tests/fixtures/wideband/generator-60m-10ms.txt`，不依赖外盘；其 12 行载波、17 对 TIME 分别归一化为 12 个对象、17 个活动段，须绑定第二文件的 60 MS/s / 10 ms 源元信息。首文件 Fs/长度不符时拒绝导入。文件头按 `time_start/time_end/sig_len` 校验覆盖和 Fs/样本数，载波行按键 `mod/sys/FC/BW/FD/LEN/SPS/CNR/SNR/TIME` 读取，TIME 保留 writer 五位小数，允许已知空白差异；重复键、缺键、坏 TIME、非有限数、LEN 不符、物理越界均整批拒绝。writer FC 是 `[0,Fs)` 正向生成轴，**RF = 绑定文件 Fc − Fs/2 + writer FC**，边界为中心±BW/2。每个 writer 行一个对象，TIME 多对保留多段；保留行号、原文、全部原始键与坐标变换，标记为 RoundedLabel。10 µs 文本量化、最近样本取整和 `ceil(Fs×5µs+0.5)` 样本不确定度均保存，不宣称样本级真值或实测占用带宽。

内置有效、冲突和 Partial 尾部场景可点击演示；同源重复导入幂等，冲突不导入，Partial 不截断原标签。可选本地 meta/json/txt 文本预览验证 Fs/Fc、完整样本长度、样本边界、频带和同源文件基名。TorchSig 演示限定 cf32_le、单通道、单 capture、零起始/零 offset、固定 Fs/Fc、完整时频标签。meta 频边为绝对 RF，JSON 频边为基带；核验只加一次 Fc，容差 `max(1e-6 Hz,4 ULP@RF)`。JSON 还检查 `schema/capture_samples/capture_duration_s/Fs/Fc`，依据样本区间、频带、类别和可用 emitter ID 建立**唯一匹配**，允许 events 换序；类别/emitter 冲突和重复歧义拒绝。`.sigmf-data` 只检查同源名称，绝不读取二进制。该 JavaScript 浏览器预览**不表示原生解析器已实现**。

确认单个有效段后可创建模拟窄带通道。对话框校验频带、输出采样率和有效确认，保存创建时 revision/sourceSnapshot；之后来源修订显示 `changed`，不自动重算。未知时间观察带没有有效段，不能创建通道。保存使用浏览器 localStorage，只存已提交值；重开不恢复模拟后台执行。导出 demo JSON 后可从帮助页“重开导出 demo JSON”完整校验并原子载入：替换前核对参数/方案配置/shape、布局、文件与视图、origin/segments/observation、修订复核、任务/输出和通道快照引用；拒绝损坏文件时保持当前状态，提交异常回滚。

底部/左右侧栏分隔条可拖动调整，面板可折叠，主图可最大化；视图菜单可恢复布局。所有长范围/字段在侧栏内换行，记录区可滚动，运行按钮留在卡片操作区。

## 页面状态

| 状态 | 场景与可点内容 |
|---|---|
| S0 | 无工程入口，检测不可用；创建演示工程恢复 Ready |
| S1 | Ready / 四标签 + 手动多段 + 五合成算法对象 / 三独立图层 |
| S2 | Loading 8 ms 可用前缀；Partial 12 ms 可用前缀，不截断标签 |
| S3 | 单 ROI 待确认，可 Enter 确认 / Esc 取消 |
| S4 | 模型缺失或 物理20 ms / 可用5 ms前缀上下文不足；标记仍可查看和手动编辑 |
| S5 | 排队/显式阶段推进，冻结请求与上下文，可继续缩放 |
| S6 | 取消/失败/部分覆盖，保留已发布片段及未分析说明 |
| S7 | 完成或正常零结果，明确请求范围和输出数量 |
| S8 | 复核/修订，六类证据：coarse、window、sample、ROI、fallback、manual |
| S9 | 通道 revision/sourceSnapshot，保存重开及来源 changed |

## P01–P17 处理映射

| 原型项 | R1 的具体实现 |
|---|---|
| P01/P02 | 三入口统一右侧检测卡，先配置，主方案/对照/实验分列 |
| P03/P04 | view/full/segment/roi 四范围；多段先选一段；ROI 显式确认 |
| P05 | 导航 + 辅助波形/PSD/证据 + 主时频/瀑布，共享物理投影 |
| P06/P07 | 持续手动标记；只读来源的显式 revision；八手柄和捕获清理 |
| P08 | 显式阶段按钮、计数、安全取消、部分覆盖和失败；无定时假进度 |
| P09/P10 | 底部统一标记记录，树按来源集合，任务只挂摘要 |
| P11/P12 | Review 不复制；未知时间窗口底纹，不伪造 t0/t1 |
| P13 | 能力方案与精修分支参数；四物理时长；草稿/滚轮保护 |
| P14/P15 | 通道对话框 revision 快照，无 DSP；demoOnly 导出 |
| P16 | 单对象多段添加/删除/拆合后Pending；不支持暂停/续算或跨来源自动融合 |
| P17 | 两类标签预览，三图层，六类证据和原始字段 |

## I01–I24 浏览器验收步骤

下表是实现映射。2026-10-13 主 Agent 已完成40组浏览器回归，覆盖 I01–I24主要路径，0失败/0 JavaScript错误；实际范围和未覆盖组合见 [阶段验证记录](../../implementation/wideband-detection/M0-P/VALIDATION.md)。原生 Qt / 第二屏 DPI / 模型效果必须另行验证。

| ID | 可重复操作 | 可检查的预期 / 当前验证状态 |
|---|---|---|
| I01 | S1：分析菜单、范围条检测、主图右键检测 | 同一卡片，runs 数不变；浏览器主要路径通过，见阶段记录 |
| I02 | 运行前框选缩放；运行后再缩放 | run.snapshot.range/contextRange/params 固定；浏览器主要路径通过，见阶段记录 |
| I03 | 开持续标记→ROI→拖动→Esc | annotations/runs 数不变，tool 回 manual；浏览器主要路径通过，见阶段记录 |
| I04 | ROI 拖到边缘→Enter | 单 ROI 有界、明确确认，不创建标记；浏览器主要路径通过，见阶段记录 |
| I05 | 背景拖框、两轴拖动、区域滚轮、至边界 | 视图有界、几何不变，视图历史独立；浏览器主要路径通过，见阶段记录 |
| I06 | 同一段时频/瀑布切换，RF/基带切换 | ID/物理范围/选择保持，轴方向正确；浏览器主要路径通过，见阶段记录 |
| I07 | 标签 2/3 与 Manual 重叠区域单击；Ctrl/Shift 树/表多选 | 命中列表含来源/段，三处选择集合一致；浏览器主要路径通过，见阶段记录 |
| I08 | 选 Manual，拖框体/八手柄/数值；Esc/Ctrl Z/Y | 一次松开一条命令，Esc 回滚，undo/redo 独立；浏览器主要路径通过，见阶段记录 |
| I09 | 选 ImportedLabel/Algorithm，尝试拖；创建修订再拖 | 初始只读，修订后 origin/sourceKind/ID 不变；浏览器主要路径通过，见阶段记录 |
| I10 | 同一算法对象接受两次，拒绝再接受 | annotations 数不变，Review 独立且可追溯；浏览器主要路径通过，见阶段记录 |
| I11 | mean≥筛选、点击列数值排序、多选/批量复核 | 总数/可见/已选、批量数量明确，仅改已选；浏览器主要路径通过，见阶段记录 |
| I12 | 三图层开关、来源/run筛选 | 可见数变化，总数/持久对象不变；浏览器主要路径通过，见阶段记录 |
| I13 | sample/ROI/coarse 切换，关闭拆分；换三方案 | 有效参数禁用准确；17为频率阶数，SCN16帧；浏览器主要路径通过，见阶段记录 |
| I14 | score输入0.8未提交→API refresh/应用失焦→Enter/Tab | 草稿保留，参数.7，提交后.8；浏览器主要路径通过，见阶段记录 |
| I15 | 滚轮经过输入/下拉/选项/侧栏/图谱 | 控件值不变，侧栏与图谱滚动保留；浏览器主要路径通过，见阶段记录 |
| I16 | 排队→推进→取消→推进；选失败/零/缺模型场景 | 四状态分开，安全取消/历史保持/覆盖明确；浏览器主要路径通过，见阶段记录 |
| I17 | 文件A→B→A，再新工程/重开 | 标记不串文件，草稿清理，任务generation隔离；浏览器主要路径通过，见阶段记录 |
| I18 | S8选择五算法卡及Manual | 方法/状态/原始当前/mean max/unique windows/噪底可查；window时间—；浏览器主要路径通过，见阶段记录 |
| I19 | 选Manual两段→拆当前→合相邻→添/删段 | 同对象/同来源/同origin，间隔未画实；浏览器主要路径通过，见阶段记录 |
| I20 | 第二文件预览真实60M生成器TXT；首文件TorchSig；相同集合再导 | 幂等/正向FC减Fs/2再加Fc/17多段/10us量化；来源不符拒绝；浏览器主要路径通过，见阶段记录 |
| I21 | 预览冲突；Partial尾部情景 | 拒绝冲突、原始字段保存、标签不截断；浏览器主要路径通过，见阶段记录 |
| I22 | 确认段→通道dialog参数错误/成功→改来源 | 创建snapshot，源修订Pending且changed不重算；未知时间拒绝；浏览器主要路径通过，见阶段记录 |
| I23 | 保存→S0→文件菜单重开；导出；损坏/缺源/存储错误 | demoOnly；layout/shape/引用等完整校验后替换；错误保留状态；浏览器主要路径通过，见阶段记录 |
| I24 | 拖三分隔条、折叠/恢复/最大化、长字段 | 目标尺寸按钮可用、图谱有空间、长字段换行；浏览器主要路径通过，见阶段记录 |

## 自检和边界

2026-10-13，临时 Edge headless / 2560×1440 viewport 快速自检：页面无 JavaScript 错误；真实 UI 操作检查了菜单入口、ROI确认/取消、冻结快照、取消中/取消完成、Manual拖动的一次命令及撤销重做、重复接受幂等、参数刷新/Tab草稿、两类内置标签预览、文件隔离、通道创建和 localStorage重开。14个其余场景可加载且无 JS 错误。追加自检核对了实际指定 meta/json 的同源配对和单次 Fc、原标签修订后 origin/sourceKind/ID 不变、数值修订后的通道 changed、FFSCN-17 固定10帧与 SCN16帧配置。临时自检脚本/PNG已清理，不新增依赖。

这只是本原型 smoke，未覆盖八手柄逐一、所有组合多选/过滤、native select 打开时跨浏览器滚轮细节、全部本地用户标签文件格式、浏览器系统失焦差异或最终 I01–I24 全量截图。主Agent的40组实际UI回归与独立评审记录另存 [阶段验收目录](../../implementation/wideband-detection/M0-P/)。2560×1440浏览器viewport不是第二连接屏原生DPI证据。

未实现：真实 IQ 读取、模型/worker、GPU DSP、原生 Qt、真实标签解析器完整格式、真实通道DSP、跨来源自动融合、暂停/续算、真实文件目录工程。上述边界在界面/导出中可见，不将占位当真实能力。

## 自动化观察接口

`window.signalStudioPrototype.snapshot()` 返回深度冻结的状态克隆，无直接 state 写入口。字段包括 `files[].annotations`、`files[].channels`、`params`、`profileParams`、`paramDrafts`、`selectedIds`、`tool`、`roi`、`roiDraft`、`geometryDraft`、`undo/redo`、`runs[].snapshot.range/contextRange/params`；`derived` 包含 `visibleIds`、`preflight`、`requestRange`。使用状态断言不替代 UI 点击验收。

```js
window.signalStudioPrototype.dispatch('scene', {scenario: 'S8'});
const state = window.signalStudioPrototype.snapshot();
window.signalStudioPrototype.dispatch('refresh'); // 重绘，不提交参数草稿
window.signalStudioPrototype.dispatch('setFile', {fileId: 'file-b'});
window.signalStudioPrototype.dispatch('advanceRun', {runId: '具体任务ID'});
window.signalStudioPrototype.dispatch('cancelRun', {runId: '具体任务ID'});
window.signalStudioPrototype.dispatch('save');
window.signalStudioPrototype.dispatch('reopen');
const demo = window.signalStudioPrototype.dispatch('exportSnapshot');
```

文件职责：`index.html` 工作区与入口；`styles.css` Brand v2 暗色工业视觉/布局；`data.js` 元数据几何和合成场景；`app.js` 状态、命令、图谱手势、表格、任务、预览/保存；本 README 为操作/设计映射。没有复制旧原型业务代码，没有修改外部原型、冻结 A1.4.3 或本仓库原生业务源码。
