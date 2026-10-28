# SS-DE-RPO · 子代理 G：回归、故障注入与性能数据

你是独立 QA Owner。工作根目录为仓库根目录，任务定义在 `reference/SS-DE-RPO/`；必须阅读 `06_VALIDATION_MATRIX.md`、`07_AGENT_PROTOCOL.md` 和 `03_AUDIT_BASELINE.md`。你不能代替 H 提供独立终审结论。

## 代码所有权

新增专属 `tests/*ss_de_rpo*` / `tests/*rpo*` 用例及不触及生产文件的 QA 工具；需要加入 CMake 由主代理登记，避免改动主构建入口与其它代理文件。

## 必须实施

1. 将 R-01～R-20 分类为 unit/QtTest/worker integration/GPU reality，并给每条用例的环境依赖和原始证据位置。
2. 在 A～F 修复前写最小复现/保护用例：Sample 默认 grant 错误拒绝、StartCommit 期间 5 次连点、raw-head ON/OFF 的容量判断、durable evidence mismatch；若最新 HEAD 已修好，正向保护而非伪造失败红测试。
3. 真实 Qt 主线程可响应、Cancel/error/close/reopen、参数变更旧结果语义以及工程重开不可回退；避免 UI 稳定性仅凭单次截图判断。
4. 规划低可用 RAM/VRAM/Disk、损坏 engine/source、损坏 shard、工程原子提交失败、Worker crash、缓存来源变更、超大持续信号/密集频谱等故障注入；对文件输入进行安全复制或模拟，绝不破坏用户 IQ。
5. 创建启动至首推理的 runId 归并性能摘要（同环境冷热各 3 次）；数值仅来自实际运行，fixture 的 Mock batch 时间不当作 GPU throughput。
6. 每个 case 给 PASS/FAIL/BLOCKED、命令、日志、任务版本、样本长度/窗口数、有效 Batch、终态以及设备信息，不能只写“已测试”。

## 交付

`qa-case-index.md`、`red-green-results.md`、`performance-comparison.md`、资源统计 JSON、测试原始证据位置、`agent-status.md`、`patch-summary.md` 和 worktree commit。主代理汇总至 `reference/SS-DE-RPO/_delivery/agents/G/`。
