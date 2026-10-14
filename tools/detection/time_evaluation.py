"""Independent frozen M4 TF evaluator, with failed files and paired bootstrap."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys

import numpy as np
from scipy.optimize import linear_sum_assignment

sys.dont_write_bytecode = True
THRESHOLDS = (0.3, 0.5, 0.7)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path):
    return json.loads(
        path.read_text(encoding="utf-8-sig"),
        parse_constant=lambda v: (_ for _ in ()).throw(
            ValueError(f"Nonfinite JSON {v}")
        ),
    )


def require_execution_freeze(path: Path, own_source: Path) -> dict:
    """Root supplies a complete code/runtime/data identity set before outcomes."""
    frozen = read_json(path)
    if (
        frozen.get("schema") != "signalstudio-m4-execution-freeze-v1"
        or frozen.get("readyForInference") is not True
    ):
        raise ValueError(
            "A ready execution freeze is required before formal outcome access"
        )
    entries = frozen.get("files", [])
    if isinstance(entries, dict):
        entries = [
            {"path": key, "sha256": value} for key, value in entries.items()
        ]
    own = own_source.resolve()
    matched = False
    for item in entries:
        file = Path(item["path"]).resolve()
        if sha256(file) != item["sha256"]:
            raise ValueError(f"Execution identity changed: {file}")
        matched |= file == own
    if not matched:
        raise ValueError(
            "Evaluator/reference source was not included in execution freeze"
        )
    return frozen


def canonical_event(event: dict) -> dict:
    output = {k: float(event[k]) for k in ("t0", "t1", "f0_hz", "f1_hz")}
    if (
        not all(math.isfinite(v) for v in output.values())
        or output["t0"] < 0
        or output["t1"] <= output["t0"]
        or output["f1_hz"] <= output["f0_hz"]
    ):
        raise ValueError("Invalid time-frequency event")
    return {**event, **output}


def truth_events(
    path: Path, sample_rate: float, center_hz: float, physical_samples: int
) -> list[dict]:
    """Keyed TXT parsing is strict; one TIME pair is one event."""
    if path.suffix.lower() == ".json":
        truth = read_json(path)
        if (
            truth["sampleRateHz"] != sample_rate
            or int(truth["physicalSamples"]) != physical_samples
        ):
            raise ValueError("Sample-exact truth source mapping mismatch")
        result = []
        for event in truth["events"]:
            a, b = int(event["startSample"]), int(event["endSample"])
            if not 0 <= a < b <= physical_samples:
                raise ValueError("Sample-exact truth outside file")
            result.append(
                canonical_event(
                    {**event, "t0": a / sample_rate, "t1": b / sample_rate}
                )
            )
        return result
    result = []
    pattern = r"[-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?"
    for line_index, line in enumerate(
        path.read_text(encoding="utf-8-sig").splitlines()[1:], start=2
    ):
        if not line.strip():
            continue
        values = {}
        for key in ("FC", "BW"):
            match = re.search(r"\b" + key + r"\s*:\s*(" + pattern + r")", line)
            if match is None:
                raise ValueError(
                    f"Malformed TXT line {line_index}: missing {key}"
                )
            values[key] = float(match.group(1))
        fc = values["FC"] - sample_rate / 2 + center_hz
        bw = values["BW"]
        match = re.search(r"\bTIME\s*:\s*(.*)$", line)
        if match is None:
            raise ValueError(f"Malformed TXT line {line_index}: missing TIME")
        tokens = match.group(1).split()
        if not tokens:
            raise ValueError(f"TXT line {line_index}: no TIME pair")
        for index, token in enumerate(tokens):
            pair = re.fullmatch("(" + pattern + "),(" + pattern + ")", token)
            if pair is None:
                raise ValueError(f"Malformed TIME token on line {line_index}")
            a, b = map(float, pair.groups())
            if b > physical_samples / sample_rate + 1e-12:
                raise ValueError("TXT truth outside file")
            result.append(
                canonical_event(
                    {
                        "id": f"{path.stem}/row-{line_index}/time-{index}",
                        "carrierId": f"{path.stem}/row-{line_index}",
                        "t0": a,
                        "t1": b,
                        "f0_hz": fc - bw / 2,
                        "f1_hz": fc + bw / 2,
                        "frequencyFrame": "RF",
                    }
                )
            )
    return result


def tf_iou(pred: dict, truth: dict) -> float:
    if pred.get("unknownTime") or truth.get("unknownTime"):
        return 0.0
    df = max(
        0,
        min(pred["f1_hz"], truth["f1_hz"]) - max(pred["f0_hz"], truth["f0_hz"]),
    )
    dt = max(0, min(pred["t1"], truth["t1"]) - max(pred["t0"], truth["t0"]))
    inter = df * dt
    union = (
        (pred["f1_hz"] - pred["f0_hz"]) * (pred["t1"] - pred["t0"])
        + (truth["f1_hz"] - truth["f0_hz"]) * (truth["t1"] - truth["t0"])
        - inter
    )
    return inter / union if union > 0 else 0.0


def frequency_iou(a, b) -> float:
    if a.get("unknownTime") or b.get("unknownTime"):
        return 0.0
    inter = max(0, min(a["f1_hz"], b["f1_hz"]) - max(a["f0_hz"], b["f0_hz"]))
    union = a["f1_hz"] - a["f0_hz"] + b["f1_hz"] - b["f0_hz"] - inter
    return inter / union if union > 0 else 0.0


def assignment(
    matrix: np.ndarray, threshold: float
) -> list[tuple[int, int, float]]:
    if matrix.size == 0:
        return []
    valid = matrix >= threshold
    weights = valid * (min(matrix.shape) + 1.0 + matrix)
    rows, cols = linear_sum_assignment(-weights)
    return [
        (int(a), int(b), float(matrix[a, b]))
        for a, b in zip(rows, cols)
        if valid[a, b]
    ]


def count_metric(tp: int, fp: int, fn: int) -> dict:
    p = tp / (tp + fp) if tp + fp else 0.0
    r = tp / (tp + fn) if tp + fn else 0.0
    return {
        "tp": int(tp),
        "fp": int(fp),
        "fn": int(fn),
        "precision": p,
        "recall": r,
        "f1": 2 * p * r / (p + r) if p + r else 0.0,
    }


def quantiles(values) -> dict:
    return {
        "count": len(values),
        "median": (
            float(np.quantile(values, 0.5, method="linear")) if values else None
        ),
        "p95": (
            float(np.quantile(values, 0.95, method="linear"))
            if values
            else None
        ),
    }


def evaluate(predictions: list[dict], truth: list[dict]) -> dict:
    predictions = sorted(
        map(canonical_event, predictions),
        key=lambda e: (e["f0_hz"], e["t0"], e.get("id", "")),
    )
    truth = list(map(canonical_event, truth))
    if (
        len(predictions) > 65536
        or len(truth) > 65536
        or len(predictions) * len(truth) > 16 * 1024 * 1024
    ):
        raise ValueError("Evaluator matrix budget exceeded")
    matrix = np.asarray(
        [[tf_iou(p, t) for t in truth] for p in predictions], dtype=np.float64
    ).reshape(len(predictions), len(truth))
    metrics = []
    for threshold in THRESHOLDS:
        pairs = assignment(matrix, threshold)
        metrics.append(
            {
                "threshold": threshold,
                **count_metric(
                    len(pairs),
                    len(predictions) - len(pairs),
                    len(truth) - len(pairs),
                ),
                "matches": [
                    {
                        "predictionIndex": a,
                        "truthIndex": b,
                        "predictionId": predictions[a].get("id"),
                        "truthId": truth[b].get("id"),
                        "tfIou": value,
                        "startErrorSeconds": predictions[a]["t0"]
                        - truth[b]["t0"],
                        "endErrorSeconds": predictions[a]["t1"]
                        - truth[b]["t1"],
                        "centerErrorHz": (
                            predictions[a]["f0_hz"]
                            + predictions[a]["f1_hz"]
                            - truth[b]["f0_hz"]
                            - truth[b]["f1_hz"]
                        )
                        / 2,
                        "bandwidthErrorHz": predictions[a]["f1_hz"]
                        - predictions[a]["f0_hz"]
                        - truth[b]["f1_hz"]
                        + truth[b]["f0_hz"],
                        "timeIou": max(
                            0,
                            min(predictions[a]["t1"], truth[b]["t1"])
                            - max(predictions[a]["t0"], truth[b]["t0"]),
                        )
                        / (
                            max(predictions[a]["t1"], truth[b]["t1"])
                            - min(predictions[a]["t0"], truth[b]["t0"])
                        ),
                    }
                    for a, b, value in pairs
                ],
            }
        )
    split = 0
    for target in truth:
        linked = [
            p
            for p in predictions
            if min(p["t1"], target["t1"]) > max(p["t0"], target["t0"])
            and frequency_iou(p, target) >= 0.3
        ]
        split += any(
            a["t1"] <= b["t0"] or b["t1"] <= a["t0"]
            for i, a in enumerate(linked)
            for b in linked[i + 1 :]
        )
    merged = 0
    for pred in predictions:
        linked = [
            t
            for t in truth
            if min(pred["t1"], t["t1"]) > max(pred["t0"], t["t0"])
            and frequency_iou(pred, t) >= 0.3
        ]
        merged += any(
            (a["t1"] < b["t0"] or b["t1"] < a["t0"])
            and frequency_iou(a, b) >= 0.3
            for i, a in enumerate(linked)
            for b in linked[i + 1 :]
        )
    return {
        "predictionCount": len(predictions),
        "truthCount": len(truth),
        "metrics": metrics,
        "splitTruthCount": split,
        "splitRate": split / len(truth) if truth else 0.0,
        "mergedPredictionCount": merged,
        "mergeRate": merged / len(predictions) if predictions else 0.0,
    }


def validate_algorithm_origin(annotation: dict) -> None:
    """Accept the mandatory original r0; exclude all reviewed/manual versions."""
    revision_id = annotation["id"] + ":r0"
    revisions = annotation.get("revisions", [])
    reviews = annotation.get("reviewHistory", [])
    origin = annotation.get("originEvidence") or {}
    segments = annotation.get("segments", [])
    if (
        annotation.get("sourceKind") != "Algorithm"
        or annotation.get("deleted") is not False
        or annotation.get("classLabel") is not None
        or annotation.get("revisionId") != revision_id
        or annotation.get("reviewRevisionId") != revision_id
        or annotation.get("reviewStatus") != "Pending"
        or len(revisions) != 1
        or len(reviews) != 1
        or revisions[0].get("id") != revision_id
        or revisions[0].get("parentRevisionId") != ""
        or revisions[0].get("action") != "algorithm-origin"
        or revisions[0].get("actor") != "model"
        or revisions[0].get("observationChanged", False)
        or any(
            revisions[0].get(key) is not None
            for key in (
                "nameBefore",
                "nameAfter",
                "noteBefore",
                "noteAfter",
                "observationBefore",
                "observationAfter",
            )
        )
        or reviews[0].get("revisionId") != revision_id
        or reviews[0].get("status") != "Pending"
        or reviews[0].get("actor") != "model"
        or origin.get("originalSegments") != segments
        or origin.get("refineMethod") == "Manual"
        or any(
            segment.get("frequencyQuality") == "Manual"
            or segment.get("timeQuality") != "AlgorithmEstimated"
            or segment.get("confirmed") is not False
            for segment in segments
        )
    ):
        raise ValueError(
            "Algorithm gate requires immutable pending algorithm r0"
        )
    changes = revisions[0].get("segmentChanges", [])
    if len(changes) != len(segments) or any(
        change.get("id") != segment.get("id")
        or change.get("before") is not None
        or change.get("after") != segment
        for change, segment in zip(changes, segments)
    ):
        raise ValueError("Algorithm r0 changes differ from original activity")


def load_predictions(
    path: Path,
    fs: float,
    center: float,
    physical: int,
    source: dict | None = None,
    expected_configuration: dict | None = None,
) -> tuple[list[dict], dict]:
    if not path.is_file():
        return [], {
            "failed": True,
            "reason": "result-missing",
            "unknownObservations": 0,
            "annotations": 0,
        }
    try:
        document = read_json(path)
        if "events" in document:
            if (
                document.get("schema")
                != "signalstudio-m4-independent-time-reference-v1"
            ):
                raise ValueError(
                    "Unsupported standalone reference result schema"
                )
        else:
            if document.get("schemaVersion") != 1:
                raise ValueError("Unsupported native validation result schema")
            run = document.get("run", {})
            if run.get("schema") != "signal-studio-detection-run" or run.get(
                "version"
            ) not in (1, 2):
                raise ValueError(
                    "Unsupported native run manifest schema/version"
                )
            if run["version"] == 2:
                for field in ("refinedCandidates", "totalRefinementCandidates"):
                    value = run.get(field)
                    if (
                        not isinstance(value, str)
                        or re.fullmatch(r"0|[1-9][0-9]*", value) is None
                        or int(value) >= 2**64
                    ):
                        raise ValueError(
                            "Invalid native v2 time progress integer"
                        )
                if int(run["refinedCandidates"]) > int(
                    run["totalRefinementCandidates"]
                ):
                    raise ValueError(
                        "Native refined-candidate count exceeds total"
                    )
        if document.get("pass") is not True:
            return [], {
                "failed": True,
                "reason": document.get(
                    "error",
                    document.get("run", {}).get("error", "run-not-complete"),
                ),
                "unknownObservations": 0,
                "annotations": len(document.get("annotations", [])),
                "resultSha256": sha256(path),
            }
        if (
            document.get("sampleRateHz") != fs
            or document.get("centerFrequencyHz") != center
            or int(document.get("physicalSamples", "-1")) != physical
        ):
            raise ValueError("Prediction source mapping mismatch")
        if source is not None:
            if (
                Path(document["sourcePath"]).resolve()
                != Path(source["path"]).resolve()
            ):
                raise ValueError(
                    "Prediction source path is not the frozen input"
                )
            if (
                "sourceSha256" in document
                and document["sourceSha256"] != source["sha256"]
            ):
                raise ValueError("Prediction source content identity mismatch")
        expected_configuration = expected_configuration or {}
        for key in ("strategy", "shortProfile"):
            if (
                key in expected_configuration
                and document.get(key) != expected_configuration[key]
            ):
                raise ValueError(f"Prediction configuration mismatch: {key}")
        result = []
        unknown = 0
        fallback = 0
        if "events" in document:
            result = list(map(canonical_event, document["events"]))
            unknown = int(document.get("unknownObservations", 0))
            fallback = sum(e.get("refineStatus") == "fallback" for e in result)
            annotations = int(document.get("trackCount", len(result)))
        else:
            annotations = 0
            for wrapped in document.get("annotations", []):
                if wrapped.get("kind") not in (
                    "frequency-annotation-v1",
                    "refined-annotation-v1",
                ):
                    raise ValueError(
                        "Unsupported native durable annotation kind"
                    )
                annotation = wrapped.get("annotation", wrapped)
                validate_algorithm_origin(annotation)
                annotations += 1
                origin = annotation.get("originEvidence") or {}
                fallback += origin.get("refineStatus") == "Fallback"
                segments = annotation.get("segments", [])
                if not segments:
                    unknown += 1
                for segment in segments:
                    if segment.get("timeQuality") in (
                        "WindowObservation",
                        "Unknown",
                    ):
                        unknown += 1
                        continue
                    r = segment["range"]
                    a, b = int(r["start"]), int(r["end"])
                    if not 0 <= a < b <= physical:
                        raise ValueError("Prediction interval outside file")
                    frame = segment.get("frequencyFrame", "RF")
                    if frame not in ("RF", "Baseband"):
                        raise ValueError("Unsupported frequency frame")
                    offset = center if frame == "Baseband" else 0.0
                    result.append(
                        canonical_event(
                            {
                                "id": segment["id"],
                                "annotationId": annotation["id"],
                                "t0": a / fs,
                                "t1": b / fs,
                                "f0_hz": r["lowerHz"] + offset,
                                "f1_hz": r["upperHz"] + offset,
                                "refineStatus": origin.get("refineStatus"),
                            }
                        )
                    )
        # Accounting placeholders are never activity or observation rectangles.
        # They match nothing, but each unknown output remains in the FP count.
        result.extend(
            {
                "id": f"unknown-time-accounting-{i}",
                "unknownTime": True,
                "isActivity": False,
                "t0": 0.0,
                "t1": physical / fs,
                "f0_hz": center - fs / 2,
                "f1_hz": center + fs / 2,
            }
            for i in range(unknown)
        )
        if any(e["t1"] > physical / fs + 1e-12 for e in result):
            raise ValueError("Prediction event exceeds source")
        return result, {
            "failed": False,
            "reason": "",
            "unknownObservations": unknown,
            "annotations": annotations,
            "fallbackAnnotations": fallback,
            "resultSha256": sha256(path),
            "strategy": document.get("strategy"),
            "shortProfile": document.get("shortProfile"),
            "semantics": document.get("semantics", "native-product"),
            "elapsedSeconds": document.get("elapsedSeconds"),
        }
    except (ValueError, KeyError, TypeError, OverflowError) as error:
        return [], {
            "failed": True,
            "reason": f"invalid-result: {error}",
            "unknownObservations": 0,
            "annotations": 0,
            "resultSha256": sha256(path),
        }


def summarize(files: list[dict], side: str) -> dict:
    metrics = []
    for ordinal, threshold in enumerate(THRESHOLDS):
        rows = [f[side]["metrics"][ordinal] for f in files]
        metric = {
            "threshold": threshold,
            **count_metric(
                sum(v["tp"] for v in rows),
                sum(v["fp"] for v in rows),
                sum(v["fn"] for v in rows),
            ),
        }
        pairs = [p for row in rows for p in row["matches"]]
        start = [abs(p["startErrorSeconds"]) for p in pairs]
        end = [abs(p["endErrorSeconds"]) for p in pairs]
        metric.update(
            startAbsoluteErrorSeconds=quantiles(start),
            endAbsoluteErrorSeconds=quantiles(end),
            combinedEdgeAbsoluteErrorSeconds=quantiles(start + end),
            centerMaeHz=(
                float(np.mean([abs(p["centerErrorHz"]) for p in pairs]))
                if pairs
                else None
            ),
            bandwidthMaeHz=(
                float(np.mean([abs(p["bandwidthErrorHz"]) for p in pairs]))
                if pairs
                else None
            ),
        )
        metrics.append(metric)
    noise = [f for f in files if f["truthCount"] == 0]
    observed_noise = [f for f in noise if not f[side + "State"]["failed"]]
    exposure = sum(
        f["effectiveBandwidthHz"] / 1e6 * f["durationSeconds"]
        for f in observed_noise
    )
    fp = sum(f[side]["predictionCount"] for f in observed_noise)
    diagnostics = [f[side + "State"] for f in files]
    n_truth = sum(f["truthCount"] for f in files)
    n_pred = sum(f[side]["predictionCount"] for f in files)
    return {
        "fileCount": len(files),
        "failedFiles": sum(s["failed"] for s in diagnostics),
        "metrics": metrics,
        "unknownObservations": sum(
            s["unknownObservations"] for s in diagnostics
        ),
        "annotations": sum(s["annotations"] for s in diagnostics),
        "fallbackAnnotations": sum(
            s.get("fallbackAnnotations", 0) for s in diagnostics
        ),
        "splitRate": (
            sum(f[side]["splitTruthCount"] for f in files) / n_truth
            if n_truth
            else 0
        ),
        "mergeRate": (
            sum(f[side]["mergedPredictionCount"] for f in files) / n_pred
            if n_pred
            else 0
        ),
        "noise": {
            "files": len(noise),
            "failedFiles": sum(f[side + "State"]["failed"] for f in noise),
            "observedFiles": len(observed_noise),
            "falsePositives": fp,
            "zeroDetectionFiles": sum(
                f[side]["predictionCount"] == 0 for f in observed_noise
            ),
            "falsePositivesPerFile": (
                fp / len(observed_noise) if observed_noise else None
            ),
            "exposureMHzSeconds": exposure,
            "falsePositivesPerMHzSecond": fp / exposure if exposure else None,
        },
    }


def bootstrap(files: list[dict], repeats=10000, seed=2026101405) -> dict:
    if not files:
        return {"fileCount": 0, "difference": None, "ci95": None}
    counts = []
    for side in ("reference", "native"):
        counts.append(
            np.asarray(
                [
                    [f[side]["metrics"][1][k] for k in ("tp", "fp", "fn")]
                    for f in files
                ],
                dtype=np.int64,
            )
        )

    def f1(c):
        denominator = 2 * c[..., 0] + c[..., 1] + c[..., 2]
        return np.divide(
            2 * c[..., 0],
            denominator,
            out=np.zeros_like(denominator, dtype=np.float64),
            where=denominator > 0,
        )

    rng = np.random.Generator(np.random.PCG64(seed))
    sampled = rng.integers(0, len(files), (repeats, len(files)))
    values = f1(counts[1][sampled].sum(axis=1)) - f1(
        counts[0][sampled].sum(axis=1)
    )
    point = float(f1(counts[1].sum(axis=0)) - f1(counts[0].sum(axis=0)))
    ci = np.quantile(values, [0.025, 0.975], method="linear").tolist()
    return {
        "fileCount": len(files),
        "seed": seed,
        "repeats": repeats,
        "difference": point,
        "ci95": ci,
        "pointDropAtMostOnePercentagePoint": point >= -0.01,
        "statisticalNoninferiorityEstablished": ci[0] >= -0.01,
    }


def evaluate_index(index: Path, freeze: Path, output: Path) -> dict:
    frozen = require_execution_freeze(freeze, Path(__file__))
    document = read_json(index)
    if not document.get("samples"):
        raise ValueError("An empty paired experiment is not valid")
    files = []
    seen = set()
    for item in document["samples"]:
        if item["sampleId"] in seen:
            raise ValueError("Duplicate sample ID in one paired experiment")
        seen.add(item["sampleId"])
        source = item["source"]
        truth = item["truth"]
        for asset in (source, truth):
            if sha256(Path(asset["path"])) != asset["sha256"]:
                raise ValueError("Frozen IQ/truth identity mismatch")
        fs = float(item["sampleRateHz"])
        center = float(item.get("centerFrequencyHz", 2.5e9))
        physical = int(item["physicalSamples"])
        labels = truth_events(Path(truth["path"]), fs, center, physical)
        ref, rs = load_predictions(
            Path(item["referenceResult"]),
            fs,
            center,
            physical,
            source,
            document.get("parameters"),
        )
        native, ns = load_predictions(
            Path(item["nativeResult"]),
            fs,
            center,
            physical,
            source,
            document.get("parameters"),
        )
        strata = {}
        if item.get("group") == "short-1-to-8ms" and labels:
            durations = sorted(
                {round((e["t1"] - e["t0"]) * 1000, 6) for e in labels}
            )
            cnrs = sorted({float(e["cnrDb"]) for e in labels if "cnrDb" in e})
            if len(durations) == 1:
                strata["burstMs"] = durations[0]
            if len(cnrs) == 1:
                strata["cnrDb"] = cnrs[0]
        files.append(
            {
                "sampleId": item["sampleId"],
                "group": item.get("group", "G"),
                "truthCount": len(labels),
                "strata": strata,
                "durationSeconds": physical / fs,
                "effectiveBandwidthHz": float(
                    item.get("effectiveBandwidthHz", fs)
                ),
                "reference": evaluate(ref, labels),
                "native": evaluate(native, labels),
                "referenceState": rs,
                "nativeState": ns,
                "sourceSha256": source["sha256"],
                "truthSha256": truth["sha256"],
            }
        )
    groups = {
        g: [f for f in files if f["group"] == g]
        for g in sorted({f["group"] for f in files})
    }
    if (
        "expectedTruthCount" in document
        and sum(f["truthCount"] for f in files)
        != document["expectedTruthCount"]
    ):
        raise ValueError(
            "Parsed truth event count differs from frozen experiment"
        )
    stratum_groups = {}
    for field in ("burstMs", "cnrDb"):
        for value in sorted(
            {f["strata"][field] for f in files if field in f["strata"]}
        ):
            stratum_groups[f"short/{field}/{value:g}"] = [
                f for f in files if f["strata"].get(field) == value
            ]
    report = {
        "schema": "signalstudio-m4-paired-time-evaluation-v1",
        "evaluator": "tf-iou-max-cardinality-then-max-iou-v1",
        "evaluatorSha256": sha256(Path(__file__)),
        "executionFreeze": str(freeze.resolve()),
        "executionFreezeSha256": sha256(freeze),
        "indexSha256": sha256(index),
        "parameters": document.get("parameters"),
        "files": files,
        "reference": summarize(files, "reference"),
        "native": summarize(files, "native"),
        "pairedFileBootstrap": bootstrap(files),
        "groups": {
            g: {
                "reference": summarize(rows, "reference"),
                "native": summarize(rows, "native"),
                "pairedFileBootstrap": bootstrap(rows),
            }
            for g, rows in groups.items()
        },
        "strata": {
            g: {
                "reference": summarize(rows, "reference"),
                "native": summarize(rows, "native"),
                "pairedFileBootstrap": bootstrap(rows),
            }
            for g, rows in stratum_groups.items()
        },
        "inputFiles": len(files),
        "noFileExcludedAfterOutcomes": True,
        "referenceBaselineComplete": all(
            not f["referenceState"]["failed"] for f in files
        ),
        "actualRealRfCaptureDomainPresent": any(
            f["group"] == "real-rf-capture" for f in files
        ),
        "frozenHistoricalGuiF1Reproduced": False,
        "externalBusinessImported": False,
    }
    short = groups.get("short-1-to-8ms", [])
    if set(groups) == {"G"}:
        report["l3PrimaryGate"] = {
            "comparison": "G sample-default paired micro TF-F1@0.5",
            "referenceBaselineComplete": report["referenceBaselineComplete"],
            "pointDifference": report["pairedFileBootstrap"]["difference"],
            "pointGatePassed": report["pairedFileBootstrap"][
                "pointDropAtMostOnePercentagePoint"
            ],
            "ci95": report["pairedFileBootstrap"]["ci95"],
            "primarySampleDefaultConfiguration": document.get(
                "parameters", {}
            ).get("strategy")
            == "sample"
            and document.get("parameters", {}).get("shortProfile") is False,
            "note": "A failed reference baseline cannot establish migration parity",
        }
    if short:
        metric = report["groups"]["short-1-to-8ms"]["native"]["metrics"][1]
        edge = metric["combinedEdgeAbsoluteErrorSeconds"]["p95"]
        report["shortPrimaryNumericTargets"] = {
            "recall": metric["recall"],
            "recallAtLeast90Percent": metric["recall"] >= 0.9,
            "combinedEdgeP95Seconds": edge,
            "edgeP95AtMost1Ms": edge is not None and edge <= 0.001,
            "scope": document.get("parameters"),
            "matchedEdgesOnly": True,
            "truthEvents": sum(f["truthCount"] for f in short),
            "primarySampleShortConfiguration": document.get(
                "parameters", {}
            ).get("strategy")
            == "sample"
            and document.get("parameters", {}).get("shortProfile") is True,
        }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)
        + "\n",
        encoding="utf-8",
    )
    return report


def self_check() -> dict:
    assert len(assignment(np.array([[0.9, 0.5], [0.5, 0.49]]), 0.5)) == 2
    assert len(assignment(np.array([[0.9, 0.5], [0.5, 0.49]]), 0.7)) == 1
    event = {"t0": 0.0, "t1": 1.0, "f0_hz": 0.0, "f1_hz": 10.0}
    assert tf_iou(event, event) == 1
    unknown = {**event, "unknownTime": True, "isActivity": False}
    counted = evaluate([event, unknown], [event])
    assert counted["metrics"][1]["tp"] == 1
    assert counted["metrics"][1]["fp"] == 1
    assert counted["metrics"][1]["f1"] == 2 / 3
    original = {
        "id": "native",
        "sourceKind": "Algorithm",
        "deleted": False,
        "classLabel": None,
        "revisionId": "native:r0",
        "reviewRevisionId": "native:r0",
        "reviewStatus": "Pending",
        "segments": [],
        "originEvidence": {"originalSegments": []},
        "revisions": [
            {
                "id": "native:r0",
                "parentRevisionId": "",
                "action": "algorithm-origin",
                "actor": "model",
                "segmentChanges": [],
            }
        ],
        "reviewHistory": [
            {"revisionId": "native:r0", "status": "Pending", "actor": "model"}
        ],
    }
    validate_algorithm_origin(original)
    try:
        validate_algorithm_origin({**original, "revisionId": "native:r1"})
    except ValueError:
        pass
    else:
        raise AssertionError("Human revision entered algorithm evaluation")
    failed = evaluate([], [event, event])
    assert all(m["tp"] == 0 and m["fn"] == 2 for m in failed["metrics"])
    exact = evaluate([event], [event])
    sample = {"reference": exact, "native": exact}
    ci = bootstrap([sample] * 10, repeats=100)
    assert ci["ci95"] == [0.0, 0.0]
    state = {
        "failed": False,
        "unknownObservations": 0,
        "annotations": 1,
        "fallbackAnnotations": 0,
    }
    noise_observed = {
        "truthCount": 0,
        "effectiveBandwidthHz": 1e6,
        "durationSeconds": 1,
        "native": evaluate([event], []),
        "nativeState": state,
    }
    noise_failed = {
        **noise_observed,
        "native": evaluate([], []),
        "nativeState": {**state, "failed": True, "annotations": 0},
    }
    noise = summarize([noise_observed, noise_failed], "native")["noise"]
    assert noise["files"] == 2 and noise["observedFiles"] == 1
    assert noise["failedFiles"] == 1 and noise["zeroDetectionFiles"] == 0
    assert noise["exposureMHzSeconds"] == 1
    assert noise["falsePositivesPerFile"] == 1
    return {
        "passed": True,
        "checks": [
            "cardinality-first-per-threshold",
            "exact-iou",
            "failed-zero-predictions-keep-truth",
            "paired-bootstrap-identical-zero-delta",
            "immutable-native-r0-and-human-exclusion",
            "unknown-time-zero-iou-retains-fp",
            "failed-noise-not-observed-exposure-or-zero-detection",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--index", type=Path)
    parser.add_argument("--freeze", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2))
        return 0
    if not all((args.index, args.freeze, args.output)):
        parser.error("--index, --freeze and --output are required")
    result = evaluate_index(args.index, args.freeze, args.output)
    print(
        json.dumps(
            {
                "output": str(args.output),
                "files": result["inputFiles"],
                "pairedFileBootstrap": result["pairedFileBootstrap"],
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
