# 基线审计

审计对象是 HEAD `4904740c353b434f7ad824ffc149880b5a0012ce`。下面的调用链来自阅读源码，不是 UI 测试结果。本轮没有构建、CTest、截图、offscreen 探针或原生验收。

## 工作区

`git status -sb` 显示分支 `rebuild/grok-ux-opt`，跟踪文件干净。未跟踪项是 `.vscode/`、`Signal_Studio_Wideband_UX_Codex_Task_Package.zip`、`reference/`、`resources/sigmf_data_demo/`、`tasks/`。这些输入不提交。

2026-10-19 审查报告把 `deriveWidebandDetection` 写成调用 `openRunSource()`，并把 `openRunSource()` 写成激活文件后调用无参 `configure()`。该描述与本 HEAD 不符，不能当作待修复缺陷。

## I-01 派生与打开

`ui/detection/detection_panel.cpp` 中，`deriveWidebandDetection` 的点击调用 `deriveRun(runId)`，不调用 `openRunSource()`。

`deriveRun()` 先按值复制 `DetectionRun`，再 `activateFile()`。源文件不在工程、激活后文件身份不符、或该 `runId` 已不在该文件时，发出消息并返回，不改配置。通过这些检查后，它调用 `configure(&snapshot)`。`DetectionPanel::configure()` 保存父运行并发出 `configurationRequested`。`MainWindow` 把它接到 `openWidebandDetection()`，后者调用 `DetectionCard::configure(parentRun)`。

`DetectionCard::configure()` 在 `parentRun` 为空时直接返回。非空时复制历史请求，写入 `parentRunId`，清空新 `runId`，把 `rangeOrigin` 设为 `custom`，并用历史 `requestedRange` 作为冻结范围。它同时把 `inputAdaptationVersion` 写成 2。这是源码事实，本轮不改。

`openRunSource()` 不调用 `configure()`。它激活源文件、选中列表行，然后发出 `changed` 和 `runFocused`。`runFocused` 只调用 `DetectionCard::focusRun()`，设置 `displayRunId_` 并刷新状态，不复制请求。列表激活、委托按钮“打开源文件任务”、工程树检测运行点击，以及 `viewRunningDetection` 的定位，都走这条路径。

文件切换会进入 `DetectionCard::refreshSource()`。上下文键变化时丢弃草稿、清空 `parentRunId`，再 `loadControls()`。因此“打开源文件”不会把历史请求灌进卡片，但会切换活动文件并丢掉上一上下文的未提交草稿。这个顺序是否在界面上覆盖用户正在看的草稿，UI 测试未运行。

`deriveRun()` 在 `configure(&snapshot)` 之前发出 `changed()`。该刷新与快照应用的先后是否让派生值留在卡片上，同样未跑 UI 测试。W1 先断言，不要预先重写这条路径。

## I-02 取消与排他

`DetectionCard::unfinishedRun()` 仍遍历全部文件，返回第一个未结束任务。它不被右侧取消按钮使用。

`cancelDetectionCardRun` 调用 `focusedUnfinished()`。该函数只在当前活动文件里查找 `displayRunId_`，且该运行必须未结束。其他文件有任务时，取消按钮禁用，提示先定位；`detectionOtherRun` 显示“其他文件有运行任务”，`viewRunningDetection` 只发 `focusRunningRunRequested`，不取消。

底部委托“取消此任务”走 `cancelSelected()`，取消当前选中且未结束的 `runId`。

`DetectionCard::submit()` 在任一 `unfinishedRun()` 存在时拒绝新提交。`DetectionTaskManager::enqueue()` 在未结束任务达到 32 或作业数达到 4096 时拒绝入队。界面排他保持。不改 32 这个上限，不改 worker 协议。上述分支均未跑 UI 测试。

## 谱参数与工程版本

`SpectralSettingsWidget` 没有按方法保存历史。`updateAvailability()` 在 Bartlett 时把重叠的 `lastValidValue` 写成 0。切回 Welch 后，源码不会恢复切换前的合法重叠。PSD 与 STFT 是两个实例，参数互不写入对方。

工程 JSON 当前版本号写为 6（`infrastructure/project_store.cpp`）。谱方法历史不进入工程文件，不升版本。

`ParameterInputPolicy` 在应用内 Tab、Shift+Tab 和点击其他位置时提交。窗口失焦不是该策略的提交事件。数值框在非 Tab 失焦时保留草稿文本，不调用 `commit()`。检测卡无效提交由 `rejectControl()` 拒绝并保留草稿文本。页签切换必须沿用这套规则。本项未跑 UI 测试。

## 未运行

没有本轮构建、CTest、截图、1920×1080 offscreen 探针，也没有外接 4K/150% 原生验收。历史归档不能当作本轮结果。
