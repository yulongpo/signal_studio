# SS-DE-RPO 子代理名册

| 代理 | 标识 | 分支 | Worktree | 职责 | 状态 |
|---|---|---|---|---|---|
| A | `15b82ee1-2c0b-4760-91a0-aac67b140969` | `fix/ss-de-rpo-a` | `D:/project/ss-de-rpo-wt/a` | 资源类型、决策、资源选项说明 | 已提交 `9f4ba37` |
| B | `ffc985e1-4f9e-4a9f-8dac-82d71a2bf060` | `fix/ss-de-rpo-b` | `D:/project/ss-de-rpo-wt/b` | Sample grant / cell / 分块 | 已提交 `dbb145e`，构建留到集成 |
| C | `32a68354-7e24-4043-9111-93fe1a561dd1` | `fix/ss-de-rpo-c` | `D:/project/ss-de-rpo-wt/c` | 启动 latch、文案、阶段 | 已提交 `7be7722` |
| D | `b72baeff-8502-4194-9d96-ce6c03c1e32f` | `fix/ss-de-rpo-d` | `D:/project/ss-de-rpo-wt/d` | 预检保持、时间线、noteSteadyMark | 已提交 `c21b5ff`；`t_accepted` 在集成时于入队锁外调用 |
| E | `9712463c-953b-42ce-8c5c-e4897e44e51a` | `fix/ss-de-rpo-e` | `D:/project/ss-de-rpo-wt/e` | publication 诊断、跟踪索引 | 已提交 `5cc3065`，比较仍在，跟踪未改 |
| F | `af2dc1b4-6d38-489b-a136-58155c42960d` | `fix/ss-de-rpo-f` | `D:/project/ss-de-rpo-wt/f` | profile/VRAM batch 选择 | 已提交 `22f2eb2`，默认 batch 仍为 2 |
| G | `ee7ebd70-7e2f-48c0-98a3-b843098446f4` | `fix/ss-de-rpo-g` | `D:/project/ss-de-rpo-wt/g` | RPO 回归用例 | 已提交 `d71eb7d`，待集成复跑 |
| H | 尚未创建 | 无写入分支 | 集成工作区只读 | 终审 | 等待集成 HEAD |

主代理工作区：`D:/project/signal_studio_rebuild`，分支 `rebuild/cursor`。  
契约：`reference/SS-DE-RPO/_delivery/M1_CONTRACT.md`。
