# M4 独立规格审查

当前复审结论：**SPEC_PASS（已检查的代码范围）**。首审 R1–R3 与复审 R4 的实现问题均已关闭；R4 最后一次源码修正仍待新版 focused pipeline 检查。该结论只表示当前已读代码符合相应规格，不代表完整 M4 节点验收：G/T 效果、同源数值对照、当前项目回归及规定第二屏原生旅程仍待实际证据。

审查基线为 M3 `6ed5ce0` 后的 M4 工作区，2026-10-14（Asia/Shanghai）。只读核对业务源码、已批准规划、M0 冻结数学文件及当前测试；只修改本报告，没有构建、GPU/模型推理、全量 G/T、原生截图、业务源码修改或提交。行号对应首审快照，修正后需复核。

## 复审关闭记录

复审时间：2026-10-14 03:00（Asia/Shanghai）。下方首审 R1–R3 保留历史快照，不代表当前仍未修正。

| 首审项 | 当前修正及核验 | 结论 |
|---|---|---|
| R1 精修后去重 | 新纯 domain `detection_refined_fusion` 按严格 source-sample overlap/touch、同载波 IoU/containment、确定性最佳 pair 反复融合；正 sample gap 保留。task 在 time refinement 与 window evidence 后调用 `fuseDetectionPublication`；集成层保留父快照/lineage，union 唯一窗口最高分，重建最终多段 r0 | 几何与来源合并实现已关闭；R4 单列证据范围问题 |
| R2 fallback 工作计数 | `readEnergy` 与 `readSpectrum` 在分配/读取前，先 forecast cells 并检查同一个 `2^34-consumedCells` 余量；主分支与 fallback 传入同一计数引用。累计超限错误不被识别为算法资源 fallback | 静态实现已关闭；没有专项执行到累计上限的注入测试 |
| R3 raw r0 范围 | decoder 要求 Pending、唯一 `algorithm-origin` r0、唯一原始 review、current segments==origin originals、无 Manual 方法/类别/删除状态、RF observation/segments，且 r0 changes 必须无 before、after 精确等于每个原始活动 | 已关闭；当前多段 roundtrip、origin 不一致/合法人工 revision 拒绝及融合记录恢复通过 |
| R4 融合证据范围 | 新版融合字段从独立 `signalstudio-refined-publication-v1` summary 构造，只保存 union uniqueHits/denominator/coverage/windows；所有单 parent 时间/频率测量及原始窗口/活动留在 `refinedFusion.parents` snapshots，清除未注明范围的顶层单 parent 测量 | 静态实现已关闭；最后修正后的新版 focused pipeline 尚待主任务构建 |

本次独立执行三个 Debug 二进制：domain refinement **15 组通过**、refined fusion **9 组通过**、time publication pipeline **通过**。前两者执行前后 SHA-256 相同；写入时间晚于其相关源码。pipeline 执行期间主任务构建替换了二进制，其结果暂不作为精确身份锁定的当前验证，待新版重跑后补记。pipeline 不带 `--file`，仅运行本地持久化/融合检查，没有模型或 GPU 前向。

| 测试二进制 | UTC 写入时间 | SHA-256 |
|---|---|---|
| `SignalStudioDetectionRefinementTests.exe` | `2026-10-13T18:40:56.7574496Z` | `2c5e600be69da8126c281e80112a0880d472d30c6fa349c5f758bc7eed3f0fdb` |
| `SignalStudioDetectionRefinedFusionTests.exe` | `2026-10-13T18:40:56.535811Z` | `e3b12b15ad96a793faa891228d01729de5ade1970d05aed31fb8d336153ad386` |
| `SignalStudioDetectionTimePipelineTests.exe`（执行前；被主任务构建替换） | `2026-10-13T18:57:08.6495274Z` | `72b24e8caadae6d82752d93ed00842a7d7e64f077737e7d8dd1ce02ba4660571` |
| `SignalStudioDetectionTimePipelineTests.exe`（执行后；仍早于 R4 最后修正） | `2026-10-13T18:59:23.7733873Z` | `e6d72d9abe8ffa0dcec8527ec02c81d9727bb2a0208e14a1a38990e3ac56bdc7` |

### R4 · P2 · 融合对象继承单 parent 测量及旧顶层统计

本项已于 `2026-10-13T19:00:09.99926Z` 的源码修正关闭。下文保留发现时快照；现在融合对象不会向证据卡提供未注明 parent 范围的 noise/threshold。

位置：`infrastructure/detection_refined_publication.cpp:102`、`:109`；`ui/annotations/annotation_evidence_widget.cpp:840`。

融合后 origin 的 windowIds/mean/max 与 `windows` 已经改成 union，但其顶层 `uniqueHits/windowCoverage/fusedLowerHz/fusedUpperHz/seedId` 仍来自第一 parent。`timeAnalysis` 也保留第一 parent 的 noise/threshold 等测量标量；仅移除 trace。证据卡直接将这些标量显示为合并对象的噪底/比较门限，缺少 parent 范围说明。这形成同一原始字段内的统计矛盾，并把单 parent 测量用于合并对象说明。

应以明确融合 summary/union 统计表示新对象；单 parent 时间/频率测量保持在 `refinedFusion.parents` 的原始快照中，或明确标示 parent scope 并让 UI 分 parent 说明。不能为合并对象复制一个 parent 的测量作为未注明范围的统一结果。相关检查应确认最终活动、窗口统计与测量范围一致。

---

## 首审记录（历史快照）

首审结论为 **SPEC_FAIL**；下方 R1–R3 对应修正前实现。当前逐项状态以上表为准。

## 范围与依据

- 已批准的 `docs/plans/wideband-detection/03_ARCHITECTURE_AND_CONTRACTS.md`、`04_ALGORITHMS_AND_VALIDATION.md`、`05_MILESTONES_AND_ACCEPTANCE.md`、`07_PROTOTYPE_AND_INTERACTION_REVIEW.md`、`08_FFSCN_TEST_GUI_CAPABILITIES.md`；M0 ADR、M0-P 冻结交互与本次用户确认的 `PARAMETER_DECISION.md` 方案 A。
- M4 `VALIDATION_PROTOCOL.md` 的 F06–F10 数学口径、物理支持、失败政策及预先固定的数值/效果门槛。参数语义以用户后来确认的方案 A 为准；协议中保留的冻结 GUI 旧参数描述不能覆盖此确认。
- `domain/detection_refinement.h/.cpp`、`infrastructure/detection_sample_energy.*`、`detection_time_analysis.*`、task/store/projection 的 M4 增量、实际进度显示及对应测试。
- 外部文件仅只读比较数学，不导入或执行业务模块。重新计算 `refine_sample.py`、`refine_roi.py`、`business/detection.py`、`business/evaluation.py` 的 SHA-256，均与 M0/M4 已记录身份一致。只读的 `business/tracker.py` 用于核实冻结去重定义，不作为本仓库构建输入。

## 首审发现

### R1 · P1 · 缺少时间精修后的活动去重

位置：`infrastructure/detection_time_analysis.cpp:314`、`:335`；`domain/detection_postprocess.cpp:495`、`:546`。

`VALIDATION_PROTOCOL.md:71` 明确要求精修后再执行 `maxGap=0` 的同载波去重，仅合并重叠/接触段，不桥接正静默间隔。当前时间分析逐个 Annotation 写入精修活动，然后直接发布；全部融合只在此前 FrequencyTracker 的名义模型支持上发生。sample 搜索向两端各扩展 8 ms，可以使此前仍分开的两个 parent 在真实精修后出现相同或重叠活动；前置融合不能保证后置事件唯一，最终会重复计事件并增加 FP。

应独立实现确定性的精修后去重，保持载波匹配规则、零 gap、唯一窗口 union/全分母统计，以及可追溯的原始 parent/活动证据。覆盖重叠、接触、正 gap 不合并和一 parent 多段的反例；不能用扩大 `minOff` 或手工修边代替。

### R2 · P2 · coarse fallback 没有进入累计工作上限

位置：`infrastructure/detection_time_analysis.cpp:230`、`:247`、`:250`、`:265`。

累计 `totalCells` 只统计主分支成功读取的 energy 或 spectrum。sample 不可靠或资源超限后，代码另读实际 STFT 为 coarse fallback；这次读取成功后的 `powerDb.size()` 没有累加，也没有再次核对 `2^34` 累计 cell 上限。因此大量 fallback parent 可以使实际工作超出声明的累计限制，即使主分支计数一直很低。

所有成功读取与 fallback 分析应共用累计计数及剩余工作预算检查，保持超限为明确任务错误/资源边界。测试需证明 fallback 工作也消耗同一限额。

### R3 · P2 · refined durable 记录未严格限定为原始算法 r0

位置：`infrastructure/detection_projection.cpp:379`、`:397`；`domain/annotation.cpp:221`。

当前 decoder 核对算法 source/run、observation 与当前活动的 ROI bounds、AlgorithmEstimated/未 confirmed，但没有要求原始 Pending r0、唯一 `algorithm-origin` revision、原始活动与 r0 after/current activity 一致及明确 RF 坐标。通用 Annotation 校验只验证 revision 引用关系，允许合法的额外人工 revision 或不同 `origin.originalSegments`。这种记录能被当成 immutable raw publication 恢复，混淆原始算法结果与后续复核历史。

durable production projection 应验证原始 r0/review/坐标/活动证据的完整关系，拒绝人工修订和原始活动不一致的记录。扩展已有多段 roundtrip，加入合法外观但超出 raw scope 的篡改拒绝用例；正常工程里的人工 revision 仍属于工程 Annotation 存储。

## 已核对的符合项

- Sample 测量来自实际源 IQ，搜索整个 nominal support ±8 ms 后按文件可用前缀裁边；局部相位、complex64 raised-cosine exact-length FFT 保持实际长度。mixed-radix/Bluestein 的卷积补零属于精确 DFT 算法内部，不改变信号分析长度，也未换成 FIR 或分块信号滤波。
- domain 消费非负 float32 带限线性功率，使用因果左侧零补移动平均、0.5% noise percentile、中位 burst、1.3 ratio、strict mask 与 3% 可靠性；先删短 run 再合并严格小于 minOff 的 gap。uint64 样本半开区间、最终短 cell 与发布 ROI 裁边分别处理。
- 用户确认 A 的 ROI edgeRatio/.6、自适应门限、奇数平滑核 1–63 单元、默认五单元；sample continuous 的 `.5*(edgeRatio/.6)` 已落实。用户 noSplit 不读取 editable minOn/minOff；sample 保留固定 .5 ms reliability，ROI pooled minOn 保留一个 cell，分别记录 `durationParametersApplied=false`。ROI 后续至少三个真实帧要求是冻结的独立观测支持检查，并不把插值列当真实帧。
- 512×64 ROI 的半像素双线性 resize、三次减均值后一次 ReLU、正像素自适应 time/frequency mask、物理 floor/ceil 边界；少于三真实帧或全部 run 无效时保留 fallback/unknown，未伪造成功。
- Sample/ROI/Coarse/None 互斥；fine 前没有执行破坏短突发的 coarse minBurst 时间过滤。None 没有活动段；不可靠 fine 只有实际测得的 coarse 能提供 fallback activity，否则保持未知时间 observation。
- 真正读取错误、nonfinite、取消与源变化不会被转换成算法资源 fallback；资源 fallback 原因单独保留。时间结果在临时 publication 中完成，成功、最终源指纹检查与结果预算通过后才替换调用者。
- 数值身份为 `signalstudio-refinement-m0p-parameters-v2`；float32 存储/NumPy 弱标量 percentile 与 float64 rolling sum 的差异已披露，不能将数学测试称为冻结 CUDA tensor parity。
- 唯一窗口仅贡献其最大分数一次，coverage 使用冻结全部 eligible-window 分母。C++ 评价器每阈值独立最大 cardinality 再最大 summed IoU，未知时间 TF/time IoU=0且保留分母；按文件匹配，Failed/NotProcessed 保留全部真值、零预测。
- run manifest v2 单独写阶段与 refinement counters；v1 默认 `none/0/0`，拒绝 v1 携带 v2 字段，v2 拒绝未知阶段或计数超限。UI 显示实际窗口/候选计数与当前计算阶段，没有将 model windows 全部完成当作时间精修或落盘完成。
- refined multi-activity durable 记录保留 origin、r0 segment changes 与窗口 raw provenance。未出现 publication end marker 时恢复 raw 频率前缀观察，恢复过程不重新跑 IQ 时间精修，不假造精确活动，也不自动更新工程索引。

## 当前检查证据与边界

已逐项读过 15 组 domain 行为测试、8 种长度的 exact FFT/direct DFT 与 roundtrip 测试、实际功率滤波/拒绝预算/取消测试，以及多活动段 durable roundtrip 和 v1/v2 manifest 检查。它们覆盖数学、数据映射与持久化行为，不能代替实际 G/T 数值与效果。

首审读取时，三个 Debug 测试二进制的写入时间均晚于相关 M4 源码，但 R1–R3 尚未修正；本报告没有另跑旧快照二进制，也不重复主任务正在执行的完整 CTest。修正后的版本与实际日志须在复审中重新核对。

完整节点批准仍待：当前版本项目 CTest/UI 回归；真实 IQ sample/ROI/coarse/None/noSplit 默认与批准扩展参数对照；冻结 G 10文件/594真值和完整 T holdout、失败/未知分母及 paired bootstrap；sample power/envelope/mask/integer runs 和 ROI pooled mask/maps 的同源 parity；短突发 Recall/起止P95与continuous跨静默反例；规定第二连接屏 4K/150% 全屏原生精修证据、复核/不复制接受/人工修订/通道与保存重開旅程。没有实际证据的项继续标未完成，不以手工修边、历史指标或数学 fixture 抵消。

本次不进入 M5，也不声称 GPU 渲染、完整部署、实际 RF 采集子域、长任务资源峰值及系统性故障压力已通过。
