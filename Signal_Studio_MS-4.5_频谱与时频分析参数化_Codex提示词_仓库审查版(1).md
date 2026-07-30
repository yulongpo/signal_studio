# Signal Studio MS-4.5：频谱与时频分析参数化闭环
## Codex 一次性目标任务提示词（基于当前仓库审查修订版）

> 仓库：`https://github.com/yulongpo/signal_studio.git`  
> 工作分支：`codex/full-signal-studio-development`  
> 前置里程碑：MS-00～MS-04 已验收关闭  
> 后续里程碑：MS-05 宽窄带联动分析  
> 本里程碑定位：在 MS-04 与 MS-05 之间补齐频谱、PSD、STFT 参数化和可复现分析闭环  
> 建议提交：`feat(ms-4.5): parameterize spectrum and spectrogram analysis`

---

## 1. 已确认的当前仓库状态

开始工作前必须再次以当前分支 HEAD 为准复核，不能直接照抄本提示词中的文件状态。

当前审查已经确认：

1. MS-04 已交付工程、RAW/IQ/WAV 有界导入、暂停/继续/取消、基础 PSD/STFT、P01/P02/P03/P05、Inspector、结果包、安装树程序、VS Code F5、CPU/CUDA 测试和 8 小时稳态验证。
2. 当前 `ApplicationController::analyze()` 仍属于固定参数的基础分析：
   - 只分析最多 16,384 个样本；
   - PSD 固定使用 Hann 窗；
   - PSD 实际 FFT 长度等于当前输入样本数；
   - STFT 固定使用不超过 1,024 点的 FFT；
   - STFT Hop Size 固定为 FFT 长度的四分之一，即固定 75% 重叠；
   - 未提供可编辑的 FFT 长度、窗函数、PSD 估计、平均、保持和平滑参数；
   - 结果参数版本仍使用固定字符串，例如 `psd-default-v1`。
3. 当前 SignalDSP 已具备可复用基础：
   - CPU/oneMKL 与可选 CUDA/cuFFT FFT 后端；
   - 单边谱、移位双边谱；
   - 幅度谱、PSD、STFT；
   - Rectangular、Hann、Hamming、Blackman 四种窗；
   - Coherent Gain 和 ENBW；
   - FIR/IIR 低通、高通、带通、带阻和自定义系数；
   - 节点启用、旁路、重排、复制、删除、预设；
   - 节点预览、频率响应、群时延；
   - 处理链取消和下游失效接口。
4. 当前 Visualization 已有 PSD/STFT 元数据、DisplayMapping、ViewRequestId 和原子帧提交能力，但没有完整的分析参数编辑模型。
5. 当前 Inspector Qt 页面主要是结果信息和图谱容器，没有频谱与时频参数编辑控件。
6. MS-05 计划包含宽窄带联动、区域和通道创建、参数继承、通道提取、处理链、优先级和取消规则，但没有明确承诺完成 FFT 长度、窗函数、PSD 估计方法、谱线平滑和 STFT 参数编辑。
7. MS-06～MS-09 分别面向插件/模型/数据集、工程化打包、复用应用和发布，也不应承担本任务。

因此，本里程碑不是重新实现 FFT 或滤波器，而是将现有 DSP 能力扩展并真正接入：

```text
参数模型
→ 参数校验
→ 后台计算
→ 缓存键
→ 图谱显示
→ 工程持久化
→ 结果追溯
→ 自动测试
```

---

## 2. 总体目标

在不进入 MS-05 宽窄带通道业务的前提下，完成专业、完整且可复现的频谱分析参数体系。

用户必须能够分别配置：

- FFT 和频谱计算参数；
- PSD 估计与平均参数；
- STFT/时频图参数；
- 窗函数；
- 频谱结果平滑；
- 时频图平滑；
- 分析前滤波；
- 显示量程、动态范围和色阶；
- 参数预设；
- 参数作用范围。

任何控件都必须真实影响计算或显示，不允许仅修改标签、元数据或静态预览。

完成后，P02 宽带浏览和 P03 检视器中的 PSD、频谱和 STFT 应当具备专业分析软件所需的参数控制、数值解释、异步重算、项目恢复和结果追溯能力。

---

## 3. 范围边界

### 3.1 本里程碑必须完成

- 频谱、PSD、STFT 的类型化参数模型；
- FFT 长度和分析帧长度配置；
- 窗函数及其参数配置；
- Periodogram 和 Welch PSD；
- 线性平均、指数平均和最大保持；
- 频率维谱线平滑；
- STFT 时间维和频率维平滑；
- 分析前 FIR/IIR 滤波配置；
- 参数预设；
- 参数校验和计算代价提示；
- 后台异步重算、取消和最新请求提交；
- 完整参数哈希、缓存键和结果来源；
- 工程保存、恢复和向前兼容；
- UI、算法、CPU/GPU、性能和稳定性测试。

### 3.2 本里程碑不得提前实现

- 宽带 Selection 创建窄带通道的完整流程；
- DDC、重采样和通道输出业务闭环；
- 多通道联动和通道继承 UI；
- 调制识别、解调和协议解析；
- 插件、ONNX 模型或数据集工作流；
- 实时 SDR 接入；
- 正式安装包发布；
- MS-05、MS-06 或后续里程碑的完整页面。

分析前滤波只能作为“当前分析输入的可选预处理快照”接入，不得借此提前完成 MS-05 的通道创建和宽窄带处理链业务。

---

## 3.3 第三方成熟依赖优先原则

本里程碑必须遵循“成熟依赖优先、适配封装、自研最小化”的实现原则。

对于 FFT、PSD、窗函数、数字滤波、平滑、重采样、线性代数、统计处理及性能优化，应优先复用已经广泛使用、持续维护、许可证兼容、具有明确数值验证和 Windows/MSVC 支持的第三方成熟库，不得为追求表面上的“无依赖”而重新实现成熟算法。

### 3.3.1 优先复用现有依赖

首先审计仓库已经引入的依赖、适配器和锁定版本，优先复用：

- oneMKL DFTI、VSL、VML、LAPACKE；
- CUDA cuFFT 及现有 CUDA 运行时适配；
- libsamplerate；
- Eigen；
- TBB；
- 当前 vcpkg manifest 和 `dependencies/dependency-lock.json` 中已批准的其他库；
- SignalDSP 已有的 FFT、滤波、重采样、处理链和后端抽象。

已有依赖能够稳定完成需求时，不得重复引入功能重叠的新库，也不得在应用层复制算法。

### 3.3.2 可评估引入的成熟库

仅当现有依赖不能专业、可靠或高效地满足需求时，才允许评估新增第三方库。可重点评估但不限于：

- **Intel oneMKL**：FFT、向量数学、统计和数值运算；
- **cuFFT**：NVIDIA GPU FFT；
- **FFTW**：可替换 CPU FFT 后端或参考验证后端；
- **Intel IPP**：窗函数、滤波、向量处理和信号处理原语；
- **liquid-dsp**：FIR/IIR、窗函数、频谱分析和通信信号处理参考；
- **KFR**：现代 C++ SIMD DSP、FFT、滤波和窗函数；
- **DSPFilters**：成熟 IIR 设计和滤波实现；
- **Boost.Math / Boost.Accumulators**：统计、累积和数值辅助；
- **Savitzky–Golay 的成熟开源实现**：仅在许可证、维护状态和数值验证合格时采用。

以上仅为候选，不代表必须引入。Codex 必须根据现有架构、需求、维护状态、性能和许可证审计选择最合适方案。

### 3.3.3 依赖选择门禁

新增任何依赖前，必须记录并验证：

1. 项目维护活跃度和发布稳定性；
2. Windows 11、MSVC 2022、C++20 和 x64 支持；
3. Debug/Release 可构建性；
4. CPU 和可选 CUDA 架构兼容性；
5. 数值精度、边界行为和线程安全；
6. 大数据和高 FFT 点数下的性能；
7. 取消、异常和资源释放能力；
8. 开源许可证与商业发布兼容性；
9. vcpkg、CMake Config Package 或可靠源码集成方式；
10. 离线缓存、版本锁定和 SHA-256 校验能力；
11. 安装包和便携包的运行时闭包影响；
12. 是否与现有依赖功能重复；
13. 是否会把第三方具体类型泄漏到公共 API；
14. 是否存在更小、更稳定的替代方案。

无法通过上述门禁的依赖不得引入。

### 3.3.4 Adapter 隔离要求

所有第三方实现必须通过 SignalDSP 或 SignalCompute 内部 Adapter 隔离，例如：

```text
IWindowProvider
ISpectrumEstimator
ISpectrumSmoother
IFilterDesigner
IFftBackend
ISignalKernelBackend
```

具体接口名称应遵循现有工程规范，不要求机械新增所有接口。

必须保证：

- 应用层和 Qt UI 不直接调用第三方库；
- 公共头文件不暴露第三方类型；
- 第三方异常在 Adapter 边界转换为项目结构化错误；
- 后端能力、版本和降级原因可查询；
- 依赖不可用时按批准策略降级，不得静默改变算法语义；
- 单元测试能够通过统一接口比较不同后端；
- 未来替换依赖时不需要重写 UI 和应用业务。

### 3.3.5 禁止重新造轮子

除非成熟依赖确实不能满足需求，并在实施记录中给出充分证据，否则禁止自行实现：

- FFT、IFFT 或 FFT 计划管理内核；
- 高性能卷积内核；
- 通用 FIR/IIR 求解内核；
- Butterworth、Chebyshev、Elliptic 等成熟滤波器设计算法；
- 通用重采样内核；
- SIMD 向量数学基础设施；
- GPU FFT 或 GPU 滤波内核；
- 通用线性代数；
- 已有成熟实现的 Savitzky–Golay、Gaussian 等平滑算法核心；
- 与现有 oneMKL、cuFFT、libsamplerate 或 SignalDSP 重复的功能。

允许自行实现的内容应主要限于：

- 参数校验和单位换算；
- 轻量算法编排；
- 缓存键和参数哈希；
- 数据布局转换；
- Adapter；
- UI 状态管理；
- 结果元数据和来源追溯；
- 对简单滑动平均等低风险算法的薄封装，但仍应优先调用已批准的向量化原语。

### 3.3.6 参考实现与交叉验证

对新增算法或后端必须至少使用一种独立成熟实现进行数值交叉验证，例如：

- oneMKL 与 cuFFT 比较；
- 当前实现与 FFTW、SciPy 或 MATLAB 参考结果比较；
- FIR/IIR 频率响应与成熟设计库或 MATLAB/SciPy 比较；
- Savitzky–Golay 与 SciPy `savgol_filter` 的离线参考数据比较。

参考工具可以只用于测试和生成基准数据，不必成为产品运行时依赖。

### 3.3.7 依赖交付要求

若新增第三方依赖，必须同步完成：

- 更新 `vcpkg.json` 或批准的依赖管理配置；
- 更新 `dependencies/dependency-lock.json`；
- 固定精确版本或提交；
- 保存获取地址、大小和 SHA-256；
- 更新离线缓存清单；
- 更新许可证审计；
- 更新运行时 DLL 闭包；
- 更新 CMake 查找和失败提示；
- 增加独立安装消费者测试；
- 验证构建树、安装树和便携运行；
- 在 MS-4.5 实施记录中说明选型理由、未选方案和权衡。

不得通过临时下载脚本、系统全局安装或本机绝对路径形成不可复现依赖。


---

## 4. 概念必须严格区分

UI、API、文档和元数据中不得把下列概念混为同一个“滤波器”或“平滑”。

### 4.1 窗函数

窗函数用于控制有限长度 FFT 的谱泄漏，包括：

- Rectangular；
- Hann；
- Hamming；
- Blackman；
- Blackman-Harris；
- Flat Top；
- Kaiser；
- Tukey。

Kaiser 必须支持 Beta 参数，Tukey 必须支持 Alpha 参数。

每种窗函数至少提供：

- 中文名；
- 英文名；
- 参数范围；
- Coherent Gain；
- ENBW；
- 推荐用途；
- 幅度准确性和谱泄漏特点。

### 4.2 PSD 估计与统计

至少支持：

- 单段 Periodogram；
- Welch PSD；
- 无平均；
- 线性平均；
- 指数平均；
- 最大保持。

平均和保持必须定义清楚其作用对象：

- Welch 分段；
- 连续分析请求；
- 当前时间视窗内的帧。

不得使用含糊的“平均”标签。

### 4.3 谱线与时频图平滑

平滑属于结果后处理，不得伪装成提高真实分辨率。

频谱至少支持：

- 无；
- 滑动平均；
- 高斯平滑；
- Savitzky-Golay 平滑。

STFT 至少支持：

- 无；
- 频率维高斯平滑；
- 时间维指数平滑；
- 可选的时间与频率二维组合。

必须保留未平滑原始结果。峰值测量、带宽测量和导出时应明确选择使用原始结果还是显示平滑结果，默认测量原始结果。

### 4.4 分析前滤波

分析前滤波是真实 DSP 预处理，必须复用现有 ProcessingChain、NodeSpec、滤波后端和预览能力。

至少允许：

- 关闭；
- FIR 低通；
- FIR 高通；
- FIR 带通；
- FIR 带阻；
- IIR 低通；
- IIR 高通；
- IIR 带通；
- IIR 带阻；
- 自定义 FIR/IIR 系数。

不得重新实现一套与现有 SignalDSP 平行的滤波器。

必须显示：

- 截止频率或通带边界；
- 阶数；
- Q 值或其他适用参数；
- 频率响应预览；
- 群时延；
- 边界策略；
- 处理前后对比；
- 当前分析结果是否已应用滤波。

---

## 5. 类型化参数契约

在 SignalDSP 中新增或扩展 Qt 无关的版本化参数契约。可以依据现有命名规范调整名称，但至少应表达以下概念：

```cpp
WindowSpecification
SpectrumAnalysisSettings
PsdEstimatorSettings
SpectrumAccumulationSettings
SpectrumSmoothingSettings
SpectrogramAnalysisSettings
SpectrogramSmoothingSettings
AnalysisPrefilterSettings
AnalysisSettingsSnapshot
AnalysisSettingsHash
AnalysisCostEstimate
```

### 5.1 频谱参数

至少包括：

```text
analysis_range_policy
frame_length
fft_length
zero_padding_policy
window
sidedness
frequency_reference
output_quantity
normalization
detrend_policy
estimator
welch_overlap
welch_segment_count
averaging_mode
averaging_count
exponential_alpha
hold_reset_generation
frequency_smoothing
measurement_source
```

### 5.2 STFT 参数

至少包括：

```text
frame_length
fft_length
hop_length
overlap_ratio
window
boundary_policy
padding_policy
detrend_policy
output_quantity
normalization
time_smoothing
frequency_smoothing
```

`hop_length` 与 `overlap_ratio` 必须建立唯一、确定的换算规则。序列化时只保存一个主值，另一个为派生值，避免冲突。

### 5.3 显示参数

复用现有 `DisplayMapping`，并补齐必要设置：

```text
amplitude_scale
range_mode
minimum
maximum
reference_level
dynamic_range
color_map
interpolation
frequency_axis_mode
```

修改纯显示参数不得触发 FFT、PSD、STFT 或滤波重算。

### 5.4 分析前滤波

应复用现有 `NodeSpec` 或等价快照，而不是复制滤波参数定义。

分析设置快照必须包含：

- 是否启用；
- 节点规范；
- 系数或可稳定重建系数的设计参数；
- 实际后端；
- 边界策略；
- 群时延；
- 节点实现版本。

### 5.5 兼容性

- 现有枚举值不得重新编号；
- 新窗函数只能追加枚举项；
- 保留现有 `calculate_spectrum()`、`calculate_psd()` 和 `calculate_stft()` 调用兼容性；
- 可以增加新重载或新版请求对象；
- 旧 API 应转发到明确的兼容默认值；
- 公共头不得暴露 Qt、oneMKL、cuFFT、Eigen 或其他第三方类型。

---

## 6. FFT 长度和数据覆盖语义

必须消除当前固定 16,384 样本、固定 1,024 点 STFT 的限制。

### 6.1 FFT 长度

界面至少提供：

- 自动；
- 256；
- 512；
- 1,024；
- 2,048；
- 4,096；
- 8,192；
- 16,384；
- 32,768；
- 65,536；
- 131,072；
- 262,144；
- 524,288；
- 1,048,576；
- 合法自定义值。

不要求所有后端支持所有长度。必须通过后端能力校验并给出可操作错误，不得静默改成其他长度。

### 6.2 帧长与 FFT 长度

必须区分：

- 分析帧长度；
- FFT 长度；
- 补零长度。

要求：

```text
FFT 长度 >= 分析帧长度
```

当 FFT 长度大于帧长时，明确执行补零。

界面必须同时显示：

```text
频点间隔 Δf = Fs / NFFT
真实等效分辨带宽 RBW ≈ ENBW_bins × Fs / FrameLength
```

必须明确说明补零只改善频率采样密度，不提高真实分辨率。

### 6.3 数据不足

当已加载数据不足以满足配置时，禁止静默缩小 FFT。

应执行以下确定性策略之一：

1. 在资源预算允许时，通过现有数据源进行后台有界补读；
2. 用户明确启用补零时补零；
3. 返回结构化错误并说明需要的样本数和当前可用样本数。

---

## 7. PSD 与 STFT 数值正确性

必须保留并扩展现有正确性保证：

- 复数 IQ 默认移位双边谱；
- 实数信号支持单边谱；
- 复数信号不得使用会丢失负频率的一侧谱；
- 单边谱只对非 DC、非 Nyquist Bin 进行正确倍增；
- 幅度谱使用 Coherent Gain 校正；
- PSD 使用窗功率和采样率归一化；
- ENBW 和 RBW 计算正确；
- 绝对频率与基带频率切换保持同一 FFT Bin；
- CPU 和 CUDA 结果满足既有容差；
- STFT 时间戳使用明确的帧中心或帧起点规则，并在文档中固定。

至少支持输出：

- 幅度谱：dBFS；
- 功率谱：dBFS；
- PSD：dBFS/Hz；
- 线性幅度；
- 线性功率；
- 线性功率密度。

不具备物理功率标定时，不得显示 dBm 或 dBm/Hz。

---

## 8. UI 设计

在右侧 Inspector 中增加可复用的“分析设置”页面或区域，建议新增：

```text
src/platform/workbench/ui/SignalAnalysisSettingsPanel.ui
```

Qt Designer 文件必须由生产目标真实编译使用。

### 8.1 面板结构

至少包含：

```text
频谱与 PSD
时频图
分析预处理
显示
预设
```

### 8.2 基础模式

基础模式显示：

- FFT 长度；
- 窗函数；
- 输出类型；
- Periodogram/Welch；
- 平均方式；
- STFT 重叠率；
- 频谱平滑；
- 时频平滑；
- 分析滤波开关；
- 动态范围；
- 色阶。

### 8.3 高级模式

高级模式显示：

- 分析帧长度；
- 补零；
- 窗函数参数；
- Welch 分段和重叠；
- 平均次数；
- 指数平均系数；
- 平滑核大小；
- 高斯 Sigma；
- Savitzky-Golay 窗长和阶数；
- FIR/IIR 参数；
- 边界策略；
- 单边/双边策略；
- 归一化；
- 当前后端；
- 预计内存和计算量。

### 8.4 派生信息

参数变化时实时显示但不立即计算：

- Bin 数；
- 频点间隔；
- ENBW；
- RBW；
- STFT 时间步长；
- STFT 帧数；
- 预计输出矩阵大小；
- 预计主机内存；
- 预计显存；
- 性能等级；
- 参数警告。

### 8.5 操作

必须提供：

- 应用；
- 取消未应用修改；
- 恢复当前图谱默认值；
- 恢复软件默认值；
- 保存为用户预设；
- 删除用户预设；
- 应用到当前 PSD；
- 应用到当前 STFT；
- 同时应用到当前 PSD 和 STFT 的共享参数。

不得在用户每输入一个字符时立即启动昂贵计算。使用显式“应用”，可辅以 200～300 ms Debounce 进行派生信息更新。

---

## 9. 预设

至少提供以下内置预设：

- 快速预览；
- 平衡分析；
- 高分辨率；
- 低噪声 PSD；
- 突发信号；
- 窄带精细分析。

预设不得机械使用固定数值。应依据：

- 采样率；
- 当前时间视窗；
- 已加载样本数；
- 实数或复数；
- 当前 CPU/GPU 后端；
- 内存和显存预算；

生成合法的具体参数。

每个预设必须说明：

- 适用场景；
- 频率分辨率；
- 时间分辨率；
- 噪声方差；
- 计算代价；
- 是否使用平滑；
- 是否可能削弱窄峰或短突发。

---

## 10. 异步计算与最新请求提交

参数应用后，必须通过现有 TaskRuntime 和 ViewRequestId 机制执行。

流程必须为：

1. 校验参数；
2. 计算参数哈希和资源估计；
3. 发布“正在重算”状态；
4. 保留旧结果或缓存结果，避免图谱空白；
5. 取消当前同类分析任务；
6. 生成新 ViewRequestId；
7. 在工作线程执行补读、预滤波、FFT/PSD/STFT和平滑；
8. 只允许最新 ViewRequestId 原子提交；
9. 旧任务即使完成也不得覆盖新结果；
10. 成功后更新图谱、Inspector、状态栏和结果状态；
11. 失败或取消不得发布半成品。

隐藏图谱必须在既有 500 ms 门禁内停止或降低无意义计算。

---

## 11. 缓存和失效

为分析结果建立稳定缓存键，至少包含：

- 数据源版本；
- 样本范围；
- 信号描述符相关字段；
- 完整分析参数快照；
- 分析前滤波快照；
- 算法实现版本；
- CPU/CUDA 后端；
- 输出类型；
- 原始结果或平滑结果标识。

参数变化必须进行最小失效：

- 修改颜色和动态范围：仅更新显示；
- 修改插值：仅更新 STFT 显示；
- 修改平滑：复用未平滑计算结果；
- 修改 FFT、窗函数、Welch 或平均参数：重算频谱；
- 修改 STFT 帧长、Hop 或窗函数：重算 STFT；
- 修改预滤波：重算滤波及所有下游；
- 修改布局：不得触发 DSP。

缓存键不得只使用 FFT 长度。

---

## 12. 工程持久化与参数版本

新增稳定序列化格式，例如：

```text
signal.analysis-settings/1.0
```

至少保存：

- 当前分析参数；
- 参数预设引用；
- 用户预设；
- 当前图谱显示设置；
- 分析前滤波快照；
- 参数作用范围；
- 参数结构版本。

建议作用域：

```text
软件默认值
→ 用户默认值
→ 当前工程默认值
→ 当前视图实例
```

为 MS-05 预留通道继承接口，但本里程碑不实现通道创建和完整继承 UI。

旧 `.signal-workspace` 必须可以打开：

- 缺失分析参数时加载兼容默认值；
- 不修改不可变 BL1.0 文档；
- 不改变旧项目已有对象含义；
- 不支持的未来主版本必须明确拒绝；
- 同主版本新增字段应安全迁移。

---

## 13. 结果追溯

不得继续使用固定的 `psd-default-v1` 代表所有参数。

`AnalysisBundle`、Inspector 状态和 Artifact 必须保存：

- 参数结构版本；
- 规范化参数快照；
- 稳定参数哈希；
- 数据源版本；
- 样本范围；
- 采样率和中心频率；
- FFT 长度；
- 帧长度；
- 窗函数及参数；
- ENBW；
- RBW；
- PSD 估计方法；
- 平均和保持方式；
- 平滑方式；
- 分析前滤波器；
- 后端和设备；
- 算法版本；
- 原始结果或显示平滑结果；
- 计算时间；
- ViewRequestId。

参数变化后，旧结果必须标记过期，不得被静默替换。

导出频谱或测量结果时，应同时导出机器可读参数元数据。

---

## 14. 测试要求

遵循当前仓库已有测试框架、CTest 标签、CPU/CUDA 矩阵和证据格式。

### 14.1 DSP 单元测试

至少覆盖：

1. 所有窗函数的系数；
2. Coherent Gain；
3. ENBW；
4. Kaiser Beta；
5. Tukey Alpha；
6. FFT 长度改变；
7. 分析帧长度与补零；
8. Bin 对齐单音；
9. 非 Bin 对齐单音；
10. 多音；
11. 实数单边谱；
12. 复数移位双边谱；
13. 幅度谱；
14. 功率谱；
15. PSD；
16. Periodogram；
17. Welch；
18. 线性平均；
19. 指数平均；
20. 最大保持；
21. 滑动平均；
22. 高斯平滑；
23. Savitzky-Golay；
24. STFT 时间维平滑；
25. STFT 频率维平滑；
26. 滤波前后频谱；
27. FIR/IIR 群时延；
28. 取消不发布半成品；
29. 参数序列化；
30. 参数哈希稳定性。

不得只验证结果非空。必须使用可复现参考信号和明确数值容差。

### 14.2 CPU/GPU 一致性

可用时验证：

- CPU Debug；
- CPU Release；
- CUDA Debug；
- CUDA Release。

至少比较：

- 频率轴；
- 幅度谱；
- PSD；
- Welch；
- STFT；
- 平滑前结果；
- 分析前滤波结果。

若 CUDA 不可用，必须准确记录环境缺失并完整验证 CPU，不能误报 GPU 通过。

### 14.3 UI 与集成测试

至少覆盖：

- 参数面板真实加载；
- FFT 长度改变后 Bin 数和间隔改变；
- 窗函数改变后泄漏变化；
- Welch 与 Periodogram 结果差异；
- 平均、保持和平滑生效；
- 分析前滤波生效；
- 非法参数被阻止；
- 资源估计更新；
- 应用和取消；
- 预设保存和恢复；
- 工程关闭重开后恢复；
- 旧工程迁移；
- 快速连续应用时旧任务不提交；
- 隐藏视图停止计算；
- 结果参数版本正确；
- 1280×720；
- 1920×1080；
- 3840×2160；
- 125%、150%、175%、200% DPI。

### 14.4 性能和稳定性

至少验证：

- 普通参数输入和切换的 UI 响应 P95 不超过既有基线；
- 参数应用后立即保留旧结果或缓存结果；
- 取消后 500 ms 内不再发布新帧；
- 快速连续修改参数不会无限堆积任务；
- 大 FFT 或大 STFT 在执行前进行内存预算；
- 不允许因用户参数导致无界矩阵分配；
- FFT 计划能够复用；
- 30 分钟连续参数切换、缩放和视图切换无持续资源增长。

本里程碑不要求重新执行完整 8 小时稳态，但必须保留现有稳态脚本兼容性，并执行针对参数切换的稳定性专项。

---

## 15. 文档与里程碑更新

新增：

```text
docs/milestones/MS-4.5/
├─ MS-4.5_计划.md
├─ MS-4.5_实施记录.md
├─ MS-4.5_测试报告.md
├─ MS-4.5_完成报告.md
├─ MS-4.5_参数模型说明.md
├─ MS-4.5_频谱数值定义.md
├─ MS-4.5_工程迁移说明.md
├─ MS-4.5_UI差异报告.md
└─ evidence/
```

更新：

- `docs/DEVELOPMENT_PLAN.md`；
- 完整开发计划中的里程碑顺序；
- `docs/ARCHITECTURE.md`；
- `docs/DECISIONS.md`；
- `docs/CHANGELOG.md`；
- `docs/TEST_PLAN.md`；
- `docs/api/dsp.md`；
- `docs/api/visualization.md`；
- `docs/api/workbench.md`；
- 用户基础分析文档；
- UI 设计说明；
- 开发需求追踪矩阵副本。

不得修改：

```text
docs/baseline/Signal-Studio-Dev-Docs/**
```

“谱图平滑”若不属于已批准的 198 项原始需求，应作为明确的增强需求记录，例如：

```text
ENH-SPEC-001
ENH-SPEC-002
```

不得静默篡改原批准需求内容或状态。

---

## 16. 完成门禁

只有以下条件全部满足，才能标记 MS-4.5 完成：

- 当前分支状态和 MS-04 交付已重新审计；
- 已审计并优先复用成熟第三方依赖，所有新增依赖完成版本锁定、许可证、运行时闭包和 Adapter 隔离；
- 后续里程碑冲突已记录；
- FFT 长度可配置且真实生效；
- 分析帧长度和补零语义正确；
- 窗函数可配置且扩展完成；
- Periodogram 和 Welch 可用；
- 平均、指数平均和最大保持可用；
- 频谱和 STFT 平滑真实生效；
- 原始结果始终可保留；
- 分析前滤波复用现有 ProcessingChain；
- 未提前实现 MS-05 通道业务；
- 参数 UI 由真实 Qt Designer 文件构建；
- 所有参数具有校验、派生信息和资源估计；
- 分析通过 TaskRuntime 后台执行；
- ViewRequestId 最新请求提交有效；
- 缓存键包含全部数值参数；
- 显示参数变化不重算 DSP；
- 工程保存、恢复和迁移通过；
- Artifact 使用真实参数哈希；
- 旧结果正确标记过期；
- CPU 全量测试通过；
- CUDA 可用时专项测试通过；
- 安装消费者和 VS Code F5 不回归；
- 1080P、4K 和 Windows DPI 适配通过；
- 现有 MS-00～MS-04 测试无回归；
- 文档、截图、日志和提交记录完整；
- 不存在占位控件、伪结果、禁用失败测试或未说明 TODO；
- 形成独立、可审查并推送到当前开发分支的提交。

---

## 17. 执行顺序

请直接在当前分支完成任务，不要只输出方案。

1. 执行 `git fetch`、分支和工作树检查；
2. 读取 MS-02～MS-04 代码、测试和验收文档；
3. 核对 MS-05～MS-09 范围；
4. 编写 MS-4.5 计划和需求映射；
5. 审计现有第三方依赖和 Adapter，形成复用/新增依赖选型记录；
6. 先建立参数契约、序列化、哈希和兼容层；
7. 扩展窗函数、PSD、平均、保持和平滑算法；
8. 复用现有滤波处理链接入分析前预处理；
9. 改造 ApplicationController 和 AnalysisBundle；
10. 接入后台任务、缓存、取消和最新请求提交；
11. 完成 Workbench/Inspector 参数 UI；
12. 完成项目持久化和旧项目迁移；
13. 完成 Artifact 参数追溯；
14. 完成 DSP、应用、UI、CPU/GPU 和性能测试；
15. 运行 CPU Debug/Release 全量测试；
16. 在可用环境运行 CUDA Debug/Release 专项；
17. 运行安装消费者和 SignalStudio.exe 自检；
18. 生成真实 UI 截图；
19. 更新全部文档；
20. 执行规格复审和代码质量复审；
21. 修复全部 Critical 和 Important 问题；
22. 创建并推送 MS-4.5 独立提交；
23. 停止，不进入 MS-05。

不得在测试失败、存在伪控件或参数没有真实影响计算时宣告完成。
