# Signal Studio UX-R6：FFSCN 高吞吐检测、流式融合与 P0 缺陷修复

- 目标仓库：`https://github.com/yulongpo/signal_studio`
- 目标分支：`rebuild/a143-native`
- 本轮审查 HEAD：`ec511662d4d84d5b9757576d24ed2124b6d8a029`；执行前重新 fetch 确认，禁止将其视为永久固定版本。
- 本文件预期位置：`reference/ux-r6/PLAN.md`。该 `reference` 是用户输入参考文件夹，不应由 Codex 任意移动或覆写。考虑当前 `.gitignore` 的 `/reference/*.md` 规则，具体提交约定以工作区现状检查为准；开发交付与验证文档优先放 `docs/implementation/ux-r6/`。

## 1. 产品目标及边界

把离线 FFSCN 检测改造成高吞吐、可增量预览、在线轨迹持续融合的检测流水线；支持当前三 Tab：原始标签、人工标记、检测结果。**正式检测结果只在新任务成功且新结果完整验证并提交时替换旧结果**。编辑模型/参数只影响配置草稿或“待更新”状态，不删除现有结果。

严禁为加速破坏现有真实 GPU TensorRT / SCN、source identity、ROI、窗口映射、事件证据、结果完整性、手工标记和窄带通道快照。不得静默降精度；数值变化需要对照验证并清楚说明。不要重新引入“统一标记记录”。

## 2. 已核查的实现与证据

- `infrastructure/detection_model_backend.cpp`：FFSCN 已在 TensorRT 上按 batch 一次 `enqueueV3`，但请求、资源限制及 worker 约束将最大 batch 限制为 2。SCN 的逻辑 batch 在模型内部仍可能逐项 enqueue，不能混为真实 GPU batch。
- `tools/detection_worker/main.cpp`：入口 `--maximum-execution-batch` 当前限定 1–2，输出有形状与资源协议。
- `infrastructure/detection_task.cpp`：模型窗口循环连续 checkpoint/chunk 落盘；所有窗口后执行 projector.finish → frequency analysis → `applyDetectionTimeAnalysis` → `fuseDetectionPublication` → 正式 publication，UI 目前主要消费终态结果。
- `infrastructure/detection_time_analysis.cpp`：Sample 模式每轨迹读整个范围的 IQ 并调用 `detectionBandEnergy`；对长时间范围存在巨大临时工作集与 CPU 复杂度风险。ROI/Coarse 使用 STFT 路径，需分别定位瓶颈。
- `infrastructure/detection_sample_energy.cpp`：自研 CPU FFT 路径（含 mixed radix / Bluestein），不能未经数值比较直接改为分块 FFT。
- `domain/detection_postprocess.cpp`：存在逐窗口频率轨迹消费与关联；适合拓展有界在线状态，但不可把轨迹观察当作已经时间精修的最终信号段。
- `ui/detection/detection_panel.cpp`：终态 publication 接入 annotation service；需与 UX-R5 当前结果替换策略核对，不允许参数编辑提前触发 result reset。
- 本轮用户真实截图：宽带信号、FFSCN GUI IQ、输入长度 65536、每帧 0.25 ms、约 180 个窗口中断且结果为空。截图可证明 UI 症状，**不能证明 worker 崩溃/显存 OOM 或具体异常根因**。

## 3. 两个必须优先解决的 P0 Bug

### BUG-R6-01：约 180 窗口中断，无结果

**最小复现**：使用用户同类真实 IQ（优先原始失败数据）；FFSCN GUI IQ；输入长度 65536；单帧时长 0.25 ms；时间精修；对应原检测范围、GPU、样本格式；记录任务确切 batch 与图中窗口数。为避免把“180”误写成固定阈值，再测不同总窗数、文件长度、输入帧参数和 batch。

**诊断检查点**：
1. 确认 UI 展示的“窗口数量”、模型处理窗口、STFT 帧、batch 索引语义；记录最后成功完成窗口及失败阶段。
2. Worker `QProcess` 状态、标准错误和协议、TensorRT context/shape、CUDA OOM、GPU 内存、H2D/D2H、运行时 batch fallback。
3. 输入读取、`iqSourceFingerprint` 和来源变化、`checkpoint()`/检测分片和磁盘预算、`appendDetectionChunk`、投影内存预算、`FrequencyTracker` 资源限制。
4. 重点排查全部窗口后 frequency/refine/fuse/publish 的错误是否在界面显示为“处理窗口处中断”；保留完整错误分类和最后阶段，不得吞掉错误。
5. 用小数据、长连续数据、零候选、密集候选和取消等对照定位；测试冷启动、重复运行及重开项目。

**修复标准**：真实复现可完成或者出现有明确阶段/原因/可操作建议的真实错误；无法完整完成时，不得伪装成功；可用完整中间结果在“进行中预览层”显示但不冒充正式结果；不发布损坏/半成品。不通过临时增大无限内存、移除 source integrity、禁用 checkpoint、跳过候选来绕过。

### BUG-R6-02：只修改检测参数或模型就清除了现有正式结果

**期望状态机**：
- `CurrentCommitted`: 旧正式结果有效且可见。
- `DraftChanged`: 用户更改参数/模型；结果仍显示，增加“当前显示上次检测结果 / 待更新”的提示，草稿不触发存储删除、结果失效/置空。
- `Running`: 新任务执行，临时结果独立呈现；旧正式结果保持，不能与预览混合。
- `Succeeded`: 新结果完整落盘校验并完成原子索引切换后，一次性替换旧正式结果；**成功零结果同样清空旧结果**。
- `Failed/Cancelled/Interrupted`: 不替换旧正式结果；清理或封存预览，给出准确状态。
- `Cleanup`: 新版本提交成功后仅清理不再引用的旧任务/来源资产；不能通过恢复扫描把旧结果重新挂到当前显示。

在 `DetectionCard::refreshSource/loadControls`、`MainWindow` 的配置变更信号、`DetectionPanel::poll`、UX-R5 的事务服务和 project persistence 中追踪真实变更链。严禁以 UI 仅隐藏旧结果、或仅挪走 in-memory annotations 的方式假装修复。工程保存/重开与通道快照必须一致。

## 4. 性能设计目标

### 4.1 可测量性能剖析

为每个模型运行记录：总模型窗口；实际 batch 分布与降级原因；读取 / STFT / 归一化 / 传输 / GPU enqueue / GPU 同步 / 后处理 / frequency track / Sample refine / fuse / flush / UI 事件耗时；首次可见时间、首次稳定时间、完成时间；峰值 CPU/RSS/GPU 显存、持久化写入字节；工作队列长度；各时段候选数量。禁止用 GUI GPU 图谱统计代替 TensorRT 模型 GPU 证据。

分别测 0.1、1、10、60 s 长持续信号和短突发、重叠、跳频、低 SNR、零结果；实际信号样本如果缺失需公开标注合成测试，不得冒称真实 RF 样本。

### 4.2 FFSCN GPU batch 扩展

- 先检测 Engine optimization profile、输出 shape、batch 资源限制；计划支持 1/2/4/8/16 的**候选可配置性**，支持不了的 profile 必须明确拒绝或降级；不能承诺每设备都能 batch 16。
- 动态吞吐/低延迟配置；以 benchmark 与显存确定默认值，优先复用缓冲区，尽量减少 host/device 传输与重复分配。
- 异步预取/双缓冲可以引入，但必须有有界背压、正确取消及线程安全。防止 CUDA stream 与 context 跨线程错误。
- SCN 物理 batch 不能虚报为 FFSCN 的一次 enqueue。

### 4.3 Sample 精修分块化

- 分块 IQ 流 + 保留频移相位、滤波状态、边缘 overlap 和活动检测状态；避免对整个长候选分配大向量或重复做整长 FFT。
- 先取得既有全长 CPU 实现的数值黄金基线，再做块长/边界/滤波器/阈值/平滑的等效性分析；必要时保留 LegacyExact 模式用于差分验收。
- 处理跨块活动、连续信号、短突发、滤波过渡、低 SNR、长空白，严格维护绝对样本坐标与 RF/基带变换。
- 长时样本分块优先 CPU 流式；只有 profiling 支持收益时再考虑 cuFFT/CUDA 自定义核。

### 4.4 增量轨迹融合与结果输出

- 在跟踪器消费每批模型窗口后更新有界状态：Created / Updated / Stable / Closed / Merged / Finalized，使用稳定 `trackId`、递增 revision、事件序列号、runId 与来源身份。
- 预览数据不直接写入正式 `file.marks` 算法结果集合，也不能污染人工标记；正式结果继续走 UX-R5 成功事务提交。
- 只在足够支持的观察基础上显示频率候选；未做时间精修时应标“时间未定位”或预估/观察支持，不伪造活动段。
- 事件队列有界，UI 节流并增量更新 (约 5–10 Hz 可作起点)，后台每窗可更新；同轨迹连续延长应更新同一框，避免重复创建。
- 精修后结果可持续替换预览版本，输出稳定 ID 映射；轨迹合并应防闪烁且保持所选对象、鼠标交互稳定。
- 明确预览时间戳、滞后量、未覆盖/排除区域；被取消或失败的预览不能成为正式结果；终态完成后发布覆盖含 0 检出的合法结果。

## 5. 目录、里程碑与计划交付

- `R6-0` 分支基线与故障复现：源码审查，记录问题和真实计时。
- `R6-1` 两个 P0 修复：保持已有结果直到成功原子替换；找到窗口中断的准确根因并修复。
- `R6-2` Profiling/Benchmark 基线与对照，用于决定加速路径，评估预算与检测质量。
- `R6-3` Sample 精修分块算法与单元/数值差分。
- `R6-4` GPU batch 调度、Engine profile 检查与自动降级。
- `R6-5` 有界流式跟踪/融合及增量发布协议。
- `R6-6` Qt 三 Tab 的预览 UI、原生交互及任务状态。
- `R6-7` 多场景 Debug/Release 构建、性能、质量、崩溃/取消/恢复测试。
- `R6-8` 独立 Review Agent 审核最终 HEAD；批准后可安全同步远端。

产物：`docs/implementation/ux-r6/{BASELINE_AUDIT,ROOT_CAUSE,PIPLINE_DESIGN,PERFORMANCE,VALIDATION,DELIVERY}.md`（拼写可统一为 `PIPELINE_DESIGN`）、代码、可运行 Debug/Release、必要的真实原生截图/录屏（可选）、日志、Review 报告、发布记录。失败原始日志保留，杜绝仅描述测试通过。

## 6. 验收门槛

- BUG-R6-01 同参数成功且根因有证据；若环境无法取得用户原 IQ，明确“尚未在该文件上复现”，不可宣称问题已完全修复。
- BUG-R6-02 模型与参数编辑、草稿取消、切换页签及项目保存后旧正式结果保持；新完成包括零结果才替换；失败取消不替换。
- 进行中预览至少提供候选轨迹持续更新示例、批次状态和稳定 ID；内存及队列有界，刷新不重算 IQ 图谱。
- 频率/时间边界精度、低 SNR、跳频、短突发、重叠、撤销和手工标记不得退化；算法差异和容忍度先写明再测。
- 不虚构 GPU 性能提升；以同设备同数据统计 p50/p95、显存、总耗时、首结果延迟、端到端检出质量，达不到建议目标如实报告。
- 新能力不得影响现有 .ssproj 持久化、真实 DDC、人工标记、只读原始标签、已创建窄带通道与工程完整性。
- 不运行用户此前明确不要求的大型 RFbench 事件矩阵。优先定向测试与真实 FFSCN 用例。

## 7. Git / 审核 / SHA 约束

- 不在本地覆盖未跟踪用户文件。Git fetch/merge 策略保守；开发可使用独立 feature 分支，阶段性 commit，但**未经独立审核禁止任何 push**。
- 仅必要时使用 `git rev-parse`、`git log`、`git merge-base`、`git status` 检查对象关系。这些不是“批量文件 SHA 扫描”。
- 严禁反复批量计算全部源码/截图/DLL/模型/日志文件 SHA-256 作为完成条件；产品固有模型绑定和检测分片完整性校验必须保留，允许故障定位时少量针对性检验。
- 独立 Review Agent 未参与编码，自行读实际 diff/代码/日志并执行关键定向复验，写出 `APPROVED/REJECTED/BLOCKED` 和准确 Reviewed-HEAD。
- 只有 `APPROVED` 与最终 HEAD 一致，工作树状态合规，且远端最新提交已安全整合，才能非强制推送。如果审核后源码变化或重合并需重审；若无法启动真正独立 Agent，停在本地不推送。
