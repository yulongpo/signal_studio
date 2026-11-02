# rebuild/grok-ux-opt 完成情况复核

复核日期：2026-10-21。基线：`4904740`（`rebuild/a143-native`）；评审提交：`9d7475d`。依据为会话 `01a154da-9f95-77a1-8af8-f53b82a5ed71` 中获批的实施计划、实际 Git 差异、原始测试文件及本次补跑结果。

## 结论

**主体代码已经实现，但用户流程、验收和交付没有闭环；当前不满足原任务的推送门槛，也不宜合入主线。**

分支有 8 个本地提交，涉及 17 个文件，增加 1439 行、删除 249 行。派生/取消、图谱控件迁移、谱方法记忆、四页属性区及任务动作都有实际 C++/Qt 实现。相对基线，没有修改 domain/application、工程存储、DSP 或 worker 源码。本次非 UI 回归通过，支持保留这些改造成果继续收口。

原交付报告中的“没有留下断言失败记录”与原始日志不符。本次复跑确认了同样的两处失败。问题既包括测试入口未随新布局迁移，也包括检测入口没有切换新页签的产品遗漏，不能只归因于 480 秒超时。

## W0–W6 判定

| 工作包 | 判定 | 依据与缺口 |
|---|---|---|
| W0 基线与验收计划 | 未按要求闭环 | 七份规格/基线/矩阵文档仍未跟踪，历史中没有 W0 文档提交；验收矩阵仍全部为“未运行”。基线审计还把 W1 修改后的调用链归到原始 `4904740`。 |
| W1 派生与取消 | 核心修复完成，验证部分完成 | 已按值复制历史运行并保留父运行及冻结范围；查看来源与派生分开；卡片取消限定当前文件的聚焦任务。本次定向测试通过，但完整入口、终态和持久化旅程未覆盖。 |
| W2 图谱控件与弹层 | 主体实现完成，验收不足 | 单一 `colormap`、图谱标题栏、溢出容器、PSD/STFT 弹层均有代码；缺独立布局与弹层焦点行为矩阵。滚轮回归仍有失败。 |
| W3 谱方法参数保持 | 基本完成 | 有实例内的方法缓存，Bartlett 返回零重叠；本次方法切换测试通过，没有修改工程版本。DPSS/Burg 参数编辑后切换、非法草稿等完整组合仍缺专门证据。 |
| W4 右侧上下文与检测卡 | 部分完成 | 四页、模型/谱型拆分、文件属性和固定操作区已落地；统一检测入口未切页；预检、草稿说明和物理计算长文仍常驻，未完成首屏精简和诊断分层。 |
| W5 任务反馈与键盘 | 主体实现完成，验证不足 | 任务卡增加阶段、进度和结果数，增加 Return/Delete QAction 与上下文菜单。快捷键测试存在直接触发动作的兜底，未充分证明按键路径及实际取消的任务 ID。 |
| W6 完整验收与发布 | 未完成 | 没有独立 UX CTest 目标；历史 Debug 仅运行筛选的 5 个 CTest，结果 4 通过、UI 超时；Release 未完成本分支构建与全量回归。缺完整原生旅程、专项回退和异常路径证据。 |

## 需要优先收口的问题

### 1. 检测入口未切换到新检测页（产品问题）

`app/main_window.cpp:4570` 的 `openWidebandDetection()` 仍只展开旧 `detectionSectionToggle` 并调用 `ensureWidgetVisible()`，没有调用 `setContextTab(1, ...)`。检测卡已被 `organizeContextPages()` 放入 QStackedWidget 的第二页，滚动不能显示非当前页。

因此从显示/标记页点击菜单检测、顶部“检测当前区域”、图谱检测入口、历史派生，或确认 ROI 时，配置可以被更新，但检测页及其运行按钮仍可能不可见。ROI 确认也调用同一入口，未满足 UX-A01 的“确认后聚焦检测页”。这是代码调用链确认的遗漏；本次未新增产品探针或修改源码。

### 2. 两个现有 UI 用例确实失败（已复现）

| 用例 | 本次失败位置 | 含义 |
|---|---|---|
| `parameterInputsIgnoreWheel` | `tests/ui_tests.cpp:1449`：侧栏滚动值没有增加 | 测试仍向已搬入文件属性对话框的有效带宽控件发送滚轮，并期待旧侧栏滚动。需要按新的实际容器验证；这个失败本身不能证明参数被滚轮改值。 |
| `widebandAnnotationSelectionAndOverlap` | `tests/ui_tests.cpp:4526`：`cardSegment->isVisible()` 为假 | 测试直接操作处于非当前检测页的控件，没有通过新的可见页签路径操作。需迁移完整界面路径，并保留原来的多段选择断言。 |

历史 `out/vs2026-qt611-debug/ui-test-results-debug.txt:1156` 与 `:2101` 已记录上述失败，最后写入时间为 2026-10-20 01:38。原交付报告 `DELIVERY.md:34` 的“没有留下断言失败记录”必须纠正。CTest 外层包装在进程退出后才读取 Qt 日志，超时输出不等于底层日志没有断言失败。

### 3. 检测卡首屏精简仍有明确未实现项

`ui/detection/detection_card.cpp:982` 在没有草稿时仍写入常驻操作说明；`:996` 成功预检仍包含上下文、输出范围、频带和指纹的多行解释；`:882` 物理时间仍保留整张计算表。相关控件在 `setupTiming`/`setupStatus` 中直接加入主布局，没有实现要求中的短摘要、折叠计算详情及预检详情入口。

专业参数折叠、模型/谱型拆分已经完成，但不足以认定 W4 全部完成。显示页也仍沿用文件概要和当前视图分组，没有完成规格中的紧凑摘要与对应工具栏跳转。

### 4. 验收证据与版本管理未收口

- `CMakeLists.txt` 没有分支变更，仍只有原来的 `signal_studio_ui` 入口，没有获批计划要求的独立 UX 测试目标。
- 新增的两个测试函数集中覆盖派生/取消和谱方法记忆，不能代替全部 UX-D/S/A/R 行为覆盖。阶段日志中的 `3 passed` 通常包含初始化和清理，只对应一个业务测试。
- `tests/ui_tests.cpp:5010` 附近在发送 Delete 后又调用 `cancelAction->trigger()`，后续只确认历史任务没变及卡片取消信号，未直接断言 Delete 已取消预期运行。
- `BASELINE_AUDIT.md`、`CONTROL_INVENTORY.md`、`UX_SPEC.md`、`OBJECTNAME_MAPPING.md`、`ACCEPTANCE_MATRIX.md`、`PLAN.md`、`DECISIONS.md` 均未跟踪；HEAD 中该目录只有 `DELIVERY.md`。完整任务依据尚不能随 clone 复现。
- `git show 4904740:ui/detection/detection_panel.cpp` 确认原始派生确实调用 `openRunSource()`，原始 `openRunSource()` 调用无参 `configure()`；原始卡片取消使用 `unfinishedRun()`。现有基线审计声称这些已被修复，是读取了修改后的工作区造成的错误归属。

## 本次实测与既有证据

本次没有重新编译，使用 2026-10-20 01:27 构建的当前 Debug 程序；分支自该次构建后只有文档提交。没有执行产品源码修改、提交、推送或新原生截图。

| 项目 | 结果 | 证据与边界 |
|---|---|---|
| Debug 非 UI CTest | **22/22 通过，124.69 秒** | 检测管线/协议/存储、标记、通道、导入、谱分析等；不能替代 GUI、正式模型及端到端兼容性验收。 |
| Debug 定向 UI | **7 个业务用例中 5 通过、2 失败，13.87 秒** | Qt 总计 `7 passed, 2 failed`，其中包含初始化和清理；offscreen、`QT_SCALE_FACTOR=1.5`，不是硬件显示验收。 |
| 历史 UI CTest | **超时且日志有断言失败** | 会话中的筛选集合 4/5，通过数不能写成 Debug 23/23 或 UI 全量通过。 |
| 既有原生截图 | 显示证据有效，旅程证据不足 | U27U2D / DISPLAY2，3840×2160、DPR 1.5、全屏；QRhi D3D11 / AMD Radeon 880M，含真实纹理上传。输入为演示数据 `realIq=false`，截图展示显示页，不能证明完整检测交互或真实模型推理。 |
| Release / 专项回退 / 正式模型 | 本次未补验 | 现有 Release UI 程序早于本分支改造，不能当作当前分支交付程序。缺正式模型验证按获批计划记为未运行，不单独作为推送阻断。 |
| 远端分支 | **不存在** | 本次 `git ls-remote --heads origin refs/heads/rebuild/grok-ux-opt refs/heads/rebuild/a143-native` 只返回基线分支 `4904740`。验收未满足时没有推送，符合原授权边界。 |

本次日志：

- `out/tasks/wideband-ux-optimization/W6/diagnostics/review-20261021/ui-focused.txt`
- `out/tasks/wideband-ux-optimization/W6/diagnostics/review-20261021/ui-focused.xml`
- `out/tasks/wideband-ux-optimization/W6/diagnostics/review-20261021/non-ui-ctest.txt`

补跑命令使用 Debug 部署目录置于 PATH 首位；UI 环境为 `QT_QPA_PLATFORM=offscreen`、`QT_SCALE_FACTOR=1.5`：

```powershell
ctest --test-dir out/vs2026-qt611-debug -C Debug --output-on-failure -E 'signal_studio_ui$'
./out/vs2026-qt611-debug_bin/SignalStudioUiTests.exe parameterInputsIgnoreWheel widebandAnnotationSelectionAndOverlap widebandDeriveAndCancelTargets spectralMethodMemorySurvivesSwitches sectionContextAndManualExpansion widebandRoiPreservesManualTool widebandConfigurationCommittedDrafts -o <log>,txt
```

## 建议收口顺序

1. 修复统一检测入口的页签切换，补“显示/标记页 → 检测入口/派生/ROI 确认 → 可见检测页与按钮”的行为断言。
2. 将两个失败回归迁移到新的可见容器和页签路径；保留滚轮不改值、容器可滚动、多段选择等真实验收意图，不能删除断言放行。
3. 完成检测卡摘要、计算详情和预检分层，补齐独立 UX 目标与取消 runId 的直接断言。
4. 保持 480 秒保护，定位全量 UI 的慢项或挂起点；完成当前源码 Debug/Release 构建、全量 CTest，以及规定屏幕上的代表性交互旅程。
5. 更正基线和交付报告，逐条更新验收矩阵并归档必要证据，纳入版本管理；满足原推送门槛后再普通推送目标分支。

当前适合继续在该分支收口。已有实现有保留价值，验收失败和证据缺口也有明确定位。
