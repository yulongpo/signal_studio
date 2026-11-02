# SS-DE-RPO · Agent G patch 摘要

只增加回归测试和本目录说明。生产源码与 `CMakeLists.txt` 未改。

| 路径 | 变更 |
|---|---|
| `tests/ss_de_rpo_logic_tests.cpp` | 新建。独立 `main`，失败返回非零。R-01、R-04、R-05、R-12。 |
| `tests/ss_de_rpo_ui_tests.cpp` | 新建。`BLOCKED_UNTIL_AGENT_C`。`main` 打印 SKIP 并返回 0。 |
| `reference/SS-DE-RPO/_delivery/agents/G/*.md` | 用例索引、红绿期望、性能对照（无数字）、状态、本摘要、CMake 请求。 |

R-01 断言方向是契约期望：6 GiB grant 加等于 ceiling 的 `workingCellBudget` 时，`makePlan` 不得因 `Invalid sample context/configuration/budgets` 失败。基线上该断言应变红。`workingCellBudget == 256*1024*1024+1` 必须拒绝。

R-04/R-05 用手写 `DetectionSourcePlan`（2044 × 196608，JSON 每窗 64 字节，证据预算 64 MiB）调用 `applyDetectionEvidencePreflight`。短卷用 `setDetectionVolumeFreeBytesProbeForTest`，剩余 1 MiB。

R-12 调用 `preserveDurableModelEvidence`，假对象为 `lastInference.h2dMs=1.5`。非持久路径不得写入 source set；`rawHeadBudgetBytes==4096` 与 `legacyCombinedEvidence` 保留证据。

未做完整构建，标记 `BLOCKED_BUILD`。
