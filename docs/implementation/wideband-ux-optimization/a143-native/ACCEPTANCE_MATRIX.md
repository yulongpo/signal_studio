# 本轮最终验收矩阵

最终源码 `399b85e970f2f22432cd95277dc30e9075ca4cb4`。下列 24 个要求全部通过；W0 的预期失败仍作为修复前证据保留。结果来自本轮最终 Debug/Release，未使用历史 U/R 或 grok 的通过结果填表。

证据简称：`D/S/C/L/P` 分别为 `evidence/ux-{detection,spectral,charts,layout,popovers}-test-results-{debug,release}.txt`；`UI` 为 `evidence/{debug,release}-ui.txt`；`N` 为 [native-debug.json](evidence/native-debug.json) / [native-release.json](evidence/native-release.json)；`兼容` 为 [production-release.json](evidence/production-release.json)。原始用例名见各组日志，完整命令与身份见 [VALIDATION.md](VALIDATION.md)。

| ID | 最终行为与直接证据 | 结果 |
|---|---|---|
| UX-D01 | D：`historicalDerivation`、`topDerivationDiscardsCrossFileDraft`；复制 parent、源、完整参数和冻结范围，不自动运行；跨文件撤销旧草稿 | PASS |
| UX-D02 | D：`viewSourceKeepsDraft`；L：`taskKeyboardAndDetails`；N：可见任务行来源/详情；同文件不覆盖配置草稿 | PASS |
| UX-D03 | D：`crossFileCancellation`、`activeTaskCancellationAndFrozenDisplay`；N：真正 Ctrl+Shift+X 指向选中 runId，Cancelled 前缀保存重开 | PASS |
| UX-D04 | D：`modelSpectrumMapping`，逐项核对原 configurationKey 和完整序列化请求；N：两正式模型 | PASS |
| UX-D05 | D：`foldedParametersAndFirstScreen`，两种离屏布局；N：基础首屏、专业/后处理/资源/计算/诊断展开与运行 | PASS |
| UX-D06 | D：`visibleRangeEntry`；C：`roiConfirmationAndEscape`；N：ROI 精确范围与时间定位独立 | PASS |
| UX-D07 | D：运行中修改显示并核对完整冻结请求及分片字节；N：正式模型运行期间更改 N/色表/动态范围，`displayChangedDuringRun=true`，前缀 SHA 不变 | PASS |
| UX-D08 | D：`preflightDetails`；UI：短文件/前缀流程；缺模型与受控无兼容设备的第一原因/详情见两个 unavailable 报告 | PASS |
| UX-S01 | S：`methodHistory`；Welch→Bartlett→Welch 恢复合法重叠，Bartlett 实算为 0；两配置谱数值 CTest 通过 | PASS |
| UX-S02 | S：`methodHistory`、`conditionalRows`、`invalidDraftRecovery`；Welch/Burg/DPSS、Hann/Kaiser 的首次默认与合法历史；N：各方法表单截图/点击 | PASS |
| UX-S03 | S：`spectralIndependence`、`framePsdPreservesSettings`；UI：宽窄带驻留帧/独立缓存；驻留帧复用 STFT 并保留统计 PSD N | PASS |
| UX-S04 | C：`chartSettingsStayStable`；L：工具栏与页签；N：波形/PSD/检测证据上下文与控件 | PASS |
| UX-S05 | L：`paletteWorkspaceOwnership`、`fileProperties`；UI：全局显示/布局保存；N：色表、电平、自动适配、宽窄带、保存重开 | PASS |
| UX-S06 | S/P：有效提交、非法值保留、Esc/失焦/刷新策略；UI：滚轮不改值（44 个下拉、14 个旋钮）并验证实际容器滚动 | PASS |
| UX-A01 | C：`roiConfirmationAndEscape`；N：实际右键 ROI、Enter 显示检测页、Esc 恢复此前工具，不创建标记/任务 | PASS |
| UX-A02 | UI：坐标方向、大样本精度、框缩放/历史、时频与瀑布选择；兼容：同一活动段的时频/瀑布与证据交互 | PASS |
| UX-A03 | UI：三来源编辑、多段、质量、批选、删除/撤销、快照关系；兼容 43 场景；N：真实 CPU DDC 与 v6 保存重开 | PASS |
| UX-R01 | L：单行、边界/命中、溢出、迟到 runId 弹出列表及刷新；1920×1080 仅离屏；N：57 场景各配置真实 4K/150% 截图与点击检查 | PASS |
| UX-R02 | C/L：布局与编辑器宿主稳定、上下文路由、文件属性、宽窄带色表；UI/N：侧栏/底部/最大化及恢复、切文件、保存重开 | PASS |
| UX-R03 | L：四个行操作和真正按键、不调用 trigger 兜底；D/N：任务状态/runId/取消前缀；文本呈现状态/阶段/进度/结果及异常 | PASS |
| UX-R04 | 两配置 GPU、窄带独立部署及软件回退报告共六项；[缺模型](evidence/missing-models.json) / [受控无兼容 GPU](evidence/no-compatible-gpu.json) 均仍可显示、标记及访问历史 | PASS |
| UX-R05 | 两配置 `signal_studio_annotation_store`：v1–v5 循环导入/升级、原始字节备份、失败原子性、v6 快照；`core` 兼容/拒绝非法输入；UI/兼容/N：v6 来源/通道保存重开 | PASS |
| UX-P01 | P：外部点击/Esc/另一入口/应用失焦/上下文关闭、内部下拉不误关、同一物理入口切换；N：实际 Windows 鼠标与键盘 | PASS |
| UX-P02 | P/C：锚点重定位、稳定不透明宿主、滚动/焦点/草稿不被刷新改变；N：设置层与所属图谱边界、完整控件可点击 | PASS |

独立 UX 为 26 个业务用例，分五组运行产生 36 个 Qt PASS（含每组 init/cleanup 共 10 个）；基础 UI 为 106 个业务用例、108 个 Qt PASS。原生 57/43 是场景数，不能与业务用例数相加。数据准备允许领域接口，用户操作经过实际可见控件。正式模型前向不外推真实 RF 精度；批量 G/T 矩阵仍交用户手动执行。
