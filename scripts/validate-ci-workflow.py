#!/usr/bin/env python3
"""解析 GitHub Actions 工作流并强制 Windows 2022 ABI 与 Qt 契约。"""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys

try:
    import yaml
except ImportError as error:  # pragma: no cover - developer-tool failure path
    raise SystemExit("PyYAML is required for workflow syntax validation") from error


REPOSITORY = Path(__file__).resolve().parents[1]
WORKFLOW = REPOSITORY / ".github" / "workflows" / "ci.yml"
INITIALIZER = "./.github/scripts/initialize-msvc.ps1"
CMAKE_PROJECT = REPOSITORY / "CMakeLists.txt"
PACKAGE_CONFIG = REPOSITORY / "cmake" / "SignalStudioConfig.cmake.in"
COMMON_POWERSHELL = REPOSITORY / "scripts" / "common.ps1"
CHECKOUT_ACTION = "actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803"
INSTALL_QT_ACTION = "jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730"
MINIMUM_QT_VERSION = "6.10.3"
CI_QT_VERSION = "6.10.3"
LOCAL_QT_VERSION = "6.11.1"
QT_ARCHITECTURE = "win64_msvc2022_64"
QT_EVIDENCE = REPOSITORY / "docs" / "milestones" / "MS-00" / "evidence" / "qt-ci-availability.json"
QT_METADATA_URL = (
    "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/"
    "qt6_6103/qt6_6103/Updates.xml"
)


def named_step(steps: list[object], name: str) -> dict[str, object]:
    for step in steps:
        if isinstance(step, dict) and step.get("name") == name:
            return step
    raise RuntimeError(f"missing workflow step: {name}")


def verify_immutable_actions(steps: list[object], job_name: str) -> None:
    actions = [str(step["uses"]) for step in steps if isinstance(step, dict) and "uses" in step]
    for action in actions:
        revision = action.rsplit("@", 1)[-1]
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise RuntimeError(f"{job_name} action must use an immutable commit: {action}")
    if CHECKOUT_ACTION not in actions:
        raise RuntimeError(f"{job_name} must use the pinned checkout action")


def verify_qt_availability_evidence() -> None:
    try:
        evidence = json.loads(QT_EVIDENCE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Qt availability evidence is unreadable: {error}") from error
    expected = {
        "schema": "signal-studio.qt-ci-availability/1.0",
        "minimum_source_compatibility_version": MINIMUM_QT_VERSION,
        "ci_version": CI_QT_VERSION,
        "local_validation_version": LOCAL_QT_VERSION,
        "architecture": QT_ARCHITECTURE,
        "metadata_url": QT_METADATA_URL,
        "metadata_http_status": 200,
        "metadata_bytes": 312649,
        "metadata_sha256": "91e0a64cc859c28eb547192a9813cc2ffaac524a98250bc39a0b79fe8da6eae1",
        "package_name": "qt.qt6.6103.win64_msvc2022_64",
        "package_name_match_line": 5994,
    }
    for key, value in expected.items():
        if evidence.get(key) != value:
            raise RuntimeError(f"Qt availability evidence field mismatch: {key}")
    unavailable = evidence.get("unavailable_ci_candidates")
    if unavailable != [
        {"version": "6.11.0", "metadata_http_status": 404},
        {"version": "6.11.1", "metadata_http_status": 404},
    ]:
        raise RuntimeError("Qt unavailable-version evidence mismatch")


def verify() -> None:
    try:
        workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    except yaml.YAMLError as error:
        raise RuntimeError(f"invalid GitHub Actions YAML: {error}") from error
    if not isinstance(workflow, dict) or not isinstance(workflow.get("jobs"), dict):
        raise RuntimeError("workflow must contain a jobs mapping")

    jobs = workflow["jobs"]
    headless = jobs.get("headless-build-test")
    ui = jobs.get("windows-ui-module-performance")
    if not isinstance(headless, dict) or not isinstance(ui, dict):
        raise RuntimeError("required MS-00 CI jobs are missing")

    if headless.get("runs-on") != "windows-2022" or "strategy" in headless:
        raise RuntimeError("headless CI must run once on windows-2022 without an OS matrix")
    if "ubuntu" in WORKFLOW.read_text(encoding="utf-8").lower():
        raise RuntimeError("Ubuntu headless validation was retired by the approved development policy")
    if ui.get("runs-on") != "windows-2022":
        raise RuntimeError("Qt-backed CI must run on windows-2022")
    expected_environment = {
        "PYTHONUTF8": "1",
        "PYTHONIOENCODING": "utf-8",
        "SIGNAL_STUDIO_MINIMUM_QT_VERSION": MINIMUM_QT_VERSION,
        "SIGNAL_STUDIO_CI_QT_VERSION": CI_QT_VERSION,
        "SIGNAL_STUDIO_LOCAL_QT_VERSION": LOCAL_QT_VERSION,
    }
    if workflow.get("env") != expected_environment:
        raise RuntimeError("workflow must separate UTF-8, minimum, CI, and local Qt contracts")
    cmake_project = CMAKE_PROJECT.read_text(encoding="utf-8")
    package_config = PACKAGE_CONFIG.read_text(encoding="utf-8")
    common_powershell = COMMON_POWERSHELL.read_text(encoding="utf-8")
    if f'set(SIGNAL_STUDIO_MINIMUM_QT_VERSION "{MINIMUM_QT_VERSION}")' not in cmake_project:
        raise RuntimeError("CMake project minimum Qt version differs from the CI contract")
    if "find_dependency(Qt6 @SIGNAL_STUDIO_MINIMUM_QT_VERSION@" not in package_config:
        raise RuntimeError("installed UI package must preserve the project minimum Qt version")
    if common_powershell.count("[version]'6.10.3'") < 2:
        raise RuntimeError("local Qt discovery must enforce the supported Qt 6.10.3 minimum")
    if "[version]'6.11.0'" in common_powershell:
        raise RuntimeError("local Qt discovery must not promote the installed Qt 6.11 version to a minimum")

    for job_name, job, headless_job in (
        ("headless-build-test", headless, True),
        ("windows-ui-module-performance", ui, False),
    ):
        steps = job.get("steps")
        if not isinstance(steps, list):
            raise RuntimeError(f"{job_name} steps must be a sequence")
        verify_immutable_actions(steps, job_name)
        acquisition = named_step(steps, "Validate immutable acquisition and package locks")
        expected_acquisition = (
            "./scripts/validate-dependency-lock.ps1 -Mode Acquisition -Headless"
            if headless_job
            else "./scripts/validate-dependency-lock.ps1 -Mode Acquisition"
        )
        if acquisition.get("shell") != "pwsh" or acquisition.get("run") != expected_acquisition:
            raise RuntimeError(f"{job_name} must validate portable acquisition locks with pwsh")
        initializer = named_step(steps, "Initialize MSVC 2022 x64 environment")
        if initializer.get("shell") != "pwsh" or initializer.get("run") != INITIALIZER:
            raise RuntimeError(f"{job_name} must call the repository MSVC initializer with pwsh")
        if "if" in initializer:
            raise RuntimeError(f"{job_name} MSVC initialization must be unconditional on Windows 2022")
        configure_index = next(
            (index for index, step in enumerate(steps) if isinstance(step, dict) and str(step.get("name", "")).startswith("Configure")),
            None,
        )
        if (
            configure_index is None
            or steps.index(acquisition) >= steps.index(initializer)
            or steps.index(initializer) >= configure_index
        ):
            raise RuntimeError(f"{job_name} must initialize MSVC before CMake configure")

    headless_steps = headless["steps"]
    headless_compatible = named_step(headless_steps, "Validate compatible Windows headless host")
    if (
        "if" in headless_compatible
        or headless_compatible.get("shell") != "pwsh"
        or headless_compatible.get("run")
        != "./scripts/validate-dependency-lock.ps1 -Mode CompatibleHost -Headless -HostEvidenceOutputPath build/ci-host-evidence.json"
    ):
        raise RuntimeError("headless CI must validate the Windows compatibility contract after MSVC initialization")
    if not (
        headless_steps.index(named_step(headless_steps, "Initialize MSVC 2022 x64 environment"))
        < headless_steps.index(headless_compatible)
        < headless_steps.index(named_step(headless_steps, "Configure headless platform and C SDK example"))
    ):
        raise RuntimeError("headless host compatibility validation is ordered incorrectly")

    ui_steps = ui["steps"]
    qt_step = next(
        (step for step in ui_steps if isinstance(step, dict) and str(step.get("uses", "")).startswith("jurplel/install-qt-action@")),
        None,
    )
    if not isinstance(qt_step, dict) or qt_step.get("uses") != INSTALL_QT_ACTION:
        raise RuntimeError("Qt-backed CI must use the immutable install-qt action commit")
    if qt_step.get("with", {}).get("version") != "${{ env.SIGNAL_STUDIO_CI_QT_VERSION }}":
        raise RuntimeError("Qt-backed CI must install the separately pinned CI compatibility version")
    if qt_step.get("with", {}).get("arch") != QT_ARCHITECTURE:
        raise RuntimeError("Qt-backed CI must install the MSVC 2022 x64 Qt kit")
    qt_verifier = named_step(ui_steps, "Verify Qt and compiler ABI")
    if "QT_VERSION" not in str(qt_verifier.get("run", "")) or "SIGNAL_STUDIO_CI_QT_VERSION" not in str(qt_verifier.get("run", "")):
        raise RuntimeError("Qt ABI verification must also enforce the CI Qt version")
    ui_compatible = named_step(ui_steps, "Validate compatible Windows UI host")
    ui_compatible_run = str(ui_compatible.get("run", ""))
    if (
        ui_compatible.get("shell") != "pwsh"
        or "$env:SIGNAL_STUDIO_QT_ROOT = $env:QT_ROOT_DIR" not in ui_compatible_run
        or "validate-dependency-lock.ps1 -Mode CompatibleHost" not in ui_compatible_run
    ):
        raise RuntimeError("Qt-backed CI must validate the compatible UI host with the installed Qt root")
    if not (
        ui_steps.index(qt_verifier)
        < ui_steps.index(ui_compatible)
        < ui_steps.index(named_step(ui_steps, "Configure all ten modules"))
    ):
        raise RuntimeError("UI host compatibility validation is ordered incorrectly")
    verify_qt_availability_evidence()
    print(
        "GitHub Actions YAML parsed; Windows-only acquisition and compatible-host checks are ordered, "
        f"actions are immutable, and Qt {CI_QT_VERSION} {QT_ARCHITECTURE} has official availability evidence"
    )


if __name__ == "__main__":
    try:
        verify()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1) from error
