"""Create a new manual validation identity; never rewrite old outcomes."""

from __future__ import annotations

import argparse
import datetime
import json
from pathlib import Path
import subprocess
import tempfile

from time_evaluation import read_json, sha256

REPOSITORY = Path(__file__).resolve().parents[2]
SOURCE_DIRECTORIES = {
    "application",
    "domain",
    "infrastructure",
    "ui",
    "app",
    "tools",
    "tests",
}
BUILD_DIRECTORIES = SOURCE_DIRECTORIES | {"scripts", "cmake", "resources"}
BUILD_ROOT_FILES = {"CMakeLists.txt", "CMakePresets.json", ".clang-format"}


def current_build_inputs(repository: Path) -> list[Path]:
    """Include new source files and deployed engines absent from old templates."""
    tracked = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=repository, text=True, encoding="utf-8"
    ).split("\0")
    paths = {
        repository / relative
        for relative in tracked
        if relative
        and (
            Path(relative).parts[0] in BUILD_DIRECTORIES
            or relative in BUILD_ROOT_FILES
        )
    }
    for configuration in ("debug", "release"):
        binary = repository / f"out/vs2026-qt611-{configuration}_bin"
        paths.update(path for path in binary.rglob("*") if path.is_file())
    return sorted(paths)


def extend_identity(frozen: dict, paths: list[Path]) -> None:
    entries = {
        str(Path(item["path"]).resolve()): item for item in frozen["files"]
    }
    for path in paths:
        resolved = path.resolve()
        entries[str(resolved)] = {
            "path": str(resolved),
            "sha256": sha256(resolved),
            "bytes": resolved.stat().st_size,
        }
    frozen["files"] = [entries[key] for key in sorted(entries)]


def mutable_build_input(path: Path, repository: Path) -> bool:
    try:
        relative = path.resolve().relative_to(repository.resolve())
    except ValueError:
        return False
    if (
        relative.parts[0] in BUILD_DIRECTORIES
        or relative.as_posix() in BUILD_ROOT_FILES
    ):
        return True
    return (
        len(relative.parts) > 1
        and relative.parts[0] == "out"
        and (
            relative.parts[1]
            in {
                "vs2026-qt611-release_bin",
                "vs2026-qt611-debug_bin",
            }
        )
    )


def create(
    template: Path,
    output: Path,
    repository: Path,
    include_current_inputs: bool = False,
) -> dict:
    if output.exists():
        raise ValueError("Use a new freeze path; old identities are immutable")
    frozen = read_json(template)
    if frozen.get("schema") != "signalstudio-m4-execution-freeze-v1":
        raise ValueError("Expected an existing M4 execution identity template")
    for item in frozen["files"]:
        path = Path(item["path"]).resolve()
        digest = sha256(path)
        if mutable_build_input(path, repository):
            item["sha256"] = digest
            item["bytes"] = path.stat().st_size
        elif digest != item["sha256"]:
            raise ValueError(
                f"Immutable model/data/reference asset changed: {path}"
            )
    # The actual M4 template uses dataAssetsVerifiedBeforeInference. Preserve
    # its identities as well as the older datasetFiles spelling.
    records = frozen.get("dataAssetsVerifiedBeforeInference", [])
    if "dataAssetCount" in frozen and len(records) != frozen["dataAssetCount"]:
        raise ValueError("Frozen dataset list differs from its declared count")
    for collection in (records, frozen.get("datasetFiles", [])):
        for item in collection:
            path = Path(item["path"]).resolve()
            if not path.is_file():
                raise ValueError(f"Frozen dataset missing: {path}")
            if sha256(path) != item["sha256"]:
                raise ValueError(f"Frozen dataset changed: {path}")
    if include_current_inputs:
        extend_identity(frozen, current_build_inputs(repository))
        frozen["currentBuildInputsIncluded"] = True
        frozen["inputAdaptationVersion"] = 2
    frozen["baseCommit"] = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=repository, text=True
    ).strip()
    frozen["createdUtc"] = datetime.datetime.now(
        datetime.timezone.utc
    ).isoformat()
    frozen["freezeId"] = output.stem
    frozen["supersedesFreezeSha256"] = sha256(template)
    frozen["changesAfterOutcomes"] = (
        "Explicit new manual run identity of current build/source bytes; "
        "old failures, interruptions, thresholds and data remain unchanged."
    )
    frozen["replacesPrimaryConfiguration"] = (
        "Manual full-matrix handover after M5; this freeze is not a test result."
    )
    frozen["manualHandover"] = True
    frozen["sourceIdentityPolicy"] = (
        "baseCommit plus per-file working/build bytes; dirty state is not hidden"
    )
    frozen["workingTreeStatus"] = subprocess.check_output(
        ["git", "status", "--porcelain"], cwd=repository, text=True
    ).splitlines()
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation prevents an accidental second invocation from replacing
    # the first run identity even if another process creates the file meanwhile.
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(frozen, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    return frozen


def self_check() -> dict:
    assert mutable_build_input(REPOSITORY / "domain/detection.h", REPOSITORY)
    assert mutable_build_input(
        REPOSITORY / "scripts/screenshot.ps1", REPOSITORY
    )
    assert mutable_build_input(REPOSITORY / "CMakeLists.txt", REPOSITORY)
    assert mutable_build_input(
        REPOSITORY / "resources/branding.qrc", REPOSITORY
    )
    assert not mutable_build_input(REPOSITORY / "docs/frozen.json", REPOSITORY)
    assert not mutable_build_input(
        REPOSITORY / "out/tasks/model.engine", REPOSITORY
    )
    build_inputs = current_build_inputs(REPOSITORY)
    assert Path(__file__).resolve() in build_inputs
    assert any(
        path.is_relative_to(REPOSITORY / "resources") for path in build_inputs
    )
    directory = REPOSITORY / "out/tasks/wideband-detection/R6/diagnostics"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="freeze-check-", dir=directory
    ) as name:
        root = Path(name).resolve()
        assert root.is_relative_to(directory.resolve())
        immutable = root / "asset.txt"
        immutable.write_text("original", encoding="utf-8")
        engine = root / "new-dynamic.engine"
        engine.write_bytes(b"synthetic identity fixture, not a model")
        extended = {
            "files": [{"path": str(immutable), "sha256": sha256(immutable)}]
        }
        extend_identity(extended, [engine, immutable, engine])
        assert len(extended["files"]) == 2
        entry = next(
            item for item in extended["files"] if Path(item["path"]) == engine
        )
        assert entry["sha256"] == sha256(engine)
        engine.write_bytes(b"changed synthetic identity fixture")
        extend_identity(extended, [engine])
        assert next(
            item for item in extended["files"] if Path(item["path"]) == engine
        )["sha256"] == sha256(engine)
        dataset = root / "dataset.wav"
        dataset.write_bytes(b"synthetic frozen dataset identity only")
        dataset_entry = {"path": str(dataset), "sha256": sha256(dataset)}
        template = root / "template.json"
        template.write_text(
            json.dumps(
                {
                    "schema": "signalstudio-m4-execution-freeze-v1",
                    "files": [
                        {"path": str(immutable), "sha256": sha256(immutable)}
                    ],
                    "dataAssetsVerifiedBeforeInference": [dataset_entry],
                    "dataAssetCount": 1,
                }
            ),
            encoding="utf-8",
        )
        created = root / "new.json"
        result = create(
            template, created, REPOSITORY, include_current_inputs=True
        )
        assert result["currentBuildInputsIncluded"]
        assert result["inputAdaptationVersion"] == 2
        assert any(
            Path(item["path"]) == Path(__file__).resolve()
            for item in result["files"]
        )
        try:
            create(template, created, REPOSITORY)
            raise AssertionError("An existing freeze was overwritten")
        except ValueError:
            pass
        immutable.write_text("original", encoding="utf-8")
        for scenario in ("changed", "missing"):
            if scenario == "changed":
                dataset.write_bytes(b"changed dataset, never rebaseline")
            else:
                dataset.unlink()
            rejected = root / f"dataset-{scenario}.json"
            try:
                create(template, rejected, REPOSITORY)
                raise AssertionError("Changed or missing dataset was accepted")
            except ValueError as error:
                assert "Frozen dataset" in str(error)
            assert not rejected.exists()
        legacy = read_json(template)
        legacy.pop("dataAssetsVerifiedBeforeInference")
        legacy.pop("dataAssetCount")
        legacy["datasetFiles"] = [dataset_entry]
        legacy_template = root / "legacy-template.json"
        legacy_template.write_text(json.dumps(legacy), encoding="utf-8")
        dataset.write_bytes(b"changed legacy dataset, never rebaseline")
        try:
            create(legacy_template, root / "legacy-rejected.json", REPOSITORY)
            raise AssertionError("Legacy changed dataset was accepted")
        except ValueError as error:
            assert "Frozen dataset" in str(error)
        immutable.write_text("changed", encoding="utf-8")
        try:
            create(template, root / "rejected.json", REPOSITORY)
            raise AssertionError("Changed immutable input was rebaselined")
        except ValueError:
            pass
    return {
        "pass": True,
        "oldFreezeOverwriteRejected": True,
        "changedImmutableInputRejected": True,
        "newRuntimeIdentityIncluded": True,
        "trackedResourcesIncluded": True,
        "datasetChangeAndMissingRejected": True,
        "legacyDatasetChangeRejected": True,
        "gpuUsed": False,
        "inferenceStarted": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--include-current-inputs",
        action="store_true",
        help="Add all tracked build inputs and both deployed "
        "packages, including new engine filenames",
    )
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2))
        return 0
    if not args.template or not args.output:
        parser.error("--template and --output are required")
    frozen = create(
        args.template.resolve(),
        args.output.resolve(),
        REPOSITORY,
        args.include_current_inputs,
    )
    print(
        json.dumps(
            {
                "freeze": str(args.output),
                "files": len(frozen["files"]),
                "sha256": sha256(args.output),
                "inferenceStarted": False,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
