# M2 固定网格与离线任务链

## 数据与证据

检测输入使用本仓库独立的 `FixedDetectionSource`，固定样本 origin 0 与 `floor(Fs × 1 ms)` hop，完整采样频带 FFT、periodic Hann、功率平均和窗口整体 ddof=1 归一化。显示端的缩放、有效显示带宽、抽帧及颜色不决定模型输入。报告 ROI 与实际 FFT 上下文独立冻结；真实 10 槽不足时不复制谱，不伪造事件。

每窗口保留绝对帧/窗口编号、样本区间、FFT 实际支持与两端 padding、RF bin 映射、张量摘要、归一化量及数值 guard。摘要覆盖已解码通道 float32 IQ 与 FFT 映射，不声称是完整外部 IQ 文件 hash。快速 size/mtime 指纹只用于源变化检查；正式 golden 使用独立完整文件 SHA-256。

数值版本为 `cpu-f64-fft-density-log-ddof1-v1`：float32 功率下限、double 密度/log 与 ddof=1 统计。默认零功率输入约 −496.213 dB，有限值保护与参考 float32 下溢成 `-inf` 有明确差异；这些 guard 及版本写入证据，不用相同 profile 名称掩盖差异。`referenceConfigCompatible` 仅说明冻结配置匹配；`productionProfileCompatible` 保持 false，待后续真实模型完整对照。

## 任务、worker 与保存

原生独立 `SignalStudioDetectionWorker` 使用冻结 v1 长度前缀协议；控制消息最多 64 KiB，二进制最多 32 MiB，明确 LE / dtype / shape / SHA-256。主进程验证 run/file/generation/request/sequence 相关字段，协议不接受任意脚本执行。CPU DSP 在后台线程，单 worker 请求串行，输入不在 Qt 事件队列积压。其它任务最多 32 个排队；主界面最多 10 Hz 读取轻量状态。

任务先保存不可变请求，再验证和处理。每个完整响应写内容寻址结果分片，原子发布 manifest 后更新完成覆盖；终态 manifest 写成功后才暴露完成及结果发布。取消保留已完成分片，未完成范围保持 unread；Partial 的物理尾部单独 excluded。Completed 仅表示任务约定范围处理结束，不声称未读区域没有信号。

存储位于 `detections/<sha256(runId)>/`，分片最多 4096 条/8 MiB。同 run 的请求和已发布分片前缀不可替换。工程 v5 仅保存运行 ID 引用，任务存储不顺带保存 `project.json`，工程保存也不回写旧进度覆盖新 manifest。活动态恢复为 Interrupted，孤立无 manifest 的分片不自动采纳。界面提供显式查找未引用运行，恢复索引不跨进程续算。

本节点 worker 只有明确标示的系统链路夹具，生产模型配置预检失败；不自动切换成夹具。夹具结果以 Algorithm 来源的观察窗口发布，没有伪造精确时间段，不能直接创建通道。真实模型、候选解码和跟踪属于 M3；精修与二维效果属于 M4。

终态提交以 `finishing` 为取消线性化点：该点之前接受的取消保留为 Cancelled，之后取消快速返回未接受。落盘期间仍核对最新来源有效性，状态改变时用同一终态重新持久化；没有持久化终态的运行不自动发布结果，已写入分片保留给显式恢复。活动及排队终态写盘失败都明确显示 Failed。

累积 Annotation 以编码量和固定结构开销作保守驻留估算，与 DSP/批处理/协议缓冲预留分开。夹具候选只接受已登记的三个数值字段。恢复使用逐分片访客，访客直接读取已验证 hash/来源身份的同一对象；要求全量 JSON 数组的调用在解析前核对累计预算。估算不替代 M5 的 RSS/显存峰值测量。

## 原型界面

检测入口展开当前文件下方的同一常驻卡，配置草稿与任务冻结快照隔离；运行/取消区固定在侧栏底部。独立 ROI 工具只预览，Enter/确认提交、Esc 恢复之前的持续标记工具，既不创建标记也不启动任务。

三来源图层栏常驻主图下方。标记表使用可见筛选、复选、多选、数值排序及分页；时间和频率显示/排序按同对象段包络，未知时间使用 observation 频带。左键重叠选择按来源与段列出，活动段手柄固定编辑该段。辅助区按 M0-P 展示时间/精修、mean/max/唯一窗口、噪底/回退三列摘要；完整范围和来源详情、不可变原始字段独立折叠，没有保存的事实明确显示暂无。

ROI 确认条在主图顶部居中，使用原型 amber 色与实际 ms/RF MHz 边界。正文为紧凑舍入显示，提示保存精确 uint64 半开样本区间、样本数及按样本差计算的持续时长；不使用相邻大绝对索引的 double 差计算时长。

单段确认与整对象接受独立：Pending 多段对象只确认目标段，不修改几何/revision/origin，其它段保持原状态。通道弹窗可在创建前确认当前有效段；未知时间仍不可创建通道。已有通道保留创建修订和来源范围，后续变化只更新关系提示。

## 自检证据

- provider QtTest 14 passed / 0 failed / 0 skipped，真实 20M WAV 采样 PSD 最大差 3.8147e-05 dB、归一化张量最大差 5.00679e-06；独立 Torch CPU oracle 重生成 hash 相同。
- provider/store/protocol 首次集成 CTest 3/3 通过；后续 task 原生测试覆盖独立 worker、批大小和唯一帧计数、无损发布与工程重开、排队/活动取消、Partial、缺 worker/模型和代际隔离。
- Qt 任务端到端专项 3 passed / 0 failed / 0 skipped（含初始化/清理）：真实 IQ→系统夹具任务→统一算法观察标记→保存重开。
- 最终Debug CTest 15/15、Qt UI 94/94、第二连接屏原生场景23/23、43个C++文件Google dry-run/Werror通过；日志及截图见 [NATIVE_VALIDATION.md](NATIVE_VALIDATION.md)。工作日志当前在 ignored `out/m2-*.log` 与 `out/vs2026-qt611-debug/`，按用户新增要求在本节点提交后先整理 `/out` 再进入M3。

系统夹具证明任务与存储链路；上述预处理差异证明采样数值对照，均不作为真实检测效果或 NVIDIA 推理/GPU显示验收。
