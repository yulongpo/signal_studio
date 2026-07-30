# MS-4.5 计划

## 目标

在已验收的 MS-04 与尚未开始的 MS-05 之间，交付可复现的频谱、PSD 和 STFT
参数化闭环。所有参数必须真实进入 DSP 计算、缓存身份、工程持久化和 Artifact
来源链；Qt 只负责编辑与显示，不在主线程执行 FFT、滤波或平滑。

## 权威与边界

1. 不修改 `docs/baseline/Signal-Studio-Dev-Docs/**`。
2. 复用 MS-02 的 oneMKL DFTI/VSL/LAPACKE、cuFFT、libsamplerate、SignalCompute
   选择/降级和 SignalDSP ProcessingChain。
3. 复用 MS-03 的 DisplayMapping、AtomicFrameCoordinator、VisibilityController
   和 Workbench Inspector。
4. 复用 MS-04 的工程、导入、TaskRuntime、Artifact 与原生 Qt 页面。
5. 不实现 MS-05 的 Selection 建通道、DDC、重采样输出、多通道继承或联动业务。
6. 不实现 MS-06～MS-09 的插件、模型、数据集、正式打包、第二应用或发布。

## 依赖选型

现有依赖能够完成本里程碑，不新增第三方库：

- FFT 和计划缓存：oneMKL DFTI / cuFFT；
- FIR/IIR 和卷积：现有 ProcessingChain 与 oneMKL VSL/LAPACKE Adapter；
- 重采样：本里程碑不新增业务，已有 libsamplerate 保持可复用；
- 参数编排、单位换算、缓存键和哈希：项目薄层；
- 简单移动平均、窗参数和核构造：低风险薄层，实际卷积继续进入既有成熟内核。

公共头继续禁止 Qt、oneMKL、CUDA、Eigen、TBB 和其他第三方类型泄漏。

## 实施顺序

1. 参数契约、校验、规范化序列化、哈希、代价估计和兼容默认值。
2. 扩展窗函数、Periodogram/Welch、平均/保持、频谱/STFT 平滑和补零。
3. 通过 ProcessingChain 接入分析前滤波，保留原始未平滑结果。
4. ApplicationController 接入两级缓存、TaskRuntime、ViewRequestId 和最新提交。
5. Qt Designer 分析设置面板接入 Inspector，显式应用并显示派生信息。
6. 工程扩展字段保存/迁移、用户预设、Artifact 参数来源和旧结果过期。
7. DSP、应用、Qt、高 DPI、CPU/CUDA、性能、稳定性和安装消费者验证。
8. 生成真实截图与证据，完成规格/质量复审，修复全部 Critical/Important。
9. 创建一个 MS-4.5 提交并推送当前分支，然后停止。

## 完成门禁

以根目录 MS-4.5 任务文档第 16 节为准。任何未运行的可选环境矩阵必须明确记录，
不得写成通过。
