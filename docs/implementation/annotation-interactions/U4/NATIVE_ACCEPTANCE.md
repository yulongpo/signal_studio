# U4 原生交互与原型对照

本轮复用 `scripts/screenshot.ps1`，没有另建截图或 U4 演示实现。
Debug 模型旅程 43/43，Release 最终旅程 46/46，Release U1/U2/U3
分别 8/8、10/10、12/12，共 119 个场景全部通过。
实际报告和 PNG 位于本目录的 `native/`，运行入口、退出码、耗时及
system-only PATH 条件记录在各报告的 `process` 与
[执行记录](diagnostics/native-stage-ledger.json)。
正式模型工程副本归档到 `projects/debug/` 和 `projects/release/`，以适配
Windows 路径长度；[目录映射](diagnostics/project-archive-map.json)记录原路径，
文件字节及工程内部相对引用保持不变。

当前屏幕为用户更换后的 U27U2D，连接序号 1，原生设备路径
`\\.\DISPLAY2`；实际 3840×2160、150% DPI、2560×1440 逻辑全屏。
每场景检查窗口所在屏、DPR 和截图尺寸；表单是同屏实际 DPI 裁剪。
旧节点的 Redmi 27 NU / DISPLAY6 记录保留原值。

## 获批 U0 的逐项闭环

本表依据 [U0 对 M0-P 的修订](../U0/M0P_DELTA.md)，对照具体行为、
直接断言和实际画面，不以浏览器演示替代 Qt 验证。主窗口保持工程树、
导航、辅助图、主图、右侧参数和底部记录的布局。原 M0-P 22 项文件
及 U0 原型冻结核对通过；不改写旧报告的汇总字段。

| 获批行为 | 本轮直接检查及原生画面 |
|---|---|
| 三来源、未知时间与来源色 | [U1-01](native/native-release-u1-r1/01-three-sources-unknown-time.png)：实际 SigMF 标签及明确的交互夹具；未知对象使用观察带，不伪造活动段 |
| 同一对象多段、当前段控制点 | [U1-02](native/native-release-u1-r1/02-current-segment-controls.png)：当前段与同对象其它段分别强调，表格显示段序号和当前段几何 |
| 图、表、树选择及悬停联动 | [U1-03](native/native-release-u1-r1/03-table-tree-plot-selection.png)、[U1-04](native/native-release-u1-r1/04-linked-hover-preserves-spectrum.png)：当前对象、其它已选和悬停分开；悬停不改变选择或重算谱 |
| 超过 100 条、跨页全选、过滤 | [U1-05](native/native-release-u1-r1/05-graph-loads-far-tree-and-page.png)、[U1-06](native/native-release-u1-r1/06-matching-selection-across-pages.png)、[U1-07](native/native-release-u1-r1/07-closed-layer-prunes-selection.png)：按 ID 加载树并跳页，全选全部匹配，关层剔除选择 |
| 三来源直接图形及表格编辑 | [U2-01](native/native-release-u2-r1/01-direct-imported-graph-edit.png)、[U2-02](native/native-release-u2-r1/02-table-f2-algorithm-edit.png)：同 ID 自动修订、原始来源和证据不变 |
| 原子范围草稿、Delete、撤销 | [U2-03](native/native-release-u2-r1/03-name-draft-delete-edits-text.png)、[U2-04](native/native-release-u2-r1/04-atomic-range-draft.png)、[U2-06](native/native-release-u2-r1/06-current-segment-single-command-undo.png)：输入 Delete 只编辑文字，多字段一次校验提交，一次命令撤销 |
| 未定位时间只改频率 | [U2-05](native/native-release-u2-r1/05-unknown-time-frequency-only.png)：时间保持未知，不因编辑频带产生算法活动段 |
| 批量删除预览与保留通道 | [U2-07](native/native-release-u2-r1/07-batch-delete-preview.png)、[U2-08](native/native-release-u2-r1/08-source-deleted-channel-snapshot-kept.png)、[U2-09](native/native-release-u2-r1/09-undo-keeps-later-background-results.png)：2 对象、4 段、1 通道预览；删除只影响当前筛选，撤销保留之后发布的对象 |
| 可选质量、明确当前段提取 | [U3-02](native/native-release-u3-r1/02-optional-quality-menu.png)、[U3-03](native/native-release-u3-r1/03-pending-located-channel-trim-form.png)：待判断对象可直接绑定第 2 段并裁剪 11–13 ms，不要求接受或段确认 |
| 已忽略、时间未定位的 UserRange | [U3-05](native/native-release-u3-r1/05-ignored-unknown-user-range-form.png)、[U3-06](native/native-release-u3-r1/06-unknown-user-range-real-ddc-narrowband.png)：人工 4–8 ms，真实 DDC，不写回算法活动段 |
| 空视区仍打开配置、显式可用前缀 | [U3-07](native/native-release-u3-r1/07-empty-visible-range-requires-explicit-prefix.png)、[U3-08](native/native-release-u3-r1/08-partial-prefix-tail-real-cpu-ddc.png)：具体 sourceTime 原因，按钮禁用；用户显式使用前缀后才扩大范围并执行 |
| 来源诊断、多选及快照关系 | [U3-09](native/native-release-u3-r1/09-physical-source-diagnostic.png)、[U3-10](native/native-release-u3-r1/10-multiple-selection-explicit-one-source.png)、[U3-11](native/native-release-u3-r1/11-bound-geometry-relations-and-snapshot.png)：一次一个来源；文字/质量不使通道失效，绑定变化/删除/撤销分别更新关系 |
| v6 重开及旧版备份 | [U3-01](native/native-release-u3-r1/01-v5-upgrade-preserves-backup-and-channel.png)、[U3-12](native/native-release-u3-r1/12-v6-reopen-preserves-user-range-and-real-ddc.png)：原始字节备份、旧通道快照不重算，新人工范围和 DDC 哈希重开一致 |

主 Agent 已直接查看本轮 U1-03、U2-04/U2-07、U3-07，以及 Debug
模型 DDC 画面，并与 U0 的选择、范围编辑、删除及 UserRange 画面核对。
表中其它场景通过真实 Qt 控件操作、直接状态断言及同屏截图记录。
这不是逐像素图像相似度测试，也不将浏览器合成纹理当作 IQ 图谱。

## 真实模型结果到 CPU DDC

两配置均实际运行 FFSCN Sample、ROI 各 3 个窗口，生成统一算法对象。
待判断、未确认活动段直接进入配置，生成 16384 个有限、非零能量
DDC 输出，保存重开后快照保留；改变绑定几何显示差异且不重算。
[Release 模型结果](native/native-release-r1/24-production-model-time-refinement.png)、
[可选质量](native/native-release-r1/25-real-algorithm-quality-is-optional.png)、
[通道参数](native/native-release-r1/26-real-algorithm-channel-parameters.png)、
[真实 DDC](native/native-release-r1/27-real-algorithm-cpu-ddc.png)、
[重开](native/native-release-r1/28-real-algorithm-ddc-saved-reopened.png)
与[完整报告](native/native-release-r1/wideband-detection-report.json)分别保存。

Release U3 的三个交互对象本身是 UI 夹具，但读取的 IQ 和 CPU DDC
是真实文件处理。输出为 8000、8192、8192 个样本，SHA-256 与 U3
节点相同；未知对象依旧没有 segmentId 或算法时间段。实际原始标签
来自 meta，手动/算法夹具不能声称模型定位效果。

## 边界与保留事实

空视区/Partial 原生画面采用受控前缀；真实停止加载及首个未读样本
边界由处理器和 Qt 回归单独验证。上下文菜单采集投递公开 Qt 鼠标
上下文事件，再点击真实菜单，不调用业务槽替代；不声明 OS 物理
输入设备测试。v5 旧 W: 来源只验证迁移，新的本机样本另建 v6 工程。

原报告 `a16AcceptanceVerified`、`prototypeComparisonVerified` 的旧汇总
值仍为 false，捕获器没有汇总全部节点；本轮以本表、逐场景断言、
实际画面及冻结身份形成对照，不修改原始 JSON 冒充自动综合结论。
性能和模型效果边界见[验证记录](VALIDATION.md)。
