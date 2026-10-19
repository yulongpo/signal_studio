# M3 独立规格审查

当前复审结论：**SPEC_PASS（已核对的实现与 L1/L2/频率层证据）**。该结论不代表完整 M3 节点验收已完成；完整脚本回归和第二屏原生截图仍待主任务完成并核对，通过后才可本地提交。

复审时间：2026-10-14 00:45（Asia/Shanghai）。首审 R1–R5 均已关闭。本次继续只读业务源码和实际证据，没有执行构建、推理、回归或提交。下方保留 00:16 的首审问题作为可追溯记录，不能将其视为当前仍未修正的问题。

## 复审关闭记录

| 首审项 | 当前实现与已读取证据 | 结论 |
| --- | --- | --- |
| R1 峰索引 | `decodeProductionResults` 输出四字段候选，projector 校验并保存真实 peakIndex；原频带及持久化 records 保留索引。`projectionRoundtrip` 现在同时传入两个分离候选，检查两条不同稳定 ID 及精确 roundtrip；实际两个 profile 的四个多候选窗口也已通过 | 已关闭 |
| R2 SCN 顺序 | 独立 `postprocessScnWindow` 每个真实 16 帧窗口执行最大谱 CNR 3 dB→参考整数测量边界→频率融合→全未滤占用屏蔽→平均谱邻带 minimum+3 dB/最大谱内部 gate→150 上限→tracker。正式 SCN 路径跳过全轨迹二次频率修边；原始/接受/拒绝/fused/门限/原因写入 scnPostprocess 并随 Annotation 保存 | 已关闭 |
| R3 前缀发布 | finish 在有 durable completedWindows 且缺投影时，于终态落盘/可见之前调用恢复投影。实际 FFSCN 生产任务在至少两个已完成窗口后取消，`takePublication` 非空且与明确恢复投影相同；观察仍无活动段，分母为完整 eligible。Failed 与 Cancelled 共用此终态代码，实际 worker 故障专项尚未在此复审运行 | 实现已关闭；实际取消证据通过 |
| R4 频率证据 | 只有 hasNoiseFloor 且 hasPeak 时才写 estimatedCnrDb；测量 flags、peak、bin、邻点数、global frame 范围完整保存。SCN 每窗 CNR/修边证据与 raw head 引用保留；没有用默认 peak=0 伪造估计。正式 SCN 没有多余的 GUI 全轨迹 frequencyAnalysis 字段 | 已关闭 |
| R5 完整频率对照 | 新独立 Python oracle 逐个读取并验证 raw head/canonical chunk/最终 projection；核对全四窗解码、SCN CNR/fusion/MIN、跨窗关联、GUI percentile/mean 修边、稳定 ID、所有原窗口频带及唯一统计。主线和 SCN 各四窗、各八条最终轨迹通过，原始/最终 Hz 边界最大误差 0 | 已关闭 |

SCN 反例测试 `scnOrderAndOccupancy` 包含两条候选：第二条被 CNR 拒绝，但仍屏蔽第一条邻带；结果 30–34 bin、minimum=-20 dB、一个有效噪点。真实频率对照的数据没有 CNR 拒绝样例，不能以实际文件替代该反例。source/profile UI 保持已批准 M0-P 的布局和参数编辑行为；SCN 对照内部固定 CNR/fusion/minimum-margin 与界面下一阶段精修参数分别在实际 evidence 中记录，不将 UI 噪底裕量 6 dB 谎报成本步骤的已生效值。

## 复审实际证据及身份核对

已读取 `out/tasks/wideband-detection/M3/diagnostics/pipeline-parity.json`、`frequency-parity.json`、`pipeline/pipeline-report.json` 和 live 日志；独立重新计算频率报告所引用的八份 raw heads、两份 annotations 与 pipeline report，**11/11 SHA-256 一致**。工具源码和 M0 assets hash 也与频率报告中的身份一致。

| 项目 | FFSCN 主线 | SCN 对照 |
| --- | ---: | ---: |
| L1 全首窗谱元素 | 655360 | 524288 |
| L1 density 最大误差 | 0.00115966796875 dB | 0.0047760009765625 dB |
| L1 normalized/input 最大误差 | 0.0001583099365234375 | 0.0047760009765625 |
| L2 三头超容差元素 | 0 | 0 |
| eligible/completed 窗口 | 4/4 | 4/4 |
| 频率 oracle/native 最终轨迹 | 8/8 | 8/8 |
| 最终原始/修正频边最大误差 | 0 Hz | 0 Hz |
| 唯一 mean/max/hits/分母/coverage 与窗口证据 | 一致 | 一致 |
| 完成后精确恢复 | 通过 | 通过 |
| 实际取消前缀非空及精确恢复 | 通过 | 本次未执行，不写通过 |

主线 L2 oracle 为冻结 epoch-1200 的独立 PyTorch CUDA 图；SCN L2 为同冻结 engine 的 Python TensorRT 对照，仅证明本机绑定/最大谱前向一致，不声称原训练模型、IQ 泛化或准确率已验证。L1 沿用 density 0.02 dB、主线 normalized 0.002、SCN physical input 0.02；L2 沿用 atol=1e-4/rtol=1e-3。全窗频边 0.05 bin、score 0.001、噪底/CNR 0.02 dB、coverage 1e-12，未根据结果扩大门限。

| 当前证据 | SHA-256 |
| --- | --- |
| `model_frequency_parity.py` | `eff7a6849c70e11c6a5ef2caa5a754f7df5cbec7cf00b7438c84a6c284174681` |
| M0 `assets.json` | `1cd7be1858dc750e73e2c3ab2966493bd1e3027c6d43b64491c751e7169a2c94` |
| `frequency-parity.json` | `951d1e3ddb9c527105599b2de59cde365189ccc696e91b2b7c356c1c01dad3c7` |
| `pipeline-parity.json` | `1e0a759a571d748adea6784c050c6554bfa15fbaedfc8ee42d006dcb98b6ca3a` |

本次实际输入为 M0 冻结 0.8 s WAV 的 0–20 ms requested ROI，分析完整模型上下文可延伸至 ROI 外；不是完整 G/T 组效果评估。GUI float64 均值、唯一窗口最大分数口径和 SCN 物理裁边的版本差异已披露。所有结果保持 unknown event time/no class，M4 sample/ROI/二维评价及历史 TF-F1 仍未完成。

## 节点提交前仍需主任务核对

截至复审读取时，`debug-ctest.log` 停留在第 19 项 `signal_studio_ui` 开始，尚无完整结束结果；`native-capture.log` 仍无最终报告。本报告不能写 19/19 或原生 25 场景已通过。主任务须完成项目 `scripts` 入口回归、规定第二屏 4K/150% 全屏截图及真实模型/原型对照，归档实际结果后才能完成 M3 本地提交。若其后业务源码改变，应对相应修正补充复核，不因本次 SPEC_PASS 跳过必要验证。

---

## 首审记录（历史快照）

首审结论：**SPEC_FAIL（00:16 实现中快照）**。

审查时间：2026-10-14 00:16（Asia/Shanghai）。基线 HEAD 为 `908016258eb9f208e32d9607bcd86bf714baeffc`，M2 已提交；本报告审查其后的 M3 工作区。实现仍在修改，以下行号指本次读取的快照，修正后需复核。

本审查只读取源码、规划、冻结资产与现有证据，不修改业务源码，不执行构建、全量测试、外部业务代码、模型转换或推理，不提交。已知的后端 host/device 预算修正由实现代理处理，不作为本报告重复发现。

## 范围与依据

- 已批准规划 `04_ALGORITHMS_AND_VALIDATION.md`、`05_MILESTONES_AND_ACCEPTANCE.md`、`07_PROTOTYPE_AND_INTERACTION_REVIEW.md`、`08_FFSCN_TEST_GUI_CAPABILITIES.md`；M0 ADR、profile 和资产清单；M0-P 的冻结交互约束。
- 当前 domain 解码/关联、native model backend、source、projection、frequency analysis、task/store、worker、模型包工具、CMake 和相关测试。
- 外部源只读核对数学与处理顺序，未复制、导入或执行其业务实现。

本次重新计算的外部 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| GUI `business/tracker.py` | `77c36a2b7f584c560e2109c97a78ff1688b527ace9765b736941657f0e862ed1` |
| GUI `business/detection.py` | `f825313f692b13813624b5adef2db8fa46099b6a11978cf7a088817160503c75` |
| GUI `algorithm/nms.py` | `5da650118b1fb8e4ee4805cb6bbc0bc53aa235d220c3c35901dd41d2aa0a6cb0` |
| SCN `algorithm/refine/BandwidthRefiner.cpp` | `979bf8d0e2152cc50be3769a9e4bc956746080c5226b7592ec4ba95c1ebe0cfb` |
| SCN `algorithm/refine/CnrRefiner.cpp` | `6319dd4a9f1b1d9c0f5b41e3c2e9cdedf292aa8911e432060e37c14890f2a69c` |

前四项与 M0 清单/实现文档中冻结身份一致。最后一项作为本次顺序核对的只读参考身份记录，不声称它已经在 M0 冻结。

## 必须修正的发现

### R1 · P1 · 峰索引丢失阻断真实多候选窗口

位置：`infrastructure/detection_task.cpp:273`、`infrastructure/detection_projection.cpp:163`、`domain/detection_postprocess.cpp:355`。

原始头在主进程解码后，候选 JSON 仅保存 `lowerBin/upperBin/score`。projector 将每条候选的 `peakIndex` 固定为 0，而 FrequencyTracker 强制每窗峰索引唯一。因此一个真实窗口只要检出两个候选就失败为 `invalid-window-frequency-candidate`。若仅取消唯一性检查，同窗不同轨迹又会得到相同 seed/Annotation ID。

应完整保存真实解码峰索引，贯通消费、durable window records、恢复及证据；增加至少两个独立载波的同窗投影/恢复用例。当前 `projectionRoundtrip` 只有单候选，不能覆盖此错误。

### R2 · P1 · SCN 的顺序和占用集合不符合冻结规则

位置：`infrastructure/detection_task.cpp:851`、`infrastructure/detection_task.cpp:915`、`infrastructure/detection_frequency_analysis.cpp` 中 `occupancy` 构造及 SCN 轨迹谱积累分支。

规划 04 §3 要求 CNR 筛选、跨频窗融合、向内带宽修正、跟踪。当前先用 FrequencyTracker 跨时间关联并完成 minHits 过滤，之后从整条轨迹支持区间重新积累平均/最大谱修边。当前没有 SCN CNR 参数或筛选阶段。先关联后修边会改变 IoU 匹配、封闭/重连和最终轨迹数，不能称为冻结 SCN 顺序的移植。

SCN 带宽修正的屏蔽集合必须来自全部未经 CNR 和展示过滤的 fused candidates。参考 `DetectionEngine.cpp` 保存独立 `allCandidates` 并在修正/截断前冻结 occupancy；当前 occupancy 仅由 minHits 后 retained tracks 构成，低 CNR/少命中候选消失后会被当作噪声。

应在 SCN 独立路径保持最大谱唯一前向，按规定顺序处理，同时保存原始候选、CNR 接受/拒绝理由和完整占用集合。CNR 的邻带均值与带宽修正的邻带最小值是不同步骤，不可用同一个估计替换。验证至少包含被 CNR 拒绝的邻近候选仍参与 occupancy，以及修边会改变后续关联的例子。

### R3 · P1 · 取消/失败后的真实前缀结果无法通过当前 UI 恢复

位置：`infrastructure/detection_task.cpp:887`、`infrastructure/detection_task.cpp:912`、`ui/detection/detection_panel.cpp:588`、`ui/detection/detection_panel.cpp:769`。

每个 production batch 已落盘并增加 completedWindows，但 job 的 annotations 在全部窗口和频率修正完成之前一直为空。中途取消或 worker 故障会发布空来源集合；运行索引同时已进入 FileState。当前“查找未引用结果”仅检查不在 FileState 中的 run，无法恢复已经引用的这条取消/失败运行的 completed prefix。源码中的 raw-prefix recovery API 并不能证明用户有可用恢复流程。

这与批准 D3“取消和保留已完成片段”及 S6 语义不符。应在终态落盘/发布前构造已完成前缀的观察投影，或为已引用且缺投影的运行提供明确恢复路径，并遵守代际/源指纹检查和无精确续算边界。验证应运行真实 production 路径，在至少一个 durable batch 后取消及故障，检查前缀观察、覆盖、保存重开和重复恢复幂等；fixture 取消测试不能替代。

### R4 · P2 · 未测量峰值被保存为 CNR，真实频率证据字段丢失

位置：`infrastructure/detection_task.cpp:285`，尤其 `318–322`。

`preserveFrequencyEvidence` 只根据 hasNoiseFloor 写 estimatedCnrDb。SCN 分支以及 GUI 的不足三帧 fallback 都可以有噪底但没有测量 peak；默认 peakDb=0 因而被当作真实峰值参与 CNR，界面会显示没有实际测量依据的数值。

应只在 hasPeak 和 hasNoiseFloor 同时有效时保存该估计。当前投影还丢弃 `hasPeak/hasMeasuredBins`、peakDb、noiseSampleCount、original/measured bin、globalFirstFrame/globalLastFrame 等已经由 frequency analysis 产生的证据。F05 要求原始/修正边界、门限和原因可审查；这些字段应无损持久化，未测量值必须明确缺失。复核应覆盖 SCN、短上下文 fallback、GUI margin=0 和确实收缩四种情况。

## 尚未满足的 M3 退出证据

### R5 · P1 · 缺真实多窗完整频率链对照

位置：`tools/detection/model_pipeline_parity.py:47`、`tests/detection_pipeline_tests.cpp:212`；规划 05 的 M3 退出条件。

当前 parity 工具只比较第一模型窗的完整预处理、相同输入原始 heads 及单窗解码；pipeline test 检查非空、unknown time/class、denominator 及重开一致。它们尚未证明窗口候选→关联→融合→GUI 频率修正的完整频率层迁移一致，也未验证 SCN 的规定顺序。

应在冻结的实际 IQ 上建立独立数学 oracle，不导入外部 business 源码，记录所有参与窗口及每个阶段的候选/轨迹数量、排序、bin/Hz 边界、原始/修正频带、唯一窗口命中与分母、噪底/门限差异。容差在运行前固定；GUI 的 float64 均值及 unique-window score 修正作为明确数值版本差异单列，不能自动被视为旧 GUI 逐值等价。SCN 频率对照与 HY 历史指标分别报告。二维效果、sample/ROI 和事件起止在 M4，不作为本次缺项补做。

M3 当前仍缺统一节点自检、必要原生真实模型截图/原型对照及最终验收归档；本报告没有运行这些检查，不能从既有模型预备探针推导它们已通过。确认上述问题解决并验证后，才可更新为 SPEC_PASS、完成本地提交，再进入 M4。

## 静态核对中符合的部分

- FFSCN 7 点峰、score/NMS 严格边界、TopK 稳定索引和 retained-only NMS；SCN 的 3 点/epsilon 与含边界规则分别实现。
- 固定 source 网格、真实帧/anchored tail、density/normalized tensor 区分；ROI 报告裁剪与完整名义/FFT 上下文分离，不以显示抽帧作为模型输入。
- FrequencyTrack 保留唯一窗口集合、immutable eligible denominator、原窗口频带和独立观察上下文；不创造发射时间/类别。
- GUI 全局 percentile 与 SCN 邻带 minimum 保留不同方法身份；SCN 平均谱没有进入模型前向。
- 固定 FP32 引擎与运行时哈希、精确 TensorRT 头版本、输入/三头形状/strides/device 检查及原始头落盘；实验 131072、FP16 和 ONNX CPU 未静默变为生产回退。
- 模型包工具可迁移并使用标准库；开发 Python 优先 `D:/profiles/anaconda/python.exe`，任务输出遵循新的 `out/tasks/wideband-detection/M3/` 规则。当前 UI 增量仍复用已确认卡片位置和原型流程，没有另加模型配置工作流。

这些为静态符合项，不替代实际构建、GPU 前向、完整频率对照、Qt/截图或性能验收。
