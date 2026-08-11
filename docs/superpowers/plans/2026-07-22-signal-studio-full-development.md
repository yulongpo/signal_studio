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

## MS-4.5 — Parameterized spectrum and spectrogram analysis

**Files:**

- Add/modify: `include/signal_studio/dsp/analysis.hpp`, `src/platform/dsp/**`
- Modify: `apps/signal_studio/**`, `include/signal_studio/{visualization,workbench}/**`
- Add: `apps/signal_studio/ui/SignalAnalysisSettingsPanel.ui`
- Add/modify: `tests/{dsp,app,visualization}/**`, `docs/milestones/MS-4.5/**`

**Implementation:** Add a versioned, deterministic spectrum/PSD/STFT parameter model with
SHA-256 identity, cost estimation, minimal invalidation, window extensions, Periodogram/Welch,
averaging/hold, spectrum and spectrogram smoothing, ProcessingChain prefiltering, raw-result
retention, TaskRuntime cancellation/latest-result commit, project migration, presets and Artifact
provenance. Build the Inspector editor from Qt Designer and keep display mapping separate from DSP.
Reuse oneMKL DFTI/VSL/LAPACKE, cuFFT and existing adapters; do not add another numerical library.

**Tests:** Deterministic numerical window/FFT/PSD/STFT/filter tests; CPU/CUDA Debug and Release
consistency; persistence/cache/provenance/latest-request tests; Qt contract, 1280/1080P/4K and
125%–200% DPI tests; 30-minute parameter/zoom/view switching stability; full CPU regression,
install consumer, executable self-test and VS Code F5 checks.

**Evidence:** `docs/milestones/MS-4.5/{development_report,test_report,acceptance_record,commit_record}.md`
plus the named Chinese milestone reports and `evidence/`.

**Commit:** `feat(ms-4.5): parameterize spectrum and spectrogram analysis`

## 2026-08-11 后续计划校准

本节替代原 MS-05～MS-09 计划。校准依据是当前分支 `codex/full-signal-studio-development`
的实际代码、BL1.0 的 198 项需求、现有测试与里程碑证据。分析范围只包含当前分支，
不使用其他分支的代码、提交、报告、标签或状态推导当前实现和后续安排。

当前分支已完成 MS-00～MS-04 和插入里程碑 MS-4.5。现有 `Selection`、DSP
`ProcessingChain`、Inspector 状态、TaskRuntime、Artifact 与参数化分析能力是 MS-05 的
输入，不得重新复制实现。`SignalPluginSDK` 当前仅有 ABI-v1 基础边界，
`SignalModelRuntime` 和 `SignalDataset` 当前只有模块描述符，不能视为功能完成。

每个后续里程碑开始前执行基线、依赖锁、用户预设确定性和工作树检查；结束时完成
CPU Debug/Release 全量、相关 CUDA 12.4 专项、安装消费者、真实数据有界测试、中文证据
文档、差异审计和 Git 提交。只删除已由文档固化的废弃生成目录，保留当前四套活动构建树
和 `.deps` 依赖缓存；每个里程碑关闭后再清理该里程碑的临时截图、安装树和诊断目录。

## MS-05 — 宽窄带联动分析

**文件范围：**

- 新增/修改：`apps/signal_studio/features/wideband/**`、`apps/signal_studio/features/narrowband/**`
- 修改：`apps/signal_studio/**`、必要的公共 Selection/Channel/Inspector 契约
- 新增：`tests/e2e/ms05/**`、`docs/user/宽窄带联动分析.md`

**实施顺序：**

1. 建立应用层 `Selection -> AnalysisChannel` 编排、稳定 ID、版本、参数继承、模板、
   工程保存/恢复和依赖安全删除；公共契约继续不暴露 Qt 或第三方类型。
2. 复用既有 DSP 节点完成有界分块的复数频移、抗混叠滤波、整数抽取/插值和有理重采样，
   保持跨块状态、边界策略、取消和下游缓存最小失效；不另写 FFT、滤波或重采样内核。
3. 完成 P02 宽带 Selection 与 P03 独立通道 Inspector 的双向定位、游标/测量、任务进度、
   结果过期和来源追溯。P04/P05 只补齐与通道任务/结果的集成，不重做已完成页面。
4. 星座、眼图、直方图和瞬时频率只消费真实通道数据；符号率或同步来源缺失时明确显示
   不适用。调制识别、ONNX/Python 算法和插件管理留在 MS-06。

**测试与退出条件：**

- 覆盖 FR-SEL-001～007、FR-DSP-001～009、FR-INS-001～007 的应用级闭环，重跑其
  既有单元测试并新增宽带选区到窄带提取 E2E。
- 使用批准的 X310 录制做有界导航、通道提取、取消/重试、工程重开、缓存失效和 Artifact
  来源验证；不创建或扫描物理 100 GB 文件。
- 完成 BL1.0 里程碑门禁 AT-05/14/23，保持 1280×720、1080P、4K 和 100%～200% DPI
  无重叠/裁剪。

**证据：** `docs/milestones/MS-05/{development_report,test_report,acceptance_record,commit_record}.md`
及四份规定的中文里程碑文档。

**提交：** `feat(ms-05): 实现宽窄带联动与窄带通道闭环`

## MS-06 — PluginSDK、ModelRuntime 与 Dataset

MS-06 按内部门禁 A～E 顺序推进，可以形成多个可审计实现提交，但只有五个门禁全部通过
后才关闭里程碑。

**A. 依赖闭包：** 安装并锁定实际需要的 ONNX Runtime、HDF5 和 pybind11，验证许可证、
Debug/Release、安装树和洁净 `PATH` 闭包。ONNX Runtime CUDA 提供程序必须与本机 CUDA
12.4 的官方兼容矩阵一致；只有所选提供程序明确需要时才安装匹配 cuDNN，且 CPU 必须
独立可用、GPU 缺失必须明确回退。

**B. SignalPluginSDK：** 完成文件、DSP、算法、解调、视图、导出插件类型；完整 manifest
身份/接口/架构/依赖/能力/输入输出/许可/哈希；发现、查询、生命周期、版本拒绝、启停、
隔离、安全模式、来源/签名/信任/外联权限可见；提供 C/C++ 示例插件和多宿主契约测试。

**C. SignalModelRuntime 与算法契约：** 以 ONNX Runtime 为默认后端，实现模型安装、注册、
解析、会话、前后处理、批处理、设备选择、取消、实际提供程序和 Artifact 来源；覆盖
FR-ALG-001～009 的估计质量、适用条件、Top-K/未知阈值/聚合、传统与 ONNX/Python
适配、解调统一输出以及不可信进程故障隔离。

**D. SignalDataset：** 完成版本化 manifest、索引、标签、分片、训练/验证/测试划分、统计、
查询、缓存和原子提交；同时提供 HDF5 与 WebDataset 适配，覆盖大块、损坏、迁移、重开
和并发读写边界。

**E. 宿主集成：** P06 插件与模型页面以及 P03 算法结果接入真实服务；提供 Python 绑定、
Headless CLI、安装后 SDK 消费工程和至少两个宿主的契约验证，公共头不得泄露 Qt、ORT、
HDF5 或 pybind11 类型。

**测试与退出条件：** 精确覆盖 MS-06 的 19 项批准需求及 TC-FUNC-102～110、136～142、
161～163；执行 ABI 兼容/恶意插件隔离、确定性 ONNX 推理、不可用提供程序回退、HDF5/
WebDataset 往返、Python 生命周期/错误、安装消费和多宿主契约测试。

**证据：** `docs/milestones/MS-06/{development_report,test_report,acceptance_record,commit_record}.md`
及四份规定的中文里程碑文档。

**提交：** `feat(ms-06): 完成插件模型与数据集平台能力`

## MS-07 — 工程化、安全、质量、打包与文档

MS-07 不能只做打包。先关闭 19 项 MS-07 需求：结构化分级日志、滚动与脱敏诊断包，
设置/布局/DPI/中文国际化、真实诊断和启动硬件检查，以及默认无网络、路径防穿越、
插件/模型最小权限、无 GUI 可测、版本化和可重复性能追踪。随后执行格式、静态分析、
警告、泄漏/安全检查和 198 项追踪矩阵闭环。

生成可重复的 CPack/NSIS 安装包与便携包，部署 Qt、oneMKL、CUDA 可选闭包、ORT、HDF5、
插件、模型、许可证和 SBOM；验证离线安装、启动、卸载、升级、回滚、不删除用户工程、
无开发机绝对路径、无未授权网络、包复现和洁净环境消费。

**证据：** `docs/milestones/MS-07/{development_report,test_report,acceptance_record,commit_record}.md`
及中文安装、质量、安全、许可证和追踪报告。

**提交：** `chore(ms-07): 完成工程质量与发布打包门禁`

## MS-08 — Beta 多应用复用证明

按批准基线实现 Signal Generator 薄壳，而不是原计划中的 Signal Review。薄壳只消费
安装后的公共 Data、DSP、TaskRuntime、Visualization/Workbench 包，同时提供独立的
Headless CLI 验证路径；不得包含 Signal Studio 私有头或复制公共模块实现。

验证第二桌面应用的生成/导入/浏览最小闭环、公共缓存和数据兼容、独立安装与 Signal
Studio 并存、不同应用不共享可写配置、ABI/schema 兼容，以及 `NFR-REUSE-101` /
`TC-REUSE-001`。若发现公共能力缺口，只能通过 ADR 和公共 API 修订解决。

**证据：** `docs/milestones/MS-08/{development_report,test_report,acceptance_record,commit_record}.md`

**提交：** `feat(ms-08): 以 Signal Generator 证明平台复用`

## MS-09 — 最终验收与正式发布

执行 `CODEX_FULL_DEVELOPMENT_TASK.md` 的全部最终验收条件，而不是只检查里程碑状态。
完成 Windows CPU Debug/Release 全量、可用时 CUDA 12.4 矩阵、GUI/E2E、三份外部录制
有界验证、算法、性能、稳定性、安装/卸载/升级/回滚、便携包、多应用并存、Python wheel、
插件 ABI、SBOM/许可证/安全、CI、追踪矩阵和仓库清洁度验证，并保存精确日志、版本、
时长、失败数与 SHA-256。

当前分支通过全部门禁后，再按总任务执行默认分支集成、远程冲突检查和发布授权步骤；
本次计划不依据其他分支状态预设合并策略。发布应用和 SDK `1.0.0`，创建并推送注释标签
`v1.0.0`，创建 GitHub Release，上传安装包、便携包、开发包、Python wheel、
调试符号、许可证/SBOM、发布说明和校验清单，并在上传后重新下载校验。

**证据：** `docs/milestones/MS-09/**`、`docs/release/最终执行报告.md`、
`docs/release/ReleaseNotes_1.0.0.md`、发布资产清单和 Release URL。

**提交：** `release: 发布 Signal Studio v1.0.0`

## 最终复审

MS-09 后对完整分支执行独立规格、代码质量和最终验收复审；所有 Critical/Important
发现必须修复并复审通过。只有总任务的最终验收条件全部有真实证据，或用户明确接受了
如实记录的偏差，才允许结束目标。
