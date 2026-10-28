# Continuation integration checks

These are directed checks of the integrated changes, not the final full acceptance verdict.

## Resource / GPU helpers

Debug `signal_studio_ss_de_rpo_logic`, `signal_studio_ss_de_rpo_resource`, `signal_studio_ss_de_rpo_gpu`: 3/3 PASS, 0.56 s. Runtime resource JSON now uses Qt infrastructure serialization, removing the handwritten JSON parser from the Qt-independent domain layer. Log: `out/tasks/ss-de-rpo/N0/logs/integrated-resource-tests.txt`.

## Tracker / refined fusion

Debug `signal_studio_detection_refined_fusion`, `signal_studio_detection_postprocess`, `signal_studio_ss_de_rpo_tracking`: 3/3 PASS, 1.77 s. Refined fusion has ten groups including 3072 disjoint bands (4,717,056 Cartesian pairs) with zero relevant comparisons. Tracker oracle/rollback/tie/boundary tests passed. Log: `out/tasks/ss-de-rpo/N0/logs/tracking-tests.txt`.

The existing tracker exhaustion case now uses truly overlapping intervals, retaining the same explicit failure and atomic-empty-output assertions. The sealed-history case expects zero comparisons of two disjoint live carriers; no safety assertion was removed. Main additionally indexed post-refinement fusion; that path was outside E's original write scope.

## Provisional start

Debug `signal_studio_ss_de_rpo_ui`, `signal_studio_ss_de_rpo_start`: 2/2 PASS, 23.88 s. Main connected the card click timestamp and provisional cancellation to the panel. Qt Test DLL is now staged for both RPO executables, allowing tests without development Qt PATH. Log: `out/tasks/ss-de-rpo/N0/logs/start-integration-tests.txt`. These are fixture/offscreen UI cases, not native 4K or GPU inference.

## Actual TensorRT

`SignalStudioDetectionModelBackendTests --adapted` ran on RTX 5060 Laptop with the deployed FFSCN/SCN engines and existing dynamic-shape golden tensors. Shapes and selected-spectrum forwards passed. This is real GPU inference over controlled input tensors, not a claim of field-RF accuracy. Original-IQ pipeline coverage is separately recorded by the 20-window benchmark.

Main added actual backend allocation fault tests, not only the pure retry model. Injected B2 failure retried B1 and executed both original input windows. Injected B1 failure rejected load without ready evidence. The cancellation test caught `stopIfCancelled` incorrectly returning the false result of `fail()`; main corrected it to return true after setting the error.

First actual GPU run failed at cancellation and is retained as `out/tasks/ss-de-rpo/N0/logs/integrated-adapted-real-gpu.txt`. Rebuilt fixed run `integrated-adapted-real-gpu-02.txt` ended with all injected checks and `Detection model backend checks passed`, exit 0. Injected allocation failure is not labelled real CUDA OOM.

## Resident benchmark harness

The same QCoreApplication process executes an excluded warmup and subsequent re-detection with --resident-repeat. Every run still creates its own Worker/context and isolated formal project. The smoke run passed warmup plus one measured re-detection, both formal commit/reopen PASS. This verifies the harness; final exclusive three-cold/three-warm performance comparison remains pending. Log: `out/tasks/ss-de-rpo/N0/diagnostics/resident-pilot-01/resident-summary.json`.
