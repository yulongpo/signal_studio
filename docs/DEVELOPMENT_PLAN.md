# Signal Studio 开发计划

完整顺序记录在 `docs/superpowers/plans/2026-07-22-signal-studio-full-development.md`。工程按一次一个可验证里程碑、一次一个可审计提交推进。

| 里程碑 | 范围 | 状态 |
|---|---|---|
| MS-00 | 净空仓库、不可变基线、依赖/工具链契约、十模块 CMake 平台 | 质量审查五项修复本地全量通过；新提交远程 CI 与独立复审待执行 |
| MS-01 | Core、Data、TaskRuntime 功能基础 | 未开始 |
| MS-02 | DSP 与 Compute 后端 | 未开始 |
| MS-03 | Visualization 与 Workbench | 未开始 |
| MS-04 | Signal Studio 基础应用 | 未开始 |
| MS-05 | 宽窄带联动分析 | 未开始 |
| MS-06 | PluginSDK、ModelRuntime、Dataset 功能 | 未开始 |
| MS-07 | 工程化、打包、文档 | 未开始 |
| MS-08 | 复用证明应用 | 未开始 |
| MS-09 | 最终发布验证与发布 | 未开始 |

修正后的 MS-00 本地自检覆盖：BL1.0 无抛出 C ABI 与异常适配器、构造期结构化 Status 不变量及满容量传播、公共枚举已知值校验、独立无 Qt 构建与组件包、精确获取/兼容宿主/精确快照三层依赖契约、同进程工具链幂等、Windows 根路径语义、确定性本机用户预设、VS Code 同构建树 F5、C/C++ SDK 示例、API 类型隔离、十个模块性能保护，以及 Debug/Release 各 45 个用例。

远程运行 `29919175820` 已针对旧实现提交 `d41c2748a465c4e843617e0a9444c8f8cc2f5015` 完成验证，其历史证据继续保留。本轮质量修复尚未产生对应远程运行，提交后必须重新验证 Ubuntu/Windows 无界面作业和 Qt 6.10.3 Windows UI 作业，并接受独立复审。本项目记录不代表代码质量评审或产品负责人验收，也不启动 MS-01。
