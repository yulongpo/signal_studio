# 计算速度库替换合同

分支 `rebuild/cursor`，基线 `3ff9a98`。2026-10-27 起产品路径把 oneMKL 固定为 1 个线程；65536×10 的 1.14 倍只属于这个短窗，不是流水线加速。R6-3 的样本流和原 WAV 完成性见 EXECUTION_STATUS，p50/p95 仍不在本文。本文件只记录本机探针、胜者和后续阶段的通过条件。

探针程序是 [tools/fft_speed_probe/main.cu](../../../tools/fft_speed_probe/main.cu)，脚本是 [scripts/probe-fft-speed.ps1](../../../scripts/probe-fft-speed.ps1)。原始行在 [evidence/fft-speed-probe/stdout.jsonl](evidence/fft-speed-probe/stdout.jsonl)。设备是 NVIDIA GeForce RTX 5060 Laptop GPU，计算能力 12.0，驱动 591.84。编译器是 CUDA 12.4 nvcc 加 Visual Studio 2019 Build Tools。计时是执行中位数，不含计划时间；GPU 执行时间来自 CUDA event。

## 下载

| 库 | 位置 | 结果 |
|---|---|---|
| FFTW 3.3.11 | `out/toolchains/fft-libs/conda`，conda-forge | 已参与计时 |
| oneMKL DFTI | 同一前缀的 `mkl` 与 `mkl-include` | 已参与计时 |
| MathDx 25.12.1 / cuFFTDx | `out/toolchains/mathdx`，CUDA 12 压缩包 | 头文件已下载。nvcc 12.4 不能编译：`dim3` 不能作为 constexpr，且头文件里的 SM 断言按更新的 CUDA 编写 |
| IPP | 未单独下载到可链接的开发包 | oneMKL DFTI 是这次实际计时的 Intel FFT |
| cuFFT | CUDA Toolkit 12.4 | 已参与计时 |

这些包在被忽略的 `out/toolchains/`，不进入 Git。

## 中位数，毫秒

手写基-2、FFTW `FFTW_MEASURE` 的执行段、oneMKL、cuFFT。`round` 是单次 H2D、执行、D2H。

| n | 手写 double | FFTW double 执行 / 计划 | MKL double 执行 / 计划 | MKL float 执行 | cuFFT Z2Z batch1 执行 / round | cuFFT Z2Z batch8 执行 / round | cuFFT C2C batch1 执行 / round |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2048 | 0.0226 | 0.0036 / 81.0 | 0.0054 / 10.7 | 0.0029 | 0.0667 / 3.88 | 0.0687 / 4.15 | 0.0484 / 3.19 |
| 8192 | 0.1128 | 0.0184 / 145 | 0.0250 / 0.85 | 0.0204 | 0.147 / 6.13 | 0.142 / 6.77 | 0.0484 / 4.25 |
| 65536 | 1.261 | 0.301 / 835 | 0.137 / 0.27 | 0.0814 | 0.134 / 6.30 | 0.360 / 11.0 | 0.0682 / 5.30 |
| 262144 | 6.441 | 1.458 / 3834 | 0.586 / 0.11 | 0.274 | 0.276 / 7.03 | 1.475 / 22.1 | 0.0855 / 4.21 |
| 10007 | 不支持 | 0.177 / 580 | 0.265 / 2.37 | 0.105 | 非 2 的幂，cuFFT 本探针未跑 |  |  |

65536 点、batch 32 的 Z2Z 执行 1.162 ms，往返 25.2 ms。主机上的 double FFT 短于这些往返时间，因此 STFT 不把 IQ 上传到 GPU。

## 胜者

- 主机上的检测 FFT，包括 float64 STFT 和 float/double 能量变换： **oneMKL DFTI**。65536 与 262144 的执行时间短于 FFTW，计划时间也短两个数量级以上。任意长度 10007 的纯执行 FFTW 略短，但一次 `FFTW_MEASURE` 计划约 580 ms，墙钟仍是 oneMKL。
- 数据已经在设备上的变换： **cuFFT**。现有周期 FIR 继续用它。cuFFTDx 这次没有可执行计时。
- 不把主机上的 IQ 上传只为了做 STFT。上表的往返时间长于 oneMKL。
- 分位：能放进设备内存的数组用 CUB。60 秒、约 9.6 GB 的 scratch 超过 8151 MiB 显存，仍走现有 CPU 基数扫描。
- 手写基-2、混合基和 Bluestein 在 oneMKL 可用时退出热路径。DLL 缺失时保留这些实现，并记录回退原因。

## 数值容差

LegacyExact 的活动段语义不变。STFT 密度与现有 CPU 帧比较，最大绝对差小于 `3.8e-5` dB；归一化张量最大绝对差小于 `5e-6`。这是现有 GPU 对照测试已经使用的门槛。能量功率与活动段继续用现有样本流测试，不放宽门限。

## 阶段通过条件

1. 本文与探针原始输出已提交。R6-3 不记为完成。
2. 一个模型窗的全部帧用一个 oneMKL 描述符完成，不再逐帧上传做 STFT。`transformBins < 2048` 仍可用现有 CPU 路径。定向测试通过上述容差。
3. 样本能量的主机 FFT 改为 oneMKL。设备上的 FIR 仍是 cuFFT。LegacyExact 函数保留为对照。
4. 设备装得下的分位选择改为 CUB，结果与现有精确分位一致。装不下的序列保持 CPU。
5. 已在设备上的 FIR/cuFFT 序列录成 CUDA Graph。重放结果与未录制路径一致。取消仍发生在图与图之间。

不在本线：窄带 DDC、Gardner、Costas、星座、工程 JSON、三来源标记、TensorRT batch、显示 GPU 与计算 GPU 互操作。
