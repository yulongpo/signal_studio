# SignalDSP C++ API

## 公共边界

头文件：

- `signal_studio/dsp/analysis.hpp`
- `signal_studio/dsp/browse_performance.hpp`
- `signal_studio/dsp/pipeline.hpp`

公共类型只使用 C++20 标准库、SignalData、SignalCompute 和 SignalCore 契约。oneMKL 与 CUDA 类型不会出现在安装头文件中。

## 分析 API

- `IFftBackend`：校验并执行复数正向/逆向 FFT。
- `make_cpu_fft_backend()`：创建批准的 CPU FFT；当前私有实现为 oneMKL DFTI，缺失时返回 unavailable。
- `make_cuda_fft_backend()`：创建实机 CUDA/cuFFT 后端。
- `make_auto_fft_backend()`：优先 CUDA，并在创建或执行失败时显式降级 CPU。
- `make_window()`：Rectangular、Hann、Hamming、Blackman、Blackman-Harris、
  Flat Top、Kaiser、Tukey 窗及 coherent gain/ENBW。
- `AnalysisSettingsSnapshot`：版本化频谱、PSD、STFT 与分析前滤波参数。
- `validate_analysis_settings()`：按输入描述符、已读范围和实/复类型校验。
- `serialize_analysis_settings()` / `parse_analysis_settings()`：确定性参数往返与主版本兼容。
- `hash_analysis_settings()`：规范化参数 SHA-256。
- `estimate_analysis_cost()`：频点、段数、STFT 行列、FFT 次数、内存、运算量、RBW。
- `classify_analysis_change()`：区分频谱平滑、STFT 平滑、两类变换和预滤波失效。
- `calculate_spectrum()`：单边或移位双边的幅度/功率/PSD 参数化频谱。
- `calculate_psd()`：Periodogram/Welch、线性/指数平均、最大保持和三种平滑。
- `calculate_stft()`：时间行、频率列的参数化有界 STFT 与二维平滑。
- `resmooth_spectrum()` / `resmooth_psd()` / `resmooth_stft()`：复用原始线性结果。
- `apply_analysis_prefilter()`：通过既有 ProcessingChain 执行分析前滤波。
- 时间/样本与频率/bin 映射函数：执行边界和 Nyquist 校验。

`SpectrumResult`、`PsdResult` 和 `StftResult` 保存原始线性值、原始显示值、最终显示值、
参数哈希、帧/FFT/hop、bin spacing、RBW、预滤波标志和实际后端来源。纯显示映射不在
本 API 内执行。

## 大文件浏览性能 API

- `LogicalRecordingSource`：使用真实录制文件与完整描述符生成源指纹和缓存身份，并以有界物理窗口映射逻辑文件长度。
- `BrowsePerformanceSession`：生成采样概览、频谱/STFT 三图帧并恢复兼容缓存。
- `BrowseInteractionSequencer`：只发布当前代际的完整三图结果，拒绝过时代际。
- `build_full_sc16_index()`：以有界块完整扫描 little-endian interleaved SC16 文件，生成正式时域索引并支持取消。

用户指定的十进制 100 GB 验证使用真实 X310 文件的逻辑重复映射；接口不会创建物理 100 GB 文件，也不把逻辑映射结果冒充为物理 100 GB 吞吐。

## 处理链 API

`ProcessingChain` 提供追加、删除、旁路、复制、重排、预设和不可变快照。`process_chain()` 执行去直流、增益、IQ 校正、解析信号/频移、FIR、IIR 和有理重采样；`FilterState` 保存 FIR/IIR 历史、重采样相位和原始处理样本计数。

`preview_node()` 返回节点前后样本、频率响应和群时延。模板导入/导出包含完整节点契约、模式、系数、比率和参数；`serialize_processing_provenance()` 固定源指纹、范围、数据源版本、节点、链版本和后端。

## 线程与错误

FFT 计划缓存可并发访问，同一计划执行受互斥保护。耗时调用由上层 TaskRuntime 调度；后台线程不得触碰 `QWidget`。所有失败通过 `core::Result`/`Status` 返回，禁止静默切换后端或返回伪结果。
