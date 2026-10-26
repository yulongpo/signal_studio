# UX-R6 两个 P0 根因及修复合同

## BUG-R6-01：164/639窗口，存储证据上限

证据来自用户明确提供的工程、其中两份原失败manifest，以及本轮原IQ/原参数/原模型重跑。不是从截图猜测CUDA OOM。

链路：`DetectionTaskManager::execute` 读取和推理成功 → `storeDetectionHeadEvidence` 保存每窗三组完整float32模型输出 → `appendDetectionChunk` 回读既有分片及head引用 → `verifyHeadEvidence` 验证布局/路径/内容SHA并返回raw字节数 → raw字节与JSON mappings共同累计到请求的32MiB `evidenceBudgetBytes` → 第166窗所在batch不能提交，已完成计数仍164 → Failed。原 `computationPhase=inference` 覆盖了推理之后的存储阶段，用户因而只看到窗口处中断。

65536输入对应输出宽16384，每窗3×16384×4=196608字节，仅164窗raw输出就32,243,712字节，再加观测/映射JSON已接近33,554,432字节预算。下一批两窗超限。阈值依赖输入频率长度、映射大小与批次；“约180”不是固定模型窗口限制。此次在Sample/ROI精修之前失败，原始失败运行实际选ROI，须另测Sample分支。

既有plan只核对**单窗口**的证据尺寸，未预估整个任务的独立raw-head磁盘负担；追加每批又重复验证全部历史分片和完整raw heads，另有待测的累积I/O成本，不能先写成已证实的提速收益。

修复设计：区分JSON/控制证据、磁盘raw-head资产和resident工作集；raw输出仍逐窗完整持久化且保持路径、shape、哈希、读回验证。新增显式有界raw-head配额和全任务预检，绝不靠移除校验、丢弃候选/窗口、重复伪造输入、无限内存或简单整体上调旧证据常量。旧请求缺新增字段时保留原有combined预算语义，不能改变旧manifest验证结论。所有资源拒绝必须指出阶段、所需/允许字节和可采取的缩小范围或调整间隔。

## BUG-R6-02：待更新被误作不可见

参数提交/模型切换 → `configurationCommitted` → `DetectionPanel::markConfigurationStale` → service `markStale` → slot.stale=true。`domain/project.h::annotationInContext` 额外要求 `!slot.stale`，统一把旧committed对象排除，影响表格、图层、选择和工程树；存储对象未被删除。begin/abandon也置stale，故只修配置控件信号不够，保存重开也不会恢复可见。

修复设计：stale仅作配置待更新提示；资格仍严格核对FileId、IQ通道、sourceSet、committedRunId及未删除状态。当前committed结果在DraftChanged、Running、失败/取消/中断时保留可查看；pending和retired永不混入。只在新结果完整验证、durable落盘且QSaveFile原子提交后切换，包括合法0结果。保留来源权限、转换快照、旧通道、代次拒绝和私有GC边界。

并行诊断报告及真实Qt/持久化复现见 `evidence/baseline/bug02-DIAGNOSIS.md`；其probe为受控源，不冒称真实worker崩溃或原生GPU。

## 状态

R6-0：两个根因已确认；修复与回归尚待R6-1闭合。原始失败日志保留在out任务目录和必要归档，不将部分前缀当成功完成。
