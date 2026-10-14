# M4 R3 独立质量复审

结论：**QUALITY_PASS（仅 R3 的 track ID 映射、两处身份查找、Fine 回退参数及回退作用域修正代码范围）**。未发现需要停止当前冻结矩阵的新算法缺陷。先前长 ID 发布故障、inactive Coarse 历史值影响 Fine、直接 Coarse 被默认参数二次覆盖三个问题，在本轮已读代码及聚焦证据范围内关闭。**M4 节点仍未完成，不能据此提交 M4 或进入 M5。** ROI 实际门限尚未保存、SCN 无效参数控件例外待决定、既定 UI CTest 未通过、新版本原生旅程待重跑、正式 220 对未完成及 strict parity 失败，均保留在完成边界内。

审查日期：2026-10-14，结果读取截止 05:16（Asia/Shanghai）。本轮为 `SPEC_REVIEW_R3.md` 之后的顺序第二阶段质量复审；基线为 M3 `6ed5ce0338cc2020b8bf6a529874e1cc07166177`。只读源码、现有日志、JSON 和文件身份，仅新增本报告。审查者没有修改源码、冻结文件、原型、参数、历史结果，没有构建、运行测试、参考/模型/GPU、提交或推送。下面的执行结果均来自主实施既有或正在进行的运行，不是审查者重新执行。

## 代码范围与关闭依据

| 检查项 | 当前行为及边界 | 质量判断 |
|---|---|---|
| 长 seed 与 DTO 上限 | `infrastructure/detection_projection.cpp:14–18/224–226/254` 按完整 `std::string` bytes 计算 SHA-256；≤128 bytes 保留原值，超限使用 `track:` + 64 hex，合计 70 bytes。未截断原 seed，未放宽 domain/DTO 的 128-byte typed ID 限制。Annotation ID 仍依完整 run/seed 生成 | 修正直接命中 R2 的恢复失败原因，短 seed 兼容；无新的数值或模型作用 |
| 完整身份与查找 | `trackEvidence:55` 和 `originRef:274` 保留完整 seed。`detection_time_analysis.cpp:212–219` 与 `detection_task.cpp:378–386` 都调用同一 helper，避免 projection 短化后时间匹配失败或频率证据静默丢失 | 两个下游消费者一致；长 ID 原始对象可追溯 |
| Fine 回退有效值 | `domain/detection_refinement.cpp:504–512` 从参数副本固定 Coarse 的 margin 6 dB、minBurst 10 ms、splitGap 20 ms；不修改 stored request，保留 split 开关。`time_analysis.cpp:302–307` 保存实际 Coarse evidence 及 fixed-default policy | 用户先编辑 Coarse 再切 Fine 的 inactive 历史值不再影响测量回退 |
| 回退作用域 | `time_analysis.cpp:275–277` 只允许 Sample/ROI 的 Fallback。直接 Coarse 仍使用自己的有效 minBurst/gap；None 保持未知时间观察。直接 Coarse 的拒绝结果不能再用默认值重跑 | 已关闭第一次 helper 修正产生的过宽 guard；不改变直接 Coarse 的用户筛选语义 |
| 失败、资源与原子替换 | readEnergy/readSpectrum 共用累计 cell 计数，先检查差值再计入，保留工作内存检查；取消、读错、源变化及 nonfinite 不转换为可靠活动。Coarse 只有 Applied 才提供 fallback 活动。`time_analysis.cpp:205/346–355` 在副本完成全部精修、源指纹及 publication budget 检查后才替换 | 当前 ID/参数修正未削弱累计资源限制、取消语义或 publication 原子边界；不以未跑的全规模压力测试作证明 |
| raw r0 和 lineage | 原始 decoder 继续限制 Algorithm/Pending、唯一 model actor 的 algorithm-origin r0、原始/current/changes 一致，拒绝人工编辑及 Manual frequency 等。融合保留完整 parent snapshots、derivedFrom、唯一窗口最高分和原分母；顶层不再继承某个 parent 的 seed/timeAnalysis/频率门限 | 本次短 ID 没有放松原始发布契约；融合代码静态范围通过，复合 durable 执行限制见后文 |

默认 6 dB/10 ms/20 ms 的计算值没有改变；改变的是 Fine fallback 对 inactive 历史值的隔离及允许回退的分支集合。获批方案 A 的 ROI `edgeRatio/.6`、1–63 奇数核和 noSplit 不读取 minOn/minOff 继续保持；sample continuous 使用 `.5*(edgeRatio/.6)`，`coverage>=.7` 禁拆分及可能跨真实静默的限制仍须报告。没有依据为了本轮质量复审调整 UI 矩阵或数学门槛。

修改集中在 helper 和清晰的分支 guard，没有引入第二套 seed 映射或隐藏参数改写。主实施报告 30 个 C++ 文件通过 Google clang 格式、5 个 Python 工具通过 Black80、PowerShell AST 与 `git diff --check` 通过；本审查读过对应状态/日志，没有重新执行格式命令。较早 `google-style-final.txt` 的 28 文件结果不用于冒称当前 30 文件的独立复验。

## P2：ROI 实际门限证据仍未完成

`SPEC_REVIEW_R3.md` 附录 A 的 `ROI_EVIDENCE_INCOMPLETE` 经源码复核成立。`04_ALGORITHMS_AND_VALIDATION.md:76` 要求“每一方法记录有效参数、门限”。当前 `roiProfiles` 在 `domain/detection_refinement.cpp:306–342` 计算实际时间 gate `fractionOff * .5 * positiveMean * (edgeRatio/.6)` 和频率 gate `fractionOff * .25 * positiveMean * (edgeRatio/.6)`，只返回布尔 profiles。ROI 成功分支 `:729–766` 保存有效参数、核大小、物理支持和最短开/关支持，没有保存两个实际 gate；`DetectionRefinementEvidence` 和 `time_analysis.cpp:30–65/321–330` 也没有 ROI 专用的实际 time/frequency threshold。

这是不可变证据记录缺项，未发现因此改变 ROI 活动几何的数学错误。`coarseDefaultNoiseMarginDb=6` 属于回退政策，frequencyAnalysis 门限属于前一阶段，都不能替代 ROI gate。`ui/annotations/annotation_evidence_widget.cpp:278–281/831–847` 明示“暂无”，没有画伪造曲线；“ROI 自适应门限 · N 单元平滑”只是规则说明，不能算保存了测量值。旧 34 场景只验证 ROI 参数/核/noSplit，未覆盖两个实际门限。

本轮 scoped code PASS 不关闭该项，也不表示所有 M4 证据要求通过。后续应保存已经计算出的两类 gate、其均值消除/裁零/池化数值域及 decision 定义，并验证原始保存、重开与详情一致；不新增未经批准的噪底算法，不把 ROI 值塞入 sample 线性功率 trace，也不使用原型合成 dB 曲线。该项不要求停止当前同数学矩阵。若实施业务补足，须由主实施按新身份、新 freeze 及完整默认样本核验管理；本审查不改 R3 冻结源。

## 现有执行证据及其实际覆盖

| 证据 | 读取到的结果 | 不可扩大到的范围 |
|---|---|---|
| `physical-fallback-tests-r3.txt` | `Time pipeline checks passed`。源码 `tests/detection_time_pipeline_tests.cpp:243–333` 使用合法注册 source/profile、Fs 20 Msps/N=65536、400000 个实际临时 CI16 samples，含两个 1 ms burst；直接 Coarse min50 ms 空、min10 ms 非空；Sample/ROI minOn50 ms 触发真实 Coarse fallback，两种 inactive 历史值活动一致，保存 10/20 ms policy，stored request 不变；同时走长 seed 查找 | 是实际 reader/STFT 集成夹具，输入为合成 IQ，不执行模型，不能替代 G/T 效果或实采 RF |
| `debug-non-ui-ctest-r3.txt` | 当前 Debug 非 UI **22/22，0 failed，135.73 s**；与 `logs/debug-current-non-ui-ctest.log` 的字节 SHA 一致 | 不包含 UI，不能叫全套 CTest 通过 |
| domain 检查 | 当前源 main 含 16 组；主实施报告 Debug/Release 16 组通过 | 较早 `fallback-parameter-tests.txt` 打印 15 组，不冒称它独立证明新 16 组运行 |
| `ui-complete-diagnostic.txt` | **94 passed/0 failed/0 skipped，389.561 s**，完整 runner 功能诊断完成 | CMake 仍 `TIMEOUT 300`，此前 CTest 超时没有变成通过；480 s 未获批/未应用。该次机器负载不支持独立性能通过结论 |
| `NATIVE_VALIDATION.md` / `native-report.json` | 旧 **34/34**，连接屏 2 / Redmi 27 NU / DISPLAY6，3840×2160、DPR 1.5、全屏，QRhi D3D11/实际纹理；真实 DDC/narrowband 图谱数据与 GPU 可见证据保留 | 版本早于 ID/fallback 修正，不能作为最新包原生通过；probe settle/pointer 比较修正是验证工具修正，不称产品 DSP 修正 |

原始长 seed fixture 为 **223 bytes**，包含 append→end-marker→recover，检查完整编码相等；不同末尾 seed 与短 seed 兼容均有聚焦覆盖。融合 fixture `:178–240` 检查 parent lineage、union accounting 和 encode→decode，但 `prepareParent:199–202` 将 `originalFieldsJson` 换为 windows。它没有证明包含完整 raw seed/frequency/time evidence 的融合 parents 经 durable store 的独立恢复。

这项是验证范围限制，未发现对应融合实现错误。后续具体 fixture 应保留真实 parent 原始字段，只向字段对象合并必要 windows，制造实际可融合事件，再 append/end-marker/独立 recover；核对顶层与每个 parent snapshot 的完整 seed、typed ID、frequency/time evidence、derivedFrom、原始/current/r0 及 unique-window 分母编码相等。当前冻结期间不修改 fixture/CLI，不把现有 codec 回环改名为 durable 通过。

## 正式 R3 结果与 strict energy parity

05:13 读取的 G-sample 配对记录已有四个完整 pair。下表 Native 和 reference 的 result 文件原始 SHA 均与对应 stage-completion 记录一致；每个 stage 返回 0，`timedOut=false`，两侧 `pass=true`，Native `exactRecovery=true`、run lifecycle 为 Completed。这些是实际输出/保存恢复成功，尚不是 L3/L4 效果 gate 结论。

| G 文件时间标识 | Native objects / activity segments | typed ID | 完整 seed / frequencyAnalysis / timeAnalysis | fused objects |
|---|---:|---|---|---:|
| `044906-196` | 61 / 77 | 全部 70 bytes | 全部保留，seed 185–189 bytes | 0 |
| `044926-842` | 55 / 75 | 全部 70 bytes | 全部保留，seed 185–189 bytes | 0 |
| `044936-468` | 9 / 8 | 全部 70 bytes | 全部保留，seed 186–188 bytes | 0 |
| `044956-845` | 84 / 99 | 全部 70 bytes | 全部保留，seed 185–189 bytes | 0 |

来源为 `out/tasks/wideband-detection/M4/diagnostics/suite-release-r3/G-sample/pair-run-records.json` 与各 `native/result.json`、`reference/result.json`。四个均没有融合对象，因此不能补足上述融合 parent durable 场景。正式矩阵仍在进行；不把剩余未完成 pair 计成功，不把 resource 中 whole-device GPU memory 当成本进程分配量，不把前四个运行通过当作全部 220 对完成或 bootstrap/effect 通过。

| `energy-parity-r3` 固定范围 | FFT length | power / smooth 超容差数 | mask / 整数端点 | strict 结果 |
|---|---:|---:|---|---|
| `G-carrier1.json` | 1000000 | 0 / 0 | mask 0；最终端点完全相同 | PASS |
| `G-arbitrary.json` | 499999 | 0 / 0 | mask 0；最终端点完全相同 | PASS |
| `T-short.json` | 800000 | 0 / 0 | mask 25，noncritical 0；8 段数量相同，最后 end +1 source sample（50 ns） | **FAIL** |

三个输入都是生成 IQ。T-short 明确 `pass=false`、`exactIntegerEndpoints=false`；临界分类仅解释差异，不豁免 strict mask 或端点一致性。不能用 L4 的 1 ms 目标抵消 50 ns 的 discrete parity 失败；相同段数和数组容差内也不等于离散等价。早期 9999 点 CPU 合成诊断的 rolling/cumsum 超差仍保留 FAIL，没有被两个 G 局部范围覆盖。`ENERGY_VALIDATION_R3.md` 对这些限制的表述与 JSON 相符。

## 冻结与证据真实性

当前 `execution-freeze-r3-release.json` SHA-256 为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`，声明 72 个文件、308 个数据资产、220 对矩阵及 `readyForInference=true`。它披露已读取旧 outcomes 后进行 ID/回退作用域修正，保留 R2 snapshot，未改权重、真值、数据、默认数学、协议 targets 或容差。R2 三个 Native missing-result 失败及第四次中断历史不能被 R3 成功覆盖或混为同一冻结实验。

本审查重新计算 14 个相关代码/工具/二进制的原始字节 SHA，与 freeze 全部匹配：refinement cpp/h、projection cpp/h、time_analysis、refined_publication、detection_task、time_pipeline/refinement tests、三个 `tools/detection/time_*.py` 及两个 Release 聚焦 CLI。报告写入后再次核验这 14 项，仍全部一致。未重新计算全部 72 文件或 308 数据资产；此前数据核验是主实施证据，不冒称本审查独立重复。检测卡属于 GUI，不在该算法 CLI freeze 内；算法 freeze 也不能证明整个 GUI 包身份。

| 关键身份 | SHA-256 |
|---|---|
| `domain/detection_refinement.cpp` | `4b5b1231a6eaa73502b76361ea78bd5f323750e8678e6e7202c4a3d1f4be089a` |
| `infrastructure/detection_projection.cpp` | `b36bd47d8c22cb4dcf8445d7b389cf954edbc243883c766e9e275a7eec11de69` |
| `infrastructure/detection_time_analysis.cpp` | `7e05dc86747bd5a03ea9de6acfe4c56591cf45d829dc8b244936f2e3546c29aa` |
| `infrastructure/detection_refined_publication.cpp` | `1ecbad4537280210e464e8437a26b6a1cbfe021e4fa8a76cdd4572fdcfefa2bf` |
| Release `SignalStudioDetectionTimePipelineTests.exe` | `61db19667a989701109f79bc508998ef35ce92925ec696aaec55edd69b897552` |
| `physical-fallback-tests-r3.txt` | `67ac10bfbf2f67cf0e396609fa150491480cfef34fd2d382f5ba6f389f1ea7eb` |
| `debug-non-ui-ctest-r3.txt` | `816c4756b4df5118909be823b1e06045d34386c062b39f45f773b033fd59f8a0` |

审查中发现的原型指纹表述歧义已关闭。原获批 M0-P `artifact-manifest.json` 本来为文本声明 `hashMode=utf8-lf`、二进制 raw。按这个既有 policy 独立核验为 **22/22 canonical 指纹一致**；working raw 为 **18/22**，app.js/data.js/README.md/styles.css 是 CRLF，归一 LF 后一致。主实施已仅修正 `SELF_REVIEW_R3.md` 和 `prototype-current-check.json`，明确区分 canonical 与 working raw；没有修改原型，也没有新增归一策略来放过内容差异。这两个说明文件不在算法 freeze 内。当前 check v2 SHA 为 `c690e541358f2caa1d50852336cab2604ccc8281807d704c9387e315ac3d9206`，不再宣称 22 项 raw bytes 全相同。

## 节点完成边界

本报告没有新增 R3 算法运行阻断。以下事项仍不能计为完成：ROI 实际 gate 保存、SCN 全参数契约、既定完整 UI CTest、新 ID/fallback 包的最终 34 场景、全部 220 对及相应分层/失败/未知分母/配对 bootstrap、必要 continuous 反例、strict 数字等价及完整 raw-seed 融合 parent durable 验证。SCN 当前 Sample/ROI/None 的 margin 可编辑且文案声称生效，但数学不使用；直接 Coarse 控件已禁用，实际固定 6 dB。该已知限制等待用户矩阵例外决定，不擅自新增 SCN 数学或扩大到 FFSCN 禁用。

规划允许 L3/L4 目标未达时如实记相应时间能力未完成；只有完整评价后才能作该结论，不能提前借此消除当前未完成矩阵，也不能消除 strict parity 失败。真实 RF holdout、旧 HY replay、全规模资源/故障压力及 M5 A16 综合旅程继续按其阶段标未完成。前次审查及 R2 失败档案保留，当前 scoped QUALITY_PASS 只代表本轮最小代码修正可通过第二阶段复审。
