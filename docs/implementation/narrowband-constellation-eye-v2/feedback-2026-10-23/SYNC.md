# 2026-10-23 分支同步说明

用户在反馈验收后明确要求“完成远端同步，并合并到 main”，随后指定“先合并到 rebuild/a143-native 分支，然后再合并到 main 分支”。本轮提交和推送采用这次授权；原 W8 报告的“不推送”描述保留为当时节点事实。

同步前，窄带功能分支为 `feature/narrowband-sampling-constellation-eye`，已提交 W0–W8 的最新节点为 `62739d9`；`origin/rebuild/a143-native` 为 `17471a6`，`origin/main` 为 `b00b9e0`。远端 main 的文件树与共同源提交 `4904740` 一致，重建分支已有后续 UI 和窄带开发。正常合并保留两边历史，不使用替换文件树或强制推送。

交付顺序为：

1. 独立复审本轮 DSP、Qt 交互和交付证据（见 [REVIEW.md](REVIEW.md)），只提交任务相关源码、测试、脚本、文档与必要截图。
2. 提交并同步窄带功能分支，将其快进合入 `rebuild/a143-native`，先同步该远端分支。
3. 将更新后的 `rebuild/a143-native` 正常合入 `main`，保留 main 原有提交历史，再同步 main。
4. 核对远端三分支指向、祖先关系及合并前后文件树；合并不改变已验收内容时复用已有验证，不重复执行相同构建和测试。

验证依据为 [FIX_REPORT.md](FIX_REPORT.md) 和 [evidence/SUMMARY.json](evidence/SUMMARY.json)：两配置相关 CTest 各 6/6、共享交互回归各 4/4、窄带 Qt 各 11/11、新调制族正例各 50/50、原生 GPU/软件共 28 场景及四种独立启动。原 W8 的两配置完整回归另存原目录，不将本轮定向结果表述为全量 37 项。

原有 `.vscode/`、任务包 ZIP、`reference/`、`tasks/`、`resources/sigmf_data_demo/` 及未跟踪的宽带 UX 文档不纳入本次修复提交，不删除、移动或覆盖。最终实际 Git 提交身份与远端核对结果以交付时的 Git 输出为准。
