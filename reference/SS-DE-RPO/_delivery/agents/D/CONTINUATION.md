# D continuation

Parent-process preparation can be cancelled, one immutable source plan is reused inside a run, and the execution timeline names shard flush separately from GUI project publication.

## Changed files

- `infrastructure/detection_model_snapshot.h`
- `infrastructure/detection_model_snapshot.cpp`
- `infrastructure/detection_source.h`
- `infrastructure/detection_source.cpp`
- `infrastructure/detection_task.h`
- `infrastructure/detection_task.cpp`
- `infrastructure/detection_worker_client.h` (comment only: one process per run, no retained engine or CUDA context)
- `tests/ss_de_rpo_pipeline_tests.cpp`
- this handoff

`CMakeLists.txt` was registered only in this worktree's build and then restored. Main registers the test.

## Interfaces

```text
bool freezeDetectionModelSnapshot(DetectionRequest&, QString& error,
                                  const std::function<bool()>& cancel = {});
bool prepareQueuedRun(directory, metadata, request, out, error,
                      const std::function<bool()>& cancel = {}) const;
bool makeDetectionSourcePlan(..., projectGeneration = 0,
                             const DetectionSourceCancel& cancel = {});
bool FixedDetectionSource::open(..., projectGeneration = 0,
                                const DetectionSourceCancel& cancel = {},
                                const DetectionSourcePlan* prepared = nullptr);
```

Empty `cancel` keeps existing callers. A true callback fails closed and does not start a worker. Stable errors:

- `Cancelled during model snapshot` (request fields unchanged, including Fixture)
- `Cancelled during detection preparation`
- `Cancelled during source planning`
- `Cancelled during source validation`
- `Prepared source plan no longer matches the source, channel, prefix, grid, model, or config`

`prepareQueuedRun` stores `QueuedDetectionRun::sourcePlan` and `hasSourcePlan`. `execute` copies that plan onto the job and opens with `plan.config` after `detectionPreparedSourcePlanMatches`. `quickFingerprint` is not part of that match: queue copies the live size/mtime onto the job, and `open` still runs `validateSource`. A mismatch does not plan a different grid. Preflight may raise `request.rawHeadBudgetBytes` after planning; opening with `plan.config` keeps that from looking like a grid change.

Successful plans only go into a process-local LRU of 8. The key is source id/path, sample format and selected channel, availability/prefix, requested range, project generation, and every `DetectionSourceConfig` field. Failures, IQ bytes, and worker state are not cached. Test helpers: `detectionSourcePlanCacheStatsForTest`, `clearDetectionSourcePlanCacheForTest`.

`noteSteadyMark` is const. Marks D records when the stage actually happens:

- `t_model_snapshot_ready` when the profile is Fixture or C already froze `modelManifestPath` and `modelSha256`
- `t_preflight_start`, `t_source_plan_ready`, `t_preflight_end`
- `t_manifest_committed` once, on queue, or adopted from `persistQueuedRun` after that write succeeds
- `t_source_open`, `t_first_model_input`, `t_inference_complete`
- `t_first_candidate_decoded`, `t_first_preview`, `t_frequency_complete` only when those non-fixture results exist
- `t_final_shard_persisted` after the non-fixture final frequency shard flush
- `t_terminal` inside `finish`, before the persisted performance snapshot

C still owns `t_click`, `t_accepted`, and `t_durable_publication`. D does not write `t_durable_publication`. Absent stages stay absent. CUDA events stay in `tensorRtInference`. Each run constructs a new `DetectionWorkerClient`; `finish` stops that process. There is no worker pool.

## Main CMake registration

```cmake
add_executable(SignalStudioSsDeRpoPipelineTests tests/ss_de_rpo_pipeline_tests.cpp)
ss_target_defaults(SignalStudioSsDeRpoPipelineTests)
set_target_properties(SignalStudioSsDeRpoPipelineTests PROPERTIES AUTOMOC OFF)
target_link_libraries(SignalStudioSsDeRpoPipelineTests PRIVATE SignalStudioInfrastructure)
add_test(NAME signal_studio_ss_de_rpo_pipeline COMMAND SignalStudioSsDeRpoPipelineTests)
set_tests_properties(signal_studio_ss_de_rpo_pipeline PROPERTIES TIMEOUT 120)
add_dependencies(SignalStudioSsDeRpoPipelineTests SignalStudioDetectionWorker)
```

Place it after `signal_studio_ss_de_rpo_logic`. The observed Debug run was 1.14 s; the timeout leaves room for the worker.

## Checks

Worktree `D:/project/ss-de-rpo-wt/continue-d`, preset `local-debug`, Jobs 2.

```text
scripts/build.ps1 -Configuration Debug -Action Configure
scripts/build.ps1 -Configuration Debug -Action Build -Jobs 2 -Targets SignalStudioSsDeRpoPipelineTests,SignalStudioDetectionTaskTests,SignalStudioDetectionSourceTests,SignalStudioSsDeRpoLogicTests,SignalStudioDetectionPipelineTests,SignalStudioDetectionWorker
scripts/build.ps1 -Configuration Debug -Action Test -TestRegex signal_studio_ss_de_rpo_pipeline|signal_studio_detection_task|signal_studio_detection_source|signal_studio_ss_de_rpo_logic|signal_studio_detection_pipeline
```

Configure and the targeted Debug build exited 0. The first pipeline run failed in the test fixture: Fs 10 kHz with the default merge frequency 100 kHz was rejected before the second cancel checkpoint. The test now sets `mergeFrequencyHz` inside the source rate. The rerun was:

```text
signal_studio_detection_pipeline     Passed  4.29 s
signal_studio_detection_task         Passed 35.45 s
signal_studio_ss_de_rpo_logic        Passed  0.40 s
signal_studio_ss_de_rpo_pipeline     Passed  1.14 s
signal_studio_detection_source       Passed  4.75 s
100% tests passed, 0 tests failed out of 5
```

The pipeline test covers snapshot and prepare cancellation, cache equality, channel/prefix/grid/SCN/candidate/path invalidation, eviction at 8, same-run prepared open, live fingerprint rejection, one `t_manifest_committed` after persist-then-queue, monotonic fixture timeline, and two sequential worker process ids.

## Limitations

- No GPU inference and no TensorRT evidence. Fixture runs do not emit `t_final_shard_persisted`, `t_frequency_complete`, `t_sample_refine_complete`, `t_first_candidate_decoded`, or `t_first_preview`.
- C is not wired to pass its StartCommit token. Default `{}` keeps current C call sites compiling.
- Debug only. Full CTest, Release, and the 4K screen were not run.
- This worktree's oneMKL deploy was skipped because `out/toolchains/fft-libs/conda/Library/bin/mkl_rt.3.dll` is absent. CPU DSP fallback behavior is unchanged.
- The plan cache is process-local and holds plans, not IQ or worker state. File identity, sample count, and the live fingerprint check remain in `open`.
- Not pushed.
