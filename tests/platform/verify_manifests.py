#!/usr/bin/env python3
"""Verify immutable baseline, supplied data, fixtures, and public-header boundaries."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import re


REPOSITORY = Path(__file__).resolve().parents[2]


def configure_utf8_stdio() -> None:
    """Keep diagnostics printable when Windows inherits a legacy code page."""
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name)
        reconfigure = getattr(stream, "reconfigure", None)
        if callable(reconfigure):
            reconfigure(encoding="utf-8", errors="backslashreplace")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_file(path: Path, size: int, expected_hash: str) -> None:
    if not path.is_file():
        raise RuntimeError(f"missing file: {path}")
    actual_size = path.stat().st_size
    if actual_size != size:
        raise RuntimeError(f"size mismatch for {path}: {actual_size} != {size}")
    actual_hash = sha256(path)
    if actual_hash != expected_hash:
        raise RuntimeError(f"SHA-256 mismatch for {path}: {actual_hash} != {expected_hash}")


def baseline() -> None:
    manifest_path = REPOSITORY / "docs" / "baseline" / "baseline-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    snapshot = manifest_path.parent / "Signal-Studio-Dev-Docs"
    source = (REPOSITORY / manifest["origin"]["relative_path"]).resolve()
    actual_files = sorted(path for path in snapshot.rglob("*") if path.is_file())
    if len(actual_files) != manifest["file_count"]:
        raise RuntimeError(f"baseline count mismatch: {len(actual_files)} != {manifest['file_count']}")
    for item in manifest["files"]:
        relative = Path(item["path"])
        verify_file(snapshot / relative, item["size_bytes"], item["sha256"])
        if source.is_dir():
            verify_file(source / relative, item["size_bytes"], item["sha256"])
    print(f"Verified {len(actual_files)} baseline files byte-for-byte")


def external() -> None:
    manifest = json.loads((REPOSITORY / "test_data" / "test-data-manifest.json").read_text(encoding="utf-8"))
    missing = []
    for item in manifest["files"]:
        path = (REPOSITORY / item["path"]).resolve()
        if not path.is_file():
            missing.append(path)
    if missing:
        print("External test data unavailable; skipped:\n" + "\n".join(map(str, missing)))
        raise SystemExit(77)
    for item in manifest["files"]:
        verify_file((REPOSITORY / item["path"]).resolve(), item["size_bytes"], item["sha256"])
    print(f"Verified {len(manifest['files'])} supplied external files byte-for-byte")


def minimal() -> None:
    environment = os.environ.copy()
    environment.update(PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    subprocess.run(
        [sys.executable, str(REPOSITORY / "test_data" / "generate_minimal_data.py"), "--check"],
        cwd=REPOSITORY,
        env=environment,
        check=True,
    )
    manifest_path = REPOSITORY / "test_data" / "minimal" / "manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    if b"\r" in manifest_bytes or not manifest_bytes.endswith(b"\n"):
        raise RuntimeError("minimal manifest must use UTF-8 with LF line endings")
    attribute = subprocess.run(
        ["git", "check-attr", "eol", "--", "test_data/minimal/manifest.json"],
        cwd=REPOSITORY,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        env=environment,
    ).stdout.strip()
    if not attribute.endswith(": eol: lf"):
        raise RuntimeError(f"minimal manifest lacks the committed LF checkout contract: {attribute}")


def public_headers() -> None:
    violations = []
    headers = list((REPOSITORY / "include" / "signal_studio").rglob("*.hpp"))
    headers.extend((REPOSITORY / "include" / "signal_studio").rglob("*.h"))
    prohibited = (
        "#include <qt", "#include <q", "#include \"qt", "#include \"q",
        "eigen::", "#include <eigen", "mkl_", "#include <mkl", "oneapi::mkl",
        "tbb::", "#include <tbb", "#include <oneapi/tbb", "h5::", "#include <hdf5", "#include <h5",
        "ort::", "#include <onnxruntime", "onnxruntime::", "fftw_", "#include <fftw",
        "std::__", "__gnu_cxx", "_container_base", "_compressed_pair",
    )
    for path in headers:
        text = path.read_text(encoding="utf-8")
        lowered = text.lower()
        for marker in prohibited:
            if marker in lowered:
                violations.append(f"{path.relative_to(REPOSITORY)} contains {marker}")
    if violations:
        raise RuntimeError("Third-party or implementation type leaked into public headers:\n" + "\n".join(violations))
    print(f"Verified {len(headers)} public headers: no prohibited Qt/Eigen/MKL/TBB/HDF5/ONNX/FFTW/stdlib-implementation types")


def dependency_lock() -> None:
    lock = json.loads((REPOSITORY / "dependencies" / "dependency-lock.json").read_text(encoding="utf-8"))
    cache = json.loads((REPOSITORY / "dependencies" / "offline-cache-manifest.json").read_text(encoding="utf-8"))
    vcpkg = json.loads((REPOSITORY / "vcpkg.json").read_text(encoding="utf-8"))
    approved_path = REPOSITORY / lock["approved_source"]
    approved = json.loads(approved_path.read_text(encoding="utf-8"))
    approved_fetch_path = REPOSITORY / lock["approved_fetch_script"]
    approved_fetch = approved_fetch_path.read_text(encoding="utf-8")
    fetch_assignments = dict(re.findall(r'^\$(commit|url|sha)\s*=\s*"([^"]+)"\s*$', approved_fetch, re.MULTILINE))
    if set(fetch_assignments) != {"commit", "url", "sha"}:
        raise RuntimeError("immutable BL1.0 fetch script assignments are incomplete")
    approved_commit = fetch_assignments["commit"]
    approved_url = fetch_assignments["url"].replace("$commit", approved_commit)
    approved_sha = fetch_assignments["sha"]
    archive_match = re.search(
        r'^\$archive\s*=\s*Join-Path\s+\$Destination\s+"([^"]+)"\s*$', approved_fetch, re.MULTILINE
    )
    if archive_match is None:
        raise RuntimeError("immutable BL1.0 fetch script archive path is missing")
    approved_relative_path = archive_match.group(1).replace("$commit", approved_commit)

    if lock["schema"] != "signal-studio.dependency-lock/1.4":
        raise RuntimeError("dependency lock schema mismatch")
    qt_contract = lock.get("qt_compatibility_contract", {})
    expected_qt_contract = {
        "minimum_supported_version": "6.10.3",
        "ci_validation_version": "6.10.3",
        "local_validation_version": "6.11.1",
        "bl1_0_qtbase_selected_version": "6.11.1#1",
        "bl1_0_qttools_selected_version": "6.11.1",
    }
    for field, expected in expected_qt_contract.items():
        if qt_contract.get(field) != expected:
            raise RuntimeError(f"Qt compatibility contract field mismatch: {field}")
    if not qt_contract.get("policy"):
        raise RuntimeError("Qt compatibility contract policy is missing")
    if lock["vcpkg"]["builtin_baseline"] != vcpkg["builtin-baseline"] or lock["vcpkg"]["builtin_baseline"] != approved["vcpkg_baseline"]:
        raise RuntimeError("vcpkg baseline mismatch")
    if (approved_commit, approved_sha) != (approved["vcpkg_baseline"], approved["vcpkg_archive_sha256"]):
        raise RuntimeError("immutable BL1.0 fetch script differs from its dependency lock")
    if (
        lock["vcpkg"]["archive_url"],
        lock["vcpkg"]["archive_sha256"],
        lock["vcpkg"]["archive_bytes"],
        lock["vcpkg"].get("source_policy"),
    ) != (
        approved_url,
        approved["vcpkg_archive_sha256"],
        approved["vcpkg_archive_bytes"],
        "exact-immutable-bl1.0-fetch-script",
    ):
        raise RuntimeError("vcpkg URL/hash/size policy differs from immutable BL1.0")

    approved_selected = [item for item in approved["dependencies"] if item["selected"]]
    extensions = lock.get("milestone_extensions", [])
    if (
        len(lock["selected_packages"]) != 14
        or len(approved_selected) != 14
        or len(vcpkg["dependencies"]) != 14 + len(extensions)
    ):
        raise RuntimeError("BL1.0 selected package count mismatch")
    if [item["name"] for item in lock["selected_packages"]] != vcpkg["dependencies"][:14]:
        raise RuntimeError("selected package order/names differ from vcpkg manifest")
    approved_versions = {item["name"]: item["version"] for item in approved_selected}
    if (
        qt_contract["bl1_0_qtbase_selected_version"] != approved_versions.get("qtbase")
        or qt_contract["bl1_0_qttools_selected_version"] != approved_versions.get("qttools")
    ):
        raise RuntimeError("Qt compatibility contract does not preserve immutable BL1.0 selections")
    tuple_fields = ("name", "version", "spdx", "official_url", "lock", "verification")
    for locked_package, approved_package in zip(lock["selected_packages"], approved_selected, strict=True):
        for field in tuple_fields:
            if locked_package.get(field) != approved_package.get(field):
                raise RuntimeError(
                    f"selected package {locked_package.get('name')} field {field} differs from immutable BL1.0"
                )
        approved_hash = approved_package.get("package_archive_sha256")
        if approved_hash is None:
            if locked_package.get("package_archive_sha256") is not None or locked_package.get("package_archive_hash_state") != "not-defined-by-bl1.0":
                raise RuntimeError(f"selected package {locked_package['name']} lacks the required BL1.0 hash policy")
        elif locked_package.get("package_archive_sha256") != approved_hash:
            raise RuntimeError(f"selected package {locked_package['name']} archive hash differs from immutable BL1.0")

    expected_extension = {
        "milestone": "MS-02",
        "name": "libsamplerate",
        "version": "0.2.2#1",
        "spdx": "BSD-2-Clause",
        "official_url": "https://github.com/libsndfile/libsamplerate",
        "lock": "vcpkg baseline 82b6bc886d7b0f8342e34babc2e0b8943f79b0e1; source ref 0.2.2; port-version 1",
        "verification": "vcpkg port source SHA512:37e0fd604907caf978659466289315befd66eec16c21a94e0b6106de18ffe803fbf2e7f3a8fb0430b33c0b784ecd6d4eaf3b9a862ed2670104647decbee913d6",
        "package_archive_sha512": "37e0fd604907caf978659466289315befd66eec16c21a94e0b6106de18ffe803fbf2e7f3a8fb0430b33c0b784ecd6d4eaf3b9a862ed2670104647decbee913d6",
    }

    def validate_extension(extension: dict[str, object]) -> None:
        for field, expected in expected_extension.items():
            if extension.get(field) != expected:
                raise RuntimeError(f"milestone dependency extension field mismatch: {field}")
        if not extension.get("policy"):
            raise RuntimeError("milestone dependency extension policy is missing")

    if len(extensions) != 1 or vcpkg["dependencies"][14:] != [extensions[0].get("name")]:
        raise RuntimeError("milestone dependency extension order/name mismatch")
    validate_extension(extensions[0])
    rejected_extension = dict(extensions[0])
    rejected_extension["version"] = "0.2.2"
    try:
        validate_extension(rejected_extension)
    except RuntimeError as exception:
        if "version" not in str(exception):
            raise
    else:
        raise RuntimeError("negative dependency extension lock test unexpectedly accepted a floating version")

    compatibility = lock.get("host_compatibility_contract", {})
    if (
        compatibility.get("schema") != "signal-studio.host-compatibility/1.0"
        or compatibility.get("platform") != "windows"
        or compatibility.get("architecture") != "x64"
        or not compatibility.get("policy")
    ):
        raise RuntimeError("portable host compatibility contract is incomplete")
    compatibility_tools = compatibility.get("tools", [])
    expected_families = {
        "Git": "git",
        "CMake": "cmake",
        "Ninja": "ninja",
        "Qt": "qt-msvc2022",
        "MSVC": "msvc",
        "Windows SDK": "windows-sdk",
        "Python": "cpython",
    }
    if len(compatibility_tools) != len(expected_families) or {item.get("name") for item in compatibility_tools} != set(expected_families):
        raise RuntimeError("portable host compatibility tool set differs from the MS-00 contract")
    for requirement in compatibility_tools:
        if (
            requirement.get("tool_family") != expected_families[requirement["name"]]
            or not requirement.get("minimum_version")
            or not requirement.get("maximum_version_exclusive")
            or "required_headless" not in requirement
            or "required_ui" not in requirement
        ):
            raise RuntimeError(f"portable host compatibility entry is incomplete: {requirement.get('name')}")

    allowed_unlocked_states = {"not-defined-by-bl1.0", "channel-managed-not-defined-by-bl1.0"}
    acquisition_contracts = lock.get("tool_acquisition_contracts", [])
    if len(acquisition_contracts) != 8 or len({item.get("name") for item in acquisition_contracts}) != 8:
        raise RuntimeError("expected eight unique tool acquisition contracts")
    for acquisition in acquisition_contracts:
        if acquisition.get("state") == "locked-by-bl1.0":
            if (
                not acquisition.get("version")
                or not acquisition.get("artifact_url")
                or not re.fullmatch(r"[0-9a-f]{64}", acquisition.get("artifact_sha256", ""))
                or not acquisition.get("policy")
            ):
                raise RuntimeError(f"BL1.0 acquisition contract is incomplete: {acquisition.get('name')}")
        elif acquisition.get("state") in allowed_unlocked_states:
            if acquisition.get("artifact_url") is not None or acquisition.get("artifact_sha256") is not None or not acquisition.get("policy"):
                raise RuntimeError(f"unlocked acquisition state is not explicit: {acquisition.get('name')}")
        else:
            raise RuntimeError(f"invalid acquisition contract state: {acquisition.get('name')}")

    captured_path = REPOSITORY / lock.get("captured_host_evidence", "")
    captured = json.loads(captured_path.read_text(encoding="utf-8"))
    if (
        captured.get("schema") != "signal-studio.captured-host-evidence/1.0"
        or captured.get("platform") != "windows"
        or captured.get("architecture") != "x64"
    ):
        raise RuntimeError("captured host evidence schema/platform is invalid")
    captured_tools = captured.get("tools", [])
    if len(captured_tools) != 8 or len({item.get("name") for item in captured_tools}) != 8:
        raise RuntimeError("captured host evidence must contain eight unique tool entries")
    for tool in captured_tools:
        if tool.get("state") == "detected":
            for field in ("tool_family", "architecture", "version", "path", "file_sha256", "hash_scope"):
                if not tool.get(field):
                    raise RuntimeError(f"captured tool {tool.get('name')} lacks {field}")
            if not re.fullmatch(r"[0-9a-f]{64}", tool["file_sha256"]):
                raise RuntimeError(f"captured tool hash is invalid: {tool.get('name')}")
        elif tool.get("state") == "not-detected":
            if any(tool.get(field) is not None for field in ("version", "path", "file_sha256", "hash_scope")):
                raise RuntimeError(f"unavailable captured tool asserts transient evidence: {tool.get('name')}")
        else:
            raise RuntimeError(f"invalid captured tool state: {tool.get('name')}")

    cuda_approved = next(item for item in approved["dependencies"] if item["name"] == "CUDA Toolkit/cuFFT")
    cuda = next(item for item in acquisition_contracts if item["name"] == "CUDA Toolkit")
    captured_cuda = next(item for item in captured_tools if item["name"] == "CUDA Toolkit")
    if captured_cuda["state"] != "not-detected":
        raise RuntimeError("CUDA must remain explicitly unavailable on the captured MS-00 host")
    expected_cuda_hash = cuda_approved["verification"].removeprefix("sha256:")
    if (cuda.get("version"), cuda.get("artifact_url"), cuda.get("artifact_sha256")) != (
        cuda_approved["version"], cuda_approved["official_url"], expected_cuda_hash
    ):
        raise RuntimeError("CUDA acquisition tuple differs from immutable BL1.0")

    for artifact in cache["artifacts"]:
        if not re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]):
            raise RuntimeError(f"invalid offline artifact hash: {artifact['id']}")
    vcpkg_cache, cuda_cache = cache["artifacts"]
    if (vcpkg_cache["relative_path"], vcpkg_cache["url"], vcpkg_cache["sha256"], vcpkg_cache["bytes"]) != (
        approved_relative_path,
        lock["vcpkg"]["archive_url"],
        lock["vcpkg"]["archive_sha256"],
        lock["vcpkg"]["archive_bytes"],
    ):
        raise RuntimeError("vcpkg offline artifact differs from the acquisition lock")
    if (cuda_cache["url"], cuda_cache["sha256"]) != (
        cuda["artifact_url"], cuda["artifact_sha256"]
    ):
        raise RuntimeError("CUDA offline artifact differs from the acquisition lock")
    print("Verified 14 immutable BL1.0 package tuples, 1 locked milestone extension, 8 acquisition contracts, portable host bounds, captured-host evidence, and 2 offline artifacts")


def portable_config() -> None:
    files = [REPOSITORY / "CMakePresets.json"] + sorted((REPOSITORY / ".vscode").glob("*.json"))
    violations = []
    absolute_windows_path = re.compile(r"(?i)(?:[a-z]:[/\\]|program files|msvc[/\\][0-9])")
    for path in files:
        text = path.read_text(encoding="utf-8")
        if absolute_windows_path.search(text):
            violations.append(str(path.relative_to(REPOSITORY)))
    if violations:
        raise RuntimeError("host-specific path in committed build/IDE configuration: " + ", ".join(violations))
    settings = json.loads((REPOSITORY / ".vscode" / "settings.json").read_text(encoding="utf-8"))
    if settings.get("cmake.configurePreset") != "local-windows-msvc-debug":
        raise RuntimeError("CMake Tools must use the discovered local user preset")
    if "/CMakeUserPresets.json" not in (REPOSITORY / ".gitignore").read_text(encoding="utf-8"):
        raise RuntimeError("generated host-specific CMakeUserPresets.json must remain ignored")
    minimum = "6.10.3"
    cmake_project = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
    package_config = (REPOSITORY / "cmake" / "SignalStudioConfig.cmake.in").read_text(encoding="utf-8")
    common_powershell = (REPOSITORY / "scripts" / "common.ps1").read_text(encoding="utf-8")
    modules = {
        "Visualization": REPOSITORY / "src" / "platform" / "visualization" / "module.cpp",
        "Workbench": REPOSITORY / "src" / "platform" / "workbench" / "module.cpp",
    }
    if f'set(SIGNAL_STUDIO_MINIMUM_QT_VERSION "{minimum}")' not in cmake_project:
        raise RuntimeError("CMake project does not enforce the supported Qt 6.10.3 minimum")
    if "find_dependency(Qt6 @SIGNAL_STUDIO_MINIMUM_QT_VERSION@" not in package_config:
        raise RuntimeError("installed UI package does not inherit the supported Qt minimum")
    if common_powershell.count("[version]'6.10.3'") < 2:
        raise RuntimeError("local Qt discovery does not enforce the supported Qt minimum")
    if "version 6.10.3 or newer" not in common_powershell:
        raise RuntimeError("local Qt discovery lacks a clear below-minimum diagnostic")
    for module_name, path in modules.items():
        module_source = path.read_text(encoding="utf-8")
        if "QT_VERSION_CHECK(6, 10, 3)" not in module_source:
            raise RuntimeError(f"{module_name} compile-time Qt minimum differs from 6.10.3")
        if f"SignalStudio::{module_name} requires Qt 6.10.3 or newer" not in module_source:
            raise RuntimeError(f"{module_name} lacks a clear below-minimum diagnostic")
    minimum_contract_files = [cmake_project, package_config, common_powershell]
    minimum_contract_files.extend(path.read_text(encoding="utf-8") for path in modules.values())
    forbidden_minimum = re.compile(
        r"(?:QT_VERSION_CHECK\(\s*6\s*,\s*11|find_(?:package|dependency)\(Qt6\s+6\.11|"
        r"\[version\]'6\.11\.0')"
    )
    if any(forbidden_minimum.search(text) for text in minimum_contract_files):
        raise RuntimeError("local Qt 6.11 identity was reintroduced as a source/configuration minimum")
    print(f"Verified {len(files)} committed preset/VS Code files contain no host-specific absolute paths")


COMMANDS = {
    "baseline": baseline,
    "external": external,
    "minimal": minimal,
    "public-headers": public_headers,
    "dependency-lock": dependency_lock,
    "portable-config": portable_config,
}


def main() -> int:
    configure_utf8_stdio()
    if len(sys.argv) != 2 or sys.argv[1] not in COMMANDS:
        print(f"usage: {Path(sys.argv[0]).name} [{'|'.join(COMMANDS)}]", file=sys.stderr)
        return 2
    try:
        COMMANDS[sys.argv[1]]()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
