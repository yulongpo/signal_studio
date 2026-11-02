# SS-DE-RPO · 子代理交接

- **Agent**：B（Sample 时间精修与分块）
- **任务编号**：SS-DE-RPO M1 Sample 预算错配
- **审查基线及实际执行 HEAD**：`82935ab34ec6126707da152bf46d1f352458ab0c`
- **Worktree 路径**：`D:\project\ss-de-rpo-wt\b`
- **工作分支和 commit**：`fix/ss-de-rpo-b`；SHA 即包含本文件的那次提交（消息 `fix(ss-de-rpo): accept large sample grants and chunk exact residency`）。未推送。
- **文件所有权/实际变更文件**：
  - `infrastructure/detection_sample_stream.cpp`
  - `infrastructure/detection_time_analysis.cpp`
  - `reference/SS-DE-RPO/_delivery/agents/B/*.md`
- **与主代理冻结接口的兼容情况**：按 `M1_CONTRACT.md` 第 2 节实现。未改契约。常量在 A 的头文件落地前使用本地同值副本，见 `interface-requests.md`。

## 1. 已确认根因（带证据分类）

| 问题 | 事实类型 | 路径 | 影响 | 修复 |
|---|---|---|---|---|
| 6 GiB 授权被 `contextValid` 当成非法配置 | CONFIRMED_CODE | `detection_sample_stream.cpp` `contextValid` | 默认 8 GiB 工作集在 Sample 入口被拒 | 删除 `memoryGrantBytes > 1 GiB`；0 授权仍拒绝 |
| `working/12` 得到 512 Mi cells | CONFIRMED_CODE | `detection_time_analysis.cpp` `applyDetectionTimeAnalysis` | 与 256 Mi cell 上限冲突，和授权错误共用一句 InvalidInput | 初始 cell 预算 `min(working/12, 256 Mi)`；超限单独 `sample.work-unit-ceiling` |
| 大授权会使整段精确缓冲具备资格 | CONFIRMED_CODE | `makePlan` `legacyEligible` | 6 GiB 授权可能选中数 GiB 连续分配 | `exact` 还必须 `<= 1 GiB`；长路径仍是 Streaming/Auto |
| 已选计划驻留超过授权 | CONFIRMED_CODE | `makePlan` streaming、`legacy()`、`refineEnergyBlocks` | 低内存必须拒绝或保持更小块 | `ResourceLimit`，detail 含 requested、available、`phase=time-refine` |

旧日志里的 `plan: Invalid sample context/configuration/budgets` 与代码吻合，本轮没有复跑，不标 `CONFIRMED_RUNTIME`。

## 2. 代码变更摘要

见 `patch-summary.md` 与 `sample-root-cause.md`。没有放宽 FIR 尾幅、分位数或精度守卫，没有伪造活动段，没有改数值能量公式。ROI/Coarse 的 `spectrumCellBudget` 回写保持原样。

## 3. 验证

| 用例 ID | 运行环境与命令 | 测试结果 | 日志/证据位置 |
|---|---|---|---|
| R-01 / 96 MiB sample stream / `spectrumCapacityMatchesActualConsumer` | 本工作树无 CMake 缓存、无 `out/`。未配置新的 Qt/VS 构建，未使用 `D:\project\signal_studio_rebuild\out` | BLOCKED_BUILD | `sample-perf.md` 仅有静态字节核算，不是实测 |

## 4. 关键性能与资源峰值

无实测峰值。静态说明见 `sample-perf.md`。6 GiB 授权不再使超过 1 GiB 的精确缓冲具备 `legacyEligible`。

## 5. 向其他代理请求的接口及其处理情况

见 `interface-requests.md`。A 发布常量后由集成者替换本地副本。D 的 steady-clock 打点不在 B 的文件里。G 不要把 R-01 写成「必须拒绝 6 GiB」，也不要把 `makePlan(LegacyExact)` 的超授权查询断言成 ResourceLimit。

## 6. 未完成 / 风险 / 下一步

- BLOCKED_BUILD：集成后应跑 `detection_sample_stream_tests`（含 96 MiB grant）和 `detection_time_pipeline_tests` 的 `spectrumCapacityMatchesActualConsumer`。
- 20/363/2044 窗口、真实 RSS 和 GPU/CPU 耗时均未测。
- 本地常量与 A 的头文件并存时，`detection_time_analysis.cpp` 可能出现无限定名歧义。

## 7. 交接声明

已在自己的 worktree 提交，未修改其它 owner 文件；未用 mock 当作真实硬件；没有修改目标分支以外的正式分支。未推送。
