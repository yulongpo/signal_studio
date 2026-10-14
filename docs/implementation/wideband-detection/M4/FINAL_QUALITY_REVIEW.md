# M4 最终质量复审

结论：**QUALITY_PASS（当前用户批准的代表性功能与流程验证范围）**。canonical 已精确集成 r5 七文件、IO-r2 四文件及获批 UI 变更，最新回归、原生与代表性输出身份相符。此前终态 phase P2 和其它已登记缺口在当前实现及对应证据范围关闭，未发现新的实际代码质量阻断。可以由主实施按节点顺序处理 M4 本地提交；**本报告不宣称全部效果门槛通过，不执行提交、推送或 M5 工作。**

2026-10-14（Asia/Shanghai）。本轮在 `FINAL_SPEC_REVIEW.md` 的 SPEC_PASS 后顺序进行，当前 HEAD 为 M3 `6ed5ce0338cc2020b8bf6a529874e1cc07166177`。只读产品源码、现有日志/JSON/身份及静态截图，仅新增本报告。没有修改产品、freeze、原型或其它产物，没有构建、重跑测试/模型/GPU、停止进程或提交。下述执行结果由主实施产生；审查者重新读取并核对身份，不冒称重新执行。

## 当前退出范围

`VALIDATION_SCOPE_DECISION.md` 记录的最新人类指令已替代“本轮必须完成220对”的要求：本轮只取少量代表性样本验证功能与流程，完整批量矩阵在 M5 完成后交用户手动执行。代码、原型、节点顺序与真实检查没有减少。参数方案 A、SCN 无效裕量控件例外及 UI480秒终止保护均已获批准，不再作为待决定项。

本次 QUALITY 判断涵盖 M4 的时间精修/融合、复核与通道链路、评价诊断工具及最终集成证据；不要求执行 M5 的全面 A16、规模/部署或手动交付。旧 R3 已停止，六个G配置60对及T/default62对配对阶段保留；T07-06范围调整中断、既有IO失败/CLI时限故障和未完成scope均不改成成功。`suite-r3-user-stop.json` 明确保留122对记录、原freeze及自有进程已停止的记录。

## 实现质量与已关闭边界

| 检查范围 | 当前行为、边界及结论 |
|---|---|
| Sample/ROI 的实际来源与参数 | Sample消费实际IQ带限功率，ROI消费实际STFT格并映射源样本；有效支持、量化/滤波不确定度及参数保存。获批edge/smooth/noSplit规则不变，coverage不是占空比。ROI新增字段只记录已算出的两gate/positiveMean/offFraction及残差dB域，没有改mask或接入新数学 |
| 回退与失败 | `infrastructure/detection_time_analysis.cpp:282–320` 仅Sample/ROI进入独立Coarse测量，固定6dB/10ms/20ms，stored值不变；只在Coarse Applied时复制活动。失败内层status/reason/domain保存，无可靠活动则保留WindowObservation。直接Coarse的用户筛选不能被默认参数重跑覆盖；真实读错、取消、nonfinite或累计超限不被转为成功 |
| 边界、资源与原子性 | 已读精修/FFT/融合路径保留uint64物理半开区间、shape/finite与工作预算检查，fusion使用严格overlap/touch且不桥接正一采样点gap。Sample功率输出、精修publication及fusion输出完成后才替换；取消/错误不发布部分精修。进度统计处理候选，包含fallback，不伪装成成功数。没有以本轮范围冒称全规模压力或固定取消完成延迟已验证 |
| 身份、lineage与durable | track helper保持≤128-byte旧seed，长seed用稳定70-byte ID，两处lookup一致，完整seed仍在原始证据。raw decoder继续限制未编辑Algorithm/Pending/model actor r0、原/current/changes一致与RF边界。融合保留parent snapshots、derivedFrom和唯一窗口最高分/原分母，不继承单parent门限为整体测量。完整223-byte seed融合对象已有append/end-marker/recover的DTO精确回环，当前完整CTest包含该fixture |
| IO错误与终态 | store分别在open/write/commit之后读取operation/relative/Qt cause，direct fallback仍false，无新增重试。task初始及两处追加统一限制4096 UTF-8 bytes，短输入原样返回；长摘要保留原始头部及最新尾部且不切断码点。实际初始/终态拒写fixture检查DTO与旧manifest字节，Windows自有锁在断言前释放。long prefix追加仅静态核对，不另称故障组合已执行 |
| 复核、证据与通道 | 证据卡只显示保存的实际数据，Sample线性功率、ROI正残差dB域及Coarse谱密度dB域区分；选择不重新读取IQ。标题为中性“精修回退”，没有保证活动等于模型观察窗。接受同一对象，明确活动段建立真实DDC及来源修订快照；修订后Changed且原通道数据/快照不静默更新 |
| 获批UI例外与可读性 | SCN backend下所有策略含历史None禁用无效margin，保留11.25快照；切回非SCN恢复margin9/score.8，原矩阵与草稿提交规则保持。480秒仅为CTest终止保护。主实施32个变更C++ Google检查PASS；当前代码采用统一ID/helper/错误预算与明确单位，未发现新的可读性问题影响行为 |

### 终态 phase P2 关闭

本 reviewer 预退出发现的矛盾已经在 canonical 关闭。`infrastructure/detection_task.cpp:877–883` 对终态拒写保持Failed，仅当旧phase为completed时退回publishing；不修改更早失败阶段、不新增写重试。`tests/detection_task_tests.cpp:317–347` 检查publishing、禁止takePublication及Interrupted/recoverable durable前缀。该断言既有专项1/1证据，现在也包含在最新Debug/Release完整CTest中。当前UI不会再对该最终拒写快照显示“阶段已完成”及最后完成格；历史首轮发现保留于 `TERMINAL_PHASE_REVIEW.md`。

## 最新执行证据及其实际含义

| 当前主目录证据 | 已结束结果与解释 |
|---|---|
| `canonical-final-debug-all.txt` / `canonical-final-debug-ui.txt` | Debug **23/23**，总502.81s；Qt UI **94/94、0 skipped**，328.204s，CTest UI328.96s，在获批480秒内 |
| `canonical-final-release-all.txt` / `canonical-final-release-ui.txt` | Release **23/23**，总89.11s；Qt UI **94/94、0 skipped**，63.252s，CTest UI63.93s |
| `logs/canonical-final-google-style.txt` | 32个变更C++文件PASS；本审查只读完成日志，没有重新执行格式工具 |
| `native-final-r5/wideband-detection-report.json` | 最新Debug包 **34/34唯一场景、exitCode0**；连接屏2 Redmi27NU / DISPLAY6、3840×2160物理、DPR1.5、2560×1440逻辑全屏；实际QRhi D3D11帧与主图59次纹理上传。显示AMD Radeon880M与推理NVIDIA RTX5060/TensorRT设备分开记录 |
| `representative-r4/T-default` | 预选T00-00纯噪声；Native/reference均返回0且无超时，完成39个实际模型窗口，均0结果，Native Completed且exactRecovery=true |
| `representative-r4/T-sample-short` | 预选T08-00两段1ms/标称CNR6dB文件；两侧均返回0，完成7个实际模型窗口，均0结果，Native Completed且exactRecovery=true；评价保留2个FN，不能称检出能力通过 |

Qt UI为offscreen软件交互证据，与新版native GPU证据分开。完整回归证明所执行检查通过，不证明任意规模/平台或全部数值等价。两个代表scope都 `diagnosticPartial=true`、`acceptanceEligible=false`；四个stage result的SHA分别与completion一致，0结果不是失败文件被剔除，也不是L3/L4通过。

新版原生同时有真实正结果：modelInferenceVerified/timeEventsVerified为true，实际Sample/ROI、同对象接受、确认活动→参数→CPU DDC→保存重开→人工修订旅程完成。ROI记录 `positive-db-residual-mean-v1`、positiveMean7.9950465363、timeGate2.6590276042、frequencyGate1.3295138021，.75/2ms/kernel11/noSplit与保存字段一致。DDC为16384 samples、有限正能量29133.4626，输出SHA `29e6aa2b970632e10eb283329c08424190eee0ecfe1942986327f4ad4e8f094e`；immutableOriginPreserved与changedChannelSnapshotPreserved均true。

本审查目视原始32/30 PNG：ROI两实际门限及残差dB说明可见，人工修订仍区分当前/原始范围及保存的Sample测量。Changed及DDC快照结论使用报告的实际检查，不把两张图的目视扩大成全部交互验收。原capture的 `prototypeComparisonVerified=false` / `a16AcceptanceVerified=false` 原样保留；本次功能旅程、SPEC局部原型对照与完整A16是不同范围。生成IQ/TorchSig文件是真实文件计算输入，不能称实采RF。

## 独立身份核对

| 对象 | 本轮重新读取/哈希结果 |
|---|---|
| r5七文件、IO-r2四文件 | **11/11 canonical匹配**各自草稿identity，0差异 |
| 获批UI三文件 | 当前SHA与已审批准身份一致；没有改runner目标、DPI或真实用例 |
| representative freeze | **72/72**当前输入逐字节匹配，0缺失/差异；未重新计算全部308数据资产，不冒称重复完成此前全数据核验 |
| native archive | **35/35**文件bytes/SHA匹配（34PNG与报告），另 **7/7**当前源码/runtime匹配 |
| M0-P原型 | 按原获批manifest既有hashMode **22/22**一致；working raw为18/22，4项CRLF文本按声明utf8-lf归一后相符，未改原型或新增容错策略 |

representative freeze SHA为 `4d8985a72f2333dd53378f9195ade67b3889fd140a2fd2660340e927b3be66a6`；native archive identity SHA为 `daa9fee818bd5f76a0c7275890957c946d5ab4cca85dff12fb9d56c8e5663cb7`。archive明确 `currentSourceIsR3NumericSource=false`，最终新包未冒称已执行旧完整矩阵。

关键源及运行身份：task `fac6cbdcc423ccb48d06b5e7298f3e8204bec600b90056c2609ea98f3d151ebb`；证据卡 `9b3611ce0f1e818692e5461d22ca08da71e4c697b452b3047c90e2549a16e49e`；Debug主程序 `cdb5ebf52dd2bd1709f12ac31d64649c507fbcbc9b867ee21cd5c0972369a151`；Debug capture `663c82e6cb894ab005bf0329cd198b50692319a57bb2e80539e307516163e823`；Release time-pipeline CLI `75b0354f4307ee85933eb090d756a4958563a9489c191a8857d5db2eb32b7f79`。

最新日志SHA：Debug All `1789f99ed0bd93d53912cf4f6d641e67685d4fa74c1613964a5a8e2a8d5594c6`；Debug UI `6d45d3df2427117eef245c0278ceeb2e22a8c0a41a11b564257768e5b601502a`；Release All `adcb2958984d167b362246117710076d0e0bbdf580d64b5208ea5c17a0c7a094`；Release UI `6461e9b569c9fcb1b699c700fe0e318147634b5e94bf4524c9281db6f57cdd73`。

## 保留的限制与节点判断

旧R3 G/sample主比较未达门槛，部分其他配置也失败；T/short-burst L4、真实RF总体效果及严格数字等价没有全部通过。此前T-short的临界mask差异/最后end一采样点差异与早期CPU parity FAIL保留，不由代表性零结果、成功截图或功能回归抵消。旧原子写入失败的底层系统原因仍未确认；本次诊断/预算及安全拒写修正不被描述为已经消除了该正式故障。

这些限制已经如实报告，当前人类批准范围不要求等待完整矩阵，也没有把其效果阈值放宽或失败改成成功。M5的全面A16、规模/部署及之后手动矩阵交付仍按后续阶段办理；手动运行必须使用最终包新身份和新输出目录，保留旧R3历史。

在本次当前范围内，没有未关闭的代码质量阻断。最终SPEC与本QUALITY均通过；主实施可完成M4节点记录及本地提交后再按顺序进入下一节点。审查结束时M4仍未提交、M5未开始。
