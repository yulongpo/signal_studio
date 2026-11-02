# Signal Studio 检测引擎可靠性与性能优化

**任务代号**：`SS-DE-RPO`  
**英文名称**：Detection Engine Reliability & Performance Optimization  
**项目**：`yulongpo/signal_studio` → `rebuild/cursor`  
**版本**：Task Package v2.0 / 2026-10-28  
**参考审查 HEAD**：`82935ab34ec6126707da152bf46d1f352458ab0c`，仅为此前审查快照；执行时必须检查实际远端 HEAD。

## 一、解压位置（强约束）

此 ZIP **内部顶层目录为 `SS-DE-RPO/`**。将 ZIP **解压到 Signal Studio 仓库根目录下的 `reference/`**，最终结构必须是：

```text
<signal_studio_repo>/
├─ reference/
│  └─ SS-DE-RPO/
│     ├─ README.md
│     ├─ 00_START_PROMPT.md
│     ├─ 01_MASTER_AGENT_PROMPT.md
│     ├─ 02_TASK_PLAN.md
│     ├─ 03_AUDIT_BASELINE.md
│     ├─ 04_RESOURCE_GOVERNANCE.md
│     ├─ 05_STARTUP_AND_PIPELINE.md
│     ├─ 06_VALIDATION_MATRIX.md
│     ├─ 07_AGENT_PROTOCOL.md
│     ├─ 08_RELEASE_GATES.md
│     ├─ agents/
│     │  ├─ AGENT_A_RESOURCE.md
│     │  ├─ ...
│     │  └─ AGENT_H_REVIEW.md
│     ├─ templates/
│     │  ├─ HANDOFF.md
│     │  ├─ DELIVERY_REPORT.md
│     │  └─ INDEPENDENT_REVIEW.md
│     └─ tasks.yaml
└─ ...仓库其余源码...
```

> 本文中的 `reference/` 是**仓库根目录内的目录**，不是 Windows/Linux 系统的绝对路径 `/reference`。严格使用单数 `reference`，不是 `references`；若原仓库还没有目录，手动创建。**不要解压成 `reference/SS-DE-RPO/SS-DE-RPO/`。**

## 二、启动方法

1. 在 Codex / Cursor / Grok 主代理中打开 **Signal Studio 仓库根目录**（而不是 `reference/SS-DE-RPO/` 子目录），确保能访问目标 Git 分支。
2. 复制粘贴 [`00_START_PROMPT.md`](00_START_PROMPT.md) 作为启动指令；主代理必须随后读取 [`01_MASTER_AGENT_PROMPT.md`](01_MASTER_AGENT_PROMPT.md) 的完整要求。
3. 强制真实创建 **A～G 七个实施/测试子代理 + H 一个独立只读审核子代理**。拥有子代理能力的平台必须使用真实委派，不允许单代理冒充多代理。
4. 子代理通过 `reference/SS-DE-RPO/agents/AGENT_*.md` 读取各自提示词，按 [`07_AGENT_PROTOCOL.md`](07_AGENT_PROTOCOL.md) 遵守文件所有权和阶段门禁。
5. 所有验收依据写入 `reference/SS-DE-RPO/_delivery/`（主代理可在执行时创建，任务包中不预置）；体积较大的编译缓存/运行日志/录屏应放在仓库既有的构建或忽略目录下，交付报告只引用相对路径，不要直接提交大体积临时文件。

## 三、核心文件

| 文件 | 内容 |
|---|---|
| `00_START_PROMPT.md` | 面向 IDE 主代理的超简短启动提示词 |
| `01_MASTER_AGENT_PROMPT.md` | 完整的总控代理可执行指令 |
| `02_TASK_PLAN.md` | P0/P1 优先级、代理分工、M0～M6 依赖与任务门禁 |
| `03_AUDIT_BASELINE.md` | 之前代码/日志审查确认的事实、版本边界与复验要求 |
| `04_RESOURCE_GOVERNANCE.md` | RAM / VRAM / Disk / 算法复杂度的统一资源契约 |
| `05_STARTUP_AND_PIPELINE.md` | 点击到首批真实推理状态机、可观察性和性能调优 |
| `06_VALIDATION_MATRIX.md` | 20 项关键场景、故障注入和真实硬件验收 |
| `07_AGENT_PROTOCOL.md` | 子代理创建、文件所有权、worktree、接口协议和冲突处理 |
| `08_RELEASE_GATES.md` | 持续集成、独立审核和远端同步门禁 |
| `agents/` | 八份独立子代理完整提示词 |
| `templates/` | 子代理交接、独立终审与整体验收模板 |
| `tasks.yaml` | 机器可读任务元数据；不依赖专用调度插件 |

## 四、交付范围

重点整改：资源配置失效或多层嵌套预算冲突、Sample 时间精修终止、点击开始后的无反馈与重复启动、Worker 启动慢、GPU/TensorRT 批处理自适应、长时信号流式处理、跟踪融合及正式结果发布一致性。保持当前分支既有的 IQ 数据语义、UI 宽带/窄带联动、工程结构及 UX-R6 结果生命周期约定；需要改变时单独提出设计决策，不准暗改。

本包是**实施任务包**，不表示已经修改代码、运行真实 GPU 或完成验收。真实执行时所有发现都须以**当前 HEAD** 重新核对。
