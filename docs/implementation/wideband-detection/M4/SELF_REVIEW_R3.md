# M4 当前版本自检状态 R3

状态：本报告保留 R3 冻结时的自检历史，不能作为最终版本的退出报告。用户现已批准 SCN profile 的无效裕量控件例外及全套 UI CTest 300→480 秒保护上限；相应修改已应用并通过定向审查。包含全部补项的隔离候选 Debug All 23/23、Qt UI 94/94 通过。当前仍未提交 M4、未进入 M5；最终集成、固定矩阵及最新原生验收以 README.md 和后续最终报告为准。

实际构建使用项目 `scripts/build.ps1`，Debug/Release 公共输出目录保持规定路径。当前 Debug 非 UI CTest 22/22 通过，135.73 秒；完整日志归档为 `debug-non-ui-ctest-r3.txt`。它不能当作 23/23：全套 Qt UI 的既有保护仍为 300 秒，此前超时；独立完整 runner 已检查 94/94，但耗时 389.561 秒，详见 UI_TEST_BUDGET_DECISION.md。

本轮关闭了三个具体问题：

1. 长 track seed 超过 Annotation 的 128-byte record ID 契约。统一短 ID 映射保留已有短 seed；较长值使用稳定 SHA-256，完整 seed 留在原始字段。频率证据与时间精修使用同一映射。长 seed 原始对象 durable 回环、不同尾部 ID、旧短 ID 兼容及融合对象 encode/decode 检查通过。融合 parent 完整原始 seed 的独立 durable 回环尚未由这个 fixture 证明，不能扩大表述。
2. Fine 回退消费已禁用 Coarse 历史值。回退固定既定 6 dB、minBurst 10 ms、splitGap 20 ms，存储值及拆分开关保持；domain 16 组检查通过。
3. 直接 Coarse 的筛选结果被默认参数二次分析覆盖。回退 guard 只允许 Sample/ROI；实际临时 CI16 文件上的 Coarse 拒绝、Fine 测量回退、不同历史配置与保存有效参数检查通过。该物理读样本 fixture 不执行模型，不作效果证据。

30 个新增/变更 C++ 文件通过 `.clang-format` 的 Google 风格检查，5 个 Python 工具通过 Black80，修改脚本通过 PowerShell AST，`git diff --check` 通过。M0-P 的 22 项指纹按获批 manifest 声明的 hashMode 全部匹配：文本采用 canonical UTF-8/LF，二进制保留原始字节。当前 18/22 项 raw SHA 也相同；app.js、data.js、README.md、styles.css 的工作副本为 CRLF，归一 LF 后与批准内容完全相同。检查只读，未改原型；canonical 与 working raw 身份分别记录于 `prototype-current-check.json`，不声称 22 项原始字节相同。

R2 的前三正式 G native 在落盘后恢复失败，旧失败、独立 reference 和第四次中断记录全部保留，未改成成功。63 个仓库内冻结输入/二进制逐字节核对后另存 R2 snapshot。R3 在修正、专项通过和 Release 构建后新建 freeze，72 个代码/运行输入与 308 个数据资产冻结；模型、真值、参数、预算、容差及门槛保持。

R3 全套 220 对顺序验证正在执行。G/sample 10 对已全部完成且保存恢复正确；L3 的 TF-IoU=.5 下原生 F1=.7101449275、参考=.8260869565，下降 11.59 个百分点，未达到门槛。64 条原生未知观察与参考 fallback parent span 的计分差异完整保留，详见 L3_G_SAMPLE_R3.md。其它策略和 T 矩阵继续，不将这个结果改为总体通过。局部能量对照见 ENERGY_VALIDATION_R3.md：G 两组严格通过，T 短突发离散 parity 未通过；不从结果反选参数。

G/ROI 和 G/Coarse 也各完成全部 10 对，无运行失败、原生恢复精确。ROI 主 F1 双方 .8345588235、差值和 CI 均为 0，达到本配置 L3 门槛；Coarse 原生 .3874239351、参考 .6713995943、差 -.2839756592，未达到门槛。Coarse 的 164 个未知观察及参考窗口回退政策保留，原生原因均为 coarse-no-supported-on-run。完整分层与身份见 L3_G_OTHER_STRATEGIES_R3.md；ROI 次要策略通过不能替代 Sample 主 gate。

G/sample 不拆分 10 对也完成，原生 9 成功/1 失败，参考 10 成功；主 F1 .6026119403 对 .7200000000，差 -.1173880597，未达到本配置门槛。失败文件第 66/159 个已完成窗口后 reading checkpoint 原子写入失败，原始失败按零预测及全部真值计账，没有重试剔除。详见 L3_G_NO_SPLIT_R3.md 和 ATOMIC_PUBLICATION_FAILURE_R3_AUDIT.md。完整 R3 72 输入已重核，仓库内 69 项逐字节快照保留于 r3-preserved，身份见 r3-input-preservation.json；canonical 矩阵继续原版本。

34 场景原生证据与 DDC 旅程已归档，版本早于本轮 ID/fallback 修正；最终仍须使用最新包重跑。独立 SPEC/QUALITY 需按最终证据补审，通过并本地提交 M4 后才进入 M5。

ROI 实际两门限未保存的 E1 已明确记录。补保存、数值域及显示、Coarse 单位修正、融合完整长 seed durable 回环的草稿在 M4 独立 source/output 目录通过相关 2/2 CTest和 17 组 domain 检查，见 EVIDENCE_COMPLETION_DRAFT.md。新原生探针增加实际门限检查但未运行；草稿尚未应用到 canonical，也不能视为当前部署包已验证补项。
