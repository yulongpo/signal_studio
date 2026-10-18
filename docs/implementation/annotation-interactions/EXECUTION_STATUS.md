# 标记交互实施状态

本轮独立于已交付的宽带检测 R1–R6，依据 [实施计划](PLAN.md)。分支 `rebuild/a143-native`。

| 节点 | 状态 | 证据 / 下一步 |
|---|---|---|
| U0 | 已获用户确认 | 本地提交 `6615b33`；状态行为 61/61、浏览器 59/59、0 页面异常；SPEC_PASS / QUALITY_PASS；[验证记录](U0/VALIDATION.md) |
| U1 | 已完成自检与本地提交 | `3384e17`；[验证记录](U1/VALIDATION.md)：23/23 全量；最终 UI 104/104；原生 8/8；SPEC_PASS / QUALITY_PASS |
| U2 | 已完成自检与本地提交 | `f2ec417`；[验证记录](U2/VALIDATION.md)：Debug 22 项非 UI 哈希一致复用＋最终完整 UI 108/108；原生 10/10；SPEC_PASS / QUALITY_PASS |
| U3 | 已完成自检与本地提交 | `c182b3e`；[验证记录](U3/VALIDATION.md)：Debug 23/23、UI 108/108；原生 12/12、3 次真实 CPU DDC；SPEC_PASS / QUALITY_PASS |
| U4 | 已完成自检与本地提交 | 本文件所在 U4 提交；[验证记录](U4/VALIDATION.md)：Debug/Release 各 23/23、UI 各 108/108；当前外接 4K 原生 119 场；8 种独立显示模式及 2 种推理不可用情景通过；[SPEC_PASS / QUALITY_PASS](U4/REVIEW.md) |

U0 所有图谱、算法框、分数、通道均为浏览器合成演示，不读取 IQ、不运行模型或 CPU DDC。没有将浏览器截图声明为指定第二屏 Qt 原生验收。此前 R1–R6 证据仍是其各自版本的证据，不证明本轮交互已在 Qt 实现。

审核基线：[本机 U0 页面](http://127.0.0.1:8785/docs/prototype/annotation-interactions-u0/index.html)、[M0-P 修订对照](U0/M0P_DELTA.md)、[独立复审](U0/REVIEW.md)。2026-10-15 用户明确回复“确认 U0 原型，继续 U1–U4（推荐）”，U0 退出条件已满足；继续按节点完成自检、本地提交，全部完成后统一推送。

U0–U4 已依序实现、自检、复审并纳入各节点提交，统一推送目标为
`origin/rebuild/a143-native`。U4 保留各历史节点原记录，并在当前外接屏
和最终程序身份上重新执行验收；当前能力、限制及手动入口见
[交付说明](U4/DELIVERY.md)。用户本机样本和 `.vscode/` 不随节点提交。
