# Agent C · 时间线证据

## 时钟

两处都是本进程 `std::chrono::steady_clock` 自该时钟纪元起的纳秒，`uint64_t`。不是墙钟。不能和另一个进程的 steady 时钟直接相减。

| 名称 | 读取 | 何时写入 |
|---|---|---|
| `t_click` | `DetectionCard::startLatchSteadyNs()` | 本地校验已通过、`requestSubmitted` 发出之前 |
| `t_accepted` | `DetectionPanel::acceptedSteadyNs()`，run 为 `acceptedRunId()` | `startRequest` 已设 `commitInFlight_`、调用 `launchCommitWorker` 之前 |

`acceptedRunId()` / `acceptedSteadyNs()` 只在 `startRequest` 返回 true 之后代表这一次启动。返回 false 不会改写它们。之后的拒绝也不清掉，避免主窗口在返回 true 的当次调用里读到空值。下一次成功的 `startRequest` 会覆盖。

本树没有 `DetectionTaskManager::noteSteadyMark`。卡片和面板都不调用它。

## 同一次点击里的顺序

1. `submit` 发现闩锁已开：发「检测正在准备，请稍候」，返回，不发第二次 `requestSubmitted`。
2. 本地校验失败：发具体错误，不建闩锁。
3. 校验通过：生成原有形式的 `runId`，打开闩锁，禁用开始按钮，按钮和预检写入「准备检测中」，记下 `t_click`，然后 `emit requestSubmitted`。
4. 槽函数里 `startRequest` 返回 false：闩锁仍在，直到 `noteStartRejected`。
5. `startRequest` 返回 true：面板记下 `t_accepted` 与 runId，工作线程继续预检和保存。按钮保持禁用。
6. 预检/保存前拒绝、未发布或 generation 变化：GUI 线程 `startRejected(runId, message)`。
7. 发布成功且 run 出现在工程中：下一次卡片 `updateShortStatus` 放下闩锁。未完成 run 继续禁用按钮。

`refreshStatus` 在 `emit` 之后仍会跑。闩锁还在且 run 尚未入库时，它不能把按钮重新打开。

## 测量

未配置 CMake，未启动界面，未跑 CTest。没有点击到绘制的样本，因此没有 p95、中位数或最大值。目标 p95 < 100 ms 未验证，不能写成已经达到。

无截图，无录屏。

G 的 R-08 可用 `QElapsedTimer` 包住一次成功的 `submit`，只记录耗时，不要对未测量的 p95 下断言。按钮断言是：函数返回前 `runWidebandDetection` 已禁用，且 `text()` 含「准备检测中」；两次 `submit` 只有一次 `requestSubmitted`。
