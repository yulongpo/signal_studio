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
