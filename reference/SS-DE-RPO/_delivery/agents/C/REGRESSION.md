# Agent C2 · UX-R3 工作流回归

角色 Grok C2。基线 `da57e8705f3ef2924a71d64c914afb434050a4cf`，分支 `codex/ss-de-rpo-c`，工作区 `continue-c`。未改 `detection_panel.cpp`，未推送，未做原生验收或 GPU 验收。

## 原失败

主树 Release 全量 CTest 日志（只读）：`D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/N0/logs/integration-release-all.txt`。`signal_studio_ux_r3_workflows` 20 通过、7 失败。

- `saveAsReopenKeepsOriginalAndMovesSaveDestination` 在源文件回到 Ready 后，`tests/ux_r3_workflow_tests.cpp:327` 的 `!isProjectDirty()` 失败。消息：Source reload changed the dirty baseline without a committed user edit。
- `manualLabelEntryPoints` 的六个按钮行（import、cancel-file、cancel-mapping、cancel-preview、switch-file、switch-preview）在约 479 行失败：旧 `resultTab` 不存在。同测菜单行当时通过。UX-R5 底栏来源页是 `importedLabelsTab`、`manualAnnotationsTab`、`detectionResultsTab`。

同日志里已通过、本轮保留断言的脏状态用例：`titleDirtyTracksCommitButNotDraftOrRefresh`、`failedIoRetainsCommittedSession`、`relocateMissingSourceRejectsSameLengthWrongContent`、`arbitraryDirectoryCreateAndDuplicateFailure`、`mappingUnknownAndCancel`。

## 诊断

修复前在本工作区 Debug、offscreen 下单独跑 `saveAsReopenKeepsOriginalAndMovesSaveDestination`，仍在 327 行失败。临时探针比较 `openProject` 捕获的 `savedProjectToken_` 与源扫描结束后的 `stateToken`，探针已删除，不在本次差异里。

源回到 Ready 后，指纹、可用样本、视图和显示与基线一致。持续差异只有注解图层 `100`→`010`，以及 `annotationFilter.source` `0`→`1`。

`prepare()` 保存工程时面板停在导入标签页。该用例随后 `addMark` 并 `refresh()`，面板源页留在 Manual，`bottomTab_` 不跟着改。再次打开原工程时，token 在 `refresh()` 之前捕获；`refresh()` 把残留的 Manual 页写进已保存的导入标签图层和过滤器。扫描结束只恢复可用性，图层和过滤器的改写还在，于是没有已提交用户编辑也会变脏。

未把扫描后的状态回写成干净 token。已提交的改名、手工框选和另存目标在打开之后仍应保持脏，直到成功保存。

## 修复

`MainWindow::openProject` 在 `applyGlobalRightSidebarSettings` 之后、写入 `savedProjectToken_` 之前：若活动文件已保存的来源与面板当前源页不同，用 `setSourceTab(savedSource, false)` 切到已保存来源，再把该文件自己的 `annotationFilter` 和 `annotationLayers` 写回去。`reconcile` 为 false。其后的 `refresh()` 再写覆盖层时与已保存图层一致。只有成功保存会更新 `savedProjectToken_`。

`manualLabelEntryPoints` 在导入按钮不可见时点击 `importedLabelsTab`，并确认三个现行来源页存在、`resultTab` 不存在。成功导入后断言标记 `sourceKind == ImportedLabel`。取消、切换文件、预览原子性、重复导入 token 不变、重开工程和按钮文案 `导入标签…` 的断言都保留。没有跳过数据行。

## 证据

命令都在 `continue-c`，`scripts/build.ps1`，preset `local-debug` / `local-release`，`--parallel 2`。`QT_QPA_PLATFORM=offscreen`，`QT_SCALE_FACTOR=1.5`。父进程 PATH 前置 `D:\softwares\Qt\6.11.1\msvc2022_64\bin`，因为 windeployqt 不复制 `Qt6Test.dll` / `Qt6Testd.dll`。未改部署脚本。

Debug 全量 `signal_studio_ux_r3_workflows` 在既有 CTest 180 秒上限处超时。超时前日志已通过：`saveAsReopenKeepsOriginalAndMovesSaveDestination`、全部六个按钮导入行、`menu-import`、`menu-cancel-file`、`menu-cancel-mapping`，以及上面列出的脏状态用例。这是 Debug offscreen 比 180 秒上限慢，日志停在 `menu-cancel-mapping` 的 PASS，没有 FAIL。

随后同一 Debug 二进制用 `-UiCases` 补跑未写完的行：`menu-cancel-preview`、`menu-switch-file`、`menu-switch-preview`、`noLabelsStillAllowsManualPlotCreation`。CTest 14.78 秒通过。QtTest `Totals: 6 passed, 0 failed`（含 init/cleanup），14429 ms。`noLabelsStillAllowsManualPlotCreation` 仍要求手工框选之后工程为脏。

Release：`scripts/build.ps1 -Configuration Release -Action All -Jobs 2 -Targets SignalStudioUxR3WorkflowTests -TestRegex '^signal_studio_ux_r3_workflows$'`。配置、编译和测试一次完成，退出码 0。CTest 19.16 秒通过。日志 `out/vs2026-qt611-release/ux-r3-workflows-release.txt`：`Totals: 27 passed, 0 failed, 0 skipped, 0 blacklisted, 18595ms`。其中包括 `saveAsReopenKeepsOriginalAndMovesSaveDestination`、12 行 `manualLabelEntryPoints`、`noLabelsStillAllowsManualPlotCreation` 和上述脏状态用例。编译警告来自既有源码，不是本差异。

未跑原生 4K 验收，未跑 GPU smoke，未写主树 `D:/project/signal_studio_rebuild` 的构建目录。
