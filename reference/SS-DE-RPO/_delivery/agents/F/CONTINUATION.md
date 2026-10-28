# SS-DE-RPO · F continuation

- Agent: F
- Branch: `codex/ss-de-rpo-f`
- Parent: `7220b2e295963288cb5d11e76946bb77646be0b0`
- Worktree: `D:\project\ss-de-rpo-wt\continue-f`
- This file is inside the delivering commit. The commit SHA is `git rev-parse HEAD` after that commit.
- Not pushed. GPU forward was not run.

## Changed files

- `infrastructure/detection_model_backend.h`
- `infrastructure/detection_model_backend.cpp`
- `tests/ss_de_rpo_gpu_tests.cpp`
- this handoff

`DetectionModelResourceLimits::maximumExecutionBatch` stays 2. No worker, task, UI, or CMake edit.

## Behavior

`planDetectionDeviceExecution` is the pure model of the load retry. `selectDetectionExecutionBatch` still chooses the tier. `freeDeviceBytes == 0` still means unknown VRAM and does not shrink the tier. A known shortfall, or the test allocator, downshifts 16/8/4/2 and retries the same windows. Batch 1 still failing rejects the load. The error says no window is skipped.

`DetectionModelRuntimeHooks::failDeviceAllocation` is empty in production and is not called. If a test sets it and it returns true, initialize fails before `cudaMalloc`. The text starts with `Injected device allocation failure`. That is not a CUDA OOM. `allocationFailureKind` is `injected` only when a later successful attempt kept that text in `batchAdaptationReason`.

`DetectionModelRuntimeHooks::cancelled` is empty in production. If it returns true, load/infer stop, `retryableResourceFailure` is cleared, and the attempt destructor releases device buffers. This is not a batch downshift.

Failed `cudaMalloc` no longer keeps a possibly non-null pointer. `releaseCudaResources` clears the sticky CUDA error, binds the selected device, synchronizes, destroys the context, then `cudaFree`s. `reset()` and a failed load attempt use that path.

After the final profile bind, a known free-byte value that cannot hold `bytesPerUnitBatch * physicalBatch + workspace` rejects that shape before `cudaMalloc`.

## Evidence

Successful load still writes the old keys (`deviceIndex`, `requestedMaximumBatch`, `logicalMaximumBatch`, `physicalBatch`, `optimizationProfile`, `deviceFreeBytesBeforeAllocation`, `deviceTotalBytes`, `deviceWorkspaceBytes`, `boundedDeviceBytes`). It also writes:

| Key | Value |
|---|---|
| `cudaDeviceIndex` | same integer as `deviceIndex`: the `cudaSetDevice` index |
| `requestedBatch` | request batch |
| `logicalBatch` | FFSCN physical tier, or the SCN enqueue count |
| `profileMin` / `profileOpt` / `profileMax` | raw rank-4 engine profile dims, not reordered |
| `freeDeviceBytes` / `totalDeviceBytes` | decision-time `cudaMemGetInfo`, decimal strings |
| `reservedDeviceBytes` | final workspace |
| `neededDeviceBytes` | unit batch bytes times physical batch, plus workspace |

A profile shape that is not rank 4 fails the load. SCN logical batch stays the enqueue count; physical batch stays 1.

## Checks

Pure helper, no CUDA call. CMake was a scratch project, not the repo `CMakeLists.txt`. Qt: `D:\softwares\Qt\6.11.1\msvc2022_64`.

```text
cmake -S temp\ss-de-rpo-f-gpu-helper -B temp\ss-de-rpo-f-gpu-helper\build -G "Visual Studio 18 2026" -A x64 -DQt6_DIR=D:\softwares\Qt\6.11.1\msvc2022_64\lib\cmake\Qt6
cmake --build temp\ss-de-rpo-f-gpu-helper\build --config Debug --parallel 2
```

`ss_de_rpo_gpu_tests.exe` exit 0. Passed: default batch 2, empty production hooks, profile dim copy/reject, downshift step, SCN logical 2 with physical 1, worker ceiling 2, unknown VRAM still records bytes, evidence JSON fields, VRAM select batch 1, batch-1 unfit, injected 2→1, injected batch-1 reject, cancel before allocation, cancel after a rejected tier, illegal tier, needed overflow.

Worktree compile, not a GPU run:

```text
scripts/build.ps1 -Action Configure -Preset local-debug
scripts/build.ps1 -Action Build -Preset local-debug -Jobs 2 -Targets SignalStudioDetectionWorker
```

Exit 0. `SignalStudioDetectionWorker.vcxproj` Debug defines `SS_DETECTION_TENSORRT=1`, and `detection_model_backend.cpp` compiled. `out/vs2026-qt611-debug_bin/SignalStudioDetectionWorker.exe` was linked. Pre-existing C4267 at `prepareDetectionModelInput` and warnings in other infrastructure files were not changed.

## Not run

`--live` and `--adapted` were not started. No TensorRT forward, no real `cudaMemGetInfo`, no engine profile readback. Those results stay `UNVERIFIED_GPU` until main runs them. The pure tests do not prove device release or a real OOM.

## Interfaces for main

Register `tests/ss_de_rpo_gpu_tests.cpp` as its own executable. It only needs the repo include path and `Qt6::Core`. It does not link `detection_model_backend.cpp` and must not define `SS_DETECTION_TENSORRT`. Do not pass `--live`.

D already copies `physicalBatch`, `logicalMaximumBatch`, `requestedMaximumBatch`, and `optimizationProfile` from worker evidence. The new keys are on `DetectionModelBackend::evidence()` and therefore on the worker ready payload. `detection_task.cpp` was not edited, so `modelProfile` does not yet copy `cudaDeviceIndex`, `profileMin`, `profileOpt`, `profileMax`, `freeDeviceBytes`, `totalDeviceBytes`, `reservedDeviceBytes`, `neededDeviceBytes`, or `allocationFailureKind`.

The worker still constructs `DetectionModelBackend` with default hooks. In-process cancel and the injected allocator run only when those hooks are set. A parent cancel during load still kills the worker process. `tools/detection_worker/main.cpp` was not changed.
