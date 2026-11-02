# Agent B 接口请求

本代理没有修改 `M1_CONTRACT.md`、`domain/detection.h`、`detection_task.cpp` 或 CMake。

## 1. 常量归 A，集成时替换本地副本

`domain/detection.h` 在本工作树还没有 `kSampleContiguousAllocationCapBytes` 和 `kSampleWorkUnitCellCeiling`。B 按契约第 1 节在两个 cpp 的匿名命名空间放了相同数值：

- `infrastructure/detection_sample_stream.cpp`（位于 `signalstudio::sample_stream`）
- `infrastructure/detection_time_analysis.cpp`（位于 `signalstudio`）

请 A 把常量放进 `domain/detection.h`。集成时删除 B 的本地副本并改为引用 A。注意 `detection_time_analysis.cpp` 的匿名命名空间在 `namespace signalstudio` 内：若头文件同时声明 `signalstudio::kSampleWorkUnitCellCeiling`，该 cpp 里的无限定查找会歧义。`sample_stream` 内的副本不会和 `signalstudio::` 常量重名，但仍应在集成时改成单一来源。

## 2. 错误文本与 A/G 的错误码

Sample 的 `Failure::ErrorCode` 没有新增枚举项。

- cell 上限：`ErrorCode::InvalidInput`，stage `plan`，detail `sample.work-unit-ceiling requested=<cells> ceiling=268435456`
- 已选计划的 `estimatedResidentBytes > memoryGrantBytes`：`ErrorCode::ResourceLimit`，detail `phase=time-refine requested=<bytes> available=<bytes>`
  - Streaming 块：stage `fir-plan`（`makePlan`）
  - 真正执行的 LegacyExact：stage `legacy-plan`（`legacy()`，分配之前）
  - 外部 Energy 块：stage `external-energy`
- 其它非法几何仍是 `Invalid sample context/configuration/budgets`
- scratch 配额不足的文案未改

`makePlan(LegacyExact)` 在精确驻留超过授权时仍返回成功计划且 `legacyEligible == false`。这是 96 MiB 长样本探针和 Auto「先查询、不够则不分配」的既有契约。G 的 R-01 应断言：6 GiB grant 且 `workingCellBudget <= 256 Mi` 时 `makePlan` **不**返回 invalid context；`exact > 1 GiB` 时 `legacyEligible == false`。不要把断言写成「`makePlan(LegacyExact)` 必须对 6 GiB 返回 ResourceLimit」，那会打断上述探针。执行路径（`run` / `legacy`）在已选计划驻留超过授权时才返回 ResourceLimit。

若 A 的 `ResourceDecision` 需要稳定码，请映射上述 detail，不要把「授权大于 1 GiB」映射成 `sample.grant-exceeds-contiguous-cap`。那个码按契约不能用来拒绝大授权。

## 3. 与 D 的 runId 时间线

B 不拥有 `detection_task.cpp`，没有调用 `noteSteadyMark`。`boundedSample` 已有的 `boundedSample` JSON 仍记录 `contextSamples`、`blockSamples`、`firRadius`、`fftSamples`、`estimatedResidentBytes`、`legacyResidentEstimateBytes`、`sourceSamplesRead`、scratch 峰值、route 和 precision reason。请集成者在父进程 steady clock 上为时间精修打点，不要用 worker 时钟和 UI 时钟相减。本补丁没有新增跨进程计时字段。

## 4. 不需要 scratch 接口变更

现有块上限（输出块与源读块均不超过 131072）已经把长信号留在流式路径。连续分配上限只影响 `legacyEligible`，没有改 scratch 文件格式或恢复协议。
