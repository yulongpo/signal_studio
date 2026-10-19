# U1 验证记录

工作基线：用户已批准的 U0 提交 `6615b33`；分支 `rebuild/a143-native`。

## 验证范围

本节点只验证统一选择、当前段展示、分页、筛选、左树按需加载和悬停。
直接编辑、批量删除的新语义和通道请求分别留给 U2、U3；本节点不声称
原生工程 v6、模型效果或 CPU DDC 的新增验收已经完成。

实际入口沿用 `scripts/build.ps1`、CTest runner 与
`scripts/screenshot.ps1 -AnnotationInteractions U1`。Qt 的 offscreen 回归
使用 QWidget 回退；原生截图使用实际 Windows 平台与硬件 QRhi，两类
结果分别记录。

## 自检过程

第一次定向检查在夹具来源格式校验时失败。修复方式是让合成夹具遵守
已有来源和证据格式，没有放宽生产校验。第二轮有效文件日志显示六项
行为通过、两项失败：表格悬停目标不在视口、树加载更多目标的外层滚动
布局未完成。原始记录保留在
`out/tasks/wideband-detection/U1/logs/ui-targeted-r1.txt` 和
`ui-targeted-r3.txt`，不计入最终通过结果。Windows GUI 子系统的空 stdout
重定向文件不作为验证结果；后续使用已有 runner 的 Qt 显式文件日志。

Debug 配置、构建和完整 CTest 已通过 **23/23**，总计 620.87 秒；其中
Qt UI **103/103**、451.25 秒，使用现有 480 秒保护。

随后原生截图发现树来源文字被 QSS 覆盖，补充局部绘制及像素回归。
修正后十项定向行为 **全部通过**（含初始化/清理为 12/12，65.591 秒）。
22 个非 UI 回归程序的 SHA-256 与已通过的完整运行版本逐个一致，
因此保留其通过结果；受影响的完整 UI 组使用同一 CTest preset、
runner 和 480 秒保护再次运行，最终 **104/104 通过**，CTest 用时
**456.14 秒**。八场景原生 **8/8 通过**。

| 最终证据 | 记录 |
|---|---|
| Debug 全量配置/构建/CTest | [23/23 日志](logs/build-ctest-debug.txt)，树文字修正前；非 UI 二进制随后逐个确认未变 |
| 树文字修正后构建 | [构建日志](logs/build-final.txt) |
| 最终十项定向行为 | [12/12 含初始化/清理](logs/ui-targeted-final.txt) |
| 最终完整 UI CTest | [104/104 详细记录](logs/ui-debug-final.txt)、[CTest](logs/ui-ctest-final.txt)、[JUnit](logs/ui-debug-final.xml) |
| 非 UI 结果复用依据 | [22 个程序的前后 SHA-256](non-ui-binaries-final.json)，全部逐字节一致 |
| 实际原生捕获 | [报告](native-report.json)、[入口日志](logs/native-debug.txt)、下方八张图 |
| 原生文字来源色 | [实际像素检查](native-source-colors.json)：Imported 358、Manual 368、Algorithm 2058 |
| 静态及输入身份 | [检查](static-checks.json)、[产物清单](artifact-manifest.json)、[独立复审](REVIEW.md) |

定向运行 r5 的九项行为全部通过（含初始化/清理为 11/11，55.993 秒）。
之后质量复审追加了空选择下悬停保焦点的边界，已在同一个结构导航用例
中补充；后续全量 runner 已执行该补充后的版本。旧 r5 全量运行因源码
修订主动中止，不计为最终通过。

| Qt 行为用例 | 直接覆盖 |
|---|---|
| `annotationSelectionAcrossPagesAndViews` | 跨 200 行分页、221 项 Shift、Ctrl、同 ID 再定位、全匹配和筛选/关层清理 |
| `annotationHoverProjectionAndNoCalculation` | 表/树/图悬停投影，不改选择、视区、modelReset 或图谱计算 |
| `annotationCurrentSegmentGeometryAndHandles` | 当前段几何、当前段排序、八个控制点及未知时间零控制点 |
| `annotationSelectionContextAndPublication` | 文件/样本通道/工程上下文隔离，后台发布保留草稿和选择 |
| `annotationTreeDemandLoadingAndViewOrder` | 每批 40 条至 268，展开状态保留，按树的完整顺序连续选择 |
| `annotationModifiedPlotClicksPreserveAnchorAndCurrent` | 已选对象上的 Ctrl/Shift、鼠标按下阶段不抢先改变锚点 |
| `annotationCheckboxShiftExtendsSelection` | 表格复选框的 Ctrl+Shift 扩展选择 |
| `annotationHoverTargetsOnlyOneSegment` | 多段对象只对悬停段绘制强调，不改变其它段 |
| `annotationTreeStructuralNavigationAndLocate` | 实际点击文件/通道、连续键盘焦点、含空选择的悬停保焦点、窄带返回宽带定位 |
| `annotationTreeSourceColorsSurviveInteractionStates` | 实际 viewport 文字像素，验证三来源在普通、当前、其它已选及悬停状态的颜色 |

原生捕获首轮在第五场景失败：驱动未等待异步树重建，且夹具仍处于
默认前 10 ms，未覆盖 17–19 ms 的远端对象。驱动改为显式设置完整
20 ms 视区，等待同 ID 的表、树就绪，原条件保留。第二轮前六场景
通过，第七场景因无选择时 inspector 按设计隐藏而拒绝滚动；驱动
改为校验显隐一致后仅滚动可见详情。这些驱动修正未改变产品行为。
失败报告仍保留在本节点 `out/` 中，不作为最终通过证据。

## 原生对照项目

| 原生场景 | U0 对照及断言 |
|---|---|
| 01 三来源及未知时间 | `01-ready.png`：保留来源色，未知时间不生成活动段 |
| 02 当前第二段 | `02-multisegment.png`：表格当前段与图形控制点一致 |
| 03 表、树、图选择 | 当前对象为实际 Ctrl 点击项，其它已选仍保留 |
| 04 悬停联动 | 悬停不改选择、视区、功率计算和纹理上传 |
| 05 图选远端对象 | `05-cross-page.png`：超过 100/200 后树按 ID 加载、表跳页，点击不缩放 |
| 06 筛选内全选 | 全部 268 个匹配，不限当前页；来源筛选后只保留 263 个算法夹具 |
| 07 关闭来源图层 | 隐藏对象退出选择，当前对象及表格同步清理 |
| 08 恢复筛选 | 重新显示图层不复活已清理的旧选择 |

八张原生图依次为 [01](screenshots/01-three-sources-unknown-time.png)、
[02](screenshots/02-current-segment-controls.png)、
[03](screenshots/03-table-tree-plot-selection.png)、
[04](screenshots/04-linked-hover-preserves-spectrum.png)、
[05](screenshots/05-graph-loads-far-tree-and-page.png)、
[06](screenshots/06-matching-selection-across-pages.png)、
[07](screenshots/07-closed-layer-prunes-selection.png)、
[08](screenshots/08-filter-restored-without-old-selection.png)。

排序、Shift、树每批 40 条、后台发布和上下文切换由 Qt 定向回归覆盖，
不以八张截图单独证明全部行为。

实际截图为第二连接屏 Redmi 27 NU / DISPLAY6，3840×2160、150% DPI、
2560×1440 逻辑全屏。三图均使用 AMD Radeon 880M 的 QRhi D3D11，
主图实际纹理上传 1 次；这属于显示证据。实际 IQ 为 20 MS/s 的
400000 样本 SigMF 文件；
四条标签来自其 meta，其余标记显式标为合成交互夹具。它们不证明模型
检测、真实 RF 精度或新增 CPU DDC 功能。
