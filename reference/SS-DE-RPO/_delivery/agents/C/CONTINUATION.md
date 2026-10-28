# Role C 续作交接

基线 `7220b2e295963288cb5d11e76946bb77646be0b0`，分支 `codex/ss-de-rpo-c`。`detection_phase.*` 未改。

## 改动文件

- `ui/detection/detection_card.h`
- `ui/detection/detection_card.cpp`
- `ui/detection/detection_panel.h`
- `ui/detection/detection_panel.cpp`
- `tests/ss_de_rpo_start_tests.cpp`

## 行为

活动 `StartCommit` 持有共享 `StartToken`。卡片在持久化 run 出现前启用取消，同步于 `requestSubmitted` 之前，信号仍是 `cancelRunRequested(startLatchRunId)`。`DetectionPanel::cancelRun` 已公开：先处理该 token，再查已持久化 run。`publishPendingStart` 成功前拒绝并恢复正式结果显示；成功后调用 `DetectionTaskManager::cancel`。token 进入 `stillCurrent`、模型快照、预检、清单和入队回调。

`startRequest(request, error, clickSteadyNs = 0)`。`clickSteadyNs == 0` 表示调用方未给点击戳，不写 `t_click`。

D 的可选停止回调按 `std::function<bool()>` 接入。本树签名仍是两参数 `freezeDetectionModelSnapshot` 与五参数 `prepareQueuedRun`，用 SFINAE 在边界前后检查；三参数 / 六参数出现后会把同一谓词传入。谓词为真表示停止。

`noteSteadyMark` 记录：有戳时的 `t_click`、`t_accepted`、`t_model_snapshot_ready`、`t_preflight_start`、`t_preflight_end`、`t_manifest_committed`、`t_start_project_commit`。发布路径在 `replaceCurrentDetection` 首次 `stillCurrent` 写 `t_recovery_ready` 与 `t_compare_ready`。`t_project_commit` 与 `t_durable_publication` 只在 `finishPublicationCommit` 确认 `installed` 之后各读一次时钟。`t_terminal` 在非 `Completed` 终态被观察到时，或在发布决定之后，每个 run 一次。`Completed` 等到发布决定。阶段标记在 `publishPendingStart` 成功后刷入，避免取消的启动继续堆积 pending marks。

不同 run id 的 `noteStartRejected` 不解除闩锁。来源上下文变化和诊断代际不匹配时不覆盖准备中文案，闩锁期间忽略诊断回报。提交线程不调用 QWidget。

测试缝：`setStartCommitGate`、`startCommitGateBlocked`、`setCancelAfterStartCommitForTest`。

## 实际检查

`scripts/build.ps1 -Action Configure -Jobs 2`，preset `local-debug`，2026-10-28 22:18:39–22:18:50 完成。接着 `-Action Build -Jobs 2 -Targets SignalStudioSsDeRpoUiTests,SignalStudioSsDeRpoStartTests`，22:18:50–22:20:34 完成，产物在本工作区 `out/vs2026-qt611-debug_bin`。为编译新测试，曾临时把目标写入 `CMakeLists.txt`，提交前已还原。

同目录 CTest（未把 Qt bin 放入 PATH）中 `signal_studio_ss_de_rpo_ui` 与 `signal_studio_ss_de_rpo_start` 均为退出码 `0xc0000135`。部署目录有 `Qt6Cored.dll`，没有 `Qt6Testd.dll`。

将 `D:/softwares/Qt/6.11.1/msvc2022_64/bin` 置于 PATH 前端，`QT_QPA_PLATFORM=offscreen`，`QT_SCALE_FACTOR=1.5`，直接运行：

- `SignalStudioSsDeRpoUiTests.exe`：Totals 4 passed, 0 failed，4486 ms。`doubleClickArmsOneStart` 的点击处理返回 9 ms，这是单次测量。`synchronousRejectionKeepsTheError` 通过。
- `SignalStudioSsDeRpoStartTests.exe`：Totals 6 passed, 0 failed，14495 ms。四项业务用例通过：过期拒绝不解锁、准备期取消不到达发布、发布前取消保留 `formal-seed`、提交后取消使 `queued-cancel` 进入 `Cancelled` 并带有上述启动标记与 `t_terminal`，该次时间线没有 `t_durable_publication`。数据为 `pipeline-fixture-v1`，未执行 GPU 推理。

## 给 main / D 的接口

main 仍要接两处，本角色未改 `app/main_window.cpp`：

- `startRequest(request, error, detectionCard_->startLatchSteadyNs())`
- `cancelRunRequested` 接到 `DetectionPanel::cancelRun`。当前 `tasks().cancel` 对尚未入队的 id 返回 false。

D 预期签名：

- `bool freezeDetectionModelSnapshot(DetectionRequest&, QString& error, std::function<bool()> cancelled = {})`
- `bool prepareQueuedRun(..., std::function<bool()> cancelled = {})`

`t_durable_publication` 在 GUI 发布成功路径由 C 写入。`infrastructure/detection_task.cpp` 里分片完成处的同名标记仍由 D 写入，本轮未改该文件。

## 未决

- main 的点击戳与 `cancelRun` 接线。
- 新测试尚未进入 `CMakeLists.txt`。
- 停止回调在 D 落地前不能中断 `freeze` / `prepare` 函数内部。
- `persistQueuedRun` 发生在被拒绝的 `publishPendingStart` 之前，失败发布仍可能留下清单文件。
- 未测 p95，未做 GPU 推理，未做 4K 实屏验收。
