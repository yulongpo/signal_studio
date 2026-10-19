# M4 io-diagnostics-r1 首轮草稿规格审查

结论：**DRAFT_SPEC_FAIL_BOUNDARY（首轮两文件快照，1 项 P2）**。分步读取 operation/path/Qt errorString 的修正及受控 Windows 锁定 fixture 符合常规失败诊断/原子发布要求；但新完整绝对路径消息无界，能够越过 `run.error` 的 4096 UTF-8 bytes 契约。应用前应补有界错误摘要。本报告不确认正式 R3 的共享锁根因，也不把隔离检查通过当作正式样例已修复。

2026-10-14（Asia/Shanghai）。审查仅只读首轮草稿、clone、既有日志、`ATOMIC_PUBLICATION_FAILURE_R3_AUDIT.md` 和契约控制流，仅新增本文件；未构建、运行测试/模型/GPU、修改 canonical/冻结/原型、提交或进入 M5。报告期间主实施已决定继续准备 store/task 及两份测试共四文件的预算修正草稿；该后续修正尚未提供执行身份，本报告**保留首轮发现，不提前给新版本 PASS**。

## 首轮受检身份

草稿根为 `out/tasks/wideband-detection/M4/deferred/io-diagnostics-r1/`，执行 clone 为 `out/tasks/wideband-detection/M4/local-builds/evidence-r4/source/`。只读时两文件在 deferred/clone 中与 `diagnostics/io-diagnostics-r1-identity.json` 逐字节 SHA 相符：

| 文件 | 首轮 SHA-256 |
|---|---|
| `infrastructure/detection_store.cpp` | `5712d90e2b0f44de6a2b899f9ec452e0c24706237f77fbe1d09164ff01b2fc84` |
| `tests/detection_store_tests.cpp` | `c3ed64f29a60a8aaba6390046d994e2a9ba5df2f8fab9be3e2901320aad32a05` |

下述 store/test 行号指这份首轮快照。后续改动不能沿用本报告的源码或执行身份。

## 已符合的实现与行为

首轮 `atomicWrite:392–415` 的路径内含检查、8 MiB 单写预算、`QSaveFile.setDirectWriteFallback(false)` 和原子 commit 保持；没有重试、写原文件回退、错误转成功或算法/生命周期变更。open、write、commit 分别完成后才构造对应错误，记录实际 operation、实际目标 path 及该步骤后的 Qt errorString。失败通过原 require/catch 返回 false；write 短写不进入 commit，commit 失败不被忽略。

`writeDetectionRun` 仍核对 immutable request、生命周期、已发布 chunk 前缀、chunk/head 字节与 DTO，然后原子替换 manifest；错误不能越过校验部分发布。此处改变的是诊断顺序和内容，不改变原子策略。测试释放自有锁之后再次显式写入同快照，不是产品自动重试策略。

新 Windows fixture `failedManifestReplacementPreservesDataAndCause:231–255` 从 `encodeDetectionRun(run)["manifestRelativePath"]` 定位自有 `QTemporaryDir` manifest。`CreateFileW(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING)` 保留验证读取、拒绝替换的句柄；调用返回后释放，再检查发布失败、旧 manifest 字节精确不变，错误包含 commit/manifest.json 且不含 Unknown error，放开锁后同快照可发布。没有访问或锁定正式 R3 文件；其他系统没有运行这个 Windows 专用分支。

| 现有日志 | 实际结果与使用边界 |
|---|---|
| `io-diagnostics-r1-old-implementation-ctest.log` | 首轮失败 0/1，只报 Unknown error。主实施确认误用空 run.manifestRelativePath 读取目录；**不是有效锁定复现**，日志保留 |
| `io-diagnostics-r1-old-implementation-ctest-r2.log` | 正确 fixture 对旧实现 0/1；失败断言实际打印 `Cannot atomically publish detection data: Unknown error`。支持旧诊断丢失操作后的原因 |
| `io-diagnostics-r1-fixed-ctest.log` | 首轮修正 1/1 PASS，测试 .77 s、总 .78 s，支持 operation/path/非 Unknown cause、旧字节保留及释放锁后成功的断言 |

修正后的 PASS 日志没有打印具体 Qt 原因或 Windows 错误码，只能证明相应断言成立。open/write 分支本轮仅静态核对，没有独立故障注入执行。正式审计的 G/noSplit 第 7 个文件仍是 Failed；现有材料无法恢复当时的 open/write/commit 步骤或底层系统原因。受控拒绝替换不能证明正式失败也由共享句柄锁定。

## I1 / P2：无界绝对路径诊断可使终态超过错误字段预算

位置：首轮 `detection_store.cpp:402–405/737/778–781`；既有 `detection_task.cpp:799/828–829/859`。

首轮 failureMessage 拼接完整 `info.absoluteFilePath()` 和完整 Qt errorString，没有 UTF-8 字节上限。`projectRoot:350–354` 仅要求目标存在且为目录，不限制输出工程根的长度；IQ source.path 可以位于另一目录，其输入字段预算不约束输出根。Unicode 路径还会使 UTF-8 bytes 大于 UTF-16 字符数。因此不能从正常 fixture 的短临时路径或正式样例的 261 字符 manifest，推导任意合法工程根的诊断都小于 4096 bytes。

`finish:799` 将该消息直接赋给 terminal.error。store decoder `:737` 使用 `string(...,4096)`，实际检查 `toUtf8().size()`；`writeDetectionRun:778–780` 在 atomicWrite 前对 encode 的终态再 decode。若原始诊断超过 4096，终态即以 `error exceeds its text budget or is empty` 被拒绝，**即使原瞬时 I/O 故障已消失，也不能写出该 Failed 快照**。finish 随后记录 terminalPersisted=false，保留旧已发布 manifest，并清除可见 publication。不能误称草稿会写出一个超限/损坏 manifest；问题是诊断自身阻止正常终态持久化。

另一个必须同时考虑的路径是 `finish:828/859` 追加 prefix recovery 或终态持久化失败消息。单条消息在 4096 内，不保证合成 terminal.error 在预算内；继续拼接第二条约 3 KiB 消息就能超限。首轮旧实现的固定短错误没有新增绝对路径贡献；当前扩展需遵守原有字段预算，不能以扩大 4096 或改变错误即失败政策解决。

本项是从实际生成/持久化控制流及字段上限得到的边界缺陷；审查没有执行长路径、长错误或故障注入测试。短 Windows fixture PASS 不覆盖此边界。

### 符合现有契约的修正方向

主实施提出将目标写成相对已知 project root 的实际 `relative`，方向合理：当前所有 atomicWrite 调用的 manifest/chunk/head 路径均由固定 ASCII 前缀和 64-hex content/run digest 组成，最大约 151 ASCII bytes；这仅改变诊断表达，不改变 QSaveFile 的实际绝对目标。可由已知项目根、run/hash 与 relative 还原具体文件，避免输出根无限进入 run.error。

若 Qt cause 使用 `left(1024)`，1024 UTF-16 code units 最多约 3072 UTF-8 bytes，加固定 operation/relative 可证明**单条**小于 4096。但仍须给 run.error 的多次追加设置统一 UTF-8 字节预算，或预留足够摘要空间；不得按字符数简单截取4096。截断应保持有效 UTF-8、说明摘要已截断、短错误字节不变。完整 Qt cause 如需保留应走既有技术日志/诊断证据；长 cause 可能含长路径，摘要还宜保留 error category 与有用原因部分，不把 `left` 后的路径片段误称完整根因。

必要的行为验证应包括：长中文消息进入终态后仍≤4096且 DTO可decode；追加 prefix/再次写入失败后仍有界；底层发布失败时旧 manifest 字节不变，故障消失后可发布同一合法失败快照；仍不重试、不启用 direct fallback、不改变活动数学或失败计账。主实施正在准备相应隔离草稿及 fixture；收到新具体身份与执行日志后才能增量关闭 I1。

## clone、canonical 与完成边界

只读时 clone store 写入 UTC `2026-10-13T23:40:02.2880768Z`，store-test binary 为 `23:40:09.1752055Z`，fixed CTest 日志为 `23:40:10.1611741Z`；执行顺序与首轮修正一致。clone 主程序/UiTests/UiCapture 仍为 `23:25:42–45Z`，早于新 store 诊断，**未重新链接这项 IO 增量**。此前七文件 evidence-r4 的 SPEC/QUALITY 与聚焦成功范围保持，不能扩大为本次新增 store/task 或完整最新 UI 包已验证。

canonical store SHA 仍为 `369676110b56df8049b71f386fe62ec0a3359fba8dbcf4dc5e5db6af2bec35ee`，与 R3 freeze 相符；freeze SHA 仍为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`。正式失败历史、失败分母和正在运行的完整矩阵不被本草稿替换。正式底层 I/O 原因未确认；SCN/480 s 待确认、canonical 集成、新原生及全回归边界保持，不据本报告提交 M4 或进入 M5。

## 增量规格复审：四文件有界草稿 v2

2026-10-14（Asia/Shanghai）。**当前增量结论：DRAFT_SPEC_PASS_V2；I1/P2 在四文件草稿实现及本轮聚焦证据范围关闭，无新增实现阻断。** 首轮两文件的 FAIL/源码身份/失败日志继续作为历史保留，不代表这份 v2 仍未修正。canonical 未集成，本结论不替代 QUALITY、全套 UI、原生或 M4 完成判断。

本轮仅只读四文件、clone、`io-diagnostics-r1-identity-v2.json`、既有日志及契约，并追加本节；未构建或运行测试/模型/GPU，未改变 canonical/冻结/原型、提交或进入 M5。

### I1 的实现关闭

store 的失败消息仍在实际 open/write/commit 完成后读取 Qt cause。目标改为相对已知工程根的真实 `relative`，Qt cause 最多 1024 UTF-16 code units；QSaveFile 实际目标、path containment、direct fallback=false、错误返回 false、原子 commit 与原预算均不变。固定 ASCII content-addressed relative 最大约 151 bytes，加 cause 的 UTF-8 最大约 3072 bytes 与固定前缀，可限制单条消息小于4096；不能把这一摘要当成完整 Windows 系统调用根因或完整无截断 Qt 原因。

`boundedErrorSummary:28–45` 从 QString 的 UTF-8 bytes 计算预算，≤4096 时原样返回；超限时保留最多3072-byte头部、5-byte `\n...\n` 和最多1019-byte尾部。头部切点向前避开 continuation bytes，尾部切点向后避开 continuation bytes，保证有效 UTF-8码点不被切断。输入大于4096时两索引均在有效区间，头尾不会重叠，实际总长≤4096；不是按字符数截取4096。

finish 的初始错误（`:819`）、prefix recovery error 追加（`:848–850`）和终态写入 error 追加（`:880–881`）均调用同一个 helper。重复追加仍重新约束总预算，保留原始头部原因及最新末尾失败；短错误字节不变。没有改变终态来源校验、coverage、失败状态、publication 处理、sourceValidity 重提交规则或添加 I/O 自动重试。I1 的“诊断自己越过 DTO 上限而阻止终态落盘”路径已被关闭。

### 新 fixture 与现有执行证据

`testLongFailureSummariesPreserveTerminalContract` 使用实际 sourceFixture 文件，但在 Validating checkpoint 的 persist callback 注入6000个中文字符的错误，未用模型/GPU前向制造 I/O失败。两种配置分别允许终态真正调用 writeDetectionRun、或再次注入长终态写失败原因。

共同断言 Failed、≤4096 bytes、原始原因开头、省略标记、预期最新原因结尾、UTF-8 roundtrip精确，以及 encode→decode DTO 后 error 完全相同。允许终态写入时，实际 durable load 为 Failed 且 error 与内存一致；拒绝终态写入时，直接比较磁盘 manifest 原始字节完全不变，loader 按既有 Queued→Interrupted/recoverable 恢复。这验证了预算修正没有用新生命周期政策覆盖旧磁盘状态。

| 日志 | 实际结果与使用边界 |
|---|---|
| `io-diagnostics-r1-long-error-old-ctest.log` | 旧 task 实现的长错误测试0/1，失败于有界 UTF-8/原因保留断言；保存原失败 |
| `io-diagnostics-r1-complete-ctest-r2.log` | 组合3/4，task失败于拒写后恢复状态检查。主实施记录该版误要求 loader.error 为空，忽略既有 Interrupted 提示；不是新产品必须清空错误的契约 |
| `io-diagnostics-r1-complete-ctest-r3.log` | **4/4、5.57 s**：time-pipeline 1.56、refinement .01、store .77、task3.20 s。当前测试直接核对磁盘字节及 Interrupted/recoverable，未通过改产品生命周期消除旧断言失败 |

本轮审查没有执行这些测试。新两种长错误 fixture 实际覆盖 finish 入口及 terminal-write 追加；prefix recovery 追加使用同一有界 helper 已静态核对，但本 fixture 在 Validating阶段失败，没有另声称执行了长 prefix recovery error 分支。Windows 自有 manifest锁定 fixture 保持，当前 store测试仍覆盖 operation/path/Qt cause、旧字节不变与解锁后同快照成功。新测试不等于实际长目录/磁盘故障系统复现，也不确认正式 R3 的底层原因。

### v2 身份与完整重链

重哈希以下 deferred/clone 四文件，与 `diagnostics/io-diagnostics-r1-identity-v2.json` 全部一致：

| 文件 | v2 SHA-256 |
|---|---|
| `infrastructure/detection_store.cpp` | `e655e794ba424738bba267967896121fb118cd857f508ba75d42e3c9a947f709` |
| `infrastructure/detection_task.cpp` | `d94193419bbc2435f2c5810d81ac209c31eccacab2f8f53d096eefb43ec7daa3` |
| `tests/detection_store_tests.cpp` | `c3ed64f29a60a8aaba6390046d994e2a9ba5df2f8fab9be3e2901320aad32a05` |
| `tests/detection_task_tests.cpp` | `a8400df761fd22ff29484eca81e5113f087de1d9550d3cfe11ebe6a3bbd29ab3` |

`io-diagnostics-r1-complete-build-r3.log` 与实际产物确认完整 Release重链。本轮读取的 clone store/task 源写入 UTC `23:49:16.0737374Z` / `23:50:18.1804211Z`，主程序/UiTests/UiCapture 均晚于这两项产品改动；test source 为 `23:52:10.1748258Z`，task-test为 `23:52:16.7935228Z`，r3 CTest为 `23:52:22.6742497Z`。

| clone 产物 | UTC 写入时间（2026-10-13） | 当前 SHA-256 |
|---|---|---|
| `SignalStudio.exe` | `23:50:29.0994326Z` | `3378bd28258c857b83c41a6296b6a90dcf84b2ba9b3cedd97838ab775672c499` |
| `SignalStudioUiTests.exe` | `23:50:30.8791287Z` | `f7932ea2621d21a8e315e3b23bdbe6e5237898e0de876127fb352f90dfa2178c` |
| `SignalStudioUiCapture.exe` | `23:50:31.7037671Z` | `1083a20c1d33c9f05f701e1526ad7868e40fb86cca852c90631809113c0f4f5a` |
| `SignalStudioDetectionStoreTests.exe` | `23:50:27.0379744Z` | `fe27587815e78c78e0044b384cd52578a4ecb829df23ce16ac1981d1f8cba917` |
| `SignalStudioDetectionTaskTests.exe` | `23:52:16.7935228Z` | `a506bb6281baccbb9bdb1e8a1d96e5c40d58a2cb66929b09734ed44d5dea678c` |

此前 UI 包尚未重链最后 store 诊断的版本缺口在 clone 编译/链接范围关闭；完整 build 不等于当前十一文件已通过全套 UI或新 native。此前七文件 Release offscreen UI94/94仍保留其原身份范围，不套用为本四文件增量执行证据。

### 保留的完成边界

canonical store/task/Release time-pipeline CLI抽核仍匹配 R3 freeze，freeze SHA不变。矩阵日志读取时推进至 G/ROI-noSplit 第7个 Native；正式完整220对仍在执行。正式 G/noSplit 失败原因尚未确认，原始失败/分母/此前效果 FAIL 不被有界摘要或隔离成功替换。

SCN profile例外与 canonical Debug UI预算决定仍待用户答复；Release在300s内通过不能代替Debug未完成或批准480s。仍待当前草稿 canonical集成、新身份下相应回归/全套UI/原生旅程及最终节点证据。只关闭I1和clone重链边界，不提交M4、不进入M5。
