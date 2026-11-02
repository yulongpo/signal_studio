# Signal Studio UX-R6 任务包

将此目录解压到项目根目录的 `reference/ux-r6/`，然后将 `CODEX_MASTER_PROMPT.md` 交给 Codex 主代理读取执行；主任务目标和约束见 `PLAN.md`，验收项见 `ACCEPTANCE_MATRIX.md`。

本任务包仅为源码分析和执行规划；没有修改本地仓库，也没有运行用户真实 IQ 样本或做 GPU benchmark。Github 审查 HEAD 为 `ec511662`，执行时必须确认最新分支。输入 65536/帧 0.25ms/约180窗口是用户观察，根因待复现。不得把截图所示症状解释为已证实的 CUDA OOM。

主要交付文档应归档至 `docs/implementation/ux-r6/`；本目录保持为规划参考源。注意仓库当前 `.gitignore` 对 `/reference/*.md` 有忽略规则，请使用明确路径并检查可跟踪状态。须保护用户 `reference/` 其它资料。
