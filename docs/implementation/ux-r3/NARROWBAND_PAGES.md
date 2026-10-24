# UX-R3 G5 窄带独立页面与共享同步

本节点在现有真实 IQ / CPU DDC / 单载波恢复链上拆分页面。实现写入共享工作区；本节点不提交、不推送、不切分支。构建、CTest、原生实屏与最终审核由主 Agent 统一执行；本文记录代码合同和待运行验证，不构成测试通过声明。

## 页面与集成 API

导航显示顺序为信号观察、星座图、眼图、调制识别、解调分析。五个 QWidget 常驻同一 `narrowbandPageStack`；切页仅选择现有页面，不重建图表，不清空同步状态。

| 页面枚举 | 稳定数值 / stack index | 导航按钮 objectName | 保存值 |
|---|---:|---|---|
| `Observe` | 0 | `observePage` | `observe` |
| `Constellation` | 1 | `modulationPage` | `constellation` |
| `Eye` | 4 | `eyePage` | `eye` |
| `Recognition` | 2 | `deepLearningPage` | `recognition` |
| `Demodulation` | 3 | `demodulationPage` | `demodulation` |

`Modulation = Constellation`、`DeepLearning = Recognition` 为兼容别名，不增加页面数。视觉顺序不能用连续递增的 enum 数值推导。旧四页调用的 0–3 含义保留，Eye 追加为 4。

- `activatePage(NarrowbandPage)`：业务导航入口；更新活动 Channel 页面，触发 `pageChanged(int)` 和 `displayParametersChanged()`。无效枚举不执行导航。
- `activatePageForAcceptance(int)`：保留原生验收入口，接受 0–4。
- `currentPageIndex()`：返回上表稳定索引。
- `setSyncPanelExpanded(bool)`：显示/隐藏同一公共参数面板，并写入活动 Channel 的显示状态；不提交分析请求。
- `analysisResult()`：当前上下文不可变的**共享分析快照**。它包含至多 20000 个星座候选点、256 条眼图轨迹和每 UI 16 点的有限绘图支持，实际长度受有效观测限制。页面按各自显示预算抽取，快照数量不等于当前绘制数量。
- `renderStatistics()`：增加 `analysisGeneration`、`analysisPending`、`pageIndex`、`syncPanelExpanded`、`constellationView`、`eyeView`，用于区分后台快照和实际显示投影。

保留所有现有图表及参数 objectName，主窗口继续将参数控件装入对应 `ChartControlStrip`。`narrowbandConstellationLarge` 只在星座页可见，`narrowbandEyeLarge` 只在眼图页可见；`narrowbandModulationStft` 仍在星座页提供实际通道时频上下文。主窗口负责树/菜单入口、五页 smoke 遍历、各页标题栏和共享弹层的上下文关闭，不能再要求眼图和星座标题栏同时可见。

## 共享参数与页面显示状态

顶部 `narrowbandSyncSettingsToggle`（“同步与参数”）控制右侧 `narrowbandSharedSyncPanel`。面板在五个页面之间共用一个控件树，默认收起；不额外创建同步服务。面板纵向滚动，包含调制族/阶数/APSK 环配置、Rs 模式与候选、CFO、RRC、定时和载波恢复、样本预算及分析/取消/恢复操作。`narrowbandAnalysisSummary` 在收起面板时仍展示当前分析状态，完整细节保留在共享面板和 tooltip 中。

当前 Channel 的 `analysis` 仍保存已确认的处理配置。星座显示沿用 `analysis.pointCount`、`analysis.constellationAutoScale`、`constellationMinimum/Maximum`；眼图沿用 `eyeComponent`、`eyePeriods`、`eyeTraces`、`analysis.eyePointsPerUi`、`analysis.eyeTimingOffsetUi`。这些状态随通道保存，两个页面之间不会相互覆盖。参数输入继续使用 `parameter_input.h`，控件提交与草稿取消仍由主窗口共享输入策略执行。

| 变化 | 行为 |
|---|---|
| 五页切换、公共同步面板开合 | 保留同一请求、同一结果指针和两页显示状态 |
| 星座显示点数、自动/手工坐标范围 | 从快照更新图形，不读取 IQ、不重复同步 |
| 眼图 I/Q、轨迹数、4/8/16 点/UI 密度 | 从共同高密度轨迹抽取显示，不读取 IQ、不重复同步；点/UI 不改变真实 SPS |
| Rs、CFO、RRC、调制假设/阶数、恢复开关、阶段、周期/折叠偏移或样本预算 | 沿现有异步服务提交新请求，立即清除过期图形；保留真实条件与锁定判据 |
| 源/可用前缀、通道配置、源时间窗、工程代次改变 | 来源或依赖不匹配的旧结果不能发布 |
| 用户取消 | 清空两图与结果；相同上下文中的普通刷新/切页不自动重启，显式分析可恢复 |

周期和折叠偏移仍沿既有完整服务重新计算，未另建只处理眼图的后台 worker。显示预算固定上界不增加真实观测；不足时保持实际轨迹数或明确空态，不重复样本补足。

## 分析依赖与异步边界

真实源文件及可用前缀 → `ChannelSampleCache` 中的 DDC/FIR/重采样结果 → `NarrowbandAnalysisService` → `NarrowbandAnalysisResult` → 星座与眼图两个显示消费者。

GUI 不重写同步算法。`AnalysisIdentity` 继续包含工程代次、请求代次、Channel ID/配置版本、来源身份和实际输出样本区间。发布前检查物理修订、时间窗与有效分析配置；取消/切换后不接受旧 worker 的完成结果。物理文件被外部改动时保留失效标记，直到明确重新分析。

原始散点无需可信 Rs；Rs 未确认时眼图为空并说明原因。CFO、匹配滤波、定时与载波阶段仍来自真实阶段数据、`clockSymbols` 和 `synchronizedSymbols`，同步失败继续展示真实可用的回退阶段并标注请求/实际阶段。PSK/QAM/APSK 的数值恢复实现、采样位置映射和锁定门槛未更改。

## v6 页面与显示状态兼容

`ProjectStore` 保存规范名称，读取同时接受旧 `modulation` → `Constellation`、`deepLearning` → `Recognition`。迁移只改变页面定位，所有分析配置、实际 Fs、通道来源快照及原有显示参数保留。

每个 Channel 增加可选显示状态对象：

```json
"narrowbandDisplay": { "version": 1, "syncPanelExpanded": false }
```

旧 v6 缺少此对象时面板默认收起；未知页面、未知显示状态版本、非法布尔值按现有工程完整校验规则失败，不替换目标工程。本节点仅改 `pageName`、Channel 页面解析和上述显示对象序列化，保留主 Agent 并行加入的 Project/FileMetadata 字段、`decodeDocument`、`stateToken` 和 `writeAssets` 控制。

主窗口保存/打开使用 `.ssproj`。直接 `ProjectStore::save/load` 的 v6 JSON 测试仅验证内部状态兼容，不代表重新开放旧 `project.json` 的正常打开入口。

## 模型状态与演示边界

识别页明确显示 `recognitionUnavailable`，真实识别按钮 `runRealRecognition` 禁用：尚未部署真实模型与推理后端。已有“运行识别演示”独立标示合成进度与分数，模型文件选择仍只是演示文件名记录；不会执行权重，也不会修改手工同步假设。

解调页 `demodulationUnavailable` 明示真实位流未部署；合成星座/眼图/位流保持原有明确标示。真实同步完成不等于调制已识别或真实位流已解调。

## 测试与交接

`tests/narrowband_analysis_ui_tests.cpp` 新增：

1. `independentPagesShareAnalysisAndKeepDisplayState`：五个按钮实际切页、图页互斥可见、共享面板跨页可达、同一结果指针/请求代次、显示预算投影、隐藏页修改 Rs 的异步失效、取消后切页不复活、未部署模型提示。
2. `pagePersistenceAndLegacyNames`：五页规范保存值与显示状态往返；旧两个名称及缺省显示状态；未知页面/显示版本拒绝且目标保留。

既有真实散点/眼图测试改为分别进入所属页面，按实际 chart 属性检查显示数量，并验证点数修改保留快照；PSK/QAM/APSK、外部文件变化、取消、通道切换等原有路径保留。所有 MainWindow 保存/打开用例改用 `.ssproj`。

`tests/narrowband_analysis_capture.h` 的分析采集现在为 RAW/SigMF 各 9 场景，共 18 场景，包含分别显示的恢复星座/眼图、识别/解调不可用、Eye 页 `.ssproj` 重开和取消。反馈采集保持 7 场景，显式切入眼图页后操作眼图标题栏。只检查当前可见标题栏；GPU 眼图检查实际顶点上传与数据绘制，不要求该页不存在的热图纹理上传。采集继续要求实际 Redmi 4K/150% 全屏，辅助屏证据不能替代指定实屏。

集成已同步 `scripts/screenshot.ps1`：验收要求 18 个唯一场景，硬件证据按页面接受纹理上传或顶点上传，并继续要求 D3D11 与实际数据绘制。星座/眼图必须有顶点上传；软件路径的 GPU 上传与绘制计数必须为零。

节点实施后的构建与采集由主 Agent 统一执行。2026-10-24 Release 在实际 Redmi 27 NU / DISPLAY6 / 4K 150% 上 GPU 和软件各 18/18 通过，报告分别为 `G7/native-narrowband-release/` 和 `G7/native-narrowband-release-software/` 中的 `narrowband-analysis-report.json`。Debug/Release 完整回归及历史失败由 VALIDATION 统一记录。新增测试使用独立生成器的确定性 IQ 文件，经实际 CPU DDC/恢复链处理；这不代表现场 RF 精度通过。

Debug 随后也在相同实屏上 GPU/软件各 18/18 通过。部署五页循环使用 CLI 显式传入 1 MHz 手工 Rs 假设，并在报告 acceptanceSymbolRateHypothesisHz 中记录；这是内置实际 IQ 图形验收的前置配置，不是对阿拉善样例真实符号率的估计。普通启动仍为 Rs 待确认，未知时眼图给出提示。真实已保存工程则沿用其用户配置和恢复结果，独立眼图页允许顶点绘制而不要求不存在的热图纹理。
