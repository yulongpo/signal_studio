# SS-DE-RPO · 交付、审核与发布门禁

## Gate 0 — 路径/版本核对

确认工作目录为 Signal Studio 仓库根目录；任务文件真实存在于 `reference/SS-DE-RPO/`。核对当前远端 `rebuild/cursor` HEAD 和本地 working tree，保护未提交内容，记录 baseline。若参考资料中的 HEAD 不同，以最新源码为准重新确认问题。

## Gate 1 — 多代理真实性与契约

至少 A～G 七个开发/测试子代理在独立 worktree/branch 执行，H 独立终审；有明确唯一写入文件归属，契约写在 `reference/SS-DE-RPO/_delivery/M1_CONTRACT.md`。缺失真实 spawn，不得标为完整满足本任务。

## Gate 2 — 代码质量与 P0

资源上下限语义一致、UI 防重入可靠、publication 真实完成、UX-R6 已复验；有最小复现/回归证据。禁止只抬阈值、跳过校验、无声截断或假成功。检查 Qt 主线程/跨进程边界、算术溢出、取消和事务持久化。

## Gate 3 — 编译及运行

使用仓库既有 CMake/VS2022/MSVC/Qt 工具链（实际版本先检测）；执行适用的 Debug/Release 编译、CTest/QtTest、真实 IQ 与 FFSCN/SCN 模型。没有本机 GPU/模型的项标 `BLOCKED/UNVERIFIED_GPU`，不能报告硬件通过；同时必须完成可执行的其它测试。

## Gate 4 — 性能证据

冷/热各 3 次、同设备同配置基线 vs 新版本，完整 `T_click→T_first_infer→T_durable_publication` 与峰值 RAM/VRAM/Disk，改动是否改善/退化有结论，主要性能风险有数据。启动 UI <100ms p95 是目标，不是未经测量的事实。

## Gate 5 — 独立 H 终审

H 单独读取最终 diff 和运行日志，并按 `templates/INDEPENDENT_REVIEW.md` 交付报告 `PASS/CONDITIONAL/FAIL`。H 为只读审查者。存在 P0_BLOCKER 时必须修复复审；CONDITIONAL 应列出不满足的验收和原因，未经门禁不能宣称全部完成。

## Gate 6 — GitHub 同步

所有必需 P0 通过、代码集成并经 H 审核以后，由主代理再次核对远端 HEAD；如有更新先安全合并，禁止强推和覆盖未提交改动。以非强制方式推送 `rebuild/cursor`，保留完整任务交付报告、提交链接、已完成/受阻测试。若无推送权限或发生冲突，保留本地阶段性提交并报告准确原因，不能假称成功同步。

## 最终报告路径

- 统一文字报告：`reference/SS-DE-RPO/_delivery/FINAL_DELIVERY.md`
- Agent 交接证据：`reference/SS-DE-RPO/_delivery/agents/`
- 独立终审：`reference/SS-DE-RPO/_delivery/INDEPENDENT_REVIEW.md`
- 性能摘要：`reference/SS-DE-RPO/_delivery/performance/`
- 大体积诊断、真实模型、IQ 输入：仓库现有忽略目录或外部安全位置，只在报告中引用，勿纳入任务包或直接提交 Git。
