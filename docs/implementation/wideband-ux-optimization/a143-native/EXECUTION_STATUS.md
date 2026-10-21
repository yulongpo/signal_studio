# a143-native 宽带 UI 优化执行状态

用户已在 2026-10-21 批准 W0–W6 全部实施、逐阶段本地提交及验收后统一普通推送 origin/rebuild/a143-native。W0 已提交 `0f67455`；W1 实现、自检完成，整理提交。禁止整体合并 grok-ux-opt；不得覆盖既存未跟踪输入。

## 身份与目录

- 开始分支 rebuild/grok-ux-opt，参考提交 9d7475d；已安全切回 rebuild/a143-native，产品源码基线 4904740。
- 当前真实外接屏：Redmi 27 NU / 连接 2 / DISPLAY6，3840×2160、DPR 1.5。执行前实时探测，不按历史设备名称选择。
- 本轮文档在本目录；中间产物 out/tasks/wideband-ux-optimization/a143-native/W0–W6/{logs,diagnostics,assets,local-builds,deferred}。
- 保留旧分支全部未跟踪 docs、.vscode、任务包、reference、resources/sigmf_data_demo、tasks。W0 存档切分支前 Git 状态及 grok UI 原始失败日志。

## 当前完成与运行

- W0：Debug/Release 产品与原测试目标已重建。Debug 完整 23/23，UI 108/108，UI 294.64 秒 / CTest 443.89 秒；Release 非 UI 22/22（36.32 秒）+ UI 1/1（59.47 秒），Qt UI 108/108。零失败、零跳过。
- W0：Debug GPU 原生演示截图通过；Release 官方基础检测旅程 23/23 原生场景通过，fixture 模式、无正式推理声明。
- W0：独立 SignalStudioUxTests 已构建，暂未注册 CTest。uxD01_historicalDerivation / uxD03_crossFileCancellation 均如预期失败，日志 diagnostics/reproduction-before-fix.txt。Qt 2 passed 包含 init/cleanup，业务 0/2。
- W0：文档子任务未产生文件，已停止；主 Agent 根据锁定源码与实际日志完成并审查全部文档。
- W1：移植局部派生/取消逻辑，明确当前视区入口；5个独立UX业务用例通过，Qt总7/7含初始化/清理。已有配置/ROI/夹具重开用例最终分别通过，相关检测存储/协议/任务/core CTest 4/4。
- W2–W6 尚未实施，禁止提前宣称通过。

## 后续顺序

1. 等基线 Debug/Release 全部结果并保存日志；审查 W0 文档，补入实测，提交 W0（含未注册的复现目标）。
2. W1 移植 bed43b2 的局部派生/取消逻辑，统一检测入口并补“当前视区”范围选择；复现转绿后注册独立 UX CTest。
3. W2 新建稳定宿主的集中设置层管理及单行工具栏；至少外部点击/Esc/子下拉/应用失焦/上下文切换/窗口调整都可验证。普通 refresh 禁止 reparent 编辑器。
4. W3 方法缓存只保存合法提交，首次方法默认值；W4 四页上下文与检测卡摘要/详情；W5 真正键盘取消 runId 断言；每节点自检并提交后继续。
5. W6 当前 Debug/Release 完整回归（原 UI 480 秒不变）、新增独立 UX、--wideband-ux 原生捕获、真实 FFSCN/SCN 代表性、CPU DDC/工程重开、GPU/软件/不可用/独立部署。最后文档、证据和普通推送。

## 重要交互决定

- 只有临时 PSD/STFT/电平/显示/溢出设置层自动收起；常驻右栏手动收起。
- 设置层同一时间一个；外部点击提交合法草稿后关闭并继续原点击；子下拉属于内部；Esc 先关子列表，再撤销草稿并关设置层；应用失焦关闭但不提交、保留草稿；文件/通道/工程切换关闭并丢弃草稿。
- 模式/N/分量、唯一色表/电平与关键操作优先，低频项进溢出，保持单行原高度。
- 显式检测/ROI/派生命令一定展示检测页。用户手动页签覆盖自动标记切页，覆盖只在当前上下文有效，不持久化隐藏永久锁。
- 不进行任何 1366×768 工作。1920×1080 离屏不当原生证据；当次唯一 4K/150% 外接屏原生全屏。
- 所有功能操作验收使用可见控件事件；不可用隐藏旧控件或动作 trigger 兜底掩盖失败。
