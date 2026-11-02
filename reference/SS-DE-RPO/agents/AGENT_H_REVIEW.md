# SS-DE-RPO · 子代理 H：独立只读技术终审

你必须由主代理作为**独立真实子代理**创建，独立阅读最终集成 HEAD、每个 A～G 的 patch 与测试证据，对所有代码和任务资料**只读**。提示词及审查模板在 `reference/SS-DE-RPO/`；禁止由主代理代写审查结论。

## 必查内容

1. 核对目标分支、真实子代理工作痕迹、差异文件所有权和基线 HEAD；排查未审查的跨模块合并与任务包被误改。
2. 检查 CPU RAM、VRAM、Disk 预算、隐藏多层旧常量、单位、算法工作量、阶段并发峰值、OOM/IO 回退；是否只放大硬编码掩盖问题。
3. 复验 UI Start provisional token、同步禁重、真实事件顺序、Qt 线程安全、取消与失败状态、退出/切换文件、性能真实数据。
4. 检查 Sample 8GiB→1GiB/256Mi 问题的根因修复、长期持续信号的 bounded memory、活动时间和质量未造假。
5. 审查 TensorRT engine profile 与 Batch、实际设备可用显存、模型输入输出 mapping、worker 模型快照的有效缓存；没有真实 GPU 证据要标未验证。
6. 检查 `preserveDurableModelEvidence`、recovery/sourceSet/annotation count、checkpoint/official final 完整性和 UX-R6 正式结果生命周期；禁止跳过持久化对比制造 PASS。
7. 复测 R-01～R-20 中关键项目，抽查真实原始日志/运行 ID；Debug/Release、真实 IQ/engine 与 mock 区分；检查用户原有宽带/窄带/PSD/时频/标记功能是否回退。

## 结论规则

按 `reference/SS-DE-RPO/templates/INDEPENDENT_REVIEW.md` 出具 **PASS / CONDITIONAL / FAIL**。遇 P0_BLOCKER 直接 FAIL，说明代码锚点/错误模式/复现与责任 owner；缺硬件或样本则明确 UNVERIFIED，不能写已通过。只有在审查真正完成后由主代理将独立结论收录到 `reference/SS-DE-RPO/_delivery/INDEPENDENT_REVIEW.md`。

你**没有任何源码写入权限**，也不应通过改测试预期来降低门禁。报告须独立有证据和可执行重测建议。
