# MS-4.5 频谱数值定义

## 1. 频率轴与帧时间

频点间隔为 `Δf = Fs / Nfft`。复信号双边结果按 `fftshift` 排列；实信号单边结果保留
DC 至 Nyquist，并按单边能量规则处理非 DC/Nyquist 项。公共 SignalDSP 参数化 API 在
`frequency_reference=baseband` 时输出基带坐标，在 `absolute` 时把调用方提供的中心频率
加入 `frequency_hz`；两者具有不同参数哈希，变化分类为 `spectrum_transform`。
Signal Studio 应用层则把 DSP/缓存设置固定规范化为 baseband，绝对频率只由显示映射在
绑定图帧时加已确认的中心频率，中心频率缺失时保持基带，不猜测。界面切换频率轴以及
切换后点击“应用”均不提交 FFT/PSD/STFT 任务。

STFT 时间戳固定为帧中心：

`t[row] = (sourceBeginSample + row * hopLength + (FrameLength - 1) / 2) / Fs`

不完整尾帧按参数选择丢弃或补零。补零只增加频率采样密度，不声称提高由窗和有效帧长
决定的真实分辨率。

## 2. 窗和带宽

窗的 coherent gain 与等效噪声带宽为：

`CG = sum(w[n]) / N`

`ENBW_bins = N * sum(w[n]^2) / sum(w[n])^2`

`ENBW_Hz = ENBW_bins * Fs / FrameLength`

幅度输出通常使用 coherent-gain 归一化，PSD 通常使用窗功率和采样率归一化。两类输出
也允许显式选择 `None`：此时保留未除窗增益/窗功率的 FFT 线性尺度；功率密度仍除以
采样率以保留 `/Hz` 维度，但不再是 full-scale 校准 PSD。界面同时显示 `Δf` 和
RBW/ENBW，避免把 bin spacing 与分辨率带宽混为一项。

实信号单边幅度采用 RMS 口径：除 DC 和偶数 FFT 的 Nyquist bin 外，双边幅度折叠后
乘 `sqrt(2)`；因此峰值为 `A` 的整 bin 余弦显示为 `A/sqrt(2)`。单边功率/PSD 的相同
非端点 bin 乘 2，DC/Nyquist 不翻倍，以保持 Parseval 能量一致。复信号只允许移位双边。

## 3. 输出量

- 线性幅度：窗归一化后的复频谱模；
- `dBFS` 幅度：`20 * log10(max(amplitude, floor))`；
- 线性功率：幅度平方；
- `dBFS` 功率：`10 * log10(max(power, floor))`；
- 线性功率谱密度：按采样率和窗功率归一化，单位 `1/Hz`；
- `dBFS/Hz`：`10 * log10(max(powerDensity, floor))`。

上述 `dBFS`/`FS` 单位只适用于批准的 coherent-gain 或 window-power 归一化。选择
`None` 时，同一输出枚举保持线性/对数编码方式，但单位必须改为：

| 输出 | `None` 的真实单位 |
|---|---|
| 对数幅度 | `dB(re 1 raw FFT amplitude unit)` |
| 对数功率 | `dB(re 1 raw FFT power unit)` |
| 对数功率密度 | `dB(re 1 raw FFT power unit/Hz)` |
| 线性幅度 | `raw FFT amplitude unit` |
| 线性功率 | `raw FFT power unit` |
| 线性功率密度 | `raw FFT power unit/Hz` |

结果对象携带实际 `normalization`；Visualization 元数据与 Artifact 的 peak/mean 单位
均从结果的输出量和归一化联合推导。Artifact 测量值使用所选 raw/smoothed 输出量，
不会把线性值改写成 dB，也不会把 raw FFT 值标为 `dBFS` 或 `dBFS/Hz`。

内部平均、Welch、最大保持和指数累积均在线性功率域完成，再转换为 dB；不在 dB
显示值上直接平均。

## 4. Periodogram、Welch 和累积

Periodogram 使用一个选定完整帧。Welch 按配置重叠切分有界段，逐段计算线性功率
密度后平均，段数为 0 时使用可用的全部完整段。累积策略定义如下：

- 线性平均：所选段的算术平均；
- 指数平均：`y[k] = α*x[k] + (1-α)*y[k-1]`；
- 最大保持：先在当前时间视窗帧内逐 bin 取最大线性值，再对同一源、变换参数、实际
  project generation、source version、backend、device、backend policy 和
  `hold_reset_generation` 的连续分析请求继续逐 bin 保持；改变 generation 或任何执行/
  数据来源身份都会重新从当前请求开始。transform cache 只保存当前请求内的未跨请求
  聚合基线；命中后仅在当前已提交 `previous_analysis` 兼容时临时合并，不把合并结果写回
  cache。backend/policy 或 source 切换后再返回旧 key 必须恢复该请求基线，不能复活切换
  前的峰值；oneMKL 与 cuFFT 不得混合。Bundle 的 `source_range` 只表示当前请求，
  `contributing_source_ranges` 按 begin/end 排序并精确去重，记录所有真正贡献的范围；
  Artifact 以 `sourceRanges=[[begin,end],...]` 完整保存该 lineage。lineage 不作为不完整的
  状态键；generation reset 清空会话历史并从当前单一范围重新开始；
- 无累积：使用估计器的当前结果。

## 5. 平滑与测量

频谱平滑包含滑动平均、高斯和 Savitzky-Golay。Savitzky-Golay 系数通过 oneMKL
LAPACKE 求解，卷积通过既有 oneMKL VSL Adapter 执行。STFT 频率维使用高斯平滑，
时间维使用指数递推。频率平滑边界采用端点延拓后执行完整归一化核，避免端点因缺少
邻点被无条件压低。

每个结果保留 `raw_linear_values`、原始显示值和最终显示值。默认测量来源为 raw；
用户选择 smoothed 时结果来源会写入参数快照和哈希。

## 6. 分析前滤波与群时延

分析前滤波先对当前有界输入执行不可变 ProcessingChain，再将同一滤波快照送入频谱、
PSD 和 STFT。结果标记 `prefilter_applied`，并保存处理链、边界、后端和群时延。
FIR/IIR 的数值与群时延由 MS-02 成熟内核和预览接口提供，不在应用层复制实现。

## 7. 后端一致性与容差

CPU 使用 oneMKL DFTI；CUDA 使用 cuFFT。专项测试比较频率轴、幅度、PSD、Welch、
STFT、平滑前结果和预滤波结果。解析信号使用绝对/相对容差，dB 结果使用明确的 dB
容差；不以“非空”作为数值通过条件。后端来源、实际设备和 FFT 精度写入每个结果。
Welch、线性/指数平均、最大保持和 STFT 只允许聚合完整 provenance 一致的帧，比较范围
包括 requested/actual backend、backend id、device、version、`complex-float64` precision、
degraded/fallback reason 和 consistency 状态。Signal Studio 同一双视图请求还要求
Spectrum/PSD 与 STFT 来源一致；不一致时整项失败，不发布部分或混合结果。
