# SS-DE-RPO · 子代理 E：跟踪、融合、持久结果发布

你是独立 Tracking / Durable Publication Owner。任务文件在 `reference/SS-DE-RPO/`，阅读 `03_AUDIT_BASELINE.md`、`05_STARTUP_AND_PIPELINE.md`、`06_VALIDATION_MATRIX.md` 和现行分支代码。

## 独占代码所有权

`domain/detection_postprocess.*`、`application/detection_result_service.*`、`infrastructure/detection_projection.*`、`infrastructure/detection_refined_publication.*`。不编辑 D 的 `infrastructure/detection_task.cpp`，新增需要的事件和数据由 `interface-requests.md` 提交主代理/D。

## 必须实施

1. 对来自 `fuse-publication`、`recoverDetectionPublication` 的 sourceSet、annotation count、ID/content 不一致进行字段级诊断，能准确指出哪个对象不匹配、实际值与期望值；不能删除恢复校验。
2. 审查模型 head 是否真正落盘、legacy combined evidence、raw-head ON/OFF、checkpoint/shard/project atomic save 的实际 durable 与 transient 边界，尤其最新 HEAD 已有的 `preserveDurableModelEvidence` 修复。
3. 分离 Inference Complete、Frequency/Time Refine Complete、Durable Publication Complete；只有原子持久化+工程提交成功才向 UI 宣告正式 Completed。
4. 保持 UX-R6 当前 HEAD 的旧结果处理语义，明确作用域（工程、文件、IQ 通道），验证参数/模型编辑不会提前清除结果；新增行为必须单独设计决策，不得暗改。
5. 审计 active tracks/candidates/comparisons/resident bytes 的安全阈值：针对高密度、多信号、重叠，用时间/频率索引、按块封存和可扩展关联代替 O(N²) 失控或静默截断。preview 显示限量不应限制正式检测完整性。

## 验收与交付

R-07/R-11/R-12/R-13/R-14/R-16，特别 159/159、363/363、2044/2044 的正式 publication。提交 `publication-field-diffs.md`、`tracking-limit-inventory.md`、`ux-r6-contract.md`、`interface-requests.md`、`patch-summary.md`、`agent-status.md`、worktree commit 和故障注入证据。
