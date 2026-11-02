# G regression

Worktree `D:\project\ss-de-rpo-wt\continue-g`, branch `codex/ss-de-rpo-g`.
Base before this handoff commit: `da57e8705f3ef2924a71d64c914afb434050a4cf` (`da57e87`).
Assigned model: grok-4.7-build-fast / xhigh.

## Owned files

- `tests/ui_tests.cpp`
- this file

`app/main_window.cpp`, `tests/ux_r3_workflow_tests.cpp`, production sources, and `CMakeLists.txt` stay at `da57e87`. `out/` is untracked. Push was not performed. GPU smoke was not run. No native window was opened.

## Baseline

Main Release full CTest, `D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/N0/logs/integration-release-all.txt`. That log has no `Cannot find font directory` warning. Three CTest failures, 95% of 60:

- `signal_studio_ux_r3_workflows` (owned by C; left as-is): `saveAsReopenKeepsOriginalAndMovesSaveDestination` and six `manualLabelEntryPoints` rows. `Totals: 20 passed, 7 failed`.
- `signal_studio_ui` Failed 43.82 sec. `Totals: 29 passed, 2 failed, 0 skipped, 0 blacklisted, 42944ms`.
  - `UiTests::singleClickSelectsWithoutChangingView` at `tests/ui_tests.cpp(775)`: `activeMarkId == id`.
  - `UiTests::paletteControlsStaySynchronized` at `tests/ui_tests.cpp(1992)`: `revealParameter(header) && rangeTag->isVisible() && maximize->isVisible()`.
- `signal_studio_ui_parameters` Failed 120.69 sec. `Totals: 56 passed, 2 failed, 0 skipped, 0 blacklisted, 120257ms`.
  - `UiTests::contextMenuActionsAndCoveredMarkSelection` at `tests/ui_tests.cpp(3204)`: `covered`.
  - `UiTests::unselectedMarkClickAndSubthresholdGesture` at `tests/ui_tests.cpp(3231)`: `activeMarkId == id`.

Isolated legacy UI, `D:/project/signal_studio_rebuild/out/tasks/ss-de-rpo/N2/legacy-ui-isolated.txt`. Release Qt 6.11.1. `Totals: 2 passed, 2 failed, 0 skipped, 0 blacklisted, 973ms`. Same two geometry failures at lines 775 and 1992.

## Repair

UX-R5 draws only the active source tab. `Session::addMark` creates a Manual annotation. `selectMarks({})` leaves the default ImportedLabel overlay, so the new mark is not hittable. `activateManualOverlay` calls the existing `annotationUiOpenSource(window, AnnotationSource::Manual)` and requires the panel tab plus an exclusive Manual layer. The three mark tests call it after `selectMarks({})`. Click points, context-menu coverage, span, and view assertions are unchanged.

`ChartControlStrip` keeps high-priority controls inline and moves priority `< 0` controls, including `dynamic`, into the overflow popover. `paletteControlsStaySynchronized` now resolves `chartControlOwner`, clicks the visible overflow button when the editor is hidden, and checks size and order on the real host: inline `specChartHead` or the strip popover. `specMaximize` stays in `specChartHead`. Palette list, file switch, Cividis/CoolEdit, colorbar red transition, narrowband `paletteIndex`, and the single `colormap` combo stay asserted.

Temporary `qWarning` geometry prints were removed before the clean Debug rebuild and before Release.

## Font constraint

Own offscreen runs set:

```text
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_SCALE_FACTOR = '1.5'
$env:QT_QPA_FONTDIR = 'C:\Windows\Fonts'
```

CTest already sets `QT_QPA_PLATFORM=offscreen` and `QT_SCALE_FACTOR=1.5` with `TIMEOUT 480` (`CMakeLists.txt` UI slots). `cmake/run_ui_test.cmake` does not set a font directory. `scripts/build.ps1` does. Without `QT_QPA_FONTDIR`, Qt offscreen warns `Cannot find font directory .../lib/fonts` and `QFontMetricsF` collapses `mainPlot` `plotRect` to `QRectF(100159,12 1x660)`. In that run the Manual overlay was already active (`source tab 1`, layers false/true/false) and `singleClickSelectsWithoutChangingView` still failed the original `activeMarkId` assertion (`out/tasks/ss-de-rpo/G/logs/click-out.txt`, `ui-four-debug.txt`: palette PASS, three mark failures, `Totals: 3 passed, 3 failed`, 10557 ms). With the font directory, the same diagnostic binary reported `plotRect QRectF(82,12 1766x654)`, selected `annotation-2`, view unchanged (`click-font-out.txt`), and all four cases PASS (`ui-four-font-debug.txt`, `Totals: 6 passed, 0 failed`, 14186 ms). The main integration log has no font warning, so its four failures are the stale setups above. The font variable is a run constraint. Timeout, assertions, and CMake sources were left unchanged.

## Commands and results

Own build tree `out/vs2026-qt611-debug`. Compiler jobs: 2. CTest jobs: 1. Both suites use the clean `tests/ui_tests.cpp` (no click diagnostics).

Debug CTest, shell exit 0. Log `out/tasks/ss-de-rpo/G/logs/ctest-debug-ui.txt`.

```text
ctest --test-dir D:\project\ss-de-rpo-wt\continue-g\out\vs2026-qt611-debug -C Debug -R "^signal_studio_ui$|^signal_studio_ui_parameters$" --output-on-failure -j 1
```

- `signal_studio_ui` Passed 190.79 sec. Raw `ui-geometry-test-results-debug.txt`: `PASS` `singleClickSelectsWithoutChangingView`, `PASS` `paletteControlsStaySynchronized`, `Totals: 31 passed, 0 failed`, 190416 ms.
- `signal_studio_ui_parameters` Passed 344.45 sec. Raw `ui-parameters-test-results-debug.txt`: `PASS` `contextMenuActionsAndCoveredMarkSelection`, `PASS` `unselectedMarkClickAndSubthresholdGesture`, `Totals: 58 passed, 0 failed`, 344113 ms.
- `100% tests passed, 0 tests failed out of 2`. Total Test time (real) = 535.25 sec.

Release build, exit 0, 122.48 s. Own `out/` only.

```text
cmake --build D:\project\ss-de-rpo-wt\continue-g\out\vs2026-qt611-debug --config Release --target SignalStudioUiTests --parallel 2
```

Linked `out/vs2026-qt611-release_bin/SignalStudioUiTests.exe`. The target dependency also staged `SignalStudio.exe` and `SignalStudioDetectionWorker.exe` into `out/vs2026-qt611-release_bin`. Pre-existing notes: oneMKL deploy skipped (`mkl_rt.3.dll` missing); windeployqt `dxcompiler.dll`/`dxil.dll` warning; C4456/C4458/C4189/C4267 in untouched sources. Session log: `C:\Users\Admin\.grok\sessions\D%3A%5Cproject%5Css-de-rpo-wt%5Ccontinue-g\2c7af937-bc18-49b0-b875-4af63daf3926\terminal\call-746eb058-e8cc-42c1-8c23-fa4bc64c45ab-1.log`.

Release CTest, shell exit 0. Log `out/tasks/ss-de-rpo/G/logs/ctest-release-ui.txt` (`EXIT=0`).

```text
ctest --test-dir D:\project\ss-de-rpo-wt\continue-g\out\vs2026-qt611-debug -C Release -R "^signal_studio_ui$|^signal_studio_ui_parameters$" --output-on-failure -j 1
```

- `signal_studio_ui` Passed 26.66 sec. Raw `ui-geometry-test-results-release.txt`: both repaired geometry cases `PASS`, `Totals: 31 passed, 0 failed, 0 skipped, 0 blacklisted, 26333ms`.
- `signal_studio_ui_parameters` Passed 64.29 sec. Raw `ui-parameters-test-results-release.txt`: both repaired parameter cases `PASS`, `Totals: 58 passed, 0 failed, 0 skipped, 0 blacklisted, 63885ms`.
- `100% tests passed, 0 tests failed out of 2`. Total Test time (real) = 90.96 sec.

Passing slot counts match the baseline 31 geometry rows and 58 parameter rows. `signal_studio_ui_annotations` was not run. Offscreen `QWARN` lines `This plugin does not support grabbing the mouse` and `propagateSizeHints` remain and do not fail the harness.

## Left outside this handoff

`signal_studio_ux_r3_workflows` still fails on the main Release log. Native 4K acceptance, GPU display, and the other 58 CTest targets were not run here. `ui_tests.cpp` is a `CMAKE_CONFIGURE_DEPENDS` input, so the own build tree reconfigured when that file changed. `CMakeLists.txt` has no source diff and was not committed.
