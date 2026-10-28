# Agent C · 补丁说明

在通过本地校验的 `DetectionCard::submit()` 里，于 `requestSubmitted` 之前打开 provisional 闩锁：保存即将提交的 `runId`、`steady_clock` 纳秒，禁用开始按钮，按钮和预检显示「准备检测中」。闩锁仍有效时第二次 `submit` 只提示「检测正在准备，请稍候」。`updateShortStatus` 在闩锁有效且没有未完成 run 时不会重新打开按钮。run 进入工程后闩锁放下，避免任务结束后按钮一直禁用。

`noteStartRejected` 只在 runId 与当前闩锁一致时清除、显示错误并刷新。面板在保存前拒绝、启动保存失败、generation 变化和未发布这几条会 `restoreFormalDisplay` 的路径上，于释放 `commitInFlight_` 之后发出 `startRejected`。卡片闩锁不在面板对象上，主窗口必须连接。

`startRequest` 成功时把该 runId 和 steady 纳秒记入 `acceptedRunId_` / `acceptedSteadyNs_`。启动事务进行中的拒绝和工程写保护使用「检测正在准备，请稍候」。`publicationCommit_` 为真时才使用「检测结果正在提交，请稍后再试」。

开始按钮 tooltip 改为：接受新检测会丢弃本文件本 IQ 通道的上次正式结果，失败或取消不恢复；其它文件和通道不动；改参数、模型或引擎不清除已提交结果；保存前拒绝会恢复被临时隐藏的正式结果。未改 `beginDetection` 或 `discardCommittedFormalResult`。

`detectionPhaseText` 增加准备、预检、Worker 初始化，并把 `publishing` 显示为「正在校验并发布正式结果」。`inference` 仍是「模型前向」。
