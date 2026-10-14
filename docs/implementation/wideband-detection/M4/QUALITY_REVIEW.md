# M4 独立质量审查

当前结论：**QUALITY_PASS（已读代码范围）**。本轮发现的评价器原始 r0、未知时间分母、失败噪声曝光，以及 raw publication 人工字段边界问题已由主实施修正；修正后的代码没有未关闭的行为问题。新版 raw decoder 拒绝用例的 focused 检查也已通过并核对二进制身份。该结论不代表 M4 节点完成：全量 CTest 的 UI 超时尚在调查，正式 G/T、同源数值对照及第二屏 34 场景旅程仍须分别完成并归档。

审查基线为 M3 `6ed5ce0` 后的 M4 工作区，首轮复审时间为 2026-10-14 03:13，最终 focused 补证时间为 03:20（Asia/Shanghai）。审查者仅修改本报告；没有修改业务代码、构建、全量 CTest、GPU/参考模型前向、正式 G/T、M5 工作或提交。主实施在审查期间继续修改源码，执行证据均按下方身份与时点限定。

## 范围与批准边界

已读 `domain/detection_refinement.*`、`detection_refined_fusion.*`；`infrastructure/detection_sample_energy.*`、`detection_time_analysis.*`、`detection_refined_publication.*`；projection/store/task 的 M4 增量；实际保存 trace、阶段进度及 UI 刷新缓存；对应数学、融合、持久化测试；独立 reference/evaluator/holdout，以及后来完成的 `time_energy_parity.py`、sample energy CLI 与 `ENERGY_PARITY.md`。

数学和 UI 语义以用户批准的 `PARAMETER_DECISION.md` 方案 A 为准：ROI threshold 乘 `edgeRatio/.6`，平滑核为 1–63 奇数单元，用户 noSplit 时 minOn/minOff 不参与时长筛选，连续轨迹保持 `coverage>=.7` 禁拆分及 sample `.5*(edgeRatio/.6)`。连续启发式可能跨真实静默是已批准且必须披露的限制，本审查没有要求改变它或其他算法门槛。

## 发现与关闭记录

下表保留首读缺陷及实际修正，不表示当前仍然存在。原始评价器问题位置对应首读快照；当前行号以源码为准。

| 项 | 首读问题与影响 | 修正及独立复审 | 状态 |
|---|---|---|---|
| Q1 / P1 / evaluator raw r0 | 首读 `time_evaluation.py:417–421` 对任何非空 `revisions` 报人工修订；`encodeAnnotation` 必然保存合法 `algorithm-origin` r0，正常 native 输出会被按失败零预测计分 | 改为明确校验唯一原始 r0、model actor、Pending review、current/original/change 一致及无人工编辑；活动限 `AlgorithmEstimated`、未 confirmed，拒绝 Manual time/frequency | 已关闭，CPU 7 项 self-check 及独立 native shape 内存 codec 检查通过 |
| Q2 / P1 / unknown denominator | 首读 loader 只计 `unknownObservations`，实际 TF 评价仅收到活动 events；一条命中加一条未知观察会错误得到 F1=1 | 两侧未知输出各进入一个不构成活动的 accounting placeholder，TF/frequency overlap 为零，仍贡献 FP；不把 observation 矩形当活动 | 已关闭；独立 native r0 一条活动+一条空活动观察得到 predictionCount=2、TP=1、FP=1、FN=0、F1=2/3 |
| Q3 / P2 / failed noise exposure | 首读 `summarize` 把失败/缺失噪声文件的全文件时间计入 observed exposure，还将其零预测称为 zeroDetectionFile；可把误报率稀释一半 | 仅本侧完成的噪声文件参与 observedFiles、曝光、FP/file 与 zeroDetectionFiles；总文件/失败数量独立保留，主评价失败文件真值仍计 FN | 已关闭；一完成+一失败、完成文件 1 FP 的 fixture 得 observedFiles=1、exposure=1 MHz·s、zeroDetectionFiles=0、FP/file=1 |
| Q4 / P2 / durable raw scope | 首读 C++ decoder 的唯一 r0/action 校验仍允许 user actor、r0 observation/name/note 编辑字段，以及同步修改 current/original/after 后的 Manual frequency | decoder 现拒绝 r0/review 非 model actor、name/note/observation 编辑字段和 Manual frequency；time 本来限 AlgorithmEstimated。新增 actor、observationChanged、Manual frequency 三类拒绝用例 | 已关闭；主实施新版 focused 通过，审查者核对通过日志及实际二进制 SHA-256 |

Q1/Q2 修正前没有建立正式 execution freeze，也没有正式新 G/T outcome，因此不需要修订数学门槛或排除输入。准备阶段 manifest 的源码身份必须在正式 freeze 时按最终源码重新核对。

独立 codec 检查使用内存模拟输入，只调用现有 evaluator loader/数学，不写新的结果文件、不运行模型。合法 native raw shape 的非空 r0 已可读取；同步设置 Manual time 后返回 `invalid-result: Algorithm gate requires immutable pending algorithm r0`。执行前后 evaluator SHA-256 均为 `f16c0ee852ef8cedc58a5c8ac6c61532d54fe004f9faeaa97d699a5546bc2390`。7 项 self-check 也在同一前后哈希下退出 0；主实施同项日志为 `out/tasks/wideband-detection/M4/logs/evaluator-final-selfcheck.json`。

## 其他已核对的质量边界

- 源样本使用 uint64 半开区间，边缘差先做整数差再转秒；cell 支持、尾格、ROI floor/ceil 映射和 uncertainty 分开保存。sample mapping 乘加被精确 energy shape 约束在 support 内；fusion 的正一 sample gap 不依赖浮点时间 epsilon。
- exact-length FFT 的 mixed-radix/Bluestein 分支保留原分析长度，scratch 与信号支持分开；预算拒绝在分配前，band-energy 失败/取消不替换输出。非有限功率被拒绝，不被当资源 fallback。
- sample/spectrum 与 separately measured coarse fallback 共用累计 cell 余量；超累计限制是任务错误。`resourceFallback` 目前依赖内部受控错误字符串，未来增加错误来源时宜改为结构化原因，但当前受检路径没有误吞读错、取消或源变化。
- domain/fusion/time publication 用临时输出，失败不发布部分活动；最终源 fingerprint 检查、publication 预算、durable shards/end marker 与 terminal manifest 相互独立。未完成 end marker 的恢复仅恢复已落盘频率证据，不重新计算或伪造精确时间。
- fusion 以整数时间 overlap/touch 和物理载波规则确定性反复合并，使用受限比较次数/工作字节预算；保留 parent snapshots/derivedFrom，唯一 window 取最高 score，union 统计使用冻结 window denominator。R4 修正后的顶层 summary 不再继承单 parent 的 timeAnalysis、seed 或频率测量；各 parent 测量留在 snapshots。
- store v2 的 phase/refined/total 使用独立字段，v1 默认 none/0/0，拒绝 v1 混入 v2 字段；refined 不能超过 total。UI 缓存签名包含这些字段，候选处理数包含 fallback，未把它显示为成功数或 duty cycle。
- evidence trace 只读取保存的有限非负实测 points，最多 512 点，使用相对 uint64 差映射；未计算伪造曲线。融合对象没有统一实测 trace，UI 不继承单 parent 的 noise/threshold。
- reference 不读取 native head 充作完整 reference；generator 先冻结完整 grid、seed/PRNG/runtime/磁盘预算，再生成 sample-exact truth，生成失败不发布 complete manifest。数值诊断分离产品 power、完整 smooth、生产决定与独立参考，并保留超差索引和失败退出码。

新增业务 C++ 文件的通用排版与现有风格一致，主体不超过 80 列。后来 energy parity 工具已进行 Black 80 格式化；少量长证据字符串/注释仍可断行，属非行为格式建议，不作为 QUALITY_FAIL。

## 独立 focused 执行证据

以下均从现有 Debug 部署目录直接执行，没有触发构建。执行前后 SHA-256 相同，退出码均为 0。

| 二进制 | 结果 | UTC 写入时间 | 前后相同 SHA-256 |
|---|---|---|---|
| `SignalStudioDetectionRefinementTests.exe` | 15 groups passed | `2026-10-13T18:40:56.7574496Z` | `2c5e600be69da8126c281e80112a0880d472d30c6fa349c5f758bc7eed3f0fdb` |
| `SignalStudioDetectionRefinedFusionTests.exe` | 9 groups passed | `2026-10-13T18:40:56.5358110Z` | `e3b12b15ad96a793faa891228d01729de5ade1970d05aed31fb8d336153ad386` |
| `SignalStudioDetectionTimePipelineTests.exe` | Time pipeline checks passed | `2026-10-13T19:02:08.9954960Z` | `864f80c35a82dc29cdd987e0fb87670aaee5c88f87a118a7673911a9c39b59bf` |

第三项覆盖 R4 最后 summary 修正，与主实施 `logs/time-pipeline-focused-final.txt` 的通过日志一致；它早于本轮 Q4 的新拒绝用例和 decoder 加严，不能用于声称 Q4 新源码已经执行通过。

03:20 补证：主实施完成新版构建后执行 `SignalStudioDetectionTimePipelineTests.exe`，`out/tasks/wideband-detection/M4/logs/time-pipeline-final-raw-contract.txt` 内容为 `Time pipeline checks passed`。审查者只读核对该日志及当前二进制 SHA-256 `9fee17fd97422009e4bc360edf87082cf8510576ad1f00f99648f723834e4108`，与主实施提供的执行身份一致；UTC 写入时间为 `2026-10-13T19:18:21.0378088Z`。当前 test 源码 SHA-256 为 `7ede0a912107cc0b7865261820308aba77bf92e8b5896706c078524db323dde1`，含 actor=user、observationChanged 与同步修改 current/original/after 的 Manual frequency 拒绝检查，以及正常多活动恢复、fusion scope 与原始活动一致性检查。新版 focused 为 Q4 的执行关闭证据；本次补证没有重跑测试，也没有将之前的旧二进制结果套到新源码。

主实施的 `logs/debug-ctest-final.log` 当前结果为 **22/23**，唯一未通过项 `signal_studio_ui` 在 `300.04 sec` 超时；不能描述为全量通过。主实施记录超时前已完成 88/94 个 UI case 且未出现断言失败，这只限定已完成的部分。03:20 读取 `logs/ui-isolated-ctest.log` 时仍只有 `Start 23: signal_studio_ui`，隔离 UI 的最终结果尚不存在，继续保留为未验证。

## 数值与原生证据仍待完成

本轮只读核验了 `out/tasks/wideband-detection/M4/diagnostics/energy-parity-cpu-math/comparison/energy-parity.json`。该结果明确标 `synthetic-cli-math-fixture-not-G-not-RF`、9999 个真实 fixture source samples、CPU：power 超冻结容差 0，smoothed 超差 1701，同一 native power 经参考 cumsum 超差 1703；mask 差异 0、整数端点相同、状态/原因相同，整体 `pass:false`。**这份 scoped strict parity 为 FAIL**，不能用相同最终端点抵消中间超差，也不能称 G/T 或真实 RF 的通过证据。本审查没有调整容差或算法。

本轮还只读检查了 `tests/ui_capture.cpp` 的 narrowband capture 修正：当实际 NarrowbandWorkspace 可见时，等待当前窄带 charts settled、GPU ready、completed frames 和 texture uploads；宽带路径保持原检查。另核对 `tests/wideband_detection_capture.h:1383` 等处保存重开使用 snapshot 内容值比较；`AnnotationChannelSnapshot` 不包含共享指针身份，序列化后可以按值核验。此前三次原生尝试分别遇到 evidence mode、隐藏宽带 settle、序列化后的 pointer identity 探针问题；主实施报告第三次已实际通过 scene 27 的 CPU DDC 数据与窄带 GPU 显示检查。这些是探针修正与局部执行证据，不是产品 DDC 修复，也不构成完整旅程通过。

34 场景旅程新增 ROI noSplit、edge .75/smooth 2 ms 对应 kernel 11、disabled minOn/minOff 及保存重开证据；原生下一轮由主实施在隔离 UI 后执行，必须使用规定 Redmi 27 NU 第二连接屏、DISPLAY6、3840×2160/150% DPI 与真实全屏。当前代码路径存在不等于这次原生旅程已通过。

M4 节点剩余证据包括：当前项目 UI 超时调查及最终回归结果，第二屏全部 34 场景及保存/接受/人工修订/DDC 来源旅程；冻结 G 10 文件/594 真值与完整 T 的正式配对效果、失败/未知分母、short/CNR 分层与 bootstrap；sample/ROI/Coarse/None/noSplit 及批准扩展参数的同源中间数组/离散边界 parity；连续跨静默反例与资源/取消边界实测。真实 RF 采集域仍缺独立数据证据。以上没有实际最终证据的项继续未完成，不将数学 fixture、旧版回归或 probe 修复替代节点验收，尚未提交 M4。
