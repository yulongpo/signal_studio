# M3 频率修正及证据

本文件记录 `infrastructure/detection_frequency_analysis` 的范围和边界。该层从真正 IQ DSP 帧独立测量频率，更新 `FrequencyTrack.frequency` 并输出 `DetectionFrequencyEvidence`。轨迹的 `originalFrequency`、模型窗口证据、名义和实际观察上下文保持不变；此层不估计发射起止、不拆时间突发、不创建活动段。M4 负责后续时间事件精修。

## 参考依据与来源核对

依据规划 `04_ALGORITHMS_AND_VALIDATION.md`、`08_FFSCN_TEST_GUI_CAPABILITIES.md` 的 F05，以及 M0 的 `assets.json`。本次只读核对原参考算法，独立实现 C++；外部业务源码不参与构建，不复制其业务实现。

| 只读参考 | 当前 SHA-256 | M0 冻结值核对 |
| --- | --- | --- |
| `D:/project/FFSCN_train2/FFSCN_TEST_GUI/business/detection.py` 的 `refine_blocks` | `f825313f692b13813624b5adef2db8fa46099b6a11978cf7a088817160503c75` | 一致 |
| `D:/project/SCN_based_PSD_Detection/algorithm/refine/BandwidthRefiner.cpp` | `979bf8d0e2152cc50be3769a9e4bc956746080c5226b7592ec4ba95c1ebe0cfb` | 一致 |

预备的模型转换、前向报告不能作为本层或 M3 正式完成证据。正式构建、CTest 和真实模型对照由主任务统一执行并归档。

## GUI 频率测量

全局噪底仅使用本次 eligible model windows 覆盖的真实 STFT 帧。按第一 eligible window 起帧至末窗最后帧，每个物理帧只记录一次，包括 anchored tail；重叠模型窗不会重复加权，显示图谱抽帧也不参与计算。ROI 请求只使用其完整模型上下文，不把文件无关部分纳入 percentile。

每帧的未归一化 float32 PSD dB 值写入自动删除的 `QTemporaryFile`。四次 radix 扫描通过保持数值顺序的 float32 bit 映射定位 10th percentile 的相邻秩，执行线性插值，无随机抽样或直方图近似。M0 和本机均为 NumPy 2.4.4，参考 `np.percentile(float32STFT, 10)` 的 scalar weak promotion 最终在 float32 中插值；差值、乘法和加减分别舍入，并在 gamma 至少 0.5 时从上端反向插值。读取块最多 4 MiB。负数、正数及不同指数保持正确数值顺序，非有限数值使任务失败。

对每条轨迹，在名义观察 `[begin,end)` 中以整数 sample/hop 网格的 searchsorted 语义取帧，积累 float64 dB 均值谱。频率边界以 FFT bin 中心的 searchsorted 取原始闭区间；门限为全局 10th percentile 加 `bandwidthMarginDb`。找带内最高峰，并向两侧寻找严格超过门限的连续区间。仅当实测宽度严格小于原始 bin 宽度的 0.5 时收缩，最终宽度至少 `ceil(0.1 * originalWidth)`；最终频边为所选 bin 中心加减半 bin。

最高峰等于门限时不会收缩，实测宽度等于一半时不会收缩。不足 3 个真实上下文帧保留原始频率，记录 `fewer-than-three-real-context-frames`。裕量为 0 时遵守 GUI 参考的关闭语义，记录 `nonpositive-margin-disables-gui-refinement`，不生成虚假的噪底或测量结果。

数值政策显式记录为 `unique-real-frames; float64-dB-mean; numpy-2.4-scalar-float32-linear-percentile`。均值积累使用 float64；参考 NumPy float32 均值的累计舍入误差没有被描述为逐 bit 相同。频率对照应同时记录门限距离和 bin 偏差。参考半 bin 映射在最左频率 bin 可超出 Nyquist 半 bin；发布端应按已有物理边界合同裁剪并记录，本测量证据保留参考转换。

## SCN 频率测量

正式 SCN 链路在 `domain/scn_detection_postprocess.cpp` 中按每个真实 16 槽模型窗口执行：最大谱空间 CNR ≥3 dB 过滤、整数 bin 边界发布、频带融合、邻带最小噪底 +3 dB 的最大谱向内收边，然后才送入跨时间窗口跟踪。未通过 CNR 的融合候选仍参与不可变噪底屏蔽集合。模型前向只接最大谱，平均谱只参与邻带噪底。完整原始候选、CNR、未过滤/已过滤融合频边和修正事实保存为每窗口 `scnPostprocess`；实际多窗口对照见 `FREQUENCY_PARITY.md`。

`applyDetectionFrequencyAnalysis` 的 SCN 分支是独立的平均/最大谱频率测量辅助 API 和单元测试入口，使用轨迹观察范围内的唯一真实帧。正式任务不在跟踪结束后调用该分支，因此不会用整条轨迹重新收边替代上述顺序。GUI 主线仍在跟踪后执行全局 percentile 频率处理，两种方法身份分开。

屏蔽集合在测量前由全部未筛选 fused tracks 固定，不受展示筛选或测量顺序影响。无有效邻点、邻带全被占用、带内无达标点都保留原边界并记录原因。GUI percentile 与 SCN 最小噪底分别保留方法身份，不混用。

## 输出证据和有界性

证据保存轨迹 seed、方法、原始/最终频率、是否变化、是否真正收缩、skip reason、有效噪底和门限、有效 peak/measured bin 标志、名义取帧范围、global percentile 帧范围与样本数、SCN 有效邻点数，以及 float64 均值/最大谱的 little-endian SHA-256。证据不内嵌完整谱矩阵；未执行的 peak/bin 测量通过标志区分，不将默认 0 解释成实际测量。

额外 scratch 的保守估算不得超过 16 MiB，并与 source 的 `estimatedPeakBytes` 一并校验工作预算；每轨迹输出也校验 `evidenceBudgetBytes`。GUI 临时磁盘上限为 `min(64 * workingSetBudgetBytes, 16 GiB)`；开始前校验计划字节数和可查询的磁盘余量，预留 64 MiB。所有轨迹累计最多处理 `64 * 1024^3` 个谱 bin，乘法前检查上限；超限明确失败，不静默改用采样噪底。该上限不表示支持无限长文件或无限轨迹。

取消在帧读取/FFT、每个 radix 读取块、各轨迹和频率测量中检查。临时文件在成功、失败或取消后自动删除。全部频率及证据仅在所有轨迹成功、最终取消检查和源指纹复查后发布；失败不部分改写调用方轨迹或证据。

## 自检用例

`tests/detection_frequency_analysis_tests.cpp` 使用独立生成的合成 IQ 写入真实 RAW 文件，再运行本仓库 CPU DSP。这些夹具不属于用户采集文件或 SCN 模型效果验证。主任务 `scripts/build.ps1` 生成的 Debug 测试程序在最后源码变更之后重新编译；针对同一 CTest 注册项的自检实测为 **1/1 PASS，0.29 秒**，没有跳过用例。

执行入口：`ctest --test-dir out/vs2026-qt611-debug -C Debug -R '^signal_studio_detection_frequency_analysis$' --output-on-failure`，临时将对应 `_bin` 加入 PATH，并在完成后恢复。日志：`out/tasks/wideband-detection/M3/logs/frequency-targeted-ctest.txt`。完整 M3 构建、节点回归及真实模型对照仍由主任务统一完成。

- 合成频谱检查 searchsorted、半 bin、严格门限、恰好 0.5 不收缩、0.1 保底及边界限制。
- 合成 IQ 的 RAW 文件经 `FixedDetectionSource` 产生 13 个唯一帧，对比两个重叠/尾锚模型窗的 20 个重复帧；percentile 与独立排序线性插值对照。
- ROI 只取 eligible window 的完整真实上下文；短轨迹按 sample/hop searchsorted 取两帧并保留 fallback。
- SCN 频率方法使用合成 IQ RAW 的唯一帧均值/最大谱及全部占用集合，对照域层的邻带最小值修正。
- 中途取消、不足预算、跨 run 轨迹、非有限谱输入均不发布部分结果；观察上下文保持不变。
- GUI 裕量为 0 时明确关闭；不够完整模型窗的 9 帧源完成空结果，不伪造频率证据。

格式按仓库 `.clang-format` 的 Google C++ 风格执行。该层测试为真实 CPU DSP/频率证据验证，不单独证明 TensorRT 前向、GPU DSP、二维事件效果或 M4 时间边界。
