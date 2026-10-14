# M4 最终 SPEC 复审

结论：**SPEC_PASS（当前用户批准的代表性功能与流程验证范围）**。canonical 已集成审过的 r5 七文件、IO-r2 四文件和获批 SCN/UI 预算变更；最新真实检查与当前身份相符。此前 ROI 门限、融合 durable 证据、错误预算及两项显示语义缺口已关闭，未发现新增 SPEC 阻断。可以按节点顺序进入最终 QUALITY 复审；本报告不替代 QUALITY，也不宣布提交、推送或 M5 已开始。

2026-10-14（Asia/Shanghai）。本轮仅只读源码、现有日志、JSON 身份与原生/原型截图，并写本文件；未修改产品、原型、freeze 或其它产物，未构建、运行测试/模型/GPU、停止进程或提交。当前 HEAD 为 M3 `6ed5ce0338cc2020b8bf6a529874e1cc07166177`。

## 1. 当前验收范围

原 M4 依据为 `05_MILESTONES_AND_ACCEPTANCE.md:29,47–49,64–72` 的 T09/T10/T11、A09/A10/A12/A17，以及 07 的复核/通道流程和 08 的 F06–F10。参数 A、SCN 专属禁用例外与 UI 480 秒保护已获用户批准。

用户最新明确要求：“不需要完成大量数据的验证，只选取少量代表性验证功能和流程，大量数据验证可以推送给我在完成M5后手动进行验证。”`VALIDATION_SCOPE_DECISION.md:3–11` 已将此记录为当前口径，替代本轮必须等待全部 220 对的要求。因此完整矩阵不是本次退出前置条件；效果门槛及已有失败也没有因此被改成通过。

R3 停止记录保存六个 G 配置的 60 对及 T/default 的 62 对配对阶段，T07-06 native 为用户范围调整中断；其它 T 与 short scopes 未完成。`suite-r3-user-stop.json` 保留停止决定、122 对记录、原 freeze 身份及自有进程已停止事实。原子写入失败、CLI 时限故障及旧输出继续保留。新代码没有续写旧冻结目录或覆盖旧效果记录。

## 2. 当前身份核对

| 对象 | 本轮独立核对 |
|---|---|
| `evidence-r5-draft-identity.json` | canonical 七文件全部匹配，0 差异 |
| `io-diagnostics-r2-draft-identity.json` | canonical 四文件全部匹配，0 差异 |
| 获批 UI 三文件 | CMake/card/UI tests 当前 SHA 与 `APPROVED_UI_SPEC_REVIEW.md` 所审身份全部一致 |
| `execution-freeze-representative-r4-release.json` | 当前 72 个输入逐字节 SHA 全部匹配；运行前固定 T00-00/T08-00、用途及参数 |
| `native-final-r5/archive-identity.json` | 35 个归档文件（34 PNG 与报告）全部匹配；7 项当前源码/运行文件全部匹配 |
| `prototype-current-check.json` | 22 项获批指纹按各自声明的 raw/utf8-lf 策略重核全部匹配；不将 4 项声明的换行归一化误判为原型改动 |

代表性 freeze SHA-256 为 `4d8985a72f2333dd53378f9195ade67b3889fd140a2fd2660340e927b3be66a6`；native archive identity SHA 为 `daa9fee818bd5f76a0c7275890957c946d5ab4cca85dff12fb9d56c8e5663cb7`。原生报告绑定当前 Debug 主程序/capture/worker 和当前 Release time-pipeline CLI；归档明确 `currentSourceIsR3NumericSource=false`，没有将最终包描述为已经执行旧完整矩阵。

## 3. 规格与实现核对

| 要求 | 当前实现与证据 | SPEC 判断 |
|---|---|---|
| T09、A10；F06–F08 | Sample 读取真实 IQ 功率，ROI 使用真实 STFT 支持及原样本映射；Fine 回退仅复制独立 Coarse Applied 活动，无可靠活动时保留 WindowObservation。`infrastructure/detection_time_analysis.cpp:239–346`。`ui/charts/plot_widget.cpp:2387–2391` 对无活动对象使用底纹并写“时间未定位 / 观察窗口”。领域/实际 CI16 夹具覆盖短活动、间隔、边界、fallback/noSplit；短突发总体检出效果不在本结论中判通过 | 功能、映射及时间依据符合当前范围 |
| T09、A17；分支有效参数 | Sample/ROI/noSplit 采用批准参数 A；Fine 回退固定 6 dB、minBurst 10 ms、gap 20 ms，仅 Sample/ROI 可进入。直接 Coarse 的用户时长筛选不会被默认参数重跑覆盖。存储值保持，当前完整回归包含相关领域和物理 pipeline 检查 | 参数作用域符合契约 |
| T10、A12；P11/P14、I10/I22/I23 | `application/annotation_service.cpp:1029–1064` 只绑定确认的明确活动段，保存 source/revision/segment 快照；`application/session.cpp:584–604` 保留实数限制、保护带、输出率与源范围校验。编辑/拒绝/删除后的依赖状态及不静默重算由现有回归覆盖。新版原生完成真实算法接受、通道参数、CPU DDC、保存重开和 Changed 旅程 | 当前真实端到端旅程通过 |
| T11、A09；F10 | 独立 reference、固定评价器与分组工具继续保留源/真值/profile/参数身份及失败计账；当前 representative 原生/参考各自实际执行。`domain/detection_refinement.cpp:366–426,1012,1070–1075` 每阈值独立最大有效匹配数再最大 IoU，Failed/NotProcessed 保留全部真值并计零预测。频率/时间/TF、G/T、HY 历史及 FFSCN-17 状态不混报 | 工具与诊断流程符合；完整效果结论保留限制 |
| A17；F09、P05/P17/I18 | `domain/detection_refinement.cpp:893–915` 同一窗口取最高分一次，mean/max 与命中数使用唯一窗口。证据卡区分观察频带、Sample、ROI、Coarse、Fallback 和人工修订；分数未校准、窗口比例不是占空比/任务完成率。ROI 实际两门限及残差 dB 域已保存；Coarse q20 profile/门限明确为谱密度 dB 域，Sample 为线性功率 | 方法、单位及证据语义符合 |

SCN `detection_card.cpp:759–825` 在所有策略（含历史 None）禁用无效裕量，说明固定 CNR/频率 3 dB 与 Coarse/回退 6 dB；非 SCN 矩阵保持。`ui_tests.cpp:4302–4339` 检查 SCN 11.25 快照保持，切回 fixture 恢复 margin=9/score=.8。`CMakeLists.txt:345–351` 保留 runner、offscreen/scale1.5 和全部用例，只将 UI 终止保护改为获批 480 秒；不把它当作交互性能门槛。

## 4. 已关闭的审查项

- **ROI 实际门限及融合 durable 证据**：当前七文件身份与此前审查一致。ROI 保存 positiveMean/offFraction/time/frequency gates/domain，原 mask 数学不变。`tests/detection_time_pipeline_tests.cpp:243–271` 使用独立目录 append/end-marker/recover，核对完整 DTO JSON 及嵌套 parent 的完整 223-byte seed；不再将仅 codec 或源码复制 parents 当作 durable 执行证据。当前完整 CTest 含此测试。
- **长 seed 身份与原始 r0**：短 seed 兼容、长 seed 确定 SHA ID；两处 lookup 适配，完整 seed 留在不可变原始字段。当前原始发布仍要求未人工编辑的 algorithm-origin r0，融合保留 parent lineage；正常人工修订继续由工程持久化保存。
- **I/O 诊断与 4096-byte 错误契约**：`detection_store.cpp:392–414` 记录实际 open/write/commit、具体相对路径和操作后的 Qt cause，保留 QSaveFile direct fallback=false。`detection_task.cpp:28–45,819,848,882` 对入口及追加统一约束 UTF-8 总预算，保留原始头部原因/最新末尾失败，短错误不变。自有 manifest 锁定及长中文入口/再次终态失败夹具随当前 CTest 通过；prefix 追加的有界性是静态核对，不另声称长 prefix 故障组合已执行。正式 R3 故障底层根因仍未确认。
- **回退标题**：`annotation_evidence_widget.cpp:251–252` 已为中性“精修回退”，不再保证活动等于原观察支持。时间质量、当前/原始范围及完整原因继续显示保存证据。
- **终态拒写的阶段**：`detection_task.cpp:877–892` 在拒写后标 Failed；仅原 phase=completed 时恢复 publishing，禁止自动发布并保持旧 durable 前缀。`detection_task_tests.cpp:317–347` 新增 publishing 断言且保留 Interrupted/recoverable 检查。没有新增重试、改变结果或原子策略。

## 5. 最新真实验证

| 证据 | 已完成结果及范围 |
|---|---|
| `canonical-final-debug-all.txt`、`canonical-final-debug-ui.txt` | Debug 23/23；总 502.81 s。UI 94/94、0 skipped，QtTest 328.204 s / CTest 328.96 s，在批准预算内 |
| `canonical-final-release-all.txt`、`canonical-final-release-ui.txt` | Release 23/23；总 89.11 s。UI 94/94、0 skipped，QtTest 63.252 s / CTest 63.93 s |
| `logs/canonical-final-google-style.txt` | 32 个变更 C++ 文件检查 PASS；本轮只读取该完成日志，没有重跑格式检查 |
| `native-final-r5/wideband-detection-report.json` | 34/34 唯一场景、exitCode=0；实际连接屏2 Redmi 27 NU / DISPLAY6，3840×2160、DPR1.5、2560×1440逻辑全屏；三个图有实际 QRhi D3D11 帧，主图59次纹理上传。显示设备与模型推理设备分开记录 |
| `representative-r4/T-default`、`T-sample-short` | T00-00 与 T08-00 原生/参考均 returnCode=0、非超时、pass=true，结果 hash 与 completion 相符。分别完成39和7个真实模型窗口；均0活动/0预测，原生 exactRecovery=true。两个 scope 均 diagnosticPartial=true、acceptanceEligible=false |

新版原生报告 `modelInferenceVerified=true`、`timeEventsVerified=true`。ROI 保存 domain=`positive-db-residual-mean-v1`、positiveMean=7.9950465363、timeGate=2.6590276042、frequencyGate=1.3295138021；.75/2 ms/kernel11/noSplit 的有效参数及保存重开相符。真实 CPU DDC 为16384样本、有限正能量；SHA-256 `29e6aa2b970632e10eb283329c08424190eee0ecfe1942986327f4ad4e8f094e`，immutableOriginPreserved 与 changedChannelSnapshotPreserved 均为 true。

本 reviewer 目视了新版24/31与获批 M0-P `review-evidence.png`、`roi-confirm.png`：三来源、统一记录、辅助证据、常驻配置卡及固定动作保持对应结构，真实 Sample 轨迹与有效 ROI 参数替换合成内容；A1.4.3 图谱比例仍按既定基线保留。原型指纹未变。原始 capture 中 `prototypeComparisonVerified=false`、`a16AcceptanceVerified=false` 保持，不将本次局部人工对照冒充完整 A16/S0–S9/I01–I24 自动验收。

## 6. 保留的边界及后续

T08-00 的两个1ms低CNR活动没有检出，不能将零结果流程成功称为短突发检出能力通过；正结果由新版实际 TorchSig Sample/ROI/复核/DDC旅程提供。旧 G/sample 主比较未达门槛，完整 T/短突发 L4 和真实 RF 效果未完成。用户减少验证量并未放宽数学阈值、删除失败或证明总体准确率。

旧 SPEC/QUALITY、截图、矩阵与草稿报告继续代表其当时身份。当前入口为 `FINAL_SELF_REVIEW.md` 与本最终 SPEC；较早“待批准/待集成/完整矩阵继续”等表述按记录顺序理解为历史，不能覆盖当前人类决定与最新完成证据。

完整 A16、部署/规模及手动验证脚本交付属于 M5 后续；不作为本轮新增阻断。手动矩阵应使用最终包的新身份与新输出目录，保留旧 R3 历史。下一步为独立最终 QUALITY；节点未提交、M5 未开始。
