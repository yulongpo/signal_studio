# M4 同源 sample 能量数值诊断

本工具只比较一个预先指定的真实文件样本区间和 RF 频带。它不运行模型，不修改产品算法、参数或容差，也不承担完整 M4 L3/L4 验收。G WAV 是生成器数据，读取真实 WAV 字节不代表真实 RF 采集。

## 输入与冻结

`SignalStudioDetectionSampleEnergyTests` 不带参数仍执行原有任意长度 FFT、物理切带、预算拒绝和取消检查。带参数时读取本仓库 `WavDescriptor` 和 `RawSampleReader` 支持的双声道 PCM16 I/Q WAV，I 在前、Q 在后、按 32768 归一化；以 131072 样本块填充完整搜索区间，直接调用产品 `detectionBandEnergy`。

配置例子如下。所有样本索引均为 source sample 的零起始半开区间，必须使用 uint64 十进制字符串；数值型索引会被拒绝。该例固定 200000 个实际样本，RF 参数只是格式例子，正式运行须使用预先冻结的实际频带。

```json
{
  "schema": "signalstudio-sample-energy-parity-config-v1",
  "startSample": "0",
  "endSample": "200000",
  "sampleRateHz": 20000000,
  "centerFrequencyHz": 2500000000,
  "bandRFHz": {"lowerHz": 2499900000, "upperHz": 2500100000},
  "smoothSeconds": 0.001,
  "sampleEdgeRatio": 0.6,
  "minOnSeconds": 0.005,
  "minOffSeconds": 0.01,
  "splitBursts": true,
  "windowCoverage": 0,
  "referenceSemantics": "gui-frozen",
  "sourceKind": "G-generator-PCM16-IQ-not-real-RF"
}
```

`smoothSeconds` 必填；其余时间参数未提供时采用当前产品默认。RF 带必须完整位于声明 tuning centre 的 Nyquist 区间内。工具将 RF 中心减去 `centerFrequencyHz` 后传给能量函数，不固定取 DC，也不把区间补成二次幂。实际产品上下文可从原始 `timeAnalysis.sourceOriginSample/sourceSupportEndSample` 及 annotation 频率得到；工具本身不根据结果选择区间或频带。

默认独立参考保留 `gui-frozen` 行为。若事先指定 `product-v2`，参考门限比例及 noSplit 时长使用本仓库已有的对应版本。两种语义的区别必须保留在输出，不能在看到结果后切换版本。C++ 始终调用当前产品 `refineDetection`；工具不改变它。

正式读取新 parity 结果前，由主实施建立 `signalstudio-m4-execution-freeze-v1`，令 `readyForInference=true`。Python 必须以 `--freeze` 验证整个 manifest，且至少包含本工具、`time_reference.py`、`validation-protocol.json`、源 WAV、配置文件及实际 native CLI executable 的 SHA-256。完整产品/工具依赖源码、runtime 和数据身份仍由主实施一起冻结；该最小工具检查不是完整 M4 freeze 的替代。native CLI 无独立 freeze 开关，必须由主实施在冻结后启动。

## 执行

只构建现有 energy 测试 target：

```powershell
cmake --build --preset local-debug --target SignalStudioDetectionSampleEnergyTests
& ./out/vs2026-qt611-debug_bin/SignalStudioDetectionSampleEnergyTests.exe
& ./out/vs2026-qt611-debug_bin/SignalStudioDetectionSampleEnergyTests.exe `
  --file <wav> --config <config.json> --output <native-output-dir>
& D:/profiles/anaconda/python.exe -B tools/detection/time_energy_parity.py `
  --file <same-wav> --config <same-config.json> --native <native-output-dir> `
  --output <comparison-output-dir> --freeze <execution-freeze.json> --device cpu
```

主实施调度 GPU 后，可显式指定 `--device cuda:0`。CUDA 不可用时直接失败，不自动换 CPU。无需加载 checkpoint、读取 native heads 或导入外部业务模块。正式运行目录位于 `out/tasks/wideband-detection/M4/diagnostics/energy-parity/`；日志放在对应 `logs/`。

CPU 数学自检不读取新 G/T 结果，无需 execution freeze：

```powershell
& D:/profiles/anaconda/python.exe -B tools/detection/time_energy_parity.py --self-check
```

## 输出和判定

native 导出完整 `native-power.f32` 与 `native-smoothed.f32`，均为 little-endian float32，无绘图抽样。完整 smooth 导出使用与 domain 相同的逐样本 float64 rolling 顺序，随后将同一完整 power 传给实际 `refineDetection`；JSON 保存实际生产 quantile、threshold、状态、原因与 source integer segments。Python再次用相同次序独立重算 rolling，检查导出 full smooth 及生产证据，避免将镜像循环本身当作产品证明。

独立功率来自本仓库 `time_reference.envelope_power`，采用 complex64、局部索引 float64 phase remainder 后转 float32、完整原长度 FFT、raised-cosine 低通和 float32 功率。参考 smooth 保持原始 torch float32 `cumsum` 左侧补零及累计差分，不替换成 float64 prefix。另将 **同一 native power** 输入参考 smooth，单独定位 smoothing 数字契约差异。

`energy-parity.json` 包含完整 power/smooth 的 max/mean/P50/P95/P99 误差、逐样本超差数量、全量 signed float64 difference 文件、全量 uint64 超差索引、noise/burst/threshold、mask SHA、全量 mask 差异索引、source 整数段数/端点、状态/原因、实际支持及文件 SHA。数量不同的 runs 不进行伪造的一一匹配；数量相同才附按顺序配对的 signed endpoint error。

容差固定来自 `VALIDATION_PROTOCOL.md` 第 5 节及 `validation-protocol.json`：power/smooth/linear threshold 的 `atol=1e-7, rtol=1e-3`，判断为 `absError <= atol + rtol * abs(reference)`。任何超差均完整报告，工具不提供放宽容差选项。mask/run/整数端点及状态/原因单独精确核对。门限临界样本仅以冻结的 value tolerance 加 threshold tolerance 定义诊断区，不作为通过豁免；即便全部 mask 差异处于该区，`pass` 仍为 false。

退出 0 表示该区间的所有 scoped 检查通过；数组、scalar、生产镜像、mask、segments、状态/原因任一未通过时退出 1 并保留报告。输入/身份/预算错误直接失败，不能将无输出等同数值通过。native fallback 未产生生产 threshold 时，Python另报完整 smooth 的诊断 threshold/mask；这些不是产品实际执行的后续决策。

## 资源与验证边界

两侧固定预算为 256 MiB。native 保留产品的 `12*N+2 MiB` 上下文检查，并由实际 FFT 再检查其 scratch；Python使用 `128*N+16*smoothWin+2 MiB` 的保守 live array/scratch 估计，超限拒绝。预算不包括 runtime/library 进程基线与 native 文件映射，属于工作数组估计，不能声称是 OS 强制 RSS 限制。CUDA 输出另外记录 peak allocated bytes。

工具源码与自检结果可以先交付给主实施做 hash。正式 G 固定 200k/400k 或实际候选上下文的执行、CUDA parity 和运行结果归档由冻结后的调度决定；本说明不预先声明数值等价。
