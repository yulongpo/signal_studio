"""Validate frozen M0 schemas and source-derived examples; never runs inference."""
from __future__ import annotations

import copy
import json
import math
import re
from pathlib import Path, PureWindowsPath
import sys

from jsonschema import Draft202012Validator, ValidationError

ROOT = Path(__file__).resolve().parents[1]
UINT64_MAX = 2**64 - 1


def read_json(path: Path):
    def reject(value):
        raise ValueError(f"Non-finite JSON number: {value}")
    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            reject(value)
        return number
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=reject,
                      parse_float=finite_float)


def finite_tree(value):
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError("Non-finite JSON number")
    if isinstance(value, dict):
        for child in value.values():
            finite_tree(child)
    elif isinstance(value, list):
        for child in value:
            finite_tree(child)


def sample(value: str) -> int:
    if not isinstance(value, str) or not re.fullmatch(r"0|[1-9][0-9]{0,19}", value):
        raise ValueError("Sample index must be canonical decimal text")
    parsed = int(value)
    if parsed < 0 or parsed > UINT64_MAX:
        raise ValueError("Sample index outside uint64")
    return parsed


def validate_range(value: dict):
    if sample(value["startSample"]) >= sample(value["endSample"]):
        raise ValueError("Empty/reversed sample interval")
    lower, upper = value["frequencyLowerHz"], value["frequencyUpperHz"]
    if not math.isfinite(lower) or not math.isfinite(upper) or lower >= upper:
        raise ValueError("Invalid frequency interval")


def validate_annotation(value: dict, schema: dict):
    finite_tree(value)
    Draft202012Validator(schema).validate(value)
    ids = set()
    for segment in value["segments"]:
        if segment["id"] in ids:
            raise ValueError("Duplicate segment ID")
        ids.add(segment["id"])
        validate_range(segment["range"])
        if "uncertaintySamples" in segment:
            sample(segment["uncertaintySamples"])
    if value.get("observation"):
        validate_range(value["observation"])
    if not value["segments"] and not value.get("observation"):
        raise ValueError("Annotation needs segments or an observation")
    evidence = value["originEvidence"]
    scores = [evidence.get("scoreMean"), evidence.get("scoreMax")]
    if any(s is not None and not math.isfinite(s) for s in scores):
        raise ValueError("Non-finite model score")
    if all(s is not None for s in scores) and scores[0] > scores[1]:
        raise ValueError("Mean score exceeds maximum")
    windows = evidence.get("windowIds", [])
    for index in windows:
        sample(index)
    if len(windows) > evidence.get("windowCount", len(windows)):
        raise ValueError("Hit windows exceed total windows")
    if value["sourceKind"] != "Algorithm" and any(s is not None for s in scores):
        raise ValueError("Non-algorithm annotation cannot claim model scores")


def validate_run(value: dict, schema: dict):
    finite_tree(value)
    Draft202012Validator(schema).validate(value)
    sample(value["projectGeneration"])
    identity = value["sourceIdentity"]
    sample(identity["formatRevision"])
    if sample(identity["availableSamples"]) > sample(identity["physicalSamples"]):
        raise ValueError("Available prefix exceeds physical source")
    validate_range(value["requestedRange"])
    if sample(value["requestedRange"]["endSample"]) > sample(identity["availableSamples"]):
        raise ValueError("Request exceeds available prefix")
    if value.get("contextRange"):
        validate_range(value["contextRange"])
        if sample(value["contextRange"]["endSample"]) > sample(identity["availableSamples"]):
            raise ValueError("Context exceeds available prefix")
    for part in value["coverage"]:
        validate_range(part["range"])
        if sample(part["range"]["endSample"]) > sample(identity["availableSamples"]):
            raise ValueError("Coverage exceeds available prefix")
    sequences = set()
    for chunk in value["chunks"]:
        sequence = sample(chunk["sequence"])
        if sequence in sequences:
            raise ValueError("Duplicate result sequence")
        sequences.add(sequence)
        path = PureWindowsPath(chunk["relativePath"])
        if path.root or path.drive or ".." in path.parts:
            raise ValueError("Result path must stay in project")


def validate_worker(value: dict, schema: dict):
    finite_tree(value)
    Draft202012Validator(schema).validate(value)
    sample(value["sequence"])
    if "projectGeneration" in value:
        sample(value["projectGeneration"])
    if len(json.dumps(value, allow_nan=False, ensure_ascii=False).encode("utf-8")) > 65536:
        raise ValueError("Control envelope exceeds 64 KiB")
    binary = value.get("binary")
    if binary:
        item_bytes = {"float32": 4, "complex64": 8, "uint8": 1}[binary["dtype"]]
        if math.prod(binary["shape"]) * item_bytes != binary["byteLength"]:
            raise ValueError("Binary shape and byte length disagree")


def annotation_from_meta(meta: dict, index: int):
    row = meta["annotations"][index]
    return {
        "schemaVersion": 1, "id": f"label-{index}", "fileId": "torchsig-0000000",
        "sourceSetId": "torchsig-set", "sourceKind": "ImportedLabel",
        "originRecordId": str(index), "revisionId": "origin", "reviewStatus": "Pending",
        "name": row["core:label"], "classLabel": row["core:label"],
        "segments": [{"id": f"segment-{index}", "range": {
            "startSample": str(row["core:sample_start"]),
            "endSample": str(row["core:sample_start"] + row["core:sample_count"]),
            "frequencyLowerHz": row["core:freq_lower_edge"],
            "frequencyUpperHz": row["core:freq_upper_edge"], "frequencyFrame": "RF"},
            "timeQuality": "SampleLabel", "frequencyQuality": "NominalLabel"}],
        "originEvidence": {"formatId": "torchsig-rfbench-sigmf-v1", "originRef": str(index)}
    }


def main():
    schemas = {name: read_json(ROOT / f"contracts/wideband-detection/v1/{name}.schema.json")
               for name in ("annotation", "run", "worker")}
    for schema in schemas.values():
        Draft202012Validator.check_schema(schema)
    fixture = ROOT / "tests/fixtures/wideband/torchsig-0000000.sigmf-meta"
    meta = read_json(fixture)
    side = read_json(fixture.with_name("torchsig-0000000.json"))
    if len(meta["annotations"]) != 4 or len(side["events"]) != 4:
        raise ValueError("Expected four source-derived labels")
    if (meta["global"]["core:sample_rate"] != side["sampling_rate_hz"]
            or meta["captures"][0]["core:frequency"] != side["capture_center_hz"]):
        raise ValueError("Metadata/sidecar sampling rate or RF center mismatch")
    for index in range(4):
        value = annotation_from_meta(meta, index)
        validate_annotation(value, schemas["annotation"])
        row, event = meta["annotations"][index], side["events"][index]
        if (row["core:sample_start"] != event["sample_start"]
                or row["core:sample_start"] + row["core:sample_count"] != event["sample_end"]
                or row["core:label"] != event["modulation"]):
            raise ValueError(f"Metadata/sidecar time or class mismatch: {index}")
        for edge, field in (("core:freq_lower_edge", "freq_lower_hz"),
                            ("core:freq_upper_edge", "freq_upper_hz")):
            if abs(row[edge] - side["capture_center_hz"] - event[field]) >= 1e-6:
                raise ValueError(f"Metadata/sidecar frequency mismatch: {index}/{edge}")
    valid = annotation_from_meta(meta, 0)
    cases = []
    for name, change in (
        ("uint64 overflow", lambda x: x["segments"][0]["range"].update(startSample=str(2**64))),
        ("reversed time", lambda x: x["segments"][0]["range"].update(endSample="1")),
        ("reversed frequency", lambda x: x["segments"][0]["range"].update(frequencyUpperHz=1)),
        ("duplicate segments", lambda x: x["segments"].append(copy.deepcopy(x["segments"][0]))),
        ("fake label model score", lambda x: x["originEvidence"].update(scoreMean=0.99)),
        ("unknown source", lambda x: x.update(sourceKind="Guess")),
        ("unknown geometry", lambda x: x.update(segments=[])),
        ("nonfinite frequency", lambda x: x["segments"][0]["range"].update(frequencyLowerHz=float("nan"))),
        ("nonfinite original field", lambda x: x["originEvidence"].update(originalFields={"cnr": float("inf")})),
        ("sample trailing newline", lambda x: x["segments"][0]["range"].update(startSample="0\n")),
    ):
        item = copy.deepcopy(valid)
        change(item)
        try:
            validate_annotation(item, schemas["annotation"])
        except (ValueError, ValidationError):
            cases.append(name)
        else:
            raise AssertionError(f"Invalid annotation accepted: {name}")
    run = {"schemaVersion": 1, "runId": "run-1", "projectGeneration": "1", "fileId": "f1",
           "sourceIdentity": {"formatRevision": "1", "sampleChannel": 0, "sampleRateHz": 20e6,
                              "physicalSamples": "400000", "availableSamples": "200000",
                              "fingerprintKind": "range-sha256", "fingerprint": "a" * 64},
           "profileId": "ffscn-gui-iq-v1", "profileVersion": 1, "parameters": {},
           "requestedRange": {**valid["segments"][0]["range"], "endSample": "180000"},
           "lifecycle": "Cancelled", "sourceValidity": "Current", "coverage": [], "chunks": []}
    validate_run(run, schemas["run"])
    for name, change in (
        ("request beyond prefix", lambda x: x["requestedRange"].update(endSample="300000")),
        ("prefix beyond source", lambda x: x["sourceIdentity"].update(availableSamples="500000")),
        ("outside result path", lambda x: x["chunks"].append({"sequence": "0", "relativePath": "../other.json", "sha256": "a" * 64, "resultCount": 1})),
        ("root-relative slash path", lambda x: x["chunks"].append({"sequence": "0", "relativePath": "/outside.json", "sha256": "a" * 64, "resultCount": 1})),
        ("root-relative backslash path", lambda x: x["chunks"].append({"sequence": "0", "relativePath": "\\outside.json", "sha256": "a" * 64, "resultCount": 1})),
        ("nonfinite parameter", lambda x: x["parameters"].update(threshold=float("inf"))),
    ):
        item = copy.deepcopy(run)
        change(item)
        try:
            validate_run(item, schemas["run"])
        except (ValueError, ValidationError):
            cases.append(name)
        else:
            raise AssertionError(f"Invalid run accepted: {name}")
    worker = {"protocolVersion": 1, "type": "hello", "requestId": "r", "sequence": "0", "payload": {}}
    validate_worker(worker, schemas["worker"])
    invalid = {**worker, "type": "batch"}
    if Draft202012Validator(schemas["worker"]).is_valid(invalid):
        raise AssertionError("Uncorrelated batch accepted")
    cases.append("uncorrelated batch")
    for name, change in (
        ("binary size mismatch", lambda x: x.update(binary={"byteLength": 8, "dtype": "float32", "byteOrder": "little", "shape": [1], "sha256": "a" * 64})),
        ("oversize envelope", lambda x: x.update(payload={"description": "a" * 65536})),
        ("worker uint64 overflow", lambda x: x.update(sequence=str(2**64))),
        ("worker sequence newline", lambda x: x.update(sequence="0\n")),
        ("worker hash newline", lambda x: x.update(binary={"byteLength": 4, "dtype": "float32", "byteOrder": "little", "shape": [1], "sha256": "a" * 64 + "\n"})),
    ):
        item = copy.deepcopy(worker)
        change(item)
        try:
            validate_worker(item, schemas["worker"])
        except (ValueError, ValidationError):
            cases.append(name)
        else:
            raise AssertionError(f"Invalid worker accepted: {name}")
    profile = read_json(ROOT / "contracts/wideband-detection/profiles/ffscn-gui-iq-v1.json")
    if profile["model"]["inputShape"] != ["batch", 1, 10, 65536]:
        raise ValueError("Frozen model input shape changed")
    if profile["validation"]["historicalEvidence"] != "conversation report; not reproduced":
        raise ValueError("Historical validation boundary changed")
    report = {"schemaChecks": len(schemas), "sourceDerivedAnnotations": 4,
              "validRunAndHandshake": True, "negativeCasesPassed": cases,
              "modelExecuted": False}
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Contract validation failed: {error}", file=sys.stderr)
        raise
