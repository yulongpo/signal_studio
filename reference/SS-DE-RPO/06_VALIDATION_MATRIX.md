# SS-DE-RPO · 回归测试与故障注入矩阵

## 验收证据层级

1. **纯逻辑单测**：不依赖 NVIDIA。验证预算、状态、来源一致性、错误码。
2. **Qt/UI 集成**：真实界面线程、QSignalSpy、按钮状态、连续点击、取消。
3. **IQ/Worker 集成**：合法真实 IQ 文件，端到端执行与磁盘提交。
4. **真实 NVIDIA/TensorRT**：真实 FFSCN/SCN 模型、engine profile 和显存采样。**fixture/mock 结果不得标记为真实 GPU PASS。**

每项记录：caseId、commit、runId、测试命令/版本、输入身份、模型/输入长度/hop、OS/CPU/GPU/VRAM、实际配置、生效预算、结果数量、日志、阶段时间、异常码、`PASS|FAIL|BLOCKED|UNVERIFIED`。

| ID | 必须复验 | 核心断言 | Owner |
|---|---|---|---|
| R-01 | 默认 8GiB + Sample Auto | 不因更大 grant 被内部 1GiB/256Mi 限制拒绝 | A+B+G |
| R-02 | 用户自定义 1/2/4/8/16GiB | 必须实际下发；超设备资源有合理降级或拒绝 | A+B+G |
| R-03 | 预算溢出/内存极低 | 正确 checked arithmetic、错误字段与资源释放 | A+G |
| R-04 | raw-head OFF | 可选文件不保留时不因它的额度拒绝 | A+D+G |
| R-05 | raw-head ON 2044×196608B | 需求=401866752B；不足明确拒绝，足够时可继续 | A+D+G |
| R-06 | Sample 20/363/2044 窗口 | 无 `Invalid sample context/configuration/budgets` 错配 | B+D+G |
| R-07 | Sample/ROI/Coarse 切换 | 时间/频率/有效活动段质量语义不被偷改 | B+E+G |
| R-08 | 100ms 内连点 5 次 | 至多一个有效检测事务/runId，立即 UI 反馈 | C+D+G |
| R-09 | preflight/worker 初始化期间取消/退出 | 不死锁，不残留按钮锁定/孤儿 Worker | C+D+G |
| R-10 | 模型资产损坏、来源改变、磁盘失败 | 准确失败、资源清理、工程不损坏 | C+D+G |
| R-11 | 159/159、363/363、2044/2044 | 推理结束不等于成功；持久发布一致才 Completed | D+E+G |
| R-12 | Durable/non-durable model evidence | 当前 `preserveDurableModelEvidence` 路径完整验证 | D+E+G |
| R-13 | UX-R6 新检测+参数/模型编辑 | 当前 HEAD 的同通道结果语义与 UI/工程一致 | C+E+G |
| R-14 | final shard 损坏 / 项目提交失败 / 恢复 | 非正式中间物不能显示为正式成功 | D+E+G |
| R-15 | TensorRT profile、batch 1/2/更大合法值 | profile 不越界、OOM 可退化且不漏窗 | F+G |
| R-16 | 重叠高密度、多轨迹、大比较数 | 不发生无声截断、正确的跟踪一致性 | E+G |
| R-17 | 高持续时间、高 sampleCount | bounded RAM/IO，逐段精修与进度可见 | B+D+G |
| R-18 | 模型/数据/参数变化后重检 | 缓存失效正确，无过期模型或来源 | D+F+G |
| R-19 | 界面响应性与阶段状态 | <100ms p95 为目标，报告实测，不能假装达标 | C+G |
| R-20 | 标记、工程重开、窄带联动、PSD、时频/瀑布 | 无明显功能回退，正常图谱交互 | G+H |

## 基准条件

同一台电脑、相同驱动/模型 engine/文件/输入帧/窗口数/Batch 策略；优化前和优化后分别至少**冷启动 3 次、热启动 3 次**。记录 `T_click→T_first_infer`、首候选、全推理、频率处理、Sample 精修、最终持久发布、总时长、CPU/RAM/VRAM/Disk 峰值与最后有效 batch；合并重复时间戳的日志不是有效测量。

## 故障分类

- `P0_BLOCKER`：静默中断/虚假成功/结果损坏/不合理预算拒绝/开始重复入队/数据丢失超出既有 UX-R6 契约/硬件不兼容却声称完成。
- `P1_BLOCKER`：严重 UI 卡顿、恢复不一致、性能大幅倒退、错误无法定位。
- `UNVERIFIED`：本机没有硬件、合法模型、IQ 数据或必须的构建依赖。可交付阶段性工作但不得宣称全面验收。
- `PASS`：有当前提交、可重复命令和原始测试证据，H 审查可核实。

保存简洁指标摘要至 `reference/SS-DE-RPO/_delivery/performance/`；大量 raw logs 到 ignored build/output 目录，并在报告指明其所在路径。
