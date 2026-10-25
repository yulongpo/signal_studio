# 决策

以下决定用于后续实现。本轮没有用测试验证它们。

## 以当前源码纠正审查引用

I-01 不按审查报告修复。`deriveWidebandDetection` 已经调用 `deriveRun()`，并由 `configure(&snapshot)` 把历史运行传给检测卡。`openRunSource()` 不调用 `configure()`。W1 先写 UI 断言。只有断言证明快照没有留在卡片上，才改刷新顺序，不另造一条派生路径。

I-02 不把右侧取消改回 `unfinishedRun()`。当前取消目标是 `focusedUnfinished()`。跨文件第一个未结束任务只用于拒绝新提交、禁用运行，以及“查看正在运行任务”。这个分工保持。

`DetectionCard::configure()` 复制快照时把 `inputAdaptationVersion` 写成 2。本轮不改这个赋值。测试要把它当成现有派生结果，而不是新需求。

## 队列与协议

界面继续在任一未结束任务存在时拒绝新提交。`enqueue()` 的 32 个未结束任务上限和 4096 作业上限不改。`tools/detection_worker/` 协议不改。

## 配置键与模型

`configurationKey` 仍是 `profile.id`，或 `profile.id:` 加 `DetectionSpectrumMode` 整型。`ffscn-gui-iq-v1` 与 `scn-max-psd-v1` 保留模型默认和四种谱。`ffscn-17-experimental-v1` 仍标实验，只保留今天已有项。`pipeline-fixture-v1` 仅在 `enableDetectionTestFixture` 为真时可见。不为后两者新增谱型组合。SCN 的噪底裕量继续禁用。

`detectionProfile` 拆成可见的 `detectionModel` 与 `detectionSpectrumMode`。不保留隐藏旧控件。`palette` 删除，`colormap` 是唯一色表编辑器。

## 谱历史与草稿

方法切换时的合法值只存在对应 `SpectralSettingsWidget` 实例中。不写入 `project.json`，工程版本保持 6。PSD 与 STFT 不共享这份历史。Bartlett 计算重叠仍是 0。

右侧页签切换沿用 `ParameterInputPolicy`：应用内焦点变化提交有效草稿，无效草稿拒绝并保留，窗口失焦不提交。滚轮不改值。

## 屏幕与发布

不安排 1366×768 的窗口、探针、截图或验收。原生验收只认连接序号 1、U27U2D、DISPLAY2 的外接 4K/150%。1920×1080 只作为 offscreen 布局探针，不是原生 DPI 验收。

推送目标只可能是 `origin/rebuild/grok-ux-opt`，而且要等后续验收满足后普通推送。不推 `rebuild/a143-native` 或 `main`。未跟踪的 `.vscode/`、任务包、`reference/`、`resources/sigmf_data_demo/`、`tasks/` 不提交。

历史检测、标记和通道归档不是本轮证据。没有新的运行计数之前，不填写通过数。
