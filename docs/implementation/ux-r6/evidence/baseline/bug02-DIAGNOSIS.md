# BUG-R6-02 独立诊断（只诊断，未实施）

时间：2026-10-26，Asia/Shanghai。诊断起止核验 HEAD 均为 `ec511662d4d84d5b9757576d24ed2124b6d8a029`。

## 结论

**已复现。旧正式结果没有在模型切换/参数提交时被删除，但被统一上下文判定排除，导致表格、图上叠加、选择/工程树入口同时失去旧结果；保存重开仍隐藏。** 根因是 `CurrentDetectionSlot::stale` 同时承担“配置待更新”和“正式结果不可见”两种语义。R6 要求前者不得导致后者。

未提交的数值草稿、直接撤销草稿及普通刷新在本次正例中没有隐藏旧结果；按 Enter 提交和切换模型均稳定触发。不能把根因写成 `refreshSource/loadControls` 无条件删除结果。

运行路径 `beginDetection`、失败/取消路径 `abandonDetection` 同样置 stale，因此仅修参数信号或重新填表不足以修复。已实现的完整成功事务（包括零结果）应保留。

## 范围和依据

- 已读取根 `AGENTS.md`、`reference/ux-r6/{README,PLAN,CODEX_MASTER_PROMPT,ACCEPTANCE_MATRIX}.md`、`docs/implementation/ux-r5/{DESIGN,DATA_LIFECYCLE}.md`。
- 本次明确的“只诊断”委托覆盖任务包主代理实施指令；不担任最终 Reviewer，不发布审核结论。
- 所有本次新增工具/日志/本报告均在 `out/tasks/ux-r6/diagnostics/bug02/`。没有修改产品、仓库测试、CMake、reference、受审文件或用户项目/数据；没有 commit/push/fetch；没有批量 SHA 或大型 RFbench。
- 试验使用临时目录内受控 IQ/检测记录，数据出自现有生命周期夹具，实际调用生产存储、正式事务与 Qt 窗口。不是模型检出质量或 GPU 推理证据。

## 具体调用链（行号对应上述 HEAD）

### 参数和模型触发

1. 数值控件 `ui/controls/adaptive_value_edit.cpp:126–140` 在有草稿且解析成功时提交，发出 `valueCommitted`；`:142–153` 为撤销及 Enter/Escape 路径。
2. `ui/detection/detection_card.cpp:405–417` 接收参数提交，先校验 candidate，再调用 `saveConfiguration`；`:711–717` 更新下一次请求配置并发出 `configurationCommitted`。整数控件等价路径在 `:355–372`。
3. 模型/输入谱的 `currentIndexChanged` 在 `ui/detection/detection_card.cpp:290–295` 进入 `switchProfile`；`:720–736` 取各方案配置、撤销未提交输入、切换配置，并在 `:734` 发出同一信号。
4. `app/main_window.cpp:1363–1364` 把该信号直接连接到 `DetectionPanel::markConfigurationStale`。
5. `ui/detection/detection_panel.cpp:814–824` 获取当前 FileId + sampleChannel，先取消相同 scope 的运行 ticket，再调用 `DetectionResultService::markStale`，然后 refresh 并 `emit changed()`。
6. `application/detection_result_service.cpp:439–453`：没有 slot 则返回；存在 pending 时退役 pending run、移除其任务索引、清 pending 并递增 generation；最终 `slot.stale=true`。**不移除 committedRunId、不删除已提交 sourceSet/marks**。
7. `app/main_window.cpp:1185` 的 changed 连接触发窗口 refresh；`:3014–3026` 先协调 selection，后刷新投影。

### 隐藏发生的位置和可见集合

`domain/project.h:237–255` 的 `annotationInContext` 已经处理 deleted、FileId、sourceSet、sampleChannel；但 Algorithm + 有 slot 的分支在 **`:249–250` 额外要求 `!slot.stale`**，再核对 `committedRunId == source.runId`。

因此对于原本满足范围条件的旧正式算法集合 C：

- 参数提交前：inContext(C)=true；提交后：stale=true，inContext(C)=false。
- 已存集合仍是 C，committedRunId 仍是旧 run；变化发生在共享 eligibility/context，而不是清理存储。
- `annotationMatchesFilter` (`domain/project.h:257–270`) 先依赖 inContext；`annotationEligibleForTab` (`:272–275`) 再要求来源一致。`AnnotationTableModel::update` (`ui/annotations/annotation_panel.cpp:259–271`) 因而不再将 C 放入 rows。
- `annotationOverlayVisible/annotationVisible` (`domain/project.h:281–287`) 也先要求 inContext。绘图在 `ui/charts/plot_widget.cpp:2574` 跳过它；命中/交互分支 `:2734,:2814,:2872,:3613` 亦使用该资格。
- `annotationSelectable` (`domain/project.h:277–279`) 直接返回 inContext；`application/annotation_service.cpp:1770–1782` 在 reconcile 时删除已无资格的 selected/active ID。工程树来源计数 `app/main_window.cpp:2288–2291` 和 ID 集合 `:2423–2433` 使用相同判定。
- 当前状态文案 `ui/annotations/annotation_panel.cpp:1664–1665` 明确把旧集合描述为“仅保留作回滚恢复”，与 R6 要求的“当前显示上次检测结果 / 待更新”冲突。

注意：来源 Tab 本来就是唯一叠加来源；切到人工页时不绘制算法来源是既有合同。修复不能让算法结果绕过当前 Tab、查询、分数过滤或 sampleChannel 隔离。

### 运行、失败、提交及保存

- 启动：`ui/detection/detection_panel.cpp:687–707` 在 Project candidate 上 begin，保存 `.ssproj` 成功后交换会话。`application/detection_result_service.cpp:406–408` 设置 stale、pendingRunId 和 generation。即使用户未先改配置，开始新任务也会隐藏旧正式集合。
- 失败/取消/来源失效：`ui/detection/detection_panel.cpp:776–786` 调用 abandon；`application/detection_result_service.cpp:429–437` 退役 pending、清 pending、保留 committed，但继续 stale=true，旧集合仍隐藏。
- 发布失败：`ui/detection/detection_panel.cpp:796–806` 只走 replaceCurrentDetection 并报告错误，失败不交换正式候选；此前已 stale 的旧集合仍被过滤。
- 完整成功：`application/detection_result_service.cpp:464–480` 验 token/context、coverage 和预算；`:482–501` 在副本退役旧集合、发布新集合、更新 slot；`:503–515` 回读 manifest/chunk/publication 比对；`:517–523` 先保存后交换会话。`infrastructure/project_file.cpp:355–362` 使用 QSaveFile 原子 commit。零结果也走同一路径，没有“必须有一个结果”的门槛。
- 保存重开：`infrastructure/project_store.cpp:579` 编码 stale、`:684` 原样解码；重开没有将 stale 恢复为 false，因此隐藏被持久化。实际探针已验证，而非只凭代码推断。
- 任务发现：`infrastructure/detection_store.cpp:1078–1108` 只枚举权威 slot 的 committed/pending，拒绝同时 retired 的 run；不扫描恢复全部旧目录。此边界无需为可见性修复而放宽。

### refreshSource/loadControls 排除项

`ui/detection/detection_card.cpp:739–777` 使用 updating_ 和信号阻断器回填控件；`:780–812` 只有上下文 key 改变才回填/丢弃草稿，没有直接改正式 marks/slot。探针在刷新、输入但未提交、撤销之后 configurationCommitted 次数仍为 0，表格保持 1 行。已经证实的触发点是提交/模型切换以及 begin/abandon，不应泛称 refresh 删除结果。

## 最小可信复现和实际记录

工具：`probe.cpp`、`build-probe.ps1`、`run-probe.ps1`。重新执行：

```powershell
& ./out/tasks/ux-r6/diagnostics/bug02/build-probe.ps1
& ./out/tasks/ux-r6/diagnostics/bug02/run-probe.ps1
```

工具以 unchanged `tests/ux_r5_detection_tests.cpp` 的 Harness 构造 **1 个真实持久化的正式对象**，复用已构建的生产 MainWindow/DetectionCard/AnnotationPanel 对象和 Core/Infrastructure 库；不重写这些实现。打开该临时 `.ssproj`，用 Qt 键事件修改 `detectionScoreThreshold`，直接驱动模型 QComboBox 的 currentIndexChanged，再真实保存重开。参数提交前强制检查 `tableRows == 1`，排除空夹具导致的伪通过。使用独立临时 QSettings，探针 TEMP/TMP 位于本诊断目录，QTemporaryDir 自动回收。

环境为 Windows 11 x64 / Qt 6.11.1 Debug / offscreen / QWidget 软件路径；不截图、不进行物理屏尺寸验收。probe 与生产对象链接的诊断程序出口为 0，表示 **基线缺陷被成功复现**，不是 R6 验收通过。日志见 `probe-debug.jsonl` 和 `probe-exit.txt`。

| phase | Algorithm 存储数 | tableRows | eligible / overlay / selectable | stale | committed |
|---|---:|---:|---|---|---|
| ui-before | 1 | 1 | 1 / 1 / 1 | false | ui-old |
| ui-uncommitted-draft | 1 | 1 | 1 / 1 / 1 | false | ui-old |
| ui-discarded-draft | 1 | 1 | 1 / 1 / 1 | false | ui-old |
| ui-tab-roundtrip | 1 | 1 | 1 / 1 / 1 | false | ui-old |
| ui-parameter-enter | 1 | 0 | 0 / 0 / 0 | true | ui-old |
| ui-parameter-save-reopen | 1 | 0 | 0 / 0 / 0 | true | ui-old |
| ui-before-model-switch | 1 | 1 | 1 / 1 / 1 | false | ui-old |
| ui-model-switch | 1 | 0 | 0 / 0 / 0 | true | ui-old |

同一探针还在真实存储事务中记录：

- `core-mark-stale` / `core-stale-reopened`：旧存储 1，可见资格 0。
- `core-pending-before-commit`：pending=core-next，committed=core-old，旧存储 1，可见资格 0。
- `core-running-rejected` / `core-failed-rejected` / `core-cancelled-rejected`：分别提交非 Completed 快照，均被拒；committed 和旧对象保持，可见资格仍为 0。此处是**受控生命周期/事务状态注入，不是真实 worker 崩溃**。
- `core-abandoned` / `core-abandoned-reopened`：清 pending 后旧存储仍 1、stale 仍 true、可见资格仍 0。
- `core-success-nonzero`：完整成功切到 core-replacement，存储 1，可见资格恢复 1。
- `core-success-zero` / `core-success-zero-reopened`：完整零结果切到 core-zero，旧对象数为 0，stale=false；重开仍为空。

overlay 列测量的是生产绘制/命中共用谓词，不是像素截图或 GPU 读回；tableRows 是真实 Qt 表模型行数。probe stderr 仅有 offscreen 字体目录提示。最初的探针编译设置问题和插件路径问题已修正；`build.log` 保留首次编译失败，`build-final.log` 为最终成功，不计作产品失败。

## 现有测试、冲突断言与实际执行

| 已有入口 | Debug | Release | 证据 |
|---|---|---|---|
| SignalStudioUxTests.exe uxR01_lateRunFilterPopupAndRefresh -v2 | 3/3（init + 1 业务 + cleanup），exit 0，4343 ms | 3/3，exit 0，1096 ms | existing-ui-debug.log / existing-ui-release.log |
| SignalStudioUxR5DetectionTests.exe | 11/11 生命周期 case，exit 0 | 11/11，exit 0 | existing-lifecycle-debug.log / existing-lifecycle-release.log |

两种配置均运行的是仓库现有可执行文件，本次不构建/修改生产目标；正例诊断探针只编译自己的入口并链接现有 Debug 产品对象。探针取证时 Git tracked diff 为空；关键对象/库时间晚于对应源码。未声称本次做了全量 clean build。

交接前 20:59 的最终共享工作区检查出现 `CMakeLists.txt` 的并行修改和新增 `tools/detection_benchmark/`；均非本诊断工具写入，未改动或回退它们。HEAD 仍为原 ec511662。完整现场状态记在 `CHECKS.json`，不能把整个共享工作区称为 clean。

**必须改正的旧断言：**

1. `tests/task_ux_tests.h:45–48`：调用 markConfigurationStale 后 `QCOMPARE(table->model()->rowCount(), 0)`，同时断言 committedRunId 仍旧。这个断言直接锁定“旧数据留作回滚但不可见”，必须改为保持旧行/ID，并补上 rows>0 的前置条件；现有 rows 可能为 0，单独复跑它不能证明非空集合发生变化。
2. `tests/ux_r5_detection_tests.cpp:288–290`：begin pending 后断言 `!annotationInContext(old)`，报错文字为 `Stale rollback results remain eligible for the current workspace`。R6 应反转该可见性断言，仍禁止 pending 集合混入正式集合。

**应保留并补足，而非删除的断言：**

- `tests/ux_r5_detection_tests.cpp:292–310`：Running/Cancelled/Failed、coverage、generation、sourceValidity、持久化 publication 不一致必须拒绝；保留并补“旧集合可见且 ID 不变”。
- `:311–320`：abandon 后旧 committed、stale、对象数及只发现权威旧 run 的校验本身不冲突，只缺 eligible/overlay/重开可见性检查。stale=true 仍可保留为待更新状态。
- `:247–280`：A→B→A、ROI 整体替换、成功零结果、Manual undo/副本和 GC 是应保留的边界。
- `:323–360` 的 stage/persist/commit/GC 故障注入，`:364–390` 的 running-save-reopen-recovery，以及通道快照/DDC/资产所有权/预算拒绝测试均应保留。本次 11/11 已实际执行。
- `tests/ux_r5_ui_tests.h:95–138` 的来源 Tab 与独立过滤合同不应改成所有来源恒显示；这是另一层过滤，不是缺陷。

## 建议修复边界和可并行实施文件集合

最小可信修复应使“slot 当前正式 run 是否权威”与“下一次配置是否待更新”分离：

1. `domain/project.h`：Algorithm 可见资格继续核对 FileId、channel、deleted、sourceSet 和 committedRunId；stale 不再作为隐藏当前 committed 结果的条件。pending/retired 结果仍无资格，不能简单把所有 Algorithm marks 放行。
2. `ui/annotations/annotation_panel.cpp`：有 committed 时 stale/running 文案说明正在显示上次正式结果，待更新/新任务独立；只有真正成功的合法零结果才显示无结果。未运行且无 committed、running 且无 committed 必须区分。
3. `tests/task_ux_tests.h`、`tests/ux_r5_detection_tests.cpp`：纠正上述两处冲突断言；补非空旧集合在 markStale/begin/abandon/拒绝提交/保存重开仍 eligible，成功非零/零才整体切换。
4. `tests/ux_r5_ui_tests.h`：补真实正式 slot 的 UI 正例，参数草稿/撤销/Enter/模型切换、Tab 往返、save/reopen 的表格 ID 与叠加资格；保留选择与过滤的原语义。

以上 **5 个文件**可形成 BUG02 的独立实施集合，不需要改 CMake，不涉及 BUG01 worker、模型、窗口日志、Sample 精修、batch 或检测算法。最终开发文档可由主代理归档到 `docs/implementation/ux-r6/`。

以下文件是已查清的边界，最小修复预计无需改；若主代理委托扩展语义再单独协调：

- `ui/detection/detection_card.cpp` / `app/main_window.cpp`：已有提交/刷新链本身正确传递配置变化；本次无证据要求重新设计草稿政策。
- `application/detection_result_service.cpp/.h`、`ui/detection/detection_panel.cpp/.h`：保留 begin/abandon 的 stale、pending、generation 和迟到提交拒绝；不要用 `stale=false` 假装修复，否则违反 pending=>stale 的当前验证条件（service.cpp:200）。
- `infrastructure/project_store.cpp` / `project_file.cpp` / `detection_store.cpp`：不需要迁移 schema 或改变原子提交、资产完整性/权威发现/GC，尤其不能扫描重新挂载 retired run。

附带行为已证实：配置提交会自动取消相同 scope 的在途任务，markStale 会作废 pending ticket；它不删除旧 committed。R6 是否改为允许旧请求继续跑属于运行配置政策，应由主代理明确边界，不能在本次可见性修复中顺带取消 generation 保护。即使继续保留当前取消政策，旧正式结果仍必须可见。

后续定向验收至少覆盖：有/无正式集合、成功零结果集合、两个 sampleChannel/文件、来源 Tab 和用户过滤、非空旧集合运行失败/取消、中断保存重开、stage/persist/commit 失败、Manual 副本和旧通道快照不变。真实 worker/GPU/4K 原生交互由 R6 主流程另行验证；本诊断不宣称这些已通过。

## 交接状态

仅完成 BUG-R6-02 诊断，未实施修复，未给出最终 Review 结论。等待主代理新的明确实施委托；最终精确 HEAD 审核须由未参与实现的其他独立子代理完成。
