# UX-R4 最终独立审核

Decision: REJECTED
Reviewed-HEAD: 26b9fffd378f1cbd54401f59dc74d4337535ffd1

审查基线：`fbb73dbd5990856dd5a63ff616bd3910893bc2ca`；分支 `rebuild/a143-native`。
审查者未参与实现。本文件为提交外审核产物，不授权 push。

## 独立结论与阻断问题

本 HEAD 不满足源证据完整可查、可复制的硬性需求，不能发布。以下问题已由实际生产代码与既有合法导入合同共同证明；现有通过日志和截图不能抵消该缺陷。按用户要求在确认拒绝后收束，不再为旧 HEAD 扩展验证。修复并提交新精确 HEAD 后，需要重新独立全面审核。

### R4-IR-01 / P1：所谓完整证据遗漏 TorchSig 补充来源原文

`ui/annotations/annotation_evidence_widget.cpp:728` 附近仅追加 `source->originalText`，随后追加 `encodeAnnotation(annotation)`。整个展示/复制路径没有纳入 `AnnotationSourceSet::supplementalOriginalText`。两处入口共用此实现，均受影响。

这不是仅少了重复字段：`infrastructure/label_import.cpp:264-282` 的 `sharedSourceReference` / `boundedRecordSnapshot` 在记录超过 64 KiB 时只在标记中保存指向来源集合的 `snapshotReference`；TorchSig RFbench 事件的引用明确指向 `supplementalOriginalText`。因此大型事件原文不在 `encodeAnnotation` 中，也不在主 `originalText` 中，用户从“技术详情与原文…”及“复制完整证据”都无法取得实际事件全文。补充头部的未识别原始字段同样遗漏。工程内原始资料仍保存，不是持久化损坏。

依据：两份 reference 要求折叠后的源证据完整可查、可复制，并保留 TorchSig、出处及不可变证据；`tests/label_import_tests.cpp::largeSharedAndRecordEvidenceRemainLossless` 明确存在合法的补充头部/大型事件共享原文合同。新 UI 测试仅检查 16000 字符 `originalFieldsJson`、引用和标记 JSON，未检查补充共享来源。

修复要求：在两处共用详情/完整复制中提供完整来源集合（至少补充原文、补充 hash 和对应引用），保持主/补充出处明确及不可变性；补一条通过真实导入产生 `supplementalOriginalText` 引用的定向 UI 用例，检查补充原文尾部确实能查看/复制。不要通过缩短测试输入或删除来源引用规避。

## 审查范围与实际完成检查

- 完整阅读两份 UX-R4 reference、交付/实现/验证/规范文档；审查相对基线全部生产代码 diff，以及测试和 runner。
- 起始 HEAD 精确匹配指定值；仅有用户旧的 `docs/implementation/ux-r3/promotion-fbb73db/` 未跟踪材料。
- 目视查看原生 before/default、Release GPU default、320 px 长名称、完整详情、辅助证据、密集 ROI、999.999 GHz 合成元数据截图。
- 独立补跑 Release UX charts/annotation/layout/popovers：4/4，通过，日志 `out/tasks/wideband-detection/UX-R4-REVIEW/logs/release-ux.txt`。
- 独立补跑一次 Release 软件原生 runner：21 主场景及详情通过；真实 CPU DDC 512 样本、复制 37072 字符；报告 `out/tasks/wideband-detection/UX-R4-REVIEW/native-release-software/report.json`。这不覆盖上述遗漏来源合同。

具体独立执行命令：

```powershell
scripts/build.ps1 -Action Test -Configuration Release -TestRegex '^signal_studio_ux_(annotation|layout|charts|popovers)$'
scripts/verify-ux-r4.ps1 -Configuration Release -SoftwareRenderer -OutputDirectory 'D:/project/signal_studio_rebuild/out/tasks/wideband-detection/UX-R4-REVIEW/native-release-software'
```

前者 CTest 4/4，耗时 13.76 s；后者 exitCode=0，耗时 21.4420609 s。没有独立重建程序；上述结果来自现有部署二进制，并结合归档构建日志核对，不宣称本审核从干净源码重新构建通过。未重跑 Debug、GPU 或大矩阵。

生产代码审查包括 `app/main_window.cpp`、`ui/annotations/annotation_panel.{cpp,h}`、`annotation_evidence_widget.{cpp,h}`、`ui/chart_control_strip.h`、`ui/charts/plot_widget.cpp` 和 `ui/project_tree_presentation.h` 的完整变更；同时查看必要的 Session/AnnotationService、来源结构及 JSON 编解码上下文。检查新增/修改的 Qt 测试、原生 runner、CMake 注册、验收和发布脚本、设计原型源文件及交付文档。没有修改实现、测试或发布脚本。

## 证据评估与需求覆盖

| 范围 | 独立判断及依据 |
|---|---|
| 面板层级、按钮与默认信息密度 | before/default 与最终 GPU default 对照确认主动作前移、危险动作区分、ID/修订技术长文移入详情。320 px 长名称截图中关键动作与范围字段可见，未见所查看场景的控件碰撞。结论限于实际查看场景，不扩展为任意合法输入全部通过。 |
| 图谱工具栏与状态 | 低频参数移动至既有溢出容器，仍复用同一编辑器；生产 diff 未引入第二份参数。删除浮动悬停名称、保留轮廓/控点；密集 ROI 截图能显示数据、选择几何、控制柄与确认入口。既有 layout/charts/popovers 测试本审核补跑通过。 |
| 当前对象、定位、编辑、撤销、创建通道 | 审查按钮到既有领域命令的连接；原生 runner 实际检查 Enter 草稿提交/撤销、上一下一段不缩放、定位缩放、拆分、删除确认/撤销、树/图/表联动、多选禁用创建、创建通道及来源快照、保存重开。独立软件运行通过，但不等于所有操作组合已验收。 |
| 工程与算法兼容性 | diff 未改动 domain/application/infrastructure、DSP、worker 或持久化实现；原生程序实际保存重开 .ssproj、创建真实通道并输出 512 个 CPU DDC 样本。归档合同回归作为补充证据，不把静态未改动等同于所有工程/模型兼容性已独立实测。 |
| 证据完整性 | 原生和新 Qt 用例可证明单个 16000 字符原始字段、引用和修订副本可复制且不改写记录；无法证明共享补充来源完整。R4-IR-01 直接违反该项验收要求。 |
| 真实原生显示 | 核对四组报告均为 21 主场景＋1 详情，合计 84＋4；当次实际 Redmi 27 NU、连接 1、`\\.\DISPLAY6`，物理 3840×2160、Qt 2560×1440、DPR 1.5，并含 Windows DisplayConfig monitorDevicePath。GPU 报告有 D3D11/RTX 5060 Laptop 及绘制/上传计数，软件报告为 QPainter。独立复跑只针对软件配置。 |
| 构建、回归与部署 | 核对归档 Debug/Release 构建、最终 UX 4/4、annotations 1/1、合同 7/7，即每配置定向 CTest 12/12；Qt 日志保留各业务函数结果。核对四份 package 报告及脚本的 system-only PATH/Windows/renderer 条件；这四次独立应用启动为实现方归档结果，本审核未另重跑。 |

目视实际打开的归档图片为：`evidence/before/01-default.png`，以及 `evidence/release-gpu/01-default.png`、`03-narrow-320-long-name.png`、`07-full-evidence-dialog.png`、`15-auxiliary-evidence.png`、`19-dense-roi-long-file.png`、`22-large-frequency-synthetic-metadata.png`。没有逐张目视全部 88 张图片。源码中的原生断言包括实际窗口/屏幕/DPR、侧栏水平边界、关键控件命中与按钮高度、QLabel heightForWidth 和控件矩形碰撞；它们不检查补充来源解析或全部文字像素，因此不能代替源证据合同与目视检查。

## 失败、跳过和可接受性判断

- 初期空指针、通道确认等待、重叠标记命中及错误截图采集问题：归档明确保留失败并区分后续修复。当前源码和最终运行足以支持相关定向流程已修正；没有把失败轮次当成一次全过。
- 标签符号链接权限：归档每配置 4 个 SKIP，并非通过；本轮未修改相关解析/发现实现，该主机限制本身不构成本次 UI 发布的独立阻断理由，但不能宣称别名场景已在本机验收。
- 软件 202.5396315 s：诊断是两张 PNG 完成时间间隔，不是已经定位的单一动作耗时。实现方复查 20.62 s，本审核独立复查 21.44 s，均未复现。原因仍未知；不能据此宣称软件性能稳定性已验收，也不能仅凭这一次间隔归因到本次代码。应保留问题及步骤日志，遇复现再定位；本报告不将它列为已确认的第二个代码缺陷。
- 原型浏览器：仅语法检查及原型源文件可审；浏览器交互不在通过证据中。本审核没有打开或控制浏览器，没有绕过已停止的 Computer Use。设计原型缺少浏览器实测可在如实标注后作为限制保留，不能代替 Qt 验收。
- 合成与真实边界：标记交互夹具、24 个密集标记和 999.999 GHz 元数据排版只验证 UI；四组报告明确 modelInferenceVerified=false。CPU DDC 与实际 IQ 图谱有独立事实，不代表 TensorRT 模型效果、现场 RF 精度或该载频实测通过。未将文档、offscreen 或显示 GPU 当作模型效果。

## 尚未覆盖的边界与后续复审要求

非模态详情的“辅助证据页隐藏后再切换对象/文件”刷新路径，以及合法大体积共享来源文本的选择/展示成本，完成静态检查但未完成独立动态验证，不能写成通过，也不以推测新增拒绝项。曾通过 Computer Use 启动现有 Release 程序并读取原生 AX/画面，第一次点击因 coordinate input geometry is unavailable 失败，之后尝试切换辅助页；用户中断并要求收束，未形成上述边界的有效结论。未修改工程业务数据或保存该抽查窗口。

本次没有从合法大型 RFbench 导入完整执行到 UI 复制的新增动态用例；R4-IR-01 是确定的源码路径缺漏，既有合法导入测试定义了触发输入，已足以拒绝。新 HEAD 复审应要求该端到端行为的实际证据，并继续核对来源完整性、上下文刷新、必要回归及最终原生证据。其他未运行项包括完整 CTest、Debug/GPU 独立复跑、所有来源/窗口组合、TensorRT 效果、Ubuntu 和现场 RF。

## 仓库与交付边界

本审核仅写入本报告和被忽略的 `out/tasks/wideband-detection/UX-R4-REVIEW/` 检查产物；报告保持提交外。用户旧 `docs/implementation/ux-r3/promotion-fbb73db/` 保留。未提交、未 fetch/push，未运行发布脚本，未批量计算 SHA。

结束核验：2026-10-25T12:39:26Z，分支仍为 `rebuild/a143-native`，HEAD 仍为 `26b9fffd378f1cbd54401f59dc74d4337535ffd1`；已跟踪工作树及 index 的 `git diff --quiet` 均返回 0。未跟踪内容仅旧 UX-R3 材料与本报告。本次独立启动的抽查进程已结束。

最终结论维持 REJECTED；该 HEAD 不具备发布批准。请在修复后提交新的精确 HEAD，再进行独立复审。
