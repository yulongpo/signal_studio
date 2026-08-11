# MS-4.5 参数模型说明

## 1. 契约边界

分析参数由 `signal::dsp::AnalysisSettingsSnapshot` 承载，当前 schema 为
`signal.analysis-settings/1.0`，算法版本为 `signal.dsp.analysis/1.0`。公共契约只使用
C++20 与 Signal Studio 自有类型，不暴露 Qt、oneMKL、CUDA、Eigen 或 TBB 类型。

显示参数由应用层 `AnalysisDisplaySettings` 独立保存。参考电平、动态范围、色表、
插值和频率轴显示方式只改变 Visualization 映射，不进入应用 DSP 缓存键，也不触发
FFT、PSD 或 STFT 重算。公共 `SpectrumAnalysisSettings::frequency_reference` 不是显示
提示：absolute 会改变 SignalDSP 输出的 `frequency_hz`，因此进入公共参数哈希并分类为
`spectrum_transform`。Signal Studio 应用层在所有设置入口和分析调用处把该字段规范化为
baseband；Qt 频率轴控件只读写 `AnalysisDisplaySettings::frequency_axis_mode`。

## 2. 频谱与 PSD 参数

`SpectrumAnalysisSettings` 包含：

- 数据覆盖：首个完整帧或全部完整帧；
- 帧长、FFT 长度和显式补零策略；
- Rectangular、Hann、Hamming、Blackman、Blackman-Harris、Flat Top、Kaiser、
  Tukey 窗；Kaiser 使用 Beta，Tukey 使用 Alpha；
- 单边或 `fftshift` 双边频谱；应用结果固定为基带坐标，绝对频率由显示映射派生；
- 幅度、功率、PSD 的线性或 dB 输出；
- coherent-gain、window-power 或不归一化；
- 不去趋势或去均值；
- Periodogram 或 Welch，包含 Welch 重叠和段数；
- 无累积、线性平均、指数平均或最大保持；
- 无平滑、滑动平均、高斯或 Savitzky-Golay 平滑；
- 测量使用原始结果或平滑结果。

帧长和 FFT 长度为 0 时采用受当前已读范围约束的自适应值。FFT 长度小于帧长非法；
FFT 长度大于帧长时必须显式启用补零。实信号单边谱和复信号移位双边谱分别校验。

## 3. STFT 参数

`SpectrogramAnalysisSettings` 独立保存帧长、FFT 长度、hop、窗函数、频谱方向、
边界策略、补零、去趋势、输出量、归一化和二维平滑。重叠率不重复持久化，而由
`1 - hop/frameLength` 唯一派生，避免两个来源不一致。

频率维平滑为高斯核；时间维平滑为指数递推。`StftResult` 同时保存原始线性矩阵、
原始显示矩阵和最终显示矩阵，因此纯平滑变化可复用原始 STFT，不重新执行 FFT。

## 4. 分析前滤波

`AnalysisPrefilterSettings` 保存启用状态、不可变 `ProcessingChain` 快照、边界策略、
后端标识和群时延。滤波系数、节点顺序、旁路状态和实现版本都进入参数序列化与哈希。
执行复用 MS-02 的 oneMKL VSL/LAPACKE Adapter；本里程碑没有实现通道创建、DDC、
重采样输出或 MS-05 处理链业务。

## 5. 序列化、哈希和兼容

`serialize_analysis_settings()` 使用固定字段顺序和稳定数值文本。参数哈希是该规范化
文本的 SHA-256，文本形式为 `sha256:<64 hex>`。同一参数快照的序列化和哈希可确定性
往返；未知的同主版本可选字段忽略，但同主版本中损坏的枚举、FFT/frame/hop 关系、
补零、归一化、边界值和 display 内容明确拒绝；settings/display 的未来主版本均明确
拒绝。工程打开先在候选状态解析全部必需分析扩展，任何失败都不替换当前工程、路径、
参数或显示状态；完全缺少分析扩展的旧工程仍迁移到默认值。

工程扩展键：

- `signal.analysis-settings`：当前 DSP 参数快照；
- `signal.analysis-display`：当前显示参数；
- `signal.analysis-user-preset.<name>`：用户预设；
- `signal.analysis-scope`：当前工程视图作用域；
- `signal.analysis-active-preset`：当前预设引用；
- `signal.analysis-active-preset-hash`：当前预设的参数哈希。

内置预设为快速预览、平衡分析、高分辨率、低噪声 PSD、突发信号和窄带精细分析。
预设依据实际已读样本数选择有界的 2 次幂参数，并显示速度、频率/时间分辨率和噪声
方差的权衡。

## 6. 校验和资源估计

校验覆盖枚举已知值、窗参数范围、奇数核长、Savitzky-Golay 阶数、Welch 重叠/段数、
指数系数、hop、单边谱适用性、处理链和输入样本数。非法参数在启动后台任务前被阻止。

`estimate_analysis_cost()` 返回实际采用的帧长/FFT、频点数、Welch 段数、STFT
行列数、FFT 次数、主机/设备内存、估算运算量、频点间隔、RBW 和时间步长。超过预算
时不允许分配无界矩阵。

## 7. 最小失效

| 变化 | 失效范围 |
|---|---|
| 仅频谱平滑 | 复用原始频谱/PSD，只重做平滑 |
| 仅 STFT 平滑 | 复用原始 STFT，只重做二维平滑 |
| 仅测量来源 raw/smoothed | 复用原始与平滑结果，不重做 FFT |
| 公共 DSP `frequency_reference` baseband/absolute | 频谱/PSD 变换；参数哈希不同 |
| 频谱帧长、FFT、窗、估计或累积 | 仅频谱/PSD 变换 |
| STFT 帧长、FFT、hop、窗或边界 | 仅 STFT 变换 |
| 分析前滤波 | 滤波及全部下游频谱、PSD、STFT |
| 仅显示映射 | 不失效 DSP 结果 |

Signal Studio 的 baseband/absolute UI 轴属于最后一行的显示映射，不会写入上一行的公共
DSP 参数；切换后点击“应用”仍不提交任务。

完整缓存身份包含源指纹、已读范围、信号描述符、算法版本、规范化参数哈希和实际
CPU/CUDA 后端；平滑专用复用不会跨越源版本、预滤波或变换参数边界。跨请求最大保持
还必须匹配 project generation、source version、实际 backend、device 与 backend
policy，并校验结果来源和 FFT provenance 自洽，禁止跨源、跨工程代际或
oneMKL/cuFFT 混合。`AnalysisBundle::source_range` 保留当前请求范围；
`contributing_source_ranges` 以 begin/end 确定性排序、精确去重。transform cache 条目只
保存当前请求基线与单一范围，命中后才按当前已提交状态合并，聚合结果不回写 cache；
不兼容切换和 `hold_reset_generation` 变化后只保留当前范围，Artifact metadata 以
`sourceRanges` 输出。lineage 不代替完整 maximum-hold 状态身份。
