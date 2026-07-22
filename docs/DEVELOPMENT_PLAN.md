# Signal Studio 开发计划

完整顺序记录在 `docs/superpowers/plans/2026-07-22-signal-studio-full-development.md`。工程按一次一个可验证里程碑、一次一个可审计提交推进。

| 里程碑 | 范围 | 状态 |
|---|---|---|
| MS-00 | 净空仓库、不可变基线、依赖/工具链契约、十模块 CMake 平台 | 第二轮远程无界面作业已通过，剩余 Qt 最低版本守卫已修复并完成本地全量自检；等待再次远程重跑及独立规范复审 |
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

远程运行 `29918020386` 的 Ubuntu 与 Windows 无界面作业均通过，Qt 作业成功安装 Qt 6.10.3 并初始化 MSVC，仅在 Visualization 遗留的 Qt 6.11 静态断言处失败。Visualization/Workbench、CMake、包配置、本机发现、依赖锁和测试现统一最低支持版本 6.10.3；新的远程结果必须等待根任务推送并实际重跑。本项目记录不能替代独立复审，也不启动 MS-01。
