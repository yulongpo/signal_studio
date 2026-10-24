# Signal Studio UX-R3 最终独立审核

Decision: REJECTED
Reviewed-HEAD: df0ba0453882287f2bda3cdaf2892dab44ece252

审核日期：2026-10-25（Asia/Shanghai）。审核者：本次独立 Review Agent，未参与本轮实现。结论只绑定上述精确提交；不是有条件通过，不授权推送。本报告在被审核提交之后生成，作为本地外部交付附件保留，不提交它来改变被审核身份。

结论：发现 **3 项 Major**，其中两项工程保存/资产问题已独立运行复现，另一项为冻结任务明确要求的星座工作区功能缺失。现有完整回归和实屏证据不能覆盖这些缺口。没有发现需要另列的 Blocker；实际指定屏幕当前存在，因此本次结论是 REJECTED，而非因设备缺失 BLOCKED。

## 1. 审核依据、边界与版本

按顺序读取实际 `AGENTS.md`、完整冻结 `REQUIREMENTS.md`、`DELIVERY.md`、`VALIDATION.md`，继而核查格式、标签、轴布局、窄带及兼容说明；阅读实际代码、基线至最终 HEAD 的关键 diff、原始 CTest/QtTest 输出和原生 JSON，目视抽查 PNG，没有以主 Agent 的计数或自述替代审核。

当前分支为 `feature/ux-r3-optimization`。初始和终结核验 HEAD 均为被审核提交。初始跟踪工作树无改动；用户原有 `.vscode/`、`reference/`、`resources/sigmf_data_demo/`、`tasks/`、旧宽带文档和 zip 等未跟踪内容保留。只写本报告和 `out/tasks/wideband-detection/UX-R3/review/` 下审查材料，没有修改实现、仓库测试、其它文档或既有证据，没有 commit、push、fetch 或切换分支。

独立核对 `36a509d56764457524faa72d9af6ff95429bcb1c` 与 `90be58e452f17b121028efc8b34af9be50a76e6e` 的 tree 均为 `1611e72a08e0bc503e5e0dd309c615cc4b04f578`；后者是被审核 HEAD 的祖先。未代替发布者执行最终远端防竞态检查；当前拒绝结论下不应进入发布。

版本范围核查成立：

- `064b509..HEAD` 的实现/测试/脚本检查范围中，只有 `app/main.cpp` 后续改变；`05d80d5..HEAD` 是交付文档与证据归档。
- `717ab8f..05d80d5` 包含启动验收入口和相关部署/关联脚本变化，没有改变 CTest 业务/测试 C++、轴布局、恢复 DSP 或检测 worker 算法。
- `05d80d5` 修复的回调引用状态确已移到 `app.exec()` 生命周期外层；固定 150 ms 改为最多 5 s 等待 settled 和硬件新帧，最终仍要求 GPU 数据绘制、上传及纹理复用，独立眼图允许仅顶点上传。旧全量不是新 CLI 的测试，12 个最终实际部署进程是该入口的验证。
- 复核 `final-binary-identities.json` 所列 Debug/Release 的 app、UiCapture、DetectionWorker 共 6 个本机二进制，实际 SHA-256 均匹配。独立复现链接的是现有 Release Core/Infrastructure 库；相关源码在完整 Release 验证后未改变。没有重新构建被审核工程。

## 2. 必须修复的 Major

### M1 / Major：正常任务进展使已成功保存的工程无法重新打开

位置：`infrastructure/project_file.cpp:79` 的 `manifest()`、`:96` 的 `verifyManifest()`、`:221` 的容器资产清单、`:252` 的加载次序；`infrastructure/detection_store.cpp:773` 的 `writeDetectionRun()` 和 `:806` 的原子 manifest 更新；`app/main_window.cpp:4970` 的当前工程保存。

外层清单递归记录 `annotations/` 和 `detections/` 的所有文件，并固定每个文件的长度与 SHA-256。这包含 **可变** 的 `detections/<run-key>/manifest.json`。普通当前路径保存允许任务继续运行。任务管理器后续通过 `writeDetectionRun()` 正常更新阶段、进度、chunk 索引和终态，而外层 `.ssproj` 保留保存时的旧摘要。下次打开在 `verifyManifest()` 失败，尚未进入内部检测 run 读取/中断恢复。即使持久 chunk 本身完好，也不能从正常工程入口恢复。

独立复现见 `review/active-save-repro.cpp`、`active-save-repro.log`（这里及下文 `review/` 均指 `out/tasks/wideband-detection/UX-R3/review/`）：使用真实生成的 CI16 文件、现有 Session 夹具、实际 `ProjectFileCodec` 和 `writeDetectionRun/loadDetectionRun` API，创建 Running run 并保存当前工程；先打开成功，再仅把计算阶段从 `reading` 合法更新为 `inference`。没有直接篡改磁盘 JSON，也没有模拟 hash 失败。结果：

```text
saved_running_project_initial_load=1
normal_progress_manifest_valid=1
saved_running_project_load_after_progress=0
error=资产长度或 SHA-256 不符：detections/.../manifest.json
target_preserved=1
```

目标内存保全正常，但已保存工程的可恢复性被破坏。chunk 发布和取消同样更新这个 manifest，源码路径包括 `DetectionTaskManager::checkpoint/finish` 和 `appendDetectionChunk`。现有原生“取消后保存再重开”不能证明“保存后继续运行/取消再重开”。

修复要求：明确区分不可变证据与可变运行索引，采用可验证的版本快照或等价一致性方案；不能简单取消 chunk、来源或不可变请求的完整性检查。补测保存后正常推进、发布新 chunk、取消及进程中断后重开，确认可恢复持久前缀、保留未读覆盖和旧来源证据。

### M2 / Major：Reference 工程普通 Save 会切换资产根，漏过活动任务保护

位置：`app/main_window.cpp:4972` 只比较工程路径的保护、`:4984` 固定 `AssetSaveMode::Copy`、`:5019` 的引用另存；`infrastructure/project_file.cpp:184`–`:187` 的资产根选择；`infrastructure/detection_task.cpp:706`、`:807`、`:1123` 的任务目录绑定与持续写入。

用户可以在活动任务期间选择 Reference 另存，资产根仍为旧根。随后在**同一新工程路径**按普通 Save：路径比较相等，保护不执行；codec 收到 Copy 后使用 `<新路径>.assets/<projectId>`，没有保留当前外部 `assetDirectory`。保存完成后会话改用新根，但活动 `Job::directory` 仍保存旧根，后续 chunk、manifest 和终态继续写旧根。新副本可能停在旧前缀，而会话继续显示新的运行结果；再次保存/恢复可能缺失对应证据。复制时也没有冻结写入中的资产。该问题独立于 M1 的外层摘要问题。

同一独立复现调用 Reference 保存，再执行普通保存实际传入的 Copy，输出：

```text
ordinary_save_after_reference_changes_asset_root=1
worker_assets=.../live.ssproj.assets/<projectId>
new_project_assets=.../reference.ssproj.assets/<projectId>
```

代码证明没有迁移活动任务的 `Job::directory`。复现直接证明 codec 切根；没有把此无模型夹具声称为实际 TensorRT 运行。闲置的 Reference 工程也会在普通 Save 时无提示变成 Copy，因为当前加载只恢复解析根，不保留用于普通保存的引用模式。

修复要求：普通保存应保留当前资产位置/模式，只有明确的复制另存才迁移；活动保护应依据资产根是否变化，而不只看工程路径。验证活动及闲置 Reference 工程的同路径 Save、关闭重开后 Save、显式 Copy 被阻止/结束后成功，并确认任务所有后续证据仍归属于正确根。

### M3 / Major：G5 星座页缺少任务书要求的判决观察和参考区域

依据：冻结 `REQUIREMENTS.md` 第 5 节明确要求星座页具有“原始/预处理/同步/判决阶段切换、参考决策区域、缩放和坐标读数”，并要求中间状态可观察。

实际 `domain/narrowband_analysis_settings.h:5` 的 `AnalysisStage` 仅有 RawIq、CfoCorrected、MatchedFiltered、TimingAligned、CarrierRecovered；`ui/narrowband_workspace.cpp:1185` 的阶段选择也只有对应五项。`renderAnalysis()`、`NarrowbandChart::rebuildGeometry()/draw()` 只消费实际散点并画网格/零轴，没有所选调制模板的参考决策区域或判决后结果。`single_carrier_recovery.cpp:277` 确实在载波环内部计算最近模板判决，但没有发布为可选的判决观察阶段。

此外 `buildPages()` 在 `:1656` 附近只给波形/PSD/时频绑定手势及驻留读数；星座图没有对应 I/Q 指针读数回调。现有自动缩放和手工上下限能够改变等比例范围，但不能替代缺失的参考区域、判决观察和 I/Q 坐标读数。

独立目视 `cf32-04-recovered-constellation.png`，可确认真实恢复散点及等比例图框；它没有证明上述缺失功能。现有 18 场景矩阵和 UI 测试也不覆盖这些要求。因此 G5“全部完成”结论过宽。

修复要求：在明确的用户调制假设下补齐参考模板/决策区域、可追溯判决观察与 I/Q 读数，保留失败/未知条件和实际来源。这里不要求部署真实识别模型或输出真实位流；两者不可用的边界可以继续保留，但不能用来豁免本项显示要求。需要相应行为及原生证据，再审核新的最终 HEAD。

## 3. Minor 与性能/布局判断

### m1 / Minor：交付未充分披露本轮 Release 性能目标未达到

`evidence/release-detection-native.json` 的真实启动反馈为 **367.1684 ms**、`startTargetMet=false`；取消可见反馈 **123.8201 ms**、取消完成 **278.1047 ms**。十万活动段合成压力场景中，筛选 p95 **416.1672 ms**、选择 p95 **435.4769 ms**，两者 `targetMet=false`，建议目标为 200 ms。不能沿用旧 R5 “Release 达到建议目标”的结论。

本轮 `DELIVERY.md`/`VALIDATION.md` 没有明确列出这些未达值。应补充真实限制；不把建议目标自动提升为新的硬门禁。`refresh()` 每次在 GUI 线程调用 `isProjectDirty()`，后者完整生成 `stateToken()`、遍历标记/活动段/运行证据，值得进行有针对性的性能剖析；本次未做因果剖析，不将该推断当作已证实的唯一原因。

### m2 / Minor：主图面积摘要遗漏较差场景，旧测试不再约束纯绘图区高度

独立从实际 `renderBeforeGestures.charts[].logicalWidgetSize` 和 `mainAxes/auxiliaryAxes.plot` 重算同外部尺寸的旧 `left=88,height=H-46` 基准，结果存于 `review/axis-area-independent-v2.json`：

| 模式 | 主图面积变化 | 辅助图面积变化 |
|---|---:|---:|
| A/B | -2.904% 至 -1.708% | -28.56% 至 -27.68% |
| C | -3.116% 至 -1.896% | -17.367% 至 -16.327% |

因此文档“主图约减少 1.7%–2.2%”不是完整范围。例如 C-waveform-tf-long 为 `(1748×836)/(1760×857)-1 = -3.116%`。合并主/辅助的 min/max 表虽覆盖该数字，却不能据其上端推导主图完整范围。

`tests/ui_tests.cpp` 从纯 `plotRect().height()` 改成 `plot.height + top + bottom -46`，代数上是 `H-46`，**确实取消了原纯数据高度下限**，保留下来的是外部空间约束；断言文案仍称只有原图层条/分隔条扣除，容易误导。不能把这个测试通过当作“纯数据区无退化”。

按冻结 UX-R3 任务判断，轴标题/刻度必须分带并真实测量，48–64 不是硬上限，也没有承诺总面积不降；A/B 左边距实际 40–62、C 78–100，文字与物理交互证据成立。故本次不把公开可测的文字带成本本身判为 Major 或不当放宽；但应纠正文案及主图范围，并保留独立纯数据区指标，避免以后用外部分配测试掩盖内部任意膨胀。短辅助图约 28% 面积代价应作为明确遗留取舍。

## 4. 已独立审查且有相应证据的路径

| 范围 | 实际阅读的主要函数/文件 | 审核判断及边界 |
|---|---|---|
| 工程容器 | `ProjectFileCodec::save/load/importLegacy/relocate/signature`；`ProjectStore::encodeDocument/decodeDocument/stateToken`；MainWindow create/open/save/close/confirm/relocate | magic/schema/UUID、稳定 ID、跨 C/D 绝对引用、轻量签名、错误绑定拒绝、只读 v6 转换、v1–v5 拒绝、QSaveFile 发布及失败 target 保全有实现和测试；不能据此豁免 M1/M2。标记分片内容寻址，不再改写虚拟 `state-v6.json`。 |
| 轴布局 | `axis_layout::measure/sharedBudget/arrange`；PlotWidget measuredAxes/axisLayout/freeze/release/paintScene；物理映射和手势 diff | 实际 QFontMetricsF、共享预算、缓存键、单位/原点、手势固定几何、曲线缓存 left/top 和 GPU/软件同源 plotRect 可核查。原生游标、轴缩放/拖动、ROI 与控制点断言有原始数值；面积成本见上。 |
| 标签 | `Inspector::exact/preview`、discoverLabelCandidates/discoverDataForLabel、prepare/assess/publish、reusableLabelFieldMapping；importMappedTextLabels、generator/TorchSig parser；LabelMappingDialog::accept；MainWindow::discoverLabels 与 AnnotationPanel::importLabels | 严格同名跨目录发现，大小写/别名/多候选显式处理；未知编码/字段不猜；精确路径、IQ binding、唯一解释复用；uint64/秒/帧/索引基准/RF 基带/舍入/预算边界及整批发布有实测。不可变原文、软删除去重、手工标记和用户修订保留。真实未知 MATLAB profile 未提供是任务允许的阻断，不是生产验证通过。 |
| 窄带数据链 | domain Project/Channel/AnalysisIdentity/NarrowbandAnalysisResult；processingSettings/requestAnalysis/installAnalysis/renderAnalysis/cancelAnalysis；analysis_service、analysis_source、constellation_analysis、eye_analysis、single_carrier_recovery | 五页及旧 v6 名称兼容、独立视图、共享不可变结果、代次/物理修订/取消检查成立；真实 CPU IQ/DDC/恢复保留，显示预算不重复同步；模型与位流明确不可用。已有阶段快照用现有类型表达，不因未采用任务书示例类名而扣分。M3 是实际缺失行为。 |
| 检测闭环 | DetectionPanel enqueue/poll/recover；DetectionTaskManager enqueue/checkpoint/finish/worker publication；detection_store 与原生 capture | 既有 worker/算法没有被 UI 改动重写。冻结请求、覆盖类别、真正零结果/未读、历史来源、质量复核、显式定位、通道快照与真实 DDC 有运行证据；活动保存恢复仍由 M1/M2 阻断。 |
| 部署/发布 | app/main.cpp 回调；verify-package/gpu_smoke/screenshot；ssproj-association/test-ssproj-association；publish-ux-r3；CMake 与 build/run_ui_test | 12 个最终部署进程、真实文件关联注册/ShellExecute/注销的证据可核查。发布脚本要求唯一批准和精确 HEAD、跟踪文件干净、fetch/祖先检查、正常 push；本报告拒绝，脚本不得发布。 |

## 5. 亲自运行和复核的证据

### 本审核运行

- **工程文件行为测试 35/35**：直接运行现有 Release `SignalStudioProjectFileTests.exe`，工作目录、TEMP/TMP 均设在 review；日志 `review/SignalStudioProjectFileTests.log`。这是既有覆盖，通过不代表 M1/M2 已被测试。
- **标签测试 22/22、0 skipped**：`review/SignalStudioLabelImportTests-v2.txt`。实际生成器 10 对象/65 段、TorchSig 4 对象及来源摘要与归档一致，读取真实指定资产，不声称未知 DATA/TXT 生产格式通过。
- **轴测试 13/13、0 skipped**：`review/SignalStudioUxR3AxisTests-v3.txt`，按项目构建脚本设置 offscreen、1.5 和 `QT_QPA_FONTDIR=%SystemRoot%/Fonts`。首次直接运行漏设 offscreen 字体路径，出现字体数据库警告及 11 passed/2 failed；文件 logger 后保留在 v2.txt。补齐构建脚本已有环境后通过，未改代码或断言，不把最初失败隐去。
- **M1/M2 独立小复现**：`review/active-save-repro.cpp`、`.rsp`、`.log`；Release 现有静态库链接。初次编译缺 `/Zc:__cplusplus`、第二次缺 `/utf-8` 的编译诊断也保留；第三次编译和运行成功。复现不等同完整真实 worker 流程，其 API 行为与 worker 持久化调用链已分别核对。
- **当前屏幕只读探针**：现有 UiCapture `--list-screens`，`review/current-screens-process.log` 显示 Windows、Redmi 27 NU、连接 **1**、`\\.\DISPLAY6`、首选/实际 **3840×2160**、DPR **1.5**、Qt **2560×1440**、Windows target device path。强制实屏没有缺失。
- `git diff --check` 通过；归档 71 个文件逐一核对 bytes/SHA-256，零不符，manifest 本身不自列；6 个交付二进制 hash 一致。日志 `review/archive-and-binary-identities.log`、`evidence-independent-check*.log`。没有覆盖归档原件。

### 已归档真实原件复核

- `debug-final-ctest-raw.log`、`release-final-ctest-raw.log` 各有 40 个实际 test block，均有通过记录（其中一个用 `Test Pass Reason` 而非字面 `Test Passed`）。完整构建/CTest转录给出 **1400.40 s / 308.44 s**；Debug 参数组 **426.05 s**。三个主 UI 组 TIMEOUT 仍为 480，没有放宽。
- 独立解析逐行 Qt PASS、data tag、Totals，并与 Qt inventory 比较：两配置主 UI **106 个唯一业务行**、窄带 **11 个唯一业务行**，无缺失、额外、重复或 skip；窄带日志明确 W8。初次审查解析脚本未包含 data tag，随后纠正解析并保存 v2 日志，不据错误解析否定归档。
- 轴 Debug/Release × GPU/软件各 24 场；窄带四配置各 18 场。核查原生 runner 的实际窗口命中、屏幕身份/首选模式、禁止 DPI 覆盖、全屏与截图尺寸条件，及 GPU fresh frame、标量热图/顶点、非 CPU D3D11 和软件零 GPU 计数条件。没有把 offscreen 当实屏。
- `release-detection-native.json` 的 46 个唯一场景包括真实 TensorRT、样本/ROI 结果、可选质量复核、来源修订、CPU DDC 能量/输出摘要、保存重开、取消持久前缀及明确标为合成的压力场景；三源标记和未读覆盖有具体记录。性能不达建议目标见 m1。
- 最终 12 份部署 JSON 中 `pass/overlaySettled/freshOverlayFrame/systemOnlyPath` 均为 true；GPU 有实际 draw/vertex 上传，宽带与五页有热图；真实眼图 GPU 热图为 0 而顶点为正是正确条件，软件三项 GPU 计数均为 0。五页的手工验收 Rs 明确为 **1 MHz**；真实已保存眼图不添加这个 CLI 假设。普通启动仍 Unknown。
- 实际关联记录为中文/空格 `.ssproj` 路径、正确引用的 exe/`--project`、项目标题与注销成功；首轮只读注册表句柄错误、早期 Debug/Release 全量失败、超时、中断及旧 overlay/Rs 失败原件仍在。未把后续成功冒称首轮全过。
- 目视抽查 A/B/C 的 `*-psd-tf-long.png`、`cf32-04-recovered-constellation.png`、`cf32-05-recovered-eye.png`、`cf32-06-recognition-unavailable.png`、`27-real-algorithm-cpu-ddc.png`。确认轴带区别、刻度/图框、真实恢复图、不可用声明和 DDC/来源界面；目视工具显示时缩图不改变原 PNG 的 3840×2160 身份。截图不能替代以上 API/手势/恢复证据。

## 6. 遗留边界和发布结论

真实未知 MATLAB DATA/TXT 样本/规范未提供时，严格配对加显式字段映射、未知字段阻断符合冻结任务的允许分支；不要求猜测。受限 SigMF profile、生成 IQ 的真实 CPU 恢复、TensorRT 模型包和 QRhi 显示分别构成不同证据；现场 RF 精度、任意 SigMF、其它 GPU/TensorRT 包、真实调制识别/解调位流、设备持续采集和 Ubuntu 不在已通过范围。

旧 U3 v5 capture 与当前 v1–v5 拒绝合同冲突，保留为历史未通过项是正确的，不要求恢复 v5。被自动审批拒绝清理的小夹具可保留，不影响本审核；没有再次尝试清理。

已确认的构建、回归、实屏和部署证据有效，但冻结要求与活动保存安全仍未完整满足。**M1、M2、M3 修复并补足有判别力的测试/证据后，必须重新派发独立审核，审查新的最终精确 HEAD。当前提交不得推送。**
