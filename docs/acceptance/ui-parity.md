# A1.4.3 UI 与交互验收

验收依据是 `docs/prototype/a1.4.3/Signal_Studio_A1.4.3_交互可靠性优化原型.html` 的最终 DOM、最终 CSS 覆盖及启动脚本，配合该目录的交互规范。本文于 2026-10-09 整理，不以早期样式变量、导入演示工程或静态截图代替实际启动状态。

审计另读了原型源目录 `D:/project/signal_studio_原型设计/Signal_Studio_A1.4.3/` 下的 `tests/interaction_regression.cjs`、`tests/回归执行记录.txt` 和 `Signal_Studio_A1.4.3_验证记录.md`。原记录报告 Node VM 回归 59 项通过；该脚本不是 Qt 测试，本轮没有运行它来证明新工程通过。原浏览器记录包含 1366×768、1920×1080 与 DPR=1.5 场景；浏览器真实工程导入未完成验证，只验证了解析回归和导出文件内容。

按用户本轮最新要求，显示适配验收仅针对 **显示器 2、4K 3840×2160、150% DPI 全屏**，对应 Qt 客户区逻辑尺寸 2560×1440。本机第 2 块连接屏为 Redmi 27 NU，Windows 原生路径为 `\\.\DISPLAY6`；连接编号与 GDI 历史编号分别记录。1366×768、其他较小尺寸及旧 DPI 截图属于历史资料，不再安排本轮验证。基础交互用例统一使用上述逻辑尺寸；真实 Windows 截图还须记录目标屏、实际 DPR、全屏状态和物理像素尺寸，offscreen 的逻辑布局通过不能替代实际显示器适配证据。

## 最终结构与默认状态

| 区域 | 最终原型要求 |
|---|---|
| 顶部 | 34 px 菜单栏；Signal Studio 标识、A1.4.3 标记、文件/编辑/视图/分析/工具。没有永久工具栏、IQ 文件标签行或交互说明条。 |
| 左侧 | 工程管理标题 35 px；添加 IQ 与导出入口；工程名称；每个文件及元数据；仅活动文件展开信号区域、全选/删除所选和窄带通道分组；底部说明。 |
| 中央 | 35 px 工作区状态行；5 px 图谱外边距与间隔；全局时间导航、辅助分析、宽带图谱按此顺序排列。 |
| 图头 | 32 px；辅助图为时域波形/功率谱分段按钮，主图为时频图/瀑布图分段按钮；主图头含色阶下拉框；三图各有最大化按钮。 |
| 主图底部 | 32 px，游标、视图范围、当前标记状态及“基于标记创建窄带通道”入口。 |
| 右侧 | 活动信号文件、当前视图、当前标记、PSD、时频设置、鼠标帮助六组。无标记时隐藏标记组；文件组初始展开，其余初始折叠；手动展开分组不因游标刷新反复收起。 |
| 底部 | 当前文件结果/后台任务/日志三个标签；默认收起为 29 px，展开总高度 150 px；没有真实任务时显示演示任务边界。 |
| 状态栏 | 25 px；就绪、时间、频率、文件上下文、交互提示、模拟数据标记。GPU backend 诊断不能替代这些信息。 |

启动工程为“射频信号分析工程”，三个文件均无标记和通道，活动文件为第一个。演示 JSON 中的两个文件、重叠标记与独立参数属于导入样例。

| 文件 | 中心频率 | 采样率 | 时长 | 默认可见时间 |
|---|---:|---:|---:|---:|
| wideband_100MHz.iq | 100 MHz | 40 MS/s | 480 s | 168–264 s |
| capture_2450MHz.iq | 2450 MHz | 20 MS/s | 240 s | 84–132 s |
| telemetry_915MHz.iq | 915 MHz | 10 MS/s | 120 s | 42–66 s |

每个文件默认显示完整采样频带、时域波形及时间向右/频率向上的时频图。显示参数为 Turbo、80 dB、0 dBFS、射频绝对频率、网格开启、色阶条隐藏、PSD 4096 点/当前可见时间窗、STFT 2048 点；波形 Y 为 [-60,60]，PSD Y 为 [-100,0]。

参数选项须与原型相符：PSD [1024,2048,4096,8192]；STFT [2048,4096,8192]；动态范围 [60,80,100] dB；参考电平 [0,-20,-40] dBFS；颜色 [Turbo,Viridis,Gray]。主图头与参数区的颜色选择同步。没有活动标记时，PSD 标记来源须回到可见时间窗。

## 布局与最终 CSS 覆盖

| 逻辑宽度 | 工程栏 | 参数栏 | 额外变化 |
|---|---:|---:|---|
| >1490 px | 270 px | 294 px | 完整图头提示与文件上下文。 |
| 1151–1490 px | 218 px | 248 px | 隐藏交互反馈；≤1430 隐藏主图操作提示；≤1330 隐藏文件上下文和部分图头提示。 |
| 991–1150 px | 194 px | 38 px | 参数内容收为窄栏；≤1190 隐藏主图方向说明。 |
| ≤990 px | 38 px | 38 px | 双窄栏；隐藏范围标签与主图底部游标/分隔符，图头间距缩小。 |

手动收起任一边栏为 38 px；展开时恢复当前断点对应宽度。最终 `.resources` 覆盖最早的 228 px 变量，最终媒体规则覆盖中间的 186 px 变量。原型 `.app` 的最终 `min-height` 为 0；不能沿用早期 620 px 高度限制而拒绝小窗口。

启动脚本 `enforceLayout(true)` 最终设置导航 82 px，辅助区在窗口高度 <850 px 时为 176 px，否则 205 px，覆盖早期 CSS 的 92/235。导航通常限制 62–128 px；辅助下限 116 px；分隔条 5 px。图谱高度预算为 `available = graphsHeight - 24`、`mainBudget = min(366,max(80,available-62-116))`，其余区域按剩余空间夹取。两分隔条双击一起恢复默认，键盘上下每次调整 10 px。本轮 2560×1440 逻辑客户区默认导航 82 px、辅助区 205 px，主图有效绘图区目标约 888 px，自动检查保留至少 875 px。此前浏览器 1366×768 约 248 px、1920×1080 约 528 px 的记录仅作历史审计来源。

画布内部边距为左 88、右 22、上 12、下 34 px；主图显示色阶条时右边距变为 70 px。导航范围左右边距为 13 px。菜单最终宽 224 px，图谱右键菜单最小宽 248 px，并限制高度以允许滚动。原型最大化遮罩 inset 为顶部 86、左右 12、底部 34 px；Qt 使用同一工作区遮罩语义，退出后恢复三图布局和原尺寸。

最终颜色基线：背景 `#0b1422`，中央 `#0d1725`，图面板 `#121f32`，图头 `#16283e`，边线 `#293f56`，普通文字 `#dbe8f8`；选中树标记背景 `#644f23`、文字 `#fff0c7`、左 2 px `#ebc966`；参数标题 hover `#243b55`。文字基线为 Segoe UI/Microsoft YaHei UI 12 px，坐标/参数值使用 Consolas 等宽体 10–11 px。Qt 以逻辑像素实现布局，字体字形和原生文件选择器存在平台差异，须在目标 DPI 另行目视检查。

## 行为验收矩阵

| 编号 | 操作 | 应观察的结果 | 验证入口 |
|---|---|---|---|
| UI-01 | 4K、150% DPI 启动 | 2560×1440 逻辑尺寸下工程栏 270 px、参数栏 294 px，三图有序，主图预算保留，没有永久工具栏或文件 tab。 | `responsiveWorkspace`、`workspaceLayout`；3840×2160 实际截图与 DPR 记录人工复核。 |
| UI-02 | 启动及切换文件 | 三个初始空文件；元数据、可见范围、模式、参数、Y 范围、选择和通道按文件恢复。 | `prototypeDisplayDefaultsAndOptions`、`independentFileDisplaySettings`、Core tests。 |
| UI-03 | 收起/展开工程栏与参数栏 | 38 px 窄栏及断点尺寸恢复，保留中央交互空间。 | `panelRailsAndBottomTabs`。 |
| UI-04 | 点三个底部标签与折叠按钮 | 切换内容并展开到 150 px；再次折叠为 29 px；任务页不伪造实际计算。 | `panelRailsAndBottomTabs`；任务文案人工复核。 |
| UI-05 | 选择标记或切到 PSD | 标记/PSD 相关参数组移到前面，未选中时文件组在前；不生成重复组。 | `sectionContextAndManualExpansion`。 |
| UI-06 | 手动展开 PSD 后游标/刷新 | 用户展开状态保持，同一上下文刷新不重新折叠。 | `sectionContextAndManualExpansion`。 |
| UI-07 | 两分隔条拖动/键盘/双击 | 受上下限与主图预算限制；取消恢复；双击恢复默认导航 82 px、辅助区 205 px。 | `splitterKeyboardResetAndEscapeRollback`，4K 目标窗口操作复核。 |
| UI-08 | 三图最大化、再次点击/按 Esc | 工作区遮罩显示该图；还原图谱和尺寸，取消未结束拖动。持续创建模式保持；Esc 优先取消手势，空闲时才退出最大化。 | `panelMaximizeAndRestore`、`creationModeSurvivesMaximize`、`escapeCancelsGestureBeforeLeavingMaximize`。 |
| UI-09 | 主图模式分段按钮 | 时频 X 时间/Y 频率向上；瀑布 X 频率/Y 时间向下；不改变业务范围。 | `mainModeCoordinateDirection`，实际按钮状态检查。 |
| UI-10 | 波形/PSD 分段按钮 | 波形 X 时间/Y 幅度；PSD X 频率/Y 功率；各 Y 范围恢复独立值，曲线实际可见。 | `auxiliaryYAxisIsolationAndEscape`、`workspaceLayout` 波形像素检查、Core tests；原生场景抓图波形/PSD 像素及目视复核。 |
| UI-11 | 主图图内或底部滚轮/左轴滚轮 | 只缩放物理 X 或 Y；同图同轴 220 ms 合为一条历史，一次后退还原整组。 | `mainAxisWheelIsolationAndHistory`、`displayChangeSurvivesPendingWheelCommit`。 |
| UI-12 | X/Y 轴拖动 | 仅平移对应轴；辅助 X 与主图时间/频带联动；未结束动作可回滚。 | 既有图谱事件用例；完整四轴/失捕获矩阵须另行检查。 |
| UI-13 | 辅助图内拖动区间 | 只放大 X 范围，幅度/功率 Y 不改变；有效宽度至少 8 px。 | Qt 图谱事件测试与人工检查。 |
| UI-14 | 导航外单击、范围内拖动、滚轮 | 跳转或平移窗口，保持跨度；滚轮缩放时间；取消回滚。 | `navigationClickPreservesSpanAndCancelsDrag`。 |
| UI-15 | 空白主图拖动 | 矩形放大；两个方向至少 8 px；历史后退/前进恢复。 | `boxZoomAndHistoryActions`。 |
| UI-16 | 主图右键持续选择开关 | 勾选状态明确；连续创建后保持模式；右键取消当前框并显示菜单。 | `continuousMarkCreation`、`contextMenuActionsAndCoveredMarkSelection`。 |
| UI-17 | Esc/失焦/隐藏/失捕获 | 释放捕获，取消临时框并恢复原视图/标记/面板。Esc 与失焦退出持续模式；失捕获只取消当前动作。 | `escapeCancelsCreation`、`escapeRollsBackMarkMove`、`captureLossAndDeactivationRollback`；隐藏与迟到事件须复核。 |
| UI-18 | 文件/模式/窗口尺寸切换中拖动 | 回滚未结束动作，旧事件不提交到新文件；取消辅助 Y 拖动后改颜色不能再覆盖原 Y。 | `fileSwitchCancelsUnfinishedEdit`、`changingPaletteCancelsAuxiliaryGestureWithoutOverwritingY`。 |
| UI-19 | 图内或树单击标记 | 仅选择，视图与历史不变；悬停不是业务选择。未选对象第一点击不直接拖动编辑。 | `singleClickSelectsWithoutChangingView`、`unselectedMarkClickAndSubthresholdGesture`、树选择测试。 |
| UI-20 | 已选框内部移动 | 只修改命中标记，保持时长/带宽，受文件边界限制；超过 5 px 才开始编辑。 | `selectedMarkMovePreservesSpan`、取消用例。 |
| UI-21 | 四边及八控制点调整 | 6 px 命中容差，时频/瀑布映射正确，边界不翻转/不越界/不缩成零。 | `allResizeHandlesStayBounded` 的 16 行，`selectedMarkCornerAndEdgeResize`。 |
| UI-22 | 标记部分位于可见范围外 | 只显示/命中真实边线和顶点，不把剪裁边当作控制点。 | `clippedMarkBorderMovesWithoutInventingHandle`。 |
| UI-23 | 重叠标记右键列表 | 同一点列出被遮盖对象，选择下层后业务视图不变。 | `contextMenuActionsAndCoveredMarkSelection`。 |
| UI-24 | 树 Ctrl/Shift/Ctrl+Shift/全选 | 按当前文件标记顺序维护范围锚点及多选；刷新不销毁派发事件中的树项；树中设锚点后可在图中继续 Shift 连选。 | `treeSelectionPreservesNodesAndShiftAnchor`、`treeToPlotShiftSelectionPreservesAnchor`；Ctrl+Shift/全选人工复核。 |
| UI-25 | 删除所选/键盘 Delete | 当前文件标记和引用通道批量删除，提示连带数量；其他文件不受影响。 | `contextMenuActionsAndCoveredMarkSelection`、Core tests；各入口人工复核。 |
| UI-26 | 双击树标记/定位 | 定位到约 20% 留白且夹在文件边界；普通选择不定位。 | Core tests；双击与右侧/菜单定位入口人工复核。 |
| UI-27 | 重命名 | 最多 80 字符，按文本显示；取消不变。 | `renameUsesPlainTextAndEightyCharacterLimit`。 |
| UI-28 | 建立窄带通道 | 从活动标记确定中心/带宽，注明演示未执行 DDC；以后改标记不自动重提取。 | Core tests；右侧结果、树分组人工复核。 |
| UI-29 | 调色阶/动态范围/参考电平/hover | 复用模拟功率采样；视图、方向、文件或采样尺寸变化才失效。 | `displayOnlyChangesPreserveBusinessViewAndPowerCache`；不能推导 DSP 性能。 |
| UI-30 | 色阶条、网格、绝对/基带频率 | 双颜色控件同步；色阶条改变绘图区预算；基带仅改变显示坐标。 | `paletteControlsStaySynchronized`、`displayOnlyChangesPreserveBusinessViewAndPowerCache`，截图复核。 |
| UI-31 | 修改 STFT 点数及极小窗口 | 时间下限 `2*hop/Fs`、频率下限 `Fs/FFT`；已有细标记保留，调整不翻转。 | Core tests、`largeSampleCoordinatePrecision`；细标记边界另行检查。 |
| UI-32 | 大绝对坐标的小窗口 | 使用原点与相对偏移，单位随跨度变，刻度不碰撞，不产生负零。 | `largeSampleCoordinatePrecision`、刻度截图复核。 |
| UI-33 | 右键三图视图历史/fit/reset | 后退、前进、适应全部、恢复默认在三图均可用；reset 同时恢复两套辅助 Y；每文件最多 40 条。 | `contextMenuActionsAndCoveredMarkSelection`、`resetRestoresCompleteViewSnapshot`、Core tests。 |
| UI-34 | 新建/移除最后文件 | 允许真正空工程；显示添加入口、禁用分析和移除入口。新建/移除有确认；新建回答 No 保留三个文件与原工程名。 | `emptyProjectAndFileActions` 的 No/Yes 路径；移除取消和入口文案人工复核。 |
| UI-35 | 添加 RAW IQ 演示元数据 | 文件选择和默认 40 MHz 采样率/100 MHz 中心/180 s 时长；校验参数，进度中锁定参数，取消不产生迟到文件，明确未读取 IQ。 | `addMetadataDialogLocksInputsAndCancelsProgress`；系统文件选择器人工复核。 |
| UI-36 | 保存/打开原生工程 | 恢复文件、标记 ID、多选、活动文件、通道引用与全部参数；非法文件不替换原状态；历史不序列化。 | `nativeProjectRoundtripAndInvalidImport` 与 Core tests。 |

## 数据格式适配边界

按此前全新独立工程决定，本仓库保存/打开使用 `signal-studio-native-project` / v1；浏览器原型 `signal-studio-a1.4.3-prototype` 与旧 A1.4 JSON 不作为原生工程导入格式。本轮完整 UI 对齐不改变该契约。A1.4.3 演示 JSON 继续是设计与行为依据；Qt 工程往返验证使用原生 JSON。该边界必须在文件选择器、说明与验收结果中明确，不能把原生往返描述为原型 JSON 互通。

时间业务使用半开 uint64 样本范围，频率使用 Hz，显示层转换为原型的秒和 MHz。保存、读取及拖动不应把完整样本位置先转换为浮点数而丢失 2^53 以上精度。文件名、标记名称和工程名称均按文本展示。

## 执行记录与真实边界

本次 Qt 用例写入 `tests/ui_tests.cpp`；由根 Agent 在实现齐备后集中构建与 CTest。2026-10-09 最终源码含像素抽取、异步模拟渲染、批量线段绘制与相同游标值跳过刷新、新建工程 No 取消及波形可见像素回归，Debug / Release 均完成 All 构建，并在 Qt 6.11.1 下通过状态/存储、显示抽取与 UI 三组 CTest。本表使用 01:35–01:36 完成的最新运行，四份原始 UI/CTest 日志已替换本轮此前记录。

| 配置 | CTest | offscreen UI | QtTest 耗时 | CTest UI 耗时 | 原始记录 |
|---|---|---|---:|---:|---|
| Debug | 3/3 通过 | 64 passed / 0 failed / 0 skipped | 30.548 s | 31.06 s | [CTest](ui-parity-ctest-debug.txt)、[UI](ui-parity-debug-results.txt) |
| Release | 3/3 通过 | 64 passed / 0 failed / 0 skipped | 10.294 s | 10.52 s | [CTest](ui-parity-ctest-release.txt)、[UI](ui-parity-release-results.txt) |

64 项含初始化、清理及行为数据行。CTest 三组总耗时分别为 Debug 31.42 s、Release 10.79 s；QtTest 表列耗时来自测试自身的 `Totals` 行。`responsiveWorkspace(4k-150-percent)` 使用 2560×1440 逻辑尺寸通过；三图最大化还原后的 panel 高度与初值完全相同，为 82 / 205 / 1000 px。

Debug [显示器 2 GPU smoke 报告](screenshots/4k-display2-debug/signal-studio-debug-gpu-screen2-4k-150-report.json) 与最新 Release [报告](screenshots/4k-display2-release/signal-studio-release-gpu-screen2-4k-150-report.json) 均确认 `pass=true`、`fourK150Verified=true`、`fullScreen=true`，目标为 connectedIndex 2 / Redmi 27 NU / 原生 DISPLAY6，屏幕原生 3840×2160、DPR 1.5、窗口逻辑 2560×1440；[Release 实际抓图](screenshots/4k-display2-release/signal-studio-release-gpu-screen2-4k-150.png) 为 3840×2160，已读回确认波形曲线可见。QRhi 使用 D3D11 / AMD Radeon(TM) 880M Graphics，主图有效高度 888 px；最新 Release 主图提交 7 帧、热图上传 1 次，覆盖层变化后仍为 1 次，`overlayReusesHeatmap=true`。辅助图源点 22488、抽取点 5622，实际通过 `QPainter::drawLines()` 批量绘制 5621 个相邻线段；`curvePathElements=5622` 仅表示缓存路径元素数。计数描述这次启动观测，不是帧率或真实计算性能；分段计时及操作前后对照见 [显示性能记录](display-performance.md)。

Release 在显示器 2 全屏完成 **12/12 原生窗口场景**，完整 [场景与诊断报告](screenshots/4k-display2-release/scenes/native-capture-report.json) 的总结果、连续滚轮、带事件循环滚轮、辅助图悬停及主图悬停诊断均为 `pass=true`。场景包括默认布局、持续创建两个重叠标记、树 Ctrl 多选、悬停、PSD、瀑布、色阶条/基带频率、主图最大化/恢复、侧栏收起、空工程和 RAW 元数据对话框。前 11 张窗口抓图均为 3840×2160、DPR 1.5；第 12 张仅抓取对话框，尺寸为 705×543、DPR 1.5，报告另核验其所在屏幕和居中范围。默认波形与 PSD 的实际颜色像素分别为 59968 / 36546，均超过最低 100 的回归门槛；[色阶条/基带频率](screenshots/4k-display2-release/scenes/07_waterfall_colorbar_baseband.png)、[最大化](screenshots/4k-display2-release/scenes/08_spec_maximized.png) 和 [添加对话框](screenshots/4k-display2-release/scenes/12_add_signal_dialog.png) 等 PNG 已由根 Agent 读回复核。

该场景程序的悬停使用 `QTest::mouseMove(QWindow*)` 向原生窗口注入 **Qt/QPA 输入事件**，并记录控件实际收到的 MouseMove、主图 cursorChanged 信号及刷新计数。连续滚轮通过实际控件的 `QWheelEvent` 与后退菜单验证。本次 `rapidWheelDiagnostic` 已通过，一次后退恢复初始完整视图；这些记录证明 Qt 输入与显示路径，不是物理 OS 鼠标递送或完整系统捕获生命周期的证明。

最终生产版本的 Windows Debug 在真实显示器 2 全屏复跑三个重点场景：最大化/恢复、分隔条键盘/双击/Esc、持续创建跨最大化，结果 **5 passed / 0 failed / 0 skipped**（含初始化和清理），QtTest 耗时 7.829 s，原始日志见 [Windows UI 记录](ui-parity-windows-debug-results.txt)。导航/辅助/主图在放大及恢复后的 D3D11 帧计数分别为 5→7→10、10→11→12、12→13→14，三图恢复原高度 82 / 205 / 1000 px，硬件 `gpuReady` 与帧增长断言通过。

最终独立包也已从各自输出目录验证：[Debug 包](debug-4k-display2-package.json) 与 [Release 包](release-4k-display2-package.json) 均为 `pass=true`、硬件渲染、显示器 2、DPR 1.5、3840×2160 抓取尺寸、7 次提交、1 次热图上传，覆盖层变化后上传仍为 1 次。运行清除 Qt 插件与缩放覆盖变量，并使用 system-only PATH，证明包可在本机脱离开发 Qt PATH 启动。上述 Debug/Release GPU 截图报告也在最终生产版本重新通过。

Release 的 [软件包回退报告](release-4k-display2-software-package.json)、[软件截图报告](screenshots/4k-display2-release/signal-studio-release-software-screen2-4k-150-report.json) 及 [实际截图](screenshots/4k-display2-release/signal-studio-release-software-screen2-4k-150.png) 单独保存，均通过同一显示器 2 / 4K / 150% DPI 条件；`hardwareRenderer=false`、帧提交 0、热图上传 0，明确证明 QWidget 软件回退。软件验证使用独立 `-software` 文件名，保留 GPU 原始证据；软件结果不计为 GPU 通过。

本轮行为测试、12 个原生窗口场景、Windows GPU 生命周期、Debug/Release 独立包及 Release 软件回退均已保留实际完成记录。offscreen 插件报告不支持系统鼠标捕获，原生场景采用 Qt/QPA 注入；这些通过结果仍不等于物理 OS 鼠标递送、全部系统捕获生命周期或所有系统文件选择器路径已人工验证。矩阵中标注的其余人工入口也按实际覆盖范围解释；旧骨架测试和历史截图不自动覆盖本轮修改。

`panelMaximizeAndRestore` 在 offscreen 下仍执行布局及分隔条对象恢复检查；在 Windows 平台额外等待三张图实际 `gpuReady`，验证放大及恢复后 QRhi 帧提交计数增长，并输出实际 backend 与计数。这两个分支分别证明布局行为与本机 GPU 资源生命周期，不能互相代替。

验收分别保留行为测试、Windows Debug/Release 构建与独立包 GPU smoke、4K 3840×2160 / 150% DPI 截图及 DPR、最大化/分隔条/Qt 输入/元数据对话框记录。本轮不再验证其他分辨率与 DPI 组合。截图只证明所展示状态，不证明全部输入路径、取消语义、持久化或 GPU 工作。GPU 成功需非 CPU QRhi 驱动与纹理上传/帧提交证据；offscreen 基础 UI 测试不能充当 GPU 证据。

本轮仍使用模拟数据，不交付真实 IQ 读取、FFT/PSD/STFT、DDC、检测或大文件性能结论。Ubuntu 的构建、GPU backend、布局和系统对话框仍需平台实测。
