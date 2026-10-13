# M3 全窗口频率层独立对照

当前状态：2026-10-14 实际全窗口频率层独立对照通过。主线和 SCN 各 4 个 eligible 窗口、各 8 条最终轨迹，原始解码、频率处理链、唯一统计及最终持久化标记一致。本结论不包括时间事件/二维指标或完整 G/T 集效果。

## 工具与输入

`tools/detection/model_frequency_parity.py` 只读取真实 WAV、M0 身份 manifest，以及 production worker/task 持久化的模型原始 head、结果 chunk 与 Annotation。没有导入或执行外部 GUI/SCN 业务模块，也没有用原业务实现作为本项目检测器。

Python 使用用户指定的 `D:/profiles/anaconda/python.exe`。该工具只导入本仓库 `tests/fixtures/wideband/generate_detection_source_golden.py:frame_density` 计算周期 Hann、Torch CPU FFT、PSD density；原始 IQ 来自经 M0 SHA-256 核对的实际双通道 PCM16 WAV。模型 head 读取布局为 `[1,3,1,N/4]`，次序 hm/bw/off，不重复 sigmoid/ReLU。

正式运行命令：

```powershell
& 'D:/profiles/anaconda/python.exe' tools/detection/model_frequency_parity.py `
  --pipeline-root out/tasks/wideband-detection/M3/diagnostics/pipeline `
  --assets docs/implementation/wideband-detection/M0/assets.json `
  --report out/tasks/wideband-detection/M3/diagnostics/frequency-parity.json
```

默认核对 pipeline 中全部正式 profile；`--profile ffscn-gui-iq-v1` 可单独诊断主线，单 profile 报告不代表另一 profile 通过。正式报告存于 M3 diagnostics，最终由主代理归档到本节点 docs；工具不执行构建、Qt 回归、提交或推送。

## 数学依据与版本

只读数学来源为 M0 已固定的 FFSCN_TEST_GUI `algorithm/nms.py`、`business/tracker.py`、`business/detection.py`，以及 SCN `BandwidthRefiner.cpp`。运行前逐个重新核对 M0 hash；SCN CNR/Fusion 两个参考文件同时记录当前只读 SHA-256，并明确它们未列入原 M0 sourceFiles 哈希条目。

Oracle 独立表达区间 IoU、排名/NMS、全局最佳一对一在线配对、miss/minHits、相交观测上下文近频融合、同中心包含关系碎片拼接。中心额外裕量固定为参考的 2 个 bin；不会从 native 当前值读取一个错误的新门限。只有名义/真实观察支持，未生成发射活动段。

本项目数字版本为 `frequency-postprocess-v1-unique-window-max-score`：同一窗口内多条频率碎片融合后保存所有原频边，每个唯一窗口贡献最大 score，再计算 mean/max/hits/coverage。原 GUI 融合在 hits 去重后仍可能重复加权 scoreMean。报告同时输出唯一窗口均分与原重复加权均分，披露这一口径差异，不静默沿用原 GUI scoreMean 名称。

FFSCN 解码分别核对 float32 几何及已有 `model_probe.py` 对应的 double 几何数学结果；SCN 3 点峰、含边界比较独立于主线 7 点峰和严格比较。SCN 原始预测频边裁到本项目物理 `[0,N-1]`，这一数字契约差异明确记录。

## 分阶段比较

| 阶段 | 实际输入及核对内容 |
|---|---|
| 身份与完整性 | WAV/冻结 checkpoint/SCN engine、数学参考、每个 canonical chunk、每窗 raw head、runtime model identity 与 annotations export SHA-256；export 必须等于最终持久化 projection，且存在 publication-end 标志 |
| 全窗口解码 | 每窗三个 head 独立局部峰、稳定 topK、stride/offset/width、NMS；数量、peakIndex 顺序、score 和频边逐项对照 |
| 来源支持 | 完整文件 padding、原始固定帧网格、名义窗口与实际 FFT 读取范围；不把 requested ROI 改成新的物理短文件 |
| 主线频率修边 | 真实 WAV 重建所有唯一分析帧；全帧全频 10% percentile；每轨迹上下文的 mean PSD、门限、主峰连通边界、保守最小宽度及半 bin 频边 |
| SCN 每窗处理 | 16 真实 dB 谱帧最大保持；CNR 中央圆整 trim 与邻带均值门限 3 dB；整数测量边界进入频带融合，IoU≥0.1 或包含≥0.5，gap=0 不合并相邻；全部未滤融合占用屏蔽；平均谱邻带最小噪底+3 dB，最大谱门限内缩；稳定排序及 150 上限 |
| 跨窗与最终标记 | 窗口来源/支持与所有原频边、稳定 seed/Annotation ID、唯一窗口分数、mean/max、hits/eligible 分母、覆盖率、原始/修正 Hz、完整持久化结果 |

SCN CNR 使用 maximum 的中央及侧邻带均值；SCN 带宽噪底使用 average 的侧邻带最小值，二者不得混淆。CNR 拒绝的候选仍参与未滤融合占用屏蔽。最终 SCN 不应再运行一次全轨迹频率修边；对照核查其缺少这一额外阶段。runtime 与每窗 metadata 必须同时声明 maximum-only 及 average 模型推理次数 0；模型 forward 数值的 L2 证据由另一 pipeline parity 工具提供。

## 判定条件和边界

原始候选数量与 peakIndex 次序、唯一窗口身份/支持、最终轨迹数量、eligible 分母、门限接受/skip 原因、修正是否应用均须一致。沿用已有候选频边误差 ≤0.05 bin、score 误差 ≤0.001；最终频边同样按 0.05 bin 换算 Hz；PSD/CNR/noise 数值使用既有 L1 的 0.02 dB 界限；coverage 数值误差 ≤1e-12。不会根据结果扩大门限。

GUI 原参考 mean 为 float32 累加，本项目 native mean 为 float64 累加；报告披露该数值边界并保留原 PSD 容差。即便噪底数值在容差内，若造成接受/轨迹数量/最终频边分歧仍判失败。原始频边、修正频边与 ROI 裁后展示边界分别核对。

本工具是有界真实数据诊断：最多 512 个模型窗口、64 Mi 个谱值。实际输入是完整 0.8 s WAV 的 0–20 ms requested ROI，而模型分析上下文可延伸到 ROI 外；主线通常为 4 个窗口、25 个唯一帧，SCN 为 4 个窗口、31 个唯一帧，实际数量以经验证的产物为准。完整 20 ms 物理文件的尾窗规则不同，不能混用。

本次只核对频率处理链和持久化证据，没有时域/ROI 精修、真实发射起止、活动段分类或事件匹配，因此不声明历史二维 TF-F1=0.8261 已复现，也不以本次频率层一致性代替 M4/M5 验收。

## 当前静态检查

已用本机 Python 完成语法检查、Black 80 列格式检查与 Ruff `E4,E7,E9,F` 检查；没有额外构建或完整回归。主代理统一完成本次 live pipeline 构建与执行后，本子任务按上列 CLI 运行，最终退出码为 0，报告 `passed=true`，两 profile 均通过。

## 实测结果

报告为 `out/tasks/wideband-detection/M3/diagnostics/frequency-parity.json`，执行日志为 `out/tasks/wideband-detection/M3/logs/frequency-parity.log`。Python 环境 NumPy `2.4.4`、Torch `2.12.0+cu132`；DSP oracle 使用 CPU，CUDA 版 Torch 的安装身份不表示此工具在 GPU 上计算 FFT。

| 项目 | FFSCN 主线 | SCN 最大谱 |
|---|---:|---:|
| 完整 eligible / completed 窗口 | 4 / 4 | 4 / 4 |
| 重建的唯一真实帧 | 25 | 31 |
| Oracle / native 最终轨迹数 | 8 / 8 | 8 / 8 |
| raw head 解码最大频边误差 | 0 bin | 0 bin |
| double 几何参考最大频边误差 | 0.00199842453 bin | 0 bin |
| 最终原始频边最大误差 | 0 Hz | 0 Hz |
| 最终修正/展示频边最大误差 | 0 Hz | 0 Hz |
| unique mean / max score 最大误差 | 0 / 0 | 0 / 0 |
| hits、eligible 分母、window 支持、immutable 原始 head/后处理引用 | 全部一致 | 全部一致 |
| coverage 最大误差 | 0 | 0 |
| 噪底最大误差 | 0.00000762939453125 dB | 0.0000152587890625 dB |
| CNR 测量最大误差 | 不适用 | 0.00000111262004054 dB |
| SCN 每窗修边最大误差 | 不适用 | 0 Hz |

主线全局 10% percentile 为 `-100.53638458251953 dB`（实际输入已整数满量程规范化的 PSD density）。最终 Annotation JSON 与 hash 验证后的 canonical publication 完全一致，原始 head 与各窗口后处理引用也逐项匹配；取消前缀另一个 run 未混入正式全覆盖 run。首次工具诊断暴露“同一 profile 目录只有一个 manifest”的错误假设，已改为按 live profile 的 immutable `manifestRelativePath` 精确选择 run，没有排除该 run 的窗口或改变数学容差。

本次实际 SCN 每窗 8 条测量均通过 CNR，共 32/32；没有实际 CNR 拒绝候选。它验证了全占用屏蔽、最小噪底与 gate 数值的一致性，但“被拒绝候选仍应屏蔽”还需结合本节点对应反例测试，不将本次真实文件声称为该反例。此次两个 profile 的唯一均分与旧重复加权均分数值差为 0；口径修正仍有独立定义和测试，不凭本批次数值相同抹去差异。

本次 oracle 提前指出的两个数学偏差已由主代理修复并在实际产物中核对：中心裕量由错误的 64 bin 恢复到冻结 2 bin；SCN CNR 后进入 fusion 的边界恢复参考整数测量 bin。未修改 M0 权重、原始 IQ 或 profile 正式冻结 hash。

| 身份 | SHA-256 |
|---|---|
| 实际 WAV | `88f96c6deab9e435db8f111463092fdae9b800aeb3cbccd82d7426551d52d9cd` |
| 主线 epoch-1200 checkpoint | `acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0` |
| 本机主线 FP32 engine | `d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81` |
| SCN engine | `ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627` |
| 最终 oracle 源文件 | `eff7a6849c70e11c6a5ef2caa5a754f7df5cbec7cf00b7438c84a6c284174681` |
| 最终频率对照报告 | `951d1e3ddb9c527105599b2de59cde365189ccc696e91b2b7c356c1c01dad3c7` |

每窗口 raw head、每个 canonical chunk、M0 assets、完整 pipeline report、数学参考及内部 DSP helper 的 SHA-256 均保存于 JSON 报告，便于后续核对同源产物。
