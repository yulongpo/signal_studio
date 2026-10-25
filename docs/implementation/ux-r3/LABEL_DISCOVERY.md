# UX-R3 G4 标签发现与字段映射

2026-10-25 反馈修复更新：当前自动发现和反向定位均支持 `data/<同名>.wav` 与 `label/<同名>.txt`，保留 `.data`；同名 WAV/DATA 冲突需明确选择。文件菜单及底部统一标记记录均提供“导入标签…”。真实 WAV/TXT 的发现→预检已验证 10 对象/65 活动段；以下 G4 冻结内容属于历史记录，当前交付见 [feedback-01/DELIVERY.md](feedback-01/DELIVERY.md)。

本文件记录 G4 子任务的实现合同和集成条件。实现只写入 `infrastructure/label_import.{h,cpp}`、`infrastructure/label_discovery.{h,cpp}`、`ui/label_mapping_dialog.{h,cpp}`、`tests/label_import_tests.cpp` 和本文件。CMake、MainWindow、domain 注册、工程格式和原生验收由主 Agent 统一处理。本文不代表构建、测试或实屏通过。

## 可核实的输入依据

2026-10-23 仅查询了 `reference/`、`resources/`、`tasks/` 的文件清单，以及已有夹具 README 指向的明确样本路径，没有全盘扫描。

- `resources/sigmf_data_demo/0000000.sigmf-meta` 和 `0000000.json`：实际读取了元数据前部；meta 声明 `cf32_le`、Fs=20 MHz、Fc=2.5 GHz，首条标签样本起点 51107、数量 292170，频率边界为绝对 RF；JSON 声明 `rfbench-v1`、400000 样本、4 事件及基带偏移坐标。沿用现有解析器的全部 schema/物理校验，未扩展为任意 SigMF 支持。
- 生成器实际标签 `D:/coding/FFSCN_train2/broadband_signal_generation/figures/examples_60MHz_10ms_20260906/sparse/label/20260906-213604-798_914585_bw_60000000_sampleTime_0.01_rollOff_0.35.txt` 可读，逐字节比较与 `tests/fixtures/wideband/generator-60m-10ms.txt` 相同。同主名 `data/*.wav` 存在，`data/*.data` 不存在。因此它证明已知生成器的带键 TXT 语法，不能证明用户未知 DATA/TXT 数据集的列、编码或单位。
- 在限定范围内未获得用户未知 TXT 的实际列规范或 `.data` 编码。新增的列式样本均为测试夹具，不能声称真实 MATLAB DATA/TXT 已验收。未知 TXT 必须经过交互映射；`.data` 的实/复、分量类型、字节序、布局、Fs/Fc 仍由普通源导入预检确定。

## 发现与信任规则

`discoverLabelCandidates(FileMetadata, options)` 为只读、非递归入口：

1. `dataset/data/name.data` 严格查 `dataset/label/name.txt`，主名和规定路径名称大小写必须一致；同时检查源同目录的同名 TXT。两个候选并存返回 `Ambiguous`，不自动择一。
2. `name.sigmf-data` 查同目录 `name.sigmf-meta`，可附加 `name.json`。仅有 JSON 而无 meta 是错误。补充 JSON 作为同一来源的一部分，不作为第二组标签重复导入。相同的完整目标物理绑定和原 profile 限制继续适用。
3. TXT 首个非空行必须具有生成器记录头及 `time_start/time_end/sig_len` 键，才路由到既有生成器解析器；随后仍整批校验记录和物理范围。其它同名 TXT 只返回 `NeedsMapping`，不会按列名或数值范围推断格式。
4. 不搜索其它主名、不递归、不读取无关标签内容。目录枚举只用于严格名称、大小写冲突和预算检查。大小写仅近似匹配、符号链接、Windows junction 或祖先目录别名均阻断为 `Ambiguous`。多个真实候选允许主界面显式选择一个后交给 prepare；别名/大小写失败不生成可选候选，需纠正路径。
5. `discoverDataForLabel(path)` 只反查 `label/name.txt` → `data/name.data`，返回路径而不猜测原始二进制配置。

发现状态：`Found`、`NotFound`、`Empty`、`NeedsMapping`、`Ambiguous`、`Error`、`LimitExceeded`、`Cancelled`。`Empty` 用于完整读取范围内确认为空白的 TXT，主界面可直接提示而不弹映射；SigMF 的合法空 annotations 由解析阶段返回 `LabelImportState::Empty`。无标签消息是“未发现标签，可手工创建或选择标签文件”，不阻断普通数据分析或手工标记。

默认限额为累计 2048 个目录项、16 KiB 预览、32 MiB 原始主/补充元数据、100000 条/段、1000000 行、JSON 深度 64，发现与解析各自有 5000 ms 协作式截止时间。调用者可在公开范围内显式调整限额。实际目录较大时会明确返回限额状态，不截断候选后擅自选取。原有规范化来源/标记累计 32 MiB、单对象分片预算继续生效。取消回调在 worker 内调用；OS 单次目录访问、文件打开或读取的阻塞无法被 Qt 同步 IO 立即中断，截止时间不代表可抢占式 IO 保证。

## 映射合同与物理坐标

`LabelFieldMapping` 是 version 1 的显式配置；默认全部语义未指定、列为 -1。UI 用 1 起点列号，0 表示未知，提交后转换成 API 的 0 起点列号。

| 字段 | 支持的显式选择 |
| --- | --- |
| 文本 | 空白 / Tab / 逗号 / 分号；跳过开头物理行数；可选注释前缀 |
| 类别 | 指定非空文本列，不从其它字段推断 |
| 时间 | 未知 / 样本 / 秒 / 帧；起点列和终点或数量列 |
| 索引 | 明确 0/1 起点，保存为 uint64 `[start,end)` |
| 区间 | exclusive end / inclusive end / start+count；秒不支持无采样依据的闭合终点 |
| 帧 | 显式提供实际帧长和帧步长，单位为样本 |
| 频率 | 未知 / 下上界 / 中心与带宽；Hz/kHz/MHz/GHz；RF/基带偏移 |
| 精度 | 显式允许最近样本舍入；可声明原文时间量化步长（秒） |

样本和帧必须是 uint64 整数字面量，不经过 double。1 起点闭区间 `[1,10]` 转换成 `[0,10)`；帧区间归一化为 `[a,b)` 后映射到 `[a*hop,(b-1)*hop+length)`。所有整数加法/乘法检查溢出。秒坐标从 0 起；出现非整数样本边界时，未明确允许舍入则拒绝。舍入误差、量化步长、原始列和映射写入证据；不声明量化精度不能当作已测得精度。

RF 列不加 Fc；基带偏移转换到 RF 时只加一次 Fc。频率必须有限、递增、处于目标物理 Nyquist 范围；带宽必须正数。验证使用 `annotationBounds` / `validateSegment`，最终发布再走 `AnnotationService` 的全量验证。超出已加载前缀但仍在物理文件内的活动段可保留，不能据此越过可用前缀创建通道。

时间未知而频率完整时生成 `observation`，其 `observationTimeQuality=Unknown`，没有 segments，也没有活动段；`[0,physicalSamples)` 仅表示文件观察支持，绝不宣称实际发射时长。原始 observation 同时进入不可变证据，预览不把该支持时间计入已知活动时间范围。

纯类别或频率未知时，现有 annotation 模型不能表达无频率对象。解析仍返回 `Incomplete`、明确 error、`incompleteRecords` 和 `unknown-label-geometry` 诊断，annotations 为空。prepare 将完整保留的 sourceSet 转成 `LabelImportDisposition::IncompleteEvidence`，主界面必须归档预览并明确确认“仅保存原始来源，0 个可定位标记”，再调用 publish。它通过 `AnnotationService::publishSourceSet(file, sourceSet, {})` 原子保存原文、hash、诊断和映射，不创建类别标记或频率框。取消不调用 publish，也不修改工程。后续重开可恢复映射，同源自动重检去重；内容或解释改变仍需 SourceChanged 决策。incompleteRecords 是原始记录数量，不能计为合法框数量；classCounts、timeExtent、frequencyExtent 在该路径保持空。此路径不要求新增 file 级字段。

简单分隔 profile 不支持带引号/转义的 CSV、任意编码或多行记录。它们返回明确错误，需要专用规范后才能支持；不会猜测处理。

## 已保存映射与来源去重

映射来源使用：

```text
formatId = mapped-txt-v1
profileId = user-confirmed-delimited-txt
parserVersion = 1
profileVersion = 1
coordinateInterpretation = encodeLabelFieldMapping(mapping) 的完整 JSON 文本
```

不新增 file 级 profile 字段。`restoreLabelFieldMapping(sourceSet, mapping, error)` 校验上述 profile、JSON 和 schemaVersion，恢复到临时对象，失败时不改传入 mapping。`reusableLabelFieldMapping` 仅复用未删除、实际路径相同、IQ 物理绑定相同且所有有效解释一致的映射；有多份不一致映射时重新打开字段配置，不按数组顺序任取。移动标签后必须显式确认字段，重新 prepare 仍检查实际绑定；同名或路径近似不作为同源证明。

来源 ID 包含主标签 hash、补充 hash、完整转换参数和物理目标身份，不包含导入时的 projectGeneration、可用前缀或标签路径。相同标签移动而目标不变可去重。源文件路径/物理配置改变需要重新核对；内容、supplemental、映射或物理绑定发生变化返回 `SourceChanged`，不自动覆盖当前来源及用户修订。已有软删除对象也不自动复活。手工 sourceSets 不参与导入来源匹配。

`publishDiscoveredLabels` 重新核对当前工程 generation、文件/通道、样本格式、Fs/Fc、物理样本数和已知指纹等绑定，并重新检查当前 sourceSets；可用前缀推进本身不使原始标签失效。来源的原文与 hash 是 worker 准备时的不可变快照；发布不在 GUI 线程重新读外部文件，之后重新发现才会检测外部标签再次变化。没有已知 IQ 指纹时，不声称对全量 IQ 做过内容哈希。

`Ready` / `Empty` 通过 `AnnotationService::publishSourceSet` 一次发布；`IncompleteEvidence` 经 UI 显式确认后只发布来源，保留解析 error 供展示；`AlreadyImported` 返回 changed=false（包括已归档的 incomplete 来源）；`SourceChanged` / `NeedsMapping` / `Rejected` 一律不发布。显式重载/比对由 app 决策，不能把 SourceChanged 直接改成 Ready 来绕过用户修订保护。撤销、手工标记和原有三来源服务保持原路径。

## 主 Agent 集成

必须由主 Agent 在 `domain/annotation.cpp::validateAnnotation` 的 importedFormat 白名单中加入 `mapped-txt-v1`（主 Agent 已确认完成）。本子任务没有写该文件，也没有借用生成器 formatId 伪装未知 TXT。`mappedProfileUsesAnnotationServiceAndProfileChangesRequireReview` 测试会在漏加注册时明确失败。CMake 需将 label_discovery.cpp 加入现有 infrastructure 库，将 label_mapping_dialog.cpp 加入实际使用的 GUI 目标（主 Agent 已接入并完成首轮 Debug 编译）。

源导入得到已确认的 FileMetadata 后，在静默、可取消的后台执行发现。可信唯一候选继续后台 prepare；未知 TXT、歧义和来源变化先在日志/状态栏给出可操作提示，通过文件菜单“重新发现当前信号标签”打开预览或字段映射，不在后台任务结束时抢占输入焦点。字段对话框在主线程显示，取消不生成 plan、不发布。复用映射也必须经过 prepare。

```cpp
// Owning thread: freeze only the necessary context before scheduling IO.
FileState snapshot;
snapshot.metadata = activeFile.metadata;
snapshot.sourceSets = activeFile.sourceSets;
const auto generation = session.projectGeneration();

// Worker: discover and return bounded preview to the owning UI thread.
auto discovery = discoverLabelCandidates(snapshot.metadata, options);
// UI: handle NotFound/Empty/errors; choose ambiguity explicitly.
// UI: restore one matching saved profile, or ask the mapping dialog.
std::optional<LabelFieldMapping> mapping;
LabelFieldMapping saved;
QString error;
// if a corresponding saved source exists:
// if (restoreLabelFieldMapping(savedSource, saved, error)) mapping = saved;
// otherwise for NeedsMapping:
// LabelMappingDialog dialog(candidate.labelPath, candidate.preview, parent);
// if (dialog.exec() != QDialog::Accepted) return;
// mapping = dialog.mapping();

// Worker: the candidate is re-discovered; all IO and parsing remain off GUI.
auto plan = prepareDiscoveredLabels(candidate, snapshot, generation, mapping, options);
// UI: check the same active file/project; show counts/classCounts/known extents,
// diagnostics, or SourceChanged choices. Then publish allowed dispositions.
// IncompleteEvidence: explicitly confirm raw-source archival, 0 located marks.
auto result = publishDiscoveredLabels(annotationService, activeFile, plan, generation);
```

取消函数应捕获共享的原子 token，不捕获易失效的 QObject 裸指针。切换工程/文件时取消并丢弃旧结果，调用 publish 时传当前 generation，而不是无条件回传旧 generation。解析期间只有新计划，现有 FileState 没有修改；错误、取消和超限不会造成部分发布。

LabelMappingDialog 仅处理有限预览和配置，不直接读文件或解析标签。参数使用现有 `ParameterInputPolicy`、`ParameterIntSpinBox`、`ParameterTextEdit`。Enter、Tab/Shift+Tab、应用内切换焦点提交，窗口失焦不提交草稿，所有参数及下拉禁止滚轮改值。显式“确认映射并预检”提交有效草稿后只返回配置，后续整批预检在 worker 执行；普通 Enter 不触发默认确认按钮。取消或关闭不返回配置。

### AnnotationPanel 手选任意 TXT

`importLabelSource(.txt, ...)` 与 discovery 共用公开的 `hasGeneratorLabelHeader(preview)`。未识别为生成器时返回 `LabelImportState::NeedsMapping` 和明确 error；识别后只走完整 `importGeneratorLabels`，坏 schema 返回 Error，绝不降级为猜测性列解析。

主 Agent 可在 AnnotationPanel 收到 NeedsMapping 后，在 worker 调用下列 API 取得有限预览，再在 GUI 线程显示 LabelMappingDialog：

```cpp
bool previewLabelSource(const QString& path, QString& preview, QString& error,
                        const LabelImportOptions& options = {},
                        qint64 maximumPreviewBytes = 16384,
                        LabelImportState* failureState = nullptr);
```

默认最多读取 16 KiB，硬上限 64 KiB，同时检查文件总大小、UTF-8、取消和截止时间。失败会清空 preview，failureState 可区分 Error / Cancelled / LimitExceeded。它不要求手选标签符合自动发现的同名路径。确认映射后在 worker 调用 `importMappedTextLabels(path, metadata, mapping, generation, options)`；纯类别的 Incomplete 结果由主 Agent 显式确认后用 AnnotationService 发布空标记批次。取消不调用发布。该手选映射来源仍保存同样的 sourceSet 映射 JSON 和原始证据。

## 验证边界

新增行为用例覆盖严格配对及反查、无匹配、陈旧候选、大小写、多候选显式选择、目录/字节/行/记录/深度限额、截止时间、取消、错误 UTF-8、索引及频率转换、帧和秒舍入、整批错误回滚、未知时间 observation、纯类别证据归档/取消/持久化重开/撤销/去重、空 TXT/合法空 SigMF、rfbench 冲突、generation 失效、手工/用户修订保留、来源变化、软删除去重和映射恢复。

首轮主 Agent 集成结果保留在 `out/tasks/wideband-detection/UX-R3/G0/logs/feature-tests-debug-v1.txt`，原始标签输出实际命名为 `out/vs2026-qt611-debug/label-test-results-debug.txt` 及 `.txt.xml`（不是 label-import-test-results-debug）。该轮标签为 18 passed / 2 failed / 0 skipped：一处指纹身份断言失败，一处缺少 mapped-txt-v1 domain 注册。原有生成器、TorchSig、规范化预算和真实资产断言均通过，没有降低它们的要求。原始日志不得以修复后补测覆盖。

用户随后授权本子任务定向构建/测试并实现 IncompleteEvidence。实际执行：

```powershell
./scripts/build.ps1 -Configuration Debug -Action All -Targets SignalStudioLabelImportTests -TestRegex '^signal_studio_label_import$' -Jobs 4
```

结果：Debug 配置/构建成功；CTest 1/1、0 失败、32.22 秒；QtTest **21 passed / 0 failed / 0 skipped**（含初始化/清理）。未修改已通过的原断言；指纹夹具明确提供初始 known-data，随后不同指纹仍必须拒绝，发布前完整比较物理绑定及指纹。原导入器的 `actualTwoSourceAssets` 再次通过：实际生成器 10 个对象/65 活动段、实际 TorchSig 4 个对象。这是指定实际 IQ 资产及标签解析证据，不是新增 RF 精度或未知 DATA/TXT 证据。

构建依赖包含 SignalStudio，本轮也实际完成了 Debug app 重编/部署；配置过程中检测到共享工作区 ui_tests.cpp 变动而重新生成一次 CMake，最终正常完成，没有将该构建描述为只编标签 exe。

归档：

- `out/tasks/wideband-detection/UX-R3/G4/logs/label-tests-debug-v2.txt`：脚本全日志。
- `out/tasks/wideband-detection/UX-R3/G4/logs/label-results-debug-v2.txt`：完整 QtTest 原始输出。
- `out/tasks/wideband-detection/UX-R3/G4/logs/label-results-debug-v2.txt.xml`：原始 XML。

上述为节点作者最初定向结果；主 Agent 集成后的 Release 标签套件为 22/22，完整 CTest 两轮 40/40。工作流 QtTest 覆盖未知映射、取消、字段未指定时拒绝与来源发布；实际 SigMF 导入、自动发现/重复发现、工程保存重开及生成器 WAV 在指定 4K 原生流程中验证。路径别名、大小写和物理范围边界见标签行为用例。用户未知的真实 DATA/TXT 编码与字段规范仍未提供，不能据夹具宣称其生产 profile 或 RF 标签精度通过；该情况必须由用户明确字段/单位后才能导入，未知频率不造框。最终配置、历史失败及证据路径见 VALIDATION。

v2 通过后按主 Agent 的后续要求增加了共享头识别、有界预览 API、手选未知 TXT 的 NeedsMapping 路由，以及 `manualTxtRequiresMappingAndPreviewIsBounded` 用例；这些后续增量等待主 Agent 统一全构建。不得将先前 21/21 作为这一增量已运行的证据。此后源码冻结供统一集成，不再自行启动会重编 app 的构建。
