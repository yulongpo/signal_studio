# Agent A 状态

- **Agent**：A（资源预算）
- **任务编号**：SS-DE-RPO M1，Sample 授权与工作单元分离
- **审查基线及实际执行 HEAD**：`82935ab34ec6126707da152bf46d1f352458ab0c`
- **Worktree 路径**：`D:\project\ss-de-rpo-wt\a`
- **工作分支**：`fix/ss-de-rpo-a`
- **commit**：本文件所在提交。提交前基线为上面的 HEAD。未推送，未 amend。
- **文件所有权/实际变更文件**：`domain/detection.h`、`domain/detection.cpp`、`ui/detection/detection_resource_options.cpp`，以及 `reference/SS-DE-RPO/_delivery/agents/A/`。`detection_resource_options.h` 无改动。
- **与主代理冻结接口的兼容情况**：`M1_CONTRACT.md` 未改。`DetectionRequest` 未增加字段。新类型仅运行时使用。

## 1. 已确认根因

| 问题 | 事实类型 | 路径 | 影响 | 修复 |
|---|---|---|---|---|
| 默认 8 GiB 工作集变成 6 GiB 授权和 512 Mi cells，再被 1 GiB 与 256 Mi cell 检查拒绝 | CONFIRMED_CODE | `detection_time_analysis.cpp` 的 `working*3/4` 与 `working/12`；`detection_sample_stream.cpp` `contextValid` | 默认 Sample 精修报非法上下文 | 行为修复归 **B**。A 提供分离后的决策函数与常量 |
| 1 GiB 被当成授权上限 | CONFIRMED_CODE | `memoryGrantBytes>1024*MiB` | 授权越大越被拒绝 | A 将 1 GiB 定义为连续分配上限。大于该值且块仍在授权内时 `Adapted`，不是 `Rejected` |

## 2. 代码变更摘要

见 `patch-summary.md` 与 `resource-policy-contract.md`。

## 3. 验证

| 用例 | 命令 | 结果 | 说明 |
|---|---|---|---|
| `decideSamplePhaseBudget` / `checkedMulU64` 本地驱动 | 在 `D:\project\ss-de-rpo-wt\a`，MSVC 14.51 x64：`cl /nologo /std:c++20 /EHsc /utf-8 /I. /Foout\tasks\ss-de-rpo-a\ domain\detection.cpp out\tasks\ss-de-rpo-a\decision_check.cpp /Feout\tasks\ss-de-rpo-a\decision_check.exe && out\tasks\ss-de-rpo-a\decision_check.exe` | PASS，退出码 **0**，输出 `PASS` | 驱动在 gitignore 的 `out/tasks/ss-de-rpo-a/`，不进入本提交。覆盖：授权 0、cell 上限、块超过授权、授权大于 1 GiB 且块不超过连续上限时 Accepted、块超过连续上限且不超过授权时 Adapted 且 `granted` 为上限、乘法溢出 `out==0` |
| `tests/detection_resource_options_tests.cpp` | 未运行 | BLOCKED | 需要既有 Qt CTest 目标。本提交未改 `applyTo` 数值与 QSettings 键 |
| R-01 现有 `makePlan` / `contextValid` | 未运行 | BLOCKED | B 尚未改 Sample 路径。现有代码仍会拒绝 6 GiB grant。这不是 A 的纯函数行为 |
| GPU | 未运行 | 无 GPU 结果 | 不声称推理、显存或批大小测量 |

## 4. 关键性能与资源峰值

无。本提交不分配 Sample 工作集，也不查询 GPU。

## 5. 向其他代理请求的接口

见 `interface-requests.md`。B 拥有 Sample 行为修复。D 不需要等链接 A 才能编译预检。F 的 batch 默认值未改。

## 6. 未完成 / 风险 / 下一步

- 在 B 合入前，默认检测仍会在 `contextValid` 被拒绝。A 不能改那些文件。
- `decideSamplePhaseBudget` 还没有生产调用方。B 集成前它只是可单测的纯函数。
- 两种拒绝同时成立时先报告 work-unit。主代理若要先报告 chunk，需要再改顺序。
- 资源选项的只读标签未经 Qt 测试进程点击。标签不写入请求。

## 7. 交接声明

已在自己的 worktree 提交，未修改其它 owner 文件；未用 mock 当作真实硬件；没有修改目标分支以外的正式分支；没有 push。
