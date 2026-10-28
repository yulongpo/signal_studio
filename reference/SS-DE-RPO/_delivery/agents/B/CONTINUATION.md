# Agent B continuation

- 角色：B，Sample 阶段预算接入实际规划与执行
- 工作树：`D:/project/ss-de-rpo-wt/continue-b`
- 分支：`codex/ss-de-rpo-b`
- 基线：`7220b2e295963288cb5d11e76946bb77646be0b0`
- 未推送。未改 A/D 文件、`decideSamplePhaseBudget` 签名、CMake、请求字段或工程 JSON。

## 变更文件

- `infrastructure/detection_sample_stream.h`
- `infrastructure/detection_sample_stream.cpp`
- `infrastructure/detection_sample_stream_scratch.cpp`
- `infrastructure/detection_time_analysis.cpp`
- `tests/ss_de_rpo_sample_tests.cpp`（主代理登记 CMake）

## 接口

`decideSamplePhaseBudget(authorizedRamBytes, estimatedChunkResidentBytes, plannedCells)` 签名与判定顺序不变。规划器消费其 `kind` / `errorCode` / `requested` / `available` / `granted`，不再只是并列的纯函数。

调用点：

- `makePlan`：Legacy 资格用全长 `count` 与 `legacyResidentEstimateBytes`。Streaming 用当前输出块，不把全长样本数当 work-unit。
- `fitStreamingBlock` / `fitExternalBlock`：块放不下时只向下二分，取仍满足决策的最大块。半径、尾预算、搜索范围、带宽和中心不变，不关闭保留输出。
- `legacy()`：非 `Accepted` 在分配 IQ 之前失败。
- `stream()`：执行前再查一次。`Adapted` 仅当连续 FFT/块字节 `<= granted`（1 GiB 上限）且总驻留 `<=` 授权时继续。
- `contextValid`：`memoryGrantBytes==0` 走 `sample.invalid-context`，并保留旧句 `Invalid sample context/configuration/budgets`。非零授权且 cell 超过 256 Mi 走 `sample.work-unit-ceiling`。cell 为 0 仍是本地 ceiling 拒绝。
- `applyDetectionTimeAnalysis`：仅当 `working/12` 超过 ceiling 时调用；`available` 是 cell 上限，不用 `granted`（那是字节）。ROI/Coarse 仍写回 `spectrumCellBudget`。`workingSetBudgetBytes*3` 溢出时返回 `resource.arithmetic-overflow`，不回绕。

块驻留与原先整数式相同：`32*fft + 32*radius + 32*block + reader + result + 4 MiB`。外部能量为 `fixed + 32*block`。首选块已经放下时不改块长，因此 96 MiB / 块 257 保持原值。

失败：

- 最小块仍超过授权：`ErrorCode::ResourceLimit`，`phase=time-refine requested=<最小驻留> available=<授权> code=sample.chunk-exceeds-grant`。
- 形成估计时 `uint64` 溢出：`ErrorCode::InvalidInput`，原文 `resource.arithmetic-overflow phase=time-refine requested=0 available=0`。不把回绕值送进决策函数。
- 取消：`ErrorCode::Cancelled`。`makePlan` 多次轮询；长执行循环按 4096 样本或块轮询。Auto 不把取消或溢出当成 ResourceLimit 回退。

`LegacyExact` 的 `makePlan` 在不合格时仍返回计划且 `legacyEligible=false`。资格仍要决策为 `Accepted`，并且精确驻留同时 `<= memoryGrantBytes` 和 `<= 1 GiB`，样本数 `<= workingCellBudget`。1 GiB 连续上限与 256 Mi cell 上限没有提高。FIR 尾预算、分位数和精度保护未改。

## 检查

本工作树 `scripts/build.ps1`，`Jobs 2`，preset `local-debug`。临时 CMake 目标只用于链接，提交前已还原。未跑 GPU 推理，未写主仓库 `out/`。

```text
scripts/build.ps1 -Configuration Debug -Action Configure
scripts/build.ps1 -Configuration Debug -Action Build -Jobs 2 -Targets SignalStudioSsDeRpoSampleTests,SignalStudioSsDeRpoLogicTests,SignalStudioDetectionSampleStreamTests
```

配置与三次目标构建退出码 0。时间分析最后一处改动后重编了 `detection_time_analysis.cpp` 并重新链接，退出码 0。既有 C4456/C4457 在 `raw_sample_reader.cpp`、`project_file.cpp`、`project_store.cpp`。本工作树没有 `mkl_rt.3.dll`，oneMKL 部署被跳过。

`QT_QPA_PLATFORM=offscreen`，PATH 含 Qt 6.11.1 `bin`：

- `out/vs2026-qt611-debug_bin/SignalStudioSsDeRpoSampleTests.exe`：`PASS ss_de_rpo_sample_tests failures=0`
- `out/vs2026-qt611-debug_bin/SignalStudioSsDeRpoLogicTests.exe`：`PASS ss_de_rpo_logic_tests failures=0`（含 R-01）
- `out/vs2026-qt611-debug_bin/SignalStudioDetectionSampleStreamTests.exe` 无参数：`Sample stream directed checks passed; native Golden cases=0`，退出码 0。这包含 96 MiB / 1.2e9 样本计划，不含 `--golden --long --gpu`

实测计划（只 `makePlan`，不读 IQ）：

- 1/2/4/8/16 GiB，n=1000：Legacy 与 Streaming 都接受。Legacy 驻留 38868929，Streaming 驻留 21064704，块 257，`legacyEligible=true`。十次合计 elapsedMs=0。
- n=100000000，授权 16 GiB：Legacy 估计 1202629632，大于 1 GiB 且小于授权，`legacyEligible=false`。Streaming scratch=800008192，驻留 33573856，块 131072。
- 授权 4 GiB、结果预留 2 GiB：Streaming 驻留 2151771136，大于 1 GiB 且不超过授权，计划成功。Legacy 计划返回但不合格。
- ceiling+1：`sample.work-unit-ceiling requested=268435457 ceiling=268435456`。
- 零授权：`sample.invalid-context` 且含旧 invalid-context 句。零授权同时超 ceiling 仍是 ceiling，不是 invalid-context。
- `UINT64_MAX` 授权与结果预留：两种模式都是上述 arithmetic-overflow 原文。Working set 前后都是 14852096。
- fs=65536，n=20000：首选块 20000，fft 32768，radius 607，taps 1215，驻留 5906400，tail 0.000080。授权减 1 后块 19999、驻留 5906368，半径与 tail 不变。最小驻留 4283392；相等授权成功；4283391 失败，`requested=4283392 available=4283391 code=sample.chunk-exceeds-grant`。
- n=2^34，Streaming，块 4096，授权 8 GiB：elapsedMs=0，驻留 21384160，Working set 前后都是 15020032，`legacyEligible=false`。
- 长上下文立即取消，以及第 2 次回调取消：`Cancelled`，stage `plan`。成功 Streaming 计划轮询 5 次。
- 96 MiB、n=1200000000：Legacy 不合格；Streaming scratch=9600008192，驻留 21064704，块仍为 257。

## 未完成

- CMake 未登记 `tests/ss_de_rpo_sample_tests.cpp`。建议独立控制台目标，链接 `SignalStudioInfrastructure` 与 `psapi`，TIMEOUT 60。测试源里有 `#pragma comment(lib, "psapi.lib")`。
- 没有全量 CTest，没有 `--golden --long --run-budget`，没有 GPU/TensorRT，没有 RFbench，没有 20/363/2044 窗口 RSS 或性能声明。
- 新测试不调用 `run()` / `refineEnergyBlocks`。Adapted 执行路径只在 `stream()` 的再检查里接上，没有单独的执行样例。
- 窄带/ROI/Coarse 的谱 cell 预算行为未改。时间分析只在原始 cell 超过 ceiling 时调用决策函数。
