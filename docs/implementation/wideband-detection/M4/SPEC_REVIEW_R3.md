# M4 R3 独立规格复审

结论：**SPEC_PASS（本次 R3 身份、查找、回退作用域及原始发布契约的已读代码范围）**。没有发现新的实现阻断。长 track ID 的发布故障和 Fine fallback 作用域问题在当前源码范围内关闭；现有聚焦日志及首个正式 G 文件的 Native 输出支持这些修正。**M4 节点仍未完成**：SCN 参数矩阵例外、UI CTest 运行预算、最终版本原生重跑，以及正式 220 对的完整结果仍有待决定或执行。短样本 strict energy parity 已有失败结果，不能表述为数值等价。

审查时间：2026-10-14 05:03（Asia/Shanghai）；基线为 M3 `6ed5ce0338cc2020b8bf6a529874e1cc07166177` 后的 M4 工作区。按已应用的 `subagent-driven-development` 两阶段流程，本报告只承担规格复审。审查者只读代码、冻结身份和现有日志/结果，仅新增本文件；没有修改业务源码、冻结文件、原型、参数或历史结果，没有构建、运行测试、模型/GPU、提交或推送。

## 依据与审查范围

依据为 `docs/plans/wideband-detection/04_ALGORITHMS_AND_VALIDATION.md` 的时间定位、失败政策和 L1–L4；`05_MILESTONES_AND_ACCEPTANCE.md` 的 M4 退出及 A09/A10/A17；`07_PROTOTYPE_AND_INTERACTION_REVIEW.md` 的 P11–P14、S8/I13/I18/I22；已批准原型 `docs/prototype/wideband-detection-r1/` 和用户方案 A `PARAMETER_DECISION.md`。参数语义以用户方案 A 为准，早期 `VALIDATION_PROTOCOL.md` 中冻结 GUI 的旧 ROI/noSplit 描述不能覆盖后来确认。

前次 `SPEC_REVIEW.md`、`QUALITY_REVIEW.md` 的基础审查和关闭记录保留。本轮重新读过 projection/task/time-analysis/refinement/refined-publication 的相关路径、通用 Annotation/DTO 限制，以及当前长 ID、raw r0、融合和实际文件 fallback 测试；不把前次旧二进制或旧截图套用到 R3。

## R3 修正复核

| 项目 | 当前实现与必要边界 | 结论 |
|---|---|---|
| 长 seed 的 typed ID | `detection_projection.cpp:224` 使用 `std::string::size()` 判断 UTF-8 bytes；≤128 原样保留，超限为 `track:` + 完整原字节的 SHA-256，长度 70。`annotation.id` 仍从完整 run/seed 生成；没有截断 tuple，也没有放宽 DTO/domain 的 128-byte 上限 | 代码符合兼容及确定性要求 |
| 完整原始身份 | `trackEvidence:55` 保留完整 `seedId`，`originRef:274` 仍保存 `track:` + raw seed，窗口及模型证据不被短 ID 替换 | 原始证据可追溯 |
| 两处查找 | `detection_task.cpp:378–386` 的 frequency evidence 和 `detection_time_analysis.cpp:212–219` 的 immutable track 查找均比较统一 helper 结果；不是只改 projection 后留下旧 seed 比较 | 长 ID 不会导致频率证据静默丢失或 Fine 找不到 track |
| Fine 回退参数 | `coarseDetectionFallbackParameters:504–512` 从副本设置 Coarse、固定 margin 6 dB/minBurst 10 ms/splitGap 20 ms；保留 split 开关和 stored request。`time_analysis.cpp:295–307` 保存实际 coarse evidence 及 fixed-default policy | 不消费界面禁用的 Coarse 用户历史值 |
| 回退作用域 | `time_analysis.cpp:275–277` 仅允许 Sample/ROI 的 Fallback 进入独立测量 Coarse。直接 Coarse 仍按已启用的 minBurst/gap 计算；拒绝后不会用弱化默认值重跑。None 保持未知时间观察 | 第一次 helper 修正的过宽 guard 已关闭 |
| 真实测量与失败 | 回退活动只能来自实际 STFT 的 Applied 结果。无可靠结果保留 Fallback/未知观察；主分支与回退共用累计 cell 预算，读错、取消、源变化、nonfinite 和累计超限不被转换成成功活动。结果在临时 publication 中完成后才替换 | 未引入名义窗口伪精确框或失败时部分替换 |

## durable 原始 r0 与融合 lineage

`decodeDetectionPublication` 仍要求同 file/sourceSet/run 的 Algorithm、Pending、唯一 model actor 的 `algorithm-origin` r0 与原始 review；不允许人工 name/note/observation 编辑字段、Manual method/frequency、confirmed/deleted/classLabel 或额外修订。原始/current 活动必须相同，r0 changes 逐段 `before=null`、`after=original/current`，观察及活动均为 RF 并在请求物理范围内。解码失败保持目标 publication 不变；没有因长 ID 修正削弱这些检查。

恢复路径 `detection_task.cpp:440–551` 对最终 records 使用上述 decoder，核对唯一对象与 end-marker count。没有完整 end marker 时只重建已落盘的频率窗口证据，不重新读取 IQ 做时间精修，也不冒称恢复了精确活动。

融合对象仍生成合法的定长 `fusion:` record ID，`parentCandidateId` 来自已缩短的 parent typed ID；`derivedFrom` 保留 parent Annotation IDs，`refinedFusion.parents` 使用完整 `encodeAnnotation(parent)` snapshots。顶层仅保存 union windows/hits/完整分母，唯一窗口使用最高分一次，不再将第一个 parent 的 seed、timeAnalysis、noise/threshold 当成融合整体测量。重建最终 original/current/r0 后才发布。

**执行证据必须收窄到实际 fixture。** 当前 `detection_time_pipeline_tests.cpp:42–121` 的 seed 为 210 个 `s` 加 13-byte `-sample-track`，合计 **223 bytes**；原始对象确有 append→end-marker→recover，并检查完整编码相等。`:178–240` 的融合 fixture 检查 parents、window union 和 encode→decode，但 `prepareParent` 将原始字段改为 windows；没有执行包含完整 raw seed 的融合 parent durable 回环。因此本轮只给融合实现静态通过及 codec fixture 通过，不声称该复合 durable 场景已执行。该项为待补验证/表述限制，未发现对应代码缺陷；R3 冻结运行期间不改测试或 CLI。

## 参数契约与用户决定边界

用户方案 A 的 ROI `edgeRatio/.6` 和 `2*floor((5*smoothSeconds/.001)/2)+1`、1–63 奇数核已实现；默认对应五单元。noSplit 不读取用户 minOn/minOff：sample 保留固定 0.5 ms 可靠性，ROI 保留一个池化单元及独立至少三实际帧支持检查，保存 `durationParametersApplied=false`。sample continuous 的默认 `.5` 按 `.5*(edgeRatio/.6)` 可调；`coverage>=.7` 禁拆分及可能跨真实静默的限制仍须在 L4 报告，不把 coverage 叫占空比。

当前 SCN `detection_card.cpp:761–765/814/819–822` 的无效可调 margin 与错误作用说明仍存在；生产 SCN 跳过可调 FFSCN frequency helper，保持固定 3 dB，Sample/ROI 门限不消费该 dB 值，Coarse/真实粗回退固定 6 dB。**不能标全参数契约已通过。** `SCN_PARAMETER_DECISION.md` 已提出禁用无效控件的 profile 例外；获批方案 A 要求保留原矩阵，所以该例外需用户确认。本报告不把等待决定另列为新增产品 bug，也不擅自把 dB 值接入 Sample/ROI 或改变 SCN 数学。FFSCN 的可调 margin 仍用于其实际频率处理，不应随 SCN 一起禁用。

`UI_TEST_BUDGET_DECISION.md` 的 300→480 s 尚未应用，当前 CMake 仍是 `TIMEOUT 300`。全套 UI 的功能诊断通过不能代替既定 CTest 终态；用户预算决定也不能降低断言、数值门槛或原生显示器条件。

## 本轮读取的现有执行证据

| 证据 | 实际结果与范围 |
|---|---|
| `logs/physical-fallback-tests-r3.txt` | `Time pipeline checks passed`；对应源码含临时实际 CI16 文件、Fs=20 Msps、65536 点 STFT、两个 1 ms burst。检查直接 Coarse min50 ms 拒绝不被默认重跑、min10 ms 可测，Sample/ROI 不可靠结果忽略不同 inactive 历史值，stored 不变，证据记录 10/20 ms 固定政策。该合成文件是物理 reader/STFT 集成 fixture，不是 G/T 模型效果或实采 RF |
| `logs/debug-current-non-ui-ctest.log` | 最新 Debug 非 UI **22/22**，135.73 s；含 refinement、time pipeline、fusion、reader/store/应用回归。不能写成全套 CTest 通过。当前 domain 主函数包含 16 组；较早 `fallback-parameter-tests.txt` 仍打印 15 组，本报告不把旧日志包装成新 16 组独立执行输出 |
| `ui-complete-diagnostic.txt` | **94 passed/0 failed/0 skipped，389.561 s**。是现有 runner 的完整功能诊断；300 s CTest 超时仍保留 |
| `diagnostics/suite-release-r3/G-sample/20260728-044906-196_bw_20000000_sampleTime_0.8_rollOff_0.35/native/result.json` | 首个 Native `pass=true`、`exactRecovery=true`，61 objects/77 activity segments，elapsed 365.491 s。61/61 typed IDs 为 70 bytes，完整 seed 185–189 bytes，全部保留 frequencyAnalysis/timeAnalysis，0 fused objects。支持长 ID 的实际生成、两处查找和原始 durable 恢复；不覆盖融合 parent durable，也不是完整效果 gate |
| `diagnostics/energy-release-r3/*/reference/energy-parity.json` | G-arbitrary/G-carrier1 两个预选区间通过。T-short 的 power/smoothed 数组超容差数均为 0，但 mask 差异 25（noncritical=0），8 对 segments 中一处 end 为 +1 source sample，`pass=false`、`exactIntegerEndpoints=false`。临界诊断不提供豁免，不以 50 ns 或相同段数宣称 strict parity 通过 |
| `NATIVE_VALIDATION.md`、`native-report.json`、旧 PNG | 34/34 原生旅程记录第二连接屏 2 / Redmi 27 NU / DISPLAY6、3840×2160、150% DPI、全屏，QRhi D3D11 与纹理上传。版本早于新 ID/fallback 修正；明确保留，最终版本仍待重跑 |

上述日志/结果均由主实施先前或当前执行产生，本审查没有重跑。正式 R3 suite 尚在进行；本报告不根据首文件非空、process pass 或未完成 pair 判定 G/T L3/L4。

## 冻结身份

`execution-freeze-r3-release.json` 的 SHA-256 为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`，`readyForInference=true`，记录 72 个 files、308 个 data assets，以及 220 对矩阵；新 suite-plan 引用相同 freeze SHA。它明确披露已读旧结果后的 ID/作用域修正、R2 失败保留和 `product-v2`，没有换权重、数据、真值、门槛或容差。

本轮重哈希以下源码/可执行文件，与 R3 freeze 全部一致；没有重新计算 308 个数据资产，也不将其先前主实施核验冒称本审查重新完成。检测卡不在该算法 CLI freeze 的 files 中，单独读得当前 SHA `f77c1fb46581ed552eb0819131e7d27fc444f8ce208b8b9f256564290a3b9160`，不把算法 freeze 当作全 GUI 身份锁定。

| 文件 | 当前 SHA-256 |
|---|---|
| `domain/detection_refinement.cpp` | `4b5b1231a6eaa73502b76361ea78bd5f323750e8678e6e7202c4a3d1f4be089a` |
| `infrastructure/detection_projection.cpp` | `b36bd47d8c22cb4dcf8445d7b389cf954edbc243883c766e9e275a7eec11de69` |
| `infrastructure/detection_task.cpp` | `417494ecb2d70cf1e3b4db5faab6f4d47fd753b3c38b7586a5cacc6a432de6bf` |
| `infrastructure/detection_time_analysis.cpp` | `7e05dc86747bd5a03ea9de6acfe4c56591cf45d829dc8b244936f2e3546c29aa` |
| `infrastructure/detection_refined_publication.cpp` | `1ecbad4537280210e464e8437a26b6a1cbfe021e4fa8a76cdd4572fdcfefa2bf` |
| Release `SignalStudioDetectionTimePipelineTests.exe` | `61db19667a989701109f79bc508998ef35ce92925ec696aaec55edd69b897552` |
| Release `SignalStudioDetectionSampleEnergyTests.exe` | `aa9f88f3df19107a77ff6aa26c8a91bd1088280aa1cef5fc407a8269555355a8` |

## 完成判断与允许未完成项

本次 R3 修正没有新增阻断。M4 全部完成/本地提交/进入 M5 的判断仍须由主实施在用户决定和最终证据齐全后给出，不能由本报告的 scoped PASS 推导。SCN 全参数契约尚未关闭；既定 UI CTest 尚未通过；新身份的原生旅程、220 对完整评价及必要 parity/continuous 反例未完成。等待决定和仍在运行的证据本身不等于新算法缺陷。

04/05 明确允许 L3/L4 目标未达到时如实记“相应时间能力未完成”，不强制通过手工修边或改阈值达标。正式报告仍须完整保留 G/T 所有文件、未知/失败分母、short/CNR 分层、配对 bootstrap 和失败样例；当前未完成矩阵不能提前用此条当作 gate 结论。strict parity 失败独立保留，不能被效果或允许 L3/L4 未完成条款抵消。

真实 RF holdout 缺失、HY 旧 1000 文件 replay 未恢复、FFSCN-17 实验 shape、全规模资源/故障压力与 M5 A16 综合旅程，继续按各自阶段和可用性标未完成，不要求本次最小 ID/fallback 修正伪造完成。旧 R2 的三个发布失败和中断历史保留于 `PUBLICATION_FAILURE_AUDIT.md`/旧 diagnostics，不能被 R3 首文件成功覆盖，也不能与 R3 结果混成同一冻结实验。

## 附录 A：ROI 证据曲线及实际门限保存补核

补核时间：2026-10-14 05:11（Asia/Shanghai）。仅追加本附录，未改变当前冻结源码、原型、算法、参数或正在运行的矩阵；未构建或执行测试/模型/GPU。本次扩大检查的是 ROI 证据完整性，正文的 R3 最小修正 scoped PASS 不代表本附录的证据项已经完成。

**结论分开记录：缺少 ROI 曲线本身不足以判定算法或 P05/P17/I18 失败；当前有明确“未保存/暂无”说明，六类时间依据仍可区分。但 ROI 实际自适应门限没有保存，是规划中明确的证据记录缺项，不能称现有 scalar/mapping 已满足全部 M4 证据要求。** 本项为 `ROI_EVIDENCE_INCOMPLETE`，不判 ROI 数学错误，也不据此停止或重启当前同数学矩阵。

### 原型与规划实际要求

| 原文依据 | 对本疑点的含义 |
|---|---|
| `07_PROTOTYPE_AND_INTERACTION_REVIEW.md:19/31`：P05 保留三图及共同物理投影；P17 增加 sample/ROI/fallback 卡与真值对照状态 | 没有逐方法指定必须存储/绘制哪一种 ROI profile，也没有定义 ROI dB noise trace 的算法 |
| `07:73/119`：六类结果必须看出时间依据；I18 要求“不伪造时间；方法、原始/当前值、失败原因可查看” | 核心是区分 method/status、几何及失败语义，不是要求所有方法显示同一曲线 |
| 原型 `README.md:5/89`、`app.js:80–81`、`data.js:23/30` | I18 演示列有“噪底可查”，ROI 文案为“合成 ROI STFT 噪底轨迹 / 非实测”；但所有 Algorithm 卡复用同一个 `noiseTrackDb` 数组和 `noiseSvg`，甚至未知时间观察也画同一模拟曲线。原型没有 ROI 专用测量 schema，不能把这组合成 dB 值当成真实 ROI 算法规范 |
| `02_UX_AND_REVIEW.md:89/91`：选中算法标记可查看噪底门限、时间能量轨迹；详情区分 sample/ROI/fallback | 确立证据查看能力，但累积谱/时间能量等项目应按实际算法来源解释，不能从列表推导每种方法必须产生同一物理量。没有依据新增 ROI dB 噪底算法或把 ROI profile 塞进 sample 线性功率图 |
| `05_MILESTONES_AND_ACCEPTANCE.md:71` 的 A16、`07:135/139` | 原型模拟功能必须真实实现或明确标未完成；“暂无”能够诚实标示缺失，不能把未实现的预览计作已通过 |
| **`04_ALGORITHMS_AND_VALIDATION.md:76`：“每一方法记录有效参数、门限、最短开/关持续时间、滤波/窗支持、精修状态及失败原因”** | 与曲线格式无关；ROI 的实际 decision thresholds 也应记录，不能以参数或“自适应门限”名称替代计算值 |

### Qt 当前可见内容与未保存说明

`annotation_evidence_widget.cpp:239–266` 区分 window/coarse/sample/ROI/fallback/manual；`:683–740` 显示时间质量、保存的不确定度、原始/当前活动及观察、method/status/reason。`:278–281` 明确写“未保存的精修数据、噪底或轨迹显示暂无”。因此 ROI 缺 trace 时有明确限制，未绘制伪造曲线或伪装 sample 功率。

曲线控件 `:315–351` 只接受 `measured-causal-mean-power-v1` 的合法保存点；无 trace 时隐藏。`:384` 的“已保存的实测包络 / 线性功率”只在实际图可见时出现，不能据这个标签认定 ROI 已有实测功率曲线。`:831–847` 对 ROI 当前显示“暂无噪底、门限、时间能量或轨迹预览数据”，再补“ROI 自适应门限 · N 单元平滑”；这里后半句是规则/核大小说明，前半句已披露没有门限测量预览。

当前 `timeAnalysis` 确有真实 STFT 支持 origin/step/end、FFT/量化支持、effectiveEdgeRatio/smooth/min on/off、roiSmoothingCells、duration/splitting flags 和 numericalVersion；它们与保存的 RF/样本活动、model/window/run 来源一起，足以支撑 I18 对方法、原始/当前值和时间依据的区分。它们不等于 ROI 曲线或实际自适应门限值。

### E1 / P2：ROI 实际 decision thresholds 未进入原始证据

位置：`domain/detection_refinement.cpp:306–342/729–738`、`infrastructure/detection_time_analysis.cpp:30–65/321–330`。

`roiProfiles` 当前计算 `positiveMean`、`fractionOff`，实际时间 gate 为 `fractionOff * .5 * positiveMean * (edgeRatio/.6)`，频率 gate 为 `fractionOff * .25 * positiveMean * (edgeRatio/.6)`，随后执行 centered mask。函数只返回 `timeOn/frequencyOn`；这些实际 gate 值没有写入 `DetectionRefinementEvidence`。ROI 成功分支也没有设置 `noise/threshold/comparisonThreshold`，所以 JSON 保存时没有任何实际 ROI time/frequency threshold。保存的 `coarseDefaultNoiseMarginDb=6` 是不同回退政策，frequencyAnalysis 的频率修边门限也是另一阶段，均不能充当该 ROI 门限。

既有真实原生归档 `native-report.json:752–778` 的 `roiActualTimeEvidence` 也确认这一点：包含 `.75`、2 ms、kernel 11、physical-source-STFT-cells、0–400000 source support 等，没有 adaptive time/frequency threshold 或 trace。原生探针 `wideband_detection_capture.h:1485–1495` 只检查核/参数/noSplit 生效并选择 Applied 对象，没有检查这些实际门限保存；34/34 不覆盖此缺项。该证据仍属于正文所注明的旧版本边界。

符合现有数学的补足是保存已经算出的 ROI 时间/频率 gate、相应数值域及 pooling decision 定义，让原始字段或证据卡能查到它们；这不需要新增噪底算法，也不以生成另一条 sample 功率曲线为必要条件。若以后增加 ROI profile 预览，应保存实际 ROI 数据、物理映射和明确单位/变换，不使用原型默认 dB 数组或 sample trace schema 冒充。

本审查不修改冻结文件。E1 应在节点证据清单中明确未完成，不能仅以“暂无”关闭“每一方法记录门限”的规格；后续补足或获批后置须有独立身份与相应保存/重开检查。曲线未保存则继续作为可见限制，不单独新增算法故障或重跑效果矩阵的理由。
