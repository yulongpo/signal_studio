# Continuation production integration

This node connects Grok A/B/C/D/E/F/G contributions to the real application. Agent session IDs, independent worktrees and authored commits are recorded in `continuation-agent-roster.md` and ignored `out/tasks/ss-de-rpo/agents/results/` records. All continuation agents use `grok-4.7-build-fast` / `xhigh`; no Codex subagents were used.

Resource preflight now checks the SourcePlan resident estimate against probed RAM. Sample applies the smaller of the user's authorization and currently available RAM to its bounded planner. Its exact scratch lease checks the actual volume. Checkpoint and `.ssproj` atomics check encoded bytes plus overlapping live files, crediting only files already on that volume. The optional bounded resource profile preserves unknown/deferred facts. Existing format and allocation caps remain.

The first real model window is prepared after the Worker begin request is flushed and while the independent Worker initializes its model. One buffered window is consumed exactly once by the normal batch loop. QProcess and DSP stay on the producer thread; readiness is the parent's observed event. This introduces no Worker/context reuse and no unbounded queue.

Provisional preparation shares the actual cancel token. Timeline marks use the parent's monotonic clock, retain the earliest observation of a stage, and are unique. Recovery and comparison are observed at distinct successful service boundaries. `t_pipeline_terminal` is separate from business `t_terminal`; `t_durable_publication` follows the actual project commit. Headless measurements use `t_request_begin`, never invent a user click. Small preparing controls repaint before preparation is dispatched.

## Checks completed at this node

- Debug directed CTest: 9/9 passed, 146.25 s. Resource, Sample, cache/timeline, GPU decision, provisional cancellation, UI feedback, UX-R5 durable faults/recovery and UX detection lifecycle are included. Log: `N0/logs/final-integration-debug-directed.txt`.
- Real TensorRT fault entry: injected B2 allocation failure retries the same two windows at B1 with two actual forwards; injected B1 fails; cancellation before and after allocation leaves readiness released. Log: `N0/logs/postallocation-cancel-real-gpu.txt`. Injection is not physical OOM.
- Original recorded IQ, Debug, Sample 20/20: formal comparison, isolated project atomic commit and reopen passed, 9 annotations/9 segments, 25357 ms. This is a pilot, not final performance evidence. Log/report: `N1/real-iq-20-debug/`.
- SourcePlan confirms a supported denser hop of 0.00005 s permits 2044 real windows on the same 0.8 s file; no frames or physical samples are extended. Earlier historical-profile-only failure is preserved. `scripts/prepare-ss-de-rpo-requests.ps1` reproduces the requests; runtime benchmarks must independently verify every count.

The first resource integration build had a QString/std::string mismatch, fixed before the passing build. One concurrent-UI CTest start run failed; an isolated diagnostic run passed 6/6 and the subsequent directed suite passed. Failed outputs remain. Grok G's prior offscreen paint p95 failure and older native success are retained separately; final current-code native evidence is still required.

Full current Debug/Release CTest, complete original-IQ 159/363/2044 publication, Sample Golden/long, native display/deployment, final cold/resident comparisons, and independent exact-HEAD H review remain release gates. No remote write has occurred at this node.

## Post-atomic start cancellation boundary

A cancellation after the project atomic save must enqueue then cancel the new run, rather than cause the queue callback to reject and restore the previous project bytes. The test hook now injects precisely inside that post-save queue callback. Debug start/UI/UX detection checks pass 3/3 (73.51 s), including reopening the saved project and verifying its formal slot stays empty. `N2/postcommit-cancel-debug-tests.txt` is the raw log.

The broader first Release CTest run exposed stale UI source-tab and overflow assumptions, and a dirty-baseline workflow failure. These failures remain release blockers and are assigned to new independent C2/G2 sessions; their original logs are retained. Original recorded IQ Release 159/159 has completed inference, Sample, formal comparison, isolated atomic commit and reopen (46660 ms under concurrent CPU development, not a performance comparison). Larger cases are still running.

## Actual native click and large-input evidence

The common native measurement harness is `SignalStudioDetectionNativeBenchmark`, launched through `scripts/benchmark-ss-de-rpo.ps1 -Native`. It drives the real QWidget run button, waits for the formal slot, checks the terminal snapshot and reopens the isolated project. A resident invocation keeps the same MainWindow/QApplication, excludes one warmup and observes a distinct Worker PID for every run. Native pilots passed three cold runs and three resident rechecks (plus the excluded warmup). They ran while development was active and are not the final performance comparison. Actual screen: connected 2, Redmi 27 NU, native DISPLAY6, 3840x2160, DPR 1.5, logical fullscreen 2560x1440.

The source and runtime baseline remains 7220b2e. Independent D3 added only parent-clock observability in 4371940; e52c3a9 adds the same native measurement harness on that calibration branch. Neither baseline production change is merged into rebuild/cursor. The original frozen baseline package remains intact. The calibration package uses its original production code and the frozen package's matching deployed Qt/CRT, model, CUDA DSP and oneMKL DLLs. It does not receive the new cache, startup overlap, resource gates or batch adaptation. Final comparison must be serialized after builds stop.

Original IQ Release 159, 363 and 2044 all formally published and reopened. Elapsed times under development load: 46660, 117626 and 616428 ms. For 2044, the parent process peak RSS was 746672128 bytes; 17 candidates completed refinement. Evidence: `N3/original-release/cases.json` and per-run measurements. These are functional/resource results, not cold/resident speedups.

Actual 20 MS/s synthetic Sample streaming processed 1.2 billion source samples (60 s) with a 96 MiB grant, 19951616-byte process peak RSS, 9600000606-byte measured scratch peak, and all scratch leases returned. Elapsed 439728 ms. The result requires measured coarse fallback and is not publication-eligible Sample activity; the precision reason and numeric evidence are retained. The earlier 300 s deadline cancellation is preserved; the successful rerun used 900 s. This is synthetic algorithm/resource evidence, separate from original IQ/model/field accuracy. Command: `benchmark-sample-ux-r6.ps1 -Durations 60 -SampleRate 20000000 -Scenarios continuous -Modes stream -DspCompute cpu -MemoryMiB 96 -ScratchMiB 16384 -TimeoutSeconds 900`.

The current Release real-model `--live` suite passed FFSCN and SCN forward/parity. Its first launch correctly rejected a stale hardcoded request hash; the test now snapshots the selected package hash before the unchanged golden comparison. Actual `--adapted` also passed, including controlled allocation retry and cancellation. Raw logs remain in `N3/`.

## Feedback instrumentation correction

The first current Debug full CTest attempt passed 59/60, including all repaired workflow/geometry/parameter tests. R-19 failed because its queued post-paint callback included unrelated event-queue work. Both the failed native 114.435 ms observation and offscreen 124.211 ms observation are preserved. The observer now samples directly after QApplication finishes the actual QWidget Paint dispatch. It separately records the queued callback delay and the handler duration. The 100 ms assertion is unchanged, and compositor/scanout is explicitly outside this instrumentation.

The corrected Debug observer passed directed CTest 1/1. Native diagnostic: 20 samples, completed-paint p95 8.29965 ms, paint-body p95 0.54058 ms, queued-callback p95 130.008 ms. This demonstrates why queued callback time is not completed-paint latency. The final serial native measurements will supersede this diagnostic. Five click attempts are now explicitly required within 100 ms; the native diagnostic burst took 17 ms and emitted one request.

`collect-ss-de-rpo-ctest.ps1` checks production-source equivalence and the registered CTest inventory before combining a full attempt with directed supplements. It preserves the initial failures and states this is case coverage, not a single all-passing attempt. The corrected Debug set covers all 60 cases (59 initial passes + one directed pass). Only the feedback test changed; the product sources used by the other 59 tests are unchanged. RPO UI/start CTest entries now use the existing file-log wrapper so GUI-subsystem failures retain diagnostics.

## Retryable start notice and final complete rerun

`final-acceptance-02` passed R-01..R-20 and all real model/native/package checks, but Release full CTest was 59/60: asynchronous status refresh erased a transaction-save error. This is a real UI lifecycle failure, not a reporting exception. e5ae911 stores a retryable notice separately from input validation, preserves it across background refresh, and clears it on retry or changed inputs/source. New-context refresh releases the old provisional card latch; a late rejection cannot overwrite the new context. The true atomic project commit retains its existing navigation lock.

Both Debug/Release start suites now pass all seven Qt rows. The added test controls the pending card boundary, where navigation is actually allowed; real pre/post-commit cancellation remains in separate integration tests. The first added test attempted navigation while the atomic commit held its lock and correctly failed; that output remains in N4/start-notice-debug-tests.txt.

Because product UI code changed, prior collected CTest coverage is no longer reused. Full builds completed for every target, and N4/final-acceptance-03 reruns all 60 registered cases in both configurations, plus native/GPU checks. This run must finish successfully before the final comparison and H review.

The performance summary reads actual cold and resident measurements, checks equal input/model/profile/parameters, requires three passed trials per side and four distinct Workers including resident warmup, and refuses ratios for failures or missing data. The sampler filters the benchmark parent and its direct Workers; one poll shares one timestamp so simultaneous RSS sums do not combine unrelated samples. Device GPU usage and volume free-space remain explicitly system-wide observations.

## Interrupted acceptance attempt and retained UI watchdog

The final-acceptance-03 process was interrupted before Debug cases 58..60 completed; it has 56 recorded passes, one workflow watchdog timeout, and three missing case outcomes. No acceptance PASS is assigned to that interrupted invocation. A fresh directed run covers the complete workflow executable and remaining UI suites. The workflow retains every one of its 27 Qt rows and assertions, with the project's established 480 s UI watchdog replacing its older 180 s total guard. Both presets were regenerated successfully. No product source or test body changed for this guard adjustment.

The coverage collector now records initially missing cases, demands passing supplements for failures and every current registered case, rechecks production/test source identity, and rejects build-definition changes beyond comments/UI watchdog normalization. Final verification will consume only successful complete case coverage; source-equivalent original-IQ runs remain separately identified. This does not turn the interrupted attempt into a single passing full run.

## Final verified closure

final-acceptance-04 exited0: all R-01..20 and both CTest gates PASS. Release full60/60,298.99s. Debug60 coverage is56 initial passes plus4 complete supplements, preserving one180s timeout and three interrupted outcomes. Native n20 Paint p95 Debug7.63922ms/Release43.7656ms. Both final-code independent deployments pass GPU and software with system-only PATH. Final real live/adapted, Sample Golden, rawON20 and source-equivalent original large cases pass. No product/test assertions were removed.

Final native baseline/new comparison is serialized and fully passed. Cold click-to-formal mean12791.739/10449.592ms; resident10052.085/7727.640ms; first inference improves in both. All14 runs including excluded warmups formally publish/reopen. All8 final project geometries match exactly (9 Algorithm annotations). See the compact tracked performance reports and N4/performance-summary.json. The final gate is a fresh read-only H review of the subsequent exact delivery commit; its result and push identity remain outside Git.

## Independent review correction and final rerun

H1 correctly returned FAIL on daa30e8. See REVIEW_FIX_CLOSURE.md and archived H_REVIEW_daa30e8_FAIL.md; no push followed it. C3 and main corrected the channel guard, post-atomic stale callback and modal caller lifetime issues. The10-row start suite and extended core tests pass. Source75cf063 was rebuilt in all targets. final-acceptance-06 is a fresh60/60 Debug (988.28s) and60/60 Release (292.53s), all R-01..20 PASS, native/model/Golden/retention checks PASS. Native n20 Paint p95 is6.4826/37.6025ms; final current Debug deployments passGPU/software.

After all tests/builds stop, final native baseline-02/current-02 each pass3cold plus resident warmup+3, all14runs. New coldformal12976.3255→10626.7699ms, resident10290.3104→7948.3501ms;8project geometries/9Algorithm objects equal. The original large headless evidence remains explicitly scoped: noSession object is on that path, onlySession UI guard changes are exempt, other processing/result-service sources must match. Old GUI coverage is not reused for this final run. The next final committed tree requires newH2 independent exactSHA PASS; reviewer/report/push records remain outsideGit.
