# SS-DE-RPO M1 契约（主代理冻结）

基线 HEAD：`82935ab34ec6126707da152bf46d1f352458ab0c`（`rebuild/cursor`）。  
本文件冻结后，B/D 与其余代理按此实现。不得改本文件；接口缺口写入各自 `interface-requests.md`。  
不要把新运行时字段写入工程 JSON。`DetectionRequest` 的持久字段保持现状。8 MiB shard、批次元组 `{1,2,4,8,16}`、FFSCN/SCN 数学与坐标保持不变。

时钟：进程内 `std::chrono::steady_clock` 的 `time_since_epoch` 纳秒。Worker 是另一进程，禁止把 worker 时钟与 UI 时钟相减。跨进程事件在父进程收到消息时用父进程 steady clock 盖章。

## 1. 资源：授权 ≠ 峰值 ≠ 工作单元

常量（A 放入 `domain/detection.h`，B 在 Sample 路径使用相同数值；集成时主代理可改为引用 A 的常量）：

- `kSampleContiguousAllocationCapBytes = 1024 * 1024 * 1024`  
  单次连续分配安全上限。它不是 RAM 授权上限。
- `kSampleWorkUnitCellCeiling = 256 * 1024 * 1024`  
  单个算法工作单元的 cell 数量上限。它不是字节预算。

`ResourceDecision`（A，`domain/detection.h` / `.cpp`，不进入工程 JSON）：

- `kind`: `Accepted | Adapted | Rejected`
- `errorCode`: 稳定英文码
- `phase`, `resource`（`ram` / `vram` / `disk` / `work-unit` / `format`）
- `requested`, `available`, `reserve`, `granted`（字节或 cell，字段注释写明单位）
- `message`：必须包含阶段、资源、需要、可用、回退

错误码至少包括：

- `sample.invalid-context`：输入/几何/参数非法（原有非法上下文）
- `sample.grant-exceeds-contiguous-cap`：不要用这个码拒绝「授权大于 1 GiB」。大于 1 GiB 的授权是合法的
- `sample.chunk-exceeds-grant`：即使最小块的驻留也超过授权
- `sample.work-unit-ceiling`：调用方把字节预算塞进 cell 预算，超过 cell 上限
- `resource.arithmetic-overflow`
- `resource.raw-head-shortfall`：仅 retain ON

纯函数由 A 实现，供单测。D 在 `applyDetectionEvidencePreflight` 内保持与下面第 3 节相同的分支，不要等链接 A 的新符号才能在自己的 worktree 编译。

## 2. Sample（B 独占）

根因：`applyDetectionTimeAnalysis` 把 `workingSetBudgetBytes * 3/4` 同时当作 `memoryGrantBytes` 和 `workingCellBudget = working/12`。`contextValid` 又把授权当成非法配置。默认 8 GiB → 6 GiB 与 512 Mi cells → `Invalid sample context/configuration/budgets`。

必须：

1. `contextValid` 不再因为 `memoryGrantBytes > 1 GiB` 拒绝。`memoryGrantBytes == 0` 仍拒绝。
2. `workingCellBudget == 0` 或 `> kSampleWorkUnitCellCeiling` 仍拒绝，但错误码/文案与「授权太大」分开。
3. `applyDetectionTimeAnalysis` 在 Sample 路径传入 stream 的 `workingCellBudget` 使用 `min(working/12, kSampleWorkUnitCellCeiling)`，使 8 GiB 默认授权不再得到 512 Mi cells。ROI/Coarse 里 `readSpectrum` 按实际频谱上调 `spectrumCellBudget` 的行为保持不变。`tests/detection_time_pipeline_tests.cpp` 的 `spectrumCapacityMatchesActualConsumer` 必须继续通过：实际 STFT cells 可以大于 `working/12`，且 `stftWorkingCellBudget >= cells`。
4. `makePlan` 的 `legacyEligible` 增加连续分配上限：精确路径估计驻留 `exact` 必须同时 `<= memoryGrantBytes` 且 `<= kSampleContiguousAllocationCapBytes`。超过连续上限时走已有 streaming/Auto 路径，不分配整段 6 GiB。不伪造活动段，不放宽 FIR/分位/精度门槛。
5. 若所选块的 `estimatedResidentBytes > memoryGrantBytes`，返回 `ErrorCode::ResourceLimit`，detail 含 requested、available、phase=`time-refine`。低内存是拒绝或更小块，不是把授权改小来假装通过。
6. 保留 `tests/detection_sample_stream_tests.cpp` 里 96 MiB grant 的既有断言。

禁止：只把 1 GiB / 256 Mi 常量改大；删除 `contextValid`。

## 3. raw-head 与证据预检（D 独占 `detection_task.cpp`）

现有回归必须保持，尤其 `testWidebandPreflightOmitsRawHeadsAndKeepsJsonBudget`：

- `retainRawModelOutput=false` 且 `rawHeadBudgetBytes=0` 且非 legacy：2044×196608 不因 raw-head 拒绝，排队后 `rawHeadBudgetBytes` 仍为 0。
- `retainRawModelOutput=false` 且 `rawHeadBudgetBytes=256 MiB` 且非 legacy：仍然拒绝，文案继续包含 `before inference`、`2044 windows x 196608 bytes`、`limit 268435456 bytes`。这是显式配额，不是「OFF 且无配额」。
- `retainRawModelOutput=true` 且配额 0 且卷剩余不足：拒绝文案继续包含窗口数、每窗字节、`require` 的总字节、`bytes remain`。
- legacy combined 预检文案保持不变。

在不破坏上述字符串的前提下：

- retain ON 且卷探针显示剩余空间足够、显式配额为 0 或 `>= 401866752` 时必须接受，并把接受的字节数写入 `rawHeadBudgetBytes`（现有 `retainedRawHeadPreflight` 行为）。
- retain ON 且显式配额小于 `windows * perWindow`：拒绝，同时写出 requested 与 limit。已有路径保持。
- 溢出用 checked multiply，禁止回绕后当成通过。
- 不要用 256 MiB 的 `kStoreBytes` 去拦截 retain OFF 且配额为 0 的运行。该常量只属于 legacy/JSON store。
- 8 MiB shard 上限不动。

`preserveDurableModelEvidence` 与 `detectionModelEvidenceIsDurable` 的条件保持：`legacyCombinedEvidence || rawHeadBudgetBytes > 0`。不要删掉恢复后的 sourceSet/annotation 比较。

## 4. 启动 UI（C 独占 card/panel/phase）

同一 Qt 事件里，第一次通过本地校验的 `DetectionCard::submit()` 在 `emit requestSubmitted` 之前：

- 记录 steady clock 为 `t_click` / `t_accepted`
- 保存 provisional runId（沿用即将提交的 `value.runId`）
- 开始按钮 `setEnabled(false)`，按钮或预检文案为 `准备检测中`
- `updateShortStatus` 在 provisional 有效且尚无 `unfinishedRun` 时不得把按钮重新打开

同一次点击处理尚未返回时，第二次 `submit` 不得再发请求。返回后的再次点击若 provisional 仍有效，提示 `检测正在准备，请稍候`，不得使用 `检测结果正在提交`。

`DetectionPanel::startRequest`：

- 启动事务进行中（`commitInFlight_ && !publicationCommit_`）的拒绝文案改为 `检测正在准备，请稍候`。
- 只有 `publicationCommit_` 为真时，工程写保护才使用 `检测结果正在提交，请稍后再试`。
- 预检失败、保存前拒绝、关闭、切源、取消：清掉 provisional，恢复按钮。已有 `restoreFormalDisplay` 语义不变。

`main_window.cpp` 的连接由主代理在集成时修改。C 在 `detection_card.h` 增加：

```cpp
void noteStartRejected(const std::string& runId, const QString& message);
bool startLatchActive() const;
```

`noteStartRejected` 只在 runId 与当前 provisional 一致时清除 latch，并显示具体错误。

UX-R6 文案对齐当前代码，不改 `beginDetection`：

- 启动事务被接受后，丢弃本文件、本 IQ 通道的旧正式结果，失败或取消不恢复。
- 其它文件和通道不动。
- 只改参数、模型或引擎不清除已提交结果。
- 保存前失败会恢复被临时隐藏的正式结果显示。

替换卡片 tooltip 中「进行中仍显示当前通道上次正式结果；只有新检测完整成功才整体替换」这句与代码相反的描述。

阶段提示至少覆盖：准备检测中、正在检查来源/模型/资源、初始化 Worker、模型前向、时间精修、正在校验并发布正式结果。已有 `detectionPhaseText` 可扩展，不要在后台线程改 QWidget。

D 在 `DetectionTaskManager` 增加：

```cpp
void noteSteadyMark(const std::string& runId, const char* name,
                    std::uint64_t steadyNanoseconds);
```

把 mark 追加进该 run 的 `performanceJson`：`timeline.clock = "steady_clock_ns"`，`timeline.marks[] = {name, steadyNs}`。run 尚未入队时先缓冲，入队后挂上。C 不直接改 task 文件。主代理集成时在 `startRequest` 成功启动 worker 之前调用 `noteSteadyMark(runId, "t_accepted", ns)`。D 自己在 worker spawn、hello、engine ready、first read、first infer start/end、refine、durable publication 用父进程时钟打点。没有 GPU 时相关点可以缺失，但不得写假时间。

## 5. Publication（E）

`replaceCurrentDetection` 里这两次拒绝保留，并在原句后面追加诊断，原句前缀不变：

- `持久检测来源/对象数与 publication 不同`
- `持久检测对象与 publication 不同`

追加：recovered/publication 的 annotation 数量、sourceSet id、第一处不同的 annotation id（若数量相同则比较编码）。不要删除 `encodeSourceSet` / `encodeAnnotation` / `recoverDetectionPublication` 比较，不要把推理完成标成 Completed。

跟踪：`maximumPreviewEvents` 只限制预览。正式 `FrequencyTrack` 不得因为预览上限被丢掉。比较次数上限仍是安全闸。优先用频率 bin 索引减少比较；索引后仍超过上限时保持现有显式错误（`association-comparison-limit-exceeded` / `frequency-fusion-comparison-limit-exceeded`），禁止静默丢正式轨迹。不要只把 4_000_000 改大当作修复。改动必须让 `tests/detection_postprocess_tests.cpp` 继续通过。

## 6. GPU batch（F）

默认 `maximumExecutionBatch` 保持 2。禁止无测量地把默认或请求 batch 改成 16。

在 `detection_model_backend.h` 增加纯函数（无 CUDA 调用）：

```cpp
// 返回 1/2/4/8/16。无法满足 batch 1 时返回 0，调用方拒绝本次加载，不跳窗。
// freeDeviceBytes == 0 表示未知，此时不按显存缩小，只按 requested、workerLimit、profileMaximum 取最小合法档。
std::uint32_t selectDetectionExecutionBatch(
    std::uint32_t requested, std::uint32_t workerLimit,
    std::uint32_t profileMaximum, std::uint64_t bytesPerUnitBatch,
    std::uint64_t freeDeviceBytes, std::uint64_t reserveBytes);
```

装载路径在已有 profile 夹紧之后，若 `cudaMemGetInfo` 成功，用该函数选择不超过 profile 的档，并写入现有 evidence 的 `physicalBatch`。已有 `retryableResourceFailure` 降档保留。OOM 后降到更小合法档并重试同一批窗口；batch 1 仍放不下则失败并写明需要/可用。没有 NVIDIA 设备时函数单测仍要跑，整机推理标 `UNVERIFIED_GPU`。

## 7. 测试登记（G 写文件，主代理登记 CMake）

只新增：

- `tests/ss_de_rpo_logic_tests.cpp`：链接 `SignalStudioInfrastructure`。覆盖 R-01（6 GiB grant + cell ceiling 下 `makePlan` 不返回 invalid context）、R-04/R-05（调用 `applyDetectionEvidencePreflight` 或 `prepareQueuedRun`，断言与第 3 节一致）、R-12（durable / non-durable 的 `preserveDurableModelEvidence` 不把未落盘 head 写进 source set；可复用现有测试风格）。
- `tests/ss_de_rpo_ui_tests.cpp`：需要 UI 源时在交接里写明链接方式。覆盖 R-08：两次 submit 只有一个 `requestSubmitted`，按钮在函数返回前为禁用且文案含 `准备检测中`。用 `QElapsedTimer` 记录返回耗时，不断言未测量的 p95。

G 不修改生产代码，不修改 `CMakeLists.txt`。在基线上 R-01 红测试失败是正确的 RED，不要把断言改回「必须拒绝 6 GiB」。修复合入后再由主代理复跑。

每个用例记录命令、退出码、PASS/FAIL/BLOCKED。没有真实 engine 的 GPU 用例写 `UNVERIFIED_GPU`。

## 8. 所有权与集成

见 `07_AGENT_PROTOCOL.md`。主代理独占 `CMakeLists.txt`、`app/main_window.cpp` 的启动连接，以及把 C 的 `t_accepted` 接到 D 的 `noteSteadyMark`。禁止 `git push --force`。未跟踪的用户文件不得删除。
