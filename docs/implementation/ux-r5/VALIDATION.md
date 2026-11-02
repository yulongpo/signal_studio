# UX-R5 验证与证据

本轮基线为 `5b2a499048d64290851ad02ecf764e17cf678231`，Windows 11 / Visual Studio 18 2026 / Qt 6.11.1 EXACT。最终审核版本由提交外 `out/tasks/ux-r5/INDEPENDENT_REVIEW.md` 的 `Reviewed-HEAD` 绑定。实施过程的失败保留，以下成功批次与失败批次分别计算。

2026-10-26 用户追加要求覆盖原任务包 UI-03 的复选框方案：三个来源 checkbox 和 31 px 工具行已经移除，来源 Tab 是唯一叠加来源。表格和工程树的来源数据仍保留；树跨来源选择切换 Tab；图谱不能命中其他 Tab 的隐藏对象。任务/日志保持最近业务来源。

## 构建和回归

公共 presets 经 `scripts/build.ps1` 构建，完整构建日志为 `out/tasks/ux-r5/logs/{debug,release}-integrated-r5-build.txt`；原生探针最后增量构建为 `{debug,release}-final-capture-build.txt`。部署仍使用本仓库 Qt/CRT 打包路径。编译器原有有符号比较、未使用变量和 D3D12 可选组件提示保留；实际显示路径采用 D3D11。

| 批次 | 结果 | 原始日志 | 覆盖 |
|---|---|---|---|
| 核心 12 组 / 每配置 | Debug/Release 各12/12 | `{debug,release}-integrated-r5-core-12.txt` | annotations、19 项 R5 service、4 项兼容业务 data 行、11 项事务、annotation_store、label_import、WAV、core、48 项 project_file、detection_store、channel_processor、project_tree |
| UX 5 组 / 每配置 | Debug/Release 各5/5 | `{debug,release}-integrated-r5-ux-5.txt` | 三 Tab / 唯一叠加与独立过滤 / 未定位转换、布局、证据全文分页搜索/复制、图谱、弹出层 |
| 旧 UI 20 个业务函数 / 每配置 | Debug/Release 各20/20；Qt totals22（含初始化/清理） | `{debug,release}-integrated-r5-ui-20.txt` | 分页/排序/选择/悬停/多段/只读/手工编辑/取消/树按需加载/真实 fixture worker 保存重开与替换 |

以上日志均位于 `out/tasks/ux-r5/logs/`，保留 CTest 输出及 QtTest 真实失败/跳过。LabelImport 的 4 个目录符号链接数据行在本机因创建符号链接权限跳过；其余标签用例实际执行。拒绝来源链接、路径越界、私有 GC 的其他隔离测试不因此删除。

首轮独立审核的菜单权限整改后，Debug/Release 完整构建、5组 UX及20个旧 UI函数重新通过，日志为 `*-review-fix-{build,ux-5,ui-20}.txt`。R5 UI 组新增 `uxR5_readOnlyMenusAndCommandGuards`，现在4个业务函数（Qt totals6），检查全部菜单/按钮和直接命令，Manual 正向及混合删除拒绝。核心/存储源码未改，沿用前述12组的实际结果。菜单探针最终增量构建日志为 `*-native-visible-menu-build.txt`；完整原生最终目录 `native-*-verified-r3`，菜单专项 `menus-*-r3-verified`，生产独立启动 `package-*-r2`。细节见 `REVIEW_FIXES.md`。

## 原生 Qt

入口 `scripts/verify-ux-r5.ps1`，不在探针中配置或构建。它从 `_bin` 部署目录、系统专用 PATH 启动实际 Qt 窗口，清除 DPI 覆盖，用 QWindow 实际鼠标/键盘、实际 Windows 显示器和 QWidget/QRhi 回读截图；不使用 offscreen 或超大窗口替代。Debug/Release、GPU/软件四次均15/15通过，共60个完整旅程主场景及16张补充窗口截图。四配置 `-MenusOnly` 各3/3，另有12张来源主场景和36张实际菜单图；合计归档124张 PNG、完整报告/步骤/launcher，索引为 `evidence/INDEX.json`。

| 配置 | 原生完整旅程 | 实际菜单专项 | 生产程序独立启动 | 实际显示 |
|---|---|---|---|---|
| Release GPU | 15/15 | 3/3 | PASS | QRhi D3D11 / AMD Radeon(TM) 880M Graphics |
| Debug GPU | 15/15 | 3/3 | PASS | QRhi D3D11 / AMD Radeon(TM) 880M Graphics |
| Release 软件 | 15/15 | 3/3 | PASS | QWidget / QPainter |
| Debug 软件 | 15/15 | 3/3 | PASS | QWidget / QPainter |

独立启动使用 `scripts/verify-package.ps1`，实际运行 `SignalStudio.exe`，不是以测试探针代替生产程序。完整报告在 `evidence/packages/`；四次均为对应部署目录 + 系统专用 PATH，硬件/软件路径符合请求。实际屏幕身份由上述原生旅程单独证明。

每次完整运行 15 场景：三来源 Tab；原始/算法键鼠只读；Tab 唯一叠加与定位；单条转换编辑去重；多选转换；生成器 TXT 100 对象/200 段原子转换、撤销重做；未知时间保持观察；树/图/表/Inspector 与真实 CPU DDC512；320/650 侧栏；390 侧栏及主/补充原文完整复制与搜索；配置待更新；ROI 新结果整体替换；完整零结果发布；工程保存重开且旧结果不复活。

实际探针识别 Redmi 27 NU，连接序号 2，`\\.\DISPLAY6`，3840×2160，Qt 2560×1440，DPR 1.5。每场景报告保留原生 monitorDevicePath、实际 DPI、全屏窗口身份、来源/选中/overlay ID、可见控件几何和各图谱 renderer 计数；最终补测还用 Win32 核对真实窗口可见和物理客户区3840×2160。GPU 成功要求非 CPU D3D11 和实际纹理/顶点数据绘制；软件明确标注 QWidget/QPainter，GPU 数据绘制计数为零。

场景报告的 `nativeJourneyElapsedMs` 包含交互、等待稳定和截图，不能当算法耗时。100 对象的 `conversionResponseMs` 测量整个按钮动作助手，包含诊断检查点写盘、滚动定位、原生输入与同步刷新，不能解释为独立应用主线程卡顿时长或纯转换计算时延。最终 `-r3` Release GPU/软件分别114/146 ms，Debug GPU/软件分别855/915 ms；对应场景记录的进程峰值 RSS 为1374.4/239.9/1417.9/276.5 MiB。GPU 场景包含 Qt 图形资源和实屏回读。原始 before/after/privateBytes 见索引，不声称 Debug 达到200ms或全配置低内存目标。这是代表性本地样本/探针测量，不作为大型 RFbench 或所有硬件性能结论。

## P0 需求与实际验证入口

| 需求 | 可审查证据 |
|---|---|
| UI-01/02/04/05/06 | R5 UI（原3个业务函数和整改新增菜单函数）、旧 UI20，原生 01/03/05/08–11；真实三表和已移除旧入口断言 |
| UI-03（用户覆盖） | `uxR5_layersAndIndependentFilters`、`annotationSelectionAcrossPagesAndViews`、原生03：没有3个 checkbox，Tab 派生唯一叠加，不重算频谱，树保全其他来源 |
| RD-01/02/03/04/05 | service19 中逐类普通写 API 拒绝和混合批次原子性；`uxR5_readOnlyMenusAndCommandGuards` 树 popup/图谱/全局/右侧和直接命令；旧 UI20 名称/属性/几何/段/删除/菜单取消；原生02/04及四配置各3来源实际菜单专项 |
| CV-01/02/04/05/07 | service19、兼容4、旧UI、原生04/05/07/13：快照不变、当前段/全段、未知时间、去重及源退役后继续人工编辑 |
| CV-03/06/08 | service19 的10/100、无效/预算整批拒绝、撤销/重做；compat 100 副本删源后保存重开；原生06/15 |
| RP-01/02/03/08 | lifecycle11 的 SCN→FFSCN→SCN、零结果和 ROI；原生 fixture worker 正常及 ROI 发布，受控 SCN 格式零结果，原生13/14 |
| RP-04/05/06/07/09 | lifecycle11 取消/失败/迟到/来源/coverage、双文件和样本通道隔离；旧 UI fixture roundtrip；原生12待更新及禁转换 |
| RP-10/11/12/13/14 | lifecycle11 真实 `.ssproj` stage/persist/commit/GC 断点、运行中保存恢复及 authoritative discover；project_file48 的进程终止/已存前缀/live anchor；原生15 |
| RP-15 | service19 及 lifecycle11 的对象/段/元数据/撤销预算前置拒绝，旧完整工程内存及磁盘保持；原生100的耗时/进程内存。未执行大型事件性能矩阵 |
| RP-16 / CP-06 | lifecycle11 私有 canonical 根、Copy/Reference/Preserve、安全共享侧文件、真实 save/GC 锁竞争；project_file48 原文件字节和未保存迁移 GC 保护 |
| CP-01/02/03 | compat4 历史 Imported/Algorithm 人工修订保全、畸形快照原子拒绝；lifecycle11 最新 run 裁决/并列诊断；原容器不覆盖 |
| CP-04 | lifecycle11 旧 Algorithm 通道退役后快照/Fs/范围/身份/configVersion 保持，保存/重开/GC 后绑定 IQ 通道下16个真实 CPU DDC 样本逐值相等、配置仍可更新且拒绝重绑 |
| CP-05/07/08 | service19 新通道 Manual 限制、source management 不进入 Manual undo；标签重新导入去重及独立人工副本；旧UI 文件/通道选择隔离；原生08/15 CPU DDC512及保存重开 |
| QA-01/02/03/04/05 | 公共构建/日志、实际原生场景、已知来源、下述失败/限制、未进行全仓指纹盘点；产品必要源/manifest/chunk 校验保留 |
| QA-06/07 | 最终精确 HEAD 的真正独立 Review 及 `scripts/publish-ux-r5.ps1`；判决/发布记录在提交外，不能由上述实施测试代签 |

## 数据与边界

原生采用仓库 `resources/sigmf_data_demo/0000000.sigmf-data`、`.sigmf-meta`、`.json`：400000 CF32 样本，20 MS/s，Fc 2.5 GHz，TorchSig 生成的合成信号，真实文件读取、4 条自动标签及重复导入。100 条 TXT 使用普通已知生成器解析入口；不是大型 RFbench。标签回归包含 WAV 头/布局及 DATA 正反向关联，未冒称本轮使用了未提供的现场 MATLAB 数据集。

正结果使用实际 `pipeline-fixture-v1` 独立 worker / durable manifest/chunk / production replay。SCN/FFSCN A→B→A 与零结果采用真实持久化链路的受控 publication；没有运行 SCN/FFSCN 模型，不据此宣称模型效果。真实 CPU DDC 与 GPU 显示分别记录，不是 GPU DSP 或现场 RF 精度证明。Ubuntu、持续采集、未实现模型识别和大型 RFbench 按既有范围未验证。

## 失败和修复留档

`out/tasks/ux-r5/diagnostics/`、`logs/` 保留全部首轮失败。主要包括：异步标签发布检查过早；底部默认 Tab 折叠未展开；fixture 发布修订封套与原始 replay 比较；工程树只投影当前页来源；原生树行被外层滚动区裁剪；测试名称超过已有80字符约束；辅助原文控件名称差异；旧测试仍要求直接编辑/撤销导入；旧工程 live pending 迁移、Reference owner 改写及未保存迁移 GC；移除图层后旧测试仍期望命中隐藏来源；任务列表异步刷新等待缺失；历史 JSON 预检助手不适用于带检测资产的 `.ssproj`，已改为实际 MainWindow 保存/打开。另有并发验证占用 worker 导致一次链接失败，以及脚本转发 regex 管道失败。修复或调整调度后补测独立记录，未删除失败或将不同批次混称一次全部通过。

系统剪贴板两次恢复探针 `clipboard-restored-probe.json`、`clipboard-restored-r2-probe.json` 记录 OpenClipboard 成功。Debug GPU `-r2` 的再次拒绝访问、第11场景失败及恢复后新目录补测分开保存；菜单专项的键盘菜单键/前台失败及外层退出码误判亦保留，见 `REVIEW_FIXES.md`。完整复制验证使用实际系统剪贴板，未以替代缓冲区充当成功。

最终构建/测试/四组原生与独立启动已闭合，未经独立 Review 不能发布。独立判决和推送结果以提交外报告及远端实际 HEAD 为准，不由实施记录预签。
