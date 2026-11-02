# Signal Studio UX-R5 — Codex 一次性总任务提示词

> 本文件是交付给 Codex 主 Agent 的**执行指令**，不是讨论草案。请在本地 Signal Studio 仓库中一次性完成 R5-0～R5-7，遇到非阻断问题自行设计、实现、测试和修复。所有远端写入受独立审核门禁约束。

## A. 任务身份

- 名称：UX-R5 — Annotation Source Separation & Current Detection Result Replacement。
- 仓库：`https://github.com/yulongpo/signal_studio`。
- 目标远端分支：`rebuild/a143-native`。
- 编制时已核实 HEAD：`5b2a499048d64290851ad02ecf764e17cf678231`；**请执行前重新核对最新 HEAD**。除非是判断分支祖先关系所需，不要反复对同一提交查 SHA。
- 用户原有本地工程及资料绝不能覆盖、重命名或删除；检查 `AGENTS.md` 等本地仓库说明并保持现有开发目录结构。
- 输入文件（全部必须读取）：
  1. `reference/ux-r5/README.md`
  2. `reference/ux-r5/PLAN.md`
  3. `reference/ux-r5/ACCEPTANCE_MATRIX.md`
  4. `reference/ux-r5/INDEPENDENT_REVIEW_GATE.md`
- 用户新增强制需求：**弃用“统一标记记录”混合表，改为“原始标签、人工标记、检测结果”三个 Tab；非人工对象只读；允许源标签/检测结果单项或批量转换为人工标记；算法切换并成功检测后整体替换旧检测结果、不保留旧结果历史。**

## B. 一次性执行原则

1. 不仅输出设计：必须推进至**代码修改、构建、单元/GUI 测试、真实 Qt 原生验收、文档、阶段性本地 commit、独立终审与符合门禁后的远端同步**。
2. 可在 R5-0～R5-6 结束时**本地**提交，持续推进下一阶段。**独立子代理最终 APPROVED 前禁止任何 Git 远端写入**，包括 `push`、远端创建分支/更新 ref、带上传的发布脚本。以下术语按 `APPROVED` 的正常拼写判断，不接受条件性或伪造批准。
3. 不要为每个阶段请求用户确认，不因一般性不确定性停止；自主基于仓库现有结构及可信样本完成最优实现。遇到确实缺失设备或独立子代理能力，保留证据标明 `BLOCKED`，禁止冒称通过或远端已同步。
4. UI 改造优先保持算法、物理坐标语义、持久化证据和工程版本兼容；真实 worker/Qt/CPU DDC 不得用 mock 冒充。
5. 保留此前 UX-R3 `.ssproj` / 标签导入 / 窄带五页以及 UX-R4 详情与复制能力，不重新进行无关重构。

## C. Git 与 SHA 的效率约束（强制）

**用户明确要求不要大量计算 SHA。**

- 开始时只需要一次 `git status -sb`、`git rev-parse HEAD`、合适的 `git fetch origin rebuild/a143-native`、必要的 `merge-base`；远端在执行期间变更时再有针对性同步。不要重复扫描仓库逐个求 hash。
- **禁止**运行大规模 `sha256sum/find ... -exec sha256sum`、为验收重复计算全部源码/模型/检测 chunk/截图/二进制 SHA 或生成多个冗余指纹清单。
- Git 本身的 commit ID、祖先关系检查与产品必要的模型身份、来源文件轻量签名、检测资产完整性校验**不属于禁止范围**，必须保留生产安全机制；优先复用已有 `detection_store` 验证函数，不能降低完整性规则。
- 最终报告只记录必要的 HEAD、阶段 commit、相关日志路径与判定，不逐个列所有文件 SHA。
- `reference/ux-r5/*.md` 应作为用户原始规划跟踪（先 `git check-ignore -v reference/ux-r5/PLAN.md`，只在必要时进行精确 .gitignore 例外）；不要改写用户规划或清理其它 `reference/`、`resources/`、`tasks/` 下未跟踪资料。

## D. 执行顺序与任务清单

### R5-0 | 准备与基线审计

- 在本地确定目标仓库和工作树，查看 `AGENTS.md`、README、当前分支及与远端的安全祖先关系。若有未提交修改，仅在安全可保留的前提下隔离工作；不得强制 reset/clean 用户内容。
- 阅读 `domain/annotation.h`、`domain/project.h`、`application/annotation_service.*`、`application/session.*`、`ui/annotations/annotation_panel.*`、`ui/annotations/annotation_evidence_widget.*`、`ui/charts/plot_widget.*`、`ui/detection/detection_panel.*`、`infrastructure/detection_store.*`、`infrastructure/project_store.*`、`infrastructure/project_file.*`、`app/main_window.*` 及相关测试。
- 明确标注当前源对象是否可以从 `canEdit/reviewBatch/deleteBatch` 路径修改；确认 `publishSourceSet` 当前追加语义、worker 发布与 `discoverDetectionRuns` 恢复路径。
- 输出 `docs/implementation/ux-r5/BASELINE_AUDIT.md`，含关键对象/源码调用链、受影响 UI objectName、旧工程迁移风险、预期不修改的模块边界。发现与任务书不同的源代码事实，以实际仓库为准并在文档中说明。

### R5-1 | 领域与 UI 合同/原型

- 设计三个 Tab：`原始标签`、`人工标记`、`检测结果`，各自独立列、筛选、工具条和操作权限；保留后台任务/日志的可见入口；消除旧“统一标记记录”混合页面。
- 设计服务层 `AnnotationCapabilities`（或等价统一权限矩阵）；阐明只读、来源管理和系统检测替换三种操作权限差异。
- 设计时间未定位 observation、半开样本区间、RF/基带、频率质量、来源自包含转换快照、不重复批量转换及撤销。
- 设计当前正式检测 slot 的唯一作用域 `(FileId, selectedSampleChannel)`、算法更改后失效/更新提示、stage/commit/retire/GC 状态机、run 恢复过滤、0 结果发布，以及 Copy/Reference 资产所有权。
- 输出 `docs/implementation/ux-r5/DESIGN.md` 和 `DATA_LIFECYCLE.md`；建议至少包含 2 张 Mermaid：业务交互、事务状态机。可制作辅助 HTML 原型，但不可用其代替真实 Qt。
- 优先设计一条“更改算法并重新检测”显式操作，而非在调节每个参数时自动重算大型模型。新结果成功时**无论模型是否相同**都替换同作用域的旧结果。

### R5-2 | 真正只读：领域层 + 图谱 + 属性 + 表格

- `AnnotationService::canEdit` 只允许 `sourceKind=Manual` 且源身份仍合法。
- `commitGeometry`、`commitObservationFrequency`、`addSegment`、`deleteSegment`、`splitSegment`、`mergeSegments`、`rename`、`restoreOrigin`、`adoptObservation`、`confirmSegment`、`reviewBatch`、`deleteBatch` 等全部通过集中权限检查；绝不能以普通 API 修改 ImportedLabel/Algorithm。
- 图谱对只读来源**显示框但不显示八方向控点，不允许边界拖动/框体平移**；单击、悬停、定位和查看原始来源证据仍正常。
- 右侧属性对只读来源用非编辑的文本/只读控件呈现物理参数，并显示“转换为人工标记后可编辑”；人工来源的编辑/撤销/来源详情继续可用。
- 标记表格的 context menu、热键、双击、拖动和隐藏命令均按同一能力策略禁用/拒绝，不能只移除可见按钮来“伪修复”。
- “导入新的数据集标签/删除整个标签来源集合”属于来源管理，不等价于直接编辑单个源对象。来源管理只能显式操作并保留审计信息。
- 增加绕 UI 直接调用服务的拒绝测试，证明非 Manual 原本可编辑的漏洞已封闭。

### R5-3 | 三 Tab 与单/批人工转换

- 建立单一 `AnnotationPanel` 三业务 Tab 容器；可以复用底层 ID/分页/排序引擎，但各 Tab 有独立投影列及工具栏。`annotationSourceFilter` 的“全部来源”混合入口不再可见。
- 明确表格列表与图谱层开关不同：隐藏图层不等于从该来源 Tab 删除对象。必要时拆分 `annotationEligibleForTab`、`annotationOverlayVisible`、`annotationSelectable`，并修复原 `annotationVisible` 复用造成的选择不一致。
- 各 Tab 快速过滤、显示总数、筛选数、选择数；转换按钮仅原始标签/检测结果页出现，人工页显示新建/编辑/删除/创建窄带通道。
- 实现 `convertToManual` 或等价**整批事务**：新 Manual `id` / SourceSet / 修订；完整且精确复制合法段与 observation；留下自包含来源摘要，原对象不可变；默认同源同修订去重，用户可确认再次复制；全量失败时一条副本也不能生成；一次 undo 撤销整批。
- 若原算法结果仅有频率观察，必须保持时间未知；人工用户显式选择时间范围之前不得产生伪造的活动段。
- 转换后定位到人工页并选中生成对象；修改人工副本绝不改源对象；源标签重导入、当前算法结果替换不得影响副本。
- `session`/工程树/图谱/右侧属性的选择均使用 ID，不用表格行号；切 Tab 必须清理或交集化异来源选择，防止跨 Tab 批量误操作。选择本身不改变缩放范围。
- 新创建窄带通道仅允许人工对象；原始/算法 Tab 中“创建通道”如需快捷入口，必须显式弹窗确认“转换并创建通道”。

### R5-4 | 最新检测结果替换（核心 P0）

- **不能只在 UI 隐藏旧框，也不能仅在 `FileState::marks` 删除 Algorithm。** 新增正式结果单一 slot 或等价权威索引，并从 `detection_panel::poll` 完整发布入口接入。
- `publishSourceSet` 仍可作为来源原子发布的底层基础，但算法路径必须通过 `replaceCurrentDetection` 或等价两阶段替换事务：
  1. 校验新 run 已 `Completed`、来源有效、覆盖信息完整、结果预算合法、结果 count 可为 0。
  2. staging 新 Annotation SourceSet、对象和相应磁盘 manifest/chunk/head；与正式集合隔离。
  3. 持久层准备好后执行原子切换正式 slot 与项目索引；缓存/选中 ID/右侧详情同步更新。
  4. 标记旧 Algorithm sourceSets / run 为 retired，清除正式可见引用，精确 GC 无引用项目自有文件；不可因后续旧 run 目录扫描而重新恢复。
- 范围整体替换，同文件同 IQ 通道**不保留 SCN、FFSCN、不同参数、不同 ROI 的旧历史框**；检测成功且 0 条也清空旧结果。
- 失败、取消、来源变化、项目切换/迟到 worker 不得替换正式集合或发布半成品。算法/参数变更应显示“结果待更新”，不可继续把旧框冒称新算法结果。为数据安全可暂存内部回滚快照，但它不是可浏览的历史记录，成功提交后必须退出当前项目引用。
- 适配 `detectionRuns`、`discoverDetectionRuns`、`recoverWidebandDetection`、manifest/项目 `Copy/Reference/Preserve` 规则。清理旧资产前必须证明不再被正式项目、在途任务、通道必要快照、外部共享引用使用。
- 对非本工程资产根、用户原始 IQ、MATLAB TXT 标签、SigMF 和模型文件绝不执行删除。
- 接入可控的崩溃点/失败注入，验证 stage、persist、commit、GC 中断后的重开一致性。不得仅依靠 `erase_if`/单独文件删除测试替代真实工程持久化场景。
- 需要新工程字段时优先保证向后兼容；如 schema 不兼容则有明确版本升级、旧版本读入与无损转换方案，**不能默默重新定义 v6 字段**。

### R5-5 | 历史工程、旧来源修订和窄带通道

- 带真实或受控夹具的旧 `.ssproj` v6 能打开、浏览、另存与重开。含多旧 Algorithm runs 时确定唯一有效当前集合；不能猜测有歧义的数据并默删。
- 历史上直接人工编辑过 ImportedLabel/Algorithm 的项目，不能因新只读权限而丢失原来的人工工作；应按受控迁移把已修订状态独立复制成 Manual，原始来源恢复只读；疑难个案展示诊断而非静默变更。
- 旧通道若绑定即将被替换的算法结果，**保留已存 `AnnotationChannelSnapshot`、DDC 输出配置、source time/band、来源文件身份**；可显示“关联检测结果已退役，使用创建快照”，不得把这种情况误写为源 IQ 不存在。
- 工程移动/另存/资产根切换应保持 Reference 安全性；源 IQ/标签文件不在检测 GC 作用域。
- 新业务流：选择原始标签/检测结果 → 明确转换到 Manual → 创建窄带通道 → 保存/重开仍有效。

### R5-6 | 实测/交付

- 编译 Debug、Release，执行受影响 CTest/QtTest；无需为本轮独立功能改变而重复跑无关大型 RFbench 事件验证（用户既有约束沿用）。要检查普通 TorchSig/生成器或已知真实数据集样本、最小有效模型运行或受控真实 publication，不能把合成任务写成模型实际推理结果。
- 覆盖 `ACCEPTANCE_MATRIX.md` 全部 P0：三 Tab、只读各入口、原始/算法转换、100 项批量原子性、同作用域模型 A→B→A、零结果、取消/失败、ROI、来源有效性、旧 run GC、崩溃恢复、通道快照和旧工程迁移。
- 在可用的 Windows 4K 150% 原生 Qt 环境进行真实点选/拖放/批量转换/更新/工程重开截图及 UI 几何检查。真实设备不可用时标 BLOCKED/未验证，不得使用静态 HTML 或 offscreen 图像声称硬件实屏验证。
- 尽可能核查 GPU 和软件两种显示路径，同时保持工作区响应。避免在主线程同步执行大型磁盘 GC 或全表重建。
- 输出 `docs/implementation/ux-r5/{IMPLEMENTATION,MIGRATION,VALIDATION,DELIVERY}.md`；失败、跳过、软硬件限制、真实产物路径必须准确披露。
- 对旧回归中明确与新需求冲突的断言，更新为新产品语义并保留原意的其它测试，不通过删除用例/硬编码常量伪造通过。
- 在用户要求的任务结束前，给出全部阶段 local commit、变更文件概要、复核证据、性能/风险。此时**还不得 push**。

### R5-7 | 真正独立 Review、修复、发布

- 确认所有阶段代码、测试和版本库内文档均已提交本地，固定待审核精确 HEAD。
- 创建并运行**未参与实现或修复**的独立 Review 子代理。交给它 `reference/ux-r5/INDEPENDENT_REVIEW_GATE.md`、验收矩阵、变更和实际测试/原生证据，必须真正复核源文件、API 语义、模型/存储事务、恢复与通道快照；不能接受主 Agent 的自评替代。
- Review 独立在仓库外写 `INDEPENDENT_REVIEW.md`，包含 `Decision: APPROVED/REJECTED/BLOCKED`、精确 `Reviewed-HEAD`、阻断问题与原始证据引用。
- 若 `REJECTED`：主 Agent 修复并本地提交，**再启新的独立审查流程绑定新 HEAD**；旧批准（如有）失效。若 `BLOCKED` 或无法启动独立子代理：停止在本地待审核状态，**不得推送**。
- 只有真正 `APPROVED`、当前 Git HEAD 与 `Reviewed-HEAD` 完全一致、跟踪工作树干净、最新远端目标仍是本地 HEAD 祖先时，才允许非强制同步 `rebuild/a143-native`；远端有新提交应安全整合、补验、**重新独立审核**再推。
- 推送后核对远端目标 HEAD，写仓库外 `PUBLISH_RECORD.json` 记录准确状态、目标分支及审核报告。严禁 `--force`、`--force-with-lease` 和不受审查的其它远端写入。

## E. 验收失败的处理

以下任一情况即不能宣称完成：

- 非 Manual 来源通过表格、图谱、右侧属性、命令或 Service API 还能改变原始/算法标记。
- 批量转换半成功、伪造时间定位，或源证据/坐标语义丢失。
- 新算法成功后旧检测框仍出现在任意可见面、工程重开或恢复列表；成功 0 结果未清空旧框。
- 旧检测资产删掉后工程打不开、人工副本断链、已创建窄带通道丢失、外部 IQ/TXT/模型被删除。
- 原生 UI 实际未运行却以截图/模拟页面伪装通过；出现文字交叠或主要操作不清晰。
- 最终独立审核没有 `APPROVED` 或 Reviewed-HEAD 不匹配而仍进行远端同步。

## F. 结束时必须输出的结果

最终回复请提供：

1. 所对齐的初始 HEAD、最终本地 HEAD、远端目标 HEAD（只要必要 Git 版本信息，不做大量 SHA 运算）。
2. R5-0～R5-7 各阶段完成情况、代码目录、核心交互变化。
3. 关键 CTest/QtTest、真实 Qt 原生截图与恢复测试原始证据路径，以及实际跳过/失败项。
4. `.ssproj`、旧算法来源、人工副本、窄带通道和磁盘资产清理的兼容性结果。
5. 独立 Review 子代理标识、审核结论及 `Reviewed-HEAD`，报告在仓库外的路径。
6. 是否已向目标远端成功 push；若未能审核或同步，清晰标注阻断原因，**不要宣称成功**。

**请现在完整读取输入文件并从 R5-0 开始执行，一次性推进到可以安全交付的最终状态。**
