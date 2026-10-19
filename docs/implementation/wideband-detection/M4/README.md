# M4 交付证据索引

M4 实现、实际自检及最终 SPEC→QUALITY 已通过，本节点提交之后进入 M5。用户已批准参数方案 A、SCN 无效裕量控件例外和 UI CTest 480 秒终止保护，并进一步要求代表性功能/流程验证、批量数据验证在 M5 后交由用户手动执行。当前范围以 VALIDATION_SCOPE_DECISION.md 为准。界面依据为已批准的 M0-P；SCN 是本次明确批准的启用矩阵例外。

本目录区分三份版本证据，避免把不同源码的结果合并成一次验证：

最新版实际自检入口为 FINAL_SELF_REVIEW.md：主目录Debug/Release各23/23、UI各94/94；新原生34/34见NATIVE_FINAL_R5.md及native-final-r5/。少量真实链路见REPRESENTATIVE_VALIDATION.md；既有低CNR短突发未检出保留，不作效果门槛通过。

| 版本 | 当前证据与用途 |
|---|---|
| R3 冻结数值版本 | execution-freeze-r3-release.json、NUMERICAL_RESULTS_R3.md、各 scope 的原始评价和身份；六个G组完整评价、T部分阶段已按用户要求停止，不继续本轮220对。输入快照保留在 r3-input-preservation.json 及 frozen-own-text-inputs-r3.zip 的逐文件清单中。ZIP 仅包含仓库内冻结文本，不是完整构建包。 |
| M4 隔离候选历史 | evidence-r4 与 IO-diagnostics-v2 的已审草稿，以及批准的 UI 例外；final-candidate-debug-all.txt 和 final-candidate-debug-ui.txt 为该历史候选自身脚本产生的 23/23、94/94 结果。之后r5/IO-r2已集成并完成主目录实际检查，结果分别归档，未覆盖旧候选身份。 |
| 较早原生版本 | NATIVE_VALIDATION.md、native-report.json、native-screenshots/；34/34，但早于后续修正。保留历史身份，不作为最终新版截图。 |

实现与参数见 IMPLEMENTATION.md、PARAMETER_DECISION.md、SCN_PARAMETER_DECISION.md、UI_TEST_BUDGET_DECISION.md。精修、融合、固定数据及评价规则分别见 CORE.md、FUSION.md、VALIDATION_PROTOCOL.md、SUITE.md 和 REFERENCE.md。SELF_REVIEW_R3.md 是历史冻结版本报告。

实际门限和 Coarse 回退证据补项见 EVIDENCE_COMPLETION_DRAFT.md，草稿已完成 SPEC→QUALITY；原子发布错误诊断及 UTF-8 错误预算见 IO_DIAGNOSTICS_DRAFT.md，同样完成两阶段审查。正式 G/noSplit 写入失败的根因仍未知，详见 ATOMIC_PUBLICATION_FAILURE_R3_AUDIT.md；新的可控失败夹具仅证明诊断行为，不能宣称修复了该次正式故障。

预退出规格审查另发现回退标题错误地保证保留原支持范围，已在 r5 改为中性标题，保持保存证据中的具体时间依据。此单行改动经 SPEC 复核、候选脚本重建及相关 UI 回环通过，见 FALLBACK_TITLE_REVIEW.md；r4 记录继续保留原身份。

预退出质量审查发现终态拒写后仍可能显示 completed，已在 IO-r2 修正并扩充既有故障断言，相关 CTest 通过。预审结论、范围和最终待检查事项见 PRE_EXIT_REVIEW.md，修正见 TERMINAL_PHASE_REVIEW.md；均不替代最终节点审查。

M4 按用户当前要求完成代表性验证、主目录集成及自检、规定第二屏的最新原生旅程、最终 SPEC→QUALITY，再本地提交。L3/L4 达不到门槛或未完整执行时明确标为未完成；局部策略通过、人工修边或成功截图不能替代主门槛。M5 和最终推送均在 M4 提交之后；批量矩阵入口在 M5 完成后交用户手动执行。
