# Sample 阶段资源决策契约（Agent A）

事实等级：**CONFIRMED_CODE**（本提交实现）。不描述 GPU 运行结果。

`ResourceDecision`、`ResourceDecisionKind`、`kSampleContiguousAllocationCapBytes`、`kSampleWorkUnitCellCeiling`、`checkedMulU64`、`decideSamplePhaseBudget` 都在 `domain/detection.h` / `domain/detection.cpp`。

它们 **不是** `DetectionRequest` 字段， **不** 写入工程 JSON。`validateDetectionRequest` 的行为不变。

## 常量

| 符号 | 值 | 含义 |
|---|---|---|
| `kSampleContiguousAllocationCapBytes` | 1073741824（1 GiB） | 单次连续分配安全上限 |
| `kSampleWorkUnitCellCeiling` | 268435456（256×1024×1024） | 一个算法工作单元的 cell 数上限 |

大于 1 GiB 的 RAM 授权是合法的。不得用 `sample.grant-exceeds-contiguous-cap` 把这种授权拒掉。

## `checkedMulU64`

- 成功：`out = a * b`，返回 true。
- `a * b` 超过 `uint64` 最大值：`out = 0`，返回 false。不回绕。
- `decideSamplePhaseBudget` 不对三个入参做乘法。调用方在形成 `estimatedChunkResidentBytes` 或 cell 预算时如果乘法溢出，应停止并使用错误码 `resource.arithmetic-overflow`，不要把回绕结果送进本函数。

## `decideSamplePhaseBudget`

签名：

```text
ResourceDecision decideSamplePhaseBudget(
    uint64_t authorizedRamBytes,
    uint64_t estimatedChunkResidentBytes,
    size_t plannedCells);
```

`phase` 固定为 `time-refine`。`reserve` 固定为 0（本函数没有预留入参；调用方若已扣除 OS/UI 预留，应在传入 `authorizedRamBytes` 之前扣除）。

`message` 始终包含 `phase=`、`resource=`、`needed=`、`available=`、`fallback=`。RAM 决策的 `unit=bytes`。work-unit 决策的 `unit=cells`，此时 `requested` / `available` / `granted` 是 cell 数。

调用方必须看 `kind`。`errorCode` 非空不代表整次任务失败。

判定顺序：

1. `authorizedRamBytes == 0`  
   `Rejected`，`sample.invalid-context`，`resource=ram`，`granted=0`。  
   即使 cell 也越界，仍先报非法上下文。
2. `plannedCells > kSampleWorkUnitCellCeiling`  
   `Rejected`，`sample.work-unit-ceiling`，`resource=work-unit`，`granted=0`。  
   等于上限则不拒绝。这不是「授权太大」。
3. `estimatedChunkResidentBytes > authorizedRamBytes`（此时授权已非 0）  
   `Rejected`，`sample.chunk-exceeds-grant`，`resource=ram`，`granted=0`。  
   等于授权则不因本条拒绝。低内存是拒绝或更小块，不是把授权改小后假装通过。
4. `estimatedChunkResidentBytes > kSampleContiguousAllocationCapBytes` 且仍 `<= authorizedRamBytes`  
   `Adapted`，`sample.grant-exceeds-contiguous-cap`，`resource=ram`，`granted = kSampleContiguousAllocationCapBytes`。  
   `fallback` 含 `planner must chunk`，并写明这不是整次任务的硬失败。规划器必须分块，不得一次分配整段授权。
5. 其余  
   `Accepted`，`errorCode` 为空，`resource=ram`，`granted = estimatedChunkResidentBytes`。  
   授权可以远大于 1 GiB。块估计不超过连续上限时，不因为授权大而 Adapted 或 Rejected。

因此：授权大于 1 GiB 时，结果只可能是 `Accepted` 或 `Adapted`，除非 cell 超过工作单元上限，或估计驻留 **大于** 授权，或授权为 0。

`sample.grant-exceeds-contiguous-cap` 只与 `Adapted` 同时出现。

`resource.raw-head-shortfall` 不属于本函数。retain ON 的卷不足由 D 在 `applyDetectionEvidencePreflight` 保持原有分支。

## 与 B 的现有字面量

B 路径里的 `1024*MiB` 与 `256*1024*1024` 数值与上述常量相同，但 **现有 `contextValid` 仍把大于 1 GiB 的授权当成非法上下文**。那是待 B 修改的行为，不是本函数的行为。集成前两套代码并存：A 提供决策，B 的 Sample 路径在 B 提交前仍会拒绝默认 6 GiB grant。
