# 宽带交互优化计划

本轮只锁定文档。未改源码，未跑测试，未提交，未推送。

## 基线

仓库 `D:\project\signal_studio_rebuild`，分支 `rebuild/grok-ux-opt`，HEAD `4904740c353b434f7ad824ffc149880b5a0012ce`。该提交与审查基线一致。2026-10-19 审查报告对 I-01、I-02 的调用链描述与这个 HEAD 的源码不符。后续实施以源码为准，不要按审查报告重写一条已经存在的路径。

未跟踪输入保持不提交：`.vscode/`、`Signal_Studio_Wideband_UX_Codex_Task_Package.zip`、`reference/`、`resources/sigmf_data_demo/`、`tasks/`。

## 本轮产物

同目录七份文档是后续编码的验收依据。`VALIDATION.md`、`DELIVERY.md`、截图目录本轮不建，因为没有新的运行结果。

## 后续顺序

1. W1 先为 `UX-D01`、`UX-D02`、`UX-D03` 写 Qt UI 断言，再决定是否改代码。源码里 `deriveWidebandDetection` 已经走 `deriveRun()`，右侧取消已经走 `focusedUnfinished()`。断言失败才改对应调用，不要按过时审查重做。
2. W2 把波形、PSD N、统计范围、STFT N、色表、电平和网格放到所属图谱工具栏。删除重复的 `palette` 下拉，`colormap` 是唯一色表编辑器。
3. W3 在 `SpectralSettingsWidget` 实例内保存方法专属合法值。不升工程版本。Bartlett 仍按实算零重叠，但切回 Welch 时恢复该实例里上次合法重叠。
4. W4 用四个右侧页签替换长滚动分段，拆开 `detectionProfile`。不保留隐藏旧控件。
5. W5 让任务行可以用键盘查看和取消指定 `runId`。不改队列上限，不改 worker 协议。
6. W6 才跑构建、CTest、offscreen 布局探针和原生验收。没有实测计数之前，文档不得填写通过数。

每步只改本步范围。DSP、检测请求语义、工程 v6 存储契约和 `tools/detection_worker/` 协议不动。

## 不做

不改 FFSCN/SCN 网络、权重、阈值含义和模型资产。不升级 TensorRT，不重导出 engine。不为 `ffscn-17-experimental-v1` 或 `pipeline-fixture-v1` 新增今天不存在的谱型组合。不把夹具结果写成真实推理。不安排 1366×768 的窗口、探针、截图或验收。

## 发布

只有后续验收满足才普通推送 `origin/rebuild/grok-ux-opt`。不推 `rebuild/a143-native` 或 `main`，不用 `--force`。当前未推送。
