# E continuation handoff

Worktree `D:/project/ss-de-rpo-wt/continue-e`, branch `codex/ss-de-rpo-e`, baseline `7220b2e295963288cb5d11e76946bb77646be0b0`. This file is committed with the change and does not name the new SHA.

## Changed files

- `domain/detection_postprocess.h`: `matchIou == 0` is valid. `FrequencyTrackerFrameStats::comparisons` counts that frame's broad-phase fusion candidates.
- `domain/detection_postprocess.cpp`: sorted frequency-interval broad phase for association and open-set fusion.
- `tests/ss_de_rpo_tracking_tests.cpp`: oracle, ties, boundaries, containment, zero threshold, merge recompute, exhaustion/rollback, index budget, preview cap, sparse 4,717,056-pair scene.

Unchanged owned files: `application/detection_result_service.*`, `infrastructure/detection_projection.*`, `infrastructure/detection_refined_publication.*`. No proven source-field inconsistency. `replaceCurrentDetection` still compares `encodeSourceSet` and `encodeAnnotation`, and still rejects with `持久检测来源/对象数与 publication 不同` or `持久检测对象与 publication 不同` plus the existing suffix. `completeCoverage` still requires `DetectionLifecycle::Completed` together with full windows. Window completion is not business success. UX-R6 `beginDetection` / `markStale` semantics are untouched.

## Algorithm

Association at `matchIou == 0` keeps the old burst-open Cartesian loop and does not build an index. IoU is never negative, so every such pair qualifies. Positive `matchIou` uses a bipartite sweep of strict overlaps (`upper > other.lower`). The exact IoU predicate still runs and still counts overlaps that miss the threshold. Pairs are still sorted by `{-iou, -score, track, candidate}`.

Fusion rebuilds `{lowerHz, upperHz, index}` after every merge, sorted by `(lowerHz, index)`. Scan `q > p` and stop early. Fragment pass stops when `right.lower >= left.upper`. Overlap pass runs only when `mergeToleranceHz > 0` and stops when `right.lower - left.upper > mergeToleranceHz`. Indices are canonicalized to `i < j` before the old key `{frequencyGap, sampleGap, overlapRank, similarity, centerDistance, earliest, i, j}`. `sameCarrier` still requires `intersection > 0`. Sealed history is not scanned.

Index bytes are `256 + count * 96`, charged on the consume `upperBytes` with `tracker-resident-byte-budget-exceeded` only while that index exists. Association and fusion indexes are not stacked. A failed consume returns before `state_` is swapped. `rememberPreview` still trims only the preview log. Formal tracks and `visiblePreviewEvents` stay. Default comparison caps remain 4,000,000. Existing resource error strings stay, including `association-comparison-limit-exceeded`, `association-pair-limit-exceeded`, `association-resident-byte-budget-exceeded`, and `frequency-fusion-comparison-limit-exceeded open=<n> sealed=<n> comparisons=<n>`.

## Checks

Worktree-local configure and build, Jobs 2, preset `local-debug`. Qt GuiPrivate warning and missing WrapVulkanHeaders are pre-existing.

```text
.\scripts\build.ps1 -Action Configure -Jobs 2
.\scripts\build.ps1 -Action Build -Targets SignalStudioDetectionPostprocessTests,SignalStudioSsDeRpoTrackingTests -Jobs 2
```

Both executables linked under `out/vs2026-qt611-debug_bin/`. That `out/` is this worktree only.

Re-run after restoring non-owned files:

```text
SignalStudioSsDeRpoTrackingTests.exe
SS-DE-RPO tracking broad-phase tests passed
TRACK_EXIT 0

SignalStudioDetectionPostprocessTests.exe
An open set whose comparisons exceed the explicit limit of 1 fails the frame
POST_EXIT 1
```

The old suite stops at that first check. Later checks in the same file were not executed. The owned sparse scene reports `comparisons == 1`, `openTracks == 3071`, `finish == 3071` against raw `C(3072,2) = 4717056`. The tolerance-2 chain `[0,10] [12,22] [24,34]` reports `comparisons == 3` and one envelope `[0,34]`. Four mutual overlaps report `comparisons == 10`.

No GPU inference, no TensorRT, no native 4K, no 159/363/2044 replay, no large RFbench, no bulk hashes. Main shared `out/` binaries were not touched.

## Interfaces for main / G

Register the owned test. Do not leave that registration to this commit. Place it beside `SignalStudioDetectionPostprocessTests`:

```cmake
add_executable(SignalStudioSsDeRpoTrackingTests tests/ss_de_rpo_tracking_tests.cpp)
ss_target_defaults(SignalStudioSsDeRpoTrackingTests)
set_target_properties(SignalStudioSsDeRpoTrackingTests PROPERTIES AUTOMOC OFF)
target_link_libraries(SignalStudioSsDeRpoTrackingTests PRIVATE SignalStudioCore)
add_test(NAME signal_studio_ss_de_rpo_tracking COMMAND SignalStudioSsDeRpoTrackingTests)
```

Retarget `tests/detection_postprocess_tests.cpp` without dropping the safety checks:

- `trackingLimitsAndCancellationAreAtomic`: carriers `[-60,-50]`, `[0,10]`, `[50,60]` with `maximumFusionComparisons = 1` and `mergeToleranceHz = 0` now have zero relevant pairs, so `consume` succeeds, `processedWindows() == 1`, and `finish` returns 3 tracks. The following empty-publish check then fails for that tracker. Keep the atomic failure by using mutually overlapping carriers such as `[0,10]`, `[9,19]`, `[18,28]`. That scene still returns `frequency-fusion-comparison-limit-exceeded`, `processedWindows() == 0`, `retainedTracks() == 0`, and an empty `finish`.
- `sealedBurstsDoNotReenterFusion`: expect `afterGap.comparisons == 0`. The two new carriers `[40,42]` and `[80,82]` do not overlap, and sealed history is still not scanned. Keep `sealedTracks == 30`, `openTracks == 2`, `finish` size 32, empty windows, and `hits == 2`. This assertion was not reached by the failing run.

`domain/detection_refined_fusion.cpp` `fuseRefinedDetectionEvents` is still a Cartesian scan with its own 4,000,000 cap. It is not owned here and was not prefiltered from `detection_refined_publication.cpp`.

## Limitations

Comparison counts of frequency-irrelevant pairs changed. Match predicate, greedy association order, and fusion min-key did not. `frameStats().comparisons` is the last successful frame's fusion candidates only. Production projection still leaves `matchIou` at 0.3. CTest will not see the new executable until main registers it.
