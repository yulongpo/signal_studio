# M4 最终自检

状态：实现、实际自检及最终 SPEC→QUALITY 通过；本节点完成本地提交后进入 M5。用户已批准 SCN 控件例外和 UI 480 秒保护，并明确将本轮数据验证改为少量代表性功能/流程，批量矩阵在 M5 完成后交用户手动执行。当前退出口径以 VALIDATION_SCOPE_DECISION.md 为准。

主目录已精确集成 r5 七文件与 IO-r2 四文件，身份分别见 evidence-r5-draft-identity.json 和 io-diagnostics-r2-draft-identity.json。两项预退出显示问题已关闭：回退标题不再保证原观察范围，终态发布拒绝不再显示 completed。正式 R3 的旧失败、超时、中断及输入快照保持。

| 检查 | 当前实际结果 |
|---|---|
| 主目录 Debug 自身脚本 All | 23/23，通过，502.81秒；canonical-final-debug-all.txt |
| Debug Qt UI | 94/94、0跳过；QtTest328.204秒、CTest328.96秒；canonical-final-debug-ui.txt |
| Google 风格 | 32个变更C++文件 dry-run / Werror 通过；logs/canonical-final-google-style.txt |
| 主目录 Release 自身脚本 All | 23/23，通过，89.11秒；UI94/94、0跳过，QtTest63.252秒/CTest63.93秒；canonical-final-release-all/ui.txt |
| 最终代表性真实链路 | 纯噪声与1ms低CNR短突发各一文件，原生/参考均成功，均零结果，原生恢复精确；REPRESENTATIVE_VALIDATION.md；不作为全数据效果 gate |
| 新版规定第二屏原生旅程 | 34/34，通过；实际连接屏2 Redmi27NU/DISPLAY6、4K/150%全屏；新ROI门限保存、复核及真实DDC快照通过；NATIVE_FINAL_R5.md |
| 原型及代码/包身份 | 原型未修改，SCN例外已获批；目视24/31与获批截图对照；native-final-r5/archive-identity.json及代表性运行freeze分别绑定当前身份 |
| 最终 SPEC→QUALITY | SPEC_PASS、QUALITY_PASS；FINAL_SPEC_REVIEW.md、FINAL_QUALITY_REVIEW.md；只覆盖当前批准范围，不宣称全部效果通过 |

效果边界保持：旧 R3 的 G/sample 主比较未通过，ROI配置局部通过；T矩阵未完整结束，短突发L4及真实RF效果未完成。代表性文件成功、完整Qt回归和原生截图均不能代替这些门槛。新二进制不会被标为已执行旧全矩阵。
