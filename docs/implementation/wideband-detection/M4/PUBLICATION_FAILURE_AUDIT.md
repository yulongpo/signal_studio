# M4 正式 Native 发布失败只读诊断

结论：**旧 R2 PUBLICATION_FAIL / P1，根因已确认；当前最小 ID 修正代码范围关闭**。失败来自原始 `FrequencyTrack.seedId` 被直接写入仅允许 128 UTF-8 bytes 的 `Annotation.originRecordId`。前三个正式 G/sample 文件的最终 records 全部未发生 refined fusion，61/55/9 个对象的 originRecordId 全部超限。因此 `fusion:` 前缀、融合数学或原始模型效果不是这次直接根因。原生进程已完成全部 159 个窗口、落盘 final records/end marker、manifest 标 Completed，随后 CLI 恢复校验失败，尚未写出 result.json 即退出 1。主实施已修正长 ID 与两处查找，并提供 Debug/Release focused 通过；本报告最终补核同时确认 Fine fallback 的实现修正，SCN UI 矩阵例外仍待用户。

本报告首读时间为 2026-10-14 04:34，最终修正补核时间为 04:38（Asia/Shanghai）；仅新增/更新本报告，没有修改源码、冻结配置、历史结果或参数，没有构建、运行测试/GPU/模型或提交。主实施已停止旧 suite 并保留 R2 快照；下文分别记录失败版本、新工作树与 focused 范围证据，不将这些证据称为新正式 suite 已通过。

## 实际失败证据

来源为 `out/tasks/wideband-detection/M4/diagnostics/suite-release-v1/G-sample/pair-run-records.json`、各 sample 的 native/process.log，以及 manifest 按引用顺序指向的最后 publication shards。只读解析实际 final records，没有重新推理。

| G sampleId | Native / reference exit | Native wall time | final annotations | originRecordId UTF-8 bytes | refined fusion objects |
|---|---|---|---|---|---|
| `20260728-044906-196_bw_20000000_sampleTime_0.8_rollOff_0.35` | 1 / 0 | 502.529 s | 61 | 185–189；61/61 超 128 | 0 |
| `20260728-044926-842_bw_20000000_sampleTime_0.8_rollOff_0.35` | 1 / 0 | 439.416 s | 55 | 185–189；55/55 超 128 | 0 |
| `20260728-044936-468_bw_20000000_sampleTime_0.8_rollOff_0.35` | 1 / 0 | 233.071 s | 9 | 186–188；9/9 超 128 | 0 |

三份 native/process.log 均只报 `originRecordId 超过文本预算`；completion 均为 `missing-result`，没有 timeout。三个 manifest 均为 Completed、completedWindows=eligibleWindows=159、error 为空，最后 publication end marker 均存在。该 Completed 表示当前生产任务完成内存发布及持久化流程，不证明写出的 Annotation 可以按其 DTO 契约恢复。

首个实际对象的 annotation.id 为 74 bytes，合法；runId 为 87 bytes、fileId 为 69 bytes，分别也合法，但组合 originRecordId 达 187 bytes：

```text
frequency-track:87:m4-sample-default-m4-source-20260728-044906-196_bw_20000000_sampleTime_0.8_rollOff_0.3569:m4-source-20260728-044906-196_bw_20000000_sampleTime_0.8_rollOff_0.353:134:534
```

该对象 `originEvidence.parentCandidateId` 为空，`originRef` 为 193 bytes；直接触发拒绝的是顶层 originRecordId，而不是 parentCandidateId 或 originRef。

失败 Native executable 的正式 launch 身份为 SHA-256 `5f245865fee5a82f9ac23f21aebd9f8dc8cd0e632a2d554d74fa4086a148338f`，executionFreeze SHA-256 为 `ed5322a02c4fb32b159bc253af1881590348d16e5ae4ad0bbdbe5a556432c0d8`。保留的 R2 `snapshot/infrastructure/detection_projection.cpp` SHA-256 为 `b9282be37c9a9dee43832d1284c834c06ac8aac68f5064cee4f57a66c79074d0`，对应下方修正前生成逻辑。

## 生成链、DTO 与 lineage 边界

修正前位置：`domain/detection_postprocess.cpp:44–50`、`infrastructure/detection_projection.cpp:244–250`、`infrastructure/annotation_json.cpp:26–31`/`:536–545`。

纯 domain 的 seedId 为完整长度前缀 tuple：`frequency-track:` + runId + fileId + windowId + peakIndex。长度前缀避免拼接歧义，是原始 track identity；每个输入 ID 合法不能保证组合 tuple ≤128。projection 的 annotation.id 本已用 SHA-256 形成 74-byte ID，却把 originRecordId 原样赋为 track.seedId。`decodeAnnotation` 对这两个 typed ID 都施加 128-byte 上限；`domain/annotation.cpp:13–14` 的 validId 同样限 128。

`validateDetectionPublicationBudget`/`reserveAnnotation` 仅 encode 并估算 resident bytes，不执行 Annotation/DTO 有效性校验；编码本身也没有拒绝超长 typed ID。因此超长 record 可以被写入 durable shard，任务可以到 Completed。CLI 在写 result.json 之前要求 `recoverDetectionPublication` 与内存结果完全相同；recover 经 `decodeDetectionPublication` 调用严格 decoder，首先读 originRecordId 时失败，顶层 catch 只输出错误并返回 1。错误晚于全部模型/精修计算，解释了数百秒后 missing-result。

| 字段/契约 | 当前生成及预算 | 与本故障的关系 |
|---|---|---|
| 非融合 annotation.id | `detection:` + 64 hex =74 bytes；DTO≤128 | 已有定长身份，无需改变 |
| 修正前 originRecordId | 完整 track tuple；本 G=185–189；DTO≤128 | 直接根因 |
| 融合 originRecordId | `fusion:` + 64 hex =71；DTO≤128 | 本次不是根因，不应缩短它 |
| 融合 annotation.id | `detection:` + 64 hex =74 | 合法；derivedFrom 保存 parent annotation IDs |
| 原始 track seed | `originalFieldsJson.seedId` 保留完整 tuple，另有 original windows | 可保留原始证据，不必塞入 typed originRecordId |
| originRef | 非融合 `track:` + raw seed；预算1024 | 本 G约191–195，未触发；保留完整 provenance |
| fusion parentCandidateId | 原样拷贝第一个 parent.originRecordId；DTO≤128 | 若 parent 仍是超长 typed ID，融合路径也有潜在相同预算问题；应由统一 projection ID 修正一并消除 |
| domain refined fusion stableId | input segment.id，允许≤256；输出借用输入 ID 再由集成层重命名 | 不负责 Annotation.originRecordId；256 上限不能覆盖 DTO 的 128 上限 |

`detection_refined_publication.cpp:197–210` 对多个 parents 生成合法的定长融合 ID，并保留 parent snapshots、derivedFrom、union window 统计；`mergeOrigins:100–101` 保存融合 originRef 与 parentCandidateId。根修正应保证所有 parent 的 typed ID 在进入 snapshots 前已经合法，同时保留 full seed/完整 parent 原始证据。不能仅调整最终融合 ID，也不能删除父证据规避序列化。

## 最小修正与审查期间的新工作树

最小修正是在 infrastructure projection 边界建立**一个确定性 track record ID helper**：raw seed≤128 bytes 时可保留兼容 ID；超限时用固定 namespace + 完整 raw seed 的 SHA-256，例如 `track:` +64 hex=70 bytes。保留完整 raw seed 在原有 originalFieldsJson/窗口证据中；不截断 tuple、不放宽通用 DTO/domain 上限，也不仅缩短正式 CLI 的 runId/fileId 来绕过生产问题。

两个以 `annotation.originRecordId==track.seedId` 找回 track 的调用点必须共用同一 helper：`detection_time_analysis` 与 task 的 `preserveFrequencyEvidence`。否则只改 projection 会使 Fine 找不到 track，或频率证据静默丢失。annotation.id 继续基于完整原 seed 计算，原始数学/活动几何、窗口集合及 lineage 不必改变。

04:32 已只读看到主实施按此方向增加 `detectionTrackRecordId`，projection 对长 seed 使用 `track:` hash，两个查找点也调用相同 helper。它静态覆盖直接根因及 fusion parentCandidateId 的长度。此时新工作树 projection SHA-256 为 `b36bd47d8c22cb4dcf8445d7b389cf954edbc243883c766e9e275a7eec11de69`，task 为 `417494ecb2d70cf1e3b4db5faab6f4d47fd753b3c38b7586a5cacc6a432de6bf`。time_analysis 随后增加下面的 Fine-only guard，最终补核 SHA-256 为 `7e05dc86747bd5a03ea9de6acfe4c56591cf45d829dc8b244936f2e3546c29aa`。

最终补核：主实施将 time pipeline 的 track seed 改为超过 200 bytes，增加不同尾部→不同短 ID、短 seed兼容、完整原 seed保留，以及原始/融合 durable exact recovery 检查；审查者已读这些断言和 `logs/long-identity-pipeline-tests.txt` 的 `Time pipeline checks passed`。主实施提供 Debug/Release 均通过；本审查没有另行执行或宣称执行前后哈希锁定。长 ID 直接故障在该代码/focused 范围已关闭，新 R3 完整默认效果仍待独立冻结和实际结果。

另建议在 final durable 发布前执行一次 DTO/domain 契约预检，使非法 publication 在任务内明确失败，不先标 Completed 再由 CLI 恢复发现。预算检查仍保留；这是序列化契约检查，不能通过放宽规则实现。预检不是修正 ID helper 的替代。

## CPU 行为回归建议（本轮未运行）

1. **真实长 tuple**：通过 FrequencyTracker 构造本 G 的 runId87/fileId69/windowId134/peak534，确认 raw seed=187 bytes；projection typed originRecordId=70、annotation.id=74，完整 seed 和窗口证据保持；encode→decode→durable end marker→recover 完全一致。不依赖 GPU/模型。
2. **边界与确定性**：helper 对 127/128/129 UTF-8 bytes（含多字节字符串）分别保持/保持/hash；同原始 tuple重复投影身份相同，不同 tuple分开；不能按字符数而忽略 UTF-8 bytes。保留现有短 seed 的 ID。
3. **两个查找点**：长 seed 的合法 projection 仍可附上 frequencyAnalysis，Fine 能定位同一 immutable track，window evidence 不丢；错误路径不能变成“找不到 track”。
4. **融合 parents**：两个长 seed parents 的重叠活动实际融合，typed fusion originRecordId=71、parentCandidateId≤128，snapshots 的 parent typed ID 合法且完整 raw seed保留；derivedFrom、unique window最高分、original/current/r0 一致，durable 恢复成功。
5. **拒绝与原子性**：人工注入129-byte typed originRecordId 的 raw record仍被拒绝且目标 publication不替换；新 pre-publish guard若采用，应拒绝同记录且保持失败原因。旧 R2 超限 records 不应被静默改写为合法历史。
6. **完整默认对照**：新 freeze 下重跑正式完整默认样本，事件数学、源支持、整数边界、状态/原因与原始模型证据分别核对。旧三次失败和后续未完成状态原样保留并按冻结失败政策计分，不能用新成功结果覆盖旧 R2 失败历史。

## SCN Coarse margin 补核

**SCN Coarse 的卡片 margin 确实禁用，实际时间 Coarse/fallback 也固定默认6 dB；不存在“直接 Coarse 可编辑 margin”的已批准语义。**

`DetectionCard::refreshBranchState` 的 `noiseMargin_->setEnabled(strategy != Coarse)` 对所有后端生效。生产 task 对 Coarse 构造 frequencyRequest 时覆盖 margin为默认6，但 SCN 随后跳过可调 frequency helper，仍走其固定3 dB CNR/频率收边契约。`applyDetectionTimeAnalysis:263–267` 对直接 Coarse 的有效参数覆盖默认6；`:292–296` 对 fallback 同样覆盖默认6。因此 SCN 的频率固定3、Coarse时间固定6是不同阶段的现有契约；request保存的 disabled历史margin不参与 Coarse数学。

`PARAMETER_FALLBACK_AUDIT.md` 的两项历史发现需分开：失败版 Fine fallback 读取 disabled Coarse **时长**历史值，随后修正状态见下节；SCN **Fine/None** 的 margin启用/说明与实际无效仍等待矩阵决策。Coarse下可以编辑的是在 split=true 时启用的 minBurst/splitGap，不是 margin。后续修正不得因此把 Coarse margin解锁或把 SCN固定3改成用户值。

## 参数修正最终补核

上一节的 Fine 历史值问题描述对应失败版及第一次读取；04:37 已核对主实施当前修正，最新状态以下表为准。

| 项目 | 当前修正与只读核验 | 最终状态 |
|---|---|---|
| 长 track ID 发布故障 | 统一 helper仅压缩超128-byte seed；projection 与 frequency/time 两处查找共用；完整 raw seed/lineage保留；长 ID 原始及融合 durable focused通过 | 代码/focused 范围关闭；旧 R2失败历史保留、新R3默认实验待完成 |
| Fine fallback 读取 inactive Coarse历史值 | 新纯 domain `coarseDetectionFallbackParameters` 只在有效副本覆盖 margin6/min10/gap20，stored不变，split保留。新增数学 fixture覆盖 Sample/ROI实际不可靠输入、历史值变化、直接Coarse与noSplit，主实施报告16组通过 | 实现范围关闭；最终新版执行身份/新freeze由主实施归档 |
| SCN Fine/None margin无效却启用 | 业务参数矩阵尚未改变；SCN Coarse已禁用margin，时间固定6、频率固定3，均与本次补核一致 | Fine/None UI例外仍等待用户确认，不能标全参数契约通过 |

第一次 helper修正曾仍用于 `Fallback && strategy!=None`，会覆盖直接 Coarse 的已启用 minBurst/gap筛选：直接 Coarse拒绝后，再用默认10/20重跑可重新发布活动。本审查立即提出，主实施已将 `detection_time_analysis.cpp:274–277` 收窄为 `Fallback && (Sample || Roi)`。当前直接 Coarse不再进入默认二次分析；这一 guard 是关闭集成作用域的必要条件，不能仅依赖“直接调用 refineDetection”单测通过。最短粗活动和粗间隔仍按直接 Coarse的用户有效值计算。

默认6/10/20的数值、Sample/ROI成功数学、Coarse自身有效参数与noSplit政策保持原定义；ID修正只改变超长typed record ID，annotation.id/活动数学不变。由于可执行文件/源码身份已改变，新R3仍须新freeze与完整默认样本核验；不得与旧R2成功/失败输出混合或覆盖历史。
