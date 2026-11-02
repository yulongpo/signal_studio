# UX-R5 强制验收矩阵

**属性：**可执行验收清单。所有结论须绑定实现时最终提交及实际日志/截图；“历史通过”“静态扫描通过”“原型有按钮”均不能冒充本轮实际功能通过。

## A. 三 Tab / 选择 / 只读（P0）

| ID | 场景 | 必须断言 | 证据建议 |
|---|---|---|---|
| UI-01 | 打开宽带工作区底部记录 | 有 `原始标签`、`人工标记`、`检测结果` 三个可点击 Tab；没有旧统一混合表业务入口；后台任务/日志仍可访问 | Qt widget tree / 截图 / 实际点击 |
| UI-02 | 多来源同时存在 | 各 Tab 只呈现所属来源，行数与业务对象一致；重名对象无串页 | QtTest & 原生截图 |
| UI-03 | 关闭原始标签图谱叠加 | 原始标签在 Tab 仍可浏览/选择/定位（可提示图层隐藏），不能因 overlay 状态消失 | QtTest / 原生 |
| UI-04 | 当前页多选、筛选全集选择 | `Ctrl/Shift`、全选、本页/全筛选标记 ID 准确；不错误选择其它 Tab 的对象 | QtTest |
| UI-05 | 工程树→表格→图谱→属性 | 一致 `activeMarkId`/segmentId；Tab 自动切换或明确高亮来源；普通选择不缩放，显式定位才移动视图 | 原生录屏/截图与业务 ID 日志 |
| UI-06 | 长名称、宽度变化、4K 150% | 无遮挡、文字交叠、无法点击；焦点、禁用、悬停、只读状态易于辨认 | 真实 Qt 4K 原生（若可用） |
| RD-01 | ImportedLabel 图上拖动/缩放 | 不能出现可编辑控制柄，鼠标、菜单、键盘均不得修改任何范围 | QtTest + 领域状态前后对比 |
| RD-02 | Algorithm 图上拖动/缩放 | 同 RD-01，但仍可查看与定位 | QtTest + 领域状态前后对比 |
| RD-03 | 原始/算法通过全部 service 写 API | `commitGeometry/ObservationFrequency`、`rename`、`add/delete/split/mergeSegment`、`adoptObservation`、`restoreOrigin`、`confirmSegment`、`reviewBatch`、`deleteBatch` 逐类拒绝，不产生修订 | C++ unit |
| RD-04 | Manual 仍可编辑 | 几何与段管理、命名、备注、删除、undo/redo 行为正常 | C++ unit + Qt 原生 |
| RD-05 | 来源集合管理 | 标签重新导入/重新绑定为“来源管理”，不成为绕过单标记只读的暗门 | C++ unit / UI |

## B. 单个/批量转换（P0）

| ID | 场景 | 必须断言 | 证据建议 |
|---|---|---|---|
| CV-01 | 原始单条转换 | 原对象字节语义/几何/证据不变；生成独立 Manual ID、来源集合与修订 | C++ unit + Qt 原生 |
| CV-02 | 算法单条转换 | 生成独立 Manual 副本；后续算法更新删除原结果后人工仍可编辑/保存/重开 | C++/store/Qt |
| CV-03 | 10/100 条批量转换 | 一次原子提交，整批成功或全失败；一次 undo/redo 一致 | unit + 保存重开 |
| CV-04 | 全部段 / 当前段 | 默认保留全部有效段；当前段选项仅复制所选段；时间半开区间、原坐标系和质量正确 | unit |
| CV-05 | 时间未定位观察 | 不得从 observation 推造活动时间；用户后续显式提供人工时窗方能形成段 | unit + UI |
| CV-06 | 无效来源、通道错位、坐标越界 | 批量完全拒绝，无孤立来源集合/半成品对象 | unit |
| CV-07 | 重复转换 | 默认识别同源同修订的既有副本，显式再次复制才允许新副本 | unit + UI |
| CV-08 | 删除旧 run/标签源文件后 | 人工副本几何、名称、类别、必要来源快照与保存恢复不依赖被删原框 | unit + .ssproj 重开 |

## C. 当前检测结果替换（P0 / 数据完整性门禁）

| ID | 场景 | 必须断言 | 证据建议 |
|---|---|---|---|
| RP-01 | 首次模型检测成功 | 仅产生一套正式结果；在来源正确的 Tab 与图中可见 | service + Qt |
| RP-02 | SCN→FFSCN→SCN 各成功 | 当前文件和样本通道始终至多一套正式 Algorithm 结果；旧运行不能从任务恢复重新出现 | service/store + Qt |
| RP-03 | 新检测成功且 0 个结果 | 旧检测框全部移除；UI 显示“完成但未检出”，覆盖区域语义正确 | service + Qt |
| RP-04 | 检测失败 / 取消 | 没有半成品标记正式发布；若保留上次有效数据只用于恢复，不误当当前配置结果 | service/store + UI |
| RP-05 | 检测运行中 | pending/preview 不进入正式结果，不允许将未发布结果转换为人工 | service/Qt |
| RP-06 | 同文件不同 IQ 样本通道 | 两通道正式结果隔离；替换 A 不影响 B | service/store |
| RP-07 | 两个独立文件 | 文件 A 更新不改变 B 的检测、原始及人工标记 | service/store |
| RP-08 | 全文件运行→ROI 成功 | ROI 范围外的旧框不残留；外部标记为未检测而不是阴性 | service/Qt |
| RP-09 | 来源改变 / 后到 worker | 旧 generation、文件、通道、来源身份的迟到发布被拒绝 | service |
| RP-10 | 保存/重开/历史扫描 | 已退役 run/SourceSet/Algorithm 结果不可复活、不可被导出、不可进入工程树 | codec/store + Qt |
| RP-11 | 断点注入：stage | 磁盘失败不损坏上次正式集合；无混杂 | fault injection |
| RP-12 | 断点注入：persist | 同上；孤儿临时数据可安全清理 | fault injection |
| RP-13 | 断点注入：commit | 事务结果为旧完整集合或新完整集合，不能部分成功 | fault injection |
| RP-14 | 断点注入：GC | 新正式结果可重开，旧临时垃圾待后续回收但不能复活 | fault injection |
| RP-15 | 检测内存/磁盘预算 | 资源超限提前失败且不损坏旧正式集合；无主线程长时间阻塞 | stress / RSS / time |
| RP-16 | 清理资产 | 仅删除本项目自有且无引用的旧分片/manifest；不删外部 IQ/TXT/模型/Reference 资产 | isolated temp filesystem tests |

## D. 兼容 / 通道 / 工程（P0）

| ID | 场景 | 必须断言 | 证据建议 |
|---|---|---|---|
| CP-01 | 现有 v6 `.ssproj` 有多个旧 Algorithm run | 明确选择/迁移唯一正式结果，原文件无损、保存重开一致 | versioned fixture |
| CP-02 | 旧项目直接编辑过 ImportedLabel | 不丢历史修改；按计划形成 Manual 副本并恢复只读原始状态，或明确待确认诊断 | migration fixture |
| CP-03 | 旧项目直接编辑过 Algorithm | 同 CP-02；不把用户修订静默删除 | migration fixture |
| CP-04 | 旧通道直接绑定旧 Algorithm | 通道和创建快照完整，DSP 参数不变，来源退役状态准确；重开不会被静默重绑 | codec + CPU DDC smoke |
| CP-05 | 新通道来自 Manual | 绑定正确，时间/频率/采样率合同不变 | Qt+CPU DDC |
| CP-06 | `Copy`/`Reference`/`Preserve` 保存或另存 | 删除仅对项目自有资产有效；Reference 外部文件全部保留 | codec/storage fixture |
| CP-07 | 原始标签重新导入后手工副本 | 原始副本与人工对象相互独立；原始导入保持来源去重 | label + service |
| CP-08 | 一份工程多信号文件 | 全部可访问，Tab 对当前文件及 IQ 通道正确 | UI + codec |

## E. 回归、真实 Qt 与发布（P0）

| ID | 场景 | 必须断言 | 证据建议 |
|---|---|---|---|
| QA-01 | Debug / Release | 影响模块均编译通过，相关 CTest 真实运行；旧受影响业务用例适配新产品语义 | build log + CTest raw |
| QA-02 | 原生 Qt / 渲染路径 | 可用硬件上实际点击、选取、只读、转换、检测替换、保存重开及截图；不是 offscreen 冒充 | native screenshot/capture |
| QA-03 | 标签真实来源 | 可信的已有 MATLAB DATA/TXT 关联样例或已知生成器、TorchSig 小样本，绝不伪称未提供的数据格式已实测 | real fixture listing |
| QA-04 | 未完成项 | 错误、跳过、阻塞、硬件不可用均写明，不能删除失败记录 | VALIDATION.md |
| QA-05 | SHA 使用 | 没有批量全仓指纹；产品内部必要安全校验仍存在且通过 | command log / diff |
| QA-06 | 独立终审 | 非实现 Agent 复查最终精确 HEAD 和原始证据，明确 APPROVED | 外部 review report |
| QA-07 | 远端同步 | 仅在 QA-06 APPROVED，且远端无冲突、HEAD 一致时非强制推送；否则停止本地 | git log / publish record |

## 最小阻断级门禁

- 任意非 Manual 对象可被任一入口编辑：**REJECTED**。
- 批量转换产生部分副本、伪造时间定位或破坏来源：**REJECTED**。
- 成功新检测后旧结果仍能在工程树/表格/图谱/任务恢复出现：**REJECTED**。
- 新检测失败、崩溃或 GC 导致 `.ssproj` 损坏、通道消失、外部数据被删除：**REJECTED**。
- 只有原型截图、无实际程序操作证据，却宣称 UI 已验收：**REJECTED/BLOCKED**。
- 缺少真正独立 Review 子代理批准或 Reviewed-HEAD 不一致：**禁止 push**。
