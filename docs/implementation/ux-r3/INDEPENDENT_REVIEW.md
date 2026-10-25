# Signal Studio UX-R3 最终独立复审

Decision: APPROVED
Reviewed-HEAD: 1608c4c87dd102fbe21e01e0d83c5e606119d50e

审核日期：2026-10-25（Asia/Shanghai）。审核者：本次独立 Review Agent，未参与原实现及本轮修复。本结论是对上述精确最终提交的明确批准，不是有条件通过。规范报告在最终提交之后生成并作为本地外部交付附件保留，不提交它来改变被审核 HEAD。

最终判断：首次拒绝的 M1/M2/M3 已闭合，两项 Minor 已修正披露和测试文案；本次完整范围内没有发现尚未解决的 Blocker/Major，没有发现缺失的必要验收证据。实际指定屏幕存在，不存在设备缺失阻塞。独立审核门通过；本报告不表示已经推送，发布者仍须执行任务书原有的精确 HEAD、工作树和最新远端安全同步检查。

## 1. 对象、依据及独立性

先核对当前项目 AGENTS.md、完整冻结 REQUIREMENTS.md、DELIVERY.md 和 VALIDATION.md，结合首次全面审核及本轮预审已亲读的实现，复查最终 diff、原始日志、JSON、PNG 与版本身份。没有以主 Agent 自述、计数或预审代替最终审核。

当前分支为 feature/ux-r3-optimization，审核开始和完成时 HEAD 均与本报告匹配，跟踪工作树和索引干净。用户原有 reference/、resources/sigmf_data_demo/、tasks/、.vscode/、旧宽带文档及 zip 等未跟踪内容保留。审核仅写本报告和 out/tasks/wideband-detection/UX-R3/review/ 审查材料，未修改实现、仓库测试、交付文档或已有证据；没有 commit、push、fetch、切换分支或运行发布脚本。

初始基线 36a509d56764457524faa72d9af6ff95429bcb1c 与 90be58e452f17b121028efc8b34af9be50a76e6e 的文件树相同，后者为最终 HEAD 的祖先。这是本地对象关系核验，不冒称审核时已刷新远端。

- 首次规范拒绝报告逐字节保留在 evidence/review-fixes/initial-independent-review-df0ba04.md；SHA-256 为 65b0643538dd089645c0e627b0ada9b06ad2e51d319b2b257e20e66a1f2aac51，与首次报告身份一致。
- 最终实现提交为 bc22bd57458a9c3b0654aa38169dd9ef07c86ce2。bc22bd5..1608c4c 仅为 docs/ 下文档及证据/截图归档，没有新增实现、测试、脚本或构建输入变化。
- 首次全面审核已经实际阅读的未变路径继续纳入本次范围；本轮逐项核查 df0ba04..bc22bd5 的修复和测试差异，并核查其后的最终归档提交，不把本次缩成只看三个修复。
- 检测 worker、存储格式内部校验和原有窄带恢复 DSP 没有被改写来替代真实功能。新增判决为真实快照的显示投影。

## 2. 原 Major 的闭合

### M1：活动 manifest 与持久前缀——已闭合

实际复查 infrastructure/project_file.cpp 的 manifest/verifyManifest/save/load，以及 detection_store.cpp 的 loadDetectionRun、verifyChunks、verifyChunk、verifyHeadEvidence 和 visitDetectionChunks。

活动运行的 manifest 采用明确 detection-live-v1 规则，冻结请求 SHA、保存时 minimumChunks 顺序和内容，以及 minimumCompletedWindows/ReadFrames/ResultCount；不再固定可变索引的瞬时字节 SHA。终态和不可变证据仍采用适当完整性校验。每个工程 run 必须有资产清单锚点，未知 integrity、请求变化、已保存前缀缺失/替换/重排会被拒绝。

保存后新 chunk/head 通过当前 run 完整存储链验证；后续 raw heads 仍检查作用域、形状、长度和 SHA，不因未出现在旧容器静态清单中而豁免。当前引用的 chunk/head 也纳入外层数量/字节预算。非终态读取投影为 Interrupted/recoverable，取消终态可通过旧容器恢复最新合法持久前缀。

独立审核运行的 live-integrity-probe-v3.log 实际证明：Running 保存后合法阶段更新可重开；保存后发布带真实二进制 heads 的新 chunk，无需重存容器即可读取；随后 Cancelling/Cancelled 仍从原容器恢复两个 chunk，容器原字节不变。独立破坏新增 heads 的字节、移走该 heads，均被拒绝且目标内存不被替换；恢复 heads 后同一原容器可再次打开。该测试调用实际存储 API，没有用手改 manifest 冒充 worker 行为。

既有新增 codec 测试还覆盖被冻结请求改变、已保存前缀丢弃、未来 chunk 损坏/缺失、未知规则/缺失锚点和 QProcess 专属发布子进程中断后恢复。两配置完整回归均含 46/46 工程文件行为用例。

真实模型证据另行核对：新 release-detection-native.json 使用实际 FFSCN GUI IQ / TensorRT，savedWhileRunning、postSaveProgressVerified、reopenedAfterCancelWithoutResave、durablePrefixExact 均为 true；保存时 completedWindows=6，后续取消 run 为 12 个 completed windows、6 个持久 chunk，冻结请求及恢复 run 的完整编码在 capture 中比较一致，运行保存的容器 hash 在推进/取消/加载期间保持不变。相关断言先验证无重存重开，再执行末尾常规保存，没有用取消后补存伪造通过。

### M2：Reference 普通保存改变资产根——已闭合

实际复查 AssetSaveMode、ProjectFileCodec::save、MainWindow::saveProject/saveProjectAsDialog 和 DetectionTaskManager::Job 的目录绑定。

普通保存使用 Preserve，从现有容器保留 copy/reference 模式和资产引用，并核对解析根与会话一致；原容器缺失或目标改变时，不无提示迁移已有根。MainWindow 当前路径保存使用 Preserve，不同路径复制明确使用 Copy；活动保护依据资产根是否改变。Reference 另存保持原根，活动 worker 的 Job::directory 因而不被错误分离。

阅读并亲自执行 uxD03_activeTaskCancellationAndFrozenDisplay：实际 QAction → QFileDialog → No 引用另存，Running 时普通 Save 保持 reference/原根且不生成新 .assets，继续增加 chunk，取消后不重存而用另一 MainWindow 打开最新前缀，闲置普通保存仍保持 reference。独立 API probe 同时核对活动和重新打开后的闲置 Reference 保存。两配置完整回归也实际包含该 GUI 用例。

首轮 terminal-b fixture 的 contextRange 不合法导致真实 writeDetectionRun 拒绝，修复是补齐第二源的有效持久历史；失败原件保留，没有删除或放宽该保存安全断言。

### M3：判决观察、参考区域及 I/Q 读数——已闭合

实际复查 domain/constellation_observation.h、NarrowbandWorkspace 的 processingSettings/requestAnalysis/installAnalysis/renderAnalysis、阶段和参考控件，NarrowbandChart 的 reference worker、取消/显示代次、绘制、iqAt 和滚轮入口，以及持久化和新增测试。

判决观察仅消费当前真实恢复符号；要求实际定时/载波锁定、明确模板与有效 RMS。未知假设、未锁定和失败时保留实际可用散点及不可用原因，不制造判决。TaggedSymbol 保留原 output/source 整数与小数采样位置；模板采用完整阶段 RMS 对齐，明确说明用户假设、绝对相位/极性未知、非识别和非位流。来源阶段/显示密度/参考层/缩放不重复运行同步。

参考区域采用真实模板的欧氏最近邻 Voronoi 半平面交集；方格内部四邻居优化，边界及交叉 QAM 缺角点保留完整竞争约束。计算在可取消的显示 worker 执行，按视野/模板缓存并核对显示代次；referencePending 纳入 settled。QPainter 矢量覆盖层与实际 IQ 的 QRhi 顶点绘制分别记录，没有把参考区域计算说成 GPU DSP。

I/Q 坐标由当前正方形 plotRect 及当前数值范围逆映射，滚轮修改等比例范围，离开/取消清除有效读数。新增布尔显示状态有缺省和严格类型校验，旧 v6 页面名称和分析配置兼容保留。

亲自补跑 conditionalDecisionsReferenceAndIqCoordinates（W8），3 个 Qt 行含 init/cleanup 全过。复核四配置各 26 个原生场景、调制族数值测试与 50 个体制/阶数夹具、噪声不出判决、定制 APSK 和独立穷举距离/区域面积断言。独立重算四矩阵的 QPSK 判决位置，误差小于 6e-9；来源采样位置与输出网格关系一致，缩放后 I/Q 逆映射误差为 0。

## 3. 全面范围复查

| 范围 | 实际审查的路径/函数与结论 |
|---|---|
| 工程文件与事务 | infrastructure/project_file.{h,cpp}：save/load/importLegacy/relocate/signature、manifest 和校验；project_store.*：encodeDocument/decodeDocument/stateToken；MainWindow 的 create/open/save/close/confirmProjectTransition/relocateSource。检查 magic/schema/稳定 UUID、原子发布、复制/引用/Preserve、已有目标保护、资产完整性、跨 C/D 绝对引用、重定位长度/格式/Fs/Fc/轻量签名、错误绑定阻断、dirty/取消/失败保全和活动保存。标记分片仍内容寻址，不改写虚拟 state-v6.json；v6 显式只读预览/复制转换，v1–v5 拒绝。 |
| 轴、几何与物理手势 | axis_layout.h 的 measure/sharedBudget/arrange；PlotWidget 的 measuredAxes/axisLayout/freezeAxisLayouts/releaseAxisLayouts/paintScene、plotRect、物理往返与手势。实际字体 ink/advance、标题/刻度独立预算、共享边界、单位/原点、DPI 缓存键和手势期间几何固定成立。GPU 热图/顶点及软件覆盖层共用实际几何；轴滚轮/拖动、ROI、标记控制点/撤销有原始数值证据。 |
| 标签与来源 | label_import.*、label_discovery.*：Inspector::exact/preview、discoverLabelCandidates/discoverDataForLabel、prepare/assess/publish、reusableLabelFieldMapping、importMappedTextLabels；LabelMappingDialog::accept、MainWindow::discoverLabels、AnnotationPanel::importLabels。严格 data/name.data ↔ label/name.txt，大小写/路径别名/多候选显式处理，未知编码/字段不猜；精确路径+IQ binding+唯一解释才复用。uint64、索引起点、秒/帧、舍入、RF/基带一次 Fc、NaN/越界/溢出、整批取消/去重和原文/修订/手工标记保全均有代码及回归。 |
| 窄带共享分析 | domain/project.h、AnalysisIdentity/NarrowbandAnalysisResult、NarrowbandWorkspace、analysis_service/source、constellation/eye/recovery。五页独立，源/配置/工程/请求代次隔离，取消清空和旧结果拒收，源物理修订失效；真实 CPU DDC/FIR/重采样/恢复、阶段快照和独立显示状态保留。M3 投影不修改原恢复算法。识别模型和真实位流明确不可用，演示隔离。 |
| 检测与标记/通道 | DetectionPanel enqueue/poll/recover、DetectionTaskManager enqueue/checkpoint/finish/publication、detection_store、AnnotationPanel/MainWindow。冻结请求、queued 至终态、真正零结果/未读/排除/覆盖不足、历史证据和来源保全；选择与定位分离、质量可选、来源修订、创建快照、2B/倍率/手工 Fs 和真实 DDC 闭环有回归及新真实模型原生证据。 |
| 构建与交付 | CMake/CRT 部署、build/run_ui_test、app/main.cpp 的验收回调生命周期与新帧等待、verify-package/gpu_smoke/screenshot、文件关联脚本、publish-ux-r3。未改 480 s 终止保护；实际 Windows/Qt 6.11.1、独立部署、正数 GPU 顶点/数据 draw 与软件零 GPU 计数、文件关联中文空格路径及注销证据成立。最终代码版本和归档身份见下。 |

## 4. 最终证据与亲自运行记录

以下复验归档相对路径基于 docs/implementation/ux-r3/；审查材料 R 指 out/tasks/wideband-detection/UX-R3/review/final-1608c4c/。

### 原始全量与覆盖

- evidence/review-fixes/debug-ctest-raw.log、release-ctest-raw.log 各有 40 个实际 CTest block，全部有通过记录；完整构建转录 debug-bc22bd5-full.txt、release-bc22bd5-full.txt 给出 Debug **40/40、1589.32 s**，Release **40/40、295.36 s**。没有仅依赖文档计数。
- 独立解析原始 Qt PASS、data tag、Totals，和当前 fixtures inventory 对比：两配置主 UI **106 个唯一业务行**、窄带 **12 个唯一业务行**，无遗漏/额外/重复/skip；窄带明确 W8。Debug 三组耗时 **299.13/417.62/194.75 s**，对应 TIMEOUT 各 **480 s**。新保存 GUI 用例在两份完整原始日志中均实际通过。
- 原完整 Debug/Release 日志中的标签 actualTwoSourceAssets 仍实际验证生成器 10 对象/65 段和 TorchSig 4 对象及来源摘要，不把它们称为未知 MATLAB DATA/TXT 的生产 profile 验证。
- 首轮新 codec 45/46、非法历史 contextRange、Debug capture 占用的 LNK1168、首次各轮全量失败/超时/中断和早期 CLI/Rs/覆盖层失败均保留。定向未设 W8 的记录只作定向；随后完整 W8 才作全量证据。

### 实屏、模型及部署

- 未改轴代码对应旧四配置各 24 场，共 **96**，初审已亲读原始几何/手势和目视 A/B/C；本次核对其代码未变、原件 hash 保留，结合新全量回归，继承该明确范围的实屏证据。
- 新窄带 Debug/Release × GPU/软件各 **26/26，共104**。独立核对 26 个唯一场景完整包含旧 18，新 8 不替换旧覆盖；PNG 源原件均为 3840×2160。硬件非 CPU D3D11、有实际顶点/热图与数据 draw；软件 GPU 计数为 0。
- 新 Release **46/46** 真实检测闭环，包含实际 TensorRT 推理、样本/ROI、复核/修订/来源、CPU DDC 能量/输出 hash、新运行保存推进/取消/无重存恢复及明确标成合成的压力场景。显示 GPU 与模型推理不是同一证明。
- 当前亲自运行 UiCapture --list-screens，记录 R/current-screens.json：Redmi 27 NU、连接 **1**、\\.\DISPLAY6、Windows target device path、首选/实际 **3840×2160**、DPR **1.5**、Qt **2560×1440**；独立全屏真实眼图进程也使用此屏。没有用旧连接序号或超大窗口代替指定屏幕。
- 最终 12 份 deployment JSON 全部核对：Debug/Release 的宽带/五页/真实眼图 × GPU/软件，均为对应 _bin、仅系统 PATH、pass/settled/新覆盖帧及相应 renderer 条件成立。真实眼图仅顶点上传、无热图是正确验收条件；软件 draw/texture/vertex 三项 GPU 计数为 0。五页验收 Rs 为明确记录的 **1 MHz 手工假设**，普通启动保持 Unknown，不冒称识别。
- 本次另外亲自执行 verify-package.ps1，加载已保存 CF32 真实眼图工程，最终 Release GPU 独立进程通过：carrierLocked、**256 条眼图**、**1 次顶点上传/640 数据 draw**、新覆盖帧、3840×2160 和仅系统 PATH 均成立。报告 R/independent-real-eye-gpu.json。
- 文件关联的原实际注册/ShellExecute 中文空格路径/注销证据和脚本未变，载入路径又获新全量及独立部署覆盖；未为审核重新修改用户注册表。
- 本次目视新增 Release GPU 判决参考、软件 I/Q 缩放读数和真实模型取消前缀截图；前次已目视的 A/B/C、恢复星座/眼图、模型不可用与 CPU DDC 图仍保留。截图与代码/数值/实际操作证据共同使用，不只看截图列表。

### 身份、独立补测及审查自身失败记录

- ARCHIVE_MANIFEST 的 **151 项**逐一检查 bytes/SHA-256，零不符；与 df0ba04 的旧 manifest 对比，原 **71 项**身份均保持。初次报告及原始复现已归档，规范旧报告在本次结束时才由此复审报告替换。
- final-binary-identities-bc22bd5.json 的 Debug/Release app、UiCapture、DetectionWorker 共 **6 个**实际二进制 hash/长度一致。亲读最终 configure/build 转录；完整回归之后仅更新 CMake Brand 修订为 bc22bd5 并重建 app/capture/worker，业务源码相同，最终 12 个部署进程重新执行。没有把 Brand 元数据重编称为另一次完整业务回归。
- 本审实际直接运行现有 Release 工程行为测试 **46/46**，日志 R/codec46.log；亲自驱动实际 Reference GUI 保存用例和 W8 条件判决/坐标用例各 **3/3 Qt 行**（含 init/cleanup），位于 R/reference-ui/、R/constellation-ui/。
- 自建小型 API probe 链接现有 Release Core/Infrastructure 库，只在 R/ 内生成夹具；最终 **8 个判别检查通过**，含未来 heads 损坏/缺失拒绝。审查 probe 初次把已有资产夹具初始化误用默认 Preserve，第二次给出不受支持的计算阶段字符串，均被实际 API 正确拒绝；修正审查夹具为显式 Copy 和合法 inference 后 v3 全过。v1/v2 诊断原样保留，不算产品失败或假装首轮通过，未改产品/仓库测试。
- 审查汇总日志：R/archive-regression-identities.log、native-model-deployment-check.log、live-integrity-probe-v3.log。没有机械重跑约 1589 s 全量；原完整日志、关键独立补测和源码边界共同支持结论。

## 5. Minor 处置及遗留风险

问题级别汇总：未解决 Blocker **0**，未解决 Major **0**，需要在本次发布前修复的 Minor **0**。首次 M1/M2/M3 的拒绝只绑定旧提交；本报告是在新精确 HEAD 上确认修复后作出的独立结论。

首次两项 Minor 已闭合：DELIVERY/VALIDATION 公开性能未达值；AXIS_LAYOUT/DELIVERY 等更正完整面积范围；旧 UI 断言文案明确只约束外部分配空间，没有再冒称纯数据高度不减。独立重算的成本仍然存在：

| 布局 | 主图纯数据面积变化 | 短辅助图纯数据面积变化 |
|---|---:|---:|
| A/B | -2.904% 至 -1.708% | -28.56% 至 -27.68% |
| C | -3.116% 至 -1.896% | -17.367% 至 -16.327% |

这是为刻度/标题独立文字带付出的真实成本，不是总面积提升。冻结任务要求 Y 轴占用改善、长文字无损、A/B/C 实测及物理交互保持，没有硬性保证总面积不降；A/B 共享左距实际 40–62 px，C 78–100 px，因此本次认可已公开的布局取舍，不将外部空间测试当内部面积证明。

最新 Release 启动 **172.3765 ms**、取消可见反馈 **67.3124 ms**，本次达到 200 ms 建议值；取消完成 **224.0829 ms**。十万合成活动段筛选/选择 p95 **415.1766/425.5413 ms** 仍未达建议值。首次 367.1684/416.1672/435.4769 ms 未达快照保留；未做独占负载或因果剖析，不称为稳定全配置性能通过。200 ms 是既有建议目标，不添加新硬门禁。

以下边界保留，不构成隐含完成声明：

- 用户未知真实 MATLAB DATA/TXT 编码及字段规范未提供，采用冻结任务允许的严格同名配对、显式映射和未知字段阻断，不猜测生产 profile；TorchSig 仍为受限 SigMF/rfbench profile。
- 轻量 RAW 身份签名不是全文件 hash；生成 IQ 经真实 CPU DDC/恢复不等于现场 RF 精度。条件判决和 RMS 模板不等于识别结果或真实位流。
- 真实识别/解调位流、设备输入、持续采集、Ubuntu、其它 GPU/TensorRT 包仍未部署/验收。当前模型包适配边界保持原交付约束。
- 历史 U3 v5 capture 与当前拒绝 v1–v5 冲突，作为历史未通过项保留，不恢复已取消的兼容范围，不冒称通过。被审批拒绝清理的临时小夹具继续保留。

## 6. 明确结论与交付绑定

本次独立复审批准精确 HEAD **1608c4c87dd102fbe21e01e0d83c5e606119d50e**。任务书允许的降级边界已明确，必要本地回归、指定实屏和独立部署证据齐全，原 Major 已实际闭合；不存在以条件通过绕过未解决问题。

本报告不被提交。后续 POST_REVIEW_SYNC.json 应关联本报告、批准 HEAD 与实际同步结果；本审核没有生成“已同步”记录，也未代替发布者执行最新远端 fetch。任何后续实现/测试或其它提交造成 HEAD 改变，都不能沿用本批准；正常发布仍按既有脚本执行完全匹配、跟踪干净和最新远端祖先检查，禁止强推。
