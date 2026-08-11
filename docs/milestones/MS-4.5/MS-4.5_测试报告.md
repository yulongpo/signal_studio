# MS-4.5 测试报告

## 1. 环境与范围

最终收口日期为 2026-08-11。环境为 Windows 11 x64、MSVC 2022 x64、CMake/Ninja、
Qt 6.11.1、CUDA Toolkit 12.4 和 NVIDIA GeForce RTX 5060 Laptop GPU。没有安装、升级
或修改第三方依赖。CPU 使用 oneMKL DFTI/VSL/LAPACKE，CUDA 使用既有 cuFFT Adapter，
重采样继续使用 libsamplerate。

## 2. 最终测试矩阵

| 配置 | 范围 | 结果 | 时间 |
|---|---|---:|---:|
| CPU Debug | 全量 CTest | 267/267 | 411.54 秒 |
| CPU Release | 全量 CTest | 267/267 | 284.80 秒 |
| CPU Debug | `ms-4.5` | 24/24 | 70.41 秒 |
| CPU Release | `ms-4.5` | 24/24 | 12.79 秒 |
| CUDA Debug | `ms-4.5` | 24/24 | 39.55 秒 |
| CUDA Release | `ms-4.5` | 24/24 | 12.29 秒 |

CPU 两套全量均为最终代码重新执行，包含公共头、批准基线、依赖锁、TaskRuntime、
安装组件消费者、无 Qt 安装消费者、默认 Windows 平台、MS-02、MS-03、MS-04、Qt、
高 DPI、截图和 MS-4.5。CUDA 后端测试实际核对 `cuFFT` provenance，未把 unavailable
或 CPU fallback 记为 CUDA 通过。

## 3. 专项覆盖

24 项专项由以下范围组成：

- 5 项 DSP 契约、Spectrum/PSD、STFT/预滤波、CPU/CUDA 与 SciPy 参考；
- 2 项 Visualization/Workbench Inspector、可见性与显示映射；
- 7 项应用参数、缓存、最新提交、工程切换、迁移、来源与短稳定性；
- 10 项 Designer、1280/1080P/4K、125%～200% DPI、异步运行时和真实截图。

参数生效测试比较 FFT 长度、补零、泄漏、窗、Periodogram/Welch、线性/指数平均、
最大保持、三种频谱平滑、STFT 两维平滑和分析前滤波的实际数值。缓存测试区分显示、
纯平滑、变换和预滤波四级失效；最大保持测试覆盖 CPU A→CPU B→CUDA policy→CPU B
以及 source A→B→A，旧 cache key 不得复活旧峰值或伪造 lineage。

## 4. 数值与后端

- 八种窗的 coherent gain 与 ENBW 有解析断言；Kaiser Beta、Tukey Alpha 和非法组合有
  边界测试；
- 实信号单边 RMS/功率翻倍保持 Parseval 一致，复信号使用移位双边；
- Spectrum/PSD 共享窗化和 FFT，Periodogram/Welch、平均和保持在线性功率域执行；
- Savitzky-Golay 系数由 LAPACKE 求解、卷积由 VSL 执行，FIR/IIR 预滤波进入既有
  ProcessingChain；
- STFT 覆盖非零源起点、帧中心、hop、尾帧丢弃/补零；
- NumPy/SciPy 固化参考与 CPU oneMKL、实机 cuFFT 的轴、幅度、PSD、Welch、STFT raw
  和预滤波结果交叉校验；
- 每帧 provenance 比较 backend、device、version、precision、fallback/degraded，混合
  来源在聚合和发布前均被拒绝。

## 5. 异步、Artifact 与恢复

正式 Qt 分析任务使用单调 CAS 状态机：取消只能在 `cancellable` 阶段取胜；任务进入
`committing` 后由 TaskRuntime 最新视图许可作最终提交门禁。设置、Artifact 和 Workspace
先提交，随后 TaskRuntime 在同一终态转换中登记 Artifact 的 payload、`manifest.json`、
`.artifact-index` 三个真实文件并记录大小/SHA-256，完成状态才对外可见。

回归覆盖：N 已计算后签发 N+1，N 的 Artifact 发布必须拒绝；N+1 正常提交；取消/失败
回滚；重启后三个文件与任务历史精确匹配；文件损坏后任务恢复为失败；未完成或缺少已
登记文件的正式结果在工程打开时清理。临时视图重算不制造正式 Artifact。

## 6. UI 与证据

`SignalAnalysisSettingsPanel.ui` 由 `qt_wrap_ui` 进入生产构建，控件值真实生成参数快照。
Qt 自动化覆盖 1280×720、1920×1080、3840×2160 及 125%、150%、175%、200% DPI。
最终 qwindows 截图使用真实 X310 录制，逻辑窗口 1920×1080、DPR 150%、物理 PNG
2880×1620，大小 845,143 字节，SHA-256 为
`aeba1a628cf68c8cee0e1b6574ec92b3f62534a69d89ae074bae7cbc252b6387`。

## 7. 稳定性

短稳定性用例在四套专项中重新执行。2026-07-30 已有 1,800 秒参数切换原始证据：
125,061 轮、缓存命中 125,057 次、拒绝过时提交 7,357 次、退出码 0、stderr 0 字节，
资源保持有界。用户明确要求不再运行 30 分钟稳定性测试，因此最终收口仅审计并保留该
历史证据，没有重复执行，也没有伪造第二次结果。MS-04 的 8 小时稳态同样不重复执行。

## 8. 静态质量、矩阵与边界

- 14 个变更 C/C++ 文件通过 `clang-format -n --Werror --style=file`；
- 7 个变更生产翻译单元重新执行 clang-tidy，退出 0、0 errors；仓库内告警为既有
  `pragma once`、ABI 枚举宽度、MSVC/Qt 分析噪声及已审计参数可交换提示，不声称零告警；
- `git diff --check`、批准基线差异、第三方依赖差异和调试标记扫描通过；
- 需求追踪矩阵 10 个工作表、10 个表、200 项需求、公式错误 0，四类覆盖率均为 100%；
- 没有修改批准基线，没有提交外部录制或构建产物，没有增加第三方依赖或重写成熟内核。

最终独立复审重跑受代理额度限制，未返回新的独立结论；主代理完成最终规格和代码质量
复审，未发现剩余 Critical/Important。Windows-only 门禁不包含 Ubuntu；远程 CI 未作为
本地提交前通过项。MS-05 未开始。
