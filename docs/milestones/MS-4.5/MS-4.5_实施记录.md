# MS-4.5 实施记录

## 仓库预检

- 2026-07-29 已执行 `git fetch --all --prune`，退出码 0。
- 当前分支为 `codex/full-signal-studio-development`。
- `origin` 的 fetch/push 地址均为 `https://github.com/yulongpo/signal_studio.git`。
- 开始实现前 HEAD 为 `cbe50ce`，与远程同名分支 ahead/behind `0/0`。
- 开始实现前唯一未跟踪文件为本里程碑根目录任务文档。

## 前序里程碑审计

- MS-02 提供 oneMKL/cuFFT FFT 计划缓存、VSL 卷积、LAPACKE IIR、libsamplerate、
  ProcessingChain、取消、下游失效和后端来源；本里程碑复用而不复制内核。
- MS-03 提供纯 C++ DisplayMapping、AtomicFrameCoordinator、ViewRequestId、
  VisibilityController 和 Qt Designer Workbench；本里程碑只扩展宿主参数编辑。
- MS-04 的基础分析固定最多 16,384 样本、PSD Hann、STFT 最大 1,024 点和 75%
  重叠，Artifact 参数版本固定为 `psd-default-v1`；这些是本里程碑要消除的限制。

## 后续边界审计

- MS-05 保留 Selection 建通道、DDC、重采样输出、通道处理链和联动优先级。
- MS-06 保留 PluginSDK、ONNX ModelRuntime、Dataset 和调制/解调算法。
- MS-07 保留正式工程化打包、SBOM 和发布级文档收口。
- MS-08 按批准基线保留第二个 Signal Generator 薄壳复用证明。
- MS-09 保留最终验收、标签和 GitHub Release。

## 依赖审计结论

没有引入新依赖。所有 FFT、滤波、重采样、卷积和线性求解继续通过既有 Adapter
进入 oneMKL、cuFFT 或 libsamplerate。MS-4.5 新代码仅负责参数校验、算法编排、
核参数、缓存身份、状态管理、序列化和 UI。

Savitzky–Golay 不采用手写通用线性代数或卷积。系数求解和实际卷积必须分别编排
既有 oneMKL LAPACKE 与 VSL Adapter；这满足现有批准依赖、数值交叉验证和第三方
隔离要求，不增加另一个功能重叠的运行时。

审计还发现并在本里程碑回归修复：外部数据可用门禁原先只检查 WAV 和其中一份
X310 文件，而测试实际消费三份批准录制；配置门禁现已要求三份文件同时存在。
MS-04 历史测试报告中两份 X310 的大小/摘要行存在对调，历史证据不回写，本里程碑
以 `test_data/test-data-manifest.json` 为机器真值并在完成报告中记录该审计差异。

## 实施状态

实现和验证进行中。最终文件、测试矩阵、性能、截图、复审、提交与推送证据将在同一
里程碑收口时补齐。
