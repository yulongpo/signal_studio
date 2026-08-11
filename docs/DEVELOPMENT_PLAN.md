# Signal Studio 开发计划

完整顺序记录在 `docs/superpowers/plans/2026-07-22-signal-studio-full-development.md`。工程按一次一个可验证里程碑、一次一个可审计提交推进。

| 里程碑 | 范围 | 状态 |
|---|---|---|
| MS-00 | 净空仓库、不可变基线、依赖/工具链契约、十模块 CMake 平台 | 已验收关闭 |
| MS-01 | Core、Data、TaskRuntime 功能基础 | 已验收关闭 |
| MS-02 | DSP 与 Compute 后端 | 已验收关闭 |
| MS-03 | Visualization 与 Workbench | 已验收关闭 |
| MS-04 | Signal Studio 基础应用 | 已验收关闭 |
| MS-4.5 | 频谱与时频分析参数化闭环 | 已验收关闭 |
| MS-05 | 宽窄带联动分析 | 未开始 |
| MS-06 | PluginSDK、ModelRuntime、Dataset 功能 | 未开始 |
| MS-07 | 工程化、打包、文档 | 未开始 |
| MS-08 | 复用证明应用 | 未开始 |
| MS-09 | 最终发布验证与发布 | 未开始 |

## 2026-08-11 后续计划校准

对当前实现、BL1.0 的 198 项需求、现有 267 项测试、依赖/预设状态和当前分支历史完成
复核。里程碑顺序不变，但 MS-05～MS-09 的内部范围已经修正，详细结论见
`docs/development/后续开发计划审查报告.md`，可执行步骤见
`docs/superpowers/plans/2026-07-22-signal-studio-full-development.md`。

- MS-05 收敛为 Selection、AnalysisChannel、既有 DSP 处理链、P02/P03 联动、Inspector
  和 AT-05/14/23，不提前实现插件、模型或调制识别。
- MS-06 分为依赖闭包、PluginSDK、ModelRuntime/算法、Dataset、宿主集成五个门禁，完整
  覆盖其 19 项批准需求；ORT CUDA 必须适配本机 CUDA 12.4，cuDNN 仅在官方运行时契约
  明确需要时安装。
- MS-07 先补齐日志、设置、诊断、安全和可维护性 19 项需求，再做质量、安装、便携、
  SBOM、许可证、文档和追踪闭环。
- MS-08 按批准基线实现 Signal Generator 薄壳与 Headless CLI，不再以 Signal Review
  作为复用证明宿主。
- 本次分析严格限定当前分支，不使用其他分支的实现、提交、报告、标签或状态调整范围。
  正式发布版本仍按总任务保持 `1.0.0` / `v1.0.0`；默认分支集成和远程状态只在 MS-09
  当前分支通过全部门禁后另行核验。
- `CMakeUserPresets.json` 当前 PATH 59 项且重复值为 0。后续每个里程碑前后保留活动
  构建树和 `.deps`，只清理已固化证据的废弃诊断、截图、安装和临时构建内容。

本次校准只更新后续计划，MS-05 仍为未开始。

修正后的 MS-00 本地自检覆盖：BL1.0 无抛出 C ABI 与异常适配器、构造期结构化 Status 不变量及满容量传播、公共枚举已知值校验、独立无 Qt 构建与组件包、精确获取/兼容宿主/精确快照三层依赖契约、同进程工具链幂等、Windows 根路径语义、确定性本机用户预设、VS Code 同构建树 F5、C/C++ SDK 示例、API 类型隔离、十个模块性能保护，以及 Debug/Release 各 45 个用例。

远程运行 `29924612586` 已针对质量修复精确提交 `a1c252f873a01fb6ae3a7b0b9e1f60553341b171` 完成历史验证；当时的 Ubuntu/Windows 无界面作业和 Qt 6.10.3 Windows UI 作业全部成功。最终独立规格与代码质量复审通过，MS-00 已验收关闭。按后续批准的验证策略，自 MS-01 起不再执行 Ubuntu 24.04 无界面构建测试，持续集成门禁收敛为 Windows 2022 无界面与 Windows Qt/UI 两项。

MS-01 已实现 54 项 Core、Data 与 TaskRuntime 需求；UI、无 Qt 和强制 CPU 的 Debug/Release 六套全新本地矩阵以及安装消费者均已通过。独立规格复审与最终代码质量复审通过且无剩余 Critical/Important。实现提交 `39f1d0f2ae9b2cc063543cbdbe69bc3ddd388fd2` 经远程整合提交 `c89412e615168b067f3f29646e778b6de5c8b1b5` 推送，Windows GitHub Actions 运行 `30187026089` 的无界面与 Qt/UI 两项作业均成功；生成构建树已清理，工作树在证据提交前保持干净。MS-01 已验收关闭，下一里程碑为 MS-02；整体开发计划尚未达到产品最终验收条件。

MS-02 已完成 DSP 处理链、oneMKL DFTI/VSL/LAPACKE CPU 适配器、libsamplerate 重采样、可选 cuFFT 适配器、统一 Compute 探测/选择/降级、预算内存池、真实缓存/索引性能基准和外部录制集成验证。无界面 Debug/Release 全量各 133/133；CPU/CUDA、Debug/Release 的 MS-02 确定性矩阵各 34/34，公共头各 1/1；CUDA Release 独立 GPU 基准、CPU/CUDA 洁净 PATH 闭包及 CPU Debug、CPU Release、CUDA Release 的 UI/无 Qt 安装消费者各 2/2。纯顺序读取基线下，真实 X310 全文件 Release 索引/同盘顺序读取比为 0.727307；十进制 100 GB 仅使用逻辑重复映射，并覆盖重复拼接、EOF 和读取上限边界，没有创建物理大文件。独立整改复审无剩余 Critical/Important。实现提交 `f6041d719ec6be9b47eee21eb04addc2a0265704` 已推送，Windows GitHub Actions 运行 `30331185758` 的无界面与 Qt/UI 两项作业均成功；MS-02 已验收关闭，下一里程碑为 MS-03。

MS-03 已完成 Visualization/Workbench 的纯 C++20 公共契约、真实 Qt Widgets 图谱与工作台、六个生产 Qt Designer `.ui` 文件、四组尺寸/DPI 预览、默认 Windows 平台运行时闭包和安装消费。50/50 项批准需求通过；CPU Debug/Release 全量各 198/198，CUDA Debug/Release 针对性集合各 59/59，Windows 无界面 Debug/Release 全量各 133/133。公共头、基线、依赖锁、VS Code、Windows-only CI 静态校验、格式和静态分析均通过；本机用户预设连续生成哈希一致且路径重复项为 0。实现提交 `a4d3a763eece76c966a3763b5831cccc98baee84` 已推送，Windows GitHub Actions 运行 `30350430444` 的无界面与 Qt/UI 两项作业均为 success。MS-03 已验收关闭；按用户指令，当前暂停并等待确认，未确认前不进入 MS-04。

2026-07-29 对 MS-03 追加批准 HTML 标准截图对齐和高 DPI 整改。P02 恢复应用菜单、左侧导航、折叠 Inspector、底部状态区和批准三图比例；P04/P07 增加完整原生中心页面。预览矩阵扩展到 1280×720、1080P、4K、100%、125%、150%、175%、200% 及 Windows 当前 DPI。CPU/CUDA Debug/Release 均重新编译并通过 63/63 项 MS-03 CTest 和 1/1 安装消费者。明显 P0/P1 差异已关闭，仍停留在 MS-03，未进入 MS-04。

MS-04 已完成工程、RAW/IQ/WAV 有界导入、暂停/继续/取消、基础 PSD/STFT、P01/P02/
P03/P05、不可变结果包、Inspector、Qt 前自检、安装树主程序和 VS Code F5 的候选实现。
20/20 项直接需求及应用/UI 回归在 CPU Debug/Release 各 243/243 全量通过，
CUDA 12.4 Debug/Release 的 MS-04 + Compute 集合各 50/50 通过；CPU 两配置安装消费者
各 2/2 通过。8 小时混合操作于 2026-07-29 12:49:59～20:50:00 连续执行 28,800 秒，
完成 70,024 次工程、导入、分析、结果和关闭循环，标准错误为 0，资源保持有界，无
崩溃或项目状态丢失。实现与环境整改提交为 `a99872e`、`e69900f`、`04932be`；最终
Windows GitHub Actions 运行 `30450012411` 的无界面和 Qt/UI 两项作业均为 success。
MS-04 已验收关闭。经 2026-07-29 指派，在 MS-04 与 MS-05 之间插入 MS-4.5，
补齐频谱、PSD、STFT、分析预滤波、参数持久化、缓存和来源追溯。MS-4.5 不修改
BL1.0 的既有里程碑编号，也不实现 Selection 建通道、DDC、重采样、调制识别、
插件/模型/数据集或发布业务；完成并验收后停止，未经新指令不进入 MS-05。

MS-4.5 已完成类型化参数、八种窗、Periodogram/Welch、平均/最大保持、频谱/STFT
平滑、分析前滤波、两级缓存、工程迁移、Artifact 来源、Qt Designer 参数面板和异步
最新提交。2026-08-11 最终代码 CPU Debug/Release 全量均为 267/267
（411.54/284.80 秒），四套 CPU/CUDA Debug/Release 专项均为 24/24
（70.41/12.79/39.55/12.29 秒）。安装/消费者、公共头、基线和依赖锁门禁包含在全量
回归并通过。30 分钟参数切换专项的历史证据为 125,061 轮、标准错误为空且资源有界；
本次遵照用户要求不重跑。没有新增第三方依赖，批准基线差异为 0。

2026-08-01 规格复审 Important 收口在原工作树上补强：损坏 settings/display 严格且事务
拒绝；`Normalization::none` 使用 raw FFT 数值/单位并贯穿显示与 Artifact；跨请求最大
保持匹配工程代际、源版本及实际 backend/device/policy，并记录完整样本范围 lineage；
公共 DSP 的 `frequency_reference` 恢复进入哈希和频谱变换失效，Signal Studio 应用仍将
DSP 坐标规范化为 baseband，UI 轴切换后点击“应用”不提交 DSP。不重跑 30 分钟专项，
不进入 MS-05。

2026-08-02 第三次规格复审把 maximum-hold 会话状态与 full-bundle transform cache 分离：
cache 只保存当前请求基线，返回前才与兼容的当前已提交结果合并，策略/后端或源版本
切换后回到旧 key 不得复活历史。该代码快照串行验收为 CPU Release 267/267（215.33
秒）、CPU Debug 267/267（353.47 秒），CPU/CUDA 四套专项均 24/24。

最终规格收口要求同一请求的所有 FFT 帧及 Spectrum/PSD/STFT 双视图完整 provenance
一致。现已增加 precision 与 backend/device/fallback 全字段门禁和确定性第 N 次切换
回归；`commit_analysis()` 在发布前拒绝混合来源。正式 Artifact 分析任务在最新视图许可
内提交设置、Workspace 和真实 Artifact 文件，并由 TaskRuntime 原子登记 payload、
`manifest.json`、`.artifact-index` 的大小/SHA-256 后密封完成；重启会清理未完成结果并
验证损坏。最终独立复审重跑受代理额度限制未返回结论，主代理完成最终规格和代码质量
复审，未发现剩余 Critical/Important。MS-4.5 完成后停止，MS-05 仍未开始。
