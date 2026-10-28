# 接口请求（Agent A → 主代理）

A 不修改 B/C/D/E/F/G 的文件，也不修改 `CMakeLists.txt` 或 `M1_CONTRACT.md`。

## 给 B（Sample 行为修复，B 独占）

请在 B 自己的文件里改行为，不要等链接失败再复制一套不同的数。

1. `contextValid`：`memoryGrantBytes == 0` 仍拒绝。`memoryGrantBytes > 1 GiB` 不再拒绝。cell 为 0 或 `> kSampleWorkUnitCellCeiling`（268435456）仍拒绝，错误码/文案与「授权太大」分开。建议码：`sample.invalid-context`、`sample.work-unit-ceiling`。
2. `applyDetectionTimeAnalysis` 传入 stream 的 `workingCellBudget` 使用 `min(working/12, kSampleWorkUnitCellCeiling)`。8 GiB 默认授权不再得到 512 Mi cells。ROI/Coarse 里 `readSpectrum` 按实际频谱上调 `spectrumCellBudget` 的行为保持不变。
3. `makePlan` 的 `legacyEligible` 增加 `exact <= kSampleContiguousAllocationCapBytes`。超过连续上限走已有 streaming/Auto，不分配整段 6 GiB。
4. 所选块 `estimatedResidentBytes > memoryGrantBytes` 时返回资源限制，detail 含 requested、available、`phase=time-refine`。可用 `decideSamplePhaseBudget`：此情况为 `Rejected` / `sample.chunk-exceeds-grant`。
5. 集成后请改引用 `domain/detection.h` 的常量。在 B 的 worktree 看不到本提交时，使用相同数值，不要把 1 GiB 或 256 Mi cell 改大。

`decideSamplePhaseBudget` 的判定顺序是：授权 0 → cell 上限 → 块驻留大于授权 → 驻留大于连续上限则 Adapted → 否则 Accepted。两种拒绝同时成立时先报告 work-unit。若主代理要求先报告 chunk，请改 A 的顺序后再让 B 依赖它。

`kind == Adapted` 或 `errorCode == sample.grant-exceeds-contiguous-cap` **不是**整次任务失败。`granted` 为 1 GiB 连续上限，规划器必须分块。

`phase` 字符串是 `time-refine`。

## 给 D（证据预检）

契约写明 D 在自己的 worktree 里保持预检分支，不要等链接 A 的新符号才能编译。`checkedMulU64` 集成后可替换 D 本地的 checked multiply；在那之前 D 继续用本地溢出检查。

`resource.raw-head-shortfall` 只属于 retain ON。A 没有在 `decideSamplePhaseBudget` 里发这个码。不要用 `detection_store.cpp` 的 `kStoreBytes`（256 MiB）拦截 retain OFF 且 `rawHeadBudgetBytes == 0` 的运行。

## 给 F（GPU batch）

A 没有添加 VRAM 选择函数，也没有改 `maximumExecutionBatch`（默认仍为 2）、`maximumDeviceBytes` 或 `maximumEngineBlobBytes`。`selectDetectionExecutionBatch` 仍由 F 放在 `detection_model_backend.h`。

## 给 G（测试）

`domain/detection.cpp` 已在现有 CMake 源列表中。本提交不新增测试文件，也不改 `CMakeLists.txt`。G 的 `tests/ss_de_rpo_logic_tests.cpp` 在链上 domain 库后可直接调用 `decideSamplePhaseBudget` 与 `checkedMulU64`。

建议断言：

- 授权 8 GiB、估计驻留不超过 1 GiB、cell 不超过上限 → `Accepted`，且不是 `Rejected`。
- 授权 8 GiB、估计驻留大于 1 GiB 且不超过授权 → `Adapted`，`granted == kSampleContiguousAllocationCapBytes`，文案含 `planner must chunk`。
- 估计驻留大于授权 → `sample.chunk-exceeds-grant`。
- cell 大于上限 → `sample.work-unit-ceiling`。
- 授权 0 → `sample.invalid-context`。
- `checkedMulU64` 溢出时返回 false 且 `out == 0`。

R-01 的红（6 GiB grant 在 **现有** `makePlan`/`contextValid` 上仍非法）在 B 合入前是预期的。不要把断言改回「必须拒绝 6 GiB」。A 的纯函数本身不会因为 6 GiB 授权拒绝。

## 给主代理

- 不需要为这两个符号改 CMake。
- 不要把 `ResourceDecision` 写入工程 JSON。
- 资源选项只增加只读 `QLabel`，objectName `workingSetAuthorizationNote`。`applyTo` 的数值和 QSettings 键未改。
- `main_window.cpp` 的启动连接仍归主代理，A 未动。
