# SS-DE-RPO · 主代理启动提示词（可直接粘贴）

你现在工作在 Signal Studio 仓库**根目录**，本次执行 **Signal Studio 检测引擎可靠性与性能优化（SS-DE-RPO）**，目标为 `yulongpo/signal_studio` 仓库的 `rebuild/cursor` 分支。

任务包已经解压到 **`reference/SS-DE-RPO/`**，不得擅自改为 `references/`、移动任务目录或要求我重新提供任务内容。

**请立即完整读取并执行 `reference/SS-DE-RPO/01_MASTER_AGENT_PROMPT.md`，并按该文件要求继续读取同目录全部核心文档及 `agents/` 子代理指令。**

务必真正创建并调度 **A～H 共 8 个子代理**协同完成资源治理、Sample 精修、启动 UI、任务引擎、publication、GPU/TensorRT、QA 回归和独立终审；使用独立 worktree/分支与唯一文件所有权。完成代码修复、测试、性能对照和阶段性提交，而不是只给出建议或计划。

执行前获取当前 `rebuild/cursor` HEAD 并核对本地未提交改动；与旧审查 HEAD 不一致时基于**最新代码**复核，不覆盖现有更改。禁止强制推送，不擅自改变 UX-R6 既有正式结果生命周期、不通过调大常量或放弃校验伪修复、不盲改 TensorRT Batch 2→16。重要证据与最终中文交付报告统一放在 `reference/SS-DE-RPO/_delivery/`，耗费大量空间的日志/缓存存于构建或其他忽略目录。

主代理必须持续推进 M0～M6，可阶段性提交；最终由独立子代理 H 审核，关闭 P0 阻断项后再按目标分支规定非强制同步远端。**无需我再逐阶段确认；若缺失真实 GPU 或模型则如实标识未验证，继续完成可独立执行的部分。**
