# SS-DE-RPO M0 基线

- 仓库根目录：`D:/project/signal_studio_rebuild`
- 分支：`rebuild/cursor`，跟踪 `origin/rebuild/cursor`
- 本地 HEAD：`82935ab34ec6126707da152bf46d1f352458ab0c`
- 远端 `origin/rebuild/cursor`（2026-10-28 fetch）：同一 SHA，无新增提交
- 已跟踪文件无本地修改。未跟踪且必须保留、不得删除或覆盖：
  - `docs/implementation/ux-r3/promotion-fbb73db/`
  - `docs/implementation/ux-r4/INDEPENDENT_REVIEW.md`
  - `docs/implementation/ux-r4/PUBLISH_RECORD.json`
  - `reference/SS-DE-RPO/`（本任务包）
  - `reference/SS-DE-RPO_Task_Package_v2.0.zip`
  - `reference/Signal_Studio_Cursor_MultiAgent_Repair_Package_20261028.zip`
  - `reference/Signal_Studio_Cursor_MultiAgent_Repair_Package_20261028/`
  - `reference/Signal_Studio_UX_R5_Reference_Task_Package.zip`
  - `reference/Signal_Studio_UX_R6_Codex_Task_Package.zip`
- 审查线索 HEAD 与当前 HEAD 相同，仍以当前源码为准，不把旧日志当作仍复现的证明。

## 当前源码已核对（CONFIRMED_CODE）

| 问题 | 结论 | 锚点 |
|---|---|---|
| Sample 8 GiB 授权被内层拒绝 | 仍存在。`working = workingSetBudgetBytes * 3/4`，`memoryGrantBytes = working`，`workingCellBudget = working/12`。`contextValid` 在 `memoryGrantBytes > 1024 MiB` 或 `workingCellBudget > 256*1024*1024` 时返回同一句 `Invalid sample context/configuration/budgets`。默认 8 GiB 得到 6 GiB grant 与 512 Mi cell budget，确定性拒绝。 | `infrastructure/detection_time_analysis.cpp` `applyDetectionTimeAnalysis` / `boundedSample`；`infrastructure/detection_sample_stream.cpp` `contextValid` |
| raw-head 2044×196608 与默认 OFF | 默认 `retainRawModelOutput=false` 且 `rawHeadBudgetBytes=0` 时，现有测试已要求 2044 窗口可通过 preflight，且不写入 raw-head 配额。显式 256 MiB 配额即使 retain 为 false，现有测试仍要求拒绝。不得改掉该回归。 | `tests/detection_task_tests.cpp` `testWidebandPreflightOmitsRawHeadsAndKeepsJsonBudget` |
| 启动重入文案 | 仍存在。`DetectionCard::submit` 只看 `unfinishedRun()`；`startRequest` 异步前设置 `commitInFlight_`，重入与工程写保护都说「检测结果正在提交」。 | `ui/detection/detection_card.cpp` `submit` / `updateShortStatus`；`ui/detection/detection_panel.cpp` `startRequest` |
| 正式 publication | 比较仍在。`windows=N/N` 不是 Completed。`preserveDurableModelEvidence` 在 HEAD 中存在，不得删除恢复比较。 | `application/detection_result_service.cpp` 约 719–729 行；`infrastructure/detection_task.cpp` `preserveDurableModelEvidence` |
| UX-R6 旧结果 | `beginDetection` 在启动事务接受时 `discardCommittedFormalResult`，失败不恢复。参数编辑本身不调用该函数。拒绝在保存前的启动会 `restoreFormalDisplay`。不得改成「成功后才替换」。卡片 tooltip 与该语义不一致，应对齐代码。 | `application/detection_result_service.cpp` `beginDetection`；`ui/detection/detection_panel.cpp` `suppressFormalDisplay` / `restoreFormalDisplay` |
| TensorRT batch | 默认 `maximumExecutionBatch = 2`。装载时已按 optimization profile 把物理 batch 夹到 profile 最大档，并在资源失败时有降档重试。禁止把默认 batch 改成 16。 | `infrastructure/detection_model_backend.h`；`detection_model_backend.cpp` profile 选择与 `retryableResourceFailure` |

## 子代理工作区

独立 worktree 与分支 `fix/ss-de-rpo-{a..g}` 由主代理在冻结本契约后创建。H 在集成 HEAD 上只读终审，不建写入分支。
