# UX-R5 基线审计（R5-0）

日期：2026-10-26。执行依据为 `reference/ux-r5/` 五份用户原始文件，未修改这些文件。

## 版本与工作区

初始 `rebuild/a143-native` / `origin/rebuild/a143-native` 均为 `5b2a499048d64290851ad02ecf764e17cf678231`，fetch 后左右差异为 0/0。没有已跟踪修改。保留用户原有 `docs/implementation/ux-r3/promotion-fbb73db/`、UX-R4 提交外审核/发布记录及 `reference/`；仅精确添加本轮五份规划。未执行整仓或素材指纹盘点，产品原有校验保持。

## 已由源码证实的调用链

| 路径 | 现状与本轮切点 |
|---|---|
| `domain/annotation.h`、`application/annotation_service.*` | 三来源已有不可变 `OriginEvidence`、来源集合、段及修订。`canEdit` 只查来源有效性，未限制 Manual；`confirmSegment/reviewBatch/deleteBatch/beginRevision` 还需收口。通道准备/绑定接受非人工来源。 |
| `domain/project.h`、`application/session.*` | `annotationVisible` 将上下文、图层、筛选合并；服务选择用它过滤。需拆开列表资格、选择资格与叠加，保持 ID 选择和普通选择不缩放。 |
| `ui/annotations/annotation_panel.*` | 单表 `annotationTable`、11 列、200 行分页；`annotationSourceFilter` 为混合来源入口。筛选、质量/删除、表格 context menu、热键、Inspector、通道入口均需按集中能力策略适配。 |
| `app/main_window.*` | `bottomButtons_` 三项为 `resultTab/taskTab/logTab`，只有一个业务页；树按文件/来源/检测运行投影。要成为三业务 Tab 和两个辅助入口。 |
| `ui/charts/plot_widget.*` | 控点/拖动已经调用 `canEdit`；强化能力策略可封住主要图形写路径，仍需实际拖放验证。 |
| `ui/annotations/annotation_evidence_widget.*` | UX-R4 已保留非模态完整详情、主/补充原文、分页搜索和完整复制；复用，不回退到常驻原始 JSON。 |
| `ui/detection/detection_panel.*` | `poll()` 先 upsert `detectionRuns` 再调用追加式 `publishSourceSet`；恢复按钮经 `discoverDetectionRuns` 后再次追加。必须改为唯一正式 slot 的事务发布。 |
| `infrastructure/detection_store.*` | manifest、chunk、head 有既有产品完整性验证；目录扫描可恢复旧 run。需权威退役过滤和受控自有资产 GC。 |
| `infrastructure/project_store.*` | v6 序列化标记/来源和 run ID，加载会从磁盘读取 run。新增可选 slot/退役字段和明确 legacy 解释，不重定义现有字段。 |
| `infrastructure/project_file.*` | `.ssproj` schema 1 内嵌 v6；`QSaveFile` 原子替换；Copy/Reference/Preserve、检测 manifest 资产锚点和源 IQ 轻量签名。正式指针更新以此事务为提交点，先提交后 GC。 |

## 数据引用与迁移风险

`Project -> FileState -> marks/sourceSets/detectionRuns`，run manifest 引用内容寻址 chunk/head；`.ssproj.assets` 清单锚定这些资产。`Channel::sourceSnapshot` 捕获创建几何/文件身份/修订；现有验证要求对应 annotation/revision 仍存在。整体替换前需允许合法退役来源使用独立快照，不能删通道、改 DSP 参数或误报 IQ 缺失。

旧 v6 可能包含多套算法结果，以及非人工对象上的人工修订。加载应在临时对象中确定唯一已完成有效结果；有歧义须诊断，原文件不动。保留人工修订副本后恢复只读原始几何。人工转换必须自包含来源摘要，不依赖即将清理的 run。

## 验证与模块边界

本文件记录源码事实，尚不宣称 UX-R5 行为通过。后续测试覆盖服务绕 UI 拒绝、整批转换、正式发布/故障恢复、旧工程/通道，以及实际 4K/150% Qt。迁移与新语义冲突的旧测试保留其原验证目的并更新，不简单删除。

不修改 STFT、TensorRT worker/模型、窄带同步与 CPU DDC 算法；不运行大型 RFbench 事件验证。构建继续用公共 presets 和现有 scripts，最终独立精确 HEAD 审核前禁止所有远端写入。
