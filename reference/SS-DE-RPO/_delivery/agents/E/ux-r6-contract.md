# UX-R6 正式结果契约（CONFIRMED_CODE）

Agent E 只记录当前代码。`beginDetection` 与 `discardCommittedFormalResult` 没有修改。`detection_task.cpp` 与 UI 没有修改。

## 接受启动后的丢弃

`DetectionResultService::beginDetection` 在 run id、slot generation 和来源校验通过后：

1. 把本 slot 的 `pendingRunId` 设为本次 run
2. `++slot->generation`
3. 调用 `discardCommittedFormalResult`

`discardCommittedFormalResult` 只处理当前 `FileState` 上、`slot.sampleChannel` 对应的上一次 `committedRunId`：

- 清空 `committedRunId`
- `suppressFormalResults = true`，`stale = true`，`unpublishedPreview = false`
- 删除该通道、该 run 的算法 `sourceSets` 与对应 marks
- 把该 run 记入 `retiredRunIds`
- 该通道上绑定这些算法标注的窄带通道关系改为 `Retired`

其它文件、其它 IQ 通道、人工标记和导入标签不在这个删除集合里。

失败、取消和放弃不会把这次丢掉的正式结果装回去：

- `abandonDetection` 只清掉仍匹配的 pending run
- `retainUnpublishedDetection` 留下未完成记录，并保持 `suppressFormalResults`
- `replaceCurrentDetection` 在未提交成功时不改调用方的 `Project`

`restoreFormalDisplay` 的注释与实现一致：只有乐观隐藏且 `committedRunId` 仍在时才重新显示。接受并安装之后 id 已经为空，该函数不会把正式结果找回来。

## 安装边界

`beginDetection` 改的是调用方传入的 `Project`。面板把当时工程复制为 candidate，再在 candidate 上调用它。`publishPendingStart` 成功后才把 candidate 写入正在使用的工程和 `.ssproj`。

因此：

- 保存前拒绝、排队失败、保存期间 generation 变化：正在使用的工程和原文件仍是上一次正式结果。相关文案含 `上次正式结果仍在`。乐观隐藏由 `restoreFormalDisplay` 收回。
- `publishPendingStart` 成功之后：本文件、本 IQ 通道的旧正式结果已经丢弃。随后失败、取消、放弃或不完整发布都不恢复它。

这与契约第 4 节一致：启动事务被接受并安装后丢弃本文件本 IQ 通道的旧正式结果；保存前失败恢复的是被临时隐藏的显示。

## 作用域

同一文件的另一个 IQ 通道、以及另一个文件，不随本次 `beginDetection` 丢弃。`markStale` 只把指定 `fileId + sampleChannel` 标成待更新，并退役该通道尚未安装的 pending run。它不调用 `discardCommittedFormalResult`，也不清空已有 `committedRunId`。

参数或模型提交走 `DetectionCard::configurationCommitted` → `DetectionPanel::markConfigurationStale`。未提交的输入草稿不会走到这里。只改参数、模型或引擎不会提前清除已提交结果。

## 测试源码中的对应断言

这些断言存在于当前树，本工作区没有重跑：

- `tests/ux_r5_detection_tests.cpp` `pendingKeepsFormalResultUntilSuccess`：`markStale` 后旧正式结果仍在；`beginDetection` 的 candidate 丢掉它；排队失败或 generation 变化不安装 candidate；安装后的放弃不恢复
- `filesAndChannels`：通道 0 标 stale 不改变通道 1 的权威结果
- `discardStaysOnItsFile`：文件 A 的已接受启动不改变文件 B
- `tests/task_ux_tests.h`：参数提交后 `committedRunId` 与标注编码不变，状态含 `待更新` 与 `当前显示上次检测结果`
