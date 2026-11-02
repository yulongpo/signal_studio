# SS-DE-RPO · 统一资源预算契约

## 核心原则

**Resource Cap ≠ Resource Grant ≠ Expected Peak ≠ Actual Peak ≠ Work Units**。上层给某阶段 6 GiB 的使用许可，不代表要一次性分配 6 GiB，亦不意味着超过该算法本身的 1 GiB 初始化安全边界。预算语义混用是本次 P0 故障的核心。

## 建议类型与事件

- `ResourceSnapshot`：CPU 总/可用物理 RAM、GPU 总/可用 VRAM、按 volume 的 Disk 可用、获取时间、探测错误及设备身份。
- `ResourcePolicy`：`Auto | Custom | StrictTest`，资源上限、OS/UI/驱动安全保留量、并发执行数、模型证据保留选项。
- `ResourceEstimate`：phase、minimum/preferred/peak resident bytes、temporary/durable disk peak、work units、阶段 overlap group。
- `ResourceGrant`：CPU RAM/VRAM/Disk 许可量、合法 batch、分块样本数、IO 预取深度、适配原因及版本化的不可变 run 快照。
- `ResourceDecision`：`Accepted | Adapted | Rejected`，稳定错误码与 `requested/available/reserve/granted`、有效回退与建议。
- 使用 checked arithmetic 和严格的 Byte / MiB / Samples / Windows 单位；不允许乘法整数溢出后错误地通过前置校验。

## 分类

| 类别 | 典型例子 | 推荐处置 |
|---|---|---|
| 真实模型规格 | TensorRT 输入 shape、engine optimization profile、FFT/输入长度 | 严格保留；清晰报告非法配置 |
| 解析/文件格式边界 | 单个 shard 8MiB、字段 ID 长度、原子文件安全 | 保留并分片，不能无限制单个文件 |
| 可调整的资源额度 | RAM/VRAM、Temp Disk、raw-head 持久化额度 | UI 自动和高级自定义；下发至所有相关层 |
| 运行算法复杂度保护 | active tracks、candidate comparisons、fusion comparisons | 可使用索引/批次/回压，不能以额度为借口静默丢正式结果 |
| 精度与有效性阈值 | Sample 时间精修质量、模型 scores、物理坐标 | 非性能额度，禁止直接放开 |

## CPU 内存

根据 `availablePhysical` 和 OS/Qt 进程保留来授权，不将 8 GiB 默认值理解为实际预分配 8 GiB。精修采用根据实际窗口/滤波器/样本块的峰值估算；大任务分块，必要时减少并发或预取。现有内层 Sample `1 GiB/256 MiB` 的检查必须重新定义和证明：若其真正约束的是算法工作单元，需要独立字段/错误码，不得与 RAM grant 直接比较。

## GPU 显存与 Batch

实时查询当前设备可用 VRAM，预留其他 CUDA/TensorRT context、显示渲染及驱动占用；允许配置安全上限但不允许不可能的承诺。TensorRT batch 必须在 engine profile min/opt/max 允许范围内，且每个 profile 的动态 shape/binding 与输出映射正确；batch 增长应附显存峰值和吞吐数据。OOM 时优先合法地减小 batch/缓存/预取深度，不能跳过窗口；设备不支持则准确失败。

## 磁盘总账

按卷（volume）区分 observation JSON、raw-head、Sample scratch、checkpoint、final shards、project atomic writes 与备份；考虑并发阶段可能重叠的峰值和 `QSaveFile` 替换时双份空间。raw-head retention OFF 不能无理由被历史原始输出配额拦截；ON 时需要准确计算窗口数和每窗字节数。不得自行清理用户工程、输入 IQ、标注等不可恢复的数据。

## 自动降级的边界

资源不足可减小 block/batch/预取、流式处理、明确提示不保留**可选**调试原始模型输出（但需遵守用户保留设置）。对于不能安全退化的模型资产不兼容、来源损坏、校验异常，必须报错，而不能 catch-all 伪造 coarse 结果。每次运行持久记录生效预算、实际峰值和降级决策，便于复现与审计。

## UI 资源选项

默认：`自动（推荐）`，展示系统现有资源、预计峰值和是否可执行。高级：RAM、VRAM、Temp/Durable Disk、raw-head ON/OFF、并发/预取（如已有可控）。仅对**下一次**运行生效；运行中的 immutable plan 不应受到界面调整污染。每个选项需要执行层 smoke test，证明不是空 UI。
