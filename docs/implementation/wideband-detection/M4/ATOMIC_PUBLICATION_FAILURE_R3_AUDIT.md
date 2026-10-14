# M4 R3 原子落盘失败审计

2026-10-14。结论：**正式 G-no-split 第 7 个文件在已完成 66/159 个模型窗口后，下一批推理前的 manifest checkpoint 发生原子写入失败；底层 I/O 原因尚未确认。旧实现的错误诊断缺陷已由独立隔离测试确认。** 正式失败保留 `Failed`、`pass:false`、原生退出码 1，runner/evaluator 的失败计账正确。不能把修正错误消息或隔离测试通过解释为正式样例已经修复，也不能据此完成 M4 或进入 M5。

本审计只读解析已存在的日志、JSON、chunk/head 字节和源码，并写入本报告；没有启动构建、测试、模型或 GPU，没有修改 canonical 源码、execution freeze、原型或正式结果，没有停止进程或提交。下述新增隔离测试由主实施执行，本审计仅复核其草稿与日志。R3 的完整矩阵仍由主实施持续执行，本报告不评价尚未完成的 scope。

## 正式失败与源码身份

失败目录记为 `N`：

```text
out/tasks/wideband-detection/M4/diagnostics/suite-release-r3/
  G-no-split/20260728-045032-983_bw_20000000_sampleTime_0.8_rollOff_0.35/native
```

该文件使用完整 0.8 s / 20 MHz 请求、`ffscn-gui-iq-v1`、batchSize=2、`timeStrategy:sample`、`splitBursts:false`。`launch.json` 记录启动时间 `2026-10-13T23:17:24.874160+00:00`；`completion.json` 记录 `returnCode:1`、`timedOut:false`、`failed:true`、`reason:result-not-complete`、wallSeconds=29.385143399995286。`result.json` 已写出，其 `pass:false`、`exactRecovery:true`、activitySegments=0。

审计时复核以下六项实际文件 SHA/字节数均与 `execution-freeze-r3-release.json` 相同，故后续控制流分析使用的是冻结版本：

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `infrastructure/detection_store.cpp` | 48407 | `369676110b56df8049b71f386fe62ec0a3359fba8dbcf4dc5e5db6af2bec35ee` |
| `infrastructure/detection_task.cpp` | 53319 | `417494ecb2d70cf1e3b4db5faab6f4d47fd753b3c38b7586a5cacc6a432de6bf` |
| `tests/detection_time_pipeline_tests.cpp` | 21889 | `57d250f01ba764283a041566b8c91c5a2196b959c16757fc93198c984300c371` |
| `tools/detection/time_suite.py` | 42677 | `c8dd975505733a5b4d600a55ac885108c7150a336d604e22468e90c9f6ed05e7` |
| `tools/detection/time_evaluation.py` | 35144 | `f16c0ee852ef8cedc58a5c8ac6c61532d54fe004f9faeaa97d699a5546bc2390` |
| `out/vs2026-qt611-release_bin/SignalStudioDetectionTimePipelineTests.exe` | 770048 | `61db19667a989701109f79bc508998ef35ce92925ec696aaec55edd69b897552` |

execution freeze SHA 为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`；失败 launch 中的 freeze/binary 身份相同。审计结束前再次只读核对 canonical store/task 与 freeze，三者 SHA 保持上述值。

## 失败边界与落盘一致性

`N/process.log` 最后两行是：

```text
phase=reading refined=0/0 elapsedSeconds=27.9288
Cannot atomically publish detection data: Unknown error
```

终态 manifest 与 `result.json.run` 内容完全相同，关键值为 `lifecycle:Failed`、completedWindows=66、eligibleWindows=159、resultCount=66、readFrames=345、`computationPhase:reading`、refinedCandidates=totalRefinementCandidates=0、`sourceValidity:Current`、`recoverable:false`。覆盖范围为 read=[0,6922768)、eligible=[0,6900000)、completed=[0,6700000)、unread=[6700000,16000000)，最后一段的 reason 明确说明没有持久化完成的模型响应。

冻结 `infrastructure/detection_task.cpp` 的顺序是：行 956 设置 reading；行 960–997 读取下一批并更新 read/eligible；行 999 调用 checkpoint；行 1017 才切 inference 并与 worker 交换；行 1049 写 head；行 1083 append chunk。checkpoint（行 778–785）只调用 `writeDetectionRun`，后者在既有请求、chunk、head 与 manifest DTO 校验后调用 `atomicWrite(manifest.json)`（store 行 736–772）。因此，本次正式错误可定位到**第 34 批、windowId 66/67 已读取后的 manifest checkpoint，尚未发送该批推理**。66 是已经完成并持久化的窗口数，不是已确认失败的窗口 ID。

这不是只依赖轮询日志得出的猜测：终态保存的 phase 仍为 reading，readFrames 已从上一批的 335 增至 345，而完成范围与 chunk 仍止于 windowId 65；其它使用同一 `atomicWrite` 的 head/chunk 路径要到 inference 后才执行。现有材料仍不能区分这次 manifest 原子写入内部是 open、write 还是 commit 返回失败。

对 `N/detections/63be7ad13b56006b22f88c512f88943a668b75659aba1c9b439e5ee41334bb4d/` 的实际字节进行了独立只读核验：

| 项目 | 核验结果 |
|---|---|
| manifest | 12735 bytes；与 result.run 相同；终态 `Failed` |
| chunk | 33 个；sequence 连续 0–32；每个 results/mappings 均为 2 项，共 66 窗 |
| 窗口引用 | 连续 windowId 0–65；最后两窗 64/65 的 firstFrameIndex 为 320/325 |
| chunk SHA | 33/33 与 manifest 引用相同；总计 414492 bytes |
| head SHA | 66/66 与 mappings 引用相同；每个 196608 bytes，共 12976128 bytes |
| head 布局 | 全部 `shape:[1,3,1,16384]`，`headOrder:[hm,bw,off]` |
| 目录残留 | 包含隐藏文件在内，未发现 manifest/chunk/head 之外的文件或未引用文件 |
| 前缀恢复 | result 有 3 个频率观察对象、0 个活动段；`exactRecovery:true` |

`finish`（task 行 787–870）在错误后从 durable prefix 恢复观察结果，再写终态 manifest。现有 `Failed` manifest 的写入时间为 `2026-10-13T23:17:53.8385966Z`；最后成功 chunk 的写入时间为 `23:17:52.6069355Z`。终态文件已经落盘，说明该目标随后至少一次原子写入成功；这不证明失败时的旧 manifest 被哪一种系统调用阻塞。目录没有残留临时文件也不能排除曾创建临时文件后被 QSaveFile 清理。

CLI 的 `exactRecovery:true` 仅证明暴露的 durable prefix 与再次恢复结果一致。`tests/detection_time_pipeline_tests.cpp` 行 435–456 仍以 Completed、publication 和 exact recovery 三项共同决定 pass，并在保存 result 后检查 Completed。不能把这三个前缀观察对象当作完成的时间检测结果。

## 诊断缺陷与新增隔离证据

冻结 store 行 400–404 将 `file.open(...) && file.write(...) == ... && file.commit()` 放在 `require` 第一个实参，而第二个实参同时构造 `file.errorString()`。两个实参的求值顺序不能保证错误字符串在文件操作之后读取；`&&` 只保证第一个实参内部的顺序。可确认这是诊断记录缺陷，`Unknown error` 不能直接表示底层 Qt/Windows 没有错误。语言规则见 [C++ draft expr.call 第 8 段](https://eel.is/c++draft/expr.call#8)。正式 R3 未记录操作名、目标路径、Qt error enum、写入字节数或原生失败 API，无法从现存材料恢复丢失的根因。

主实施在独立构建副本中新增 `failedManifestReplacementPreservesDataAndCause`，草稿在：

```text
out/tasks/wideband-detection/M4/deferred/io-diagnostics-r1/
  infrastructure/detection_store.cpp
  tests/detection_store_tests.cpp
```

草稿测试行 231–255 用 `QTemporaryDir` 创建自有 manifest，以 `CreateFileW(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING)` 保留允许验证读取、拒绝替换的句柄。测试在写入返回后释放句柄，并断言失败返回、旧 manifest 字节未变、消息含 commit/manifest.json 且不含 Unknown error，随后同一快照可成功写入。没有读取或锁定正式 R3 文件。

| 隔离证据 | 已复核结果 | 能支持的结论 |
|---|---|---|
| `logs/io-diagnostics-r1-old-implementation-ctest-r2.log` | 旧实现 0/1，失败信息为 `Failed replacement must retain its operation, path and Qt cause: Cannot atomically publish detection data: Unknown error` | 受控拒绝替换时旧实现确实丢失可用的错误原因 |
| `logs/io-diagnostics-r1-fixed-ctest.log` | 修正后 1/1 PASS，单项 0.77 s、总计 0.78 s | 测试断言全部通过，包括旧数据保留、commit/path/非 Unknown 消息与释放锁后成功 |
| 草稿 store 行 402–414 | 分别保存 open/write/commit 结果，再读取 errorString、构造含 operation/path 的消息；directWriteFallback 仍为 false | 消除了这三步操作与错误读取之间的实参求值顺序依赖；未改变失败应返回失败或原子性策略 |

首轮 `logs/io-diagnostics-r1-old-implementation-ctest.log` 只报 `Unknown error`。主实施说明该轮 fixture 误用空 `run.manifestRelativePath` 读取目录；后续改成 `encodeDetectionRun(run)["manifestRelativePath"]`，才得到上表的有效旧实现复现。首轮不是有效的共享锁复现证据，日志保留，不用于确认诊断修正。

固定测试日志没有打印具体 Qt 原因文本或 Windows 错误码；它通过断言证明原因文本已不再是 Unknown error。**隔离测试确认的是诊断缺陷和该拒绝替换场景中的旧数据保留，不能确认正式第 7 个文件当时也被共享句柄锁定。** [Qt 6.11 QSaveFile 文档](https://doc.qt.io/qt-6.11/qsavefile.html)说明该类使用同目录临时文件与提交替换；关闭 direct fallback 保留原子性。该文档为 6.11 系列 API 说明，不能代替本次失败的系统调用轨迹。

## 路径、预算、并发与计账

当前没有确定的路径、资源或数据一致性根因。

失败目录的最长实际已有文件绝对路径为 323 个字符，manifest 路径为 261 个字符。相邻 9 个 G-no-split 成功文件也全部具有相同的 323/261 长度，完成 159/159 窗、159 个 head、81 个 chunk，且各成功终态 manifest 为 25860 bytes，比失败终态更大。失败目标本身随后成功写出 Failed manifest。故“路径超过 260 字符必然失败”与本次证据不一致；临时名称、具体 API、瞬时访问/共享条件等仍需失败时证据，不能仅用路径总长度归因。

| G-no-split 文件时间标识 | 结果 | 完成窗 | head / chunk |
|---|---|---:|---:|
| 044906-196 | Completed / pass=true | 159 | 159 / 81 |
| 044926-842 | Completed / pass=true | 159 | 159 / 81 |
| 044936-468 | Completed / pass=true | 159 | 159 / 81 |
| 044956-845 | Completed / pass=true | 159 | 159 / 81 |
| 045012-147 | Completed / pass=true | 159 | 159 / 81 |
| 045026-193 | Completed / pass=true | 159 | 159 / 81 |
| **045032-983** | **Failed / pass=false** | **66** | **66 / 33** |
| 045046-348 | Completed / pass=true | 159 | 159 / 81 |
| 045101-650 | Completed / pass=true | 159 | 159 / 81 |
| 045110-462 | Completed / pass=true | 159 | 159 / 81 |

同一 045032-983 源文件此前在 G-sample、G-roi、G-coarse 中均为 Completed / pass=true、159/159 窗。这能反驳“该源文件必定在窗口 66 触发模型/解析错误”的直接归因；不能排除运行时环境差异。

失败时已经持久化的 chunk/head 合计 13390620 bytes。即使用全部 chunk 字节作为 mappings 的保守上界，也远低于 134217728 bytes evidence budget；chunk 总量远低于 256 MiB store budget，manifest 远低于 8 MiB 单写上限。错误文本也不是这些预算或路径逃逸检查的文本。现有材料没有显示持久化预算越界、SHA 破坏或缺失引用。

completion 记录 main PID 峰值 RSS=49061888 bytes、whole-device GPU 峰值=1969 MiB；最后资源采样位于 elapsedSeconds=25.423440199985635，早于最后 reading 日志。采样没有完整 worker 进程树内存、句柄数量、磁盘空闲、文件系统 I/O 错误、失败时共享句柄或临时文件轨迹，因此不能据此证实或排除 OS 资源不足、瞬时 I/O/权限变化或共享锁。

`readBounded` 使用函数内局部 QFile，返回 bytes 后文件对象销毁；`writeDetectionRun` 在旧 manifest 读取返回及 chunk/head 校验之后才替换 manifest。未发现这条顺序路径自身把旧 manifest 验证句柄保留到替换的直接证据。suite 行 779–834 先等待 native stage，再运行该文件 reference，目录分别为 native/reference；这条链路没有同时读写同一个 native manifest 的 reference 操作。以上静态边界不能排除其它进程或未记录的外部访问。

scope-summary 显示 G-sample/G-roi/G-coarse 各 10 文件 native/reference failure 均为 0；G-no-split 10 文件的 `nativeFailedRuns=1`、`referenceFailedRuns=0`、`nativeAccounting.failedFiles=1`。该失败文件在 evaluator 中 `predictionCount=0`、truthCount=3，三个 IoU 门槛均为 TP=0 / FP=0 / FN=3，nativeState.failed=true。runner 行 608–621 对 pass=false 或非零退出码计失败；evaluator 行 422–431 将 pass 非 true 的文件转为无预测的失败。原始 3 个前缀观察没有获得预测分。`acceptanceEligible:true` 只说明完整 scope 进入计账，`numericResultsAreAcceptanceClaims:false` 仍然保持；这不是 M4 验收通过。

## 剩余诊断与归档身份

下一步可确认的修正范围是保留操作后的错误原因；主实施已有隔离草稿及定向证据。正式失败的系统原因需要另起隔离诊断：保存 open/write/commit 的实际结果、Qt error enum 与 operation 后 errorString、目标/临时路径和长度、请求/实际写入字节数；若再次出现失败，应同时捕获对应 Windows 失败 API/错误及共享句柄、磁盘与进程资源状态。普通 `GetLastError()` 在其它 Qt 操作之后读取可能已失去对应关系，不能直接当作失败 API 原因。

可用新目录中的相同快照先检查 I/O，无需以模型前向代替原子替换诊断；不能复写原 R3 的输出路径。受控锁定、长路径或资源故障实验各自只能证明对应场景。只有独立复现正式条件并取得真实失败操作/原因后，才能进一步判定底层根因；一次重新成功不足以解释这次历史失败。任何后续正式结果须拥有新的执行身份，并保留 R3 失败计账。

以下 SHA 固定本审计实际读取的材料，路径以 `N` 或 M4 任务目录为基准：

| 材料 | SHA-256 |
|---|---|
| `N/process.log` | `6c583707d9e8b560beaee521a868bbb0045d3500fb157cfd7166469ad5ce11b4` |
| `N/result.json` | `364a3d860e2d3a586d9658f8c99102643add6b363a5b5ec01766a404a2216b96` |
| `N/completion.json` | `da37f5d98543f3e25cc0835df57bf1a62d68f511471554e702c78bc59aa5d836` |
| `N/launch.json` | `efc1971c050430c1dd9605a48b1f1b5224129f6e16fbac5c79bb8bf6031d534b` |
| `N/resources.jsonl` | `183043cf9b7835525673803cdd927285960263bee7fc59824977f77c1973fc0f` |
| `N/detections/63be7ad13b56006b22f88c512f88943a668b75659aba1c9b439e5ee41334bb4d/manifest.json` | `f556c774d003b8be09cda574f8005961bfd78e9e1036b01bc2854299e76b9cf9` |
| `diagnostics/suite-release-r3/G-no-split/scope-summary.json` | `76f16eaae0c494eea8a4d1c80e954d0e19c3f3e94968f1455d4a4dd148e992cb` |
| `diagnostics/suite-release-r3/G-no-split/evaluation.json` | `96df2318a14f8ba8d993796ef8623d05aa79f137d28dc10d5aa03b0096c1ef46` |
| `diagnostics/suite-release-r3/G-no-split/pair-run-records.json` | `ef4a08903b81425d3a1f48d89ec46460496a817f441e39df42db45df03870cc9` |
| `deferred/io-diagnostics-r1/infrastructure/detection_store.cpp` | `5712d90e2b0f44de6a2b899f9ec452e0c24706237f77fbe1d09164ff01b2fc84` |
| `deferred/io-diagnostics-r1/tests/detection_store_tests.cpp` | `c3ed64f29a60a8aaba6390046d994e2a9ba5df2f8fab9be3e2901320aad32a05` |
| `logs/io-diagnostics-r1-old-implementation-ctest-r2.log` | `0f43c77d8e48169174038091b11c5bce3be40960a5e08a0de534812c28da7f12` |
| `logs/io-diagnostics-r1-fixed-ctest.log` | `37bd376823483c8a720ee3cc5f67b9fd1280240fc964148e379a85112653df54` |

审计状态：正式失败边界、已发布前缀一致性、失败计账已核对；诊断记录缺陷在隔离场景中已确认；正式底层 I/O 原因未确认，正式完整矩阵和 M4 完成判断由主实施继续处理。
