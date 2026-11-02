# G continuation

Worktree `D:\project\ss-de-rpo-wt\continue-g`, branch `codex/ss-de-rpo-g`.
Base before this handoff commit: `802054896707a07cb2e29a5cb8d408a53b45f6ec`.
Assigned model: grok-4.7-build-fast / xhigh.
No push. No GPU. No production edits. No CMake edits. No writes under `D:\project\signal_studio_rebuild\out`.

## Owned files

- `tests/ss_de_rpo_logic_tests.cpp`
- `tests/ss_de_rpo_ui_tests.cpp`
- `tests/ss_de_rpo_acceptance_tests.cpp`
- `scripts/verify-ss-de-rpo.ps1`
- `scripts/benchmark-ss-de-rpo.ps1`
- `scripts/summarize-ss-de-rpo.py`
- this file

`SignalStudioSsDeRpoAcceptanceTests` is still absent from CMake. The Debug executable was compiled in this worktree with AUTOMOC off and linked to `SignalStudioInfrastructure` plus `SignalStudioCore`. Its default path does not launch a Worker.

## Report-builder fix

`[ordered]@{ ... suiteError = $null ... }` threw `System.ArgumentException: Argument types do not match` after the tests had finished, so earlier verify directories have case logs and no `matrix.json`. The script now creates an empty ordered map and assigns each key. A missing suite error is stored as an empty string. Rows are assigned from `List.ToArray()`.

## Commands and results

Debug logic and UI build, exit 0. Sources are older than the executables used below.

```text
$env:SS_QT_ROOT = 'D:\softwares\Qt\6.11.1\msvc2022_64'
& D:\project\ss-de-rpo-wt\continue-g\scripts\build.ps1 -Configuration Debug -Action Build -Jobs 2 -Targets SignalStudioSsDeRpoLogicTests,SignalStudioSsDeRpoUiTests
```

Log: `C:\Users\Admin\.grok\sessions\D%3A%5Cproject%5Css-de-rpo-wt%5Ccontinue-g\766fc7f9-aaeb-4b7b-962a-7d6c1b9bc328\terminal\call-19978aa6-c18d-4733-85eb-faea8fb400d2-100.log`. Pre-existing C4458/C4189 warnings were left in place.

Acceptance compile, `cl-exit=0`. `/Fd` is `out\tasks\ss-de-rpo\G\acceptance-obj\acceptance.pdb`. The link line is in the previous session command; the binary is `out\vs2026-qt611-debug_bin\SignalStudioSsDeRpoAcceptanceTests.exe`.

Logic tests, exit 0. Log: `out\tasks\ss-de-rpo\G\manual\logic.txt`. Every printed `R-*` and `publication-*` line is PASS, including R-01 at a 6 GiB grant with the cell ceiling, R-04 raw-OFF preflight, R-05 retain-true 401866752 bytes, R-12 nondurable, R-15 CPU selector, and both publication prefixes. `legacyCombinedEvidence` is set only on in-memory C++ request objects. It is not written into public request JSON.

Acceptance matrix, exit 1. Resident stub, exit 2. Logs: `out\tasks\ss-de-rpo\G\manual\acceptance.txt`, `acceptance-matrix.json`, `resident.txt`.

```text
& out\vs2026-qt611-debug_bin\SignalStudioSsDeRpoAcceptanceTests.exe --matrix-out out\tasks\ss-de-rpo\G\manual\acceptance-matrix.json
& out\vs2026-qt611-debug_bin\SignalStudioSsDeRpoAcceptanceTests.exe --resident-redetect
```

- `R-06-20 PASS windows=20 prefixSeconds=0.02625 endSample=525000 physicalSamples=16000000 ffscn-gui-iq-v1 hop=0.00025 fft=65536 frames=10 hopFrames=5 schema=2` catalog `user-failure-run-records.json#11`
- `R-06-159 PASS windows=159 endSample=11896001` catalog `#0`, hop `0.0001`, fft `4096`
- `R-06-363 PASS windows=363 endSample=13936001` catalog `#0`, hop `0.0001`, fft `4096`
- `R-06-2044 FAIL` max full-file plan 639. Last profile search plans 142 windows. No repeated synthetic frames.
- `--resident-redetect` prints `R-19-warm BLOCKED` and returns 2 before planning. No Worker.

The real wav is `D:/coding/FFSCN_train2/FFSCN_TEST_GUI/test_data/data/20260728-045101-650_bw_20000000_sampleTime_0.8_rollOff_0.35.wav` (16000000 samples, Fs 20000000, header 44). Prefix `0.02625 s` is 525000 samples. Counts come from production `SourcePlan`.

Offscreen UI, process exit 1, about 12.5 s. Functional evidence only.

```text
$env:PATH = 'D:\softwares\Qt\6.11.1\msvc2022_64\bin;' + $env:PATH
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_SCALE_FACTOR = '1.5'
$env:QT_QPA_FONTDIR = 'C:\Windows\Fonts'
& out\vs2026-qt611-debug_bin\SignalStudioSsDeRpoUiTests.exe -o out\tasks\ss-de-rpo\G\manual\ui-offscreen.txt,txt -v2
```

`evidence=functional-offscreen name=offscreen geometry=2560x1440 dpr=n/a window=2560x1440`. Twenty Paint samples. `paintP95Ms=140.638 handlerP95Ms=16.3692`. The slot FAIL is `paintP95 < 100.0`. Handler time is not the metric. `provisionalCancelBeforeCommit` PASS. Qt6Testd.dll is not deployed; the Qt bin directory is required on PATH.

Native UI, process exit 0, about 14.6 s. `QT_QPA_PLATFORM` and `QT_SCALE_FACTOR` were removed. Same PATH, font directory, and `-o ...,txt -v2`. Log: `out\tasks\ss-de-rpo\G\manual\ui-native.txt`.

Candidates: `TL160ADMP36 geometry=0,0,1707x1067 dpr=1.5000` and `Redmi 27 NU geometry=2560,0,2560x1440 dpr=1.5000`. Selected only `Redmi 27 NU`. `evidence=native-fullscreen window=2560x1440`. The QScreen name is `Redmi 27 NU`; the log does not print `DISPLAY6`. Twenty Paint samples. `paintP95Ms=81.3916 handlerP95Ms=23.6782`. Paint and provisional cancel PASS. This run is outside the offscreen verify matrix.

Blocked benchmark, exit 2. The first attempt into `benchmark-blocked` exited 1 because that directory was not empty.

```text
& scripts\benchmark-ss-de-rpo.ps1 -Configuration Debug -Mode Both -OutputDirectory out\tasks\ss-de-rpo\G\benchmark-blocked-r2
```

`launcher.json` schema `signal-studio-ss-de-rpo-benchmark-launch-v1`. `gpuExecuted=false`, `residentApplication=false`. Cold status `BLOCKED_EXCLUSIVE_SLOT`, arguments end with `--formal-commit`, and do not include `--raw-head-budget-mib`. `t_pipeline_terminal` and `t_terminal` are both null. Warm protocol is one process, `--resident-repeat 3` and `--formal-commit`. No process was started. `Assert-PublicRawOff` accepted `D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/N0/request-raw-off-v2.json`: no `legacyCombinedEvidence`, `retainRawModelOutput=false`, `rawHeadBudgetBytes` string-compares as `0`, hop `0.00025`, fft `65536`, frames `10`, hop frames `5`.

Verify after the report-builder fix, exit 3. This re-ran logic, offscreen UI, and acceptance. It did not rebuild and did not run the native screen.

```text
pwsh -NoProfile -File scripts\verify-ss-de-rpo.ps1 -Configuration Debug -Jobs 2 -SkipBuild -OutputDirectory out\tasks\ss-de-rpo\G\verify-debug-r4
```

Host line: `matrix=...\verify-debug-r4\matrix.json fail=True blocked=True` then `EXIT:3`. `allowGpu=false`, `fullCTest=false`, `gpuExecuted=false`, `nativeR19Pass=false`, `workerLaunched=false`, `syntheticRepeatedFrames=false`, `suiteError=""`, 77 rows.

The offscreen paint sample inside this verify run is a new observation: `n=20 paintP95Ms=144.61 handlerP95Ms=20.7465`, FAIL at 144.610 ms. The earlier manual offscreen sample remains 140.638 ms. Both miss the 100 ms target. The matrix uses 144.610 ms.

Summary, both speedups unavailable:

```text
D:\profiles\anaconda\python.exe scripts\summarize-ss-de-rpo.py --benchmark out\tasks\ss-de-rpo\G\benchmark-blocked-r2\launcher.json --matrix out\tasks\ss-de-rpo\G\verify-debug-r4\matrix.json --baseline reference\SS-DE-RPO\_delivery\performance\BASELINE_COLD.json --output out\tasks\ss-de-rpo\G\summary-r1\summary.json
```

Printed `coldSpeedup=false`, `warmSpeedup=false`. The blocked cold row is unclassified and has no elapsed time. No Worker pid was invented. Baseline elapsed in that JSON is read from the existing baseline file; it is not a new G measurement.

Worktree root files named `$out` and `vc140.pdb` were absent after the acceptance compile and again before this handoff (`root-junk=0`).

## Matrix from verify-debug-r4

| Id | Status |
| --- | --- |
| R-01 | PASS |
| R-02 | PASS |
| R-03 | PASS |
| R-04 | PASS |
| R-05 | PASS |
| R-06 | FAIL |
| R-07 | UNVERIFIED |
| R-08 | PASS |
| R-09 | PASS |
| R-10 | UNVERIFIED |
| R-11 | BLOCKED_EXCLUSIVE_SLOT |
| R-12 | PASS |
| R-13 | UNVERIFIED |
| R-14 | UNVERIFIED |
| R-15 | UNVERIFIED_GPU |
| R-16 | UNVERIFIED |
| R-17 | BLOCKED_EXCLUSIVE_SLOT |
| R-18 | UNVERIFIED |
| R-19 | FAIL |
| R-20 | UNVERIFIED |
| CTest-Debug | BLOCKED_NOT_RUN |

R-06 FAIL is the 2044-window row. R-19 FAIL is the offscreen visible-paint p95. R-08 and R-09 PASS are offscreen functional results. R-12 PASS is the unit publication-prefix and nondurable checks, not a durable GUI publication. R-15 PASS children are the CPU selector only. Exit 0 requires every R-01..R-20 and the requested CTest row to be PASS, so this suite is not a PASS.

## Interfaces for main

- Visible-paint loop uses `DetectionPanel::setStartCommitGate` / `startCommitGateBlocked`, then `cancelRun`, then waits until the gate is clear and no start commit or latch remains. It does not call `noteStartRejected` and it does not disconnect `requestSubmitted`.
- Benchmark cold runs are fresh processes with `--formal-commit`. Warm is one `QCoreApplication` and `--resident-repeat N`. Do not pass `--raw-head-budget-mib 0`.
- Read `t_pipeline_terminal` and `t_terminal` only when a timeline mark has that exact name. `infrastructure/detection_task.h` still defines `kDetectionMarkTerminal` as `t_terminal`. A missing stamp stays null.
- Native layout selects the only QScreen at 3840x2160 and 1.5, then fullscreen logical 2560x1440. Offscreen stays labelled functional.

## Remaining main acceptance

- Register the acceptance target in CMake. Keep the manual executable until that lands.
- Release build and Release matrix.
- `-FullCTest` for the requested configurations.
- Exclusive GPU slot: three fresh `--formal-commit` processes, one resident `--resident-repeat 3` parent with a fresh Worker per measured run, physical resource samples, `model-backend --live` and `--adapted`, and `verify-sample-ux-r6.ps1`.
- QRhi display and software-fallback evidence.
- Formal durable publication: new isolated `.ssproj`, source compare, atomic save, reopen. The N0 request file stays untouched.
- R-07, R-10, R-13, R-14, R-16, R-18, and R-20 have no owned executable coverage.
- Offscreen preparing-paint p95 is above 100 ms. The separate native Redmi run is below 100 ms and is not in `nativeR19Pass`.
- 2044 windows on this 0.8 s file stays FAIL at max plan 639.
- Independent H review of the exact committed HEAD. G does not issue that review. Do not push from this worktree as part of this handoff.
