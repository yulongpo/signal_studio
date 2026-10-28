# SS-DE-RPO · 子代理 B：Sample 时间精修与分块优化

你是独立 Sample Refinement Owner。任务资料路径 `reference/SS-DE-RPO/`，必须读 `03_AUDIT_BASELINE.md`、`04_RESOURCE_GOVERNANCE.md` 和主代理生成的 `_delivery/M1_CONTRACT.md`。按主代理确认的资源接口实施。

## 独占代码所有权

`infrastructure/detection_time_analysis.*`、`infrastructure/detection_sample_stream*`、独立 Sample scratch/helper；`domain/detection_refinement*` 如需更改必须获主代理单独授权。**禁止直接编辑 `infrastructure/detection_task.cpp` 或 A 的资源配置文件。**

## 必须实施

1. 实证审计 `workingSetBudget*3/4 → workingCellBudget=working/12` 及 `sample_stream::contextValid` 的 `1 GiB/256 MiB` 上限关系。区分实际申请峰值、工作单元数量、内部硬安全检查；大内存授权不能成为拒绝小需求的理由。
2. 将错误拆分为具体无效输入/资源不足/算法超规模/来源异常等可追溯 errorCode 和字段；合法小工作集在 8GiB 授权下可执行，低内存可有界分块/适当降级。
3. 长持续信号：重复 Sample range 的 IQ 读取、FIR、energy、quantile、scratch 和缓存分块优化，避免全段大 buffer 与重复读取；保留强制的时间精修质量/数值精度检查。
4. 如无法精确精修，只能在真实频率证据允许的条件下报告有标记的 coarse fallback，不能伪造时间活动结果；外部文件/模型错误不得 catch-all 隐藏。
5. 提供每候选的输入采样数、块峰值、工作量、FIR 模式、回退原因、耗时日志，与 D 的 runId 时间线关联。

## 验收与交付

和 G 测 R-01/R-02/R-06/R-07/R-17：20/363/2044 窗口，不因错误映射的 1GiB/256Mi 上限失败；低内存仍稳健；时间坐标与质量门槛不因性能优化悄然变化。提交 `sample-root-cause.md`、`sample-perf.md`、`interface-requests.md`、`patch-summary.md`、`agent-status.md`、worktree commit 和真实可执行命令。
