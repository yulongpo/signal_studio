# M4 evidence-r4 草稿独立规格复审

结论：**DRAFT_SPEC_PASS（已读七文件及现有聚焦证据范围）**。E1 的 ROI 实际门限保存和包含完整长 seed 的融合 durable 回环，已在隔离草稿范围补齐；未发现新增算法或发布行为阻断。**canonical R3 尚未应用，不能据此关闭 canonical 的缺项或宣布 M4 完成。** 最新测试 CLI 已包含最后的回退证据修正；UI 文案编译通过，但现有 UI 可执行文件早于最后 infrastructure 改动，新原生旅程未执行。

2026-10-14 05:56（Asia/Shanghai）。审查者只读 deferred 草稿、独立 clone、canonical 差异、现有日志及 G/sample 归档，仅新增本报告；未改源码/冻结/原型/参数，未构建、运行测试/模型/GPU、提交或应用草稿。依据为已批准 04/05/07、用户方案 A `PARAMETER_DECISION.md`，以及 `SPEC_REVIEW_R3.md` 的附录 A。

## 草稿与 canonical 边界

草稿根为 `out/tasks/wideband-detection/M4/deferred/evidence-r4/`，独立 clone 为 `out/tasks/wideband-detection/M4/local-builds/evidence-r4/source/`。本轮七个文件的 SHA-256 在草稿与 clone 中逐一相同。以下行号均指草稿/clone，不指尚未应用的 canonical。

canonical 的 refinement/projection/task/time-analysis 源码及 Release time-pipeline CLI 重哈希仍匹配 R3 freeze；freeze 自身仍为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`。本轮只抽核这些相关条目，没有重新核验全部 72 files 或 308 data assets。草稿不参与正在执行的 R3 矩阵。

## 规格逐项复核

| 项目 | 草稿实现、实际覆盖与判断 |
|---|---|
| E1：ROI 实际门限 | `roiProfiles:306–345` 保存已经计算的 positiveMean/offFraction，以及 time/frequency gate；原三次去均值、ReLU、比较表达式、居中 mask、时长过滤和物理映射不变。`evidenceJson:66–71` 保存四项实际值及 `positive-db-residual-mean-v1`。**E1 实现范围在草稿中关闭**；不把它叫 sample 线性功率或校准 PSD 噪底 |
| 独立数值期望 | 新 `roiGatesRecordTheMeasuredResidualDomain` 使用无插值的 512×64 格，一季度为 0 dB、其余 -100 dB。手算 mean=-75，ReLU 后 positiveMean=75、offFraction=.75、time gate=28.125、frequency gate=14.0625；检查同一物理活动 `[116,148)`、频带 `[228,484]`，并拒绝将其保存为 sample noise/comparisonThreshold。不是只镜像实现的空断言 |
| 实际文件到 JSON | physical fallback fixture 经实际临时 CI16 reader/STFT，检查 ROI 的保存 JSON 存在两门限及 domain。当前这项用 Fine 无可靠 run→Coarse 活动的路径；Applied ROI 的数值由 domain fixture覆盖，生产 Applied ROI 保存/重开及可见 UI 仍待新版原生旅程 |
| 融合 durable 缺口 | `prepareParent:199–205` 保留原始 fields、仅替换 windows。`:243–271` 新独立临时目录 append→end-marker→recover，要求全部 DTO JSON 精确相同，再确认两份 parent snapshot 的完整 223-byte seed。检查原 fields 后执行完整回环，不再是仅 codec 解码。**此前复合 durable 验证缺口在草稿聚焦检查中关闭**；不扩大为两条真实模型 track 的全链效果证据 |
| Fine→Coarse 无可靠活动证据 | `time_analysis:309–316` 在 Applied 判定外保存实际 coarse evidence、fixed-default policy、status/reason 和 `spectrum-density-db-v1`；`:317–319` 仍仅在 Applied 时复制活动。只有真实谱读取并成功完成 coarse 分支才保存该记录，读错/取消/预算失败仍走原错误政策。没有把 fallback 的名义 parent 支持伪装为活动 |
| 参数与状态 | Fine-only guard、固定 6 dB/10 ms/20 ms、stored 配置及 split 开关保持；直接 Coarse 的用户时长筛选不被默认重跑。外层 method/status 保留 Fine 尝试，内层 coarse status/reason 表达独立回退测量，二者作用域清楚 |
| UI/capture | `annotation_evidence_widget:840–862` 显示 ROI 两门限并说明去均值 dB 域、不是线性功率噪底；Coarse 的已有 noise/threshold 显示为 dB 域。capture 草稿检查 ROI 两值和 domain，但尚未执行；不能将 assert 存在或 UI 编译当截图通过 |

新增字段只记录现有计算值。diff 中没有修改模型、真值、阈值参数、mask 比较、活动生成、评价器、容差或 fallback 活动政策；静态数学保持不变，不将这一结论扩大为全量数据执行等价已证明。

## Coarse 的真实数值域

`domain/detection_refinement.cpp:813–838` 先取每个实际帧带内最强 ceil(5%×bins) 个 `powerDb` 值，在 **dB 域求均值**形成 temporal profile；noise 为这个 profile 的 q20，threshold=noise+既定 6 dB，mask 比较也是 dB 值。它不是带限线性功率，也不是在线性功率中求 q20 后转 dB。

所以 UI 的“Coarse 谱密度噪底（dB 域）”和“实际比较门限（dB 域）”方向正确。ROI 的 `positive-db-residual-mean-v1` 则是从 dB ROI 去均值、截正后所得均值 gate，不能和绝对谱密度 dB 或 sample 线性功率混为一个量。原型的通用模拟 noise curve 不定义这些真实数值域；本草稿未新增 ROI 曲线，仍保留无 trace 时的暂无/隐藏说明。

Fine fallback 的 dB 测量保存于 `timeAnalysis.fallbackCoarseEvidence`。主卡仍展示原 Fine 的测量域，内层记录可在不可变原始字段查看，不把内层 coarse noise 复制到 Sample 的线性功率字段。没有发现新的单位串用。

## 现有执行证据与版本限制

`logs/evidence-r4-final-actual-ctest.log` 在独立 clone 中记录 **2/2，1.21 s**：time-pipeline 1.18 s、refinement .01 s。结合当前测试源码，包含 17 个 domain 行为组、原始/融合 durable 回环、实际文件 fallback 参数/ROI 门限保存检查。较早文档中的 1.26 s 属于先前轮次，不作为最后 infrastructure 修正的执行证据。本审查没有重跑。

| 产物/证据 | 本轮读取的身份与时点（UTC） |
|---|---|
| clone `SignalStudioDetectionRefinementTests.exe` | `2026-10-13T21:22:47.7939035Z`；SHA `35fb2b345f816f62fb36dca5b0f84687956c113f88a77e1df84e5a53164bb5a9` |
| clone `SignalStudioDetectionTimePipelineTests.exe` | `2026-10-13T21:47:07.1634241Z`；SHA `06a87728277ae763bca514fd8fe9517062e581d1d45417023700de7b5d162772` |
| 最后 build/CTest 日志 | `evidence-r4-fallback-actual-build.log` 写入 `21:47:07.2214235Z`，实际编译 time_analysis 并重链 time-pipeline；CTest 写入 `21:47:08.581558Z` |
| UI 文案及 capture 编译 | `evidence-r4-ui-build.log` 记录 annotation_evidence_widget/ui_capture 编译成功。clone UI 源写入 `21:24:56.2709071Z`，UiTests/UiCapture 为 `21:30:17/23Z` |
| 完整 UI 产物最新性 | clone `time_analysis.cpp` 最后写入 `21:46:37.6628883Z`；`SignalStudio.exe` 为 `21:30:11Z`。当前主程序、UiTests、UiCapture 均早于最后回退证据改动，不能称完整 UI 包已包含全部最终七文件草稿。后续原生验证前须使用包含最后修正的重新链接产物 |

最新 Fine→Coarse **Fallback** 的内层字段保存目前有静态核对和最后编译/聚焦回归支持，但现有 physical fixture 的两个 Fine 分支都断言获得非空 coarse 活动；直接 Coarse 的拒绝另有检查。它没有专门执行并断言“Fine 失败、独立 Coarse 也 Fallback、内层 status/reason/domain 保存且活动为空”这一组合。此为尚未覆盖的执行边界，未找到对应实现缺陷，不把它包装成该新组合已实测。

## 草稿身份

以下七文件在 deferred 与 clone 的当前字节完全相同：

| 文件 | SHA-256 |
|---|---|
| `domain/detection_refinement.h` | `d137990069a730bb8f2dadbb4ffa8fedaccc88d245b0705662d4d91f62e2316b` |
| `domain/detection_refinement.cpp` | `86fa33d603276d9c3c62a5115c48c4bb91413772c2dc7dfb060e03d37a05d131` |
| `infrastructure/detection_time_analysis.cpp` | `2559dd69a79cccee6e839e9da677ed2fc3c0b25776d6d310f0d42a422971df3a` |
| `ui/annotations/annotation_evidence_widget.cpp` | `83c14cc6c8758ea5b076fe6754f7b4a2ae2e27a45c7363557ff6381122cd4c27` |
| `tests/detection_refinement_tests.cpp` | `62aacf3aff831bb86a0b73e083d08e72b516a25ab795fadda14aa0061b673ec8` |
| `tests/detection_time_pipeline_tests.cpp` | `ad3625cd293ebc52a657964345feb76f4f8155caaa367e54ff806f4888a792a2` |
| `tests/wideband_detection_capture.h` | `550690abe0665ec5171102bffd11b151916280168ceef4c54a0ffb8dc5cc2112` |

## 未完成项与节点判断

本草稿没有新的代码规格阻断。真实待完成的是 canonical 集成及对应新身份检查、完整 UI 产物重新链接、最新版原生 ROI 门限/保存重开证据、全套回归，以及上述内层 Coarse Fallback 新记录组合的专项验证。这些不能以 deferred/clone 聚焦通过代替；也不修改正在运行的 R3 frozen 输入。

本轮读取 `G-sample-r3/scope-summary.json` 与 `L3_G_SAMPLE_R3.md`：G/sample 完整 10 对均完成、双方无失败运行，TF-IoU=.5 的 F1 为 Native `.7101449275`、reference `.8260869565`，差值 `-.115942029`、CI `[-.16747644,-.08702884]`，**L3 FAIL，效果项未完成**。Native 的 64 unknown 与 reference 的 64 fallback parent span 分别保留其政策与分母；草稿不把 parent span 改成活动，也不能靠多存几项证据关闭这个效果 gate。没有据此修改阈值或真值。

完整 220 对尚未结束，其他 G 策略/T scope 继续按 R3 freeze 执行；本报告不给未完成 scope 判通过。SCN 参数矩阵例外和 UI 300→480 s 决定仍未答复，当前规则继续保留。草稿不能作为这些待决定项的默认批准，也不代表 M4 可以提交或进入 M5。

## 增量复审：silent fallback 与完整 UI 重链

2026-10-14（Asia/Shanghai）。本轮仅追加只读复审；未构建、运行测试/模型/GPU、修改 canonical/冻结/原型、提交或进入 M5。**增量结论仍为 DRAFT_SPEC_PASS，无新增实现阻断。** 正文中“Fine→Coarse 同为 Fallback 的组合尚未执行”和“UI 产物早于最后 infrastructure 改动”描述的是前次快照，现已分别由下列新证据关闭；其余未完成边界继续保留。

### 全零真实 CI16 fixture

最新草稿 `tests/detection_time_pipeline_tests.cpp:373–419` 写出 400000 个全零实际 CI16 complex samples（1600000 bytes），刷新 source fingerprint/runId/projectGeneration，并重新打开实际 FixedDetectionSource。Sample/ROI 均通过既有 production time-analysis 路径计算，没有注入预制 spectrum 或 energy。

两分支分别检查：current/original activity 都为空，存在 observation，时间质量为 **WindowObservation**，外层 refineStatus 为 Fallback；内层 `fallbackCoarseEvidence` 的 status 为 `fallback`、原因精确为 `coarse-no-supported-on-run`、domain 为 `spectrum-density-db-v1`，noise/threshold 是实际数值，并保留 fixed-default policy。另要求外层原因非空且不含 `measured-coarse-fallback`，防止没有可靠活动却声称获得粗回退活动。这正好覆盖正文所指出的未执行组合。

WindowObservation 是已知模型观察支持、活动起止未可靠定位的契约，不是精确发射段。`04_ALGORITHMS_AND_VALIDATION.md:76`、`VALIDATION_PROTOCOL.md:45` 和原 projection 均允许/要求失败保留这类观察；因此不能把“未知活动时间”误等同于枚举 `TimeQuality::Unknown`。主实施记录首轮测试误期待 Unknown；首轮 `evidence-r4-silent-fallback-ctest.log` 的 1/2 失败原样保留，当前只修正测试期望，六个其他草稿文件及产品数学均未改变。该历史失败不能覆盖为通过，也不归因于产品产生了活动。

`logs/evidence-r4-silent-fallback-ctest-r2.log` 在独立 clone 记录 **2/2、1.61 s**（time pipeline 1.58 s、refinement .01 s）。本轮只读核对，不重跑。该证据关闭 silent source 的实际 reader/STFT/Fine/Coarse 调用及 immutable JSON 字段生成组合；它不另声称 silent 输出经过原生 UI 保存重开或完整文件效果评价。正文已通过的独立融合 durable 回环保持其原范围。

### 草稿身份与 UI 重链版本

本轮重哈希 deferred/clone 七个文件，均与 `diagnostics/evidence-r4-current-identity.json` 相符；相较正文身份表，仅 time-pipeline 测试变为 SHA `13c14ac7d04d1d025ed82f077e78541396d91c495c4ea525d904e1d2f9cdee5c`，其余六个 SHA 不变。当前产品草稿未因首轮测试失败修改数学或时间质量行为。

读取 `logs/evidence-r4-complete-relink.log`、`evidence-r4-silent-fallback-build-r2.log` 及实际产物：

| clone Release 产物 | UTC 写入时间 | 当前 SHA-256 |
|---|---|---|
| `SignalStudio.exe` | `2026-10-13T23:25:42.1626493Z` | `879ad50287303ea588f6b5758155b6b46822d20f40516c23a3a74c9878e58352` |
| `SignalStudioUiTests.exe` | `2026-10-13T23:25:44.3509848Z` | `f33457321d629e6aab435aabf60dff102a82075de6253ace00bc0ca5a2506028` |
| `SignalStudioUiCapture.exe` | `2026-10-13T23:25:45.7189337Z` | `1d5bfafdb758f74ac23e3252cc6523d4c8902f2011b1f8d6074868577b0b415d` |
| `SignalStudioDetectionTimePipelineTests.exe` | `2026-10-13T23:27:19.8880786Z` | `44fb6806db2f266ce6ef4c5bd761f60785e2ec9e270b5e2622c9b08c34bb8ac3` |

这三个 UI 产物已晚于最后 `time_analysis.cpp` 的 `21:46:37.6628883Z`；测试 CLI 又晚于当前测试源码的 `23:27:10.6821019Z`，r2 CTest 日志为 `23:27:33.4983567Z`。**此前完整 UI 尚未重新链接的版本缺口已关闭。** 完整 build/relink 仍不等于 UI 运行、原生截图或部署验收通过；新原生探针尚未执行。

### 保留的完成边界

本轮 canonical refinement/time-analysis/Release CLI 抽核仍匹配 R3 freeze，freeze SHA 不变；没有将草稿应用到正式矩阵。读取 suite 日志时已推进至 G/noSplit 10/10 native，但完整 220 对仍未结束，本附录不据进度判定其他 scope 效果。G/sample 已归档的 L3 FAIL、不放宽容差与失败/未知分母政策继续保留。

尚待 canonical 集成及对应新身份检查、最新版原生 ROI 门限/保存重开旅程、全套 UI/项目回归；SCN 参数矩阵例外与 480 s 预算仍等待用户决定。只关闭本轮补足的两项草稿验证边界，不将其余待决定/执行项转为默认批准或 M4/M5 完成。
