# UX-R4 R0 基线

2026-10-25；本地 `rebuild/a143-native` 和一次 fetch 后的远端均为
`fbb73dbd5990856dd5a63ff616bd3910893bc2ca`，远端为本地祖先。已跟踪内容干净；
`docs/implementation/ux-r3/promotion-fbb73db/` 是既有未跟踪材料，保留不纳入 R4。

已完整读取 reference 中 R4 Review_and_Plan 和 Codex_Master_Prompt。沿用 R3
WAV 标签导入、CompactHeader 和窄带五页。只处理标记工作区投影和必要联动。

当次原生探针：Redmi 27 NU，连接 1，`\\.\DISPLAY6`，3840×2160，DPR 1.5，
Qt 2560×1440。身份取自当前 Windows DisplayConfig，不沿用历史屏幕编号。

## 源码确认的问题

| 问题 | 源码位置 / 控件 | 修复方向 |
|---|---|---|
| 首屏完整 ID、修订、来源长文 | annotation_panel.cpp / annotationIdentity | 对象、质量、段摘要；技术身份移入详情 |
| 证据事实全部常驻 | annotation_evidence_widget.cpp / annotationEvidenceWidget | 默认短摘要；共享独立详情视图 |
| 三个段动作同一行且无条件可用 | split/merge/deleteAnnotationSegment | 拆分管理与危险操作；条件与原因 |
| 侧栏主操作位于长内容末尾 | main_window.cpp / locateMark、deleteMarks | 主操作前移、创建通道直达 |
| 按钮默认透明无边框 | main_window.cpp / 应用 QSS | 明确语义及六种反馈状态 |
| 鼠标提示绘入标记框上方 | plot_widget.cpp / hovered 分支 | 保留状态轮廓，名称进入现有坐标读数栏 |
| 工具栏中低频参数挤占标题 | ChartControlStrip | 低频控件进入原有溢出，保留唯一编辑器 |

基线采集入口：`SignalStudioUiCapture --ux-r4 <absolute-dir> --baseline`。
图谱输入为实际 20 MS/s CF32 IQ；标记为明确命名的交互夹具，不代表模型推理。
基线图片及实测几何归档到 evidence/before；后续 VALIDATION 记录实际结果。
不执行源码、日志、图片、DLL 的指纹扫描。
