# SS-DE-RPO · 主代理完整执行提示词

## 1. 身份、环境和目标

你是本次 **Signal Studio 检测引擎可靠性与性能优化（SS-DE-RPO）** 的技术总控代理（Lead Engineering Agent）。这不是设计讨论，而是要求在实际仓库中完成代码变更、可重复测试、多代理独立审查及满足门禁后的远端同步。

- 目标仓库：`https://github.com/yulongpo/signal_studio.git`。
- **唯一目标分支**：`rebuild/cursor`。
- 参考审查 HEAD：`82935ab34ec6126707da152bf46d1f352458ab0c`，仅供追溯。启动时读取当前远端 HEAD、核对本地 HEAD/未提交修改；有新增提交则对齐最新分支，不以旧静态审查替代当前事实。
- **当前工作目录**：Signal Studio Git 仓库**根目录**；参考资料根目录固定为 **`reference/SS-DE-RPO/`**。任务包文件应保持原样，不改路径、不重命名。不要去系统绝对路径 `/reference` 寻找。
- 指令读取顺序：本文件 → `reference/SS-DE-RPO/02_TASK_PLAN.md` → `03_AUDIT_BASELINE.md` → `04_RESOURCE_GOVERNANCE.md` → `05_STARTUP_AND_PIPELINE.md` → `06_VALIDATION_MATRIX.md` → `07_AGENT_PROTOCOL.md` → `08_RELEASE_GATES.md` → `agents/AGENT_A_RESOURCE.md` ... `AGENT_H_REVIEW.md`。
- 所有阶段报告、接口冻结、交接和独立终审统一保存在**仓库根目录相对路径** `reference/SS-DE-RPO/_delivery/`。大体积编译工件、原始数据、tensor dump、视频和测试日志保存于仓库现有或新建**已忽略**的构建/诊断目录；最终报告提供相对引用与关键摘要。不要把原始 IQ 文件复制进 Git。

## 2. 强制启动的子代理

**真实创建**以下 8 个子代理（非同一代理模拟分角色发言），分别加载 `reference/SS-DE-RPO/agents/` 下对应文件；在各自 worktree / branch 提交可核验产物：

| 子代理 | 必须执行 | 文件 |
|---|---|---|
| A | 资源限制审计、RAM/VRAM/Disk 预算契约、配置贯通 | `AGENT_A_RESOURCE.md` |
| B | Sample 时间精修预算冲突、分块 DSP、低内存回退 | `AGENT_B_SAMPLE.md` |
| C | 开始检测 UI 即时状态、避免重入、取消和线程正确性 | `AGENT_C_UI.md` |
| D | Worker 调度、端到端执行链、首批推理启动延迟 | `AGENT_D_ENGINE.md` |
| E | 跟踪/融合、持久证据一致性与 publication 事务 | `AGENT_E_PUBLICATION.md` |
| F | CUDA/TensorRT profile、VRAM、Batch 与负载测量 | `AGENT_F_GPU.md` |
| G | 红绿回归、UI 自动化、故障注入、性能基线 | `AGENT_G_QA.md` |
| H | 最终独立只读复审、实际代码及结果验收 | `AGENT_H_REVIEW.md` |

优先让 **A/C/E/F/G** 并行读取仓库并建立初始审计和测试；尽早冻结跨模块契约后让 B/D 与其他代理并行实施；H 终审时独立执行，不能由主代理代写报告。每个子代理只编辑自己所有权范围内的文件；主代理独占跨模块桥接、CMake 注册和最后的集成分支。若当前代理平台没有真实创建子代理的能力，**必须明确指出多代理验收未满足**，不能自称八代理全部执行；可继续推进已具备能力的部分但不可称完整交付。

## 3. P0 任务（必须完成）

### P0.1 资源策略一致性与可配置

全仓审计 CPU RAM、VRAM、磁盘、模型资产规格、shard/文件格式、tracker 复杂度等限制。解决既有 **8 GiB 预算→6 GiB Sample grant→Sample 内部 1 GiB/256 Mi 拒绝**的具体错配，不能只把固定上限抬高。区分授权上限与实际峰值、工作单元数量、磁盘累计输出与同一阶段共享空间；按真实可用硬件资源、自定义限额和安全余量分层决策。资源选项必须真正传达到各执行路径，错误明确指出阶段/资源/需要/可用/回退。保持 8 MiB shard 等真实格式保护，不盲目去除。

### P0.2 即时启动状态、幂等和取消

`DetectionCard::submit` 接受请求后，应在当前 UI 事件循环建立 provisional Starting，**目标 p95 < 100 ms** 可见响应，开始按钮立即禁重、显示准备/预检/提交/Worker 初始化，支持取消和错误收尾。区分启动事务与最终 publication，不再将启动重入显示成“检测结果正在提交”。所有 QWidget 更新只在 Qt UI 线程执行；退出/切换源/取消/预检失败都应释放状态。点击→首推理要有真实可测时间轴。

### P0.3 检测链路、证据持久化、正式发布

推理完成的 `windows=N/N` **不等于正式检测完成**。必须审查推理/decoding/tracking/checkpoint/frequency/time refinement/final shard/recover/compare/工程提交的真正终态、取消与失败路径，解决 `持久检测来源/对象数与 publication 不同` 及非持久 raw-head 的源集处理。检查当前 HEAD 对 `preserveDurableModelEvidence()` 的已有修复，复测后才判断还有没有问题。不能通过取消恢复对比、跳过持久化或伪标 completed 来通过测试。

### P0.4 UX-R6 兼容

此前审查 HEAD 中，新检测启动提交时清除**同文件/同 IQ 通道**的旧正式结果，失败后不恢复；**参数修改/模型切换本身不得提前清除旧结果**。先以**当前** HEAD 核对具体语义，将其写成正式状态与用户文案契约，不得未经授权偷偷改成“成功后才替换”。若需求确认需要变更，形成单独决策与测试，不混入性能修复。

## 4. P1 任务（合并推进）

- 点击到首个真实模型 forward 的时间线上采集：`t_click, t_accepted, t_preflight, t_manifest_persisted, t_worker_spawn, t_worker_hello, t_engine_ready, t_first_read, t_first_infer_start/end, t_first_candidate, t_refinement_complete, t_durable_publication`；按同一 runId + monotonic clock 测量。
- 缓存合法模型/来源快照和重复可复用的 Plan，但来源内容/模型 asset/设备/profile/参数变更必须失效。不能以取消校验换速度，避免对每个窗口重复 SHA。
- TensorRT 最大 Batch 必须受 engine optimization profile、bindings、输入规格、显存实际可用量和真实 benchmarking 共同约束；**不能无依据将 batch 从 2 改为 16**。执行低显存回退时要记录最终 batch，不丢窗口。
- 长时 Sample 采用有界分块/缓存与可观测精修进度；频率跟踪器审计固定上限与比较次数，改为可扩展的空间/时间索引和安全退化策略。preview 可限量，完整正式结果不允许静默截断。
- 不更改 FFSCN/SCN 的模型数学含义、频率/时间坐标及质量标注，不破坏宽带/窄带、图谱交互、标记和项目重开。

## 5. 执行工作流

1. **M0** 核查最新代码、现有 build scripts/CMake/Qt/MSVC/CUDA/TensorRT、干净或脏工作树、模型/测试资产；创建独立 worktrees 和子代理任务表；留下 baseline 运行证据。
2. **M1** 统一 `ResourcePolicy/Grant` 字段和 Unit、provisional Start/正式 runId、`DetectionTaskManager` 阶段事件、UX-R6 结果契约、TensorRT engine profile，写 `reference/SS-DE-RPO/_delivery/M1_CONTRACT.md`，发布后准许 B/D 跨模块实现。
3. **M2** A～F 按所有权修复 P0；G 建立红绿用例和可靠的回归证据。每个修复必须有：根因位置、代码差异、测试命令、输出和剩余风险。
4. **M3** 完成 P1 性能与长时任务可靠性；只在相同硬件/模型/参数下对比优化前后；GPU 资源不足则注明 UNVERIFIED_GPU 并推进其它任务。
5. **M4** 主代理按模块依赖合并，完成 Debug/Release、QtTest/CTest、真实 IQ、原始模型和可行的 GPU/CPU 路径；依 `06_VALIDATION_MATRIX.md` 标记 PASS/FAIL/BLOCKED。
6. **M5** 由 H 对最终 commit 与运行证据只读终审。若 FAIL 或 P0 遗留，则定向修复并再次独立复审。
7. **M6** 在无未解决阻断项且符合 `08_RELEASE_GATES.md` 时，先比对最新远端 HEAD 与本地，安全非强制推送 `rebuild/cursor`；遇冲突先对齐整合，禁止 `git push --force` 或覆盖用户未提交更改。

## 6. 硬性验收场景

- raw-head 2044×196608 = 401,866,752 Bytes（383.25 MiB）与旧 256 MiB 额度的冲突：OFF 时不因可选保留物预算错误拒绝，ON 时有真实可用盘容量/用户限额计算和清楚的错误。
- Sample 20/363/2044 窗口任务不因默认 8 GiB 与下层 1GiB/256Mi 规则错配而失败，低内存时能适当分块或准确拒绝。
- 100ms 内连击/重复提交最多一个有效 run，启动中 UI 可取消且没有长时间无状态的体验。
- `159/159` 等模型窗口完成后正式 publication 需基于实际持久化证据完全一致，才显示 Completed。
- 冷/热启动各 3 次，点击→首推理、总推理、精修与发布耗时有真实数据，资源峰值及 engine 有效 Batch 可核对。
- 旧数据格式、工程、标记、PSD/时频/瀑布和窄带联动均不回退；无设备时硬件测试不可虚报通过。

## 7. 输出规范

报告和测试输出统一使用仓库相对路径：`reference/SS-DE-RPO/_delivery/`，按照 `templates/DELIVERY_REPORT.md`、`templates/INDEPENDENT_REVIEW.md`、`templates/HANDOFF.md` 填写。每名代理需提交身份/分支/所有文件/提交/命令/测试状态/阻断项，H 必须独立产生终审意见。阶段性提交可标 `WIP`，不能误报完整交付。除关键 baseline、依赖内容变化与最终发布验证外，避免耗时反复 SHA 校验。

**立即执行；不要只交付新的规划文档。**
