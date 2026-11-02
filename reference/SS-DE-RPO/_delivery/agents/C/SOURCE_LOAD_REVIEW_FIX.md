# C4 source-load publication fix

- Agent: C4, Grok 4.7 Fast / xhigh, no subagents, no push.
- Worktree: `D:\project\ss-de-rpo-wt\continue-c`
- Baseline: `83e365f4c7b621809a66852e6d7727009a311db3`
- Report: `D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/H_REVIEW_83e365f_FAIL.md` P1.
- Owned files: `app/main_window.cpp`, `app/main_window.h`, `tests/ss_de_rpo_start_tests.cpp`, this file.
- The commit SHA is the `git rev-parse HEAD` in the handoff. It is not written here.

## Defect

`MainWindow::pollSourceLoads` runs every 80 ms. It wrote `availableSamples`, `navigationEnvelope`, and `view`, and on `snapshot.finished` it set Ready, Partial, or Failed and erased that loader. It did not consult the detection project-write lock.

`startRequest` copies the project, and the worker later saves that copy. `finishStartCommit` assigns it back on the success path and on the same-project clearance path. `beginPublicationCommit` copies the project again, and `finishPublicationCommit` assigns that copy when `installed` is true. Both assigns happen on the UI thread before `commitInFlight_` is cleared.

If the poll observed a finished read first, the loader was already gone. The assign restored the captured Loading prefix. Nothing restarted the read. A later save stored that short prefix, and `openProject` resumes from `availableSamples()`, so a short prefix stays Partial. The active file can be Ready while another file is still loading. Measured start-commit time of about 98-174 ms makes the 80 ms poll able to land inside the lock.

## Fix

While `DetectionPanel::projectCommitInProgress()` is true, `pollSourceLoads` does not mutate the project, advance `LoadJob::previous`, or erase a loader whose file and project generation are still current. A generation change or a missing file still drops that loader. Shutdown and `closeProject` still clear `sourceLoads_` without applying.

`projectCommitInProgress()` is the same `commitInFlight_` condition as the session write guard. The poll does not call `acceptProjectWrite`, because that guard emits a user-visible reject message. The poll does not call `noteProjectWrite`. That would move `contentEpoch` and make `stillCurrent` reject the in-flight start or publication.

After the UI-thread snapshot install drops the lock, the next poll is the normal production path. It applies the loader's current snapshot, including a read that already finished, and does not call `startSourceLoad` again. Cancelled and failed commits also drop the lock without installing a snapshot; the same later poll applies the retained loader.

The detection transaction can still write the prefix it captured when the commit began. Memory is repaired on the next poll. The following project save is what a later open reads. This change does not add a second save inside the detection transaction.

## Reproduction before the fix

Isolated Debug binary, production poll unchanged except the fixture seam. Three new rows, semicolon-separated `SS_UI_CASES`. Each failed at the locked poll:

`project prefix overwritten status=0 samples=20000 envelope=1250 generation=1`

`LoadStatus::Ready` is 0. The real second-file reader had finished 20000 samples and 1250 envelope points, and the locked poll published that into the project. Qt totals: 2 passed, 3 failed, 7641 ms. CTest exit 8. Log: `out/tasks/ss-de-rpo/C4/logs/repro-before-fix-qt.txt`.

## Checks after the fix

Same isolated tree. No `out/vs2026-qt611-*` build or deploy.

1. Configure and first build, before the poll change:
   `cmake -S . -B out/tasks/ss-de-rpo/C4/local-builds/debug -G "Visual Studio 18 2026" -A x64 -DSS_QT_ROOT=D:/softwares/Qt/6.11.1/msvc2022_64 -DSS_DETECTION_TRT_ROOT=D:/project/signal_studio_rebuild/out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11 -DSS_BUILD_TESTS=ON -DSS_DEPLOY_RUNTIME=ON -DSS_OUTPUT_PREFIX=tasks/ss-de-rpo/C4/runtime`
   then `cmake --build out/tasks/ss-de-rpo/C4/local-builds/debug --config Debug --target SignalStudioSsDeRpoStartTests --parallel`
   Exit 0. Log: `out/tasks/ss-de-rpo/C4/logs/configure-build-repro.txt`.

2. Targeted CTest before the fix, exit 8. Log: `out/tasks/ss-de-rpo/C4/logs/repro-before-fix.txt`.

3. Rebuild after the poll change. Exit 0. `main_window.cpp` recompiled. Log: `out/tasks/ss-de-rpo/C4/logs/build-after-fix.txt`. oneMKL staging skipped because `mkl_rt.3.dll` is absent. windeployqt warned that `dxcompiler.dll` / `dxil.dll` were absent. Both are existing deploy notes. Runtime output stayed under `out/tasks/ss-de-rpo/C4/runtime-debug_bin`.

4. Targeted rows after the fix. CTest exit 0, 11.67 s. Qt log `out/tasks/ss-de-rpo/C4/logs/targeted-after-fix-qt.txt`: the three new rows passed, totals 5 passed, 0 failed, 11440 ms.

5. Full `signal_studio_ss_de_rpo_start` with `SS_UI_CASES` unset. CTest exit 0, 38.31 s. Qt log `out/tasks/ss-de-rpo/C4/logs/start-ctest-after-fix-qt.txt`, totals 13 passed, 0 failed, 38074 ms. Passed rows include the previous start rows, both `acceptedStartContextChangeNeverRestoresFormal` rows, the modal rename row, and the three new rows:
   - `sourceLoadSurvivesStartCommitInstall`
   - `sourceLoadSurvivesPublicationCommitInstall`
   - `cancelledStartCommitAppliesFinishedSourceLoad`

The new rows use two real int16 IQ files. The active file is Ready. The second file's `SourceLoader` is waited on until `snapshot().finished` and `loaded == sampleCount`. The 80 ms timer is stopped only through the fixture seam. The asserted poll is `pollSourceLoads`. Gates are the existing start-completion gate, publication gate, and start-commit gate. While the lock is held, the poll must leave Loading, `availableSamples == 0`, an empty navigation envelope, and the finished loader alive, and must not change `contentEpoch`. After release, the start and publication rows still show that captured Loading prefix, then one poll reaches Ready, the original sample count, and a non-empty envelope without incrementing the load generation. Save plus `ProjectFileCodec::load` and `openProject` keep that full prefix. The cancel row rejects the start, leaves no committed run, and the following poll applies the same loader. No sleep and no file-size race.

## Limits

Debug `signal_studio_ss_de_rpo_start` only. Release, the rest of CTest, GPU, native 4K, and performance were not run. The offscreen run warned that the isolated output directory has no Qt font directory. That warning was already present on the failing run and did not fail the totals. Main integrates and does the full acceptance and the next independent review.
