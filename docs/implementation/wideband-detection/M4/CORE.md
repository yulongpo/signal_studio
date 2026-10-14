# M4 纯 domain 时间精修与评价

下文保留核心作者初版及 14 组自检的历史数学记录。随后用户确认 [方案 A](PARAMETER_DECISION.md)，当前实现升级为 `signalstudio-refinement-m0p-parameters-v2`：ROI 边缘比例和可调平滑生效、关闭拆分时忽略用户 min on/off、连续轨迹边缘比例按默认 0.5/0.6 缩放。默认 ROI 仍为五单元，sample 默认连续比例仍为 0.5；新扩展的参数变化检查是第 15 组。最终项目验证以节点归档为准，不能将初版的“不使用参数”描述当作当前产品行为。

`domain/detection_refinement.h/.cpp` 是独立 C++20 实现，不依赖 Qt，也不导入外部业务源码。数学依据只读核对 M0 冻结的 `refine_sample.py`、`refine_roi.py`、`business/detection.py` 和 `business/evaluation.py`。当前 SHA-256 分别为 `0ec2916095eab0686db47b6169926ac6c4e61ed768a6752b82ab59415844edad`、`c6d06e2a681a6e2e2c86a8c782e57cc83da0d777b8181d9e85f56fac56543a99`、`f825313f692b13813624b5adef2db8fa46099b6a11978cf7a088817160503c75`、`e460dd0f3b85c233bde0247df9b5634502b2dd1bc03d27677db211b1148667e9`。滤波与读取由 infrastructure 提供，domain 只消费物理 source-cell 功率与 STFT。

## 数学与分支

- Sample 使用左侧补零、保留全部窗口样本的因果移动平均；noise 为 0.5% 分位、burst 为中位数，burst 必须严格高于 noise×1.3；threshold=noise+edgeRatio×(burst-noise)，strict `>` 建 mask，on fraction 至少 3%。先丢弃不足 minOn 的连续活动，再合并严格短于 minOff 的间隔；关闭拆分仍返回 first–last 边缘。源样本格、最终短 cell、发布 ROI 裁边均以 uint64 半开区间计算。
- ROI 将真实 crop 双线性缩放至 frequency×time = 512×64，使用 `align_corners=false` 的半像素坐标。三次减均值、ReLU 后从正像素比例及均值建立 time/frequency 自适应门限，再做居中五点池化（包含零 padding、strict `>0.5`）。最短活动/静默以每 pooled column 的物理长度换算并向上取整；半开 pooled 边界经 floor/ceil 映射真实帧。不足三个真实帧、或每个活动段均映射不足三个真实帧时 fallback，不能把 64 个插值列当成真实观测帧。
- unique-window coverage≥0.7 的 sample/ROI 轨迹按连续载波处理；sample 的有效 edgeRatio 改为 0.5，两分支禁止拆分。该规则不改变 Coarse 的拆分。
- Coarse 仅采用真实 crop 内每帧最强 5% frequency bins 的平均 dB、其 20% 时间分位和 `bandwidthMarginDb` 门限。合并短 gap 后再按 `coarseMinBurstSeconds` 筛段；关闭拆分时只收缩 first–last 边界。Fine 不运行 coarse 最短突发筛选，ROI 不使用 Sample 的 edgeRatio/smooth。
- None 保留 observation，没有活动段。Fine/Coarse 不可靠时仅保留调用方提供并经过发布裁边的 `fallbackActivity`；调用方没有实际 coarse 活动证据时保持空 activity/未知时间，不把 observation 自动变成事件。

数值版本为 `signalstudio-refinement-f32-f64rolling-v1`。Sample power、smoothed 与分位临时副本均为 float32，分位使用 nth_element 与 NumPy 2.4 弱标量 float32 插值；门限计算值和实际 float32 比较值分别记录。独立实现以 float64 rolling sum 代替 torch float32 cumulative-sum subtraction，ROI/Coarse 的均值使用 float64。不能据数学夹具直接宣称与冻结 CUDA 逐值等价；真实来源的张量与最终边界差异须由节点实际对照报告说明。

输入按 working-cell budget 校验 shape、finite 值及物理连续格，最多 100000 个活动 run，拒绝无效/超预算输入。Sample 的峰值大数组为调用方 float32 power、float32 smoothed 和一次 float32 quantile 副本，而非三个 double 数组。取消回调在阶段之间以及 sample/coarse 循环内检查；取消结果没有部分活动段。证据独立记录有效参数、门限、源 origin/step/end、滤波支持、FFT 支持和量化不确定度；未把样本格精度当作物理滤波精度。

## 评价口径

每个正 IoU threshold（≤1）独立做矩形 Hungarian 一对一匹配；valid edge 的 cardinality bonus 为较小矩阵维度+1，保证先最大化有效匹配数，再最大化这些匹配的 summed IoU。没有跨文件匹配。未知时间预测的 TF/time IoU 为 0，但仍留在预测分母；frequency 评价可使用其频率证据。Failed/NotProcessed 文件保留全部真值并强制零预测。矩阵乘积及单侧事件数受 matrixCellBudget 约束，无效真值或配置报错，不能静默排除。

Dataset 按文件求匹配后 micro aggregate，并重算匹配起止误差 median/P95 及频率误差 mean。起止误差先做 uint64 差，再转秒；频率/time/TF 指标保持分离。`splitTruthCount` 是有至少两个 TF 正重叠预测的真值数，`mergedPredictionCount` 是有至少两个 TF 正重叠真值的预测数，属于不随门限变化的重叠拓扑诊断，不等于匹配 TP。`summarizeDetectionWindows` 对同一窗口仅取最高分一次，平均/最大分数和 coverage 使用唯一窗口及冻结分析窗口分母。

## 本次核心验证

2026-10-14，独立 MSVC 14.51 C++20 `/W4 /Od /fp:precise` 编译与执行通过，没有编译警告，14 个行为组通过。日志位于 `out/tasks/wideband-detection/M4/logs/core-standalone-msvc.txt`。包括细化筛选/合并顺序、3–4 ms 短突发、gap 等号、关闭拆分、连续轨迹、因果平滑、超过 2^53 的物理索引与尾格裁边、可靠回退/None、ROI 双线性映射与真实帧 fallback、Coarse 分支、预算/finite/取消、最大 cardinality 反例、unknownTime 分母、跨文件禁止匹配、失败文件保留真值、重叠拓扑和唯一窗口分数。

独立数学 oracle 仅使用 torch tensor/interpolate/pool 运算，没有导入冻结业务源码。20×16 夹具经 512×64 pooling 得到 pooled runs `[7,28)` 与 `[39,57)`，映射真实时间 `[102,109)` 与 `[112,118)`、频率 `[104,112)`，核心测试一致。脚本及输出分别位于 `out/tasks/wideband-detection/M4/diagnostics/roi_math_oracle.py` 和 `roi-math-oracle.json`。此证据是数学夹具，不能替代真实模型或数据集准确率、原生 UI 或 GPU 渲染验收。

主 Agent 集成重建后，已独立执行当前 `out/vs2026-qt611-debug_bin/SignalStudioDetectionRefinementTests.exe`，14 组通过；执行前确认该二进制的 UTC 写入时间 `2026-10-13T17:51:56Z` 晚于最后核心源码修改 `2026-10-13T17:50:43Z`。较早的项目二进制曾因旧 ROI 预期失败，修正后的预期来自上述独立数学 oracle；不能将早期二进制和当前结果混记。

完整项目 CTest、实际 IQ FFT、真实数据效果与原生联动由主 Agent 集成验证。本核心工作没有提交、推送或进入 M5。
