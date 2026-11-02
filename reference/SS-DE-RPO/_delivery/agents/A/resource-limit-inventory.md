# 资源限制清单（Agent A）

基线 HEAD：`82935ab34ec6126707da152bf46d1f352458ab0c`。  
本文件只记录在该工作树源码中读到的定义与调用。事实等级均为 **CONFIRMED_CODE**。没有在本轮启动检测进程，没有 GPU 测量。

分类：

- **authorization**：允许某阶段使用的字节（或设备）上限。它不是一次分配。
- **work-unit**：单个算法步骤的数量上限（cell、窗、比较次数）。它不是字节授权。
- **format**：分片、字段、协议或文件格式边界。

## Sample 1 GiB / 256 Mi cell（行为修复归 B）

这两处仍是当前拒绝点。Agent A **没有**修改 `detection_sample_stream`、`detection_time_analysis` 或 `detection_refinement`。把默认 8 GiB 工作集变成可运行，是 **B 的行为修复**。

| 文件 | 符号 / 表达式 | 默认或字面量 | 调用方（生产代码） | 类别 | 当前行为 |
|---|---|---|---|---|---|
| `infrastructure/detection_sample_stream.cpp` `contextValid` | `rt.memoryGrantBytes>1024*MiB`，同时 `!rt.memoryGrantBytes` | 1024 MiB = 1 GiB；0 也拒绝 | `makePlan`；`makePlan` 再被 `run`、`refineEnergyBlocks`，以及 `detection_time_analysis.cpp` 的 `stream::makePlan` 调用 | 源码把它写成上下文非法。契约要求改成 **contiguous allocation cap**，不是 RAM authorization | 授权大于 1 GiB 时整段 `contextValid` 失败，文案 `Invalid sample context/configuration/budgets` |
| `infrastructure/detection_sample_stream.cpp` `contextValid` | `!o.workingCellBudget \|\| o.workingCellBudget>256*1024*1024ULL` | 0 拒绝；上限 256×1024×1024 cells | 同上 | **work-unit**（cell 个数） | 与「授权太大」共用同一句非法上下文 |
| `domain/detection_refinement.cpp` `refineDetection` | `!options.workingCellBudget \|\| options.workingCellBudget > 256 * 1024 * 1024` | 同上 256 Mi cells | `refineDetection` 的直接调用方在精修路径 | **work-unit** | 超过则 `Invalid refinement input or bounds` |
| `infrastructure/detection_time_analysis.cpp` `applyDetectionTimeAnalysis` | `working = request.workingSetBudgetBytes * 3 / 4` | 默认工作集 8 GiB 时 working = 6 GiB | `infrastructure/detection_task.cpp` 时间精修调用 `applyDetectionTimeAnalysis` | 混用：同一 `working` 既当授权又当 cell 来源 | B 必须拆开 |
| `infrastructure/detection_time_analysis.cpp` `boundedSample` | `runtime.memoryGrantBytes=working` | 同上，6 GiB | `applyDetectionTimeAnalysis` 的 Sample 分支 | 被送进上面的 1 GiB 检查 | 大于 1 GiB 即被 `contextValid` 拒绝 |
| `infrastructure/detection_time_analysis.cpp` `applyDetectionTimeAnalysis` | `options.workingCellBudget = working / 12` | 6 GiB / 12 = 512 Mi cells | 同一函数，随后进入 Sample stream 与 `refineDetection` | **work-unit** 被用字节预算除出来 | 512 Mi cells > 256 Mi cells，再次非法上下文 |

算术（CONFIRMED_CODE，不是本轮运行日志）：`kDetectionWorkingSetTargetBytes` 为 8 GiB，且物理内存不小于该值时 `detectionWorkingSetDefaultBytes()` 为 8 GiB。`8 GiB * 3/4 = 6 GiB`，`6 GiB / 12 = 512 Mi cells`。6 GiB > 1 GiB，且 512 Mi cells > 256 Mi cells。这就是默认配置在 Sample 精修被拒绝的代码路径。

`makePlan` 的 `legacyEligible` 目前只要求 `exact<=rt.memoryGrantBytes` 与 `p.count<=workingCellBudget`。连续分配上限尚未写在这里。B 负责加上 `<= kSampleContiguousAllocationCapBytes`，超过则走已有 streaming/Auto，不把整段授权一次分配出去。

## 本提交新增（尚未被 B/D 链接）

| 文件 | 符号 | 值 | 调用方 | 类别 |
|---|---|---|---|---|
| `domain/detection.h` | `kSampleContiguousAllocationCapBytes` | 1024×1024×1024 字节 | `decideSamplePhaseBudget`。B 的 1 GiB 字面量尚未改为引用它 | contiguous allocation cap，**不是** authorization |
| `domain/detection.h` | `kSampleWorkUnitCellCeiling` | 256×1024×1024 cells | `decideSamplePhaseBudget`。B/精修里的 256 Mi 字面量尚未改为引用它 | **work-unit** |
| `domain/detection.h` / `.cpp` | `decideSamplePhaseBudget` | 纯函数，无默认预算 | 本提交无其它生产调用方 | 决策，不分配 |
| `domain/detection.h` / `.cpp` | `checkedMulU64` | 溢出返回 false 且 `out=0` | 本提交无其它生产调用方 | 算术闸，防止回绕后当成通过 |
| `domain/detection.h` | `ResourceDecision` | 运行时结构 | 不进入 `DetectionRequest`，不写工程 JSON | 决策记录 |

## 授权（字节上限，不是单次分配）

| 文件 | 符号 | 默认 | 调用方 | 类别 |
|---|---|---|---|---|
| `domain/detection.h` | `kDetectionWorkingSetTargetBytes` | 8 GiB | `detectionPhysicalMemoryBytes` 探测失败时的回退；`detectionWorkingSetLimitBytes`；`detectionWorkingSetDefaultBytes` | authorization 目标。默认是 `min(8 GiB, 物理内存)` |
| `domain/detection.h` | `kDetectionWorkingSetMinBytes` | 16 MiB | `validateDetectionRequest`；资源选项对话框工作集下限 | authorization 下限 |
| `domain/detection.h` | `DetectionRequest::workingSetBudgetBytes` | `detectionWorkingSetDefaultBytes()` | 资源选项 `applyTo`；时间精修读取该字段 | authorization。UI 上限是物理内存 |
| `domain/detection.h` | `kDetectionDiskBudgetTargetBytes` | 64 GiB | `detectionDiskBudgetDefaultBytes` | 磁盘 authorization 目标，再与卷剩余空间取较小值 |
| `domain/detection.h` | `kDetectionExplicitRawHeadMaxBytes` | 2 GiB | `validateDetectionRequest`：仅当 **未** retain 且 `rawHeadBudgetBytes` 大于该值 | 显式配额上限。retain OFF 且配额 0 不走这条 |
| `ui/detection/detection_resource_options.cpp` | `workingSetBudgetMiB` | 默认工作集的 MiB 数；QSettings 键 `detection/resources/workingSetBudgetBytes` | 对话框确定后 `save` / `applyTo` | authorization。本提交不改数值与键 |
| `ui/detection/detection_resource_options.cpp` | `workingSetAuthorizationNote` | 只读，显示 `detectionPhysicalMemoryBytes`、默认工作集，以及「更大的工作集授权是使用上限，不是一次连续分配。」 | 无执行层读取 | 说明，不是新配额 |
| `infrastructure/detection_sample_stream.h` | `Runtime::memoryGrantBytes` | 结构默认 96 MiB | 时间精修在 Sample 路径覆写为 `working` | 阶段授权。默认 96 MiB 不是 UI 的 8 GiB |
| `infrastructure/detection_model_backend.h` | `maximumDeviceBytes` | 4 GiB | `validateDetectionModelMemory`；`detection_model_backend.cpp` 装载路径 | 设备字节上限。F 负责按实际空闲显存选择 batch。A 未改 |
| `infrastructure/detection_model_backend.h` | `maximumEngineBlobBytes` | 128 MiB | 同上，以及 `ffscn_engine_package.cpp` 的 `kMaximumEngineBytes` | 模型 blob 格式/装载上限 |
| `infrastructure/detection_model_backend.h` | `maximumExecutionBatch` | 2 | 装载时与 `request.parameters.batchSize` 取较小值；worker 可传入 | 合法 batch 档。默认保持 2。不是显存授权本身 |

## work-unit

| 文件 | 符号 | 默认 | 调用方 | 类别 |
|---|---|---|---|---|
| `domain/detection_refinement.h` | `workingCellBudget` | 4 Mi cells | 时间精修覆写为 `working/12` | work-unit。结构默认不是 512 Mi |
| `domain/detection_postprocess.h` | `maximumAssociationComparisons` / `maximumFusionComparisons` | 4_000_000 | `detection_postprocess.cpp` 超过则 `association-comparison-limit-exceeded` / `frequency-fusion-comparison-limit-exceeded` | work-unit 安全闸。E 负责不靠静默丢正式轨迹修复 |
| `domain/detection_postprocess.h` | `maximumPreviewEvents` | 1024 | 预览队列 | 只限制预览 |
| `domain/detection_postprocess.h` | `residentByteBudget` | 32 MiB | 跟踪器限额校验 | 跟踪驻留预算，不是 Sample RAM 授权 |
| `domain/detection_postprocess.h` | `maximumWindows` | 262144 | 跟踪器限额校验（绝对上限检查另有 1_000_000） | work-unit |
| `domain/annotation.h` | `annotationSegmentBudget` | 100000 | Sample `contextValid`、活动段输出、任务发布计数 | work-unit / 结果条数 |

## format

| 文件 | 符号 | 默认 | 调用方 | 类别 |
|---|---|---|---|---|
| `infrastructure/detection_store.cpp` | `kChunkBytes` | 8 MiB | 分片读写 | format。本轮不改 |
| `infrastructure/detection_store.cpp` | `kStoreBytes` | 256 MiB | `jsonStoreLimit`：legacy 用该值；非 legacy 为 `max(kStoreBytes, evidenceBudgetBytes)` | **legacy JSON store**。不是 RAM，也不是「retain OFF 且配额 0」的 raw-head 闸。D 负责预检行为 |
| `domain/detection.h` | `kDetectionLegacySampleScratchBytes` | 16 GiB | `detection_store.cpp` 缺省历史 manifest | 历史格式默认，不是新请求默认 |
| `infrastructure/runtime_log.h` | `kDefaultRotateBytes` | 8 MiB，保留 3 份 | 资源选项里的只读说明 | format。对话框写明这两项固定 |
| `domain/detection.cpp` `validateDetectionRun` | `performanceJson.size() > 65536` | 64 KiB | 运行校验 | format |
| `domain/detection.h` | `isDetectionExecutionBatchTier` | 1/2/4/8/16 | `validateDetectionRequest` | format / 引擎档。A 未改档位 |

`validateDetectionRequest` 的通过/拒绝条件在本提交中没有改动。
