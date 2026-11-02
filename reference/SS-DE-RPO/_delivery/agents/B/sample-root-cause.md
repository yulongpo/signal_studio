# Sample 预算错配根因（Agent B）

基线 HEAD：`82935ab34ec6126707da152bf46d1f352458ab0c`。证据等级：`CONFIRMED_CODE`。本工作树没有已配置构建，没有新的运行日志，不把旧日志升格为 `CONFIRMED_RUNTIME`。

## 错配

默认工作集目标是 `kDetectionWorkingSetTargetBytes = 8 GiB`（`domain/detection.h`）。`applyDetectionTimeAnalysis` 取

`working = workingSetBudgetBytes * 3/4`，默认即 6 GiB。

同一数值被同时当成两件不同的事：

| 字段 | 基线赋值 | 8 GiB 工作集下的值 | 内层检查 |
|---|---|---|---|
| `memoryGrantBytes` | `boundedSample` 中 `runtime.memoryGrantBytes = working` | 6 GiB | `contextValid` 拒绝 `> 1024 MiB` |
| `workingCellBudget` | `working / 12` | 512 Mi cells | `contextValid` 拒绝 `> 256 Mi` cells |

两条检查共用一句 `InvalidInput`：`Invalid sample context/configuration/budgets`。授权大于 1 GiB 被当成非法配置。512 Mi cells 也不是「这次精修真的需要 512 Mi 个单元」，而是把字节预算除以 12 塞进 cell 预算。`working > 3 GiB` 时 `working/12` 必然超过 256 Mi cell 上限（`256 Mi * 12 = 3 GiB`）。

审计文档 `03_AUDIT_BASELINE.md` 记录的 `plan: Invalid sample context/configuration/budgets` 与上述分支吻合。该日志不能证明每一次失败都只有这一个原因。

## 三个量

- **授权** `memoryGrantBytes`：这一阶段允许使用的 RAM。6 GiB 授权是合法的。`memoryGrantBytes == 0` 仍然非法。
- **连续分配上限** `kSampleContiguousAllocationCapBytes = 1 GiB`：一次连续驻留的安全上限，不是 RAM 授权上限。精确路径的 `exact` 必须同时 `<= memoryGrantBytes` 且 `<= 1 GiB`。超过连续上限时不分配整段多 GiB 缓冲，Streaming/Auto 继续走已有分块路径。
- **工作单元上限** `kSampleWorkUnitCellCeiling = 256 Mi` cells：算法工作单元个数，不是字节。`workingCellBudget == 0` 或 `> 256 Mi` 仍拒绝，detail 为 `sample.work-unit-ceiling requested=<cells> ceiling=268435456`，错误码仍是 `InvalidInput`。

本工作树的 `domain/detection.h` 还没有这两个常量。Sample 路径在匿名命名空间里使用契约第 1 节的同一数值。

## 修复后的入口

`applyDetectionTimeAnalysis` 传入 Sample 的初始 cell 预算改为 `min(working/12, kSampleWorkUnitCellCeiling)`。8 GiB 工作集得到 256 Mi cells，不再得到 512 Mi cells。`boundedSample` 仍把 `memoryGrantBytes` 设为完整 `working`，包括 6 GiB。

ROI/Coarse 在 `readSpectrum` 成功之后的两处 `options.workingCellBudget = spectrumCellBudget` 没有改。`readSpectrumBody` 按字节预算和 256 Mi cell 上限自行计算频谱矩阵容量，实际 STFT cells 仍可以大于 `working/12`，`stftWorkingCellBudget` 仍是该容量。`spectrumCapacityMatchesActualConsumer` 依赖的就是这条覆盖，而不是 Sample 的 `working/12`。

## 精确路径与低内存

`makePlan` 的 `legacyEligible` 增加 `exact <= kSampleContiguousAllocationCapBytes`。6 GiB 授权不能让数 GiB 的整段 IQ/能量缓冲变成可执行的 LegacyExact。FIR 半径、尾幅、分位数和精度守卫的公式没有改，也没有伪造活动段。

`makePlan(LegacyExact)` 在精确驻留超过授权时仍返回计划，并把 `legacyEligible` 置为 false。这是既有的 96 MiB / 1.2e9 样本探针和 Auto 回退所依赖的查询，不是已选中的执行。真正执行该计划的 `legacy()` 若 `estimatedResidentBytes > memoryGrantBytes`，在分配之前返回 `ErrorCode::ResourceLimit`，detail 为 `phase=time-refine requested=<bytes> available=<bytes>`。

Streaming 计划在 `makePlan` 里用同一 detail 拒绝超过授权的块驻留。块驻留仍按固定 FFT/块公式计算，不随 N 变成整段缓冲。scratch 配额不足仍是原来的 scratch 文案，不把授权改小来假装通过。外部 Energy 块路径同样在执行前比较覆盖后的 `estimatedResidentBytes`。
