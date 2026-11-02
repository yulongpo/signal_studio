# SS-DE-RPO continuation contract

Baseline: `7220b2e295963288cb5d11e76946bb77646be0b0`, branch `rebuild/cursor`.
The user approved implementation, local commits, and a normal push after final independent PASS. Subsequent user steering selects **grok-4.7-build-fast / xhigh (Extra High)** for all Grok agents. No Codex subagents are scheduled.

## Ownership and integration

A: domain/detection.h/.cpp, ui/detection/detection_resource_options.* and new independent resource helpers.
B: infrastructure/detection_sample_stream*, infrastructure/detection_time_analysis.*.
C: ui/detection/detection_card.*, detection_panel.*, detection_phase.*.
D: infrastructure/detection_task.*, detection_source.*, detection_worker_client.*, detection_model_snapshot.*.
E: domain/detection_postprocess.*, application/detection_result_service.*, infrastructure/detection_projection.*, detection_refined_publication.*.
F: infrastructure/detection_model_backend.* and new independent GPU helpers.
G: tests/*ss_de_rpo*, new tests/*rpo*, scripts/*ss-de-rpo* (except agent launcher owned by main).
H: independent read-only final review, no implementation.
Main: CMake, existing global scripts, app/main_window.cpp, cross-module glue, integration, evidence and publication.

Old worktrees contain untracked task files: preserve them and use new continuation worktrees. Maximum four concurrent CPU workers; GPU measurements exclusive. Agents may commit their own tested work, never push or edit shared deployment outputs.

## Frozen interfaces

1. Preserve existing request budget fields/defaults and v6 project format. Resource diagnostics are optional `performanceJson` extensions. `ResourceDecision` remains the single Sample budget decision; bytes and cells never substitute for each other. Add resource snapshots/ledger as runtime-only helpers. Retain raw-head preflight rejection prefixes and 8 MiB shard boundary.
2. B calls `decideSamplePhaseBudget(authorization, estimatedResidentBytes, cells)` from the real planner/execution. Work-unit and contiguous-allocation limits remain separate; no raising constants. Low resource adapts blocks or reports requested/available/phase, never changes requested geometry or silently disables retained outputs.
3. C owns a shared atomic cancellation token for active StartCommit. `DetectionPanel::cancelRun` handles a provisional start before consulting persisted runs. Prepare cancellation keeps the prior formal result before the start transaction commit; cancellation racing after successful commit uses normal queued-run cancellation. Extend `startRequest(request,error,clickSteadyNs=0)` and pass the card click stamp from main.
4. D adds optional `std::function<bool()>` cancellation arguments with default `{}` to `freezeDetectionModelSnapshot` and `prepareQueuedRun`; preserve existing calls. C passes its token and shutdown checks. Model/source planning calls checkpoint cancellation; failures do not strand locks or Worker processes.
5. C can call existing `noteSteadyMark` for start phases and publication transaction phases. D records remaining parent-process steady timestamps. `t_final_shard_persisted` names shard completion; `t_durable_publication` only names successful recovery/compare/project commit in the GUI publication path. Missing stages remain absent. GPU events remain separate.
6. D reuses immutable prepared SourcePlan within the same run after identity revalidation, and bounded preparation caches keyed on actual source/model/profile/config dependencies. Keep independent Worker per run; no persistent context pool.
7. E uses a deterministic sorted frequency-interval broad phase, retaining exact predicates and existing pair/merge tie ordering. For matchIou==0 preserve all pairs if necessary. Account index memory. Genuine candidate/comparison exhaustion still explicitly fails without partial mutation; preview eviction never drops formal tracks.
8. F preserves requested/default batch 2, selects only real profile-supported shapes, records physical/logical batch, CUDA index and free/reserved/required VRAM. Reuse existing --live/--adapted tests. Controlled allocation failure is labelled injected, not a real OOM claim.

## Acceptance and evidence

R-01 through R-20 require current-version executable evidence. Reuse repository scripts, add one RPO suite entrypoint. Distinguish synthetic files, original IQ, fixture worker, true TensorRT forward, native QRhi display and software fallback. Do not run large RFbench or batch file hashing.

Cold baseline/new: fresh application/Worker/model, three runs each. Warm baseline/new: resident application re-detect, three runs each, independent Worker remains explicitly recorded. Native feedback p95 uses repeated visible-paint observations. Do not use a failed run as the denominator of a speedup.

Node exits: implementation, relevant checks, local commit, next node. Final tracked implementation/evidence commit precedes independent H exact-HEAD review. Final review and push record stay outside tracked files. A required missing test blocks full PASS and push. Push only origin/rebuild/cursor, no force and no changes to unrelated branches.
