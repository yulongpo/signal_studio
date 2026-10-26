# UX-R6 执行边界与流水线设计

实施顺序为真实P0基线/修复 → 分阶段计时 → 流式Sample差分 → Engine能力内Batch → 有界轨迹事件 → 临时预览UI → 多场景性能/质量/原生 → 精确HEAD独立审核/发布。具体阶段状态见 EXECUTION_STATUS。

## 数据与提交边界

`RawSampleReader → FixedDetectionSource → isolated TensorRT worker → decode/tracker → frequency/refine/fuse → durable completed publication → UX-R5正式事务`。

预览由独立run/generation/source身份的有界队列输送；候选与稳定/精修状态明确区分。预览不能直接写file.marks、不能转换为人工/创建通道、不能被工程目录发现为正式集合。原committed集合继续可见，当前来源Tab仍决定唯一正式标记叠加；预览需明确样式/标签且不得混同正式结果。

轨迹需要稳定ID、递增revision/sequence及Created/Updated/Stable/Closed/Merged/Finalized关系。跨批次延长更新同一对象，合并保留ID映射与选择；UI消费约5–10Hz，切文件/旧代次不穿透，不触发IQ频谱重算。完整结果含合法零结果仍只走现有原子成功提交；失败取消清理或封存预览而不清旧正式集合。

## P0资源合同

旧请求的32MiB combined证据政策继续按旧语义验证。新请求为完整raw-head磁盘资产增加单独显式配额，JSON mappings原配额保持，单分片/总JSON/头布局/内容SHA/原子写入仍受限。预先核算完整窗口数与预计raw-head字节，运行期仍逐片累计拒绝越界；resident工作集独立维持。具体新增字段和上限须随R6-1实际实现/测试更新。

## 数值和模型合同

Sample先记录LegacyExact黄金结果，分块实现须保留绝对样本、频移相位、滤波overlap、阈值/平滑/最小活动间隔状态，记录方法版本与边界不确定度；先写容忍度再测跨块、短突发、低SNR、长空白。LegacyExact保留作差分入口，不能把不同FFT长度导致的滤波变化称逐值等价。

现有FFSCN Engine实际profiles最大batch2、SCN物理batch1。候选1/2/4/8/16需查真实Engine形状和资源；不支持必须明确拒绝或降级，不能虚报GPU enqueue。若重建同权重FP32 Engine，必须另做原始head数值对照和检测质量/资源验证。默认配置由同设备测量决定，不预签性能收益。

计时分别记录读取/STFT/归一化/传输/enqueue/同步/解码/跟踪/精修/融合/落盘/UI，CPU/RSS、GPU指标明确归因和采样范围。端到端首候选/首稳定/总时长及p50/p95以原始事件计算，不用显示GPU统计替代模型GPU。不进行大型RFbench事件矩阵。
