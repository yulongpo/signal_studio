# Agent C · 接口请求

C 不改 `app/main_window.cpp`、`DetectionTaskManager`、`detection_result_service` 或 `CMakeLists.txt`。下面三处要由主代理在集成时接上。

## 1. 主窗口：同步拒绝

`requestSubmitted` 发出时卡片闩锁已经打开。`startRequest` 返回 false 时面板没有 `startRejected`（正式结果也还没被隐藏，重入除外）。请把现有连接改成：

```cpp
connect(detectionCard_, &DetectionCard::requestSubmitted, this,
        [this](const DetectionRequest& request) {
          QString error;
          if (!detection_->startRequest(request, error)) {
            detectionCard_->noteStartRejected(request.runId, error);
            return;
          }
          // 仅在 D 的 noteSteadyMark 已进入本编译后取消注释。
          // detection_->tasks().noteSteadyMark(
          //     detection_->acceptedRunId(), "t_accepted",
          //     detection_->acceptedSteadyNs());
        });
```

不要在 `noteStartRejected` 之外再 `log(error)`，否则同一句会进日志两次。`noteStartRejected` 自己会 `emit message`。

## 2. 主窗口：异步拒绝

保存前拒绝、`!job->ok`、generation 变化、以及启动事务 `!published` 都会在 GUI 线程、`releaseCommitLock` 之后发出：

```cpp
void DetectionPanel::startRejected(QString runId, QString message);
```

```cpp
connect(detection_, &DetectionPanel::startRejected, this,
        [this](const QString& runId, const QString& message) {
          detectionCard_->noteStartRejected(runId.toStdString(), message);
        });
```

runId 对不上当前闩锁时 `noteStartRejected` 什么都不做。成功入库的 run 由卡片自己放闩锁，不要对成功再调 `noteStartRejected`。

面板在这些失败路径上仍会 `emit message`。接上之后，面板 message 和卡片 message 会各记一条相同错误。若只要一条，保留卡片这条（预检文案也靠它），面板那条可以以后再收。

## 3. D / 主代理：`noteSteadyMark`

本树没有该方法。C 不调用。契约签名：

```cpp
void noteSteadyMark(const std::string& runId, const char* name,
                    std::uint64_t steadyNanoseconds);
```

主代理在 `startRequest` 返回 true 之后、用上面的 `acceptedRunId()` 与 `acceptedSteadyNs()` 调用 `noteSteadyMark(runId, "t_accepted", ns)`。纳秒是 steady_clock，不是 `QDateTime`。D 未合并前不要写这行，否则本分支无法编译。

`t_click` 在 `DetectionCard::startLatchSteadyNs()`。面板不读卡片。需要点击点时由主窗口在 `requestSubmitted` 里自己读。

## 4. G：R-08 怎么链

不要把 UI 源码链进只含 `SignalStudioInfrastructure` 的逻辑测试。`DetectionCard` 有 `Q_OBJECT`。现成目标 `SignalStudioUiTests` 已编译 `${SS_UI_SOURCES}`（含 `detection_card.cpp` 与 `detection_panel.cpp`），并链接 `SignalStudioCore`、`SignalStudioInfrastructure`、`SignalStudioBrand`、`Qt6::Widgets`、`Qt6::Test`。新文件 `tests/ss_de_rpo_ui_tests.cpp` 请主代理按同一组源和库登记。C 不改 CMake。

R-08 断言：两次 `submit` 一次 `requestSubmitted`；返回前按钮禁用且文案含「准备检测中」。本地校验必须先通过（草稿、来源、配置、范围、方案、quick model）。`submit` 不做 `makeDetectionSourcePlan`。

## 5. 不请求改动的行为

取消在准备窗口仍禁用，因为 task 里还没有这个 run。run 入库后沿用原取消。不要求 D 为了文案去写 `worker-init`；映射已在 `detectionPhaseText`。当前 task 不写这个 phase 时，运行摘要不会出现「初始化 Worker」。准备中的可见句子是按钮上的「准备检测中」。
