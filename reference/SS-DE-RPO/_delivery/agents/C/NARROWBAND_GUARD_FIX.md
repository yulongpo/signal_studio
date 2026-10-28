# C5 窄带通道写入守卫交接

- Agent：C5，Grok 4.7 Fast / xhigh，无子代理，无远端写入。
- Worktree：`D:\project\ss-de-rpo-wt\continue-g`
- 分支：`codex/ss-de-rpo-c5`
- 基线：`83e365f4c7b621809a66852e6d7727009a311db3`
- 依据：`D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/agents/results/G3/FINAL.md` 的窄带 P1。G3 未跑测试，本代理只把该报告当作缺陷描述。
- 所有权：`ui/narrowband_workspace.cpp`、`ui/narrowband_workspace.h`、`tests/narrowband_analysis_ui_tests.cpp`、本文件。未改 CMake、`app/`、`main_window`、`application/`、`session`。
- 本文与代码在同一提交。该提交的 SHA 不能写进自身；交接回复里的 `git rev-parse HEAD` 就是这个提交。

## 修复

窄带工作区里所有直接写入序列化 `Channel` 字段的控件、页切换、同步面板和异步回调，都先经过 `Session::acceptProjectWrite`。守卫拒绝时不写字段、不调用 `noteProjectWrite`，并用已提交通道把控件显示恢复回去。守卫放开且字段实际变化时，在同一轮、刷新或请求显示/分析之前调用已有的 `Session::noteProjectWrite`。该通知只增加 `contentEpoch`，不增加 `projectGeneration` 或 `configVersion`，因此不会单独取消正在进行的分析。

值没有变化时不增加内容代。通道指针在守卫返回之后重新取得，回调不跨守卫保存 `Channel*`。复合修改（同步恢复、阶段升降、匹配滤波/载波连带字段）放在一次编辑里，只记一次内容代。

| 入口 | 守卫时机 |
|---|---|
| 波形 / PSD N / STFT N / 频率标签 | 写入前。拒绝后不 `requestDisplay`。 |
| 同步面板展开 | 写入前。拒绝后不改面板、不发 `displayParametersChanged`。 |
| 页按钮与 `activatePage` | 写入 `page` 前。拒绝后不切页。无通道时仍允许只改界面。 |
| 星座阶段、参考、点数、自动缩放、轴范围、滚轮缩放 | 轴范围先校验 `min < max`。缩放回调在守卫前完成数值计算。 |
| 去 DC、功率归一化、眼图阶段/密度/偏移/滚降 | 眼图阶段在副本上 `enableStage` 后一次写回。 |
| 定时/载波/匹配滤波、CFO、环带宽、相位、RRC 跨度、样本预算 | 连带降级在副本上完成。 |
| APSK 环文本 | 先做星座校验。校验失败不咨询守卫。拒绝时清掉草稿并显示已提交文本。 |
| 调制假设、阶数、符号率、符号率模式、采用候选 | 非单载波的“同步恢复”仍只提示，不咨询守卫。单载波恢复整组写入一次。 |
| 眼图分量 / 周期 / 轨迹 | 写入前。 |
| 波形 Y 轴复位 | 直接写 `waveformAutoScale` 之前。已经为 true 时不增加内容代，仍按原路径请求显示。 |
| 波形/PSD Y 轴手势 | 仍走已有 `setChannelAmplitudeRange` / `setChannelPsdRange`。会话拒绝后，图轴显示重新读取的通道值，不用被拒绝的请求值。这里不额外 `noteProjectWrite`。 |

未改模型、DSP 数学和工程 JSON。未改 MainWindow 自己的“自动幅度”处理。未改 Session 的平移/缩放 API。网格和分割条仍是 QSettings。识别模型组合框仍只标记演示结果过期。游标和帧 PSD 仍走 Session 游标状态。

## 复现与测试

G3 的界面顺序是：宽带检测运行且尚未进入提交锁，打开同一文件的窄带通道，正式发布开始时窄带工作区仍打开。组合框和按钮直接改 `Channel`，既不看 `acceptProjectWrite`，也不增加内容代，随后保存快照会盖掉这些修改。

`projectWriteGuardFreezesNarrowbandChannelEdits` 用演示文件和真实控件，不调用与实现同形的辅助函数：

- 守卫拒绝时，实际驱动波形、PSD、STFT、频率、星座页、眼图页、同步面板、眼图分量/周期、星座阶段、去 DC、调制假设、符号率和同步恢复。`ProjectStore::stateToken`、`contentEpoch`、`projectGeneration`、分析代、分析结果指针和通道字段保持拒绝前的值，控件回到已提交显示。
- 未知调制假设下的同步恢复仍只写状态文字“无法同步…”，不改通道。
- 放开守卫后，显示类编辑成功并增加内容代，不增加工程代。星座页沿用原有 `requestDisplay`，演示源会把分析状态写成“来源没有可用的真实通道 IQ。”，分析代不增加。
- 再次拒绝时，已是 QPSK 的同步恢复、PSD、眼图页、同步面板、周期、符号率和去 DC 都不能改通道或状态文字。
- 再次放开后，同步恢复、眼图周期、去 DC、手工符号率和眼图页都写入成功。

首轮失败日志来自修复前的二进制。断言停在 `guardCalls>0`：工作区当时根本不咨询守卫。随后只修正了两处状态文字期望，使它们符合上述既有的“无法同步”提示和星座页分析请求；产品代码那时尚未修改。修复后的定向复跑通过。

## 命令与结果

配置和构建只用 `out/tasks/ss-de-rpo/C5/`。运行库目录是 `out/tasks/ss-de-rpo/C5/cmake-debug_bin`。没有使用 `out/vs2026-qt611-debug` 或 `scripts/build.ps1` 的共享预设。

1. 配置（退出码 0）

```text
cmake -S . -B "out/tasks/ss-de-rpo/C5/local-builds/debug" -G "Visual Studio 18 2026" -A x64 -DSS_QT_ROOT="D:/softwares/Qt/6.11.1/msvc2022_64" -DSS_BUILD_TESTS=ON -DSS_DEPLOY_RUNTIME=ON -DSS_OUTPUT_PREFIX="tasks/ss-de-rpo/C5/cmake"
```

2. 修复前构建（退出码 0）

```text
cmake --build "out/tasks/ss-de-rpo/C5/local-builds/debug" --config Debug --target SignalStudioNarrowbandAnalysisUiTests --parallel
```

3. 第一次启动测试进程退出码 `-1073741515`（`STATUS_DLL_NOT_FOUND`）。`windeployqt` 不会把 `Qt6Testd.dll` 放到应用目录。加上 Qt bin 的 `PATH` 后重跑。该次没有留下 Qt Test 日志。

4. 修复前测试（退出码 1）

```text
$env:PATH = "D:\softwares\Qt\6.11.1\msvc2022_64\bin;" + $env:PATH
$env:QT_QPA_PLATFORM = "offscreen"
$env:QT_SCALE_FACTOR = "1.5"
$env:QT_QPA_FONTDIR = "$env:SystemRoot\Fonts"
& "out/tasks/ss-de-rpo/C5/cmake-debug_bin/SignalStudioNarrowbandAnalysisUiTests.exe" projectWriteGuardFreezesNarrowbandChannelEdits -o "out/tasks/ss-de-rpo/C5/logs/narrowband-guard-ui-before.txt,txt"
```

日志：`out/tasks/ss-de-rpo/C5/logs/narrowband-guard-ui-before.txt`。`FAIL projectWriteGuardFreezesNarrowbandChannelEdits`，`tests/narrowband_analysis_ui_tests.cpp(741)`，`guardCalls>0` 为假。Totals: 2 passed, 1 failed，4092 ms。

5. 修复后按同一构建命令重编，退出码 0。只重编了 `narrowband_workspace.cpp`、测试源和对应 MOC，并重新链接。既有 C4458 警告仍在。Dxcompiler 与 oneMKL 跳过是既有部署输出。

6. 修复后测试（退出码 0），命令同上，日志改为 `out/tasks/ss-de-rpo/C5/logs/narrowband-guard-ui-after.txt`。`PASS projectWriteGuardFreezesNarrowbandChannelEdits`。Totals: 3 passed, 0 failed，3178 ms。

未跑完整 CTest、原生 4K、GPU smoke 或 TensorRT。未推送。
