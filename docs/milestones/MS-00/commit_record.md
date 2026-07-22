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
| 当前修正状态 | 本地执行 amend 后由根任务推送；本记录不嵌入 amend 后自引用哈希 |

定义提交自身的可变哈希不嵌入该提交。amend 后使用以下命令解析：

```powershell
git rev-parse HEAD
git show -s --format="%H %s" HEAD
```

本轮 amend 前门禁：

- 同一进程全新 Debug 与 Release 配置、clean build、各 41/41；
- 两配置中的无 Qt 八模块包测试均通过；
- 两次初始化保持 PATH 和用户预设哈希不变；
- Python/PowerShell 依赖校验器解析并匹配 BL1.0 `.tar.gz` 获取脚本；
- 工作流 YAML、MSVC 初始化顺序和 Qt ABI 静态校验通过；
- PowerShell 7 与 Windows PowerShell 5.1 重复生成用户预设，字节和 SHA-256 完全一致；
- 公共头文件、路径、基线、数据验证通过；
- 明确审计暂存差异并执行 `git diff --cached --check`；
- 工作树在 amend 后保持干净。

第二轮远程 GitHub CI 结论仍为失败，但 Ubuntu 与 Windows 无界面作业均已通过；Qt 作业成功安装 6.10.3 并初始化 MSVC，只在 Visualization 的旧 6.11 静态断言处失败。当前最低版本修正提交只有在根任务推送并产生新运行后才能更新远程结论。
