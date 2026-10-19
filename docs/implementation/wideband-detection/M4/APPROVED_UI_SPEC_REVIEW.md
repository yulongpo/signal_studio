# M4 已批准 UI 变更 SPEC 复审

结论：**SPEC_PASS_APPROVED_UI**。本次 SCN 噪底裕量控件例外和 UI CTest 480 秒终止保护符合用户已批准的两项决定，未发现本次增量的阻断项。本结论限定下述三个 canonical 文件中的批准变更，不代表 M4 整体通过。

## 依据与范围

`SCN_PARAMETER_DECISION.md` 与 `UI_TEST_BUDGET_DECISION.md` 最后的决定段均记录：2026-10-14 用户回复“按建议确认，并继续推进完成任务”，批准建议 A。因此两项决定已获批准，不再属于等待确认。文件中较早的待确认说明保留为历史。

本次只读核对 `CMakeLists.txt`、`ui/detection/detection_card.cpp`、`tests/ui_tests.cpp`，读取已完成的 `out/tasks/wideband-detection/M4/logs/approved-scn-ui-case.txt`。未修改源码、原型或 freeze，未构建、执行测试或 GPU，也未读取正在执行的 Debug All 日志判定通过。HEAD 差异还含此前 M4 的阶段显示及测试目标；这些既有增量不属于本次批准的审查对象。

| 当前 canonical 文件 | SHA-256 |
|---|---|
| `CMakeLists.txt` | `d37722d664c4c2c7518257c378071cce3272535b61c6d70cf9a3f112aac036cc` |
| `ui/detection/detection_card.cpp` | `064a0e300d53f3ebc3d68ccec177c317e0a35887f3e516e5ab83bd55a5f820a6` |
| `tests/ui_tests.cpp` | `c504fc656ee42bc9dddb95dceb6dcbd01bf2f2a027c1ec42beaf85d6e9a803ff` |

## SCN 参数契约

`DetectionCard::refreshBranchState()`（`detection_card.cpp:801–829`）只为 SCN 增加 backend 条件：`noiseMargin_->setEnabled(!scn && strategy != DetectionTimeStrategy::Coarse)`。SCN 的 Sample、ROI、Coarse 以及从历史快照载入的 None 都禁用该控件；非 SCN 继续采用原来的 `strategy != Coarse` 条件。edge/smooth、min on/off、Coarse gap/min burst 与 split 的启用条件未变。控件位置、分支按钮及运行/取消动作没有因这项例外改变。

`refreshTiming()`（759–775）明确说明“CNR / 频率收边固定 3 dB；Coarse / 粗回退固定 6 dB”，并说明噪底裕量不可调而已保存值保留。SCN ROI 帮助改为自适应门限及 SCN 裕量固定；非 SCN ROI 帮助保持原文。该增量只改变控件启用和说明，不将存储的 dB 数值接入 Sample/ROI 数学，也不改变固定门限。

禁用控件没有写入 `configuration_.parameters.bandwidthMarginDb`。现有 `configure()`（1099–1121）复制完整历史 request，`loadControls()`（599–625）从已存储成员加载数值；`switchProfile()`（587–597）先保存当前 profile 配置，再恢复目标 profile 配置。由此 SCN 的历史值与其它 profile 的已提交值分别保留。历史 None 仅作为载入快照边界核对，本次没有新增 None 可选动作或承诺它可启动检测。

`widebandConfigurationCommittedDrafts()` 的新增断言（`ui_tests.cpp:4302–4339`）先在非 SCN Sample 提交 margin=9，再加载 SCN margin=11.25 快照，逐一切换 Sample/ROI/Coarse 并核对禁用与 11.25 不变；随后加载 None 快照继续核对。切回夹具后检查 margin 重新启用且恢复为 9，score 仍为 0.8。原有输入草稿、Enter 提交、滚轮及文件切换后的 score 检查继续保留。

已完成的 Debug Qt 日志显示 `initTestCase`、`widebandConfigurationCommittedDrafts`、`cleanupTestCase` 共 **3 passed、0 failed、1931 ms**。这表示一个场景及初始化/清理通过，不是三个独立业务场景，也不是全套 UI 回归。日志中的 QRhi 不受支持提示不构成 GPU 或规定第二显示器的原生验证证据。

## UI 运行预算

`CMakeLists.txt:345–351` 中批准的差异仅为 `signal_studio_ui` 的 `TIMEOUT 300` → `TIMEOUT 480`。同一测试仍调用 `cmake/run_ui_test.cmake`、同一 `SignalStudioUiTests` 目标和日志参数，环境仍为 `QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5`；其它已有测试 timeout 未在本次变更中调整。测试场景和旧断言未删减，新增的 SCN 断言属于上述既有场景。

480 秒是 CTest 终止保护，不是交互性能验收目标或新的通过结果。旧 300 秒超时记录仍有效；本次定向场景日志不能关闭新预算下完整 Debug CTest 验证。正在执行的隔离完整候选必须以结束后的真实结果另行验收。

## 冻结与完成边界

本次独立按 working-file 原始字节重核 `execution-freeze-r3-release.json` 的全部 **72 个 inputs：72/72 SHA-256 相符、0 缺失/差异**。三个获批文件在该 freeze 的 `files` 中均为 0 个条目。freeze 文件 SHA-256 仍为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`；本次 UI 变更没有改变该冻结矩阵输入。

允许继续推进本次批准变更的后续验证。canonical 的 evidence-r4/IO 草稿尚未集成；新完整 Debug 回归、新原生执行以及正式矩阵完成结果均不由本报告替代。本报告不宣布效果 gate、全部 M4 回归或 M4 节点完成，不授权提交或进入 M5。
