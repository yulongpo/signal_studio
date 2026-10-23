# W0 基线审计

本文件的源码事实来自 `git show 4904740:<path>` 和切回该提交后的只读核查，不能引用参考分支的已改源码充作基线。起点本地/远端 `rebuild/a143-native` 均为 `4904740`；原工作区处于 `rebuild/grok-ux-opt` / `9d7475d`，切换前跟踪文件干净，原未跟踪输入全部保留。

## 确证风险

- `DetectionPanel` 派生按钮调用 `openRunSource()`；后者激活文件后调用无参 `configure()`，没有向 `DetectionCard::configure(parentRun)` 传递历史快照。本轮 `uxD01_historicalDerivation` 实际失败，父运行未带入。
- `DetectionCard` 取消按钮调用跨全部文件查找的 `unfinishedRun()`。本轮 `uxD03_crossFileCancellation` 在文件 B 上发现取消 A 仍启用，实际失败。
- `SpectralSettingsWidget::updateAvailability()` 在 Bartlett 将重叠写成零，没有方法历史，切回 Welch 不能恢复原重叠。

## 现有界面与状态

主窗口有全局导航、辅助波形/PSD/检测证据、主时频/瀑布；右侧依次是文件、检测、视图、标记、通道、辅助设置、时频设置。主图 `palette` 和右侧 `colormap` 是两个编辑器，绑定同一 DisplaySettings。波形分量、PSD/STFT N、电平等位于长表单；检测物理计算、帮助、预检常驻多行。

参数输入的真源/草稿分工、全局显示偏好、文件级有效带宽、通道级配置及冻结检测请求必须保留。工程原生版本 6，继续导入 v1–v5，来源证据和创建快照不变。全工程已有未结束任务时禁止新提交；任务管理器 32 个未结束任务/4096 作业上限不在本轮修改范围。

## 本次基线实测

Debug 完整 CTest 23/23，UI 108/108，UI 294.64 秒，总 443.89 秒；Release 非 UI 22/22（36.32 秒）与 UI 1/1（59.47 秒）合计覆盖全部23项，Qt 108/108。均零失败/跳过，UI 480 秒保护保持。

Debug 真实 D3D11 演示截图与 Release 基础检测原生23场景通过；后者为流程夹具，不执行正式模型。当前外接屏 Redmi 27 NU / 连接2 / DISPLAY6 / 3840×2160 / DPR1.5。日志、SHA、输入性质和截图见 VALIDATION，不外推真实 RF 或优化后界面。

新复现目标暂不注册正常 CTest，两个业务用例均预期失败；Qt初始化/清理的两个通过不能算业务通过。产品源码仍为4904740，只有W0测试支架及文档新增。参考分支旧错误日志已另存 W0 diagnostics。
