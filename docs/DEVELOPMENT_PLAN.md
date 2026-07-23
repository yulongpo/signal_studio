# Signal Studio 开发计划

完整顺序记录在 `docs/superpowers/plans/2026-07-22-signal-studio-full-development.md`。工程按一次一个可验证里程碑、一次一个可审计提交推进。

| 里程碑 | 范围 | 状态 |
|---|---|---|
| MS-00 | 净空仓库、不可变基线、依赖/工具链契约、十模块 CMake 平台 | 已验收关闭 |
| MS-01 | Core、Data、TaskRuntime 功能基础 | 已验收关闭 |
| MS-02 | DSP 与 Compute 后端 | 已验收关闭 |
| MS-03 | Visualization 与 Workbench | 已验收关闭 |
| MS-04 | Signal Studio 基础应用 | 进行中 |
| MS-05 | 宽窄带联动分析 | 未开始 |
| MS-06 | PluginSDK、ModelRuntime、Dataset 功能 | 未开始 |
| MS-07 | 工程化、打包、文档 | 未开始 |
| MS-08 | 复用证明应用 | 未开始 |
| MS-09 | 最终发布验证与发布 | 未开始 |

修正后的 MS-00 本地自检覆盖：BL1.0 无抛出 C ABI 与异常适配器、构造期结构化 Status 不变量及满容量传播、公共枚举已知值校验、独立无 Qt 构建与组件包、精确获取/兼容宿主/精确快照三层依赖契约、同进程工具链幂等、Windows 根路径语义、确定性本机用户预设、VS Code 同构建树 F5、C/C++ SDK 示例、API 类型隔离、十个模块性能保护，以及 Debug/Release 各 45 个用例。

远程运行 `29924612586` 已针对质量修复精确提交 `a1c252f873a01fb6ae3a7b0b9e1f60553341b171` 完成历史验证；当时的 Ubuntu/Windows 无界面作业和 Qt 6.10.3 Windows UI 作业全部成功。最终独立规格与代码质量复审通过，MS-00 已验收关闭。按后续批准的验证策略，自 MS-01 起不再执行 Ubuntu 24.04 无界面构建测试，持续集成门禁收敛为 Windows 2022 无界面与 Windows Qt/UI 两项。

MS-01 在 MS-00 平台骨架上交付 SignalCore（统一 `Result`/`Status`）、SignalData（实/复数容器、RAW 全标量与布局、WAV、分块有界读取、有界预览、选区导出、多分辨率索引缓存）与 SignalTaskRuntime（有界资源池、优先级、DAG、暂停/恢复/取消、进度、超时、重试、幂等、视图过期、结构化失败、来源过滤、历史与制品恢复），覆盖 54 项批准需求。无界面 Debug/Release（101/101）与 UI Debug/Release（106/106）四个配置全量 CTest 通过，合计 414/414，需求映射 54/54。MS-01 已验收关闭。

MS-02 交付 SignalCompute（`IComputeBackend`/`IBufferPool`/`IBackendSelector`，CPU+CUDA 探测、预算约束、自动选择与显式降级）与 SignalDSP（窗函数、时域统计、IQ 度量、FIR 滤波、多相重采样、`IFftBackend` 适配器+cuFFT Z2Z GPU 后端、Welch PSD dB/Hz、STFT）。遵守 ADR-006/ADR-009，公共头无第三方类型，依赖 DAG 不变。无界面 Debug/Release（118/118）与 UI Debug/Release（CUDA，130/130）四配置全量 CTest 通过，合计 496/496。oneMKL CPU FFT 后端因本机未安装为已记录环境偏差（不伪造），cuFFT GPU 后端经解析信号验证。MS-02 已验收关闭。

MS-03 交付 SignalVisualization（`IDataSeries`/`ViewportController`/`IChartView`+5 个真实 QPainter 控件：时域/功率谱/瀑布图/星座图/眼图/`OverlayModel`/`ColorScale`/`TimeNavigator`，隐藏停止计算、频率单位自适应+Hz 级精度）与 SignalWorkbench（`IServiceRegistry`/`PanelFactory`/`ICommandRegistry`/`ConfigurableDiagnosticsProvider`）。公共头无 QWidget（`native_widget()->void*`），Qt 私有链接，依赖 DAG 不变。无界面 118/118、UI(CUDA) 142/142 四配置全量 CTest 通过，合计 520/520。MS-03 已验收关闭，下一里程碑为 MS-04（Signal Studio 基础应用）；整体开发计划尚未达到产品最终验收条件。
