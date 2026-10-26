# UX-R5 工程与来源兼容

## 格式边界

`.ssproj` 的 magic/schemaVersion=1 及内嵌原生 v6 保持。新增可选 `currentDetectionSlots` 和人工标记的 `conversionSnapshot`，不重新解释原有几何/样本索引字段。样本区间仍为 uint64 半开区间 `[begin,end)`，频率仍为保存的 RF / Baseband / RealOneSided。

转换快照保存源 ID、修订、类别/名称/备注、源 SourceSet（含主/补充原文、来源 hash、身份）、原段/观察及原始证据。源 ID 和 run/chunk 引用是追溯信息，加载副本不要求对应外部 run 或源标签仍存在。人工编辑基准是转换时几何，不会双向改写来源。

## 旧 v6 的临时加载流程

1. 完整解码到临时 Project，完成原有格式、资产及物理范围校验。
2. `migrateRevisedSourceAnnotations` 识别历史非人工用户修订；忽略正常 `model`、`source`、`native-parser` 原始发布。将用户改后状态独立复制为 Manual，完整旧记录/修订保存于人工基准证据；原对象恢复原始段/观察及首次命名/备注，追加系统迁移说明，原始 `OriginEvidence` 保持。
3. `migrateLegacyDetectionSlots` 在每个文件/样本通道中裁决唯一已完成、有效来源、完整覆盖且具发布来源的最新 run。并列时间、缺少可判定来源或多个未发布任务产生明确诊断，不猜测删除。原 `.ssproj` 不被打开操作覆盖。
4. 从正式来源、标记和运行索引退役其它 Algorithm 集合。必要旧通道来源仅保留 ID tombstone 与独立创建快照，既有通道配置及源 IQ 保留。
5. 完整验证成功后才安装会话；保存经 QSaveFile 原子发布，之后才允许有界私有资产 GC。

单纯打开旧工程不会启动模型、重算通道或删除旧磁盘资产。历史 source 已删除、身份变化或修订无法安全还原时给出诊断并保留原工程；不声称所有有歧义旧文件均可自动转换。

## 通道关系

新创建必须从合法 Manual，非人工快捷入口明确确认“转换并创建通道”。旧通道可以仍具有 ImportedLabel/Algorithm 的 `AnnotationChannelSnapshot`。算法结果被替换后，快照保留 revision、source identity、创建时间/频带、Fs、滤波和 DDC 配置；关系标记为 `Retired`，表示“关联检测结果已退役，使用创建快照”，不表示原始 IQ 缺失。定位可回到创建快照的来源范围。

## Copy / Reference / Preserve 与 GC

正式替换先准备候选、核对持久 run/shard，再原子保存工程，最后交换内存状态。GC 不在主线程递归扫描大目录，只处理 canonical 私有根中明确退役且无引用的 store 资产；Reference 或共享根不清理。运行中及已提交索引拥有权与恢复过滤来自 slot，孤儿目录不能作为恢复旧正式结果的依据。

故障、保存取消、磁盘写入失败仍保留旧完整工程；commit 后 GC 失败只留下待回收文件，新正式状态可重开。具体执行结果与失败记录在 VALIDATION.md 中逐项归档，设计本身不作为运行通过证据。
