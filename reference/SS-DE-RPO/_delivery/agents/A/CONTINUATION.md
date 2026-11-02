# Agent A continuation

- 角色：A，资源快照与阶段重叠磁盘账本
- 工作树：`D:/project/ss-de-rpo-wt/continue-a`
- 分支：`codex/ss-de-rpo-a`
- 基线：`7220b2e295963288cb5d11e76946bb77646be0b0`
- 未推送。未改 `detection_task.cpp`、CMake、请求字段或工程 JSON。

## 变更文件

- `domain/detection.h`
- `domain/detection.cpp`
- `ui/detection/detection_resource_options.cpp`
- `tests/ss_de_rpo_resource_tests.cpp`（主代理登记 CMake）

## 接口

运行时类型不进入 `DetectionRequest`，也不进入工程 JSON。`decideSamplePhaseBudget(authorizedRamBytes, estimatedChunkResidentBytes, plannedCells)` 的签名和判定顺序不变，仍是 Sample 的唯一预算决策。cell 只走该函数；`resourceDecisionUnit` 仅在 `resource=="work-unit"` 时返回 `cells`。

`captureRuntimeResourceSnapshot(path, vram)` 读取物理内存与卷剩余空间。`vram==nullptr` 时显存为 `NotRequested`，字节保持 0。F/D 把 `cudaMemGetInfo` 的结果填进 `VramProbeInput`；本组件不调用 CUDA。探测失败不把 8 GiB 默认工作集当成实测值。

`assessRawHeadDisk` 不改请求，也不替换 `applyDetectionEvidencePreflight` 的原有句子。legacy combined 时 `applies==false`。retain OFF 且配额 0 不收取 2044×196608，也不因未发生的乘法溢出拒绝。retain ON 的配额、未知卷或卷不足使用 `resource.raw-head-shortfall`。显式配额且 retain OFF 使用 `resource.explicit-raw-head-quota`；超过 2 GiB 使用 `resource.explicit-raw-head-cap`。溢出是 `resource.arithmetic-overflow`。

`appendPhaseOverlapDiskLines` 加三组，同一卷同一 phase 相加：

- `checkpoint`：保留的 raw-head、结果 shard、checkpoint 的 QSaveFile 双份（新文件 + 尚未替换的旧文件）
- `refine`：raw-head、结果 shard、已提交的一份 checkpoint、scratch。scratch 只在这一相
- `publishing`：raw-head、结果 shard、发布 shard、已提交 checkpoint、工程原子保存双份。scratch 已回收，不计入

`scratchVolumeKey` 不同则单独成峰。shard 单文件超过 8 MiB（`kDetectionShardFileCapBytes`）为 `resource.shard-boundary`，`resource=format`。等于 8 MiB 合法。`evaluatePhaseOverlapDiskLedger` 的 `requested` 是同时占用峰值，`granted` 在接受时等于峰值，拒绝时为 0。raw-head 自身超过可用空间才是 `resource.raw-head-shortfall`；其余超额是 `resource.disk-shortfall`。

`mergeRuntimeResourceDiagnostics` 把 `resources` 并进可选 `performanceJson`，schema 仍是 `signal-studio-detection-profile-v1`，字节用十进制字符串，超过 65536 则失败且不改输出。D 的 `performanceSnapshot` 会重写诊断并丢掉未知键；在 D 于快照之后调用合并，或在快照对象里插入 `resources` 之前，诊断不会留在 checkpoint 里。

选项对话框只增加只读标签 `runtimeResourceSnapshotNote`。`applyTo`、QSettings 键和预算默认值未改。

## 检查

独立编译并运行，未跑 GPU 推理，未改主仓库 `out/` 部署产物：

```text
cl /std:c++20 /EHsc /utf-8 /W4 /I <worktree>
  tests/ss_de_rpo_resource_tests.cpp domain/detection.cpp
ss_de_rpo_resource_tests.exe
```

结果：`PASS ss_de_rpo_resource_tests failures=0`。日志在 `out/tasks/ss-de-rpo/continue-a/resource-tests.txt`（不入库）。覆盖 checked 乘加溢出、Sample 五条判定、raw-head OFF/ON/legacy/2044×196608、账本峰值与双份原子保存、8 MiB shard、分卷、保留为 0 不收费，以及 JSON 合并。

同一次进程的实机快照（RUNTIME，不是授权）：RAM `Probed`，总计 67772461056，可用 25620795392；卷 `D:\`，`GetDiskFreeSpaceExW`，可用 1013778743296。显存 `NotRequested`。两次捕获的 steady ns 非递减。

`ui/detection/detection_resource_options.cpp` 用 Qt 6.11.1 头文件 `/c` 编译通过。只有 Qt 6 对 `compressEvent` 的既有 C4996。没有打开对话框。

## 未完成

- CMake 未登记 `tests/ss_de_rpo_resource_tests.cpp`。建议独立控制台目标；该文件没有 Qt 依赖，链接已有 `SignalStudioCore` 即可。
- `infrastructure/detection_task.cpp` 仍使用自己的预检句子。D 可调用上述 helper，不要为了接 helper 改掉现有拒绝前缀。
- 没有把快照写进正在运行的 `performanceSnapshot`。
- 没有打开资源选项对话框，没有全量 CTest，没有 RFbench，没有 CUDA。
