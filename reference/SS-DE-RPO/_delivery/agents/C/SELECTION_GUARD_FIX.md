# C6 选择写守卫交接

- Agent：C6，Grok 4.7 Fast / xhigh，无子代理，无远端写入。
- Worktree：`D:\project\ss-de-rpo-wt\continue-a`
- 分支：`codex/ss-de-rpo-c6`
- 基线：`83e365f4c7b621809a66852e6d7727009a311db3`
- 依据：`D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/agents/results/G3/FINAL.md` 的 P1。P0 标签迭代和窄带通道直接写不在本代理文件内。
- 所有权：`application/annotation_service.cpp`、`application/annotation_service.h`、`application/session.cpp`、`application/session.h`、`tests/annotation_tests.cpp`、`tests/core_tests.cpp`、本文件。
- 本文与代码在同一提交。该提交的 SHA 不能写进自身；交接回复里的 `git rev-parse HEAD` 就是这个提交。

## 修复

项目写守卫现在挡在持久选择突变之前。拒绝时不 normalize、不 reconcile、不改选择字段。守卫放开后，普通选择照常生效，并只通过既有 `wrote` 通知一次。守卫是事务门，不是人工编辑权限。Imported 与 Algorithm 在门打开时仍可选择；`canEdit`、几何、改名和 `deleteBatch` 仍拒绝它们。

悬停只改 `hoveredMarkId` / `hoveredSegmentId`，不查守卫，也不发 `wrote`。已经干净的 `reconcileSelection` 同样不查守卫。会改持久字段的 reconcile 才先查守卫。

| 入口 | 守卫时机 |
|---|---|
| `select` | 先只读校验活动 id 和段。非法请求直接失败，不查守卫。合法请求，包括与当前选择相同的请求，在 normalize / 写入之前查 `blockedWrite`。 |
| `chooseSelection` | 先只读校验目标、段和发起视图顺序。失败不查守卫。通过后在 normalize 之前查守卫，锚点在同一次快照里写完再通知。 |
| `reconcileSelection` | 先做只读计划。持久字段不会变时只更新悬停。否则在 apply 之前查守卫；拒绝时悬停也不改。 |
| `normalizeSelection` | 仅给已经过守卫或已经 `record` 的内部路径使用，不再走公共 reconcile。 |
| `Session::setProjectWriteGuard` | 把同一个 `acceptProjectWrite` 装到 `AnnotationService`。不在这里安装 `wrote` 通知，避免只设置会话守卫时把每次 `record` 都算进内容代。 |
| `Session::selectMarks` | 转给 `AnnotationService::select`。无活动文件返回失败。拒绝时选择字段不变。 |
| `Session::focusMark` | `select` 失败则在恢复锚点和 `setView` 之前返回 false。成功时在一次抑制通知里恢复原锚点，再按最终快照通知一次。视图若真变化，`setView` 仍自己调用 `noteProjectWrite`。 |
| `Session::deleteSelectedMarks` | 关联通道的删除仍由会话守卫提前返回。标记删除成功后的清空选择放在 `WroteSuppression` 里；这次 `select` 若被拒绝，不再写 `psdFromSelection`。 |
| `Session::activateFile` | 导航需要变化时仍先查会话守卫。当前文件的 reconcile 失败则返回 false，并且不再对该文件调用 hover。 |

`selectionAnchorId` 和 `selectionSampleChannel` 不在 `ProjectStore::stateToken` 里，但会被提交快照盖回，所以和 `selectedMarkIds`、`activeMarkId`、`activeSegmentId`、`psdFromSelection`、标注 `activeSegmentId` / `range`、修订编辑 id 一起纳入守卫快照。悬停不在该快照里。

一次命令只通知一次。`createManual`、`deleteBatch`、`deleteAlgorithmBatch` 已由 `record` 通知，随后的选择整理不再通知。`deleteSelectedMarks` 和 `focusMark` 用 `WroteSuppression` 吃掉内层 `select` 的通知。`undo` / `redo` 仍不发 `wrote`。`notifying_` 挡住通知回调里的重入。重复的相同选择不增加内容代。

## 复现与测试

G3 的界面复现是：检测 StartCommit 或 PublishCommit 持有冻结工程时，图谱或列表仍能改选择；提交结束把旧快照写回，选择丢失。本代理没有跑这条 UI、原生或 GPU 路径。

先把四个实现文件退回 `83e365f`，用同一套新测试编译。旧测试全部通过，新断言失败：

- `signal_studio_annotations`：`FAIL selection write guard: Rejecting guard must refuse an unchanged selection before reconcile`
- `signal_studio_core`：既有 9 条 PASS 之后，`FAIL Rejecting guard must block Session selection changes`

恢复实现后，同一隔离 Debug 构建里两个 CTest 都通过。annotation 24 条 PASS，core 10 条 PASS，其中包括原通道守卫测试和新选择测试。

annotation 测试不链接 `SignalStudioInfrastructure`，因此用字段指纹、修订向量、历史和悬停断言。`ProjectStore::stateToken`、内容代和工程代在 core 测试里检查。段切换会改变 token 里的活动段和投影范围，但不改变 `revisionId` 或修订条数。

## 命令与结果

隔离目录：`out/tasks/ss-de-rpo/C6/`。没有使用 `scripts/build.ps1`，没有写入 `out/vs2026-qt611-*`。

1. 配置，退出码 0。

```
cmake -S . -B out/tasks/ss-de-rpo/C6/local-builds/debug -G "Visual Studio 18 2026" -A x64 -DSS_QT_ROOT=D:/softwares/Qt/6.11.1/msvc2022_64 -DSS_BUILD_TESTS=ON -DSS_DEPLOY_RUNTIME=OFF -DSS_OUTPUT_PREFIX=tasks/ss-de-rpo/C6/isolated
```

2. 基线实现加新测试：`cmake --build out/tasks/ss-de-rpo/C6/local-builds/debug --config Debug --target SignalStudioCoreTests SignalStudioAnnotationTests --parallel`  
   日志：`out/tasks/ss-de-rpo/C6/logs/red-build.log`、`red-build-core.log`。退出码 0。

3. 基线 CTest，退出码 8。日志：`out/tasks/ss-de-rpo/C6/logs/red-ctest.log`。  
   `PATH` 前置 `D:\softwares\Qt\6.11.1\msvc2022_64\bin`。

```
ctest --test-dir out/tasks/ss-de-rpo/C6/local-builds/debug -C Debug -R "^(signal_studio_core|signal_studio_annotations)$" --output-on-failure
```

4. 恢复修复后的构建，退出码 0。日志：`out/tasks/ss-de-rpo/C6/logs/green-build.log`。同一条 build 命令。

5. 修复后 CTest，退出码 0。日志：`out/tasks/ss-de-rpo/C6/logs/green-ctest.log`。  
   直接运行可执行文件的完整 PASS 列表：`green-annotation.txt`（退出码 0，24 条）、`green-core.txt`（退出码 0，10 条）。  
   可执行文件在 `out/tasks/ss-de-rpo/C6/isolated-debug_bin/`。

未跑全量 CTest、UI、GPU、TensorRT、原生 4K 或 headless IQ。

## 主代理后续

这些调用方在选择被拒绝后仍继续写或继续手势。本代理未改这些文件。

- `app/main_window.cpp` `selectMark` 4795–4830：空 id 的 `selectMarks({})` 被忽略，`result` 保持默认成功；非空路径记录 `chooseSelection` 失败，但两种路径都会写 `psdFromSelection`，并继续 `propagateGlobalRightSidebarSettings`、`refresh`、`scheduleRightSidebarSettingsSave`。拒绝后应在 PSD 写之前返回。
- `selectAllMarks` 4832–4848：忽略 `selectMarks` 的返回值，随后仍写 `psdFromSelection`。
- `locateMark` 4850–4855：忽略 `focusMark` 的 false，随后仍 `refresh` 并打开检查页。
- `refresh` 3222：忽略 `reconcileSelection`。干净选择不会打守卫。若提交期间 reconcile 确实要改持久字段，会打到守卫并可能发出节流后的“正在提交”提示。
- `ui/charts/plot_widget.cpp` 3155–3168：活动标记变化时忽略 `select` 的结果，手势仍进入 `MarkEdit`。拒绝时应中止这次编辑手势。3740–3751 的全选忽略 `selectMarks` 后仍 `emit stateChanged`。
- `ui/annotations/annotation_panel.cpp`：复选框 294–298、`chooseRow` 1063–1068、键盘 1162–1168、相邻段 1986–1988 已在失败时停止。`editCell` 1199–1201 忽略 `select` 后继续编辑。定位按钮 869–872 和 653–656、表格定位 741–744 在 `focusMark` 失败后仍 `emit changed`。删除 1458 和隐藏 1558 忽略 reconcile 后继续用当前选择。`refresh` 1635、来源 Tab 592、本页选择 677、清选 719、过滤 1692、全选 1116 忽略 `selectMarks` / reconcile。
- `ui/detection/detection_card.cpp` 730–740：段组合框在 `select` 失败时发消息，但仍 `refresh()`；只有成功才发 `selectionChanged`。

`DetectionPanel` 现有接线保持有效：先 `setProjectWriteGuard`，再覆盖 annotation 守卫，再单独 `setWroteNotifier`。不要把通知装进 `setProjectWriteGuard`。

Headless publication 依赖 `AnnotationService`。本次没有放宽来源身份规则。主代理可以重跑原来的 IQ / publication 套件，而不是改那些断言。窄带通道直接写仍属 C5。`MainWindow::selectMark` 与标签模态仍属 main。
