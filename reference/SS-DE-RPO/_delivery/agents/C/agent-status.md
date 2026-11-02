# Agent C 交接

- **Agent**：C
- **任务编号**：SS-DE-RPO M1 第 4 节，启动闩锁与准备阶段文案
- **审查基线**：`82935ab34ec6126707da152bf46d1f352458ab0c`
- **Worktree**：`D:\project\ss-de-rpo-wt\c`
- **分支**：`fix/ss-de-rpo-c`
- **提交**：`fix(ss-de-rpo): latch detection start and name the prepare phase`。本文与代码在同一提交，精确 SHA 以提交后的 `git rev-parse HEAD` 为准。
- **所有权**：`ui/detection/detection_card.h`、`detection_card.cpp`、`detection_panel.h`、`detection_panel.cpp`、`detection_phase.h`，以及本目录。未改 `app/main_window.cpp`、detection task、detection result service、CMakeLists。
- **与冻结契约**：公开方法 `noteStartRejected`、`startLatchActive` 按契约。另有只读 `startLatchRunId`、`startLatchSteadyNs`、`acceptedRunId`、`acceptedSteadyNs`，供主窗口读取时钟。未调用尚不存在的 `noteSteadyMark`。

## 1. 已确认根因

| 问题 | 事实类型 | 位置 | 影响 | 修复 |
|---|---|---|---|---|
| 本地校验通过后、run 入库前开始按钮仍可用 | CONFIRMED_CODE | `DetectionCard::submit` 原实现在 `emit requestSubmitted` 前不禁用按钮；`updateShortStatus` 在无 `unfinishedRun` 时按校验结果打开按钮 | 同一工程可在启动事务完成前再次进入 `startRequest` | 校验通过后同步闩锁并禁用；状态刷新不得在此窗口打开按钮 |
| 启动事务重入使用正式结果提交文案 | CONFIRMED_CODE | `startRequest` 与 `setProjectWriteGuard` 在任意 `commitInFlight_` 时写「检测结果正在提交，请稍后再试」 | 准备阶段被说成结果提交 | 仅 `publicationCommit_` 使用该句；启动事务使用「检测正在准备，请稍候」 |
| 按钮 tooltip 与接受后丢弃正式结果相反 | CONFIRMED_CODE | `updateShortStatus` 原 tooltip | 界面声称失败仍保留上次正式结果 | 改为接受后丢弃、失败不恢复、保存前拒绝才恢复临时隐藏 |

## 2. 代码变更摘要

见 `patch-summary.md`。状态矩阵见 `ui-state-table.md`。时钟见 `ui-timeline-evidence.md`。主窗口接线见 `interface-requests.md`。

## 3. 验证

| 用例 | 命令 | 结果 | 证据 |
|---|---|---|---|
| R-08 两次 submit | 未运行。未做 CMake configure | BLOCKED | 无日志。实现按契约写在 `submit` |
| R-09 预检期间取消/退出 | 未运行 | BLOCKED | 准备阶段取消按钮仍禁用，见状态表 |
| R-10 来源/磁盘失败 | 未运行 | BLOCKED | 失败路径发 `startRejected` 并 `restoreFormalDisplay` |
| R-13 参数/模型不提前删正式结果 | 未运行 | BLOCKED | 未改 `beginDetection`。tooltip 与该规则一致 |
| R-19 p95 < 100 ms | 未运行 | BLOCKED | 无样本。不声称达标 |

无截图、无录屏。

## 4. 性能

无测量。`t_click` 与 `t_accepted` 已记入成员，未写入 performance JSON。

## 5. 对其它代理的请求

见 `interface-requests.md`。主窗口两条连接和 D 的 `noteSteadyMark` 都未在本提交中调用。

## 6. 未完成

集成前，启动被拒绝时开始按钮会停在「准备检测中」，直到主窗口调用 `noteStartRejected`。成功路径不依赖该连接：run 出现在工程里后卡片自己放闩锁。

`worker-init` 仅有文案映射。当前 task 不写这个 phase。

## 7. 交接声明

已在 `fix/ss-de-rpo-c` 提交，未推送。未修改其它 owner 的源文件。未把未测量的界面耗时写成已通过。未改目标分支以外的正式分支。
