# M4 IO diagnostics 草稿 v2 增量质量审查

结论：**DRAFT_QUALITY_PASS_V2（仅当前四文件及已结束的相关日志范围）**。UTF-8 摘要预算、首尾原因保留和终态追加路径正确，未发现本增量新增质量阻断。SPEC 的 I1/P2 在草稿范围关闭；分步错误读取仍保持拒写及原子策略。**canonical/freeze 尚未应用，正式 R3 的底层写入失败原因仍未知，不能声称正式发布故障已修复。** 本报告不扩大为完整 M4、最新全套 UI 或 native 通过。

2026-10-14（Asia/Shanghai）。只读 `deferred/io-diagnostics-r1/` 的 store/task 两份实现及两份测试、对应 clone、v2 身份、IO 草稿文档和现有日志，仅新增本文件。未修改源码、canonical、freeze、原型、历史结果，未构建、运行测试/模型/GPU、提交或进入 M5。执行结果由主实施产生，本审查没有重跑。

## 当前版本与检查结论

四文件在 deferred、`local-builds/evidence-r4/source/` 与 `diagnostics/io-diagnostics-r1-identity-v2.json` 中原始字节 SHA 全部相同。identity SHA 为 `ba65dd5c3bdc407eae0d68876f9b3c2a86a370c36aacc59144e3c0a23601ab15`。下述源码行号均指当前草稿/clone。

| 项目 | 具体核对与质量结论 |
|---|---|
| UTF-8 预算和索引 | `detection_task.cpp:28–45` 先由 QString 生成 UTF-8；≤4096 bytes 原样返回。超限时 head≤3072，separator=5，tail≤1019，总量≤4096。输入超4096时 head 与 tail 初始索引均在有效范围且不重叠；切点最多避开一个码点的 continuation bytes，不越界，也不留下半个码点 |
| 首尾原因与短错误 | head 切点向前，tail 切点向后，保留原错误开头和最后原因。再次追加后同样重新摘要，不累计突破4096。正常≤4096 bytes 的 task error 与追加结果不被截断或加省略符；这一“不变”指 summary 的短输入，store 的失败字符串有意新增 operation/relative/cause，不声称其与旧 Unknown 字符串完全相同 |
| 全部 finish 入口 | `:819` 初始终态、`:848–850` durable prefix recovery error 和 `:880–881` 终态写失败追加，均调用同一 helper。没有遗漏已读 finish 内的 error 拼接，也没有扩大 DTO 的4096上限 |
| store 单条诊断 | `detection_store.cpp:392–415` 保留真实绝对写入目标，诊断用固定 ASCII content-addressed relative；Qt cause 最多1024 UTF-16 units，转换后最多约3072 UTF-8 bytes。加最大约151-byte relative 和固定前缀仍在4096内；不会再把任意长工程根直接加入 run.error |
| 操作顺序与失败政策 | open、write、commit 分别完成后读取错误，短写不进入 commit；原 path containment、8 MiB 单写预算、directWriteFallback=false 与失败返回 false 保持。没有新增 retry、direct write、忽略 commit 或算法/生命周期策略 |
| 终态持久化 | `finish:873–892` 仍在持久化成功后暴露 terminalPersisted；写失败保留 Failed 内存状态、terminalPersisted=false 并清空 publication。旧 sourceValidity 的成功写入后重提交规则未改，不把它误称新增 IO 重试。summary 使合法长错误满足 DTO，不将拒写转成功 |
| Windows 句柄 | `tests/detection_store_tests.cpp:231–256` 只锁自有 QTemporaryDir manifest，允许校验读取而拒绝替换。writeDetectionRun 按其既有 catch/false 契约返回后立即 CloseHandle，再做可能抛出的断言；没有在失败断言后遗留句柄。解锁后的第二次写是 fixture 显式调用，不是产品自动重试 |

相对路径仍指向实际项目内目标，可由已知项目根还原；它不改变 QSaveFile 的实际目标。Qt cause 的 `left(1024)` 是摘要，可能截掉长原因的后半部，不能称完整系统调用原因。task 的 first/last 保留是整体 run error 的开头和最新尾部，也不保证保留每个中间原因。切点有效性来自 QString→UTF-8 的合法输入；没有把字符数当作 UTF-8 字节数。

四文件均未发现超过80字符的行或行尾空白；helper 的预算常量和三处调用集中，未引入新数据容器或算法计算。本审查未重跑 clang-format。没有新增 uint64 算术或原子发布协议变更；旧诊断预算、发布原因未知与本次修正范围分开记录。

## fixture 的实际覆盖

长错误测试 `tests/detection_task_tests.cpp:404–470` 使用实际 IQ sourceFixture，但在 Validating checkpoint 的持久化 callback 注入6000个中文字符原因；不会为了制造 IO失败执行模型前向。两种配置分别允许真实终态写入、再次拒绝终态写入。断言 Failed、≤4096 bytes、原始原因开头、明确省略符、预期最后原因、UTF-8 roundtrip 精确，以及 encode→decode 后 error 精确相同。

允许终态写入时，实际 durable load 为 Failed，error 与内存一致，证明长摘要不再阻止终态满足既有 DTO。拒绝终态写入时，直接比较旧 manifest 原始字节不变，loader 依既有契约把旧活动 Queued 记录恢复为 Interrupted/recoverable；没有通过修改产品生命周期或清空 loader 提示消除首轮断言失败。

这两条实际覆盖 **初始长错误和 terminal-write 长错误追加**。测试在 Validating 阶段失败，未执行 long prefix recovery error；该第三处只完成静态核对。fixture 的多字节输入是三字节中文，未另执行精确4096/4097-byte、二字节/四字节切点或实际长目录系统故障；代码的边界证明支持这些切点，不把静态结论改称已执行测试。短 error 的原样返回同样是静态核对，不额外编造逐字节短错误测试。

Windows 锁定 fixture 比较拒绝替换后的旧 manifest 完整字节，检查 commit/manifest.json/不含 Unknown error，释放句柄后同 snapshot 可发布。现有 PASS 日志只证明这些断言成立，没有打印具体 Qt 原因或 Windows 错误码。open/write 分支未另作独立系统故障注入；受控 commit 拒绝不能证明正式 R3 失败由共享句柄锁定。

## 已结束日志与产物身份

`logs/io-diagnostics-r1-complete-ctest-r3.log` 为 **4/4 PASS，5.57 s**：time-pipeline1.56、refinement.01、store.77、task3.20秒。它包含本轮 store/task 新 fixture，并继续覆盖 evidence-r4 的两项聚焦回归；不等于所有23项或全套 UI 已执行。

旧日志保持其实际意义：首轮 store fixture 误用尚未回填的 manifestRelativePath，读取目录，不是有效锁定复现；修正 fixture 对旧实现的 `old-implementation-ctest-r2.log` 真正显示 Unknown error 诊断失败。旧 task 的 `long-error-old-ctest.log` 真正失败于有界 UTF-8/原因断言。组合 `complete-ctest-r2.log` 的3/4包含误期待 loader.error 为空的检查失败；当前改为旧字节及 Interrupted/recoverable 契约检查。没有把历史失败覆盖为成功或统一归因产品故障。

完整 build 日志及产物核对支持 clone UI targets 已晚于产品 store/task 增量（源UTC `23:49:16Z`/`23:50:18Z`）；当前测试源码为 `23:52:10Z`，task-test产物为 `23:52:16Z`，r3 CTest 日志较晚。

| clone Release 产物 | UTC时间（2026-10-13） | SHA-256 |
|---|---|---|
| `SignalStudio.exe` | `23:50:29Z` | `3378bd28258c857b83c41a6296b6a90dcf84b2ba9b3cedd97838ab775672c499` |
| `SignalStudioUiTests.exe` | `23:50:30Z` | `f7932ea2621d21a8e315e3b23bdbe6e5237898e0de876127fb352f90dfa2178c` |
| `SignalStudioUiCapture.exe` | `23:50:31Z` | `1083a20c1d33c9f05f701e1526ad7868e40fb86cca852c90631809113c0f4f5a` |
| `SignalStudioDetectionStoreTests.exe` | `23:50:27Z` | `fe27587815e78c78e0044b384cd52578a4ecb829df23ce16ac1981d1f8cba917` |
| `SignalStudioDetectionTaskTests.exe` | `23:52:16Z` | `a506bb6281baccbb9bdb1e8a1d96e5c40d58a2cb66929b09734ed44d5dea678c` |

UI 完整重链关闭版本落后问题，不能把此前七文件的 Release offscreen UI94/94套用为当前 IO四文件/完整十一文件已运行 UI或native。本轮未读到这些新执行结果，也没有为本审查发起运行。

关键日志 SHA：complete-build-r3 `79c1e2905d0a5435df7178e3cd8f780fdec17cbceb5dfc9eebb5dba4a95dc03e`；complete-ctest-r3 `21d711abab0f00f6cde37a422855fe8f074595645f3c92a79cf74a45444143fc`；有效 old-implementation-r2 `0f43c77d8e48169174038091b11c5bce3be40960a5e08a0de534812c28da7f12`；long-error-old `0f872bda90b0d28ca04843a6d70268238b84058e504fdb4e371c27f42bd0af29`；complete-ctest-r2 `9ff9f4968b3ada6b1408ced40c9dc0ba16840e6cc9950bc182062ecdfc3631c6`。

## v2 文件与完成边界

| 相对路径 | deferred / clone / v2记录一致的SHA-256 |
|---|---|
| `infrastructure/detection_store.cpp` | `e655e794ba424738bba267967896121fb118cd857f508ba75d42e3c9a947f709` |
| `infrastructure/detection_task.cpp` | `d94193419bbc2435f2c5810d81ac209c31eccacab2f8f53d096eefb43ec7daa3` |
| `tests/detection_store_tests.cpp` | `c3ed64f29a60a8aaba6390046d994e2a9ba5df2f8fab9be3e2901320aad32a05` |
| `tests/detection_task_tests.cpp` | `a8400df761fd22ff29484eca81e5113f087de1d9550d3cfe11ebe6a3bbd29ab3` |

canonical store/task 与 Release time-pipeline CLI三项抽核仍符合 R3 freeze；freeze SHA仍为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`。报告写入后再次核验四个草稿/clone 文件与三项 canonical，仍全部一致。不把三项抽核扩大成72 files/308 data assets完整复验。本次只审 IO四文件，不重做或扩大旧全 M4 审计。

正式220对矩阵仍运行，既有发布失败、效果 FAIL 和失败分母均不被诊断草稿替换。SCN profile例外与UI预算两项用户决定待确认；canonical集成及相应新身份回归、最新全套UI/原生和最终节点证据仍未完成。草稿 PASS 仅说明本增量诊断与预算修正可通过质量复审，不代表 M4可以提交或进入M5。
