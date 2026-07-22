# MS-00 提交记录

| 字段 | 值 |
|---|---|
| 分支 | `codex/full-signal-studio-development` |
| MS-00 前基点 | `c205122` |
| 里程碑提交标识 | 主题 `chore(ms-00): establish verified Signal Studio development baseline` |
| 提交方式 | 显式暂存和全部验证后执行 `git commit --amend --no-edit` |
| 归档分支/标签 | `archive/pre-signal-studio-dev-20260722-145422` / `pre-signal-studio-dev-20260722-145422` |
| 第一轮已推送修订 | `3b9598c6fab995e13c3c1a563db611b60fe2f817`；触发远程运行 `29915975454` |
| 第二轮已推送修订 | `1d16dfe29c03d74458185d96438313c7db5f96b8`；触发远程运行 `29918020386` |
| 最低版本修正提交 | `d41c2748a465c4e843617e0a9444c8f8cc2f5015`；触发远程运行 `29919175820`，三项作业全部成功 |
| 本次证据提交 | 主题 `docs(ms-00): 记录远程 CI 验收结果`；不修改上述被测实现提交 |
| 质量修复提交 | `a1c252f873a01fb6ae3a7b0b9e1f60553341b171`；主题 `fix(ms-00): 提升工具链与公共契约稳健性`；作为 `18d2eeb` 之上的新提交创建，不 amend |
| 质量修复远程验证 | GitHub Actions 运行 `29924612586`；三个目标作业全部成功 |
| 独立复审收口提交 | 主题 `docs(ms-00): 完成里程碑独立复审`；只修改中文文档与证据 |

里程碑实现提交曾通过 amend 收敛，当前被测实现哈希已固定为表中值。本次独立证据提交不在自身文档中写入自引用哈希；创建后使用以下命令解析：

```powershell
git rev-parse HEAD
git show -s --format="%H %s" HEAD
```

最低版本修正提交的本地门禁：

- 同一进程全新 Debug 与 Release 配置、clean build、各 41/41；
- 两配置中的无 Qt 八模块包测试均通过；
- 两次初始化保持 PATH 和用户预设哈希不变；
- Python/PowerShell 依赖校验器解析并匹配 BL1.0 `.tar.gz` 获取脚本；
- 工作流 YAML、MSVC 初始化顺序和 Qt ABI 静态校验通过；
- PowerShell 7 与 Windows PowerShell 5.1 重复生成用户预设，字节和 SHA-256 完全一致；
- 公共头文件、路径、基线、数据验证通过；
- 明确审计暂存差异并执行 `git diff --cached --check`；
- 工作树在 amend 后保持干净。

第三轮远程 GitHub CI 运行 `29919175820` 针对最低版本修正提交执行，状态为 `completed`、结论为 `success`。Windows UI 模块/性能作业与 Windows、Ubuntu 两项无界面作业全部成功。前两轮失败结论仍作为各自旧提交的历史证据保留，并由第三轮成功结果取代为当前远程验收状态。

本次文档提交只补充并同步已发生的远程证据，不改变提交 `d41c2748a465c4e843617e0a9444c8f8cc2f5015` 的实现内容。其提交哈希在创建后通过 `git rev-parse HEAD` 解析，不在提交自身文档中写入自引用哈希。

质量修复提交的本地门禁为 Debug/Release 全新配置各 45/45、VS Code 本机 Debug 树 45/45 及明确 F5 目标直接启动、三种依赖模式和注入式负向测试、Windows 根路径回归、CI/VS Code 静态校验、基线/外部数据/公共头文件/安装包消费。精确对应的远程运行 `29924612586` 已成功，最终独立规格与代码质量复审均通过。MS-00 已验收关闭，下一里程碑为 MS-01。
