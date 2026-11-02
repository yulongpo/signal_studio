# Agent D · 接口请求

本代理不改 `M1_CONTRACT.md`。下面是集成时需要主代理处理的接点。

## 1. `t_accepted` / `t_click`

`DetectionTaskManager::noteSteadyMark(runId, name, steadyNanoseconds)` 已加在 `infrastructure/detection_task.h`。

请主代理在 `startRequest` 已经接受、并且 `pump` / worker 启动之前调用：

```cpp
tasks.noteSteadyMark(runId, "t_accepted", parentSteadyNs);
```

`parentSteadyNs` 必须是 UI 进程同一次 `steady_clock::time_since_epoch` 纳秒。若 C 在同一次点击里已经取得 `t_click`，也用这个函数传入，名字用 `t_click`。D 没有改 `app/main_window.cpp`。

run 尚未入队时调用会缓冲；`queuePreparedRun` 成功入队时写入 `performanceJson`。入队之后调用则追加，不删除已有 mark。

## 2. 不要在 PersistRun 回调里调用

`queuePreparedRun` 在持有任务 mutex 期间调用 `persistRun_`。该 mutex 不是递归锁。Persist 回调里再调用 `noteSteadyMark` 会在同一线程上死锁。请在入队函数返回之后，或从其它线程在入队之前调用。

## 3. 时钟

不要把 worker 进程的时间戳与 `timeline.marks[].steadyNs` 相减。跨进程事件只使用父进程收到消息时盖上的纳秒。

`t_first_infer_start` / `t_first_infer_end` 是父进程第一次 batch `exchange` 的前后，不是 CUDA event。GPU 间隔仍在现有 `tensorRtInference` / `gpuProfile`。

## 4. performanceJson 形状与上限

非空 `performanceJson` 必须仍是对象，且 `schema` 为 `signal-studio-detection-profile-v1`，否则现有 `writeDetectionRun` 会拒绝。timeline 是附加对象：

- `timeline.clock` = `steady_clock_ns`
- `timeline.runId` = runId
- `timeline.marks[]` = `{name, steadyNs}`，两者都是字符串，`steadyNs` 为十进制

64 KiB 上限没有放宽。请不要按窗、按候选调用 `noteSteadyMark`。本代理自己只写上面列出的少量相位点。

空 runId、null name、空 name 会被忽略。

## 5. 没有新的持久请求字段

`DetectionRequest` 的工程 JSON 字段不变。timeline 只在 run 的 `performanceJson` 里。

## 6. 对其它代理

- C：继续不改 `detection_task.*`。按钮和阶段文案由 C 负责。
- E：`preserveDurableModelEvidence` 与 publication 恢复比较保持原条件。D 不改 result service。
- F：执行 batch 仍用现有 `detectionExecutionBatchSize` 与 worker evidence 的 `logicalMaximumBatch`。D 不改 backend，也不把默认 batch 改成 16。
- A：预检分支仍在 `detection_task.cpp` 本地实现，没有链接 A 的新符号。
