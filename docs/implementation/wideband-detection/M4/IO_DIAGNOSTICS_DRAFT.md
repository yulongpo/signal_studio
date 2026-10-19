# M4 原子写入错误诊断草稿

正式 R3 G/sample noSplit 的第七文件在 reading checkpoint 失败；它与旧 R2 的长 record ID 故障不同。完整事实、33 个 chunk 和 66 个 head 的身份核对见 ATOMIC_PUBLICATION_FAILURE_R3_AUDIT.md。现有错误无法确认底层原因，不将共享锁、路径或预算当作已证实原因。

隔离草稿位于 `out/tasks/wideband-detection/M4/deferred/io-diagnostics-r1/`，两文件 SHA 及 clone 一致性见 `diagnostics/io-diagnostics-r1-identity.json`。草稿把 QSaveFile open/write/commit 与读取错误信息分开，记录失败操作、路径及 Qt 原因，仍禁止 direct-write fallback；失败立即拒绝，不新增重试，不改变已发布文件、生命周期或预算。canonical/freeze 尚未应用这项修正，正式 R3 失败也没有被替换。

Windows 专项夹具只对自有 QTemporaryDir 的 manifest 使用 CreateFileW：允许校验读取、拒绝文件替换，校验失败时旧 manifest 字节不变，释放锁后相同 snapshot 可发布。正确夹具下，旧实现明确返回 `Cannot atomically publish detection data: Unknown error`，拒绝诊断断言；见 `logs/io-diagnostics-r1-old-implementation-ctest-r2.log`。修正后 store CTest 1/1 通过、0.78 秒，要求错误包含 commit、manifest.json 和非 Unknown 的实际原因；见 `logs/io-diagnostics-r1-fixed-ctest.log`。

首轮新增测试错误读取尚未在内存回填的空 manifestRelativePath，实际读了目录；已改用 encodeDetectionRun 的实际 manifestPath。首轮日志保留，不用它证明错误诊断缺陷。有效复现和修正后的执行均在本仓库复制的独立 Release 构建中完成，没有运行模型或另占 GPU。

此前 evidence-r4 七文件已分别通过 SPEC/QUALITY、聚焦回归和 Release offscreen UI 94/94；此次两文件是额外诊断草稿，不属于那次七文件通过的范围。clone 当前 store test 已重新链接这项修正，完整 UI targets 尚未重链；不能把旧 UI 成功称作当前九文件全部执行通过。最终集成仍需对应审查及新版验证。

增量预算修正：首轮 SPEC 发现无界 absolute path 可能超过 run.error 的 4096 UTF-8 byte 契约，报告中的 P2 保留。当前草稿扩为 store/task 和各测试共四文件，身份为 `diagnostics/io-diagnostics-r1-identity-v2.json`。store 诊断改用项目相对的固定 ASCII content-addressed 路径和最多 1024 个 UTF-16 单元的 Qt 原因；不改变写入目标。task 在入口及两处错误追加后统一做最多 4096 bytes 的 UTF-8 摘要，保留开头的原始原因和末尾的最新失败，插入明确省略符，切点避开多字节码点；短错误不变。

新增两条实际 IQ 源但验证 checkpoint 即注入长中文原因的回归：终态可写时，Failed 记录完整满足 DTO 预算并能 durable 加载；终态再次拒写时，追加摘要仍是有效 UTF-8、可 encode/decode，磁盘保持原始 Queued manifest 字节，加载按原契约显示 Interrupted/recoverable。旧实现长错误检查失败日志为 `logs/io-diagnostics-r1-long-error-old-ctest.log`。预算草稿第一次组合检查误要求恢复后的 error 为空，忽略 loader 的 Interrupted 提示；已修正为直接比较原 manifest 字节并检查恢复状态，组合首轮 3/4 日志保留。

最新全部目标通过复制的 scripts/build.ps1 Release Build 重新链接，相关 time-pipeline/refinement/store/task CTest **4/4、5.57秒**，见 `logs/io-diagnostics-r1-complete-build-r3.log`、`logs/io-diagnostics-r1-complete-ctest-r3.log`。四文件仍待 SPEC→QUALITY 增量复审，未应用 canonical、未重跑新 native；旧七文件的 UI 成功不扩大为当前十一文件全套 UI 或原生验收。正式 R3 未改，其一次失败原因仍未知。

四文件增量复审现已按顺序完成：SPEC 为 DRAFT_SPEC_PASS_V2，QUALITY 为 DRAFT_QUALITY_PASS_V2，未发现新增阻断，见对应两份 IO_DIAGNOSTICS_DRAFT_*_REVIEW.md。I1 仅在 identity-v2 草稿范围关闭；prefix 错误追加仅静态核对，未声称故障组合执行覆盖。正式失败根因及 canonical 集成、最终 UI/native 边界继续保留。两份草稿身份清单也已复制到本目录 evidence-r4-draft-identity.json、io-diagnostics-draft-identity-v2.json，便于审核。
