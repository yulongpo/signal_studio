# SS-DE-RPO · 多子代理协作、所有权和交接协议

## A. 子代理创建与执行

主代理必须利用当前平台的真实 sub-agent/delegate/spawn 能力派生**A～H 八个独立子代理**。每个代理收到 `reference/SS-DE-RPO/agents/AGENT_X_*.md` 的对应指令，交付独立任务标识、工作分支、patch 和测试证据。多个提示词由一个代理串行回答**不算**八个子代理。没有这一能力时必须记录「多代理要求未满足」，不能声称完整交付。

所有子代理应在同一版本 baseline 的独立 `git worktree` / `fix/ss-de-rpo-{agent}` 分支工作；使用非强制 merge/rebase/cherry-pick 由主代理整合。编译缓存与大日志可单独目录，保持版本库干净。若在 Windows 上已有用户改动，先记录、隔离和保留，禁止删除或 reset。

## B. 唯一文件所有权

| Owner | 可修改区域（以仓库实际存在路径为准） | 不可直接修改 |
|---|---|---|
| A | `domain/detection.h/.cpp`、`ui/detection/detection_resource_options.*`、独立 resource policy 文件 | B/D/F 核心执行文件 |
| B | `infrastructure/detection_time_analysis.*`、`detection_sample_stream*`、对应新 scratch helper | `detection_task.cpp`、A 资源入口 |
| C | `ui/detection/detection_card.*`、`detection_panel.*`、`detection_phase.*` | D task、E result service |
| D | `infrastructure/detection_task.*`、`detection_source.*`、`detection_worker_client.*`、`detection_model_snapshot.*` | C UI、E postprocess、F backend |
| E | `domain/detection_postprocess.*`、`application/detection_result_service.*`、`infrastructure/detection_projection.*`、`detection_refined_publication.*` | D task、C panel |
| F | `infrastructure/detection_model_backend.*`、明确登记的独立 GPU helper | D worker client、C UI |
| G | 新增 `tests/*ss_de_rpo*` 或 `tests/*rpo*`、独立 QA 工具脚本 | A～F 生产代码 |
| H | 仓库全部区域**只读** | **禁止修改任何代码或审查证据** |
| 主代理 | CMake/全局 build scripts、跨模块 glue、所有权争议文件、集成/发布 | 不得冒充 H 独立审查 |

如发现新路径或跨代理依赖，必须将接口请求发送给主代理，在 `_delivery/M1_CONTRACT.md` 或追加 ADR 中完成归属认领。绝不允许多人抢改同一个源文件。

## C. 执行阶段

- **波次 0**：A/C/E/F/G 各自在独立工作空间开始审计、写 red test/确认现状；主代理同步读取基线。
- **契约冻结**：主代理决定资源单位/错误码、Starting 状态与 provisional token、worker 性能事件、durable evidence 与 UX-R6 规则。记录在 `_delivery/M1_CONTRACT.md`。
- **波次 1**：B/D 按契约进入开发；A/C/E/F/G 持续推进并对接；所有代理只处理拥有的文件。
- **集成审查**：主代理合并 patch，G 的回归对比测试合格后才调用独立 H 审查。H FAIL 必须继续整改而非换措辞重新宣称 PASS。

## D. Agent 交接工件

每个 A～G 子代理必须提交：`agent-status.md`、`interface-requests.md`、`patch-summary.md`、worktree 分支、commit、最小复现或对照用例、测试命令、PASS/FAIL/BLOCKED 与关键原始证据路径。填报方式参考 `reference/SS-DE-RPO/templates/HANDOFF.md`。

主代理将独立交付登记到 `reference/SS-DE-RPO/_delivery/agents/{A..H}/`，并保存 `agent-roster.md`、`change-index.md`、`test-matrix.md`、`M1_CONTRACT.md`、`FINAL_DELIVERY.md`。巨大二进制测试记录保留在 ignored build 目录，不直接入库。

## E. 关键冲突/资源协调

- GPU/TensorRT 实测须按测试计划预订单卡独占窗口，避免不同代理争用显存导致无效对比。
- 仅在原始基线、确定文件/模型身份变化和最终发布前进行必要 SHA 核对；无需每次读取/修改文件都耗时计算 SHA。
- 不改变其它分支；禁止 `git push --force`、全量 `git reset --hard` 清除用户改动、复制数据集到 Git、未经授权修改软件语义。
- 不能虚构任何运行结果、Commit ID 或独立审核记录。
