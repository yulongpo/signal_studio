# M4 预退出审查记录

本次由主 Agent 实施，独立 Agent 按 SPEC→QUALITY 顺序只读审查。范围为隔离候选的 M4 实现，以及 T09/T10/T11、A09/A10/A12/A17 对应流程；不提前审查或执行 M5。完整矩阵、主目录集成与新版原生证据尚未齐备，因此结论不能代替最终节点审查。

SPEC 新发现一项回退标题语义错误，修正为中性标题后源码复核 SPEC_PASS；详情与身份见 FALLBACK_TITLE_REVIEW.md。其它所查流程未发现新增规格阻断。

QUALITY 新发现一项 P2：终态拒写后 Failed 保留 completed 阶段。修正及既有故障回归通过，SPEC 增量复核通过，QUALITY 复核关闭该项；详见 TERMINAL_PHASE_REVIEW.md。最终预检查结论为 PRE_QUALITY_PASS，已读候选范围未发现其它新增实际代码缺陷。

双方均明确保留以下退出前置：固定 220 对结束并归档；采用 r5 七文件与 IO-r2 四文件集成主目录并绑定身份；主目录必要构建/回归；最新版 Sample/ROI/真实模型→确认→CPU DDC→保存重开→Changed 旅程及规定第二屏截图；最终 SPEC→QUALITY、本地提交。此前候选 23/23 属于标题与阶段修正之前的版本，不扩写为这次最新全套结果。
