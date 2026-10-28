# Agent B 补丁摘要

分支 `fix/ss-de-rpo-b`。基线 `82935ab34ec6126707da152bf46d1f352458ab0c`。只改 Sample 所有权内的两个 cpp，外加本目录文档。

## `infrastructure/detection_sample_stream.cpp`

- `contextValid` 删除 `memoryGrantBytes > 1024 MiB`。`memoryGrantBytes == 0` 仍与其它非法几何一起返回 `Invalid sample context/configuration/budgets`。
- `workingCellBudget == 0` 或 `> 256 Mi` 从该大条件中拆出，`InvalidInput`，detail `sample.work-unit-ceiling requested=<cells> ceiling=<256 Mi>`。
- `legacyEligible` 要求精确驻留 `exact <= memoryGrantBytes`、`exact <= 1 GiB`，且 `count <= workingCellBudget`。原有的 `size_t` 上限和结果字节配额检查保留。FIR 尾幅、分位数和精度守卫未改。
- Streaming 计划、`legacy()` 执行、外部 Energy 块：`estimatedResidentBytes > memoryGrantBytes` 时 `ErrorCode::ResourceLimit`，detail `phase=time-refine requested=<bytes> available=<bytes>`。不把授权改小。
- `makePlan(LegacyExact)` 仍可返回「不可执行」的计划（`legacyEligible == false`），以便 96 MiB 长样本探针和 Auto 回退保持原语义。超过授权的精确计划在 `legacy()` 分配前失败。

## `infrastructure/detection_time_analysis.cpp`

- Sample 初始 `options.workingCellBudget = min(working/12, 256 Mi cells)`。8 GiB 工作集（6 GiB working）不再把 512 Mi cells 送进 Sample。
- `boundedSample` 仍执行 `runtime.memoryGrantBytes = working`，6 GiB 授权现在合法。
- ROI/Coarse 两处 `options.workingCellBudget = spectrumCellBudget` 未改。`readSpectrum` 仍可按实际频谱把 `stftWorkingCellBudget` 抬到不少于实际 cells，且该值可以大于 `working/12`。

未改 `detection_sample_stream.h`、scratch、能量公式、`detection_task.cpp`、`domain/detection.h`、UI 和 CMake。
