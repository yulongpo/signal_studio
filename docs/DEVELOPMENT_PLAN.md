# Signal Studio 开发计划

完整顺序记录在 `docs/superpowers/plans/2026-07-22-signal-studio-full-development.md`。工程按一次一个可验证里程碑、一次一个可审计提交推进。

| 里程碑 | 范围 | 状态 |
|---|---|---|
| MS-00 | 净空仓库、不可变基线、依赖/工具链契约、十模块 CMake 平台 | 本地全量自检及第三轮远程三项 CI 门禁通过；等待独立规范复审再次核查 |
| MS-01 | Core、Data、TaskRuntime 功能基础 | 未开始 |
| MS-02 | DSP 与 Compute 后端 | 未开始 |
| MS-03 | Visualization 与 Workbench | 未开始 |
| MS-04 | Signal Studio 基础应用 | 未开始 |
| MS-05 | 宽窄带联动分析 | 未开始 |
| MS-06 | PluginSDK、ModelRuntime、Dataset 功能 | 未开始 |
| MS-07 | 工程化、打包、文档 | 未开始 |
| MS-08 | 复用证明应用 | 未开始 |
| MS-09 | 最终发布验证与发布 | 未开始 |

修正后的 MS-00 本地自检覆盖：BL1.0 无抛出 C ABI 与异常适配器、构造期结构化 Status 不变量、独立无 Qt 构建与组件包、包括已批准 vcpkg `.tar.gz` 在内的精确依赖/离线缓存契约、同进程工具链幂等、确定性且路径唯一的本机用户预设、便携配置、C/C++ SDK 示例、API 类型隔离、十个模块性能保护，以及 Debug/Release 各 41 个用例。

远程运行 `29919175820` 已针对提交 `d41c2748a465c4e843617e0a9444c8f8cc2f5015` 完成验证：Ubuntu 与 Windows 无界面作业、Qt 6.10.3 Windows UI 模块/性能作业均通过。前两轮失败证据继续作为旧提交历史保留，当前远程验收状态以第三轮成功结果为准。独立规范复审仍须再次核查；本项目记录不代表代码质量评审或产品负责人验收，也不启动 MS-01。
