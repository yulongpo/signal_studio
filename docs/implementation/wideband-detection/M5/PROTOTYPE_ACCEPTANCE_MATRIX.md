# M5 A16 原型与 Qt 验收映射

结论：**A16 待补验证，不能依据既有 94 项 QtTest 与 34 场景原生记录宣布综合通过。** 已有记录证明了多数结构与正向旅程；下表列出的组合动作、异常状态和原生边界仍须 M5 实际执行。未覆盖不等于产品缺失或产品 bug，也不要求新增算法或批量数据矩阵。

本次为独立只读映射审计，仅写本文件，未改产品、原型、freeze 或既有产物，未构建、测试或使用 GPU。输入基线为已提交 M4 `15bec9fc24eb24f186c4aaface0f8ace35f2ec28`、规划 05/07、获批 M0-P，以及 M1–M4 的源码、测试与报告。M5 正在准备的压力、缺资产/设备、独立启动、cold/hot 和手动工具工作不在本轮已完成证据中。

## 证据口径

- M0-P 为 `wideband-detection-r1.0`；本轮按声明的 raw/utf8-lf 策略重核批准的 22 项指纹，全部匹配。依据 `M0-P/artifact-manifest.json`、`M4/prototype-current-check.json`。浏览器40组检查只证明原型。
- Qt 来源为 `tests/ui_tests.cpp` 与 M4 已结束的 `canonical-final-debug-ui.txt`、`canonical-final-release-ui.txt`：各94 passed、0 failed/skip。94包含初始化、清理和数据行，不是94个原型需求；下表只引用读到的具体断言。
- 原生 N 为 `M4/native-final-r5/wideband-detection-report.json` 及同目录 PNG/identity：34个唯一场景、实际第二连接屏 Redmi27NU / DISPLAY6、4K/150%全屏。`a16AcceptanceVerified=false` 与 `prototypeComparisonVerified=false` 原样保留。已有局部目视对照见 `M4/NATIVE_FINAL_R5.md`、`M4/FINAL_SPEC_REVIEW.md`，不能替代未执行的动作。
- 下表 `已覆盖` 表示本项所列的行为有对应执行证据；`部分` 表示已有断言只覆盖该项一部分；`待原生` 表示有 Qt/底层检查但缺规定屏幕旅程。支持层测试明确标为非 Widgets 行为测试。后续新包必须绑定新源码/包身份，不能将本表的旧执行记录扩为新版本通过。

Qt 方法与行号均指 M4 基线的 `tests/ui_tests.cpp`。N编号为文件名数字前缀；N02包含主窗口与实际可见卡片裁剪。完整文件名可在上述原生目录查得。

## P01–P17 产品映射

| 项 | 具体 Qt 行为测试及已核断言 | 已有原生依据 | 状态及 M5 补项 |
|---|---|---|---|
| P01 三入口 | `widebandConfigurationCommittedDrafts:4246` 入口保持非模态、ROI请求不被覆盖；原生 helper `widebandCheckConfigurationEntrances:338` 实际依次触发菜单/宽带按钮/上下文菜单，核同一非模态卡及0任务 | N01/02，report.configurationEntrances 三项pass | 部分：补每入口预设范围的精确样本/Hz比较及快捷键冲突核对；三入口同卡已有证据 |
| P02 常驻检测卡/profile | `widebandConfigurationCommittedDrafts:4312–4339` 切SCN与fixture、读取快照/默认策略并恢复配置；`sectionContextAndManualExpansion:2225` 卡片上下文/折叠 | N01/02/31；FFSCN真实旅程N23–33 | 部分：常驻落位已覆盖；SCN/FFSCN-17 缺资产或不支持状态的实际可见说明随S4补 |
| P03 四范围/选段 | `widebandConfigurationCommittedDrafts:4290–4301` ROI/available-file/未定位活动预检；`widebandAnnotationSelectionAndOverlap:4106–4112` 多段下卡片与详情段同步 | N02 available-file请求，N07–10 ROI；N20/21选择确认一段 | 部分：补四种范围和多段选择的完整原生矩阵、visible范围与提交后快照 |
| P04 ROI确认 | `widebandRoiPreservesManualTool:4362` 实际drag、Esc/确认按钮、物理范围、0标记/0任务/视图不变 | N07–10实际Esc/Enter、工具恢复及对象/任务不变 | 部分：普通确认/取消已覆盖；拖到时间/有效频带边缘待补I04 |
| P05 三图/物理投影 | `mainModeCoordinateDirection:486` 两模式轴方向；`widebandAuxiliaryRenderingAndGestures:1734` 波形/PSD/缩放；`widebandLinkedCursorsAndFrameSpectrum:1951` 共享物理游标 | N01/05/17–19/24/32有实际三图及三来源 | 部分：三图/证据布局已有；同一活动段跨时频/瀑布的完整原生选择保持待I06 |
| P06 持续手动工具 | `continuousMarkCreation:507` 连续两次drag产生两对象且工具保留；`escapeCancelsCreation:524` 取消无对象；`widebandRoiPreservesManualTool` 恢复先前工具 | N07/08恢复manual，第二Esc退出 | 部分：ROI恢复已覆盖；规定屏幕持续创建两框/右键工具切换尚未单独执行 |
| P07 手柄/只读来源 | `selectedMarkMovePreservesSpan:544`、`selectedMarkCornerAndEdgeResize:907`、`allResizeHandlesStayBounded:2530`；`torchSigLabelsNativeRoundtrip:4006–4009` 原始编辑禁用、修订后启用 | N05导入来源数值修订；N29算法数值修订保留原始证据 | 部分：只读框试拖→修订后拖、一次命令及原生八手柄边界待I08/I09 |
| P08 真实任务状态 | `widebandFixtureTaskNativeRoundtrip:4150` 真文件/任务Completed/保存；支持 `detection_task_tests` 的失败、取消、终态、代际检查 | N03/04任务计数/快照；N23真实生产任务阶段 | 部分：排队/取消中/失败/部分覆盖/零结果界面及运行中草稿待S5–S7/I16 |
| P09 统一结果/筛选 | `panelRailsAndBottomTabs:2188` 实际页签；`annotationTableBoundsAndIndependentSelection:4023` 200行分页且隐藏选择保持；`widebandAnnotationSelectionAndOverlap:4114` review筛选 | N05/06统一三来源记录；N03/04后台任务 | 部分：source/run/mean过滤、数值排序后批复核与统计不变待I11/I12 |
| P10 工程树/运行摘要 | `treeSelectionPreservesNodesAndShiftAnchor:705`、`annotationTableBoundsAndIndependentSelection` 1200对象树≤300及分页；`widebandFixtureTaskNativeRoundtrip:4238` 切文件仍列原run | N01/03/05/23的来源集合与运行摘要 | 已有结构/有界1200对象证据；100k压力与资源数字为M5另项，当前不计通过 |
| P11 同对象复核 | `unifiedAnnotationDraftsAndSegmentSwitch:3962–3973` 单段确认、接受、undo/redo；支持 `annotation_tests::testSourcesAndImmutableOrigin` 幂等/不可变来源 | N20–22 Pending单段确认；N25真实算法接受同ID/同数量 | 部分：实际算法接受两次→拒绝→恢复及其历史待I10 |
| P12 未知时间 | `widebandFixtureTaskNativeRoundtrip:4188–4199` 无segments、有observation、保存不丢；支持 `annotation_tests::testUnknownObservation` 不自动变活动 | N03/05/17及实测窗口观察；原始/current事实分列 | 已覆盖观察语义；不能从此证明短突发总体准确率 |
| P13 有效参数/物理时长 | `widebandConfigurationCommittedDrafts` SCN禁用及值保留；`parameterDraftsRequireExplicitCommit`、`parameterInputsIgnoreWheel` | N02时长事实；N31 ROI .75/2ms/noSplit；N32/33门限持久化 | 部分：完整Sample/ROI/Coarse×split启用矩阵及原生草稿/wheel待I13–I15 |
| P14 通道真实DDC | `unifiedAnnotationDraftsAndSegmentSwitch:3947–3967` 未确认禁用创建、勾选/取消不留确认；支持 `annotation_tests::testChannelSnapshotsAndSourceChanges` | N20–22确认/弹窗/取消/重开；N25–30真实CPU DDC与Changed快照 | 主要旅程已覆盖；非法保护带/输出率/实数源的界面拒绝边界随I22补 |
| P15 正式无损导出 | Widgets暂无标记JSON/CSV导出动作断言；支持 QtTest `LabelImportTests::codecFullRoundtripAndExports:318` 检查codec/导出值 | N06/28/33仅保存重开，未导出 | 部分：必须通过实际UI写JSON/CSV并核原始/当前/revision字段；浏览器demoOnly导出不作正式证据 |
| P16 单对象split/merge | `unifiedAnnotationDraftsAndSegmentSwitch` 检查选段/编辑；支持 `annotation_tests::testSegmentsAndSelection:688` split/merge/undo/同对象约束 | N20单段确认，没有split/merge操作 | 部分：I19实际多突发标签UI操作待补；暂停/跨进程续算/跨来源自动融合后置范围保持 |
| P17 两标签/六证据卡 | `torchSigLabelsNativeRoundtrip`、`widebandFixtureTaskNativeRoundtrip` 选中证据/折叠原始字段；支持LabelImportTests两类资产 | N17–19原始证据，N24 Sample，N29人工修订，N32/33 ROI | 部分：生成器TXT原生流程及Coarse/Fallback逐卡选择待I18/I20；不强制发明ROI曲线算法 |

## S0–S9 状态映射

| 状态 | 具体 Qt/支持检查 | 已有原生依据 | 状态及 M5 补项 |
|---|---|---|---|
| S0 无工程/无数据 | `emptyProjectAndFileActions:633` 无工程禁加源、创建磁盘工程、无file时谱操作禁用；未断言检测预检原因 | 无对应34场景 | 待原生：空状态的检测不可运行原因、无自动模型/假结果 |
| S1 Ready+标签 | `torchSigLabelsNativeRoundtrip:3997–4020` 实际4标签、幂等、只读、保存；`continuousMarkCreation` 手动工具 | N01实际TorchSig源/标签/手动，检测尚未提交 | 已覆盖这个Ready状态；第二标签格式由I20补 |
| S2 Loading/Partial | `loadedPrefixStopAndRoundtrip:2452` 实际256MiB源加载→点击stop→Partial，view≤prefix并重开保留；`importRealFilesAndCancelPrefix:3256` | 无对应34场景 | 待原生：Loading/Partial可用前缀及检测范围受限；不能用Ready图替代 |
| S3 ROI配置 | `widebandRoiPreservesManualTool`、`widebandConfigurationCommittedDrafts` | N02/07–10/31 | 部分：普通确认取消已有；边缘及全启用矩阵待I04/I13 |
| S4 模型/上下文未就绪 | Widgets已有配置预检非法活动范围；支持source/backend测试与M3资产/运行时报告。暂无UI短上下文/缺模型/不兼容GPU组合断言 | 无对应34场景；N23为模型成功 | 待原生：具体阻断原因、仍可标签/手动/历史；不补帧、不假检测 |
| S5 排队/运行 | `widebandFixtureTaskNativeRoundtrip` 真任务；`widebandConfigurationCommittedDrafts` 非运行时草稿；支持不可变request/代际 | N23真实阶段/计数，N02提交冻结范围 | 部分：实际运行中改view/草稿、查看/取消、快照不变待I02/I14/I16 |
| S6 取消/失败/部分覆盖 | 支持 `testPartialAndActiveCancellation`、`testFailuresAndGeneration`、`testFailedTerminalPersistenceKeepsRecoveryPrefix`；真实历史M3取消与M4失败记录 | N03/04仅Completed；不代表失败状态已显示 | 待原生：可区分取消/失败/部分范围，错误摘要/技术详情，保留历史/已发布前缀 |
| S7 完成/零结果 | `widebandFixtureTaskNativeRoundtrip` Completed；M4两个representative CLI零结果成功/恢复 | N03 Completed非零；没有零结果UI | 部分：Completed已覆盖，零结果界面与失败/缺资产区分待I16 |
| S8 六证据/复核 | `torchSigLabelsNativeRoundtrip`、`unifiedAnnotationDraftsAndSegmentSwitch`、`widebandFixtureTaskNativeRoundtrip` | N05/17–19/24/25/29/32 | 部分：Coarse/Fallback与完整复核组合待I10/I18；不从标题/按钮存在推断通过 |
| S9 通道持久化 | `unifiedAnnotationDraftsAndSegmentSwitch`；支持source snapshots与annotation/project_store | N20–22/25–30/33；16384真实DDC、保存重开及Changed不重算 | 已覆盖正向依赖旅程；损坏/源缺失UI与非法参数边界另见I22/I23 |

## I01–I24 交互映射

| 项 | 已有具体 Qt 行为断言 | 已有原生场景/报告 | 必须补验或当前状态 |
|---|---|---|---|
| I01 三入口同卡/正确范围 | `widebandConfigurationCommittedDrafts:4276–4301` 入口/ROI/available/活动预检 | N01/02；configurationEntrances实操三入口，无模态/复制/任务 | 部分：每入口预设范围精确比较尚未记录；不能用三个pass替代该子项 |
| I02 提交前可更新，提交后冻结 | `widebandConfigurationCommittedDrafts` request范围与草稿保留；未在已运行任务中改view | N02冻结available-file；N23运行 | 部分：运行后实际缩放/修改配置，再比run.request完全不变；提交前visible范围更新也应记录 |
| I03 manual→ROI→drag→Esc | `widebandRoiPreservesManualTool:4362–4412` 0对象/0task/view不变与创建工具恢复 | N07/08及roiInteraction实际操作 | 已覆盖；普通退出不等于停止任务 |
| I04 ROI Enter/边缘 | `widebandRoiPreservesManualTool` 物理单位/确认条合法、确认提交；未拖越界 | N09/10 Enter只提交ROI且对象/task不变 | 部分：补时间/有效频带两边缘的clip及半开样本合法性 |
| I05 缩放/轴平移/wheel | `boxZoomAndHistoryActions:609`、`mainAxisWheelIsolationAndHistory:2326`、`widebandAuxiliaryRenderingAndGestures:1734`、`navigationClickPreservesSpanAndCancelsDrag:2358` | 34场景只说明既有视图与显示，未逐个实施这些手势 | 待原生：背景/轴/wheel边界、标记几何不变；沿A1.4.3手势而非参考GUI平移 |
| I06 同段跨两模式 | `mainModeCoordinateDirection`/`largeSampleCoordinatePrecision` 测模式轴映射；没有同段选择保持断言 | 无对应模式切换场景 | 部分：选择明确segment，时频↔瀑布往返后ID/range/表树选择不变 |
| I07 重叠/多选同步 | `widebandAnnotationSelectionAndOverlap:4087–4145` 重叠菜单带来源/段号、Shift不拉入隐藏对象、Ctrl取消；`treeToPlotShiftSelectionPreservesAnchor:2910` | N05只有多来源可见、没有重叠多选操作 | 待原生：跨来源重叠选择与Ctrl/Shift，核表/图/树一致 |
| I08 手动拖/手柄/数值/undo | `selectedMarkMovePreservesSpan`、`selectedMarkCornerAndEdgeResize`、`allResizeHandlesStayBounded`、`escapeRollsBackMarkMove`、`captureLossAndDeactivationRollback:2748`；`unifiedAnnotationDraftsAndSegmentSwitch` undo/redo | N05/N29数值修订，不是手动八手柄测试 | 部分：规定屏幕drag/resize、松开一次命令、Esc/undo/redo精确恢复 |
| I09 来源初始只读→修订拖 | `torchSigLabelsNativeRoundtrip:4006–4009` 初始数值禁用后启用；支持 `testSourcesAndImmutableOrigin` | N05/N29数值修订保留source/origin | 部分：试拖原始标签/算法框不变，进入修订再拖，原始值/来源不变 |
| I10 两次接受/拒绝恢复 | `unifiedAnnotationDraftsAndSegmentSwitch` 接受/undo/redo；支持 `testSourcesAndImmutableOrigin` 幂等与`testConfirmationUndoRedoAndRejection` | N25一次真实算法接受，同对象同数量 | 部分：接受两次后拒绝/恢复，核ID/数量/source/history |
| I11 分数排序后批复核 | `widebandAnnotationSelectionAndOverlap:4114–4145` 仅review筛选、Shift选择、按钮数量；未实际执行批复核；`annotationTableBoundsAndIndependentSelection` 分页 | 无完整场景 | 待补Qt/原生：用不同mean/max值检查数值排序与mean筛选，批复核仅选中可见对象，隐藏对象不变 |
| I12 图层/source/run过滤 | Widgets暂无相应切换断言；workspace测试只核layer bar高度 | N05/各N要求三层均checked，不是依次切层 | 待补Qt/原生：逐层/source/run过滤，visible统计改变而stored total/计算结果不变 |
| I13 四策略/不拆分矩阵 | `widebandConfigurationCommittedDrafts:4324–4339` SCN margin与历史None；支持refinement参数golden | N31 ROI noSplit minOn/off禁用及实际.75/2ms证据 | 部分：Sample/ROI/Coarse×split全控件实际enabled状态与单位，stored inactive值保持 |
| I14 草稿/刷新/应用失焦/提交 | `parameterDraftsRequireExplicitCommit:1175` Tab/后台refresh/模拟ActiveWindowFocusReason；`widebandConfigurationCommittedDrafts:4278–4288,4346–4359` 检测输入Enter/refresh/切文件 | N05/N29数值Enter提交；无运行中草稿/失焦记录 | 待原生：检测草稿在任务更新/应用失焦保持未应用，再Enter/Tab提交下一次配置 |
| I15 wheel/侧栏/图谱 | `parameterInputsIgnoreWheel:1335` 遍历combo/spin与focus/方向；`widebandConfigurationCommittedDrafts` score wheel不变；图谱wheel专用测试 | native滚动主要是ensureVisible/scroll定位，没有wheel保值组合 | 待原生：实际wheel经过编辑/弹出列表不改值，侧栏滚动/图谱缩放仍工作 |
| I16 取消/失败/零结果/缺模型 | Widgets `widebandFixtureTaskNativeRoundtrip` 仅成功；支持任务失败/取消/恢复/长期错误测试及representative零结果CLI | N03/04 Completed、N23运行；其余状态没有可见场景 | 待原生：四状态、取消反馈、历史/覆盖/错误，终态拒写不能显示completed |
| I17 切文件/工程代际 | `fileSwitchCancelsUnfinishedEdit:2489` 拖动回滚；`widebandFixtureTaskNativeRoundtrip:4235–4243` 完成后跨文件run列表；支持 `testFailuresAndGeneration:164` 替代工程丢旧代际 | 无运行中切工程的原生场景 | 部分：实际任务运行中切文件/工程，迟到事件不串图、不入新工程 |
| I18 未定位/Sample/ROI/Fallback | `widebandFixtureTaskNativeRoundtrip:4199–4231` 选中未知fixture证据与原始折叠，不改变对象；支持物理time-pipeline | N17–19标签；N24 Sample；N29人工修订；N32/33 ROI；未知观察在N03/05 | 部分：逐一选Coarse及Fallback（有实测粗活动/无可靠活动），核method/status/原始-current/完整reason及正确数值域 |
| I19 多段标签split/merge | `unifiedAnnotationDraftsAndSegmentSwitch` 只选段与编辑；支持 `testSegmentsAndSelection:688` 拆分/合并/undo/单对象 | N20只有单段确认，未split/merge | 待补Qt/原生：实际多段标签UI拆/并，间隔不画实、其它对象不变、保留revision关联；跨gap人工merge须明确语义 |
| I20 两类导入 | `torchSigLabelsNativeRoundtrip:3976` 真meta/data、幂等、RF/400000样本/原始字段；非Widgets `LabelImportTests::actualTwoSourceAssets:592` 两资产 | N01真TorchSig导入/字段/来源；N06重开 | 部分：生成器WAV/TXT真实UI导入、重复幂等；两格式量化/绝对RF/配套JSON无重复框的原生核对 |
| I21 冲突/Partial标签 | 非Widgets `LabelImportTests::torchSigConflictsAndUnsupported:234`、`generatorRoundingAndRejectWholeBatch:135`；`testUint64AndPartial`持久化；`loadedPrefixStopAndRoundtrip`源前缀 | 无冲突/不足前缀可见场景 | 待补Qt/原生：实际冲突诊断及不部分发布；有效标签不因Partial前缀被截断，范围/可用性明确 |
| I22 确认→通道→来源变更 | `unifiedAnnotationDraftsAndSegmentSwitch:3947–3973` 勾选/取消不留下确认；支持 `testChannelSnapshotsAndSourceChanges:795`及Session保护带/实数限制 | N20–22/25–30全旅程；原revision/sourceTime/DDC保留且Changed | 主要旅程已覆盖；非法BW/输出率/保护带/实数源的UI诊断待补，不能只据底层false称界面通过 |
| I23 重开/导出/缺源/损坏 | `nativeProjectRoundtripAndInvalidImport:2583` 非法工程不替换；`torchSigLabelsNativeRoundtrip`、`widebandFixtureTaskNativeRoundtrip`重开；支持export/shard integrity/source失效 | N06/22/28/30/33实际保存重开 | 部分：真实UI JSON/CSV导出、source missing/result损坏状态与历史来源可读；不只检查错误返回值 |
| I24 布局/最大化/极值 | `workspaceLayout:368`、`responsiveWorkspace:970`（4K/150%）、`propertyFieldsFitSidebar:1002`、`panelMaximizeAndRestore:2267`、`renameUsesPlainTextAndEightyCharacterLimit:2836`、`largeSampleCoordinatePrecision:812`、`uiStatePersistenceAndRecentProjects:3744` | N11–16三拖动条提交/Esc回退、固定运行区可见 | 部分：布局拖动已覆盖；规定屏幕最大化/恢复+长名称/极大RF的组合与可见性待补。uint64时间极值不等于RF极值 |

## 已批准差异与不能扩大解释的结果

SCN 例外单列：用户批准所有时间策略禁用无效噪底裕量，明确CNR/频率3dB、Coarse/回退6dB；存储用户值不改、切回其它profile恢复，布局/动作保持。`SCN_PARAMETER_DECISION.md`、`APPROVED_UI_SPEC_REVIEW.md`/QUALITY、上述Qt配置用例有执行依据。此例外不要求修改冻结原型或把dB裕量引入线性门限。UI480秒仅终止保护；不能当P95交互性能目标。

M4真实Sample/ROI与DDC已替换对应原型合成内容；fixture原生仍明确“系统链路夹具，非模型”。ROI已保存实际残差门限和domain；没有保存的轨迹显示暂无，不将原型合成曲线当实测。标签类别不作为模型识别输出。用户已将批量效果验证留待M5后手动执行，L3/L4既有失败/未完成保留，不在A16映射中转成通过。

## 最小补验组合与收口

以下是基于缺口的建议输入，不是执行结果。主Agent可串行扩展既有capture，以少量真实文件/明确fixture完成动作，不必增加模型矩阵：

1. **状态/运行**：S0/S2/S4–S7、I02/I16/I17；覆盖无工程、Loading/Partial、缺资产/设备/短上下文、运行中view/draft冻结、取消/失败/零结果、切工程隔离。根任务已有缺资产/设备计划尚未作为本表通过证据。
2. **选择/复核**：I06–I12、I18/I19；同段模式切换、重叠多选、只读/修订拖动、两次接受/拒绝/恢复、mean排序过滤和真实批操作、图层/run过滤、六类证据及同对象split/merge。100k性能夹具若包含这些断言可提供对应支持；仅计时/非空截图不能关闭行为项。
3. **参数/文件/通道**：I01/I04/I13–I15/I20–I24；精确范围、ROI边缘、全启用矩阵、实际wheel/草稿、生成器标签/冲突、非法通道、正式导出、源缺失/结果损坏与规定屏幕极值布局。

补验应记录前后Annotation ID/segment/revision/source、run.request、存储/可见计数及实际错误/覆盖，结合控件操作和截图，不能只记录按钮存在。新增M5源码/测试、GPU/软件独立包与cold/hot证据必须以结束后的新身份另行更新矩阵。若不实现某原型模拟能力，按A16明确列未完成及影响，不能把本表“待补”改为通过。

用户另要求**最后push前README真实反映状态**；这是最终交付前待核的人类要求，不因本映射文件建立而完成。M5最终SPEC/QUALITY及提交、推送判断由后续完成证据决定。本轮综合标记保持 `A16_PENDING_MAPPING_GAPS`，不修改旧capture的两项false。

### 后续执行通知的边界

矩阵写成后，主Agent通知已有新的 `-FinalAcceptance` Release 原生 r1 **43/43**：包含S0、重复接受/拒绝/撤回复核、mean=.5与sort/Ctrl双选批复核、模式切换、标签split/merge/restore、JSON/CSV对话框导出、三策略×split及100k段压力；其报告仍明确压力为synthetic fixture。所报选择/filter P95为124.657/113.73ms、各20次。本轮尚未读取这些新增原始报告/源码身份逐项确认，因此没有用该通知改写上表为通过，也不将相关操作继续描述成从未执行。上表表示M4基线的证据缺口；后续应据新原始记录更新为已关闭、仍有边界或失败，缺模型/禁用CUDA启动、生成器导入及冷热资源等正在补充的工作同样不提前判通过。
