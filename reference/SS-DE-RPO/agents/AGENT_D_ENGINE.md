# SS-DE-RPO · 子代理 D：检测执行引擎与首推理延迟

你是独立 Task Engine Owner。任务资料 `reference/SS-DE-RPO/`，阅读 `03_AUDIT_BASELINE.md`、`04_RESOURCE_GOVERNANCE.md`、`05_STARTUP_AND_PIPELINE.md`，并读取主代理冻结的 `_delivery/M1_CONTRACT.md`。

## 独占代码所有权

`infrastructure/detection_task.*`、`infrastructure/detection_source.*`、`infrastructure/detection_worker_client.*`、`infrastructure/detection_model_snapshot.*`。不要直接改 C 的 UI、E 的 publication 或 F 的 backend；接口需求由主代理协调。

## 必须实施

1. 真实追踪 startRequest/runStartCommit/finishStartCommit/pump/execute 的每项异步操作：Plan、模型/数据来源快照、资源预检、manifest 和工程提交、Worker QProcess、首读 IQ、DSP、模型输入、首批 TensorRT 交换、decode、track、checkpoint、refine、durable publication。
2. 建立同一 runId 的端到端单调时钟，准确区分 `click`、`queued`、`worker ready`、`first infer`、`first visible result`、`durable completed`。不要把历史 `phase=none/interrupted` 作为新的点击时间。
3. 优化重复 SourcePlan、重复哈希、模型资产快照、工程对象复制、Worker 冷启动、batch-to-batch 非必要同步和 checkpoint 频率。缓存必须依据文件/asset/profile/config 变化失效，不牺牲身份/事务安全。
4. 对接 A 的统一 ResourceGrant、F 的合法 TensorRT profile/Batch 和 C 的异步 UI 状态事件；自己不引入新暗藏硬上限。增量窗口 tracking/preview 与 durable final publication 不得混为一谈。
5. 读→预处理→GPU 推理→decode 的队列应有上限和背压，序号/物理频率/时间窗严格对应；OOM/磁盘不足/取消/退出后有明确终态、资源释放和可用日志。
6. 核对最新 `preserveDurableModelEvidence()` 后 E 的 recovery/compare 结果；不得跳过校验。

## 验收与交付

R-04/05/06/08～12/14/17/18。输出 `start-chain-callgraph.md`、`run-state-telemetry.json`（或结构等效）、`startup-perf.md`、`interface-requests.md`、`patch-summary.md`、`agent-status.md`、worktree commit、测试和性能日志路径。GPU 不在当前硬件上可用则如实标识。
