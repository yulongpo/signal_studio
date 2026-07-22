#!/usr/bin/env python3
"""Validate the deterministic VS Code, CMake preset, and F5 target workflow."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


REPOSITORY = Path(__file__).resolve().parents[1]
LOCAL_DEBUG_PRESET = "local-windows-msvc-debug"
LOCAL_RELEASE_PRESET = "local-windows-msvc-release"
SOURCE_DEBUG_PRESET = "windows-msvc-debug"
TARGET_NAME = "signal_studio_platform_tests"
TARGET_RELATIVE_PATH = Path("build") / LOCAL_DEBUG_PRESET / "bin" / f"{TARGET_NAME}.exe"


def read_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"unable to read JSON {path.relative_to(REPOSITORY)}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON root must be an object: {path.relative_to(REPOSITORY)}")
    return value


def task_by_label(tasks: list[object], label: str) -> dict[str, object]:
    for task in tasks:
        if isinstance(task, dict) and task.get("label") == label:
            return task
    raise RuntimeError(f"missing VS Code task: {label}")


def task_preset(task: dict[str, object]) -> str:
    args = task.get("args")
    if not isinstance(args, list):
        raise RuntimeError(f"task args must be a list: {task.get('label')}")
    try:
        index = args.index("-Preset")
        preset = args[index + 1]
    except (ValueError, IndexError) as error:
        raise RuntimeError(f"task lacks -Preset: {task.get('label')}") from error
    if not isinstance(preset, str):
        raise RuntimeError(f"task preset must be a string: {task.get('label')}")
    return preset


def verify_static_contract() -> None:
    settings = read_json(REPOSITORY / ".vscode" / "settings.json")
    tasks_document = read_json(REPOSITORY / ".vscode" / "tasks.json")
    launch_document = read_json(REPOSITORY / ".vscode" / "launch.json")
    presets = read_json(REPOSITORY / "CMakePresets.json")

    for setting in ("cmake.configurePreset", "cmake.buildPreset", "cmake.testPreset"):
        if settings.get(setting) != LOCAL_DEBUG_PRESET:
            raise RuntimeError(f"{setting} must use {LOCAL_DEBUG_PRESET}")
    if settings.get("cmake.buildBeforeRun") is not False:
        raise RuntimeError("CMake Tools implicit pre-run build must be disabled; the validated task chain owns F5")

    tasks = tasks_document.get("tasks")
    if not isinstance(tasks, list):
        raise RuntimeError("VS Code tasks must be a sequence")
    expected_task_presets = {
        "Signal Studio: Configure Debug": LOCAL_DEBUG_PRESET,
        "Signal Studio: Build Debug": LOCAL_DEBUG_PRESET,
        "Signal Studio: Test Debug": LOCAL_DEBUG_PRESET,
        "Signal Studio: Configure Release": LOCAL_RELEASE_PRESET,
        "Signal Studio: Build Release": LOCAL_RELEASE_PRESET,
        "Signal Studio: Test Release": LOCAL_RELEASE_PRESET,
    }
    for label, expected in expected_task_presets.items():
        if task_preset(task_by_label(tasks, label)) != expected:
            raise RuntimeError(f"VS Code task preset diverges from the local tree: {label}")
    if task_by_label(tasks, "Signal Studio: Build Debug").get("dependsOn") != "Signal Studio: Configure Debug":
        raise RuntimeError("Debug build must depend on local Debug configure")
    if task_by_label(tasks, "Signal Studio: Test Debug").get("dependsOn") != "Signal Studio: Build Debug":
        raise RuntimeError("Debug test must depend on local Debug build")
    if task_by_label(tasks, "Signal Studio: Build Release").get("dependsOn") != "Signal Studio: Configure Release":
        raise RuntimeError("Release build must depend on local Release configure")
    if task_by_label(tasks, "Signal Studio: Test Release").get("dependsOn") != "Signal Studio: Build Release":
        raise RuntimeError("Release test must depend on local Release build")
    validator_task = task_by_label(tasks, "Signal Studio: Validate Debug Tree")
    if validator_task.get("dependsOn") != "Signal Studio: Build Debug":
        raise RuntimeError("F5 validator must depend on the local Debug build")
    validator_args = validator_task.get("args")
    if not isinstance(validator_args, list) or "--require-configured-target" not in validator_args:
        raise RuntimeError("F5 validator task must reject an unconfigured or stale target")

    configurations = launch_document.get("configurations")
    if not isinstance(configurations, list) or len(configurations) != 1 or not isinstance(configurations[0], dict):
        raise RuntimeError("MS-00 must expose one deterministic F5 configuration")
    launch = configurations[0]
    expected_program = "${workspaceFolder}/" + TARGET_RELATIVE_PATH.as_posix()
    if launch.get("program") != expected_program:
        raise RuntimeError(f"F5 program must resolve from {LOCAL_DEBUG_PRESET}")
    if launch.get("preLaunchTask") != "Signal Studio: Validate Debug Tree":
        raise RuntimeError("F5 must use the configure/build/target-validation task chain")
    if launch.get("args") != ["--case", "core.version"]:
        raise RuntimeError("F5 smoke target arguments changed")

    configure_presets = presets.get("configurePresets")
    if not isinstance(configure_presets, list):
        raise RuntimeError("CMake configure presets must be a sequence")
    source_debug = next(
        (item for item in configure_presets if isinstance(item, dict) and item.get("name") == SOURCE_DEBUG_PRESET), None
    )
    base = next(
        (item for item in configure_presets if isinstance(item, dict) and item.get("name") == "windows-msvc-base"), None
    )
    if not isinstance(source_debug, dict) or not isinstance(base, dict):
        raise RuntimeError("committed Debug/base CMake presets are missing")
    if base.get("binaryDir") != "${sourceDir}/build/${presetName}":
        raise RuntimeError("CMake preset binaryDir must follow the final local preset name")

    common_script = (REPOSITORY / "scripts" / "common.ps1").read_text(encoding="utf-8")
    for marker in (
        '$localName = "local-$sourcePreset"',
        "Get-SignalStudioSourcePreset",
        "configurePreset = $localName",
    ):
        if marker not in common_script:
            raise RuntimeError(f"local preset generation/acceptance contract is missing: {marker}")

    user_presets_path = REPOSITORY / "CMakeUserPresets.json"
    if user_presets_path.is_file():
        user_presets = read_json(user_presets_path)
        for collection_name in ("configurePresets", "buildPresets", "testPresets"):
            collection = user_presets.get(collection_name)
            if not isinstance(collection, list) or not any(
                isinstance(item, dict) and item.get("name") == LOCAL_DEBUG_PRESET for item in collection
            ):
                raise RuntimeError(f"generated user presets lack {LOCAL_DEBUG_PRESET} in {collection_name}")
        local_configure = next(
            item for item in user_presets["configurePresets"]
            if isinstance(item, dict) and item.get("name") == LOCAL_DEBUG_PRESET
        )
        inherits = local_configure.get("inherits")
        if not isinstance(inherits, list) or SOURCE_DEBUG_PRESET not in inherits:
            raise RuntimeError("generated local Debug preset does not inherit the committed Debug contract")


def parse_cache(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        values[key] = value
    return values


def verify_configured_target() -> None:
    build_dir = REPOSITORY / "build" / LOCAL_DEBUG_PRESET
    cache_path = build_dir / "CMakeCache.txt"
    target_path = REPOSITORY / TARGET_RELATIVE_PATH
    build_graph = build_dir / "build.ninja"
    for path in (cache_path, target_path, build_graph):
        if not path.is_file():
            raise RuntimeError(f"F5 target is unconfigured or missing: {path.relative_to(REPOSITORY)}")
    cache = parse_cache(cache_path)
    expected_cache = {
        "CMAKE_HOME_DIRECTORY": str(REPOSITORY).replace("\\", "/"),
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_GENERATOR": "Ninja",
        "SIGNAL_STUDIO_BUILD_UI": "ON",
    }
    for key, expected in expected_cache.items():
        actual = cache.get(key, "").replace("\\", "/")
        if actual.casefold() != expected.casefold():
            raise RuntimeError(f"F5 cache mismatch for {key}: {actual!r} != {expected!r}")
    if f"bin/{TARGET_NAME}.exe" not in build_graph.read_text(encoding="utf-8", errors="replace").replace("\\", "/"):
        raise RuntimeError("F5 target is absent from the local Debug build graph")

    inputs = [REPOSITORY / "CMakeLists.txt", REPOSITORY / "CMakePresets.json"]
    inputs.extend((REPOSITORY / "cmake").rglob("*.cmake"))
    inputs.extend((REPOSITORY / "include").rglob("*.hpp"))
    inputs.extend((REPOSITORY / "include").rglob("*.h"))
    inputs.extend((REPOSITORY / "src").rglob("*.cpp"))
    inputs.append(REPOSITORY / "tests" / "platform" / "test_main.cpp")
    newest_input = max(path.stat().st_mtime_ns for path in inputs if path.is_file())
    if target_path.stat().st_mtime_ns < newest_input:
        raise RuntimeError("F5 target is stale relative to committed CMake/C++ inputs; rebuild the local Debug tree")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--require-configured-target", action="store_true")
    args = parser.parse_args()
    try:
        verify_static_contract()
        if args.require_configured_target:
            verify_configured_target()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    suffix = " and configured non-stale F5 target" if args.require_configured_target else ""
    print(f"Verified coherent VS Code local preset/tree contract{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
