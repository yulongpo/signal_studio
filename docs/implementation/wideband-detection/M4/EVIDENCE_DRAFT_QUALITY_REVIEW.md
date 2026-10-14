# M4 evidence-r4 草稿独立质量复审

结论：**DRAFT_QUALITY_PASS（仅当前七文件草稿、隔离 clone 与已结束的聚焦/UI 证据范围）**。未发现本轮证据补项引入的质量阻断。ROI gate 保存、Coarse 数值域与失败回退记录、完整长 seed 的融合 durable fixture 及全零实际 CI16 双重 fallback 组合，在此草稿范围可通过第二阶段复审。**草稿尚未应用到 canonical R3，新 native 旅程未跑，不能据此关闭 canonical 的证据缺项或宣布 M4 完成。** 正式 R3 的 G/sample、G/Coarse 效果失败、G-no-split 实际发布失败和 strict parity 失败继续保留。

审查日期：2026-10-14（Asia/Shanghai）。依据为 `EVIDENCE_DRAFT_SPEC_REVIEW.md` 正文及增量的 DRAFT_SPEC_PASS、`EVIDENCE_COMPLETION_DRAFT.md`、获批参数方案 A，以及 M4 04/05/07 的证据与退出要求。本轮只读 deferred 七文件、对应独立 clone、身份记录、已结束日志与相关正式 scope 摘要，仅新增本报告；没有修改 canonical、草稿源码、freeze、原型或历史结果，没有构建、运行测试/参考/模型/GPU、提交或进入 M5。执行结果均由主实施产生，本审查没有重跑。

## 代码与数据边界

草稿位于 `out/tasks/wideband-detection/M4/deferred/evidence-r4/`，对应 clone 位于 `local-builds/evidence-r4/source/`。以下源码行号指草稿/clone，不指 canonical。审查逐一计算七文件原始字节 SHA，deferred、clone 及 `diagnostics/evidence-r4-current-identity.json` 三者完全相同。该 identity 的 SHA 为 `d0fdb1fef136635e2f63f6ad6c33601a3742be78ba1ab269d722fdf4a295e0c4`。

| 检查 | 具体实现与质量判断 |
|---|---|
| ROI gate 及掩码 | `domain/detection_refinement.cpp:306–348/741–744` 只向 evidence 保存已算出的 positiveMean、offFraction 和两类 gate。三次去均值、裁零、两处原比较表达式、居中 mask、时长筛选及源坐标映射均未改。新字段没有被反过来用作新的门限参数；静态差异支持数学保持，不扩大为全数据逐位执行等价已证明 |
| 保存数值域 | `detection_refinement.h:72–77` 明确是 dB ROI 去均值/截正后的正残差均值，既非校准绝对 dB 功率，也非 sample 线性功率。`time_analysis.cpp:66–71` 保存 `positive-db-residual-mean-v1` 与四项实际值，不借用 noise/comparisonThreshold。四个 optional 在同一 roiProfiles 调用一起写入；数值 0 是“已测得零”，不会因 optional 的条件被误当缺失 |
| Coarse 单位 | `refinement.cpp:813–837` 的 profile 是每帧最强 ceil(5%×bins) 的 powerDb 均值，noise 为该 dB profile 的 q20，threshold 为 noise+6 dB，比较也在 dB 域。UI `:840–848` 的 Coarse 谱密度噪底/门限标签与实际运算一致；没有把内层 Coarse noise 复制进 sample 功率字段 |
| 失败回退记录 | `time_analysis.cpp:301–320` 在实际谱读取成功且 coarse.ok 后，保存内层测量、status、reason、fixed-default policy 与 `spectrum-density-db-v1`。Applied 判定仍单独控制活动复制和 measured-coarse-fallback 文案；Coarse Fallback 只能补证据，不能产生名义活动。输入不足以计算 profile 时，optional 测量保持缺失，状态/原因仍可保存，不伪造 noise/threshold |
| 原子与资源 | Fine-only guard、默认 6 dB/10 ms/20 ms、stored request、split、取消、源指纹和累计 cell 预算路径均保持。新增证据是固定数量 scalar/string，没有新数组或样本缓存；最终 `:364–367` 仍先核验 publication budget，再整体替换。新增 JSON 大小仍进入原有 budget 校验，不绕过拒绝或取消边界 |
| UI/capture | `annotation_evidence_widget.cpp:855–862` 区分 ROI 残差域和线性功率，未增加原型合成曲线。无 trace 仍按现有规则隐藏/注明暂无；内层失败 Coarse evidence 保存在原始字段。capture `:1485–1494` 增加 gate/domain 存在断言，但尚未执行新 native 探针，不能把编译或断言存在算作原生可见验证 |

七文件均未发现超过 80 字符的行或行尾空白；新增字段和原因的作用域清楚。本审查未重跑 clang-format，也不把简单行宽检查称作完整 Google 风格工具复验。未发现新增 uint64 算术、无界循环或资源增长；原有全规模压力/故障执行范围并未因此补齐。

## fixture 是否证明了所声称的行为

`tests/detection_refinement_tests.cpp:238–257` 的 512×64 无插值 ROI 使用独立可手算输入：四分之一为 0 dB，其余 -100 dB，去均值后为 +75/-25，截正后 positiveMean=75、offFraction=.75、time gate=28.125、frequency gate=14.0625。检查活动 `[116,148)`、频带 `[228,484]`，并检查未混入 sample noise/comparisonThreshold。它同时约束数值域和既有物理活动，属于有效行为检查；不是从实现重新算一遍同值的镜像测试。domain 主函数现在包含 17 组。

time-pipeline 的 physical fixture 用 Fs 20 Msps、N=65536、400000 个临时 CI16 samples、合法注册 source/profile 走实际 reader/STFT。两 burst 场景保留直接 Coarse 用户筛选、Fine fixed-default 回退及 stored 参数不变的原断言；ROI 还检查实际生成 JSON 的两 gate/domain。这个生产保存路径当前是 ROI 无可靠 run 后取得 Coarse 活动，Applied ROI 的数值由上述 domain fixture 覆盖；不能将它描述为最新原生 Applied ROI 保存重开已通过。

`tests/detection_time_pipeline_tests.cpp:373–419` 的 silent fixture 实际写出 **1600000 bytes 全零 CI16**，更新源指纹、runId 和 project generation，再重新打开 FixedDetectionSource。Sample/ROI 都执行真实读取与实际独立 Coarse 尝试。两者检查 current/original 活动为空、observation 保留、WindowObservation、外层 Fallback；内层检查 `status=fallback`、`coarse-no-supported-on-run`、实际 noise/threshold、dB 域及 fixed-default policy，且外层不能声称 measured-coarse-fallback 活动。这里的“未知活动时间”不等于 `TimeQuality::Unknown`：模型窗口仍是已知观察支持，WindowObservation 符合原契约。

首轮新增 silent 检查误期望 Unknown，`evidence-r4-silent-fallback-ctest.log` 的 1/2 FAIL 仍保留；当前只改测试期望，其他六个草稿文件与数学没有随之改动。最终 r2 日志记录 **2/2 PASS、1.61 s**，时间 pipeline 1.58 s、refinement .01 s。此证据关闭 silent 双重 Fallback 的实际调用与不可变字段生成边界；不声称 silent 模型效果或 UI 保存重开已验证。输入是合成 IQ 实际文件，不是实采 RF。

融合 fixture `:199–205` 不再丢弃 parent 原始 fields，只替换所需 windows。`:243–271` 在独立临时目录 append→end-marker→recover，检查恢复后 **完整 DTO JSON 精确相等**，另确认两份 parent snapshot 的完整 **223-byte seed**。seed 检查读取的是回环前 fusedFields，但完整恢复相等检查覆盖了相同嵌套字段，因此不是仅 codec 解码或只检查预存值。原/current/r0、parent snapshots、derivedFrom、unique-window accounting 都包含在完整编码相等中。

这关闭此前“完整长 seed 的融合对象未做 durable 回环”这一 fixture 缺口。parents 的活动是构造的发布夹具，第二 parent 从第一个复制并调整；未运行两条真实模型 track 的完整生产链，也没有在该 fixture 新增真实 frequencyAnalysis/timeAnalysis 测量。不能把完整 seed 保留扩大成所有生产复合测量均已执行验证。

## 已结束的构建/测试产物与 UI 结论

本轮在主实施确认 UI 终态后才读取其结果，没有读取或干扰进行中的 UI 执行。现有 clone Release 的完整重链日志与二进制身份支持 UI 已包含最后 infrastructure 改动：

| clone Release 产物 | UTC 写入时间 | SHA-256 |
|---|---|---|
| `SignalStudio.exe` | `2026-10-13T23:25:42Z` | `879ad50287303ea588f6b5758155b6b46822d20f40516c23a3a74c9878e58352` |
| `SignalStudioUiTests.exe` | `2026-10-13T23:25:44Z` | `f33457321d629e6aab435aabf60dff102a82075de6253ace00bc0ca5a2506028` |
| `SignalStudioUiCapture.exe` | `2026-10-13T23:25:45Z` | `1d5bfafdb758f74ac23e3252cc6523d4c8902f2011b1f8d6074868577b0b415d` |
| `SignalStudioDetectionTimePipelineTests.exe` | `2026-10-13T23:27:19Z` | `44fb6806db2f266ce6ef4c5bd761f60785e2ec9e270b5e2622c9b08c34bb8ac3` |

最后 time-analysis 源写入为 `21:46:37Z`，UI 三 targets 都较晚；最新测试源码 `23:27:10Z` 早于聚焦 CLI。build 日志也记录相应重链，不只依赖时间戳猜测。前次 SPEC 正文的 UI 旧产物限制已由增量明确更新，本报告采用当前身份。

`logs/evidence-r4-ui-ctest-300.log` 记录 **1/1 PASS，UI 57.93 s，CTest 总计 57.94 s**；clone `out/vs2026-qt611-release/ui-test-results-release.txt` 为 **94 passed/0 failed/0 skipped，57608 ms**。clone CMake `:345–350` 保留 `QT_QPA_PLATFORM=offscreen`、scale 1.5 与 **TIMEOUT 300**。这是隔离 Release **offscreen 软件 UI** 功能回归，不证明 native GPU、第二块真实屏幕、当前原生 gate 显示/保存重开，也不证明完整项目 23/23 已跑。没有理由将其耗时差异归因于证据字段或据此批准 480 秒预算。

canonical Debug UI 仍由主实施按原 300 秒单独复查；本报告没有其完成结果，不把 clone Release 通过套到 canonical Debug。SCN 参数矩阵例外与 UI 480 秒用户决定仍待确认，草稿没有默认应用两项选择。

关键已结束日志 SHA：silent-r2 CTest `30b2356148476f1bd8e8a9a10b3d4222c85c6902e22963ca7197101e1346944a`；首轮 silent FAIL `ca17aab0eca17537e2d165cc9e578755e12710bb3d02a7eb12349f4734d37036`；完整重链 `da51fae694f037db66452ed72cff3828f85083e1a0455397645880263375c744`；UI CTest `76c2934758b1639f5c96e9747f969c2f85a789440648d3a81b018c074808f318`；UI 明细 `b75355d505fa8153c82dd541f6334e52b47eba4f67d7638a99291afa3259fd42`。

## 草稿身份与 canonical 隔离

| 七文件相对路径 | deferred / clone / identity 相同的 SHA-256 |
|---|---|
| `domain/detection_refinement.cpp` | `86fa33d603276d9c3c62a5115c48c4bb91413772c2dc7dfb060e03d37a05d131` |
| `domain/detection_refinement.h` | `d137990069a730bb8f2dadbb4ffa8fedaccc88d245b0705662d4d91f62e2316b` |
| `infrastructure/detection_time_analysis.cpp` | `2559dd69a79cccee6e839e9da677ed2fc3c0b25776d6d310f0d42a422971df3a` |
| `ui/annotations/annotation_evidence_widget.cpp` | `83c14cc6c8758ea5b076fe6754f7b4a2ae2e27a45c7363557ff6381122cd4c27` |
| `tests/detection_refinement_tests.cpp` | `62aacf3aff831bb86a0b73e083d08e72b516a25ab795fadda14aa0061b673ec8` |
| `tests/detection_time_pipeline_tests.cpp` | `13c14ac7d04d1d025ed82f077e78541396d91c495c4ea525d904e1d2f9cdee5c` |
| `tests/wideband_detection_capture.h` | `550690abe0665ec5171102bffd11b151916280168ceef4c54a0ffb8dc5cc2112` |

canonical refinement cpp/h、projection、task、time_analysis、refined_publication、time-pipeline test 及 Release time-pipeline CLI，共八项抽核仍匹配 R3 freeze；freeze SHA 仍为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`。报告写入后再次核验七个草稿/clone 文件与八个 canonical 条目，身份仍全部一致。本审查未核验全部 72 files/308 data assets，不把抽核扩大成全仓库身份证明。草稿新增证据没有进入正在执行的 R3 数值矩阵。

## 正式验收仍未完成

只读现有正式报告/摘要确认：G/sample 10 对完整组在 TF-IoU=.5 为 Native F1 `.7101449275`、reference `.8260869565`，**L3 FAIL**；G/Coarse 为 `.3874239351` 对 `.6713995943`，**L3 FAIL**。G/ROI 在该配置主 gate 达到门槛（双方 `.8345588235`），但 TF-IoU=.7 仍差一个 TP，不能叫所有中间数值等价，也不能覆盖 G/sample 主 gate 或 T/L4。证据补项不改变未知/失败分母或参考 fallback parent span 的政策，不能靠增加字段消除效果失败。

最新 G-no-split 的 `scope-summary.json`、pair-run-records 与 evaluation 均为 **1 个 Native failed run/file**、reference 0。case `045032-983` 返回 1、未 timeout，约 29.385 s，主实施记录 `Cannot atomically publish detection data: Unknown error` 正在调查；原始 failed result 与零预测/全部真值计账保留。此前主实施曾读混别组摘要，不能据此另报 runner 计数缺陷。本报告不诊断该落盘根因、不声称此 scope 无失败或该发布问题已被七文件草稿关闭。

完整 220 对仍运行；T-short strict parity 的 25 个临界 mask 差异和最后 end +1 source sample 仍为 FAIL，不放宽容差。尚待 canonical 集成与新身份/默认样本核验、最新版 native ROI gate 可见及保存重开旅程、正式完整矩阵/分层/失败与未知计账、需要的全套回归及用户两项决定。新原生 capture 尚未运行；没有提交 M4 或进入 M5。草稿范围无质量阻断与正式退出证据尚未齐全是两项不同判断。
