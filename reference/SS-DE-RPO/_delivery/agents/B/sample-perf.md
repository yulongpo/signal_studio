# Sample 性能说明（Agent B）

状态：**未能实测**。本工作树没有 `CMakeCache.txt`，也没有 `out/`。按任务约束没有新配 Qt/VS 构建，也没有使用 `D:\project\signal_studio_rebuild\out`。因此没有墙钟、峰值 RSS 或 GPU 计时。下面只是对现有公式的静态核算，不是基准。

## 静态核算

Streaming/Auto 的执行驻留仍是块工作区，不是整段 IQ：

`32 * fftSamples + 16 * (2 * radius + outputBlockSamples) + 16 * outputBlockSamples + readerReserve + resultByteGrant + 4 MiB`

`outputBlockSamples` 不超过 131072，FFT 不超过 `maximumFftSamples`（默认 1<<20）。N 只进入 scratch 预约 `8 * count + 8192`，那是配额内的临时文件，不是一次连续 RAM 分配。

既有 96 MiB 探针（`tests/detection_sample_stream_tests.cpp`，1.2e9 样本、20 MS/s、块长 257、`readerResidentReserveBytes = 0`、`resultByteGrant = 16 MiB`）按该公式约为：

- 带宽 `0.24 * 20e6`，未截断的 FIR 半径约 607，`fftSamples = 2048`
- 块工作区约 21 MiB，仍 `<= 96 MiB`
- scratch 预约仍是 `9600008192`（`8 * 1200000000 + 8192`）

这次改动没有改这个公式。1 GiB 连续上限只让 `legacyEligible` 在 `exact` 超过 1 GiB 时为 false。6 GiB 授权下，数 GiB 的精确缓冲不再具备 LegacyExact 资格，长信号留在已有 Streaming/Auto 路径。

短上下文仍可走精确路径：`kAutoExactSamples = 1<<20`，约 1M 样本的 `exact` 远低于 1 GiB，也低于 6 GiB 授权。

## 没有测到的内容

- 20/363/2044 窗口的端到端耗时
- 6 GiB 授权下的实际 RSS 峰值
- 分块 FIR 相对整段精确路径的加速比
- GPU FIR 与 CPU 回退的时间差

这些需要本工作树内的已配置构建，或主代理集成后的定向测试。不能把上述静态字节数写成实测性能通过。
