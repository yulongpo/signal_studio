# M3 模型解码与频率关联领域层

实现数字版本：`frequency-postprocess-v1-unique-window-max-score`。本记录不修改 M0 冻结 profile、输入契约和外部源文件。领域实现无 Qt、TensorRT 或外部业务源码依赖；尚待主代理通过项目脚本构建、执行测试及真实文件逐阶段对照。

## 依据与边界

只读核对规划 04/08、`contracts/wideband-detection/profiles/ffscn-gui-iq-v1.json`、M0 `assets.json`、本仓库独立 `model_probe.py:decode`。下列外部数学参考当前 SHA-256 均与 M0 一致：

| 参考文件 | SHA-256 | 提取规则 |
|---|---|---|
| FFSCN_TEST_GUI `algorithm/nms.py` | `5da650118b1fb8e4ee4805cb6bbc0bc53aa235d220c3c35901dd41d2aa0a6cb0` | 7 点峰、分数严格比较、标准 NMS |
| FFSCN_TEST_GUI `business/tracker.py` | `77c36a2b7f584c560e2109c97a78ff1688b527ace9765b736941657f0e862ed1` | 全局最佳一对一关联、漏窗封闭、频带融合、中心对齐包含关系 |
| SCN `algorithm/refine/BandwidthRefiner.cpp` | `979bf8d0e2152cc50be3769a9e4bc956746080c5226b7592ec4ba95c1ebe0cfb` | 全候选占用屏蔽、邻带最小噪底、仅向内收缩 |

SCN 3 点峰差值 `<1e-6`、score/NMS 含边界比较同时按规划和只读 `ScnDecoder.cpp` 核对。未复制业务源码或交互流程。领域输出只有频带与观察上下文，不生成发射时间、活动段、调制类别或 emitter 身份；M4 时间精修尚未实现。

## 模型解码 API

`decodeDetectionHeads` 接受一个 batch 成员的已激活 float32 `hm/bw/off` 与 `DetectionDecoderConfig`。FFSCN 单成员 shape 为 `[1,1,N/4]`；SCN 为 `[N/4,1,1]`。调用方负责在协议/运行时验证 batch shape 和三个 head 的逐个 shape，再传入领域视图。领域再次严格验证长度、布局、N/4、全部有限值、hm/off 在 `[0,1]`、bw 非负，不重复 sigmoid/ReLU。

主线和 FFSCN-17 使用 7 点精确局部峰、score `>`、NMS IoU `>`；SCN 使用 3 点峰及差值 `<1e-6`、score `>=`、NMS IoU `>=`。稳定排名为分数降序、peakIndex 升序；先 topK，再验证宽度和边界；宽度为零仍消耗 topK 排名。只有已经保留的候选参与后续抑制，因此 A-B-C 中 B 被 A 抑制后不能继续抑制 C。

stride=4，center=`4*(index+off)`，width=`4*bw`。两边裁到 `[0,N-1]`，裁完为空则跳过；head 无效或算术溢出则整次失败。FFSCN 使用 float32 几何运算，SCN 使用 double 运算。SCN 原参考解码器本身不裁边，本接口按本项目物理频率边界契约裁边；原 head/peakIndex 不丢弃。该边界差异及 FFSCN 与 Python oracle 的 float 运算差异须在 L2 报告单列，不能扩大已有 0.05 bin 容差。

`DetectionDecodeSummary` 保存峰数、排名数、topK 截断、无效宽度数及 NMS 抑制数。候选含 lowerBin/upperBin/score/peakIndex。N 最大 262144、topK 最大 512；失败/取消时 output 和 summary 保持调用前内容。

## 连续窗口与频率关联 API

`FrequencyTracker::reset` 固定 runId/fileId、完整 FFT RF 映射、totalAnalysisWindows、matchIou/minHits/maxMiss、mergeToleranceHz、包含关系中心裕量及碎片上下文允许间隔。`consume` 只接受该 run/file 的连续 eligible ordinal，从 0 开始；窗口 ID 全局唯一，重复、乱序、外部 scope 和退行名义支持均拒绝，状态不改变。

`WindowObservation` 分别提供名义窗口支持和实际读取/FFT 支持，均为原始样本整数坐标。根任务从 M2 固定源网格直接提供，不以显示矩阵或手工标记替代。候选通过 firstBinHz/binWidthHz 转为 RF，同时保留原始 bin、peakIndex 和 score。

在线配对按 IoU 降序、候选分数降序、活动轨迹索引、候选索引排序，全局最佳一对一消费。频边按命中窗口原边界平均；一次漏窗增加一次 miss，只有 miss `>maxMiss` 才封闭。minHits 为 `min(configured,max(1,totalAnalysisWindows))`，保持参考单窗口文件行为。

`finish` 先过滤不足 minHits 的轨迹，再融合支持相交且频率间隔不超过 mergeToleranceHz 的片段；之后按 IoU/窄带包含及中心对齐进行上下文碎片拼接。拼接的 gap 由调用方按冻结分支推导：细化拆分启用取 minOff，粗拆分取 coarseSplitGap，关闭拆分取 0。本层不把拼接间隔填成持续发射。样本 gap/支持排序保持 uint64，超过 double 精确整数范围也不丢失样本精度。

输出 `FrequencyTrack` 保存稳定 seedId（run/file/最早唯一窗口/peakIndex 的带长度前缀身份）、原频边平均、原频边包络、融合频边、完整窗口证据及其原频边。相同输入重放生成相同 seed；根任务可对其做 SHA-256 生成项目 Annotation ID。

窗口证据按 ordinal 保存。频率融合遇同一窗口多条碎片时保留所有原频边，每个唯一窗口取这些碎片的最大 score；scoreMean/scoreMax/hits/coverage 从该同一唯一窗口集合计算。原 GUI 融合有 hits 去重而 scoreMean 仍重复加权的口径不一致，本数字版本明确修正且保留原始 score，不沿用历史 scoreMean 名义。coverage=`uniqueHitWindows/immutableEligibleWindows`；取消/未完成窗口仍在原分母中，completedWindows 由任务层另记。该 coverage 不表示占空比或任务进度。

## 资源、取消和持久化边界

默认最多 100000 窗口、每窗 512 候选、4096 活动轨迹、32768 保留轨迹、100000 原频边证据、262144 配对，单次关联/最终融合各最多 4000000 比较。保守 32 MiB resident 预算包括字符串、容器增长和完整事务副本。超限明确失败，不随机丢候选或偷偷缩小分母。

consume 构造事务后一次替换，finish 在局部结果完成后一次发布；任意轮询取消/无效数据/资源失败都保留前状态或前输出。finish 可对已消费前缀做显式部分恢复；持久化和自动发布策略由任务层处理。当前内存 API 不能声称无上限长文件跟踪；超大轨迹集合须另经授权规划的 M5 资源验收证明，不能用 UI 大集合夹具替代。

## SCN 频率收缩

`refineScnFrequencyBands` 接受真实 averageDb/maximumDb、完整 RF bin 网格、待修频带和全部融合候选 occupancy。occupation 先合并为不可变区间，与显示/复核筛选隔离。

频带转为 `[floor(lower),ceil(upper))` bin 范围，邻带宽度为 `max(1,floor(bandwidthBins/10))`。仅在两侧邻带的有限 averageDb、且未被任何 occupancy 占用的点中取最小值，门限为 minimum+marginDb。候选内部使用 maximumDb `>=` 门限找首末 bin，并与原频边取交集，只向内收缩。没有邻点、带内没有达标有限点、空原范围或空修正范围保留原频边与明确 skippedReason。

最多 262144 谱点、100000 频带，单次至多访问 64 Mi 谱点后明确失败；取消或失败不发布半套修正。average 仅用于噪底，不能据此新增平均谱模型 forward。**真正最大谱唯一推理须由 production worker 的前处理及集成测试另行证明；本领域测试不代表已运行模型。**

## 验证交付

`tests/detection_postprocess_tests.cpp` 定义 11 组纯 C++ 行为测试：score/local-peak/NMS 临界值、A-B-C、并列 dense topK、offset/裁边、无效/溢出 head、重复/外部窗口、唯一统计、漏窗/重连/minHits、全局一对一配对、包含/错中心、近频与上下文融合、取消/各资源上限、SCN 最小噪底/只最大谱判边/占用屏蔽/缺点 fallback。

本子任务只做 Google clang-format 静态检查和人工边界核对，未构建、未运行测试、未运行模型、未提交/推送。主代理统一接入 CMake 与脚本，进行真实 L1/L2、Qt 回归与原生截图后更新节点验收。

正式 SCN 接入顺序见 FREQUENCY_EVIDENCE.md：逐窗口 CNR 3 dB → 整数测量频边 → IoU 0.1 / overlap 0.5 / gap 0 融合 → 全部未过滤融合候选屏蔽、平均谱邻带 MIN +3 dB / 最大谱内缩 → 150 候选上限 → 时间窗关联。此固定对照参数与原型卡片保留的 sample/ROI 时间精修噪底裕量分开，界面方案说明及实际窗口证据均明确列示。它不是 GUI 主线的默认门限，也不替代 M4 时间精修。
