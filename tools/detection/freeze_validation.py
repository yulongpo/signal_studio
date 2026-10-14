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
    "application", "domain", "infrastructure", "ui", "app", "tools", "tests",
}


def mutable_build_input(path: Path, repository: Path) -> bool:
    try:
        relative = path.resolve().relative_to(repository.resolve())
    except ValueError:
        return False
    if relative.parts[0] in SOURCE_DIRECTORIES:
        return True
    return len(relative.parts) > 1 and relative.parts[0] == "out" and (
        relative.parts[1] in {
            "vs2026-qt611-release_bin", "vs2026-qt611-debug_bin",
        }
    )


def create(template: Path, output: Path, repository: Path) -> dict:
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
            raise ValueError(f"Immutable model/data/reference asset changed: {path}")
    # Dataset records are preserved, not rebaselined to whatever is on disk.
    for item in frozen.get("datasetFiles", []):
        path = Path(item["path"]).resolve()
        if sha256(path) != item["sha256"]:
            raise ValueError(f"Frozen dataset changed: {path}")
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
    assert not mutable_build_input(REPOSITORY / "docs/frozen.json", REPOSITORY)
    assert not mutable_build_input(REPOSITORY / "out/tasks/model.engine", REPOSITORY)
    directory = REPOSITORY / "out/tasks/wideband-detection/M5/diagnostics"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="freeze-check-", dir=directory) as name:
        root = Path(name)
        immutable = root / "asset.txt"
        immutable.write_text("original", encoding="utf-8")
        template = root / "template.json"
        template.write_text(json.dumps({
            "schema": "signalstudio-m4-execution-freeze-v1",
            "files": [{"path": str(immutable), "sha256": sha256(immutable)}],
        }), encoding="utf-8")
        created = root / "new.json"
        create(template, created, REPOSITORY)
        try:
            create(template, created, REPOSITORY)
            raise AssertionError("An existing freeze was overwritten")
        except ValueError:
            pass
        immutable.write_text("changed", encoding="utf-8")
        try:
            create(template, root / "rejected.json", REPOSITORY)
            raise AssertionError("Changed immutable input was rebaselined")
        except ValueError:
            pass
    return {"pass": True, "oldFreezeOverwriteRejected": True,
            "changedImmutableInputRejected": True, "inferenceStarted": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2))
        return 0
    if not args.template or not args.output:
        parser.error("--template and --output are required")
    frozen = create(args.template.resolve(), args.output.resolve(), REPOSITORY)
    print(json.dumps({"freeze": str(args.output), "files": len(frozen["files"]),
                      "sha256": sha256(args.output), "inferenceStarted": False},
                     indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
