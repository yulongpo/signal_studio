# Agent D · 启动链与打点位置

基线 `82935ab34ec6126707da152bf46d1f352458ab0c`。本文件只描述本 worktree 里实际读到的父进程路径。UI、`main_window.cpp` 和 publication 服务没有修改。

## 调用链

```text
DetectionCard::submit                         （C，本补丁不改）
  DetectionPanel::startRequest                （C，本补丁不改）
    DetectionTaskManager::prepareQueuedRun
      validateDetectionRequest
      makeDetectionSourcePlan
      applyDetectionEvidencePreflight         文案与分支保持原样
      detectionExecutionBatchSize
    queuePreparedRun
      adoptSteadyMarks                        入队前把缓冲 mark 写入 performanceJson
      persistRun_
    pump                                      不打点；只拉起 execute 线程
      execute
        checkpoint                            重写 performanceJson 时保留已有 mark
        FixedDetectionSource::open
        DetectionWorkerClient::start
        读窗循环 / 首次 batch exchange
        非 fixture：decode、track、frequency、time refine、最终 shard
        finish                                终态 performanceJson 再次带上全部 mark
```

`noteSteadyMark` 在 run 尚未进入 `jobs_` 时只按 runId 缓冲。`queuePreparedRun` 和 `retainPreview` 入队或更新该 run 时追加，不覆盖已有 mark。`performanceSnapshot` 每次重写诊断 JSON 时从同一列表生成 `timeline`，避免 checkpoint 把先前的 mark 丢掉。

## 父进程实际打点

时钟都是本进程 `std::chrono::steady_clock::now().time_since_epoch()` 的纳秒。Worker 进程时钟没有读入，也没有与这些值相减。

| 名称 | 位置 | 条件 |
|---|---|---|
| `t_worker_spawn` | `DetectionWorkerClient::start`，`waitForStarted` 成功之后 | 进程没有起来则不写 |
| `t_worker_hello` | 同一函数，握手响应 `type == ready` 之后 | 没收到握手则不写 |
| `t_engine_ready` | 同一函数，`begin` 交换成功并写入 `evidence_` 之后 | `begin` 失败则不写 |
| `t_first_iq_read` | `execute` 里第一次 `readModelWindow` 成功返回之后 | 第一次读失败则不写；后续窗不重复 |
| `t_first_infer_start` | 第一次 batch `worker.exchange` 调用之前 | 只记第一批 |
| `t_first_infer_end` | 该次 exchange 成功返回之后 | 失败则只有 start，没有 end |
| `t_sample_refine_complete` | `applyDetectionTimeAnalysis` 返回成功之后 | 仅非 fixture；失败不写 |
| `t_durable_publication` | 最终 `frequency-publication-end-v1` shard `flush` 成功之后、`finish` 之前 | 仅非 fixture；flush 失败不写 |

`execute` 在 `worker.start` 返回后把 client 上的三枚启动戳转发进 `noteSteadyMark` 一次。`exchange()` 内部不再打点，因此 `begin` 和最后的 `completed` 包不会被记成推理。`pump` 不打点。

## performanceJson

有 mark 时追加或重建：

```json
{
  "schema": "signal-studio-detection-profile-v1",
  "timeline": {
    "clock": "steady_clock_ns",
    "runId": "<runId>",
    "marks": [
      {"name": "t_worker_spawn", "steadyNs": "123"}
    ]
  }
}
```

`steadyNs` 是十进制字符串。入队时若诊断 JSON 还是空的，会补上既有 schema，否则 `writeDetectionRun` 会拒绝没有 schema 的 performance 对象。已有 profiler 字段保留，只替换 `timeline`。

空 runId、空指针 name、空字符串 name 忽略。同名 mark 再次到达时继续追加，不删除旧项。
