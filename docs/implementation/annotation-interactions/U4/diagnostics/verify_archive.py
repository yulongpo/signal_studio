"""Check archived references and write a byte-preserving U4 manifest."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[5]
ARCHIVE = ROOT / "docs/implementation/annotation-interactions/U4"
MANIFEST = ARCHIVE / "artifact-manifest.json"
SELF_CHECK = ARCHIVE / "diagnostics/archive-selfcheck.json"
DOCUMENTS = [ROOT / "README.md", ROOT / "docs/architecture/narrowband.md",
             ARCHIVE.parent / "EXECUTION_STATUS.md"]


def read_json(path):
    return json.loads(path.read_bytes())


def identity(path):
    payload = path.read_bytes()
    return {"path": path.relative_to(ROOT).as_posix(), "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest()}


def referenced_path(base, value):
    relative = PurePosixPath(value)
    assert not relative.is_absolute() and ".." not in relative.parts, value
    result = (base / value).resolve()
    assert result.is_relative_to(base.resolve()), value
    assert result.is_file(), str(result)
    return result


def references(value):
    if isinstance(value, dict):
        if "sha256" in value and ("path" in value or "relativePath" in value):
            yield value
        for item in value.values():
            yield from references(item)
    elif isinstance(value, list):
        for item in value:
            yield from references(item)


def verify_reference(base, ref):
    path = referenced_path(base, ref.get("path", ref.get("relativePath")))
    entry = identity(path)
    assert entry["sha256"] == ref["sha256"], str(path)
    if "bytes" in ref:
        assert entry["bytes"] == int(ref["bytes"]), str(path)
    if path.suffix == ".f32":
        count = 1
        for dimension in ref["shape"]:
            count *= dimension
        assert entry["bytes"] == count * 4, str(path)
    return path


def self_check():
    projects = []
    annotation_refs = set()
    manifests = set()
    chunks = set()
    heads = set()
    for path in sorted(ARCHIVE.rglob("*.json")):
        project = read_json(path)
        if not isinstance(project, dict) or project.get("schema") != "signal-studio-native-project":
            continue
        projects.append(path)
        base = path.parent
        for source in project.get("files", []):
            for ref in references(source.get("annotationStore", {})):
                annotation_refs.add(verify_reference(base, ref))
            for run_id in source.get("detectionRuns", []):
                run_hash = hashlib.sha256(run_id.encode("utf-8")).hexdigest()
                manifest = referenced_path(base, f"detections/{run_hash}/manifest.json")
                manifests.add(manifest)
                value = read_json(manifest)
                assert value["request"]["runId"] == run_id
                for ref in value["chunks"]:
                    chunk = verify_reference(base, ref)
                    chunks.add(chunk)
                    for head in references(read_json(chunk)):
                        head_path = verify_reference(base, head)
                        assert head_path.suffix == ".f32"
                        heads.add(head_path)

    frozen = read_json(ARCHIVE / "diagnostics/input-freeze.json")
    entries = list(frozen["inputs"])
    for config in frozen["configurations"].values():
        for group in config.values():
            entries.extend(group)
    assert len(entries) == 384
    assert all(identity(ROOT / entry["path"]) == entry for entry in entries)

    links = []
    for path in DOCUMENTS + sorted(ARCHIVE.glob("*.md")):
        text = path.read_text(encoding="utf-8")
        for target in re.findall(r"\]\(([^)]+)\)", text):
            target = target.strip("<>")
            parsed = urlsplit(target)
            if parsed.scheme or target.startswith("#"):
                continue
            output = (path.parent / unquote(parsed.path)).resolve()
            assert output.exists(), f"{path}: {target}"
            links.append({"document": path.relative_to(ROOT).as_posix(),
                          "target": target})
    result = {"node": "U4", "recordedUtc": datetime.now(timezone.utc).isoformat(),
              "pass": True, "frozenIdentities": len(entries),
              "projects": len(projects), "annotationReferences": len(annotation_refs),
              "detectionManifests": len(manifests), "detectionChunks": len(chunks),
              "rawModelHeads": len(heads), "localMarkdownLinks": len(links),
              "scope": "Archive references, paths, byte counts, SHA-256, tensor sizes, frozen runtime and local document links; external IQ is intentionally not copied"}
    SELF_CHECK.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


def make_manifest():
    files = [identity(path) for path in sorted(ARCHIVE.rglob("*"))
             if path.is_file() and path != MANIFEST]
    value = {"node": "U4", "recordedUtc": datetime.now(timezone.utc).isoformat(),
             "branch": "rebuild/a143-native",
             "implementationCommit": "c182b3ebb34632daa246ddfd41903915dd648554",
             "previousDeliveryCommit": "a69a68e5b3b7994ebfada013397e58ae154a41da",
             "scope": "Every U4 archive file except this manifest; final document inputs recorded separately; implementation/runtime freeze in diagnostics/input-freeze.json",
             "fileCount": len(files), "totalBytes": sum(item["bytes"] for item in files),
             "files": files, "documentInputs": [identity(path) for path in DOCUMENTS]}
    MANIFEST.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"files": len(files), "bytes": value["totalBytes"]}))


def verify_manifest():
    value = read_json(MANIFEST)
    expected = value["files"] + value["documentInputs"]
    assert all(identity(ROOT / item["path"]) == item for item in expected)
    actual = {path.relative_to(ROOT).as_posix() for path in ARCHIVE.rglob("*")
              if path.is_file() and path != MANIFEST}
    assert actual == {item["path"] for item in value["files"]}
    print(json.dumps({"pass": True, "archiveFiles": len(actual),
                      "documentInputs": len(value["documentInputs"])}))


if sys.argv[1] == "check":
    self_check()
elif sys.argv[1] == "manifest":
    make_manifest()
elif sys.argv[1] == "verify":
    verify_manifest()
else:
    raise SystemExit("Use check, manifest or verify")
