# Agent D · 补丁摘要

## 改了什么

`DetectionTaskManager::noteSteadyMark` 把父进程 steady clock 纳秒追加到该 run 的 `performanceJson.timeline`。run 还不存在时先按 runId 缓冲，入队或 `retainPreview` 更新时挂上。checkpoint、批次清单回写和 `finish` 重写诊断 JSON 时从同一列表生成 marks，不丢弃已有项。

Worker 启动链上能分开观察到的三个时刻在 `DetectionWorkerClient::start` 里盖章，再由 `execute` 转发一次：

- `t_worker_spawn`：`waitForStarted` 成功
- `t_worker_hello`：收到握手 `ready`
- `t_engine_ready`：`begin` 交换成功

`execute` 另外只在真实到达时写入 `t_first_iq_read`、`t_first_infer_start`、`t_first_infer_end`、`t_sample_refine_complete`、`t_durable_publication`。第一次推理失败会留下 start 而不写 end。Fixture 没有 refine 和 durable publication 点。

## 预检

`applyDetectionEvidencePreflight` 及其四段错误字符串没有改。legacy 与 retain ON 已有 checked multiply；JSON 预算和显式 raw-head 配额用除法判断，通过时乘积不会回绕。因此没有改测试要搜索的句子，也没有在 retain OFF 时把非 0 的 `rawHeadBudgetBytes` 清零。

`detectionModelEvidenceIsDurable` 与 `preserveDurableModelEvidence` 仍要求 `legacyCombinedEvidence || rawHeadBudgetBytes > 0`。

## 没有改的文件

- `infrastructure/detection_source.h` / `.cpp`：预检字段不需要新语义。
- `infrastructure/detection_model_snapshot.*`：每次仍读取清单并检查 sha256 形式和引擎文件是否存在。这里没有可失效的缓存，不能证明可以去掉或跳过哈希检查，所以未改。
- UI、`detection_result_service`、`detection_model_backend`、sample stream、`CMakeLists.txt`、`M1_CONTRACT.md`、测试源文件。

## 文件

- `infrastructure/detection_task.h`
- `infrastructure/detection_task.cpp`
- `infrastructure/detection_worker_client.h`
- `infrastructure/detection_worker_client.cpp`
- `reference/SS-DE-RPO/_delivery/agents/D/` 下本目录五份说明
