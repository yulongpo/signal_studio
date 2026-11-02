# SS-DE-RPO · 既有审查发现与版本边界

**注意：本文件记录此前对 `rebuild/cursor`、审查时 HEAD `82935ab34ec6126707da152bf46d1f352458ab0c` 的代码与日志分析。它是整改线索，不是新一轮的当前 HEAD 证明。**

## 1. 已确认的预算不一致

- `domain/detection.h`：默认工作集目标 `kDetectionWorkingSetTargetBytes = 8 GiB`。
- `infrastructure/detection_time_analysis.cpp`：Sample 时间精修分配 `working = request.workingSetBudgetBytes * 3/4`；`workingCellBudget=working/12`；精修实际运行策略又传 `memoryGrantBytes=working`。
- `infrastructure/detection_sample_stream.cpp` 的上下文校验存在上限（`memoryGrantBytes > 1024 MiB`、`workingCellBudget > 256 MiB` 时拒绝）。默认配置产生 6 GiB grant 与 512 MiB cell budget，从而与内层检查冲突。
- 已有日志中有 `20/20`、`363/363`、`2044/2044` 窗口处理后在 Sample refine 报 `plan: Invalid sample context/configuration/budgets` 的记录。该日志与上述代码逻辑吻合；不能据此断言每一项错误的全部根因都相同。

**整改要求**：分离实际资源需要、可用配额、工作量限制和强安全边界；超过真实峰值才拒绝或分块，而不是因为用户允许更多内存反而拒绝。

## 2. raw-head 磁盘预算

- 日志记录两次 `Raw model head disk budget rejected before inference: 2044 windows x 196608 bytes; limit 268435456 bytes`。
- 2044×196608=401866752 Bytes≈383.25 MiB，高于固定 256 MiB。若用户未要求保存 raw-head，则不应被可选原始模型输出额度阻止；若为必要证据，则需区分不同类型的持久化，并准确估算实际存储峰值。
- 新 HEAD 可能已更改容量策略；执行前核对是否仍存在。

## 3. 启动 UX 与重复提交

- `ui/detection/detection_card.cpp::updateShortStatus()` 的开始按钮使能逻辑在旧审查中主要检查 `unfinishedRun()`，而 `ui/detection/detection_panel.cpp::startRequest()` 先建立 `commitInFlight_=true` 并异步准备运行，二者之间存在 UI 可能继续使能的窗口。
- 日志多次出现启动阶段误导性提示“检测结果正在提交，请稍后再试”，而非实际正在执行的启动 preflight/manifest 事务。
- `runStartCommit()` 本来已采用后台准备；修复方向不是不分青红皂白再加一层线程，而是显式 Starting 状态、同步禁重、耗时定位、线程安全和失败取消收尾。

## 4. 完成状态与 publication

- 日志存在在全部窗口处理完毕后仍发生 `持久检测来源/对象数与 publication 不同` 的失败记录，例有 `159/159` 完成后发布失败。
- 最新审查时 HEAD 中包含 `preserveDurableModelEvidence()`，意图避免将非持久模型证据混入正式来源集合。**必须对最新代码复测**，不能假设补丁已彻底修好，亦不能沿用旧日志断言当前仍稳定复现。
- `application/detection_result_service.cpp` 中 sourceSet/annotation 数/对象校验不得删除；需区别 model evidence、preview、partial checkpoint 和 durable final publication。

## 5. UX-R6 既有结果生命周期

- 审查时 HEAD 的 `DetectionResultService::beginDetection()` 会在启动事务成功时调用 `discardCommittedFormalResult()`：**新检测接受后，本文件/通道原有正式结果被丢弃，失败后不恢复**。
- 参数或模型的单纯修改不应提前清除正式结果；不同 IQ 文件和通道不能互相污染。
- 当前执行的目标是**与实际 HEAD 保持一致**并检查 UI 提示、工程重开、失败/取消。若主代理认为产品层面需要成功后替换策略，应在设计报告提出单独决策，不得作为隐藏副作用改变。

## 6. 待核对的硬限制和性能风险

| 入口 | 旧审查发现 | 需要核对的内容 |
|---|---|---|
| `infrastructure/detection_model_backend.h` | `maximumDeviceBytes=4 GiB`、`maximumEngineBlobBytes=128 MiB`、`maximumExecutionBatch=2` | 模型 profile/安全性/自适应和设备峰值 |
| `domain/detection_postprocess.cpp` | 跟踪数量、候选比较数、驻留字节等边界 | 是否会导致长任务静默截断或失败 |
| `infrastructure/detection_task.cpp` | projector/refine 预算多次拆分、checkpoint/worker 开销 | 二次限制/复制/IO、实际阶段峰值 |
| `infrastructure/detection_source.cpp` | 单次源读取/块大小限制 | 应保留安全 chunking，不等同全程内存限额 |
| `ui/detection/detection_resource_options.*` | 部分资源选项已经开放 | 是否真正传到全部执行子路径 |

## 7. 证据等级要求

每个问题必须标注 `CONFIRMED_CODE`、`CONFIRMED_LOG`、`CONFIRMED_RUNTIME`、`HYPOTHESIS`、`RESOLVED_WITH_RUNTIME_EVIDENCE` 之一。无当前源码锚点/复测证据不能写“最新版本确认仍存在”。
