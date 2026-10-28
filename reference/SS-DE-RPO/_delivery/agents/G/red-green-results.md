# SS-DE-RPO · Agent G 红绿结果

基线 HEAD：`82935ab34ec6126707da152bf46d1f352458ab0c`  
分支：`fix/ss-de-rpo-g`  
工作树：`D:\project\ss-de-rpo-wt\g`  
任务版本：M1 契约（冻结文件，本代理未改）

## 执行状态

`BLOCKED_BUILD`。工作树没有 CMake 缓存，环境没有 `SS_QT_ROOT`。按任务要求没有做完整解决方案构建，因此下面没有进程退出码，也没有把源码阅读写成已运行的 PASS/FAIL。

计划命令（主代理登记 CMake 之后）：

```text
SignalStudioSsDeRpoLogicTests
SignalStudioSsDeRpoUiTests
```

CTest 名：`signal_studio_ss_de_rpo_logic`、`signal_studio_ss_de_rpo_ui`。

## 源码对照下的期望（不是运行结果）

| 用例 | 期望 | 依据 |
|---|---|---|
| R-01 | **RED**。`makePlan` 失败，`Failure::detail` 为 `Invalid sample context/configuration/budgets`。测试会把该字符串打到 FAIL 行。 | `infrastructure/detection_sample_stream.cpp` `contextValid`：`memoryGrantBytes>1024*MiB` 与其它非法条件共用这一句。6 GiB（`6ull<<30`）大于 1 GiB。cell ceiling 相等本身不触发 `workingCellBudget>256*1024*1024ULL`。 |
| R-01-cell-ceiling-allowed | 期望通过。96 MiB grant + `workingCellBudget==256*1024*1024` 能通过 `contextValid`，小上下文 `makePlan` 返回 true。 | 同上，比较是 `>` 不是 `>=`。 |
| R-01-cell-ceiling-plus-one | 期望通过（断言为「必须拒绝」）。96 MiB grant + ceiling+1，`makePlan` 返回 false。不锁定旧文案，避免修复把错误码拆开后变红。 | `workingCellBudget>256*1024*1024ULL`。 |
| R-04 | 期望通过。retain false、`rawHeadBudgetBytes==0`、非 legacy、2044×196608、JSON 预算 64 MiB：预检返回空，配额保持 0。 | `explicitRawHeadPreflightError` 在配额为 0 时直接返回空；随后 JSON 每窗 64 B 低于预算。 |
| R-04-explicit-256MiB | 期望通过。仍失败，文案含 `before inference`、`2044 windows x 196608 bytes`、`limit 268435456`。 | `explicitRawHeadPreflightError`。256 MiB = 268435456。2044 > 268435456/196608。 |
| R-05 | 期望通过。retain true、配额 0、卷探针剩余 1 MiB：失败，文案含 `401866752` 或同时含 2044 与 196608，并含 `bytes remain`。配额不被写成需求量。 | `retainedRawHeadPreflight`。`2044*196608=401866752`。探针剩余 1048576。 |
| R-12-nondurable | 期望通过。三旗标都关闭时，假 `lastInference` 不写入 `supplementalOriginalText` / `supplementalHash`。 | `detectionModelEvidenceIsDurable` 为 `legacyCombinedEvidence \|\| rawHeadBudgetBytes > 0`。 |
| R-12-raw-head-budget | 期望通过。`rawHeadBudgetBytes==4096` 时保留证据，hash 长度 64，文本含 `lastInference`。 | 与 `tests/detection_task_tests.cpp` `testModelEvidenceDurabilityFollowsHeadRetention` 相同 API。 |
| R-12-legacy-combined | 期望通过。`legacyCombinedEvidence` 与 raw-head 预算路径写出相同 supplemental。 | 同上。 |
| R-08 | **SKIP**，进程返回 0。不是 RED，也不是 UI 通过。 | 基线 `ui/detection/detection_card.h` 没有 `startLatchActive()`。`DetectionCard::submit` 是 private。 |

R-01 是本基线上唯一故意保持的红断言。不要把「6 GiB 必须被 Invalid sample context 拒绝」写回测试。

R-02、R-03、R-06、R-07、R-09～R-11、R-13～R-20：本提交没有用例，状态 `NOT_RUN`。R-15/R-18 的 GPU 部分另标 `UNVERIFIED_GPU`。

样本长度 / 窗口数（逻辑用例的输入，不是实测吞吐）：

- R-01：1000 个复数样本，Fs=1000，`workingCellBudget` 为 268435456 或 268435457，grant 为 6 GiB 或 96 MiB。`makePlan` 不读 IQ。
- R-04/R-05：手写 `DetectionSourcePlan`，`eligibleWindowCount=2044`，`modelHeadBytesPerWindow=196608`，`estimatedWindowEvidenceBytes=64`。没有 51125000 样本的宽带文件。
- R-12：一个 `lastInference.h2dMs=1.5` 的 JSON 对象。没有模型、没有 runId。
