# MS-4.5 频谱数值定义

## 1. 频率轴与帧时间

频点间隔为 `Δf = Fs / Nfft`。复信号双边结果按 `fftshift` 排列；实信号单边结果保留
DC 至 Nyquist，并按单边能量规则处理非 DC/Nyquist 项。绝对频率模式在基带频率上
加已确认的中心频率；中心频率缺失时保持基带，不猜测。

STFT 时间戳固定为帧中心：

`t[row] = (sourceBeginSample + row * hopLength + (FrameLength - 1) / 2) / Fs`

不完整尾帧按参数选择丢弃或补零。补零只增加频率采样密度，不声称提高由窗和有效帧长
决定的真实分辨率。

## 2. 窗和带宽

窗的 coherent gain 与等效噪声带宽为：

`CG = sum(w[n]) / N`

`ENBW_bins = N * sum(w[n]^2) / sum(w[n])^2`

`ENBW_Hz = ENBW_bins * Fs / FrameLength`

幅度输出使用 coherent-gain 归一化，PSD 使用窗功率和采样率归一化。界面同时显示
`Δf` 和 RBW/ENBW，避免把 bin spacing 与分辨率带宽混为一项。

## 3. 输出量

- 线性幅度：窗归一化后的复频谱模；
- `dBFS` 幅度：`20 * log10(max(amplitude, floor))`；
- 线性功率：幅度平方；
- `dBFS` 功率：`10 * log10(max(power, floor))`；
- 线性功率谱密度：按采样率和窗功率归一化，单位 `1/Hz`；
- `dBFS/Hz`：`10 * log10(max(powerDensity, floor))`。

内部平均、Welch、最大保持和指数累积均在线性功率域完成，再转换为 dB；不在 dB
显示值上直接平均。

## 4. Periodogram、Welch 和累积

Periodogram 使用一个选定完整帧。Welch 按配置重叠切分有界段，逐段计算线性功率
密度后平均，段数为 0 时使用可用的全部完整段。累积策略定义如下：

- 线性平均：所选段的算术平均；
- 指数平均：`y[k] = α*x[k] + (1-α)*y[k-1]`；
- 最大保持：逐 bin 取最大线性值，并由 `hold_reset_generation` 显式复位；
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
容差；不以“非空”作为数值通过条件。后端来源和实际设备写入每个结果。
