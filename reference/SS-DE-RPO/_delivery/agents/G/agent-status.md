# SS-DE-RPO · 子代理交接

- **Agent**：G（QA）
- **任务编号**：SS-DE-RPO M1 回归，R-01 / R-04 / R-05 / R-08 / R-12
- **审查基线及实际执行 HEAD**：`82935ab34ec6126707da152bf46d1f352458ab0c`
- **Worktree 路径**：`D:\project\ss-de-rpo-wt\g`
- **工作分支和 commit**：`fix/ss-de-rpo-g`。提交说明 `test(ss-de-rpo): add sample grant and raw-head regressions`。SHA 以该提交的 `git rev-parse HEAD` 为准，不在提交后再改本文件。
- **文件所有权/实际变更文件**：只新增 `tests/ss_de_rpo_logic_tests.cpp`、`tests/ss_de_rpo_ui_tests.cpp` 与本目录交付说明。未改生产源码，未改 `CMakeLists.txt`。
- **与主代理冻结接口的兼容情况**：逻辑用例只用基线已有的 `makePlan`、`applyDetectionEvidencePreflight`、`setDetectionVolumeFreeBytesProbeForTest`、`preserveDurableModelEvidence`。UI 等待 C 的 `DetectionCard::startLatchActive()`，契约第 4 节已冻结，G 不另造符号。

## 1. 已确认根因（带证据分类）

| 问题 | 事实类型 | 路径/行号或日志 | 影响 | 修复 |
|---|---|---|---|---|
| 6 GiB grant 与 1 GiB 上下文上限冲突 | CONFIRMED_CODE | `infrastructure/detection_sample_stream.cpp` `contextValid`，`memoryGrantBytes>1024*MiB` | R-01 在基线上应为红 | 属 B。G 只加断言 |
| cell ceiling 相等允许、+1 拒绝 | CONFIRMED_CODE | 同函数 `workingCellBudget>256*1024*1024ULL` | ceiling 用例期望通过 | 属 B |
| raw-head off/on 预检分支 | CONFIRMED_CODE | `infrastructure/detection_task.cpp` `applyDetectionEvidencePreflight` | R-04/R-05 期望保持现有拒绝文案 | 属 D。G 不改行为 |
| durable evidence 条件 | CONFIRMED_CODE | 同文件 `detectionModelEvidenceIsDurable` | R-12 复用现有测试语义 | 属 D/E |
| 启动闩锁 API 缺失 | CONFIRMED_CODE | `ui/detection/detection_card.h` 无 `startLatchActive`；`submit` 为 private | R-08 不能在基线上编译期望断言 | 属 C |

## 2. 代码变更摘要

新增两个测试翻译单元和本目录说明。逻辑测试在失败时打印 `Failure::detail` 或预检 `QString`。R-01 要求 6 GiB + cell ceiling 的 `makePlan` 成功。UI 测试打印 SKIP 并返回 0。

## 3. 验证

| 用例 ID | 运行环境与命令 | 测试结果 | 日志/证据位置 |
|---|---|---|---|
| R-01 | 未链接、未运行 | BLOCKED_BUILD；期望 RED | `red-green-results.md` |
| R-01 ceiling / +1 | 同上 | BLOCKED_BUILD；期望 PASS | 同上 |
| R-04 / R-05 | 同上 | BLOCKED_BUILD；期望 PASS | 同上 |
| R-12 | 同上 | BLOCKED_BUILD；期望 PASS | 同上 |
| R-08 | 同上 | BLOCKED_UNTIL_AGENT_C；SKIP 返回 0，不是 UI 通过 | `tests/ss_de_rpo_ui_tests.cpp` |
| R-15 | 无 engine 运行 | UNVERIFIED_GPU | 无日志 |

## 4. 关键性能与资源峰值

未测。见 `performance-comparison.md`。没有资源 JSON。

## 5. 向其他代理请求的接口及其处理情况

见 `interface-requests.md`。CMake 登记交给主代理。`startLatchActive` 已在 M1 契约，由 C 实现。

## 6. 未完成 / 风险 / 下一步

- 主代理把两个 `add_executable` 写入 `CMakeLists.txt` 后才能得到真实退出码。R-01 在未合并 B 时应为非零。
- C 落地后把 UI SKIP 换成按钮双击断言，并改用 `SignalStudioUxTests` 的 UI 源链接。需要真实 IQ 与已保存工程，空 `projectDirectory` 不会发出 `requestSubmitted`。
- R-02～R-07、R-09～R-11、R-13～R-20 本提交没有新用例。
- 未推送。未代替 H 做终审。

## 7. 交接声明

已在自己的 worktree 提交，未修改其它 owner 文件；未用 mock 当作真实硬件；没有修改目标分支以外的正式分支。未写入 `D:\project\signal_studio_rebuild`。
