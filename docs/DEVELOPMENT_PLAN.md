# Signal Studio 开发计划

完整顺序记录在 `docs/superpowers/plans/2026-07-22-signal-studio-full-development.md`。工程按一次一个可验证里程碑、一次一个可审计提交推进。

| 里程碑 | 范围 | 状态 |
|---|---|---|
| MS-00 | 净空仓库、不可变基线、依赖/工具链契约、十模块 CMake 平台 | 已验收关闭 |
| MS-01 | Core、Data、TaskRuntime 功能基础 | 已验收关闭 |
| MS-02 | DSP 与 Compute 后端 | 已验收关闭 |
| MS-03 | Visualization 与 Workbench | 已验收关闭 |
| MS-04 | Signal Studio 基础应用 | 未开始 |
| MS-05 | 宽窄带联动分析 | 未开始 |
| MS-06 | PluginSDK、ModelRuntime、Dataset 功能 | 未开始 |
| MS-07 | 工程化、打包、文档 | 未开始 |
| MS-08 | 复用证明应用 | 未开始 |
| MS-09 | 最终发布验证与发布 | 未开始 |

修正后的 MS-00 本地自检覆盖：BL1.0 无抛出 C ABI 与异常适配器、构造期结构化 Status 不变量及满容量传播、公共枚举已知值校验、独立无 Qt 构建与组件包、精确获取/兼容宿主/精确快照三层依赖契约、同进程工具链幂等、Windows 根路径语义、确定性本机用户预设、VS Code 同构建树 F5、C/C++ SDK 示例、API 类型隔离、十个模块性能保护，以及 Debug/Release 各 45 个用例。

远程运行 `29924612586` 已针对质量修复精确提交 `a1c252f873a01fb6ae3a7b0b9e1f60553341b171` 完成历史验证；当时的 Ubuntu/Windows 无界面作业和 Qt 6.10.3 Windows UI 作业全部成功。最终独立规格与代码质量复审通过，MS-00 已验收关闭。按后续批准的验证策略，自 MS-01 起不再执行 Ubuntu 24.04 无界面构建测试，持续集成门禁收敛为 Windows 2022 无界面与 Windows Qt/UI 两项。

MS-01 已实现 54 项 Core、Data 与 TaskRuntime 需求；UI、无 Qt 和强制 CPU 的 Debug/Release 六套全新本地矩阵以及安装消费者均已通过。独立规格复审与最终代码质量复审通过且无剩余 Critical/Important。实现提交 `39f1d0f2ae9b2cc063543cbdbe69bc3ddd388fd2` 经远程整合提交 `c89412e615168b067f3f29646e778b6de5c8b1b5` 推送，Windows GitHub Actions 运行 `30187026089` 的无界面与 Qt/UI 两项作业均成功；生成构建树已清理，工作树在证据提交前保持干净。MS-01 已验收关闭，下一里程碑为 MS-02；整体开发计划尚未达到产品最终验收条件。

MS-02 已完成 DSP 处理链、oneMKL DFTI/VSL/LAPACKE CPU 适配器、libsamplerate 重采样、可选 cuFFT 适配器、统一 Compute 探测/选择/降级、预算内存池、真实缓存/索引性能基准和外部录制集成验证。无界面 Debug/Release 全量各 133/133；CPU/CUDA、Debug/Release 的 MS-02 确定性矩阵各 34/34，公共头各 1/1；CUDA Release 独立 GPU 基准、CPU/CUDA 洁净 PATH 闭包及 CPU Debug、CPU Release、CUDA Release 的 UI/无 Qt 安装消费者各 2/2。纯顺序读取基线下，真实 X310 全文件 Release 索引/同盘顺序读取比为 0.727307；十进制 100 GB 仅使用逻辑重复映射，并覆盖重复拼接、EOF 和读取上限边界，没有创建物理大文件。独立整改复审无剩余 Critical/Important。实现提交 `f6041d719ec6be9b47eee21eb04addc2a0265704` 已推送，Windows GitHub Actions 运行 `30331185758` 的无界面与 Qt/UI 两项作业均成功；MS-02 已验收关闭，下一里程碑为 MS-03。

MS-03 已完成 Visualization/Workbench 的纯 C++20 公共契约、真实 Qt Widgets 图谱与工作台、六个生产 Qt Designer `.ui` 文件、四组尺寸/DPI 预览、默认 Windows 平台运行时闭包和安装消费。50/50 项批准需求通过；CPU Debug/Release 全量各 198/198，CUDA Debug/Release 针对性集合各 59/59，Windows 无界面 Debug/Release 全量各 133/133。公共头、基线、依赖锁、VS Code、Windows-only CI 静态校验、格式和静态分析均通过；本机用户预设连续生成哈希一致且路径重复项为 0。实现提交 `a4d3a763eece76c966a3763b5831cccc98baee84` 已推送，Windows GitHub Actions 运行 `30350430444` 的无界面与 Qt/UI 两项作业均为 success。MS-03 已验收关闭；按用户指令，当前暂停并等待确认，未确认前不进入 MS-04。

2026-07-29 对 MS-03 追加批准 HTML 标准截图对齐和高 DPI 整改。P02 恢复应用菜单、左侧导航、折叠 Inspector、底部状态区和批准三图比例；P04/P07 增加完整原生中心页面。预览矩阵扩展到 1280×720、1080P、4K、100%、125%、150%、175%、200% 及 Windows 当前 DPI。CPU/CUDA Debug/Release 均重新编译并通过 63/63 项 MS-03 CTest 和 1/1 安装消费者。明显 P0/P1 差异已关闭，仍停留在 MS-03，未进入 MS-04。
