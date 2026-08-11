# MS-4.5 完成报告

## 1. 完成结论

MS-4.5 已完成频谱与时频分析参数化闭环。规范化参数快照从 Qt Designer 面板进入
TaskRuntime、SignalDSP、缓存、工程扩展和 Artifact 来源；FFT/PSD/STFT、窗、估计、
平均/保持、平滑和分析前滤波均真实影响结果。

最终代码四套 CPU/CUDA Debug/Release 专项均为 24/24，CPU Debug/Release 全量均为
267/267。30 分钟既有稳定性证据保留且遵照用户要求不重跑。MS-05 未进入。

## 2. 完成功能

- `signal.analysis-settings/1.0` 参数模型、校验、规范化序列化、SHA-256 和代价估计；
- Spectrum、PSD、STFT 独立 frame/FFT/hop、补零、单/双边和归一化；
- Rectangular、Hann、Hamming、Blackman、Blackman-Harris、Flat Top、Kaiser、Tukey；
- Periodogram、Welch、线性/指数平均和跨请求最大保持；
- 移动平均、高斯、Savitzky-Golay 频谱平滑及 STFT 频率/时间平滑；
- ProcessingChain 分析前滤波、群时延和有界异步预览；
- Qt Designer 基础/高级面板、内置预设、派生信息和显示映射；
- 取消、最新 `ViewRequestId`、工程代际、旧结果拒绝和最小缓存失效；
- 工程保存、旧工程迁移、用户预设、未来主版本拒绝和事务失败恢复；
- Artifact 参数哈希、算法、后端/设备/精度、源范围、任务与贡献范围追溯。

## 3. 架构与可靠性

FFT 继续使用 oneMKL DFTI/cuFFT，卷积/FIR 使用 VSL，IIR 和 Savitzky-Golay 求解使用
LAPACKE，重采样继续使用 libsamplerate。没有新增依赖或复制成熟 DSP 内核。

正式分析任务只在最新视图许可仍有效、Artifact/Workspace 已提交、三个真实 Artifact
文件已登记并校验后原子完成。取消与提交使用单调 CAS 状态机；任务完成后取消不能回写
状态。重启会核验文件大小/SHA-256，并清理未完成或无法证明完整提交的正式结果。

## 4. 最终证据

| 门禁 | 结果 |
|---|---:|
| CPU Debug 全量 | 267/267，411.54 秒 |
| CPU Release 全量 | 267/267，284.80 秒 |
| CPU Debug MS-4.5 | 24/24，70.41 秒 |
| CPU Release MS-4.5 | 24/24，12.79 秒 |
| CUDA Debug MS-4.5 | 24/24，39.55 秒 |
| CUDA Release MS-4.5 | 24/24，12.29 秒 |

截图为 2880×1620、845,143 字节，SHA-256
`aeba1a628cf68c8cee0e1b6574ec92b3f62534a69d89ae074bae7cbc252b6387`。追踪矩阵为
68,341 字节，SHA-256
`26f0f9a01bb75f2cdd466ccdc1752c5cee3ddc2d9e20292eafb423317a8a735e`。

## 5. 复审与边界

首轮独立规格/代码质量复审的所有 Important 反馈已整改。最终快照的独立复审重跑因代理
额度限制未返回结论；主代理完成逐项规格和代码质量复审，未发现剩余 Critical/Important。
该环境限制已如实记录。

MS-05～MS-09 规划未改变。本里程碑未实现 Selection 建通道、DDC、重采样输出、
多通道继承、插件、模型、数据集、发布或第二应用。推送后立即停止。
