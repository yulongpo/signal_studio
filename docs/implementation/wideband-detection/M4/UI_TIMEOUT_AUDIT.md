# M4 Debug UI 300 秒超时审计

审计时间：2026-10-14 03:29（北京时间）。基线为 `6ed5ce0338cc2020b8bf6a529874e1cc07166177`（M3），当前工作区为 M4 未提交实现。此审计只读取源码、Git 差异、现有日志和一次进程快照；未启动构建、测试、GPU 或模型计算，未修改业务实现与 CTest 超时。

## 结论与验收影响

M4 全量 Debug UI 回归仍未通过，不能以已完成场景的 PASS 或三项 focused 通过代替全套验收。已发生的两次 300 秒超时表现为跨场景累计变慢，并非某一个新时间精修场景挂起。独立运行仍比 M3 相同前 88 场景多 54.107 秒；新增样式阶段缓存之后，整体耗时没有回到 M3 水平。

现有证据尚不能区分 M4 公共 UI 开销变化与机器运行条件变化。可以排除的较窄解释是“全量 CTest 的其它测试并发挤占导致超时”以及“只有新时间精修/推理任务在拖慢”：独立运行仍超时，且 300 秒的大部分已在进入宽带任务夹具前消耗。不能由这些事实进一步断言 CPU、绘制、布局、QSettings 或特定新控件就是根因。

## 已有测量

| 记录 | 已完成场景 timing rows | 场景耗时之和 | 结束情况 |
| --- | ---: | ---: | --- |
| M3 `debug-ui-timing.jsonl` | 92 | 249.069 s | Qt totals 94/94，249.184 s；CTest 249.89 s，通过 |
| M4 全套后 UI，`ui-timeout-comparison.jsonl` | 88 | 297.364 s | CTest 300.04 s，宽带夹具开始后超时 |
| M4 加阶段样式缓存后的独立 UI，`ui-isolated-timeout.jsonl` | 89 | 299.274 s | CTest 300.05 s，宽带夹具刚记录 PASS 后超时 |

Qt 的 94 包含 initTestCase/cleanupTestCase，92 个 timing rows 包含各数据行。逐项 timer 在每项 init 开始、cleanup 中落盘，包含测试体里的窗口构造、事件处理、断言及局部窗口销毁；它不是单纯算法计时。场景合计和 Qt/CTest 总时长不能互相替代。

独立 UI 由 CTest 于 03:19:10.290 启动，最后已落盘条目 `widebandFixtureTaskNativeRoundtrip` 完成时间为 03:24:10.366，其自身为 6.275 秒。文本日志有该项 PASS，但没有整个 Qt 测试的 totals。尚未完成的 M3 同名场景为 `widebandConfigurationCommittedDrafts`、`widebandRoiPreservesManualTool`、`widebandAnnotationSelectionAndOverlap`，M3 三项合计 5.742 秒。

按两次 M4 都完成的相同前 88 行比较：

| 前缀 | M3 | M4 首次 | M4 独立 |
| --- | ---: | ---: | ---: |
| 30 行 | 81.432 s | 99.194 s | 94.899 s |
| 60 行 | 158.830 s | 194.750 s | 187.917 s |
| 88 行 | 238.892 s | 297.364 s | 292.999 s |

相同前 88 行，首次 M4 比 M3 多 58.472 秒（24.48%），独立 M4 多 54.107 秒（22.65%）；独立比首次仅少 4.365 秒（1.47%）。首次有 82/88 行较 M3 慢，独立有 85/88 行较 M3 慢。M3 的 `initialFileViewShowsFirstFivePercentOrTenMilliseconds` 为 0 ms，比较比例时排除该行；其余 87 行的独立/M3 比例中位数为 1.2167。

部分场景耗时如下，单位 ms：

| 场景 | M3 | M4 首次 | M4 独立 |
| --- | ---: | ---: | ---: |
| workspaceLayout | 1872 | 1854 | 1880 |
| continuousMarkCreation | 2295 | 3205 | 3401 |
| panelMaximizeAndRestore | 4945 | 7141 | 6147 |
| creationModeSurvivesMaximize | 2945 | 4154 | 5142 |
| narrowbandAuxiliaryRenderingAndGestures | 7542 | 9049 | 8026 |
| importRealFilesAndCancelPrefix | 3945 | 5310 | 6207 |
| narrowbandDemoResourceAndFourPages | 10312 | 13561 | 12852 |
| waveformBandwidthAndVisiblePaneStftSettings | 4156 | 5348 | 5716 |
| uiStatePersistenceAndRecentProjects | 3678 | 5332 | 4762 |
| widebandFixtureTaskNativeRoundtrip | 4435 | 未完成 | 6275 |

两次 M4 的单项也有双向约 1 秒的变化：`importFormatAndAdaptiveDraft` 独立多 1.264 秒，`narrowbandAuxiliaryRenderingAndGestures` 独立少 1.023 秒。这证明存在运行间波动，但不证明 54 秒整体差异全部来自运行条件。

M3 的 18 个非 UI CTest 条目耗时合计 111.58 秒，M4 的 22 个非 UI 合计 113.34 秒且全部通过，新增四项合计 0.44 秒。此对照未呈现与 UI 相当的普遍放慢，但无法排除只影响 UI 事件、绘制或窗口路径的运行条件差异。M3 与 M4 来自不同时间段，尚无当前机器条件下交替运行的 M3 对照。

## 源码定位与触发条件

`app/main_window.cpp`、`tests/ui_tests.cpp`、`cmake/run_ui_test.cmake` 相对 M3 无差异；`git diff --stat 6ed5ce0 -- ui app tests/ui_tests.cpp cmake/run_ui_test.cmake` 的 UI 变化只在 annotation evidence、detection card 与 detection panel 共五个文件。现有 Plot、窄带工作区与测试等待机制没有 M4 源码差异。CTest 保持 `QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5`、`TIMEOUT 300`，日志明确为 QWidget/offscreen 软件 UI 路径，不是 GPU 验收。

- **跨场景公共入口**：`MainWindow` 构造时建立所有卡片与继承样式，随后 refresh；`DemoMainWindow` 再添加三个 demo 文件并 refresh。`showWindow` 执行 show、processEvents 和 qWait(20)。多数场景重复这条路径。窗口销毁保存 UI 与右栏设置、销毁图谱控件，也被逐场景 timer 包含。对应 `app/main_window.cpp:326,435,2509`、`tests/ui_tests.cpp:81,262,277`。
- **统一刷新**：`MainWindow::refresh` 调用 `updateBottom`，后者无论底部是否展开，都调用 `AnnotationPanel::refresh` 与 `DetectionCard::refresh`，再调用三个 plot 的 syncState。已有工作包括列表/检查器同步、检测卡来源/活动段/时长/分支/状态刷新、全局布局与绘制事件。这是应首先分段测量的公共路径，当前没有函数级耗时，不能断言它增加了多少。对应 `app/main_window.cpp:2798,3187`。
- **阶段条**：新增一个 QWidget、五个 3 px QLabel 和布局，每个窗口都构造；无运行时隐藏。`refreshStatus` 中的五次 setStyleSheet 仅在 latestRun 存在且 `completedStageIndex` 变化时执行，当前已缓存。无检测运行的普通 demo 场景不会执行该样式分支。因而“每次全局 refresh 都执行五次样式更新”已经不符合当前源码，更不能解释当前无任务场景的整体差异。对应 `ui/detection/detection_card.cpp:522,851,884`。
- **实测曲线**：Inspector 与 Auxiliary 各新增一个固定 70 px、初始隐藏的 QWidget。`setAnnotation` 已有 `displayKey` 与共享 origin 的 early return，`clearEvidence` 已有 `empty_` guard；二者与 M3 相同。新增 JSON 解析和最多 512 点曲线更新在 showScores 中，只有证据内容/选择状态变化才走到这里，无标记时不反复解析。手动标记无 origin 时得到空对象，曲线保持隐藏。这条路径值得在导入标记/选择编辑中测量，但不是每次后台 refresh 无条件进行大段 JSON 解析。对应 `ui/annotations/annotation_evidence_widget.cpp:307,618,635,796`。
- **检测轮询**：已有 100 ms `DetectionPanel::poll` 调用 tasks pump/snapshots/refreshRunList。M4 的 Signature 新增 phase 和两个候选计数，因此运行中进度变化可以刷新运行卡并发出 runProgressChanged，后者调用 DetectionCard::refresh。此反馈只在存在任务时有实质增量；空任务列表不会因新字段而创建运行卡。真实时间分析位于 `DetectionTaskManager::execute` 的后台任务中，普通 demo 创建不启动该任务。对应 `ui/detection/detection_panel.cpp:119,516,554,615`、`app/main_window.cpp:1116`、`infrastructure/detection_task.cpp:744,872,1137`。

上述事实降低了阶段样式循环、每次刷新解析证据、空窗口启动真实精修三个解释的可信度。新增隐藏控件的构造、继承样式、首次布局及对象销毁仍是候选，但仅凭新增八个 QWidget 或固定高度无法量化其影响。

## 最小证据实验建议（本审计未执行）

1. **先建立同机 M3/M4 对照**：保留当前脏工作区，在独立产物目录准备 M3 的同工具链 Debug UI 可执行及配套部署文件；使用与当前相同 Qt、offscreen、1.5 缩放、启动目录和测试初始设置。按 M3→M4→M4→M3 顺序串行执行相同四项：`workspaceLayout`、`continuousMarkCreation`、`panelMaximizeAndRestore`、`narrowbandDemoResourceAndFourPages`，至少两个交替循环。四项覆盖空任务窗口、标记交互、布局和窄带，且不启动真实检测。分别记录 per-case wall time、进程 CPU 增量与完整命令。恢复相同已备份 QSettings 初始状态，避免用户设置或前一轮窗口保存导致布局变化；不得删除用户设置。当前仅比较历史 M3 与当前 M4，不能替代这一对照。
2. **按对照结果分段测量**：只有对照显示稳定的版本差异才为诊断构建增加少量聚合计时，记录 MainWindow 构造/析构、Demo 初始化、showWindow、refresh 总次数和累积耗时、updateBottom 内的 AnnotationPanel/DetectionCard、三个 plot syncState；事件侧累计 Paint/LayoutRequest/StyleChange/UpdateRequest 的数量和耗时。QTRY/qWait 等等待单独计时，记录期望 settle 与实际 settle 时刻。用每场景末尾一条聚合日志，避免逐事件写盘制造新开销。主线程 CPU 同比变高指向实际执行开销；wall time 变高而 CPU/事件工作量不变则优先检查调度、等待、I/O。两者都需要实测，不由时间比例推定。
3. **仅当热点落在新增控件附近才消融**：使用独立诊断产物，每次只禁用一个新增因素（阶段条创建、测量曲线创建或 showScores 新解析），与原 M4 按相同小组交替运行。先测控件创建的效果，再测其刷新，不能一次删除整个卡片或改变布局/图谱参数。必须观察可重复的耗时与事件计数变化，再提出业务修复。无任务小组中的阶段样式执行计数应为 0；若不为 0，先找实际 run 来源与触发条件。
4. **保持验收边界**：找到并验证原因后再跑原 300 秒、全部 92 个场景的 CTest。focused 三项的 5 passed/9.378 秒（含 init/cleanup）说明这些项独立可完成，不能证明全套性能或顺序运行通过。延长 TIMEOUT 只会改变验收条件，不能作为本次原因证据或修复结果。

若同机 M3 也在相同小组上达到 M4 水平，则应优先固定运行条件并重新建立当前性能基线；若 M4 仍稳定较慢且超出自身交替波动，继续按测量定位代码。没有当前 M3 对照、CPU/时钟/系统负载轨迹和函数级时间之前，不应给出“机器资源波动已确认”或“阶段标签已确认根因”的结论。

## 证据文件

- M3：`docs/implementation/wideband-detection/M3/debug-ui-timing.jsonl`、`debug-ui.txt`、`debug-ctest.txt`、`artifact-manifest.json`。
- M4 首次：`out/tasks/wideband-detection/M4/logs/ui-timeout-comparison.jsonl`、`ui-timeout-comparison.txt`、`debug-ctest-final.log`。
- M4 独立：`out/tasks/wideband-detection/M4/logs/ui-isolated-timeout.jsonl`、`ui-isolated-ctest.log`；原始 UI 文本当时位于 `out/vs2026-qt611-debug/ui-test-results-debug.txt`，该公共输出路径之后的运行可能覆盖，复核应优先使用节点归档 timing。
- focused：`out/tasks/wideband-detection/M4/logs/ui-progress-focused.txt`。

03:23:26 的只读进程快照可见当前 CTest 与其 PowerShell 父进程，Python 条目为已有 `http.server 8784 --bind 127.0.0.1`。此快照并非全程系统资源测量，不能据此证明整段运行不存在调度、时钟、温度或后台 I/O 差异。
