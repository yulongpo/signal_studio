# C3 审查修复交接

- Agent：C3，grok-4.7-build-fast / xhigh，无子代理，无远端写入。
- Worktree：`D:\project\ss-de-rpo-wt\continue-c`
- 基线：`daa30e8599e4ca76dbbb05967338fb1f813290f2`
- 依据：`D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/H_FINAL_REVIEW.md` 的 P0/P1。
- 所有权：`application/session.cpp`、`tests/core_tests.cpp`、本文件。未改 `session.h`。未改 `detection_panel.cpp/.h`、`ss_de_rpo_start_tests.cpp`、`main_window.cpp`，也未改主工作区。
- 本文与代码在同一提交。该提交的 SHA 不能写进自身；交接回复里的 `git rev-parse HEAD` 就是这个提交。

## 修复

拒绝中的 `acceptProjectWrite` 现在挡在序列化通道变更之前。无效参数、不存在的对象，以及已经处于目标导航/视图的调用仍按原返回值结束，并且不咨询守卫。守卫放开后，原有的有效通道几何、快照、DSP 和格式路径不变。

| 入口 | 守卫时机 |
|---|---|
| `activateChannel` | 活动文件、活动通道或窄带工作区标志任一将变化时。三者已一致则直接返回 true。 |
| `activateFile` | 文件将切换，或这次重选会清掉活动通道 / 关闭窄带工作区时。文件不存在仍直接返回 false。 |
| `createAnnotationChannel` | `bindChannelSource` 成功之后、分配 id 和写入 `channels` 之前。创建失败不消耗 id。 |
| `createChannel` / `createChannelFromActiveMark` | 只有旧标记迁移会先改文件时，才在 `attachLegacyAnnotations` 之前再查一次。普通创建由 `createAnnotationChannel` 拦截。 |
| `updateChannel` | 身份、几何和名称校验通过，且 `configVersion` 还能增加时。 |
| `removeChannel` | 通道存在时，擦除之前。 |
| `deleteSelectedMarks(true)` | 所选标记确实挂有通道时，在删除标记之前整次返回。没有关联通道时不新增项目守卫。 |
| `setChannelView` | 原有守卫保留。 |
| `setChannelAmplitudeRange` / `setChannelPsdRange` | 数值确有变化时。 |
| `restoreChannelViewSnapshot` | 钳位后的结果与当前视图不同时。相同则返回 true。 |
| `channelBack` / `channelForward` | 历史非空时，弹出历史之前。 |

未改通道几何公式、快照比较、FIR/DDC 参数或工程 JSON 格式。成功的通道编辑仍不调用 `noteProjectWrite`。

## 复现与测试

H 的界面复现是：两个文件，检测源上已有正式结果，另一个文件上有窄带通道；启动提交尚未走到 `finishStartCommit` 时点击那个通道。`activateChannel` 会改 `activeFileId`，而 `publishPendingStart` 不检查活动文件，随后 `finishStartCommit` 用保存前的字节盖回工程。本代理没有跑这条 UI / 原生 / GPU 路径。对应的会话层行为由 core 测试锁定。

`testProjectWriteGuardBlocksChannelEdits` 使用两个文件、三个通道。守卫拒绝时检查：

- 跨文件 `activateChannel`、同文件切换通道、重开窄带工作区、会清除活动通道的同文件 `activateFile` 均失败。
- `createChannelFromActiveMark`、`createChannel`、`createAnnotationChannel`、有效 `updateChannel`、`removeChannel`、会删除关联通道的 `deleteSelectedMarks(true)`、通道 PSD 范围和通道视图均失败。
- 每次失败后 `ProjectStore::stateToken`、工程代、内容代、活动文件、活动通道、窄带标志、各文件通道数和标记数、通道名称、配置版本、中心频率、带宽、源时段、可见时段、PSD 轴和来源快照指针都与拒绝前一致。
- 当前通道再激活、未知通道/文件、非法改时段、未变化的 PSD/幅度、当前视图还原、空历史后退/前进不调用守卫。

放开守卫后：跨文件激活、回到原通道、只改名称的更新（配置版本 +1，几何和快照指针不变）、从活动标记创建、显式创建、删除其中一个通道、设置 PSD 范围、重新打开工作区，以及同文件 `activateFile` 清除通道导航，均恢复。此前 core 断言全部保留。

首轮失败是测试顺序：释放后的创建把活动通道换成了新通道，重开检查仍要求原通道。产品代码未改。修正断言顺序后重跑通过。

## 命令与结果

工作树内 Debug，`scripts/build.ps1` 选择已有 `local-debug` preset。CMake 因快进后的 `CMakeLists.txt` 比本工作树 stamp 新而重新配置。`SignalStudioCore` 变更使本工作树 `out/vs2026-qt611-debug_bin/SignalStudio.exe` 被重新链接；windeployqt 报告 DLL 已是最新。未改这些源文件，未写主工作区或共享部署目录。

1. `.\scripts\build.ps1 -Action Build -Configuration Debug -Targets SignalStudioCoreTests`  
   退出码 0。`SignalStudioCoreTests.exe` 链接成功。Dxcompiler 警告来自 windeployqt，oneMKL 部署因本工作树缺少 `mkl_rt.3.dll` 跳过。二者都是既有构建输出，不是这次断言。

2. `.\scripts\build.ps1 -Action Test -Configuration Debug -TestRegex '^signal_studio_core$' -VerboseTests`  
   退出码 8。`signal_studio_core` 0.28 s，失败句：`Rejecting guard must block reopening the narrowband workspace`。前面的既有 PASS 已打印，新测试在该句中断。

3. 只修正 `tests/core_tests.cpp` 的活动通道顺序后，重复上述 Build 与 Test。  
   Build 退出码 0，只重编 `core_tests.cpp`。Test 退出码 0：`signal_studio_core` Passed，1.28 s，1/1。输出含原有 8 条 PASS，以及 `PASS project write guard blocks channel navigation and edits`。

未跑集成、UI、原生或 GPU。未做硬件推理。

## 主代理后续

`DetectionPanel::finishStartCommit` 在已发布后仍可能写回 `priorProject`。该修复仍属主代理的 `detection_panel.cpp/.h` 与 `ss_de_rpo_start_tests.cpp`。本修复关掉的是 Session 通道 API 在守卫期间改 `activeFileId` 和其它序列化通道状态。

`MainWindow` 仍有直接写 `session_.project().narrowbandWorkspaceOpen` 的路径。它们不改变 `activeFileId`，因此不会单独触发 H 描述的活动文件回滚；成功的 StartCommit 仍会用 baseline 盖掉这次修改。主代理若要消掉这条 P1，需要改调用方，不要绕过 Session：

- `app/main_window.cpp` 1872–1875：文件菜单“打开宽带工作区”忽略 `activateFile` 的返回值，随后仍把窄带标志写成 false。
- 1777–1783 与 4648–4651：标记双击在 `focusMark` 之后无条件写 false。
- 969–974 与 977–984：窄带回宽带回调和工作模式按钮直接写该标志。
- 1825–1827：`定位来源标记` 忽略 `activateChannel` 的返回值并继续 `locateActiveChannelSource`。后者在 3486 处调用 `activateFile`；守卫拒绝时现在会提前返回，3492 的直接写不会执行。调用方仍应在激活失败时停止。
- 1842–1846 与 1849–1864：重命名和删除不看 `updateChannel` / `removeChannel` 的返回值。删除在拒绝时仍记录“已删除窄带通道”。状态不会改，提示是错的。
- 3467–3474 与 4054–4061：`activateTreeChannel` 和通道对话框只在 Session 调用成功后写标志。守卫拒绝时到不了这些写。
- 5100–5133：`openNarrowbandDemoProject` 在 `newProject` 被拒绝后仍直接改工程名，并向当前文件 `marks.push_back`。这不是通道 API，但是同一提交窗口里的项目写入。

`setPsdFromSelection` 与 `setEffectiveBandwidthHz` 仍直接改序列化文件状态，不调用 `acceptProjectWrite`。它们不是通道导航或通道记录突变，这次没有改。标记选择和 `addMark` 走 `AnnotationService`；检测面板已把该服务的写守卫接到同一个 `acceptProjectWrite`。
