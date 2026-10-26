# UX-R6 执行边界与流水线设计

实施顺序为真实P0基线/修复 → 分阶段计时 → 流式Sample差分 → Engine能力内Batch → 有界轨迹事件 → 临时预览UI → 多场景性能/质量/原生 → 精确HEAD独立审核/发布。具体阶段状态见 EXECUTION_STATUS。

## 数据与提交边界

`RawSampleReader → FixedDetectionSource → isolated TensorRT worker → decode/tracker → frequency/refine/fuse → durable completed publication → UX-R5正式事务`。

预览由独立run/generation/source身份的有界队列输送；候选与稳定/精修状态明确区分。预览不能直接写file.marks、不能转换为人工/创建通道、不能被工程目录发现为正式集合。原committed集合继续可见，当前来源Tab仍决定唯一正式标记叠加；预览需明确样式/标签且不得混同正式结果。

轨迹需要稳定ID、递增revision/sequence及Created/Updated/Stable/Closed/Merged/Finalized关系。跨批次延长更新同一对象，合并保留ID映射与选择；UI消费约5–10Hz，切文件/旧代次不穿透，不触发IQ频谱重算。完整结果含合法零结果仍只走现有原子成功提交；失败取消清理或封存预览而不清旧正式集合。

## P0资源合同

旧请求缺 `rawHeadBudgetBytes` 时解码为0，32MiB combined证据政策继续按旧语义验证。新请求该字段默认256MiB、上限2GiB，为完整raw-head磁盘资产单独设限，JSON mappings原配额保持；单片8MiB、总JSON256MiB、头布局/路径/内容SHA/原子写入仍受限。预先核算完整窗口数与精确raw-head字节及保守JSON观测映射上界，运行期仍逐片累计拒绝越界；resident工作集独立维持。测量入口可显式指定0=legacy combined，不能把新配额误称原请求没有变化。

## 数值和模型合同

Sample先记录LegacyExact黄金结果，分块实现须保留绝对样本、频移相位、滤波overlap、阈值/平滑/最小活动间隔状态，记录方法版本与边界不确定度；先写容忍度再测跨块、短突发、低SNR、长空白。LegacyExact保留作差分入口，不能把不同FFT长度导致的滤波变化称逐值等价。

现有FFSCN Engine实际profiles最大batch2、SCN物理batch1。候选1/2/4/8/16需查真实Engine形状和资源；不支持必须明确拒绝或降级，不能虚报GPU enqueue。若重建同权重FP32 Engine，必须另做原始head数值对照和检测质量/资源验证。默认配置由同设备测量决定，不预签性能收益。

计时分别记录读取/STFT/归一化/传输/enqueue/同步/解码/跟踪/精修/融合/落盘/UI，CPU/RSS、GPU指标明确归因和采样范围。端到端首候选/首稳定/总时长及p50/p95以原始事件计算，不用显示GPU统计替代模型GPU。不进行大型RFbench事件矩阵。

## R6-2 观测实现

Domain的可选`DetectionRun.performanceJson`上限64KiB，持久化仍用run v2；旧文件缺字段不伪造测量。解码/编码检查profile schema和大小，失败不替换目标。固定32个聚合stage key只保留count/total/max/mean，没有每窗无限计时历史；端到端分位数由独立重复运行报告计算。worker stderr只保留末8KiB，主线程只读受mutex保护的有界快照；QProcess在producer线程创建、停止与销毁。

FFSCN实际CUDA事件在同一stream记录H2D/enqueue/D2H四个边界，沿用原有最终同步；SCN逐项enqueue分别计数，不能混作物理batch。固定64个event容量覆盖最大16次物理调用，事件失败显式报错，不填估算GPU时间。Windows分别采样父/worker的CPU时间、RSS/峰值RSS/private bytes，nvidia-smi仅作为整卡聚合补充。正常完成先关闭worker stdin并等正常退出，取消及超时仍终止所属进程；观察到的exitCode/status与调用方主动终止分开记录。

质量测量复用现有标签可信解析器，以物理RF/sample坐标裁剪到冻结请求。频率IoU0.3、事件时间IoU0.5先固定；最大基数二分匹配，不用预测反推真值或重复匹配同一标签。配对数量/内存封顶，超限明确unavailable，不截短结果；未知时间观察不加入事件集合。大样本坐标先做uint64差再转double，避免边界误差计算丢失单样本差。
