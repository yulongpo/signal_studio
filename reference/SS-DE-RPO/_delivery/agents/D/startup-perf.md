# Agent D · 启动时钟

## 时钟

进程内 `std::chrono::steady_clock` 的 `time_since_epoch` 纳秒。字段 `timeline.clock` 固定为 `steady_clock_ns`。这不是 `QElapsedTimer`，也不是 worker 进程时钟。已有的 `firstDecodedCandidateMs`、`frequencyTracksReadyMs` 和 TensorRT CUDA 事件仍留在原来的字段里，没有换算进 timeline。

负的 epoch count 不写入，避免把回绕值当成一次真实时刻。本机 steady clock 正常为正。

## 已写入的 mark

这些名字会在对应时刻真实到达时出现。没有到达的不会补 0。

- `t_worker_spawn`
- `t_worker_hello`
- `t_engine_ready`
- `t_first_iq_read`
- `t_first_infer_start`
- `t_first_infer_end`
- `t_sample_refine_complete`
- `t_durable_publication`

`t_engine_ready` 表示父进程收到 `begin` 的 evidence，不表示 CUDA 已计时。没有 GPU 或 worker 在 `begin` 前失败时，这个点缺失。

Fixture 后端不进入 refine / 最终 publication 分支，因此没有 `t_sample_refine_complete` 和 `t_durable_publication`。

## 本代理没有写入的点

| 名称 | 原因 |
|---|---|
| `t_click` | 在 UI 点击路径。本代理不改 card/panel |
| `t_accepted` | API 已提供。主代理在启动 worker 前调用 `noteSteadyMark`；D 不在 `main_window.cpp` 里调用 |
| `t_preflight_start` / `t_preflight_end` | `prepareQueuedRun` 是 const，预检没有单独的时钟点。为保持现有拒绝文案，没有在预检里加计时 |
| `t_model_snapshot_ready` | 快照在 task 执行路径之外。`detection_model_snapshot` 没有可证明的缓存，本代理未改 |
| `t_source_plan_ready` | plan 在预检和 `source.open` 各发生一次。没有单独打点，避免把同一次准备记成两个时钟 |
| `t_manifest_committed` | 清单写入存在，但不是本轮要求的执行期相位。入队持久化不另造一个时刻 |
| `t_first_model_input` | 第一批模型输入就是第一次 inference exchange 的发送。单独再记会和 `t_first_infer_start` 重复 |
| `t_first_candidate_decoded` | 现有字段是 job 内 `QElapsedTimer` 毫秒，不是 steady epoch。没有抄进 timeline |
| `t_first_preview` | `publishPreview` 存在，但契约交给 D 的点不包括预览节流 |
| `t_inference_complete` | 循环结束和协议 `completed` 包不是单独的推理完成时刻。没有把最后一次交换再记一笔 |
| `t_frequency_complete` | `frequencyReadyMs` 同样是 job 内毫秒计时，不是 steady epoch |
| `t_terminal` | `finish` 持久化终态。正式 publication 点已经是 `t_durable_publication`，终态提交不再打第二下 |
| CUDA / GPU stream 时间 | 不是父进程 steady clock。继续留在 `tensorRtInference` |

## 测量

本轮按指令没有启动完整 CMake configure，也没有跑 CTest 或 GPU 推理。没有冷/热启动样本，因此没有 p95、中位数或百分比提升。GPU 状态：`UNVERIFIED_GPU`。

## raw-head 预检

`applyDetectionEvidencePreflight` 的拒绝字符串没有改。溢出不能被当成通过：

- legacy combined：`perWindow == 0` 直接返回；否则 `windows > uint64 max / perWindow` 时拒绝，不使用回绕后的乘积。
- JSON observation 与显式 raw-head：只有 `windows <= budget / perWindow` 才通过。该关系成立时乘积不会超过 `uint64`。
- retain ON：乘之前检查 `windows > uint64 max / perWindow`，溢出走原有 overflow 句。

因此没有再插入新的乘法，也没有改测试要搜索的句子。`retainRawModelOutput == false` 且 `rawHeadBudgetBytes == 0` 仍不按 raw-head 拒绝，配额保持 0。显式非 0 配额在 retain OFF 时仍走 `explicitRawHeadPreflightError`，不会被清成 0。

`detectionModelEvidenceIsDurable` 与 `preserveDurableModelEvidence` 的条件仍是 `legacyCombinedEvidence || rawHeadBudgetBytes > 0`。恢复路径里的 source set / annotation 比较没有删除。
