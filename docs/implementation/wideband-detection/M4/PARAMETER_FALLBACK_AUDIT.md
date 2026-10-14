# M4 Fine fallback 与 SCN 裕量参数审计

结论：**PARAMETER_SEMANTICS_FAIL（本次补审的非默认参数交互契约）**。发现两项 P2 阻断：Fine fallback 使用已禁用且注明不生效的 Coarse 用户历史参数；SCN 的噪底裕量控件保持启用并说明参与时间精修，实际生产链却不使用该值。没有找到获批的“fallback 可使用 disabled branch 用户参数”例外，不能把这两项解释成已批准限制。既往 SPEC/QUALITY_PASS 是此前已检查范围的结论，不覆盖本次新增参数组合。

本报告为 2026-10-14 04:21（Asia/Shanghai）的只读核查。只新增本报告，没有改业务源码、UI、数学、冻结配置或参考参数，没有构建、测试、GPU/模型运行或提交。主实施报告当前第二屏 34 场景已通过、Qt 94/94 在 389.6 s 完成，UI timeout 300→480 的批准仍在等待；这些正常路径证据不覆盖本报告的参数历史→fallback 组合。正在执行的正式 R2 default 实验应继续保持其已冻结源码身份，不能途中修改代码。

## 获批依据与作用域

1. `docs/plans/wideband-detection/02_UX_AND_REVIEW.md:38`：卡片“只启用当前分支真正使用的最短活动/静默等参数”。`07_PROTOTYPE_AND_INTERACTION_REVIEW.md:114` 的 I13 同样要求“只启用生效参数”。
2. 获批 M0-P 的 `docs/prototype/wideband-detection-r1/app.js:78` 将 splitGap/minBurst 在非 Coarse 时禁用，并明确 sample 文案“粗分段参数不生效”；README 第 25 行要求 sample/ROI/coarse 互斥，各分支有效参数由禁用状态及文字表达。`scripts/verify_wideband_prototype.mjs:203–217` 检查 ROI 下 minBurst 禁用、noSplit 时对应时长禁用。
3. 用户方案 A 的 `M4/PARAMETER_DECISION.md:3` 明确保持“原型布局、控件、固定动作、策略说明和参数启用矩阵”；第 12 行规定“全局噪底裕量仍用于频率处理，sample 的门限仍由带限线性功率得出”。批准的 ROI/边缘比例/平滑/noSplit/连续规则没有新增 Coarse 历史参数的 fallback 例外。
4. `04_ALGORITHMS_AND_VALIDATION.md:52` 冻结 SCN CNR 与频率收边裕量为 3 dB。M4 `VALIDATION_PROTOCOL.md:45` 允许不可靠 Fine 保留真实粗估计并标 fallback；第 69 行规定 Coarse 的 noise percentile+6 dB、默认 minBurst=10 ms、splitGap=20 ms。允许真实粗回退本身，不等于允许界面注明不生效的用户历史参数暗中参与。

“不先运行 Coarse 过滤”只限制 Fine 之前的处理，不能单凭它推导 fallback 的全部政策。本结论另依据明确的禁用矩阵和“不生效”文案；现有文件没有声明 fallback-only 可编辑参数、例外说明或有效参数矩阵。

## A1 / P2：Fine fallback 继承 inactive Coarse 用户值

位置：`ui/detection/detection_card.cpp:810–813`、`:817–818`；`infrastructure/detection_time_analysis.cpp:291–304`；`domain/detection_refinement.cpp:822–844`。

sample/ROI 模式下 splitGap/minBurst 禁用，但已提交值仍保存在同一 `DetectionParameters`。真实 Fine 返回 Fallback 后，调用方执行 `coarseParameters=request.parameters`，只改变 timeStrategy 并把 bandwidthMarginDb 固定为默认 6。`coarseSplitGapSeconds` 和 `coarseMinBurstSeconds` 没有归一到固定默认。若 splitBursts=true，Coarse 实际按这些历史用户值合并间隔、筛最短活动。

用户先选择 Coarse，将 20 ms/10 ms 改为 10 ms/50 ms，再切 Sample 或 ROI；此时两控件禁用，sample 文案还明确“不生效”，然而同一 Fine fallback 可以改变段数、丢弃全部粗活动并成为未知时间。只检查 Fine 成功时“不读 Coarse 参数”的现有 domain 测试无法覆盖这个集成路径。splitBursts=false 时 Coarse 不执行相应筛选/合并，不能据这个子集通过否定 split=true 的问题。

**符合当前批准契约的修正方向**：保留用户已提交的 Coarse 配置，供切回 Coarse 使用；仅构造 Sample/ROI 的有效 fallback 参数时，将 coarseMinBurst/coarseSplitGap 明确设为固定默认 10/20 ms，同时继续固定 Coarse 裕量 6 dB。原生 Coarse 策略仍使用其当前已启用的用户值；noSplit 保持现有首末收缩。保存的 fallbackCoarseEvidence 应明确说明固定回退策略与实际有效参数，不能把 disabled 用户值称为生效值。无需改变获批控件矩阵或 Fine 数学。

另一种方案是允许 Fine fallback 使用 Coarse 历史值；它需要新的用户批准、可见说明与矩阵/配置契约，当前没有该授权，不能直接按此解释现实现。

## A2 / P2：SCN 噪底裕量启用与说明不实

位置：`ui/detection/detection_card.cpp:761–765`、`:814`、`:819–822`；`domain/scn_detection_postprocess.cpp:110–129`；`infrastructure/detection_task.cpp:1123–1127`；`domain/detection_refinement.cpp` Sample/ROI 分支及 `detection_time_analysis.cpp:294–295`。

SCN profileHelp 声明 SCN CNR/频率收边固定 3 dB，却同时声称“卡片噪底裕量用于 sample / ROI 时间精修”。margin 在 Sample/ROI（以及 None）下仍启用。实际生产 task 对 SCN 跳过 `applyDetectionFrequencyAnalysis`，使用 `postprocessScnWindow` 固定 3 dB 的结果；Sample 门限由 noise、burst、edgeRatio 得到，ROI 由自适应正像素门限、edgeRatio/.6 得到，均不读取 bandwidthMarginDb；Fine fallback 又把它固定为 6。因此修改 SCN 卡片 margin=0/6/12 不改变这些测量门限或活动，控件所宣称的作用不存在。

`applyDetectionFrequencyAnalysis` 内部保留一个能消费可调 margin 的 SCN helper 分支，但生产 task 明确不调用它，不能用该 helper 的存在证明 SCN 卡片参数生效。FFSCN Fine 下 margin 仍用于此前的实际频率修边，从而可间接改变时间输入频带；不应把这个正常作用一并禁用。

**确定应修正的是无效控件与错误说明。** 不应将 dB margin 接入 Sample 的线性功率门限、ROI 的批准自适应门限，或把 SCN 固定 3 dB 改为默认 6 dB来“使控件有用”，这些都改变获批数学。优先建议为 SCN profile 明确固定裕量作用域、禁用这个没有可调作用的控件并显示固定 SCN 3 dB / Coarse fallback 6 dB；但 M0-P 矩阵原为 profile 通用规则，方案 A 又要求保持矩阵，**这个 SCN profile 例外仍需用户确认新的参数矩阵**。只改一句帮助文字但继续让无效参数可编辑，不能彻底关闭此项。

## 可执行 fixture 建议（本轮未运行）

| Fixture | 预先固定输入与操作 | 当前可辨认的反例及修正后期望 |
|---|---|---|
| F1 / fallback 配置作用域 | 纯 domain Coarse STFT：Fs=1000、1 sample/slot、200 槽、8 bins；背景 -100 dB，on 槽 `[20,35)`、`[47,62)` 至少一 bin 为 -80 dB，windowCoverage=.1、split=true；门限固定 -94 dB。Sample 能量用恒定 1，明确触发 ratio fallback；ROI 使用同一预置谱、已启用 minOn=50 ms（超过任一 15 ms run）触发无可靠 pooled run。集成层对同一谱按真实 fallback policy 构造参数 | 默认 gap20/min10 合并为 `[20,62)`；历史 gap10/min10 返回两段；历史 gap20/min50 删除全部。修正后 Fine 的三组历史值均返回默认单段，而直接 Coarse 三组仍各有原效果 |
| F2 / 原生配置历史 | 不运行 GPU，以配置服务/Qt offscreen 提交 Coarse gap10/min50→Sample、再→ROI；检查 disabled 状态、request 中保留的历史值和独立 effective fallback 参数 | 保存历史值是合法行为；Sample/ROI 实际回退不能消费它们。另做 Fine 成功、fallback split=false、continuous=.7 的组合，保证既有成功/noSplit/连续政策不变 |
| F3 / SCN margin 作用域 | 固定同一 source 与 raw SCN candidate，复用 `postprocessScnWindow`、同一 frequency track 和 Fine 数据；分别冻结 margin0/6/12，分别覆盖 Sample 成功/回退、ROI 成功/回退、None | 当前生产几何/门限相同，应明确固定证据。用户批准 profile 例外后，SCN margin 控件应不能编辑，并显示固定值作用域；FFSCN 保留可调 margin 的 frequency fixture，避免误禁用其它后端 |
| F4 / 默认不变 | 旧/新实现同一完整默认请求，coarse margin6/min10/gap20；Sample/ROI 成功与 fallback、直接 Coarse、三种 noSplit、SCN 固定3；reuse frozen raw model/window inputs，禁止改 source/真值/门槛 | 每个有效数学参数、power/profile/mask/run、整数边缘、活动数量、fallback 原因与 lineage 逐项相同；正式默认样本重新核验，不能仅靠这个静态等式宣布全链已验证 |

F1 是显式数学/配置夹具，恒定 energy 与预置 spectrum 用于定位参数作用域，不冒称同一真实 IQ 的联合测量；再由 F2/实际 pipeline 的预先固定不可靠 Fine 或资源 fallback 输入验证调用路径。应同时保留 `fallbackCoarseEvidence` 的有效 6/10/20 值与“不是用户 inactive 值”的来源说明。

## 默认数学与冻结运行的影响

**A1 按上述固定 fallback 默认修正，6 dB、minBurst10 ms、splitGap20 ms 的数学完全不变（静态参数等式）。** 当前默认 request 中继承的两值本来就是 `.010/.020`；显式覆盖为同值不改变 mean/percentile、strict mask、gap 合并/过滤次序或半开边缘。Fine 成功路径不变；直接 Coarse 自定义值不变；noSplit 不变；SCN 仍固定 3 dB。没有依据为了这个问题修改参考数学、权重、真值或数值容差。

A2 的建议仅在获得 profile 矩阵例外确认后禁用无效控制并修正说明，也不改变默认数学；若改为新的 SCN 数学作用，则已超出当前批准范围，不能承诺默认不变。

上述是代码参数流的静态结论，本轮没有执行等价测试。任何源码/UI 修正都会产生新的可执行文件/源码身份，必须在当前正式 R2 冻结实验之外建立新 freeze，并对完整默认样本重新核验；不能途中替换正在运行的代码或用当前默认效果反选非默认参数。当前 default R2 可继续作为该冻结版本的默认证据，但不足以关闭这两项非默认参数契约阻断，M4 全参数验收不能沿用此前 PASS。

## 只读快照身份

| 当前受检文件 | SHA-256 |
|---|---|
| `domain/detection_refinement.cpp` | `322cda0ff6b6dc241b9b35fb300e5a256add66a2b3acd2a4389ad72d54435d59` |
| `infrastructure/detection_time_analysis.cpp` | `5c4055b1719c97502e110366a8ba691d871543ed05c3b85a91913398d106a709` |
| `ui/detection/detection_card.cpp` | `f77c1fb46581ed552eb0819131e7d27fc444f8ce208b8b9f256564290a3b9160` |

行号对应本次只读快照；本报告没有修改上述文件，也没有修改 `QUALITY_REVIEW.md` 的历史证据。
