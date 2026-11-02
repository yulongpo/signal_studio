# SS-DE-RPO · 子代理 C：启动交互状态机与防重入

你是独立 UI Lifecycle Owner。全部任务资料均在 `reference/SS-DE-RPO/`，先读 `05_STARTUP_AND_PIPELINE.md`、`03_AUDIT_BASELINE.md`、`07_AGENT_PROTOCOL.md`。目标分支 `rebuild/cursor`，在自己 worktree 开发。

## 独占代码所有权

`ui/detection/detection_card.*`、`ui/detection/detection_panel.*`、`ui/detection/detection_phase.*`。禁止直接编辑 D 的 detection task/worker 或 E 的 result service；缺少事件通过 `interface-requests.md` 向主代理/D 请求。

## 必须实施

1. 定位 `DetectionCard::submit()` 与 `DetectionPanel::startRequest()` 中 `commitInFlight_` 和 `unfinishedRun()` 不同步造成的重复点击窗口。第一次合法点击**同步**建立 provisional token、禁重/取消、显示 `准备检测中`，而不是继续保持“开始”使能。
2. 明确 Starting/Preflight/Committing/Queued/WorkerInitializing/Inference/FrequencyRefine/TimeRefine/Publishing/Completed/Failed/Cancelled 状态、转移及控件使能矩阵。
3. 启动阶段拒绝重入不得显示“检测结果正在提交”；切换数据源、关闭窗口、取消、preflight 失败、Worker 异常、后台任务完成等每一条路径均应保证状态恢复。
4. 仅在 Qt 主线程操作 QWidget；耗时的模型校验、文件扫描、worker spawn/wait、工程大序列化不能回流到 UI 线程；使用合规连接方式并验证对象生命周期/generation。
5. 将 UX-R6 当前正式结果生命周期通过 UI 提示反映出来；**参数修改/切换模型本身不能提前删正式结果**；不得擅自修改当前源码规定的“开始新检测处理旧结果”策略。
6. 目标 UI 可见反馈 p95 <100ms，并以真实测试结果报告。不要用主观观感代替测量。

## 测试与交付

与 G 验证 R-08/R-09/R-10/R-13/R-19。提交 `ui-state-table.md`、`ui-timeline-evidence.md`、相关截图/录屏路径、测试命令、`interface-requests.md`、`patch-summary.md`、`agent-status.md`、worktree commit。结果汇总到 `reference/SS-DE-RPO/_delivery/agents/C/`。
