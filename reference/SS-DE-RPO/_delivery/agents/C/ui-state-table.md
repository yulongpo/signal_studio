# Agent C · 启动控件状态

基线 `82935ab34ec6126707da152bf46d1f352458ab0c`。闩锁在 `DetectionCard`，启动事务锁在 `DetectionPanel::commitInFlight_`。卡片看不到面板。成功路径在 run 进入工程后自行放下闩锁；失败路径要主窗口调用 `noteStartRejected`。

QWidget 更新只在卡片/面板所属线程。`submit` 与 `noteStartRejected` 若从其它线程进入，会 `QueuedConnection` 回 GUI 线程再改控件。`finishStartCommit` 仍由工作线程 `QueuedConnection` 回到面板线程。

## 控件

| 状态 | 开始按钮 | 预检/摘要 | 取消 | 再次点击 |
|---|---|---|---|---|
| 空闲，本地校验通过，无未完成 run | 可用。已有正式结果时文案「应用并重新检测」，否则「开始检测」 | 「配置就绪 · 可运行」 | 禁用 | 进入准备 |
| 本地校验失败 | 保持可用（无未完成 run 时） | 原有错误文案 | 禁用 | 不发 `requestSubmitted`，不建闩锁 |
| 准备中：`startLatchActive()` 且没有未完成 run | 禁用，文案为 `detectionPhaseText("preparing")`，即「准备检测中」 | 无其它错误时预检和摘要也是「准备检测中」 | 禁用。还没有可取消的 run | 立刻 `message`「检测正在准备，请稍候」，不再 `emit requestSubmitted` |
| run 已进入任一文件的 `detectionRuns` | 闩锁放下。未完成 run 仍禁用按钮 | `detectionPhaseText(computationPhase)` | 当前文件上未完成且非 Cancelling 时可取消 | 卡片提示已有未完成任务 |
| 未完成 run 结束 | 无新闩锁且校验通过则可再次开始 | 最近 run 的阶段文案 | 禁用 | 可开始下一次 |
| `noteStartRejected` 且 runId 一致 | 放下闩锁并 `refreshStatus`，按钮按空闲规则恢复 | 预检改为该错误 | 按空闲规则 | 可再次开始 |
| `noteStartRejected` 且 runId 不一致或闩锁已关 | 不变 | 不显示这次 message | 不变 | 不变 |

`updateShortStatus` 在 `startLatchActive()` 且 `unfinishedRun()==nullptr` 时不会把开始按钮设回可用。run 一旦出现在工程里，闩锁放下，之后由未完成 run 接管禁用。

切源、关窗不在卡片里单独清闩锁。切源后按钮仍显示准备中，直到失败信号或该 run 出现。关窗时卡片与面板一起销毁。

## 面板锁

| 条件 | 文案 |
|---|---|
| `commitInFlight_ && !publicationCommit_`（启动事务） | 「检测正在准备，请稍候」 |
| `commitInFlight_ && publicationCommit_`（正式结果提交） | 「检测结果正在提交，请稍后再试」 |
| 两者都为假 | 工程写保护放行 |

同一句用于 `startRequest` 重入、工程写保护、`markConfigurationStale`、`discoverRecoverable`。只有 `publicationCommit_` 为真才使用「检测结果正在提交，请稍后再试」。

## 阶段文案 `detectionPhaseText`

| phase | 文案 |
|---|---|
| `preparing` / `starting` | 准备检测中 |
| `preflight` | 正在检查来源/模型/资源 |
| `worker-init` / `initializing` | 初始化 Worker |
| `inference` | 模型前向（原文保留） |
| `refine` | 时间精修（原文保留） |
| `publishing` | 正在校验并发布正式结果 |

当前 task 会写 `preflight` 与 `publishing`。它不写 `worker-init`。准备阶段的可见文案来自按钮、预检和摘要，不依赖 task 的 phase。

## 失败时谁清闩锁

| 路径 | 面板 | 卡片闩锁 |
|---|---|---|
| `startRequest` 同步返回 false（来源、路径、已有事务） | 未隐藏正式结果，或仅拒绝重入 | 不在面板内清除。主窗口必须 `noteStartRejected(request.runId, error)` |
| `rejectedBeforeSave`、保存失败 `!job->ok`、generation 变化 | `restoreFormalDisplay` 后 `releaseCommitLock`，再 `startRejected` | 主窗口把 `startRejected` 接到 `noteStartRejected` |
| 已保存前拒绝之外的 `!published` | 同上，并 `presentationRequested` | 同上 |
| 启动事务成功且 run 已在工程中 | `presentationRequested` 会刷新卡片 | 卡片见到相同 runId 后自己放下闩锁，不需要 `noteStartRejected` |

`restoreFormalDisplay` 的原语义未改：只恢复仍带 `committedRunId` 的临时隐藏。接受后的丢弃不在这里恢复。
