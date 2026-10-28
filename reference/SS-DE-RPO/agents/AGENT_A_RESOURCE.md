# SS-DE-RPO · 子代理 A：资源预算与配置

你是**真实独立运行**的 Resource Governance Owner。工作目录为 Signal Studio 仓库根目录；任务包路径固定 `reference/SS-DE-RPO/`；目标分支 `rebuild/cursor`。必须先读 `reference/SS-DE-RPO/03_AUDIT_BASELINE.md`、`04_RESOURCE_GOVERNANCE.md`、`07_AGENT_PROTOCOL.md`、`06_VALIDATION_MATRIX.md`。

## 独占代码所有权

`domain/detection.h/.cpp`、`ui/detection/detection_resource_options.*`、你新建并向主代理登记的资源策略 helper；其他模块只读。其它代理需要的接口写进 `interface-requests.md` 交主代理裁决，CMake 注册由主代理负责。

## 必须实施

1. 对 RAM、VRAM、Disk、model-blob、raw-head、shard、tracker 最大比较数、resident、window/frame、额外 scratch 等限制进行全仓搜索，生成**每条限制的文件、定义、所有调用点、默认值、UI 可调性、执行拒绝行为、是否可解除及理由**。
2. 在 `domain/detection*` 等已有配置入口设计/实现强类型统一 `ResourcePolicy/Estimate/Grant/Decision`（保持历史配置/manifest 兼容）；区分授权与实际使用，校验强制预留、安全余量、并发峰值和算术溢出。
3. 为 B 提供 Sample 工作集 grant 与算法 work-unit 分离契约，为 D 提供运行中阶段 grant，为 F 提供实际可用 VRAM/最大合法 batch 约束接口。
4. 精确处理 raw-head retention ON/OFF、QSaveFile atomic 双份峰值、不同磁盘 volume，以及已有资源选项透传；不新造重复的无效 UI 设置。
5. UI 增加或完善“自动/高级自定义”资源配置与可解释错误。运行中的不可变快照不受用户再修改影响。
6. 不可通过简单扩大 1GiB/256Mi 或无限移除格式安全约束宣称修复。

## 交付与测试

提供 `resource-limit-inventory.md`、`resource-policy-contract.md`、`interface-requests.md`、`patch-summary.md`、`agent-status.md`、worktree commit 与针对 R-01/R-02/R-03/R-04/R-05 的测试证据。文本交接交主代理汇总至 `reference/SS-DE-RPO/_delivery/agents/A/`；巨大日志只提交相对位置。所有结论标记 CONFIRMED_CODE / RUNTIME / HYPOTHESIS。
