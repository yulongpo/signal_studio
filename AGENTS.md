# Signal Studio Codex development rules

## Task workflow

1. Read this file and the assigned task document completely.
2. Inspect `git status` before changing files.
3. Use the immutable approved baseline under `docs/baseline/Signal-Studio-Dev-Docs` as the primary authority.
4. Implement only the assigned milestone and its validation/evidence.
5. Update `docs/DEVELOPMENT_PLAN.md`, the relevant core documents, and `docs/CHANGELOG.md`.
6. Build, test, audit the diff, and create the milestone commit.

## Product and toolchain

Signal Studio is a Windows offline IQ-signal analysis product and reusable C++20 platform. The production stack is Qt 6.11 Widgets, CMake, Ninja, and MSVC 2022 x64. Optional CUDA support must degrade cleanly to CPU and must never be installed automatically.

## Authority and scope

Authority order:

1. `docs/baseline/Signal-Studio-Dev-Docs/` (immutable BL1.0 snapshot)
2. `CODEX_FULL_DEVELOPMENT_TASK.md`
3. the active milestone task
4. `docs/DEVELOPMENT_PLAN.md`
5. `docs/ARCHITECTURE.md`, `docs/UI_DESIGN.md`, `docs/TEST_PLAN.md`, `docs/DECISIONS.md`, `docs/CHANGELOG.md`

The former upstream content is not an implementation dependency. Its preserved recovery refs are `archive/pre-signal-studio-dev-20260722-145422` and `pre-signal-studio-dev-20260722-145422`.

## Engineering rules

- Public modules must follow the approved dependency DAG and expose only public, third-party-free C++ contracts.
- Qt is private to Visualization and Workbench.
- UI work must not block the main thread; background work must not touch `QWidget` directly.
- Large files are accessed in bounded chunks and are never committed.
- Do not add placeholders, fake results, disabled failing tests, or unverified completion claims.
- Every new behavior has a test or an explicit, honest reason why it is not currently testable.
- Do not edit the approved baseline, external source documents, external test recordings, or `references_rep/`.

## Completion gate

The milestone is complete only when its scoped implementation works, Debug and Release validations required by the task pass, install/consumer checks pass where required, evidence is recorded, the baseline remains intact, and the Git working tree is clean after the milestone commit.
