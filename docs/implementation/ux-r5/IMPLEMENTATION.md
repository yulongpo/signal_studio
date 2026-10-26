# UX-R5 实现记录

本轮在 `rebuild/a143-native` 基于 `5b2a499048d64290851ad02ecf764e17cf678231` 开发。R5-0 审计与 R5-1 合同分别见 BASELINE_AUDIT、DESIGN、DATA_LIFECYCLE。以下为实现和当前实际检查，不替代独立终审。

## R5-2 / R5-3

- `AnnotationCapabilities` 集中检查来源、当前作用域与身份。非 Manual 的所有普通写入、复核、删除、创建通道及开始编辑入口拒绝；Mixed batch 前置整批拒绝。图谱控点/手势复用此政策。Imported/Algorithm 来源管理不进入人工 undo。
- AnnotationPanel 持有三个实际页面及各自 TableModel/TableView，分别为 importedLabel / manualAnnotation / detectionResult。200 行分页，独立 query/filter/sort/page；工具按来源呈现；没有 source/run 历史下拉或统一混合业务页。
- `annotationSelectable`、`annotationEligibleForTab`、`annotationOverlayVisible` 分开；图/树/表基于 ID 联动，普通选择不缩放。用户追加要求后，三个图层 checkbox 和31px独占工具行完全移除，Tab 派生唯一绘制/命中来源，树仍投影所有来源且跨来源选择自动切页，切换不重算分析。属性只读时隐藏几何输入和写按钮，显示物理值与显式转换。
- `convertToManual` 一次前置验证、一个新 Manual SourceSet、一次 undo/redo；支持全部段/当前段及明确再次复制，默认按来源 ID/修订/段模式去重。32 MiB 元数据、128 MiB 命令及对象/段预算保持。完整 conversionSnapshot 随 JSON 保存，源文件/旧 run 删除后副本可独立编辑。
- 新通道限定 Manual，只读来源快捷入口确认转换后再创建。现有通道 Retired 快照定位、几何/源文件身份及 DSP 配置保全。

## R5-4 / R5-5

- DetectionResultService 维护每文件+样本通道唯一 slot；poll 和 recover 使用 ticket 的工程/slot generation 与不可变身份校验，Complete coverage 与持久 publication 一致后，候选 Project 经 `.ssproj` QSaveFile 原子保存，再无抛出地切换内存。
- A→B→A、同模型不同参数与 ROI 均整体替换；0 结果有效清空。stale/pending 结果不作为当前标记显示或转换。参数提交发待更新信号，显式“应用并重新检测”启动下一任务。
- 旧 run/SourceSet/Algorithm 从正式索引退出，恢复只跟随 committed/pending 权威 ID。旧 run ID 防复活，通道必要 ID 独立墓碑保留，不保存一整套历史框。
- 受控根 ownership、Reference 追加共享侧文件（已有文件字节不变）、保存全程目标锁及 Copy 来源锁保护 GC。后台每轮最多 4 run/256 文件；只删明确退役、无引用、无链接、canonical 私有根中经 store 验证的路径。尚未保存的旧工程迁移还需核对原容器 asset manifest，不能提前删旧运行。外部 IQ/TXT/SigMF/模型不在删除候选中。
- 兼容可选 v6 字段；旧人工修订先保全 Manual，再裁决旧多 run，已验证的 live anchor 保留 pending 实际前缀；无检测文件保持字段缺省。并列/缺证据工程明确诊断，原文件不改写。GUI 保存对自身后台 GC 的短暂锁竞争仅在 IO worker 内最多重试1秒，codec 仍立即拒绝冲突。详见 MIGRATION。

## 当前检查与退出边界

Debug/Release 公共完整构建已通过；各12组核心/存储、5组 UX、20个旧 UI 业务函数最终集成回归均通过。旧测试中直接编辑源对象、导入进入人工 undo、图谱命中其他 Tab 等冲突断言改为新合同，保留原来的物理范围、证据、取消、分页和来源保护检查。完整结果、4个标签符号链接权限跳过、首轮失败及修复映射见 VALIDATION。

原生已确认实际 Redmi 27 NU、连接2、DISPLAY6、3840×2160、150%，使用实际 Qt 输入、真实文件/CPU DDC、独立 fixture worker 及受控持久化零结果。Debug/Release × GPU/软件均15/15，共60主场景；4次生产程序独立启动通过。76张截图、完整原始报告、构建/回归日志归档 `evidence/`。实际性能数值及限制见 VALIDATION，未据此宣称所有配置性能通过。R5-7 独立精确 HEAD 审核/发布报告保留提交外。

生产算法/STFT/TensorRT worker、窄带同步、CPU DDC 未重写。检测事务使用真实 store/.ssproj 与受控 publication 验证，不据此宣称模型推理精度；不运行大型 RFbench，不进行批量文件指纹盘点。
