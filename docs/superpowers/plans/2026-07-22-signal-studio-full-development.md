# Signal Studio Full Development Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use subagent-driven-development to implement this plan task-by-task, with specification review before code-quality review for every milestone.

**Goal:** Deliver Signal Studio V1.0.0 as a production-quality Windows Qt/C++ application and reusable ten-module platform, satisfying the approved BL1.0 requirements, using the supplied real data, and publishing verified installer/portable artifacts through GitHub.

**Architecture:** A C++20/CMake superbuild exposes only the ten approved public modules (`Core`, `Data`, `DSP`, `Compute`, `TaskRuntime`, `Visualization`, `Workbench`, `PluginSDK`, `ModelRuntime`, `Dataset`). `signal_studio_app` composes those services; Qt remains private to `Visualization` and `Workbench`; optional backends are selected by capability discovery. Immutable data-source versions, half-open 64-bit sample ranges, typed view requests, cancellable tasks, content-addressed caches, and stale-result rejection are cross-cutting invariants.

**Tech Stack:** C++20, CMake/Ninja/MSVC 2022, Qt 6.11.1 Widgets, vcpkg manifest at the approved baseline, oneMKL/Eigen/TBB, HDF5, ONNX Runtime, nlohmann-json, toml++, pybind11, spdlog/fmt, GoogleTest/Benchmark, CPack/NSIS, PowerShell, Python, GitHub Actions.

---

## Fixed inputs and acceptance policy

- Approved documentation source: `../Signal_Studio_开发文档/Signal-Studio-Dev-Docs` (BL1.0, 198 requirements).
- Real data source: `../test_data`; large sample files stay external and are identified by size and SHA-256.
- Formal baseline is copied verbatim to `docs/baseline/Signal-Studio-Dev-Docs` and treated as read-only.
- Each milestone ends only after its scoped build and tests pass, its four evidence documents are written, and a Git commit is created.
- No test, performance, GPU, stability, or release result may be claimed without captured evidence. An unavailable optional capability is recorded as an environment deviation and must degrade cleanly.
- The final gate is the checklist in `CODEX_FULL_DEVELOPMENT_TASK.md`, not completion of this plan file.

## Common verification commands

```powershell
pwsh -NoProfile -File scripts/bootstrap.ps1
pwsh -NoProfile -File scripts/configure.ps1 -Preset windows-msvc-release
cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release --output-on-failure
pwsh -NoProfile -File scripts/run_quality_gates.ps1
```

Milestone-specific tests below supplement these commands. Each milestone commit uses `git add` with an explicit path list, verifies `git diff --cached --check`, then commits with the stated message.

## MS-00 — Baseline, repository, dependencies, and platform build

**Files:**

- Add: `docs/baseline/Signal-Studio-Dev-Docs/**`
- Add: `docs/baseline/BASELINE_INFO.md`, `docs/baseline/baseline-manifest.json`, `docs/baseline/sha256sums.txt`
- Add: `test_data/README.md`, `test_data/test-data-manifest.json`, `test_data/generate_minimal_data.py`
- Add: `docs/development/toolchain_report.md`, `docs/development/dependency_report.md`, `docs/development/dependency_license_audit.md`
- Add: `CMakeLists.txt`, `CMakePresets.json`, `cmake/**`, `vcpkg.json`, `vcpkg-configuration.json`
- Add: `include/signal_studio/**`, `src/platform/**`, `tests/platform/**`
- Add: `scripts/bootstrap.ps1`, `scripts/configure.ps1`, `scripts/build.ps1`, `scripts/test.ps1`
- Add: `.vscode/settings.json`, `.vscode/tasks.json`, `.vscode/launch.json`, `.vscode/extensions.json`
- Modify: `.gitignore`, `README.md`

**Implementation:** Materialize and checksum the approved baseline; record the exact toolchain and optional-capability status; pin vcpkg; create actual buildable version/error/service-boundary APIs and ten CMake targets; enforce dependency direction and Qt-private linkage; expose reproducible CPU and capability-aware CUDA presets; make VS Code use the same scripts and presets.

**Tests:** Configure/build Debug and Release; run target-consumption, version, error, dependency-boundary, and install-tree smoke tests; verify baseline and real-data manifests; capture tool versions and dependency licenses.

**Evidence:** `docs/milestones/MS-00/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `chore(ms-00): establish verified Signal Studio development baseline`

## MS-01 — Core, Data, and TaskRuntime

**Files:**

- Add/modify: `include/signal_studio/core/**`, `src/platform/core/**`
- Add/modify: `include/signal_studio/data/**`, `src/platform/data/**`
- Add/modify: `include/signal_studio/task_runtime/**`, `src/platform/task_runtime/**`
- Add: `tests/core/**`, `tests/data/**`, `tests/task_runtime/**`, `tests/integration/ms01/**`
- Add: `docs/api/core.md`, `docs/api/data.md`, `docs/api/task_runtime.md`

**Implementation:** Implement structured errors and logging, configuration/schema validation, cancellation/progress, bounded scheduler and resource policy, file/mmap/stream data sources, metadata/provenance, RAW IQ/WAV parsing, immutable `DataSourceVersionId`, 64-bit half-open range math, chunk/tile cache, view-request identity, stale-result rejection, task/data orthogonal state, and atomic artifact commits.

**Tests:** Unit tests for range overflow/boundaries, parsers, metadata, cancellation, scheduler fairness, cache consistency and crash-safe commit; integration tests against supplied WAV and both multi-gigabyte SC16 files without loading them wholly into memory.

**Evidence:** `docs/milestones/MS-01/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `feat(ms-01): implement core data and task runtime foundations`

## MS-02 — DSP and Compute

**Files:**

- Add/modify: `include/signal_studio/dsp/**`, `src/platform/dsp/**`
- Add/modify: `include/signal_studio/compute/**`, `src/platform/compute/**`
- Add: `tests/dsp/**`, `tests/compute/**`, `benchmarks/**`, `tests/integration/ms02/**`
- Add: `docs/api/dsp.md`, `docs/api/compute.md`

**Implementation:** Implement window generation, FFT/FFT-shift/scaling, one-sided/two-sided spectrum, PSD dB/Hz, STFT tiles, time-domain statistics, IQ metrics, channel filtering/down-conversion/resampling, deterministic CPU dispatch, backend capability registry, oneMKL default FFT and guarded optional cuFFT. Cache keys include all numerical parameters, backend, and source provenance.

**Tests:** Analytical-tone/noise/impulse comparisons, Parseval and frequency-axis tolerances, PSD unit tests, STFT orientation tests, cache equivalence, cancellation, backend fallback, benchmark baselines, and real-data processing samples.

**Evidence:** `docs/milestones/MS-02/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `feat(ms-02): add validated DSP and compute backends`

## MS-03 — Visualization and Workbench

**Files:**

- Add/modify: `include/signal_studio/visualization/**`, `src/platform/visualization/**`
- Add/modify: `include/signal_studio/workbench/**`, `src/platform/workbench/**`
- Add: `resources/**`, `tests/visualization/**`, `tests/workbench/**`, `tests/gui/**`
- Add: `docs/api/visualization.md`, `docs/api/workbench.md`

**Implementation:** Build reusable Qt widgets and a dark high-density workbench: time, PSD, STFT and result views; shared `LoadedDataRange`; shared PSD/STFT integer-Hz `FrequencyViewport`; required wheel/drag semantics; linked cursors; resize/reorder/hide controls; display-only color mapping; persistent layout; command/dock/property/status infrastructure; accessible 28x28 logical hit targets.

**Tests:** QTest interaction/state tests, render-image comparisons with controlled tolerances, viewport synchronization, hide-and-stop within 500 ms, DPI/layout tests at 1280x720, 1366x768 and 1600x900, and keyboard/accessibility smoke tests.

**Evidence:** `docs/milestones/MS-03/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `feat(ms-03): deliver reusable visualization workbench`

## MS-04 — Signal Studio basic application

**Files:**

- Add: `apps/signal_studio/**`, `tests/app/**`, `tests/e2e/ms04/**`
- Modify: root build/install configuration, resources and documentation
- Add: `docs/user/import-and-analysis.md`, `docs/ui/p01-p02.md`

**Implementation:** Compose only public platform services into the branded Windows application; implement P01/P02, import wizard and confirmed filename parsing, recent/project/workspace lifecycle, data browser, task list, result history, export, errors, settings, undoable view actions, and safe partial-result behavior. Use approved icon assets and preserve STFT/frequency interaction invariants.

**Tests:** End-to-end import and analysis for all supplied files, malformed/permission/empty cases, project reopen, task cancel/retry, result export, stale request suppression, headless self-test before Qt startup, and GUI screenshots.

**Evidence:** `docs/milestones/MS-04/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `feat(ms-04): complete Signal Studio import and basic analysis workflows`

## MS-05 — Wideband and narrowband linked analysis

**Files:**

- Add/modify: `apps/signal_studio/features/wideband/**`, `apps/signal_studio/features/narrowband/**`
- Add: `tests/e2e/ms05/**`, `docs/user/wideband-narrowband.md`

**Implementation:** Implement P03/P04/P05/P06/P07 flows, overview/detail linkage, region/channel creation, parameter inheritance, marker and measurement management, artifact provenance, channel extraction, modulation/statistics panels, and request-priority/cancellation rules for linked views.

**Tests:** Real-data wideband navigation, narrowband extraction and reopen; deterministic linkage state; cache reuse/invalidation; cancellation; output provenance; all page/interaction acceptance cases from the approved matrix.

**Evidence:** `docs/milestones/MS-05/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `feat(ms-05): implement linked wideband and narrowband analysis`

## MS-06 — PluginSDK, ModelRuntime, and Dataset

**Files:**

- Add/modify: `include/signal_studio/plugin_sdk/**`, `src/platform/plugin_sdk/**`
- Add/modify: `include/signal_studio/model_runtime/**`, `src/platform/model_runtime/**`
- Add/modify: `include/signal_studio/dataset/**`, `src/platform/dataset/**`
- Add: `plugins/examples/**`, `python/**`, `tests/plugin_sdk/**`, `tests/model_runtime/**`, `tests/dataset/**`
- Add: `docs/sdk/**`, `docs/plugin/**`, `docs/python/**`

**Implementation:** Implement ABI-v1 discovery/query/lifecycle/capability/error boundary and example plugin; ONNX Runtime session/provider selection, preprocessing/postprocessing, cancellation and artifact metadata; HDF5 dataset adapter, schema/provenance/split/version operations; Python bindings and SDK examples without exposing private Qt types.

**Tests:** ABI compatibility and hostile-plugin isolation, example plugin E2E, deterministic model smoke tests, unavailable-provider fallback, HDF5 round-trip/large-chunk tests, Python import/lifetime/error tests, and public SDK consumer builds.

**Evidence:** `docs/milestones/MS-06/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `feat(ms-06): finish plugin model and dataset platform modules`

## MS-07 — Engineering, quality, packaging, and documentation

**Files:**

- Add: `.github/workflows/ci.yml`, `cmake/Packaging.cmake`, `packaging/**`
- Add: `.clang-format`, `.clang-tidy`, `scripts/run_quality_gates.ps1`, `scripts/package.ps1`, `scripts/run_stability.ps1`
- Add: `docs/development/**`, `docs/user/**`, `docs/api/**`, `docs/release/**`
- Add: `docs/development/开发需求追踪矩阵.xlsx`

**Implementation:** Enforce format/static-analysis/warnings; add sanitized tests where supported; generate installer and portable ZIP; deploy Qt runtime/plugins; write installation, build, debug, architecture, API/SDK/plugin, troubleshooting and packaging documents; create a source-linked traceability workbook covering all 198 requirements.

**Tests:** Clean-machine-style install-tree smoke, portable launch/self-test, uninstall metadata, Debug/Release/CPU matrix, capability-aware CUDA preset, packaging reproducibility, license inventory, security scan, formula-error-free workbook validation, and configurable stability runner.

**Evidence:** `docs/milestones/MS-07/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `chore(ms-07): harden engineering quality and release packaging`

## MS-08 — Beta second application and reuse proof

**Files:**

- Add: `apps/signal_review/**`, `tests/e2e/ms08/**`, `docs/reuse/**`
- Modify: build/package/CI configuration

**Implementation:** Build a second thin application from public modules only, demonstrating alternate workbench composition, import/review/export flows, module-level reuse and independent packaging without private-header or Qt implementation leakage.

**Tests:** Public-only include/link audit, second-app E2E, install/package smoke, shared cache/data compatibility, ABI checks and reuse matrix validation.

**Evidence:** `docs/milestones/MS-08/{development_report,test_report,acceptance_record,commit_record}.md`

**Commit:** `feat(ms-08): prove platform reuse with Signal Review beta`

## MS-09 — Release validation and GitHub publication

**Files:**

- Add: `docs/milestones/MS-09/{development_report,test_report,acceptance_record,commit_record}.md`
- Add: `docs/release/FINAL_ACCEPTANCE_REPORT.md`, `docs/release/RELEASE_NOTES_v1.0.0.md`
- Add/update: release manifests, SHA-256 files, installer and portable deliverables under `dist/`

**Implementation:** Execute the complete final-acceptance matrix; close or explicitly approve every requirement deviation; run the requested stability duration; validate installer/portable on a clean deployment directory; audit licenses, security, docs, CI and repository cleanliness; update main safely; sign/annotate tag `v1.0.0`; push commits/tags; create the formal GitHub release and upload artifacts/checksums.

**Tests:** Full CTest/GUI/E2E/real-data/performance/stability/package/install matrix with exact logs, versions, durations and failure counts; SHA-256 verification after upload/download; remote default-branch/tag/release verification.

**Evidence:** milestone files above plus `docs/release/FINAL_ACCEPTANCE_REPORT.md`, generated reports/logs and release URL.

**Commit:** `release: publish Signal Studio v1.0.0`

## Final review

After MS-09, request an independent final code/acceptance review over the complete branch, fix and re-review all Critical/Important findings, then use `finishing-a-development-branch` to integrate and publish the validated result. Mark the active goal complete only after every final acceptance item is evidenced or the user has expressly accepted a recorded deviation.
