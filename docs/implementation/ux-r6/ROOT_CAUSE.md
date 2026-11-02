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

## R6-1 实施及当前证据

新增兼容可选请求字段 `rawHeadBudgetBytes`：新请求默认256MiB，硬上限2GiB；缺字段的旧请求解码为0，继续按旧combined规则核验。JSON32MiB默认值、单片8MiB、总JSON256MiB、resident工作预算均未上调。追加、加载和流式访问分别累计JSON与raw-head，并且每份raw数据仍完整保存、验证布局/路径/内容SHA。新增全任务精确head字节及保守JSON映射容量预检，不在第166窗才发现配额不足。阶段拆分为decoding、storing-heads、tracking、checkpoint，UI文案明确实际失败阶段。

新存储用例证明：小JSON配额可承载单独有界的完整65536输出；旧combined仍拒绝、JSON超额仍拒绝、第二份head超配额拒绝且prefix不变、重开可验证、篡改raw字节仍被发现。新任务用例用不存在worker证明整任务head配额不足在推理/任务目录创建前拒绝。

BUG02删除的只是 `annotationInContext` 中 `!slot.stale` 条件；其余committed身份、文件/通道、来源和删除判断保留。文案说明正在显示上次正式结果。实际D/R回归验证非空旧集合在草稿/撤销/Enter/模型切换/Tab/运行/失败/取消/中断/事务失败及重开后仍可查看，成功和成功0仍原子替换。

本轮用户原IQ重跑：ROI短范围0.02s为15/15、11375ms、10对象/10活动段；Sample同范围为15/15、9472ms、10对象/10活动段。完整原请求另运行到200/639窗口后**主动取消**，179057ms，23个时间未定位前缀观察/0活动段，CLI原始pass=false、Cancelled，未把它算成完整成功。该试验证明跨过旧164预算故障点及取消路径，不证明完整0.8s任务成功。

当前基础修复/定向自检完成，完整639窗口、各策略质量及全部R6性能/原生终验仍须后续实测闭合。任何缩短范围或取消试验均不代替该最终要求。原始失败日志保留，不将部分前缀当成功正式发布。

## R6-2 完整范围发现的后续容量不一致

同一用户原IQ、65536/0.25ms/batch2/ROI完整请求，新独立head配额已允许全部639/639模型窗持久化。之后在`refine`阶段第23/44候选失败，原始错误为`Spectrum ROI shape exceeds the working-cell budget`，总1730925ms，原CLI pass=false、Failed。完整head共125,632,512B（639×196608），并非GPU OOM或再次触发原32MiB combined配额。

真实调用链：`readSpectrum`以float32矩阵`4*cells`加少量辅助空间准入；调用domain `refineDetection`时却沿用Sample的`working/12`格数限制。物理谱矩阵可满足读取层的字节限额，随后仍被domain `validateSpectrum` 拒绝，错误作为整个任务Failed返回。这是容量计算不一致，不能简单放开domain的安全守卫或减少频率/时间格来绕过。

本地修正统一消费方实际内存账本：保留float32完整谱格、物理时间槽/频率边缘、源FFT峰值、固定ROI双线性resize及Coarse行/profile工作余量，计算有界谱cell容量；读取准入与传入domain的容量一致，原工作字节上限和256M-cell硬限继续保持。数值矩阵、插值、阈值和坐标不变；超限在读取前明确给出所需/允许格数，再走已有真实Coarse/Unknown流程。Sample转Coarse前释放已完成的能量向量，避免E与整谱重叠占用。新增定向用例覆盖“旧Sample格数上限不足、实际谱账本容纳”的零IQ物理矩阵，完整原范围复验仍待后续实际执行，不能预记成功。

原失败的durable prefix可供显式恢复，但没有完成候选替换事务，不代表正式结果。证据在 `evidence/r6-3/full-roi-prefallback-failure/`，原工程只读。
