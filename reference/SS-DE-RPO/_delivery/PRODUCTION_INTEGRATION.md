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
