# MS-4.5 开发报告

MS-4.5 将 MS-04 固定参数分析扩展为可复现的参数闭环。SignalDSP 提供第三方无关的
参数快照、窗口/估计策略、平滑、来源和失效分类；ApplicationController 编排
TaskRuntime、两级缓存、工程代际、迁移与 Artifact；Qt Designer 面板只负责编辑和绑定，
信号处理不在 GUI 线程执行。

实现复用 MS-02 的 oneMKL DFTI/VSL/LAPACKE、cuFFT、libsamplerate 和
ProcessingChain，复用 MS-03 的 Visualization/Workbench/ViewRequestId，复用 MS-04 的
工程、导入、任务和结果中心。没有增加第三方库，也没有重新实现成熟 FFT、滤波、
重采样、卷积或线性代数内核。

Spectrum/PSD 共享 FFT；纯平滑复用 raw；显示与测量来源变化不误失效 DSP；预滤波变化
才失效全部下游。最大保持只在兼容工程代际、源版本、实际后端/设备/策略间累积，缓存
始终保存单请求基线，旧 key 不会复活历史峰值。

正式任务使用 `cancellable -> canceling/committing -> finalized` 单调状态机。提交在最新
视图许可下更新工程设置、提交 Artifact/Workspace，并把三个真实 Artifact 文件原子登记
到 TaskRuntime 后才密封完成；失败回滚结果，重启时清理未完成或无法证明完整提交的正式
结果。后台线程不接触 QWidget。

最终验证为四套 CPU/CUDA 专项各 24/24、CPU Debug/Release 全量各 267/267。详细测试、
截图、矩阵、规格和质量证据见本目录。MS-05 未进入。
